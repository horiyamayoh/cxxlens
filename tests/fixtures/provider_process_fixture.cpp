#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <cxxlens/relations/company_lock_acquire.hpp>
#include <cxxlens/sdk/provider.hpp>
#include <fcntl.h>
#include <sys/socket.h>

#include "provider_timeout_readiness.hpp"
#if defined(__linux__) && defined(__GLIBC__)
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

namespace
{
	using namespace cxxlens::sdk;
	using namespace cxxlens::sdk::provider;
	namespace timeout_fixture = cxxlens::test::provider_timeout;

	constexpr std::string_view provider_id = "company.test.process-provider";

	template <std::unsigned_integral T>
	void append_big_endian(std::vector<std::byte>& output, const T value)
	{
		for (std::size_t index = sizeof(T); index > 0U; --index)
			output.push_back(static_cast<std::byte>(value >> ((index - 1U) * 8U)));
	}

	[[nodiscard]] std::vector<std::byte> control(const std::string_view text)
	{
		std::vector<std::byte> output;
		if (text.size() < 24U)
			output.push_back(static_cast<std::byte>(0x60U | text.size()));
		else if (text.size() <= std::numeric_limits<std::uint8_t>::max())
		{
			output.push_back(std::byte{0x78});
			output.push_back(static_cast<std::byte>(text.size()));
		}
		else
		{
			output.push_back(std::byte{0x79});
			append_big_endian(output, static_cast<std::uint16_t>(text.size()));
		}
		for (const auto byte : text)
			output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
		return output;
	}

	[[nodiscard]] detached_row output_row()
	{
		using relation = cxxlens::company::relations::lock_acquire;
		relation::builder builder;
		if (!builder.set<relation::acquire>(
				detached_cell::typed("company_lock_acquire_id", "lock-acquire:1")) ||
			!builder.set<relation::lock>(detached_cell::typed("company_lock_id", "lock:1")) ||
			!builder.set<relation::source>(detached_cell::typed("source_span_id", "span:1")) ||
			!builder.set<relation::mode>(
				detached_cell{{scalar_kind::open_symbol, "company.lock-mode/1", false},
							  cell_state::present,
							  scalar_value{std::string{"exclusive"}},
							  std::nullopt}) ||
			!builder.set<relation::ordinal>(detached_cell::unsigned_integer(0U)))
			std::exit(EXIT_FAILURE);
		auto row = std::move(builder).finish();
		if (!row)
			std::exit(EXIT_FAILURE);
		const auto& descriptor = relation::descriptor();
		auto identity = derive_domain_identity(descriptor, *row);
		if (!identity)
			std::exit(EXIT_FAILURE);
		row->cells.at("company.lock.acquire.v1.acquire") =
			detached_cell::typed("company_lock_acquire_id", std::move(*identity));
		if (!validate_domain_identity(descriptor, *row))
			std::exit(EXIT_FAILURE);
		return std::move(*row);
	}

	class stdout_sink final : public frame_sink
	{
	  public:
		result<void> write(const std::span<const std::byte> bytes) override
		{
			std::cout.write(reinterpret_cast<const char*>(bytes.data()),
							static_cast<std::streamsize>(bytes.size()));
			return std::cout.good()
				? result<void>{}
				: cxxlens::sdk::unexpected(error{"provider.fixture-write", "stdout", {}});
		}
	};

#if defined(__linux__) && defined(__GLIBC__)
	class child_process_guard final
	{
	  public:
		explicit child_process_guard(const pid_t child) noexcept : child_{child} {}
		child_process_guard(const child_process_guard&) = delete;
		child_process_guard& operator=(const child_process_guard&) = delete;
		~child_process_guard() noexcept
		{
			cleanup();
		}
		void release() noexcept
		{
			child_ = -1;
		}
		void cleanup_now() noexcept
		{
			cleanup();
		}

	  private:
		void cleanup() noexcept
		{
			const auto child = child_;
			child_ = -1;
			if (child <= 0)
				return;
			(void)::kill(child, SIGKILL);
			while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR)
			{
			}
		}

