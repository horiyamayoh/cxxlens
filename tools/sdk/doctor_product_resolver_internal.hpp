#pragma once

#include "doctor_product_catalog_internal.hpp"

namespace cxxlens::sdk::doctor
{
	struct capability_result
	{
		std::string id;
		std::string kind;
		resolution_state state{resolution_state::unknown};
		diagnosis_reason reason{diagnosis_reason::missing_capability};
		std::vector<std::string> dependencies;
		std::vector<std::string> candidate_ids;
	};

	struct missing_result
	{
		std::string capability_id;
		diagnosis_reason reason{diagnosis_reason::missing_capability};
		std::string explanation;
	};

	struct plan_step
	{
		std::string id;
		std::vector<std::string> dependencies;
		std::string action;
		std::string unlocks;
		diagnosis_reason reason{diagnosis_reason::missing_capability};
	};

	struct conflict_result
	{
		std::string capability_id;
		std::vector<std::string> candidate_ids;
	};

	struct resolution
	{
		std::string catalog_binding_id;
		std::string catalog_document_version;
		std::string use_case_id;
		std::string consumer;
		std::string question;
		resolution_state state{resolution_state::unknown};
		diagnosis_reason reason{diagnosis_reason::missing_capability};
		std::vector<capability_result> capability_path;
		std::vector<missing_result> missing;
		std::vector<plan_step> completion_plan;
		std::vector<std::string> unresolved;
		std::vector<conflict_result> conflicts;
		std::vector<std::string> provenance;
	};

	[[nodiscard]] inline std::string kind_name(const capability_kind kind)
	{
		switch (kind)
		{
			case capability_kind::input:
				return "input";
			case capability_kind::provider:
				return "provider";
			case capability_kind::relation:
				return "relation";
			case capability_kind::query:
				return "query";
			case capability_kind::store:
				return "store";
			case capability_kind::recipe:
				return "recipe";
		}
		return "unknown";
	}

	struct provider_selection
	{
		resolution_state state{resolution_state::unknown};
		diagnosis_reason reason{diagnosis_reason::missing_provider};
		const provider_candidate* candidate{};
		std::vector<std::string> candidate_ids;
	};

	[[nodiscard]] inline std::optional<unsigned>
	sandbox_assurance_rank(const std::string_view assurance) noexcept
	{
		if (assurance == "none")
			return 0U;
		if (assurance == "best_effort")
			return 1U;
		if (assurance == "enforced")
			return 2U;
		return std::nullopt;
	}

	[[nodiscard]] inline bool bounded_project_context(const project_context& project) noexcept
	{
		std::size_t bytes{};
		std::size_t nodes{};
		const auto node = [&](const std::size_t count = 1U)
		{
			if (count > maximum_project_node_count - nodes)
				return false;
			nodes += count;
			return true;
		};
		const auto text = [&](const std::string& value)
		{
			if (value.size() > maximum_json_string_bytes ||
				value.size() > maximum_project_bytes - bytes || !valid_utf8(value) || !node())
				return false;
			bytes += value.size();
			return true;
		};
		const auto texts = [&](const std::vector<std::string>& values)
		{
			return values.size() <= maximum_json_collection_count && node() &&
				std::ranges::all_of(values, text);
		};
		if (!node() || !text(project.project_id) || !text(project.catalog_id) ||
			!text(project.catalog_digest) || !text(project.logical_root) ||
			!text(project.environment_digest) || !text(project.environment.release_version) ||
			!text(project.environment.surface) || !text(project.environment.os) ||
			!text(project.environment.architecture) ||
			!text(project.environment.compiler_provider_major) ||
			!text(project.environment.linkage) ||
			(project.source_input &&
			 (!text(project.source_snapshot_id) || !text(project.compilation_database_id))) ||
			(project.store_input &&
			 (!text(project.store_backend) || !text(project.store_format))) ||
			project.provider_candidates.size() > maximum_json_collection_count || !node())
			return false;
		for (const auto& candidate : project.provider_candidates)
		{
			if (!node() || !text(candidate.candidate_id) || !text(candidate.provider_id) ||
				!text(candidate.provider_version) || !text(candidate.package_identity) ||
				!text(candidate.provider_manifest_digest) ||
				!text(candidate.provider_binary_digest) ||
				!text(candidate.provider_semantic_contract_digest) || !texts(candidate.features) ||
				!texts(candidate.relations) || !texts(candidate.interpretations) ||
				!text(candidate.sandbox_minimum) || !text(candidate.sandbox_policy_digest))
				return false;
		}
		return true;
	}

