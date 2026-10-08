#include <algorithm>
#include <array>
#include <bit>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

#include <cxxlens/sdk/exception_cleanup_facets.hpp>

#include "query_projection_plan_limits_internal.hpp"
#include "query_projection_rows_internal.hpp"
#include "query_result_internal.hpp"
namespace cxxlens::sdk::query
{
	namespace
	{
		using refs = std::vector<std::size_t>;
		using rows = std::vector<const annotated_row*>;
		using groups = std::array<rows, 9>;
		constexpr std::array<std::string_view, 9> relations{"build.compile_unit.v1",
															"source.file.v1",
															"source.span.v1",
															"cc.entity.v1",
															"cc.entity_detail.v1",
															"cc.body.v1",
															"cc.exceptional_exit.v1",
															"cc.declaration.v1",
															"cc.declaration_inventory.v1"};
		constexpr std::array<std::string_view, 9> identifiers{"compile_unit",
															  "snapshot",
															  "span",
															  "entity",
															  "detail",
															  "body",
															  "exit",
															  "declaration",
															  "inventory"};
		constexpr std::string_view spec_profile = "clang22-function-exception-specification/1",
								   cleanup_profile = "clang22-destroy-object-cleanup-emission/1",
								   target_profile = "clang22-destructor-emission-target/1";
		struct failure
		{
			error value;
		};
		[[noreturn]] void fail(std::string_view field,
							   std::string_view reason,
							   std::string_view code = "sdk.exception-cleanup-input-invalid")
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
		struct identity_less
		{
			using is_transparent = void;
			template <class L, class R>
			bool operator()(const L& l, const R& r) const
			{
				for (std::size_t i = 0; i < 4U; ++i)
				{
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
					fail(field, "limit-exceeded", "sdk.exception-cleanup-budget");
				used += amount;
			}
			void work(std::size_t n = 1)
			{
				if (stop.stop_requested() || (limits.cancelled && limits.cancelled()))
					fail("projection", "stop-requested", "sdk.exception-cleanup-cancelled");
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
						fail("row", "limit-exceeded", "sdk.exception-cleanup-budget");
					total += n * factor;
					if (validation_payload)
					{
						if (n > limits.maximum_operations - *validation_payload)
							fail("operations", "limit-exceeded", "sdk.exception-cleanup-budget");
						*validation_payload += n;
					}
				};
				const auto fixed = [&](std::size_t n)
				{
					work();
					if (total > limits.maximum_retained_bytes ||
						n > limits.maximum_retained_bytes - total)
						fail("row", "limit-exceeded", "sdk.exception-cleanup-budget");
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
		struct projector
		{
			budget& b;
			groups& input;
			exception_cleanup_projection output;
			std::array<std::map<view_identity, rows, identity_less>, 9> index;
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
						fail("operations", "limit-exceeded", "sdk.exception-cleanup-budget");
					b.work((value.size() + 1U) * factor);
				}
			}
			const rows& find(std::size_t group, std::string_view id, const view_identity& world)
			{
				static const rows empty;
				if (id.empty())
					return empty;
				view_identity key{id, world[1], world[2], world[3]};
				lookup_work(key, index[group].size());
				const auto at = index[group].find(key);
				return at == index[group].end() ? empty : at->second;
			}
			void peak(std::size_t amount)
			{
				if (amount > b.limits.maximum_retained_bytes - b.retained)
					fail("temporary", "limit-exceeded", "sdk.exception-cleanup-budget");
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
				const auto descriptors = standard_relation_descriptors();
				for (std::size_t group = 0; group < input.size(); ++group)
				{
					const auto d =
						std::ranges::find(descriptors, relations[group], &relation_descriptor::id);
					if (d == descriptors.end())
						fail(relations[group], "descriptor-missing");
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
								"sdk.exception-cleanup-input-invalid",
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
							lookup_work(key, index[group].size());
							// Conservative capacity/node allowance before both map and vector
							// growth.
							b.retain(sizeof(view_identity) + 256U +
									 2U * sizeof(const annotated_row*));
							index[group][key].push_back(row);
						}
					}
				}
				for (auto& group : index)
					for (auto& [key, alternatives] : group)
					{
						(void)key;
						std::ranges::sort(
							alternatives,
							[&](const auto* x, const auto* y)
							{
								const auto estimate_x = b.estimate(*x), estimate_y = b.estimate(*y);
								if (estimate_x > b.limits.maximum_retained_bytes - estimate_y)
									fail("temporary",
										 "limit-exceeded",
										 "sdk.exception-cleanup-budget");
								peak(estimate_x + estimate_y);
								auto a = x->canonical_form(), c = y->canonical_form();
								b.work(a.size() + c.size() + 1U);
								return a < c;
							});
					}
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
					{"sdk.exception-cleanup-" + std::string{reason}, std::string{subject}, {}});
			}
			bool callable(std::string_view kind)
			{
				return kind == "function" || kind == "method" || kind == "constructor" ||
					kind == "destructor" || kind == "conversion";
			}
			bool known_noncallable(std::string_view kind)
			{
				return kind == "variable" || kind == "parameter" || kind == "field" ||
					kind == "record" || kind == "enum" || kind == "enumerator" ||
					kind == "namespace" || kind == "typedef" || kind == "type_alias" ||
					kind == "template" || kind == "struct" || kind == "class" || kind == "union" ||
					kind == "alias" || kind == "template_type_parameter";
			}
			finite_population_state entity(std::string_view id,
										   const view_identity& world,
										   refs& evidence,
										   std::string_view required_kind = {})
			{
				const auto& candidates = find(3U, id, world);
				if (candidates.empty())
					return finite_population_state::unknown;
				bind(evidence, 3U, candidates);
				if (!agree(candidates, {"entity", "kind", "semantic_owner"}))
					return finite_population_state::conflicting;
				const auto kind = text(*candidates.front(), "kind");
				if (required_kind.empty())
					return callable(kind)		  ? finite_population_state::complete
						: known_noncallable(kind) ? finite_population_state::conflicting
												  : finite_population_state::unknown;
				return kind == required_kind ? finite_population_state::complete
					: (callable(kind) || known_noncallable(kind))
					? finite_population_state::conflicting
					: finite_population_state::unknown;
			}
			finite_population_state
			unit(std::string_view id, const view_identity& world, refs& evidence)
			{
				const auto& candidates = find(0U, id, world);
				if (candidates.empty())
					return finite_population_state::unknown;
				bind(evidence, 0U, candidates);
				return agree(candidates, {"compile_unit"}) ? finite_population_state::complete
														   : finite_population_state::conflicting;
			}
			finite_population_state
			source(std::string_view id, const view_identity& world, refs& evidence)
			{
				const auto& spans = find(2U, id, world);
				if (spans.empty())
					return finite_population_state::unknown;
				bind(evidence, 2U, spans);
				if (!agree(spans, {"span", "snapshot", "file", "begin", "end"}))
					return finite_population_state::conflicting;
				const auto& s = *spans.front();
				const auto& files = find(1U, text(s, "snapshot"), world);
				if (files.empty())
					return finite_population_state::partial;
				bind(evidence, 1U, files);
				if (!agree(files, {"snapshot", "file", "size"}))
					return finite_population_state::conflicting;
				const auto begin = number(s, "begin"), end = number(s, "end"),
						   size = number(*files.front(), "size");
				if (!begin || !end || !size)
					return finite_population_state::partial;
				return text(s, "file") == text(*files.front(), "file") && *begin <= *end &&
						*end <= *size
					? finite_population_state::complete
					: finite_population_state::conflicting;
			}
			void specification(const view_identity& world, const rows& originals)
			{
				const auto& r = representative(originals,
											   {"exception_spec_kind",
												"exception_spec_nonthrowing",
												"exception_spec_state",
												"exception_spec_profile"});
				const auto& entities = find(3U, text(r, "entity"), world);
				if (!entities.empty() && agree(entities, {"entity", "kind"}) &&
					known_noncallable(text(*entities.front(), "kind")) &&
					!present(r, "exception_spec_kind") && !present(r, "exception_spec_profile") &&
					!present(r, "exception_spec_nonthrowing") &&
					!present(r, "exception_spec_state"))
					return;
				b.charge(b.populations, 1U, b.limits.maximum_populations, "observations");
				b.retain(2U * sizeof(observed_function_exception_specification));
				observed_function_exception_specification p;
				p.detail = copy(world[0]);
				p.function = copy(text(r, "entity"));
				p.compile_unit = copy(text(r, "compile_unit"));
				p.source_span = copy(text(r, "source"));
				p.universe = copy(world[1]);
				p.variant = copy(world[2]);
				p.interpretation = copy(world[3]);
				bind(p.evidence, 4U, originals);
				p.identity_state = combine(unit(p.compile_unit, world, p.evidence),
										   entity(p.function, world, p.evidence));
				if (!agree(originals, {"detail", "entity", "compile_unit", "source"}))
					p.identity_state = finite_population_state::conflicting;
				p.source_state = source(p.source_span, world, p.evidence);
				p.kind = copy(text(r, "exception_spec_kind"));
				p.profile = copy(text(r, "exception_spec_profile"));
				p.observation_state = copy(text(r, "exception_spec_state"));
				p.nonthrowing = boolean(r, "exception_spec_nonthrowing");
				if (!agree(originals,
						   {"exception_spec_kind",
							"exception_spec_nonthrowing",
							"exception_spec_state",
							"exception_spec_profile"}))
				{
					p.specification_state = finite_population_state::conflicting;
					p.nonthrowing.reset();
				}
				else if (p.profile == spec_profile)
				{
					const bool true_kind = p.kind == "dynamic_none" || p.kind == "no_throw" ||
						p.kind == "basic_noexcept" || p.kind == "noexcept_true";
					const bool false_kind = p.kind == "none" || p.kind == "dynamic" ||
						p.kind == "ms_any" || p.kind == "noexcept_false";
					const bool partial_kind = p.kind == "dependent_noexcept" ||
						p.kind == "unevaluated" || p.kind == "uninstantiated" ||
						p.kind == "unparsed";
					if (true_kind || false_kind)
					{
						if (p.observation_state == "complete" && p.nonthrowing)
							p.specification_state = *p.nonthrowing == true_kind
								? finite_population_state::complete
								: finite_population_state::conflicting;
						else
							p.specification_state = finite_population_state::partial;
					}
					else if (partial_kind)
					{
						p.specification_state = p.nonthrowing || p.observation_state == "complete"
							? finite_population_state::conflicting
							: finite_population_state::partial;
					}
					else if (p.kind == "no_prototype")
					{
						p.specification_state = p.nonthrowing || p.observation_state == "complete"
							? finite_population_state::conflicting
							: finite_population_state::unknown;
					}
					else
						p.specification_state = finite_population_state::partial;
				}
				else if (!p.kind.empty() || !p.profile.empty() || !p.observation_state.empty() ||
						 p.nonthrowing)
					p.specification_state = finite_population_state::partial;
				for (const auto& axis :
					 std::array{std::pair{p.specification_state, "specification-unavailable"},
								std::pair{p.identity_state, "function-identity-unavailable"},
								std::pair{p.source_state, "specification-source-unavailable"}})
					if (axis.first != finite_population_state::complete)
						gap(p, p.detail, axis.second);
				output.specifications.push_back(std::move(p));
			}
			struct membership
			{
				bool present{}, contradictory{};
			};
			membership
			contains(const annotated_row& r, std::string_view field, std::string_view wanted)
			{
				const auto* values = bytes(r, field);
				if (!values)
					return {};
				std::size_t pos{}, count{};
				bool found = false;
				std::set<std::string_view> seen_members;
				struct temporary_storage
				{
					budget& owner;
					std::size_t charged{};
					~temporary_storage()
					{
						owner.retained -= charged;
					}
					void keep(std::size_t n)
					{
						owner.retain(n);
						charged += n;
						owner.temporary_peak = std::max(owner.temporary_peak, owner.retained);
					}
				} temporary{b};
				// Framing is original detached-set encoding. No allocation or identifier
				// grammar.
				while (pos < values->size())
				{
					b.work();
					if (values->size() - pos < 4U)
						fail(field, "truncated-set");
					std::uint32_t size{};
					for (unsigned shift = 0; shift < 32U; shift += 8U)
						size |= std::to_integer<std::uint32_t>((*values)[pos++]) << shift;
					if (size == 0U || size > values->size() - pos)
						fail(field, "invalid-set-member");
					b.charge(b.members, 1U, b.limits.maximum_members, "set-members");
					b.work(size + wanted.size() + 1U);
					const auto* data = reinterpret_cast<const char*>(values->data() + pos);
					std::string_view value{data, size};
					b.work((value.size() + 1U) *
						   (static_cast<std::size_t>(std::bit_width(seen_members.size())) + 1U));
					temporary.keep(sizeof(std::string_view) + 128U);
					if (!seen_members.insert(value).second)
						fail(field, "duplicate-set-member");
					if (value == wanted)
						found = true;
					pos += size;
					++count;
				}

				const auto declared = number(r, "declaration_count");
				return {found, declared && *declared != count};
			}
			finite_population_state declaration(observed_cleanup_emission& p,
												const view_identity& world)
			{
				const auto& declarations = find(7U, p.declaration, world);
				if (declarations.empty())
					return finite_population_state::unknown;
				bind(p.evidence, 7U, declarations);
				if (!agree(declarations, {"declaration", "entity", "source", "kind"}))
					return finite_population_state::conflicting;
				const auto& r = *declarations.front();
				p.object = copy(text(r, "entity"));
				p.declaration_source = copy(text(r, "source"));
				p.declaration_source_state = source(p.declaration_source, world, p.evidence);
				if (text(r, "kind") != "Var")
					return finite_population_state::conflicting;
				auto state = entity(p.object, world, p.evidence, "variable");
				const auto& objects = find(3U, p.object, world);
				for (const auto* object : objects)
					if (present(*object, "semantic_owner") &&
						text(*object, "semantic_owner") != p.function)
						state = finite_population_state::conflicting;
				bool admitted = false, contradictory = false;
				for (const auto& [key, inventories] : index[8U])
				{
					b.work();
					if (key[1] != world[1] || key[2] != world[2] || key[3] != world[3])
						continue;
					for (const auto* inventory : inventories)
					{
						b.work(p.compile_unit.size() + 1U);
						if (text(*inventory, "compile_unit") != p.compile_unit)
							continue;
						if (text(*inventory, "profile") !=
							"clang22-explicit-admitted-named-declarations/1")
							continue;
						const auto admitted_member =
							contains(*inventory, "declarations", p.declaration);
						if (admitted_member.present)
						{
							bind(p.evidence, 8U, std::array{inventory});
							admitted = true;
							contradictory |= admitted_member.contradictory ||
								!agree(inventories,
									   {"compile_unit",
										"profile",
										"declaration_count",
										"declarations"});
						}
					}
				}
				if (contradictory)
					return finite_population_state::conflicting;
				return combine(state,
							   admitted ? finite_population_state::complete
										: finite_population_state::unknown);
			}
			finite_population_state physical_scope(observed_cleanup_emission& p,
												   const view_identity& world,
												   const rows& originals)
			{
				if (!agree(originals,
						   {"exit",
							"compile_unit",
							"scope_detail",
							"function",
							"definition_source",
							"variant",
							"variant_kind",
							"variant_index",
							"variant_symbol",
							"body",
							"profile",
							"lowering_profile"}))
					return finite_population_state::conflicting;
				auto state = combine(unit(p.compile_unit, world, p.evidence),
									 entity(p.function, world, p.evidence));
				const auto& details = find(4U, p.scope_detail, world);
				if (details.empty())
					state = combine(state, finite_population_state::unknown);
				else
				{
					bind(p.evidence, 4U, details);
					if (!agree(details, {"detail", "entity", "compile_unit", "source"}))
						state = finite_population_state::conflicting;
					for (const auto* detail : details)
						if (text(*detail, "entity") != p.function ||
							text(*detail, "compile_unit") != p.compile_unit ||
							text(*detail, "source") != p.definition_source)
							state = finite_population_state::conflicting;
				}
				if (!p.body.empty())
				{
					const auto& bodies = find(5U, p.body, world);
					if (bodies.empty())
						state = combine(state, finite_population_state::unknown);
					else
					{
						bind(p.evidence, 5U, bodies);
						if (!agree(bodies, {"body", "compile_unit", "function"}))
							state = finite_population_state::conflicting;
						for (const auto* body : bodies)
							if (text(*body, "compile_unit") != p.compile_unit ||
								text(*body, "function") != p.function)
								state = finite_population_state::conflicting;
					}
				}
				const auto& r = *originals.front();
				const auto carrier =
					text(r, "role") == "lowering_variant" ? p.exit : p.lowering_variant;
				const auto& variants = find(6U, carrier, world);
				if (variants.empty())
					return combine(state, finite_population_state::unknown);
				bind(p.evidence, 6U, variants);
				if (!agree(variants,
						   {"exit",
							"role",
							"compile_unit",
							"scope_detail",
							"function",
							"definition_source",
							"variant_kind",
							"variant_index",
							"variant_symbol",
							"profile",
							"lowering_profile"}))
					return finite_population_state::conflicting;
				for (const auto* variant : variants)
				{
					if (text(*variant, "role") != "lowering_variant" ||
						number(*variant, "ordinal") != std::optional<std::uint64_t>{0U})
						return finite_population_state::conflicting;
					for (auto field : {"compile_unit",
									   "scope_detail",
									   "function",
									   "definition_source",
									   "variant_kind",
									   "variant_index",
									   "variant_symbol",
									   "profile",
									   "lowering_profile"})
					{
						b.work();
						const auto *a = cell(*variant, field), *c = cell(r, field);
						if (a && a->value)
							std::visit(
								[&](const auto& v)
								{
									if constexpr (requires { v.size(); })
										b.work(v.size());
								},
								*a->value);
						if (c && c->value)
							std::visit(
								[&](const auto& v)
								{
									if constexpr (requires { v.size(); })
										b.work(v.size());
								},
								*c->value);
						if (!a || !c || a->value != c->value)
							return finite_population_state::conflicting;
					}
					if (present(*variant, "body"))
					{
						const auto& carrier_bodies = find(5U, text(*variant, "body"), world);
						if (carrier_bodies.empty())
							state = combine(state, finite_population_state::unknown);
						else
						{
							bind(p.evidence, 5U, carrier_bodies);
							if (!agree(carrier_bodies, {"body", "compile_unit", "function"}))
								return finite_population_state::conflicting;
							for (const auto* body : carrier_bodies)
								if (text(*body, "compile_unit") != p.compile_unit ||
									text(*body, "function") != p.function)
									return finite_population_state::conflicting;
						}
						if (!p.body.empty() && text(*variant, "body") != p.body)
							return finite_population_state::conflicting;
					}
				}
				const auto kind = text(r, "variant_kind");
				if (kind != "function" && kind != "constructor" && kind != "destructor")
					state = combine(state, finite_population_state::unknown);
				else if (kind == "function" &&
						 number(r, "variant_index") != std::optional<std::uint64_t>{0U})
					return finite_population_state::conflicting;
				if (text(r, "profile") != "clang22-original-exceptional-occurrences/1" ||
					text(r, "lowering_profile") != "clang22-written-definition-analysis-lowering/1")
					state = combine(state, finite_population_state::unknown);
				return state;
			}
			void cleanup(const view_identity& world, const rows& originals)
			{
				const auto& r = representative(originals,
											   {"cleanup_declaration",
												"cleanup_registration_ordinal",
												"cleanup_emission_ordinal",
												"cleanup_route",
												"cleanup_profile",
												"cleanup_target",
												"cleanup_target_usr",
												"cleanup_target_dtor_type",
												"cleanup_target_profile"});
				b.charge(b.populations, 1U, b.limits.maximum_populations, "observations");
				b.retain(2U * sizeof(observed_cleanup_emission));
				observed_cleanup_emission p;
				p.exit = copy(world[0]);
				p.lowering_variant = copy(text(r, "variant"));
				p.scope_detail = copy(text(r, "scope_detail"));
				p.function = copy(text(r, "function"));
				p.compile_unit = copy(text(r, "compile_unit"));
				p.body = copy(text(r, "body"));
				p.definition_source = copy(text(r, "definition_source"));
				p.universe = copy(world[1]);
				p.variant = copy(world[2]);
				p.interpretation = copy(world[3]);
				bind(p.evidence, 6U, originals);
				p.scope_state = physical_scope(p, world, originals);
				p.definition_source_state = source(p.definition_source, world, p.evidence);
				p.declaration = copy(text(r, "cleanup_declaration"));
				p.route = copy(text(r, "cleanup_route"));
				p.profile = copy(text(r, "cleanup_profile"));
				p.registration_ordinal = number(r, "cleanup_registration_ordinal");
				p.emission_ordinal = number(r, "cleanup_emission_ordinal");
				// Original compiler emission, registration and normalized
				// object/target bindings are independent facets.
				const auto& emitted = representative(
					originals, {"cleanup_profile", "cleanup_route", "cleanup_emission_ordinal"});
				p.profile = copy(text(emitted, "cleanup_profile"));
				p.route = copy(text(emitted, "cleanup_route"));
				p.emission_ordinal = number(emitted, "cleanup_emission_ordinal");
				if (!agree(originals,
						   {"cleanup_profile", "cleanup_route", "cleanup_emission_ordinal"}))
					p.emission_state = finite_population_state::conflicting;
				else if (p.profile == cleanup_profile)
				{
					if (p.emission_ordinal && *p.emission_ordinal == 0U)
						p.emission_state = finite_population_state::conflicting;
					else if (p.emission_ordinal &&
							 (p.route == "normal" || p.route == "exceptional"))
						p.emission_state = finite_population_state::complete;
					else
						p.emission_state = finite_population_state::partial;
				}
				else if (!p.profile.empty() || !p.route.empty() || p.emission_ordinal)
					p.emission_state = finite_population_state::partial;
				const auto& registered =
					representative(originals, {"cleanup_profile", "cleanup_registration_ordinal"});
				p.registration_ordinal = number(registered, "cleanup_registration_ordinal");
				if (!agree(originals, {"cleanup_profile", "cleanup_registration_ordinal"}))
					p.registration_state = finite_population_state::conflicting;
				else if (text(registered, "cleanup_profile") == cleanup_profile &&
						 p.registration_ordinal)
					p.registration_state = *p.registration_ordinal > 0U
						? finite_population_state::complete
						: finite_population_state::conflicting;
				else if (p.registration_ordinal)
					p.registration_state = finite_population_state::partial;
				const auto& object_row = representative(originals, {"cleanup_declaration"});
				p.declaration = copy(text(object_row, "cleanup_declaration"));
				if (!agree(originals, {"cleanup_declaration"}))
					p.declaration_state = finite_population_state::conflicting;
				else if (!p.declaration.empty())
					p.declaration_state = declaration(p, world);
				const auto& target_row = representative(originals,
														{"cleanup_target_usr",
														 "cleanup_target_dtor_type",
														 "cleanup_target_profile",
														 "emitter_methods"});
				p.target_profile = copy(text(target_row, "cleanup_target_profile"));
				p.target_dtor_type = number(target_row, "cleanup_target_dtor_type");
				p.emitter_methods = number(target_row, "emitter_methods");
				if (const auto* usr = bytes(target_row, "cleanup_target_usr"))
				{
					b.retain(2U * usr->size());
					p.target_usr = *usr;
				}
				const auto& target_entity_row = representative(originals, {"cleanup_target"});
				p.target = copy(text(target_entity_row, "cleanup_target"));
				const bool any_target = !p.target.empty() || !p.target_profile.empty() ||
					p.target_dtor_type || present(target_row, "cleanup_target_usr");
				if (!agree(originals,
						   {"cleanup_target_usr",
							"cleanup_target_dtor_type",
							"cleanup_target_profile",
							"emitter_methods"}))
					p.target_attribution_state = finite_population_state::conflicting;
				else if (p.emitter_methods)
				{
					p.destructor_target_excluded = (*p.emitter_methods & 1U) == 0U;
					if (*p.destructor_target_excluded)
						p.target_attribution_state = any_target
							? finite_population_state::conflicting
							: finite_population_state::complete;
					else if (p.target_profile == target_profile && p.target_dtor_type &&
							 *p.target_dtor_type <= 5U && present(target_row, "cleanup_target_usr"))
						p.target_attribution_state = finite_population_state::complete;
					else
						p.target_attribution_state = finite_population_state::partial;
				}
				else if (any_target)
					p.target_attribution_state = finite_population_state::partial;
				if (!agree(originals, {"cleanup_target"}))
					p.target_state = finite_population_state::conflicting;
				else if (!p.target.empty())
					p.target_state = entity(p.target, world, p.evidence, "destructor");
				for (const auto& axis :
					 std::array{std::pair{p.scope_state, "cleanup-scope-unavailable"},
								std::pair{p.definition_source_state,
										  "cleanup-definition-source-unavailable"},
								std::pair{p.emission_state, "cleanup-emission-unavailable"},
								std::pair{p.registration_state, "cleanup-registration-unavailable"},
								std::pair{p.declaration_state, "cleanup-declaration-unavailable"},
								std::pair{p.target_attribution_state,
										  "destructor-attribution-unavailable"},
								std::pair{p.target_state, "destructor-target-unavailable"}})
					if (axis.first != finite_population_state::complete)
						gap(p, p.exit, axis.second);
				output.cleanups.push_back(std::move(p));
			}
			exception_cleanup_projection run()
			{
				initialize();
				for (const auto& [world, original] : index[4U])
					specification(world, original);
				for (const auto& [world, original] : index[6U])
					cleanup(world, original);
				return std::move(output);
			}
		};
		result<exception_cleanup_projection> project(exception_cleanup_input input,
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
				std::array<bool, 9> available{}, seen{};
				available.fill(true);
				if (queries)
				{
					std::size_t plan_bytes{};
					if (auto valid =
							detail::check_source_plan_limits(*queries,
															 limits.maximum_source_queries,
															 limits.maximum_source_plan_bytes,
															 stop,
															 "sdk.exception-cleanup",
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
							fail("rows", "limit-exceeded", "sdk.exception-cleanup-budget");
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
					input.detail_inputs_complete = complete(4U);
					input.exit_inputs_complete = complete(6U);
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
										   input.exits,
										   input.declarations,
										   input.inventories};
					for (std::size_t group = 0; group < spans.size(); ++group)
					{
						b.retain(2U * spans[group].size() * sizeof(const annotated_row*));
						if (spans[group].size() > limits.maximum_rows - b.rows)
							fail("rows", "limit-exceeded", "sdk.exception-cleanup-budget");
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
				projector work{b, borrowed, {}, {}, {}, row_validation_reused};
				auto output = work.run();
				output.compile_units_complete = input.compile_units_complete;
				output.detail_inputs_complete = input.detail_inputs_complete;
				output.exit_inputs_complete = input.exit_inputs_complete;
				output.declaration_inputs_complete = input.declaration_inputs_complete;
				if (queries)
				{
					for (std::size_t group = 0; group < seen.size(); ++group)
						if (!seen[group])
						{
							b.retain(2U *
									 (sizeof(query_unresolved) + relations[group].size() + 128U));
							output.unresolved.push_back({"sdk.exception-cleanup-scan-missing",
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
					"sdk.exception-cleanup-resource-exhausted", "projection", "allocation"};
			}
			catch (const std::length_error&)
			{
				return error{"sdk.exception-cleanup-resource-exhausted", "projection", "length"};
			}
		}
	} // namespace
	result<exception_cleanup_projection> project_exception_cleanup_facets(
		exception_cleanup_input input, finite_population_limits limits, std::stop_token stop)
	{
		return project(input, nullptr, limits, stop, nullptr);
	}
	result<exception_cleanup_projection>
	project_exception_cleanup_facets(const application_query_results& input,
									 finite_population_limits limits,
									 std::stop_token stop)
	{
		return project({}, &input, limits, stop, nullptr);
	}
	result<exception_cleanup_projection>
	project_exception_cleanup_facets(exception_cleanup_input input,
									 finite_population_limits limits,
									 std::stop_token stop,
									 projection_resource_usage& usage)
	{
		return project(input, nullptr, limits, stop, &usage);
	}
	result<exception_cleanup_projection>
	project_exception_cleanup_facets(const application_query_results& input,
									 finite_population_limits limits,
									 std::stop_token stop,
									 projection_resource_usage& usage)
	{
		return project({}, &input, limits, stop, &usage);
	}
} // namespace cxxlens::sdk::query
