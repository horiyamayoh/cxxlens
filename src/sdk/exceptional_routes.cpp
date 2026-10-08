#include <algorithm>
#include <array>
#include <bit>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

#include <cxxlens/relations/build_compile_unit.hpp>
#include <cxxlens/relations/cc_body.hpp>
#include <cxxlens/relations/cc_declaration.hpp>
#include <cxxlens/relations/cc_declaration_inventory.hpp>
#include <cxxlens/relations/cc_entity.hpp>
#include <cxxlens/relations/cc_entity_detail.hpp>
#include <cxxlens/relations/cc_exceptional_block.hpp>
#include <cxxlens/relations/cc_exceptional_exit.hpp>
#include <cxxlens/relations/cc_exceptional_successor.hpp>
#include <cxxlens/relations/cc_syntax_node.hpp>
#include <cxxlens/relations/source_file.hpp>
#include <cxxlens/relations/source_span.hpp>
#include <cxxlens/sdk/exceptional_routes.hpp>

#include "query_projection_plan_limits_internal.hpp"
#include "query_projection_rows_internal.hpp"
#include "query_result_internal.hpp"
namespace cxxlens::sdk::query
{
	namespace
	{
		using refs = std::vector<std::size_t>;
		using rows = std::vector<const annotated_row*>;
		using groups = std::array<rows, 12>;
		constexpr std::array<std::string_view, 12> relations{"build.compile_unit.v1",
															 "source.file.v1",
															 "source.span.v1",
															 "cc.entity.v1",
															 "cc.entity_detail.v1",
															 "cc.body.v1",
															 "cc.syntax_node.v1",
															 "cc.declaration.v1",
															 "cc.declaration_inventory.v1",
															 "cc.exceptional_exit.v1",
															 "cc.exceptional_block.v1",
															 "cc.exceptional_successor.v1"};
		constexpr std::array<std::string_view, 12> identifiers{"compile_unit",
															   "snapshot",
															   "span",
															   "entity",
															   "detail",
															   "body",
															   "node",
															   "declaration",
															   "inventory",
															   "exit",
															   "block",
															   "successor"};
		constexpr std::string_view topology_profile = "clang22-original-lowering-topology/1",
								   boundary_profile = "clang22-original-invoke-eh-boundary/1",
								   spec_profile = "clang22-function-exception-specification/1",
								   occurrence_profile =
									   "clang22-original-exceptional-occurrences/1",
								   lowering_profile =
									   "clang22-written-definition-analysis-lowering/1";
		struct failure
		{
			error value;
		};
		[[noreturn]] void fail(std::string_view field,
							   std::string_view reason,
							   std::string_view code = "sdk.exceptional-route-input-invalid")
		{
			throw failure{{std::string{code}, std::string{field}, std::string{reason}}};
		}
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
			const auto* v =
				c && present(row, name) ? std::get_if<std::string>(&*c->value) : nullptr;
			return v ? std::string_view{*v} : std::string_view{};
		}
		const std::vector<std::byte>* bytes(const annotated_row& row, std::string_view name)
		{
			const auto* c = cell(row, name);
			return c && present(row, name) ? std::get_if<std::vector<std::byte>>(&*c->value)
										   : nullptr;
		}
		std::optional<std::uint64_t> number(const annotated_row& row, std::string_view name)
		{
			const auto* c = cell(row, name);
			const auto* v =
				c && present(row, name) ? std::get_if<std::uint64_t>(&*c->value) : nullptr;
			return v ? std::optional{*v} : std::nullopt;
		}
		std::optional<bool> boolean(const annotated_row& row, std::string_view name)
		{
			const auto* c = cell(row, name);
			const auto* v = c && present(row, name) ? std::get_if<bool>(&*c->value) : nullptr;
			return v ? std::optional{*v} : std::nullopt;
		}
		using identity = std::array<std::string, 4>;
		using view_identity = std::array<std::string_view, 4>;
		struct budget;
		int compare_identity_text(budget& meter, std::string_view left, std::string_view right);
		struct identity_less
		{
			using is_transparent = void;
			budget* meter{};
			template <class L, class R>
			bool operator()(const L& l, const R& r) const
			{
				for (std::size_t i = 0; i < 4U; ++i)
				{
					if (meter)
					{
						const auto order = compare_identity_text(*meter, l[i], r[i]);
						if (order != 0)
							return order < 0;
						continue;
					}
					if (l[i] < r[i])
						return true;
					if (r[i] < l[i])
						return false;
				}
				return false;
			}
		};
		finite_population_state combine(finite_population_state a, finite_population_state b)
		{
			if (a == finite_population_state::conflicting ||
				b == finite_population_state::conflicting)
				return finite_population_state::conflicting;
			if (a == finite_population_state::complete && b == finite_population_state::complete)
				return finite_population_state::complete;
			if (a == finite_population_state::unknown && b == finite_population_state::unknown)
				return finite_population_state::unknown;
			return finite_population_state::partial;
		}