	[[nodiscard]] inline std::variant<project_context, product_error>
	canonical_project_context(const project_context& project)
	{
		if (!bounded_project_context(project))
			return product_error{"doctor.project-invalid", "project", "resource-bound"};
		if (project.provider_candidates.size() > maximum_capability_count)
			return product_error{"doctor.project-invalid", "provider_candidates", "count-limit"};
		if (!strict_id(project.project_id) || !strict_id(project.catalog_id) ||
			!semantic_digest_value(project.catalog_digest) ||
			project.catalog_id != "catalog:" + project.catalog_digest ||
			!strict_id(project.logical_root) || !project.logical_root.starts_with("project://") ||
			project.logical_root.size() == std::string_view{"project://"}.size() ||
			!digest_value(project.environment_digest) ||
			!semantic_version_value(project.environment.release_version) ||
			project.environment.release_version.starts_with("0.") ||
			!strict_id(project.environment.surface) || !strict_id(project.environment.os) ||
			!strict_id(project.environment.architecture) ||
			!strict_id(project.environment.compiler_provider_major) ||
			(project.environment.linkage != "static" && project.environment.linkage != "shared") ||
			(project.source_input &&
			 (!strict_id(project.source_snapshot_id) ||
			  !strict_id(project.compilation_database_id))) ||
			(!project.source_input &&
			 (!project.source_snapshot_id.empty() || !project.compilation_database_id.empty())) ||
			(project.store_input &&
			 ((project.store_backend != "memory" && project.store_backend != "sqlite") ||
			  project.store_format != "cxxlens.snapshot.v3")))
			return product_error{"doctor.project-invalid", "project", "invalid-direct-context"};
		auto valid_set = [](const auto& values)
		{
			if (!std::ranges::all_of(values, strict_id))
				return false;
			for (auto left = values.begin(); left != values.end(); ++left)
				for (auto right = std::next(left); right != values.end(); ++right)
					if (*left == *right)
						return false;
			return true;
		};
		for (const auto& candidate : project.provider_candidates)
		{
			if (candidate.features.size() > maximum_capability_count ||
				candidate.relations.size() > maximum_capability_count ||
				candidate.interpretations.size() > maximum_capability_count)
				return product_error{
					"doctor.project-invalid", "provider_candidates", "count-limit"};
			if (!semantic_digest_value(candidate.candidate_id) ||
				!strict_id(candidate.provider_id) ||
				!semantic_version_value(candidate.provider_version) ||
				candidate.provider_version.starts_with("0.") ||
				!strict_id(candidate.package_identity) ||
				!digest_value(candidate.provider_manifest_digest) ||
				!digest_value(candidate.provider_binary_digest) ||
				!digest_value(candidate.provider_semantic_contract_digest) ||
				candidate.protocol_major == 0U ||
				!sandbox_assurance_rank(candidate.sandbox_minimum) ||
				!digest_value(candidate.sandbox_policy_digest) || !valid_set(candidate.features) ||
				!valid_set(candidate.relations) || !valid_set(candidate.interpretations))
				return product_error{
					"doctor.project-invalid", "provider_candidates", "invalid-direct-candidate"};
		}
		project_context output = project;
		for (auto& candidate : output.provider_candidates)
		{
			std::ranges::sort(candidate.features);
			std::ranges::sort(candidate.relations);
			std::ranges::sort(candidate.interpretations);
		}
		std::ranges::sort(output.provider_candidates, {}, &provider_candidate::candidate_id);
		if (std::ranges::adjacent_find(output.provider_candidates,
									   [](const auto& left, const auto& right)
									   {
										   return left.candidate_id == right.candidate_id;
									   }) != output.provider_candidates.end())
			return product_error{
				"doctor.project-invalid", "provider_candidates", "duplicate-candidate-id"};
		return output;
	}

	[[nodiscard]] inline provider_selection
	classify_provider_candidate(const provider_candidate& candidate,
								const project_context& project,
								const provider_support_spec& support)
	{
		const auto offered = sandbox_assurance_rank(candidate.sandbox_minimum);
		const auto required = sandbox_assurance_rank(support.sandbox_minimum);
		if (candidate.protocol_major != support.protocol_major ||
			candidate.protocol_minor != support.protocol_minor || !offered || !required ||
			*offered < *required ||
			std::ranges::find(support.supported_tuples, project.environment) ==
				support.supported_tuples.end())
			return {resolution_state::disproved,
					diagnosis_reason::unsupported_tuple,
					&candidate,
					{candidate.candidate_id}};
		return {
			resolution_state::proved, diagnosis_reason::none, &candidate, {candidate.candidate_id}};
	}

