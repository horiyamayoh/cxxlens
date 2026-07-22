#include "llvm/clang22/materialization_io.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/sdk/common.hpp>
#include <dirent.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
	using namespace cxxlens::detail::clang22::materialization;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] std::vector<std::byte> bytes(const std::string_view value)
	{
		const auto view = std::as_bytes(std::span{value.data(), value.size()});
		return {view.begin(), view.end()};
	}

	[[nodiscard]] std::vector<std::byte> byte_prefix(const std::vector<std::byte>& input,
													 const std::size_t size)
	{
		const auto view = std::span{input}.first(size);
		return {view.begin(), view.end()};
	}

#if defined(__linux__) && defined(F_ADD_SEALS) && defined(F_GET_SEALS) && defined(F_SEAL_WRITE) && \
	defined(F_SEAL_GROW) && defined(F_SEAL_SHRINK) && defined(F_SEAL_SEAL)
	[[nodiscard]] int locate_materialization_memfd()
	{
		auto* directory = ::opendir("/proc/self/fd");
		require(directory != nullptr, "cannot enumerate /proc/self/fd for the private memfd");
		const auto directory_descriptor = ::dirfd(directory);
		int found = -1;
		while (const auto* entry = ::readdir(directory))
		{
			char* end{};
			const auto parsed = std::strtol(entry->d_name, &end, 10);
			if (end == entry->d_name || *end != '\0' || parsed < 0 ||
				parsed == directory_descriptor)
				continue;
			const auto path = std::string{"/proc/self/fd/"} + entry->d_name;
			std::array<char, 256U> target{};
			const auto count = ::readlink(path.c_str(), target.data(), target.size() - 1U);
			if (count <= 0)
				continue;
			const std::string_view observed{target.data(), static_cast<std::size_t>(count)};
			if (observed.find("memfd:cxxlens-materialization") == std::string_view::npos)
				continue;
			require(found < 0, "more than one materialization memfd was unexpectedly retained");
			found = static_cast<int>(parsed);
		}
		require(::closedir(directory) == 0, "cannot close the /proc/self/fd enumeration");
		require(found >= 0, "the private spool is not backed by the required Linux memfd");
		return found;
	}

	[[nodiscard]] std::string proc_fd_path(const int descriptor)
	{
		return "/proc/self/fd/" + std::to_string(descriptor);
	}
