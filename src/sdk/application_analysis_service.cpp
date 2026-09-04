#include <algorithm>
#include <new>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

#include <cxxlens/sdk/application_analysis.hpp>

#include "application_analysis_internal.hpp"
#include "application_materialization_adoption_internal.hpp"
#include "application_materialization_execution_internal.hpp"
#include "gcc_replay_planner_internal.hpp"
#include "msvc_replay_planner_internal.hpp"
#include "provider_runtime_internal.hpp"

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

		[[nodiscard]] std::string_view fidelity_name(const replay_fidelity value) noexcept
		{
			switch (value)
			{
				case replay_fidelity::exact:
					return "exact";
				case replay_fidelity::semantics_preserving:
					return "semantics_preserving";
				case replay_fidelity::approximation:
					return "approximation";
				case replay_fidelity::unsupported:
					return "unsupported";
				case replay_fidelity::nonsemantic:
					return "nonsemantic";
			}
			return {};
		}

		[[nodiscard]] bool gap_less(const capture_gap& left, const capture_gap& right)
		{
			return std::tie(left.field, left.state, left.reason, left.completion_action) <
				std::tie(right.field, right.state, right.reason, right.completion_action);
		}

		void canonicalize_gaps(std::vector<capture_gap>& values)
		{
			std::ranges::sort(values, gap_less);
			values.erase(std::ranges::unique(values).begin(), values.end());
		}

		[[nodiscard]] result<std::string> invocation_digest(const replay_plan::implementation& plan)
		{
			std::vector<canonical_value> values;
			values.reserve(plan.effective_arguments.size());
			for (const auto& argument : plan.effective_arguments)
				values.push_back(canonical_value::from_string(argument));
			auto encoded = canonical_binary(canonical_value::from_tuple(std::move(values)));
			if (!encoded)
				return unexpected(std::move(encoded.error()));
			return semantic_digest(
				"application-analysis-effective-invocation.v1",
				std::string{reinterpret_cast<const char*>(encoded->data()), encoded->size()});
		}

		[[nodiscard]] result<std::string>
		environment_digest(const detail::decoded_capture_unit& unit)
		{
			std::vector<canonical_value> values;
			values.reserve(unit.environment_effects.size());
			for (const auto& effect : unit.environment_effects)
				values.push_back(canonical_value::from_tuple({
					canonical_value::from_string(effect.name),
					canonical_value::from_string(effect.state),
					canonical_value::from_string(effect.semantic_value.value_or("")),
					canonical_value::from_string(effect.reason),
					canonical_value::from_string(effect.completion_action),
				}));
			auto encoded = canonical_binary(canonical_value::from_tuple(std::move(values)));
			if (!encoded)
				return unexpected(std::move(encoded.error()));
			return semantic_digest(
				"application-analysis-environment.v1",
				std::string{reinterpret_cast<const char*>(encoded->data()), encoded->size()});
		}

		[[nodiscard]] std::vector<capture_gap>
		capture_gaps_for_unit(const std::span<const capture_gap> gaps, const std::size_t unit_index)
		{
			const auto unit_prefix = "compile_units[" + std::to_string(unit_index) + "].";
			std::vector<capture_gap> output;
			for (const auto& gap : gaps)
				if (!gap.field.starts_with("compile_units[") || gap.field.starts_with(unit_prefix))
					output.push_back(gap);
			return output;
		}

		[[nodiscard]] canonical_value mapping_value(const detail::replay_option_mapping& value)
		{
			std::vector<canonical_value> replay_tokens;
			replay_tokens.reserve(value.replay_tokens.size());
			for (const auto& token : value.replay_tokens)
				replay_tokens.push_back(canonical_value::from_string(token));
			return canonical_value::from_tuple({
				canonical_value::from_string(value.production_token),
				canonical_value::from_tuple(std::move(replay_tokens)),
				canonical_value::from_string(std::string{fidelity_name(value.fidelity)}),
				canonical_value::from_string(value.affected_scope),
				canonical_value::from_string(value.reason),
				canonical_value::from_string(value.completion_action),
			});
		}

		[[nodiscard]] canonical_value gap_value(const capture_gap& value)
		{
			return canonical_value::from_tuple(
				{canonical_value::from_string(value.field),
				 canonical_value::from_string(value.state),
				 canonical_value::from_string(value.reason),
				 canonical_value::from_string(value.completion_action)});
		}

		[[nodiscard]] result<std::shared_ptr<replay_plan::implementation>>
		make_replay_plan(const capture_bundle::implementation& bundle,
						 const detail::decoded_capture_unit& unit,
						 const std::size_t unit_index,
						 const import_limits& limits)
		{
			std::vector<std::string> effective_arguments;
			std::vector<detail::replay_option_mapping> option_mappings;
			std::vector<capture_gap> mapping_unresolved;
			std::string analysis_frontend;
			if (bundle.projection.toolchain_family == "gcc")
			{
				auto mapped = detail::map_gcc_16_2_replay_arguments(
					bundle.projection, unit, unit_index, limits);
				if (!mapped)
					return unexpected(std::move(mapped.error()));
				effective_arguments = std::move(mapped->effective_arguments);
				option_mappings = std::move(mapped->option_mappings);
				mapping_unresolved = std::move(mapped->unresolved);
				analysis_frontend = "clang-23.1.0-gcc-mode";
			}
			else
			{
				auto mapped = detail::map_msvc_19_51_replay_arguments(
					bundle.projection, unit, unit_index, limits);
				if (!mapped)
					return unexpected(std::move(mapped.error()));
				effective_arguments = std::move(mapped->effective_arguments);
				option_mappings = std::move(mapped->option_mappings);
				mapping_unresolved = std::move(mapped->unresolved);
				analysis_frontend = "clang-cl-23.1.0";
			}

			auto value = std::make_shared<replay_plan::implementation>();
			value->capture_bundle_digest = bundle.digest;
			value->compile_unit_id = unit.compile_unit_id;
			value->analysis_frontend = std::move(analysis_frontend);
			value->target_abi = bundle.target_abi;
			value->source_closure_digest = unit.source_closure_digest;
			if (value->source_closure_digest.empty())
				return unexpected(error{"application-analysis.capture-invalid",
										"source_closure",
										"missing-validated-binding"});
			value->effective_arguments = std::move(effective_arguments);
			value->option_mappings = std::move(option_mappings);
			value->unresolved = capture_gaps_for_unit(bundle.gaps, unit_index);
			value->unresolved.insert(
				value->unresolved.end(), mapping_unresolved.begin(), mapping_unresolved.end());
			canonicalize_gaps(value->unresolved);

			std::vector<canonical_value> effective;
			effective.reserve(value->effective_arguments.size());
			for (const auto& token : value->effective_arguments)
				effective.push_back(canonical_value::from_string(token));
			std::vector<canonical_value> mappings;
			mappings.reserve(value->option_mappings.size());
			for (const auto& mapping : value->option_mappings)
				mappings.push_back(mapping_value(mapping));
			std::vector<canonical_value> unresolved;
			unresolved.reserve(value->unresolved.size());
			for (const auto& gap : value->unresolved)
				unresolved.push_back(gap_value(gap));
			auto encoded = canonical_binary(canonical_value::from_tuple({
				canonical_value::from_string("cxxlens.compiler-replay-plan.v1"),
				canonical_value::from_string(value->capture_bundle_digest),
				canonical_value::from_string(value->compile_unit_id),
				canonical_value::from_string(value->analysis_frontend),
				canonical_value::from_string(value->target_abi),
				canonical_value::from_tuple(std::move(effective)),
				canonical_value::from_tuple(std::move(mappings)),
				canonical_value::from_string(value->source_closure_digest),
				canonical_value::from_tuple(std::move(unresolved)),
			}));
			if (!encoded)
				return unexpected(
					error{"application-analysis.capture-invalid", "replay_plan", "encoding"});
			value->digest = content_digest(*encoded);
			return value;
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
	std::string_view imported_project::catalog_semantic_digest() const noexcept
	{
		return value_->catalog.catalog_digest;
	}
	std::span<const replay_plan> imported_project::replay_plans() const noexcept
	{
		return value_->replay_plans;
	}
	std::span<const capture_gap> imported_project::unresolved() const noexcept
	{
		return value_->unresolved;
	}

	result<imported_project> import_capture(const capture_bundle& bundle,
											const import_limits limits)
	{
		try
		{
			if (auto valid = limits.validate(); !valid)
				return unexpected(std::move(valid.error()));
			if (bundle.value_->projection.compile_units.size() > limits.maximum_compile_units)
				return unexpected(
					error{"application-analysis.import-limit-exceeded", "compile_units", "count"});

			auto value = std::make_shared<imported_project::implementation>();
			value->capture_bundle_digest = bundle.value_->digest;
			value->capture = bundle.value_;
			value->replay_plans.reserve(bundle.value_->projection.compile_units.size());
			value->replay_plan_values.reserve(bundle.value_->projection.compile_units.size());
			std::vector<catalog_compile_unit> catalog_units;
			catalog_units.reserve(bundle.value_->projection.compile_units.size());
			std::vector<canonical_value> catalog_environments;
			catalog_environments.reserve(bundle.value_->projection.compile_units.size());
			for (std::size_t index{}; index < bundle.value_->projection.compile_units.size();
				 ++index)
			{
				auto plan_value = make_replay_plan(
					*bundle.value_, bundle.value_->projection.compile_units[index], index, limits);
				if (!plan_value)
					return unexpected(std::move(plan_value.error()));
				value->replay_plan_values.push_back(*plan_value);
				replay_plan plan{std::move(*plan_value)};
				auto invocation = invocation_digest(*plan.value_);
				auto environment =
					environment_digest(bundle.value_->projection.compile_units[index]);
				if (!invocation || !environment)
					return unexpected(!invocation ? std::move(invocation.error())
												  : std::move(environment.error()));
				catalog_units.push_back(
					{std::string{plan.compile_unit_id()},
					 std::move(*invocation),
					 bundle.value_->projection.compile_units[index].source_content_digest,
					 *environment});
				catalog_environments.push_back(
					canonical_value::from_string(std::move(*environment)));
				value->unresolved.insert(
					value->unresolved.end(), plan.unresolved().begin(), plan.unresolved().end());
				value->replay_plans.push_back(std::move(plan));
			}
			auto encoded_environments =
				canonical_binary(canonical_value::from_tuple(std::move(catalog_environments)));
			if (!encoded_environments)
				return unexpected(std::move(encoded_environments.error()));
			auto catalog_environment = semantic_digest(
				"application-analysis-project-environment.v1",
				std::string{reinterpret_cast<const char*>(encoded_environments->data()),
							encoded_environments->size()});
			if (!catalog_environment)
				return unexpected(std::move(catalog_environment.error()));
			auto catalog = project_catalog::make(bundle.value_->logical_project_root,
												 std::move(*catalog_environment),
												 std::move(catalog_units));
			if (!catalog)
				return unexpected(std::move(catalog.error()));
			value->catalog = std::move(*catalog);
			canonicalize_gaps(value->unresolved);
			std::vector<canonical_value> fields;
			fields.reserve(value->replay_plans.size() + 1U);
			fields.push_back(canonical_value::from_string(value->capture_bundle_digest));
			for (const auto& plan : value->replay_plans)
				fields.push_back(canonical_value::from_string(std::string{plan.digest()}));
			auto identity = canonical_identity_digest("application-imported-project", fields);
			if (!identity)
				return unexpected(
					error{"application-analysis.capture-invalid", "imported_project", "identity"});
			value->id = "imported-project:" + *identity;
			return imported_project{std::move(value)};
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(
				error{"application-analysis.import-limit-exceeded", "replay_plan", "allocation"});
		}
		catch (const std::length_error&)
		{
			return unexpected(error{
				"application-analysis.import-limit-exceeded", "replay_plan", "allocation-length"});
		}
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
															std::nullopt,
															budget,
															cancellation});
		return materialization_request{std::move(value)};
	}

	result<materialization_request>
	materialization_request::make(relation_engine engine,
								  snapshot_draft publication,
								  std::vector<std::string> relation_descriptor_ids,
								  std::string interpretation,
								  provider::provider_selection_request provider,
								  std::vector<provider::provider_candidate> candidates,
								  provider::execution_budget budget,
								  const std::stop_token& cancellation)
	{
		auto request = make(std::move(engine),
							std::move(publication),
							std::move(relation_descriptor_ids),
							std::move(interpretation),
							std::move(provider),
							budget,
							cancellation);
		if (!request)
			return unexpected(std::move(request.error()));
		auto selection = provider::select_provider(request->value_->provider, candidates);
		if (!selection)
			return unexpected(std::move(selection.error()));
		auto value = std::make_shared<implementation>(*request->value_);
		value->selection = std::move(*selection);
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

	result<materialization_result> materialize(snapshot_store& store,
											   const imported_project& project,
											   const materialization_request& request)
	{
		if (request.value_->cancellation.stop_requested())
		{
			auto value = std::make_shared<materialization_result::implementation>();
			value->terminal = materialization_terminal::cancelled;
			return materialization_result{std::move(value)};
		}
		if (!request.value_->selection)
			return unexpected(error{"application-analysis.target-unavailable",
									"materialization",
									"application analysis providers are not configured"});
		auto plan = detail::make_application_materialization_execution_plan(
			*project.value_,
			request.value_->engine,
			request.value_->publication,
			request.value_->relation_descriptor_ids,
			request.value_->interpretation,
			*request.value_->selection,
			request.value_->budget,
			request.value_->cancellation);
		if (!plan)
			return unexpected(std::move(plan.error()));
		auto processes = provider::make_system_provider_process_port();
		if (!processes)
			return unexpected(error{"application-analysis.target-unavailable",
									"provider-runtime",
									"system process port is unavailable"});
		std::vector<detail::prepared_application_materialization> prepared;
		prepared.reserve(plan->units.size());
		for (auto& unit : plan->units)
		{
			auto executed = provider::detail::execute_provider_process(*processes, unit.process);
			if (!executed)
				return unexpected(std::move(executed.error()));
			if (!executed->succeeded())
			{
				auto value = std::make_shared<materialization_result::implementation>();
				value->terminal = executed->terminal == "provider.cancelled"
					? materialization_terminal::cancelled
					: (executed->sealing_error ? materialization_terminal::rejected
											   : materialization_terminal::failed);
				value->unresolved = std::move(executed->diagnostics);
				return materialization_result{std::move(value)};
			}
			if (auto valid = provider::detail::validate_provider_process_runtime_binding(
					*executed, unit.process);
				!valid)
				return unexpected(std::move(valid.error()));
			auto runtime_receipt =
				provider::detail::provider_runtime_receipt_digest(*executed->runtime_receipt);
			if (!runtime_receipt)
				return unexpected(std::move(runtime_receipt.error()));
			const auto& manifest = unit.process.selection.selected_candidate().description;
			detail::materialization_runtime_binding runtime{
				manifest.provider_id,
				manifest.provider_version,
				executed->measured_executable_digest,
				manifest.provider_semantic_contract_digest,
				unit.process.task_input_digest,
				*runtime_receipt};
			auto unit_prepared =
				detail::prepare_sealed_application_materialization(request.value_->engine,
																   unit.task,
																   *executed->sealed,
																   std::move(runtime),
																   *runtime_receipt,
																   unit.replay_plan_digest,
																   unit.host_partitions);
			if (!unit_prepared)
				return unexpected(std::move(unit_prepared.error()));
			prepared.push_back(std::move(*unit_prepared));
		}
		auto adopted = detail::publish_prepared_application_materializations(
			request.value_->engine, store, std::move(prepared));
		if (!adopted)
			return unexpected(std::move(adopted.error()));
		const auto& manifest =
			plan->units.front().process.selection.selected_candidate().description;

		auto value = std::make_shared<materialization_result::implementation>();
		value->terminal =
			adopted->publication.terminal == detail::materialization_terminal::complete
			? materialization_terminal::published_complete
			: materialization_terminal::published_partial;
		value->published_snapshot = std::move(adopted->publication.snapshot);
		value->coverage = std::move(adopted->coverage);
		value->unresolved = std::move(adopted->unresolved);
		value->conflicts = std::move(adopted->conflicts);
		value->differential_disagreements = std::move(adopted->differential_disagreements);
		value->provenance =
			application_analysis_provenance{manifest.provider_id,
											manifest.provider_version,
											manifest.provider_binary_digest,
											manifest.provider_semantic_contract_digest,
											adopted->provider_input_digest,
											adopted->replay_plan_digest,
											adopted->runtime_receipt_digest};
		return materialization_result{std::move(value)};
	}
} // namespace cxxlens::sdk
