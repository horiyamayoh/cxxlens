#include <algorithm>
#include <array>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <tuple>

#include <cxxlens/sdk/template_events.hpp>

#include "query_projection_plan_limits_internal.hpp"
#include "query_projection_rows_internal.hpp"
#include "query_result_internal.hpp"
namespace cxxlens::sdk::query
{
	namespace
	{
		using identity = std::array<std::string, 4>;
		using refs = std::vector<std::size_t>;
		constexpr std::array<std::string_view, 9> relations{"build.compile_unit.v1",
															"source.file.v1",
															"source.span.v1",
															"cc.entity.v1",
															"cc.syntax_node.v1",
															"cc.template_candidate.v1",
															"cc.constant_evaluation_root.v1",
															"cc.constant_evaluated_call.v1",
															"cc.template_inventory.v1"};
		constexpr std::array<std::string_view, 9> identifiers{"compile_unit",
															  "snapshot",
															  "span",
															  "entity",
															  "node",
															  "candidate",
															  "root",
															  "call",
															  "inventory"};
		struct failure
		{
			error value;
		};
		[[noreturn]] void fail(std::string_view field,
							   std::string_view reason,
							   std::string_view code = "sdk.template-event-input-invalid")
		{
			throw failure{{std::string{code}, std::string{field}, std::string{reason}}};
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
		std::vector<std::byte> bytes(const annotated_row& row, std::string_view name)
		{
			const auto* c = cell(row, name);
			const auto* v = c && c->state == cell_state::present && c->value
				? std::get_if<std::vector<std::byte>>(&*c->value)
				: nullptr;
			return v ? *v : std::vector<std::byte>{};
		}
		std::optional<std::uint64_t> number(const annotated_row& row, std::string_view name)
		{
			const auto* c = cell(row, name);
			const auto* v = c && c->state == cell_state::present && c->value
				? std::get_if<std::uint64_t>(&*c->value)
				: nullptr;
			return v ? std::optional{*v} : std::nullopt;
		}
		bool boolean(const annotated_row& row, std::string_view name)
		{
			const auto* c = cell(row, name);
			const auto* v = c && c->state == cell_state::present && c->value
				? std::get_if<bool>(&*c->value)
				: nullptr;
			return v && *v;
		}
		identity key(std::string id, const annotated_row& row, const std::string& variant)
		{
			return {std::move(id), row.presence.universe, variant, row.interpretation};
		}
		template <class T>
		identity key(std::string id, const T& value)
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
					fail(field, "limit-exceeded", "sdk.template-event-budget");
				used += amount;
			}
			void work(std::size_t n = 1)
			{
				if (stop.stop_requested() || (limits.cancelled && limits.cancelled()))
					fail("projection", "stop-requested", "sdk.template-event-cancelled");
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
						fail("row", "limit-exceeded", "sdk.template-event-budget");
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
			std::size_t group;
			const annotated_row* row;
			std::string canonical, payload;
		};
		struct projection
		{
			budget& b;
			template_event_input input;
			template_event_projection output;
			std::array<std::map<identity, refs>, 9> maps;
			std::array<std::map<identity, refs>, 5> unit_members;
			std::vector<std::string> payloads;
			const annotated_row& row(std::size_t i) const
			{
				return output.evidence.at(i).row;
			}
			const refs& find(std::size_t group, const identity& id) const
			{
				static const refs empty;
				const auto at = maps[group].find(id);
				return at == maps[group].end() ? empty : at->second;
			}
			bool equal(const refs& values)
			{
				if (values.empty())
					return false;
				for (auto i : values)
				{
					b.work();
					if (payloads[i] != payloads[values.front()])
						return false;
				}
				return true;
			}
			void axis(finite_population_state& state,
					  std::vector<query_unresolved>& gaps,
					  std::string_view id,
					  std::string_view reason,
					  bool conflict = false)
			{
				b.work();
				b.retain(id.size() + reason.size() + 128);
				gaps.push_back({"sdk.template-event-" + std::string(reason), std::string(id), {}});
				downgrade(state, conflict);
			}
			template <class T>
			void gap(T& value, std::string_view id, std::string_view reason, bool conflict = false)
			{
				axis(value.state, value.gaps, id, reason, conflict);
			}
			std::optional<std::vector<std::string>> strings(const annotated_row& value,
															std::string_view field)
			{
				if (!present(value, field))
					return std::nullopt;
				const auto raw = bytes(value, field);
				b.retain(raw.size() * 8);
				std::vector<std::string> result;
				for (std::size_t i = 0; i < raw.size();)
				{
					b.work();
					if (raw.size() - i < 4)
						fail(field, "truncated-set");
					std::uint32_t size{};
					for (unsigned shift = 0; shift < 32; shift += 8)
						size |= std::to_integer<std::uint32_t>(raw[i++]) << shift;
					if (size > raw.size() - i)
						fail(field, "truncated-set");
					b.charge(b.members, 1, b.limits.maximum_members, "set-members");
					std::string id;
					id.reserve(size);
					for (const auto end = i + size; i < end; ++i)
						id += static_cast<char>(raw[i]);
					if (id.empty() || (!result.empty() && result.back() >= id))
						fail(field, "canonical-set-required");
					result.push_back(std::move(id));
				}
				return result;
			}
			finite_population_state reference(std::size_t group,
											  std::string_view id,
											  const template_event_population& scope,
											  refs& evidence,
											  std::vector<query_unresolved>& gaps)
			{
				if (id.empty())
					return finite_population_state::unknown;
				const auto& values = find(group, key(std::string(id), scope));
				b.bind(evidence, values);
				if (values.empty())
				{
					gaps.push_back({"sdk.template-event-reference-missing", std::string(id), {}});
					return finite_population_state::unknown;
				}
				if (!equal(values))
				{
					gaps.push_back(
						{"sdk.template-event-reference-conflicting", std::string(id), {}});
					return finite_population_state::conflicting;
				}
				if (group >= 4 && text(row(values.front()), "compile_unit") != scope.compile_unit)
				{
					gaps.push_back(
						{"sdk.template-event-reference-unit-conflicting", std::string(id), {}});
					return finite_population_state::conflicting;
				}
				return finite_population_state::complete;
			}
			template <class T>
			void source(T& value, const template_event_population& scope)
			{
				value.source_state =
					reference(2, value.source_span, scope, value.evidence, value.gaps);
				if (value.source_state != finite_population_state::complete)
					return;
				const auto& span = row(find(2, key(value.source_span, scope)).front());
				const auto snapshot = text(span, "snapshot");
				const auto file = text(span, "file");
				const auto begin = number(span, "begin"), end = number(span, "end");
				value.source_state = reference(1, snapshot, scope, value.evidence, value.gaps);
				if (value.source_state != finite_population_state::complete)
					return;
				const auto& original = row(find(1, key(snapshot, scope)).front());
				const auto size = number(original, "size");
				if (file != text(original, "file") || !begin || !end || !size || *end < *begin ||
					*end > *size)
					axis(value.source_state,
						 value.gaps,
						 value.source_span,
						 "source-bounds-conflicting",
						 true);
			}
			template <class T>
			const annotated_row* original(T& value,
										  std::size_t group,
										  std::string_view id,
										  const template_event_population& scope)
			{
				const auto& values = find(group, key(std::string(id), scope));
				b.bind(value.evidence, values);
				value.state = finite_population_state::complete;
				if (values.empty())
				{
					gap(value, id, "member-missing");
					return nullptr;
				}
				if (!equal(values))
				{
					gap(value, id, "member-conflicting", true);
					return nullptr;
				}
				const auto& observed = row(values.front());
				if (text(observed, "compile_unit") != scope.compile_unit)
				{
					gap(value, id, "member-unit-conflicting", true);
					return nullptr;
				}

				return &observed;
			}
			constexpr static std::string_view candidate_profile =
				"clang22-final-candidate-substitution-events/1";
			constexpr static std::string_view evaluation_profile =
				"clang22-legacy-constant-evaluation/1";
			void usr(finite_population_state& state,
					 std::vector<query_unresolved>& gaps,
					 std::string_view id,
					 const std::vector<std::byte>& raw,
					 const template_event_population& scope,
					 refs& evidence)
			{
				state = reference(3, id, scope, evidence, gaps);
				if (state != finite_population_state::complete)
					return;
				const auto& r = row(find(3, key(std::string(id), scope)).front());
				if (!present(r, "provider_local_key") || raw.empty())
				{
					axis(state, gaps, id, "usr-unavailable");
					return;
				}
				constexpr std::string_view prefix = "clang-usr:";
				b.retain(prefix.size() + raw.size());
				std::vector<std::byte> framed;
				framed.reserve(prefix.size() + raw.size());
				for (char c : prefix)
					framed.push_back(static_cast<std::byte>(c));
				framed.insert(framed.end(), raw.begin(), raw.end());
				if (bytes(r, "provider_local_key") != framed)
					axis(state, gaps, id, "usr-conflicting", true);
			}
			observed_template_candidate candidate(std::string id,
												  const template_event_population& scope)
			{
				observed_template_candidate v;
				v.candidate = std::move(id);
				v.compile_unit = scope.compile_unit;
				const auto* o = original(v, 5, v.candidate, scope);
				if (!o)
					return v;
				b.retain(b.estimate(*o));
				v.source_span = text(*o, "source");
				v.candidate_entity = text(*o, "candidate_entity");
				v.route = text(*o, "route");
				v.profile = text(*o, "profile");
				v.deduction_result = text(*o, "deduction_result");
				v.exclusion_disposition = text(*o, "exclusion_disposition");
				v.binding_state = text(*o, "binding_state");
				v.reason = text(*o, "reason");
				v.candidate_usr = bytes(*o, "candidate_usr");
				v.ordinal = number(*o, "ordinal");
				v.overload_failure = number(*o, "overload_failure");
				if (present(*o, "completed"))
					v.completed = boolean(*o, "completed");
				if (present(*o, "is_system"))
					v.is_system = boolean(*o, "is_system");
				constexpr std::array routes{"method_overload",
											"function_overload",
											"conversion_overload",
											"address_target",
											"single_template_selection",
											"variable_partial",
											"explicit_function_specialization",
											"explicit_function_instantiation",
											"class_partial"};
				constexpr std::array results{"success",
											 "invalid",
											 "instantiation_depth",
											 "incomplete",
											 "incomplete_pack",
											 "inconsistent",
											 "underqualified",
											 "substitution_failure",
											 "deduced_mismatch",
											 "deduced_mismatch_nested",
											 "non_deduced_mismatch",
											 "too_many_arguments",
											 "too_few_arguments",
											 "invalid_explicit_arguments",
											 "nondependent_conversion_failure",
											 "constraints_not_satisfied",
											 "miscellaneous_failure",
											 "cuda_target_mismatch",
											 "already_diagnosed"};
				if (v.profile != candidate_profile ||
					std::ranges::find(routes, v.route) == routes.end() || !v.ordinal ||
					std::ranges::find(results, v.deduction_result) == results.end())
					gap(v, v.candidate, "candidate-profile-unavailable");
				if (v.completed != std::optional{true})
					gap(v, v.candidate, "candidate-terminal-unavailable");
				const auto expected = v.deduction_result == "success"	? "deduction_success"
					: v.deduction_result == "substitution_failure"		? "substitution_exclusion"
					: v.deduction_result == "constraints_not_satisfied" ? "constraint_exclusion"
																		: "other_exclusion";
				if (v.completed == std::optional{true} && v.exclusion_disposition != expected)
					gap(v, v.candidate, "candidate-disposition-conflicting", true);
				source(v, scope);
				usr(v.subject_state,
					v.gaps,
					v.candidate_entity,
					v.candidate_usr,
					scope,
					v.evidence);
				canonical(v.gaps);
				return v;
			}
			observed_constant_evaluation_root root(std::string id,
												   const template_event_population& scope)
			{
				observed_constant_evaluation_root v;
				v.root = std::move(id);
				v.compile_unit = scope.compile_unit;
				const auto* o = original(v, 6, v.root, scope);
				if (!o)
					return v;
				b.retain(b.estimate(*o));
				v.mode = text(*o, "mode");
				v.interpreter = text(*o, "interpreter");
				v.profile = text(*o, "profile");
				v.completion = text(*o, "completion");
				v.call_state = text(*o, "call_state");
				v.reason = text(*o, "reason");
				v.ordinal = number(*o, "ordinal");
				v.call_count = number(*o, "call_count");
				if (present(*o, "requested_constant_context"))
					v.requested_constant_context = boolean(*o, "requested_constant_context");
				if (present(*o, "potential_check"))
					v.potential_check = boolean(*o, "potential_check");
				if (present(*o, "fold_failure"))
					v.fold_failure = boolean(*o, "fold_failure");
				const auto members = strings(*o, "call_ids");
				if (members)
					v.call_ids = *members;
				v.terminal_state = v.completion == "complete" ? finite_population_state::complete
															  : finite_population_state::partial;
				v.invocation_state = finite_population_state::complete;
				constexpr std::array modes{"constant_expression",
										   "constant_expression_unevaluated",
										   "constant_fold",
										   "ignore_side_effects"};
				if (v.profile != evaluation_profile || !v.ordinal ||
					std::ranges::find(modes, v.mode) == modes.end() ||
					(v.interpreter != "legacy" && v.interpreter != "bytecode"))
					gap(v, v.root, "root-profile-unavailable");
				if (v.interpreter != "legacy" || v.call_state != "complete" || !v.call_count ||
					!members || v.terminal_state != finite_population_state::complete)
					axis(v.invocation_state, v.gaps, v.root, "root-call-enumeration-unavailable");
				if (v.requested_constant_context == std::optional{true} &&
					v.potential_check == std::optional{false} &&
					v.fold_failure == std::optional{true})
					axis(v.invocation_state, v.gaps, v.root, "qualifying-evaluation-failed");
				if (v.call_count && members && *v.call_count != v.call_ids.size())
					axis(v.invocation_state, v.gaps, v.root, "root-call-count-conflicting", true);
				canonical(v.gaps);
				return v;
			}
			observed_constant_evaluated_call call(std::string id,
												  const template_event_population& scope)
			{
				observed_constant_evaluated_call v;
				v.call = std::move(id);
				v.compile_unit = scope.compile_unit;
				const auto* o = original(v, 7, v.call, scope);
				if (!o)
					return v;
				b.retain(b.estimate(*o));
				v.source_span = text(*o, "source");
				v.expression = text(*o, "expression");
				v.owner_entity = text(*o, "owner_entity");
				v.owner_usr = bytes(*o, "owner_usr");
				v.invocation_kind = text(*o, "invocation_kind");
				v.expression_kind = text(*o, "expression_kind");
				v.lifetime_source = text(*o, "lifetime_source");
				v.lifetime_usr = bytes(*o, "lifetime_usr");
				v.lifetime_path = bytes(*o, "lifetime_path");
				v.profile = text(*o, "profile");
				v.membership_state = text(*o, "membership_state");
				v.binding_state = text(*o, "binding_state");
				v.reason = text(*o, "reason");
				v.ordinal = number(*o, "ordinal");
				v.depth = number(*o, "depth");
				if (present(*o, "is_system"))
					v.is_system = boolean(*o, "is_system");
				v.root_count = number(*o, "root_count");
				const auto roots = strings(*o, "root_ids");
				if (roots)
					v.root_ids = *roots;
				const auto targets = strings(*o, "callee_usr_hexes");
				if (targets)
					v.callee_usr_hexes = *targets;
				constexpr std::array kinds{"function",
										   "builtin",
										   "constructor",
										   "destructor",
										   "allocation",
										   "deallocation",
										   "pseudo_destructor"};
				if (v.profile != evaluation_profile || !v.ordinal || !v.depth ||
					std::ranges::find(kinds, v.invocation_kind) == kinds.end() ||
					v.membership_state != "complete")
					gap(v, v.call, "call-membership-unavailable");
				v.root_state = finite_population_state::complete;
				if (!v.root_count || !roots || v.root_ids.empty())
					axis(v.root_state, v.gaps, v.call, "call-root-membership-unavailable");
				else if (*v.root_count != v.root_ids.size())
					axis(v.root_state, v.gaps, v.call, "call-root-count-conflicting", true);
				v.target_state = finite_population_state::complete;
				if (!targets || (targets->empty() && v.invocation_kind != "pseudo_destructor") ||
					v.binding_state != "complete")
					axis(v.target_state, v.gaps, v.call, "call-target-unavailable");
				for (const auto& target : v.callee_usr_hexes)
				{
					b.work(target.size());
					if (target.empty() || target.size() % 2U ||
						std::ranges::any_of(target,
											[](char c)
											{
												return !((c >= '0' && c <= '9') ||
														 (c >= 'a' && c <= 'f'));
											}))
						axis(v.target_state, v.gaps, v.call, "call-target-bytes-conflicting", true);
				}
				source(v, scope);
				v.expression_state = reference(4, v.expression, scope, v.evidence, v.gaps);
				usr(v.owner_state, v.gaps, v.owner_entity, v.owner_usr, scope, v.evidence);
				v.lifetime_state = finite_population_state::complete;
				// Lifetime origin is independently source-bound. It is required for an
				// implicit destruction occurrence, but unrelated to explicit-call
				// membership.
				if (v.invocation_kind == "destructor" && v.expression_kind.empty())
				{
					struct origin
					{
						std::string source_span;
						finite_population_state source_state{finite_population_state::unknown};
						refs evidence;
						std::vector<query_unresolved> gaps;
					};
					origin life;
					life.source_span = v.lifetime_source;
					source(life, scope);
					b.bind(v.evidence, life.evidence);
					v.gaps.insert(v.gaps.end(), life.gaps.begin(), life.gaps.end());
					v.lifetime_state = life.source_state;
					if (v.lifetime_path.empty())
						axis(v.lifetime_state, v.gaps, v.call, "lifetime-path-unavailable");
				}
				canonical(v.gaps);
				return v;
			}
			void inventory(const identity& actual, const refs& inventory_rows)
			{
				b.charge(b.members, 1, b.limits.maximum_populations, "populations");
				b.retain(4096);
				template_event_population scope;
				scope.compile_unit = actual[0];
				scope.universe = actual[1];
				scope.variant = actual[2];
				scope.interpretation = actual[3];
				scope.candidate_state = scope.evaluation_root_state = scope.invocation_state =
					finite_population_state::complete;
				const auto lower_all = [&](std::string_view reason, bool conflict = false)
				{
					for (auto* state : {&scope.candidate_state,
										&scope.evaluation_root_state,
										&scope.invocation_state})
						axis(*state, scope.gaps, scope.compile_unit, reason, conflict);
				};
				if (!input.compile_units_complete)
					lower_all("unit-scan-unavailable");
				if (!input.inventory_inputs_complete)
					lower_all("inventory-scan-unavailable");
				b.bind(scope.evidence, inventory_rows);
				if (inventory_rows.empty() || !equal(inventory_rows))
				{
					lower_all(inventory_rows.empty() ? "inventory-missing"
													 : "inventory-conflicting",
							  !inventory_rows.empty());
					output.populations.push_back(std::move(scope));
					return;
				}
				const auto& r = row(inventory_rows.front());
				scope.inventory = text(r, "inventory");
				const auto unit =
					reference(0, scope.compile_unit, scope, scope.evidence, scope.gaps);
				if (unit != finite_population_state::complete)
					lower_all("unit-unavailable", unit == finite_population_state::conflicting);
				else if (text(row(find(0, actual).front()), "variant") != scope.variant)
					lower_all("unit-world-conflicting", true);
				for (std::size_t family = 0; family < 2; ++family)
				{
					const std::string prefix = family == 0 ? "candidate" : "evaluation_root";
					auto& state = family == 0 ? scope.candidate_state : scope.evaluation_root_state;
					auto& count = family == 0 ? scope.candidate_count : scope.evaluation_root_count;
					auto& ids = family == 0 ? scope.candidate_ids : scope.evaluation_root_ids;
					auto& profile =
						family == 0 ? scope.candidate_profile : scope.evaluation_root_profile;
					profile = text(r, prefix + "_profile");
					count = number(r, prefix + "_count");
					const auto members = strings(r, prefix + "_ids");
					if (members)
						ids = *members;
					if (profile != (family == 0 ? candidate_profile : evaluation_profile) ||
						text(r, prefix + "_state") != "complete" || !count || !members)
						axis(state, scope.gaps, scope.inventory, "enumeration-unavailable");
					if (!(family == 0 ? input.candidate_inputs_complete
									  : input.root_inputs_complete))
						axis(state, scope.gaps, scope.inventory, "domain-scan-unavailable");
					if (count && members && *count != ids.size())
						axis(state,
							 scope.gaps,
							 scope.inventory,
							 "enumeration-count-conflicting",
							 true);
					std::set<std::string> returned;
					const auto group = family + 5;
					if (const auto at = unit_members[group - 4].find(actual);
						at != unit_members[group - 4].end())
						for (auto index : at->second)
						{
							b.work();
							b.retain(256);
							returned.insert(text(row(index), identifiers[group]));
						}
					for (const auto& id : ids)
					{
						b.work();
						const auto& originals = find(group, key(id, scope));
						if (originals.empty())
							axis(state, scope.gaps, id, "enumeration-member-missing");
						else if (!equal(originals) ||
								 text(row(originals.front()), "compile_unit") != scope.compile_unit)
							axis(state, scope.gaps, id, "enumeration-member-conflicting", true);
						else if (text(row(originals.front()), "profile") != profile)
							axis(state, scope.gaps, id, "member-profile-conflicting", true);
						returned.erase(id);
					}
					if (!returned.empty())
						axis(state,
							 scope.gaps,
							 scope.inventory,
							 "enumeration-extra-member-conflicting",
							 true);
				}
				for (const auto& id : scope.candidate_ids)
				{
					b.work();
					scope.candidates.push_back(candidate(id, scope));
				}
				for (const auto& id : scope.evaluation_root_ids)
				{
					b.work();
					scope.roots.push_back(root(id, scope));
				}
				if (scope.evaluation_root_state != finite_population_state::complete)
					axis(scope.invocation_state,
						 scope.gaps,
						 scope.inventory,
						 "root-census-unavailable",
						 scope.evaluation_root_state == finite_population_state::conflicting);
				if (!input.call_inputs_complete)
					axis(scope.invocation_state,
						 scope.gaps,
						 scope.inventory,
						 "call-scan-unavailable");
				std::map<std::string, std::size_t, std::less<>> root_index;
				std::set<std::string> calls;
				for (std::size_t i = 0; i < scope.roots.size(); ++i)
				{
					auto& original = scope.roots[i];
					b.work();
					b.retain(256 + original.root.size());
					root_index.emplace(original.root, i);
					if (original.state != finite_population_state::complete ||
						original.invocation_state != finite_population_state::complete)
						axis(scope.invocation_state,
							 scope.gaps,
							 original.root,
							 "root-invocations-unavailable",
							 original.state == finite_population_state::conflicting ||
								 original.invocation_state == finite_population_state::conflicting);
					for (const auto& id : original.call_ids)
					{
						b.work();
						b.retain(256 + id.size());
						calls.insert(id);
					}
				}
				for (const auto& id : calls)
				{
					b.work();
					auto v = call(id, scope);
					for (const auto& root_id : v.root_ids)
					{
						b.work();
						const auto at = root_index.find(root_id);
						if (at == root_index.end())
							axis(v.root_state, v.gaps, v.call, "root-member-missing");
						else
						{
							const auto& original = scope.roots[at->second];
							if (!std::ranges::binary_search(original.call_ids, id))
								axis(v.root_state,
									 v.gaps,
									 v.call,
									 "root-call-membership-conflicting",
									 true);
							b.bind(v.evidence, original.evidence);
						}
					}
					for (const auto& original : scope.roots)
					{
						b.work();
						if (std::ranges::binary_search(original.call_ids, id) &&
							!std::ranges::binary_search(v.root_ids, original.root))
							axis(v.root_state,
								 v.gaps,
								 v.call,
								 "call-root-membership-conflicting",
								 true);
					}
					if (v.state != finite_population_state::complete ||
						v.root_state != finite_population_state::complete)
						axis(scope.invocation_state,
							 scope.gaps,
							 id,
							 "call-membership-unavailable",
							 v.state == finite_population_state::conflicting ||
								 v.root_state == finite_population_state::conflicting);
					canonical(v.gaps);
					scope.calls.push_back(std::move(v));
				}
				if (const auto at = unit_members[3].find(actual); at != unit_members[3].end())
					for (auto i : at->second)
					{
						b.work();
						if (!calls.contains(text(row(i), "call")))
							axis(scope.invocation_state,
								 scope.gaps,
								 scope.inventory,
								 "extra-call-conflicting",
								 true);
					}
				// Member terminal/binding axes remain inspectable; incomplete terminal
				// candidate events cannot close the exclusion census.
				for (const auto& v : scope.candidates)
					if (v.state != finite_population_state::complete)
						axis(scope.candidate_state,
							 scope.gaps,
							 v.candidate,
							 "candidate-terminal-unavailable",
							 v.state == finite_population_state::conflicting);
				canonical(scope.gaps);
				output.populations.push_back(std::move(scope));
			}
		};
		result<template_event_projection>
		project_rows(template_event_input input,
					 finite_population_limits limits,
					 std::stop_token stop,
					 budget& b,
					 const std::array<std::vector<const annotated_row*>, 9>* borrowed = nullptr)
		{
			if (auto valid = limits.validate(); !valid)
				return valid.error();
			try
			{
				b.work();
				projection p{b, input, {}, {}, {}, {}};
				p.output.compile_units_complete = input.compile_units_complete;
				p.output.inventory_inputs_complete = input.inventory_inputs_complete;
				p.output.candidate_inputs_complete = input.candidate_inputs_complete;
				p.output.root_inputs_complete = input.root_inputs_complete;
				p.output.call_inputs_complete = input.call_inputs_complete;
				const std::array groups{input.units,
										input.files,
										input.spans,
										input.entities,
										input.syntax,
										input.candidates,
										input.roots,
										input.calls,
										input.inventories};
				const auto visit_group = [&](std::size_t group, auto&& visit)
				{
					if (borrowed)
					{
						for (const auto* original : (*borrowed)[group])
							visit(*original);
					}
					else
					{
						for (const auto& original : groups[group])
							visit(original);
					}
				};
				// Lexical token spans are independent input rows. Validate all of
				// them, but retain only exact source references of this domain.
				using source_key = std::array<std::string_view, 4>;
				std::set<source_key> source_members;
				for (const auto group : {5U, 7U})
					visit_group(group,
								[&](const annotated_row& original)
								{
									b.work();
									for (const auto name : {"source", "lifetime_source"})
									{
										const auto* c = cell(original, name);
										const auto* id =
											c && c->state == cell_state::present && c->value
											? std::get_if<std::string>(&*c->value)
											: nullptr;
										if (!id || id->empty())
											continue;
										for (const auto& variant : original.presence.fragments)
										{
											b.work();
											const source_key member{*id,
																	original.presence.universe,
																	variant,
																	original.interpretation};
											if (!source_members.contains(member))
											{
												b.retain(sizeof(source_key) + 96U);
												source_members.insert(member);
											}
										}
									}
								});
				std::set<source_key> entity_members, syntax_members;
				for (const auto group : {5U, 7U})
					visit_group(group,
								[&](const annotated_row& original)
								{
									for (const auto name :
										 {"candidate_entity", "owner_entity", "expression"})
									{
										b.work();
										const auto* c = cell(original, name);
										const auto* id =
											c && c->state == cell_state::present && c->value
											? std::get_if<std::string>(&*c->value)
											: nullptr;
										if (!id || id->empty())
											continue;
										auto& members = std::string_view(name) == "expression"
											? syntax_members
											: entity_members;
										for (const auto& variant : original.presence.fragments)
										{
											b.work();
											const source_key member{*id,
																	original.presence.universe,
																	variant,
																	original.interpretation};
											if (!members.contains(member))
											{
												b.retain(sizeof(source_key) + 96U);
												members.insert(member);
											}
										}
									}
								});
				std::vector<entry> entries;
				const auto descriptors = standard_relation_descriptors();
				for (std::size_t group = 0; group < groups.size(); ++group)
				{
					const auto descriptor =
						std::ranges::find(descriptors, relations[group], &relation_descriptor::id);
					if (descriptor == descriptors.end())
						fail(relations[group], "descriptor-missing");
					visit_group(
						group,
						[&](const annotated_row& r)
						{
							b.work();
							b.charge(b.rows, 1, limits.maximum_rows, "rows");

							if (auto valid = detail::validate_projected_relation_row(
									r,
									*descriptor,
									"sdk.template-event-input-invalid",
									[&]
									{
										b.work();
									});
								!valid)
								throw failure{valid.error()};
							b.charge(b.conditions,
									 r.presence.fragments.size(),
									 limits.maximum_condition_expansions,
									 "conditions");
							if (group == 2U || group == 3U || group == 4U)
							{
								const auto* c = cell(r, identifiers[group]);
								const auto* id =
									c && c->value ? std::get_if<std::string>(&*c->value) : nullptr;
								const auto& members = group == 2U ? source_members
									: group == 3U				  ? entity_members
																  : syntax_members;
								if (!id ||
									!std::ranges::any_of(r.presence.fragments,
														 [&](const auto& variant)
														 {
															 b.work();
															 return members.contains(
																 source_key{*id,
																			r.presence.universe,
																			variant,
																			r.interpretation});
														 }))
									return;
							}
							b.retain(b.estimate(r));
							std::string payload;
							for (const auto& [name, c] : r.values)
							{
								b.work();
								payload += name + '=' + c.canonical_form() + '\n';
							}
							const auto canonical = r.canonical_form();
							b.charge(b.evidence,
									 canonical.size(),
									 limits.maximum_evidence_bytes,
									 "evidence-bytes");
							entries.push_back({group, &r, canonical, std::move(payload)});
						});
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
					const auto index = p.output.evidence.size();
					p.output.evidence.push_back({std::string(relations[e.group]), *e.row});
					p.payloads.push_back(std::move(e.payload));
					const auto id = text(*e.row, identifiers[e.group]);
					if (id.empty())
						fail(relations[e.group], "identity-missing");
					for (const auto& variant : e.row->presence.fragments)
					{
						b.work();
						b.retain(1024 + id.size() + variant.size() +
								 e.row->presence.universe.size() + e.row->interpretation.size());
						const auto actual = key(id, *e.row, variant);
						p.maps[e.group][actual].push_back(index);
						if (e.group >= 4)
							p.unit_members[e.group - 4]
										  [key(text(*e.row, "compile_unit"), *e.row, variant)]
											  .push_back(index);
					}
				}
				std::set<identity> scopes;
				for (const auto& [actual, ignored] : p.maps[0])
				{
					(void)ignored;
					b.work();
					b.retain(256);
					scopes.insert(actual);
				}
				for (const auto& [actual, ignored] : p.unit_members[4])
				{
					(void)ignored;
					b.work();
					b.retain(256);
					scopes.insert(actual);
				}
				for (const auto family : {1U, 2U, 3U})
					for (const auto& [actual, ignored] : p.unit_members[family])
					{
						(void)ignored;
						b.work();
						b.retain(256);
						scopes.insert(actual);
					}
				for (const auto& actual : scopes)
				{
					const auto at = p.unit_members[4].find(actual);
					static const refs empty;
					p.inventory(actual, at == p.unit_members[4].end() ? empty : at->second);
				}
				return std::move(p.output);
			}
			catch (const failure& e)
			{
				return e.value;
			}
			catch (const std::bad_alloc&)
			{
				return error{"sdk.template-event-resource-exhausted", "projection", "allocation"};
			}
			catch (const std::length_error&)
			{
				return error{"sdk.template-event-resource-exhausted", "projection", "length"};
			}
			(void)stop;
		}
	} // namespace
	result<template_event_projection> project_template_events(template_event_input input,
															  finite_population_limits limits,
															  std::stop_token stop)
	{
		budget b{limits, stop};
		return project_rows(input, limits, stop, b);
	}
	result<template_event_projection>
	project_template_events(const application_query_results& input,
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
			const std::function<bool()> active = [&]
			{
				b.work();
				return false;
			};
			if (auto valid = detail::check_source_plan_limits(input,
															  limits.maximum_source_queries,
															  limits.maximum_source_plan_bytes,
															  stop,
															  "sdk.template-event",
															  &plan_bytes,
															  active);
				!valid)
				return valid.error();
			std::array<std::vector<const annotated_row*>, 9> groups;
			std::array<bool, 9> present{}, complete{};
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
					scan.result.conflicts().empty() &&
					scan.result.differential_disagreements().empty();
				const auto rows = query_transfer_access::borrow_rows(scan.result);
				b.charge(b.rows, rows.size(), limits.maximum_rows, "scan-rows");
				if (groups[group].size() > limits.maximum_rows - rows.size())
					fail("scan-rows", "limit-exceeded", "sdk.template-event-budget");
				b.retain(rows.size() * sizeof(const annotated_row*));
				groups[group].reserve(groups[group].size() + rows.size());
				for (const auto& original : rows)
				{
					b.work();
					groups[group].push_back(&original);
				}
			}
			b.rows = 0;
			template_event_input raw{};
			raw.compile_units_complete = present[0] && complete[0];
			raw.inventory_inputs_complete = present[8] && complete[8];
			raw.candidate_inputs_complete = present[5] && complete[5];
			raw.root_inputs_complete = present[6] && complete[6];
			raw.call_inputs_complete = present[7] && complete[7];
			auto result = project_rows(raw, limits, stop, b, &groups);
			if (!result)
				return result;
			for (std::size_t group = 0; group < 9; ++group)
				if (!present[group])
					result->unresolved.push_back({"sdk.template-event-scan-missing",
												  std::string(relations[group]),
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
			return error{"sdk.template-event-resource-exhausted", "projection", "allocation"};
		}
		catch (const std::length_error&)
		{
			return error{"sdk.template-event-resource-exhausted", "projection", "length"};
		}
	}
} // namespace cxxlens::sdk::query
