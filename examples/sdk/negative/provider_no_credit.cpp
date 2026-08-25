#include <array>

#include <cxxlens/sdk/provider.hpp>

namespace
{
	class sink final : public cxxlens::sdk::provider::frame_sink
	{
		cxxlens::sdk::result<void> write(std::span<const std::byte>) override
		{
			return {};
		}
	};
} // namespace

int main()
{
	sink output;
	cxxlens::sdk::provider::protocol_writer writer{output};
	const std::array<std::byte, 2U> hello_control{std::byte{0x61U}, std::byte{0x78U}};
	auto rejected = writer.send(cxxlens::sdk::provider::message_type::hello, hello_control);
	return !rejected && rejected.error().code == "provider.backpressure" ? 0 : 1;
}