		pid_t child_{};
	};

	[[nodiscard]] bool append_unsigned_decimal(char* const output,
											   const std::size_t capacity,
											   std::size_t& offset,
											   const std::uint64_t value) noexcept
	{
		std::array<char, 32U> digits{};
		std::size_t digit_count{};
		std::uint64_t remaining = value;
		do
		{
			digits[digit_count++] = static_cast<char>('0' + remaining % 10U);
			remaining /= 10U;
		} while (remaining != 0U);
		if (offset + digit_count > capacity)
			return false;
		while (digit_count > 0U)
			output[offset++] = digits[--digit_count];
		return true;
	}

	[[nodiscard]] bool
	write_all(const int descriptor, const char* const bytes, const std::size_t size) noexcept
	{
		std::size_t offset{};
		while (offset < size)
		{
			const auto written = ::write(descriptor, bytes + offset, size - offset);
			if (written > 0)
			{
				offset += static_cast<std::size_t>(written);
				continue;
			}
			if (written < 0 && errno == EINTR)
				continue;
			return false;
		}
		return true;
	}

	[[nodiscard]] std::optional<std::uint64_t> process_start_time() noexcept
	{
		const auto descriptor = ::open("/proc/self/stat", O_RDONLY | O_CLOEXEC);
		if (descriptor < 0)
			return std::nullopt;
		std::array<char, 4096U> buffer{};
		std::size_t size{};
		while (size < buffer.size())
		{
			const auto count = ::read(descriptor, buffer.data() + size, buffer.size() - size);
			if (count > 0)
			{
				size += static_cast<std::size_t>(count);
				continue;
			}
			if (count < 0 && errno == EINTR)
				continue;
			break;
		}
		(void)::close(descriptor);
		if (size == 0U)
			return std::nullopt;
		std::size_t closing_name = size;
		while (closing_name > 0U && buffer[closing_name - 1U] != ')')
			--closing_name;
		if (closing_name == 0U || closing_name + 1U >= size)
			return std::nullopt;
		std::size_t cursor = closing_name;
		while (cursor < size && buffer[cursor] == ' ')
			++cursor;
		if (cursor >= size)
			return std::nullopt;
		++cursor;
		for (std::uint32_t field = 4U; field <= 22U; ++field)
		{
			while (cursor < size && buffer[cursor] == ' ')
				++cursor;
			if (cursor < size && buffer[cursor] == '-')
				++cursor;
			if (cursor >= size || buffer[cursor] < '0' || buffer[cursor] > '9')
				return std::nullopt;
			std::uint64_t value{};
			while (cursor < size && buffer[cursor] >= '0' && buffer[cursor] <= '9')
			{
				const auto digit = static_cast<std::uint64_t>(buffer[cursor] - '0');
				if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U)
					return std::nullopt;
				value = value * 10U + digit;
				++cursor;
			}
			if (field == 22U)
				return value;
		}
		return std::nullopt;
	}

	[[nodiscard]] bool write_process_marker(const std::string_view temporary_path,
											const std::string_view marker_path) noexcept
	{
		const auto start_time = process_start_time();
		if (!start_time)
			return false;
		std::array<char, 64U> line{};
		std::size_t size{};
		if (!append_unsigned_decimal(
				line.data(), line.size(), size, static_cast<std::uint64_t>(::getpid())) ||
			size >= line.size())
			return false;
		line[size++] = ' ';
		if (!append_unsigned_decimal(line.data(), line.size(), size, *start_time))
			return false;
		if (size >= line.size())
			return false;
		line[size++] = '\n';
		const auto descriptor = ::open(
			temporary_path.data(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, S_IRUSR | S_IWUSR);
		if (descriptor < 0)
			return false;
		const bool written = write_all(descriptor, line.data(), size);
		const bool closed = ::close(descriptor) == 0;
		if (!written || !closed)
		{
			(void)::unlink(temporary_path.data());
			return false;
		}
		if (::rename(temporary_path.data(), marker_path.data()) != 0)
		{
			(void)::unlink(temporary_path.data());
			return false;
		}
		return true;
	}

	struct process_identity
	{
		std::uint32_t pid{};
		std::uint64_t start_time{};
	};
	inline constexpr std::size_t internal_identity_bytes = 16U;

	[[nodiscard]] bool write_internal_identity(const int descriptor) noexcept
	{
		const auto start_time = process_start_time();
		if (!start_time ||
			static_cast<unsigned long long>(::getpid()) > std::numeric_limits<std::uint32_t>::max())
			return false;
		std::array<std::byte, internal_identity_bytes> encoded{};
		const auto pid = static_cast<std::uint32_t>(::getpid());
		for (std::size_t index = sizeof(pid); index > 0U; --index)
			encoded[sizeof(pid) - index] = static_cast<std::byte>(pid >> ((index - 1U) * 8U));
		for (std::size_t index = sizeof(*start_time); index > 0U; --index)
			encoded[sizeof(pid) + sizeof(*start_time) - index] =
				static_cast<std::byte>(*start_time >> ((index - 1U) * 8U));
		return write_all(descriptor, reinterpret_cast<const char*>(encoded.data()), encoded.size());
	}

	[[nodiscard]] std::optional<process_identity>
	decode_internal_identity(const std::span<const std::byte> encoded) noexcept
	{
		if (encoded.size() != internal_identity_bytes)
			return std::nullopt;
		std::uint32_t pid{};
		for (std::size_t index{}; index < sizeof(pid); ++index)
			pid = (pid << 8U) |
				static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(encoded[index]));
		std::uint64_t start_time{};
		for (std::size_t index{}; index < sizeof(start_time); ++index)
			start_time = (start_time << 8U) |
				static_cast<std::uint64_t>(
							 std::to_integer<std::uint8_t>(encoded[sizeof(pid) + index]));
		if (pid == 0U || start_time == 0U)
			return std::nullopt;
		return process_identity{pid, start_time};
	}

	[[nodiscard]] bool write_timeout_readiness_record(const std::string_view readiness_path,
													  const std::uint64_t nonce,
													  const process_identity holder,
													  const process_identity sentinel) noexcept
	{
		const auto direct_start_time = process_start_time();
		const auto raw_process_group = ::getpgid(0);
		if (!direct_start_time || raw_process_group <= 0 ||
			static_cast<unsigned long long>(::getpid()) >
				std::numeric_limits<std::uint32_t>::max() ||
			static_cast<unsigned long long>(raw_process_group) >
				std::numeric_limits<std::uint32_t>::max())
			return false;
		const timeout_fixture::readiness_record record{
			nonce,
			static_cast<std::uint32_t>(::getpid()),
			static_cast<std::uint32_t>(raw_process_group),
			*direct_start_time,
			holder.pid,
			holder.start_time,
			sentinel.pid,
			sentinel.start_time};
		const auto encoded = timeout_fixture::encode(record);
		const std::string readiness_path_storage{readiness_path};
		const auto descriptor = ::open(readiness_path_storage.c_str(), O_WRONLY | O_CLOEXEC);
		if (descriptor < 0)
			return false;
		const bool written =
			write_all(descriptor, reinterpret_cast<const char*>(encoded.data()), encoded.size());
		const bool closed = ::close(descriptor) == 0;
		return written && closed;
	}

	void sleep_for_seconds(const std::uint32_t seconds) noexcept
	{
		timespec remaining{static_cast<time_t>(seconds), 0};
		while (::nanosleep(&remaining, &remaining) < 0 && errno == EINTR)
		{
		}
	}
