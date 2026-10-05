#include <algorithm>
#include <array>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <tuple>

#include <cxxlens/sdk/record_surfaces.hpp>

#include "query_projection_plan_limits_internal.hpp"

namespace cxxlens::sdk::query
{
	namespace
	{
		constexpr std::array<std::string_view, 8> relations{"build.compile_unit.v1",
															"source.file.v1",
															"source.span.v1",
															"cc.entity.v1",
															"cc.entity_detail.v1",
															"cc.entity_edge.v1",
															"cc.record_surface.v1",
															"cc.record_inventory.v1"};
		using world = std::array<std::string, 3>;
		using key = std::tuple<std::size_t, std::string, world>;
		struct failure
		{
			error value;
		};
		[[noreturn]] void fail(std::string code, std::string field, std::string detail)
		{
			throw failure{{std::move(code), std::move(field), std::move(detail)}};
		}
		void check(std::stop_token stop)
		{
			if (stop.stop_requested())
				fail("sdk.record-cancelled", "projection", "stop-requested");
		}
		void
		charge(std::size_t& total, std::size_t amount, std::size_t limit, std::string_view field)
		{
			if (total > limit || amount > limit - total)
				fail("sdk.record-budget", std::string{field}, "limit-exceeded");
			total += amount;
		}
		const detached_cell& cell(const annotated_row& row, std::string_view name)
		{
			const auto found = row.values.find("output." + std::string{name});
			if (found == row.values.end())
				fail("sdk.record-input-invalid", std::string{name}, "column-missing");
			return found->second;
		}
		std::string text(const annotated_row& row, std::string_view name)
		{
			const auto& value = cell(row, name);
			if (value.state != cell_state::present || !value.value)
				return {};
			const auto* actual = std::get_if<std::string>(&*value.value);
			if (!actual)
				fail("sdk.record-input-invalid", std::string{name}, "string-column-required");
			return *actual;
		}
		bool same_payload(const annotated_row& a, const annotated_row& b)
		{
			return std::ranges::equal(a.values,
									  b.values,
									  [](const auto& x, const auto& y)
									  {
										  return x.first == y.first &&
											  x.second.type == y.second.type &&
											  x.second.state == y.second.state &&
											  x.second.value == y.second.value &&
											  x.second.unknown_reason == y.second.unknown_reason;
									  });
		}
		template <class T>
		T scalar(const annotated_row& row, std::string_view name)
		{
			const auto& value = cell(row, name);
			if (value.state != cell_state::present || !value.value ||
				!std::holds_alternative<T>(*value.value))
				fail("sdk.record-input-invalid", std::string{name}, "present-scalar-required");
			return std::get<T>(*value.value);
		}
		std::vector<std::string> strings(const annotated_row& row,
										 std::string_view name,
										 std::size_t& bytes,
										 std::size_t& references,
										 const record_surface_limits& limits,
										 std::stop_token stop)
		{
			const auto& value = cell(row, name);
			if (value.state != cell_state::present || !value.value ||
				!std::holds_alternative<std::vector<std::byte>>(*value.value))
				fail("sdk.record-input-invalid", std::string{name}, "present-set-required");
			const auto& encoded = std::get<std::vector<std::byte>>(*value.value);
			std::vector<std::string> result;
			for (std::size_t begin{}; begin < encoded.size();)
			{
				check(stop);
				if (encoded.size() - begin < 4U)
					fail("sdk.record-input-invalid", std::string{name}, "set-length-truncated");
				std::uint32_t count{};
				for (unsigned shift{}; shift < 32U; shift += 8U)
					count |= std::to_integer<std::uint32_t>(encoded[begin++]) << shift;
				if (count > encoded.size() - begin)
					fail("sdk.record-input-invalid", std::string{name}, "set-element-truncated");
				charge(
					bytes, count + sizeof(std::string), limits.maximum_retained_bytes, "set-bytes");
				charge(references, 1U, limits.maximum_evidence_references, "set-elements");
				std::string element;
				element.reserve(count);
				for (std::size_t index{}; index < count; ++index)
					element.push_back(std::to_integer<char>(encoded[begin++]));
				result.push_back(std::move(element));
			}
			return result;
		}
		void normalize(std::vector<query_unresolved>& gaps)
		{
			std::ranges::sort(gaps,
							  {},
							  [](const auto& gap)
							  {
								  return std::tie(gap.code, gap.subject, gap.detail);
							  });
			gaps.erase(std::ranges::unique(gaps).begin(), gaps.end());
		}
		struct budget
		{
			std::size_t bytes{}, evidence{}, references{};
		};
		struct entry
		{
			std::size_t group{};
			const annotated_row* row{};
			std::string canonical;
		};
		result<record_surface_projection> project_rows(record_surface_input input,
													   record_surface_limits limits,
													   std::stop_token stop,
													   budget& retained)
		{
			if (auto valid = limits.validate(); !valid)
				return valid.error();
			try
			{
				check(stop);
				const std::array groups{input.units,
										input.files,
										input.spans,
										input.entities,
										input.details,
										input.edges,
										input.surfaces,
										input.inventories};
				constexpr std::array<std::string_view, 8> identifiers{"compile_unit",
																	  "snapshot",
																	  "span",
																	  "entity",
																	  "entity",
																	  "source_entity",
																	  "surface",
																	  "inventory"};
				std::vector<entry> entries;
				std::size_t expansions{}, members{}, operations{};
				const auto work = [&]()
				{
					check(stop);
					charge(operations, 1U, limits.maximum_operations, "operations");
				};
				const auto descriptors = standard_relation_descriptors();
				for (std::size_t group{}; group < groups.size(); ++group)
				{
					const auto descriptor =
						std::ranges::find(descriptors, relations[group], &relation_descriptor::id);
					if (descriptor == descriptors.end())
						fail("sdk.record-input-invalid",
							 std::string{relations[group]},
							 "descriptor-missing");
					for (const auto& row : groups[group])
					{
						check(stop);
						if (entries.size() >= limits.maximum_rows)
							fail("sdk.record-budget", "rows", "limit-exceeded");
						if (auto valid = row.validate(); !valid)
							return valid.error();
						for (const auto& column : descriptor->columns)
						{
							const auto& actual = cell(row, column.name);
							if (actual.type != column.type || !actual.validate())
								fail("sdk.record-input-invalid",
									 column.id,
									 "column-type-or-value-invalid");
						}
						charge(expansions,
							   row.presence.fragments.size(),
							   limits.maximum_condition_expansions,
							   "condition-expansions");
						auto canonical = row.canonical_form();
						charge(retained.evidence,
							   canonical.size(),
							   limits.maximum_evidence_bytes,
							   "evidence-bytes");
						charge(retained.bytes,
							   canonical.size() * 2U + sizeof(entry) +
								   sizeof(record_surface_evidence),
							   limits.maximum_retained_bytes,
							   "retained-evidence");
						entries.push_back({group, &row, std::move(canonical)});
					}
				}
				std::ranges::sort(entries,
								  {},
								  [](const auto& e)
								  {
									  return std::tie(e.group, e.canonical);
								  });
				record_surface_projection output;
				output.compile_units_complete = input.compile_units_complete;
				output.inventory_inputs_complete = input.inventory_inputs_complete;
				std::map<key, std::vector<std::size_t>> index;
				using edge_key =
					std::tuple<world, std::string, std::string, std::string, std::string>;
				std::map<edge_key, std::vector<std::size_t>> edge_index;
				for (std::size_t ref{}; ref < entries.size(); ++ref)
				{
					check(stop);
					const auto& entry = entries[ref];
					output.evidence.push_back({std::string{relations[entry.group]}, *entry.row});
					const auto id = text(*entry.row, identifiers[entry.group]);
					for (const auto& variant : entry.row->presence.fragments)
					{
						check(stop);
						charge(retained.bytes,
							   sizeof(key) + 96U + id.size() + variant.size() +
								   entry.row->presence.universe.size() +
								   entry.row->interpretation.size(),
							   limits.maximum_retained_bytes,
							   "conditioned-index");
						charge(retained.references,
							   1U,
							   limits.maximum_evidence_references,
							   "conditioned-index-references");
						index[{entry.group,
							   id,
							   {entry.row->presence.universe, variant, entry.row->interpretation}}]
							.push_back(ref);
						if (entry.group == 5U)
						{
							const auto unit = text(*entry.row, "compile_unit"),
									   kind = text(*entry.row, "kind"),
									   target = text(*entry.row, "target_entity");
							charge(retained.bytes,
								   sizeof(edge_key) + unit.size() + kind.size() + target.size() +
									   id.size() + variant.size() +
									   entry.row->presence.universe.size() +
									   entry.row->interpretation.size() + 96U,
								   limits.maximum_retained_bytes,
								   "conditioned-edge-index");
							charge(retained.references,
								   1U,
								   limits.maximum_evidence_references,
								   "edge-index-references");
							edge_index[{{entry.row->presence.universe,
										 variant,
										 entry.row->interpretation},
										unit,
										id,
										kind,
										target}]
								.push_back(ref);
						}
					}
				}
				for (const auto& [identity, candidates] : index)
				{
					if (std::get<0>(identity) != 6U)
						continue;
					check(stop);
					const auto& condition = std::get<2>(identity);
					const auto lookup =
						[&](std::size_t group,
							const std::string& id) -> const std::vector<std::size_t>&
					{
						const auto found = index.find({group, id, condition});
						static const std::vector<std::size_t> empty;
						return found == index.end() ? empty : found->second;
					};
					std::map<std::string, std::vector<std::size_t>> payloads;
					std::set<std::string> type_payloads;
					for (const auto ref : candidates)
					{
						work();
						std::string payload;
						for (const auto& [name, value] : entries[ref].row->values)
							payload += name + ":" + value.canonical_form() + "\n";
						charge(retained.bytes,
							   payload.size() + 96U,
							   limits.maximum_retained_bytes,
							   "candidate-payloads");
						payloads[std::move(payload)].push_back(ref);
						std::string type_payload;
						for (const auto name : {"type_reference_count",
												"type_reference_state",
												"type_reference_profile",
												"type_reference_targets",
												"type_reference_reason"})
						{
							const auto canonical = cell(*entries[ref].row, name).canonical_form();
							charge(retained.bytes,
								   canonical.size() + 1U,
								   limits.maximum_retained_bytes,
								   "type-candidate-payloads");
							type_payload += canonical + "\n";
						}
						charge(retained.bytes,
							   96U,
							   limits.maximum_retained_bytes,
							   "type-candidate-payloads");
						type_payloads.insert(std::move(type_payload));
					}
					for (const auto& [payload, refs] : payloads)
					{
						check(stop);
						if (output.surfaces.size() >= limits.maximum_surfaces)
							fail("sdk.record-budget", "surfaces", "limit-exceeded");
						const auto& row = *entries[refs.front()].row;
						record_surface surface;
						surface.id = text(row, "surface");
						surface.entity = text(row, "entity");
						surface.compile_unit = text(row, "compile_unit");
						surface.source_span = text(row, "source");
						surface.profile = text(row, "profile");
						surface.universe = condition[0];
						surface.variant = condition[1];
						surface.interpretation = condition[2];
						surface.is_definition = scalar<bool>(row, "is_definition");
						surface.declared_methods = scalar<std::uint64_t>(row, "method_count");
						surface.declared_fields = scalar<std::uint64_t>(row, "field_count");
						surface.declared_base_specifiers =
							scalar<std::uint64_t>(row, "base_specifier_count");
						const auto state = text(row, "enumeration_state");
						if (state != "complete" && state != "partial" && state != "unknown")
							fail("sdk.record-input-invalid",
								 "enumeration_state",
								 "state-unsupported");
						surface.state = state == "complete" ? record_surface_state::complete
							: state == "unknown"			? record_surface_state::unknown
															: record_surface_state::partial;
						const auto retain = [&](std::vector<std::size_t>& target, std::size_t ref)
						{
							charge(retained.references,
								   1U,
								   limits.maximum_evidence_references,
								   "output-references");
							charge(retained.bytes,
								   sizeof(ref),
								   limits.maximum_retained_bytes,
								   "output-references");
							target.push_back(ref);
						};
						for (const auto ref : refs)
							retain(surface.evidence, ref);
						const auto gap =
							[&](std::string code, std::string detail, bool conflict = false)
						{
							charge(retained.bytes,
								   sizeof(query_unresolved) + code.size() + surface.id.size() +
									   detail.size(),
								   limits.maximum_retained_bytes,
								   "frontier-bytes");
							charge(retained.references,
								   1U,
								   limits.maximum_evidence_references,
								   "frontier-references");
							surface.gaps.push_back(
								{std::move(code), surface.id, std::move(detail)});
							if (conflict)
								surface.state = record_surface_state::conflicting;
							else if (surface.state == record_surface_state::complete)
								surface.state = record_surface_state::partial;
						};
						if (payloads.size() > 1U)
							gap("sdk.record-payload-conflicting",
								"retain-every-conditioned-candidate",
								true);
						if (!surface.is_definition && state == "complete")
							gap("sdk.record-definition-conflicting",
								"forward-record-cannot-enumerate-complete",
								true);
						if (surface.profile != "clang22-explicit-record-surface/1")
							gap("sdk.record-profile-unsupported", surface.profile);
						if (state != "complete")
							gap("sdk.record-enumeration-unavailable", text(row, "reason"));
						if (lookup(0U, surface.compile_unit).empty())
							gap("sdk.record-unit-missing", surface.compile_unit);
						else
							for (const auto ref : lookup(0U, surface.compile_unit))
								retain(surface.evidence, ref);
						const auto bind_entity =
							[&](const std::string& id, bool field, bool method) -> std::string
						{
							std::set<std::string> kinds;
							const auto& entities = lookup(3U, id);
							if (entities.empty())
								gap("sdk.record-entity-missing", id);
							for (const auto ref : entities)
							{
								work();
								const auto candidate = text(*entries[ref].row, "kind");
								kinds.insert(candidate);
								retain(surface.evidence, ref);
							}
							if (kinds.size() > 1U)
								gap("sdk.record-entity-kind-conflicting", id, true);
							const std::string kind =
								kinds.size() == 1U ? *kinds.begin() : std::string{};
							if ((!kind.empty() && field && kind != "field") ||
								(!kind.empty() && method && kind != "method" &&
								 kind != "constructor" && kind != "destructor" &&
								 kind != "conversion"))
								gap("sdk.record-member-kind-conflicting", id, true);
							return kind;
						};
						const auto record_kind = bind_entity(surface.entity, false, false);
						if (!record_kind.empty() && record_kind != "class" &&
							record_kind != "struct" && record_kind != "union")
							gap("sdk.record-entity-kind-conflicting", surface.entity, true);
						const auto bind_source = [&](const std::string& span_id)
						{
							bool found{};
							std::set<std::pair<std::string, std::string>> sources;
							const auto& spans = lookup(2U, span_id);
							for (const auto ref : spans)
							{
								work();
								const auto& span = *entries[ref].row;
								if (!same_payload(span, *entries[spans.front()].row))
									gap("sdk.record-source-conflicting", span_id, true);
								const auto file = text(span, "file"),
										   snapshot = text(span, "snapshot");
								charge(retained.bytes,
									   file.size() + snapshot.size() + 96U,
									   limits.maximum_retained_bytes,
									   "source-candidates");
								sources.emplace(file, snapshot);
								retain(surface.evidence, ref);
								const auto& files = lookup(1U, snapshot);
								for (const auto source_ref : files)
								{
									work();
									const auto& source = *entries[source_ref].row;
									if (!same_payload(source, *entries[files.front()].row))
										gap("sdk.record-source-conflicting", snapshot, true);
									const auto begin = scalar<std::uint64_t>(span, "begin"),
											   end = scalar<std::uint64_t>(span, "end");
									if (text(source, "file") != file || begin > end ||
										end > scalar<std::uint64_t>(source, "size"))
										gap("sdk.record-source-binding-conflicting", span_id, true);
									else
										found = true;
									retain(surface.evidence, source_ref);
								}
							}
							if (!found)
								gap("sdk.record-source-missing", span_id);
							if (sources.size() > 1U)
								gap("sdk.record-source-conflicting", span_id, true);
							return sources;
						};
						const auto source_candidates = bind_source(surface.source_span);
						if (source_candidates.size() == 1U)
						{
							surface.file = source_candidates.begin()->first;
							surface.source_snapshot = source_candidates.begin()->second;
						}
						bool declaration_found{};
						for (const auto ref : lookup(4U, surface.entity))
						{
							work();
							const auto& detail = *entries[ref].row;
							if (text(detail, "compile_unit") != surface.compile_unit ||
								text(detail, "source") != surface.source_span)
								continue;
							declaration_found = true;
							retain(surface.evidence, ref);
							if (scalar<bool>(detail, "is_definition") != surface.is_definition)
								gap("sdk.record-declaration-conflicting", surface.entity, true);
						}
						if (!declaration_found)
							gap("sdk.record-declaration-missing", surface.entity);
						const auto methods = strings(
							row, "methods", retained.bytes, retained.references, limits, stop);
						const auto fields = strings(
							row, "fields", retained.bytes, retained.references, limits, stop);
						surface.base_targets = strings(
							row, "base_targets", retained.bytes, retained.references, limits, stop);
						const auto type_gap =
							[&](std::string code, std::string detail, bool conflict = false)
						{
							charge(retained.bytes,
								   sizeof(query_unresolved) + code.size() + surface.id.size() +
									   detail.size(),
								   limits.maximum_retained_bytes,
								   "type-reference-frontier");
							charge(retained.references,
								   1U,
								   limits.maximum_evidence_references,
								   "type-reference-frontier");
							surface.gaps.push_back({std::move(code), surface.id, detail});
							surface.type_reference_reason = std::move(detail);
							if (conflict)
								surface.type_reference_state = record_surface_state::conflicting;
							else if (surface.type_reference_state == record_surface_state::complete)
								surface.type_reference_state = record_surface_state::partial;
						};
						bool any_type_property{}, all_type_properties = true;
						for (const auto name : {"type_reference_count",
												"type_reference_state",
												"type_reference_profile",
												"type_reference_targets"})
						{
							const bool present = cell(row, name).state == cell_state::present;
							any_type_property |= present;
							all_type_properties &= present;
						}
						if ((any_type_property && !all_type_properties) ||
							(!any_type_property &&
							 cell(row, "type_reference_reason").state == cell_state::present))
							fail("sdk.record-input-invalid",
								 "type_references",
								 "incomplete-enumeration-payload");
						if (all_type_properties)
						{
							const auto type_state = text(row, "type_reference_state");
							if (type_state != "complete" && type_state != "partial" &&
								type_state != "unknown")
								fail("sdk.record-input-invalid",
									 "type_reference_state",
									 "state-unsupported");
							surface.declared_type_references =
								scalar<std::uint64_t>(row, "type_reference_count");
							surface.type_reference_state = type_state == "complete"
								? record_surface_state::complete
								: type_state == "partial" ? record_surface_state::partial
														  : record_surface_state::unknown;
							surface.type_reference_profile = text(row, "type_reference_profile");
							surface.type_reference_reason = text(row, "type_reference_reason");
							surface.type_reference_targets = strings(row,
																	 "type_reference_targets",
																	 retained.bytes,
																	 retained.references,
																	 limits,
																	 stop);
							if (type_payloads.size() > 1U)
								type_gap("sdk.record-type-reference-candidates-conflicting",
										 "retain-every-conditioned-type-reference-candidate",
										 true);
							if (*surface.declared_type_references !=
									surface.type_reference_targets.size() ||
								(!surface.is_definition && type_state == "complete"))
								type_gap("sdk.record-type-reference-conflicting",
										 "invalid-local-cardinality-or-definition",
										 true);
							if (surface.type_reference_profile !=
								"clang22-explicit-nonsystem-nominal-type-uses/1")
							{
								type_gap("sdk.record-type-reference-profile-unsupported",
										 surface.type_reference_profile);
								if (surface.type_reference_state !=
									record_surface_state::conflicting)
									surface.type_reference_state = record_surface_state::unknown;
							}
							for (const auto& target : surface.type_reference_targets)
							{
								work();
								std::set<std::string> kinds;
								for (const auto ref : lookup(3U, target))
								{
									work();
									auto kind = text(*entries[ref].row, "kind");
									charge(retained.bytes,
										   kind.size() + 96U,
										   limits.maximum_retained_bytes,
										   "type-reference-kinds");
									kinds.insert(std::move(kind));
									retain(surface.evidence, ref);
								}
								if (kinds.empty())
									type_gap("sdk.record-type-reference-missing", target);
								else if (kinds.size() != 1U ||
										 (*kinds.begin() != "class" && *kinds.begin() != "struct" &&
										  *kinds.begin() != "union" && *kinds.begin() != "enum"))
									type_gap(
										"sdk.record-type-reference-kind-conflicting", target, true);
								bool referenced{};
								const auto edges = edge_index.find({condition,
																	surface.compile_unit,
																	surface.entity,
																	"uses_type",
																	target});
								if (edges != edge_index.end())
									for (const auto ref : edges->second)
									{
										work();
										if (text(*entries[ref].row, "resolution") == "resolved" &&
											text(*entries[ref].row, "source") ==
												surface.source_span)
										{
											referenced = true;
											retain(surface.evidence, ref);
										}
									}
								if (!referenced)
									type_gap("sdk.record-type-reference-edge-missing", target);
							}
							if (surface.type_reference_state != record_surface_state::complete &&
								surface.type_reference_reason.empty())
								surface.type_reference_reason =
									"type-reference-enumeration-unavailable";
						}
						else
							surface.type_reference_reason =
								"producer-type-reference-enumeration-unavailable";
						if (methods.size() > surface.declared_methods ||
							fields.size() > surface.declared_fields ||
							surface.base_targets.size() > surface.declared_base_specifiers ||
							(state == "complete" &&
							 (methods.size() != surface.declared_methods ||
							  fields.size() != surface.declared_fields ||
							  surface.base_targets.size() != surface.declared_base_specifiers)))
							gap("sdk.record-cardinality-conflicting",
								"declared-and-observed-members-differ",
								true);
						if (surface.is_definition)
							for (const auto ref : lookup(5U, surface.entity))
							{
								work();
								const auto& edge = *entries[ref].row;
								if (text(edge, "compile_unit") != surface.compile_unit)
									continue;
								const auto target = text(edge, "target_entity");
								const auto edge_kind = text(edge, "kind");
								if (all_type_properties &&
									surface.type_reference_state ==
										record_surface_state::complete &&
									edge_kind == "uses_type" &&
									text(edge, "source") == surface.source_span &&
									target != surface.entity &&
									!std::ranges::binary_search(surface.type_reference_targets,
																target))
									type_gap("sdk.record-type-reference-unlisted", target, true);
								if (edge_kind == "inherits" &&
									!std::ranges::binary_search(surface.base_targets, target))
									gap("sdk.record-unlisted-base-conflicting", target, true);
								if (edge_kind != "owns")
									continue;
								for (const auto entity_ref : lookup(3U, target))
								{
									work();
									const auto kind = text(*entries[entity_ref].row, "kind");
									const bool method = kind == "method" || kind == "constructor" ||
										kind == "destructor" || kind == "conversion";
									if ((kind == "field" &&
										 !std::ranges::binary_search(fields, target)) ||
										(method && !std::ranges::binary_search(methods, target)))
										gap("sdk.record-unlisted-member-conflicting", target, true);
								}
							}
						for (const auto& [ids, target, field] :
							 std::array{std::tuple{&methods, &surface.methods, false},
										std::tuple{&fields, &surface.fields, true}})
							for (const auto& id : *ids)
							{
								check(stop);
								charge(members, 1U, limits.maximum_members, "members");
								record_surface_member member;
								member.entity = id;
								member.kind = bind_entity(id, field, !field);
								std::set<
									std::tuple<std::string, std::string, std::vector<std::string>>>
									detail_payloads;
								for (const auto ref : lookup(4U, id))
								{
									work();
									const auto& detail = *entries[ref].row;
									if (text(detail, "compile_unit") != surface.compile_unit)
										continue;
									(void)bind_source(text(detail, "source"));
									auto flags = strings(detail,
														 "flags",
														 retained.bytes,
														 retained.references,
														 limits,
														 stop);
									const auto access = text(detail, "access"),
											   type = text(detail, "canonical_type");
									auto semantic_flags = flags;
									std::erase(semantic_flags, "inline");
									charge(retained.bytes,
										   access.size() + type.size() +
											   flags.size() * sizeof(std::string) +
											   cell(detail, "flags").canonical_form().size() * 2U +
											   96U,
										   limits.maximum_retained_bytes,
										   "member-candidates");
									detail_payloads.emplace(
										access, type, std::move(semantic_flags));
									if (detail_payloads.size() == 1U)
									{
										member.access = access;
										member.canonical_type = type;
										member.flags.insert(
											member.flags.end(), flags.begin(), flags.end());
									}
									retain(member.evidence, ref);
									retain(surface.evidence, ref);
								}
								if (detail_payloads.empty())
									gap("sdk.record-member-detail-missing", id);
								std::ranges::sort(member.flags);
								member.flags.erase(std::ranges::unique(member.flags).begin(),
												   member.flags.end());
								if (detail_payloads.size() > 1U)
								{
									gap("sdk.record-member-detail-conflicting", id, true);
									member.access.clear();
									member.canonical_type.clear();
									member.flags.clear();
								}
								bool owned{};
								const auto ownership = edge_index.find(
									{condition, surface.compile_unit, surface.entity, "owns", id});
								if (ownership != edge_index.end())
									for (const auto ref : ownership->second)
									{
										work();
										if (text(*entries[ref].row, "resolution") == "resolved")
										{
											owned = true;
											retain(member.evidence, ref);
											retain(surface.evidence, ref);
										}
									}
								if (!owned)
									gap("sdk.record-member-owner-missing", id);
								charge(retained.bytes,
									   sizeof(member) + id.size() * 2U + member.access.size() +
										   member.kind.size() + member.canonical_type.size(),
									   limits.maximum_retained_bytes,
									   "member-output");
								target->push_back(std::move(member));
							}
						for (const auto& base : surface.base_targets)
						{
							check(stop);
							charge(members, 1U, limits.maximum_members, "base-members");
							const auto kind = bind_entity(base, false, false);
							if (!kind.empty() && kind != "class" && kind != "struct" &&
								kind != "union")
								gap("sdk.record-base-kind-conflicting", base, true);
							bool inherited{};
							const auto inheritance = edge_index.find({condition,
																	  surface.compile_unit,
																	  surface.entity,
																	  "inherits",
																	  base});
							if (inheritance != edge_index.end())
								for (const auto ref : inheritance->second)
								{
									work();
									if (text(*entries[ref].row, "resolution") == "resolved")
									{
										inherited = true;
										retain(surface.evidence, ref);
									}
								}
							if (!inherited)
								gap("sdk.record-base-edge-missing", base);
						}
						std::ranges::sort(surface.evidence);
						surface.evidence.erase(std::ranges::unique(surface.evidence).begin(),
											   surface.evidence.end());
						normalize(surface.gaps);
						charge(retained.bytes,
							   sizeof(surface) + surface.id.size() + surface.entity.size() +
								   surface.compile_unit.size() + surface.source_span.size() +
								   surface.file.size() + surface.source_snapshot.size() +
								   surface.profile.size() + surface.universe.size() +
								   surface.variant.size() + surface.interpretation.size() +
								   surface.type_reference_profile.size() +
								   surface.type_reference_reason.size(),
							   limits.maximum_retained_bytes,
							   "surface-output");
						output.surfaces.push_back(std::move(surface));
					}
				}
				if (!input.inventories.empty())
				{
					using unit_key = std::pair<world, std::string>;
					using inventory_key = std::tuple<world, std::string, std::string>;
					std::map<unit_key, std::set<std::string>> definitions;
					std::map<unit_key, std::set<std::array<std::string, 2>>> definition_sources;
					std::map<unit_key,
							 std::map<std::array<std::string, 2>, std::vector<std::size_t>>>
						declaration_definitions;
					std::map<unit_key, std::vector<std::size_t>> uncertain_definitions;
					std::map<inventory_key, std::vector<std::size_t>> inventories;
					for (const auto& [identity, refs] : index)
					{
						const auto group = std::get<0>(identity);
						if (group != 4U && group != 6U && group != 7U)
							continue;
						const auto& condition = std::get<2>(identity);
						for (const auto ref : refs)
						{
							work();
							const auto& row = *entries[ref].row;
							const auto unit = text(row, "compile_unit");
							charge(retained.bytes,
								   sizeof(inventory_key) + 128U + unit.size() +
									   condition[0].size() + condition[1].size() +
									   condition[2].size() + std::get<1>(identity).size() +
									   (group == 4U ? text(row, "source").size() +
												text(row, "entity").size()
													: text(row, "profile").size()),
								   limits.maximum_retained_bytes,
								   "inventory-index");
							charge(retained.references,
								   1U,
								   limits.maximum_evidence_references,
								   "inventory-index-references");
							if (group == 6U && scalar<bool>(row, "is_definition"))
							{
								charge(retained.bytes,
									   text(row, "entity").size() + text(row, "source").size() +
										   sizeof(unit_key) + 192U + unit.size() +
										   condition[0].size() + condition[1].size() +
										   condition[2].size(),
									   limits.maximum_retained_bytes,
									   "inventory-definition-sources");
								definitions[{condition, unit}].insert(text(row, "surface"));
								definition_sources[{condition, unit}].insert(
									{text(row, "entity"), text(row, "source")});
							}
							else if (group == 4U && scalar<bool>(row, "is_definition"))
							{
								const auto entity = text(row, "entity");
								const auto found = index.find({3U, entity, condition});
								bool record{}, uncertain = found == index.end();
								if (found != index.end())
									for (const auto entity_ref : found->second)
									{
										work();
										const auto kind = text(*entries[entity_ref].row, "kind");
										const bool is_record =
											kind == "class" || kind == "struct" || kind == "union";
										record |= is_record;
										constexpr std::array<std::string_view, 19> nonrecords{
											"function",
											"method",
											"constructor",
											"destructor",
											"conversion",
											"namespace",
											"namespace_alias",
											"enum",
											"enum_constant",
											"alias",
											"field",
											"parameter",
											"variable",
											"concept",
											"template_type_parameter",
											"template_non_type_parameter",
											"template_template_parameter",
											"template",
											"declaration"};
										uncertain |= !is_record &&
											std::ranges::find(nonrecords, kind) == nonrecords.end();
									}
								if (record)
									declaration_definitions[{condition, unit}]
														   [{entity, text(row, "source")}]
															   .push_back(ref);
								if (uncertain)
								{
									charge(retained.bytes,
										   sizeof(unit_key) + 128U + unit.size() +
											   condition[0].size() + condition[1].size() +
											   condition[2].size(),
										   limits.maximum_retained_bytes,
										   "inventory-uncertain-definitions");
									charge(retained.references,
										   1U,
										   limits.maximum_evidence_references,
										   "inventory-uncertain-references");
									uncertain_definitions[{condition, unit}].push_back(ref);
								}
							}
							else if (group == 7U)
								inventories[{condition, unit, text(row, "profile")}].push_back(ref);
						}
					}
					for (const auto& [identity, refs] : inventories)
					{
						const auto& condition = std::get<0>(identity);
						const auto& unit = std::get<1>(identity);
						std::map<std::string, std::vector<std::size_t>> payloads;
						for (const auto ref : refs)
						{
							work();
							std::string payload;
							for (const auto& [name, value] : entries[ref].row->values)
							{
								const auto part = name + ":" + value.canonical_form() + "\n";
								charge(retained.bytes,
									   part.size(),
									   limits.maximum_retained_bytes,
									   "inventory-payloads");
								payload += part;
							}
							charge(retained.bytes,
								   96U,
								   limits.maximum_retained_bytes,
								   "inventory-payloads");
							payloads[std::move(payload)].push_back(ref);
						}
						for (const auto& [payload, candidates] : payloads)
						{
							work();
							if (output.inventories.size() >= limits.maximum_inventories)
								fail("sdk.record-budget", "inventories", "limit-exceeded");
							const auto& row = *entries[candidates.front()].row;
							record_inventory inventory;
							inventory.id = text(row, "inventory");
							inventory.compile_unit = unit;
							inventory.profile = text(row, "profile");
							inventory.universe = condition[0];
							inventory.variant = condition[1];
							inventory.interpretation = condition[2];
							inventory.declared_definitions =
								scalar<std::uint64_t>(row, "definition_count");
							const auto state = text(row, "enumeration_state");
							inventory.state = state == "complete" ? record_surface_state::complete
								: state == "partial"			  ? record_surface_state::partial
																  : record_surface_state::unknown;
							const auto gap =
								[&](std::string code, std::string subject, bool conflict = false)
							{
								if (conflict)
									inventory.state = record_surface_state::conflicting;
								else if (inventory.state == record_surface_state::complete)
									inventory.state = record_surface_state::partial;
								charge(retained.bytes,
									   sizeof(query_unresolved) + code.size() + subject.size() +
										   inventory.id.size(),
									   limits.maximum_retained_bytes,
									   "inventory-gaps");
								inventory.gaps.push_back(
									{std::move(code), inventory.id, std::move(subject)});
							};
							const auto retain = [&](std::size_t ref)
							{
								charge(retained.references,
									   1U,
									   limits.maximum_evidence_references,
									   "inventory-evidence");
								charge(retained.bytes,
									   sizeof(ref),
									   limits.maximum_retained_bytes,
									   "inventory-evidence");
								inventory.evidence.push_back(ref);
							};
							const auto lookup =
								[&](std::size_t group,
									const std::string& id) -> const std::vector<std::size_t>&
							{
								const auto found = index.find({group, id, condition});
								static const std::vector<std::size_t> empty;
								return found == index.end() ? empty : found->second;
							};
							const auto bind = [&](std::size_t group,
												  const std::string& id,
												  std::string_view kind) -> const annotated_row*
							{
								const auto& bound = lookup(group, id);
								if (bound.empty())
								{
									gap("sdk.record-inventory-binding-missing",
										std::string{kind} + ":" + id);
									return nullptr;
								}
								for (const auto ref : bound)
								{
									work();
									retain(ref);
									if (!same_payload(*entries[bound.front()].row,
													  *entries[ref].row))
									{
										gap("sdk.record-inventory-binding-conflicting",
											std::string{kind} + ":" + id,
											true);
										return nullptr;
									}
								}
								return entries[bound.front()].row;
							};
							for (const auto ref : candidates)
								retain(ref);
							(void)bind(0U, unit, "compile-unit");
							inventory.definitions = strings(row,
															"definitions",
															retained.bytes,
															retained.references,
															limits,
															stop);
							inventory.system_definitions = strings(row,
																   "system_definitions",
																   retained.bytes,
																   retained.references,
																   limits,
																   stop);
							charge(members,
								   inventory.definitions.size() +
									   inventory.system_definitions.size(),
								   limits.maximum_members,
								   "inventory-definitions");
							if (payloads.size() > 1U)
								gap("sdk.record-inventory-candidate-conflicting", unit, true);
							if (inventory.declared_definitions < inventory.definitions.size() ||
								(state == "complete" &&
								 inventory.declared_definitions != inventory.definitions.size()))
								gap("sdk.record-inventory-cardinality-conflicting", unit, true);
							if (!std::ranges::includes(inventory.definitions,
													   inventory.system_definitions))
								gap("sdk.record-inventory-system-subset-conflicting", unit, true);
							if (const auto found = declaration_definitions.find({condition, unit});
								found != declaration_definitions.end())
							{
								if (inventory.declared_definitions < found->second.size())
									gap("sdk.record-inventory-cardinality-conflicting", unit, true);
								const auto observed = definition_sources.find({condition, unit});
								for (const auto& [source, evidence] : found->second)
								{
									work();
									for (const auto ref : evidence)
										retain(ref);
									if (observed == definition_sources.end() ||
										!observed->second.contains(source))
										gap("sdk.record-inventory-declared-definition-missing",
											source[0] + ":" + source[1]);
								}
							}
							if (const auto found = uncertain_definitions.find({condition, unit});
								found != uncertain_definitions.end())
								for (const auto ref : found->second)
								{
									work();
									retain(ref);
									gap("sdk.record-inventory-definition-kind-unavailable",
										text(*entries[ref].row, "entity"));
								}
							if (const auto found = definitions.find({condition, unit});
								found != definitions.end())
							{
								if (inventory.declared_definitions < found->second.size())
									gap("sdk.record-inventory-cardinality-conflicting", unit, true);
								for (const auto& id : found->second)
								{
									work();
									if (!std::ranges::binary_search(inventory.definitions, id))
										gap("sdk.record-inventory-definition-unlisted",
											id,
											state == "complete");
								}
							}
							for (const auto& id : inventory.definitions)
							{
								work();
								const auto& surfaces = lookup(6U, id);
								if (surfaces.empty())
									gap("sdk.record-inventory-definition-missing", id);
								std::optional<std::array<std::string, 3>> identity_fields;
								for (const auto ref : surfaces)
								{
									work();
									retain(ref);
									const auto& surface = *entries[ref].row;
									const auto entity = text(surface, "entity"),
											   source = text(surface, "source");
									const std::array actual_identity{
										entity, source, text(surface, "profile")};
									if (text(surface, "compile_unit") != unit ||
										!scalar<bool>(surface, "is_definition") ||
										(identity_fields && *identity_fields != actual_identity))
										gap("sdk.record-inventory-definition-conflicting",
											id,
											true);
									identity_fields = actual_identity;
									const auto& entities = lookup(3U, entity);
									if (entities.empty())
										gap("sdk.record-inventory-entity-missing", entity);
									for (const auto entity_ref : entities)
									{
										work();
										retain(entity_ref);
										const auto kind = text(*entries[entity_ref].row, "kind");
										if (kind != "class" && kind != "struct" && kind != "union")
											gap("sdk.record-inventory-entity-conflicting",
												entity,
												true);
									}
									if (const auto* span = bind(2U, source, "source-span"))
										if (const auto* file = bind(
												1U, text(*span, "snapshot"), "source-snapshot"))
											if (text(*span, "file") != text(*file, "file") ||
												scalar<std::uint64_t>(*span, "begin") >
													scalar<std::uint64_t>(*span, "end") ||
												scalar<std::uint64_t>(*span, "end") >
													scalar<std::uint64_t>(*file, "size"))
												gap("sdk.record-inventory-source-conflicting",
													source,
													true);
									bool declared{};
									for (const auto detail_ref : lookup(4U, entity))
									{
										work();
										const auto& detail = *entries[detail_ref].row;
										if (text(detail, "compile_unit") != unit ||
											text(detail, "source") != source)
											continue;
										retain(detail_ref);
										declared = true;
										if (!scalar<bool>(detail, "is_definition"))
											gap("sdk.record-inventory-declaration-conflicting",
												entity,
												true);
									}
									if (!declared)
										gap("sdk.record-inventory-declaration-missing", entity);
								}
							}
							if (inventory.profile !=
								"clang22-explicit-admitted-record-definitions/1")
							{
								if (inventory.state != record_surface_state::conflicting)
									inventory.state = record_surface_state::unknown;
								gap("sdk.record-inventory-profile-unsupported", inventory.profile);
							}
							if (state != "complete" && state != "partial" && state != "unknown")
								gap("sdk.record-inventory-state-unsupported", state);
							if (const auto reason = text(row, "reason"); !reason.empty())
								gap("sdk.record-inventory-producer-frontier", reason);
							std::ranges::sort(inventory.evidence);
							inventory.evidence.erase(
								std::ranges::unique(inventory.evidence).begin(),
								inventory.evidence.end());
							normalize(inventory.gaps);
							charge(retained.bytes,
								   sizeof(inventory) + inventory.id.size() + unit.size() +
									   inventory.profile.size() + condition[0].size() +
									   condition[1].size() + condition[2].size(),
								   limits.maximum_retained_bytes,
								   "inventory-output");
							output.inventories.push_back(std::move(inventory));
						}
					}
				}
				if (!input.observations_complete)
					output.unresolved.push_back({"sdk.record-observations-partial",
												 "projection",
												 "local-enumeration-does-not-close-project"});
				return output;
			}
			catch (const failure& value)
			{
				return value.value;
			}
			catch (const std::bad_alloc&)
			{
				return error{"sdk.record-resource-exhausted", "projection", "allocation"};
			}
			catch (const std::length_error&)
			{
				return error{"sdk.record-resource-exhausted", "projection", "length"};
			}
		}
	} // namespace