		struct budget
		{
			finite_population_limits limits;
			std::stop_token stop;
			std::size_t retained{}, temporary_peak{}, evidence{}, references{}, operations{},
				conditions{}, members{}, rows{}, populations{};
			void charge(std::size_t& used,
						std::size_t amount,
						std::size_t maximum,
						std::string_view field)
			{
				if (used > maximum || amount > maximum - used)
					fail(field, "limit-exceeded", "sdk.exceptional-route-budget");
				used += amount;
			}
			void work(std::size_t n = 1)
			{
				if (stop.stop_requested() || (limits.cancelled && limits.cancelled()))
					fail("projection", "stop-requested", "sdk.exceptional-route-cancelled");
				charge(operations, n, limits.maximum_operations, "operations");
			}
			bool canonical_less(std::string_view left, std::string_view right)
			{
				work();
				const auto common = std::min(left.size(), right.size());
				for (std::size_t i{}; i < common; ++i)
				{
					work(2U);
					const auto a = static_cast<unsigned char>(left[i]);
					const auto b = static_cast<unsigned char>(right[i]);
					if (a != b)
						return a < b;
				}
				return left.size() < right.size();
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
			std::size_t estimate(const annotated_row& row,
								 bool encoding = true,
								 std::size_t* validation_payload = nullptr)
			{
				std::size_t total = 2048;
				const auto add = [&](std::size_t n, std::size_t factor = 8U)
				{
					if (!encoding)
						factor = 2U;
					// Geometry reads lengths; encoding and owned copies charge their bytes.
					work();
					if (total > limits.maximum_retained_bytes ||
						n > (limits.maximum_retained_bytes - total) / factor)
						fail("row", "limit-exceeded", "sdk.exceptional-route-budget");
					total += n * factor;
					if (validation_payload)
					{
						if (n > limits.maximum_operations - *validation_payload)
							fail("operations", "limit-exceeded", "sdk.exceptional-route-budget");
						*validation_payload += n;
					}
				};
				const auto fixed = [&](std::size_t n)
				{
					work();
					if (total > limits.maximum_retained_bytes ||
						n > limits.maximum_retained_bytes - total)
						fail("row", "limit-exceeded", "sdk.exceptional-route-budget");
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
		int compare_identity_text(budget& meter, std::string_view left, std::string_view right)
		{
			meter.work();
			const auto common = std::min(left.size(), right.size());
			for (std::size_t i{}; i < common; ++i)
			{
				meter.work(2U);
				const auto a = static_cast<unsigned char>(left[i]);
				const auto b = static_cast<unsigned char>(right[i]);
				if (a != b)
					return a < b ? -1 : 1;
			}
			return left.size() < right.size() ? -1 : (left.size() > right.size() ? 1 : 0);
		}

		struct projector
		{
			budget& b;
			groups& input;
			const exceptional_route_input& admission;
			exceptional_route_projection output;
			std::array<std::map<view_identity, rows, identity_less>, 12> index;
			std::map<const annotated_row*, std::size_t> owned;
			bool row_validation_reused{};
			std::string copy(std::string_view value)
			{
				b.work(value.size() + 1U);
				b.retain(2U * value.size() + sizeof(std::string));
				return std::string{value};
			}
			void lookup_work(const view_identity& key, std::size_t size)
			{
				const auto factor = static_cast<std::size_t>(std::bit_width(size)) + 1U;
				for (auto value : key)
				{
					if (value.size() + 1U > b.limits.maximum_operations / factor)
						fail("operations", "limit-exceeded", "sdk.exceptional-route-budget");
					b.work((value.size() + 1U) * factor);
				}
			}
			const rows& find(std::size_t group, std::string_view id, const view_identity& world)
			{
				static const rows empty;
				if (id.empty())
					return empty;
				view_identity key{id, world[1], world[2], world[3]};
				b.work();
				const auto at = index[group].find(key);
				return at == index[group].end() ? empty : at->second;
			}
			void peak(std::size_t amount)
			{
				if (amount > b.limits.maximum_retained_bytes - b.retained)
					fail("temporary", "limit-exceeded", "sdk.exceptional-route-budget");
				b.temporary_peak = std::max(b.temporary_peak, b.retained + amount);
			}
			std::string canonical(const annotated_row& row)
			{
				peak(2U * b.estimate(row));
				auto value = row.canonical_form();
				b.work(value.size() + 1U);
				return value;
			}
			void initialize()
			{
				for (auto& group : index)
					group = std::map<view_identity, rows, identity_less>{identity_less{&b}};
				const std::array<const relation_descriptor*, 12> descriptors{
					&build::relations::compile_unit::descriptor(),
					&source::relations::file::descriptor(),
					&source::relations::span::descriptor(),
					&cc::relations::entity::descriptor(),
					&cc::relations::entity_detail::descriptor(),
					&cc::relations::body::descriptor(),
					&cc::relations::syntax_node::descriptor(),
					&cc::relations::declaration::descriptor(),
					&cc::relations::declaration_inventory::descriptor(),
					&cc::relations::exceptional_exit::descriptor(),
					&cc::relations::exceptional_block::descriptor(),
					&cc::relations::exceptional_successor::descriptor()};
				for (std::size_t group = 0; group < input.size(); ++group)
				{
					const auto* d = descriptors[group];
					for (const auto* row : input[group])
					{
						b.work();
						b.charge(b.rows, 1U, b.limits.maximum_rows, "rows");
						b.charge(b.conditions,
								 row->presence.fragments.size(),
								 b.limits.maximum_condition_expansions,
								 "conditions");
						// Inspect every original field/container, including rows whose optional
						// facet is absent. Length inspection does not traverse payload bytes.
						std::size_t validation_payload{};
						(void)b.estimate(
							*row, false, row_validation_reused ? nullptr : &validation_payload);
						// Generic row validation traverses values and annotated text.
						b.work(validation_payload);
						if (auto valid = detail::validate_projected_relation_row(
								*row,
								*d,
								"sdk.exceptional-route-input-invalid",
								[&]
								{
									b.work();
								},
								row_validation_reused);
							!valid)
							throw failure{valid.error()};
						const auto id = text(*row, identifiers[group]);
						if (id.empty())
							fail(relations[group], "identity-missing");
						for (const auto& variant : row->presence.fragments)
						{
							view_identity key{
								id, row->presence.universe, variant, row->interpretation};
							b.work();
							// Conservative capacity/node allowance before both map and vector
							// growth.
							b.retain(sizeof(view_identity) + 256U +
									 2U * sizeof(const annotated_row*));
							index[group][key].push_back(row);
						}
					}
				}
				std::size_t retained_keys{};
				{
					// Only populations with alternatives need ordering keys. One borrowed
					// original can belong to several worlds; encode it once across them.
					const auto pointer_less =
						[&](const annotated_row* left, const annotated_row* right)
					{
						b.work();
						return std::less<const annotated_row*>{}(left, right);
					};
					std::map<const annotated_row*, std::string, decltype(pointer_less)> keys{
						pointer_less};
					for (auto& group : index)
						for (auto& [key, alternatives] : group)
						{
							(void)key;
							b.work();
							if (alternatives.size() < 2U)
								continue;
							for (const auto* original : alternatives)
							{
								b.work();
								if (keys.find(original) != keys.end())
									continue;
								constexpr auto node = sizeof(decltype(keys)::value_type) + 256U;
								b.retain(node);
								auto encoded = canonical(*original);
								const auto buffer = encoded.capacity() + 1U;
								b.retain(buffer);
								retained_keys += node + buffer;
								keys.emplace(original, std::move(encoded));
							}
							std::ranges::sort(alternatives,
											  [&](const auto* left, const auto* right)
											  {
												  return b.canonical_less(keys.at(left),
																		  keys.at(right));
											  });
						}
				}
				// The map and string buffers have expired before their reservation is
				// refunded; the observed scratch peak remains part of returned usage.
				b.retained -= retained_keys;
			}
			void
			bind(refs& evidence, std::size_t group, std::span<const annotated_row* const> originals)
			{
				for (const auto* r : originals)
				{
					b.work(static_cast<std::size_t>(std::bit_width(owned.size())) + 1U);
					auto at = owned.find(r);
					if (at == owned.end())
					{
						b.retain(b.estimate(*r, false) + 2U * sizeof(finite_population_evidence) +
								 2U * relations[group].size() + 128U);
						auto encoded = canonical(*r);
						b.charge(b.evidence,
								 encoded.size(),
								 b.limits.maximum_evidence_bytes,
								 "evidence-bytes");
						const auto ref = output.evidence.size();
						// Canonical bytes conservatively bound the actual row payload copy.
						b.work(encoded.size() + relations[group].size() + 1U);
						output.evidence.push_back({std::string{relations[group]}, *r});
						b.retain(128U + sizeof(std::pair<const annotated_row* const, std::size_t>));
						at = owned.emplace(r, ref).first;
					}
					b.charge(b.references,
							 1U,
							 b.limits.maximum_evidence_references,
							 "evidence-references");
					b.retain(2U * sizeof(std::size_t));
					evidence.push_back(at->second);
				}
			}
			bool agree(const rows& originals, std::initializer_list<std::string_view> fields)
			{
				// Unobserved optional columns are not a contrary observation. Comparing
				// only actual present observations also keeps unrelated flags out of this
				// axis.
				for (auto name : fields)
				{
					const detached_cell* first = nullptr;
					for (const auto* r : originals)
					{
						b.work();
						const auto* value = cell(*r, name);
						if (!present(*r, name))
							continue;
						b.work(name.size() + value->type.parameter.size() + 1U);
						if (value->value)
							std::visit(
								[&](const auto& v)
								{
									if constexpr (requires { v.size(); })
										b.work(v.size());
								},
								*value->value);
						if (!first)
							first = value;
						else if (first->type != value->type || first->value != value->value)
							return false;
					}
				}
				return true;
			}
			const annotated_row& representative(const rows& original,
												std::initializer_list<std::string_view> fields)
			{
				const annotated_row* best = original.front();
				std::size_t most{};
				for (const auto* r : original)
				{
					std::size_t count{};
					for (auto field : fields)
					{
						b.work();
						count += present(*r, field) ? 1U : 0U;
					}
					if (count > most)
					{
						most = count;
						best = r;
					}
				}
				return *best;
			}
			template <class T>
			void gap(T& into, std::string_view subject, std::string_view reason)
			{
				b.work();
				b.retain(2U * (sizeof(query_unresolved) + subject.size() + reason.size() + 64U));
				into.gaps.push_back(
					{"sdk.exceptional-route-" + std::string{reason}, std::string{subject}, {}});
			}
			using state = finite_population_state;
			state observation(std::string_view value)
			{
				return value == "complete" ? state::complete
					: value.empty()		   ? state::unknown
										   : state::partial;
			}
			void world_fields(auto& value, const view_identity& world)
			{
				value.universe = copy(world[1]);
				value.semantic_variant = copy(world[2]);
				value.interpretation = copy(world[3]);
			}
			state unit(std::string_view id, const view_identity& world, refs& evidence)
			{
				const auto& candidates = find(0U, id, world);
				if (candidates.empty())
					return state::unknown;
				bind(evidence, 0U, candidates);
				return agree(candidates, {"compile_unit"}) ? state::complete : state::conflicting;
			}
			state carrier(std::string_view id,
						  std::string_view unit_id,
						  const view_identity& world,
						  refs& evidence)
			{
				const auto& candidates = find(9U, id, world);
				if (candidates.empty())
					return state::unknown;
				bind(evidence, 9U, candidates);
				if (!agree(
						candidates,
						{"exit", "role", "ordinal", "compile_unit", "profile", "lowering_profile"}))
					return state::conflicting;
				const auto& r = representative(
					candidates, {"role", "ordinal", "compile_unit", "profile", "lowering_profile"});
				if ((!text(r, "compile_unit").empty() && !unit_id.empty() &&
					 text(r, "compile_unit") != unit_id) ||
					(number(r, "ordinal") && number(r, "ordinal") != 0U))
					return state::conflicting;
				const auto role = text(r, "role");
				if (role == "written_throw" || role == "escaping_call" ||
					role == "unhandled_resume" || role == "termination" ||
					role == "lowering_helper" || role == "ordinary_instruction")
					return state::conflicting;
				return role == "lowering_variant" && number(r, "ordinal") && !unit_id.empty() &&
						text(r, "compile_unit") == unit_id &&
						text(r, "profile") == occurrence_profile &&
						text(r, "lowering_profile") == lowering_profile
					? state::complete
					: state::partial;
			}
			state occurrence_scope(const rows& occurrences,
								   std::string_view variant_id,
								   const view_identity& world,
								   refs& evidence)
			{
				const auto& variants = find(9U, variant_id, world);
				if (variants.empty())
					return state::unknown;
				bind(evidence, 9U, variants);
				const std::initializer_list<std::string_view> fields = {"scope_detail",
																		"function",
																		"compile_unit",
																		"body",
																		"definition_source",
																		"variant_kind",
																		"variant_index",
																		"variant_symbol"};
				if (!agree(occurrences, fields) || !agree(variants, fields))
					return state::conflicting;
				auto result = state::complete;
				for (const auto field : fields)
				{
					const detached_cell* expected = nullptr;
					for (const auto* row : variants)
					{
						b.work();
						if (present(*row, field))
						{
							expected = cell(*row, field);
							break;
						}
					}
					bool observed = false;
					for (const auto* row : occurrences)
					{
						b.work();
						if (!present(*row, field))
							continue;
						observed = true;
						const auto* actual = cell(*row, field);
						if (expected)
						{
							b.work(field.size() + 1U);
							std::visit(
								[&](const auto& v)
								{
									if constexpr (requires { v.size(); })
										b.work(v.size());
								},
								*actual->value);
							std::visit(
								[&](const auto& v)
								{
									if constexpr (requires { v.size(); })
										b.work(v.size());
								},
								*expected->value);
							if (actual->type != expected->type || actual->value != expected->value)
								return state::conflicting;
						}
					}
					// Body/source are independently optional; neither is an identity
					// substitute.
					if ((!expected || !observed) && field != std::string_view{"body"} &&
						field != std::string_view{"definition_source"})
						result = combine(result, state::unknown);
				}
				return result;
			}
			state source(std::string_view id, const view_identity& world, refs& evidence)
			{
				const auto& originals = find(2U, id, world);
				if (originals.empty())
					return state::unknown;
				bind(evidence, 2U, originals);
				if (!agree(originals, {"span", "file", "snapshot", "begin", "end"}))
					return state::conflicting;
				const auto& row = *originals.front();
				const auto& files = find(1U, text(row, "snapshot"), world);
				if (files.empty())
					return state::partial;
				bind(evidence, 1U, files);
				if (!agree(files, {"snapshot", "file", "size"}))
					return state::conflicting;
				const auto begin = number(row, "begin"), end = number(row, "end"),
						   size = number(*files.front(), "size");
				if (!begin || !end || !size)
					return state::partial;
				return *begin <= *end && *end <= *size &&
						text(row, "file") == text(*files.front(), "file")
					? state::complete
					: state::conflicting;
			}
			bool callable(std::string_view kind) const
			{
				return kind == "function" || kind == "method" || kind == "constructor" ||
					kind == "destructor" || kind == "conversion";
			}
			state scope(exceptional_route_variant& value,
						const view_identity& world,
						const rows& originals)
			{
				auto result = unit(value.compile_unit, world, value.evidence);
				if (!agree(
						originals,
						{"scope_detail", "function", "compile_unit", "body", "definition_source"}))
					return state::conflicting;
				const auto& details = find(4U, value.detail, world);
				if (details.empty())
					result = combine(result, state::unknown);
				else
				{
					bind(value.evidence, 4U, details);
					if (!agree(details, {"entity", "compile_unit", "source"}))
						return state::conflicting;
					for (const auto* detail : details)
					{
						if ((!text(*detail, "entity").empty() && !value.function.empty() &&
							 text(*detail, "entity") != value.function) ||
							(!text(*detail, "compile_unit").empty() &&
							 text(*detail, "compile_unit") != value.compile_unit) ||
							(!text(*detail, "source").empty() && !value.definition_source.empty() &&
							 text(*detail, "source") != value.definition_source))
							return state::conflicting;
						if (text(*detail, "entity").empty() ||
							text(*detail, "compile_unit").empty())
							result = combine(result, state::unknown);
					}
				}
				const auto& entities = find(3U, value.function, world);
				if (entities.empty())
					result = combine(result, state::unknown);
				else
				{
					bind(value.evidence, 3U, entities);
					if (!agree(entities, {"entity", "kind"}))
						return state::conflicting;
					if (!callable(text(*entities.front(), "kind")))
					{
						const auto kind = text(*entities.front(), "kind");
						if (kind == "variable" || kind == "parameter" || kind == "field" ||
							kind == "record" || kind == "enum" || kind == "enumerator" ||
							kind == "namespace" || kind == "type_alias")
							return state::conflicting;
						result = combine(result, state::unknown);
					}
				}
				if (!value.body.empty())
				{
					const auto& bodies = find(5U, value.body, world);
					if (bodies.empty())
						result = combine(result, state::unknown);
					else
					{
						bind(value.evidence, 5U, bodies);
						if (!agree(bodies, {"body", "function", "compile_unit"}))
							return state::conflicting;
						for (const auto* body : bodies)
						{
							if ((!text(*body, "function").empty() && !value.function.empty() &&
								 text(*body, "function") != value.function) ||
								(!text(*body, "compile_unit").empty() &&
								 !value.compile_unit.empty() &&
								 text(*body, "compile_unit") != value.compile_unit))
								return state::conflicting;
							if (text(*body, "function").empty() ||
								text(*body, "compile_unit").empty())
								result = combine(result, state::unknown);
						}
					}
				}
				return result;
			}
			void blocks()
			{
				for (const auto& [world, original] : index[10U])
				{
					const auto& r = *original.front();
					b.charge(b.members, 1U, b.limits.maximum_members, "blocks");
					b.retain(2U * sizeof(observed_exceptional_block));
					observed_exceptional_block value;
					value.block = copy(world[0]);
					value.variant = copy(text(r, "variant"));
					value.compile_unit = copy(text(r, "compile_unit"));
					value.profile = copy(text(r, "profile"));
					value.membership_state = copy(text(r, "membership_state"));
					world_fields(value, world);
					value.ordinal = number(r, "ordinal").value_or(0U);
					value.instruction_count = number(r, "instruction_count").value_or(0U);
					value.is_entry = boolean(r, "is_entry").value_or(false);
					value.terminator_opcode = number(r, "terminator_opcode");
					value.terminator_kind = copy(text(r, "terminator_kind"));
					bind(value.evidence, 10U, original);
					value.identity_state = agree(original,
												 {"block",
												  "variant",
												  "compile_unit",
												  "profile",
												  "ordinal",
												  "instruction_count",
												  "is_entry",
												  "membership_state",
												  "terminator_opcode",
												  "terminator_kind"})
						? !value.variant.empty() && !value.compile_unit.empty() &&
								value.profile == topology_profile && number(r, "ordinal") &&
								number(r, "instruction_count") && boolean(r, "is_entry")
							? observation(value.membership_state)
							: state::partial
						: state::conflicting;
					value.variant_state =
						carrier(value.variant, value.compile_unit, world, value.evidence);
					value.state = combine(value.identity_state, value.variant_state);
					if (value.state != state::complete)
						gap(value, value.block, "block-unavailable");
					output.blocks.push_back(std::move(value));
				}
			}
			state block_fk(std::string_view id,
						   std::string_view variant_id,
						   std::string_view unit_id,
						   const view_identity& world,
						   refs& evidence,
						   std::optional<std::uint64_t> instruction = {})
			{
				const auto& candidates = find(10U, id, world);
				if (candidates.empty())
					return state::unknown;
				bind(evidence, 10U, candidates);
				if (!agree(candidates,
						   {"block",
							"variant",
							"compile_unit",
							"ordinal",
							"instruction_count",
							"profile",
							"membership_state"}))
					return state::conflicting;
				const auto& r = representative(candidates,
											   {"variant",
												"compile_unit",
												"ordinal",
												"instruction_count",
												"profile",
												"membership_state"});
				if ((!text(r, "variant").empty() && !variant_id.empty() &&
					 text(r, "variant") != variant_id) ||
					(!text(r, "compile_unit").empty() && !unit_id.empty() &&
					 text(r, "compile_unit") != unit_id) ||
					(instruction && number(r, "instruction_count") &&
					 *instruction >= *number(r, "instruction_count")))
					return state::conflicting;
				return text(r, "profile") == topology_profile && !variant_id.empty() &&
						!unit_id.empty() && text(r, "variant") == variant_id &&
						text(r, "compile_unit") == unit_id && number(r, "ordinal") &&
						number(r, "instruction_count")
					? observation(text(r, "membership_state"))
					: state::partial;
			}
			state invoke_fk(std::string_view id,
							std::string_view variant_id,
							std::string_view unit_id,
							const view_identity& world,
							refs& evidence,
							std::string_view block_id,
							std::optional<std::uint64_t> instruction)
			{
				const auto& candidates = find(9U, id, world);
				if (candidates.empty())
					return state::unknown;
				bind(evidence, 9U, candidates);
				if (!agree(candidates,
						   {"exit",
							"variant",
							"compile_unit",
							"is_invoke",
							"lowered_block",
							"instruction_ordinal",
							"block_ordinal"}))
					return state::conflicting;
				const auto& r = representative(
					candidates, {"is_invoke", "lowered_block", "instruction_ordinal"});
				if ((!text(r, "variant").empty() && !variant_id.empty() &&
					 text(r, "variant") != variant_id) ||
					(!text(r, "compile_unit").empty() && !unit_id.empty() &&
					 text(r, "compile_unit") != unit_id) ||
					boolean(r, "is_invoke") == false ||
					(!text(r, "lowered_block").empty() && text(r, "lowered_block") != block_id) ||
					(instruction && number(r, "instruction_ordinal") &&
					 instruction != number(r, "instruction_ordinal")))
					return state::conflicting;
				auto result = !variant_id.empty() && !unit_id.empty() &&
						text(r, "variant") == variant_id && text(r, "compile_unit") == unit_id &&
						boolean(r, "is_invoke") == true && text(r, "lowered_block") == block_id &&
						instruction && instruction == number(r, "instruction_ordinal")
					? state::complete
					: state::partial;
				result = combine(result, occurrence_scope(candidates, variant_id, world, evidence));
				const auto& blocks = find(10U, block_id, world);
				if (blocks.empty())
					return combine(result, state::unknown);
				const auto ordinal = number(representative(blocks, {"ordinal"}), "ordinal");
				if (ordinal && number(r, "block_ordinal") && ordinal != number(r, "block_ordinal"))
					return state::conflicting;
				if (!ordinal || !number(r, "block_ordinal"))
					result = combine(result, state::unknown);
				return result;
			}
			void successors()
			{
				for (const auto& [world, original] : index[11U])
				{
					const auto& r = *original.front();
					b.charge(b.members, 1U, b.limits.maximum_members, "successors");
					b.retain(2U * sizeof(observed_exceptional_successor));
					observed_exceptional_successor value;
					value.successor = copy(world[0]);
					value.variant = copy(text(r, "variant"));
					value.compile_unit = copy(text(r, "compile_unit"));
					value.profile = copy(text(r, "profile"));
					value.membership_state = copy(text(r, "membership_state"));
					world_fields(value, world);
					value.from_block = copy(text(r, "from_block"));
					value.to_block = copy(text(r, "to_block"));
					value.kind = copy(text(r, "kind"));
					value.invoke = copy(text(r, "invoke"));
					value.ordinal = number(r, "ordinal").value_or(0U);
					value.terminator_instruction_ordinal =
						number(r, "terminator_instruction_ordinal").value_or(0U);
					bind(value.evidence, 11U, original);
					value.identity_state = agree(original,
												 {"successor",
												  "variant",
												  "compile_unit",
												  "profile",
												  "from_block",
												  "to_block",
												  "kind",
												  "invoke",
												  "ordinal",
												  "terminator_instruction_ordinal",
												  "membership_state"})
						? !value.variant.empty() && !value.compile_unit.empty() &&
								value.profile == topology_profile && number(r, "ordinal") &&
								number(r, "terminator_instruction_ordinal") &&
								(value.kind == "normal" || value.kind == "unwind" ||
								 value.kind == "ordinary")
							? observation(value.membership_state)
							: state::partial
						: state::conflicting;
					value.variant_state =
						carrier(value.variant, value.compile_unit, world, value.evidence);
					value.from_state = block_fk(value.from_block,
												value.variant,
												value.compile_unit,
												world,
												value.evidence,
												number(r, "terminator_instruction_ordinal"));
					const auto& from_blocks = find(10U, value.from_block, world);
					if (!from_blocks.empty())
					{
						const auto count =
							number(representative(from_blocks, {"instruction_count"}),
								   "instruction_count");
						const auto instruction = number(r, "terminator_instruction_ordinal");
						if (count && instruction && (*count == 0U || *instruction != *count - 1U))
							value.from_state = state::conflicting;
						else if (!count || !instruction)
							value.from_state = combine(value.from_state, state::unknown);
						const auto& block = representative(from_blocks, {"terminator_kind"});
						if (text(block, "terminator_kind") == "invoke" && value.kind == "ordinary")
							value.from_state = state::conflicting;
						else if ((value.kind == "normal" || value.kind == "unwind") &&
								 text(block, "terminator_kind") != "invoke")
							value.from_state = combine(value.from_state, state::unknown);
					}
					value.to_state = block_fk(
						value.to_block, value.variant, value.compile_unit, world, value.evidence);
					if (!value.invoke.empty())
						value.invoke_state = invoke_fk(value.invoke,
													   value.variant,
													   value.compile_unit,
													   world,
													   value.evidence,
													   value.from_block,
													   number(r, "terminator_instruction_ordinal"));
					else if (value.kind == "ordinary")
						value.invoke_state = state::complete;
					if ((value.kind == "normal" && value.ordinal != 0U) ||
						(value.kind == "unwind" && value.ordinal != 1U) ||
						(value.kind == "ordinary" && !value.invoke.empty()))
						value.identity_state = state::conflicting;
					value.state = combine(combine(value.identity_state, value.variant_state),
										  combine(value.from_state, value.to_state));
					value.state = combine(value.state, value.invoke_state);
					if (value.state != state::complete)
						gap(value, value.successor, "successor-unavailable");
					output.successors.push_back(std::move(value));
				}
			}
			struct decoded_set
			{
				bool observed{}, duplicate{};
				std::size_t count{};
			};
			template <class Callback>
			decoded_set members(const annotated_row& row, std::string_view field, Callback emit)
			{
				const auto* values = bytes(row, field);
				if (!values)
					return {};
				decoded_set result{true, false, 0U};
				std::size_t position{}, charged{};
				struct release
				{
					budget& b;
					std::size_t& n;
					~release()
					{
						b.retained -= n;
					}
				} temporary{b, charged};
				std::set<std::string_view> seen;
				while (position < values->size())
				{
					b.work();
					if (values->size() - position < 4U)
						fail(field, "truncated-set");
					std::uint32_t length{};
					for (unsigned shift = 0U; shift < 32U; shift += 8U)
						length |= std::to_integer<std::uint32_t>((*values)[position++]) << shift;
					if (!length || length > values->size() - position)
						fail(field, "invalid-set-member");
					b.charge(b.members, 1U, b.limits.maximum_members, "set-members");
					const auto* data = reinterpret_cast<const char*>(values->data() + position);
					const std::string_view id{data, length};
					b.work((id.size() + 1U) *
						   (static_cast<std::size_t>(std::bit_width(seen.size())) + 1U));
					b.retain(128U + sizeof(std::string_view));
					charged += 128U + sizeof(std::string_view);
					b.temporary_peak = std::max(b.temporary_peak, b.retained);
					if (!seen.insert(id).second)
						result.duplicate = true;
					emit(id);
					++result.count;
					position += length;
				}
				return result;
			}
			state declaration(observed_invoke_exceptional_boundary& value,
							  const view_identity& world,
							  std::string_view function,
							  std::string_view definition_source)
			{
				const auto& original_declarations = find(7U, value.boundary_declaration, world);
				if (original_declarations.empty())
					return state::unknown;
				bind(value.evidence, 7U, original_declarations);
				if (!agree(original_declarations, {"declaration", "entity", "source", "kind"}))
					return state::conflicting;
				const auto& row = *original_declarations.front();
				const auto kind = text(row, "kind");
				if ((!function.empty() && !text(row, "entity").empty() &&
					 text(row, "entity") != function) ||
					(!definition_source.empty() && !text(row, "source").empty() &&
					 text(row, "source") != definition_source))
					return state::conflicting;
				if (kind != "Function" && kind != "CXXMethod" && kind != "CXXConstructor" &&
					kind != "CXXDestructor" && kind != "CXXConversion")
					return state::partial;
				bool admitted{}, incomplete{}, contradictory{};
				for (const auto& [key, inventories] : index[8U])
				{
					b.work(key[1].size() + world[1].size() + key[2].size() + world[2].size() +
						   key[3].size() + world[3].size() + 1U);
					if (key[1] != world[1] || key[2] != world[2] || key[3] != world[3])
						continue;
					for (const auto* inventory : inventories)
					{
						b.work(value.compile_unit.size() + text(*inventory, "compile_unit").size() +
							   1U);
						if (text(*inventory, "compile_unit") != value.compile_unit ||
							value.compile_unit.empty())
							continue;
						const auto check_membership =
							[&](std::string_view member_field,
								std::string_view count_field,
								std::string_view state_field,
								std::initializer_list<std::string_view> fields)
						{
							bool found{};
							const auto set = members(
								*inventory,
								member_field,
								[&](std::string_view member)
								{
									b.work(member.size() + value.boundary_declaration.size() + 1U);
									found |= member == value.boundary_declaration;
								});
							if (!found)
								return;
							bind(value.evidence, 8U, std::array{inventory});
							const auto count = number(*inventory, count_field);
							const auto enumeration = text(*inventory, state_field);
							const bool supported =
								enumeration == "complete" || enumeration == "partial";
							// Admission precedes identity failures. A partial census may
							// count unbound definitions, but its actual positive members
							// still prove this declaration's unit membership.
							contradictory |= set.duplicate || (count && *count < set.count) ||
								(count && enumeration == "complete" && *count != set.count) ||
								!agree(inventories, fields);
							admitted |= count.has_value() && supported;
							incomplete |= !count.has_value() || !supported;
						};
						if (text(*inventory, "physical_definition_profile") ==
							"clang22-original-physical-definitions/1")
							check_membership("physical_definition_ids",
											 "physical_definition_count",
											 "physical_definition_state",
											 {"compile_unit",
											  "physical_definition_count",
											  "physical_definition_ids",
											  "physical_definition_state",
											  "physical_definition_profile"});
						if (text(*inventory, "profile") ==
							"clang22-explicit-admitted-named-declarations/1")
							check_membership("declarations",
											 "declaration_count",
											 "enumeration_state",
											 {"compile_unit",
											  "profile",
											  "declaration_count",
											  "declarations",
											  "enumeration_state"});
					}
				}
				return contradictory ? state::conflicting
					: admitted && !function.empty() && !text(row, "entity").empty()
					? state::complete
					: admitted || incomplete ? state::partial
											 : state::unknown;
			}

			void specification(observed_invoke_exceptional_boundary& value,
							   const view_identity& world,
							   std::string_view detail)
			{
				const auto& originals = find(4U, detail, world);
				if (originals.empty())
					return;
				bind(value.evidence, 4U, originals);
				const auto& row = representative(originals,
												 {"exception_spec_kind",
												  "exception_spec_profile",
												  "exception_spec_state",
												  "exception_spec_nonthrowing"});
				value.exception_spec_kind = copy(text(row, "exception_spec_kind"));
				value.exception_spec_profile = copy(text(row, "exception_spec_profile"));
				value.exception_spec_nonthrowing = boolean(row, "exception_spec_nonthrowing");
				if (!agree(originals,
						   {"exception_spec_kind",
							"exception_spec_profile",
							"exception_spec_state",
							"exception_spec_nonthrowing"}))
				{
					value.exception_spec_state = state::conflicting;
					value.exception_spec_nonthrowing.reset();
					return;
				}
				if (value.exception_spec_profile != spec_profile)
				{
					value.exception_spec_state =
						value.exception_spec_profile.empty() ? state::unknown : state::partial;
					return;
				}
				const auto& kind = value.exception_spec_kind;
				const bool yes = kind == "dynamic_none" || kind == "no_throw" ||
					kind == "basic_noexcept" || kind == "noexcept_true";
				const bool no = kind == "none" || kind == "dynamic" || kind == "ms_any" ||
					kind == "noexcept_false";
				if (yes || no)
					value.exception_spec_state = !value.exception_spec_nonthrowing ? state::partial
						: *value.exception_spec_nonthrowing != yes		  ? state::conflicting
						: text(row, "exception_spec_state") == "complete" ? state::complete
																		  : state::partial;
				else if (kind == "dependent_noexcept" || kind == "unevaluated" ||
						 kind == "uninstantiated" || kind == "unparsed")
					value.exception_spec_state = value.exception_spec_nonthrowing ||
							text(row, "exception_spec_state") == "complete"
						? state::conflicting
						: state::partial;
				else if (kind == "no_prototype")
					value.exception_spec_state = value.exception_spec_nonthrowing ||
							text(row, "exception_spec_state") == "complete"
						? state::conflicting
						: state::unknown;
				else
					value.exception_spec_state = state::partial;
			}
			state successor_fk(std::string_view id,
							   const observed_invoke_exceptional_boundary& value,
							   const view_identity& world,
							   refs& evidence,
							   std::string_view kind,
							   std::uint64_t ordinal)
			{
				const auto& originals = find(11U, id, world);
				if (originals.empty())
					return state::unknown;
				bind(evidence, 11U, originals);
				if (!agree(originals,
						   {"successor",
							"compile_unit",
							"variant",
							"profile",
							"kind",
							"invoke",
							"from_block",
							"to_block",
							"ordinal",
							"terminator_instruction_ordinal",
							"membership_state"}))
					return state::conflicting;
				const auto& r = representative(originals,
											   {"compile_unit",
												"variant",
												"profile",
												"kind",
												"invoke",
												"from_block",
												"to_block",
												"ordinal",
												"terminator_instruction_ordinal",
												"membership_state"});
				const auto expected = {
					std::pair{std::string_view{"compile_unit"},
							  std::string_view{value.compile_unit}},
					std::pair{std::string_view{"variant"}, std::string_view{value.variant}},
					std::pair{std::string_view{"from_block"},
							  std::string_view{value.lowered_block}},
					std::pair{std::string_view{"invoke"}, std::string_view{value.exit}},
					std::pair{std::string_view{"kind"}, kind}};
				auto result = state::complete;
				for (const auto& [field, wanted] : expected)
				{
					b.work(text(r, field).size() + wanted.size() + 1U);
					if (!text(r, field).empty() && !wanted.empty() && text(r, field) != wanted)
						return state::conflicting;
					if (text(r, field).empty() || wanted.empty())
						result = combine(result, state::unknown);
				}
				const auto successor_ordinal = number(r, "ordinal"),
						   instruction = number(r, "terminator_instruction_ordinal");
				if ((successor_ordinal && *successor_ordinal != ordinal) ||
					(instruction && value.instruction_ordinal &&
					 instruction != value.instruction_ordinal))
					return state::conflicting;
				if (!successor_ordinal || !instruction || !value.instruction_ordinal)
					result = combine(result, state::unknown);
				result = combine(result,
								 block_fk(value.lowered_block,
										  value.variant,
										  value.compile_unit,
										  world,
										  evidence,
										  instruction));
				result = combine(
					result,
					block_fk(
						text(r, "to_block"), value.variant, value.compile_unit, world, evidence));
				const auto& from = find(10U, value.lowered_block, world);
				if (from.empty())
					return combine(result, state::unknown);
				bind(evidence, 10U, from);
				if (!agree(from, {"terminator_kind", "instruction_count"}))
					return state::conflicting;
				const auto& block = representative(from, {"terminator_kind", "instruction_count"});
				const auto count = number(block, "instruction_count");
				if ((!text(block, "terminator_kind").empty() &&
					 text(block, "terminator_kind") != "invoke") ||
					(count && instruction && (*count == 0U || *instruction != *count - 1U)))
					return state::conflicting;
				if (!count || text(block, "terminator_kind") != "invoke")
					result = combine(result, state::unknown);
				return combine(result,
							   text(r, "profile") == topology_profile
								   ? observation(text(r, "membership_state"))
								   : state::partial);
			}
			void invokes()
			{
				for (const auto& [world, original] : index[9U])
				{
					const auto& r = representative(original,
												   {"is_invoke",
													"lowered_block",
													"normal_successor",
													"unwind_successor",
													"eh_boundary_profile"});
					if (boolean(r, "is_invoke") != true && !present(r, "eh_boundary_profile") &&
						!present(r, "normal_successor") && !present(r, "unwind_successor"))
						continue;
					b.charge(b.members, 1U, b.limits.maximum_members, "invokes");
					b.retain(2U * sizeof(observed_invoke_exceptional_boundary));
					observed_invoke_exceptional_boundary value;
					value.exit = copy(world[0]);
					value.variant = copy(text(r, "variant"));
					value.compile_unit = copy(text(r, "compile_unit"));
					world_fields(value, world);
					value.lowered_block = copy(text(r, "lowered_block"));
					value.normal_successor = copy(text(r, "normal_successor"));
					value.unwind_successor = copy(text(r, "unwind_successor"));
					value.block_ordinal = number(r, "block_ordinal");
					value.instruction_ordinal = number(r, "instruction_ordinal");
					value.is_invoke = boolean(r, "is_invoke");
					bind(value.evidence, 9U, original);
					value.identity_state = agree(original,
												 {"exit",
												  "compile_unit",
												  "variant",
												  "is_invoke",
												  "block_ordinal",
												  "instruction_ordinal",
												  "lowered_block",
												  "normal_successor",
												  "unwind_successor"})
						? carrier(value.variant, value.compile_unit, world, value.evidence)
						: state::conflicting;
					value.identity_state =
						combine(value.identity_state,
								occurrence_scope(original, value.variant, world, value.evidence));
					value.placement_state = block_fk(value.lowered_block,
													 value.variant,
													 value.compile_unit,
													 world,
													 value.evidence,
													 value.instruction_ordinal);
					if (!value.block_ordinal || !value.instruction_ordinal)
						value.placement_state = combine(value.placement_state, state::unknown);
					const auto& blocks = find(10U, value.lowered_block, world);
					if (!blocks.empty() && value.block_ordinal &&
						number(*blocks.front(), "ordinal") &&
						value.block_ordinal != number(*blocks.front(), "ordinal"))
						value.placement_state = state::conflicting;
					value.successors_state = value.is_invoke == true
						? combine(successor_fk(value.normal_successor,
											   value,
											   world,
											   value.evidence,
											   "normal",
											   0U),
								  successor_fk(value.unwind_successor,
											   value,
											   world,
											   value.evidence,
											   "unwind",
											   1U))
						: state::partial;
					if (value.normal_successor == value.unwind_successor &&
						!value.normal_successor.empty())
						value.successors_state = state::conflicting;
					const auto& boundary = representative(original,
														  {"eh_boundary_declaration",
														   "eh_selected_scope_kind",
														   "eh_disposition",
														   "eh_boundary_profile",
														   "eh_boundary_state"});
					value.boundary_declaration = copy(text(boundary, "eh_boundary_declaration"));
					value.selected_scope_kind = copy(text(boundary, "eh_selected_scope_kind"));
					value.disposition = copy(text(boundary, "eh_disposition"));
					value.boundary_profile = copy(text(boundary, "eh_boundary_profile"));
					value.boundary_observation_state = copy(text(boundary, "eh_boundary_state"));
					if (!agree(original,
							   {"eh_boundary_declaration",
								"eh_selected_scope_kind",
								"eh_disposition",
								"eh_boundary_profile",
								"eh_boundary_state"}))
						value.boundary_state = state::conflicting;
					else if (value.boundary_profile == boundary_profile)
					{
						const auto& kind = value.selected_scope_kind;
						const auto& disposition = value.disposition;
						const bool known = (kind == "terminate" &&
											(disposition == "direct_function_spec_termination" ||
											 disposition == "other_termination")) ||
							(kind == "catch" && disposition == "catch_dispatch") ||
							(kind == "cleanup" && disposition == "cleanup_dispatch") ||
							(kind == "filter" && disposition == "filter_dispatch") ||
							(kind == "none" && disposition == "unknown");
						const bool known_kind = kind == "terminate" || kind == "catch" ||
							kind == "cleanup" || kind == "filter" || kind == "none";
						const bool known_disposition =
							disposition == "direct_function_spec_termination" ||
							disposition == "other_termination" || disposition == "catch_dispatch" ||
							disposition == "cleanup_dispatch" || disposition == "filter_dispatch" ||
							disposition == "unknown";
						value.boundary_state =
							!known && known_kind && known_disposition && disposition != "unknown"
							? state::conflicting
							: known && disposition != "unknown"
							? observation(value.boundary_observation_state)
							: state::partial;
					}
					else if (!value.boundary_profile.empty() || !value.disposition.empty())
						value.boundary_state = state::partial;
					value.boundary_declaration_state = declaration(
						value, world, text(r, "function"), text(r, "definition_source"));
					specification(value, world, text(r, "scope_detail"));
					for (const auto& axis : std::array{
							 std::pair{value.identity_state, "invoke-identity-unavailable"},
							 std::pair{value.placement_state, "invoke-placement-unavailable"},
							 std::pair{value.successors_state, "invoke-successors-unavailable"},
							 std::pair{value.boundary_state, "invoke-boundary-unavailable"},
							 std::pair{value.boundary_declaration_state,
									   "boundary-declaration-unavailable"},
							 std::pair{value.exception_spec_state,
									   "exception-specification-unavailable"}})
						if (axis.first != state::complete)
							gap(value, value.exit, axis.second);
					output.invokes.push_back(std::move(value));
				}
			}
			void variants()
			{
				for (const auto& [world, originals] : index[9U])
				{
					const auto& r = representative(originals,
												   {"lowered_topology_profile",
													"lowered_topology_state",
													"lowered_block_count",
													"lowered_successor_count",
													"lowered_block_ids",
													"lowered_successor_ids",
													"lowered_entry"});
					if (text(r, "role") != "lowering_variant")
						continue;
					b.charge(b.populations, 1U, b.limits.maximum_populations, "variants");
					b.retain(2U * sizeof(exceptional_route_variant));
					exceptional_route_variant value;
					value.carrier = copy(world[0]);
					value.kind = copy(text(r, "variant_kind"));
					if (const auto* symbol = bytes(r, "variant_symbol"))
						value.symbol =
							copy({reinterpret_cast<const char*>(symbol->data()), symbol->size()});
					value.index = number(r, "variant_index").value_or(0U);
					value.detail = copy(text(r, "scope_detail"));
					value.function = copy(text(r, "function"));
					value.compile_unit = copy(text(r, "compile_unit"));
					value.body = copy(text(r, "body"));
					value.definition_source = copy(text(r, "definition_source"));
					value.universe = copy(world[1]);
					value.variant = copy(world[2]);
					value.interpretation = copy(world[3]);
					value.profile = copy(text(r, "lowered_topology_profile"));
					value.entry = copy(text(r, "lowered_entry"));
					value.block_count = number(r, "lowered_block_count");
					value.successor_count = number(r, "lowered_successor_count");
					bind(value.evidence, 9U, originals);
					value.carrier_state =
						carrier(value.carrier, value.compile_unit, world, value.evidence);
					if (!agree(originals, {"variant_kind", "variant_index", "variant_symbol"}))
						value.carrier_state = state::conflicting;
					else if ((value.kind != "function" && value.kind != "constructor" &&
							  value.kind != "destructor") ||
							 !number(r, "variant_index") || !present(r, "variant_symbol"))
						value.carrier_state = combine(value.carrier_state, state::partial);
					else if (value.kind == "function" && value.index != 0U)
						value.carrier_state = state::conflicting;
					value.scope_state = scope(value, world, originals);
					value.source_state = source(value.definition_source, world, value.evidence);
					const auto blocks = members(r,
												"lowered_block_ids",
												[&](std::string_view id)
												{
													b.retain(2U * sizeof(std::string));
													value.block_ids.push_back(copy(id));
												});
					const auto successors = members(r,
													"lowered_successor_ids",
													[&](std::string_view id)
													{
														b.retain(2U * sizeof(std::string));
														value.successor_ids.push_back(copy(id));
													});
					const auto sort = [&](auto& values)
					{
						std::ranges::sort(values,
										  [&](const auto& a, const auto& c)
										  {
											  b.work(a.size() + c.size() + 1U);
											  return a < c;
										  });
					};
					sort(value.block_ids);
					sort(value.successor_ids);
					value.enumeration_state = agree(originals,
													{"lowered_entry",
													 "lowered_block_count",
													 "lowered_block_ids",
													 "lowered_successor_count",
													 "lowered_successor_ids",
													 "lowered_topology_state",
													 "lowered_topology_profile"})
						? value.profile == topology_profile && value.block_count &&
								value.successor_count && blocks.observed && successors.observed
							? observation(text(r, "lowered_topology_state"))
							: state::partial
						: state::conflicting;
					if (!admission.topology_inputs_complete ||
						!admission.occurrence_inputs_complete)
						value.enumeration_state = combine(value.enumeration_state, state::unknown);
					if (blocks.duplicate || successors.duplicate ||
						(blocks.observed && value.block_count &&
						 *value.block_count != blocks.count) ||
						(successors.observed && value.successor_count &&
						 *value.successor_count != successors.count))
						value.enumeration_state = state::conflicting;
					std::size_t temporary_bytes{};
					struct release
					{
						budget& b;
						std::size_t& n;
						~release()
						{
							b.retained -= n;
						}
					} temporary{b, temporary_bytes};
					const auto keep = [&](std::size_t amount)
					{
						b.retain(amount);
						temporary_bytes += amount;
						b.temporary_peak = std::max(b.temporary_peak, b.retained);
					};
					std::vector<std::string_view> actual_blocks, actual_successors;
					std::set<std::uint64_t> ordinals;
					state payloads = state::complete;
					std::size_t entries{};
					bool correct_entry{};
					for (std::size_t i{}; i < output.blocks.size(); ++i)
					{
						const auto& block = output.blocks[i];
						b.work(block.variant.size() + value.carrier.size() + block.universe.size() +
							   value.universe.size() + block.semantic_variant.size() +
							   value.variant.size() + block.interpretation.size() +
							   value.interpretation.size() + 1U);
						if (block.variant != value.carrier || block.universe != value.universe ||
							block.semantic_variant != value.variant ||
							block.interpretation != value.interpretation)
							continue;
						b.retain(2U * sizeof(std::size_t));
						value.blocks.push_back(i);
						keep(2U * sizeof(std::string_view));
						actual_blocks.push_back(block.block);
						value.enumeration_state =
							combine(value.enumeration_state, block.identity_state);
						payloads = combine(payloads, block.state);
						if (block.identity_state == state::complete)
						{
							keep(128U);
							b.work(static_cast<std::size_t>(std::bit_width(ordinals.size())) + 1U);
							if (!ordinals.insert(block.ordinal).second)
								value.enumeration_state = state::conflicting;
							if (block.is_entry)
							{
								++entries;
								correct_entry |= block.block == value.entry;
							}
						}
					}
					std::set<std::tuple<std::string_view, std::uint64_t, std::uint64_t>>
						successor_slots;
					for (std::size_t i{}; i < output.successors.size(); ++i)
					{
						const auto& edge = output.successors[i];
						b.work(edge.variant.size() + value.carrier.size() + edge.universe.size() +
							   value.universe.size() + edge.semantic_variant.size() +
							   value.variant.size() + edge.interpretation.size() +
							   value.interpretation.size() + 1U);
						if (edge.variant != value.carrier || edge.universe != value.universe ||
							edge.semantic_variant != value.variant ||
							edge.interpretation != value.interpretation)
							continue;
						b.retain(2U * sizeof(std::size_t));
						value.successors.push_back(i);
						keep(2U * sizeof(std::string_view));
						actual_successors.push_back(edge.successor);
						value.enumeration_state =
							combine(value.enumeration_state, edge.identity_state);
						payloads = combine(payloads, edge.state);
						if (edge.identity_state == state::complete)
						{
							keep(128U + sizeof(std::string_view));
							b.work(
								(edge.from_block.size() + 1U) *
								(static_cast<std::size_t>(std::bit_width(successor_slots.size())) +
								 1U));
							if (!successor_slots
									 .emplace(edge.from_block,
											  edge.terminator_instruction_ordinal,
											  edge.ordinal)
									 .second)
								value.enumeration_state = state::conflicting;
						}
					}
					for (std::size_t i{}; i < output.invokes.size(); ++i)
					{
						const auto& invoke = output.invokes[i];
						b.work(invoke.variant.size() + value.carrier.size() +
							   invoke.universe.size() + value.universe.size() +
							   invoke.semantic_variant.size() + value.variant.size() +
							   invoke.interpretation.size() + value.interpretation.size() + 1U);
						if (invoke.variant == value.carrier && invoke.universe == value.universe &&
							invoke.semantic_variant == value.variant &&
							invoke.interpretation == value.interpretation)
						{
							b.retain(2U * sizeof(std::size_t));
							value.invokes.push_back(i);
						}
					}
					sort(actual_blocks);
					sort(actual_successors);
					const auto equal = [&](const auto& expected, const auto& actual)
					{
						if (expected.size() != actual.size())
							return false;
						for (std::size_t i{}; i < expected.size(); ++i)
						{
							b.work(expected[i].size() + actual[i].size() + 1U);
							if (expected[i] != actual[i])
								return false;
						}
						return true;
					};
					if (admission.topology_inputs_complete && blocks.observed &&
						!equal(value.block_ids, actual_blocks))
						value.enumeration_state = state::conflicting;
					if (admission.topology_inputs_complete && successors.observed &&
						!equal(value.successor_ids, actual_successors))
						value.enumeration_state = state::conflicting;
					value.entry_state = value.entry.empty()			 ? state::unknown
						: entries == 1U && correct_entry			 ? state::complete
						: value.enumeration_state == state::complete ? state::conflicting
																	 : state::unknown;
					if (entries > 1U)
						value.entry_state = state::conflicting;
					value.topology_state =
						combine(combine(value.enumeration_state, value.entry_state), payloads);
					if (value.topology_state != state::complete)
						gap(value, value.carrier, "topology-unavailable");
					if (value.scope_state != state::complete)
						gap(value, value.carrier, "scope-unavailable");
					if (value.carrier_state != state::complete)
						gap(value, value.carrier, "carrier-unavailable");
					output.variants.push_back(std::move(value));
				}
			}
			exceptional_route_projection run()
			{
				initialize();
				blocks();
				successors();
				invokes();
				variants();
				return std::move(output);
			}
		};
		result<exceptional_route_projection> project(exceptional_route_input input,
													 const application_query_results* queries,
													 finite_population_limits limits,
													 std::stop_token stop,
													 projection_resource_usage* usage)
		{
			if (usage)
				*usage = {};
			if (auto valid = limits.validate(); !valid)
				return valid.error();
			try
			{
				budget b{limits, stop};
				b.work();
				groups borrowed;
				bool row_validation_reused = queries != nullptr;
				std::array<bool, 12> available{}, seen{};
				available.fill(true);
				if (queries)
				{
					std::size_t plan_bytes{};
					if (auto valid =
							detail::check_source_plan_limits(*queries,
															 limits.maximum_source_queries,
															 limits.maximum_source_plan_bytes,
															 stop,
															 "sdk.exceptional-route",
															 &plan_bytes,
															 [&]
															 {
																 b.work();
																 return false;
															 });
						!valid)
						return valid.error();
					b.retain(plan_bytes);
					for (const auto& scan : queries->scans)
					{
						b.work();
						const auto at = std::ranges::find(relations, scan.relation_id);
						if (at == relations.end())
							continue;
						const auto group = static_cast<std::size_t>(at - relations.begin());
						seen[group] = true;
						row_validation_reused &= query_transfer_access::rows_validated(scan.result);
						available[group] &= scan.result.execution() == execution_status::complete &&
							scan.result.conflicts().empty() &&
							scan.result.differential_disagreements().empty();
						const auto originals = query_transfer_access::borrow_rows(scan.result);
						b.retain(1024U +
								 2U * (borrowed[group].size() + originals.size()) *
									 sizeof(const annotated_row*));
						if (originals.size() > limits.maximum_rows - b.rows)
							fail("rows", "limit-exceeded", "sdk.exceptional-route-budget");
						b.rows += originals.size();
						borrowed[group].reserve(borrowed[group].size() + originals.size());
						for (const auto& r : originals)
						{
							b.work();
							borrowed[group].push_back(&r);
						}
					}
					b.rows = 0;
					const auto complete = [&](std::size_t group)
					{
						return seen[group] && available[group];
					};
					input.compile_units_complete = complete(0U);
					input.scope_inputs_complete = complete(3U) && complete(4U) && complete(5U);
					input.occurrence_inputs_complete = complete(9U);
					input.topology_inputs_complete = complete(10U) && complete(11U);
					input.declaration_inputs_complete = complete(7U) && complete(8U);
				}
				else
				{
					const std::array spans{input.units,
										   input.files,
										   input.spans,
										   input.entities,
										   input.details,
										   input.bodies,
										   input.syntax_nodes,
										   input.declarations,
										   input.inventories,
										   input.exits,
										   input.blocks,
										   input.successors};
					for (std::size_t group = 0; group < spans.size(); ++group)
					{
						b.retain(2U * spans[group].size() * sizeof(const annotated_row*));
						if (spans[group].size() > limits.maximum_rows - b.rows)
							fail("rows", "limit-exceeded", "sdk.exceptional-route-budget");
						b.rows += spans[group].size();
						borrowed[group].reserve(spans[group].size());
						for (const auto& r : spans[group])
						{
							b.work();
							borrowed[group].push_back(&r);
						}
					}
					b.rows = 0;
				}
				projector work{b, borrowed, input, {}, {}, {}, row_validation_reused};
				auto output = work.run();
				output.compile_units_complete = input.compile_units_complete;
				output.scope_inputs_complete = input.scope_inputs_complete;
				output.occurrence_inputs_complete = input.occurrence_inputs_complete;
				output.topology_inputs_complete = input.topology_inputs_complete;
				output.declaration_inputs_complete = input.declaration_inputs_complete;
				if (queries)
				{
					for (std::size_t group = 0; group < seen.size(); ++group)
						if (!seen[group])
						{
							b.retain(2U *
									 (sizeof(query_unresolved) + relations[group].size() + 128U));
							output.unresolved.push_back({"sdk.exceptional-route-scan-missing",
														 std::string{relations[group]},
														 "independent-scan-unavailable"});
						}
					b.retain(sizeof(application_query_results) + 2U * queries->snapshot_id.size() +
							 2U * queries->scans.size() * sizeof(application_relation_scan));
					output.source_queries = *queries;
				}
				if (usage)
					*usage = {b.operations, std::max(b.retained, b.temporary_peak)};
				return output;
			}
			catch (const failure& e)
			{
				return e.value;
			}
			catch (const std::bad_alloc&)
			{
				return error{
					"sdk.exceptional-route-resource-exhausted", "projection", "allocation"};
			}
			catch (const std::length_error&)
			{
				return error{"sdk.exceptional-route-resource-exhausted", "projection", "length"};
			}
		}
	} // namespace
	result<exceptional_route_projection> project_exceptional_routes(exceptional_route_input input,
																	finite_population_limits limits,
																	std::stop_token stop)
	{
		return project(input, nullptr, limits, stop, nullptr);
	}
	result<exceptional_route_projection>
	project_exceptional_routes(const application_query_results& input,
							   finite_population_limits limits,
							   std::stop_token stop)
	{
		return project({}, &input, limits, stop, nullptr);
	}
	result<exceptional_route_projection>
	project_exceptional_routes(exceptional_route_input input,
							   finite_population_limits limits,
							   std::stop_token stop,
							   projection_resource_usage& usage)
	{
		return project(input, nullptr, limits, stop, &usage);
	}
	result<exceptional_route_projection>
	project_exceptional_routes(const application_query_results& input,
							   finite_population_limits limits,
							   std::stop_token stop,
							   projection_resource_usage& usage)
	{
		return project({}, &input, limits, stop, &usage);
	}
} // namespace cxxlens::sdk::query
