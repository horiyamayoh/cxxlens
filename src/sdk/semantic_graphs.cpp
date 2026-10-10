#include <algorithm>
#include <array>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

#include <cxxlens/sdk/semantic_graphs.hpp>

#include "query_projection_plan_limits_internal.hpp"

namespace cxxlens::sdk::query
{
	namespace
	{
		constexpr std::array<std::string_view, 7U> relation_ids{"build.compile_unit.v1",
																"source.file.v1",
																"cc.entity.v1",
																"cc.entity_edge.v1",
																"cc.call_site.v1",
																"cc.call_direct_target.v1",
																"source.include.v1"};
		using world = std::array<std::string, 3U>;
		struct failure
		{
			error value;
		};
		[[noreturn]] void fail(std::string code, std::string field, std::string detail)
		{
			throw failure{{std::move(code), std::move(field), std::move(detail)}};
		}
		void cancelled(std::stop_token stop)
		{
			if (stop.stop_requested())
				fail("sdk.graph-cancelled", "projection", "stop-requested");
		}
		void
		add(std::size_t& target, std::size_t amount, std::size_t maximum, std::string_view field)
		{
			if (target > maximum || amount > maximum - target)
				fail("sdk.graph-budget", std::string{field}, "limit-exceeded");
			target += amount;
		}
		std::string_view text(const annotated_row& row, std::string_view name)
		{
			const auto found = row.values.find("output." + std::string{name});
			if (found == row.values.end() || found->second.state != cell_state::present ||
				!found->second.value)
				return {};
			const auto* value = std::get_if<std::string>(&*found->second.value);
			return value ? std::string_view{*value} : std::string_view{};
		}
		bool safe(std::string_view value)
		{
			return !value.empty() && value.size() <= 4096U &&
				validate_utf8_text(value).has_value() &&
				std::ranges::none_of(value,
									 [](unsigned char c)
									 {
										 return c < 32U || c == 127U;
									 });
		}
		template <class T>
		void unique(std::vector<T>& values)
		{
			std::ranges::sort(values);
			values.erase(std::ranges::unique(values).begin(), values.end());
		}
		bool selects(const std::vector<std::string>& values, std::string_view value)
		{
			return std::ranges::binary_search(values, value);
		}
		void normalize(std::vector<query_unresolved>& gaps)
		{
			std::ranges::sort(gaps,
							  {},
							  [](const auto& g)
							  {
								  return std::tie(g.code, g.subject, g.detail);
							  });
			gaps.erase(std::ranges::unique(gaps).begin(), gaps.end());
		}
		std::array<bool, 7U> needed(const semantic_graph_spec& spec)
		{
			std::array<bool, 7U> result{};
			result[0] = true;
			result[1] = spec.file_nodes;
			result[2] = !spec.node_kinds.empty();
			for (const auto& kind : spec.edge_kinds)
				if (kind == "calls")
					result[2] = result[4] = result[5] = true;
				else if (kind == "includes")
					result[1] = result[6] = true;
				else
					result[2] = result[3] = true;
			return result;
		}
		struct entry
		{
			std::size_t group{};
			const annotated_row* row{};
			std::string canonical, payload;
		};
		std::string payload(const annotated_row& row)
		{
			std::string result;
			for (const auto& [name, cell] : row.values)
				result += name + '=' + cell.canonical_form() + '\n';
			return result;
		}
		void validate_column_types(const annotated_row& row, std::size_t group)
		{
			const auto descriptors = standard_relation_descriptors();
			const auto descriptor =
				std::ranges::find(descriptors, relation_ids[group], &relation_descriptor::id);
			if (descriptor == descriptors.end())
				fail("sdk.graph-input-invalid",
					 std::string{relation_ids[group]},
					 "registry-descriptor-missing");
			for (const auto& column : descriptor->columns)
			{
				const auto found = row.values.find("output." + column.name);
				if (found != row.values.end() &&
					(found->second.type.scalar != column.type.scalar ||
					 found->second.type.parameter != column.type.parameter))
					fail("sdk.graph-input-invalid", column.id, "column-type-mismatch");
			}
		}
		struct candidate
		{
			std::size_t first{};
			std::vector<std::size_t> evidence;
			std::optional<std::size_t> output_index;
		};
		using candidates = std::map<std::string_view, candidate, std::less<>>;
		struct retention_budget
		{
			std::size_t evidence_bytes{}, expanded_bytes{}, references{};
		};
	} // namespace

