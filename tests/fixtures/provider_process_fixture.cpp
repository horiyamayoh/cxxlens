#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <cxxlens/sdk/provider.hpp>
#include <sys/socket.h>

namespace
{
	using namespace cxxlens::sdk;
	using namespace cxxlens::sdk::provider;

	constexpr std::string_view provider_id = "company.test.process-provider";
	constexpr std::string_view semantic_digest_value =
		"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

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
	auto frames = decode_frame_stream(bytes);
	if (!frames || frames->size() != 5U || frames->at(0U).type != message_type::hello_ack ||
		frames->at(2U).type != message_type::open_task ||
		frames->at(3U).type != message_type::credit)
		return EXIT_FAILURE;

	if (mode == "crash")
		(void)::raise(SIGSEGV);
	if (mode == "timeout")
		std::this_thread::sleep_for(std::chrono::seconds{5});
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
	protocol_writer writer{sink};
	writer.grant_credit({64U * 1024U * 1024U, 65536U});
	const auto identity =
		std::string{mode == "wrong-identity" ? "company.test.other" : provider_id} + "|1.0.0|" +
		std::string{std::getenv("CXXLENS_PROVIDER_BINARY_DIGEST")} + "|" +
		std::string{semantic_digest_value};
	if (!writer.send(message_type::hello, control(identity)))
		return EXIT_FAILURE;
	if (!writer.send(message_type::task_accepted, control("task-1")))
		return EXIT_FAILURE;
	if (mode == "network-check")
	{
		const auto descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
		if (descriptor >= 0)
			return EXIT_FAILURE;
	}
	if (mode == "failed")
	{
		if (!writer.send(message_type::task_failed, control("provider.schema-invalid|fixture")))
			return EXIT_FAILURE;
	}
	else if (!writer.send(message_type::task_complete, control("task-1|complete")))
		return EXIT_FAILURE;
	return EXIT_SUCCESS;
}
