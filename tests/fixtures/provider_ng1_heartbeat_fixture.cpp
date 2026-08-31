#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "protocol_v2/cbor.hpp"
#include "protocol_v2/codec.hpp"

#if defined(__linux__) && defined(__GLIBC__)
#include <csignal>

#include <fcntl.h>
#include <unistd.h>
#endif

namespace
{
	[[nodiscard]] std::optional<std::string> environment(const char* name)
	{
		const auto* value = std::getenv(name);
		return value == nullptr ? std::nullopt : std::optional<std::string>{value};
	}

#if defined(__linux__) && defined(__GLIBC__)
	[[nodiscard]] std::optional<std::uint64_t> process_start_time() noexcept
	{
		const auto descriptor = ::open("/proc/self/stat", O_RDONLY | O_CLOEXEC);
		if (descriptor < 0)
			return std::nullopt;
		char buffer[4096]{};
		const auto count = ::read(descriptor, buffer, sizeof(buffer));
		(void)::close(descriptor);
		if (count <= 0)
			return std::nullopt;
		const std::string_view text{buffer, static_cast<std::size_t>(count)};
		const auto name_end = text.rfind(')');
		if (name_end == std::string_view::npos)
			return std::nullopt;
		std::size_t begin = name_end + 1U;
		while (begin < text.size() && text[begin] == ' ')
			++begin;
		for (std::uint32_t field = 3U; field < 22U; ++field)
		{
			const auto end = text.find(' ', begin);
			if (end == std::string_view::npos)
				return std::nullopt;
			begin = end + 1U;
		}
		const auto end = text.find(' ', begin);
		const auto value =
			text.substr(begin, end == std::string_view::npos ? text.size() - begin : end - begin);
		std::uint64_t output{};
		const auto parsed = std::from_chars(value.data(), value.data() + value.size(), output);
		return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size()
			? std::optional<std::uint64_t>{output}
			: std::nullopt;
	}

	[[nodiscard]] bool write_all(const std::span<const std::byte> bytes) noexcept
	{
		std::size_t offset{};
		while (offset < bytes.size())
		{
			const auto written =
				::write(STDOUT_FILENO, bytes.data() + offset, bytes.size() - offset);
			if (written > 0)
				offset += static_cast<std::size_t>(written);
			else if (written < 0 && errno == EINTR)
				continue;
			else
				return false;
		}
		return true;
	}
#endif
} // namespace

int main(const int argument_count, const char* const* arguments)
{
#if defined(__linux__) && defined(__GLIBC__)
	if (argument_count != 2)
		return 2;
	const auto provider = environment("CXXLENS_PROVIDER_ID");
	const auto task = environment("CXXLENS_PROVIDER_TASK_ID");
	const auto stream_text = environment("CXXLENS_PROVIDER_NG1_STREAM_ID");
	std::uint64_t stream{};
	const auto parsed = stream_text
		? std::from_chars(stream_text->data(), stream_text->data() + stream_text->size(), stream)
		: std::from_chars_result{};
	if (!provider || !task || !stream_text || stream == 0U || parsed.ec != std::errc{} ||
		parsed.ptr != stream_text->data() + stream_text->size())
		return 3;

	auto control = cxxlens::protocol_v2::cbor::encode(cxxlens::protocol_v2::cbor::map{
		{"provider_id", *provider},
		{"provider_version", "1.0.0"},
		{"schema", "cxxlens.provider-control.task-accepted.v1"},
		{"task_id", *task},
	});
	if (!control)
		return 4;
	cxxlens::protocol_v2::limits limits;
	limits.minimum_minor = 0U;
	limits.maximum_minor = 0U;
	auto encoded =
		cxxlens::protocol_v2::encode_frame({cxxlens::protocol_v2::message_type::task_accepted,
											cxxlens::protocol_v2::protocol_major,
											0U,
											0U,
											stream,
											0U,
											std::move(*control),
											{},
											{},
											{}},
										   limits);
	if (!encoded || !write_all(*encoded))
		return 5;

	const auto descendant = ::fork();
	if (descendant < 0)
		return 6;
	if (descendant == 0)
	{
		const auto started = process_start_time();
		std::ofstream marker{arguments[1]};
		if (!started || !marker.good() || !(marker << ::getpid() << ' ' << *started << '\n'))
			::_exit(7);
		marker.close();
		for (;;)
			(void)::pause();
	}
	for (;;)
		(void)::pause();
#else
	(void)argument_count;
	(void)arguments;
	return 77;
#endif
}
