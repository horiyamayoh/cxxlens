#include <algorithm>
#include <array>
#include <bit>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <tuple>

#include <cxxlens/sdk/function_compiler_facets.hpp>

namespace cxxlens::sdk::query
{
	namespace
	{
		using refs = std::vector<std::size_t>;
		using identity = std::array<std::string, 4U>;
		constexpr std::string_view dispatch_profile = "clang22-original-call-dispatch/1";
		constexpr std::string_view candidate_profile =
			"clang22-materialized-static-override-candidates/1";
		constexpr std::string_view storage_profile = "clang22-written-automatic-local-storage/1";
		constexpr std::string_view layout_profile = "clang22-declared-automatic-object-layout/1";
		struct failure
		{
			error value;
		};
		[[noreturn]] void fail(std::string_view field,
							   std::string_view reason,
							   std::string_view code = "sdk.function-facet-input-invalid")
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
		std::string_view text(const annotated_row& row, std::string_view name)
		{
			const auto* c = cell(row, name);
			const auto* v = c && c->state == cell_state::present && c->value
				? std::get_if<std::string>(&*c->value)
				: nullptr;
			return v ? std::string_view{*v} : std::string_view{};
		}
		std::optional<std::uint64_t> number(const annotated_row& row, std::string_view name)
		{
			const auto* c = cell(row, name);
			const auto* v = c && c->state == cell_state::present && c->value
				? std::get_if<std::uint64_t>(&*c->value)
				: nullptr;
			return v ? std::optional{*v} : std::nullopt;
		}
		finite_population_state state(std::string_view value)
		{
			if (value == "complete" || value == "observed")
				return finite_population_state::complete;
			if (value == "partial")
				return finite_population_state::partial;
			if (value == "conflicting")
				return finite_population_state::conflicting;
			return finite_population_state::unknown;
		}
		struct budget
		{
			finite_population_limits limits;
			std::stop_token stop;
			std::size_t operations{}, retained{}, members{}, references{};
			void charge(std::size_t& used,
						std::size_t amount,
						std::size_t maximum,
						std::string_view field)
			{
				if (stop.stop_requested() || (limits.cancelled && limits.cancelled()))
					fail(field, "cancelled", "sdk.function-facet-cancelled");
				if (used > maximum || amount > maximum - used)
					fail(field, "limit-exceeded", "sdk.function-facet-budget");
				used += amount;
			}
			void work(std::size_t n = 1U)
			{
				charge(operations, n, limits.maximum_operations, "operations");
			}
			void bytes(std::size_t n)
			{
				charge(retained, n, limits.maximum_retained_bytes, "retained-bytes");
			}
			void member()
			{
				charge(members, 1U, limits.maximum_members, "members");
			}
			std::string copy(std::string_view s)
			{
				work(s.size() + 1U);
				bytes(s.size() + sizeof(std::string));
				return std::string{s};
			}
			void bind(refs& to, const refs& from)
			{
				charge(references,
					   from.size(),
					   limits.maximum_evidence_references,
					   "evidence-references");
				bytes(from.size() * sizeof(std::size_t));
				to.insert(to.end(), from.begin(), from.end());
			}
			finite_population_limits nested()
			{
				auto result = limits;
				result.maximum_operations /= 2U;
				result.maximum_retained_bytes /= 2U;
				result.maximum_members /= 2U;
				result.maximum_evidence_references /= 2U;
				charge(members, result.maximum_members, limits.maximum_members, "members");
				charge(references,
					   result.maximum_evidence_references,
					   limits.maximum_evidence_references,
					   "evidence-references");
				result.maximum_evidence_bytes =
					std::min(result.maximum_evidence_bytes, result.maximum_retained_bytes);
				result.maximum_source_plan_bytes =
					std::min(result.maximum_source_plan_bytes, result.maximum_retained_bytes);
				work(result.maximum_operations);
				bytes(result.maximum_retained_bytes);
				return result;
			}
		};
		struct builder
		{
			budget& b;
			function_compiler_facet_projection result;
			std::map<identity, refs> entities;
			const annotated_row& row(std::size_t ref) const
			{
				if (ref >= result.original_actions.evidence.size())
					fail("evidence", "foreign-index");
				return result.original_actions.evidence[ref].row;
			}
			bool world(const annotated_row& r, const function_action_population& p) const
			{
				return r.interpretation == p.interpretation && r.presence.universe == p.universe &&
					std::ranges::find(r.presence.fragments, p.variant) !=
					r.presence.fragments.end();
			}
			refs select(const refs& evidence,
						std::string_view relation,
						std::string_view column,
						std::string_view id,
						const function_action_population& p)
			{
				refs found;
				for (auto ref : evidence)
				{
					b.work();
					if (ref >= result.original_actions.evidence.size())
						fail("evidence", "foreign-index");
					const auto& item = result.original_actions.evidence[ref];
					if (item.relation_id == relation && text(item.row, column) == id &&
						world(item.row, p))
					{
						b.bytes(sizeof(ref));
						found.push_back(ref);
					}
				}
				return found;
			}
			std::vector<std::string> strings(const annotated_row& r, std::string_view name)
			{
				const auto* c = cell(r, name);
				if (!c || c->state != cell_state::present || !c->value)
					return {};
				const auto* data = std::get_if<std::vector<std::byte>>(&*c->value);
				if (!data)
					fail(name, "set-bytes-required");
				std::vector<std::string> values;
				for (std::size_t offset{}; offset < data->size();)
				{
					b.work();
					if (data->size() - offset < 4U)
						fail(name, "set-length-truncated");
					std::uint32_t length{};
					for (unsigned shift{}; shift < 32U; shift += 8U)
						length |= std::to_integer<std::uint32_t>((*data)[offset++]) << shift;
					if (length > data->size() - offset)
						fail(name, "set-value-truncated");
					b.work(length);
					b.bytes(length + sizeof(std::string));
					b.member();
					std::string value;
					value.reserve(length);
					for (const auto end = offset + length; offset < end; ++offset)
						value += static_cast<char>((*data)[offset]);
					if (value.empty() || (!values.empty() && values.back() >= value))
						fail(name, "canonical-set-required");
					values.push_back(std::move(value));
				}
				return values;
			}
			void gap(std::vector<query_unresolved>& gaps,
					 finite_population_state& axis,
					 std::string_view subject,
					 std::string_view reason,
					 bool conflict = false)
			{
				b.bytes(256U + subject.size() + reason.size());
				b.member();
				gaps.push_back(
					{"sdk.function-facet-" + std::string{reason}, std::string{subject}, {}});
				if (conflict)
					axis = finite_population_state::conflicting;
				else if (axis == finite_population_state::complete)
					axis = finite_population_state::partial;
			}
			bool same(const refs& originals, std::span<const std::string_view> columns)
			{
				if (originals.empty())
					return false;
				for (auto ref : originals)
					for (const auto name : columns)
					{
						b.work();
						const auto* left = cell(row(originals.front()), name);
						const auto* right = cell(row(ref), name);
						if (!left || !right)
						{
							if (left != right)
								return false;
							continue;
						}
						const auto cost = [](const detached_cell& c)
						{
							return c.value ? std::visit(
												 [](const auto& v) -> std::size_t
												 {
													 if constexpr (requires { v.size(); })
														 return v.size();
													 else
														 return 1U;
												 },
												 *c.value)
										   : 1U;
						};
						b.work(cost(*left) + cost(*right));
						if (left->type != right->type || left->state != right->state ||
							left->value != right->value ||
							left->unknown_reason != right->unknown_reason)
							return false;
					}
				return true;
			}
			bool observed_agree(const refs& originals, std::span<const std::string_view> columns)
			{
				for (const auto name : columns)
				{
					refs observed;
					for (auto ref : originals)
					{
						b.work();
						if (present(row(ref), name))
						{
							b.bytes(sizeof(ref));
							observed.push_back(ref);
						}
					}
					const std::array field{name};
					if (!observed.empty() && !same(observed, field))
						return false;
				}
				return true;
			}
			bool all_present(const refs& originals, std::span<const std::string_view> columns)
			{
				if (originals.empty())
					return false;
				for (auto ref : originals)
					for (const auto name : columns)
					{
						b.work();
						if (!present(row(ref), name))
							return false;
					}
				return true;
			}
			void index()
			{
				for (std::size_t ref{}; ref < result.original_actions.evidence.size(); ++ref)
				{
					b.work();
					const auto& item = result.original_actions.evidence[ref];
					if (item.relation_id != "cc.entity.v1")
						continue;
					for (const auto& variant : item.row.presence.fragments)
					{
						b.work();
						b.bytes(256U + text(item.row, "entity").size() + variant.size());
						entities[{std::string{text(item.row, "entity")},
								  item.row.presence.universe,
								  variant,
								  item.row.interpretation}]
							.push_back(ref);
					}
				}
			}
			bool binding(const observed_function_action& a,
						 const function_action_population& p,
						 const refs& originals)
			{
				constexpr std::array<std::string_view, 15U> fields{"operation",
																   "site",
																   "scope_declaration",
																   "compile_unit",
																   "function",
																   "body",
																   "kind",
																   "origin",
																   "source",
																   "expression",
																   "call",
																   "expression_context",
																   "context_declaration",
																   "node",
																   "element_index"};
				if (!same(originals, fields))
					return false;
				for (auto ref : originals)
				{
					const auto& r = row(ref);
					b.work();
					if (text(r, "scope_declaration") != p.declaration ||
						text(r, "compile_unit") != p.compile_unit ||
						text(r, "function") != p.function || text(r, "operation") != a.operation ||
						text(r, "site") != a.site)
						return false;
				}
				return true;
			}
			observed_function_dispatch dispatch(const observed_function_action& a,
												const function_action_population& p,
												const refs& originals)
			{
				observed_function_dispatch out;
				b.member();
				b.bytes(sizeof(out));
				out.operation = b.copy(a.operation);
				out.site = b.copy(a.site);
				out.call = b.copy(a.call);
				out.kind = b.copy(a.kind);
				out.binding_state = binding(a, p, originals) ? finite_population_state::complete
															 : finite_population_state::conflicting;
				b.bind(out.evidence, originals);
				for (auto axis : {a.source_state, a.site_binding_state, a.context_state})
					if (axis != finite_population_state::complete)
						gap(out.gaps,
							out.binding_state,
							a.operation,
							"dispatch-source-site-context-unavailable",
							axis == finite_population_state::conflicting);
				if (!a.call.empty() && a.call_state != finite_population_state::complete)
					gap(out.gaps,
						out.binding_state,
						a.operation,
						"dispatch-call-unavailable",
						a.call_state == finite_population_state::conflicting);
				if (out.binding_state == finite_population_state::conflicting)
					gap(out.gaps,
						out.binding_state,
						a.operation,
						"operation-owner-conflicting",
						true);
				refs facets;
				for (auto ref : originals)
					if (present(row(ref), "dispatch_kind"))
					{
						b.bytes(sizeof(ref));
						facets.push_back(ref);
					}
				if (!a.call.empty() && a.call_state == finite_population_state::complete)
				{
					auto calls = select(a.evidence, "cc.call_site.v1", "call", a.call, p);
					for (auto ref : calls)
						if (present(row(ref), "dispatch_kind"))
							facets.push_back(ref);
					b.bind(out.evidence, calls);
				}
				constexpr std::array<std::string_view, 3U> dispatch_fields{
					"dispatch_kind", "dispatch_state", "dispatch_profile"};
				constexpr std::array<std::string_view, 5U> candidate_fields{"candidate_presence",
																			"candidate_count",
																			"candidate_targets",
																			"candidate_state",
																			"candidate_profile"};
				if (facets.empty())
				{
					gap(out.gaps, out.dispatch_state, a.operation, "dispatch-unavailable");
					return out;
				}
				if (!observed_agree(facets, dispatch_fields))
				{
					gap(out.gaps, out.dispatch_state, a.operation, "dispatch-conflicting", true);
					return out;
				}
				const auto& r = row(facets.front());
				out.dispatch_kind = b.copy(text(r, "dispatch_kind"));
				out.dispatch_profile = b.copy(text(r, "dispatch_profile"));
				out.dispatch_state = state(text(r, "dispatch_state"));
				if (!all_present(facets, dispatch_fields))
					gap(out.gaps, out.dispatch_state, a.operation, "dispatch-unavailable");
				constexpr std::array kinds{"direct", "virtual", "indirect", "dependent", "unknown"};
				if (out.dispatch_profile != dispatch_profile ||
					std::ranges::find(kinds, out.dispatch_kind) == kinds.end())
					gap(out.gaps, out.dispatch_state, a.operation, "dispatch-profile-unavailable");
				out.candidate_presence = b.copy(text(r, "candidate_presence"));
				out.candidate_profile = b.copy(text(r, "candidate_profile"));
				out.candidate_count = number(r, "candidate_count");
				out.candidate_targets = strings(r, "candidate_targets");
				out.candidate_state = state(text(r, "candidate_state"));
				if (!observed_agree(facets, candidate_fields))
					gap(out.gaps, out.candidate_state, a.operation, "candidate-conflicting", true);
				if (out.candidate_presence == "present" && !all_present(facets, candidate_fields))
					gap(out.gaps, out.candidate_state, a.operation, "candidate-unavailable");
				if (out.candidate_presence != "present" && out.candidate_presence != "absent" &&
					out.candidate_presence != "unknown")
					gap(out.gaps,
						out.candidate_state,
						a.operation,
						"candidate-presence-unavailable");
				if (out.candidate_presence == "present")
				{
					if (out.candidate_profile != candidate_profile || !out.candidate_count ||
						!present(r, "candidate_targets"))
						gap(out.gaps,
							out.candidate_state,
							a.operation,
							"candidate-population-unavailable");
					if (out.candidate_count && *out.candidate_count < out.candidate_targets.size())
						gap(out.gaps,
							out.candidate_state,
							a.operation,
							"candidate-count-conflicting",
							true);
					if (out.candidate_state == finite_population_state::complete &&
						out.candidate_count && *out.candidate_count != out.candidate_targets.size())
						gap(out.gaps,
							out.candidate_state,
							a.operation,
							"candidate-count-conflicting",
							true);
					for (const auto& id : out.candidate_targets)
					{
						b.work();
						const auto found =
							entities.find({id, p.universe, p.variant, p.interpretation});
						if (found == entities.end())
						{
							gap(out.gaps, out.candidate_state, id, "candidate-target-unavailable");
							continue;
						}
						b.bind(out.evidence, found->second);
						constexpr std::array<std::string_view, 2U> target_fields{"entity", "kind"};
						constexpr std::array callable{
							"function", "method", "constructor", "destructor", "conversion"};
						if (!same(found->second, target_fields) ||
							std::ranges::find(callable, text(row(found->second.front()), "kind")) ==
								callable.end())
							gap(out.gaps,
								out.candidate_state,
								id,
								"candidate-target-conflicting",
								true);
					}
				}
				else if (out.candidate_presence == "absent" &&
						 ((out.candidate_count && *out.candidate_count != 0U) ||
						  !out.candidate_targets.empty()))
					gap(out.gaps,
						out.candidate_state,
						a.operation,
						"absent-candidate-payload-conflicting",
						true);
				return out;
			}
			observed_function_automatic_storage storage(const observed_function_action& a,
														const function_action_population& p,
														const refs& originals)
			{
				observed_function_automatic_storage out;
				b.member();
				b.bytes(sizeof(out));
				out.operation = b.copy(a.operation);
				out.object_declaration = b.copy(a.object_declaration);
				out.object_entity = b.copy(a.object_entity);
				out.object_type = b.copy(a.object_type);
				b.bind(out.evidence, originals);
				constexpr std::array<std::string_view, 7U> fields{"storage_duration",
																  "storage_state",
																  "storage_profile",
																  "storage_size_bytes",
																  "storage_alignment_bytes",
																  "storage_abi_context",
																  "storage_target_triple"};
				out.binding_state = binding(a, p, originals) && same(originals, fields)
					? finite_population_state::complete
					: finite_population_state::conflicting;
				if (originals.empty())
				{
					gap(out.gaps, out.binding_state, a.operation, "storage-original-unavailable");
					return out;
				}
				const auto& r = row(originals.front());
				out.storage_duration = b.copy(text(r, "storage_duration"));
				out.profile = b.copy(text(r, "storage_profile"));
				out.abi_context = b.copy(text(r, "storage_abi_context"));
				out.target_triple = b.copy(text(r, "storage_target_triple"));
				out.size_bytes = number(r, "storage_size_bytes");
				out.alignment_bytes = number(r, "storage_alignment_bytes");
				out.layout_state = state(text(r, "storage_state"));
				if (out.storage_duration.empty() || out.profile.empty())
					gap(out.gaps, out.binding_state, a.operation, "storage-admission-unavailable");
				if ((!out.storage_duration.empty() && out.storage_duration != "automatic") ||
					(!out.profile.empty() && out.profile != layout_profile) ||
					a.kind != "type_use" || a.type_use_kind != "local")
					gap(out.gaps,
						out.binding_state,
						a.operation,
						"storage-admission-conflicting",
						true);
				if (a.source_state != finite_population_state::complete ||
					a.context_state != finite_population_state::complete)
					gap(out.gaps,
						out.binding_state,
						a.operation,
						"storage-source-context-unavailable",
						a.source_state == finite_population_state::conflicting ||
							a.context_state == finite_population_state::conflicting);
				if (out.abi_context.empty() || out.target_triple.empty())
					gap(out.gaps, out.binding_state, a.operation, "storage-abi-unavailable");
				if (out.layout_state == finite_population_state::complete &&
					(!out.size_bytes || !out.alignment_bytes || !*out.alignment_bytes))
					gap(out.gaps,
						out.layout_state,
						a.operation,
						"storage-layout-conflicting",
						true);
				return out;
			}
			std::optional<bool> local_storage(const observed_function_action& a,
											  const function_action_population& p,
											  const refs& originals,
											  function_compiler_facet_population& out)
			{
				auto details =
					select(p.evidence, "cc.entity_detail.v1", "entity", a.object_entity, p);
				details.erase(
					std::ranges::remove_if(details,
										   [&](auto ref)
										   {
											   b.work();
											   return text(row(ref), "source") != a.object_source ||
												   text(row(ref), "compile_unit") != p.compile_unit;
										   })
						.begin(),
					details.end());
				const auto entity =
					entities.find({a.object_entity, p.universe, p.variant, p.interpretation});
				b.bind(out.evidence, details);
				if (details.empty() || entity == entities.end())
				{
					gap(out.gaps,
						out.storage_enumeration_state,
						a.operation,
						"local-storage-classification-unavailable");
					return std::nullopt;
				}
				b.bind(out.evidence, entity->second);
				constexpr std::array<std::string_view, 1U> detail_fields{"flags"};
				constexpr std::array<std::string_view, 2U> entity_fields{"kind", "semantic_owner"};
				if (!same(details, detail_fields) || !same(entity->second, entity_fields) ||
					text(row(entity->second.front()), "semantic_owner") != p.function ||
					text(row(entity->second.front()), "kind") != "variable")
				{
					gap(out.gaps,
						out.storage_enumeration_state,
						a.operation,
						"local-storage-classification-conflicting",
						true);
					return std::nullopt;
				}
				const auto flags = strings(row(details.front()), "flags");
				const auto has = [&](std::string_view flag)
				{
					b.work();
					return std::ranges::binary_search(flags, flag);
				};
				if (!has("finite_variable_storage_v1"))
				{
					gap(out.gaps,
						out.storage_enumeration_state,
						a.operation,
						"local-storage-classification-unavailable");
					return std::nullopt;
				}
				std::string_view duration;
				std::size_t classified{};
				for (const auto& [flag, name] :
					 std::array<std::pair<std::string_view, std::string_view>, 4U>{
						 {{"storage_automatic", "automatic"},
						  {"storage_static", "static"},
						  {"storage_thread", "thread"},
						  {"storage_parameter", "parameter"}}})
					if (has(flag))
					{
						duration = name;
						++classified;
					}
				if (classified != 1U || duration == "parameter")
				{
					gap(out.gaps,
						out.storage_enumeration_state,
						a.operation,
						"local-storage-classification-conflicting",
						true);
					return std::nullopt;
				}
				if (!originals.empty() && present(row(originals.front()), "storage_duration") &&
					text(row(originals.front()), "storage_duration") != duration)
					gap(out.gaps,
						out.storage_enumeration_state,
						a.operation,
						"local-storage-duration-conflicting",
						true);
				return duration == "automatic" && !has("storage_non_object");
			}
			void population(const function_action_population& p)
			{
				if (result.populations.size() >= b.limits.maximum_populations)
					fail("populations", "limit-exceeded", "sdk.function-facet-budget");
				function_compiler_facet_population out;
				b.bytes(sizeof(out));
				out.declaration = b.copy(p.declaration);
				out.function = b.copy(p.function);
				out.compile_unit = b.copy(p.compile_unit);
				out.body = b.copy(p.body);
				out.source_span = b.copy(p.source_span);
				out.universe = b.copy(p.universe);
				out.variant = b.copy(p.variant);
				out.interpretation = b.copy(p.interpretation);
				out.enumeration_state = p.enumeration_state;
				out.binding_state = finite_population_state::complete;
				b.bind(out.evidence, p.evidence);
				auto details = select(p.evidence, "cc.entity_detail.v1", "entity", p.function, p);
				details.erase(
					std::ranges::remove_if(details,
										   [&](auto ref)
										   {
											   b.work();
											   return text(row(ref), "source") != p.source_span ||
												   text(row(ref), "compile_unit") != p.compile_unit;
										   })
						.begin(),
					details.end());
				constexpr std::array<std::string_view, 4U> fields{"automatic_storage_count",
																  "automatic_storage_ids",
																  "automatic_storage_state",
																  "automatic_storage_profile"};
				b.bytes(details.size() * sizeof(std::size_t));
				refs inventories = details;
				if (!p.body.empty())
				{
					auto bodies = select(p.evidence, "cc.body.v1", "body", p.body, p);
					b.bind(out.evidence, bodies);
					if (bodies.empty())
						gap(out.gaps,
							out.storage_enumeration_state,
							p.declaration,
							"storage-body-unavailable");
					b.bytes(bodies.size() * sizeof(std::size_t));
					inventories.insert(inventories.end(), bodies.begin(), bodies.end());
				}
				if (result.storage_inputs_complete && !inventories.empty() &&
					observed_agree(inventories, fields))
				{
					const auto& r = row(inventories.front());
					out.storage_profile = b.copy(text(r, "automatic_storage_profile"));
					out.automatic_storage_count = number(r, "automatic_storage_count");
					out.automatic_storage_ids = strings(r, "automatic_storage_ids");
					out.storage_enumeration_state = state(text(r, "automatic_storage_state"));
					if (details.empty() ||
						(!p.body.empty() && inventories.size() == details.size()) ||
						!all_present(inventories, fields))
						gap(out.gaps,
							out.storage_enumeration_state,
							p.declaration,
							"storage-census-unavailable");
					if (out.storage_profile != storage_profile || !out.automatic_storage_count ||
						!present(r, "automatic_storage_ids"))
						gap(out.gaps,
							out.storage_enumeration_state,
							p.declaration,
							"storage-census-unavailable");
					if (out.automatic_storage_count &&
						(*out.automatic_storage_count < out.automatic_storage_ids.size() ||
						 (out.storage_enumeration_state == finite_population_state::complete &&
						  *out.automatic_storage_count != out.automatic_storage_ids.size())))
						gap(out.gaps,
							out.storage_enumeration_state,
							p.declaration,
							"storage-census-conflicting",
							true);
				}
				else
					gap(out.gaps,
						out.storage_enumeration_state,
						p.declaration,
						"storage-census-unavailable",
						!inventories.empty() && !observed_agree(inventories, fields));
				std::set<std::string, std::less<>> observed;
				for (const auto& a : p.actions)
				{
					b.work();
					auto originals =
						select(a.evidence, "cc.operation.v1", "operation", a.operation, p);
					if (!binding(a, p, originals))
						gap(out.gaps,
							out.binding_state,
							a.operation,
							"operation-owner-conflicting",
							true);
					constexpr std::array callable_kinds{"invocation",
														"construction",
														"allocation",
														"deallocation",
														"initialization_failure_deallocation"};
					if (result.dispatch_inputs_complete &&
						(!a.call.empty() ||
						 std::ranges::find(callable_kinds, a.kind) != callable_kinds.end()))
						out.dispatches.push_back(dispatch(a, p, originals));
					if (std::ranges::binary_search(out.automatic_storage_ids, a.operation))
					{
						b.bytes(a.operation.size() + 128U);
						observed.insert(a.operation);
						out.automatic_storage.push_back(storage(a, p, originals));
					}
					if (a.kind == "type_use" && a.type_use_kind == "local")
					{
						const auto automatic = local_storage(a, p, originals, out);
						if (automatic &&
							*automatic !=
								std::ranges::binary_search(out.automatic_storage_ids, a.operation))
							gap(out.gaps,
								out.storage_enumeration_state,
								a.operation,
								"storage-admission-census-conflicting",
								true);
					}
				}
				for (const auto& id : out.automatic_storage_ids)
				{
					b.work();
					if (!observed.contains(id))
						gap(out.gaps,
							out.storage_enumeration_state,
							id,
							"storage-member-unavailable");
				}
				if (out.enumeration_state != finite_population_state::complete)
					gap(out.gaps,
						out.storage_enumeration_state,
						p.declaration,
						"action-census-unavailable");
				const auto finish = [&](auto& value)
				{
					std::ranges::sort(value.evidence,
									  [&](auto left, auto right)
									  {
										  b.work();
										  return left < right;
									  });
					value.evidence.erase(std::ranges::unique(value.evidence).begin(),
										 value.evidence.end());
					std::ranges::sort(value.gaps,
									  [&](const auto& left, const auto& right)
									  {
										  b.work(left.code.size() + right.code.size() +
												 left.subject.size() + right.subject.size() +
												 left.detail.size() + right.detail.size() + 1U);
										  return std::tie(left.code, left.subject, left.detail) <
											  std::tie(right.code, right.subject, right.detail);
									  });
					value.gaps.erase(std::ranges::unique(value.gaps).begin(), value.gaps.end());
					for (const auto& g : value.gaps)
					{
						b.member();
						b.bytes(sizeof(g) + g.code.size() + g.subject.size() + g.detail.size());
						result.unresolved.push_back(g);
					}
				};
				for (auto& value : out.dispatches)
					finish(value);
				for (auto& value : out.automatic_storage)
					finish(value);
				finish(out);
				result.populations.push_back(std::move(out));
			}
			function_compiler_facet_projection run()
			{
				index();
				for (const auto& p : result.original_actions.populations)
				{
					b.work();
					population(p);
				}
				std::ranges::sort(result.unresolved,
								  [&](const auto& left, const auto& right)
								  {
									  b.work(left.code.size() + right.code.size() +
											 left.subject.size() + right.subject.size() +
											 left.detail.size() + right.detail.size() + 1U);
									  return std::tie(left.code, left.subject, left.detail) <
										  std::tie(right.code, right.subject, right.detail);
								  });
				result.unresolved.erase(
					std::ranges::unique(result.unresolved,
										[&](const auto& left, const auto& right)
										{
											b.work(left.code.size() + right.code.size() +
												   left.subject.size() + right.subject.size() +
												   left.detail.size() + right.detail.size() + 1U);
											return left == right;
										})
						.begin(),
					result.unresolved.end());
				return std::move(result);
			}
		};
		template <class F>
		result<function_compiler_facet_projection> project(F&& original,
														   finite_population_limits limits,
														   std::stop_token stop,
														   bool dispatch_complete,
														   bool storage_complete)
		{
			try
			{
				budget b{limits, stop};
				b.work();
				auto nested = b.nested();
				auto actions = original(nested);
				if (!actions)
					return actions.error();
				function_compiler_facet_projection result;
				result.original_actions = std::move(*actions);
				result.dispatch_inputs_complete = dispatch_complete;
				result.storage_inputs_complete = storage_complete;
				result.dispatch_inputs_complete = result.dispatch_inputs_complete &&
					result.original_actions.operation_inputs_complete;
				result.storage_inputs_complete = result.storage_inputs_complete &&
					result.original_actions.operation_inputs_complete &&
					result.original_actions.scope_inputs_complete &&
					result.original_actions.admission_inputs_complete;
				builder value{b, std::move(result), {}};
				return value.run();
			}
			catch (const failure& f)
			{
				return f.value;
			}
			catch (const std::bad_alloc&)
			{
				return error{
					"sdk.function-facet-resource-exhausted", "allocation", "bounded-owned-input"};
			}
			catch (const std::length_error&)
			{
				return error{
					"sdk.function-facet-resource-exhausted", "allocation", "bounded-owned-input"};
			}
		}
	} // namespace
	result<function_compiler_facet_projection> project_function_compiler_facets(
		function_compiler_facet_input input, finite_population_limits limits, std::stop_token stop)
	{
		return project(
			[&](auto nested)
			{
				return project_function_actions(input.actions, nested, stop);
			},
			limits,
			stop,
			input.dispatch_inputs_complete,
			input.storage_inputs_complete);
	}
	result<function_compiler_facet_projection>
	project_function_compiler_facets(const application_query_results& input,
									 finite_population_limits limits,
									 std::stop_token stop)
	{
		return project(
			[&](auto nested)
			{
				return project_function_actions(input, nested, stop);
			},
			limits,
			stop,
			true,
			true);
	}
} // namespace cxxlens::sdk::query