	result<void> semantic_graph_limits::validate() const
	{
		if (!maximum_rows || !maximum_condition_expansions || !maximum_evidence_bytes ||
			!maximum_expanded_row_bytes || !maximum_evidence_references || !maximum_specs ||
			!maximum_graphs || !maximum_graph_nodes || !maximum_graph_edges ||
			!maximum_source_queries || !maximum_source_plan_bytes)
			return error{"sdk.graph-limit-invalid", "limits", "positive-required"};
		return {};
	}

	namespace
	{
		result<semantic_graph_projection>
		project_rows(semantic_graph_input input,
					 std::span<const semantic_graph_spec> requested,
					 semantic_graph_limits limits,
					 std::stop_token stop,
					 retention_budget& retained)
		{
			if (auto valid = limits.validate(); !valid)
				return valid.error();
			try
			{
				cancelled(stop);
				if (requested.empty())
					fail("sdk.graph-spec-invalid", "specs", "nonempty-required");
				if (requested.size() > limits.maximum_specs)
					fail("sdk.graph-budget", "specs", "limit-exceeded");
				auto& bytes = retained.evidence_bytes;
				auto& expanded_bytes = retained.expanded_bytes;
				auto& references = retained.references;
				std::size_t expansions{}, graph_count{};
				std::vector<semantic_graph_spec> specs;
				std::set<std::string, std::less<>> spec_ids;
				for (const auto& original : requested)
				{
					cancelled(stop);
					if (!safe(original.id) || !spec_ids.insert(original.id).second ||
						(!original.file_nodes && original.node_kinds.empty()) ||
						original.edge_kinds.empty())
						fail("sdk.graph-spec-invalid",
							 original.id,
							 "invalid-or-duplicate-projection");
					std::size_t amount = original.id.size() + sizeof(semantic_graph_spec);
					for (const auto* kinds : {&original.node_kinds, &original.edge_kinds})
						for (const auto& kind : *kinds)
						{
							if (!safe(kind))
								fail("sdk.graph-spec-invalid", original.id, "invalid-kind");
							add(amount,
								kind.size() + sizeof(std::string),
								limits.maximum_evidence_bytes,
								"spec-bytes");
						}
					add(bytes, amount, limits.maximum_evidence_bytes, "spec-bytes");
					auto spec = original;
					unique(spec.node_kinds);
					unique(spec.edge_kinds);
					if (selects(spec.edge_kinds, "includes") && !spec.file_nodes)
						fail("sdk.graph-spec-invalid", spec.id, "includes-requires-file-nodes");
					specs.push_back(std::move(spec));
				}
				std::ranges::sort(specs, {}, &semantic_graph_spec::id);
				const std::array groups{input.units,
										input.files,
										input.entities,
										input.entity_edges,
										input.call_sites,
										input.call_targets,
										input.includes};
				std::vector<entry> entries;
				for (std::size_t group{}; group < groups.size(); ++group)
					for (const auto& row : groups[group])
					{
						cancelled(stop);
						if (entries.size() >= limits.maximum_rows)
							fail("sdk.graph-budget", "rows", "limit-exceeded");
						if (auto valid = row.validate(); !valid)
							fail(
								"sdk.graph-input-invalid", valid.error().field, valid.error().code);
						validate_column_types(row, group);
						auto canonical = row.canonical_form();
						auto columns = payload(row);
						add(bytes,
							canonical.size(),
							limits.maximum_evidence_bytes,
							"evidence-bytes");
						add(bytes,
							canonical.size(),
							limits.maximum_evidence_bytes,
							"owned-evidence-bytes");
						add(bytes, columns.size(), limits.maximum_evidence_bytes, "payload-bytes");
						add(expansions,
							row.presence.fragments.size(),
							limits.maximum_condition_expansions,
							"conditions");
						for (const auto& variant : row.presence.fragments)
						{
							(void)variant;
							add(expanded_bytes,
								canonical.size(),
								limits.maximum_expanded_row_bytes,
								"condition-bytes");
						}
						entries.push_back({group, &row, std::move(canonical), std::move(columns)});
					}
				std::ranges::sort(entries,
								  {},
								  [](const auto& value)
								  {
									  return std::tie(value.group, value.canonical);
								  });
				semantic_graph_projection output;
				output.evidence.reserve(entries.size());
				std::map<world, std::array<std::vector<std::size_t>, 7U>> worlds;
				for (std::size_t index{}; index < entries.size(); ++index)
				{
					cancelled(stop);
					const auto& item = entries[index];
					output.evidence.push_back({std::string{relation_ids[item.group]}, *item.row});
					for (const auto& variant : item.row->presence.fragments)
					{
						cancelled(stop);
						worlds[{item.row->presence.universe, variant, item.row->interpretation}]
							  [item.group]
								  .push_back(index);
					}
				}
				for (const auto& [condition, rows] : worlds)
				{
					cancelled(stop);
					std::map<std::string, candidates, std::less<>> targets, sites;
					for (const auto group : {4U, 5U})
						for (const auto index : rows[group])
						{
							cancelled(stop);
							add(references,
								1U,
								limits.maximum_evidence_references,
								"call-index-references");
							auto& values =
								(group == 4U
									 ? sites
									 : targets)[std::string{text(*entries[index].row, "call")}];
							const auto [position, inserted] = values.try_emplace(
								entries[index].payload, candidate{index, {}, {}});
							(void)inserted;
							position->second.evidence.push_back(index);
						}
					for (const auto& spec : specs)
					{
						cancelled(stop);
						add(graph_count, 1U, limits.maximum_graphs, "graphs");
						std::size_t spec_bytes = spec.id.size() + sizeof(semantic_graph);
						for (const auto* kinds : {&spec.node_kinds, &spec.edge_kinds})
							for (const auto& kind : *kinds)
								spec_bytes += kind.size() + sizeof(std::string);
						for (const auto& field : condition)
							spec_bytes += field.size();
						add(expanded_bytes,
							spec_bytes,
							limits.maximum_expanded_row_bytes,
							"graph-spec-bytes");
						semantic_graph graph;
						graph.spec = spec;
						graph.universe = condition[0];
						graph.variant = condition[1];
						graph.interpretation = condition[2];
						graph.state = input.observations_complete ? semantic_graph_state::complete
																  : semantic_graph_state::partial;
						const auto gap = [&](std::string code,
											 std::string subject,
											 std::string detail = {},
											 bool conflict = false)
						{
							add(references,
								1U,
								limits.maximum_evidence_references,
								"gap-references");
							add(expanded_bytes,
								code.size() + subject.size() + detail.size() +
									sizeof(query_unresolved),
								limits.maximum_expanded_row_bytes,
								"gap-bytes");
							graph.gaps.push_back(
								{std::move(code), std::move(subject), std::move(detail)});
							if (conflict)
								graph.state = semantic_graph_state::conflicting;
							else if (graph.state != semantic_graph_state::conflicting)
								graph.state = semantic_graph_state::partial;
						};
						if (!input.observations_complete)
							gap("sdk.graph-enumeration-unavailable",
								spec.id,
								"supply-complete-observed-scans");
						std::map<std::pair<std::size_t, std::size_t>, std::string> call_payloads;
						std::map<std::string, candidates, std::less<>> units, nodes, edges;
						std::size_t node_count{}, edge_count{};
						const auto collect = [&](auto& map,
												 std::string id,
												 std::string_view columns,
												 std::size_t index)
						{
							add(references,
								1U,
								limits.maximum_evidence_references,
								"evidence-references");
							auto& values = map[std::move(id)];
							const auto [position, inserted] =
								values.try_emplace(columns, candidate{index, {}, {}});
							(void)inserted;
							position->second.evidence.push_back(index);
						};
						for (const auto index : rows[0])
						{
							cancelled(stop);
							const auto unit = text(*entries[index].row, "compile_unit");
							if (unit.empty())
								gap("sdk.graph-unit-id-missing", spec.id);
							else
								collect(units, std::string{unit}, entries[index].payload, index);
						}
						for (const auto& [id, values] : units)
						{
							add(expanded_bytes,
								id.size() + sizeof(std::string),
								limits.maximum_expanded_row_bytes,
								"graph-unit-bytes");
							graph.compile_units.push_back(id);
							if (values.size() != 1U)
								gap("sdk.graph-unit-conflicting", id, {}, true);
						}
						if (units.empty())
							gap("sdk.graph-unit-missing", spec.id);
						const auto requirements = needed(spec);
						for (const auto group : {1U, 2U})
						{
							if (!requirements[group])
								continue;
							for (const auto index : rows[group])
							{
								cancelled(stop);
								const auto& row = *entries[index].row;
								const auto kind =
									group == 1U ? std::string_view{"file"} : text(row, "kind");
								if (group == 2U && !selects(spec.node_kinds, "*") &&
									!selects(spec.node_kinds, kind))
									continue;
								const auto id = text(row, group == 1U ? "file" : "entity");
								if (id.empty() || kind.empty())
								{
									gap("sdk.graph-node-identity-missing", spec.id);
									continue;
								}
								const auto previous = nodes.find(id);
								if (previous == nodes.end() ||
									!previous->second.contains(entries[index].payload))
									add(node_count, 1U, limits.maximum_graph_nodes, "graph-nodes");
								collect(nodes, std::string{id}, entries[index].payload, index);
							}
						}
						for (const auto& [id, values] : nodes)
						{
							if (values.size() != 1U)
								gap("sdk.graph-node-conflicting", id, {}, true);
							for (const auto& [columns, candidate] : values)
							{
								(void)columns;
								const auto& item = entries[candidate.first];
								const auto kind = item.group == 1U ? std::string_view{"file"}
																   : text(*item.row, "kind");
								const auto name =
									text(*item.row,
										 item.group == 1U ? "logical_path" : "qualified_name");
								add(expanded_bytes,
									id.size() + kind.size() + name.size() +
										sizeof(semantic_graph_node),
									limits.maximum_expanded_row_bytes,
									"graph-node-bytes");
								graph.nodes.push_back(
									{id, std::string{kind}, std::string{name}, candidate.evidence});
							}
						}
						const auto append = [&](semantic_graph_edge edge,
												std::string_view columns,
												std::size_t index,
												bool direct_target =
													false) -> std::optional<std::size_t>
						{
							if (edge.id.empty())
							{
								gap("sdk.graph-edge-identity-missing", spec.id);
								return {};
							}
							if (edge.compile_unit.empty() || !units.contains(edge.compile_unit))
								gap("sdk.graph-edge-unit-missing", edge.id, edge.compile_unit);
							if (edge.from.empty() || !nodes.contains(edge.from))
								gap("sdk.graph-source-missing", edge.id, edge.from);
							if (edge.to.empty() || !nodes.contains(edge.to))
								gap("sdk.graph-target-missing", edge.id, edge.to);
							if (edge.resolution != "resolved" && edge.resolution != "direct" &&
								!(direct_target && edge.resolution == "syntactic_direct"))
								gap("sdk.graph-edge-unresolved", edge.id, edge.resolution);
							collect(edges, edge.id, columns, index);
							const auto found = edges.find(edge.id);
							auto selected = found->second.find(columns);
							if (!selected->second.output_index)
							{
								add(edge_count, 1U, limits.maximum_graph_edges, "graph-edges");
								add(expanded_bytes,
									edge.id.size() + edge.from.size() + edge.to.size() +
										edge.kind.size() + edge.source_span.size() +
										edge.compile_unit.size() + edge.resolution.size() +
										sizeof(semantic_graph_edge),
									limits.maximum_expanded_row_bytes,
									"graph-edge-bytes");
								edge.evidence = {index};
								selected->second.output_index = graph.edges.size();
								graph.edges.push_back(std::move(edge));
							}
							else
							{
								// The same payload remains one edge with every annotated origin.
								graph.edges[*selected->second.output_index].evidence.push_back(
									index);
							}
							return selected->second.output_index;
						};
						for (const auto index : rows[3])
						{
							cancelled(stop);
							const auto& row = *entries[index].row;
							if (!selects(spec.edge_kinds, text(row, "kind")))
								continue;
							append({std::string{text(row, "edge")},
									std::string{text(row, "source_entity")},
									std::string{text(row, "target_entity")},
									std::string{text(row, "kind")},
									std::string{text(row, "source")},
									std::string{text(row, "compile_unit")},
									std::string{text(row, "resolution")},
									{}},
								   entries[index].payload,
								   index);
						}
						if (selects(spec.edge_kinds, "includes"))
							for (const auto index : rows[6])
							{
								cancelled(stop);
								const auto& row = *entries[index].row;
								append({std::string{text(row, "include")},
										std::string{text(row, "from_file")},
										std::string{text(row, "to_file")},
										"includes",
										std::string{text(row, "source")},
										std::string{text(row, "compile_unit")},
										std::string{text(row, "resolution")},
										{}},
									   entries[index].payload,
									   index);
							}
						if (selects(spec.edge_kinds, "calls"))
						{
							std::set<std::string, std::less<>> seen_calls;
							// Source and target payloads are both part of a call edge candidate.
							for (const auto& [call_id, site_candidates] : sites)
								for (const auto& [site_columns, site] : site_candidates)
								{
									(void)site_columns;
									const auto index = site.first;
									cancelled(stop);
									const auto& row = *entries[index].row;
									const auto& id = call_id;
									seen_calls.insert(id);
									semantic_graph_edge edge{id,
															 std::string{text(row, "caller")},
															 {},
															 "calls",
															 std::string{text(row, "source")},
															 std::string{text(row, "compile_unit")},
															 "unresolved",
															 {}};
									const auto found = targets.find(id);
									if (found == targets.end())
									{
										if (const auto selected =
												append(edge, entries[index].payload, index))
											for (const auto origin : site.evidence)
												if (origin != index)
												{
													add(references,
														1U,
														limits.maximum_evidence_references,
														"call-site-evidence");
													graph.edges[*selected].evidence.push_back(
														origin);
												}
									}
									else
										for (const auto& [target_columns, target_candidate] :
											 found->second)
										{
											(void)target_columns;
											const auto target_index = target_candidate.first;
											cancelled(stop);
											const auto& target = *entries[target_index].row;
											edge.to = text(target, "target");
											edge.resolution = text(target, "resolution");
											const auto declared_unit = text(target, "compile_unit");
											if (!declared_unit.empty() &&
												declared_unit != edge.compile_unit)
												gap("sdk.graph-call-owner-mismatch",
													id,
													std::string{declared_unit});
											add(expanded_bytes,
												entries[index].payload.size() +
													entries[target_index].payload.size(),
												limits.maximum_expanded_row_bytes,
												"call-join-bytes");
											auto& columns = call_payloads[{index, target_index}];
											columns = entries[index].payload + "\nTARGET\n" +
												entries[target_index].payload;
											if (const auto selected =
													append(edge, columns, index, true))
											{
												for (const auto origin : site.evidence)
													if (origin != index)
													{
														add(references,
															1U,
															limits.maximum_evidence_references,
															"call-site-evidence");
														graph.edges[*selected].evidence.push_back(
															origin);
													}
												for (const auto origin : target_candidate.evidence)
												{
													add(references,
														1U,
														limits.maximum_evidence_references,
														"call-target-evidence");
													graph.edges[*selected].evidence.push_back(
														origin);
												}
											}
										}
								}
							for (const auto& [id, target_candidates] : targets)
								if (!seen_calls.contains(id))
									for (const auto& [columns, target] : target_candidates)
									{
										const auto index = target.first;
										const auto& row = *entries[index].row;
										if (const auto selected =
												append({id,
														{},
														std::string{text(row, "target")},
														"calls",
														{},
														{},
														"unresolved",
														{}},
													   columns,
													   index))
											for (const auto origin : target.evidence)
												if (origin != index)
												{
													add(references,
														1U,
														limits.maximum_evidence_references,
														"orphan-evidence");
													graph.edges[*selected].evidence.push_back(
														origin);
												}
									}
						}
						for (const auto& [id, values] : edges)
							if (values.size() != 1U)
								gap("sdk.graph-edge-conflicting", id, {}, true);
						std::ranges::sort(graph.nodes,
										  {},
										  [](const auto& value)
										  {
											  return std::tie(
												  value.id, value.kind, value.name, value.evidence);
										  });
						std::ranges::sort(graph.edges,
										  {},
										  [](const auto& value)
										  {
											  return std::tie(value.id,
															  value.from,
															  value.to,
															  value.kind,
															  value.resolution,
															  value.evidence);
										  });
						for (auto& edge : graph.edges)
							unique(edge.evidence);
						normalize(graph.gaps);
						output.graphs.push_back(std::move(graph));
					}
				}
				cancelled(stop);
				return output;
			}
			catch (const failure& value)
			{
				return value.value;
			}
			catch (const std::bad_alloc&)
			{
				return error{"sdk.graph-resource-exhausted", "projection", "allocation"};
			}
			catch (const std::length_error&)
			{
				return error{"sdk.graph-resource-exhausted", "projection", "length"};
			}
		}
	} // namespace

