#include <algorithm>
#include <set>
#include <utility>

#include <cxxlens/sdk/application_analysis.hpp>

namespace cxxlens::sdk
{
	struct replay_plan::implementation
	{
		std::string digest;
		std::string capture_bundle_digest;
		std::string compile_unit_id;
		std::string analysis_frontend;
		std::string target_abi;
		std::vector<capture_gap> unresolved;
	};

	struct imported_project::implementation
	{
		std::string id;
		std::string capture_bundle_digest;
		std::vector<replay_plan> replay_plans;
		std::vector<capture_gap> unresolved;
	};

	struct materialization_request::implementation
	{
		relation_engine engine;
		snapshot_draft publication;
		std::vector<std::string> relation_descriptor_ids;
		std::string interpretation;
		provider::provider_selection_request provider;
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

	namespace
	{
		[[nodiscard]] error invalid(std::string field, std::string detail)
		{
			return {"application-analysis.request-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool digest_like(const std::string_view value)
		{
			const auto marker = value.rfind("sha256:");
			if (marker == std::string_view::npos || marker + 7U + 64U != value.size())
				return false;
			return std::ranges::all_of(value.substr(marker + 7U),
									   [](const char byte)
									   {
										   return (byte >= '0' && byte <= '9') ||
											   (byte >= 'a' && byte <= 'f');
									   });
		}
	} // namespace

	replay_plan::replay_plan(std::shared_ptr<const implementation> value) : value_{std::move(value)}
	{
	}

	std::string_view replay_plan::digest() const noexcept
	{
		return value_->digest;
	}
	std::string_view replay_plan::capture_bundle_digest() const noexcept
	{
		return value_->capture_bundle_digest;
	}
	std::string_view replay_plan::compile_unit_id() const noexcept
	{
		return value_->compile_unit_id;
	}
	std::string_view replay_plan::analysis_frontend() const noexcept
	{
		return value_->analysis_frontend;
	}
	std::string_view replay_plan::target_abi() const noexcept
	{
		return value_->target_abi;
	}
	std::span<const capture_gap> replay_plan::unresolved() const noexcept
	{
		return value_->unresolved;
	}

	imported_project::imported_project(std::shared_ptr<const implementation> value)
		: value_{std::move(value)}
	{
	}

	std::string_view imported_project::id() const noexcept
	{
		return value_->id;
	}
	std::string_view imported_project::capture_bundle_digest() const noexcept
	{
		return value_->capture_bundle_digest;
	}
	std::span<const replay_plan> imported_project::replay_plans() const noexcept
	{
		return value_->replay_plans;
	}
	std::span<const capture_gap> imported_project::unresolved() const noexcept
	{
		return value_->unresolved;
	}

	result<imported_project> import_capture(const capture_bundle&, const import_limits limits)
	{
		if (auto valid = limits.validate(); !valid)
			return unexpected(std::move(valid.error()));
		return unexpected(error{"application-analysis.target-unavailable",
								"replay-planner",
								"GCC and MSVC replay targets are not configured"});
	}

	materialization_request::materialization_request(std::shared_ptr<const implementation> value)
		: value_{std::move(value)}
	{
	}

	result<materialization_request>
	materialization_request::make(relation_engine engine,
								  snapshot_draft publication,
								  std::vector<std::string> relation_descriptor_ids,
								  std::string interpretation,
								  provider::provider_selection_request provider,
								  provider::execution_budget budget,
								  const std::stop_token& cancellation)
	{
		if (relation_descriptor_ids.empty())
			return unexpected(invalid("relation_descriptor_ids", "empty"));
		std::set<std::string, std::less<>> unique;
		for (const auto& descriptor_id : relation_descriptor_ids)
		{
			if (!unique.insert(descriptor_id).second)
				return unexpected(invalid("relation_descriptor_ids", "duplicate"));
			if (auto descriptor = engine.require_id(descriptor_id); !descriptor)
				return unexpected(invalid("relation_descriptor_ids", "unknown"));
		}
		if (interpretation.empty())
			return unexpected(invalid("interpretation", "empty"));
		if (auto valid = publication.series.validate(); !valid)
			return unexpected(std::move(valid.error()));
		if (publication.snapshot_semantics_version.major == 0U ||
			!digest_like(publication.catalog_semantic_digest) ||
			(publication.expected_parent_publication &&
			 !digest_like(*publication.expected_parent_publication)))
			return unexpected(invalid("publication", "authority"));
		if (provider.provider_id.empty() || provider.provider_version.major == 0U ||
			!digest_like(provider.provider_binary_digest) ||
			!digest_like(provider.provider_semantic_contract_digest))
			return unexpected(invalid("provider", "identity"));
		if (auto valid = provider.sandbox.validate(); !valid)
			return unexpected(std::move(valid.error()));
		if (provider.fallback_policy)
			if (auto valid = provider.fallback_policy->validate(provider.provider_version); !valid)
				return unexpected(std::move(valid.error()));
		if (auto valid = budget.validate(); !valid)
			return unexpected(std::move(valid.error()));

		auto value =
			std::make_shared<implementation>(implementation{std::move(engine),
															std::move(publication),
															std::move(relation_descriptor_ids),
															std::move(interpretation),
															std::move(provider),
															budget,
															cancellation});
		return materialization_request{std::move(value)};
	}

	std::span<const std::string> materialization_request::relation_descriptor_ids() const noexcept
	{
		return value_->relation_descriptor_ids;
	}
	std::string_view materialization_request::interpretation() const noexcept
	{
		return value_->interpretation;
	}
	const provider::execution_budget& materialization_request::budget() const noexcept
	{
		return value_->budget;
	}

	materialization_result::materialization_result(std::shared_ptr<const implementation> value)
		: value_{std::move(value)}
	{
	}

	materialization_terminal materialization_result::terminal() const noexcept
	{
		return value_->terminal;
	}
	const std::optional<snapshot_handle>&
	materialization_result::published_snapshot() const noexcept
	{
		return value_->published_snapshot;
	}
	std::span<const provider::coverage_unit> materialization_result::coverage() const noexcept
	{
		return value_->coverage;
	}
	std::span<const provider::unresolved_item> materialization_result::unresolved() const noexcept
	{
		return value_->unresolved;
	}
	std::span<const claim_conflict> materialization_result::conflicts() const noexcept
	{
		return value_->conflicts;
	}
	std::span<const differential_disagreement>
	materialization_result::differential_disagreements() const noexcept
	{
		return value_->differential_disagreements;
	}
	const std::optional<application_analysis_provenance>&
	materialization_result::provenance() const noexcept
	{
		return value_->provenance;
	}

	result<materialization_result>
	materialize(snapshot_store&, const imported_project&, const materialization_request& request)
	{
		if (request.value_->cancellation.stop_requested())
		{
			auto value = std::make_shared<materialization_result::implementation>();
			value->terminal = materialization_terminal::cancelled;
			return materialization_result{std::move(value)};
		}
		return unexpected(error{"application-analysis.target-unavailable",
								"materialization",
								"application analysis providers are not configured"});
	}
} // namespace cxxlens::sdk