	[[nodiscard]] inline provider_selection select_provider(const project_context& project,
															const provider_support_spec& support)
	{
		if (project.provider_candidates.empty())
			return {};
		// Diagnose configured candidates; this does not execute them or prove query completeness.
		std::vector<provider_selection> proved;
		std::vector<provider_selection> unknown;
		std::vector<provider_selection> disproved;
		for (const auto& candidate : project.provider_candidates)
		{
			auto classified = classify_provider_candidate(candidate, project, support);
			if (classified.state == resolution_state::proved)
				proved.push_back(std::move(classified));
			else if (classified.state == resolution_state::unknown)
				unknown.push_back(std::move(classified));
			else
				disproved.push_back(std::move(classified));
		}
		if (proved.size() > 1U)
		{
			provider_selection output{resolution_state::conflicting,
									  diagnosis_reason::conflicting_capability,
									  nullptr,
									  {}};
			for (const auto& item : proved)
				output.candidate_ids.push_back(item.candidate->candidate_id);
			std::ranges::sort(output.candidate_ids);
			return output;
		}
		if (proved.size() == 1U)
			return proved.front();
		if (!unknown.empty())
			return unknown.front();
		return disproved.front();
	}

	[[nodiscard]] inline std::variant<std::vector<capability_spec>, product_error>
	topological_path(const capability_catalog& catalog, const use_case_spec& use_case)
	{
		if (catalog.capabilities.empty() ||
			catalog.capabilities.size() > maximum_capability_count ||
			use_case.capability_path.empty() ||
			use_case.capability_path.size() > maximum_capability_count)
			return product_error{"doctor.catalog-invalid", "capability_path", "count"};
		std::map<std::string, capability_spec, std::less<>> capabilities;
		for (auto capability : catalog.capabilities)
		{
			if (capability.dependencies.size() > maximum_capability_count ||
				capability.relation_ids.size() > maximum_capability_count)
				return product_error{"doctor.catalog-invalid", "capabilities", "count-limit"};
			std::ranges::sort(capability.dependencies);
			std::ranges::sort(capability.relation_ids);
			if (!strict_id(capability.id) || !derived_capability_probe(capability) ||
				capability.completion_action.empty() ||
				capability.completion_action.size() > maximum_json_string_bytes ||
				std::ranges::adjacent_find(capability.dependencies) !=
					capability.dependencies.end() ||
				std::ranges::adjacent_find(capability.relation_ids) !=
					capability.relation_ids.end() ||
				!std::ranges::all_of(capability.dependencies, strict_id) ||
				!std::ranges::all_of(capability.relation_ids, strict_id) ||
				!capabilities.emplace(capability.id, std::move(capability)).second)
				return product_error{
					"doctor.catalog-invalid", "capabilities", "invalid-or-duplicate-id"};
		}
		std::set<std::string, std::less<>> requested;
		for (const auto& id : use_case.capability_path)
			if (!strict_id(id) || !requested.insert(id).second || !capabilities.contains(id))
				return product_error{
					"doctor.catalog-invalid", "capability_path", "unknown-or-duplicate"};
		std::map<std::string, std::size_t, std::less<>> indegree;
		std::map<std::string, std::vector<std::string>, std::less<>> dependants;
		for (const auto& id : requested)
		{
			const auto& capability = capabilities.at(id);
			std::set<std::string, std::less<>> unique_dependencies;
			for (const auto& dependency : capability.dependencies)
			{
				if (!requested.contains(dependency) ||
					!unique_dependencies.insert(dependency).second)
					return product_error{
						"doctor.catalog-invalid", "capabilities", "invalid-dependency"};
				dependants[dependency].push_back(id);
			}
			indegree[id] = capability.dependencies.size();
		}
		std::set<std::string, std::less<>> ready;
		for (const auto& [id, count] : indegree)
			if (count == 0U)
				ready.insert(id);
		std::vector<capability_spec> output;
		output.reserve(requested.size());
		while (!ready.empty())
		{
			const auto id = *ready.begin();
			ready.erase(ready.begin());
			output.push_back(capabilities.at(id));
			for (const auto& dependant : dependants[id])
				if (--indegree[dependant] == 0U)
					ready.insert(dependant);
		}
		if (output.size() != requested.size())
			return product_error{"doctor.catalog-invalid", "capabilities", "dependency-cycle"};
		return output;
	}