	result<semantic_graph_projection>
	project_semantic_graphs(semantic_graph_input input,
							std::span<const semantic_graph_spec> specs,
							semantic_graph_limits limits,
							std::stop_token stop)
	{
		retention_budget retained;
		return project_rows(input, specs, limits, stop, retained);
	}

	result<semantic_graph_projection>
	project_semantic_graphs(const application_query_results& input,
							std::span<const semantic_graph_spec> specs,
							semantic_graph_limits limits,
							std::stop_token stop)
	{
		if (auto valid = limits.validate(); !valid)
			return valid.error();
		try
		{
			cancelled(stop);
			if (auto bounded = detail::check_source_plan_limits(input,
																limits.maximum_source_queries,
																limits.maximum_source_plan_bytes,
																stop,
																"sdk.graph");
				!bounded)
				return bounded.error();
			std::array<std::vector<annotated_row>, 7U> groups;
			std::array<bool, 7U> present{}, complete{}, closed{};
			complete.fill(true);
			closed.fill(true);
			std::array<std::vector<std::string>, 7U> closures;
			std::array<bool, 7U> conflicts{};
			std::size_t row_count{};
			retention_budget retained;
			for (const auto& scan : input.scans)
			{
				cancelled(stop);
				const auto found = std::ranges::find(relation_ids, scan.relation_id);
				if (found == relation_ids.end())
					continue;
				const auto group = static_cast<std::size_t>(found - relation_ids.begin());
				present[group] = true;
				complete[group] = complete[group] &&
					scan.result.execution() == execution_status::complete &&
					scan.result.inputs_complete() && scan.result.unresolved_items().empty() &&
					scan.result.conflicts().empty() &&
					scan.result.differential_disagreements().empty();
				closed[group] =
					closed[group] && scan.result.closed() && !scan.result.closure_ids().empty();
				conflicts[group] = conflicts[group] || !scan.result.conflicts().empty();
				for (const auto& id : scan.result.closure_ids())
				{
					cancelled(stop);
					add(retained.references,
						1U,
						limits.maximum_evidence_references,
						"source-closure-references");
					add(retained.expanded_bytes,
						id.size() + sizeof(std::string),
						limits.maximum_expanded_row_bytes,
						"source-closure-bytes");
				}
				closures[group].insert(closures[group].end(),
									   scan.result.closure_ids().begin(),
									   scan.result.closure_ids().end());
				auto cursor = scan.result.rows();
				while (true)
				{
					cancelled(stop);
					auto next = cursor.next();
					if (!next)
						return next.error();
					if (!*next)
						break;
					add(row_count, 1U, limits.maximum_rows, "rows");
					auto row = (*next)->copy();
					if (!row)
						return row.error();
					add(retained.evidence_bytes,
						row->canonical_form().size(),
						limits.maximum_evidence_bytes,
						"evidence-bytes");
					groups[group].push_back(std::move(*row));
				}
			}
			auto result = project_rows(semantic_graph_input{groups[0],
															groups[1],
															groups[2],
															groups[3],
															groups[4],
															groups[5],
															groups[6],
															true},
									   specs,
									   limits,
									   stop,
									   retained);
			if (!result)
				return result;
			auto& metadata_bytes = retained.expanded_bytes;
			auto& metadata_references = retained.references;
			for (auto& graph : result->graphs)
			{
				const auto requirements = needed(graph.spec);
				graph.closed = graph.state == semantic_graph_state::complete;
				for (std::size_t group{}; group < requirements.size(); ++group)
				{
					cancelled(stop);
					if (!requirements[group])
						continue;
					if (!present[group] || !complete[group])
					{
						add(metadata_references,
							1U,
							limits.maximum_evidence_references,
							"scan-gap-references");
						add(metadata_bytes,
							relation_ids[group].size() + sizeof(query_unresolved) + 128U,
							limits.maximum_expanded_row_bytes,
							"scan-gap-bytes");
						graph.gaps.push_back({present[group] ? "sdk.graph-scan-incomplete"
															 : "sdk.graph-scan-missing",
											  std::string{relation_ids[group]},
											  "preserve-original-scan-frontiers"});
						if (conflicts[group])
							graph.state = semantic_graph_state::conflicting;
						else if (graph.state != semantic_graph_state::conflicting)
							graph.state = semantic_graph_state::partial;
					}
					graph.closed =
						graph.closed && present[group] && complete[group] && closed[group];
					for (const auto& id : closures[group])
					{
						add(metadata_references,
							1U,
							limits.maximum_evidence_references,
							"closure-references");
						add(metadata_bytes,
							id.size() + sizeof(std::string),
							limits.maximum_expanded_row_bytes,
							"closure-bytes");
					}
					graph.closure_ids.insert(
						graph.closure_ids.end(), closures[group].begin(), closures[group].end());
				}
				unique(graph.closure_ids);
				normalize(graph.gaps);
			}
			for (std::size_t group{}; group < present.size(); ++group)
				if (!present[group])
				{
					add(metadata_references,
						1U,
						limits.maximum_evidence_references,
						"missing-scan-references");
					add(metadata_bytes,
						relation_ids[group].size() + sizeof(query_unresolved) + 128U,
						limits.maximum_expanded_row_bytes,
						"missing-scan-bytes");
					result->unresolved.push_back({"sdk.graph-scan-missing",
												  std::string{relation_ids[group]},
												  "independent-scan-unavailable"});
				}
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
			return error{"sdk.graph-resource-exhausted", "projection", "allocation"};
		}
		catch (const std::length_error&)
		{
			return error{"sdk.graph-resource-exhausted", "projection", "length"};
		}
	}
} // namespace cxxlens::sdk::query