	result<void> record_surface_limits::validate() const
	{
		if (!maximum_rows || !maximum_condition_expansions || !maximum_evidence_bytes ||
			!maximum_retained_bytes || !maximum_evidence_references || !maximum_surfaces ||
			!maximum_inventories || !maximum_members || !maximum_operations ||
			!maximum_source_queries || !maximum_source_plan_bytes)
			return error{"sdk.record-limit-invalid", "limits", "positive-required"};
		return {};
	}
	result<record_surface_projection> project_record_surfaces(record_surface_input input,
															  record_surface_limits limits,
															  std::stop_token stop)
	{
		budget retained;
		return project_rows(input, limits, stop, retained);
	}
	result<record_surface_projection> project_record_surfaces(
		const application_query_results& input, record_surface_limits limits, std::stop_token stop)
	{
		if (auto valid = limits.validate(); !valid)
			return valid.error();
		try
		{
			check(stop);
			if (auto valid = detail::check_source_plan_limits(input,
															  limits.maximum_source_queries,
															  limits.maximum_source_plan_bytes,
															  stop,
															  "sdk.record");
				!valid)
				return valid.error();
			budget retained;
			std::array<std::vector<annotated_row>, 8> groups;
			std::array<bool, 8> present{};
			std::array<bool, 8> scan_complete{};
			scan_complete.fill(true);
			std::size_t rows{};
			bool complete = true;
			for (const auto& scan : input.scans)
			{
				check(stop);
				const auto found = std::ranges::find(relations, scan.relation_id);
				if (found == relations.end())
					continue;
				const auto group = static_cast<std::size_t>(found - relations.begin());
				present[group] = true;
				const auto current_complete =
					scan.result.execution() == execution_status::complete &&
					scan.result.inputs_complete() && scan.result.conflicts().empty() &&
					scan.result.differential_disagreements().empty();
				scan_complete[group] &= current_complete;
				if (group != 7U)
					complete &= current_complete;
				auto cursor = scan.result.rows();
				while (true)
				{
					check(stop);
					auto next = cursor.next();
					if (!next)
						return next.error();
					if (!*next)
						break;
					charge(rows, 1U, limits.maximum_rows, "rows");
					auto row = (*next)->copy();
					if (!row)
						return row.error();
					charge(retained.evidence,
						   row->canonical_form().size(),
						   limits.maximum_evidence_bytes,
						   "detached-scan-evidence");
					groups[group].push_back(std::move(*row));
				}
			}
			complete &= std::ranges::all_of(std::span{present}.first(7U),
											[](bool value)
											{
												return value;
											});
			auto result = project_rows({groups[0],
										groups[1],
										groups[2],
										groups[3],
										groups[4],
										groups[5],
										groups[6],
										complete,
										groups[7],
										present[0] && scan_complete[0],
										present[7] && scan_complete[7]},
									   limits,
									   stop,
									   retained);
			if (!result)
				return result.error();
			for (std::size_t group{}; group < present.size(); ++group)
				if (!present[group])
					result->unresolved.push_back({"sdk.record-scan-missing",
												  std::string{relations[group]},
												  "independent-scan-unavailable"});
			normalize(result->unresolved);
			result->source_queries = input;
			return result;
		}
		catch (const failure& value)
		{
			return value.value;
		}
		catch (const std::bad_alloc&)
		{
			return error{"sdk.record-resource-exhausted", "projection", "allocation"};
		}
		catch (const std::length_error&)
		{
			return error{"sdk.record-resource-exhausted", "projection", "length"};
		}
	}
} // namespace cxxlens::sdk::query