	[[nodiscard]] inline capability_result
	evaluate_capability(const capability_spec& capability,
						const project_context& project,
						const capability_catalog& catalog,
						const provider_selection& selection,
						const std::map<std::string, capability_result, std::less<>>& prior,
						const relation_registry& registry)
	{
		capability_result output{capability.id,
								 kind_name(capability.kind),
								 resolution_state::unknown,
								 diagnosis_reason::missing_capability,
								 capability.dependencies,
								 {}};
		std::vector<std::string> conflicting_candidate_ids;
		bool has_disproved_dependency{};
		bool has_unresolved_dependency{};
		for (const auto& dependency : capability.dependencies)
		{
			const auto found = prior.find(dependency);
			if (found == prior.end())
				return output;
			if (found->second.state == resolution_state::conflicting)
				conflicting_candidate_ids.insert(conflicting_candidate_ids.end(),
												 found->second.candidate_ids.begin(),
												 found->second.candidate_ids.end());
			else if (found->second.state == resolution_state::disproved)
				has_disproved_dependency = true;
			else if (found->second.state != resolution_state::proved)
				has_unresolved_dependency = true;
		}
		if (!conflicting_candidate_ids.empty())
		{
			std::ranges::sort(conflicting_candidate_ids);
			conflicting_candidate_ids.erase(std::ranges::unique(conflicting_candidate_ids).begin(),
											conflicting_candidate_ids.end());
			output.state = resolution_state::conflicting;
			output.reason = diagnosis_reason::conflicting_capability;
			output.candidate_ids = std::move(conflicting_candidate_ids);
			return output;
		}
		if (has_disproved_dependency)
		{
			output.state = resolution_state::disproved;
			output.reason = diagnosis_reason::disproved_dependency;
			return output;
		}
		if (has_unresolved_dependency)
		{
			output.state = resolution_state::partial;
			output.reason = diagnosis_reason::unknown_dependency;
			return output;
		}
		const auto probe = derived_capability_probe(capability);
		if (!probe)
			return output;
		switch (*probe)
		{
			case capability_probe::project_catalog:
				output.state = resolution_state::proved;
				output.reason = diagnosis_reason::none;
				break;
			case capability_probe::source_closure:
				output.state = resolution_state::unknown;
				output.reason = project.source_input ? diagnosis_reason::source_closure_unavailable
													 : diagnosis_reason::missing_input;
				break;
			case capability_probe::provider_protocol:
				output.state = selection.state;
				output.reason = selection.reason;
				output.candidate_ids = selection.candidate_ids;
				break;
			case capability_probe::provider_features:
				if (selection.candidate == nullptr)
					break;
				output.state = std::ranges::includes(selection.candidate->features,
													 catalog.provider_support.required_features)
					? resolution_state::proved
					: resolution_state::disproved;
				output.reason = output.state == resolution_state::proved
					? diagnosis_reason::none
					: diagnosis_reason::unsupported_tuple;
				break;
			case capability_probe::provider_relations:
				if (selection.candidate == nullptr)
					break;
				for (const auto& relation_id : capability.relation_ids)
				{
					const auto parsed = split_relation_id(relation_id);
					if (!parsed || !parsed->semantic_major ||
						!registry.require(parsed->name, *parsed->semantic_major))
						return output;
				}
				output.state = std::ranges::includes(selection.candidate->relations,
													 capability.relation_ids) &&
						std::ranges::includes(selection.candidate->interpretations,
											  catalog.provider_support.required_interpretations)
					? resolution_state::proved
					: resolution_state::disproved;
				output.reason = output.state == resolution_state::proved
					? diagnosis_reason::none
					: diagnosis_reason::unsupported_tuple;
				break;
			case capability_probe::dependency_only:
				output.state = resolution_state::proved;
				output.reason = diagnosis_reason::none;
				break;
			case capability_probe::store:
				if (!project.store_input)
				{
					output.state = resolution_state::unknown;
					output.reason = diagnosis_reason::missing_input;
				}
				else
				{
					const auto supported =
						std::ranges::find(catalog.store_support.backends, project.store_backend) !=
							catalog.store_support.backends.end() &&
						project.store_format == catalog.store_support.format;
					if (!supported)
					{
						output.state = resolution_state::disproved;
						output.reason = diagnosis_reason::unsupported_tuple;
					}
					else
					{
						output.state = resolution_state::proved;
						output.reason = diagnosis_reason::none;
					}
				}
				break;
		}
		return output;
	}

