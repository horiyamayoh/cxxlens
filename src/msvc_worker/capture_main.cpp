#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include "msvc_capture_bundle.hpp"
#include "runtime/msvc_capture_file_port.hpp"

namespace
{
	[[nodiscard]] std::vector<std::byte> bytes(const std::string_view value)
	{
		std::vector<std::byte> output;
		output.reserve(value.size());
		for (const auto byte : value)
			output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
		return output;
	}

	[[nodiscard]] std::string digest(const char digit)
	{
		return "sha256:" + std::string(64U, digit);
	}

	[[nodiscard]] cxxlens::application_analysis_worker::msvc_capture_input vector_input()
	{
		using namespace cxxlens::application_analysis_worker;
		msvc_capture_input input;
		input.project_id = "project:msvc-canonical-vector";
		input.canonical_project_root = "C:\\cxxlens vector\\unicode-project";
		input.canonical_working_directory = "C:\\cxxlens vector\\unicode-project\\build";
		input.canonical_compiler_path =
			"C:\\VS\\VC\\Tools\\MSVC\\14.51.36231\\bin\\Hostx64\\x64\\cl.exe";
		input.compiler_binary_digest = digest('1');
		input.windows_sdk_root = "C:\\Program Files (x86)\\Windows Kits\\10";
		input.abi_digest = digest('2');
		input.builtin_headers_digest = digest('3');
		input.builtin_macros_digest = digest('4');
		input.include_search_digest = digest('5');
		input.original_arguments = {
			input.canonical_compiler_path,
			"@C:\\cxxlens vector\\unicode-project\\build\\options.rsp",
			"C:\\cxxlens vector\\unicode-project\\src\\main.cpp",
			"/c",
		};
		input.main_source = {"C:\\cxxlens vector\\unicode-project\\src\\main.cpp",
							 bytes("#include <model.hpp>\nint main(){return model();}\n"),
							 "main",
							 "utf8"};
		input.dependency_sources = {{"C:\\cxxlens vector\\unicode-project\\include\\model.hpp",
									 bytes("inline int model(){return 0;}\n"),
									 "header",
									 "utf8"}};
		input.response_files = {{"C:\\cxxlens vector\\unicode-project\\build\\options.rsp",
								 bytes("/std:c++latest /DUNICODE=1"),
								 std::nullopt}};
		return input;
	}

} // namespace

int main(const int argc, const char* const argv[])
{
	if (argc != 3)
	{
		std::cerr << "usage: cxxlens-msvc-capture --check-vector <hex-file> | "
					 "--emit-vector <bundle-file>\n";
		return EXIT_FAILURE;
	}
	auto encoded = cxxlens::application_analysis_worker::encode_msvc_capture_bundle(vector_input());
	if (!encoded)
	{
		std::cerr << encoded.error().code << ':' << encoded.error().field << ':'
				  << encoded.error().detail << '\n';
		return EXIT_FAILURE;
	}
	if (std::string_view{argv[1]} == "--emit-vector")
		return cxxlens::application_analysis_worker::write_worker_binary_file(argv[2], *encoded)
			? EXIT_SUCCESS
			: EXIT_FAILURE;
	if (std::string_view{argv[1]} == "--check-vector")
	{
		auto expected = cxxlens::application_analysis_worker::read_worker_text_file(argv[2]);
		if (!expected)
			return EXIT_FAILURE;
		if (*expected != cxxlens::sdk::content_digest(*encoded) + '\n')
		{
			std::cerr << "application-analysis.canonical-vector-mismatch\n";
			return EXIT_FAILURE;
		}
		return EXIT_SUCCESS;
	}
	return EXIT_FAILURE;
}
