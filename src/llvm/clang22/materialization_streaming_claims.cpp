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
			auto claim_authority =
				make_materialization_v2_1_claim_authority(request, producer, guarantee);
			if (!claim_authority)
				return sdk::unexpected(std::move(claim_authority.error()));
			// Seal the complete selected-request entry journal before any production plan or
			// provider dispatch can observe mutable task-window state.
			auto selected_request_binding_set =
				seal_materialization_incremental_selected_request_binding_set(*claim_authority);
			if (!selected_request_binding_set)
				return sdk::unexpected(std::move(selected_request_binding_set.error()));
			return materialization_v2_1_claim_context{std::move(*claim_authority),
													  std::move(*selected_request_binding_set),
													  std::move(producer),
													  std::move(guarantee)};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(bridge_error("allocation", "unavailable"));
		}
	}
} // namespace cxxlens::detail::clang22::materialization
