#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <cxxlens/sdk/provider.hpp>

int main(int argc, char** argv)
{
	if (argc != 5)
	{
		std::cerr << "usage: cxxlens-provider-scaffold OUTPUT PROVIDER_ID portable|clang22-native "
					 "RELATION\n";
		return 2;
	}
	const std::filesystem::path output{argv[1]};
	auto files = cxxlens::sdk::provider::make_scaffold({argv[2], argv[3], argv[4]});
	if (!files)
	{
		std::cerr << files.error().code << ':' << files.error().field << '\n';
		return 1;
	}
	if (std::filesystem::exists(output) && !std::filesystem::is_empty(output))
	{
		std::cerr << "provider.scaffold-target-not-empty\n";
		return 1;
	}
	for (const auto& file : *files)
	{
		const auto path = output / file.relative_path;
		std::filesystem::create_directories(path.parent_path());
		std::ofstream stream{path, std::ios::binary | std::ios::trunc};
		stream << file.content;
		if (!stream)
		{
			std::cerr << "provider.scaffold-write-failed:" << file.relative_path << '\n';
			return 1;
		}
	}
	std::cout << "generated " << files->size() << " files for " << argv[2] << '\n';
	return 0;
}
