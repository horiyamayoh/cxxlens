#include <array>
#include <cstddef>

#include <cxxlens/sdk.hpp>

int main()
{
	constexpr std::array invalid{std::byte{0xff}};
	auto decoded = cxxlens::sdk::decode_capture_bundle(invalid);
	return !decoded && decoded.error().code == "application-analysis.capture-invalid" ? 0 : 1;
}