#endif
} // namespace

int main(const int argument_count, const char* const* arguments)
{
	if (argument_count != 2)
		return EXIT_FAILURE;
	const std::string_view mode{arguments[1]};
	std::string input{std::istreambuf_iterator<char>{std::cin}, std::istreambuf_iterator<char>{}};
	std::vector<std::byte> bytes;
	bytes.reserve(input.size());
	for (const auto byte : input)
		bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
	const auto environment = [](const char* name) -> std::optional<std::string>
	{
		const auto* value = std::getenv(name);
		return value == nullptr ? std::nullopt : std::optional<std::string>{value};
	};
	auto expected_manifest = environment("CXXLENS_PROVIDER_MANIFEST");
	auto expected_task_id = environment("CXXLENS_PROVIDER_TASK_ID");
	auto expected_task_digest = environment("CXXLENS_PROVIDER_TASK_INPUT_DIGEST");
	auto expected_invocation = environment("CXXLENS_PROVIDER_NORMALIZED_INVOCATION_DIGEST");
	auto expected_toolchain = environment("CXXLENS_PROVIDER_TOOLCHAIN_DIGEST");
	auto expected_environment = environment("CXXLENS_PROVIDER_ENVIRONMENT_DIGEST");
	auto expected_major = environment("CXXLENS_PROVIDER_PROTOCOL_MAJOR");
	auto expected_minor = environment("CXXLENS_PROVIDER_PROTOCOL_MINOR");
	if (!expected_manifest || !expected_task_id || !expected_task_digest || !expected_invocation ||
		!expected_toolchain || !expected_environment || !expected_major || !expected_minor)
		return EXIT_FAILURE;
	protocol_limits input_limits;
	const auto parse_version = [](const std::string_view text, std::uint16_t& output)
	{
		const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), output);
		return error == std::errc{} && end == text.data() + text.size();
	};
	if (!parse_version(*expected_major, input_limits.protocol_major) ||
		!parse_version(*expected_minor, input_limits.maximum_minor) ||
		input_limits.protocol_major != protocol_v2_major ||
		input_limits.maximum_minor != protocol_v2_minor)
		return EXIT_FAILURE;
	input_limits.minimum_minor = protocol_v2_minor;
	auto frames = decode_frame_stream(bytes, input_limits);
	if (!frames)
		return EXIT_FAILURE;
	auto validated = validate_host_transcript(*frames,
											  {*expected_manifest,
											   {*expected_task_id,
												*expected_task_digest,
												*expected_invocation,
												*expected_toolchain,
												*expected_environment},
											   input_limits});
	if (!validated)
		return EXIT_FAILURE;
	const std::string task_id{validated->task.task_id};

	if (mode == "crash")
		(void)::raise(SIGSEGV);
	if (mode == "timeout")
		std::this_thread::sleep_for(std::chrono::seconds{5});

