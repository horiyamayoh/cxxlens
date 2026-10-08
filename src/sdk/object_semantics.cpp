#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <tuple>

#include <cxxlens/sdk/object_semantics.hpp>

#include "object_path_internal.hpp"
#include "query_projection_plan_limits_internal.hpp"
#include "query_projection_rows_internal.hpp"
#include "query_result_internal.hpp"
namespace cxxlens::sdk::query
{
	namespace
	{
		using namespace object_detail;
		using key_type = std::array<std::string, 4>;
		using refs = std::vector<std::size_t>;
		constexpr std::array<std::string_view, 17> relations{"build.compile_unit.v1",
															 "source.file.v1",
															 "source.span.v1",
															 "cc.entity.v1",
															 "cc.entity_detail.v1",
															 "cc.declaration.v1",
															 "cc.body.v1",
															 "cc.cfg_node.v1",
															 "cc.cfg_edge.v1",
															 "cc.type.v1",
															 "cc.syntax_node.v1",
															 "cc.sequence_context.v1",
															 "cc.sequence_pair.v1",
															 "cc.object_state_observation.v1",
															 "cc.declaration_inventory.v1",
															 "cc.constant_evaluation_root.v1",
															 "cc.template_inventory.v1"};
		constexpr std::array<std::string_view, 17> identifiers{"compile_unit",
															   "snapshot",
															   "span",
															   "entity",
															   "entity",
															   "declaration",
															   "body",
															   "node",
															   "edge",
															   "type",
															   "node",
															   "context",
															   "pair",
															   "observation",
															   "inventory",
															   "root",
															   "inventory"};
		const detached_cell* cell(const annotated_row& row, std::string_view name)
		{
			const auto at = row.values.find("output." + std::string{name});
			return at == row.values.end() ? nullptr : &at->second;
		}
		bool present(const annotated_row& row, std::string_view name)
		{
			const auto* c = cell(row, name);
			return c && c->state == cell_state::present && c->value;
		}
		std::string_view text(const annotated_row& row, std::string_view name)
		{
			const auto* c = cell(row, name);
			const auto* value = c && c->state == cell_state::present && c->value
				? std::get_if<std::string>(&*c->value)
				: nullptr;
			return value ? std::string_view{*value} : std::string_view{};
		}
		std::span<const std::byte> bytes(const annotated_row& row, std::string_view name)
		{
			const auto* c = cell(row, name);
			const auto* value = c && c->state == cell_state::present && c->value
				? std::get_if<std::vector<std::byte>>(&*c->value)
				: nullptr;
			return value ? std::span<const std::byte>{*value} : std::span<const std::byte>{};
		}
		std::optional<bool> boolean(const annotated_row& row, std::string_view name)
		{
			const auto* c = cell(row, name);
			const auto* value = c && c->state == cell_state::present && c->value
				? std::get_if<bool>(&*c->value)
				: nullptr;
			return value ? std::optional{*value} : std::nullopt;
		}
		std::optional<std::uint64_t> number(const annotated_row& row, std::string_view name)
		{
			const auto* c = cell(row, name);
			const auto* value = c && c->state == cell_state::present && c->value
				? std::get_if<std::uint64_t>(&*c->value)
				: nullptr;
			return value ? std::optional{*value} : std::nullopt;
		}
		key_type key(std::string_view id, const annotated_row& row, std::string_view variant)
		{
			return {
				std::string{id}, row.presence.universe, std::string{variant}, row.interpretation};
		}
		template <class T>
		key_type key(std::string_view id, const T& v)
		{
			return {std::string{id}, v.universe, v.variant, v.interpretation};
		}
		void downgrade(finite_population_state& state, bool conflict)
		{
			if (conflict)
				state = finite_population_state::conflicting;
			else if (state == finite_population_state::complete)
				state = finite_population_state::partial;
		}
		struct projector
		{
			budget& b;
			object_semantics_input input;
			object_semantics_projection output;
			std::array<std::map<key_type, refs>, 17> maps;
			std::map<std::size_t, std::size_t> evidence_map;
			std::vector<const annotated_row*> originals;
			std::vector<std::size_t> groups;
			void gap(finite_population_state& state,
					 std::vector<query_unresolved>& gaps,
					 std::string_view id,
					 std::string_view reason,
					 bool conflict = false)
			{
				b.work();
				b.retain(id.size() + reason.size() + sizeof(query_unresolved) + 64U);
				gaps.push_back({"sdk.object-" + std::string{reason}, std::string{id}, {}});
				downgrade(state, conflict);
			}
			std::string copy(std::string_view s)
			{
				b.retain(s.size() + sizeof(std::string));
				return std::string{s};
			}
			const refs& find(std::size_t group, const key_type& actual)
			{
				b.work();
				static const refs empty;
				const auto at = maps[group].find(actual);
				return at == maps[group].end() ? empty : at->second;
			}
			bool equal(const refs& entries)
			{
				if (entries.empty())
					return false;
				for (auto i : entries)
				{
					b.work();
					// Original rows are immutable throughout this projection. Their
					// scalar alternatives all have reflexive equality, so the same
					// validated row needs no repeated cell comparison.
					if (originals[i] == originals[entries.front()])
						continue;
					const auto& left = originals[i]->values;
					const auto& right = originals[entries.front()]->values;
					if (left.size() != right.size())
						return false;
					if (!std::ranges::equal(
							left,
							right,
							[&](const auto& x, const auto& y)
							{
								const auto charge = [&](const auto& c)
								{
									b.work(c.first.size() + c.second.type.parameter.size() + 1U);
									if (c.second.unknown_reason)
										b.work(c.second.unknown_reason->size());
									if (c.second.value)
									{
										if (const auto* value =
												std::get_if<std::string>(&*c.second.value))
											b.work(value->size());
										if (const auto* value = std::get_if<std::vector<std::byte>>(
												&*c.second.value))
											b.work(value->size());
									}
								};
								charge(x);
								charge(y);
								return x.first == y.first && x.second.type == y.second.type &&
									x.second.state == y.second.state &&
									x.second.value == y.second.value &&
									x.second.unknown_reason == y.second.unknown_reason;
							}))
						return false;
				}
				return true;
			}
			void bind(refs& evidence, const refs& entries)
			{
				for (auto index : entries)
				{
					b.work();
					b.charge(b.references,
							 1U,
							 b.limits.maximum_evidence_references,
							 "evidence-references");
					auto at = evidence_map.find(index);
					std::size_t value{};
					if (at == evidence_map.end())
					{
						const auto& row = *originals[index];
						// Conservative estimate before any canonicalization/output copy.
						// Original envelope and cell strings are included, including
						// independent witnesses.
						std::size_t estimate = 2048U;
						const auto add = [&](std::size_t n)
						{
							b.work();
							if (n > b.limits.maximum_retained_bytes ||
								estimate > b.limits.maximum_retained_bytes - n)
								fail("row", "retained-bytes", "sdk.object-budget");
							estimate += n;
						};
						for (const auto& [name, c] : row.values)
						{
							add(name.size() + c.type.parameter.size() + 256U);
							if (c.unknown_reason)
								add(c.unknown_reason->size());
							if (c.value)
							{
								if (const auto* s = std::get_if<std::string>(&*c.value))
									add(s->size());
								if (const auto* v = std::get_if<std::vector<std::byte>>(&*c.value))
									add(v->size());
							}
						}
						const auto strings = [&](const auto& list)
						{
							for (const auto& s : list)
								add(s.size() + 128U);
						};
						strings(row.presence.fragments);
						strings(row.claim_contributors);
						strings(row.provenance);
						add(row.presence.universe.size() + row.interpretation.size());
						const auto producer = [&](const auto& p)
						{
							add(p.id.size() + p.semantic_contract.size() + 128U);
						};
						const auto guarantee = [&](const auto& g)
						{
							add(g.approximation.size() + g.scope.size() + g.assumptions.size() +
								128U);
							strings(g.verification_modalities);
						};
						for (const auto& p : row.producer_contracts)
							producer(p);
						for (const auto& g : row.contributor_guarantees)
							guarantee(g);
						for (const auto& e : row.contributor_edges)
						{
							add(e.claim_contributor.size() + e.provenance.size() +
								e.interpretation.size() + e.condition.universe.size() + 256U);
							producer(e.producer);
							guarantee(e.guarantee);
							strings(e.condition.fragments);
						}
						if (estimate > b.limits.maximum_retained_bytes / 3U)
							fail("row", "temporary-bound", "sdk.object-budget");
						b.retain(3U * estimate);
						const auto canonical = row.canonical_form();
						b.charge(b.evidence,
								 canonical.size(),
								 b.limits.maximum_evidence_bytes,
								 "evidence-bytes");
						value = output.evidence.size();
						output.evidence.push_back({copy(relations[groups[index]]), row});
						evidence_map.emplace(index, value);
					}
					else
						value = at->second;
					b.retain(sizeof(std::size_t));
					evidence.push_back(value);
				}
			}
			template <class T>
			const annotated_row*
			reference(std::size_t group, std::string_view id, T& v, finite_population_state& state)
			{
				const auto& entries = find(group, key(id, v));
				bind(v.evidence, entries);
				if (id.empty() || entries.empty())
				{
					gap(state, v.gaps, id, "reference-missing");
					return nullptr;
				}
				if (!equal(entries))
				{
					gap(state, v.gaps, id, "reference-conflicting", true);
					return nullptr;
				}
				const auto* row = originals[entries.front()];
				if (group >= 4U && group != 5U && group != 9U && group != 14U && group != 16U &&
					text(*row, "compile_unit") != v.compile_unit)
				{
					gap(state, v.gaps, id, "reference-unit-conflicting", true);
					return nullptr;
				}
				if (group == 5U)
				{
					// Declaration IDs are physical originals without a unit column. Bind them
					// only through the independently recorded same-unit declaration
					// inventory.
					bool admitted{};
					for (const auto& [actual, inventory_entries] : maps[14U])
					{
						b.work();
						if (actual[1] != v.universe || actual[2] != v.variant ||
							actual[3] != v.interpretation)
							continue;
						for (auto original_index : inventory_entries)
						{
							b.work();
							const auto& inventory = *originals[original_index];
							if (text(inventory, "compile_unit") != v.compile_unit)
								continue;
							const auto ids = members(inventory, "declarations");
							if (std::ranges::binary_search(ids, std::string{id}))
							{
								bind(v.evidence, {original_index});
								admitted = true;
							}
						}
					}
					if (!admitted)
					{
						gap(state, v.gaps, id, "declaration-unit-membership-missing");
						return nullptr;
					}
				}
				return row;
			}
			template <class T>
			void source(std::string_view id, T& v, finite_population_state& state)
			{
				state = finite_population_state::complete;
				const auto* span = reference(2U, id, v, state);
				if (!span)
					return;
				const auto* file = reference(1U, text(*span, "snapshot"), v, state);
				if (!file)
					return;
				const auto begin = number(*span, "begin"), end = number(*span, "end"),
						   size = number(*file, "size");
				if (text(*span, "file") != text(*file, "file") || !begin || !end || !size ||
					*begin > *end || *end > *size)
					gap(state, v.gaps, id, "source-range-conflicting", true);
			}
			template <class T>
			void usr(std::size_t group,
					 std::string_view id,
					 std::span<const std::byte> raw,
					 T& v,
					 finite_population_state& state)
			{
				const auto* entity = reference(group, id, v, state);
				if (!entity)
					return;
				const auto framed = bytes(*entity, "provider_local_key");
				constexpr std::string_view prefix = "clang-usr:";
				if (raw.empty() || !present(*entity, "provider_local_key"))
				{
					gap(state, v.gaps, id, "usr-unavailable");
					return;
				}
				bool agrees = framed.size() == prefix.size() + raw.size();
				if (agrees)
				{
					for (std::size_t i{}; i < prefix.size(); ++i)
					{
						b.work();
						agrees &= framed[i] == static_cast<std::byte>(prefix[i]);
					}
					for (std::size_t i{}; i < raw.size(); ++i)
					{
						b.work();
						agrees &= framed[prefix.size() + i] == raw[i];
					}
				}
				if (!agrees)
					gap(state, v.gaps, id, "usr-conflicting", true);
			}
			original_object_observation base(std::size_t group, const key_type& actual)
			{
				b.member();
				b.retain(sizeof(original_object_observation) + 256U);
				original_object_observation v;
				v.observation = copy(actual[0]);
				v.universe = copy(actual[1]);
				v.variant = copy(actual[2]);
				v.interpretation = copy(actual[3]);
				const auto& entries = find(group, actual);
				bind(v.evidence, entries);
				v.membership_state = finite_population_state::complete;
				if (!equal(entries))
				{
					gap(v.membership_state, v.gaps, v.observation, "member-conflicting", true);
					return v;
				}
				const auto& row = *originals[entries.front()];
				v.compile_unit = copy(text(row, "compile_unit"));
				v.profile = copy(text(row, "profile"));
				v.source_span = copy(text(row, "source"));
				v.is_system = boolean(row, "is_system");
				v.function = copy(text(row, "owner_entity"));
				v.declaration = copy(text(row, "declaration"));
				v.body = copy(text(row, "body"));
				v.expression = copy(text(row, "expression"));
				if (text(row, "membership_state") != "complete")
					gap(v.membership_state, v.gaps, v.observation, "membership-unavailable");
				const auto* unit = reference(0U, v.compile_unit, v, v.membership_state);
				if (unit && present(*unit, "variant") && text(*unit, "variant") != v.variant)
					gap(v.membership_state,
						v.gaps,
						v.observation,
						"unit-variant-conflicting",
						true);
				source(v.source_span, v, v.source_state);
				v.owner_state = finite_population_state::complete;
				usr(3U, v.function, bytes(row, "owner_usr"), v, v.owner_state);
				const auto* decl = reference(5U, v.declaration, v, v.owner_state);
				if (decl && text(*decl, "entity") != v.function)
					gap(v.owner_state,
						v.gaps,
						v.observation,
						"owner-declaration-conflicting",
						true);
				if (!v.body.empty())
				{
					const auto* body = reference(6U, v.body, v, v.owner_state);
					if (body && text(*body, "function") != v.function)
						gap(v.owner_state, v.gaps, v.observation, "owner-body-conflicting", true);
				}
				if (!v.expression.empty())
					reference(10U, v.expression, v, v.owner_state);
				if (text(row, "binding_state") != "complete")
					gap(v.owner_state, v.gaps, v.observation, "binding-unavailable");
				return v;
			}
			original_object_observation facet_base(std::size_t group,
												   const key_type& actual,
												   std::string_view declaration_field,
												   std::string_view profile_field)
			{
				b.member();
				b.retain(sizeof(original_object_observation) + 256U);
				original_object_observation v;
				v.observation = copy(actual[0]);
				v.universe = copy(actual[1]);
				v.variant = copy(actual[2]);
				v.interpretation = copy(actual[3]);
				const auto& entries = find(group, actual);
				bind(v.evidence, entries);
				v.membership_state = finite_population_state::complete;
				v.owner_state = finite_population_state::complete;
				if (!equal(entries))
				{
					gap(v.membership_state, v.gaps, v.observation, "facet-conflicting", true);
					return v;
				}
				const auto& row = *originals[entries.front()];
				v.compile_unit = copy(text(row, "compile_unit"));
				v.function = copy(text(row, "function"));
				v.declaration = copy(text(row, declaration_field));
				v.profile = copy(text(row, profile_field));
				v.source_span = copy(text(row, "source"));
				v.is_system = boolean(row, "is_system");
				if (group == 10U)
					v.expression = copy(actual[0]);
				const auto* unit = reference(0U, v.compile_unit, v, v.membership_state);
				if (unit && present(*unit, "variant") && text(*unit, "variant") != v.variant)
					gap(v.membership_state,
						v.gaps,
						v.observation,
						"unit-variant-conflicting",
						true);
				reference(3U, v.function, v, v.owner_state);
				const auto* declaration = reference(5U, v.declaration, v, v.owner_state);
				if (declaration && text(*declaration, "entity") != v.function)
					gap(v.owner_state,
						v.gaps,
						v.observation,
						"owner-declaration-conflicting",
						true);
				if (group == 10U)
					source(v.source_span, v, v.source_state);
				return v;
			}
			const annotated_row* declaration_detail(original_object_observation& v)
			{
				const auto* declaration = reference(5U, v.declaration, v, v.owner_state);
				if (!declaration)
					return nullptr;
				refs matching;
				for (auto index : find(4U, key(v.function, v)))
				{
					b.work();
					const auto& row = *originals[index];
					if (text(row, "compile_unit") == v.compile_unit &&
						text(row, "source") == text(*declaration, "source"))
					{
						b.retain(sizeof(std::size_t));
						matching.push_back(index);
					}
				}
				bind(v.evidence, matching);
				if (matching.empty())
				{
					gap(v.owner_state, v.gaps, v.declaration, "physical-detail-missing");
					return nullptr;
				}
				if (!equal(matching))
				{
					gap(v.owner_state, v.gaps, v.declaration, "physical-detail-conflicting", true);
					return nullptr;
				}
				return originals[matching.front()];
			}
			void exit_facet(const key_type& actual)
			{
				auto v =
					facet_base(8U, actual, "function_exit_declaration", "function_exit_profile");
				v.kind = "function_exit";
				original_function_exit fact;
				fact.edge = copy(actual[0]);
				fact.exit_state = finite_population_state::complete;
				const auto& entries = find(8U, actual);
				if (!equal(entries))
					fact.exit_state = finite_population_state::conflicting;
				else
				{
					const auto& row = *originals[entries.front()];
					fact.exit_kind = copy(text(row, "function_exit_kind"));
					fact.from = copy(text(row, "from"));
					fact.to = copy(text(row, "to"));
					if (v.profile != "clang22-original-function-exits/1" ||
						text(row, "function_exit_state") != "complete")
						gap(fact.exit_state, v.gaps, v.observation, "exit-facet-unavailable");
					const auto* from = reference(7U, fact.from, v, fact.exit_state);
					const auto* to = reference(7U, fact.to, v, fact.exit_state);
					if (from && to)
					{
						v.body = copy(text(*from, "body"));
						fact.body = copy(v.body);
						if (v.body.empty() || text(*to, "body") != v.body ||
							text(*from, "function") != v.function ||
							text(*to, "function") != v.function)
							gap(fact.exit_state,
								v.gaps,
								v.observation,
								"exit-node-owner-conflicting",
								true);
						const auto* body = reference(6U, v.body, v, fact.exit_state);
						if (body)
						{
							if (!present(*body, "function_exit_declaration"))
								gap(fact.exit_state,
									v.gaps,
									v.observation,
									"exit-body-declaration-missing");
							if (present(*body, "function_exit_declaration") &&
								text(*body, "function_exit_declaration") != v.declaration)
								gap(fact.exit_state,
									v.gaps,
									v.observation,
									"exit-body-declaration-conflicting",
									true);
							if (text(*body, "function") != v.function ||
								text(*body, "exit") != fact.to)
								gap(fact.exit_state,
									v.gaps,
									v.observation,
									"exit-body-conflicting",
									true);
							auto body_source = finite_population_state::complete;
							source(text(*body, "source"), v, body_source);
							if (body_source != finite_population_state::complete)
								gap(fact.exit_state,
									v.gaps,
									v.observation,
									"exit-body-source-unavailable",
									body_source == finite_population_state::conflicting);
						}
					}
					const auto* detail = declaration_detail(v);
					if (detail)
					{
						v.source_span = copy(text(*detail, "source"));
						source(v.source_span, v, v.source_state);
						fact.return_kind = copy(text(*detail, "return_kind"));
						fact.is_main = boolean(*detail, "is_main");
						fact.is_coroutine = boolean(*detail, "is_coroutine");
						fact.coroutine_has_return_void =
							boolean(*detail, "coroutine_has_return_void");
						if (text(*detail, "function_exit_profile") != v.profile ||
							text(*detail, "function_exit_state") != "complete")
							gap(fact.exit_state, v.gaps, v.observation, "exit-role-unavailable");
					}
					else
						gap(fact.exit_state, v.gaps, v.observation, "exit-role-missing");
				}
				v.fact = std::move(fact);
				output.observations.push_back(std::move(v));
			}
			std::optional<original_integer_interval> interval(const annotated_row& row,
															  std::string_view lower,
															  std::string_view upper,
															  original_object_observation& v,
															  finite_population_state& state)
			{
				if (!present(row, lower) || !present(row, upper))
				{
					gap(state, v.gaps, v.observation, "original-interval-unavailable");
					return std::nullopt;
				}
				const auto lo = text(row, lower), hi = text(row, upper);
				b.work(lo.size() + hi.size());
				const auto canonical = [](std::string_view value)
				{
					if (value.empty())
						return false;
					const bool negative = value.front() == '-';
					if (negative)
						value.remove_prefix(1U);
					if (value.empty() || (value.front() == '0' && (value.size() != 1U || negative)))
						return false;
					return std::ranges::all_of(value,
											   [](char digit)
											   {
												   return digit >= '0' && digit <= '9';
											   });
				};
				const auto less = [](std::string_view left, std::string_view right)
				{
					const bool ln = left.front() == '-', rn = right.front() == '-';
					if (ln != rn)
						return ln;
					if (ln)
					{
						left.remove_prefix(1U);
						right.remove_prefix(1U);
					}
					const bool smaller =
						left.size() != right.size() ? left.size() < right.size() : left < right;
					return ln ? left != right && !smaller : smaller;
				};
				if (!canonical(lo) || !canonical(hi) || less(hi, lo))
				{
					gap(state, v.gaps, v.observation, "original-interval-conflicting", true);
					return std::nullopt;
				}
				return original_integer_interval{copy(lo), copy(hi)};
			}

