#include <algorithm>
#include <array>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <tuple>

#include <cxxlens/sdk/target_resolution.hpp>

#include "query_projection_plan_limits_internal.hpp"
#include "query_projection_row_copy_internal.hpp"
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
															 "cc.type.v1",
															 "cc.declaration.v1",
															 "cc.syntax_node.v1",
															 "cc.operation.v1",
															 "source.include.v1",
															 "cc.entity_edge.v1",
															 "cc.call_site.v1",
															 "cc.declaration_inventory.v1",
															 "cc.target_resolution_slot.v1"};
		constexpr std::array<std::string_view, 13> identifiers{"compile_unit",
															   "snapshot",
															   "span",
															   "entity",
															   "type",
															   "declaration",
															   "node",
															   "operation",
															   "include",
															   "edge",
															   "call",
															   "inventory",
															   "slot"};
		constexpr std::string_view target_profile = "clang22-original-consumer-target-relations/1";
		struct failure
		{
			error value;
		};
		[[noreturn]] void fail(std::string_view field,
							   std::string_view reason,
							   std::string_view code = "sdk.target-input-invalid")
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
					fail(field, "limit-exceeded", "sdk.target-budget");
				used += amount;
			}
			void work(std::size_t n = 1)
			{
				if (stop.stop_requested() || (limits.cancelled && limits.cancelled()))
					fail("projection", "stop-requested", "sdk.target-cancelled");
				charge(operations, n, limits.maximum_operations, "operations");
			}
			int compare(std::string_view a, std::string_view c)
			{
				work();
				const auto common = std::min(a.size(), c.size());
				for (std::size_t i{}; i < common; ++i)
				{
					work();
					const auto left = static_cast<unsigned char>(a[i]);
					const auto right = static_cast<unsigned char>(c[i]);
					if (left != right)
						return left < right ? -1 : 1;
				}
				return a.size() == c.size() ? 0 : a.size() < c.size() ? -1 : 1;
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
						fail("row", "limit-exceeded", "sdk.target-budget");
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
			std::size_t owned_geometry(const annotated_row& row,
									   bool encoding = false,
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
						fail("row", "limit-exceeded", "sdk.target-budget");
					total += n * factor;
					if (validation_payload)
					{
						if (n > limits.maximum_operations - *validation_payload)
							fail("operations", "limit-exceeded", "sdk.target-budget");
						*validation_payload += n;
					}
				};
				const auto fixed = [&](std::size_t n)
				{
					work();
					if (total > limits.maximum_retained_bytes ||
						n > limits.maximum_retained_bytes - total)
						fail("row", "limit-exceeded", "sdk.target-budget");
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
		void canonical(std::vector<query_unresolved>& values, budget& b)
		{
			const auto order = [&](const auto& a, const auto& c)
			{
				if (const auto code = b.compare(a.code, c.code))
					return code;
				if (const auto subject = b.compare(a.subject, c.subject))
					return subject;
				return b.compare(a.detail, c.detail);
			};
			std::ranges::sort(values,
							  [&](const auto& a, const auto& c)
							  {
								  return order(a, c) < 0;
							  });
			values.erase(std::ranges::unique(values,
											 [&](const auto& a, const auto& c)
											 {
												 return order(a, c) == 0;
											 })
							 .begin(),
						 values.end());
		}

		struct entry
		{
			std::size_t group;
			const annotated_row* original;
			std::string canonical;
			std::size_t payload_bytes{};
		};
		struct projection
		{
			budget& b;
			target_resolution_input input;
			target_resolution_projection output;
			std::array<std::map<identity, refs>, 13> maps;
			std::map<identity, refs> unit_inventories, unit_slots, file_ids;
			std::vector<std::size_t> payload_bytes;
			std::map<std::pair<const annotated_row*, std::string>, std::vector<std::string>>
				decoded_sets;
			projection(budget& bounds, target_resolution_input raw) : b(bounds), input(raw) {}
			void gap(std::vector<query_unresolved>& gaps,
					 finite_population_state& axis,
					 std::string_view subject,
					 std::string_view reason,
					 bool conflict = false)
			{
				b.work();
				b.retain(sizeof(query_unresolved) + subject.size() + reason.size() + 64);
				gaps.push_back({"sdk.target-" + std::string{reason}, std::string{subject}, {}});
				downgrade(axis, conflict);
			}
			bool same_fields(const refs& values, std::initializer_list<std::string_view> names)
			{
				if (values.empty())
					return false;
				if (values.size() == 1)
				{
					b.work();
					return true;
				}
				for (auto name : names)
				{
					const detached_cell* first = nullptr;
					for (auto i : values)
					{
						b.work(name.size() + 1);
						if (!present(row(i), name))
							continue;
						const auto* actual = cell(row(i), name);
						if (!first)
						{
							first = actual;
							continue;
						}
						const auto av = actual->canonical_form(), fv = first->canonical_form();
						b.work(av.size() + fv.size() + 1);
						if (av != fv)
							return false;
					}
				}
				return true;
			}
			bool membership(const refs& values)
			{
				return same_fields(values,
								   {"slot",
									"compile_unit",
									"subject_kind",
									"subject_ordinal",
									"domain",
									"relation_kind",
									"slot_index",
									"profile"});
			}
			const annotated_row& row(std::size_t i) const
			{
				return output.evidence.at(i).row;
			}
			const refs& find(std::size_t g, const identity& id) const
			{
				static const refs empty;
				const auto found = maps[g].find(id);
				return found == maps[g].end() ? empty : found->second;
			}
			const refs& indexed(const std::map<identity, refs>& index, const identity& id) const
			{
				static const refs empty;
				const auto found = index.find(id);
				return found == index.end() ? empty : found->second;
			}
			bool equal(const refs& values)
			{
				if (values.empty())
					return false;
				if (values.size() == 1)
				{
					b.work();
					return true;
				}
				std::map<std::string, const detached_cell*> seen;
				for (auto i : values)
					for (const auto& [name, c] : row(i).values)
					{
						b.work(name.size() + 1);
						if (c.state != cell_state::present || !c.value)
							continue;
						if (const auto found = seen.find(name); found != seen.end())
						{
							const auto av = c.canonical_form(),
									   fv = found->second->canonical_form();
							b.work(av.size() + fv.size() + 1);
							if (av != fv)
								return false;
						}
						else
						{
							b.retain(name.size() +
									 sizeof(std::pair<std::string, const detached_cell*>) + 96);
							seen.emplace(name, &c);
						}
					}
				return true;
			}
			const std::vector<std::string>& strings(const annotated_row& r, std::string_view name)
			{
				static const std::vector<std::string> empty;
				if (!present(r, name))
					return empty;
				const auto cache_key = std::pair{&r, std::string{name}};
				b.work(name.size() + 1);
				if (const auto found = decoded_sets.find(cache_key); found != decoded_sets.end())
					return found->second;
				b.retain(sizeof(cache_key) + name.size() + 256);
				const auto* c = cell(r, name);
				const auto& raw = std::get<std::vector<std::byte>>(*c->value);
				b.retain(raw.size() * 8 + sizeof(std::vector<std::string>));
				b.work(raw.size());
				std::vector<std::string> result;
				for (std::size_t i = 0; i < raw.size();)
				{
					b.work();
					if (raw.size() - i < 4)
						fail(name, "truncated-set");
					std::uint32_t n{};
					for (unsigned shift = 0; shift < 32; shift += 8)
						n |= std::to_integer<std::uint32_t>(raw[i++]) << shift;
					if (n > raw.size() - i)
						fail(name, "truncated-set");
					b.charge(b.members, 1, b.limits.maximum_members, "set-members");
					b.retain(sizeof(std::string) + n);
					std::string value;
					value.reserve(n);
					for (std::size_t end = i + n; i < end; ++i)
						value += static_cast<char>(raw[i]);
					if (value.empty() || (!result.empty() && result.back() >= value))
						fail(name, "canonical-set-required");
					result.push_back(std::move(value));
				}
				return decoded_sets.emplace(cache_key, std::move(result)).first->second;
			}
			void canonical_refs(refs& values)
			{
				b.work(values.size() + 1);
				std::ranges::sort(values,
								  [&](auto a, auto c)
								  {
									  b.work();
									  return a < c;
								  });
				values.erase(std::ranges::unique(values,
												 [&](auto a, auto c)
												 {
													 b.work();
													 return a == c;
												 })
								 .begin(),
							 values.end());
			}
			const refs* bind_reference(std::size_t group,
									   std::string_view id,
									   const target_resolution_population& p,
									   observed_target_resolution_slot& s,
									   finite_population_state& axis)
			{
				if (id.empty())
					return nullptr;
				const auto& values = find(group, key(std::string{id}, p));
				b.bind(s.evidence, values);
				if (values.empty())
				{
					gap(s.gaps, axis, id, "reference-unavailable");
					return nullptr;
				}
				if (!equal(values))
				{
					gap(s.gaps, axis, id, "reference-conflicting", true);
					return nullptr;
				}
				return &values;
			}
			void source(observed_target_resolution_slot& s, const target_resolution_population& p)
			{
				s.source_state = finite_population_state::complete;
				if (s.source_span.empty())
				{
					gap(s.gaps, s.source_state, s.slot, "source-unavailable");
					return;
				}
				const auto* spans = bind_reference(2, s.source_span, p, s, s.source_state);
				if (!spans)
					return;
				const auto& span = row(spans->front());
				s.file = text(span, "file");
				s.source_snapshot = text(span, "snapshot");
				s.begin = number(span, "begin");
				s.end = number(span, "end");
				const auto* files = bind_reference(1, s.source_snapshot, p, s, s.source_state);
				if (!files)
					return;
				const auto size = number(row(files->front()), "size");
				if (!size || !s.begin || !s.end || s.file.empty() ||
					text(row(files->front()), "file").empty())
					gap(s.gaps, s.source_state, s.slot, "source-bounds-unavailable");
				else if (text(row(files->front()), "file") != s.file || *s.begin > *s.end ||
						 *s.end > *size)
					gap(s.gaps, s.source_state, s.slot, "source-bounds-conflicting", true);
			}
			bool supported(std::string_view domain, std::string_view kind)
			{
				static const std::map<std::string_view, std::set<std::string_view>> kinds{
					{"callable",
					 {"calls",
					  "constructs",
					  "invocation",
					  "construction",
					  "allocation",
					  "deallocation",
					  "initialization_failure_deallocation",
					  "destruction",
					  "cleanup_function"}},
					{"state_access",
					 {"accesses_member", "reads", "writes", "addresses", "accesses"}},
					{"ownership", {"owns"}},
					{"inheritance", {"inherits"}},
					{"override", {"overrides"}},
					{"nominal_type", {"uses_type", "template_argument"}},
					{"include", {"includes"}}};
				const auto f = kinds.find(domain);
				return f != kinds.end() && f->second.contains(kind);
			}
			void subjects(observed_target_resolution_slot& s, const target_resolution_population& p)
			{
				s.subject_state = finite_population_state::complete;
				bool observed = false;
				const auto check = [&](std::size_t group, const std::string& id)
				{
					if (id.empty())
						return;
					observed = true;
					const auto* values = bind_reference(group, id, p, s, s.subject_state);
					if (!values)
						return;
					const auto& original = row(values->front());
					if (group == 5)
					{
						bool admitted = false, uncertain = false;
						for (auto i : indexed(unit_inventories, key(p.compile_unit, p)))
						{
							b.work();
							const auto& inv = row(i);
							if (!present(inv, "declarations"))
							{
								uncertain = true;
								continue;
							}
							const auto& members = strings(inv, "declarations");
							b.work(id.size() + 1);
							if (std::ranges::binary_search(members, id))
								admitted = true;
						}
						if (!admitted)
							gap(s.gaps,
								s.subject_state,
								id,
								uncertain ? "declaration-unit-unavailable"
										  : "declaration-unit-unbound");
					}
					else if (text(original, "compile_unit") != p.compile_unit)
						gap(s.gaps, s.subject_state, id, "subject-unit-conflicting", true);
					// Only directly recorded bindings are comparable. A base/type position
					// source can be a strict subspan of its original declaration, and
					// ownership is not proximity.
					const auto compatible =
						[&](std::string_view original_field, const std::string& recorded)
					{
						if (!recorded.empty() && present(original, original_field) &&
							text(original, original_field) != recorded)
							gap(s.gaps, s.subject_state, id, "subject-binding-conflicting", true);
					};
					if (group == 7)
					{
						compatible("expression", s.expression);
						compatible("call", s.call);
						compatible("scope_declaration", s.declaration);
						compatible("function", s.owner);
					}
					if (group == 10)
					{
						compatible("expression", s.expression);
						compatible("caller", s.owner);
					}
					if (group == 9)
						compatible("source_entity", s.owner);
				};
				check(5, s.declaration);
				check(6, s.expression);
				check(7, s.operation);
				check(8, s.include);
				check(9, s.edge);
				check(10, s.call);
				if (!s.owner.empty())
					bind_reference(3, s.owner, p, s, s.subject_state);
				if (!observed)
					gap(s.gaps, s.subject_state, s.slot, "normalized-subject-unavailable");
			}
			void targets(observed_target_resolution_slot& s, const target_resolution_population& p)
			{
				s.target_state = finite_population_state::complete;
				bool observed = false;
				for (const auto& [group, id] :
					 std::array<std::pair<std::size_t, const std::string*>, 2>{
						 {{3, &s.target_entity}, {4, &s.target_type}}})
				{
					b.work();
					if (!id->empty())
					{
						observed = true;
						bind_reference(group, *id, p, s, s.target_state);
					}
				}
				if (!s.target_file.empty())
				{
					observed = true;
					const auto& files = indexed(file_ids, key(s.target_file, p));
					b.bind(s.evidence, files);
					if (files.empty())
						gap(s.gaps, s.target_state, s.slot, "target-file-unavailable");
					else
					{
						// Several actual snapshots of one target file are valid; each
						// snapshot's payload must independently agree. Never join a file ID to
						// the source-snapshot key.
						for (auto i : files)
						{
							b.work();
							const auto original = find(1, key(text(row(i), "snapshot"), p));
							if (!equal(original))
								gap(s.gaps,
									s.target_state,
									s.slot,
									"target-file-conflicting",
									true);
						}
					}
				}
				const auto compare = [&](std::size_t group,
										 const std::string& id,
										 std::string_view field,
										 const std::string& target)
				{
					if (id.empty() || target.empty())
						return;
					const auto& originals = find(group, key(id, p));
					if (!equal(originals))
						return;
					for (auto i : originals)
					{
						b.work();
						if (present(row(i), field) && text(row(i), field) != target)
							gap(s.gaps,
								s.target_state,
								s.slot,
								"original-target-conflicting",
								true);
					}
				};
				compare(7, s.operation, "target", s.target_entity);

				compare(9, s.edge, "target_entity", s.target_entity);
				compare(8, s.include, "to_file", s.target_file);
				if (!observed)
					gap(s.gaps, s.target_state, s.slot, "normalized-target-unavailable");
				if (s.resolution == "resolved" &&
					((s.domain == "include" && s.target_file.empty()) ||
					 (s.domain != "include" && s.target_entity.empty() && s.target_type.empty())))
					gap(s.gaps, s.target_state, s.slot, "resolved-target-unavailable");
			}
			observed_target_resolution_slot slot(const refs& original,
												 const target_resolution_population& p)
			{
				const auto& native = row(original.front());
				observed_target_resolution_slot s;
				b.retain(sizeof(s) + payload_bytes[original.front()] * 2 + 1024);
				s.slot = text(native, "slot");
				s.compile_unit = text(native, "compile_unit");
				s.domain = text(native, "domain");
				s.relation_kind = text(native, "relation_kind");
				s.subject_kind = text(native, "subject_kind");
				s.profile = text(native, "profile");
				s.subject_ordinal = number(native, "subject_ordinal");
				s.slot_index = number(native, "slot_index");
				s.eligibility = text(native, "eligibility");
				s.resolution = text(native, "resolution");
				s.observation_state = text(native, "observation_state");
				s.reason = text(native, "reason");
				s.source_span = text(native, "source");
				s.declaration = text(native, "declaration");
				s.expression = text(native, "expression");
				s.operation = text(native, "operation");
				s.include = text(native, "include");
				s.edge = text(native, "edge");
				s.call = text(native, "call");
				s.owner = text(native, "owner");
				s.target_entity = text(native, "target_entity");
				s.target_file = text(native, "target_file");
				s.target_type = text(native, "target_type");
				if (present(native, "is_system"))
					s.is_system = boolean(native, "is_system");
				b.bind(s.evidence, original);
				source(s, p);
				subjects(s, p);
				targets(s, p);
				if (!same_fields(original, {"source"}))
					gap(s.gaps, s.source_state, s.slot, "slot-source-conflicting", true);
				if (!same_fields(original, {"is_system"}))
				{
					s.is_system.reset();
					gap(s.gaps,
						s.source_state,
						s.slot,
						"slot-system-eligibility-conflicting",
						true);
				}
				if (!same_fields(original,
								 {"declaration",
								  "expression",
								  "operation",
								  "include",
								  "edge",
								  "call",
								  "owner"}) ||
					!membership(original) || s.compile_unit != p.compile_unit)
					gap(s.gaps, s.subject_state, s.slot, "slot-subject-conflicting", true);
				if (!same_fields(original, {"eligibility"}))
				{
					s.eligibility = "unknown";
					gap(s.gaps, s.subject_state, s.slot, "slot-eligibility-conflicting", true);
				}
				if (!same_fields(original,
								 {"target_entity",
								  "target_file",
								  "target_type",
								  "resolution",
								  "observation_state"}))
					gap(s.gaps, s.target_state, s.slot, "slot-target-conflicting", true);
				// Unknown compiler outcomes and absent is_system remain observable
				// independent axes; neither changes the independent finite enumeration.
				if (s.eligibility != "eligible" && s.eligibility != "excluded" &&
					s.eligibility != "unknown")
					gap(s.gaps, s.subject_state, s.slot, "eligibility-unsupported");
				if (s.resolution != "resolved" && s.resolution != "unresolved" &&
					s.resolution != "unknown" && s.resolution != "not_applicable")
					gap(s.gaps, s.target_state, s.slot, "resolution-unsupported");
				if (s.observation_state != "complete")
				{
					b.work();
					b.retain(s.slot.size() + s.observation_state.size() + 128);
					s.gaps.push_back(
						{"sdk.target-observation-incomplete", s.slot, s.observation_state});
				}
				canonical_refs(s.evidence);
				canonical(s.gaps, b);
				return s;
			}
			void populations()
			{
				std::set<identity> units;
				for (const auto& [id, r] : maps[0])
				{
					b.work();
					b.retain(512 + id[0].size() + id[1].size() + id[2].size() + id[3].size());
					units.insert(id);
				}
				for (const auto& index : {&unit_inventories, &unit_slots})
					for (const auto& [id, r] : *index)
					{
						(void)r;
						b.work();
						b.retain(512 + id[0].size() + id[1].size() + id[2].size() + id[3].size());
						units.insert(id);
					}
				for (const auto& unit : units)
				{
					b.work();
					if (output.populations.size() >= b.limits.maximum_populations)
						fail("populations", "limit-exceeded", "sdk.target-budget");
					b.retain(sizeof(target_resolution_population) + 1024 + unit[0].size() +
							 unit[1].size() + unit[2].size() + unit[3].size());
					target_resolution_population p;
					p.compile_unit = unit[0];
					p.universe = unit[1];
					p.variant = unit[2];
					p.interpretation = unit[3];
					const auto& originals = indexed(unit_inventories, unit);
					b.bind(p.evidence, originals);
					const auto& unit_rows = find(0, unit);
					b.bind(p.evidence, unit_rows);
					p.enumeration_state = finite_population_state::complete;
					if (!input.compile_units_complete || unit_rows.empty())
						gap(p.gaps, p.enumeration_state, p.compile_unit, "unit-unavailable");
					else if (!equal(unit_rows))
						gap(p.gaps, p.enumeration_state, p.compile_unit, "unit-conflicting", true);
					if (!input.inventory_inputs_complete || !input.slot_inputs_complete)
						gap(p.gaps, p.enumeration_state, p.compile_unit, "scan-unavailable");
					if (originals.empty())
					{
						p.enumeration_state = finite_population_state::unknown;
						gap(p.gaps, p.enumeration_state, p.compile_unit, "inventory-unavailable");
					}
					else
					{
						const auto& inv = row(originals.front());
						p.inventory = text(inv, "inventory");
						p.profile = text(inv, "target_slot_profile");
						p.slot_count = number(inv, "target_slot_count");
						p.slot_ids = strings(inv, "target_slot_ids");
						b.retain(p.inventory.size() + p.profile.size());
						if (!p.slot_count || !present(inv, "target_slot_ids") ||
							!present(inv, "target_slot_state") || p.profile != target_profile)
						{
							if (p.enumeration_state != finite_population_state::conflicting)
								p.enumeration_state = finite_population_state::unknown;
							gap(p.gaps,
								p.enumeration_state,
								p.inventory,
								"inventory-facet-unavailable");
						}
						else if (text(inv, "target_slot_state") != "complete")
							gap(p.gaps,
								p.enumeration_state,
								p.inventory,
								"inventory-facet-incomplete",
								text(inv, "target_slot_state") == "conflicting");
						for (auto i : originals)
						{
							b.work();
							const auto& r = row(i);
							if (text(r, "inventory") != p.inventory)
								gap(p.gaps,
									p.enumeration_state,
									p.inventory,
									"inventory-identity-conflicting",
									true);
							for (auto name : {"target_slot_profile",
											  "target_slot_count",
											  "target_slot_ids",
											  "target_slot_state"})
							{
								if (!present(r, name))
								{
									if (p.enumeration_state != finite_population_state::conflicting)
										p.enumeration_state = finite_population_state::unknown;
									gap(p.gaps,
										p.enumeration_state,
										p.inventory,
										"inventory-facet-unavailable");
									continue;
								}
								if (present(inv, name) &&
									cell(r, name)->canonical_form() !=
										cell(inv, name)->canonical_form())
									gap(p.gaps,
										p.enumeration_state,
										p.inventory,
										"inventory-facet-conflicting",
										true);
							}
						}
						if (p.slot_count && present(inv, "target_slot_ids") &&
							p.slot_ids.size() != *p.slot_count)
							gap(p.gaps,
								p.enumeration_state,
								p.inventory,
								"inventory-cardinality-conflicting",
								true);
					}
					std::set<std::string> ids;
					for (auto i : indexed(unit_slots, unit))
					{
						b.work();
						b.retain(text(row(i), "slot").size() + 128);
						ids.insert(text(row(i), "slot"));
					}
					for (const auto& expected : p.slot_ids)
					{
						b.work();
						b.retain(expected.size() + 128);
						ids.insert(expected);
					}
					std::map<std::tuple<std::string,
										std::uint64_t,
										std::string,
										std::string,
										std::uint64_t,
										std::string>,
							 std::string>
						admitted;
					for (const auto& id : ids)
					{
						b.work();
						const auto& observed = find(12, key(id, p));
						if (observed.empty())
						{
							gap(p.gaps, p.enumeration_state, id, "slot-unavailable");
							continue;
						}
						b.charge(b.members, 1, b.limits.maximum_members, "slots");
						auto s = slot(observed, p);
						if (!s.subject_ordinal || !s.slot_index || s.subject_kind.empty())
							gap(p.gaps, p.enumeration_state, id, "slot-admission-unavailable");
						else
						{
							b.retain(s.subject_kind.size() + s.domain.size() +
									 s.relation_kind.size() + s.profile.size() + id.size() + 512);
							b.work();
							const auto [f, added] = admitted.emplace(std::tuple{s.subject_kind,
																				*s.subject_ordinal,
																				s.domain,
																				s.relation_kind,
																				*s.slot_index,
																				s.profile},
																	 id);
							if (!added && f->second != id)
								gap(p.gaps,
									p.enumeration_state,
									id,
									"slot-admission-collision",
									true);
						}

						b.bind(p.evidence, s.evidence);
						if (!membership(observed) || s.compile_unit != p.compile_unit)
							gap(p.gaps,
								p.enumeration_state,
								id,
								"slot-membership-conflicting",
								true);
						if (s.profile != target_profile || !supported(s.domain, s.relation_kind))
							gap(p.gaps, p.enumeration_state, id, "slot-contract-unsupported");
						if (!originals.empty() &&
							present(row(originals.front()), "target_slot_ids") &&
							!std::ranges::binary_search(p.slot_ids, id))
							gap(p.gaps, p.enumeration_state, id, "slot-outside-inventory", true);
						p.slots.push_back(std::move(s));
					}
					if (p.slot_count && p.slots.size() != *p.slot_count)
						gap(p.gaps, p.enumeration_state, p.inventory, "slot-population-incomplete");
					canonical_refs(p.evidence);
					canonical(p.gaps, b);
					for (const auto& g : p.gaps)
					{
						b.retain(g.code.size() + g.subject.size() + g.detail.size() + sizeof(g));
						output.unresolved.push_back(g);
					}
					for (const auto& s : p.slots)
						for (const auto& g : s.gaps)
						{
							b.retain(g.code.size() + g.subject.size() + g.detail.size() +
									 sizeof(g));
							output.unresolved.push_back(g);
						}
					output.populations.push_back(std::move(p));
				}
				canonical(output.unresolved, b);
			}
		};
		result<target_resolution_projection>
		project_rows(target_resolution_input input,
					 finite_population_limits limits,
					 std::stop_token stop,
					 budget& b,
					 const std::array<std::vector<const annotated_row*>, 13>* borrowed = nullptr,
					 bool row_validation_reused = false)
		{
			if (auto valid = limits.validate(); !valid)
				return valid.error();
			try
			{
				(void)stop;
				b.work();
				projection work{b, input};
				work.output.compile_units_complete = input.compile_units_complete;
				work.output.inventory_inputs_complete = input.inventory_inputs_complete;
				work.output.slot_inputs_complete = input.slot_inputs_complete;
				const std::array groups{input.units,
										input.files,
										input.spans,
										input.entities,
										input.types,
										input.declarations,
										input.syntax_nodes,
										input.operations,
										input.includes,
										input.entity_edges,
										input.call_sites,
										input.inventories,
										input.slots};
				const auto descriptors = standard_relation_descriptors();
				struct entry_storage
				{
					budget& meter;
					std::size_t nodes{}, text{};
					~entry_storage()
					{
						meter.retained -= nodes + text;
					}
				} staged{b};
				std::vector<entry> entries;
				const auto grow_entries = [&]
				{
					b.work();
					if (entries.size() < entries.capacity())
						return;
					b.work(entries.size());
					const auto capacity = entries.capacity();
					const auto next = capacity > limits.maximum_rows / 2U
						? limits.maximum_rows
						: std::max(std::size_t{1U}, 2U * capacity);
					if (next <= entries.size() ||
						next > limits.maximum_retained_bytes / (2U * sizeof(entry)))
						fail("retained-bytes", "limit-exceeded", "sdk.target-budget");
					const auto reserved = 2U * next * sizeof(entry);
					b.retain(reserved);
					const auto old = staged.nodes;
					staged.nodes += reserved;
					entries.reserve(next);
					const auto actual = entries.capacity() * sizeof(entry);
					if (actual > reserved)
						fail("retained-bytes", "limit-exceeded", "sdk.target-budget");
					// reserve has destroyed the old array before its storage is refunded.
					b.retained -= old + reserved - actual;
					staged.nodes = actual;
				};
				for (std::size_t group = 0; group < groups.size(); ++group)
				{
					const auto descriptor =
						std::ranges::find(descriptors, relations[group], &relation_descriptor::id);
					if (descriptor == descriptors.end())
						fail(relations[group], "descriptor-missing");
					const auto admit = [&](const annotated_row& r)
					{
						b.work();
						b.charge(b.rows, 1, limits.maximum_rows, "rows");
						(void)b.estimate(r); // Keep the original per-row geometry ceiling.
						std::size_t validation_payload{};
						b.retain(
							b.owned_geometry(
								r, false, row_validation_reused ? nullptr : &validation_payload) +
							2U * sizeof(finite_population_evidence) + 2U * relations[group].size() +
							128U);
						const auto encoding = b.owned_geometry(r, true);
						if (encoding > limits.maximum_retained_bytes / 2U)
							fail("retained-bytes", "limit-exceeded", "sdk.target-budget");
						b.retain(2U * encoding);
						struct encoding_storage
						{
							budget& meter;
							std::size_t bytes;
							~encoding_storage()
							{
								meter.retained -= bytes;
							}
							void keep(std::size_t amount)
							{
								meter.work();
								if (amount > bytes)
									fail("retained-bytes", "limit-exceeded", "sdk.target-budget");
								bytes -= amount;
							}
						} temporary{b, 2U * encoding};
						b.work(validation_payload);
						if (auto valid = detail::validate_projected_relation_row(
								r,
								*descriptor,
								"sdk.target-input-invalid",
								[&]
								{
									b.work();
								},
								row_validation_reused);
							!valid)
							throw failure{valid.error()};
						b.charge(b.conditions,
								 r.presence.fragments.size(),
								 limits.maximum_condition_expansions,
								 "conditions");
						std::size_t payload_size{};
						// Only Slot storage consumes this byte count; no payload text is retained.
						if (group == 12U)
							for (const auto& [name, value] : r.values)
							{
								b.work(name.size() + 1U);
								const auto canonical = value.canonical_form();
								b.work(canonical.size());
								b.charge(payload_size,
										 name.size(),
										 limits.maximum_retained_bytes,
										 "retained-bytes");
								b.charge(payload_size,
										 canonical.size(),
										 limits.maximum_retained_bytes,
										 "retained-bytes");
								b.charge(payload_size,
										 2U,
										 limits.maximum_retained_bytes,
										 "retained-bytes");
							}
						auto canonical = r.canonical_form();
						b.work(canonical.size());
						b.charge(b.evidence,
								 canonical.size(),
								 limits.maximum_evidence_bytes,
								 "evidence-bytes");
						b.work();
						const auto text_bytes = canonical.capacity() + 1U;
						temporary.keep(text_bytes);
						staged.text += text_bytes;
						grow_entries();
						entries.push_back({group, &r, std::move(canonical), payload_size});
					};
					if (borrowed)
					{
						for (const auto* r : (*borrowed)[group])
							admit(*r);
					}
					else
						for (const auto& r : groups[group])
							admit(r);
				}
				std::ranges::sort(entries,
								  [&](const auto& a, const auto& c)
								  {
									  b.work();
									  if (a.group != c.group)
										  return a.group < c.group;
									  return b.compare(a.canonical, c.canonical) < 0;
								  });
				b.work();
				if (entries.size() > limits.maximum_retained_bytes / (2U * sizeof(std::size_t)))
					fail("retained-bytes", "limit-exceeded", "sdk.target-budget");
				b.retain(2U * entries.size() * sizeof(std::size_t));
				work.payload_bytes.reserve(entries.size());
				for (auto& e : entries)
				{
					b.work();
					const auto i = work.output.evidence.size();
					b.work(relations[e.group].size() + 1U);
					work.output.evidence.push_back(
						{std::string{relations[e.group]},
						 detail::copy_projected_row(*e.original,
													[&](std::size_t amount)
													{
														b.work(amount);
													})});
					work.payload_bytes.push_back(e.payload_bytes);
					const auto id = text(*e.original, identifiers[e.group]);
					if (id.empty())
						fail(relations[e.group], "identity-missing");
					for (const auto& variant : e.original->presence.fragments)
					{
						b.work();
						b.retain(id.size() + variant.size() + e.original->presence.universe.size() +
								 e.original->interpretation.size() + 1024);
						work.maps[e.group][key(id, *e.original, variant)].push_back(i);
						if (e.group == 11)
							work
								.unit_inventories[key(
									text(*e.original, "compile_unit"), *e.original, variant)]
								.push_back(i);
						if (e.group == 12)
							work
								.unit_slots[key(
									text(*e.original, "compile_unit"), *e.original, variant)]
								.push_back(i);
						if (e.group == 1)
							work.file_ids[key(text(*e.original, "file"), *e.original, variant)]
								.push_back(i);
					}
				}
				// Canonical ordering keys have no remaining reader. Destroy them and
				// their array before returning their reservation to the live budget.
				std::vector<entry>{}.swap(entries);
				b.retained -= staged.nodes + staged.text;
				staged.nodes = staged.text = 0U;
				work.populations();
				return std::move(work.output);
			}
			catch (const failure& e)
			{
				return e.value;
			}
			catch (const std::bad_alloc&)
			{
				return error{"sdk.target-resource-exhausted", "projection", "allocation"};
			}
			catch (const std::length_error&)
			{
				return error{"sdk.target-resource-exhausted", "projection", "length"};
			}
		}
		result<target_resolution_projection> project_queries(const application_query_results& input,
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
																  "sdk.target",
																  &plan_bytes,
																  cancelled);
					!valid)
					return valid.error();
				b.retain(plan_bytes);
				std::array<std::vector<const annotated_row*>, 13> groups;
				std::array<bool, 13> seen{}, complete{};
				std::size_t borrowed_rows{};
				bool row_validation_reused{true};
				complete.fill(true);
				for (const auto& scan : input.scans)
				{
					b.work();
					const auto name = std::ranges::find(relations, scan.relation_id);
					if (name == relations.end())
						continue;
					const auto group = static_cast<std::size_t>(name - relations.begin());
					row_validation_reused &= query_transfer_access::rows_validated(scan.result);
					seen[group] = true;
					complete[group] &= scan.result.execution() == execution_status::complete;
					if (group == 0)
						complete[group] &= scan.result.inputs_complete() &&
							scan.result.conflicts().empty() &&
							scan.result.differential_disagreements().empty();
					if (group == 12)
						complete[group] &= scan.result.conflicts().empty() &&
							scan.result.differential_disagreements().empty();
					const auto originals = query_transfer_access::borrow_rows(scan.result);
					if (originals.size() > limits.maximum_rows - borrowed_rows)
						fail("rows", "limit-exceeded", "sdk.target-budget");
					const auto count = groups[group].size() + originals.size();
					// Reserve borrowed pointer storage before allocation. The source query
					// owns every row until the final evidence clone and side-channel copy.
					b.retain(1024U);
					if (count > limits.maximum_retained_bytes / (2U * sizeof(const annotated_row*)))
						fail("retained-bytes", "limit-exceeded", "sdk.target-budget");
					b.retain(2U * count * sizeof(const annotated_row*));
					b.work();
					if (count > groups[group].capacity())
						b.work(groups[group].size());
					groups[group].reserve(count);
					for (const auto& r : originals)
					{
						b.work();
						++borrowed_rows;
						groups[group].push_back(&r);
					}
				}
				const auto available = [&](std::size_t group)
				{
					return seen[group] && complete[group];
				};
				target_resolution_input raw{};
				raw.compile_units_complete = available(0U);
				raw.inventory_inputs_complete = available(11U);
				raw.slot_inputs_complete = available(12U);
				auto output = project_rows(raw, limits, stop, b, &groups, row_validation_reused);
				if (!output)
					return output.error();
				for (std::size_t group = 0; group < groups.size(); ++group)
					if (!seen[group])
					{
						b.work();
						b.retain(relations[group].size() + 256);
						output->unresolved.push_back({"sdk.target-scan-missing",
													  std::string{relations[group]},
													  "independent-scan-unavailable"});
					}
				canonical(output->unresolved, b);
				output->source_queries = input;
				return output;
			}
			catch (const failure& e)
			{
				return e.value;
			}
			catch (const std::bad_alloc&)
			{
				return error{"sdk.target-resource-exhausted", "projection", "allocation"};
			}
			catch (const std::length_error&)
			{
				return error{"sdk.target-resource-exhausted", "projection", "length"};
			}
		}
	} // namespace
	result<target_resolution_projection> project_target_resolution(target_resolution_input input,
																   finite_population_limits limits,
																   std::stop_token stop)
	{
		budget b{limits, stop};
		return project_rows(input, limits, stop, b);
	}
	result<target_resolution_projection>
	project_target_resolution(const application_query_results& input,
							  finite_population_limits limits,
							  std::stop_token stop)
	{
		return project_queries(input, limits, stop);
	}
} // namespace cxxlens::sdk::query
