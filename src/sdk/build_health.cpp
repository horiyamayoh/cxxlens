#include <algorithm>
#include <array>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <tuple>

#include <cxxlens/sdk/build_health.hpp>

#include "query_projection_plan_limits_internal.hpp"
#include "query_result_internal.hpp"

namespace cxxlens::sdk::query
{
	namespace
	{
		using key_type = std::array<std::string, 4U>;
		using refs = std::vector<std::size_t>;
		constexpr std::array<std::string_view, 6U> relations{"build.project.v1",
															 "build.variant.v1",
															 "build.compile_unit.v1",
															 "source.file.v1",
															 "build.compile_unit_analysis.v1",
															 "build.analysis_inventory.v1"};
		constexpr std::array<std::string_view, 6U> identifiers{
			"project", "variant", "compile_unit", "snapshot", "compile_unit", "inventory"};
		constexpr std::string_view unit_profile = "clang22-selected-unit-analysis/1",
								   inventory_profile = "clang22-selected-analysis-units/1";
		struct failure
		{
			error value;
		};
		[[noreturn]] void fail(std::string_view field,
							   std::string_view detail,
							   std::string_view code = "sdk.health-input-invalid")
		{
			throw failure{{std::string{code}, std::string{field}, std::string{detail}}};
		}
		const detached_cell* cell(const annotated_row& row, std::string_view name)
		{
			const auto found = row.values.find("output." + std::string{name});
			return found == row.values.end() ? nullptr : &found->second;
		}
		bool present(const annotated_row& row, std::string_view name)
		{
			const auto* c = cell(row, name);
			return c && c->state == cell_state::present && c->value;
		}
		std::string text(const annotated_row& row, std::string_view name)
		{
			const auto* c = cell(row, name);
			const auto* v = c && c->state == cell_state::present && c->value
				? std::get_if<std::string>(&*c->value)
				: nullptr;
			return v ? *v : std::string{};
		}
		std::optional<std::uint64_t> number(const annotated_row& row, std::string_view name)
		{
			const auto* c = cell(row, name);
			const auto* v = c && c->state == cell_state::present && c->value
				? std::get_if<std::uint64_t>(&*c->value)
				: nullptr;
			return v ? std::optional{*v} : std::nullopt;
		}
		template <class T>
		key_type key(std::string id, const T& value)
		{
			return {std::move(id), value.universe, value.variant, value.interpretation};
		}
		void canonical(std::vector<query_unresolved>& values)
		{
			std::ranges::sort(values,
							  {},
							  [](const auto& v)
							  {
								  return std::tie(v.code, v.subject, v.detail);
							  });
			values.erase(std::ranges::unique(values).begin(), values.end());
		}
		void downgrade(finite_population_state& state, bool conflict)
		{
			if (conflict)
				state = finite_population_state::conflicting;
			else if (state == finite_population_state::complete)
				state = finite_population_state::partial;
		}
		struct budget
		{
			finite_population_limits limits;
			std::stop_token stop;
			std::size_t retained{}, evidence{}, references{}, operations{}, conditions{}, members{},
				rows{};
			void charge(std::size_t& used,
						std::size_t amount,
						std::size_t maximum,
						std::string_view field)
			{
				if (used > maximum || amount > maximum - used)
					fail(field, "limit-exceeded", "sdk.health-budget");
				used += amount;
			}
			void work(std::size_t n = 1)
			{
				if (stop.stop_requested() || (limits.cancelled && limits.cancelled()))
					fail("projection", "stop-requested", "sdk.health-cancelled");
				charge(operations, n, limits.maximum_operations, "operations");
			}
			void retain(std::size_t n)
			{
				charge(retained, n, limits.maximum_retained_bytes, "retained-bytes");
			}
			void bind(refs& into, const refs& from)
			{
				work(from.size());
				charge(references,
					   from.size(),
					   limits.maximum_evidence_references,
					   "evidence-references");
				retain(from.size() * sizeof(std::size_t));
				into.insert(into.end(), from.begin(), from.end());
			}
			std::size_t estimate(const annotated_row& row)
			{
				std::size_t total = 2048;
				const auto add = [&](std::size_t n)
				{
					work();
					if (total > limits.maximum_retained_bytes ||
						n > (limits.maximum_retained_bytes - total) / 8)
						fail("row", "limit-exceeded", "sdk.health-budget");
					total += n * 8;
				};
				for (const auto& [name, c] : row.values)
				{
					add(name.size() + c.type.parameter.size() + 128);
					if (c.unknown_reason)
						add(c.unknown_reason->size());
					if (c.value)
					{
						if (const auto* v = std::get_if<std::string>(&*c.value))
							add(v->size());
						if (const auto* v = std::get_if<std::vector<std::byte>>(&*c.value))
							add(v->size());
					}
				}
				const auto strings = [&](const auto& values)
				{
					for (const auto& v : values)
						add(v.size() + 128);
				};
				const auto producer = [&](const auto& p)
				{
					add(p.id.size() + p.semantic_contract.size() + 128);
				};
				const auto guarantee = [&](const auto& g)
				{
					add(g.approximation.size() + g.scope.size() + g.assumptions.size() + 128);
					strings(g.verification_modalities);
				};
				strings(row.claim_contributors);
				strings(row.provenance);
				strings(row.presence.fragments);
				add(row.interpretation.size() + row.presence.universe.size());
				for (const auto& p : row.producer_contracts)
					producer(p);
				for (const auto& g : row.contributor_guarantees)
					guarantee(g);
				for (const auto& e : row.contributor_edges)
				{
					add(e.claim_contributor.size() + e.provenance.size() + e.interpretation.size() +
						e.condition.universe.size() + 128);
					producer(e.producer);
					guarantee(e.guarantee);
					strings(e.condition.fragments);
				}
				return total;
			}
		};
		struct entry
		{
			std::size_t group{};
			const annotated_row* row{};
			std::string canonical, payload;
		};
		struct projection
		{
			budget& b;
			build_health_input input;
			build_health_projection output;
			std::array<std::map<key_type, refs>, 6U> index;
			std::map<key_type, std::vector<std::string>> project_units;
			std::vector<std::string> payloads;
			const annotated_row& row(std::size_t i) const
			{
				return output.evidence.at(i).row;
			}
			const refs& find(std::size_t group, const key_type& id) const
			{
				static const refs empty;
				const auto f = index[group].find(id);
				return f == index[group].end() ? empty : f->second;
			}
			bool equal(const refs& values)
			{
				if (values.empty())
					return false;
				for (auto i : values)
				{
					b.work(payloads[i].size());
					if (payloads[i] != payloads[values.front()])
						return false;
				}
				return true;
			}
			template <class T>
			void gap(T& value, std::string_view reason, bool conflict = false)
			{
				b.work();
				b.retain(reason.size() + value.project.size() + 128U);
				value.gaps.push_back({"sdk.health-" + std::string{reason}, value.project, {}});
				downgrade(value.state, conflict);
			}
			void selection_gap(build_health_population& value,
							   std::string_view reason,
							   bool conflict = false)
			{
				gap(value, reason, conflict);
				downgrade(value.selection_state, conflict);
			}
			std::optional<std::vector<std::string>> strings(const annotated_row& value,
															std::string_view field)
			{
				if (!present(value, field))
					return {};
				const auto* c = cell(value, field);
				const auto* bytes = std::get_if<std::vector<std::byte>>(&*c->value);
				if (!bytes)
					fail(field, "set-payload-invalid");
				b.work(bytes->size());
				if (bytes->size() > b.limits.maximum_retained_bytes / 16U)
					fail(field, "limit-exceeded", "sdk.health-budget");
				b.retain(bytes->size() * 16U + 128U);
				std::vector<std::string> result;
				for (std::size_t i{}; i < bytes->size();)
				{
					b.work();
					if (bytes->size() - i < 4U)
						fail(field, "truncated-set");
					std::uint32_t length{};
					for (unsigned shift{}; shift < 32U; shift += 8U)
						length |= std::to_integer<std::uint32_t>((*bytes)[i++]) << shift;
					if (length > bytes->size() - i)
						fail(field, "truncated-set");
					b.charge(b.members, 1U, b.limits.maximum_members, "set-members");
					std::string id;
					id.reserve(length);
					for (std::size_t end = i + length; i < end; ++i)
						id.push_back(static_cast<char>((*bytes)[i]));
					if (id.empty() || (!result.empty() && result.back() >= id))
						fail(field, "canonical-set-required");
					result.push_back(std::move(id));
				}
				return result;
			}
			template <class T>
			bool
			reference(T& value, std::size_t group, const std::string& id, std::string_view role)
			{
				const auto& actual = find(group, key(id, value));
				b.bind(value.evidence, actual);
				if (actual.empty())
				{
					gap(value, std::string{role} + "-missing");
					return false;
				}
				if (!equal(actual))
				{
					gap(value, std::string{role} + "-conflicting", true);
					return false;
				}
				return true;
			}
			observed_compile_unit_analysis unit(const std::string& id,
												const build_health_population& scope)
			{
				observed_compile_unit_analysis value;
				value.compile_unit = id;
				value.project = scope.project;
				value.universe = scope.universe;
				value.variant = scope.variant;
				value.interpretation = scope.interpretation;
				value.state = finite_population_state::complete;
				b.retain(id.size() + scope.project.size() + scope.universe.size() +
						 scope.variant.size() + scope.interpretation.size() + sizeof(value) + 512U);
				if (!input.catalog_inputs_complete)
					gap(value, "catalog-inputs-unavailable");
				const auto& units = find(2U, key(id, value));
				if (!reference(value, 2U, id, "unit"))
					return value;
				const auto& original = row(units.front());
				value.source_snapshot = text(original, "main_source");
				if (text(original, "project") != scope.project ||
					text(original, "variant") != scope.variant)
					gap(value, "unit-world-or-project-conflicting", true);
				reference(value, 0U, scope.project, "project");
				if (reference(value, 1U, scope.variant, "variant"))
					if (text(row(find(1U, key(scope.variant, value)).front()), "project") !=
						scope.project)
						gap(value, "variant-project-conflicting", true);
				if (reference(value, 3U, value.source_snapshot, "source"))
				{
					const auto& source = row(find(3U, key(value.source_snapshot, value)).front());
					value.file = text(source, "file");
					if (text(source, "project") != scope.project)
						gap(value, "source-project-conflicting", true);
				}
				const auto& outcomes = find(4U, key(id, value));
				if (!input.analysis_inputs_complete)
					gap(value, "analysis-inputs-unavailable");
				if (!reference(value, 4U, id, "analysis"))
					return value;
				const auto& original_outcome = row(outcomes.front());
				value.profile = text(original_outcome, "profile");
				value.parse_outcome = text(original_outcome, "parse_outcome");
				value.semantic_output = text(original_outcome, "semantic_output");
				value.reason = text(original_outcome, "reason");
				value.parse_error_count = number(original_outcome, "parse_error_count");
				value.fatal_error_count = number(original_outcome, "fatal_error_count");
				if (text(original_outcome, "project") != scope.project ||
					text(original_outcome, "main_source") != value.source_snapshot)
					gap(value, "analysis-unit-binding-conflicting", true);
				if (value.profile != unit_profile)
					gap(value, "analysis-profile-unavailable");
				if (value.parse_outcome == "unavailable")
					gap(value, "parser-outcome-unavailable");
				else if (value.parse_outcome != "success" && value.parse_outcome != "recovery" &&
						 value.parse_outcome != "failed")
					gap(value, "parser-outcome-unavailable");
				else if (!value.parse_error_count || !value.fatal_error_count)
					gap(value, "parser-counters-unavailable");
				else if (*value.fatal_error_count > *value.parse_error_count ||
						 (value.parse_outcome == "success" && *value.parse_error_count != 0U) ||
						 (value.parse_outcome == "recovery" &&
						  (*value.parse_error_count == 0U || *value.fatal_error_count != 0U)))
					gap(value, "parser-axes-conflicting", true);
				if (value.semantic_output != "produced" && value.semantic_output != "not_produced")
					gap(value, "semantic-output-unavailable");
				return value;
			}
			build_health_population inventory(const key_type& id, const refs& originals)
			{
				const auto& original = row(originals.front());
				build_health_population value;
				value.inventory_id = id[0];
				value.universe = id[1];
				value.variant = id[2];
				value.interpretation = id[3];
				value.project = text(original, "project");
				value.profile = text(original, "profile");
				value.enumeration_state = text(original, "enumeration_state");
				value.selected_variant_state = text(original, "selected_variant_state");
				value.declared_units = number(original, "compile_unit_count");
				value.selected_variant_count = number(original, "selected_variant_count");
				value.state = value.enumeration_state == "complete" &&
						input.inventory_inputs_complete && input.catalog_inputs_complete
					? finite_population_state::complete
					: finite_population_state::unknown;
				value.selection_state = value.selected_variant_state == "complete" &&
						input.inventory_inputs_complete && input.catalog_inputs_complete
					? finite_population_state::complete
					: finite_population_state::unknown;
				b.retain(sizeof(value) + payloads[originals.front()].size());
				b.bind(value.evidence, originals);
				if (!equal(originals))
					selection_gap(value, "inventory-conflicting", true);
				if (value.profile != inventory_profile)
					selection_gap(value, "inventory-profile-unavailable");
				if (value.enumeration_state != "complete" || !input.inventory_inputs_complete ||
					!input.catalog_inputs_complete)
					gap(value, "unit-selection-unavailable");
				if (value.selected_variant_state != "complete" ||
					!input.inventory_inputs_complete || !input.catalog_inputs_complete)
					selection_gap(value, "variant-selection-unavailable");
				const auto units = strings(original, "compile_units"),
						   variants = strings(original, "selected_variant_ids");
				if (!units || !value.declared_units)
					gap(value, "unit-members-unavailable");
				else
				{
					value.compile_unit_ids = *units;
					if (*value.declared_units != units->size())
						gap(value, "unit-count-conflicting", true);
				}
				if (!variants || !value.selected_variant_count)
					selection_gap(value, "variant-members-unavailable");
				else
				{
					value.selected_variant_ids = *variants;
					if (*value.selected_variant_count != variants->size())
						selection_gap(value, "variant-count-conflicting", true);
					if (!std::ranges::binary_search(*variants, value.variant))
						selection_gap(value, "world-outside-selected-variants", true);
				}
				if (!reference(value, 0U, value.project, "project"))
					downgrade(value.selection_state,
							  value.state == finite_population_state::conflicting);
				if (reference(value, 1U, value.variant, "variant"))
					if (text(row(find(1U, key(value.variant, value)).front()), "project") !=
						value.project)
						selection_gap(value, "variant-project-conflicting", true);
				if (find(1U, key(value.variant, value)).empty() ||
					!equal(find(1U, key(value.variant, value))))
					downgrade(value.selection_state,
							  value.state == finite_population_state::conflicting);
				if (units && value.enumeration_state == "complete")
				{
					const auto actual = project_units.find(key(value.project, value));
					const std::vector<std::string> empty;
					const auto& observed = actual == project_units.end() ? empty : actual->second;
					if (observed != *units)
					{
						const bool extra =
							std::ranges::any_of(observed,
												[&](const auto& id)
												{
													return !std::ranges::binary_search(*units, id);
												});
						gap(value, "selected-unit-membership-unavailable", extra);
					}
				}
				for (const auto& unit_id : value.compile_unit_ids)
				{
					b.work();
					b.charge(b.members, 1U, b.limits.maximum_members, "units");
					auto observed = unit(unit_id, value);
					b.bind(value.evidence, observed.evidence);
					value.units.push_back(std::move(observed));
				}
				return value;
			}
			void populations()
			{
				std::map<key_type, refs> domains;
				for (const auto& [id, originals] : index[5U])
				{
					b.work();
					if (output.populations.size() >= b.limits.maximum_populations)
						fail("populations", "limit-exceeded", "sdk.health-budget");
					auto value = inventory(id, originals);
					const auto domain = key(value.project, value);
					b.retain(value.project.size() + value.universe.size() + value.variant.size() +
							 value.interpretation.size() + 256U);
					domains[domain].push_back(output.populations.size());
					output.populations.push_back(std::move(value));
				}
				for (const auto& [domain, values] : domains)
					if (values.size() > 1U)
						for (auto i : values)
							selection_gap(
								output.populations[i], "inventory-domain-conflicting", true);
				for (const auto& [domain, units] : project_units)
					if (!domains.contains(domain))
					{
						b.work();
						if (output.populations.size() >= b.limits.maximum_populations)
							fail("populations", "limit-exceeded", "sdk.health-budget");
						build_health_population missing;
						missing.project = domain[0];
						missing.universe = domain[1];
						missing.variant = domain[2];
						missing.interpretation = domain[3];
						b.retain(sizeof(missing) + domain[0].size() + domain[1].size() +
								 domain[2].size() + domain[3].size() + 256U);
						gap(missing, "selected-unit-inventory-missing");
						for (const auto& id : units)
							b.retain(id.size() + sizeof(std::string));
						missing.compile_unit_ids = units;
						for (const auto& id : units)
							missing.units.push_back(unit(id, missing));
						output.populations.push_back(std::move(missing));
					}
				// Cross-world repetition of one original selected configuration set is
				// metadata consistency, not a union inferred from returned worlds.
				using global_key = std::array<std::string, 3U>;
				std::map<global_key, refs> selections;
				for (std::size_t i{}; i < output.populations.size(); ++i)
				{
					const auto& p = output.populations[i];
					b.work();
					b.retain(p.project.size() + p.universe.size() + p.interpretation.size() + 128U);
					selections[{p.project, p.universe, p.interpretation}].push_back(i);
				}
				for (const auto& [domain, values] : selections)
				{
					(void)domain;
					const build_health_population* first = nullptr;
					for (auto i : values)
					{
						b.work();
						const auto& p = output.populations[i];
						if (p.selected_variant_count && !p.selected_variant_state.empty())
						{
							first = &p;
							break;
						}
					}
					bool consistent = true;
					std::set<std::string> returned;
					for (auto i : values)
					{
						b.work();
						const auto& p = output.populations[i];
						b.retain(p.variant.size() + 64U);
						returned.insert(p.variant);
						if (first && p.selected_variant_count && !p.selected_variant_state.empty())
							consistent &=
								p.selected_variant_count == first->selected_variant_count &&
								p.selected_variant_ids == first->selected_variant_ids &&
								p.selected_variant_state == first->selected_variant_state;
					}
					for (auto i : values)
					{
						auto& p = output.populations[i];
						if (!consistent)
							selection_gap(p, "selected-variant-census-conflicting", true);
						for (const auto& expected : p.selected_variant_ids)
						{
							b.work();
							if (!returned.contains(expected))
								selection_gap(p, "selected-variant-world-missing");
						}
					}
				}
				for (auto& p : output.populations)
				{
					std::ranges::sort(p.evidence);
					p.evidence.erase(std::ranges::unique(p.evidence).begin(), p.evidence.end());
					canonical(p.gaps);
					for (auto& u : p.units)
					{
						std::ranges::sort(u.evidence);
						u.evidence.erase(std::ranges::unique(u.evidence).begin(), u.evidence.end());
						canonical(u.gaps);
					}
					const auto retain_gaps = [&](const auto& gaps)
					{
						for (const auto& gap : gaps)
						{
							b.work();
							b.retain(gap.code.size() + gap.subject.size() + gap.detail.size() +
									 sizeof(gap));
						}
					};
					retain_gaps(p.gaps);
					output.unresolved.insert(output.unresolved.end(), p.gaps.begin(), p.gaps.end());
					for (const auto& u : p.units)
					{
						retain_gaps(u.gaps);
						output.unresolved.insert(
							output.unresolved.end(), u.gaps.begin(), u.gaps.end());
					}
				}
				std::ranges::sort(
					output.populations,
					{},
					[](const auto& p)
					{
						return std::tie(
							p.project, p.universe, p.variant, p.interpretation, p.inventory_id);
					});
				canonical(output.unresolved);
			}
		};
		result<build_health_projection> project_rows(build_health_input input,
													 finite_population_limits limits,
													 std::stop_token stop,
													 budget& b)
		{
			if (auto valid = limits.validate(); !valid)
				return valid.error();
			try
			{
				(void)stop;
				b.work();
				projection p{b, input, {}, {}, {}, {}};
				p.output.catalog_inputs_complete = input.catalog_inputs_complete;
				p.output.analysis_inputs_complete = input.analysis_inputs_complete;
				p.output.inventory_inputs_complete = input.inventory_inputs_complete;
				const std::array groups{input.projects,
										input.variants,
										input.units,
										input.source_files,
										input.unit_analysis,
										input.analysis_inventories};
				const auto descriptors = standard_relation_descriptors();
				std::vector<entry> entries;
				for (std::size_t group{}; group < groups.size(); ++group)
				{
					const auto descriptor =
						std::ranges::find(descriptors, relations[group], &relation_descriptor::id);
					if (descriptor == descriptors.end())
						fail(relations[group], "descriptor-missing");
					for (const auto& row : groups[group])
					{
						b.work();
						b.charge(b.rows, 1U, limits.maximum_rows, "rows");
						b.retain(b.estimate(row));
						if (auto valid = row.validate(); !valid)
							return valid.error();
						if (row.values.size() != descriptor->columns.size())
							fail(relations[group], "columns-invalid");
						for (const auto& column : descriptor->columns)
						{
							b.work();
							const auto* c = cell(row, column.name);
							if (!c || c->type != column.type || !c->validate())
								fail(column.id, "column-type-or-value-invalid");
						}
						b.charge(b.conditions,
								 row.presence.fragments.size(),
								 limits.maximum_condition_expansions,
								 "conditions");
						std::string payload;
						for (const auto& [name, c] : row.values)
						{
							b.work();
							payload += name + '=' + c.canonical_form() + '\n';
						}
						const auto canonical = row.canonical_form();
						b.charge(b.evidence,
								 canonical.size(),
								 limits.maximum_evidence_bytes,
								 "evidence-bytes");
						entries.push_back({group, &row, canonical, std::move(payload)});
					}
				}
				std::ranges::sort(entries,
								  {},
								  [](const auto& e)
								  {
									  return std::tie(e.group, e.canonical);
								  });
				for (auto& e : entries)
				{
					b.work();
					const auto i = p.output.evidence.size();
					p.output.evidence.push_back({std::string{relations[e.group]}, *e.row});
					p.payloads.push_back(std::move(e.payload));
					const auto id = text(*e.row, identifiers[e.group]);
					if (id.empty())
						fail(relations[e.group], "identity-missing");
					for (const auto& variant : e.row->presence.fragments)
					{
						b.work();
						b.retain(id.size() + variant.size() + e.row->presence.universe.size() +
								 e.row->interpretation.size() + 512U);
						const key_type actual{
							id, e.row->presence.universe, variant, e.row->interpretation};
						p.index[e.group][actual].push_back(i);
						if (e.group == 2U)
						{
							b.retain(text(*e.row, "project").size() + id.size() + 128U);
							p
								.project_units[{
									text(*e.row, "project"), actual[1], actual[2], actual[3]}]
								.push_back(id);
						}
					}
				}
				for (auto& [ignored, units] : p.project_units)
				{
					(void)ignored;
					std::ranges::sort(units);
					units.erase(std::ranges::unique(units).begin(), units.end());
				}
				p.populations();
				return std::move(p.output);
			}
			catch (const failure& e)
			{
				return e.value;
			}
			catch (const std::bad_alloc&)
			{
				return error{"sdk.health-resource-exhausted", "projection", "allocation"};
			}
			catch (const std::length_error&)
			{
				return error{"sdk.health-resource-exhausted", "projection", "length"};
			}
		}
	} // namespace
	result<build_health_projection> project_build_health(build_health_input input,
														 finite_population_limits limits,
														 std::stop_token stop)
	{
		budget b{limits, stop};
		return project_rows(input, limits, stop, b);
	}
	result<build_health_projection> project_build_health(const application_query_results& input,
														 finite_population_limits limits,
														 std::stop_token stop)
	{
		if (auto valid = limits.validate(); !valid)
			return valid.error();
		try
		{
			budget b{limits, stop};
			b.work();
			std::size_t plan_bytes{};
			const std::function<bool()> cancelled = [&]
			{
				b.work();
				return false;
			};
			if (auto valid = detail::check_source_plan_limits(input,
															  limits.maximum_source_queries,
															  limits.maximum_source_plan_bytes,
															  stop,
															  "sdk.health",
															  &plan_bytes,
															  cancelled);
				!valid)
				return valid.error();
			std::array<std::vector<annotated_row>, 6U> groups;
			std::array<bool, 6U> present{}, complete{};
			complete.fill(true);
			for (const auto& scan : input.scans)
			{
				b.work();
				const auto at = std::ranges::find(relations, scan.relation_id);
				if (at == relations.end())
					continue;
				const auto group = static_cast<std::size_t>(at - relations.begin());
				present[group] = true;
				complete[group] &= scan.result.execution() == execution_status::complete &&
					scan.result.inputs_complete() && scan.result.conflicts().empty() &&
					scan.result.differential_disagreements().empty();
				for (const auto& original : query_transfer_access::borrow_rows(scan.result))
				{
					b.work();
					b.charge(b.rows, 1U, limits.maximum_rows, "scan-rows");
					b.retain(b.estimate(original));
					groups[group].push_back(original);
				}
			}
			b.rows = 0U;
			auto result = project_rows({groups[0],
										groups[1],
										groups[2],
										groups[3],
										groups[4],
										groups[5],
										present[0] && complete[0] && present[1] && complete[1] &&
											present[2] && complete[2] && present[3] && complete[3],
										present[4] && complete[4],
										present[5] && complete[5]},
									   limits,
									   stop,
									   b);
			if (!result)
				return result;
			for (std::size_t i{}; i < relations.size(); ++i)
				if (!present[i])
					result->unresolved.push_back({"sdk.health-scan-missing",
												  std::string{relations[i]},
												  "supply-independent-public-scan"});
			canonical(result->unresolved);
			b.retain(plan_bytes);
			result->source_queries = input;
			return result;
		}
		catch (const failure& e)
		{
			return e.value;
		}
		catch (const std::bad_alloc&)
		{
			return error{"sdk.health-resource-exhausted", "projection", "allocation"};
		}
		catch (const std::length_error&)
		{
			return error{"sdk.health-resource-exhausted", "projection", "length"};
		}
	}
} // namespace cxxlens::sdk::query
