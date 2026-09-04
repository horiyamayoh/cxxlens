#include "msvc_capture_session.hpp"

#ifdef _WIN32

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <new>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "msvc_capture_bundle.hpp"
#include "msvc_source_dependencies.hpp"
#include "runtime/msvc_capture_file_port.hpp"
#include "runtime/msvc_process_port.hpp"

namespace cxxlens::application_analysis_worker
{
	namespace
	{
		constexpr std::size_t maximum_file_bytes = std::size_t{48U} * 1024U * 1024U;

		[[nodiscard]] sdk::error capture_error(std::string field, std::string detail)
		{
			return {"application-analysis.msvc-capture-unavailable",
					std::move(field),
					std::move(detail)};
		}

		[[nodiscard]] sdk::result<std::wstring> environment(const wchar_t* name)
		{
			const auto size = GetEnvironmentVariableW(name, nullptr, 0U);
			if (size == 0U)
				return sdk::unexpected(capture_error("configuration", "missing"));
			std::wstring value(size, L'\0');
			const auto written = GetEnvironmentVariableW(name, value.data(), size);
			if (written == 0U || written >= size)
				return sdk::unexpected(capture_error("configuration", "read"));
			value.resize(written);
			return value;
		}

		[[nodiscard]] sdk::result<std::string> utf8(const std::wstring_view value)
		{
			if (value.empty())
				return std::string{};
			if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
				return sdk::unexpected(capture_error("path", "utf16-length"));
			const auto size = WideCharToMultiByte(CP_UTF8,
												  WC_ERR_INVALID_CHARS,
												  value.data(),
												  static_cast<int>(value.size()),
												  nullptr,
												  0,
												  nullptr,
												  nullptr);
			if (size <= 0)
				return sdk::unexpected(capture_error("path", "utf16-invalid"));
			std::string output(static_cast<std::size_t>(size), '\0');
			if (WideCharToMultiByte(CP_UTF8,
									WC_ERR_INVALID_CHARS,
									value.data(),
									static_cast<int>(value.size()),
									output.data(),
									size,
									nullptr,
									nullptr) != size)
				return sdk::unexpected(capture_error("path", "utf16-conversion"));
			return output;
		}

		[[nodiscard]] sdk::result<std::wstring> utf16(const std::string_view value)
		{
			if (value.empty())
				return std::wstring{};
			if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
				return sdk::unexpected(capture_error("path", "utf8-length"));
			const auto size = MultiByteToWideChar(CP_UTF8,
												  MB_ERR_INVALID_CHARS,
												  value.data(),
												  static_cast<int>(value.size()),
												  nullptr,
												  0);
			if (size <= 0)
				return sdk::unexpected(capture_error("path", "utf8-invalid"));
			std::wstring output(static_cast<std::size_t>(size), L'\0');
			if (MultiByteToWideChar(CP_UTF8,
									MB_ERR_INVALID_CHARS,
									value.data(),
									static_cast<int>(value.size()),
									output.data(),
									size) != size)
				return sdk::unexpected(capture_error("path", "utf8-conversion"));
			return output;
		}

		[[nodiscard]] bool under(const std::wstring_view path, const std::wstring_view root)
		{
			return path.size() >= root.size() &&
				_wcsnicmp(path.data(), root.data(), root.size()) == 0 &&
				(path.size() == root.size() || root.back() == L'\\' || path[root.size()] == L'\\');
		}

		[[nodiscard]] bool source_dependencies_option(const std::wstring_view value) noexcept
		{
			constexpr std::wstring_view option{L"/sourceDependencies"};
			return value.size() >= option.size() &&
				_wcsnicmp(value.data(), option.data(), option.size()) == 0;
		}

		[[nodiscard]] sdk::result<std::string> configured_project_id()
		{
			auto value = environment(L"CXXLENS_MSVC_PROJECT_ID");
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			return utf8(*value);
		}

		[[nodiscard]] sdk::result<std::string>
		digest_text(const std::initializer_list<std::string_view> values)
		{
			std::vector<std::byte> bytes;
			for (const auto value : values)
			{
				if (value.size() > std::numeric_limits<std::size_t>::max() - bytes.size() - 1U)
					return sdk::unexpected(capture_error("identity", "length"));
				for (const auto byte : value)
					bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
				bytes.push_back(std::byte{});
			}
			return sdk::content_digest(bytes);
		}

