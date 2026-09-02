#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <vector>

#include <cxxlens/sdk/application_analysis.hpp>

namespace
{
	int run(const int argc, char** argv)
	{
		if (argc != 2)
			return 2;
		std::ifstream input{argv[1], std::ios::binary | std::ios::ate};
		if (!input)
			return 3;
		const auto end = input.tellg();
		if (end <= std::streampos{})
			return 4;
		const auto byte_count = static_cast<std::uint64_t>(static_cast<std::streamoff>(end));
		constexpr std::uint64_t maximum_test_bundle_bytes{std::uint64_t{1024U} * 1024U};
		if (byte_count > maximum_test_bundle_bytes ||
			byte_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
			return 4;
		std::vector<std::byte> bytes(static_cast<std::size_t>(byte_count));
		input.seekg(0);
		input.read(reinterpret_cast<char*>(bytes.data()),
				   static_cast<std::streamsize>(bytes.size()));
		if (!input)
			return 5;
		auto bundle = cxxlens::sdk::decode_capture_bundle(bytes);
		if (!bundle)
		{
			std::cerr << bundle.error().code << ':' << bundle.error().field << ':'
					  << bundle.error().detail << '\n';
			return 6;
		}
		return bundle->production_compiler() == "gcc-16.2.0" &&
				bundle->capture_adapter() == "compile-commands" &&
				bundle->target_abi() == "x86_64-linux-gnu" &&
				bundle->project_id() == "project:cli-capture" &&
				bundle->compile_unit_count() == 1U && !bundle->gaps().empty()
			? 0
			: 7;
	}
} // namespace

int main(const int argc, char** argv) noexcept
{
	try
	{
		return run(argc, argv);
	}
	catch (...)
	{
		return 8;
	}
}