	[[nodiscard]] inline std::variant<resolution, product_error>
	resolve(const std::string_view use_case_id,
			const project_context& input_project,
			const capability_catalog& catalog_token)
	{
		if (!strict_id(use_case_id))
			return product_error{"doctor.unknown-use-case", "use_case_id", "invalid"};
		auto canonicalized_project = canonical_project_context(input_project);
		if (std::holds_alternative<product_error>(canonicalized_project))
			return std::get<product_error>(std::move(canonicalized_project));
		auto project = std::get<project_context>(std::move(canonicalized_project));
		auto catalog = catalog_token;
		if (catalog.binding_id != "cxxlens.sdk-doctor-catalog.v1" ||
			catalog.document_version != "1.0.0")
			return product_error{"doctor.catalog-invalid", "catalog_binding", "unsupported"};
		if (catalog.use_cases.empty() || catalog.use_cases.size() > maximum_capability_count ||
			catalog.provider_support.required_features.size() > maximum_capability_count ||
			catalog.provider_support.required_relations.size() > maximum_capability_count ||
			catalog.provider_support.required_interpretations.size() > maximum_capability_count ||
			catalog.provider_support.supported_tuples.empty() ||
			catalog.provider_support.supported_tuples.size() > maximum_capability_count ||
			catalog.store_support.backends.empty() ||
			catalog.store_support.backends.size() > maximum_capability_count)
			return product_error{"doctor.catalog-invalid", "catalog", "count-limit"};
		auto sort_unique = [](auto& values)
		{
			std::ranges::sort(values);
			return std::ranges::adjacent_find(values) == values.end();
		};
		if (!sort_unique(catalog.provider_support.required_features) ||
			!sort_unique(catalog.provider_support.required_relations) ||
			!sort_unique(catalog.provider_support.required_interpretations) ||
			!sort_unique(catalog.provider_support.supported_tuples) ||
			!sort_unique(catalog.store_support.backends))
			return product_error{"doctor.catalog-invalid", "catalog", "duplicate-value"};
		if (catalog.provider_support.protocol_major == 0U ||
			!sandbox_assurance_rank(catalog.provider_support.sandbox_minimum) ||
			!strict_id(catalog.store_support.format) ||
			!std::ranges::all_of(catalog.provider_support.required_features, strict_id) ||
			!std::ranges::all_of(catalog.provider_support.required_relations, strict_id) ||
			!std::ranges::all_of(catalog.provider_support.required_interpretations, strict_id) ||
			!std::ranges::all_of(catalog.store_support.backends, strict_id))
			return product_error{"doctor.catalog-invalid", "catalog", "invalid-value"};
		std::set<std::string, std::less<>> use_case_ids;
		for (const auto& use_case : catalog.use_cases)
			if (!strict_id(use_case.id) || !strict_id(use_case.consumer) ||
				use_case.question.empty() || use_case.question.size() > maximum_json_string_bytes ||
				!use_case_ids.insert(use_case.id).second)
				return product_error{
					"doctor.catalog-invalid", "use_cases", "invalid-or-duplicate-id"};
		for (const auto& tuple : catalog.provider_support.supported_tuples)
			if (!semantic_version_value(tuple.release_version) ||
				tuple.release_version.starts_with("0.") || !strict_id(tuple.surface) ||
				!strict_id(tuple.os) || !strict_id(tuple.architecture) ||
				!strict_id(tuple.compiler_provider_major) ||
				(tuple.linkage != "static" && tuple.linkage != "shared"))
				return product_error{"doctor.catalog-invalid", "supported_tuples", "invalid"};
		std::set<std::string, std::less<>> catalog_relation_ids;
		for (const auto& capability : catalog.capabilities)
			catalog_relation_ids.insert(capability.relation_ids.begin(),
										capability.relation_ids.end());
		if (!std::ranges::equal(catalog_relation_ids, catalog.provider_support.required_relations))
			return product_error{"doctor.catalog-invalid", "provider_support", "relation-mismatch"};
		const auto found_use_case =
			std::ranges::find(catalog.use_cases, use_case_id, &use_case_spec::id);
		if (found_use_case == catalog.use_cases.end())
			return product_error{"doctor.unknown-use-case", "use_case_id", "not-admitted"};
		auto path = topological_path(catalog, *found_use_case);
		if (std::holds_alternative<product_error>(path))
			return std::get<product_error>(std::move(path));
		auto registry = known_relation_registry();
		if (!registry)
			return product_error{registry.error().code, "relation", registry.error().detail};
		const auto selection = select_provider(project, catalog.provider_support);
		resolution output;
		output.catalog_binding_id = catalog.binding_id;
		output.catalog_document_version = catalog.document_version;
		output.use_case_id = found_use_case->id;
		output.consumer = found_use_case->consumer;
		output.question = found_use_case->question;
		output.provenance.push_back("project.catalog-id=" + project.catalog_id);
		for (const auto& candidate_id : selection.candidate_ids)
			output.provenance.push_back("provider.candidate-id=" + candidate_id);
		std::map<std::string, capability_result, std::less<>> by_id;
		std::optional<diagnosis_reason> first_actionable_reason;
		for (const auto& capability : std::get<std::vector<capability_spec>>(path))
		{
			auto result =
				evaluate_capability(capability, project, catalog, selection, by_id, *registry);
			by_id.emplace(capability.id, result);
			output.capability_path.push_back(result);
			if (result.state == resolution_state::unknown ||
				result.state == resolution_state::partial)
				output.unresolved.push_back(capability.id);
			if (result.state == resolution_state::conflicting)
				output.conflicts.push_back({capability.id, result.candidate_ids});
			if (result.state != resolution_state::proved)
			{
				output.missing.push_back(
					{capability.id,
					 result.reason,
					 "The capability cannot be proved for this project context."});
				const bool actionable = result.state != resolution_state::conflicting &&
					std::ranges::all_of(capability.dependencies,
										[&](const std::string& dependency)
										{
											const auto found = by_id.find(dependency);
											return found != by_id.end() &&
												found->second.state == resolution_state::proved;
										});
				if (actionable)
				{
					if (!first_actionable_reason)
						first_actionable_reason = result.reason;
					output.completion_plan.push_back({"completion." + capability.id,
													  capability.dependencies,
													  capability.completion_action,
													  capability.id,
													  result.reason});
				}
			}
		}
		auto first_with_state = [&](const resolution_state state) -> const capability_result*
		{
			const auto found =
				std::ranges::find(output.capability_path, state, &capability_result::state);
			return found == output.capability_path.end() ? nullptr : &*found;
		};
		const auto first_non_proved =
			std::ranges::find_if(output.capability_path,
								 [](const capability_result& result)
								 {
									 return result.state != resolution_state::proved;
								 });
		const bool has_proved =
			std::ranges::any_of(output.capability_path,
								[](const capability_result& result)
								{
									return result.state == resolution_state::proved;
								});
		const bool has_non_proved = first_non_proved != output.capability_path.end();
		if (const auto* conflicting_item = first_with_state(resolution_state::conflicting))
		{
			output.state = resolution_state::conflicting;
			output.reason = conflicting_item->reason;
			output.completion_plan.clear();
		}
		else if (has_proved && has_non_proved)
		{
			output.state = resolution_state::partial;
			output.reason = first_actionable_reason.value_or(first_non_proved->reason);
		}
		else if (!has_non_proved)
			output.state = resolution_state::proved, output.reason = diagnosis_reason::none;
		else if (const auto* disproved_item = first_with_state(resolution_state::disproved))
			output.state = resolution_state::disproved, output.reason = disproved_item->reason;
		else
			output.state = resolution_state::unknown, output.reason = first_non_proved->reason;
		return output;
	}

