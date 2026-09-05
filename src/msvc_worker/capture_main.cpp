#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include "msvc_capture_bundle.hpp"
#include "runtime/msvc_capture_file_port.hpp"
#ifdef _WIN32
#include "msvc_capture_session.hpp"
#include "runtime/msvc_process_port.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

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

	int vector_command(const int argc, const char* const argv[])
	{
		if (argc != 3)
			return EXIT_FAILURE;
		auto encoded =
			cxxlens::application_analysis_worker::encode_msvc_capture_bundle(vector_input());
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

#ifdef _WIN32
	[[nodiscard]] std::string utf8(const std::wstring_view value)
	{
		if (value.empty())
			return {};
		const auto size = WideCharToMultiByte(CP_UTF8,
											  WC_ERR_INVALID_CHARS,
											  value.data(),
											  static_cast<int>(value.size()),
											  nullptr,
											  0,
											  nullptr,
											  nullptr);
		if (size <= 0)
			return {};
		std::string output(static_cast<std::size_t>(size), '\0');
		if (WideCharToMultiByte(CP_UTF8,
								WC_ERR_INVALID_CHARS,
								value.data(),
								static_cast<int>(value.size()),
								output.data(),
								size,
								nullptr,
								nullptr) != size)
			return {};
		return output;
	}
#endif

} // namespace

#ifndef _WIN32
int main(const int argc, const char* const argv[])
{
	return vector_command(argc, argv);
}
#else
int wmain(const int argc, wchar_t* argv[])
{
	if (argc == 3 &&
		(std::wstring_view{argv[1]} == L"--check-vector" ||
		 std::wstring_view{argv[1]} == L"--emit-vector"))
	{
		const auto mode = utf8(argv[1]);
		const auto path = utf8(argv[2]);
		if (mode.empty() || path.empty())
			return EXIT_FAILURE;
		const char* narrow[]{"cxxlens-msvc-capture", mode.c_str(), path.c_str()};
		return vector_command(3, narrow);
	}
	auto compiler = cxxlens::application_analysis_worker::configured_msvc_compiler();
	if (!compiler)
	{
		std::cerr << compiler.error().code << ':' << compiler.error().field << ':'
				  << compiler.error().detail << '\n';
		return EXIT_FAILURE;
	}
	std::vector<std::wstring> arguments;
	arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0U);
	for (int index{1}; index < argc; ++index)
		arguments.emplace_back(argv[index]);
	auto result = cxxlens::application_analysis_worker::capture_msvc_command(*compiler, arguments);
	if (!result)
	{
		std::cerr << result.error().code << ':' << result.error().field << ':'
				  << result.error().detail << '\n';
		return EXIT_FAILURE;
	}
	if (result->capture_error)
		std::cerr << result->capture_error->code << ':' << result->capture_error->field << ':'
				  << result->capture_error->detail << '\n';
	return static_cast<int>(result->compiler_exit_code);
}
#endif
