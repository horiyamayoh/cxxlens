#include "msvc_capture_bundle.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <new>
#include <ranges>
#include <set>
#include <tuple>
#include <utility>

#include "sdk/source_identity_internal.hpp"

namespace cxxlens::application_analysis_worker
{
	namespace
	{
		using sdk::canonical_value;
		using sdk::result;

		[[nodiscard]] sdk::error invalid(std::string field, std::string detail)
		{
			return {
				"application-analysis.msvc-capture-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::error limit(std::string field, std::string detail)
		{
			return {"application-analysis.msvc-capture-limit-exceeded",
					std::move(field),
					std::move(detail)};
		}

		[[nodiscard]] char fold(const char value) noexcept
		{
			return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
		}

		[[nodiscard]] bool canonical_windows_path(const std::string_view value) noexcept
		{
			if (value.size() < 3U || value.front() < 'A' || value.front() > 'Z' ||
				value[1U] != ':' || value[2U] != '\\' || value.contains('/'))
				return false;
			if (value.size() > 3U && value.back() == '\\')
				return false;
			std::size_t offset{3U};
			while (offset < value.size())
			{
				const auto next = value.find('\\', offset);
				const auto segment = value.substr(
					offset, next == std::string_view::npos ? value.size() - offset : next - offset);
				if (segment.empty() || segment == "." || segment == "..")
					return false;
				for (const auto byte : segment)
					if (static_cast<unsigned char>(byte) <= 0x1fU || byte == 0x7f ||
						std::string_view{"<>:\"|?*"}.contains(byte))
						return false;
				if (next == std::string_view::npos)
					break;
				offset = next + 1U;
			}
			return true;
		}

		[[nodiscard]] bool at_or_below(const std::string_view path,
									   const std::string_view root) noexcept
		{
			if (path.size() < root.size() ||
				!std::ranges::equal(path.substr(0U, root.size()), root, {}, fold, fold))
				return false;
			return path.size() == root.size() || root.back() == '\\' || path[root.size()] == '\\';
		}

		[[nodiscard]] std::string logical_path(const std::string_view path,
											   const std::string_view root)
		{
			auto suffix = path.substr(root.size());
			if (suffix.starts_with('\\'))
				suffix.remove_prefix(1U);
			std::string output{"project://"};
			for (const auto byte : suffix)
				output.push_back(byte == '\\' ? '/' : byte);
			return output;
		}

		[[nodiscard]] canonical_value observed(canonical_value value)
		{
			return canonical_value::from_tuple({canonical_value::from_string("observed"),
												std::move(value),
												canonical_value::from_string({}),
												canonical_value::from_string({})});
		}

		[[nodiscard]] canonical_value derived(canonical_value value)
		{
			return canonical_value::from_tuple({canonical_value::from_string("derived"),
												std::move(value),
												canonical_value::from_string({}),
												canonical_value::from_string({})});
		}

		[[nodiscard]] bool digest_like(const std::string_view value) noexcept
		{
			return value.size() == 71U && value.starts_with("sha256:") &&
				std::ranges::all_of(value.substr(7U),
									[](const char byte)
									{
										return (byte >= '0' && byte <= '9') ||
											(byte >= 'a' && byte <= 'f');
									});
		}

		[[nodiscard]] bool source_role(const std::string_view value) noexcept
		{
			return value == "main" || value == "header" || value == "generated" ||
				value == "forced-include" || value == "macro-file";
		}

		[[nodiscard]] bool source_encoding(const std::string_view value) noexcept
		{
			return value == "utf8" || value == "utf16le" || value == "utf16be" ||
				value == "locale_dependent" || value == "binary_or_unknown";
		}

		struct prepared_source
		{
			std::string file_id;
			std::string logical_path;
			std::string digest;
			std::vector<std::byte> content;
			std::string role;
			std::string encoding;
		};
		struct source_authority
		{
			std::string_view root;
			std::string field;
		};

		[[nodiscard]] result<prepared_source> prepare_source(const captured_source& source,
															 source_authority authority)
		{
			if (!canonical_windows_path(source.canonical_path) ||
				!at_or_below(source.canonical_path, authority.root))
				return sdk::unexpected(
					invalid(std::move(authority.field) + ".path", "outside-project-root"));
			if (!source_role(source.role) || !source_encoding(source.encoding))
				return sdk::unexpected(invalid(std::move(authority.field), "role-or-encoding"));
			auto logical = logical_path(source.canonical_path, authority.root);
			auto file = sdk::detail::derive_source_file_id(
				std::string_view{logical}.substr(std::string_view{"project://"}.size()));
			if (!file)
				return sdk::unexpected(std::move(file.error()));
			return prepared_source{std::move(*file),
								   std::move(logical),
								   sdk::content_digest(source.content),
								   source.content,
								   source.role,
								   source.encoding};
		}

		[[nodiscard]] canonical_value source_member(const prepared_source& source)
		{
			return canonical_value::from_tuple({
				canonical_value::from_string(source.file_id),
				canonical_value::from_string(source.logical_path),
				derived(canonical_value::from_string(source.digest)),
				observed(canonical_value::from_bytes(source.content)),
				canonical_value::from_integer(static_cast<std::int64_t>(source.content.size())),
				derived(canonical_value::from_string(source.role)),
				observed(canonical_value::from_string(source.encoding)),
				canonical_value::from_boolean(true),
			});
		}

		[[nodiscard]] std::vector<canonical_value>
		semantic_arguments(const msvc_capture_input& input)
		{
			const auto normalized_path = [&](const std::string_view path)
			{
				if (canonical_windows_path(path) && at_or_below(path, input.canonical_project_root))
					return logical_path(path, input.canonical_project_root);
				if (canonical_windows_path(path) && at_or_below(path, input.windows_sdk_root))
					return std::string{"windows-sdk://"} +
						logical_path(path, input.windows_sdk_root)
							.substr(std::string_view{"project://"}.size());
				return std::string{"$unmapped-absolute-path"};
			};
			std::vector<canonical_value> output;
			output.reserve(input.original_arguments.size());
			for (std::size_t index{}; index < input.original_arguments.size(); ++index)
			{
				const auto& token = input.original_arguments[index];
				if (index == 0U)
				{
					output.push_back(canonical_value::from_string("$production-compiler"));
					continue;
				}
				if (token == "/Fo" || token == "/Fd" || token == "/Fe" ||
					token == "/sourceDependencies")
				{
					if (index + 1U < input.original_arguments.size())
						++index;
					continue;
				}
				if (token.starts_with("/Fo") || token.starts_with("/Fd") ||
					token.starts_with("/Fe") || token == "/c" || token == "/nologo" ||
					token.starts_with("/sourceDependencies"))
					continue;
				if (token == "/I" || token == "/FI" || token == "/external:I")
				{
					output.push_back(canonical_value::from_string(token));
					if (index + 1U < input.original_arguments.size())
						output.push_back(canonical_value::from_string(
							normalized_path(input.original_arguments[++index])));
					continue;
				}
				const auto normalize_attached = [&](const std::string_view prefix)
				{
					return std::string{prefix} + normalized_path(token.substr(prefix.size()));
				};
				if (token.starts_with("/external:I"))
					output.push_back(
						canonical_value::from_string(normalize_attached("/external:I")));
				else if (token.starts_with("/FI"))
					output.push_back(canonical_value::from_string(normalize_attached("/FI")));
				else if (token.starts_with("/I"))
					output.push_back(canonical_value::from_string(normalize_attached("/I")));
				else if (token.starts_with('@') && canonical_windows_path(token.substr(1U)))
					output.push_back(
						canonical_value::from_string("@" + normalized_path(token.substr(1U))));
				else if (canonical_windows_path(token))
					output.push_back(canonical_value::from_string(normalized_path(token)));
				else
					output.push_back(canonical_value::from_string(token));
			}
			return output;
		}
	} // namespace

	result<std::vector<std::byte>> encode_msvc_capture_bundle(const msvc_capture_input& input,
															  const msvc_capture_limits limits)
	{
		try
		{
			if (input.original_arguments.empty() ||
				input.original_arguments.size() > limits.maximum_arguments)
				return sdk::unexpected(limit("original_arguments", "count"));
			if (input.dependency_sources.size() > limits.maximum_sources - 1U ||
				input.response_files.size() >
					limits.maximum_sources - 1U - input.dependency_sources.size() ||
				input.response_files.size() > limits.maximum_response_files)
				return sdk::unexpected(limit("source_closure", "count"));
			for (const auto* path : {&input.canonical_project_root,
									 &input.canonical_working_directory,
									 &input.canonical_compiler_path,
									 &input.windows_sdk_root})
				if (path->size() > limits.maximum_string_bytes || !canonical_windows_path(*path))
					return sdk::unexpected(invalid("path", "not-canonical-windows-path"));
			if (!at_or_below(input.canonical_working_directory, input.canonical_project_root))
				return sdk::unexpected(invalid("working_directory", "outside-project-root"));
			for (const auto* digest : {&input.compiler_binary_digest,
									   &input.abi_digest,
									   &input.builtin_headers_digest,
									   &input.builtin_macros_digest,
									   &input.include_search_digest})
				if (!digest_like(*digest))
					return sdk::unexpected(invalid("toolchain", "digest"));

			auto main =
				prepare_source(input.main_source, {input.canonical_project_root, "main_source"});
			if (!main || main->role != "main")
				return sdk::unexpected(main ? invalid("main_source.role", "main-required")
											: std::move(main.error()));
			std::vector<prepared_source> sources;
			sources.reserve(input.dependency_sources.size() + input.response_files.size() + 1U);
			sources.push_back(std::move(*main));
			for (std::size_t index{}; index < input.dependency_sources.size(); ++index)
			{
				auto source = prepare_source(input.dependency_sources[index],
											 {input.canonical_project_root,
											  "dependency_sources[" + std::to_string(index) + "]"});
				if (!source)
					return sdk::unexpected(std::move(source.error()));
				sources.push_back(std::move(*source));
			}
			for (std::size_t index{}; index < input.response_files.size(); ++index)
			{
				captured_source response{input.response_files[index].canonical_path,
										 input.response_files[index].content,
										 "generated",
										 "utf8"};
				auto source = prepare_source(response,
											 {input.canonical_project_root,
											  "response_files[" + std::to_string(index) + "]"});
				if (!source)
					return sdk::unexpected(std::move(source.error()));
				sources.push_back(std::move(*source));
			}
			std::ranges::sort(sources, {}, &prepared_source::file_id);
			if (std::ranges::adjacent_find(sources, {}, &prepared_source::file_id) != sources.end())
				return sdk::unexpected(invalid("source_closure", "duplicate-file"));

			std::uint64_t total_bytes{};
			std::map<std::string, std::uint64_t, std::less<>> unique_blobs;
			std::vector<canonical_value> members;
			members.reserve(sources.size());
			for (const auto& source : sources)
			{
				if (source.content.size() > limits.maximum_source_bytes - total_bytes)
					return sdk::unexpected(limit("source_closure", "bytes"));
				total_bytes += source.content.size();
				unique_blobs.emplace(source.digest, source.content.size());
				members.push_back(source_member(source));
			}
			auto member_value = canonical_value::from_tuple(std::move(members));
			auto member_bytes = sdk::canonical_binary(member_value);
			if (!member_bytes)
				return sdk::unexpected(std::move(member_bytes.error()));
			const auto manifest_digest = sdk::content_digest(*member_bytes);
			const auto membership = observed(canonical_value::from_string("complete"));
			std::uint64_t unique_bytes{};
			for (const auto& [digest, size] : unique_blobs)
			{
				(void)digest;
				unique_bytes += size;
			}
			const std::array closure_fields{
				canonical_value::from_string(manifest_digest),
				canonical_value::from_integer(static_cast<std::int64_t>(sources.size())),
				canonical_value::from_integer(static_cast<std::int64_t>(unique_blobs.size())),
				canonical_value::from_integer(static_cast<std::int64_t>(unique_bytes)),
				membership,
			};
			auto closure_digest =
				sdk::canonical_identity_digest("application-source-closure", closure_fields);
			if (!closure_digest)
				return sdk::unexpected(std::move(closure_digest.error()));
			const auto closure_id = "source-closure:" + *closure_digest;
			auto closure = canonical_value::from_tuple({
				canonical_value::from_string(closure_id),
				canonical_value::from_string(*closure_digest),
				canonical_value::from_string(manifest_digest),
				canonical_value::from_integer(static_cast<std::int64_t>(sources.size())),
				canonical_value::from_integer(static_cast<std::int64_t>(unique_blobs.size())),
				canonical_value::from_integer(static_cast<std::int64_t>(unique_bytes)),
				std::move(member_value),
				membership,
			});

			const auto main_source = std::ranges::find(sources, "main", &prepared_source::role);
			if (main_source == sources.end())
				return sdk::unexpected(invalid("main_source", "missing"));
			auto snapshot = sdk::detail::derive_source_snapshot_id(
				main_source->file_id, main_source->digest, main_source->encoding);
			if (!snapshot)
				return sdk::unexpected(std::move(snapshot.error()));
			auto semantic = semantic_arguments(input);
			const std::array compile_fields{
				canonical_value::from_string(input.project_id),
				canonical_value::from_string(*snapshot),
				canonical_value::from_string(
					logical_path(input.canonical_working_directory, input.canonical_project_root)),
				canonical_value::from_tuple(std::move(semantic)),
				canonical_value::from_string("c++"),
				canonical_value::from_string("19.51.36231"),
				canonical_value::from_string("x86_64-pc-windows-msvc"),
				observed(canonical_value::from_string(input.compiler_binary_digest)),
				observed(canonical_value::from_string("$captured-windows-sdk")),
				observed(canonical_value::from_string(input.abi_digest)),
				observed(canonical_value::from_string(input.builtin_headers_digest)),
				observed(canonical_value::from_string(input.builtin_macros_digest)),
				observed(canonical_value::from_string(input.include_search_digest)),
			};
			auto compile_digest =
				sdk::canonical_identity_digest("application-compile-unit", compile_fields);
			if (!compile_digest)
				return sdk::unexpected(std::move(compile_digest.error()));

			std::vector<canonical_value> arguments;
			arguments.reserve(input.original_arguments.size());
			for (const auto& argument : input.original_arguments)
				arguments.push_back(canonical_value::from_string(argument));
			std::vector<canonical_value> response_files;
			response_files.reserve(input.response_files.size());
			for (const auto& response : input.response_files)
			{
				if (response.parent_index && *response.parent_index >= input.response_files.size())
					return sdk::unexpected(invalid("response_files", "parent-index"));
				response_files.push_back(canonical_value::from_tuple({
					canonical_value::from_string(
						logical_path(response.canonical_path, input.canonical_project_root)),
					observed(canonical_value::from_string(sdk::content_digest(response.content))),
					canonical_value::from_integer(
						static_cast<std::int64_t>(response.content.size())),
					response.parent_index ? canonical_value::from_integer(
												static_cast<std::int64_t>(*response.parent_index))
										  : canonical_value::null(),
				}));
			}
			auto unit = canonical_value::from_tuple({
				canonical_value::from_string("compile-unit:" + *compile_digest),
				derived(canonical_value::from_string(*snapshot)),
				canonical_value::from_string(main_source->file_id),
				canonical_value::from_string(main_source->logical_path),
				canonical_value::from_string(main_source->digest),
				canonical_value::from_integer(
					static_cast<std::int64_t>(main_source->content.size())),
				canonical_value::from_string(
					logical_path(input.canonical_working_directory, input.canonical_project_root)),
				canonical_value::from_string("c++"),
				observed(canonical_value::from_tuple(std::move(arguments))),
				observed(canonical_value::from_tuple(std::move(response_files))),
				observed(canonical_value::from_tuple({})),
				observed(canonical_value::from_tuple({})),
				observed(canonical_value::from_string(input.canonical_working_directory)),
				observed(canonical_value::from_string(input.language_standard)),
				observed(canonical_value::from_string("msvc")),
				canonical_value::from_string(closure_id),
			});
			auto toolchain = canonical_value::from_tuple({
				canonical_value::from_string("msvc"),
				canonical_value::from_string("19.51.36231"),
				observed(canonical_value::from_string(input.canonical_compiler_path)),
				observed(canonical_value::from_string(input.compiler_binary_digest)),
				canonical_value::from_string("x86_64-pc-windows-msvc"),
				observed(canonical_value::from_string(input.windows_sdk_root)),
				observed(canonical_value::from_string(input.abi_digest)),
				observed(canonical_value::from_string(input.builtin_headers_digest)),
				observed(canonical_value::from_string(input.builtin_macros_digest)),
				observed(canonical_value::from_string(input.include_search_digest)),
			});
			auto root = canonical_value::from_tuple({
				canonical_value::from_string("cxxlens.build-capture-bundle.v1"),
				std::move(toolchain),
				canonical_value::from_string("msbuild-cltool-proxy"),
				canonical_value::from_string("x86_64-pc-windows-msvc"),
				canonical_value::from_string(input.project_id),
				canonical_value::from_tuple({std::move(unit)}),
				canonical_value::from_tuple({std::move(closure)}),
				canonical_value::from_tuple({}),
				canonical_value::from_string("project://"),
				derived(canonical_value::from_tuple({canonical_value::from_tuple({
					canonical_value::from_string(input.canonical_project_root),
					canonical_value::from_string("project://"),
				})})),
			});
			auto encoded = sdk::canonical_binary(root);
			if (!encoded)
				return sdk::unexpected(std::move(encoded.error()));
			if (encoded->size() > limits.maximum_bundle_bytes)
				return sdk::unexpected(limit("capture_bundle", "bytes"));
			return encoded;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(limit("capture_bundle", "allocation"));
		}
		catch (const std::length_error&)
		{
			return sdk::unexpected(limit("capture_bundle", "allocation-length"));
		}
	}
} // namespace cxxlens::application_analysis_worker