	[[nodiscard]] inline std::variant<resolution, product_error>
	resolve(const std::string_view use_case_id, const project_context& project)
	{
		const installed_product_catalog_loader loader;
		auto loaded = loader.load();
		if (std::holds_alternative<product_error>(loaded))
			return std::get<product_error>(std::move(loaded));
		return resolve(use_case_id, project, std::get<capability_catalog>(std::move(loaded)));
	}

	[[nodiscard]] inline json_value to_json(const resolution& value)
	{
		json_value::array_type path;
		for (const auto& item : value.capability_path)
		{
			json_value::array_type dependencies;
			for (const auto& dependency : item.dependencies)
				dependencies.push_back(json_value::string_value(dependency));
			path.push_back(json_value::object_value({
				{"id", json_value::string_value(item.id)},
				{"kind", json_value::string_value(item.kind)},
				{"requires", json_value::array_value(std::move(dependencies))},
				{"reason_code", json_value::string_value(std::string{reason_name(item.reason)})},
				{"state", json_value::string_value(std::string{state_name(item.state)})},
			}));
		}
		json_value::array_type missing;
		for (const auto& item : value.missing)
			missing.push_back(json_value::object_value({
				{"capability_id", json_value::string_value(item.capability_id)},
				{"explanation", json_value::string_value(item.explanation)},
				{"reason_code", json_value::string_value(std::string{reason_name(item.reason)})},
			}));
		json_value::array_type plan;
		for (const auto& item : value.completion_plan)
		{
			json_value::array_type dependencies;
			for (const auto& dependency : item.dependencies)
				dependencies.push_back(json_value::string_value(dependency));
			plan.push_back(json_value::object_value({
				{"action", json_value::string_value(item.action)},
				{"id", json_value::string_value(item.id)},
				{"requires", json_value::array_value(std::move(dependencies))},
				{"reason_code", json_value::string_value(std::string{reason_name(item.reason)})},
				{"unlocks", json_value::string_value(item.unlocks)},
			}));
		}
		// Derive preserved semantics from the sealed capability result every time the
		// projection is built.  This keeps a projection faithful when a caller is
		// inspecting a copied result and prevents stale summary fields from being
		// mistaken for authority.
		json_value::array_type coverage;
		json_value::array_type unresolved;
		for (const auto& item : value.capability_path)
		{
			coverage.push_back(json_value::string_value(item.id));
			if (item.state == resolution_state::unknown || item.state == resolution_state::partial)
				unresolved.push_back(json_value::string_value(item.id));
		}
		json_value::array_type conflicts;
		for (const auto& item : value.conflicts)
		{
			json_value::array_type candidate_ids;
			for (const auto& candidate_id : item.candidate_ids)
				candidate_ids.push_back(json_value::string_value(candidate_id));
			conflicts.push_back(json_value::object_value({
				{"candidate_ids", json_value::array_value(std::move(candidate_ids))},
				{"capability_id", json_value::string_value(item.capability_id)},
				{"reason_code", json_value::string_value("doctor.conflicting-capability")},
			}));
		}
		json_value::array_type provenance{
			json_value::string_value("cxxlens.sdk-doctor-project.v2"),
			json_value::string_value("cxxlens.sdk-doctor-catalog.v1")};
		for (const auto& item : value.provenance)
			provenance.push_back(json_value::string_value(item));
		std::ranges::sort(provenance,
						  [](const json_value& left, const json_value& right)
						  {
							  return left.string < right.string;
						  });
		json_value::array_type guarantees{
			json_value::string_value("unknown-not-collapsed-to-empty-success"),
			json_value::string_value("product-only-diagnosis"),
		};
		if (value.state == resolution_state::proved)
			guarantees.push_back(json_value::string_value("all-capabilities-proved"));
		else if (value.state == resolution_state::conflicting)
			guarantees.push_back(json_value::string_value("conflict-preserved-no-fallback"));
		else
			guarantees.push_back(json_value::string_value("non-proved-state-preserved"));
		return json_value::object_value({
			{"catalog_binding",
			 json_value::object_value({
				 {"document_version", json_value::string_value(value.catalog_document_version)},
				 {"id", json_value::string_value(value.catalog_binding_id)},
			 })},
			{"capability_path", json_value::array_value(std::move(path))},
			{"completion_plan", json_value::array_value(std::move(plan))},
			{"missing", json_value::array_value(std::move(missing))},
			{"preserved_semantics",
			 json_value::object_value({
				 {"closure",
				  json_value::array_value(
					  {json_value::string_value(value.missing.empty() ? "dependency-graph-closed"
																	  : "dependency-graph-open")})},
				 {"conflict", json_value::array_value(std::move(conflicts))},
				 {"coverage", json_value::array_value(std::move(coverage))},
				 {"differential_disagreement", json_value::array_value({})},
				 {"guarantee", json_value::array_value(std::move(guarantees))},
				 {"logical_explain", json_value::array_value(unresolved)},
				 {"physical_explain", json_value::array_value({})},
				 {"provenance", json_value::array_value(std::move(provenance))},
				 {"unresolved", json_value::array_value(std::move(unresolved))},
			 })},
			{"question", json_value::string_value(value.question)},
			{"result",
			 json_value::object_value({
				 {"explanation",
				  json_value::string_value("Capability resolution is derived from product context "
										   "and explicit dependencies.")},
				 {"guarantee",
				  json_value::string_value(
					  "Product-only values are evaluated; unknown remains explicit.")},
				 {"reason_code", json_value::string_value(std::string{reason_name(value.reason)})},
				 {"state", json_value::string_value(std::string{state_name(value.state)})},
			 })},
			{"schema", json_value::string_value("cxxlens.sdk-doctor-resolution.v2")},
			{"use_case_id", json_value::string_value(value.use_case_id)},
			{"consumer", json_value::string_value(value.consumer)},
			{"document_version", json_value::string_value("2.0.0")},
		});
	}

