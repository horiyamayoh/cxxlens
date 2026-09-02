#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

#include "gcc_auxiliary_capture_internal.hpp"
#include "gcc_capture_file_port_internal.hpp"

namespace cxxlens::sdk::detail
{
	namespace
	{
		[[nodiscard]] error invalid(std::string field, std::string detail)
		{
			return {
				"application-analysis.capture-input-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] error allocation_failure()
		{
			return {"application-analysis.capture-allocation-failed", "capture", "allocation"};
		}

		[[nodiscard]] bool at_or_below(const std::string_view path,
									   const std::string_view root) noexcept
		{
			return path == root ||
				(path.size() > root.size() && path.starts_with(root) &&
				 (root == "/" || path[root.size()] == '/'));
		}

		[[nodiscard]] result<void>
		validate_compiler_bindings(const compile_commands_capture& capture,
								   const gcc_compile_commands_capture_request& request,
								   const gcc_toolchain_observation& toolchain)
		{
			if (request.compiler_path.empty() || request.compiler_path.front() != '/' ||
				request.compiler_path.contains('\0'))
				return unexpected(invalid("compiler_path", "absolute-path-required"));
			if (!toolchain.canonical_binary_path.value ||
				toolchain.canonical_binary_path.state != capture_observation_state::observed)
				return unexpected(invalid("compiler_path", "measured-identity-missing"));

			for (std::size_t index{}; index < capture.entries().size(); ++index)
			{
				const auto& compiler = capture.entries()[index].arguments.front();
				const auto field = "compile_commands[" + std::to_string(index) + "].arguments[0]";
				if (compiler.empty() || compiler.front() != '/')
					return unexpected(invalid(field, "absolute-compiler-path-required"));
				if (compiler != request.compiler_path &&
					compiler != *toolchain.canonical_binary_path.value)
					return unexpected(invalid(field, "compiler-identity-mismatch"));
			}
			return {};
		}

		[[nodiscard]] result<std::string> source_path(const compile_command_entry& entry,
													  const std::string_view canonical_directory)
		{
			if (entry.file.empty())
				return unexpected(invalid("compile_command.file", "empty"));
			if (entry.file.starts_with('/'))
				return entry.file;
			if (entry.file.size() == std::numeric_limits<std::size_t>::max() ||
				canonical_directory.size() >
					std::numeric_limits<std::size_t>::max() - entry.file.size() - 1U)
				return unexpected(invalid("compile_command.file", "path-overflow"));
			return std::string{canonical_directory} + "/" + entry.file;
		}

		[[nodiscard]] std::string source_encoding(const std::vector<std::byte>& bytes)
		{
			const auto text =
				std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
			return validate_utf8_text(text) ? "utf8" : "binary_or_unknown";
		}
	} // namespace

	result<std::vector<std::byte>>
	capture_gcc_compile_commands(gcc_capture_file_port& files,
								 gcc_probe_process_port& processes,
								 const gcc_compile_commands_capture_request& request,
								 const import_limits limits,
								 const std::stop_token& cancellation)
	{
		if (auto valid = limits.validate(); !valid)
			return unexpected(std::move(valid.error()));
		if (request.compiler_path.empty() || request.compiler_path.front() != '/' ||
			request.compiler_path.contains('\0'))
			return unexpected(invalid("compiler_path", "absolute-path-required"));
		try
		{
			auto root =
				files.canonical_directory(request.project_root, limits.maximum_string_bytes);
			if (!root)
				return unexpected(std::move(root.error()));
			if (root->size() > limits.maximum_string_bytes)
				return unexpected(
					{"application-analysis.import-limit-exceeded", "capture.path", "path-bytes"});
			auto toolchain = probe_gcc_toolchain(processes,
												 {request.compiler_path,
												  *root,
												  request.process_limits,
												  request.absolute_wall_deadline_ns},
												 cancellation);
			if (!toolchain)
				return unexpected(std::move(toolchain.error()));
			auto database = files.read_regular_file(
				request.compile_commands_path,
				{limits.maximum_bundle_bytes, limits.maximum_string_bytes, {}});
			if (!database)
				return unexpected(std::move(database.error()));
			if (database->canonical_path.size() > limits.maximum_string_bytes)
				return unexpected(
					{"application-analysis.import-limit-exceeded", "capture.path", "path-bytes"});
			if (database->content.size() > limits.maximum_bundle_bytes)
				return unexpected(
					{"application-analysis.import-limit-exceeded", "capture.file", "byte-count"});
			const auto database_text = std::string_view{
				reinterpret_cast<const char*>(database->content.data()), database->content.size()};
			auto decoded = decode_compile_commands(database_text, limits);
			if (!decoded)
				return unexpected(std::move(decoded.error()));
			if (auto bound = validate_compiler_bindings(*decoded, request, *toolchain); !bound)
				return unexpected(std::move(bound.error()));

			gcc_compile_commands_bundle_input input;
			input.project_id = request.project_id;
			input.physical_project_root = std::move(*root);
			input.toolchain = std::move(*toolchain);
			input.sources.reserve(decoded->entries().size());
			input.invocations.reserve(decoded->entries().size());
			std::uint64_t remaining = limits.maximum_source_closure_bytes;
			for (const auto& entry : decoded->entries())
			{
				auto working =
					files.canonical_directory(entry.directory, limits.maximum_string_bytes);
				if (!working)
					return unexpected(std::move(working.error()));
				if (working->size() > limits.maximum_string_bytes)
					return unexpected({"application-analysis.import-limit-exceeded",
									   "capture.path",
									   "path-bytes"});
				if (!at_or_below(*working, input.physical_project_root))
					return unexpected(
						invalid("compile_command.directory", "path-outside-project-root"));
				auto requested_source = source_path(entry, *working);
				if (!requested_source)
					return unexpected(std::move(requested_source.error()));
				auto source = files.read_regular_file(
					*requested_source,
					{remaining, limits.maximum_string_bytes, input.physical_project_root});
				if (!source)
					return unexpected(std::move(source.error()));
				if (source->canonical_path.size() > limits.maximum_string_bytes)
					return unexpected({"application-analysis.import-limit-exceeded",
									   "capture.path",
									   "path-bytes"});
				if (source->content.size() > remaining)
					return unexpected({"application-analysis.import-limit-exceeded",
									   "capture.file",
									   "byte-count"});
				remaining -= static_cast<std::uint64_t>(source->content.size());
				const auto encoding = source_encoding(source->content);
				auto auxiliary = capture_gcc_auxiliary_files(files,
															 entry.arguments,
															 *working,
															 input.physical_project_root,
															 remaining,
															 limits);
				if (!auxiliary)
					return unexpected(std::move(auxiliary.error()));
				if (auxiliary->captured_bytes > remaining)
					return unexpected({"application-analysis.import-limit-exceeded",
									   "capture.auxiliary_files",
									   "byte-count"});
				remaining -= auxiliary->captured_bytes;
				auxiliary->invocation.source_closure_members =
					std::move(auxiliary->closure_members);
				input.invocations.push_back(std::move(auxiliary->invocation));
				input.sources.push_back({std::move(source->content),
										 encoding,
										 std::move(source->canonical_path),
										 std::move(*working)});
			}
			return encode_gcc_compile_commands_bundle(*decoded, input, limits);
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(allocation_failure());
		}
		catch (const std::length_error&)
		{
			return unexpected(allocation_failure());
		}
	}
} // namespace cxxlens::sdk::detail
