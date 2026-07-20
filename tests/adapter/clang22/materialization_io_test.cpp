#include "llvm/clang22/materialization_io.hpp"

#include <algorithm>
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
			if (fail_update_)
				return materialization_io_failure{materialization_io_failure_kind::hash,
												  materialization_io_operation::digest_update};
			data_.insert(data_.end(), input.begin(), input.end());
			return {};
		}

		materialization_io_result<std::string> finish() override
		{
			if (fail_finish_)
				return materialization_io_failure{materialization_io_failure_kind::hash,
												  materialization_io_operation::digest_finalize};
			return cxxlens::sdk::content_digest(data_);
		}

		std::vector<std::byte> data_;
		bool fail_update_{};
		bool fail_finish_{};
	};

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
		require(!failed && failed.error().kind == materialization_io_failure_kind::read &&
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
		digest.fail_update_ = true;
		failed = capture_bounded_input(reader, spool, digest, {4U, 2U});
		require(!failed &&
					failed.error() ==
						materialization_io_failure{materialization_io_failure_kind::hash,
												   materialization_io_operation::digest_update},
				"incremental hash update failure escaped its private port");

		reader = fragmented_reader{bytes("abc"), {2U}};
		spool = memory_spool{};
		digest = injectable_digest{};
		digest.fail_finish_ = true;
		failed = capture_bounded_input(reader, spool, digest, {4U, 2U});
		require(!failed && spool.sealed_ &&
					failed.error() ==
						materialization_io_failure{materialization_io_failure_kind::hash,
												   materialization_io_operation::digest_finalize},
				"incremental hash finalization failure escaped its private port");
	}
} // namespace

int main()
{
	boundary_state_machine();
	fragmented_digest_oracle();
	default_limit_stays_chunk_bounded();
	typed_failures();
	return 0;
}