	[[nodiscard]] inline json_value to_json(const std::vector<relation_check>& checks,
											const capability_catalog& catalog_token)
	{
		const auto& catalog = catalog_token;
		json_value::array_type components;
		std::size_t missing{};
		for (const auto& item : checks)
		{
			if (item.state != "proved")
				++missing;
			json_value::object_type component{
				{"id", json_value::string_value(item.id)},
				{"reason_code", json_value::string_value(item.reason_code)},
				{"state", json_value::string_value(item.state)},
			};
			components.push_back(json_value::object_value(std::move(component)));
		}
		return json_value::object_value({
			{"catalog_binding",
			 json_value::object_value({
				 {"document_version", json_value::string_value(catalog.document_version)},
				 {"id", json_value::string_value(catalog.binding_id)},
			 })},
			{"components", json_value::array_value(std::move(components))},
			{"mode", json_value::string_value("relation-presence")},
			{"missing", json_value::unsigned_value(missing)},
			{"requested", json_value::unsigned_value(checks.size())},
			{"schema", json_value::string_value("cxxlens.sdk-doctor-relation-presence.v2")},
			{"state", json_value::string_value(missing == 0U ? "proved" : "unknown")},
			{"document_version", json_value::string_value("2.0.0")},
		});
	}

	[[nodiscard]] inline std::string markdown_projection(const std::string_view json)
	{
		return "# cxxlens SDK capability diagnosis\n\n```json\n" + std::string{json} + "\n```\n";
	}