#if defined(__linux__) && defined(__GLIBC__)
	constexpr std::string_view timeout_grandchild_prefix = "timeout-grandchild:";
	if (mode.starts_with(timeout_grandchild_prefix))
	{
		const auto fixture = mode.substr(timeout_grandchild_prefix.size());
		const auto marker_separator = fixture.find('|');
		const auto readiness_separator = marker_separator == std::string_view::npos
			? std::string_view::npos
			: fixture.find('|', marker_separator + 1U);
		if (marker_separator == std::string_view::npos ||
			readiness_separator == std::string_view::npos)
			return EXIT_FAILURE;
		const auto marker_path = fixture.substr(0U, marker_separator);
		const auto readiness_path =
			fixture.substr(marker_separator + 1U, readiness_separator - marker_separator - 1U);
		const auto nonce_text = fixture.substr(readiness_separator + 1U);
		std::uint64_t nonce{};
		const auto [nonce_end, nonce_error] =
			std::from_chars(nonce_text.data(), nonce_text.data() + nonce_text.size(), nonce);
		if (marker_path.empty() || readiness_path.empty() || nonce_text.empty() ||
			nonce_error != std::errc{} || nonce_end != nonce_text.data() + nonce_text.size() ||
			nonce == 0U)
			return EXIT_FAILURE;
		const std::string holder_marker{std::string{marker_path} + ".holder"};
		const std::string holder_marker_temporary{holder_marker + ".tmp"};
		const std::string sentinel_marker{std::string{marker_path} + ".sentinel"};
		const std::string sentinel_marker_temporary{sentinel_marker + ".tmp"};
		std::array<int, 2U> ready_pipe{-1, -1};
		if (::pipe(ready_pipe.data()) != 0)
			return EXIT_FAILURE;
		const auto holder = ::fork();
		if (holder < 0)
			return EXIT_FAILURE;
		if (holder == 0)
		{
			(void)::close(ready_pipe[0U]);
			const auto sentinel = ::fork();
			if (sentinel < 0)
				::_exit(EXIT_FAILURE);
			if (sentinel == 0)
			{
				(void)::close(STDOUT_FILENO);
				(void)::close(STDERR_FILENO);
				if (!write_process_marker(sentinel_marker_temporary, sentinel_marker))
					::_exit(EXIT_FAILURE);
				if (!write_internal_identity(ready_pipe[1U]))
					::_exit(EXIT_FAILURE);
				(void)::close(ready_pipe[1U]);
				sleep_for_seconds(30U);
				::_exit(EXIT_SUCCESS);
			}
			child_process_guard sentinel_guard{sentinel};
			if (!write_process_marker(holder_marker_temporary, holder_marker))
			{
				sentinel_guard.cleanup_now();
				::_exit(EXIT_FAILURE);
			}
			if (!write_internal_identity(ready_pipe[1U]))
			{
				sentinel_guard.cleanup_now();
				::_exit(EXIT_FAILURE);
			}
			(void)::close(ready_pipe[1U]);
			sentinel_guard.release();
			sleep_for_seconds(30U);
			::_exit(EXIT_SUCCESS);
		}
		child_process_guard holder_guard{holder};
		(void)::close(ready_pipe[1U]);
		std::array<std::byte, internal_identity_bytes * 2U> ready{};
		std::size_t received{};
		while (received < ready.size())
		{
			const auto count =
				::read(ready_pipe[0U], ready.data() + received, ready.size() - received);
			if (count > 0)
				received += static_cast<std::size_t>(count);
			else if (count < 0 && errno == EINTR)
				continue;
			else
				break;
		}
		(void)::close(ready_pipe[0U]);
		const auto first_identity = received == ready.size()
			? decode_internal_identity(
				  std::span<const std::byte>{ready}.first(internal_identity_bytes))
			: std::nullopt;
		const auto second_identity = received == ready.size()
			? decode_internal_identity(
				  std::span<const std::byte>{ready}.last(internal_identity_bytes))
			: std::nullopt;
		if (received != ready.size() || !first_identity || !second_identity)
		{
			std::cerr << "timeout-grandchild internal readiness record bytes=" << received << '\n';
			return EXIT_FAILURE;
		}
		const auto holder_pid = static_cast<std::uint32_t>(holder);
		const process_identity holder_identity =
			first_identity->pid == holder_pid ? *first_identity : *second_identity;
		const process_identity sentinel_identity =
			first_identity->pid == holder_pid ? *second_identity : *first_identity;
		if (holder_identity.pid != holder_pid || sentinel_identity.pid == holder_pid ||
			sentinel_identity.pid == 0U ||
			!write_timeout_readiness_record(
				readiness_path, nonce, holder_identity, sentinel_identity))
		{
			std::cerr << "timeout-grandchild authenticated readiness record failed\n";
			return EXIT_FAILURE;
		}
		holder_guard.release();
		// Keep the leader alive with its descendants so the host observes a typed timeout
		// rather than an EOF/truncated transcript before process-group cleanup.
		sleep_for_seconds(30U);
		return EXIT_SUCCESS;
	}
