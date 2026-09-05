#include "application_analysis_command_service_internal.hpp"

#include <cstdint>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "bounded_json_internal.hpp"
#include "gcc_capture_file_port_internal.hpp"

namespace cxxlens::sdk::detail
{
	namespace
	{
		[[nodiscard]] error command_error(std::string detail)
		{
			return {"application-analysis.import-runtime-unavailable", "bundle", std::move(detail)};
		}

		[[nodiscard]] result<void>
		add_string(json_value::object_type& object, std::string key, const std::string_view value)
		{
			auto encoded = json_value::string(std::string{value});
			if (!encoded)
				return unexpected(std::move(encoded.error()));
			object.emplace(std::move(key), std::move(*encoded));
			return {};
		}

		[[nodiscard]] result<json_value> gap_projection(const capture_gap& gap)
		{
			json_value::object_type output;
			if (auto added = add_string(output, "completion_action", gap.completion_action); !added)
				return unexpected(std::move(added.error()));
			if (auto added = add_string(output, "field", gap.field); !added)
				return unexpected(std::move(added.error()));
			if (auto added = add_string(output, "reason", gap.reason); !added)
				return unexpected(std::move(added.error()));
			if (auto added = add_string(output, "state", gap.state); !added)
				return unexpected(std::move(added.error()));
			return json_value::object(std::move(output));
		}

		[[nodiscard]] result<json_value> gap_array(const std::span<const capture_gap> gaps)
		{
			json_value::array_type output;
			output.reserve(gaps.size());
			for (const auto& gap : gaps)
			{
				auto encoded = gap_projection(gap);
				if (!encoded)
					return unexpected(std::move(encoded.error()));
				output.push_back(std::move(*encoded));
			}
			return json_value::array(std::move(output));
		}

		[[nodiscard]] result<json_value> replay_projection(const replay_plan& replay)
		{
			json_value::object_type output;
			if (auto added = add_string(output, "analysis_frontend", replay.analysis_frontend());
				!added)
				return unexpected(std::move(added.error()));
			if (auto added = add_string(output, "compile_unit_id", replay.compile_unit_id());
				!added)
				return unexpected(std::move(added.error()));
			if (auto added = add_string(output, "digest", replay.digest()); !added)
				return unexpected(std::move(added.error()));
			if (auto added = add_string(output, "target_abi", replay.target_abi()); !added)
				return unexpected(std::move(added.error()));
			auto unresolved = gap_array(replay.unresolved());
			if (!unresolved)
				return unexpected(std::move(unresolved.error()));
			output.emplace("unresolved", std::move(*unresolved));
			return json_value::object(std::move(output));
		}

		[[nodiscard]] result<std::string> projection(const capture_bundle& bundle,
													 const imported_project& project)
		{
			json_value::object_type capture;
			if (auto added = add_string(capture, "adapter", bundle.capture_adapter()); !added)
				return unexpected(std::move(added.error()));
			if (auto added = add_string(capture, "digest", bundle.digest()); !added)
				return unexpected(std::move(added.error()));
			if (auto added =
					add_string(capture, "production_compiler", bundle.production_compiler());
				!added)
				return unexpected(std::move(added.error()));
			if (auto added = add_string(capture, "target_abi", bundle.target_abi()); !added)
				return unexpected(std::move(added.error()));
			capture.emplace("compile_unit_count",
							json_value::unsigned_integer(
								static_cast<std::uint64_t>(bundle.compile_unit_count())));
			auto capture_unresolved = gap_array(bundle.gaps());
			if (!capture_unresolved)
				return unexpected(std::move(capture_unresolved.error()));
			capture.emplace("unresolved", std::move(*capture_unresolved));

			json_value::array_type replay_plans;
			replay_plans.reserve(project.replay_plans().size());
			for (const auto& replay : project.replay_plans())
			{
				auto encoded = replay_projection(replay);
				if (!encoded)
					return unexpected(std::move(encoded.error()));
				replay_plans.push_back(std::move(*encoded));
			}

			json_value::object_type imported;
			if (auto added =
					add_string(imported, "capture_digest", project.capture_bundle_digest());
				!added)
				return unexpected(std::move(added.error()));
			if (auto added = add_string(
					imported, "catalog_semantic_digest", project.catalog_semantic_digest());
				!added)
				return unexpected(std::move(added.error()));
			if (auto added = add_string(imported, "project_id", project.id()); !added)
				return unexpected(std::move(added.error()));
			imported.emplace("replay_plans", json_value::array(std::move(replay_plans)));
			auto project_unresolved = gap_array(project.unresolved());
			if (!project_unresolved)
				return unexpected(std::move(project_unresolved.error()));
			imported.emplace("unresolved", std::move(*project_unresolved));

			auto capture_object = json_value::object(std::move(capture));
			if (!capture_object)
				return unexpected(std::move(capture_object.error()));
			auto imported_object = json_value::object(std::move(imported));
			if (!imported_object)
				return unexpected(std::move(imported_object.error()));
			json_value::object_type root;
			root.emplace("capture", std::move(*capture_object));
			root.emplace("imported_project", std::move(*imported_object));
			auto root_object = json_value::object(std::move(root));
			if (!root_object)
				return unexpected(std::move(root_object.error()));
			return canonical_json_line(*root_object);
		}
	} // namespace

	result<std::string>
	import_application_analysis_command(const application_analysis_import_command_request& request)
	{
		try
		{
			if (auto valid = request.limits.validate(); !valid)
				return unexpected(std::move(valid.error()));
			auto files = make_system_gcc_capture_file_port();
			if (!files)
				return unexpected(command_error("file-port-construction"));
			auto input = files->read_regular_file(
				request.bundle_path,
				{request.limits.maximum_bundle_bytes, request.limits.maximum_string_bytes, {}});
			if (!input)
				return unexpected(std::move(input.error()));
			auto bundle = decode_capture_bundle(input->content, request.limits);
			if (!bundle)
				return unexpected(std::move(bundle.error()));
			auto project = import_capture(*bundle, request.limits);
			if (!project)
				return unexpected(std::move(project.error()));
			return projection(*bundle, *project);
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(command_error("allocation"));
		}
		catch (const std::length_error&)
		{
			return unexpected(command_error("allocation-length"));
		}
	}
} // namespace cxxlens::sdk::detail