	[[nodiscard]] inline std::string read_file(const std::string_view path, std::string& error)
	{
		error.clear();
		const std::filesystem::path file_path{path};
		std::error_code metadata_error;
		const auto announced_size = std::filesystem::file_size(file_path, metadata_error);
		if (!metadata_error && announced_size > maximum_project_bytes)
		{
			error = "doctor.project-invalid:project:byte-limit";
			return {};
		}
		std::ifstream input{std::string{path}, std::ios::binary};
		if (!input)
		{
			error = "doctor.project-invalid:project:open";
			return {};
		}
		std::string output;
		if (!metadata_error)
			output.reserve(static_cast<std::size_t>(announced_size));
		std::array<char, 4096U> buffer{};
		while (input)
		{
			input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
			const auto count = input.gcount();
			if (count <= 0)
				break;
			const auto byte_count = static_cast<std::size_t>(count);
			if (byte_count > maximum_project_bytes - output.size())
			{
				error = "doctor.project-invalid:project:byte-limit";
				return {};
			}
			output.append(buffer.data(), byte_count);
		}
		if (!input.eof())
		{
			error = "doctor.project-invalid:project:read";
			return {};
		}
		return output;
	}

	[[nodiscard]] inline std::string_view trim_ascii(const std::string_view value) noexcept
	{
		std::size_t begin{};
		while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\r'))
			++begin;
		std::size_t end = value.size();
		while (end > begin &&
			   (value[end - 1U] == ' ' || value[end - 1U] == '\r' || value[end - 1U] == '\n'))
			--end;
		return value.substr(begin, end - begin);
	}

	[[nodiscard]] inline std::optional<std::vector<std::string>>
	parse_yaml_flow_string_set(const std::string_view input)
	{
		const auto value = trim_ascii(input);
		if (value.size() < 2U || value.front() != '[' || value.back() != ']')
			return std::nullopt;
		std::vector<std::string> output;
		std::size_t begin{1U};
		while (begin < value.size() - 1U)
		{
			const auto comma = value.find(',', begin);
			const auto end = comma == std::string_view::npos ? value.size() - 1U : comma;
			const auto item = trim_ascii(value.substr(begin, end - begin));
			if (!strict_id(item) || output.size() >= maximum_json_collection_count)
				return std::nullopt;
			output.emplace_back(item);
			if (comma == std::string_view::npos)
				break;
			begin = comma + 1U;
		}
		std::ranges::sort(output);
		if (std::ranges::adjacent_find(output) != output.end())
			return std::nullopt;
		return output;
	}

} // namespace cxxlens::sdk::doctor
