#include "materialization_streaming_claims.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <new>
#include <ranges>
#include <string_view>
#include <utility>

#include "materialization_identity.hpp"

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		[[nodiscard]] sdk::error bridge_error(const std::string_view field,
											  const std::string_view detail)
		{
			return {"materialization.identity-mismatch", std::string{field}, std::string{detail}};
		}

		[[nodiscard]] sdk::result<json_value> string_value(std::string value,
														   const std::string_view field)
		{
			auto output = json_string(std::move(value));
			if (!output)
				return sdk::unexpected(bridge_error(field, "invalid-string"));
			return output;
		}

		[[nodiscard]] sdk::result<json_value> object_value(json_value::object_type value,
														   const std::string_view field)
		{
			auto output = json_object(std::move(value));
			if (!output)
				return sdk::unexpected(bridge_error(field, "invalid-object"));
			return output;
		}

		[[nodiscard]] sdk::result<json_document>
		make_claim_authority_document(const materialization_v2_1_tool_authority& tool,
									  const materialization_v2_1_worker_authority& worker)
		{
			json_value::object_type tool_members;
			for (const auto& [name, value] : {
					 std::pair{std::string_view{"executable"}, std::string_view{tool.executable}},
					 std::pair{std::string_view{"interface_version"},
							   std::string_view{tool.interface_version}},
					 std::pair{std::string_view{"distribution_version"},
							   std::string_view{tool.distribution_version}},
					 std::pair{std::string_view{"source_revision"},
							   std::string_view{tool.source_revision}},
					 std::pair{std::string_view{"source_tree"}, std::string_view{tool.source_tree}},
				 })
			{
				auto item = string_value(std::string{value}, "tool." + std::string{name});
				if (!item || !tool_members.emplace(std::string{name}, std::move(*item)).second)
					return sdk::unexpected(
						bridge_error("tool", item ? "duplicate-member" : item.error().detail));
			}

			json_value::object_type worker_members;
			for (const auto& [name, value] : {
					 std::pair{std::string_view{"provider_id"},
							   std::string_view{worker.provider_id}},
					 std::pair{std::string_view{"provider_version"},
							   std::string_view{worker.provider_version}},
					 std::pair{std::string_view{"semantic_contract_digest"},
							   std::string_view{worker.semantic_contract_digest}},
				 })
			{
				auto item = string_value(std::string{value}, "worker." + std::string{name});
				if (!item || !worker_members.emplace(std::string{name}, std::move(*item)).second)
					return sdk::unexpected(
						bridge_error("worker", item ? "duplicate-member" : item.error().detail));
			}
			worker_members.emplace("protocol_major",
								   json_value::unsigned_integer(worker.protocol_major));
			worker_members.emplace("protocol_minor",
								   json_value::unsigned_integer(worker.protocol_minor));
			json_value::array_type required_features;
			required_features.reserve(worker.required_features.size());
			for (const auto& feature : worker.required_features)
			{
				auto feature_value = json_value::string(feature);
				if (!feature_value)
					return sdk::unexpected(bridge_error("worker.required_features", "string"));
				required_features.push_back(std::move(*feature_value));
			}
			worker_members.emplace("required_features",
								   json_value::array(std::move(required_features)));

			auto tool_object = object_value(std::move(tool_members), "tool");
			auto worker_object = object_value(std::move(worker_members), "worker");
			if (!tool_object || !worker_object)
				return sdk::unexpected(!tool_object ? std::move(tool_object.error())
													: std::move(worker_object.error()));
			json_value::object_type root_members;
			root_members.emplace("tool", std::move(*tool_object));
			root_members.emplace("worker", std::move(*worker_object));
			auto root = object_value(std::move(root_members), "claims-authority");
			if (!root)
				return sdk::unexpected(std::move(root.error()));
			return parse_json_object(canonical_json(*root));
		}

		[[nodiscard]] sdk::result<std::vector<materialization_authority_binding>>
		make_authority_bindings(const materialization_occurrence_receipt& occurrence)
		{
			constexpr std::array expected{
				std::pair{std::string_view{
							  "schemas/cxxlens_ng_clang22_materialization_contract.schema.yaml"},
						  std::string_view{"materialization-contract-schema"}},
				std::pair{
					std::string_view{"schemas/cxxlens_ng_clang22_materialization_contract.yaml"},
					std::string_view{"materialization-contract"}},
				std::pair{std::string_view{
							  "schemas/cxxlens_ng_clang22_materialization_report.schema.yaml"},
						  std::string_view{"materialization-report-schema"}},
				std::pair{std::string_view{
							  "schemas/cxxlens_ng_clang22_materialization_request.schema.yaml"},
						  std::string_view{"materialization-request-schema"}},
				std::pair{std::string_view{"schemas/cxxlens_ng_relation_registry.yaml"},
						  std::string_view{"relation-registry"}},
			};
			std::vector<materialization_authority_binding> output;
			output.reserve(expected.size());
			for (const auto& [path, role] : expected)
			{
				const auto found = std::ranges::find(
					occurrence.files,
					role,
					[](const materialization_measured_file& file) -> std::string_view
					{
						return file.authority.role;
					});
				if (found == occurrence.files.end() || found->authority.digest.empty())
					return sdk::unexpected(bridge_error("installation.authority", path));
				output.push_back({std::string{path}, found->authority.digest});
			}
			return output;
		}
	} // namespace

	sdk::result<materialization_v2_1_claim_context>
	make_materialization_v2_1_claim_context(validated_materialization_request_v2_1& request,
											const materialization_occurrence_receipt& occurrence)
	{
		try
		{
			const auto& admitted = request.request();
			if (admitted.task_count() == 0U ||
				admitted.task_count() > std::numeric_limits<std::size_t>::max())
				return sdk::unexpected(bridge_error("tasks", "cardinality"));

			std::vector<validated_task_request> tasks;
			tasks.reserve(static_cast<std::size_t>(admitted.task_count()));
			for (std::uint64_t index{}; index < admitted.task_count(); ++index)
			{
				auto execution = request.task_execution(index);
				if (!execution)
					return sdk::unexpected(std::move(execution.error()));
				if (!execution->source || !execution->source->sealed() || !execution->task_input ||
					!execution->task_input->sealed())
					return sdk::unexpected(bridge_error("task", "unsealed-spool"));
				validated_task_request task{
					std::move(execution->input),
					std::move(execution->metadata.provider_task_id),
					std::move(execution->metadata.provider_execution_id),
					std::move(execution->metadata.task_input_digest),
					std::move(execution->metadata.sandbox),
					{},
					std::move(execution->source_receipt),
				};
				tasks.push_back(std::move(task));
			}

			auto document = make_claim_authority_document(admitted.tool(), admitted.worker());
			if (!document)
				return sdk::unexpected(std::move(document.error()));
			auto bindings = make_authority_bindings(occurrence);
			if (!bindings)
				return sdk::unexpected(std::move(bindings.error()));

			materialization_producer_authority producer{
				admitted.tool().executable,
				admitted.tool().interface_version,
				admitted.tool().distribution_version,
				admitted.tool().source_revision,
				admitted.tool().source_tree,
				std::move(*bindings),
			};
			materialization_guarantee_authority guarantee{
				{},
				{"clang22.materialization-sealed.v1",
				 "provider.transcript-sealed.v1",
				 "sdk.claim-envelope-validated.v1"},
			};
			validated_materialization_request legacy{
				std::move(*document),
				admitted.catalog(),
				admitted.engine(),
				admitted.output_descriptors(),
				std::move(tasks),
				admitted.publication(),
			};
			return materialization_v2_1_claim_context{
				std::move(legacy), std::move(producer), std::move(guarantee)};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(bridge_error("allocation", "unavailable"));
		}
	}
} // namespace cxxlens::detail::clang22::materialization