#endif
	if (mode == "output-limit")
	{
		std::cout << std::string(1024U * 1024U, 'x');
		return EXIT_SUCCESS;
	}
	if (mode == "malformed")
	{
		std::cout << "not-a-provider-frame";
		return EXIT_SUCCESS;
	}

	stdout_sink sink;
	protocol_limits output_limits;
	output_limits.protocol_major = protocol_v2_major;
	output_limits.minimum_minor = protocol_v2_minor;
	output_limits.maximum_minor = protocol_v2_minor;
	protocol_writer writer{sink, output_limits};
	writer.grant_credit({64U * 1024U * 1024U, 65536U});
	auto identity = *expected_manifest;
	if (mode == "wrong-identity")
		identity.replace(identity.find(provider_id), provider_id.size(), "company.test.other");
	const auto hello_flags =
		mode == "bad-eos" ? static_cast<std::uint16_t>(frame_flag::end_of_stream) : std::uint16_t{};
	if (!writer.send(message_type::hello, control(identity), {}, hello_flags))
		return EXIT_FAILURE;
	if (mode == "minimal")
	{
		auto complete = encode_task_complete_metadata({task_id});
		return complete && writer.send(message_type::task_complete, *complete) ? EXIT_SUCCESS
																			   : EXIT_FAILURE;
	}
	if (!writer.send(message_type::schema_negotiate, frames->at(1U).control))
		return EXIT_FAILURE;
	if (mode == "failed" || mode == "failure-success" || mode == "failure-unknown")
	{
		const auto reason = mode == "failure-success"
			? "provider.success"
			: (mode == "failure-unknown" ? "provider.unknown-reason" : "provider.schema-invalid");
		auto failed = encode_task_failed_metadata({reason, task_id, "fixture"});
		return failed && writer.send(message_type::task_failed, *failed) ? EXIT_SUCCESS
																		 : EXIT_FAILURE;
	}
	if (mode == "invalid-utf8")
	{
		const std::array invalid_control{std::byte{0x61}, std::byte{0x80}};
		if (!writer.send(message_type::task_accepted, invalid_control))
			return EXIT_FAILURE;
		auto failed = encode_task_failed_metadata({"provider.schema-invalid", task_id, "fixture"});
		return failed && writer.send(message_type::task_failed, *failed) ? EXIT_SUCCESS
																		 : EXIT_FAILURE;
	}
	if (mode != "missing-accepted")
	{
		auto accepted = encode_task_accepted_metadata(
			{std::string{provider_id}, "1.0.0", mode == "wrong-task" ? "other-task" : task_id});
		if (!accepted || !writer.send(message_type::task_accepted, *accepted))
			return EXIT_FAILURE;
	}
	if (mode == "missing-accepted")
	{
		auto complete = encode_task_complete_metadata({task_id});
		return complete && writer.send(message_type::task_complete, *complete) ? EXIT_SUCCESS
																			   : EXIT_FAILURE;
	}
	if (mode == "nul-control")
	{
		constexpr char nul_code[]{"provider.schema-invalid\0suffix"};
		auto failed = encode_task_failed_metadata(
			{std::string{nul_code, sizeof(nul_code) - 1U}, task_id, "fixture"});
		return failed && writer.send(message_type::task_failed, *failed) ? EXIT_SUCCESS
																		 : EXIT_FAILURE;
	}
	if (mode == "provider-credit" || mode == "provider-open-task" || mode == "provider-batch-ack")
	{
		const auto type = mode == "provider-credit"
			? message_type::credit
			: (mode == "provider-open-task" ? message_type::open_task : message_type::batch_ack);
		if (!writer.send(type, control("provider-forbidden")))
			return EXIT_FAILURE;
		auto complete = encode_task_complete_metadata({task_id});
		return complete && writer.send(message_type::task_complete, *complete) ? EXIT_SUCCESS
																			   : EXIT_FAILURE;
	}
	if (mode == "optional-extension" &&
		!writer.send(static_cast<message_type>(65000U),
					 control("company.optional-extension"),
					 {},
					 static_cast<std::uint16_t>(frame_flag::optional_extension)))
		return EXIT_FAILURE;
	if (mode == "network-check")
	{
		const auto descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
		if (descriptor >= 0)
			return EXIT_FAILURE;
	}
	if (mode == "fd-clean")
		for (int descriptor = 3; descriptor < 1024; ++descriptor)
		{
			errno = 0;
			if (::fcntl(descriptor, F_GETFD) >= 0 || errno != EBADF)
				return EXIT_FAILURE;
		}

	const auto& schema = cxxlens::company::relations::lock_acquire::descriptor();
	const auto descriptor =
		std::string{mode == "unknown-descriptor" ? "company.unknown.v1" : schema.id};
	const auto descriptor_digest = mode == "unknown-descriptor"
		? std::string{"sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"}
		: schema.descriptor_digest;
	auto begin = encode_batch_begin_metadata(
		{task_id, descriptor, descriptor_digest, "dependency-1", "atomic-1", "batch-1"});
	if (!begin || !writer.send(message_type::batch_begin, *begin))
		return EXIT_FAILURE;
	if (mode == "unsealed-batch")
	{
		auto complete = encode_task_complete_metadata({task_id});
		return complete && writer.send(message_type::task_complete, *complete) ? EXIT_SUCCESS
																			   : EXIT_FAILURE;
	}
	const auto row = output_row();
	std::vector<batch_column_summary> summaries;
	std::vector<std::string> chunk_digests;
	std::vector<encoded_column_chunk> encoded_chunks;
	for (std::size_t index = 0U; index < schema.columns.size(); ++index)
	{
		const auto& column = schema.columns[index];
		const auto encoding = column.type.scalar == scalar_kind::boolean
			? "fixed-width-bool-u8"
			: (column.type.scalar == scalar_kind::signed_integer
				   ? "fixed-width-i64-le"
				   : (column.type.scalar == scalar_kind::unsigned_integer
						  ? "fixed-width-u64-le"
						  : ((column.type.scalar == scalar_kind::open_symbol ||
							  column.type.scalar == scalar_kind::closed_symbol)
								 ? "dictionary-index-u32-le"
								 : ((column.type.scalar == scalar_kind::bytes ||
									 column.type.scalar == scalar_kind::set)
										? "bytes-offsets-u32-le"
										: "utf8-offsets-u32-le"))));
		const auto cell = row.cells.contains(column.id) ? row.cells.at(column.id)
														: detached_cell::absent(column.type);
		column_chunk_record chunk{task_id,
								  "dependency-1",
								  "atomic-1",
								  "batch-1",
								  schema.id,
								  schema.descriptor_digest,
								  column.id,
								  0U,
								  1U,
								  0U,
								  encoding,
								  {cell},
								  {}};
		auto encoded = encode_column_chunk(chunk, column);
		if (!encoded)
			return EXIT_FAILURE;
		if (mode == "bad-column" && index == 0U)
			encoded->payload.back() ^= std::byte{1U};
		summaries.push_back({column.id, encoded->payload.size(), 1U});
		chunk_digests.push_back(encoded->chunk_digest);
		encoded_chunks.push_back(std::move(*encoded));
	}
	if (mode == "reordered-column")
		std::swap(encoded_chunks[0U], encoded_chunks[1U]);
	for (const auto& encoded : encoded_chunks)
		if (!writer.send(message_type::column_chunk, encoded.control, encoded.payload))
			return EXIT_FAILURE;
	columnar_batch_end terminal{task_id,
								"dependency-1",
								"atomic-1",
								"batch-1",
								schema.id,
								schema.descriptor_digest,
								1U,
								std::move(summaries),
								std::move(chunk_digests),
								{}};
	terminal.batch_digest = columnar_batch_digest(terminal);
	auto encoded_terminal = encode_columnar_batch_end(terminal);
	if (!encoded_terminal)
		return EXIT_FAILURE;
	if (mode == "inconsistent-batch")
		encoded_terminal->payload.back() ^= std::byte{1U};
	if (mode == "column-length-mismatch")
		encoded_terminal->payload[10U + schema.columns.front().id.size()] ^= std::byte{1U};
	if (!writer.send(message_type::batch_end, encoded_terminal->control, encoded_terminal->payload))
		return EXIT_FAILURE;
	const std::array coverage{coverage_unit{mode == "incomplete-coverage" ? "project" : "task",
											mode == "incomplete-coverage" ? "catalog" : task_id,
											"covered",
											{}}};
	const std::span<const unresolved_item> unresolved;
	const std::span<const evidence_item> evidence;
	auto coverage_control = encode_coverage_metadata(coverage);
	auto unresolved_control = encode_unresolved_metadata(unresolved);
	auto evidence_control = encode_evidence_metadata(evidence);
	const auto terminal_flags = mode == "success-eos"
		? static_cast<std::uint16_t>(frame_flag::end_of_stream)
		: std::uint16_t{};
	auto complete_control =
		encode_task_complete_metadata({mode == "wrong-complete-task" ? "other-task" : task_id});
	// Protocol 2 requires every control value to be canonical CBOR.  Use a canonical empty map to
	// exercise a task_complete occurrence with no typed completion fields; an empty byte span would
	// be rejected by the encoder before the malformed provider transcript reaches the host.
	constexpr std::array missing_complete_control{std::byte{0xa0}};
	const std::span<const std::byte> final_control = mode == "missing-complete-control"
		? std::span<const std::byte>{missing_complete_control}
		: std::span<const std::byte>{*complete_control};
	if (!coverage_control || !unresolved_control || !evidence_control || !complete_control ||
		!writer.send(message_type::coverage_chunk, *coverage_control) ||
		!writer.send(message_type::unresolved_chunk, *unresolved_control) ||
		!writer.send(message_type::progress, *evidence_control) ||
		!writer.send(message_type::task_complete, final_control, {}, terminal_flags))
		return EXIT_FAILURE;
	return EXIT_SUCCESS;
}