		[[nodiscard]] std::string language_standard(const std::span<const std::string> arguments)
		{
			for (const auto& argument : arguments)
				if (argument.starts_with("/std:"))
					return argument.substr(5U);
			return "c++14";
		}

	} // namespace

	sdk::result<msvc_capture_command_result>
	capture_msvc_command(const std::wstring& compiler, const std::vector<std::wstring>& arguments)
	{
		std::optional<std::uint32_t> compiler_exit_code;
		try
		{
			auto project_root_value = environment(L"CXXLENS_MSVC_PROJECT_ROOT");
			auto capture_directory_value = environment(L"CXXLENS_MSVC_CAPTURE_DIRECTORY");
			auto windows_sdk_value = environment(L"CXXLENS_WINDOWS_SDK_ROOT");
			auto project_id = configured_project_id();
			if (!project_root_value || !capture_directory_value || !windows_sdk_value ||
				!project_id || std::ranges::any_of(arguments, source_dependencies_option))
			{
				auto executed = run_msvc_process(compiler, arguments);
				if (!executed)
					return sdk::unexpected(std::move(executed.error()));
				compiler_exit_code = executed->exit_code;
				return msvc_capture_command_result{
					executed->exit_code,
					std::nullopt,
					capture_error("configuration", "missing-or-source-dependencies-conflict")};
			}

			auto root = canonical_worker_directory(*project_root_value);
			auto workspace = make_msvc_capture_workspace(*capture_directory_value);
			auto windows_sdk = canonical_worker_directory(*windows_sdk_value);
			auto working = current_worker_directory();
			if (!root || !workspace || !windows_sdk || !working || !under(*working, *root))
			{
				auto executed = run_msvc_process(compiler, arguments);
				if (!executed)
					return sdk::unexpected(std::move(executed.error()));
				compiler_exit_code = executed->exit_code;
				return msvc_capture_command_result{executed->exit_code,
												   std::nullopt,
												   capture_error("configuration", "invalid-path")};
			}

			(*workspace)->clear_dependency_output();
			auto capture_arguments = arguments;
			capture_arguments.emplace_back(L"/sourceDependencies");
			capture_arguments.push_back((*workspace)->dependency_output_path());
			auto executed = run_msvc_process(compiler, capture_arguments);
			if (!executed)
				return sdk::unexpected(std::move(executed.error()));
			compiler_exit_code = executed->exit_code;
			if (executed->exit_code != 0U)
			{
				(*workspace)->clear_dependency_output();
				return msvc_capture_command_result{executed->exit_code, std::nullopt, std::nullopt};
			}

			auto dependency_text =
				(*workspace)->read_dependency_output(std::size_t{16U} * 1024U * 1024U);
			(*workspace)->clear_dependency_output();
			if (!dependency_text)
				return msvc_capture_command_result{
					executed->exit_code, std::nullopt, std::move(dependency_text.error())};
			auto dependencies = decode_msvc_source_dependencies(*dependency_text);
			if (!dependencies)
				return msvc_capture_command_result{
					executed->exit_code, std::nullopt, std::move(dependencies.error())};

			auto source_value = utf16(dependencies->source);
			if (!source_value)
				return msvc_capture_command_result{
					executed->exit_code, std::nullopt, std::move(source_value.error())};
			auto source = canonical_worker_file(*source_value);
			if (!source || !under(*source, *root))
				return msvc_capture_command_result{executed->exit_code,
												   std::nullopt,
												   capture_error("source", "outside-project-root")};

			msvc_capture_input input;
			input.project_id = std::move(*project_id);
			auto root_text = utf8(*root);
			auto working_text = utf8(*working);
			auto canonical_compiler = canonical_worker_file(compiler);
			if (!canonical_compiler)
				return msvc_capture_command_result{
					executed->exit_code, std::nullopt, std::move(canonical_compiler.error())};
			auto compiler_text = utf8(*canonical_compiler);
			auto sdk_text = utf8(*windows_sdk);
			if (!root_text || !working_text || !compiler_text || !sdk_text)
				return msvc_capture_command_result{
					executed->exit_code, std::nullopt, capture_error("path", "encoding")};
			input.canonical_project_root = std::move(*root_text);
			input.canonical_working_directory = std::move(*working_text);
			input.canonical_compiler_path = std::move(*compiler_text);
			input.windows_sdk_root = std::move(*sdk_text);
			auto compiler_bytes = read_worker_binary_file(compiler, maximum_file_bytes);
			if (!compiler_bytes)
				return msvc_capture_command_result{
					executed->exit_code, std::nullopt, std::move(compiler_bytes.error())};
			input.compiler_binary_digest = sdk::content_digest(*compiler_bytes);
			auto abi_digest = digest_text({"x86_64-pc-windows-msvc", "19.51.36256"});
			if (!abi_digest)
				return msvc_capture_command_result{
					executed->exit_code, std::nullopt, std::move(abi_digest.error())};
			input.abi_digest = std::move(*abi_digest);
			auto builtin_headers_digest =
				digest_text({input.compiler_binary_digest, "windows-sdk-10.1.26100.8249"});
			if (!builtin_headers_digest)
				return msvc_capture_command_result{
					executed->exit_code, std::nullopt, std::move(builtin_headers_digest.error())};
			input.builtin_headers_digest = std::move(*builtin_headers_digest);
			input.original_arguments.push_back(input.canonical_compiler_path);
			for (const auto& argument : arguments)
			{
				auto argument_text = utf8(argument);
				if (!argument_text)
					return msvc_capture_command_result{
						executed->exit_code, std::nullopt, std::move(argument_text.error())};
				input.original_arguments.push_back(std::move(*argument_text));
			}
			input.builtin_macros_digest = input.compiler_binary_digest;
			auto include_digest = digest_text(
				{"msvc-19.51.36256", "windows-sdk-10.1.26100.8249", "default-devshell-x64"});
			if (!include_digest)
				return msvc_capture_command_result{
					executed->exit_code, std::nullopt, std::move(include_digest.error())};
			input.include_search_digest = std::move(*include_digest);
			input.language_standard = language_standard(input.original_arguments);

			auto source_text = utf8(*source);
			auto source_bytes = read_worker_binary_file(*source, maximum_file_bytes);
			if (!source_text || !source_bytes)
				return msvc_capture_command_result{
					executed->exit_code, std::nullopt, capture_error("source", "read-or-encoding")};
			input.main_source = {
				std::move(*source_text), std::move(*source_bytes), "main", "binary_or_unknown"};
			for (const auto& dependency : dependencies->includes)
			{
				auto dependency_value = utf16(dependency);
				if (!dependency_value)
					return msvc_capture_command_result{
						executed->exit_code, std::nullopt, std::move(dependency_value.error())};
				auto canonical = canonical_worker_file(*dependency_value);
				if (!canonical)
				{
					if (!input.source_closure_membership)
						input.source_closure_membership = unavailable_capture_field{
							"dependency-member-unreadable",
							"restore-the-dependency-member-and-recapture"};
					continue;
				}
				if (!under(*canonical, *root))
				{
					if (!input.source_closure_membership)
						input.source_closure_membership = unavailable_capture_field{
							"dependency-member-outside-project-root",
							"recapture-with-a-qualified-logical-read-root"};
					continue;
				}
				auto path = utf8(*canonical);
				if (!path)
					return msvc_capture_command_result{
						executed->exit_code, std::nullopt, std::move(path.error())};
				auto content = read_worker_binary_file(*canonical, maximum_file_bytes);
				if (!content)
				{
					if (!input.source_closure_membership)
						input.source_closure_membership = unavailable_capture_field{
							"dependency-member-unreadable",
							"restore-the-dependency-member-and-recapture"};
					continue;
				}
				input.dependency_sources.push_back(
					{std::move(*path), std::move(*content), "header", "binary_or_unknown"});
			}

			auto encoded = encode_msvc_capture_bundle(input);
			if (!encoded)
				return msvc_capture_command_result{
					executed->exit_code, std::nullopt, std::move(encoded.error())};
			auto published = (*workspace)->publish_bundle(*encoded);
			if (!published)
				return msvc_capture_command_result{
					executed->exit_code, std::nullopt, std::move(published.error())};
			return msvc_capture_command_result{
				executed->exit_code, std::move(*published), std::nullopt};
		}
		catch (const std::bad_alloc&)
		{
			if (compiler_exit_code)
				return msvc_capture_command_result{
					*compiler_exit_code, std::nullopt, capture_error("capture", "allocation")};
			return sdk::unexpected(capture_error("capture", "allocation"));
		}
		catch (const std::length_error&)
		{
			if (compiler_exit_code)
				return msvc_capture_command_result{*compiler_exit_code,
												   std::nullopt,
												   capture_error("capture", "allocation-length")};
			return sdk::unexpected(capture_error("capture", "allocation-length"));
		}
	}
} // namespace cxxlens::application_analysis_worker

#endif
