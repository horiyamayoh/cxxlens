#pragma once

/**
 * @file application_analysis_service_internal.hpp
 * @brief Source-private access and result composition for application materialization services.
 */

#include <memory>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>

#include "application_analysis_internal.hpp"
#include "application_materialization_adoption_internal.hpp"

namespace cxxlens::sdk
{
	struct materialization_request::implementation
	{
		relation_engine engine;
		snapshot_draft publication;
		std::vector<std::string> relation_descriptor_ids;
		std::string interpretation;
		provider::provider_selection_request provider;
		std::optional<provider::provider_selection> selection;
		provider::execution_budget budget;
		std::stop_token cancellation;
	};

	struct materialization_result::implementation
	{
		materialization_terminal terminal{materialization_terminal::failed};
		std::optional<snapshot_handle> published_snapshot;
		std::vector<provider::coverage_unit> coverage;
		std::vector<provider::unresolved_item> unresolved;
		std::vector<claim_conflict> conflicts;
		std::vector<differential_disagreement> differential_disagreements;
		std::optional<application_analysis_provenance> provenance;
	};

	struct application_analysis_materialization_access_internal final
	{
		[[nodiscard]] static const materialization_request::implementation&
		request(const materialization_request& value) noexcept
		{
			return *value.value_;
		}

		[[nodiscard]] static materialization_result
		result(std::shared_ptr<const materialization_result::implementation> value)
		{
			return materialization_result{std::move(value)};
		}
	};

	namespace detail
	{

		[[nodiscard]] inline materialization_result application_materialization_terminal_result(
			const cxxlens::sdk::materialization_terminal terminal,
			std::vector<provider::unresolved_item> unresolved = {})
		{
			auto value = std::make_shared<materialization_result::implementation>();
			value->terminal = terminal;
			value->unresolved = std::move(unresolved);
			return application_analysis_materialization_access_internal::result(std::move(value));
		}

		[[nodiscard]] inline materialization_result
		application_materialization_published_result(application_materialization_adoption adoption,
													 const provider::manifest& manifest)
		{
			auto value = std::make_shared<materialization_result::implementation>();
			value->terminal = adoption.publication.terminal ==
					cxxlens::sdk::detail::materialization_terminal::complete
				? sdk::materialization_terminal::published_complete
				: sdk::materialization_terminal::published_partial;
			value->published_snapshot = std::move(adoption.publication.snapshot);
			value->coverage = std::move(adoption.coverage);
			value->unresolved = std::move(adoption.unresolved);
			value->conflicts = std::move(adoption.conflicts);
			value->differential_disagreements = std::move(adoption.differential_disagreements);
			value->provenance =
				application_analysis_provenance{manifest.provider_id,
												manifest.provider_version,
												manifest.provider_binary_digest,
												manifest.provider_semantic_contract_digest,
												std::move(adoption.provider_input_digest),
												std::move(adoption.replay_plan_digest),
												std::move(adoption.runtime_receipt_digest)};
			return application_analysis_materialization_access_internal::result(std::move(value));
		}
	} // namespace detail
} // namespace cxxlens::sdk
