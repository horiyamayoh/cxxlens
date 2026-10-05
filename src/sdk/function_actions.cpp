#include <algorithm>
#include <array>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <tuple>

#include <cxxlens/sdk/function_actions.hpp>

#include "query_projection_plan_limits_internal.hpp"
#include "query_projection_rows_internal.hpp"
#include "query_result_internal.hpp"

namespace cxxlens::sdk::query
{
	namespace
	{
		using identity = std::array<std::string, 4>;
		using refs = std::vector<std::size_t>;
		constexpr std::array<std::string_view, 13> relations{"build.compile_unit.v1",
															 "source.file.v1",
															 "source.span.v1",
															 "cc.entity.v1",
															 "cc.entity_detail.v1",
															 "cc.declaration.v1",
															 "cc.type.v1",
															 "cc.type_component.v1",
															 "cc.body.v1",
															 "cc.syntax_node.v1",
															 "cc.cfg_node.v1",
															 "cc.call_site.v1",
															 "cc.operation.v1"};
		constexpr std::array<std::string_view, 13> identifiers{"compile_unit",
															   "snapshot",
															   "span",
															   "entity",
															   "detail",
															   "declaration",
															   "type",
															   "owner_type",
															   "body",
															   "node",
															   "node",
															   "call",
															   "operation"};
		constexpr std::string_view action_profile = "clang22-function-compiler-actions/1";
		struct failure
		{
			error value;
		};
		[[noreturn]] void fail(std::string_view field,
							   std::string_view reason,
							   std::string_view code = "sdk.action-input-invalid")
		{
			throw failure{{std::string{code}, std::string{field}, std::string{reason}}};
		}
		const detached_cell* cell(const annotated_row& row, std::string_view name)
		{
			const auto found = row.values.find("output." + std::string{name});
			return found == row.values.end() ? nullptr : &found->second;
		}
		std::string_view borrowed_text(const annotated_row& row, std::string_view name)
		{
			const auto* c = cell(row, name);
			const auto* v = c && c->state == cell_state::present && c->value
				? std::get_if<std::string>(&*c->value)
				: nullptr;
			return v ? std::string_view{*v} : std::string_view{};
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
		template <class T>
		void gap(T& value, std::string_view subject, std::string_view reason, bool conflict = false)
		{
			value.gaps.push_back({"sdk.action-" + std::string{reason}, std::string{subject}, {}});
			downgrade(value.state, conflict);
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
					fail(field, "limit-exceeded", "sdk.action-budget");
				used += amount;
			}
			void work(std::size_t n = 1)
			{
				if (stop.stop_requested() || (limits.cancelled && limits.cancelled()))
					fail("projection", "stop-requested", "sdk.action-cancelled");
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
			std::size_t estimate(const annotated_row& row, bool encoding = true)
			{
				std::size_t total = 2048;
				const auto add = [&](std::size_t n, std::size_t factor = 8U)
				{
					if (!encoding)
						factor = 2U;
					work(n);
					work();
					if (total > limits.maximum_retained_bytes ||
						n > (limits.maximum_retained_bytes - total) / factor)
						fail("row", "limit-exceeded", "sdk.action-budget");
					total += n * factor;
				};
				const auto fixed = [&](std::size_t n)
				{
					work();
					if (total > limits.maximum_retained_bytes ||
						n > limits.maximum_retained_bytes - total)
						fail("row", "limit-exceeded", "sdk.action-budget");
					total += n;
				};
				for (const auto& [name, c] : row.values)
				{
					// One owned detached cell plus map-node/framing overhead. The
					// dynamic payload allowance below still covers encoded copies.
					fixed(sizeof(decltype(row.values)::value_type) + 256U);
					add(name.size() + c.type.parameter.size());
					if (c.unknown_reason)
						add(c.unknown_reason->size(), 16U);
					if (c.value)
					{
						if (const auto* v = std::get_if<std::string>(&*c.value))
							add(v->size(), 16U);
						if (const auto* v = std::get_if<std::vector<std::byte>>(&*c.value))
							add(v->size());
					}
				}
				const auto strings = [&](const auto& values)
				{
					for (const auto& v : values)
					{
						fixed(sizeof(v) + 128U);
						add(v.size());
					}
				};
				const auto producer = [&](const auto& p)
				{
					fixed(sizeof(p) + 128U);
					add(p.id.size() + p.semantic_contract.size());
				};
				const auto guarantee = [&](const auto& g)
				{
					fixed(sizeof(g) + 128U);
					add(g.approximation.size() + g.scope.size() + g.assumptions.size());
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
					fixed(sizeof(e) + 128U);
					add(e.claim_contributor.size() + e.provenance.size() + e.interpretation.size() +
						e.condition.universe.size());
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
			std::string canonical;
			std::size_t payload_bytes{};
		};
		struct projection
		{
			budget& b;
			function_action_input input;
			function_action_projection output;
			std::array<std::map<identity, refs>, 13> maps;
			std::map<identity, refs> declaration_sources, detail_sources, scope_operations,
				function_bodies, function_syntax, function_details, body_nodes, type_members, sites;
			std::vector<std::size_t> payloads;
			std::map<identity, std::pair<finite_population_state, refs>> site_states;
			projection(budget& bounds, function_action_input raw) : b(bounds), input(raw) {}
			const annotated_row& row(std::size_t i) const
			{
				return output.evidence.at(i).row;
			}
			const refs& find(std::size_t group, const identity& id) const
			{
				static const refs empty;
				const auto f = maps[group].find(id);
				return f == maps[group].end() ? empty : f->second;
			}
			const refs& indexed(const std::map<identity, refs>& index, const identity& id) const
			{
				static const refs empty;
				const auto f = index.find(id);
				return f == index.end() ? empty : f->second;
			}
			bool equal(const refs& r)
			{
				if (r.empty())
					return false;
				for (auto i : r)
				{
					b.work();
					const auto& left = row(i).values;
					const auto& right = row(r.front()).values;
					if (left.size() != right.size())
						return false;
					auto x = left.begin(), y = right.begin();
					for (; x != left.end(); ++x, ++y)
					{
						for (const auto* value : {&*x, &*y})
						{
							b.work(value->first.size() + value->second.type.parameter.size() + 1U);
							if (value->second.unknown_reason)
								b.work(value->second.unknown_reason->size());
							if (value->second.value)
							{
								if (const auto* text =
										std::get_if<std::string>(&*value->second.value))
									b.work(text->size());
								if (const auto* bytes =
										std::get_if<std::vector<std::byte>>(&*value->second.value))
									b.work(bytes->size());
							}
						}
						if (x->first != y->first ||
							x->second.type.scalar != y->second.type.scalar ||
							x->second.type.parameter != y->second.type.parameter ||
							x->second.type.optional != y->second.type.optional ||
							x->second.state != y->second.state ||
							x->second.value != y->second.value ||
							x->second.unknown_reason != y->second.unknown_reason)
							return false;
					}
				}
				return true;
			}
			std::vector<std::string> strings(const annotated_row& r, std::string_view name)
			{
				if (!present(r, name))
					return {};
				const auto raw = bytes(r, name);
				b.retain(raw.size() * 8);
				std::vector<std::string> result;
				for (std::size_t i = 0; i < raw.size();)
				{
					b.work();
					if (raw.size() - i < 4)
						fail(name, "truncated-set");
					std::uint32_t n = 0;
					for (unsigned shift = 0; shift < 32; shift += 8)
						n |= std::to_integer<std::uint32_t>(raw[i++]) << shift;
					if (n > raw.size() - i)
						fail(name, "truncated-set");
					b.charge(b.members, 1, b.limits.maximum_members, "set-members");
					std::string value;
					value.reserve(n);
					for (std::size_t end = i + n; i < end; ++i)
						value += static_cast<char>(raw[i]);
					if (value.empty() || (!result.empty() && result.back() >= value))
						fail(name, "canonical-set-required");
					result.push_back(std::move(value));
				}
				return result;
			}
			template <class T>
			void source(T& value, std::string_view subject)
			{
				const auto& spans = find(2, key(value.source_span, value));
				b.bind(value.evidence, spans);
				if (spans.empty())
				{
					gap(value, subject, "source-unavailable");
					return;
				}
				if (!equal(spans))
				{
					gap(value, subject, "source-conflicting", true);
					return;
				}
				const auto& observed = row(spans.front());
				value.file = text(observed, "file");
				value.source_snapshot = text(observed, "snapshot");
				value.begin = number(observed, "begin");
				value.end = number(observed, "end");
				const auto& files = find(1, key(value.source_snapshot, value));
				b.bind(value.evidence, files);
				if (files.empty())
				{
					gap(value, subject, "source-snapshot-unavailable");
					return;
				}
				if (!equal(files))
				{
					gap(value, subject, "source-snapshot-conflicting", true);
					return;
				}
				const auto size = number(row(files.front()), "size");
				if (text(row(files.front()), "file") != value.file || !size || !value.begin ||
					!value.end || (*value.end) < (*value.begin) || (*value.end) > (*size))
					gap(value, subject, "source-bounds-conflicting", true);
			}
			finite_population_state reference(std::size_t group,
											  std::string_view id,
											  const function_action_population& scope,
											  refs& evidence,
											  std::vector<query_unresolved>& gaps)
			{
				if (id.empty())
					return finite_population_state::unknown;
				const auto& r = find(group, key(std::string{id}, scope));
				b.bind(evidence, r);
				if (r.empty())
				{
					gaps.push_back({"sdk.action-reference-unavailable",
									std::string{id},
									std::string{relations[group]}});
					return finite_population_state::unknown;
				}
				if (!equal(r))
				{
					gaps.push_back({"sdk.action-reference-conflicting",
									std::string{id},
									std::string{relations[group]}});
					return finite_population_state::conflicting;
				}
				return finite_population_state::complete;
			}
			std::optional<bool> contains(const function_action_population& scope,
										 std::string_view span,
										 refs& evidence,
										 std::vector<query_unresolved>& gaps)
			{
				function_action_population bound;
				bound.universe = scope.universe;
				bound.variant = scope.variant;
				bound.interpretation = scope.interpretation;
				bound.source_span = span;
				bound.state = finite_population_state::complete;
				source(bound, span);
				b.bind(evidence, bound.evidence);
				gaps.insert(gaps.end(), bound.gaps.begin(), bound.gaps.end());
				if (bound.state != finite_population_state::complete || !scope.begin || !scope.end)
					return std::nullopt;
				return scope.file == bound.file && scope.source_snapshot == bound.source_snapshot &&
					*scope.begin <= *bound.begin && *bound.end <= *scope.end;
			}
			finite_population_state type(std::string id,
										 const function_action_population& scope,
										 refs& evidence,
										 std::vector<query_unresolved>& gaps,
										 std::string* structure = nullptr)
			{
				auto state = finite_population_state::complete;
				std::set<std::string> visited;
				std::vector<std::string> pending;
				b.retain(id.size() + sizeof(std::string));
				pending.push_back(std::move(id));
				while (!pending.empty())
				{
					b.work();
					auto current = std::move(pending.back());
					pending.pop_back();
					b.retain(current.size() + 128);
					if (!visited.insert(current).second)
						continue;
					const auto refstate = reference(6, current, scope, evidence, gaps);
					if (refstate != finite_population_state::complete)
					{
						downgrade(state, refstate == finite_population_state::conflicting);
						continue;
					}
					const auto& native = row(find(6, key(current, scope)).front());
					if (structure && visited.size() == 1)
						*structure = text(native, "structure_preimage");
					if (!input.type_inputs_complete ||
						text(native, "structure_state") != "complete" ||
						text(native, "structure_profile") != "clang22-structural-type/1" ||
						!present(native, "structure_preimage") || boolean(native, "dependent"))
					{
						downgrade(state, false);
						gaps.push_back({"sdk.action-type-structure-unavailable", current, {}});
					}
					for (auto i : indexed(type_members, key(current, scope)))
					{
						b.work();
						b.bind(evidence, refs{i});
						const auto child = text(row(i), "component_type");
						if (!child.empty() && !visited.contains(child))
						{
							b.retain(child.size() + sizeof(std::string));
							pending.push_back(child);
						}
					}
				}
				return state;
			}
			observed_call_target_signature signature(const annotated_row& native,
													 const function_action_population& scope,
													 const refs& original)
			{
				observed_call_target_signature value;
				value.target = text(native, "target");
				value.profile = text(native, "target_signature_profile");
				value.canonical_type = text(native, "target_canonical_type");
				value.canonical_type_profile = text(native, "target_canonical_type_profile");
				value.canonical_type_digest = text(native, "target_canonical_type_digest");
				value.structural_signature_digest =
					text(native, "target_structural_signature_digest");
				value.language = text(native, "target_language");
				value.linkage = text(native, "target_linkage");
				value.module_domain = text(native, "target_module_domain");
				value.usr = bytes(native, "target_usr");
				value.state = text(native, "target_signature_state") == "complete" &&
						value.profile == "clang22-original-target-signature/1"
					? finite_population_state::complete
					: finite_population_state::unknown;
				b.bind(value.evidence, original);
				if (!equal(original))
					gap(value, value.target, "target-signature-conflicting", true);
				if (value.profile.empty() && value.target.empty())
					return value;
				for (auto name : {"target_usr",
								  "target_canonical_type",
								  "target_structural_signature_digest",
								  "target_canonical_type_digest",
								  "target_canonical_type_profile",
								  "target_language",
								  "target_linkage",
								  "target_module_domain"})
					if (!present(native, name))
						gap(value, value.target, "target-signature-facet-unavailable");
				const auto typestate = type(value.canonical_type,
											scope,
											value.evidence,
											value.gaps,
											&value.canonical_type_structure);
				if (typestate != finite_population_state::complete)
					gap(value,
						value.target,
						"target-type-unavailable",
						typestate == finite_population_state::conflicting);
				const auto& types = find(6, key(value.canonical_type, scope));
				if (equal(types))
				{
					const auto& original_type = row(types.front());
					if ((present(native, "target_canonical_type_digest") &&
						 present(original_type, "component_signature_digest") &&
						 text(original_type, "component_signature_digest") !=
							 value.canonical_type_digest) ||
						(present(native, "target_canonical_type_profile") &&
						 present(original_type, "structure_profile") &&
						 text(original_type, "structure_profile") != value.canonical_type_profile))
						gap(value, value.target, "target-type-signature-conflicting", true);
				}
				const auto& entities = find(3, key(value.target, scope));
				b.bind(value.evidence, entities);
				if (!entities.empty())
				{
					if (!equal(entities))
						gap(value, value.target, "target-entity-conflicting", true);
					else if ((present(native, "target_usr") &&
							  present(row(entities.front()), "provider_local_key") &&
							  bytes(row(entities.front()), "provider_local_key") != value.usr) ||
							 (present(native, "target_structural_signature_digest") &&
							  present(row(entities.front()), "structural_signature_digest") &&
							  text(row(entities.front()), "structural_signature_digest") !=
								  value.structural_signature_digest))
						gap(value, value.target, "target-entity-signature-conflicting", true);
				}
				canonical(value.gaps);
				return value;
			}
			std::string context(observed_function_action& value,
								const function_action_population& scope)
			{
				if (value.expression_context.empty() || value.expression_context == "written_scope")
				{
					value.context_state = value.context_declaration.empty()
						? finite_population_state::complete
						: finite_population_state::conflicting;
					return {};
				}
				if (value.origin != "cfg" ||
					(value.expression_context != "default_argument" &&
					 value.expression_context != "default_initializer"))
				{
					value.context_state = finite_population_state::unknown;
					return {};
				}
				value.context_state =
					reference(5, value.context_declaration, scope, value.evidence, value.gaps);
				if (value.context_state != finite_population_state::complete)
					return {};
				const auto& declaration =
					row(find(5, key(value.context_declaration, scope)).front());
				const auto entity = text(declaration, "entity");
				value.context_state = reference(3, entity, scope, value.evidence, value.gaps);
				if (value.context_state != finite_population_state::complete)
					return {};
				const auto& observed_entity = row(find(3, key(entity, scope)).front());
				const auto expected_kind =
					value.expression_context == "default_argument" ? "parameter" : "field";
				if (text(observed_entity, "kind") != expected_kind)
				{
					value.context_state = finite_population_state::conflicting;
					return {};
				}
				refs details;
				for (auto i : indexed(detail_sources,
									  key(entity + '\n' + text(declaration, "source"), scope)))
				{
					b.work();
					if (text(row(i), "compile_unit") == scope.compile_unit)
						b.bind(details, refs{i});
				}
				b.bind(value.evidence, details);
				if (details.empty())
				{
					value.context_state = finite_population_state::unknown;
					return {};
				}
				if (!equal(details))
				{
					value.context_state = finite_population_state::conflicting;
					return {};
				}
				const auto owner = text(observed_entity, "semantic_owner");
				if (owner.empty())
				{
					value.context_state = finite_population_state::unknown;
					return {};
				}
				const auto owner_state = reference(3, owner, scope, value.evidence, value.gaps);
				if (owner_state != finite_population_state::complete)
				{
					value.context_state = owner_state;
					return {};
				}
				if (value.expression_context == "default_argument")
				{
					const auto kind = text(row(find(3, key(owner, scope)).front()), "kind");
					const auto flags = strings(row(details.front()), "flags");
					if (kind != "function" && kind != "method" && kind != "constructor" &&
						kind != "destructor" && kind != "conversion")
						value.context_state = finite_population_state::conflicting;
					else if (!std::ranges::binary_search(flags, "finite_type_use_admission_v1"))
						value.context_state = finite_population_state::unknown;
					else if (!std::ranges::binary_search(flags, "default_argument"))
						value.context_state = finite_population_state::conflicting;
				}
				else
				{
					const auto& caller = find(3, key(scope.function, scope));
					b.bind(value.evidence, caller);
					if (!equal(caller))
						value.context_state = caller.empty() ? finite_population_state::unknown
															 : finite_population_state::conflicting;
					else if (text(row(caller.front()), "kind") != "constructor" ||
							 text(row(caller.front()), "semantic_owner") != owner)
						value.context_state = finite_population_state::conflicting;
				}
				function_action_population bound;
				bound.universe = scope.universe;
				bound.variant = scope.variant;
				bound.interpretation = scope.interpretation;
				bound.source_span = text(declaration, "source");
				bound.state = finite_population_state::complete;
				source(bound, value.context_declaration);
				b.bind(value.evidence, bound.evidence);
				value.gaps.insert(value.gaps.end(), bound.gaps.begin(), bound.gaps.end());
				if (bound.state != finite_population_state::complete)
					downgrade(value.context_state,
							  bound.state == finite_population_state::conflicting);
				return owner;
			}
			bool context_owner(std::string_view owner,
							   std::string_view declared_owner,
							   observed_function_action& value,
							   const function_action_population& scope)
			{
				if (value.expression_context.empty() || value.expression_context == "written_scope")
					return owner.empty() || owner == scope.function;
				if (value.context_state != finite_population_state::complete)
					return false;
				if (value.expression_context == "default_argument")
				{
					if (owner.empty())
						value.context_state = finite_population_state::unknown;
					return owner == declared_owner;
				}
				if (owner.empty() || owner == scope.function)
					return true;
				const auto& entities = find(3, key(std::string{owner}, scope));
				b.bind(value.evidence, entities);
				if (entities.empty())
				{
					value.context_state = finite_population_state::unknown;
					return false;
				}
				if (!equal(entities))
				{
					value.context_state = finite_population_state::conflicting;
					return false;
				}
				return equal(entities) && text(row(entities.front()), "kind") == "constructor" &&
					text(row(entities.front()), "semantic_owner") == declared_owner;
			}
			observed_function_action action(const refs& original, function_action_population& scope)
			{
				const auto& native = row(original.front());
				observed_function_action value;
				b.retain(sizeof(value) + payloads[original.front()]);
				value.operation = text(native, "operation");
				value.site = text(native, "site");
				value.scope_declaration = text(native, "scope_declaration");
				value.function = text(native, "function");
				value.compile_unit = text(native, "compile_unit");
				value.body = text(native, "body");
				value.kind = text(native, "kind");
				value.origin = text(native, "origin");
				value.profile = text(native, "profile");
				value.evaluation = text(native, "evaluation");
				value.outcome = text(native, "outcome");
				value.observation_state = text(native, "observation_state");
				value.site_state = text(native, "site_state");
				value.site_binding_state = value.site_state == "complete" && !value.site.empty()
					? finite_population_state::complete
					: value.site_state == "partial" ? finite_population_state::partial
													: finite_population_state::unknown;
				value.source_span = text(native, "source");
				value.expression = text(native, "expression");
				value.call = text(native, "call");
				value.parent_site = text(native, "parent_site");
				value.expression_context = text(native, "expression_context");
				value.context_declaration = text(native, "context_declaration");
				value.node = text(native, "node");
				value.element_kind = text(native, "element_kind");
				value.program_point_profile = text(native, "program_point_profile");
				value.target_kind = text(native, "target_kind");
				value.type_use_kind = text(native, "type_use_kind");
				value.ordinal = number(native, "ordinal").value_or(0);
				value.element_index = number(native, "element_index");
				value.program_point = number(native, "program_point");
				value.subject_index = number(native, "subject_index");
				if (present(native, "parameter_has_default_argument"))
					value.parameter_has_default_argument =
						boolean(native, "parameter_has_default_argument");
				value.object_declaration = text(native, "object_declaration");
				value.object_entity = text(native, "object_entity");
				value.object_expression = text(native, "object_expression");
				value.object_source = text(native, "object_source");
				value.object_type = text(native, "object_type");
				value.object_binding_state = text(native, "object_state");
				value.state = value.observation_state == "complete"
					? finite_population_state::complete
					: value.observation_state == "partial" ? finite_population_state::partial
														   : finite_population_state::unknown;
				b.bind(value.evidence, original);
				if (!equal(original))
					gap(value, value.operation, "occurrence-conflicting", true);
				if (value.scope_declaration != scope.declaration ||
					value.compile_unit != scope.compile_unit ||
					(!value.function.empty() && value.function != scope.function) ||
					(!value.body.empty() && !scope.body.empty() && value.body != scope.body))
					gap(value, value.operation, "owner-conflicting", true);
				constexpr std::array known_kinds{"invocation",
												 "construction",
												 "allocation",
												 "initialization_failure_deallocation",
												 "deallocation",
												 "destruction",
												 "cleanup_function",
												 "load",
												 "store",
												 "update",
												 "atomic",
												 "type_use",
												 "throw_expression",
												 "coroutine_suspend"};
				if (value.profile != action_profile ||
					std::ranges::find(known_kinds, value.kind) == known_kinds.end() ||
					(value.origin != "ast" && value.origin != "cfg" &&
					 value.origin != "declaration"))
					gap(value, value.operation, "profile-or-kind-unavailable");
				constexpr std::array evaluations{
					"potentially_evaluated", "unevaluated", "declarative", "dependent", "unknown"};
				if (std::ranges::find(evaluations, value.evaluation) == evaluations.end() ||
					(value.outcome != "ordinary" && value.outcome != "initialization_failure"))
					gap(value, value.operation, "evaluation-or-outcome-unavailable");
				if (value.site_state == "complete" && value.site.empty())
					gap(value, value.operation, "site-conflicting", true);
				if (value.parameter_has_default_argument &&
					(value.kind != "type_use" || value.origin != "declaration" ||
					 value.type_use_kind != "parameter"))
					gap(value, value.operation, "parameter-default-facet-conflicting", true);
				if (!value.source_span.empty())
				{
					function_action_population bound;
					bound.universe = scope.universe;
					bound.variant = scope.variant;
					bound.interpretation = scope.interpretation;
					bound.source_span = value.source_span;
					bound.state = finite_population_state::complete;
					source(bound, value.operation);
					value.file = bound.file;
					value.source_snapshot = bound.source_snapshot;
					value.begin = bound.begin;
					value.end = bound.end;
					value.source_state = bound.state;
					b.bind(value.evidence, bound.evidence);
					value.gaps.insert(value.gaps.end(), bound.gaps.begin(), bound.gaps.end());
				}
				if (value.source_state != finite_population_state::complete)
					gap(value,
						value.operation,
						"occurrence-source-unavailable",
						value.source_state == finite_population_state::conflicting);
				const auto declared_owner = context(value, scope);
				if (value.context_state != finite_population_state::complete)
					gap(value,
						value.operation,
						"expression-context-unavailable",
						value.context_state == finite_population_state::conflicting);
				value.expression_state =
					reference(9, value.expression, scope, value.evidence, value.gaps);
				if (!value.expression.empty() &&
					value.expression_state == finite_population_state::complete)
				{
					const auto& expression = row(find(9, key(value.expression, scope)).front());
					const auto function = text(expression, "function");
					if (text(expression, "compile_unit") != scope.compile_unit)
						value.expression_state = finite_population_state::conflicting;
					else if (!context_owner(function, declared_owner, value, scope))
						value.expression_state =
							value.context_state == finite_population_state::unknown ||
								value.context_state == finite_population_state::partial
							? value.context_state
							: finite_population_state::conflicting;
					if (value.origin == "ast" && function != scope.function)
						value.expression_state = finite_population_state::conflicting;
				}
				if (!value.expression.empty() &&
					value.expression_state != finite_population_state::complete)
					gap(value,
						value.operation,
						"expression-unavailable",
						value.expression_state == finite_population_state::conflicting);
				if (value.origin == "ast" && value.expression.empty())
					gap(value, value.operation, "ast-expression-unavailable");
				value.call_state = reference(11, value.call, scope, value.evidence, value.gaps);
				if (!input.call_inputs_complete)
					value.call_state = finite_population_state::unknown;
				if (!value.call.empty() && value.call_state == finite_population_state::complete)
				{
					const auto& call = row(find(11, key(value.call, scope)).front());
					if (text(call, "compile_unit") != scope.compile_unit ||
						(!value.expression.empty() && present(call, "expression") &&
						 text(call, "expression") != value.expression))
						value.call_state = finite_population_state::conflicting;
					else if (!context_owner(text(call, "caller"), declared_owner, value, scope))
						value.call_state =
							value.context_state == finite_population_state::unknown ||
								value.context_state == finite_population_state::partial
							? value.context_state
							: finite_population_state::conflicting;
					else if (text(call, "caller").empty() &&
							 value.expression_context != "default_initializer")
						value.call_state = finite_population_state::unknown;
				}
				if ((!value.call.empty() &&
					 value.call_state != finite_population_state::complete) ||
					(value.kind == "invocation" && value.call.empty()))
					gap(value,
						value.operation,
						"call-unavailable",
						value.call_state == finite_population_state::conflicting);
				value.cfg_state = value.origin == "cfg"
					? reference(10, value.node, scope, value.evidence, value.gaps)
					: finite_population_state::complete;
				if (value.origin == "cfg" && value.cfg_state == finite_population_state::complete)
				{
					const auto& node = row(find(10, key(value.node, scope)).front());
					const auto count = number(node, "element_count");
					if (text(node, "body") != value.body ||
						text(node, "function") != scope.function ||
						text(node, "compile_unit") != scope.compile_unit || !value.element_index ||
						!count || *value.element_index >= *count || value.element_kind.empty())
						value.cfg_state = finite_population_state::conflicting;
				}
				if (value.origin == "cfg" && value.cfg_state != finite_population_state::complete)
					gap(value,
						value.operation,
						"cfg-unavailable",
						value.cfg_state == finite_population_state::conflicting);
				if (value.origin != "cfg" &&
					(!value.node.empty() || value.element_index || !value.element_kind.empty()))
					gap(value, value.operation, "non-cfg-placement-conflicting", true);
				// A future point profile remains unknown; element_index is never a flow
				// point.
				if (value.program_point || !value.program_point_profile.empty())
					gap(value, value.operation, "program-point-profile-unavailable");
				value.target_signature = signature(native, scope, original);
				if (!value.site.empty())
				{
					const auto site_key = key(value.compile_unit + '\n' + value.site, scope);
					auto found = site_states.find(site_key);
					if (found == site_states.end())
					{
						auto state = finite_population_state::complete;
						refs witnesses;
						std::map<std::string, std::pair<std::string, std::size_t>> facets;
						std::optional<std::pair<std::uint64_t, std::size_t>> subject_index;
						for (auto ref : indexed(sites, key(value.site, scope)))
						{
							b.work();
							const auto& candidate = row(ref);
							if (text(candidate, "compile_unit") != scope.compile_unit)
								continue;
							if (witnesses.empty())
								witnesses.push_back(ref);
							if (text(candidate, "site_state") != "complete")
								downgrade(state, false);
							if (text(candidate, "profile") != action_profile)
								downgrade(state, false);
							for (auto field : {"scope_declaration",
											   "kind",
											   "type_use_kind",
											   "expression",
											   "expression_context",
											   "context_declaration",
											   "object_declaration",
											   "object_entity",
											   "target",
											   "target_kind"})
								if (present(candidate, field))
								{
									b.work();
									const auto payload = text(candidate, field);
									b.retain(payload.size() + 256);
									const auto [facet, inserted] =
										facets.emplace(field, std::pair{payload, ref});
									if (!inserted && facet->second.first != payload)
									{
										state = finite_population_state::conflicting;
										witnesses = {facet->second.second, ref};
									}
								}
							if (const auto slot = number(candidate, "subject_index"))
							{
								if (subject_index && subject_index->first != *slot)
								{
									state = finite_population_state::conflicting;
									witnesses = {subject_index->second, ref};
								}
								else if (!subject_index)
									subject_index = std::pair{*slot, ref};
							}
						}
						b.retain(sizeof(site_key) + site_key[0].size() + 256);
						found =
							site_states.emplace(site_key, std::pair{state, std::move(witnesses)})
								.first;
					}
					value.site_binding_state = found->second.first;
					b.bind(value.evidence, found->second.second);
					if (value.site_binding_state != finite_population_state::complete)
						value.gaps.push_back({"sdk.action-site-group-unavailable", value.site, {}});
				}
				value.type_state = value.object_type.empty()
					? finite_population_state::unknown
					: type(value.object_type, scope, value.evidence, value.gaps);
				value.object_state = value.object_binding_state == "complete"
					? finite_population_state::complete
					: value.object_binding_state == "partial" ? finite_population_state::partial
															  : finite_population_state::unknown;
				for (const auto& binding : std::array<std::pair<std::size_t, std::string_view>, 4>{
						 {{5, value.object_declaration},
						  {3, value.object_entity},
						  {9, value.object_expression},
						  {2, value.object_source}}})
					if (!binding.second.empty())
					{
						const auto state = reference(
							binding.first, binding.second, scope, value.evidence, value.gaps);
						if (state != finite_population_state::complete)
							downgrade(value.object_state,
									  state == finite_population_state::conflicting);
					}
				if (!value.object_declaration.empty())
				{
					const auto& declarations = find(5, key(value.object_declaration, scope));
					if (equal(declarations) && !value.object_entity.empty() &&
						text(row(declarations.front()), "entity") != value.object_entity)
						downgrade(value.object_state, true);
				}
				if (value.object_state == finite_population_state::complete &&
					value.object_declaration.empty() && value.object_entity.empty() &&
					value.object_expression.empty() && value.type_use_kind != "return_type")
					value.object_state = finite_population_state::conflicting;
				if (!value.object_source.empty())
				{
					function_action_population bound;
					bound.universe = scope.universe;
					bound.variant = scope.variant;
					bound.interpretation = scope.interpretation;
					bound.source_span = value.object_source;
					bound.state = finite_population_state::complete;
					source(bound, value.operation);
					b.bind(value.evidence, bound.evidence);
					value.gaps.insert(value.gaps.end(), bound.gaps.begin(), bound.gaps.end());
					if (bound.state != finite_population_state::complete)
						downgrade(value.object_state,
								  bound.state == finite_population_state::conflicting);
				}
				// Classification axes do not erase an independently enumerated action.
				canonical(value.gaps);
				return value;
			}
			void admission(function_action_population& scope, const annotated_row& detail)
			{
				using admission_key = std::array<std::string, 3>;
				std::map<admission_key, std::vector<const observed_function_action*>> actual;
				std::map<std::string, std::set<std::uint64_t>> slots;
				std::map<std::string, std::set<std::string>> implicit;
				std::map<std::pair<std::string, std::uint64_t>, observed_function_action*>
					placements;
				for (auto& value : scope.actions)
				{
					b.work();
					b.retain(sizeof(admission_key) + value.expression.size() +
							 value.object_declaration.size() + 256);
					if (value.origin == "ast")
						actual[{"ast", value.expression, value.kind}].push_back(&value);
					if (value.origin == "declaration" && value.kind == "type_use")
					{
						actual[{"declaration",
								value.type_use_kind == "return_type" ? scope.declaration
																	 : value.object_declaration,
								value.type_use_kind}]
							.push_back(&value);
						if (value.subject_index)
							slots[value.type_use_kind].insert(*value.subject_index);
					}
					constexpr std::array implicit_kinds{"constructor",
														"new_allocator",
														"automatic_object_dtor",
														"delete_dtor",
														"member_dtor",
														"base_dtor",
														"temporary_dtor",
														"cleanup_function"};
					if (value.origin == "cfg" &&
						std::ranges::find(implicit_kinds, value.element_kind) !=
							implicit_kinds.end())
						implicit[value.node].insert(value.operation);
					if (value.origin == "cfg" && value.element_index)
					{
						const auto [position, inserted] =
							placements.emplace(std::pair{value.node, *value.element_index}, &value);
						if (!inserted)
						{
							value.cfg_state = finite_population_state::conflicting;
							position->second->cfg_state = finite_population_state::conflicting;
							gap(value, value.operation, "cfg-element-occurrence-conflicting", true);
							gap(*position->second,
								position->second->operation,
								"cfg-element-occurrence-conflicting",
								true);
						}
					}
				}
				std::set<admission_key> expected;
				std::set<std::string> uncertain_roles;
				bool uncertain_syntax{};
				const auto expect = [&](admission_key observed, std::string_view subject)
				{
					b.work();
					b.retain(sizeof(observed) + observed[0].size() + observed[1].size() +
							 observed[2].size() + 128);
					expected.insert(observed);
					const auto found = actual.find(observed);
					if (found == actual.end() || found->second.empty())
						gap(scope, subject, "admitted-occurrence-unavailable");
					else if (found->second.size() != 1)
						gap(scope, subject, "admitted-occurrence-conflicting", true);
				};
				std::set<std::string> seen_syntax;
				for (auto i : indexed(function_syntax,
									  key(scope.compile_unit + '\n' + scope.function, scope)))
				{
					b.work();
					const auto& syntax = row(i);
					const auto id = text(syntax, "node");
					if (!seen_syntax.insert(id).second)
						continue;
					const auto& original = find(9, key(id, scope));
					b.bind(scope.evidence, original);
					if (!equal(original))
					{
						gap(scope, id, "admission-syntax-conflicting", true);
						continue;
					}
					const auto inside =
						contains(scope, text(syntax, "source"), scope.evidence, scope.gaps);
					if (!inside)
					{
						gap(scope, id, "admission-source-unavailable");
						uncertain_syntax = true;
						continue;
					}
					if (!*inside)
						continue; // Actual other physical redeclaration, not this scope.
					const auto flags = strings(syntax, "flags");
					if (!std::ranges::binary_search(flags, "finite_operation_admission_v1"))
					{
						gap(scope, id, "syntax-admission-unavailable");
						uncertain_syntax = true;
					}
					for (const auto& flag : flags)
					{
						b.work();
						if (flag.starts_with("operation_"))
							expect({"ast", id, flag.substr(10)}, id);
					}
				}
				const auto flags = strings(detail, "flags");
				if (!std::ranges::binary_search(flags, "finite_type_use_admission_v1"))
				{
					gap(scope, scope.function, "declaration-admission-unavailable");
					uncertain_roles.insert("return_type");
					uncertain_roles.insert("parameter");
					uncertain_roles.insert("local");
				}
				if (std::ranges::binary_search(flags, "type_use_return_type"))
				{
					expect({"declaration", scope.declaration, "return_type"}, scope.declaration);
					if (slots["return_type"] != std::set<std::uint64_t>{0})
						gap(scope, scope.declaration, "return-slot-conflicting", true);
				}
				std::set<std::string> seen_details;
				for (auto i : indexed(function_details,
									  key(scope.compile_unit + '\n' + scope.function, scope)))
				{
					b.work();
					const auto& original_detail = row(i);
					const auto id = text(original_detail, "detail");
					if (!seen_details.insert(id).second)
						continue;
					const auto& original = find(4, key(id, scope));
					b.bind(scope.evidence, original);
					if (!equal(original))
					{
						gap(scope, id, "type-admission-detail-conflicting", true);
						continue;
					}
					const auto inside = contains(
						scope, text(original_detail, "source"), scope.evidence, scope.gaps);
					if (!inside)
					{
						gap(scope, id, "type-admission-source-unavailable");
						continue;
					}
					if (!*inside)
						continue;
					const auto child_flags = strings(original_detail, "flags");
					if (!std::ranges::binary_search(child_flags, "finite_type_use_admission_v1"))
					{
						gap(scope, id, "type-admission-unavailable");
						uncertain_roles.insert("parameter");
						uncertain_roles.insert("local");
					}
					for (auto role : {"parameter", "local"})
						if (std::ranges::binary_search(child_flags,
													   "type_use_" + std::string{role}))
						{
							const auto& declarations =
								indexed(declaration_sources,
										key(text(original_detail, "entity") + '\n' +
												text(original_detail, "source"),
											scope));
							b.bind(scope.evidence, declarations);
							std::set<std::string> ids;
							for (auto ref : declarations)
							{
								b.work();
								ids.insert(text(row(ref), "declaration"));
							}
							if (ids.size() != 1)
							{
								gap(scope,
									id,
									"type-admission-declaration-unavailable",
									ids.size() > 1);
								uncertain_roles.insert(role);
							}
							else
								expect({"declaration", *ids.begin(), role}, *ids.begin());
						}
				}
				const auto check_slots =
					[&](std::string_view role, std::optional<std::uint64_t> count)
				{
					if (!count)
					{
						gap(scope, scope.function, "subject-count-unavailable");
						return;
					}
					const auto& observed = slots[std::string{role}];
					if (observed.size() != *count)
					{
						gap(scope, scope.function, "subject-slots-unavailable");
						return;
					}
					std::uint64_t index{};
					for (auto slot : observed)
					{
						b.work();
						if (slot != index++)
							gap(scope, scope.function, "subject-slot-conflicting", true);
					}
					std::size_t occurrence_count{};
					for (const auto& value : scope.actions)
					{
						b.work();
						if (value.origin == "declaration" && value.kind == "type_use" &&
							value.type_use_kind == role)
						{
							++occurrence_count;
							if (!value.subject_index || *value.subject_index >= *count)
								gap(scope, value.operation, "subject-slot-conflicting", true);
						}
					}
					if (occurrence_count != *count)
						gap(scope, scope.function, "subject-occurrences-conflicting", true);
				};
				check_slots("parameter", number(detail, "parameter_count"));
				check_slots("capture", number(detail, "capture_count"));
				for (const auto& [admitted, occurrences] : actual)
				{
					b.work();
					if (admitted[0] == "declaration" && admitted[2] == "capture")
						continue; // Independent original capture cardinality/slots above.
					if (admitted[0] == "declaration" && admitted[1].empty() &&
						admitted[2] == "parameter" &&
						std::ranges::all_of(occurrences,
											[](const auto* occurrence)
											{
												return occurrence->object_entity.empty();
											}))
						continue; // Original unnamed parameter cardinality/slots are
								  // independent.
					if (admitted[0] == "declaration" &&
						(admitted[2] == "parameter" || admitted[2] == "local") &&
						!expected.contains(admitted))
					{
						bool owner_frontier = true;
						for (const auto* occurrence : occurrences)
						{
							b.work();
							const auto& entities = find(3, key(occurrence->object_entity, scope));
							const auto& declarations = find(5, key(admitted[1], scope));
							b.bind(scope.evidence, entities);
							b.bind(scope.evidence, declarations);
							if (!equal(entities) || !equal(declarations) ||
								text(row(declarations.front()), "entity") !=
									occurrence->object_entity)
							{
								owner_frontier = false;
								break;
							}
							const auto& details =
								indexed(detail_sources,
										key(occurrence->object_entity + '\n' +
												text(row(declarations.front()), "source"),
											scope));
							b.bind(scope.evidence, details);
							if (!equal(details) ||
								text(row(details.front()), "compile_unit") != scope.compile_unit)
							{
								owner_frontier = false;
								break;
							}
							const auto detail_flags = strings(row(details.front()), "flags");
							if (!std::ranges::binary_search(detail_flags,
															"finite_type_use_admission_v1") ||
								!std::ranges::binary_search(detail_flags,
															"type_use_" + admitted[2]))
							{
								owner_frontier = false;
								break;
							}
							const auto semantic_owner =
								text(row(entities.front()), "semantic_owner");
							if (semantic_owner == scope.function)
							{
								owner_frontier = false;
								break;
							}
							gap(scope,
								occurrence->operation,
								semantic_owner.empty() ? "type-admission-owner-unavailable"
													   : "type-admission-owner-conflicting",
								!semantic_owner.empty());
						}
						if (owner_frontier)
							continue;
					}
					if (!expected.contains(admitted) &&
						!(admitted[0] == "ast" && uncertain_syntax) &&
						!(admitted[0] == "declaration" && uncertain_roles.contains(admitted[2])))
						gap(scope,
							occurrences.front()->operation,
							"occurrence-outside-admission",
							true);
				}
				if (!scope.body.empty())
				{
					const auto& bodies = find(8, key(scope.body, scope));
					b.bind(scope.evidence, bodies);
					if (!equal(bodies))
					{
						gap(scope, scope.body, "body-unavailable", !bodies.empty());
						return;
					}
					const auto& body = row(bodies.front());
					if (text(body, "eligibility") != "closed")
						gap(scope, scope.body, "body-cfg-unavailable");
					if (text(body, "compile_unit") != scope.compile_unit ||
						text(body, "function") != scope.function)
						gap(scope, scope.body, "body-owner-conflicting", true);
					for (auto name : {"operation_count",
									  "operation_ids",
									  "operation_state",
									  "operation_profile"})
						if (!present(body, name))
							gap(scope, scope.body, "body-operation-facet-unavailable");
					if ((present(body, "operation_count") &&
						 number(body, "operation_count") != scope.operation_count) ||
						(present(body, "operation_ids") &&
						 strings(body, "operation_ids") != scope.operation_ids) ||
						(present(body, "operation_state") &&
						 text(body, "operation_state") != text(detail, "operation_state")) ||
						(present(body, "operation_profile") &&
						 text(body, "operation_profile") != scope.profile))
						gap(scope, scope.body, "body-operation-facet-conflicting", true);
					std::set<std::string> observed_nodes;
					for (auto i : indexed(body_nodes, key(scope.body, scope)))
					{
						b.work();
						const auto& node = row(i);
						const auto id = text(node, "node");
						if (!observed_nodes.insert(id).second)
							continue;
						const auto& original = find(10, key(id, scope));
						b.bind(scope.evidence, original);
						if (!equal(original))
						{
							gap(scope, id, "cfg-admission-conflicting", true);
							continue;
						}
						if (text(node, "compile_unit") != scope.compile_unit ||
							text(node, "function") != scope.function)
							gap(scope, id, "cfg-admission-owner-conflicting", true);
						const auto count = number(node, "implicit_operation_count");
						if (!count || text(node, "implicit_operation_state") != "complete" ||
							text(node, "implicit_operation_profile") != action_profile)
							gap(scope, id, "cfg-implicit-admission-unavailable");
						if (count && *count != implicit[id].size())
							gap(scope, id, "cfg-implicit-occurrence-unavailable");
					}
					const auto count = number(body, "node_count");
					if (!count || *count != observed_nodes.size())
						gap(scope, scope.body, "cfg-node-population-unavailable");
				}
			}
			void scopes()
			{
				using scope_key = std::array<std::string, 6>;
				std::map<scope_key, refs> scopes_by_source;
				for (const auto& [id, r] : maps[4])
				{
					for (auto i : r)
					{
						b.work();
						const auto& detail = row(i);
						const auto function = text(detail, "entity");
						const auto& entities = find(3, {function, id[1], id[2], id[3]});
						const auto flags = strings(detail, "flags");
						const auto kind =
							entities.empty() ? std::string{} : text(row(entities.front()), "kind");
						const bool callable = kind == "function" || kind == "method" ||
							kind == "constructor" || kind == "destructor" || kind == "conversion";
						if (!present(detail, "operation_profile") &&
							(!callable ||
							 !std::ranges::binary_search(flags, "declaration_population_admitted")))
							continue;
						b.retain(sizeof(scope_key) + payloads[i] + 128);
						scopes_by_source[{function,
										  text(detail, "compile_unit"),
										  text(detail, "source"),
										  id[1],
										  id[2],
										  id[3]}]
							.push_back(i);
					}
				}
				std::set<identity> bound_operations, bound_declarations;
				for (const auto& [owner, r] : scopes_by_source)
				{
					b.work();
					if (output.populations.size() >= b.limits.maximum_populations)
						fail("populations", "limit-exceeded", "sdk.action-budget");
					const auto& detail = row(r.front());
					function_action_population value;
					b.retain(sizeof(value) + payloads[r.front()]);
					value.function = owner[0];
					value.compile_unit = owner[1];
					value.source_span = owner[2];
					value.universe = owner[3];
					value.variant = owner[4];
					value.interpretation = owner[5];
					value.profile = text(detail, "operation_profile");
					value.operation_count = number(detail, "operation_count");
					value.operation_ids = strings(detail, "operation_ids");
					value.state = text(detail, "operation_state") == "complete" &&
							value.operation_count && present(detail, "operation_ids") &&
							value.profile == action_profile && input.compile_units_complete &&
							input.scope_inputs_complete && input.operation_inputs_complete &&
							input.admission_inputs_complete
						? finite_population_state::complete
						: finite_population_state::unknown;
					b.bind(value.evidence, r);
					if (!equal(r))
						gap(value, value.function, "scope-conflicting", true);
					if (value.state == finite_population_state::unknown)
						gap(value, value.function, "scope-unavailable");
					const auto& units = find(0, key(value.compile_unit, value));
					b.bind(value.evidence, units);
					if (!equal(units))
						gap(value, value.compile_unit, "unit-unavailable", !units.empty());
					const auto& entities = find(3, key(value.function, value));
					b.bind(value.evidence, entities);
					if (!equal(entities))
						gap(value, value.function, "function-unavailable", !entities.empty());
					source(value, value.function);
					const auto& declarations = indexed(
						declaration_sources, key(value.function + '\n' + value.source_span, value));
					b.bind(value.evidence, declarations);
					std::set<std::string> declaration_ids;
					for (auto i : declarations)
					{
						b.work();
						declaration_ids.insert(text(row(i), "declaration"));
					}
					if (declaration_ids.size() != 1)
						gap(value,
							value.function,
							"scope-declaration-unavailable",
							declaration_ids.size() > 1);
					else
					{
						value.declaration = *declaration_ids.begin();
						bound_declarations.insert(key(value.declaration, value));
						if (!equal(find(5, key(value.declaration, value))))
							gap(value, value.declaration, "scope-declaration-conflicting", true);
					}
					const auto flags = strings(detail, "flags");
					if (std::ranges::binary_search(flags, "finite_function_body_v1") &&
						std::ranges::binary_search(flags, "body_written"))
					{
						std::set<std::string> ids;
						for (auto i :
							 indexed(function_bodies,
									 key(value.compile_unit + '\n' + value.function, value)))
						{
							b.work();
							const auto inside =
								contains(value, text(row(i), "source"), value.evidence, value.gaps);
							if (!inside)
								gap(value, value.function, "body-source-unavailable");
							else if (*inside)
								ids.insert(text(row(i), "body"));
						}
						if (ids.size() != 1)
							gap(value, value.function, "written-body-unavailable", ids.size() > 1);
						else
							value.body = *ids.begin();
					}
					else if (!std::ranges::binary_search(flags, "finite_function_body_v1") ||
							 !std::ranges::binary_search(flags, "body_absent"))
						gap(value, value.function, "body-eligibility-unavailable");
					std::set<std::string> actual_ids;
					for (auto i :
						 indexed(scope_operations,
								 key(value.compile_unit + '\n' + value.declaration, value)))
					{
						b.work();
						const auto id = text(row(i), "operation");
						if (!actual_ids.insert(id).second)
							continue;
						const auto operation_key = key(id, value);
						bound_operations.insert(operation_key);
						b.charge(b.members, 1, b.limits.maximum_members, "actions");
						value.actions.push_back(action(find(12, operation_key), value));
					}
					if (value.operation_count &&
						*value.operation_count != value.operation_ids.size())
						gap(value, value.declaration, "inventory-count-conflicting", true);
					for (const auto& id : value.operation_ids)
					{
						b.work();
						if (!actual_ids.contains(id))
							gap(value, id, "inventory-member-unavailable");
					}
					for (const auto& id : actual_ids)
					{
						b.work();
						if (!std::ranges::binary_search(value.operation_ids, id))
							gap(value, id, "occurrence-outside-inventory", true);
					}
					admission(value, detail);
					value.enumeration_state = value.state;
					for (const auto& observed : value.actions)
						if (observed.state != finite_population_state::complete)
							gap(value,
								observed.operation,
								"occurrence-incomplete",
								observed.state == finite_population_state::conflicting);
					canonical(value.gaps);
					output.populations.push_back(std::move(value));
				}
				// A surviving written function declaration without its original detail
				// keeps an unknown scope. Do not infer a compile unit from physical source
				// proximity.
				for (const auto& [id, r] : maps[5])
				{
					b.work();
					if (bound_declarations.contains(id))
						continue;
					const auto& declaration = row(r.front());
					const auto& entities =
						find(3, {text(declaration, "entity"), id[1], id[2], id[3]});
					if (entities.empty())
						continue;
					const auto kind = text(row(entities.front()), "kind");
					if (kind != "function" && kind != "method" && kind != "constructor" &&
						kind != "destructor" && kind != "conversion")
						continue;
					// Observed intentionally unadmitted compiler declarations are not new
					// function-action scopes. Missing details cannot establish that
					// exclusion.
					if (!indexed(detail_sources,
								 {text(declaration, "entity") + '\n' + text(declaration, "source"),
								  id[1],
								  id[2],
								  id[3]})
							 .empty())
						continue;
					if (output.populations.size() >= b.limits.maximum_populations)
						fail("populations", "limit-exceeded", "sdk.action-budget");
					function_action_population value;
					b.retain(sizeof(value) + payloads[r.front()]);
					value.declaration = id[0];
					value.function = text(declaration, "entity");
					value.source_span = text(declaration, "source");
					value.universe = id[1];
					value.variant = id[2];
					value.interpretation = id[3];
					b.bind(value.evidence, r);
					b.bind(value.evidence, entities);
					source(value, value.declaration);
					gap(value, value.declaration, "owning-detail-unavailable");
					output.populations.push_back(std::move(value));
				}
				// Keep original actions whose owning detail/declaration was not observed.
				// Their absence cannot turn a selected unit into a known empty action
				// population.
				for (const auto& [id, r] : maps[12])
				{
					b.work();
					if (bound_operations.contains(id))
						continue;
					if (output.populations.size() >= b.limits.maximum_populations)
						fail("populations", "limit-exceeded", "sdk.action-budget");
					function_action_population value;
					const auto& native = row(r.front());
					value.declaration = text(native, "scope_declaration");
					value.function = text(native, "function");
					value.compile_unit = text(native, "compile_unit");
					value.body = text(native, "body");
					value.universe = id[1];
					value.variant = id[2];
					value.interpretation = id[3];
					gap(value, value.declaration, "owning-scope-unavailable");
					b.bind(value.evidence, r);
					value.actions.push_back(action(r, value));
					output.populations.push_back(std::move(value));
				}
				for (auto& value : output.populations)
					canonical(value.gaps);
				std::ranges::sort(output.populations,
								  {},
								  [](const auto& value)
								  {
									  return std::tie(value.compile_unit,
													  value.function,
													  value.declaration,
													  value.source_span,
													  value.universe,
													  value.variant,
													  value.interpretation);
								  });
			}
		};
		void normalize(function_action_projection& output)
		{
			const auto unique = [](auto& values)
			{
				std::ranges::sort(values);
				values.erase(std::ranges::unique(values).begin(), values.end());
			};
			for (auto& population : output.populations)
			{
				unique(population.evidence);
				for (auto& action : population.actions)
				{
					unique(action.evidence);
					unique(action.target_signature.evidence);
					canonical(action.target_signature.gaps);
				}
				std::ranges::sort(population.actions,
								  {},
								  [](const auto& action)
								  {
									  return std::tie(action.operation, action.ordinal);
								  });
			}
			canonical(output.unresolved);
		}
		result<function_action_projection>
		project_rows(function_action_input input,
					 finite_population_limits limits,
					 std::stop_token stop,
					 budget& b,
					 const std::array<std::vector<const annotated_row*>, 13>* borrowed = nullptr)
		{
			if (auto valid = limits.validate(); !valid)
				return valid.error();
			try
			{
				(void)stop;
				b.work();
				projection work{b, input};
				work.output.compile_units_complete = input.compile_units_complete;
				work.output.scope_inputs_complete = input.scope_inputs_complete;
				work.output.operation_inputs_complete = input.operation_inputs_complete;
				work.output.admission_inputs_complete = input.admission_inputs_complete;
				work.output.type_inputs_complete = input.type_inputs_complete;
				work.output.call_inputs_complete = input.call_inputs_complete;
				const std::array groups{input.units,
										input.files,
										input.spans,
										input.entities,
										input.details,
										input.declarations,
										input.types,
										input.type_components,
										input.bodies,
										input.syntax_nodes,
										input.cfg_nodes,
										input.call_sites,
										input.operations};
				const auto visit_group = [&](std::size_t group, auto&& visit)
				{
					if (borrowed)
					{
						for (const auto* row : (*borrowed)[group])
							visit(*row);
					}
					else
					{
						for (const auto& row : groups[group])
							visit(row);
					}
				};
				// The original source scan can contain every lexical token span.
				// Retain referenced carriers only, after validating all source rows.
				using source_key = std::array<std::string_view, 4>;
				std::set<source_key> referenced_syntax;
				struct syntax_selection
				{
					const annotated_row* original{};
					bool conflicting{};
				};
				std::map<source_key, syntax_selection> syntax_rows;
				const auto same_payload = [&](const annotated_row& left, const annotated_row& right)
				{
					if (left.values.size() != right.values.size())
						return false;
					return std::ranges::equal(
						left.values,
						right.values,
						[&](const auto& x, const auto& y)
						{
							const auto charge_cell = [&](const auto& value)
							{
								b.work(value.first.size() + value.second.type.parameter.size() +
									   1U);
								if (value.second.unknown_reason)
									b.work(value.second.unknown_reason->size());
								if (value.second.value)
								{
									if (const auto* text =
											std::get_if<std::string>(&*value.second.value))
										b.work(text->size());
									if (const auto* bytes = std::get_if<std::vector<std::byte>>(
											&*value.second.value))
										b.work(bytes->size());
								}
							};
							charge_cell(x);
							charge_cell(y);
							return x.first == y.first &&
								x.second.type.scalar == y.second.type.scalar &&
								x.second.type.parameter == y.second.type.parameter &&
								x.second.type.optional == y.second.type.optional &&
								x.second.state == y.second.state &&
								x.second.unknown_reason == y.second.unknown_reason &&
								x.second.value == y.second.value;
						});
				};
				const auto each_key =
					[&](const annotated_row& row, std::string_view name, auto&& use)
				{
					const auto id = borrowed_text(row, name);
					if (id.empty())
						return;
					for (const auto& variant : row.presence.fragments)
					{
						b.work();
						use(source_key{id, row.presence.universe, variant, row.interpretation});
					}
				};
				for (const auto group : {11U, 12U})
					visit_group(group,
								[&](const annotated_row& row)
								{
									for (const auto field : {"expression", "object_expression"})
										each_key(row,
												 field,
												 [&](const source_key& id)
												 {
													 if (!referenced_syntax.contains(id))
													 {
														 b.retain(sizeof(source_key) + 96U);
														 referenced_syntax.insert(id);
													 }
												 });
								});
				visit_group(9U,
							[&](const annotated_row& row)
							{
								each_key(row,
										 "node",
										 [&](const source_key& id)
										 {
											 const auto found = syntax_rows.find(id);
											 if (found == syntax_rows.end())
											 {
												 b.retain(sizeof(source_key) +
														  sizeof(syntax_selection) + 96U);
												 syntax_rows.emplace(id,
																	 syntax_selection{&row, false});
											 }
											 else if (!found->second.conflicting &&
													  !same_payload(row, *found->second.original))
												 found->second.conflicting = true;
										 });
							});
				const auto explicit_nonaction = [&](const annotated_row& row)
				{
					const auto* c = cell(row, "flags");
					const auto* encoded = c && c->state == cell_state::present && c->value
						? std::get_if<std::vector<std::byte>>(&*c->value)
						: nullptr;
					if (!encoded)
						return false;
					b.work(encoded->size() + 1U);
					bool marker = false;
					for (std::size_t offset = 0; offset < encoded->size();)
					{
						if (encoded->size() - offset < 4U)
							return false;
						std::uint32_t n = 0;
						for (unsigned i = 0; i < 4U; ++i)
							n |= std::to_integer<std::uint32_t>((*encoded)[offset + i]) << (i * 8U);
						offset += 4U;
						if (n > encoded->size() - offset)
							return false;
						const std::string_view flag{
							reinterpret_cast<const char*>(encoded->data() + offset), n};
						offset += n;
						marker |= flag == "finite_operation_admission_v1";
						if (flag.starts_with("operation_"))
							return false;
					}
					return marker;
				};
				const auto retain_syntax = [&](const annotated_row& row)
				{
					if (!explicit_nonaction(row))
						return true;
					bool retain = false;
					each_key(row,
							 "node",
							 [&](const source_key& id)
							 {
								 if (referenced_syntax.contains(id))
								 {
									 retain = true;
									 return;
								 }
								 const auto at = syntax_rows.find(id);
								 if (at == syntax_rows.end())
								 {
									 retain = true;
									 return;
								 }
								 retain |= at->second.conflicting;
							 });
					return retain;
				};
				std::set<source_key> needed_details, needed_declarations, needed_types;
				const auto add_needed =
					[&](auto& into, const annotated_row& row, std::string_view field)
				{
					each_key(row,
							 field,
							 [&](const source_key& id)
							 {
								 if (!into.contains(id))
								 {
									 b.retain(sizeof(source_key) + 96U);
									 into.insert(id);
								 }
							 });
				};
				const auto needed =
					[&](const auto& into, const annotated_row& row, std::string_view field)
				{
					bool found{};
					each_key(row,
							 field,
							 [&](const source_key& id)
							 {
								 found |= into.contains(id);
							 });
					return found;
				};
				const auto callable_kind = [](std::string_view kind)
				{
					return kind.empty() || kind == "function" || kind == "method" ||
						kind == "constructor" || kind == "destructor" || kind == "conversion";
				};
				visit_group(3U,
							[&](const annotated_row& row)
							{
								b.work();
								if (callable_kind(borrowed_text(row, "kind")))
									add_needed(needed_details, row, "entity");
							});
				// A declared type-use admission without an independently known empty
				// role set remains selected. Actual action object/context joins also
				// retain their original declaration/detail carriers.
				const auto explicit_no_type_use = [&](const annotated_row& row)
				{
					const auto* c = cell(row, "flags");
					const auto* encoded = c && c->state == cell_state::present && c->value
						? std::get_if<std::vector<std::byte>>(&*c->value)
						: nullptr;
					if (!encoded)
						return false;
					b.work(encoded->size() + 1U);
					bool marker{};
					for (std::size_t offset{}; offset < encoded->size();)
					{
						if (encoded->size() - offset < 4U)
							return false;
						std::uint32_t n{};
						for (unsigned i{}; i < 4U; ++i)
							n |= std::to_integer<std::uint32_t>((*encoded)[offset + i]) << (8U * i);
						offset += 4U;
						if (n > encoded->size() - offset)
							return false;
						const std::string_view flag{
							reinterpret_cast<const char*>(encoded->data() + offset), n};
						offset += n;
						marker |= flag == "finite_type_use_admission_v1";
						if (flag.starts_with("type_use_"))
							return false;
					}
					return marker;
				};
				visit_group(4U,
							[&](const annotated_row& row)
							{
								if (!explicit_no_type_use(row) || present(row, "operation_count") ||
									present(row, "operation_ids"))
									add_needed(needed_details, row, "entity");
							});
				for (const auto group : {11U, 12U})
					visit_group(
						group,
						[&](const annotated_row& row)
						{
							for (const auto field : {"function", "caller", "object_entity"})
								add_needed(needed_details, row, field);
							for (const auto field :
								 {"scope_declaration", "object_declaration", "context_declaration"})
								add_needed(needed_declarations, row, field);
							for (const auto field :
								 {"type", "object_type", "target_canonical_type"})
								add_needed(needed_types, row, field);
						});
				visit_group(5U,
							[&](const annotated_row& row)
							{
								if (needed(needed_declarations, row, "declaration"))
									add_needed(needed_details, row, "entity");
							});
				visit_group(4U,
							[&](const annotated_row& row)
							{
								if (needed(needed_details, row, "entity"))
									add_needed(needed_types, row, "type");
							});
				bool changed = true;
				while (changed)
				{
					const auto before = needed_types.size();
					visit_group(7U,
								[&](const annotated_row& row)
								{
									b.work();
									if (needed(needed_types, row, "owner_type"))
										add_needed(needed_types, row, "component_type");
								});
					changed = before != needed_types.size();
				}
				const auto retain_carrier = [&](std::size_t group, const annotated_row& row)
				{
					if (group == 4U)
						return needed(needed_details, row, "entity");
					if (group == 5U)
						return needed(needed_details, row, "entity") ||
							needed(needed_declarations, row, "declaration");
					if (group == 6U)
						return needed(needed_types, row, "type");
					if (group == 7U)
						return needed(needed_types, row, "owner_type");
					return true;
				};
				std::set<source_key> source_members;
				for (const auto group : {4U, 5U, 8U, 9U, 10U, 11U, 12U})
					visit_group(
						group,
						[&](const annotated_row& row)
						{
							if ((group == 9U && !retain_syntax(row)) || !retain_carrier(group, row))
								return;
							for (const auto field : {"source", "object_source"})
							{
								b.work();
								const auto id = borrowed_text(row, field);
								if (id.empty())
									continue;
								for (const auto& variant : row.presence.fragments)
								{
									b.work();
									const source_key member{
										id, row.presence.universe, variant, row.interpretation};
									if (!source_members.contains(member))
									{
										b.retain(sizeof(source_key) + 96U);
										source_members.insert(member);
									}
								}
							}
						});
				const auto descriptors = standard_relation_descriptors();
				std::vector<entry> entries;
				for (std::size_t group = 0; group < groups.size(); ++group)
				{
					const auto descriptor =
						std::ranges::find(descriptors, relations[group], &relation_descriptor::id);
					if (descriptor == descriptors.end())
						fail(relations[group], "descriptor-missing");
					visit_group(
						group,
						[&](const annotated_row& row)
						{
							b.work();
							b.charge(b.rows, 1, limits.maximum_rows, "rows");
							if (auto valid = detail::validate_projected_relation_row(
									row,
									*descriptor,
									"sdk.action-input-invalid",
									[&]
									{
										b.work();
									});
								!valid)
								throw failure{valid.error()};
							b.charge(b.conditions,
									 row.presence.fragments.size(),
									 limits.maximum_condition_expansions,
									 "conditions");
							if (borrowed_text(row, identifiers[group]).empty())
								fail(relations[group], "identity-missing");
							if ((group == 9U && !retain_syntax(row)) || !retain_carrier(group, row))
								return;
							if (group == 2U)
							{
								const auto id = borrowed_text(row, "span");
								if (!std::ranges::any_of(row.presence.fragments,
														 [&](const auto& variant)
														 {
															 b.work();
															 return source_members.contains(
																 source_key{id,
																			row.presence.universe,
																			variant,
																			row.interpretation});
														 }))
									return;
							}
							// Retain the detached original row once. Equality below compares
							// original typed cells, so no serialized payload copy is needed.
							b.retain(b.estimate(row, false));
							const auto temporary = b.estimate(row);
							if (temporary > (limits.maximum_retained_bytes - b.retained) / 2U)
								fail("canonical-temporary", "limit-exceeded", "sdk.action-budget");
							std::size_t payload_bytes = 128U;
							for (const auto& [name, value] : row.values)
							{
								b.work();
								payload_bytes += name.size() + value.type.parameter.size() + 128U;
								if (value.unknown_reason)
									payload_bytes += value.unknown_reason->size() * 2U;
								if (value.value)
								{
									if (const auto* text = std::get_if<std::string>(&*value.value))
										payload_bytes += text->size() * 2U;
									if (const auto* bytes =
											std::get_if<std::vector<std::byte>>(&*value.value))
										payload_bytes += bytes->size() * 2U;
								}
							}
							auto canonical = row.canonical_form();
							b.retain(canonical.size() * 2U + sizeof(entry));
							b.charge(b.evidence,
									 canonical.size(),
									 limits.maximum_evidence_bytes,
									 "evidence-bytes");
							entries.push_back({group, &row, std::move(canonical), payload_bytes});
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
					const auto i = work.output.evidence.size();
					work.output.evidence.push_back({std::string{relations[e.group]}, *e.row});
					work.payloads.push_back(e.payload_bytes);
					const auto id = text(*e.row, identifiers[e.group]);
					if (id.empty())
						fail(relations[e.group], "identity-missing");
					for (const auto& variant : e.row->presence.fragments)
					{
						b.work();
						b.retain(id.size() + variant.size() + e.row->presence.universe.size() +
								 e.row->interpretation.size() + 768);
						const auto owner = key(id, *e.row, variant);
						work.maps[e.group][owner].push_back(i);
						if (e.group == 4)
							work.detail_sources[key(text(*e.row, "entity") + '\n' +
														text(*e.row, "source"),
													*e.row,
													variant)]
								.push_back(i);
						if (e.group == 5)
							work.declaration_sources[key(text(*e.row, "entity") + '\n' +
															 text(*e.row, "source"),
														 *e.row,
														 variant)]
								.push_back(i);
						if (e.group == 7)
							work.type_members[key(text(*e.row, "owner_type"), *e.row, variant)]
								.push_back(i);
						if (e.group == 8)
							work.function_bodies[key(text(*e.row, "compile_unit") + '\n' +
														 text(*e.row, "function"),
													 *e.row,
													 variant)]
								.push_back(i);
						if (e.group == 9)
							work.function_syntax[key(text(*e.row, "compile_unit") + '\n' +
														 text(*e.row, "function"),
													 *e.row,
													 variant)]
								.push_back(i);
						if (e.group == 10)
							work.body_nodes[key(text(*e.row, "body"), *e.row, variant)].push_back(
								i);
						if (e.group == 12)
						{
							work.scope_operations[key(text(*e.row, "compile_unit") + '\n' +
														  text(*e.row, "scope_declaration"),
													  *e.row,
													  variant)]
								.push_back(i);
							if (!text(*e.row, "site").empty())
								work.sites[key(text(*e.row, "site"), *e.row, variant)].push_back(i);
						}
					}
				}
				for (const auto& [id, r] : work.maps[4])
					for (auto i : r)
					{
						b.work();
						const auto& native = work.row(i);
						const auto& entities =
							work.find(3, {text(native, "entity"), id[1], id[2], id[3]});
						if (!work.equal(entities))
							continue;
						const auto semantic_owner =
							text(work.row(entities.front()), "semantic_owner");
						if (!semantic_owner.empty())
						{
							b.retain(semantic_owner.size() + text(native, "compile_unit").size() +
									 512);
							work.function_details[{text(native, "compile_unit") + '\n' +
													   semantic_owner,
												   id[1],
												   id[2],
												   id[3]}]
								.push_back(i);
						}
					}
				work.scopes();
				normalize(work.output);
				return std::move(work.output);
			}
			catch (const failure& exception)
			{
				return exception.value;
			}
			catch (const std::bad_alloc&)
			{
				return error{"sdk.action-resource-exhausted", "projection", "allocation"};
			}
			catch (const std::length_error&)
			{
				return error{"sdk.action-resource-exhausted", "projection", "length"};
			}
		}
		result<function_action_projection> project_queries(const application_query_results& input,
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
																  "sdk.action",
																  &plan_bytes,
																  cancelled);
					!valid)
					return valid.error();
				b.retain(plan_bytes);
				std::array<std::vector<const annotated_row*>, 13> groups;
				std::array<bool, 13> present{}, complete{};
				complete.fill(true);
				for (const auto& scan : input.scans)
				{
					b.work();
					const auto name = std::ranges::find(relations, scan.relation_id);
					if (name == relations.end())
						continue;
					const auto group = static_cast<std::size_t>(name - relations.begin());
					present[group] = true;
					// Successful original scans establish availability. The independently
					// checked typed scope/action admission closes this domain; unrelated
					// semantic inputs_complete frontiers remain in the original
					// source_queries.
					complete[group] &= scan.result.execution() == execution_status::complete &&
						scan.result.conflicts().empty() &&
						scan.result.differential_disagreements().empty();

					const auto rows = query_transfer_access::borrow_rows(scan.result);
					b.charge(b.rows, rows.size(), limits.maximum_rows, "scan-rows");
					if (rows.size() > limits.maximum_rows ||
						groups[group].size() > limits.maximum_rows - rows.size())
						fail("rows", "limit-exceeded", "sdk.action-budget");
					// Account for the temporary pointer array before allocating it;
					// original rows remain owned by the immutable query handle.
					b.retain((groups[group].size() + rows.size()) * sizeof(const annotated_row*));
					groups[group].reserve(groups[group].size() + rows.size());
					for (const auto& row : rows)
					{
						b.work();
						groups[group].push_back(&row);
					}
				}
				const auto available = [&](std::size_t group)
				{
					return present[group] && complete[group];
				};
				b.rows = 0;
				function_action_input raw{};
				raw.compile_units_complete = available(0);
				raw.scope_inputs_complete =
					available(1) && available(2) && available(3) && available(4) && available(5);
				raw.operation_inputs_complete = available(12);
				raw.admission_inputs_complete = available(8) && available(9) && available(10);
				raw.type_inputs_complete = available(6) && available(7);
				raw.call_inputs_complete = available(11);
				auto output = project_rows(raw, limits, stop, b, &groups);
				if (!output)
					return output.error();
				for (std::size_t group = 0; group < groups.size(); ++group)
					if (!present[group])
						output->unresolved.push_back({"sdk.action-scan-missing",
													  std::string{relations[group]},
													  "independent-scan-unavailable"});
				canonical(output->unresolved);
				output->source_queries = input;
				return output;
			}
			catch (const failure& exception)
			{
				return exception.value;
			}
			catch (const std::bad_alloc&)
			{
				return error{"sdk.action-resource-exhausted", "projection", "allocation"};
			}
			catch (const std::length_error&)
			{
				return error{"sdk.action-resource-exhausted", "projection", "length"};
			}
		}
	} // namespace
	result<function_action_projection> project_function_actions(function_action_input input,
																finite_population_limits limits,
																std::stop_token stop)
	{
		budget b{limits, stop};
		return project_rows(input, limits, stop, b);
	}
	result<function_action_projection>
	project_function_actions(const application_query_results& input,
							 finite_population_limits limits,
							 std::stop_token stop)
	{
		return project_queries(input, limits, stop);
	}
} // namespace cxxlens::sdk::query
