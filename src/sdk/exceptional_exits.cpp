#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

#include <cxxlens/sdk/exceptional_exits.hpp>

#include "query_projected_row_encoding_internal.hpp"
#include "query_projection_plan_limits_internal.hpp"
#include "query_projection_row_copy_internal.hpp"
#include "query_projection_rows_internal.hpp"
#include "query_result_internal.hpp"
namespace cxxlens::sdk::query
{
	namespace
	{
		using refs = std::vector<std::size_t>;
		using size_groups = std::array<std::vector<query_transfer_access::row_size_view>, 8>;
		constexpr std::array<std::string_view, 8> relations{"build.compile_unit.v1",
															"source.file.v1",
															"source.span.v1",
															"cc.entity.v1",
															"cc.entity_detail.v1",
															"cc.body.v1",
															"cc.syntax_node.v1",
															"cc.exceptional_exit.v1"};
		constexpr std::array<std::string_view, 8> identifiers{
			"compile_unit", "snapshot", "span", "entity", "detail", "body", "node", "exit"};
		constexpr std::string_view occurrence_profile =
									   "clang22-original-exceptional-occurrences/1",
								   lowering_profile =
									   "clang22-written-definition-analysis-lowering/1";
		struct failure
		{
			error value;
		};
		[[noreturn]] void fail(std::string_view field,
							   std::string_view reason,
							   std::string_view code = "sdk.exceptional-exit-input-invalid")
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
					fail(field, "limit-exceeded", "sdk.exceptional-exit-budget");
				used += amount;
			}
			void work(std::size_t n = 1)
			{
				if (stop.stop_requested() || (limits.cancelled && limits.cancelled()))
					fail("projection", "stop-requested", "sdk.exceptional-exit-cancelled");
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
			std::size_t estimate(const annotated_row& row, bool encoding = true)
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
						fail("row", "limit-exceeded", "sdk.exceptional-exit-budget");
					total += n * factor;
				};
				const auto fixed = [&](std::size_t n)
				{
					work();
					if (total > limits.maximum_retained_bytes ||
						n > limits.maximum_retained_bytes - total)
						fail("row", "limit-exceeded", "sdk.exceptional-exit-budget");
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
		struct identity_less
		{
			using is_transparent = void;
			budget* b;
			template <class L, class R>
			bool operator()(const L& l, const R& r) const
			{
				for (std::size_t part{}; part < 4U; ++part)
				{
					b->work();
					const auto extent = std::min(l[part].size(), r[part].size());
					for (std::size_t i{}; i < extent; ++i)
					{
						b->work(2U);
						const auto left = static_cast<unsigned char>(l[part][i]);
						const auto right = static_cast<unsigned char>(r[part][i]);
						if (left != right)
							return left < right;
					}
					if (l[part].size() != r[part].size())
						return l[part].size() < r[part].size();
				}
				return false;
			}
		};
		struct projection
		{
			budget& b;
			exceptional_exit_input input;
			exceptional_exit_projection output;
			using identity_map = std::map<identity, refs, identity_less>;
			std::array<identity_map, 8> maps;
			projection(budget& limits, exceptional_exit_input rows)
				: b(limits), input(rows), maps{identity_map{identity_less{&b}},
											   identity_map{identity_less{&b}},
											   identity_map{identity_less{&b}},
											   identity_map{identity_less{&b}},
											   identity_map{identity_less{&b}},
											   identity_map{identity_less{&b}},
											   identity_map{identity_less{&b}},
											   identity_map{identity_less{&b}}}
			{
			}
			using scope_identity = std::array<std::string, 5>;
			struct scope_less
			{
				using is_transparent = void;
				budget* meter;
				template <class L, class R>
				bool operator()(const L& l, const R& r) const
				{
					for (std::size_t i = 0; i < 5U; ++i)
					{
						meter->work();
						const auto common = std::min(l[i].size(), r[i].size());
						for (std::size_t at{}; at < common; ++at)
						{
							meter->work();
							const auto left = static_cast<unsigned char>(l[i][at]);
							const auto right = static_cast<unsigned char>(r[i][at]);
							if (left != right)
								return left < right;
						}
						if (l[i].size() != r[i].size())
							return l[i].size() < r[i].size();
					}
					return false;
				}
			};
			std::map<scope_identity, exceptional_exit_population, scope_less> scopes{
				scope_less{&b}};
			void canonical(refs& values)
			{
				b.work(values.size() *
					   (static_cast<std::size_t>(std::bit_width(values.size())) + 1U));
				std::ranges::sort(values);
				values.erase(std::ranges::unique(values).begin(), values.end());
			}
			bool equal(std::string_view left, std::string_view right)
			{
				b.work();
				if (left.size() != right.size())
					return false;
				for (std::size_t at{}; at < left.size(); ++at)
				{
					b.work();
					if (static_cast<unsigned char>(left[at]) !=
						static_cast<unsigned char>(right[at]))
						return false;
				}
				return true;
			}
			std::string copy(std::string_view s)
			{
				b.work(s.size() + 1U);
				b.retain(sizeof(std::string) + s.size() * 2U);
				return std::string{s};
			}
			std::string raw_bytes(const annotated_row& row, std::string_view name)
			{
				const auto* v = bytes(row, name);
				if (!v)
					return {};
				b.work(v->size());
				b.retain(sizeof(std::string) + v->size() * 2U);
				std::string out;
				out.reserve(v->size());
				for (auto c : *v)
					out.push_back(static_cast<char>(std::to_integer<unsigned char>(c)));
				return out;
			}
			const annotated_row& row(std::size_t ref) const
			{
				return output.evidence.at(ref).row;
			}
			const refs&
			find(std::size_t group, std::string_view id, const exceptional_exit_population& p)
			{
				static const refs empty;
				const auto at =
					maps[group].find(view_identity{id, p.universe, p.variant, p.interpretation});
				return at == maps[group].end() ? empty : at->second;
			}
			void bind(refs& into, const refs& from)
			{
				b.bind(into, from);
			}
			template <class T>
			void gap(T& into, std::string_view subject, std::string_view reason)
			{
				b.work();
				b.retain(sizeof(query_unresolved) + 2U * (subject.size() + reason.size()) + 128U);
				into.gaps.push_back(
					{"sdk.exceptional-exit-" + std::string{reason}, std::string{subject}, {}});
			}
			std::size_t cell_cost(const detached_cell* c)
			{
				if (!c)
					return 1U;
				std::size_t n = 1U + c->type.parameter.size();
				if (c->unknown_reason)
					n += c->unknown_reason->size();
				if (c->value)
					std::visit(
						[&](const auto& v)
						{
							if constexpr (requires { v.size(); })
								n += v.size();
						},
						*c->value);
				return n;
			}
			bool agree(const refs& original, std::initializer_list<std::string_view> fields)
			{
				if (original.empty())
					return false;
				for (const auto ref : original)
				{
					b.work();
					if (ref == original.front())
						continue;
					for (const auto name : fields)
					{
						const auto* x = cell(row(original.front()), name);
						const auto* y = cell(row(ref), name);
						b.work(cell_cost(x) + cell_cost(y));
						if (!x || !y)
						{
							if (x != y)
								return false;
						}
						else if (x->type != y->type || x->state != y->state ||
								 x->value != y->value || x->unknown_reason != y->unknown_reason)
							return false;
					}
				}
				return true;
			}
			std::vector<std::string> strings(const annotated_row& r, std::string_view name)
			{
				std::vector<std::string> out;
				const auto* data = bytes(r, name);
				if (!data)
					return out;
				for (std::size_t pos = 0; pos < data->size();)
				{
					b.work();
					if (data->size() - pos < 4U)
						fail(name, "truncated-set");
					std::uint32_t size{};
					for (unsigned shift = 0; shift < 32U; shift += 8U)
						size |= std::to_integer<std::uint32_t>((*data)[pos++]) << shift;
					if (size > data->size() - pos)
						fail(name, "truncated-set");
					b.charge(b.members, 1U, b.limits.maximum_members, "set-members");
					b.retain(sizeof(std::string) + size * 2U);
					std::string s;
					s.reserve(size);
					for (std::size_t i = 0; i < size; ++i)
						s.push_back(
							static_cast<char>(std::to_integer<unsigned char>((*data)[pos++])));
					if (s.empty())
						fail(name, "invalid-set-member");
					out.push_back(std::move(s));
				}
				std::ranges::sort(out,
								  [&](const auto& x, const auto& y)
								  {
									  b.work(x.size() + y.size() + 1U);
									  return x < y;
								  });
				if (std::adjacent_find(out.begin(), out.end()) != out.end())
					fail(name, "duplicate-set-member");
				return out;
			}
			finite_population_state
			source(std::string_view id, exceptional_exit_population& p, refs& evidence)
			{
				const auto& spans = find(2U, id, p);
				if (id.empty() || spans.empty())
					return finite_population_state::unknown;
				bind(evidence, spans);
				if (!agree(spans, {"span", "snapshot", "file", "begin", "end"}))
					return finite_population_state::conflicting;
				const auto& s = row(spans.front());
				const auto& files = find(1U, text(s, "snapshot"), p);
				if (files.empty())
					return finite_population_state::partial;
				bind(evidence, files);
				if (!agree(files, {"snapshot", "file", "size"}))
					return finite_population_state::conflicting;
				const auto begin = number(s, "begin"), end = number(s, "end"),
						   size = number(row(files.front()), "size");
				if (!begin || !end || !size || !present(s, "file") ||
					!present(row(files.front()), "file"))
					return finite_population_state::partial;
				if (text(s, "file") != text(row(files.front()), "file") || *begin > *end ||
					*end > *size)
					return finite_population_state::conflicting;
				return finite_population_state::complete;
			}
			exceptional_exit_population& scope(std::string_view detail,
											   std::string_view unit,
											   const annotated_row& r,
											   std::string_view variant)
			{
				const std::array<std::string_view, 5> view{
					detail, unit, r.presence.universe, variant, r.interpretation};
				b.work();
				auto at = scopes.find(view);
				if (at != scopes.end())
					return at->second;
				b.retain(sizeof(scope_identity) +
						 2U *
							 (detail.size() + unit.size() + r.presence.universe.size() +
							  variant.size() + r.interpretation.size()) +
						 256U);
				scope_identity key{std::string{detail},
								   std::string{unit},
								   r.presence.universe,
								   std::string{variant},
								   r.interpretation};
				b.charge(b.populations, 1U, b.limits.maximum_populations, "populations");
				b.retain(sizeof(exceptional_exit_population));
				auto& p = scopes[key];
				p.detail = copy(detail);
				p.compile_unit = copy(unit);
				p.universe = copy(r.presence.universe);
				p.variant = copy(variant);
				p.interpretation = copy(r.interpretation);
				return p;
			}
			void census(exceptional_exit_population& p, const refs& details, const refs& bodies)
			{
				refs facets;
				for (const auto* group : {&details, &bodies})
					for (auto ref : *group)
					{
						b.work();
						const auto& r = row(ref);
						bool any = false, all = true;
						for (const auto f : {"exceptional_exit_count",
											 "exceptional_exit_ids",
											 "exceptional_exit_state",
											 "exceptional_exit_profile",
											 "exceptional_lowering_profile"})
						{
							any |= present(r, f);
							all &= present(r, f);
						}
						if (any && !all)
						{
							p.enumeration_state =
								combine(p.enumeration_state, finite_population_state::partial);
							gap(p, p.detail, "census-facet-incomplete");
						}
						if (all)
						{
							b.retain(sizeof(ref));
							facets.push_back(ref);
						}
					}
				if (facets.empty())
				{
					gap(p, p.detail, "census-unobserved");
					return;
				}
				const auto& r = row(facets.front());
				p.occurrence_count = number(r, "exceptional_exit_count");
				p.occurrence_ids = strings(r, "exceptional_exit_ids");
				p.profile = copy(text(r, "exceptional_exit_profile"));
				p.lowering_profile = copy(text(r, "exceptional_lowering_profile"));
				if (!agree(facets,
						   {"exceptional_exit_count",
							"exceptional_exit_ids",
							"exceptional_exit_state",
							"exceptional_exit_profile",
							"exceptional_lowering_profile"}))
				{
					p.enumeration_state = finite_population_state::conflicting;
					gap(p, p.detail, "census-conflicting");
					return;
				}
				const auto state = text(r, "exceptional_exit_state");
				if (p.profile != occurrence_profile || p.lowering_profile != lowering_profile ||
					state != "complete" || !input.compile_units_complete ||
					!input.scope_inputs_complete || !input.occurrence_inputs_complete)
				{
					p.enumeration_state = state == "conflicting"
						? finite_population_state::conflicting
						: finite_population_state::partial;
					gap(p, p.detail, "census-unavailable");
					return;
				}
				if (!p.occurrence_count || *p.occurrence_count != p.occurrence_ids.size())
				{
					p.enumeration_state = finite_population_state::conflicting;
					gap(p, p.detail, "census-cardinality");
					return;
				}
				if (p.enumeration_state != finite_population_state::partial)
					p.enumeration_state = finite_population_state::complete;
			}
			void scope_binding(exceptional_exit_population& p)
			{
				const auto& details = find(4U, p.detail, p);
				bind(p.evidence, details);
				p.scope_state = finite_population_state::complete;
				if (details.empty())
				{
					p.scope_state = finite_population_state::unknown;
					gap(p, p.detail, "scope-detail-missing");
					return;
				}
				if (!agree(details, {"detail", "compile_unit", "entity", "source"}))
				{
					p.scope_state = finite_population_state::conflicting;
					gap(p, p.detail, "scope-detail-conflicting");
					return;
				}
				const auto& d = row(details.front());
				p.function = copy(text(d, "entity"));
				p.definition_source = copy(text(d, "source"));
				if (text(d, "compile_unit") != p.compile_unit)
				{
					p.scope_state = finite_population_state::conflicting;
					gap(p, p.detail, "scope-unit-conflicting");
				}
				const auto& units = find(0U, p.compile_unit, p);
				bind(p.evidence, units);
				if (units.empty())
					p.scope_state = combine(p.scope_state, finite_population_state::unknown);
				const auto& entities = find(3U, p.function, p);
				bind(p.evidence, entities);
				if (entities.empty())
					p.scope_state = combine(p.scope_state, finite_population_state::unknown);
				else if (!agree(entities, {"entity", "kind"}))
					p.scope_state = finite_population_state::conflicting;
				else
				{
					const auto kind = text(row(entities.front()), "kind");
					if (kind != "function" && kind != "method" && kind != "constructor" &&
						kind != "destructor" && kind != "conversion")
						p.scope_state = finite_population_state::conflicting;
				}
				bool written = false, absent = false;
				for (auto ref : details)
				{
					auto flags = strings(row(ref), "flags");
					if (std::ranges::find(flags, "finite_function_body_v1") != flags.end())
					{
						written |= std::ranges::find(flags, "body_written") != flags.end();
						absent |= std::ranges::find(flags, "body_absent") != flags.end();
					}
				}
				if (!written)
					p.scope_state = combine(p.scope_state, finite_population_state::unknown);
				if (written && absent)
					p.scope_state = finite_population_state::conflicting;

				// The actual FunctionDecl range and its lexical body range differ.
				// Only the original lowering's body FK associates the two carriers.
				std::set<std::string_view> body_ids;
				for (const auto& [key, original] : maps[7U])
				{
					b.work();
					if (!equal(key[1], p.universe) || !equal(key[2], p.variant) ||
						!equal(key[3], p.interpretation))
						continue;
					for (auto ref : original)
					{
						b.work();
						const auto& e = row(ref);
						if (belongs(e, p) && present(e, "body"))
						{
							b.retain(sizeof(std::string_view) + 96U);
							body_ids.insert(text(e, "body"));
						}
					}
				}
				refs bodies;
				if (body_ids.size() > 1U)
				{
					p.scope_state = finite_population_state::conflicting;
					gap(p, p.detail, "scope-body-conflicting");
				}
				for (auto id : body_ids)
				{
					const auto& original = find(5U, id, p);
					bind(bodies, original);
					p.body = copy(id);
					if (original.empty())
					{
						p.scope_state = combine(p.scope_state, finite_population_state::unknown);
						gap(p, id, "scope-body-missing");
						continue;
					}
					if (!agree(original, {"body", "compile_unit", "function", "source"}))
					{
						p.scope_state = finite_population_state::conflicting;
						gap(p, id, "scope-body-conflicting");
						continue;
					}
					const auto& body = row(original.front());
					if (!present(body, "compile_unit") || !present(body, "function"))
						p.scope_state = combine(p.scope_state, finite_population_state::unknown);
					else if (text(body, "compile_unit") != p.compile_unit ||
							 text(body, "function") != p.function)
					{
						p.scope_state = finite_population_state::conflicting;
						gap(p, id, "scope-body-owner-conflicting");
					}
					p.scope_state =
						combine(p.scope_state, source(text(body, "source"), p, p.evidence));
				}
				bind(p.evidence, bodies);

				p.scope_state = combine(p.scope_state, source(p.definition_source, p, p.evidence));
				const auto& spans = find(2U, p.definition_source, p);
				if (!spans.empty())
				{
					p.file = copy(text(row(spans.front()), "file"));
					p.source_snapshot = copy(text(row(spans.front()), "snapshot"));
				}
				census(p, details, bodies);
			}

			bool callable(std::string_view kind) const
			{
				return kind == "function" || kind == "method" || kind == "constructor" ||
					kind == "destructor" || kind == "conversion";
			}
			bool belongs(const annotated_row& r, const exceptional_exit_population& p) const
			{
				return text(r, "scope_detail") == p.detail &&
					text(r, "compile_unit") == p.compile_unit;
			}
			void discover()
			{
				for (const auto& [key, original] : maps[4U])
					for (auto ref : original)
					{
						b.work();
						const auto& r = row(ref);
						exceptional_exit_population context;
						context.universe = key[1];
						context.variant = key[2];
						context.interpretation = key[3];
						const auto& entities = find(3U, text(r, "entity"), context);
						bool known_noncallable = !entities.empty() &&
							agree(entities, {"entity", "kind"}) &&
							!callable(text(row(entities.front()), "kind"));
						auto flags = strings(r, "flags");
						const auto has = [&](std::string_view name)
						{
							return std::ranges::find(flags, name) != flags.end();
						};
						const bool absent = has("finite_function_body_v1") && has("body_absent") &&
							!has("body_written");
						const bool facet = present(r, "exceptional_exit_count") ||
							present(r, "exceptional_exit_ids") ||
							present(r, "exceptional_exit_state");
						if ((known_noncallable || absent) && !facet)
							continue;
						if (entities.empty() && !facet && !has("body_written"))
							continue;
						scope(key[0], text(r, "compile_unit"), r, key[2]);
					}
				for (const auto& [key, original] : maps[7U])
					for (auto ref : original)
					{
						b.work();
						const auto& r = row(ref);
						scope(text(r, "scope_detail"), text(r, "compile_unit"), r, key[2]);
					}
				// A physical written body without its detail/census is an independent
				// frontier, rather than a fabricated known-empty lowering.
				for (const auto& [key, original] : maps[5U])
					for (auto ref : original)
					{
						b.work();
						const auto& r = row(ref);
						bool matched = false;
						for (const auto& [scope_key, p] : scopes)
						{
							b.work();
							if (equal(scope_key[2], key[1]) && equal(scope_key[3], key[2]) &&
								equal(scope_key[4], key[3]) &&
								p.compile_unit == text(r, "compile_unit"))
							{
								const auto& details = find(4U, p.detail, p);
								for (auto d : details)
								{
									b.work();
									if (text(row(d), "entity") == text(r, "function"))
										matched = true;
								}
							}
						}
						if (!matched)
						{
							auto& p = scope({}, text(r, "compile_unit"), r, key[2]);
							p.function = copy(text(r, "function"));
							p.definition_source = copy(text(r, "source"));
							p.body = copy(text(r, "body"));
							gap(p, p.body, "written-body-detail-missing");
						}
					}
			}
			finite_population_state expression(observed_exceptional_exit& o,
											   exceptional_exit_population& p,
											   const refs& original)
			{
				if (!agree(original, {"expression"}))
					return finite_population_state::conflicting;
				if (o.expression.empty())
					return finite_population_state::unknown;
				const auto& nodes = find(6U, o.expression, p);
				bind(o.evidence, nodes);
				if (nodes.empty())
					return finite_population_state::unknown;
				if (!agree(nodes, {"node", "compile_unit", "source", "function"}))
					return finite_population_state::conflicting;
				const auto& n = row(nodes.front());
				if (!present(n, "compile_unit"))
					return finite_population_state::unknown;
				if (text(n, "compile_unit") != p.compile_unit)
					return finite_population_state::conflicting;
				if (!present(n, "source"))
					return finite_population_state::unknown;
				if (!o.source_span.empty() && text(n, "source") != o.source_span)
					return finite_population_state::conflicting;
				// Declaration defaults can retain a different lexical owner. It is not an
				// execution-owner substitute; the original lowering scope remains
				// independent.
				if (present(n, "function") && text(n, "function") != p.function)
					return finite_population_state::partial;
				return finite_population_state::complete;
			}
			finite_population_state target(observed_exceptional_exit& o,
										   exceptional_exit_population& p,
										   const refs& original)
			{
				if (!agree(original, {"target", "target_usr"}))
					return finite_population_state::conflicting;
				if (o.target.empty())
					return finite_population_state::unknown;
				const auto& entities = find(3U, o.target, p);
				bind(o.evidence, entities);
				if (entities.empty())
					return finite_population_state::unknown;
				if (!agree(entities,
						   {"entity",
							"kind",
							"canonicalization",
							"provider_local_key",
							"structural_signature_digest"}))
					return finite_population_state::conflicting;
				const auto& e = row(entities.front());
				if (!callable(text(e, "kind")))
					return finite_population_state::conflicting;
				// provider_local_key is opaque; it is not a raw-USR equality contract.
				return finite_population_state::complete;
			}
			observed_exceptional_exit occurrence(const refs& original,
												 exceptional_exit_population& p)
			{
				b.charge(b.members, 1U, b.limits.maximum_members, "occurrences");
				b.retain(sizeof(observed_exceptional_exit));
				observed_exceptional_exit o;
				const auto& r = row(original.front());
				bind(o.evidence, original);
				o.exit = copy(text(r, "exit"));
				o.variant = copy(text(r, "variant"));
				o.role = copy(text(r, "role"));
				o.eligibility = copy(text(r, "eligibility"));
				o.profile = copy(text(r, "profile"));
				o.lowering_profile = copy(text(r, "lowering_profile"));
				o.observation_state = copy(text(r, "observation_state"));
				o.ordinal = number(r, "ordinal").value_or(0U);
				o.source_span = copy(text(r, "source"));
				o.expression = copy(text(r, "expression"));
				o.target = copy(text(r, "target"));
				o.target_usr = raw_bytes(r, "target_usr");
				o.original_expression_ordinal = number(r, "original_expression_ordinal");
				o.block_ordinal = number(r, "block_ordinal");
				o.instruction_ordinal = number(r, "instruction_ordinal");
				o.successor_ordinal = number(r, "successor_ordinal");
				o.intrinsic_id = number(r, "intrinsic_id");
				o.compiler_route = number(r, "compiler_route");
				o.emitter_methods = number(r, "emitter_methods");
				o.is_invoke = boolean(r, "is_invoke");
				o.does_not_throw = boolean(r, "does_not_throw");
				o.does_not_return = boolean(r, "does_not_return");
				o.state = finite_population_state::complete;
				if (!agree(original,
						   {"role",
							"eligibility",
							"profile",
							"lowering_profile",
							"observation_state",
							"ordinal",
							"variant_kind",
							"variant_index",
							"variant_symbol",
							"variant",
							"block_ordinal",
							"instruction_ordinal",
							"successor_ordinal",
							"intrinsic_id",
							"compiler_route",
							"emitter_methods",
							"is_invoke",
							"does_not_throw",
							"does_not_return",
							"original_expression_ordinal"}))
					o.state = finite_population_state::conflicting;
				else if (!number(r, "ordinal") || !number(r, "variant_index") ||
						 o.profile != occurrence_profile ||
						 o.lowering_profile != lowering_profile ||
						 o.observation_state != "complete")
					o.state = finite_population_state::partial;
				else if (o.eligibility != "eligible" && o.eligibility != "excluded")
					o.state = finite_population_state::partial;
				else if (o.role == "written_throw" || o.role == "escaping_call" ||
						 o.role == "unhandled_resume" || o.role == "termination")
				{
					if (o.eligibility != "eligible")
						o.state = finite_population_state::conflicting;
				}
				else if (o.role == "lowering_helper" || o.role == "ordinary_instruction" ||
						 o.role == "lowering_variant")
				{
					if (o.eligibility != "excluded")
						o.state = finite_population_state::conflicting;
				}
				else
					o.state = finite_population_state::partial;
				if (!agree(original, {"source"}))
					o.source_state = finite_population_state::conflicting;
				else
					o.source_state = source(o.source_span, p, o.evidence);
				o.expression_state = expression(o, p, original);
				o.target_state = target(o, p, original);
				if (o.state != finite_population_state::complete)
					gap(o, o.exit, "observation-unavailable");
				if (o.source_state != finite_population_state::complete)
					gap(o, o.exit, "source-unavailable");
				if (o.expression_state != finite_population_state::complete)
					gap(o, o.exit, "expression-unavailable");
				if (o.target_state != finite_population_state::complete)
					gap(o, o.exit, "target-unavailable");
				return o;
			}
			void populate(exceptional_exit_population& p)
			{
				std::map<std::string_view, const refs*, std::less<>> actual;
				for (const auto& [key, original] : maps[7U])
				{
					b.work();
					if (!equal(key[1], p.universe) || !equal(key[2], p.variant) ||
						!equal(key[3], p.interpretation))
						continue;
					if (std::ranges::any_of(original,
											[&](auto ref)
											{
												b.work();
												return belongs(row(ref), p);
											}))
					{
						b.retain(sizeof(decltype(actual)::value_type) + 96U);
						actual.emplace(key[0], &original);
					}
				}
				if (p.enumeration_state == finite_population_state::complete)
				{
					if (actual.size() != p.occurrence_ids.size())
					{
						p.enumeration_state = finite_population_state::conflicting;
						gap(p, p.detail, "census-membership");
					}
					for (const auto& id : p.occurrence_ids)
					{
						b.work(id.size() + 1U);
						if (!actual.contains(id))
						{
							p.enumeration_state = finite_population_state::conflicting;
							gap(p, id, "census-member-missing");
						}
					}
				}
				const std::initializer_list<std::string_view> membership_fields = {
					"exit",
					"compile_unit",
					"scope_detail",
					"function",
					"definition_source",
					"body",
					"variant_kind",
					"variant_index",
					"variant_symbol",
					"ordinal",
					"variant",
					"profile",
					"lowering_profile"};
				std::map<std::string_view, std::size_t, std::less<>> variants;
				for (const auto& [id, original] : actual)
				{
					b.work();
					const auto& r = row(original->front());
					if (!agree(*original, membership_fields) || !belongs(r, p))
					{
						p.enumeration_state = finite_population_state::conflicting;
						gap(p, id, "occurrence-identity-conflicting");
					}
					if (text(r, "function") != p.function ||
						text(r, "definition_source") != p.definition_source ||
						(!text(r, "body").empty() && text(r, "body") != p.body))
					{
						p.scope_state = finite_population_state::conflicting;
						gap(p, id, "occurrence-scope-conflicting");
					}
					if (text(r, "role") != "lowering_variant")
						continue;
					b.retain(sizeof(exceptional_exit_variant) +
							 sizeof(decltype(variants)::value_type) + 96U);
					exceptional_exit_variant v;
					v.variant = copy(id);
					v.kind = copy(text(r, "variant_kind"));
					v.index = number(r, "variant_index").value_or(0U);
					v.symbol = raw_bytes(r, "variant_symbol");
					bind(v.evidence, *original);
					auto carrier = occurrence(*original, p);
					v.state = carrier.state;
					if ((number(r, "ordinal") && carrier.ordinal != 0U) ||
						!carrier.variant.empty() || carrier.eligibility != "excluded")
					{
						v.state = finite_population_state::conflicting;
						p.enumeration_state = finite_population_state::conflicting;
						gap(v, id, "variant-carrier-invalid");
					}
					if (v.kind != "function" && v.kind != "constructor" && v.kind != "destructor")
					{
						v.state = combine(v.state, finite_population_state::partial);
						gap(v, id, "variant-kind-unknown");
					}
					if (v.kind == "function" && number(r, "variant_index") && v.index != 0U)
					{
						v.state = finite_population_state::conflicting;
						gap(v, id, "variant-index-conflicting");
					}
					bind(v.evidence, carrier.evidence);
					v.gaps = std::move(carrier.gaps);
					variants.emplace(id, p.variants.size());
					p.variants.push_back(std::move(v));
				}
				if (p.variants.empty())
				{
					p.enumeration_state =
						combine(p.enumeration_state, finite_population_state::partial);
					gap(p, p.detail, "lowering-variant-missing");
				}
				std::set<std::pair<std::size_t, std::uint64_t>> ordinals;
				for (const auto& [id, original] : actual)
				{
					const auto& r = row(original->front());
					b.work();
					if (text(r, "role") == "lowering_variant")
						continue;
					const auto at = variants.find(text(r, "variant"));
					if (at == variants.end())
					{
						p.enumeration_state = input.occurrence_inputs_complete
							? finite_population_state::conflicting
							: finite_population_state::partial;
						gap(p, id, "variant-carrier-missing");
						continue;
					}
					auto& v = p.variants[at->second];
					auto o = occurrence(*original, p);
					const auto& carrier = row(actual.at(v.variant)->front());
					if (!agree(*original, membership_fields) ||
						text(r, "variant_kind") != text(carrier, "variant_kind") ||
						number(r, "variant_index") != number(carrier, "variant_index") ||
						bytes(r, "variant_symbol") == nullptr ||
						bytes(carrier, "variant_symbol") == nullptr ||
						*bytes(r, "variant_symbol") != *bytes(carrier, "variant_symbol"))
					{
						o.state = finite_population_state::conflicting;
						p.enumeration_state = finite_population_state::conflicting;
						gap(o, id, "variant-binding-conflicting");
					}
					b.retain(sizeof(decltype(ordinals)::value_type) + 96U);
					if (number(r, "ordinal") &&
						(o.ordinal == 0U || !ordinals.emplace(at->second, o.ordinal).second))
					{
						o.state = finite_population_state::conflicting;
						p.enumeration_state = finite_population_state::conflicting;
						gap(o, id, "ordinal-conflicting");
					}
					v.state = combine(v.state, o.state);
					v.occurrences.push_back(std::move(o));
				}
				// Distinct physical variant carriers cannot claim the same original
				// lowering.
				for (std::size_t i = 0; i < p.variants.size(); ++i)
					for (std::size_t j = 0; j < i; ++j)
					{
						b.work(p.variants[i].symbol.size() + p.variants[j].symbol.size() + 1U);
						if (p.variants[i].kind == p.variants[j].kind &&
							p.variants[i].index == p.variants[j].index &&
							p.variants[i].symbol == p.variants[j].symbol)
						{
							p.enumeration_state = finite_population_state::conflicting;
							p.variants[i].state = finite_population_state::conflicting;
							p.variants[j].state = finite_population_state::conflicting;
							gap(p, p.detail, "duplicate-lowering-variant");
						}
					}
				p.state = combine(p.enumeration_state, p.scope_state);
				for (auto& v : p.variants)
				{
					p.state = combine(p.state, v.state);
					canonical(v.evidence);
					for (auto& o : v.occurrences)
						canonical(o.evidence);
					std::ranges::sort(v.occurrences,
									  [&](const auto& x, const auto& y)
									  {
										  b.work(x.exit.size() + y.exit.size() + 1U);
										  return std::tie(x.ordinal, x.exit) <
											  std::tie(y.ordinal, y.exit);
									  });
				}
				for (auto& v : p.variants)
					for (std::size_t i = 0; i < v.occurrences.size(); ++i)
					{
						b.work();
						if (v.occurrences[i].state == finite_population_state::complete &&
							v.occurrences[i].ordinal != i + 1U)
						{
							v.state = finite_population_state::conflicting;
							p.enumeration_state = finite_population_state::conflicting;
							p.state = finite_population_state::conflicting;
							gap(v, v.occurrences[i].exit, "ordinal-membership-conflicting");
						}
					}
				canonical(p.evidence);
			}
			void finish()
			{
				discover();
				for (auto& [key, p] : scopes)
				{
					(void)key;
					scope_binding(p);
					populate(p);
					b.retain(sizeof(exceptional_exit_population));
					output.populations.push_back(std::move(p));
				}
			}
		};
		result<exceptional_exit_projection>
		project_rows(exceptional_exit_input input,
					 finite_population_limits limits,
					 std::stop_token stop,
					 budget& b,
					 const std::array<std::vector<const annotated_row*>, 8>* borrowed = nullptr,
					 bool row_validation_reused = false,
					 const size_groups* wire_sizes = nullptr)
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
				work.output.occurrence_inputs_complete = input.occurrence_inputs_complete;
				const std::array groups{input.units,
										input.files,
										input.spans,
										input.entities,
										input.details,
										input.bodies,
										input.syntax_nodes,
										input.exits};
				const auto visit = [&](std::size_t group, auto&& callback)
				{
					if (borrowed)
					{
						for (const auto* r : (*borrowed)[group])
							callback(*r);
					}
					else
						for (const auto& r : groups[group])
							callback(r);
				};
				const auto descriptors = standard_relation_descriptors();

				// Validate every original row and input quota before staging any
				// selected evidence. Independent exceptional membership does not
				// require owning unrelated syntax or lexical-token spans.
				for (std::size_t group = 0; group < groups.size(); ++group)
				{
					const auto d =
						std::ranges::find(descriptors, relations[group], &relation_descriptor::id);
					if (d == descriptors.end())
						fail(relations[group], "descriptor-missing");
					visit(group,
						  [&](const annotated_row& r)
						  {
							  b.work();
							  b.charge(b.rows, 1U, limits.maximum_rows, "rows");
							  b.charge(b.conditions,
									   r.presence.fragments.size(),
									   limits.maximum_condition_expansions,
									   "conditions");
							  if (!row_validation_reused)
							  {
								  for (const auto& [name, c] : r.values)
								  {
									  b.work(name.size() + c.type.parameter.size() + 1U);
									  if (c.unknown_reason)
										  b.work(c.unknown_reason->size());
									  if (c.value)
										  std::visit(
											  [&](const auto& value)
											  {
												  if constexpr (requires { value.size(); })
													  b.work(value.size());
											  },
											  *c.value);
								  }
								  const auto strings = [&](const auto& values)
								  {
									  for (const auto& value : values)
										  b.work(value.size() + 1U);
								  };
								  strings(r.presence.fragments);
								  strings(r.claim_contributors);
								  strings(r.provenance);
								  b.work(r.presence.universe.size() + r.interpretation.size() + 1U);
								  for (const auto& producer : r.producer_contracts)
									  b.work(producer.id.size() +
											 producer.semantic_contract.size() + 1U);
								  for (const auto& guarantee : r.contributor_guarantees)
								  {
									  b.work(guarantee.approximation.size() +
											 guarantee.scope.size() + guarantee.assumptions.size() +
											 1U);
									  strings(guarantee.verification_modalities);
								  }
								  for (const auto& edge : r.contributor_edges)
								  {
									  b.work(edge.claim_contributor.size() +
											 edge.provenance.size() + edge.interpretation.size() +
											 edge.condition.universe.size() +
											 edge.producer.id.size() +
											 edge.producer.semantic_contract.size() +
											 edge.guarantee.approximation.size() +
											 edge.guarantee.scope.size() +
											 edge.guarantee.assumptions.size() + 1U);
									  strings(edge.condition.fragments);
									  strings(edge.guarantee.verification_modalities);
								  }
							  }
							  if (auto valid = detail::validate_projected_relation_row(
									  r,
									  *d,
									  "sdk.exceptional-exit-input-invalid",
									  [&]
									  {
										  b.work();
									  },
									  row_validation_reused);
								  !valid)
								  throw failure{valid.error()};
							  if (text(r, identifiers[group]).empty())
								  fail(relations[group], "identity-missing");
						  });
				}
				std::set<view_identity, identity_less> needed_syntax{identity_less{&b}},
					needed_sources{identity_less{&b}};
				const auto add_needed =
					[&](auto& into, const annotated_row& r, std::string_view field)
				{
					const auto id = text(r, field);
					if (id.empty())
						return;
					for (const auto& variant : r.presence.fragments)
					{
						const view_identity key{id, r.presence.universe, variant, r.interpretation};
						if (!into.contains(key))
						{
							b.retain(sizeof(view_identity) + 128U);
							into.insert(key);
						}
					}
				};
				const auto selected =
					[&](const auto& from, const annotated_row& r, std::string_view field)
				{
					const auto id = text(r, field);
					for (const auto& variant : r.presence.fragments)
					{
						if (from.contains(
								view_identity{id, r.presence.universe, variant, r.interpretation}))
							return true;
					}
					return false;
				};
				visit(7U,
					  [&](const annotated_row& r)
					  {
						  add_needed(needed_syntax, r, "expression");
					  });
				for (auto group : {4U, 5U, 7U})
					visit(group,
						  [&](const annotated_row& r)
						  {
							  add_needed(needed_sources, r, "source");
							  if (group == 7U)
								  add_needed(needed_sources, r, "definition_source");
						  });
				visit(6U,
					  [&](const annotated_row& r)
					  {
						  if (selected(needed_syntax, r, "node"))
							  add_needed(needed_sources, r, "source");
					  });
				const auto full_size = [&](const annotated_row& r, std::size_t group)
				{
					const auto step = [&]
					{
						b.work();
					};
					const auto overflow = [&]
					{
						fail("evidence-bytes", "limit-exceeded", "sdk.exceptional-exit-budget");
					};
					std::optional<std::size_t> base;
					if (wire_sizes)
						for (const auto& owner : (*wire_sizes)[group])
							if ((base = owner.find(&r, step)))
								break;
					return base ? detail::admitted_projected_row_size_from_base(
									  *base, r.multiplicity, step, overflow)
								: detail::admitted_projected_row_size(r, step, overflow);
				};
				struct entry
				{
					std::size_t group;
					const annotated_row* original;
					std::string canonical;
				};
				std::vector<entry> entries;

				for (std::size_t group = 0; group < groups.size(); ++group)
				{
					visit(group,
						  [&](const annotated_row& r)
						  {
							  b.work();
							  if ((group == 6U && !selected(needed_syntax, r, "node")) ||
								  (group == 2U && !selected(needed_sources, r, "span")))
								  return;
							  b.retain(b.estimate(r, false));
							  const auto temporary = b.estimate(r);
							  if (temporary > (limits.maximum_retained_bytes - b.retained) / 2U)
								  fail("canonical-temporary",
									   "limit-exceeded",
									   "sdk.exceptional-exit-budget");
							  b.temporary_peak =
								  std::max(b.temporary_peak, b.retained + 2U * temporary);
							  const auto encoded_size = full_size(r, group);
							  b.charge(b.evidence,
									   encoded_size,
									   limits.maximum_evidence_bytes,
									   "evidence-bytes");
							  // This complete array is the first variable field of the fixed
							  // annotated-row wire grammar. Distinct arrays decide full row
							  // order; only equal arrays need the remaining canonical fields.
							  detail::projected_row_size_sink prefix_size{
								  [&]
								  {
									  b.work();
								  },
								  [&]
								  {
									  fail("canonical-prefix",
										   "limit-exceeded",
										   "sdk.exceptional-exit-budget");
								  }};
							  detail::emit_row_strings(prefix_size, r.claim_contributors);
							  b.work(prefix_size.size() + 1U);
							  b.retain(sizeof(entry) + prefix_size.size() * 2U);
							  detail::projected_row_string_sink prefix;
							  detail::emit_row_strings(prefix, r.claim_contributors);
							  auto encoded = std::move(prefix.value);
							  entries.push_back({group, &r, std::move(encoded)});
						  });
				}
				std::ranges::sort(entries,
								  [&](const auto& x, const auto& y)
								  {
									  b.work();
									  if (x.group != y.group)
										  return x.group < y.group;
									  return b.canonical_less(x.canonical, y.canonical);
								  });
				for (std::size_t first{}; first < entries.size();)
				{
					b.work();
					std::size_t last = first + 1U;
					while (last < entries.size())
					{
						b.work();
						if (entries[first].group != entries[last].group ||
							entries[first].canonical.size() != entries[last].canonical.size() ||
							b.canonical_less(entries[first].canonical, entries[last].canonical))
							break;
						++last;
					}
					if (last - first > 1U)
					{
						for (std::size_t i = first; i < last; ++i)
						{
							b.work();
							const auto temporary = b.estimate(*entries[i].original);
							if (temporary > (limits.maximum_retained_bytes - b.retained) / 2U)
								fail("canonical-temporary",
									 "limit-exceeded",
									 "sdk.exceptional-exit-budget");
							b.temporary_peak =
								std::max(b.temporary_peak, b.retained + 2U * temporary);
							const auto encoded_size =
								full_size(*entries[i].original, entries[i].group);
							b.work(encoded_size + 1U);
							b.retain(encoded_size * 2U);
							auto encoded = entries[i].original->canonical_form();
							entries[i].canonical = std::move(encoded);
						}
						std::ranges::sort(std::span{entries}.subspan(first, last - first),
										  [&](const auto& x, const auto& y)
										  {
											  return b.canonical_less(x.canonical, y.canonical);
										  });
					}
					first = last;
				}
				for (const auto& e : entries)
				{
					b.work();
					const auto ref = work.output.evidence.size();
					b.retain(sizeof(finite_population_evidence) + relations[e.group].size() * 2U);
					b.work(relations[e.group].size() + 1U);
					work.output.evidence.push_back(
						{std::string{relations[e.group]},
						 detail::copy_projected_row(*e.original,
													[&](std::size_t amount)
													{
														b.work(amount);
													})});
					for (const auto& variant : e.original->presence.fragments)
					{
						b.work();
						const auto id = text(*e.original, identifiers[e.group]);
						b.retain(sizeof(identity) + sizeof(ref) + 512U +
								 2U *
									 (id.size() + e.original->presence.universe.size() +
									  variant.size() + e.original->interpretation.size()));
						work.maps[e.group][identity{std::string{id},
													e.original->presence.universe,
													variant,
													e.original->interpretation}]
							.push_back(ref);
					}
				}
				work.finish();
				return std::move(work.output);
			}
			catch (const failure& e)
			{
				return e.value;
			}
			catch (const std::bad_alloc&)
			{
				return error{"sdk.exceptional-exit-resource-exhausted", "projection", "allocation"};
			}
			catch (const std::length_error&)
			{
				return error{"sdk.exceptional-exit-resource-exhausted", "projection", "length"};
			}
		}
		result<exceptional_exit_projection>
		project_queries(const application_query_results& input,
						finite_population_limits limits,
						std::stop_token stop,
						projection_resource_usage* usage = nullptr)
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
																  "sdk.exceptional-exit",
																  &plan_bytes,
																  cancelled);
					!valid)
					return valid.error();
				b.retain(plan_bytes);
				std::array<std::vector<const annotated_row*>, 8> groups;
				size_groups wire_sizes;
				std::array<bool, 8> present{}, complete{};
				bool row_validation_reused = true;
				complete.fill(true);
				for (const auto& scan : input.scans)
				{
					b.work();
					b.retain(query_transfer_access::row_size_metadata_bytes(
						scan.result,
						[&]
						{
							fail("retained-bytes", "limit-exceeded", "sdk.exceptional-exit-budget");
						}));
					const auto name = std::ranges::find(relations, scan.relation_id);
					if (name == relations.end())
						continue;
					const auto group = static_cast<std::size_t>(name - relations.begin());
					present[group] = true;
					row_validation_reused &= query_transfer_access::rows_validated(scan.result);
					complete[group] &= scan.result.execution() == execution_status::complete &&
						scan.result.conflicts().empty() &&
						scan.result.differential_disagreements().empty();
					const auto rows = query_transfer_access::borrow_rows(scan.result);
					const auto sizes = query_transfer_access::borrow_row_sizes(scan.result);
					if (!sizes.base_sizes.empty())
					{
						b.work();
						b.retain(2U * sizeof(query_transfer_access::row_size_view));
						if (wire_sizes[group].size() == wire_sizes[group].capacity())
						{
							const auto capacity = wire_sizes[group].capacity();
							if (capacity > std::numeric_limits<std::size_t>::max() / 2U ||
								capacity > (b.limits.maximum_operations - 1U) / 4U)
								fail("operations", "limit-exceeded", "sdk.exceptional-exit-budget");
							b.work(4U * capacity + 1U);
							if (capacity > (b.limits.maximum_retained_bytes - b.retained) /
									sizeof(query_transfer_access::row_size_view))
								fail("retained-bytes",
									 "limit-exceeded",
									 "sdk.exceptional-exit-budget");
							b.temporary_peak = std::max(
								b.temporary_peak,
								b.retained +
									capacity * sizeof(query_transfer_access::row_size_view));
							wire_sizes[group].reserve(capacity ? capacity * 2U : 1U);
						}
						wire_sizes[group].push_back(sizes);
					}
					b.charge(b.rows, rows.size(), limits.maximum_rows, "scan-rows");
					b.retain(1024U + scan.relation_id.size() * 2U +
							 (groups[group].size() + rows.size()) * sizeof(const annotated_row*));
					groups[group].reserve(groups[group].size() + rows.size());
					for (const auto& r : rows)
					{
						b.work();
						groups[group].push_back(&r);
					}
				}
				const auto available = [&](std::size_t group)
				{
					return present[group] && complete[group];
				};
				b.rows = 0;
				exceptional_exit_input raw{};
				raw.compile_units_complete = available(0U);
				raw.scope_inputs_complete = available(4U) && available(5U);
				raw.occurrence_inputs_complete = available(7U);
				auto output =
					project_rows(raw, limits, stop, b, &groups, row_validation_reused, &wire_sizes);
				if (!output)
					return output.error();
				for (std::size_t group = 0; group < groups.size(); ++group)
					if (!present[group])
					{
						b.retain(sizeof(query_unresolved) + relations[group].size() + 256U);
						output->unresolved.push_back({"sdk.exceptional-exit-scan-missing",
													  std::string{relations[group]},
													  "independent-scan-unavailable"});
					}
				b.retain(sizeof(application_query_results) + input.snapshot_id.size() * 2U +
						 input.scans.size() * sizeof(application_relation_scan));
				output->source_queries = input;
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
				return error{"sdk.exceptional-exit-resource-exhausted", "projection", "allocation"};
			}
			catch (const std::length_error&)
			{
				return error{"sdk.exceptional-exit-resource-exhausted", "projection", "length"};
			}
		}
	} // namespace
	result<exceptional_exit_projection> project_exceptional_exits(exceptional_exit_input input,
																  finite_population_limits limits,
																  std::stop_token stop)
	{
		budget b{limits, stop};
		return project_rows(input, limits, stop, b);
	}
	result<exceptional_exit_projection>
	project_exceptional_exits(const application_query_results& input,
							  finite_population_limits limits,
							  std::stop_token stop)
	{
		return project_queries(input, limits, stop);
	}
	result<exceptional_exit_projection> project_exceptional_exits(exceptional_exit_input input,
																  finite_population_limits limits,
																  std::stop_token stop,
																  projection_resource_usage& usage)
	{
		usage = {};
		budget b{limits, stop};
		auto output = project_rows(input, limits, stop, b);
		if (output)
			usage = {b.operations, std::max(b.retained, b.temporary_peak)};
		return output;
	}
	result<exceptional_exit_projection>
	project_exceptional_exits(const application_query_results& input,
							  finite_population_limits limits,
							  std::stop_token stop,
							  projection_resource_usage& usage)
	{
		usage = {};
		return project_queries(input, limits, stop, &usage);
	}
} // namespace cxxlens::sdk::query
