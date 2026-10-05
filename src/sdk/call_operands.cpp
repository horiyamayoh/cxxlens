#include <algorithm>
#include <array>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <tuple>

#include <cxxlens/sdk/call_operands.hpp>

#include "query_projection_plan_limits_internal.hpp"
#include "query_projection_rows_internal.hpp"
#include "query_result_internal.hpp"

namespace cxxlens::sdk::query
{
	namespace
	{
		using identity = std::array<std::string, 4>;
		using refs = std::vector<std::size_t>;
		constexpr std::array<std::string_view, 12> relations{"build.compile_unit.v1",
															 "source.file.v1",
															 "source.span.v1",
															 "cc.entity.v1",
															 "cc.entity_detail.v1",
															 "cc.type.v1",
															 "cc.type_component.v1",
															 "cc.body.v1",
															 "cc.syntax_node.v1",
															 "cc.call_site.v1",
															 "cc.call_direct_target.v1",
															 "cc.call_operand.v1"};
		constexpr std::array<std::string_view, 12> identifiers{"compile_unit",
															   "snapshot",
															   "span",
															   "entity",
															   "entity",
															   "type",
															   "owner_type",
															   "body",
															   "node",
															   "call",
															   "call",
															   "operand"};
		struct failure
		{
			error value;
		};
		[[noreturn]] void fail(std::string_view field,
							   std::string_view reason,
							   std::string_view code = "sdk.call-input-invalid")
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
		std::string_view borrowed_text(const annotated_row& row, std::string_view name)
		{
			const auto* c = cell(row, name);
			const auto* v = c && c->state == cell_state::present && c->value
				? std::get_if<std::string>(&*c->value)
				: nullptr;
			return v ? std::string_view{*v} : std::string_view{};
		}
		template <class Work>
		std::optional<bool> borrowed_symbol(const annotated_row& row,
											std::string_view name,
											std::string_view wanted,
											Work&& work)
		{
			const auto* c = cell(row, name);
			const auto* values = c && c->state == cell_state::present && c->value
				? std::get_if<std::vector<std::byte>>(&*c->value)
				: nullptr;
			if (!values)
				return std::nullopt;
			for (std::size_t i{}; i < values->size();)
			{
				work();
				if (values->size() - i < 4U)
					return std::nullopt;
				std::uint32_t size{};
				for (unsigned shift{}; shift < 32U; shift += 8U)
					size |= std::to_integer<std::uint32_t>((*values)[i++]) << shift;
				if (size > values->size() - i)
					return std::nullopt;
				bool equal = size == wanted.size();
				if (equal)
					for (std::size_t offset{}; offset < size; ++offset)
						if (std::to_integer<unsigned char>((*values)[i + offset]) !=
							static_cast<unsigned char>(wanted[offset]))
						{
							equal = false;
							break;
						}
				if (equal)
					return true;
				i += size;
			}
			return false;
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
			value.gaps.push_back({"sdk.call-" + std::string{reason}, std::string{subject}, {}});
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
					fail(field, "limit-exceeded", "sdk.call-budget");
				used += amount;
			}
			void work(std::size_t n = 1)
			{
				if (stop.stop_requested() || (limits.cancelled && limits.cancelled()))
					fail("projection", "stop-requested", "sdk.call-cancelled");
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
						fail("row", "limit-exceeded", "sdk.call-budget");
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
			call_operand_input input;
			call_operand_projection output;
			std::array<std::map<identity, refs>, 12> maps;
			std::map<identity, refs> call_members, caller_calls, type_members, function_bodies,
				function_syntax;
			std::vector<std::string> payloads;
			const annotated_row& row(std::size_t i) const
			{
				return output.evidence.at(i).row;
			}
			const refs& find(std::size_t group, const identity& id) const
			{
				static const refs empty;
				auto f = maps[group].find(id);
				return f == maps[group].end() ? empty : f->second;
			}
			bool equal(const refs& r)
			{
				if (r.empty())
					return false;
				for (auto i : r)
				{
					b.work();
					if (payloads[i] != payloads[r.front()])
						return false;
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
				const auto& file = row(files.front());
				const auto size = number(file, "size");
				if (text(file, "file") != value.file || !size || !value.begin || !value.end ||
					(*value.end) < (*value.begin) || (*value.end) > (*size))
					gap(value, subject, "source-bounds-conflicting", true);
			}
			template <class T>
			void unit(T& value, std::string_view subject)
			{
				const auto& r = find(0, key(value.compile_unit, value));
				b.bind(value.evidence, r);
				if (r.empty())
					gap(value, subject, "unit-unavailable");
				else if (!equal(r))
					gap(value, subject, "unit-conflicting", true);
			}
			finite_population_state reference(std::size_t group,
											  std::string_view id,
											  const call_operand_population& call,
											  refs& evidence,
											  std::vector<query_unresolved>& gaps)
			{
				if (id.empty())
					return finite_population_state::unknown;
				const auto& r = find(group, key(std::string{id}, call));
				b.bind(evidence, r);
				if (r.empty())
				{
					gaps.push_back({"sdk.call-reference-unavailable",
									std::string{id},
									std::string{relations[group]}});
					return finite_population_state::unknown;
				}
				if (!equal(r))
				{
					gaps.push_back({"sdk.call-reference-conflicting",
									std::string{id},
									std::string{relations[group]}});
					return finite_population_state::conflicting;
				}
				return finite_population_state::complete;
			}
			observed_call_operand operand(const refs& original, const call_operand_population& call)
			{
				const auto& native = row(original.front());
				observed_call_operand value;
				b.retain(payloads[original.front()].size() + sizeof(value));
				value.id = text(native, "operand");
				value.call = text(native, "call");
				value.kind = text(native, "kind");
				value.origin = text(native, "origin");
				value.index = number(native, "index").value_or(0);
				value.source_span = text(native, "source");
				value.expression = text(native, "expression");
				value.type = text(native, "type");
				value.referenced_entity = text(native, "referenced_entity");
				value.formal_parameter_index = number(native, "formal_parameter_index");
				value.actual_argument_index = number(native, "actual_argument_index");
				value.written_argument_index = number(native, "written_argument_index");
				value.implicit = boolean(native, "implicit");
				value.default_argument = boolean(native, "default_argument");
				value.value_category = text(native, "value_category");
				value.evaluation_state = text(native, "evaluation_state");
				value.state = text(native, "observation_state") == "complete"
					? finite_population_state::complete
					: text(native, "observation_state") == "partial"
					? finite_population_state::partial
					: finite_population_state::unknown;
				b.bind(value.evidence, original);
				if (!equal(original))
					gap(value, value.id, "operand-conflicting", true);
				if (text(native, "compile_unit") != call.compile_unit || value.call != call.call)
					gap(value, value.id, "operand-owner-conflicting", true);
				if (value.state != finite_population_state::complete)
					gap(value, value.id, "operand-observation-unavailable");
				value.type_state = reference(5, value.type, call, value.evidence, value.gaps);
				value.expression_state =
					reference(8, value.expression, call, value.evidence, value.gaps);
				value.reference_state =
					reference(3, value.referenced_entity, call, value.evidence, value.gaps);
				const auto* named_cell = cell(native, "referenced_entity");
				if (named_cell && named_cell->state == cell_state::absent)
					value.reference_state = finite_population_state::complete;
				if (!value.expression.empty() &&
					value.expression_state == finite_population_state::complete &&
					text(row(find(8, key(value.expression, call)).front()), "compile_unit") !=
						call.compile_unit)
					value.expression_state = finite_population_state::conflicting;
				if (!value.source_span.empty())
				{
					call_operand_population bound;
					bound.universe = call.universe;
					bound.variant = call.variant;
					bound.interpretation = call.interpretation;
					bound.source_span = value.source_span;
					bound.evidence.clear();
					bound.gaps.clear();
					bound.state = finite_population_state::complete;
					source(bound, value.id);
					value.file = bound.file;
					value.source_snapshot = bound.source_snapshot;
					value.begin = bound.begin;
					value.end = bound.end;
					value.source_state = bound.state;
					b.bind(value.evidence, bound.evidence);
					value.gaps.insert(value.gaps.end(), bound.gaps.begin(), bound.gaps.end());
				}
				for (const auto state : {value.type_state,
										 value.expression_state,
										 value.reference_state,
										 value.source_state})
					if (state == finite_population_state::conflicting)
						gap(value, value.id, "operand-reference-conflicting", true);
				if ((!value.type.empty() && value.type_state == finite_population_state::unknown) ||
					(!value.expression.empty() &&
					 value.expression_state == finite_population_state::unknown) ||
					(!value.referenced_entity.empty() &&
					 value.reference_state == finite_population_state::unknown) ||
					(!value.source_span.empty() &&
					 value.source_state != finite_population_state::complete))
					gap(value, value.id, "operand-reference-unavailable");
				if (value.default_argument && value.written_argument_index)
					gap(value, value.id, "default-argument-written-index-conflicting", true);
				canonical(value.gaps);
				return value;
			}
			void signatures(call_operand_population& call)
			{
				const auto& original = find(10, key(call.call, call));
				std::map<std::string, refs> candidates;
				for (auto i : original)
				{
					b.work();
					candidates[payloads[i]].push_back(i);
				}
				for (const auto& [payload, r] : candidates)
				{
					b.retain(payload.size() + sizeof(observed_call_target_signature));
					const auto& native = row(r.front());
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
					b.bind(value.evidence, r);
					if (candidates.size() != 1)
						gap(value, value.target, "target-signature-conflicting", true);
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
					if (value.state == finite_population_state::unknown)
						gap(value, value.target, "target-signature-unavailable");

					std::set<std::string> visited;
					std::vector<std::string> pending;
					b.retain(value.canonical_type.size() + sizeof(std::string));
					pending.push_back(value.canonical_type);
					while (!pending.empty())
					{
						b.work();
						auto type_id = std::move(pending.back());
						pending.pop_back();
						b.retain(type_id.size() + 128U);
						if (!visited.insert(type_id).second)
							continue;
						const bool root = type_id == value.canonical_type;
						const auto& types = find(5, key(type_id, call));
						b.bind(value.evidence, types);
						if (types.empty())
						{
							gap(value,
								type_id,
								root ? "target-type-unavailable"
									 : "target-type-component-unavailable");
							continue;
						}
						if (!equal(types))
						{
							gap(value,
								type_id,
								root ? "target-type-conflicting"
									 : "target-type-component-conflicting",
								true);
							continue;
						}
						const auto& type = row(types.front());
						if (root)
						{
							value.canonical_type_structure = text(type, "structure_preimage");
							if ((present(native, "target_canonical_type_digest") &&
								 present(type, "component_signature_digest") &&
								 text(type, "component_signature_digest") !=
									 value.canonical_type_digest) ||
								(present(native, "target_canonical_type_profile") &&
								 present(type, "structure_profile") &&
								 text(type, "structure_profile") != value.canonical_type_profile))
								gap(value, value.target, "target-type-signature-conflicting", true);
						}
						// These are original observed type facets, not an interpreted type grammar.
						// Every referenced node keeps its own availability and original evidence.
						if (text(type, "structure_state") != "complete" ||
							boolean(type, "dependent") ||
							text(type, "structure_profile") != "clang22-structural-type/1" ||
							!present(type, "structure_preimage") || !input.type_inputs_complete)
							gap(value, type_id, "target-type-structure-unavailable");
						const auto components = type_members.find(key(type_id, call));
						if (components != type_members.end())
							for (auto ref : components->second)
							{
								b.work();
								b.bind(value.evidence, refs{ref});
								const auto child = text(row(ref), "component_type");
								if (!child.empty() && !visited.contains(child))
								{
									b.retain(child.size() + sizeof(std::string));
									pending.push_back(child);
								}
							}
					}
					const auto& entities = find(3, key(value.target, call));
					b.bind(value.evidence, entities);
					if (!entities.empty())
					{
						if (!equal(entities))
							gap(value, value.target, "target-entity-conflicting", true);
						else if ((present(native, "target_usr") &&
								  present(row(entities.front()), "provider_local_key") &&
								  bytes(row(entities.front()), "provider_local_key") !=
									  value.usr) ||
								 (present(native, "target_structural_signature_digest") &&
								  present(row(entities.front()), "structural_signature_digest") &&
								  text(row(entities.front()), "structural_signature_digest") !=
									  value.structural_signature_digest))
							gap(value, value.target, "target-entity-signature-conflicting", true);
					}
					// Missing external entity rows do not erase the compiler's original signature
					// facet.
					canonical(value.gaps);
					call.signatures.push_back(std::move(value));
				}
			}
			void calls()
			{
				for (const auto& [id, r] : maps[9])
				{
					b.work();
					if (output.calls.size() >= b.limits.maximum_populations)
						fail("calls", "limit-exceeded", "sdk.call-budget");
					const auto& native = row(r.front());
					call_operand_population call;
					b.retain(payloads[r.front()].size() + sizeof(call));
					call.call = id[0];
					call.universe = id[1];
					call.variant = id[2];
					call.interpretation = id[3];
					call.compile_unit = text(native, "compile_unit");
					call.caller = text(native, "caller");
					call.source_span = text(native, "source");
					call.kind = text(native, "kind");
					call.expression = text(native, "expression");
					call.profile = text(native, "operand_profile");
					call.argument_count = number(native, "argument_count");
					call.operand_count = number(native, "operand_count");
					call.state = text(native, "operand_population_state") == "complete" &&
							call.profile == "clang22-actual-call-operands/1" &&
							call.operand_count && input.call_inputs_complete &&
							input.operand_inputs_complete && input.compile_units_complete
						? finite_population_state::complete
						: finite_population_state::unknown;
					b.bind(call.evidence, r);
					if (!equal(r))
						gap(call, call.call, "site-conflicting", true);
					if (call.state == finite_population_state::unknown)
						gap(call, call.call, "operand-population-unavailable");
					unit(call, call.call);
					source(call, call.call);
					if (!call.expression.empty())
					{
						const auto state =
							reference(8, call.expression, call, call.evidence, call.gaps);
						if (state != finite_population_state::complete)
							gap(call,
								call.call,
								"call-expression-unavailable",
								state == finite_population_state::conflicting);
						else if (text(row(find(8, key(call.expression, call)).front()),
									  "compile_unit") != call.compile_unit)
							gap(call, call.call, "call-expression-owner-conflicting", true);
					}
					const auto bodies =
						function_bodies.find(key(call.compile_unit + '\n' + call.caller, call));
					if (bodies != function_bodies.end())
					{
						std::set<std::string> ids;
						refs matching;
						for (auto i : bodies->second)
						{
							call_operand_population body;
							body.universe = call.universe;
							body.variant = call.variant;
							body.interpretation = call.interpretation;
							body.source_span = text(row(i), "source");
							body.state = finite_population_state::complete;
							source(body, body.source_span);
							if (contains_source(body, call.source_span, call).value_or(false))
							{
								b.retain(text(row(i), "body").size() + sizeof(std::size_t) + 128U);
								ids.insert(text(row(i), "body"));
								matching.push_back(i);
							}
						}
						if (ids.size() == 1)
						{
							call.body = *ids.begin();
							b.bind(call.evidence, matching);
						}
					}
					const auto owned = call_members.find(id);
					std::map<std::pair<std::string, std::uint64_t>, std::string> slots;
					std::set<std::uint64_t> arguments;
					if (owned != call_members.end())
					{
						std::map<std::string, refs> members;
						for (auto i : owned->second)
						{
							b.work();
							members[text(row(i), "operand")].push_back(i);
						}
						for (const auto& [operand_id, original] : members)
						{
							b.work();
							b.charge(b.members, 1, b.limits.maximum_members, "operands");
							auto value = operand(original, call);
							if (!slots.emplace(std::pair{value.kind, value.index}, operand_id)
									 .second)
								gap(call, call.call, "operand-slot-conflicting", true);
							if (value.kind == "argument" && value.actual_argument_index)
							{
								if (!arguments.insert(*value.actual_argument_index).second)
									gap(call, call.call, "argument-index-conflicting", true);
								// Compiler AST argument indices may start after a static operator's
								// syntactic object.
							}
							if (value.state == finite_population_state::conflicting)
								gap(call, call.call, "operand-binding-conflicting", true);
							call.operands.push_back(std::move(value));
						}
					}
					if (call.operand_count && *call.operand_count != call.operands.size())
						gap(call,
							call.call,
							"operand-count-incomplete",
							call.operands.size() > *call.operand_count);
					if (call.argument_count && *call.argument_count != arguments.size())
						gap(call,
							call.call,
							"argument-population-incomplete",
							arguments.size() > *call.argument_count);
					signatures(call);
					canonical(call.gaps);
					output.calls.push_back(std::move(call));
				}
				for (const auto& [id, r] : call_members)
					if (find(9, id).empty())
					{
						b.work();
						output.unresolved.push_back({"sdk.call-operand-owner-missing", id[0], {}});
						(void)r;
					}
			}
			template <class T>
			std::optional<bool> contains_source(const T& owner,
												std::string_view span,
												const call_operand_population& world)
			{
				call_operand_population candidate;
				candidate.universe = world.universe;
				candidate.variant = world.variant;
				candidate.interpretation = world.interpretation;
				candidate.source_span = span;
				candidate.state = finite_population_state::complete;
				source(candidate, span);
				if (candidate.state != finite_population_state::complete || !owner.begin ||
					!owner.end)
					return std::nullopt;
				return owner.file == candidate.file &&
					owner.source_snapshot == candidate.source_snapshot &&
					*owner.begin <= *candidate.begin && *candidate.end <= *owner.end;
			}
			void scopes()
			{
				using scope_key = std::array<std::string, 6>;
				std::map<scope_key, refs> scopes;
				for (const auto& [id, r] : maps[4])
				{
					const auto& entities = find(3, id);
					if (!equal(entities))
						continue;
					const auto kind = text(row(entities.front()), "kind");
					if (kind != "function" && kind != "method" && kind != "constructor" &&
						kind != "destructor" && kind != "conversion")
						continue;
					for (auto i : r)
					{
						b.work();
						const auto& detail = row(i);
						const scope_key owner{id[0],
											  text(detail, "compile_unit"),
											  text(detail, "source"),
											  id[1],
											  id[2],
											  id[3]};
						b.retain(sizeof(owner) + payloads[i].size() + 128);
						scopes[owner].push_back(i);
					}
				}
				for (const auto& [owner, r] : scopes)
				{
					b.work();
					if (output.function_scopes.size() >= b.limits.maximum_populations)
						fail("scopes", "limit-exceeded", "sdk.call-budget");
					const auto& native = row(r.front());
					function_call_scope value;
					b.retain(payloads[r.front()].size() + sizeof(value));
					value.function = owner[0];
					value.compile_unit = owner[1];
					value.source_span = owner[2];
					value.universe = owner[3];
					value.variant = owner[4];
					value.interpretation = owner[5];
					value.profile = text(native, "call_site_profile");
					value.call_count = number(native, "call_site_count");
					value.call_ids = strings(native, "call_site_ids");
					value.state = text(native, "call_site_state") == "complete" &&
							value.call_count && present(native, "call_site_ids") &&
							value.profile == "clang22-function-emitted-call-sites/1" &&
							input.compile_units_complete && input.scope_inputs_complete &&
							input.call_inputs_complete
						? finite_population_state::complete
						: finite_population_state::unknown;
					b.bind(value.evidence, r);
					if (!equal(r))
						gap(value, value.function, "function-scope-conflicting", true);
					if (value.state == finite_population_state::unknown)
						gap(value, value.function, "function-scope-unavailable");
					unit(value, value.function);
					source(value, value.function);
					if (value.call_count && *value.call_count != value.call_ids.size())
						gap(value, value.function, "function-scope-count-conflicting", true);
					std::set<std::string> actual;
					const auto found =
						caller_calls.find(key(value.compile_unit + '\n' + value.function, value));
					if (found != caller_calls.end())
						for (auto i : found->second)
						{
							b.work();
							call_operand_population context;
							context.universe = value.universe;
							context.variant = value.variant;
							context.interpretation = value.interpretation;
							const auto inside =
								contains_source(value, text(row(i), "source"), context);
							if (!inside)
								gap(value, value.function, "function-extra-source-unavailable");
							else if (*inside)
							{
								b.retain(text(row(i), "call").size() + 128U);
								actual.insert(text(row(i), "call"));
								b.bind(value.evidence, refs{i});
							}
						}

					std::set<std::string> admitted;
					call_operand_population syntax_world;
					syntax_world.universe = value.universe;
					syntax_world.variant = value.variant;
					syntax_world.interpretation = value.interpretation;
					const auto syntax = function_syntax.find(
						key(value.compile_unit + '\n' + value.function, value));
					if (syntax != function_syntax.end())
						for (auto i : syntax->second)
						{
							b.work();
							const auto inside =
								contains_source(value, text(row(i), "source"), syntax_world);
							if (!inside)
							{
								gap(value, value.function, "function-syntax-source-unavailable");
								continue;
							}
							if (!*inside)
								continue;
							const auto id = text(row(i), "node");
							const auto& originals = find(8, key(id, value));
							b.bind(value.evidence, originals);
							if (!equal(originals))
								gap(value, id, "function-syntax-conflicting", true);
							const auto flags = strings(row(i), "flags");
							if (!std::ranges::binary_search(flags, "finite_call_admission_v1"))
								gap(value, id, "function-call-admission-unavailable");
							else if (std::ranges::binary_search(flags, "admitted_call_site"))
							{
								b.retain(id.size() + 128U);
								admitted.insert(id);
							}
						}
					std::map<std::string, std::string> expressions;
					for (const auto& id : value.call_ids)
					{
						b.work();
						const auto& sites = find(9, key(id, value));
						b.bind(value.evidence, sites);
						if (sites.empty())
							gap(value, value.function, "function-call-missing");
						else if (!equal(sites))
							gap(value, value.function, "function-call-conflicting", true);
						else if (text(row(sites.front()), "compile_unit") != value.compile_unit ||
								 text(row(sites.front()), "caller") != value.function)
							gap(value, value.function, "function-call-owner-conflicting", true);
						if (!sites.empty() && equal(sites))
						{
							const auto expression = text(row(sites.front()), "expression");
							const auto& nodes = find(8, key(expression, value));
							b.bind(value.evidence, nodes);
							if (expression.empty() || nodes.empty())
								gap(value, id, "function-call-expression-unavailable");
							else if (!equal(nodes))
								gap(value, id, "function-call-expression-conflicting", true);
							else
							{
								const auto& node = row(nodes.front());
								const auto flags = strings(node, "flags");
								if (text(node, "compile_unit") != value.compile_unit ||
									text(node, "source") != text(row(sites.front()), "source"))
									gap(value,
										id,
										"function-call-expression-owner-conflicting",
										true);
								if (!std::ranges::binary_search(flags, "finite_call_admission_v1"))
									gap(value, id, "function-call-admission-unavailable");
								else if (!std::ranges::binary_search(flags, "admitted_call_site"))
									gap(value, id, "function-call-expression-not-admitted", true);
								b.retain(expression.size() + id.size() + 256U);
								if (!expressions.emplace(expression, id).second)
									gap(value,
										expression,
										"function-call-expression-duplicate",
										true);
							}
						}
					}
					for (const auto& id : admitted)
					{
						b.work();
						if (!expressions.contains(id))
							gap(value, id, "function-admitted-call-missing");
					}
					for (const auto& id : actual)
						if (!std::ranges::binary_search(value.call_ids, id))
							gap(value, value.function, "function-call-outside-inventory", true);
					const auto bodies = function_bodies.find(
						key(value.compile_unit + '\n' + value.function, value));
					const auto flags = strings(native, "flags");
					if (bodies != function_bodies.end() &&
						std::ranges::binary_search(flags, "finite_function_body_v1") &&
						std::ranges::binary_search(flags, "body_written"))
					{
						std::set<std::string> ids;
						refs matching;
						call_operand_population context;
						context.universe = value.universe;
						context.variant = value.variant;
						context.interpretation = value.interpretation;
						for (auto i : bodies->second)
							if (contains_source(value, text(row(i), "source"), context)
									.value_or(false))
							{
								b.retain(text(row(i), "body").size() + sizeof(std::size_t) + 128U);
								ids.insert(text(row(i), "body"));
								matching.push_back(i);
							}
						if (ids.size() > 1)
							gap(value, value.function, "definition-body-scope-ambiguous", true);
						if (ids.size() == 1)
						{
							value.body = *ids.begin();
							b.bind(value.evidence, matching);
							for (auto i : matching)
							{
								b.work();
								const auto& body = row(i);
								if ((present(body, "call_site_count") &&
									 number(body, "call_site_count") != value.call_count) ||
									(present(body, "call_site_ids") &&
									 strings(body, "call_site_ids") != value.call_ids) ||
									(present(body, "call_site_state") &&
									 text(body, "call_site_state") !=
										 text(native, "call_site_state")) ||
									(present(body, "call_site_profile") &&
									 text(body, "call_site_profile") != value.profile))
									gap(value,
										value.function,
										"definition-body-scope-conflicting",
										true);
							}
						}
					}
					canonical(value.gaps);
					output.function_scopes.push_back(std::move(value));
				}
			}
		};
		void normalize(call_operand_projection& output)
		{
			const auto refs_unique = [](auto& v)
			{
				std::ranges::sort(v);
				v.erase(std::ranges::unique(v).begin(), v.end());
			};
			for (auto& call : output.calls)
			{
				refs_unique(call.evidence);
				for (auto& operand : call.operands)
					refs_unique(operand.evidence);
				for (auto& signature : call.signatures)
					refs_unique(signature.evidence);
			}
			for (auto& scope : output.function_scopes)
				refs_unique(scope.evidence);
			canonical(output.unresolved);
		}
		result<call_operand_projection>
		project_rows(call_operand_input input,
					 finite_population_limits limits,
					 std::stop_token stop,
					 budget& b,
					 bool scope_only,
					 const std::array<std::vector<const annotated_row*>, 12>* borrowed = nullptr)
		{
			if (auto valid = limits.validate(); !valid)
				return valid.error();
			try
			{
				(void)stop;
				b.work();
				projection work{b, input, {}, {}, {}, {}, {}, {}, {}, {}};
				work.output.compile_units_complete = input.compile_units_complete;
				work.output.call_inputs_complete = input.call_inputs_complete;
				work.output.operand_inputs_complete = input.operand_inputs_complete;
				work.output.scope_inputs_complete = input.scope_inputs_complete;
				work.output.type_inputs_complete = input.type_inputs_complete;
				const std::array groups{input.units,
										input.files,
										input.spans,
										input.entities,
										input.details,
										input.types,
										input.type_components,
										input.bodies,
										input.syntax_nodes,
										input.call_sites,
										input.direct_targets,
										input.operands};
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
				// Retain the actual selected semantic carriers once. Independent
				// admission markers exclude known non-call syntax; absent/missing
				// markers and all same-ID alternatives remain retained frontiers.
				// Every original row is still validated and charged below.
				using view_key = std::array<std::string_view, 4>;
				std::array<std::set<view_key>, 12> selected;
				const auto add =
					[&](std::size_t group, std::string_view id, const annotated_row& row)
				{
					if (id.empty())
						return;
					for (const auto& variant : row.presence.fragments)
					{
						b.work();
						const view_key k{id, row.presence.universe, variant, row.interpretation};
						if (!selected[group].contains(k))
						{
							b.retain(sizeof(view_key) + 96U);
							selected[group].insert(k);
						}
					}
				};
				const auto contains =
					[&](std::size_t group, std::string_view id, const annotated_row& row)
				{
					return std::ranges::any_of(
						row.presence.fragments,
						[&](const auto& variant)
						{
							b.work();
							return selected[group].contains(
								view_key{id, row.presence.universe, variant, row.interpretation});
						});
				};
				visit_group(3U,
							[&](const annotated_row& row)
							{
								b.work();
								const auto kind = borrowed_text(row, "kind");
								if (kind.empty() || kind == "function" || kind == "method" ||
									kind == "constructor" || kind == "destructor" ||
									kind == "conversion")
									add(4U, borrowed_text(row, "entity"), row);
							});
				visit_group(4U,
							[&](const annotated_row& row)
							{
								b.work();
								if (present(row, "call_site_count") ||
									present(row, "call_site_ids") ||
									present(row, "call_site_profile") ||
									contains(4U, borrowed_text(row, "entity"), row))
								{
									add(4U, borrowed_text(row, "entity"), row);
									add(3U, borrowed_text(row, "entity"), row);
								}
							});
				visit_group(8U,
							[&](const annotated_row& row)
							{
								b.work();
								const auto finite = borrowed_symbol(row,
																	"flags",
																	"finite_call_admission_v1",
																	[&]
																	{
																		b.work();
																	}),
										   admitted = borrowed_symbol(row,
																	  "flags",
																	  "admitted_call_site",
																	  [&]
																	  {
																		  b.work();
																	  });
								if (!finite || !*finite || !admitted || *admitted)
									add(8U, borrowed_text(row, "node"), row);
							});
				for (const auto group : {9U, 10U, 11U})
				{
					if (scope_only && (group == 10U || group == 11U))
						continue;
					visit_group(
						group,
						[&](const annotated_row& row)
						{
							b.work();
							add(3U,
								borrowed_text(row,
											  group == 9U		 ? "caller"
												  : group == 10U ? "target"
																 : "referenced_entity"),
								row);
							if (group == 9U)
								add(4U, borrowed_text(row, "caller"), row);
							add(8U, borrowed_text(row, "expression"), row);
							if (!scope_only)
								add(5U,
									borrowed_text(row,
												  group == 10U ? "target_canonical_type" : "type"),
									row);
						});
				}
				visit_group(7U,
							[&](const annotated_row& row)
							{
								b.work();
								if (contains(4U, borrowed_text(row, "function"), row))
									add(7U, borrowed_text(row, "body"), row);
							});
				// All component alternatives of a selected type remain visible;
				// child closure is reached through original explicit component IDs.
				if (!scope_only)
				{
					std::map<view_key, std::vector<const annotated_row*>> components;
					visit_group(6U,
								[&](const annotated_row& row)
								{
									b.work();
									const auto owner = borrowed_text(row, "owner_type");
									for (const auto& variant : row.presence.fragments)
									{
										b.work();
										b.retain(sizeof(view_key) + sizeof(const annotated_row*) +
												 96U);
										components[view_key{owner,
															row.presence.universe,
															variant,
															row.interpretation}]
											.push_back(&row);
									}
								});
					std::vector<view_key> pending;
					std::set<view_key> visited;
					for (const auto& id : selected[5])
					{
						b.retain(sizeof(view_key));
						pending.push_back(id);
					}
					while (!pending.empty())
					{
						b.work();
						const auto id = pending.back();
						pending.pop_back();
						if (visited.contains(id))
							continue;
						b.retain(sizeof(view_key) + 96U);
						visited.insert(id);
						const auto at = components.find(id);
						if (at == components.end())
							continue;
						for (const auto* row : at->second)
						{
							b.work();
							add(6U, borrowed_text(*row, "owner_type"), *row);
							const auto child = borrowed_text(*row, "component_type");
							if (child.empty())
								continue;
							for (const auto& variant : row->presence.fragments)
							{
								b.work();
								const view_key child_id{
									child, row->presence.universe, variant, row->interpretation};
								if (!selected[5].contains(child_id))
								{
									b.retain(sizeof(view_key) * 2U + 96U);
									selected[5].insert(child_id);
									pending.push_back(child_id);
								}
							}
						}
					}
				}
				const auto relevant = [&](std::size_t group, const annotated_row& row)
				{
					if (group == 0U || group == 9U || group == 10U || group == 11U)
						return true;
					return contains(group, borrowed_text(row, identifiers[group]), row);
				};
				for (const auto group : {4U, 7U, 8U, 9U, 11U})
				{
					if (scope_only && group == 11U)
						continue;
					visit_group(group,
								[&](const annotated_row& row)
								{
									b.work();
									if (relevant(group, row))
										add(2U, borrowed_text(row, "source"), row);
								});
				}
				visit_group(2U,
							[&](const annotated_row& row)
							{
								b.work();
								if (relevant(2U, row))
									add(1U, borrowed_text(row, "snapshot"), row);
							});
				const auto descriptors = standard_relation_descriptors();
				std::vector<entry> entries;
				for (std::size_t group = 0; group < groups.size(); ++group)
				{
					if (scope_only && (group == 5 || group == 6 || group == 10 || group == 11))
						continue;
					auto descriptor =
						std::ranges::find(descriptors, relations[group], &relation_descriptor::id);
					if (descriptor == descriptors.end())
						fail(relations[group], "descriptor-missing");
					visit_group(group,
								[&](const annotated_row& row)
								{
									b.work();
									b.charge(b.rows, 1, limits.maximum_rows, "rows");
									const auto retained = b.estimate(row);
									if (auto valid = detail::validate_projected_relation_row(
											row,
											*descriptor,
											"sdk.call-input-invalid",
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
									if (!relevant(group, row))
										return;
									b.retain(retained);
									std::string payload;
									for (const auto& [name, value] : row.values)
										payload += name + '=' + value.canonical_form() + '\n';
									const auto canonical = row.canonical_form();
									b.charge(b.evidence,
											 canonical.size(),
											 limits.maximum_evidence_bytes,
											 "evidence-bytes");
									entries.push_back({group, &row, canonical, std::move(payload)});
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
					work.payloads.push_back(std::move(e.payload));
					const auto id = text(*e.row, identifiers[e.group]);
					if (id.empty())
						fail(relations[e.group], "identity-missing");
					for (const auto& variant : e.row->presence.fragments)
					{
						b.work();
						b.retain(id.size() + variant.size() + e.row->presence.universe.size() +
								 e.row->interpretation.size() + 512);
						const auto owner = key(id, *e.row, variant);
						work.maps[e.group][owner].push_back(i);
						if (e.group == 11)
							work.call_members[key(text(*e.row, "call"), *e.row, variant)].push_back(
								i);
						if (e.group == 8)
							work.function_syntax[key(text(*e.row, "compile_unit") + '\n' +
														 text(*e.row, "function"),
													 *e.row,
													 variant)]
								.push_back(i);
						if (e.group == 9)
							work.caller_calls[key(text(*e.row, "compile_unit") + '\n' +
													  text(*e.row, "caller"),
												  *e.row,
												  variant)]
								.push_back(i);
						if (e.group == 6)
							work.type_members[key(text(*e.row, "owner_type"), *e.row, variant)]
								.push_back(i);
						if (e.group == 7)
							work.function_bodies[key(text(*e.row, "compile_unit") + '\n' +
														 text(*e.row, "function"),
													 *e.row,
													 variant)]
								.push_back(i);
					}
				}
				if (!scope_only)
					work.calls();
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
				return error{"sdk.call-resource-exhausted", "projection", "allocation"};
			}
			catch (const std::length_error&)
			{
				return error{"sdk.call-resource-exhausted", "projection", "length"};
			}
		}
		result<call_operand_projection> project_queries(const application_query_results& input,
														finite_population_limits limits,
														std::stop_token stop,
														bool scope_only)
		{
			if (auto valid = limits.validate(); !valid)
				return valid.error();
			try
			{
				budget b{limits, stop};
				b.work();
				if (auto valid = detail::check_source_plan_limits(input,
																  limits.maximum_source_queries,
																  limits.maximum_source_plan_bytes,
																  stop,
																  "sdk.call");
					!valid)
					return valid.error();
				std::array<std::vector<const annotated_row*>, 12> groups;
				std::array<bool, 12> present{}, complete{};
				complete.fill(true);
				for (const auto& scan : input.scans)
				{
					b.work();
					auto name = std::ranges::find(relations, scan.relation_id);
					if (name == relations.end())
						continue;
					const auto group = static_cast<std::size_t>(name - relations.begin());
					if (scope_only && (group == 5 || group == 6 || group == 10 || group == 11))
						continue;
					present[group] = true;
					complete[group] &= scan.result.execution() == execution_status::complete &&
						scan.result.inputs_complete() && scan.result.conflicts().empty() &&
						scan.result.differential_disagreements().empty();
					const auto rows = query_transfer_access::borrow_rows(scan.result);
					b.charge(b.rows, rows.size(), limits.maximum_rows, "scan-rows");
					if (rows.size() > limits.maximum_rows ||
						groups[group].size() > limits.maximum_rows - rows.size())
						fail("rows", "limit-exceeded", "sdk.call-budget");
					b.retain(rows.size() * sizeof(const annotated_row*));
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
				call_operand_input raw{};
				raw.compile_units_complete = available(0);
				raw.call_inputs_complete = available(9) && available(8);
				raw.operand_inputs_complete = available(11);
				raw.scope_inputs_complete = available(4);
				raw.type_inputs_complete = available(5) && available(6);
				auto output = project_rows(raw, limits, stop, b, scope_only, &groups);
				if (!output)
					return output.error();
				for (std::size_t i = 0; i < groups.size(); ++i)
					if (!present[i] &&
						(i == 0 || i == 1 || i == 2 || i == 3 || i == 4 || i == 9 ||
						 (!scope_only && (i == 5 || i == 6 || i == 8 || i == 10 || i == 11))))
						output->unresolved.push_back({"sdk.call-scan-missing",
													  std::string{relations[i]},
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
				return error{"sdk.call-resource-exhausted", "projection", "allocation"};
			}
			catch (const std::length_error&)
			{
				return error{"sdk.call-resource-exhausted", "projection", "length"};
			}
		}
	} // namespace
	result<call_operand_projection> project_call_operands(call_operand_input input,
														  finite_population_limits limits,
														  std::stop_token stop)
	{
		budget b{limits, stop};
		return project_rows(input, limits, stop, b, false);
	}
	result<call_operand_projection> project_call_operands(const application_query_results& input,
														  finite_population_limits limits,
														  std::stop_token stop)
	{
		return project_queries(input, limits, stop, false);
	}
	result<call_operand_projection> project_function_call_scopes(call_operand_input input,
																 finite_population_limits limits,
																 std::stop_token stop)
	{
		budget b{limits, stop};
		return project_rows(input, limits, stop, b, true);
	}
	result<call_operand_projection>
	project_function_call_scopes(const application_query_results& input,
								 finite_population_limits limits,
								 std::stop_token stop)
	{
		return project_queries(input, limits, stop, true);
	}
} // namespace cxxlens::sdk::query