#endif

	class fragmented_reader final : public materialization_byte_reader
	{
	  public:
		explicit fragmented_reader(std::vector<std::byte> input,
								   std::vector<std::size_t> fragments = {})
			: input_{std::move(input)}, fragments_{std::move(fragments)}
		{
		}

		materialization_io_result<std::size_t> read(std::span<std::byte> destination) override
		{
			maximum_destination_ = std::max(maximum_destination_, destination.size());
			if (throw_allocation_)
				throw std::bad_alloc{};
			if (fail_call_ && calls_ == *fail_call_)
				return materialization_io_failure{materialization_io_failure_kind::read,
												  materialization_io_operation::input_read};
			if (overreport_)
				return destination.size() + 1U;
			++calls_;
			if (offset_ == input_.size())
			{
				++eof_reads_;
				return 0U;
			}
			const auto fragment = fragments_.empty()
				? destination.size()
				: fragments_[(calls_ - 1U) % fragments_.size()];
			require(fragment != 0U, "test fragment must be a positive short-read bound");
			const auto count = std::min({fragment, destination.size(), input_.size() - offset_});
			std::ranges::copy(std::span{input_}.subspan(offset_, count), destination.begin());
			offset_ += count;
			return count;
		}

		std::vector<std::byte> input_;
		std::vector<std::size_t> fragments_;
		std::size_t offset_{};
		std::size_t calls_{};
		std::size_t eof_reads_{};
		std::size_t maximum_destination_{};
		std::optional<std::size_t> fail_call_;
		bool overreport_{};
		bool throw_allocation_{};
	};

	class memory_spool final : public materialization_private_spool
	{
	  public:
		materialization_io_result<void> append(const std::span<const std::byte> input) override
		{
			if (throw_allocation_on_write_)
				throw std::bad_alloc{};
			if (fail_write_call_ && write_calls_ == *fail_write_call_)
				return materialization_io_failure{materialization_io_failure_kind::write,
												  materialization_io_operation::spool_write};
			if (sealed_)
				return materialization_io_failure{materialization_io_failure_kind::spool,
												  materialization_io_operation::spool_write};
			++write_calls_;
			data_.insert(data_.end(), input.begin(), input.end());
			return {};
		}

		materialization_io_result<void> seal() override
		{
			if (fail_seal_)
				return materialization_io_failure{materialization_io_failure_kind::spool,
												  materialization_io_operation::spool_seal};
			sealed_ = true;
			return {};
		}

		std::vector<std::byte> data_;
		std::size_t write_calls_{};
		std::optional<std::size_t> fail_write_call_;
		bool fail_seal_{};
		bool throw_allocation_on_write_{};
		bool sealed_{};
	};

	class injectable_digest final : public materialization_digest_accumulator
	{
	  public:
		materialization_io_result<void> update(const std::span<const std::byte> input) override
		{
			if (update_failure_)
				return materialization_io_failure{*update_failure_,
												  materialization_io_operation::digest_update};
			data_.insert(data_.end(), input.begin(), input.end());
			return {};
		}

		materialization_io_result<std::string> finish() override
		{
			if (finish_failure_)
				return materialization_io_failure{*finish_failure_,
												  materialization_io_operation::digest_finalize};
			if (malformed_finish_)
				return std::string{"SHA256:not-canonical"};
			return cxxlens::sdk::content_digest(data_);
		}

		std::vector<std::byte> data_;
		std::optional<materialization_io_failure_kind> update_failure_;
		std::optional<materialization_io_failure_kind> finish_failure_;
		bool malformed_finish_{};
	};

	class contradictory_replay_spool final : public materialization_replayable_spool
	{
	  public:
		explicit contradictory_replay_spool(const bool overreport) : overreport_{overreport} {}

		materialization_io_result<void> append(std::span<const std::byte>) override
		{
			return materialization_io_failure{
				materialization_io_failure_kind::invalid_configuration,
				materialization_io_operation::spool_write};
		}

		materialization_io_result<void> seal() override
		{
			return {};
		}

		materialization_io_result<std::size_t> read(std::span<std::byte> destination) override
		{
			return read_at(0U, destination);
		}

		materialization_io_result<std::size_t> read_at(std::uint64_t,
													   std::span<std::byte> destination) override
		{
			return overreport_ ? destination.size() + 1U : 0U;
		}

		materialization_io_result<void> rewind() override
		{
			return {};
		}

		[[nodiscard]] std::uint64_t size_bytes() const noexcept override
		{
			return 1U;
		}

		[[nodiscard]] bool sealed() const noexcept override
		{
			return true;
		}

	  private:
		bool overreport_{};
	};

	void phase_authentic_failure_matrix()
	{
		constexpr std::array authentic{
			materialization_io_failure{materialization_io_failure_kind::read,
									   materialization_io_operation::input_read},
			materialization_io_failure{materialization_io_failure_kind::write,
									   materialization_io_operation::spool_write},
			materialization_io_failure{materialization_io_failure_kind::spool,
									   materialization_io_operation::spool_seal},
			materialization_io_failure{materialization_io_failure_kind::hash,
									   materialization_io_operation::digest_finalize},
		};
		for (const auto& failure : authentic)
			require(is_materialization_actual_io_or_hash_failure(failure),
					"actual port I/O/hash failure was removed from stable taxonomy");

		constexpr std::array private_failures{
			materialization_io_failure{materialization_io_failure_kind::allocation,
									   materialization_io_operation::buffer_allocation},
			materialization_io_failure{materialization_io_failure_kind::invalid_configuration,
									   materialization_io_operation::configuration},
			materialization_io_failure{materialization_io_failure_kind::read,
									   materialization_io_operation::digest_finalize},
			materialization_io_failure{materialization_io_failure_kind::hash,
									   materialization_io_operation::spool_read},
			materialization_io_failure{materialization_io_failure_kind::spool,
									   materialization_io_operation::configuration},
		};
		for (const auto& failure : private_failures)
			require(!is_materialization_actual_io_or_hash_failure(failure),
					"configuration/allocation/operation-drift entered stable taxonomy");
	}

	void expect_capture(std::vector<std::byte> input,
						const std::uint64_t limit,
						std::vector<std::size_t> fragments)
	{
		const auto expected_size =
			static_cast<std::size_t>(std::min<std::uint64_t>(input.size(), limit + 1U));
		const auto expected = byte_prefix(input, expected_size);
		fragmented_reader reader{std::move(input), std::move(fragments)};
		memory_spool spool;
		auto captured = capture_bounded_input(reader, spool, {limit, 2U});
		require(captured.has_value(), "valid bounded input capture failed");
		require(captured->byte_limit == limit && captured->observed_size_bytes == expected_size,
				"bounded raw-input byte count differs from exact prefix authority");
		const auto expected_digest = cxxlens::sdk::content_digest(expected);
		if (captured->observed_prefix_digest != expected_digest)
			std::cerr << "actual: " << captured->observed_prefix_digest
					  << "\nexpected: " << expected_digest << '\n';
		require(captured->observed_prefix_digest == expected_digest,
				"incremental digest differs from exact prefix authority");
		require(captured->complete == (reader.input_.size() <= limit),
				"bounded raw-input completeness differs from EOF observation");
		require(spool.sealed_ && spool.data_ == expected && reader.offset_ == expected_size,
				"private spool or consumed prefix differs from observation");
		require(reader.maximum_destination_ <= 2U,
				"state machine allocated or requested more than one bounded chunk");
		if (reader.input_.size() <= limit)
			require(reader.eof_reads_ == 1U, "complete input was claimed without observing EOF");
		else
			require(reader.eof_reads_ == 0U && reader.offset_ == limit + 1U,
					"oversize input read or claimed bytes beyond exact limit+1 prefix");
	}

	void boundary_state_machine()
	{
		const auto source = bytes("abcdef");
		for (const auto size : {0U, 3U, 4U, 5U, 6U})
			expect_capture(byte_prefix(source, size), 4U, {1U, 2U, 1U});
		expect_capture({}, 0U, {1U});
		expect_capture(byte_prefix(source, 1U), 0U, {1U});
		expect_capture(byte_prefix(source, 2U), 0U, {1U});
	}

	void fragmented_digest_oracle()
	{
		std::vector<std::byte> input(257U);
		for (std::size_t index{}; index < input.size(); ++index)
			input[index] = static_cast<std::byte>((index * 37U) & 0xffU);
		for (const auto size : {1U, 55U, 56U, 57U, 63U, 64U, 65U, 127U, 128U, 129U, 257U})
		{
			const auto prefix = byte_prefix(input, size);
			const auto expected = cxxlens::sdk::content_digest(prefix);
			fragmented_reader reader{prefix, {1U, 63U, 2U, 64U, 3U, 17U}};
			memory_spool spool;
			auto captured = capture_bounded_input(reader, spool, {300U, 17U});
			require(captured && captured->complete &&
						captured->observed_size_bytes == prefix.size() &&
						captured->observed_prefix_digest == expected && spool.data_ == prefix,
					"incremental SHA-256 differs from SDK one-shot authority");
		}
	}

	void default_limit_stays_chunk_bounded()
	{
		const auto input = bytes("abc");
		fragmented_reader reader{input, {1U}};
		memory_spool spool;
		auto captured = capture_bounded_input(reader, spool);
		require(captured && captured->complete &&
					captured->byte_limit == maximum_raw_request_bytes &&
					captured->observed_size_bytes == input.size() && spool.data_ == input &&
					reader.maximum_destination_ <= default_stream_chunk_bytes,
				"one-GiB authority did not stay behind the bounded-chunk state machine");
	}

	void typed_failures()
	{
		fragmented_reader reader{bytes("abcdef"), {2U}};
		memory_spool spool;
		auto invalid = capture_bounded_input(reader, spool, {4U, 0U});
		require(!invalid &&
					invalid.error().kind ==
						materialization_io_failure_kind::invalid_configuration &&
					invalid.error().operation == materialization_io_operation::configuration,
				"zero chunk size was not a typed configuration failure");
		invalid = capture_bounded_input(reader, spool, {4U, maximum_stream_chunk_bytes + 1U});
		require(!invalid &&
					invalid.error().kind == materialization_io_failure_kind::invalid_configuration,
				"unbounded chunk size was accepted");
		invalid = capture_bounded_input(
			reader,
			spool,
			{std::numeric_limits<std::uint64_t>::max() / 8U, default_stream_chunk_bytes});
		require(!invalid &&
					invalid.error().kind == materialization_io_failure_kind::invalid_configuration,
				"SHA-256 bit-length overflow configuration was accepted");

		reader = fragmented_reader{bytes("abcdef"), {2U}};
		spool = memory_spool{};
		reader.fail_call_ = 1U;
		auto failed = capture_bounded_input(reader, spool, {4U, 2U});
		require(!failed &&
					failed.error() ==
						materialization_io_failure{materialization_io_failure_kind::read,
												   materialization_io_operation::input_read} &&
					spool.data_ == bytes("ab") && !spool.sealed_,
				"fragmented read failure was not retained without a complete claim");

		reader = fragmented_reader{bytes("abcd"), {2U}};
		spool = memory_spool{};
		reader.fail_call_ = 2U;
		failed = capture_bounded_input(reader, spool, {4U, 2U});
		require(!failed && failed.error().kind == materialization_io_failure_kind::read,
				"failed EOF probe incorrectly produced a complete observation");

		reader = fragmented_reader{bytes("abc"), {2U}};
		spool = memory_spool{};
		reader.overreport_ = true;
		failed = capture_bounded_input(reader, spool, {4U, 2U});
		require(!failed &&
					failed.error().kind == materialization_io_failure_kind::invalid_configuration &&
					spool.data_.empty(),
				"reader count beyond requested span was accepted");

		reader = fragmented_reader{bytes("abcdef"), {2U}};
		spool = memory_spool{};
		spool.fail_write_call_ = 1U;
		failed = capture_bounded_input(reader, spool, {4U, 2U});
		require(!failed &&
					failed.error() ==
						materialization_io_failure{materialization_io_failure_kind::write,
												   materialization_io_operation::spool_write} &&
					!spool.sealed_,
				"spool write failure was not retained as a private typed failure");

		reader = fragmented_reader{bytes("abc"), {2U}};
		spool = memory_spool{};
		spool.fail_seal_ = true;
		failed = capture_bounded_input(reader, spool, {4U, 2U});
		require(!failed &&
					failed.error() ==
						materialization_io_failure{materialization_io_failure_kind::spool,
												   materialization_io_operation::spool_seal},
				"spool seal failure was not retained as a private typed failure");

		reader = fragmented_reader{bytes("abc"), {2U}};
		spool = memory_spool{};
		reader.throw_allocation_ = true;
		failed = capture_bounded_input(reader, spool, {4U, 2U});
		require(!failed &&
					failed.error() ==
						materialization_io_failure{materialization_io_failure_kind::allocation,
												   materialization_io_operation::input_read},
				"allocation failure escaped the typed private boundary");

		reader = fragmented_reader{bytes("abc"), {2U}};
		spool = memory_spool{};
		spool.throw_allocation_on_write_ = true;
		failed = capture_bounded_input(reader, spool, {4U, 2U});
		require(!failed &&
					failed.error() ==
						materialization_io_failure{materialization_io_failure_kind::allocation,
												   materialization_io_operation::spool_write},
				"spool allocation failure escaped the typed private boundary");

		reader = fragmented_reader{bytes("abc"), {2U}};
		spool = memory_spool{};
		injectable_digest digest;
		digest.update_failure_ = materialization_io_failure_kind::hash;
		failed = capture_bounded_input(reader, spool, digest, {4U, 2U});
		require(!failed &&
					failed.error() ==
						materialization_io_failure{materialization_io_failure_kind::hash,
												   materialization_io_operation::digest_update},
				"incremental hash update failure escaped its private port");

		reader = fragmented_reader{bytes("abc"), {2U}};
		spool = memory_spool{};
		digest = injectable_digest{};
		digest.finish_failure_ = materialization_io_failure_kind::hash;
		failed = capture_bounded_input(reader, spool, digest, {4U, 2U});
		require(!failed && spool.sealed_ &&
					failed.error() ==
						materialization_io_failure{materialization_io_failure_kind::hash,
												   materialization_io_operation::digest_finalize},
				"incremental hash finalization failure escaped its private port");

		reader = fragmented_reader{bytes("abc"), {2U}};
		spool = memory_spool{};
		digest = injectable_digest{};
		digest.malformed_finish_ = true;
		failed = capture_bounded_input(reader, spool, digest, {4U, 2U});
		require(!failed && spool.sealed_ &&
					failed.error().kind == materialization_io_failure_kind::invalid_configuration &&
					failed.error().operation == materialization_io_operation::digest_finalize,
				"malformed successful capture digest escaped private configuration taxonomy");

		for (const auto overreport : {false, true})
		{
			contradictory_replay_spool replay{overreport};
			auto replay_digest = digest_materialization_spool(replay, 1U);
			require(!replay_digest &&
						replay_digest.error().kind ==
							materialization_io_failure_kind::invalid_configuration &&
						replay_digest.error().operation == materialization_io_operation::spool_read,
					"successful replay count contradiction entered I/O failure taxonomy");
		}
	}

	void anonymous_replay_spool()
	{
		auto spool = make_materialization_private_spool();
		require(spool.has_value(), "anonymous private spool creation failed");
		const auto first = bytes("abc");
		const auto second = bytes("defgh");
		require((*spool)->append(first) && (*spool)->append(second) &&
					(*spool)->size_bytes() == 8U && !(*spool)->sealed(),
				"anonymous spool did not retain the exact unsealed byte census");

		std::array<std::byte, 3U> middle{};
		auto read = (*spool)->read_at(2U, middle);
		require(read && *read == middle.size() &&
					std::vector<std::byte>{middle.begin(), middle.end()} == bytes("cde"),
				"cursor-independent private-spool read differs");
		require((*spool)->seal() && (*spool)->sealed(), "anonymous spool did not become immutable");
		auto sealed_append = (*spool)->append(bytes("x"));
		require(!sealed_append &&
					sealed_append.error().kind ==
						materialization_io_failure_kind::invalid_configuration &&
					(*spool)->size_bytes() == 8U,
				"sealed anonymous spool accepted an append");

		std::array<std::byte, 8U> replay{};
		auto replayed = (*spool)->read(replay);
		require(replayed && *replayed == replay.size() &&
					std::vector<std::byte>{replay.begin(), replay.end()} == bytes("abcdefgh"),
				"sealed anonymous spool did not replay from byte zero");
		require(static_cast<bool>((*spool)->rewind()), "anonymous spool rewind failed");
		std::array<std::byte, 2U> prefix{};
		replayed = (*spool)->read(prefix);
		require(replayed && *replayed == prefix.size() &&
					std::vector<std::byte>{prefix.begin(), prefix.end()} == bytes("ab"),
				"anonymous spool rewind changed replay bytes");

		auto digest = digest_materialization_spool(**spool, 3U);
		require(digest && *digest == cxxlens::sdk::content_digest(bytes("abcdefgh")),
				"streaming anonymous-spool digest differs from the exact bytes");
	}

	void kernel_seal_is_adversarially_immutable()
	{
#if defined(__linux__) && defined(F_ADD_SEALS) && defined(F_GET_SEALS) && defined(F_SEAL_WRITE) && \
	defined(F_SEAL_GROW) && defined(F_SEAL_SHRINK) && defined(F_SEAL_SEAL)
		auto spool = make_materialization_private_spool();
		require(spool.has_value(), "private memfd creation failed for kernel-seal test");
		const auto original = bytes("authority");
		require((*spool)->append(original) && (*spool)->seal() && (*spool)->sealed(),
				"private memfd did not reach its verified sealed state");

		const auto descriptor = locate_materialization_memfd();
		const auto path = proc_fd_path(descriptor);
		const auto reopened = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
		require(reopened >= 0, "cannot reopen the private memfd through /proc/self/fd");
		constexpr auto required_seals = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
		const auto observed_seals = ::fcntl(reopened, F_GET_SEALS);
		require(observed_seals >= 0 && (observed_seals & required_seals) == required_seals,
				"reopened private memfd lacks an exact required kernel seal bit");

		const auto replacement = std::byte{'X'};
		errno = 0;
		require(::pwrite(reopened, &replacement, 1U, 0) < 0 && errno == EPERM,
				"reopened sealed memfd accepted pwrite mutation");
		errno = 0;
		require(::ftruncate(reopened, static_cast<off_t>(original.size() + 1U)) < 0 &&
					errno == EPERM,
				"reopened sealed memfd accepted growth");
		errno = 0;
		require(::ftruncate(reopened, static_cast<off_t>(original.size() - 1U)) < 0 &&
					errno == EPERM,
				"reopened sealed memfd accepted shrinkage");
		errno = 0;
		require(::fcntl(reopened, F_ADD_SEALS, F_SEAL_WRITE) < 0 && errno == EPERM,
				"reopened sealed memfd accepted further seal mutation");
		require(::close(reopened) == 0, "cannot close the adversarially reopened memfd");

		std::array<std::byte, 9U> replay{};
		auto replayed = (*spool)->read_at(0U, replay);
		require(replayed && *replayed == replay.size() &&
					std::vector<std::byte>{replay.begin(), replay.end()} == original,
				"adversarial mutation attempts changed sealed spool bytes");
#else
		auto spool = make_materialization_private_spool();
		require(!spool &&
					spool.error() ==
						materialization_io_failure{
							materialization_io_failure_kind::invalid_configuration,
							materialization_io_operation::spool_create},
				"unsupported platform did not fail closed before private-spool creation");
#endif
	}

	void preseal_mutation_fails_closed()
	{
#if defined(__linux__) && defined(F_ADD_SEALS) && defined(F_GET_SEALS) && defined(F_SEAL_WRITE) && \
	defined(F_SEAL_GROW) && defined(F_SEAL_SHRINK) && defined(F_SEAL_SEAL)
		{
			auto spool = make_materialization_private_spool();
			require(spool.has_value() && (*spool)->append(bytes("authority")),
					"cannot create private memfd for pre-seal content mutation probe");
			const auto reopened =
				::open(proc_fd_path(locate_materialization_memfd()).c_str(), O_RDWR | O_CLOEXEC);
			require(reopened >= 0, "cannot reopen private memfd before content seal");
			const auto replacement = std::byte{'X'};
			require(::pwrite(reopened, &replacement, 1U, 0) == 1,
					"cannot inject the pre-seal content mutation");
			require(::close(reopened) == 0, "cannot close pre-seal content mutation fd");
			auto sealed = (*spool)->seal();
			require(!sealed &&
						sealed.error() ==
							materialization_io_failure{
								materialization_io_failure_kind::invalid_configuration,
								materialization_io_operation::spool_seal} &&
						!(*spool)->sealed(),
					"pre-seal content mutation escaped the sealed-byte binding");
			require(!(*spool)->seal() && !(*spool)->append(bytes("x")),
					"content-binding failure did not leave the spool terminally poisoned");
		}

		{
			auto spool = make_materialization_private_spool();
			require(spool.has_value() && (*spool)->append(bytes("authority")),
					"cannot create private memfd for pre-seal size mutation probe");
			const auto reopened =
				::open(proc_fd_path(locate_materialization_memfd()).c_str(), O_RDWR | O_CLOEXEC);
			require(reopened >= 0, "cannot reopen private memfd before size seal");
			require(::ftruncate(reopened, 10) == 0, "cannot inject the pre-seal size mutation");
			require(::close(reopened) == 0, "cannot close pre-seal size mutation fd");
			auto sealed = (*spool)->seal();
			require(!sealed &&
						sealed.error() ==
							materialization_io_failure{
								materialization_io_failure_kind::invalid_configuration,
								materialization_io_operation::spool_seal} &&
						!(*spool)->sealed(),
					"pre-seal size mutation escaped the actual-size binding");
			require(!(*spool)->seal() && !(*spool)->append(bytes("x")),
					"size-binding failure did not leave the spool terminally poisoned");
		}
#endif
	}

	void factory_and_seal_fail_closed()
	{
#if defined(__linux__) && defined(F_ADD_SEALS) && defined(F_GET_SEALS) && defined(F_SEAL_WRITE) && \
	defined(F_SEAL_GROW) && defined(F_SEAL_SHRINK) && defined(F_SEAL_SEAL)
		const auto child = ::fork();
		require(child >= 0, "cannot fork the memfd factory failure probe");
		if (child == 0)
		{
			struct rlimit descriptors{};
			if (::getrlimit(RLIMIT_NOFILE, &descriptors) != 0)
				::_exit(2);
			descriptors.rlim_cur = 0;
			if (::setrlimit(RLIMIT_NOFILE, &descriptors) != 0)
				::_exit(3);
			auto spool = make_materialization_private_spool();
			const auto failed_closed = !spool &&
				spool.error() ==
					materialization_io_failure{materialization_io_failure_kind::spool,
											   materialization_io_operation::spool_create};
			::_exit(failed_closed ? 0 : 4);
		}
		int status{};
		while (::waitpid(child, &status, 0) < 0)
			require(errno == EINTR, "cannot wait for the memfd factory failure probe");
		require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
				"memfd creation failure did not fail closed with a typed factory error");

		auto spool = make_materialization_private_spool();
		require(spool.has_value() && (*spool)->append(bytes("mutable")),
				"cannot create private memfd for seal-failure probe");
		const auto descriptor = locate_materialization_memfd();
		const auto path = proc_fd_path(descriptor);
		const auto reopened = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
		require(reopened >= 0, "cannot reopen private memfd for seal-failure probe");
		require(::fcntl(reopened, F_ADD_SEALS, F_SEAL_SEAL) == 0,
				"cannot inject an irreversible partial-seal failure");
		require(::close(reopened) == 0, "cannot close partial-seal failure probe fd");
		auto sealed = (*spool)->seal();
		require(!sealed &&
					sealed.error() ==
						materialization_io_failure{materialization_io_failure_kind::spool,
												   materialization_io_operation::spool_seal} &&
					!(*spool)->sealed(),
				"failed kernel sealing was reported as a logically sealed spool");
#endif
	}
} // namespace

int main()
{
	phase_authentic_failure_matrix();
	boundary_state_machine();
	fragmented_digest_oracle();
	default_limit_stays_chunk_bounded();
	typed_failures();
	anonymous_replay_spool();
	kernel_seal_is_adversarially_immutable();
	preseal_mutation_fails_closed();
	factory_and_seal_fail_closed();
	return 0;
}