			void lifetime_facet(original_object_observation v,
								const annotated_row& row,
								finite_population_state state)
			{
				original_lifetime_access fact;
				fact.storage = copy(text(row, "object_declaration"));
				fact.phase = copy(text(row, "object_phase"));
				fact.actual_access = true;
				fact.phase_state = state;
				const auto* object = reference(5U, fact.storage, v, fact.phase_state);
				const auto entity = text(row, "object_entity");
				reference(3U, entity, v, fact.phase_state);
				if (object && text(*object, "entity") != entity)
					gap(fact.phase_state,
						v.gaps,
						v.observation,
						"object-declaration-conflicting",
						true);
				const auto* receiver =
					reference(10U, text(row, "object_receiver"), v, fact.phase_state);
				const auto declared_type = text(row, "declared_object_type");
				reference(9U, declared_type, v, fact.phase_state);
				if (receiver)
				{
					if (!present(*receiver, "object_declaration") ||
						!present(*receiver, "object_entity") ||
						!present(*receiver, "canonical_type"))
						gap(fact.phase_state, v.gaps, v.observation, "direct-receiver-unavailable");
					else if (text(*receiver, "kind") != "DeclRefExpr" ||
							 text(*receiver, "function") != v.function ||
							 text(*receiver, "object_declaration") != fact.storage ||
							 text(*receiver, "object_entity") != entity ||
							 text(*receiver, "canonical_type") != declared_type)
						gap(fact.phase_state,
							v.gaps,
							v.observation,
							"direct-receiver-conflicting",
							true);
					finite_population_state source_state = finite_population_state::complete;
					source(text(*receiver, "source"), v, source_state);
					if (source_state != finite_population_state::complete)
						gap(fact.phase_state,
							v.gaps,
							v.observation,
							"receiver-source-unavailable",
							source_state == finite_population_state::conflicting);
				}
				v.body = copy(text(row, "object_body"));
				const auto* body = reference(6U, v.body, v, fact.phase_state);
				if (body && !present(*body, "function_exit_declaration"))
					gap(fact.phase_state, v.gaps, v.observation, "point-body-owner-unavailable");
				else if (body &&
						 (text(*body, "function") != v.function ||
						  text(*body, "function_exit_declaration") != v.declaration))
					gap(fact.phase_state,
						v.gaps,
						v.observation,
						"point-body-owner-conflicting",
						true);
				const auto* node = reference(7U, text(row, "object_cfg_node"), v, fact.phase_state);
				const auto element = number(row, "object_cfg_element_index");
				const auto count = node ? number(*node, "element_count") : std::nullopt;
				if (!element || !count)
					gap(fact.phase_state, v.gaps, v.observation, "cfg-point-unavailable");
				else if (text(*node, "function") != v.function || text(*node, "body") != v.body ||
						 *element >= *count)
					gap(fact.phase_state, v.gaps, v.observation, "cfg-point-conflicting", true);
				if (v.profile != "clang22-original-direct-record-member-lifetime/1" ||
					text(row, "kind") != "CXXMemberCallExpr")
					gap(fact.phase_state, v.gaps, v.observation, "profile-unavailable");
				const auto polymorphic = boolean(row, "actual_polymorphic_use");
				if (!polymorphic ||
					(fact.phase != "live" && fact.phase != "ended" && fact.phase != "not_started"))
					gap(fact.phase_state, v.gaps, v.observation, "lifetime-axis-unavailable");
				if (fact.phase == "live")
					fact.permitted_in_phase = true;
				else if (fact.phase == "ended" || fact.phase == "not_started")
					fact.permitted_in_phase = false;
				if (polymorphic == std::optional{true})
				{
					b.member();
					b.retain(sizeof(original_object_observation) + 256U);
					original_object_observation dynamic;
					dynamic.is_system = v.is_system;
					dynamic.observation = copy(v.observation);
					dynamic.kind = copy("dynamic_object_access");
					dynamic.profile = copy(v.profile);
					dynamic.compile_unit = copy(v.compile_unit);
					dynamic.function = copy(v.function);
					dynamic.declaration = copy(v.declaration);
					dynamic.body = copy(v.body);
					dynamic.source_span = copy(v.source_span);
					dynamic.expression = copy(v.expression);
					dynamic.universe = copy(v.universe);
					dynamic.variant = copy(v.variant);
					dynamic.interpretation = copy(v.interpretation);
					dynamic.source_state = v.source_state;
					dynamic.owner_state = v.owner_state;
					dynamic.membership_state = v.membership_state;
					b.charge(b.references,
							 v.evidence.size(),
							 b.limits.maximum_evidence_references,
							 "evidence-references");
					b.retain(v.evidence.size() * sizeof(std::size_t));
					dynamic.evidence = v.evidence;
					for (const auto& gap_value : v.gaps)
					{
						b.work();
						b.retain(sizeof(query_unresolved) + 128U);
						dynamic.gaps.push_back({copy(gap_value.code),
												copy(gap_value.subject),
												copy(gap_value.detail)});
					}
					original_dynamic_object_access value;
					value.storage = copy(fact.storage);
					value.actual_virtual_or_dynamic_type_access = true;
					value.valid_dynamic_object = fact.phase == "live";
					value.dynamic_state = fact.phase_state;
					dynamic.fact = std::move(value);
					output.observations.push_back(std::move(dynamic));
				}
				v.fact = std::move(fact);
				output.observations.push_back(std::move(v));
			}
			void syntax_facet(const key_type& actual)
			{
				auto v = facet_base(10U, actual, "object_scope_declaration", "object_fact_profile");
				const auto& entries = find(10U, actual);
				if (!equal(entries))
				{
					v.kind = "object_syntax";
					output.observations.push_back(std::move(v));
					return;
				}
				const auto& row = *originals[entries.front()];
				v.kind = copy(text(row, "object_fact_kind"));
				auto state = finite_population_state::complete;
				if (text(row, "object_fact_state") != "complete")
					gap(state, v.gaps, v.observation, "syntax-facet-unavailable");
				if (v.kind == "direct_member_lifetime")
				{
					lifetime_facet(std::move(v), row, state);
					return;
				}
				const auto operand_id = text(row, "operand");
				const auto* operand = reference(10U, operand_id, v, state);
				const auto operand_type = text(row, "operand_type"),
						   result_type = text(row, "canonical_type");
				if (operand)
				{
					if (!present(*operand, "canonical_type"))
						gap(state, v.gaps, v.observation, "operand-type-unavailable");
					else if (text(*operand, "canonical_type") != operand_type)
						gap(state, v.gaps, v.observation, "operand-type-conflicting", true);
					if (text(*operand, "function") != v.function)
						gap(state, v.gaps, v.observation, "operand-owner-conflicting", true);
					finite_population_state operand_source = finite_population_state::complete;
					source(text(*operand, "source"), v, operand_source);
					if (operand_source != finite_population_state::complete)
						gap(state,
							v.gaps,
							v.observation,
							"operand-source-unavailable",
							operand_source == finite_population_state::conflicting);
				}
				reference(9U, operand_type, v, state);
				const auto* type = reference(9U, result_type, v, state);
				if (v.kind == "enum_cast")
				{
					original_enum_value fact;
					fact.operand = copy(operand_id);
					fact.operand_type = copy(operand_type);
					fact.result_type = copy(result_type);
					fact.enumeration_state = state;
					if (v.profile != "clang22-original-enum-cast/1")
						gap(fact.enumeration_state, v.gaps, v.observation, "profile-unavailable");
					if (type)
					{
						fact.fixed_underlying = boolean(*type, "enum_fixed_underlying");
						if (text(*type, "enum_value_profile") !=
								"clang22-original-enum-value-domain/1" ||
							text(*type, "enum_value_state") != "complete" || !fact.fixed_underlying)
							gap(fact.enumeration_state,
								v.gaps,
								v.observation,
								"enum-domain-unavailable");
						if (fact.fixed_underlying && !*fact.fixed_underlying)
						{
							fact.compiler_value_domain = interval(*type,
																  "enum_value_lower",
																  "enum_value_upper",
																  v,
																  fact.enumeration_state);
							fact.original_value = interval(row,
														   "original_value_lower",
														   "original_value_upper",
														   v,
														   fact.enumeration_state);
						}
					}
					v.fact = std::move(fact);
				}
				else if (v.kind == "bool_integral_conversion" || v.kind == "bool_bit_cast" ||
						 v.kind == "bool_representation_load")
				{
					original_bool_representation fact;
					fact.kind = copy(v.kind == "bool_integral_conversion" ? "integral_conversion"
										 : v.kind == "bool_bit_cast"	  ? "bit_cast"
																		  : "representation_load");
					fact.operand = copy(operand_id);
					fact.operand_type = copy(operand_type);
					fact.result_type = copy(result_type);
					fact.canonical_bool = boolean(row, "canonical_bool");
					fact.representation_state = state;
					if (!type ||
						text(*type, "builtin_profile") != "clang22-original-builtin-type/1" ||
						text(*type, "builtin_state") != "complete" ||
						!present(*type, "builtin_kind"))
						gap(fact.representation_state,
							v.gaps,
							v.observation,
							"canonical-bool-unavailable");
					else if (text(*type, "builtin_kind") != "Bool" ||
							 fact.canonical_bool != std::optional{true})
						gap(fact.representation_state,
							v.gaps,
							v.observation,
							"canonical-bool-conflicting",
							true);
					if (v.profile != "clang22-original-bool-representation/1")
						gap(fact.representation_state,
							v.gaps,
							v.observation,
							"profile-unavailable");
					if (fact.kind != "integral_conversion")
					{
						if (text(row, "representation_state") != "complete")
							gap(fact.representation_state,
								v.gaps,
								v.observation,
								"representation-unavailable");
						fact.original_representation = interval(row,
																"original_value_lower",
																"original_value_upper",
																v,
																fact.representation_state);
					}
					v.fact = std::move(fact);
				}
				else if (v.kind == "direct_type_access" || v.kind == "direct_static_downcast")
				{
					const auto object_decl = text(row, "object_declaration"),
							   object_entity = text(row, "object_entity");
					const auto* object = reference(5U, object_decl, v, state);
					reference(3U, object_entity, v, state);
					if (object && text(*object, "entity") != object_entity)
						gap(state, v.gaps, v.observation, "object-declaration-conflicting", true);
					const auto declared_type = text(row, "declared_object_type");
					reference(9U, declared_type, v, state);
					const auto* address = reference(10U, text(row, "object_address"), v, state);
					if (address && text(*address, "function") != v.function)
						gap(state, v.gaps, v.observation, "object-address-owner-conflicting", true);
					if (text(row, "current_object_profile") !=
							"clang22-owned-object-initialization-before-address/1" ||
						text(row, "current_object_state") != "live_unreplaced")
						gap(state, v.gaps, v.observation, "current-object-unavailable");
					const auto storage = copy(object_decl);
					if (v.kind == "direct_type_access")
					{
						original_type_access fact;
						fact.storage = storage;
						fact.effective_type = copy(declared_type);
						fact.access_type = copy(result_type);
						fact.actual_value_access = true;
						fact.language_type_access_permitted =
							boolean(row, "type_access_permission");
						fact.effective_type_state = state;
						if (v.profile != "clang22-original-direct-type-access/1")
							gap(fact.effective_type_state,
								v.gaps,
								v.observation,
								"profile-unavailable");
						v.fact = std::move(fact);
					}
					else
					{
						original_downcast fact;
						fact.kind = "static_pointer";
						fact.storage = storage;
						fact.dynamic_type = copy(declared_type);
						fact.target_type = copy(text(row, "object_target_type"));
						fact.nonnull = true;
						fact.target_is_compatible_dynamic_subobject =
							boolean(row, "downcast_compatible_with_declared_type");
						fact.dynamic_state = state;
						reference(9U, fact.target_type, v, fact.dynamic_state);
						if (v.profile != "clang22-original-direct-static-downcast/1")
							gap(fact.dynamic_state, v.gaps, v.observation, "profile-unavailable");
						v.fact = std::move(fact);
					}
				}
				else
					gap(v.membership_state, v.gaps, v.observation, "syntax-kind-unavailable");
				output.observations.push_back(std::move(v));
			}
			void contexts(const annotated_row& pair,
						  std::string_view side,
						  original_object_observation& v,
						  original_sequence_pair& fact)
			{
				auto& out = side == "left" ? fact.left_contexts : fact.right_contexts;
				std::string id = copy(text(pair, std::string{side} + "_context"));
				if (text(pair, std::string{side} + "_context_state") != "complete")
					gap(fact.context_state, v.gaps, v.observation, "context-unavailable");
				std::set<std::string> visited;
				std::optional<std::uint64_t> previous_depth, access;
				while (!id.empty())
				{
					b.member();
					b.retain(id.size() + 128U);
					if (!visited.insert(id).second)
					{
						gap(fact.context_state, v.gaps, id, "context-cycle", true);
						break;
					}
					const auto* ctx = reference(11U, id, v, fact.context_state);
					if (!ctx)
						break;
					const auto depth = number(*ctx, "depth"),
							   ordinal = number(*ctx, "access_ordinal");
					const auto root = number(*ctx, "checker_root"),
							   expected_root = number(pair, "checker_root");
					if (!depth || !ordinal || !root || !expected_root ||
						text(*ctx, "expression").empty() || text(*ctx, "owner_entity").empty() ||
						text(*ctx, "language").empty() ||
						text(*ctx, "profile") != "clang22-original-argument-sequencing-context/1" ||
						text(*ctx, "membership_state") != "complete" ||
						text(*ctx, "binding_state") != "complete")
						gap(fact.context_state,
							v.gaps,
							id,
							"context-binding-unavailable",
							text(*ctx, "membership_state") == "conflicting" ||
								text(*ctx, "binding_state") == "conflicting");
					if ((root && expected_root && root != expected_root) ||
						(previous_depth && depth &&
						 (*previous_depth == 0U || *depth != *previous_depth - 1U)) ||
						(access && ordinal && access != ordinal) ||
						(!text(*ctx, "expression").empty() &&
						 text(*ctx, "expression") !=
							 text(pair, std::string{side} + "_expression")) ||
						(!text(*ctx, "owner_entity").empty() &&
						 text(*ctx, "owner_entity") != v.function) ||
						(!text(*ctx, "language").empty() &&
						 text(*ctx, "language") != text(pair, "language")))
						gap(fact.context_state, v.gaps, id, "context-owner-conflicting", true);
					previous_depth = depth;
					access = ordinal;
					original_sequence_pair::argument_context item;
					item.invocation = copy(text(*ctx, "invocation"));
					item.argument_expression = copy(text(*ctx, "argument"));
					const auto index = number(*ctx, "argument_index");
					const auto indeterminate = boolean(*ctx, "indeterminately_sequenced");
					if (!index || !indeterminate)
						gap(fact.context_state, v.gaps, id, "context-axis-unavailable");
					item.argument_index = index.value_or(0U);
					item.indeterminately_sequenced = indeterminate.value_or(false);
					constexpr std::array<std::string_view, 4U> post17{
						"c++17", "c++20", "c++23", "c++26"};
					if (indeterminate == std::optional{true} &&
						std::ranges::find(post17, text(*ctx, "language")) == post17.end())
						gap(fact.context_state, v.gaps, id, "context-language-conflicting", true);
					reference(10U, item.invocation, v, fact.context_state);
					reference(10U, item.argument_expression, v, fact.context_state);
					b.retain(sizeof(item));
					out.push_back(std::move(item));
					id = copy(text(*ctx, "parent_context"));
					if (id.empty() && depth && *depth != 0U)
						gap(fact.context_state, v.gaps, v.observation, "context-parent-missing");
				}
				std::ranges::reverse(out);
			}
			void sequence(const key_type& actual)
			{
				auto v = base(12U, actual);
				v.kind = "sequence_pair";
				const auto& entries = find(12U, actual);
				if (!equal(entries))
				{
					output.observations.push_back(std::move(v));
					return;
				}
				const auto& row = *originals[entries.front()];
				original_sequence_pair fact;
				fact.storage_state = fact.sequencing_state = fact.context_state =
					finite_population_state::complete;
				fact.left_expression = copy(text(row, "left_expression"));
				fact.right_expression = copy(text(row, "right_expression"));
				fact.left_source = copy(text(row, "left_source"));
				fact.right_source = copy(text(row, "right_source"));
				fact.storage_declaration = copy(text(row, "storage_declaration"));
				fact.storage_entity = copy(text(row, "storage_entity"));
				fact.left_modifies = boolean(row, "left_modifies");
				fact.right_modifies = boolean(row, "right_modifies");
				fact.scalar_nonreference_storage = boolean(row, "scalar_nonreference_storage");
				fact.jointly_potentially_evaluated = boolean(row, "jointly_potentially_evaluated");
				fact.language = copy(text(row, "language"));
				constexpr std::array<std::string_view, 8U> languages{
					"c", "c++98", "c++11", "c++14", "c++17", "c++20", "c++23", "c++26"};
				if (std::ranges::find(languages, fact.language) == languages.end())
					gap(fact.sequencing_state, v.gaps, v.observation, "language-unavailable");
				fact.sequencing = copy(text(row, "checker_relation"));
				if (v.profile != "clang22-original-sequence-checker-candidates/1")
					gap(v.membership_state, v.gaps, v.observation, "profile-unavailable");
				reference(10U, fact.left_expression, v, fact.sequencing_state);
				reference(10U, fact.right_expression, v, fact.sequencing_state);
				if (fact.left_expression.empty() || fact.left_expression == fact.right_expression)
					gap(fact.sequencing_state,
						v.gaps,
						v.observation,
						"access-identity-conflicting",
						true);
				finite_population_state left_source{}, right_source{};
				source(fact.left_source, v, left_source);
				source(fact.right_source, v, right_source);
				if (left_source != finite_population_state::complete ||
					right_source != finite_population_state::complete)
					gap(fact.sequencing_state,
						v.gaps,
						v.observation,
						"access-source-unavailable",
						left_source == finite_population_state::conflicting ||
							right_source == finite_population_state::conflicting);
				usr(3U, fact.storage_entity, bytes(row, "storage_usr"), v, fact.storage_state);
				const auto* storage =
					reference(5U, fact.storage_declaration, v, fact.storage_state);
				if (storage && text(*storage, "entity") != fact.storage_entity)
					gap(fact.storage_state,
						v.gaps,
						v.observation,
						"storage-declaration-conflicting",
						true);
				contexts(row, "left", v, fact);
				contexts(row, "right", v, fact);
				bool indeterminate{};
				for (const auto& l : fact.left_contexts)
					for (const auto& r : fact.right_contexts)
					{
						b.work();
						if (l.invocation == r.invocation && l.argument_index != r.argument_index &&
							l.indeterminately_sequenced && r.indeterminately_sequenced)
							indeterminate = true;
					}
				if (indeterminate)
					fact.sequencing = "indeterminately_sequenced";
				else if (fact.sequencing != "unsequenced" && fact.sequencing != "sequenced" &&
						 fact.sequencing != "indeterminately_sequenced")
					gap(fact.sequencing_state, v.gaps, v.observation, "sequencing-unavailable");
				if (fact.context_state != finite_population_state::complete)
					gap(fact.sequencing_state,
						v.gaps,
						v.observation,
						"context-closure-unavailable",
						fact.context_state == finite_population_state::conflicting);
				v.fact = std::move(fact);
				output.observations.push_back(std::move(v));
			}
			std::string storage_identity(const annotated_row& row,
										 original_object_observation& v,
										 finite_population_state& state)
			{
				const auto path = bytes(row, "storage_path");
				if (!present(row, "storage_path"))
				{
					gap(state, v.gaps, v.observation, "storage-path-unavailable");
					return {};
				}
				(void)decode_path(path, b);
				const auto kind = text(row, "storage_kind");
				const auto call = number(row, "call_index"), generation = number(row, "generation");
				std::string anchor;
				if (kind == "declaration")
				{
					anchor = copy(text(row, "storage_declaration"));
					const auto entity = text(row, "storage_entity");
					usr(3U, entity, bytes(row, "storage_usr"), v, state);
					const auto* declaration = reference(5U, anchor, v, state);
					if (declaration && text(*declaration, "entity") != entity)
						gap(state, v.gaps, v.observation, "storage-declaration-conflicting", true);
				}
				else if (kind == "allocation")
				{
					const auto allocation = number(row, "allocation_ordinal");
					if (allocation)
						anchor = std::to_string(*allocation);
					else
						gap(state, v.gaps, v.observation, "allocation-identity-unavailable");
				}
				else if (kind == "temporary")
				{
					anchor = copy(text(row, "storage_source"));
					finite_population_state s{};
					source(anchor, v, s);
					if (s != finite_population_state::complete)
						gap(state,
							v.gaps,
							v.observation,
							"temporary-source-unavailable",
							s == finite_population_state::conflicting);
				}
				else
					gap(state, v.gaps, v.observation, "storage-kind-unavailable");
				if (!call || !generation)
					gap(state, v.gaps, v.observation, "storage-generation-unavailable");
				if (text(row, "storage_state") != "complete")
					gap(state, v.gaps, v.observation, "storage-binding-unavailable");
				// This lossless key represents this compiler observation's original storage
				// generation/path. It is not a canonical entity or alias/pointee deduction.
				b.retain(anchor.size() + path.size() * 2U + 256U);
				std::string result = copy(text(row, "evaluation_root")) + ":" + std::string{kind} +
					":" + std::to_string(anchor.size()) + ":" + anchor + ":" +
					std::to_string(call.value_or(0U)) + ":" +
					std::to_string(generation.value_or(0U)) + ":";
				constexpr std::string_view digits = "0123456789abcdef";
				for (auto octet : path)
				{
					b.work();
					const auto value = std::to_integer<unsigned>(octet);
					result += digits[value >> 4U];
					result += digits[value & 15U];
				}
				return result;
			}
			void object(const key_type& actual)
			{
				auto v = base(13U, actual);
				const auto& entries = find(13U, actual);
				if (!equal(entries))
				{
					v.kind = "object_state";
					output.observations.push_back(std::move(v));
					return;
				}
				const auto& row = *originals[entries.front()];
				v.kind = copy(text(row, "state_kind"));
				auto state = finite_population_state::complete;
				const auto storage = storage_identity(row, v, state);
				if (v.profile != "clang22-legacy-interpreter-object-state/1")
					gap(v.membership_state, v.gaps, v.observation, "profile-unavailable");
				if (text(row, "semantic_state") != "complete")
					gap(state, v.gaps, v.observation, "semantic-axis-unavailable");
				const auto* root = reference(15U, text(row, "evaluation_root"), v, state);
				if (root &&
					(!boolean(*root, "requested_constant_context").value_or(false) ||
					 boolean(*root, "potential_check").value_or(true) ||
					 text(*root, "interpreter") != "legacy" ||
					 text(*root, "profile") != "clang22-legacy-constant-evaluation/1"))
					gap(state, v.gaps, v.observation, "original-evaluation-unavailable");
				if (v.kind == "absent_subobject")
					gap(state, v.gaps, v.observation, "absent-lifecycle-unavailable");
				const auto access = text(row, "access_kind");
				const bool formal = access == "read" || access == "assign" ||
					access == "increment" || access == "decrement" || access == "member_call";
				auto type_state = state;
				reference(9U, text(row, "complete_type"), v, type_state);
				reference(9U, text(row, "subobject_type"), v, type_state);
				if (v.kind == "inactive_union_member")
				{
					original_union_access fact;
					fact.storage = storage;
					fact.active_member_state = copy(text(row, "active_member_state"));
					fact.active_state = state;
					const auto raw = bytes(row, "active_field_usr");
					if (present(row, "active_field_usr"))
					{
						b.retain(raw.size());
						fact.active_field =
							std::string(reinterpret_cast<const char*>(raw.data()), raw.size());
					}
					const auto selected = bytes(row, "selected_field_usr");
					b.retain(selected.size());
					fact.selected_field = std::string(
						reinterpret_cast<const char*>(selected.data()), selected.size());
					fact.actual_read = access == "read";
					fact.common_initial_sequence_permitted =
						boolean(row, "common_initial_sequence_permitted");
					if ((fact.active_member_state == "active" &&
						 (!fact.active_field || fact.active_field->empty())) ||
						(fact.active_member_state == "none" && fact.active_field &&
						 !fact.active_field->empty()))
						gap(fact.active_state,
							v.gaps,
							v.observation,
							"active-member-conflicting",
							true);
					if (fact.active_member_state != "active" && fact.active_member_state != "none")
						gap(fact.active_state, v.gaps, v.observation, "active-member-unavailable");
					v.fact = std::move(fact);
				}
				else if (v.kind == "incompatible_static_downcast")
				{
					original_downcast fact;
					fact.kind = access == "static_reference_downcast" ? "static_reference"
																	  : "static_pointer";
					fact.storage = storage;
					fact.dynamic_type = copy(text(row, "complete_type"));
					fact.target_type = copy(text(row, "target_type"));
					fact.dynamic_state = type_state;
					reference(9U, fact.target_type, v, fact.dynamic_state);
					fact.nonnull = true;
					fact.target_is_compatible_dynamic_subobject = false;
					v.fact = std::move(fact);
				}
				else
				{
					original_lifetime_access fact;
					fact.storage = storage;
					fact.phase = v.kind == "before_dynamic_construction" ? "not_started"
						: v.kind == "absent_subobject"					 ? "unknown"
																		 : "outside_lifetime";
					fact.actual_access = formal;
					fact.permitted_in_phase = formal ? std::optional{false} : std::nullopt;
					fact.phase_state = state;
					if (v.kind != "absent_subobject" && v.kind != "ended_call_frame" &&
						v.kind != "deleted_allocation" && v.kind != "before_dynamic_construction")
						gap(fact.phase_state, v.gaps, v.observation, "state-kind-unavailable");
					v.fact = std::move(fact);
				}
				if (boolean(row, "actual_polymorphic_use").value_or(false))
				{
					auto dynamic = base(13U, actual);
					dynamic.kind = "dynamic_object_access";
					b.retain(storage.size());
					original_dynamic_object_access fact;
					fact.storage = storage;
					fact.actual_virtual_or_dynamic_type_access = true;
					fact.valid_dynamic_object = false;
					fact.dynamic_state = type_state;
					dynamic.fact = std::move(fact);
					output.observations.push_back(std::move(dynamic));
				}
				output.observations.push_back(std::move(v));
			}
			std::vector<std::string> members(const annotated_row& row, std::string_view field)
			{
				const auto original = bytes(row, field);
				std::vector<std::string> ids;
				for (std::size_t i{}; i < original.size();)
				{
					b.member();
					if (original.size() - i < 4U)
						fail(field, "set-truncated");
					std::uint32_t size{};
					for (unsigned shift{}; shift < 32U; shift += 8U)
						size |= std::to_integer<std::uint32_t>(original[i++]) << shift;
					if (size > original.size() - i)
						fail(field, "set-truncated");
					b.retain(size + sizeof(std::string));
					b.work(size);
					std::string id(reinterpret_cast<const char*>(original.data() + i), size);
					i += size;
					if (id.empty() || (!ids.empty() && ids.back() >= id))
						fail(field, "canonical-set");
					ids.push_back(std::move(id));
				}
				return ids;
			}
			void inventory(const key_type& unit,
						   std::string_view prefix,
						   std::size_t group,
						   bool complete,
						   std::string_view profile)
			{
				b.work();
				b.charge(b.members, 1U, b.limits.maximum_populations, "populations");
				b.retain(sizeof(original_object_event_population) + 256U);
				original_object_event_population p;
				p.compile_unit = copy(unit[0]);
				p.universe = copy(unit[1]);
				p.variant = copy(unit[2]);
				p.interpretation = copy(unit[3]);
				p.stream = copy(prefix);
				p.profile = copy(profile);
				p.enumeration_state = finite_population_state::complete;
				reference(0U, p.compile_unit, p, p.enumeration_state);
				const auto& all = maps[14U];
				const annotated_row* inventory_row{};
				std::size_t evidence_owner{};
				for (const auto& [actual, entries] : all)
				{
					b.work();
					if (actual[1] != p.universe || actual[2] != p.variant ||
						actual[3] != p.interpretation)
						continue;
					for (auto index : entries)
					{
						b.work();
						if (text(*originals[index], "compile_unit") != p.compile_unit)
							continue;
						bind(p.evidence, {index});
						if (inventory_row && !equal({index, evidence_owner}))
							gap(p.enumeration_state,
								p.gaps,
								p.compile_unit,
								"inventory-conflicting",
								true);
						else
						{
							inventory_row = originals[index];
							evidence_owner = index;
						}
					}
				}
				if (!complete || !input.inventory_inputs_complete)
					gap(p.enumeration_state, p.gaps, p.compile_unit, "scan-unavailable");
				if (!inventory_row)
				{
					gap(p.enumeration_state, p.gaps, p.compile_unit, "inventory-missing");
					output.populations.push_back(std::move(p));
					return;
				}
				const std::string field{prefix};
				p.member_count = number(*inventory_row, field + "_count");
				if (!present(*inventory_row, field + "_ids") || !p.member_count ||
					text(*inventory_row, field + "_state") != "complete" ||
					text(*inventory_row, field + "_profile") != profile)
					gap(p.enumeration_state, p.gaps, p.compile_unit, "inventory-frontier");
				p.member_ids = members(*inventory_row, field + "_ids");
				if (p.member_count && *p.member_count != p.member_ids.size())
					gap(p.enumeration_state,
						p.gaps,
						p.compile_unit,
						"inventory-count-conflicting",
						true);
				for (const auto& id : p.member_ids)
				{
					const auto* member = reference(group, id, p, p.enumeration_state);
					if (member &&
						(text(*member, "compile_unit") != p.compile_unit ||
						 text(*member, "profile") != profile ||
						 text(*member, "membership_state") != "complete"))
						gap(p.enumeration_state, p.gaps, id, "member-membership-conflicting", true);
				}
				for (const auto& [actual, entries] : maps[group])
				{
					b.work();
					if (actual[1] != p.universe || actual[2] != p.variant ||
						actual[3] != p.interpretation)
						continue;
					for (auto index : entries)
					{
						b.work();
						if (text(*originals[index], "compile_unit") == p.compile_unit &&
							!std::ranges::binary_search(p.member_ids, actual[0]))
							gap(p.enumeration_state,
								p.gaps,
								actual[0],
								"extra-member-conflicting",
								true);
					}
				}
				output.populations.push_back(std::move(p));
			}
		};
		using borrowed_groups = std::array<std::vector<const annotated_row*>, 17>;
		result<object_semantics_projection> project_rows(object_semantics_input input,
														 budget& b,
														 const borrowed_groups* borrowed = nullptr)
		{
			return capture<object_semantics_projection>(
				[&]()
				{
					b.work();
					projector p{b, input, {}, {}, {}, {}, {}};
					const std::array groups{input.compile_units,
											input.files,
											input.spans,
											input.entities,
											input.details,
											input.declarations,
											input.bodies,
											input.cfg_nodes,
											input.cfg_edges,
											input.types,
											input.syntax_nodes,
											input.sequence_contexts,
											input.sequence_pairs,
											input.object_states,
											input.declaration_inventories,
											input.evaluation_roots,
											std::span<const annotated_row>{}};
					const auto descriptors = standard_relation_descriptors();
					const auto visit = [&](std::size_t group, auto&& callback)
					{
						if (borrowed)
						{
							for (const auto* row : (*borrowed)[group])
								callback(*row);
						}
						else
						{
							for (const auto& row : groups[group])
								callback(row);
						}
					};
					for (std::size_t group{}; group < groups.size(); ++group)
					{
						const auto d = std::ranges::find(
							descriptors, relations[group], &relation_descriptor::id);
						if (d == descriptors.end())
							fail(relations[group], "descriptor-missing");
						visit(group,
							  [&](const annotated_row& row)
							  {
								  b.work();
								  b.charge(b.rows, 1U, b.limits.maximum_rows, "rows");
								  if (auto valid = detail::validate_projected_relation_row(
										  row,
										  *d,
										  "sdk.object-input-invalid",
										  [&]
										  {
											  b.work();
										  });
									  !valid)
									  throw failure{valid.error()};
								  b.charge(b.conditions,
										   row.presence.fragments.size(),
										   b.limits.maximum_condition_expansions,
										   "conditions");
								  const auto id = text(row, identifiers[group]);
								  if (id.empty())
									  fail(relations[group], "identity-missing");
								  // Validate every original, retain bounded borrowed metadata, and
								  // compare cells only when an actual reference reaches an ID.
								  // No canonical payload is copied for unrelated lexical rows.
								  b.retain(sizeof(const annotated_row*) + sizeof(std::size_t) +
										   32U);
								  const auto index = p.originals.size();
								  p.originals.push_back(&row);
								  p.groups.push_back(group);
								  for (const auto& variant : row.presence.fragments)
								  {
									  b.work();
									  b.retain(id.size() + row.presence.universe.size() +
											   variant.size() + row.interpretation.size() + 256U);
									  p.maps[group][key(id, row, variant)].push_back(index);
								  }
							  });
					}
					for (const auto& [actual, entries] : p.maps[8U])
					{
						if (std::ranges::any_of(
								entries,
								[&](auto i)
								{
									return present(*p.originals[i], "function_exit_kind") ||
										present(*p.originals[i], "function_exit_profile");
								}))
							p.exit_facet(actual);
					}
					for (const auto& [actual, entries] : p.maps[10U])
					{
						if (std::ranges::any_of(
								entries,
								[&](auto i)
								{
									return present(*p.originals[i], "object_fact_kind") ||
										present(*p.originals[i], "object_fact_profile");
								}))
							p.syntax_facet(actual);
					}
					for (const auto& [actual, entries] : p.maps[12U])
					{
						(void)entries;
						p.sequence(actual);
					}
					for (const auto& [actual, entries] : p.maps[13U])
					{
						(void)entries;
						p.object(actual);
					}
					for (const auto& [unit, entries] : p.maps[0U])
					{
						(void)entries;
						p.inventory(unit,
									"sequence_pair",
									12U,
									input.sequence_inputs_complete,
									"clang22-original-sequence-checker-candidates/1");
						p.inventory(unit,
									"object_state",
									13U,
									input.object_state_inputs_complete,
									"clang22-legacy-interpreter-object-state/1");
					}
					return std::move(p.output);
				});
		}
	} // namespace
	result<object_semantics_projection> project_object_semantics(object_semantics_input input,
																 finite_population_limits limits,
																 std::stop_token stop,
																 projection_resource_usage& usage)
	{
		usage = {};
		if (auto valid = limits.validate(); !valid)
			return valid.error();
		budget b{limits, stop};
		auto result = project_rows(input, b);
		if (result)
			usage = {b.operations, b.retained};
		return result;
	}
	result<object_semantics_projection> project_object_semantics(object_semantics_input input,
																 finite_population_limits limits,
																 std::stop_token stop)
	{
		projection_resource_usage usage;
		return project_object_semantics(input, limits, stop, usage);
	}
	result<object_semantics_projection>
	project_object_semantics(const application_query_results& input,
							 finite_population_limits limits,
							 std::stop_token stop,
							 projection_resource_usage& usage)
	{
		usage = {};
		if (auto valid = limits.validate(); !valid)
			return valid.error();
		budget b{limits, stop};
		auto result = capture<object_semantics_projection>(
			[&]() -> object_semantics_projection
			{
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
																  "sdk.object",
																  &plan_bytes,
																  active);
					!valid)
					throw failure{valid.error()};
				borrowed_groups groups;
				std::array<bool, 17> seen{}, complete{};
				complete.fill(true);
				for (const auto& scan : input.scans)
				{
					b.work();
					const auto at = std::ranges::find(relations, scan.relation_id);
					if (at == relations.end())
						continue;
					const auto group = static_cast<std::size_t>(at - relations.begin());
					seen[group] = true;
					complete[group] &= scan.result.execution() == execution_status::complete &&
						scan.result.conflicts().empty() &&
						scan.result.differential_disagreements().empty();
					const auto rows = query_transfer_access::borrow_rows(scan.result);
					b.charge(b.rows, rows.size(), limits.maximum_rows, "scan-rows");
					b.retain(rows.size() * sizeof(const annotated_row*));
					groups[group].reserve(groups[group].size() + rows.size());
					for (const auto& row : rows)
					{
						b.work();
						groups[group].push_back(&row);
					}
				}
				object_semantics_input raw{};
				raw.source_inputs_complete = seen[1] && seen[2] && complete[1] && complete[2];
				raw.owner_inputs_complete =
					seen[0] && seen[3] && seen[5] && complete[0] && complete[3] && complete[5];
				raw.body_inputs_complete = seen[6] && complete[6];
				raw.cfg_inputs_complete = seen[7] && seen[8] && complete[7] && complete[8];
				raw.type_inputs_complete = seen[9] && complete[9];
				raw.syntax_inputs_complete = seen[10] && complete[10];
				raw.sequence_inputs_complete = seen[11] && seen[12] && complete[11] && complete[12];
				raw.object_state_inputs_complete = seen[13] && complete[13];
				raw.inventory_inputs_complete = seen[14] && complete[14];
				raw.evaluation_inputs_complete = seen[15] && complete[15];
				b.rows = 0;
				auto projected = project_rows(raw, b, &groups);
				if (!projected)
					throw failure{projected.error()};
				b.retain(plan_bytes + sizeof(application_query_results));
				projected->source_queries = input;
				return std::move(*projected);
			});
		if (result)
			usage = {b.operations, b.retained};
		return result;
	}
	result<object_semantics_projection>
	project_object_semantics(const application_query_results& input,
							 finite_population_limits limits,
							 std::stop_token stop)
	{
		projection_resource_usage usage;
		return project_object_semantics(input, limits, stop, usage);
	}
} // namespace cxxlens::sdk::query
