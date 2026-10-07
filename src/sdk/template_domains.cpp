#include <algorithm>
#include <array>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <tuple>

#include <cxxlens/sdk/template_domains.hpp>

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
															"cc.template_subject.v1",
															"cc.constraint_node.v1",
															"cc.lambda_capture.v1",
															"cc.template_instantiation_frame.v1",
															"cc.template_inventory.v1"};
		constexpr std::array<std::string_view, 9> identifiers{"compile_unit",
															  "snapshot",
															  "span",
															  "entity",
															  "subject",
															  "node",
															  "capture",
															  "frame",
															  "inventory"};
		struct failure
		{
			error value;
		};
		[[noreturn]] void fail(std::string_view field,
							   std::string_view reason,
							   std::string_view code = "sdk.template-input-invalid")
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
					fail(field, "limit-exceeded", "sdk.template-budget");
				used += amount;
			}
			void work(std::size_t n = 1)
			{
				if (stop.stop_requested() || (limits.cancelled && limits.cancelled()))
					fail("projection", "stop-requested", "sdk.template-cancelled");
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
						fail("row", "limit-exceeded", "sdk.template-budget");
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
			template_domain_input input;
			template_domain_projection output;
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
				gaps.push_back({"sdk.template-" + std::string(reason), std::string(id), {}});
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
											  const template_domain_population& scope,
											  refs& evidence,
											  std::vector<query_unresolved>& gaps)
			{
				if (id.empty())
					return finite_population_state::unknown;
				const auto& values = find(group, key(std::string(id), scope));
				b.bind(evidence, values);
				if (values.empty())
				{
					gaps.push_back({"sdk.template-reference-missing", std::string(id), {}});
					return finite_population_state::unknown;
				}
				if (!equal(values))
				{
					gaps.push_back({"sdk.template-reference-conflicting", std::string(id), {}});
					return finite_population_state::conflicting;
				}
				if (group >= 4 && text(row(values.front()), "compile_unit") != scope.compile_unit)
				{
					gaps.push_back(
						{"sdk.template-reference-unit-conflicting", std::string(id), {}});
					return finite_population_state::conflicting;
				}
				return finite_population_state::complete;
			}
			template <class T>
			void source(T& value, const template_domain_population& scope)
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
										  const template_domain_population& scope)
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
				if (text(observed, "observation_state") != "complete")
					gap(value, id, "observation-unavailable");
				return &observed;
			}
			void arguments(observed_template_subject& value, const annotated_row& original)
			{
				value.argument_binding_state = finite_population_state::complete;
				if (value.argument_profile != "clang22-canonical-template-argument-tuple/1" ||
					value.argument_state != "complete" ||
					!present(original, "canonical_arguments") || value.canonical_arguments.empty())
					axis(value.argument_binding_state,
						 value.gaps,
						 value.subject,
						 "argument-unavailable");
			}
			observed_template_subject subject(std::string id,
											  const template_domain_population& scope)
			{
				observed_template_subject value;
				value.subject = std::move(id);
				value.compile_unit = scope.compile_unit;
				const auto* original_row = original(value, 4, value.subject, scope);
				if (!original_row)
					return value;
				const auto& r = *original_row;
				value.kind = text(r, "kind");
				value.profile = text(r, "profile");
				value.source_span = text(r, "source");
				value.definition_source = text(r, "definition_source");
				value.entity = text(r, "entity");
				value.owner_entity = text(r, "owner_entity");
				value.primary = text(r, "primary");
				value.owner = text(r, "owner");
				const auto raw_usr = bytes(r, "semantic_usr");
				for (auto byte : raw_usr)
					value.semantic_usr.push_back(static_cast<char>(byte));
				value.materialization_kind = text(r, "materialization_kind");
				value.target_state = text(r, "target_state");
				value.argument_profile = text(r, "argument_profile");
				value.argument_state = text(r, "argument_state");
				value.normalization_state = text(r, "normalization_state");
				value.capture_state = text(r, "capture_state");
				value.reason = text(r, "reason");
				value.ordinal = number(r, "ordinal");
				value.capture_count = number(r, "capture_count");
				value.closure_size_bits = number(r, "closure_size_bits");
				value.canonical_arguments = bytes(r, "canonical_arguments");
				value.constraint_root = text(r, "constraint_root");
				if (present(r, "is_system"))
					value.is_system = boolean(r, "is_system");
				if (present(r, "dependent"))
					value.dependent = boolean(r, "dependent");
				if (const auto ids = strings(r, "capture_ids"))
					value.capture_ids = *ids;
				constexpr std::array kinds{"primary",
										   "implicit_instance",
										   "argument_use",
										   "dependent_type_use",
										   "dependent_unresolved_call",
										   "requires_clause",
										   "requires_expression",
										   "constraint_root",
										   "lambda"};
				if ((value.profile != "clang22-original-template-domains/1" &&
					 value.profile != "clang22-original-template-domains/2") ||
					std::ranges::find(kinds, value.kind) == kinds.end())
					gap(value, value.subject, "profile-or-kind-unavailable");
				if (value.profile == "clang22-original-template-domains/2" &&
					value.kind == "dependent_type_use" && text(r, "occurrence_kind").empty())
					gap(value, value.subject, "occurrence-kind-unavailable");
				source(value, scope);
				value.entity_state = reference(3, value.entity, scope, value.evidence, value.gaps);
				if (value.entity_state == finite_population_state::complete &&
					!value.semantic_usr.empty())
				{
					const auto expected =
						bytes(row(find(3, key(value.entity, scope)).front()), "provider_local_key");
					if (!present(row(find(3, key(value.entity, scope)).front()),
								 "provider_local_key"))
						axis(value.entity_state,
							 value.gaps,
							 value.subject,
							 "entity-usr-unavailable");
					else
					{
						constexpr std::string_view prefix = "clang-usr:";
						b.retain(prefix.size() + raw_usr.size());
						std::vector<std::byte> framed;
						framed.reserve(prefix.size() + raw_usr.size());
						for (const char c : prefix)
							framed.push_back(static_cast<std::byte>(c));
						framed.insert(framed.end(), raw_usr.begin(), raw_usr.end());
						if (expected != framed)
							axis(value.entity_state,
								 value.gaps,
								 value.subject,
								 "entity-usr-conflicting",
								 true);
					}
				}
				value.owner_state = reference(4, value.owner, scope, value.evidence, value.gaps);
				if (!value.primary.empty())
					reference(4, value.primary, scope, value.evidence, value.gaps);
				if (!value.owner_entity.empty())
					value.owner_entity_state =
						reference(3, value.owner_entity, scope, value.evidence, value.gaps);
				if (value.kind == "implicit_instance" || value.kind == "argument_use")
					arguments(value, r);
				if (value.kind == "constraint_root")
				{
					value.normalization_binding_state = finite_population_state::complete;
					if (value.normalization_state != "complete" || value.constraint_root.empty())
						axis(value.normalization_binding_state,
							 value.gaps,
							 value.subject,
							 "normalization-unavailable");
					else
						value.normalization_binding_state =
							reference(5, value.constraint_root, scope, value.evidence, value.gaps);
				}
				if (value.kind == "lambda")
				{
					value.capture_binding_state = finite_population_state::complete;
					if (value.capture_state != "complete" || !value.capture_count ||
						!present(r, "capture_ids") ||
						*value.capture_count != value.capture_ids.size())
						axis(value.capture_binding_state,
							 value.gaps,
							 value.subject,
							 "capture-population-unavailable");
					for (const auto& member : value.capture_ids)
					{
						const auto state = reference(6, member, scope, value.evidence, value.gaps);
						if (state != finite_population_state::complete)
							axis(value.capture_binding_state,
								 value.gaps,
								 member,
								 "capture-member-unavailable",
								 state == finite_population_state::conflicting);
						else if (text(row(find(6, key(member, scope)).front()), "lambda") !=
								 value.subject)
							axis(value.capture_binding_state,
								 value.gaps,
								 member,
								 "capture-owner-conflicting",
								 true);
					}
				}
				canonical(value.gaps);
				return value;
			}
			observed_constraint_node constraint(std::string id,
												const template_domain_population& scope)
			{
				observed_constraint_node value;
				value.node = std::move(id);
				value.compile_unit = scope.compile_unit;
				const auto* r = original(value, 5, value.node, scope);
				if (!r)
					return value;
				value.root_subject = text(*r, "root_subject");
				value.path = text(*r, "path");
				value.kind = text(*r, "kind");
				value.profile = text(*r, "profile");
				value.source_span = text(*r, "source");
				value.left_child = text(*r, "left_child");
				value.right_child = text(*r, "right_child");
				value.mapping_state = text(*r, "mapping_state");
				value.parameter_mapping = bytes(*r, "parameter_mapping");
				value.reason = text(*r, "reason");
				value.depth = number(*r, "depth");
				value.child_count = number(*r, "child_count");
				source(value, scope);
				value.tree_state = finite_population_state::complete;
				value.mapping_binding_state = finite_population_state::complete;
				const bool compound = value.kind == "conjunction" || value.kind == "disjunction",
						   unary = value.kind == "concept" || value.kind == "fold_and" ||
					value.kind == "fold_or",
						   atomic = value.kind == "atomic";
				const auto arity = compound ? 2U : unary ? 1U : 0U;
				if (value.profile != "clang22-sema-associated-constraint-normalization/1" ||
					(!compound && !unary && !atomic))
					axis(
						value.tree_state, value.gaps, value.node, "constraint-profile-unavailable");
				else if (!value.depth || !*value.depth || !value.child_count ||
						 *value.child_count != arity || value.left_child.empty() != (arity == 0) ||
						 value.right_child.empty() != (arity < 2))
					axis(value.tree_state,
						 value.gaps,
						 value.node,
						 "constraint-shape-conflicting",
						 true);
				const auto owner =
					reference(4, value.root_subject, scope, value.evidence, value.gaps);
				if (owner != finite_population_state::complete)
					axis(value.tree_state,
						 value.gaps,
						 value.node,
						 "constraint-root-unavailable",
						 owner == finite_population_state::conflicting);
				else if (text(row(find(4, key(value.root_subject, scope)).front()), "kind") !=
						 "constraint_root")
					axis(value.tree_state,
						 value.gaps,
						 value.node,
						 "constraint-owner-conflicting",
						 true);
				if (value.state != finite_population_state::complete)
					axis(value.tree_state,
						 value.gaps,
						 value.node,
						 "constraint-observation-unavailable",
						 value.state == finite_population_state::conflicting);
				for (const auto& child : {value.left_child, value.right_child})
					if (!child.empty())
					{
						const auto state = reference(5, child, scope, value.evidence, value.gaps);
						if (state != finite_population_state::complete)
							axis(value.tree_state,
								 value.gaps,
								 child,
								 "constraint-child-unavailable",
								 state == finite_population_state::conflicting);
						else
						{
							const auto& member = row(find(5, key(child, scope)).front());
							const auto depth = number(member, "depth");
							const auto suffix = child == value.left_child ? ".0" : ".1";
							if (text(member, "root_subject") != value.root_subject ||
								!value.depth || !depth ||
								*value.depth == std::numeric_limits<std::uint64_t>::max() ||
								*depth != *value.depth + 1 ||
								text(member, "path") != value.path + suffix)
								axis(value.tree_state,
									 value.gaps,
									 child,
									 "constraint-child-conflicting",
									 true);
						}
					}
				if (value.mapping_state != "complete" || !cell(*r, "parameter_mapping") ||
					(cell(*r, "parameter_mapping")->state != cell_state::absent &&
					 (!present(*r, "parameter_mapping") || value.parameter_mapping.empty())))
					axis(value.mapping_binding_state,
						 value.gaps,
						 value.node,
						 "constraint-mapping-unavailable");
				canonical(value.gaps);
				return value;
			}
			observed_lambda_capture capture(std::string id, const template_domain_population& scope)
			{
				observed_lambda_capture value;
				value.capture = std::move(id);
				value.compile_unit = scope.compile_unit;
				const auto* r = original(value, 6, value.capture, scope);
				if (!r)
					return value;
				value.lambda = text(*r, "lambda");
				value.kind = text(*r, "kind");
				value.profile = text(*r, "profile");
				value.source_span = text(*r, "source");
				value.captured_entity = text(*r, "captured_entity");
				const auto original_usr = bytes(*r, "captured_usr");
				for (auto byte : original_usr)
					value.captured_usr.push_back(static_cast<char>(byte));
				value.index = number(*r, "index");
				value.field_index = number(*r, "field_index");
				value.offset_bits = number(*r, "offset_bits");
				value.size_bits = number(*r, "size_bits");
				value.reason = text(*r, "reason");
				source(value, scope);
				value.entity_state =
					reference(3, value.captured_entity, scope, value.evidence, value.gaps);
				if (value.entity_state == finite_population_state::complete &&
					!original_usr.empty())
				{
					const auto& entity = row(find(3, key(value.captured_entity, scope)).front());
					if (!present(entity, "provider_local_key"))
						axis(value.entity_state,
							 value.gaps,
							 value.capture,
							 "capture-entity-usr-unavailable");
					else
					{
						constexpr std::string_view prefix = "clang-usr:";
						b.retain(prefix.size() + original_usr.size());
						std::vector<std::byte> framed;
						framed.reserve(prefix.size() + original_usr.size());
						for (const char c : prefix)
							framed.push_back(static_cast<std::byte>(c));
						framed.insert(framed.end(), original_usr.begin(), original_usr.end());
						if (bytes(entity, "provider_local_key") != framed)
							axis(value.entity_state,
								 value.gaps,
								 value.capture,
								 "capture-entity-usr-conflicting",
								 true);
					}
				}
				value.layout_state = finite_population_state::complete;
				const auto owner = reference(4, value.lambda, scope, value.evidence, value.gaps);
				if (value.profile != "clang22-lambda-capture-storage/1" ||
					owner != finite_population_state::complete || !value.field_index ||
					!value.offset_bits || !value.size_bits)
					axis(value.layout_state,
						 value.gaps,
						 value.capture,
						 "capture-layout-unavailable",
						 owner == finite_population_state::conflicting);
				else
				{
					const auto& lambda = row(find(4, key(value.lambda, scope)).front());
					const auto size = number(lambda, "closure_size_bits");
					if (text(lambda, "kind") != "lambda" || !size || *value.offset_bits > *size ||
						*value.size_bits > *size - *value.offset_bits)
						axis(value.layout_state,
							 value.gaps,
							 value.capture,
							 "capture-layout-conflicting",
							 true);
				}
				if (value.state != finite_population_state::complete)
					axis(value.layout_state,
						 value.gaps,
						 value.capture,
						 "capture-observation-unavailable",
						 value.state == finite_population_state::conflicting);
				canonical(value.gaps);
				return value;
			}
			observed_template_instantiation_frame frame(std::string id,
														const template_domain_population& scope)
			{
				observed_template_instantiation_frame value;
				value.frame = std::move(id);
				value.compile_unit = scope.compile_unit;
				const auto* r = original(value, 7, value.frame, scope);
				if (!r)
					return value;
				value.parent = text(*r, "parent");
				value.kind = text(*r, "kind");
				value.profile = text(*r, "profile");
				value.source_span = text(*r, "source");
				value.completion_state = text(*r, "completion_state");
				const auto eu = bytes(*r, "entity_usr"), tu = bytes(*r, "template_usr");
				for (auto byte : eu)
					value.entity_usr.push_back(static_cast<char>(byte));
				for (auto byte : tu)
					value.template_usr.push_back(static_cast<char>(byte));
				value.argument_profile = text(*r, "argument_profile");
				value.argument_state = text(*r, "argument_state");
				value.canonical_arguments = bytes(*r, "canonical_arguments");
				value.reason = text(*r, "reason");
				value.ordinal = number(*r, "ordinal");
				value.depth = number(*r, "depth");
				if (present(*r, "is_instantiation"))
					value.is_instantiation = boolean(*r, "is_instantiation");
				source(value, scope);
				value.stack_state = finite_population_state::complete;
				if (value.profile != "clang22-original-template-instantiation-stack/1" ||
					value.completion_state != "ended" || !value.ordinal || !value.depth ||
					!value.is_instantiation)
					axis(value.stack_state, value.gaps, value.frame, "stack-unavailable");
				if (!value.parent.empty())
				{
					const auto state =
						reference(7, value.parent, scope, value.evidence, value.gaps);
					if (state != finite_population_state::complete)
						axis(value.stack_state,
							 value.gaps,
							 value.parent,
							 "stack-parent-unavailable",
							 state == finite_population_state::conflicting);
					else
					{
						const auto& parent = row(find(7, key(value.parent, scope)).front());
						const auto ordinal = number(parent, "ordinal"),
								   depth = number(parent, "depth");
						if (!ordinal || !depth || !value.ordinal || !value.depth ||
							*ordinal >= *value.ordinal || *depth > *value.depth ||
							(value.is_instantiation && *value.is_instantiation &&
							 *depth >= *value.depth))
							axis(value.stack_state,
								 value.gaps,
								 value.frame,
								 "stack-parent-conflicting",
								 true);
					}
				}
				if (value.parent.empty() && value.depth && value.is_instantiation &&
					*value.depth != (*value.is_instantiation ? 1U : 0U))
					axis(value.stack_state,
						 value.gaps,
						 value.frame,
						 "stack-root-depth-conflicting",
						 true);
				value.argument_binding_state = finite_population_state::complete;
				if (value.argument_profile != "clang22-canonical-template-argument-tuple/1" ||
					value.argument_state != "complete" || !present(*r, "canonical_arguments") ||
					value.canonical_arguments.empty())
					axis(value.argument_binding_state,
						 value.gaps,
						 value.frame,
						 "argument-unavailable");
				canonical(value.gaps);
				return value;
			}
			void inventory(const identity& actual, const refs& inventory_rows)
			{
				b.charge(b.members, 1, b.limits.maximum_populations, "populations");
				b.retain(4096);
				template_domain_population scope;
				scope.compile_unit = actual[0];
				scope.universe = actual[1];
				scope.variant = actual[2];
				scope.interpretation = actual[3];
				scope.subject_state = scope.constraint_state = scope.capture_state =
					scope.frame_state = finite_population_state::complete;
				const auto lower_all = [&](std::string_view reason, bool conflict = false)
				{
					for (auto* state : {&scope.subject_state,
										&scope.constraint_state,
										&scope.capture_state,
										&scope.frame_state})
						axis(*state, scope.gaps, scope.compile_unit, reason, conflict);
				};
				if (!input.compile_units_complete)
					lower_all("unit-scan-unavailable");
				if (!input.inventory_inputs_complete)
					lower_all("inventory-scan-unavailable");

				b.bind(scope.evidence, inventory_rows);
				if (inventory_rows.empty())
				{
					lower_all("inventory-missing");
					output.populations.push_back(std::move(scope));
					return;
				}
				if (!equal(inventory_rows))
				{
					lower_all("inventory-conflicting", true);
					output.populations.push_back(std::move(scope));
					return;
				}
				const auto& r = row(inventory_rows.front());
				scope.inventory = text(r, "inventory");
				scope.profile = text(r, "profile");
				if (scope.profile != "clang22-original-template-domains/1" &&
					scope.profile != "clang22-original-template-domains/2")
					lower_all("inventory-profile-unavailable");
				if (scope.profile == "clang22-original-template-domains/1")
					axis(scope.subject_state,
						 scope.gaps,
						 scope.inventory,
						 "legacy-subject-admission-unavailable");
				const auto unit =
					reference(0, scope.compile_unit, scope, scope.evidence, scope.gaps);
				if (unit != finite_population_state::complete)
					lower_all("unit-unavailable", unit == finite_population_state::conflicting);
				else if (text(row(find(0, actual).front()), "variant") != scope.variant)
					lower_all("unit-world-conflicting", true);
				for (std::size_t family = 0; family < 4; ++family)
				{
					constexpr std::array prefixes{"subject", "constraint", "capture", "frame"};
					const auto prefix = std::string(prefixes[family]);
					const std::array complete_inputs{input.subject_inputs_complete,
													 input.constraint_inputs_complete,
													 input.capture_inputs_complete,
													 input.frame_inputs_complete};
					auto* state = family == 0 ? &scope.subject_state
						: family == 1		  ? &scope.constraint_state
						: family == 2		  ? &scope.capture_state
											  : &scope.frame_state;
					auto* count = family == 0 ? &scope.subject_count
						: family == 1		  ? &scope.constraint_count
						: family == 2		  ? &scope.capture_count
											  : &scope.frame_count;
					auto* ids = family == 0 ? &scope.subject_ids
						: family == 1		? &scope.constraint_ids
						: family == 2		? &scope.capture_ids
											: &scope.frame_ids;
					if (!input.domain_inputs_complete && !complete_inputs[family])
						axis(*state, scope.gaps, scope.inventory, "domain-scan-unavailable");
					*count = number(r, prefix + "_count");
					const auto original_ids = strings(r, prefix + "_ids");
					if (original_ids)
						*ids = *original_ids;
					if (text(r, prefix + "_state") != "complete" || !count->has_value() ||
						!original_ids)
						axis(*state, scope.gaps, scope.inventory, "enumeration-unavailable");
					else if (**count != ids->size())
						axis(*state,
							 scope.gaps,
							 scope.inventory,
							 "enumeration-count-conflicting",
							 true);
					const auto observed = unit_members[family].find(actual);
					std::set<std::string> returned;
					if (observed != unit_members[family].end())
						for (auto index : observed->second)
						{
							b.work();
							b.retain(256);
							returned.insert(text(row(index), identifiers[family + 4]));
						}
					for (const auto& member : *ids)
					{
						b.work();
						const auto& rows = find(family + 4, key(member, scope));
						if (rows.empty())
							axis(*state, scope.gaps, member, "enumeration-member-missing");
						else if (!equal(rows) ||
								 text(row(rows.front()), "compile_unit") != scope.compile_unit)
							axis(
								*state, scope.gaps, member, "enumeration-member-conflicting", true);
						if (family == 0 && !rows.empty() &&
							text(row(rows.front()), "profile") != scope.profile)
							axis(*state,
								 scope.gaps,
								 member,
								 "subject-admission-profile-unavailable");
						returned.erase(member);
					}
					if (!returned.empty())
						axis(*state,
							 scope.gaps,
							 scope.inventory,
							 "enumeration-extra-member-conflicting",
							 true);
				}
				for (const auto& id : scope.subject_ids)
				{
					b.work();
					scope.subjects.push_back(subject(id, scope));
				}
				for (const auto& id : scope.constraint_ids)
				{
					b.work();
					scope.constraints.push_back(constraint(id, scope));
				}
				for (const auto& id : scope.capture_ids)
				{
					b.work();
					scope.captures.push_back(capture(id, scope));
				}
				for (const auto& id : scope.frame_ids)
				{
					b.work();
					scope.frames.push_back(frame(id, scope));
				}
				// Index the original owned members once; unrelated families retain their own
				// closure.
				std::map<std::string, std::vector<std::size_t>> lambda_members, root_members;
				std::map<std::pair<std::string, std::string>, std::size_t> paths;
				std::map<std::pair<std::string, std::uint64_t>, std::size_t> capture_slots, fields;
				std::map<std::uint64_t, std::size_t> ordinals;
				for (std::size_t i = 0; i < scope.captures.size(); ++i)
				{
					b.work();
					auto& member = scope.captures[i];
					b.retain(256 + member.lambda.size());
					lambda_members[member.lambda].push_back(i);
					const auto bind_slot = [&](auto& index, const auto& slot)
					{
						if (!slot)
							return;
						b.retain(256 + member.lambda.size());
						const auto [other, inserted] =
							index.emplace(std::pair{member.lambda, *slot}, i);
						if (!inserted)
						{
							for (const auto n : {i, other->second})
								axis(scope.captures[n].layout_state,
									 scope.captures[n].gaps,
									 scope.captures[n].capture,
									 "capture-slot-conflicting",
									 true);
						}
					};
					bind_slot(capture_slots, member.index);
					bind_slot(fields, member.field_index);
				}
				for (std::size_t i = 0; i < scope.constraints.size(); ++i)
				{
					b.work();
					auto& member = scope.constraints[i];
					b.retain(512 + member.root_subject.size() + member.path.size());
					root_members[member.root_subject].push_back(i);
					const auto [other, inserted] =
						paths.emplace(std::pair{member.root_subject, member.path}, i);
					if (!inserted)
						for (const auto n : {i, other->second})
							axis(scope.constraints[n].tree_state,
								 scope.constraints[n].gaps,
								 scope.constraints[n].node,
								 "constraint-path-conflicting",
								 true);
				}
				for (auto& member : scope.constraints)
				{
					b.work();
					if (member.path == "0")
						continue;
					const auto dot = member.path.rfind('.');
					if (dot == std::string::npos ||
						(member.path.substr(dot) != ".0" && member.path.substr(dot) != ".1"))
					{
						axis(member.tree_state,
							 member.gaps,
							 member.node,
							 "constraint-path-conflicting",
							 true);
						continue;
					}
					const auto parent =
						paths.find({member.root_subject, member.path.substr(0, dot)});
					if (parent == paths.end())
						axis(member.tree_state,
							 member.gaps,
							 member.node,
							 "constraint-parent-unavailable");
					else
					{
						const auto& node = scope.constraints[parent->second];
						const auto& expected =
							member.path.substr(dot) == ".0" ? node.left_child : node.right_child;
						if (expected != member.node)
							axis(member.tree_state,
								 member.gaps,
								 member.node,
								 "constraint-parent-conflicting",
								 true);
					}
				}
				for (std::size_t i = 0; i < scope.frames.size(); ++i)
				{
					b.work();
					auto& member = scope.frames[i];
					if (!member.ordinal)
						continue;
					b.retain(128);
					const auto [other, inserted] = ordinals.emplace(*member.ordinal, i);
					if (!inserted)
						for (const auto n : {i, other->second})
							axis(scope.frames[n].stack_state,
								 scope.frames[n].gaps,
								 scope.frames[n].frame,
								 "stack-ordinal-conflicting",
								 true);
					if (scope.frame_count && *member.ordinal >= *scope.frame_count)
						axis(member.stack_state,
							 member.gaps,
							 member.frame,
							 "stack-ordinal-conflicting",
							 true);
				}
				for (auto& value : scope.subjects)
				{
					b.work();
					if (value.kind == "lambda")
					{
						const auto members = lambda_members.find(value.subject);
						if (members != lambda_members.end())
							for (const auto index : members->second)
							{
								b.work();
								const auto& member = scope.captures[index];
								if (!std::ranges::binary_search(value.capture_ids, member.capture))
									axis(value.capture_binding_state,
										 value.gaps,
										 value.subject,
										 "capture-extra-member-conflicting",
										 true);
							}
					}
					if (value.kind != "constraint_root")
						continue;
					if (value.normalization_binding_state == finite_population_state::complete)
					{
						const auto& root = row(find(5, key(value.constraint_root, scope)).front());
						if (text(root, "root_subject") != value.subject ||
							text(root, "path") != "0" ||
							number(root, "depth") != std::optional<std::uint64_t>{1})
							axis(value.normalization_binding_state,
								 value.gaps,
								 value.subject,
								 "normalization-root-conflicting",
								 true);
					}
					const auto members = root_members.find(value.subject);
					if (members != root_members.end())
						for (const auto index : members->second)
						{
							b.work();
							const auto& node = scope.constraints[index];
							if (node.tree_state != finite_population_state::complete)
								axis(value.normalization_binding_state,
									 value.gaps,
									 node.node,
									 "normalization-member-unavailable",
									 node.tree_state == finite_population_state::conflicting);
						}
				}
				canonical(scope.gaps);
				output.populations.push_back(std::move(scope));
			}
		};
		result<template_domain_projection>
		project_rows(template_domain_input input,
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
				p.output.domain_inputs_complete = input.domain_inputs_complete ||
					(input.subject_inputs_complete && input.constraint_inputs_complete &&
					 input.capture_inputs_complete && input.frame_inputs_complete);
				p.output.subject_inputs_complete =
					input.domain_inputs_complete || input.subject_inputs_complete;
				p.output.constraint_inputs_complete =
					input.domain_inputs_complete || input.constraint_inputs_complete;
				p.output.capture_inputs_complete =
					input.domain_inputs_complete || input.capture_inputs_complete;
				p.output.frame_inputs_complete =
					input.domain_inputs_complete || input.frame_inputs_complete;
				const std::array groups{input.units,
										input.files,
										input.spans,
										input.entities,
										input.subjects,
										input.constraints,
										input.captures,
										input.frames,
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
				for (const auto group : {4U, 5U, 6U, 7U})
					visit_group(group,
								[&](const annotated_row& original)
								{
									b.work();
									for (const auto name : {"source", "definition_source"})
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
									"sdk.template-input-invalid",
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
							if (group == 2U)
							{
								const auto* c = cell(r, "span");
								const auto* id =
									c && c->value ? std::get_if<std::string>(&*c->value) : nullptr;
								if (!id ||
									!std::ranges::any_of(r.presence.fragments,
														 [&](const auto& variant)
														 {
															 b.work();
															 return source_members.contains(
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
				return error{"sdk.template-resource-exhausted", "projection", "allocation"};
			}
			catch (const std::length_error&)
			{
				return error{"sdk.template-resource-exhausted", "projection", "length"};
			}
			(void)stop;
		}
	} // namespace
	result<template_domain_projection> project_template_domains(template_domain_input input,
																finite_population_limits limits,
																std::stop_token stop)
	{
		budget b{limits, stop};
		return project_rows(input, limits, stop, b);
	}
	result<template_domain_projection>
	project_template_domains(const application_query_results& input,
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
															  "sdk.template",
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
					scan.result.inputs_complete() && scan.result.conflicts().empty() &&
					scan.result.differential_disagreements().empty();
				const auto rows = query_transfer_access::borrow_rows(scan.result);
				b.charge(b.rows, rows.size(), limits.maximum_rows, "scan-rows");
				if (groups[group].size() > limits.maximum_rows - rows.size())
					fail("scan-rows", "limit-exceeded", "sdk.template-budget");
				b.retain(rows.size() * sizeof(const annotated_row*));
				groups[group].reserve(groups[group].size() + rows.size());
				for (const auto& original : rows)
				{
					b.work();
					groups[group].push_back(&original);
				}
			}
			b.rows = 0;
			template_domain_input raw{};
			raw.compile_units_complete = present[0] && complete[0];
			raw.inventory_inputs_complete = present[8] && complete[8];
			raw.subject_inputs_complete = present[4] && complete[4];
			raw.constraint_inputs_complete = present[5] && complete[5];
			raw.capture_inputs_complete = present[6] && complete[6];
			raw.frame_inputs_complete = present[7] && complete[7];
			auto result = project_rows(raw, limits, stop, b, &groups);
			if (!result)
				return result;
			for (std::size_t group = 0; group < 9; ++group)
				if (!present[group])
					result->unresolved.push_back({"sdk.template-scan-missing",
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
			return error{"sdk.template-resource-exhausted", "projection", "allocation"};
		}
		catch (const std::length_error&)
		{
			return error{"sdk.template-resource-exhausted", "projection", "length"};
		}
	}
} // namespace cxxlens::sdk::query
