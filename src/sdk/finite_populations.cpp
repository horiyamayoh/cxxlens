#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

#include <cxxlens/sdk/finite_populations.hpp>

#include "query_projection_plan_limits_internal.hpp"

namespace cxxlens::sdk::query
{
	namespace
	{
		using key = std::array<std::string, 4U>;
		using indices = std::vector<std::size_t>;
		struct failure
		{
			error value;
		};
		[[noreturn]] void fail(std::string field,
							   std::string reason,
							   std::string code = "sdk.population-input-invalid")
		{
			throw failure{{std::move(code), std::move(field), std::move(reason)}};
		}
		void check(std::stop_token stop)
		{
			if (stop.stop_requested())
				fail("projection", "stop-requested", "sdk.population-cancelled");
		}
		void
		charge(std::size_t& used, std::size_t amount, std::size_t maximum, std::string_view field)
		{
			if (used > maximum || amount > maximum - used)
				fail(std::string{field}, "limit-exceeded", "sdk.population-budget");
			used += amount;
		}
		const detached_cell* cell(const annotated_row& row, std::string_view name)
		{
			const auto found = row.values.find("output." + std::string{name});
			return found == row.values.end() ? nullptr : &found->second;
		}
		std::string text(const annotated_row& row, std::string_view name)
		{
			const auto* found = cell(row, name);
			const auto* value = found && found->state == cell_state::present && found->value
				? std::get_if<std::string>(&*found->value)
				: nullptr;
			return value ? *value : std::string{};
		}
		std::optional<std::uint64_t> number(const annotated_row& row, std::string_view name)
		{
			const auto* found = cell(row, name);
			const auto* value = found && found->state == cell_state::present && found->value
				? std::get_if<std::uint64_t>(&*found->value)
				: nullptr;
			return value ? std::optional{*value} : std::nullopt;
		}
		std::string payload(const annotated_row& row)
		{
			std::string result;
			for (const auto& [name, value] : row.values)
				result += name + '=' + value.canonical_form() + '\n';
			return result;
		}
		key world(std::string id, const annotated_row& row, const std::string& variant)
		{
			return {std::move(id), row.presence.universe, variant, row.interpretation};
		}
		key world(std::string id, const finite_population& value)
		{
			return {std::move(id), value.universe, value.variant, value.interpretation};
		}
		void gap(finite_population& value,
				 std::string code,
				 std::string detail = {},
				 bool conflict = false)
		{
			value.gaps.push_back({std::move(code),
								  value.id.empty() ? value.compile_unit : value.id,
								  std::move(detail)});
			if (conflict)
				value.state = finite_population_state::conflicting;
			else if (value.state != finite_population_state::conflicting &&
					 value.state != finite_population_state::unknown)
				value.state = finite_population_state::partial;
		}
		void canonical(std::vector<query_unresolved>& gaps)
		{
			std::ranges::sort(gaps,
							  {},
							  [](const auto& v)
							  {
								  return std::tie(v.code, v.subject, v.detail);
							  });
			gaps.erase(std::ranges::unique(gaps).begin(), gaps.end());
		}
		struct spec
		{
			std::string_view domain, inventory, member, member_id, member_set, count, profile;
		};
		constexpr spec declarations{"declarations",
									"cc.declaration_inventory.v1",
									"cc.declaration.v1",
									"declaration",
									"declarations",
									"declaration_count",
									"clang22-explicit-admitted-named-declarations/1"};
		constexpr spec comments{"comments",
								"source.comment_inventory.v1",
								"source.comment.v1",
								"comment",
								"comments",
								"comment_count",
								"clang22-frozen-raw-comments/1"};
		constexpr spec includes{"includes",
								"source.include_inventory.v1",
								"source.include.v1",
								"include",
								"includes",
								"include_count",
								"clang22-preprocessed-inclusion-directives/1"};
		constexpr spec flow{"flow",
							"cc.flow_inventory.v1",
							"cc.flow_fact.v1",
							"fact",
							"facts",
							"fact_count",
							"clang22-local-may-flow-facts/1"};
		struct entry
		{
			std::size_t group;
			const annotated_row* row;
			std::string canonical;
		};
		struct budgets
		{
			std::size_t bytes{}, references{}, members{}, operations{}, conditions{};
		};
		struct projection_work
		{
			finite_population_limits limits;
			std::stop_token stop;
			budgets used;
			finite_population_projection output;
			std::array<std::map<key, indices>, 9U> maps;
			std::map<key, indices> body_nodes;
			void work()
			{
				check(stop);
				charge(used.operations, 1U, limits.maximum_operations, "operations");
			}
			const annotated_row& row(std::size_t ref) const
			{
				return output.evidence.at(ref).row;
			}
			indices lookup(std::size_t group, const key& subject)
			{
				work();
				const auto found = maps[group].find(subject);
				return found == maps[group].end() ? indices{} : found->second;
			}
			void retain(indices& target, const indices& refs)
			{
				charge(used.references,
					   refs.size(),
					   limits.maximum_evidence_references,
					   "evidence-references");
				charge(used.bytes,
					   refs.size() * sizeof(std::size_t),
					   limits.maximum_retained_bytes,
					   "retained-references");
				target.insert(target.end(), refs.begin(), refs.end());
			}
			bool equal(const indices& refs)
			{
				if (refs.empty())
					return false;
				const auto value = payload(row(refs.front()));
				for (const auto ref : refs)
				{
					work();
					if (payload(row(ref)) != value)
						return false;
				}
				return true;
			}
			std::vector<std::string> strings(const annotated_row& row, std::string_view column)
			{
				const auto* found = cell(row, column);
				if (!found || found->state != cell_state::present || !found->value)
					return {};
				const auto* bytes = std::get_if<std::vector<std::byte>>(&*found->value);
				if (!bytes)
					fail(std::string{column}, "set-bytes-required");
				charge(used.bytes,
					   bytes->size() * 8U,
					   limits.maximum_retained_bytes,
					   "decoded-set-bytes");
				std::vector<std::string> result;
				for (std::size_t offset{}; offset < bytes->size();)
				{
					work();
					if (bytes->size() - offset < 4U)
						fail(std::string{column}, "truncated-length");
					std::uint32_t length{};
					for (unsigned shift{}; shift < 32U; shift += 8U)
						length |= std::to_integer<std::uint32_t>((*bytes)[offset++]) << shift;
					if (length > bytes->size() - offset)
						fail(std::string{column}, "truncated-value");
					charge(used.members, 1U, limits.maximum_members, "decoded-members");
					std::string value;
					value.reserve(length);
					for (std::size_t end = offset + length; offset < end; ++offset)
						value += static_cast<char>((*bytes)[offset]);
					if (value.empty() || (!result.empty() && result.back() >= value))
						fail(std::string{column}, "canonical-set-required");
					result.push_back(std::move(value));
				}
				return result;
			}
			void source(finite_population& population, finite_population_member& member)
			{
				const auto refs = lookup(2U, world(member.source_span, population));
				retain(member.evidence, refs);
				if (refs.empty())
				{
					gap(population, "sdk.population-source-missing", member.id);
					return;
				}
				if (!equal(refs))
				{
					gap(population, "sdk.population-source-conflicting", member.id, true);
					return;
				}
				const auto& span = row(refs.front());
				member.file = text(span, "file");
				member.source_snapshot = text(span, "snapshot");
				member.begin = number(span, "begin");
				member.end = number(span, "end");
				const auto files = lookup(1U, world(member.source_snapshot, population));
				retain(member.evidence, files);
				if (files.empty())
				{
					gap(population, "sdk.population-file-missing", member.id);
					return;
				}
				if (!equal(files))
				{
					gap(population, "sdk.population-file-conflicting", member.id, true);
					return;
				}
				const auto& file = row(files.front());
				const auto size = number(file, "size");
				if (text(file, "file") != member.file || !size || !member.begin || !member.end ||
					*member.end < *member.begin || *member.end > *size)
					gap(population, "sdk.population-source-outside-snapshot", member.id, true);
				if (!population.file.empty() &&
					(population.file != member.file ||
					 population.source_snapshot != member.source_snapshot))
					gap(population, "sdk.population-member-file-mismatch", member.id, true);
			}
			void entity(finite_population& population, finite_population_member& member)
			{
				if (member.entity.empty())
					return;
				const auto refs = lookup(3U, world(member.entity, population));
				retain(member.evidence, refs);
				if (refs.empty())
					gap(population, "sdk.population-entity-missing", member.entity);
				else if (!equal(refs))
					gap(population, "sdk.population-entity-conflicting", member.entity, true);
			}
			void file(finite_population& population)
			{
				if (population.source_snapshot.empty())
					return;
				const auto refs = lookup(1U, world(population.source_snapshot, population));
				retain(population.evidence, refs);
				if (refs.empty())
				{
					gap(population, "sdk.population-file-missing", population.source_snapshot);
					return;
				}
				if (!equal(refs))
				{
					gap(population,
						"sdk.population-file-conflicting",
						population.source_snapshot,
						true);
					return;
				}
				const auto& observed = row(refs.front());
				population.source_size = number(observed, "size");
				if (population.file != text(observed, "file"))
					gap(population,
						"sdk.population-inventory-file-mismatch",
						population.file,
						true);
			}
		};
		std::array<std::string_view, 9U> relations(spec current)
		{
			return {"build.compile_unit.v1",
					"source.file.v1",
					"source.span.v1",
					"cc.entity.v1",
					"cc.entity_detail.v1",
					"cc.body.v1",
					"cc.cfg_node.v1",
					current.inventory,
					current.member};
		}
		constexpr std::array<std::string_view, 9U> identifiers{"compile_unit",
															   "snapshot",
															   "span",
															   "entity",
															   "entity",
															   "body",
															   "node",
															   "inventory",
															   ""};
		void validate_member(projection_work& work,
							 spec current,
							 finite_population& population,
							 finite_population_member& member,
							 const annotated_row& row)
		{
			if (current.domain != "declarations" &&
				text(row, "compile_unit") != population.compile_unit)
				gap(population, "sdk.population-member-unit-mismatch", member.id, true);
			if (!member.source_span.empty())
				work.source(population, member);
			else if (current.domain != "flow")
				gap(population, "sdk.population-source-missing", member.id);
			work.entity(population, member);
			if (current.domain == "declarations")
			{
				member.declaration_kind = member.kind;
				member.kind.clear();
				const auto entities = work.lookup(3U, world(member.entity, population));
				if (work.equal(entities))
					member.kind = text(work.row(entities.front()), "kind");
				if (population.file_state == "complete" && !member.source_snapshot.empty() &&
					!std::ranges::binary_search(population.parsed_source_snapshots,
												member.source_snapshot))
					gap(population,
						"sdk.population-declaration-outside-parsed-sources",
						member.id,
						true);
				const auto candidates = work.lookup(4U, world(member.entity, population));
				indices bound;
				for (const auto ref : candidates)
				{
					work.work();
					const auto& detail = work.row(ref);
					if (text(detail, "compile_unit") == population.compile_unit &&
						text(detail, "source") == member.source_span)
					{
						bound.push_back(ref);
						const auto flags = work.strings(detail, "flags");
						if (!std::ranges::binary_search(flags, "finite_declarations_v1"))
							gap(population,
								"sdk.population-declaration-category-profile-missing",
								member.id);
						else if (!std::ranges::binary_search(flags,
															 "declaration_population_admitted"))
							gap(population,
								"sdk.population-declaration-not-admitted",
								member.id,
								true);
					}
				}
				work.retain(member.evidence, bound);
				if (bound.empty())
					gap(population, "sdk.population-declaration-detail-missing", member.id);
				else if (!work.equal(bound))
					gap(population,
						"sdk.population-declaration-detail-conflicting",
						member.id,
						true);
			}
			else if (current.domain == "comments")
			{
				if (text(row, "profile") != current.profile)
					gap(population, "sdk.population-comment-profile-unavailable", member.id);
				if (member.kind != "line" && member.kind != "block")
					gap(population, "sdk.population-comment-kind-unavailable", member.id);
				const auto bytes = number(row, "byte_count");
				if (member.begin && member.end && bytes && *bytes != *member.end - *member.begin)
					gap(population, "sdk.population-comment-bytes-conflicting", member.id, true);
				const auto* spelling = cell(row, "spelling_bytes");
				const auto* raw =
					spelling && spelling->state == cell_state::present && spelling->value
					? std::get_if<std::vector<std::byte>>(&*spelling->value)
					: nullptr;
				if (!raw || !bytes || raw->size() != *bytes)
					gap(population, "sdk.population-comment-spelling-conflicting", member.id, true);
				else
				{
					charge(work.used.bytes,
						   raw->size(),
						   work.limits.maximum_retained_bytes,
						   "comment-spelling");
					member.spelling_bytes = *raw;
				}
			}
			else if (current.domain == "includes")
			{
				if (text(row, "from_file") != population.file)
					gap(population, "sdk.population-include-file-conflicting", member.id, true);
				const auto state = text(row, "resolution");
				if (state != "resolved" && state != "external" && state != "unresolved")
					gap(population, "sdk.population-include-resolution-unavailable", member.id);
				if (state == "resolved" && text(row, "to_file").empty())
					gap(population, "sdk.population-include-target-missing", member.id);
			}
			else if (current.domain == "flow")
			{
				if (text(row, "function") != population.function)
					gap(population, "sdk.population-flow-function-mismatch", member.id, true);
				const auto refs = work.lookup(6U, world(text(row, "node"), population));
				work.retain(member.evidence, refs);
				if (refs.empty())
					gap(population, "sdk.population-flow-node-missing", member.id);
				else
				{
					if (!work.equal(refs))
						gap(population, "sdk.population-flow-node-conflicting", member.id, true);
					for (const auto ref : refs)
					{
						const auto& node = work.row(ref);
						if (text(node, "compile_unit") != population.compile_unit ||
							text(node, "function") != population.function ||
							text(node, "body") != population.body)
							gap(population,
								"sdk.population-flow-node-owner-mismatch",
								member.id,
								true);
					}
				}
				if (!number(row, "program_point"))
					gap(population, "sdk.population-flow-point-missing", member.id);
			}
		}
		void lines(projection_work& work, finite_population& population, const annotated_row& row)
		{
			population.physical_line_profile = text(row, "physical_line_profile");
			if (population.physical_line_profile != "source.physical-lines.crlf-lf-cr/1")
				gap(population,
					"sdk.population-line-profile-unavailable",
					population.physical_line_profile);
			const auto* value = cell(row, "physical_line_starts");
			const auto* bytes = value && value->state == cell_state::present && value->value
				? std::get_if<std::vector<std::byte>>(&*value->value)
				: nullptr;
			if (!bytes || bytes->size() % 8U != 0U)
			{
				gap(population, "sdk.population-line-encoding-conflicting", {}, true);
				return;
			}
			charge(
				work.used.bytes, bytes->size(), work.limits.maximum_retained_bytes, "line-starts");
			for (std::size_t offset{}; offset < bytes->size();)
			{
				work.work();
				std::uint64_t start{};
				for (unsigned shift{}; shift < 64U; shift += 8U)
					start |= std::to_integer<std::uint64_t>((*bytes)[offset++]) << shift;
				if (population.source_size &&
					(start >= *population.source_size ||
					 (population.physical_line_starts.empty()
						  ? start != 0U
						  : start <= population.physical_line_starts.back())))
					gap(population,
						"sdk.population-line-bounds-conflicting",
						std::to_string(start),
						true);
				population.physical_line_starts.push_back(start);
			}
			if (population.source_size &&
				((*population.source_size == 0U) != population.physical_line_starts.empty()))
				gap(population, "sdk.population-line-count-conflicting", {}, true);
		}
		void flow_bindings(projection_work& work,
						   finite_population& population,
						   const std::set<std::string>& admitted)
		{
			std::map<std::string, const annotated_row*, std::less<>> actual;
			for (const auto& member : population.members)
				if (!member.evidence.empty())
					actual.emplace(member.id, &work.row(member.evidence.front()));
			for (const auto& [id, row] : actual)
			{
				const auto kind = text(*row, "kind");
				const auto referenced = [&](std::string_view column, std::string_view expected)
				{
					const auto target = text(*row, column);
					if (target.empty())
						return;
					const auto found = actual.find(target);
					if (found == actual.end())
					{
						gap(population, "sdk.population-flow-reference-missing", target);
						return;
					}
					if ((expected != "event" && text(*found->second, "kind") != expected) ||
						(expected == "event" && text(*found->second, "kind") != "use" &&
						 text(*found->second, "kind") != "definition") ||
						text(*found->second, "node") != text(*row, "node") ||
						number(*found->second, "program_point") != number(*row, "program_point") ||
						(expected == "use" &&
						 text(*found->second, "subject") != text(*row, "subject")))
						gap(population, "sdk.population-flow-reference-conflicting", id, true);
				};
				referenced("use_fact", "use");
				referenced("event_fact", "event");
				if (kind == "def_use" && text(*row, "use_fact").empty())
					gap(population, "sdk.population-flow-use-binding-missing", id);
				for (const auto& definition : work.strings(*row, "definition_facts"))
				{
					const auto found = actual.find(definition);
					if (found == actual.end())
						gap(population, "sdk.population-flow-definition-missing", definition);
					else if (text(*found->second, "kind") != "definition" ||
							 (kind == "def_use" &&
							  text(*found->second, "subject") != text(*row, "subject")))
						gap(population,
							"sdk.population-flow-definition-conflicting",
							definition,
							true);
				}
				for (const auto& subject : work.strings(*row, "live_subjects"))
					if (work.lookup(3U, world(subject, population)).empty())
						gap(population, "sdk.population-live-subject-missing", subject);
			}
			using point_key = std::tuple<std::string, std::string, std::uint64_t>;
			std::set<point_key> expected;
			std::map<std::string, std::uint64_t, std::less<>> node_sizes;
			const auto facet_gap =
				[&](std::string& state, std::string code, std::string detail, bool conflict = false)
			{
				population.gaps.push_back({std::move(code), population.id, std::move(detail)});
				if (conflict)
					state = "conflicting";
				else if (state != "conflicting")
					state = "partial";
			};
			const auto owned_nodes = work.body_nodes.find(world(population.body, population));
			if (owned_nodes != work.body_nodes.end())
				for (const auto ref : owned_nodes->second)
				{
					const auto& node = work.row(ref);
					const auto identity = world(text(node, "node"), population);
					{
						if (text(node, "body") != population.body ||
							text(node, "function") != population.function ||
							text(node, "compile_unit") != population.compile_unit)
							continue;
						work.work();
						const auto size = number(node, "element_count");
						if (!size)
						{
							facet_gap(population.reaching_state,
									  "sdk.population-cfg-element-count-missing",
									  identity[0]);
							facet_gap(population.liveness_state,
									  "sdk.population-cfg-element-count-missing",
									  identity[0]);
							continue;
						}
						node_sizes.emplace(identity[0], *size);
						expected.emplace(identity[0], "in", 0U);
						expected.emplace(identity[0], "out", *size);
					}
				}
			for (const auto& [id, row] : actual)
				if (text(*row, "kind") == "definition" || text(*row, "kind") == "use")
				{
					const auto point = number(*row, "program_point");
					if (!point)
						continue;
					const auto node = text(*row, "node");
					expected.emplace(node, "before:" + id, *point);
					expected.emplace(node, "after:" + id, *point);
				}
			for (const auto ref : work.lookup(5U, world(population.body, population)))
				if (const auto count = number(work.row(ref), "node_count");
					count && *count != node_sizes.size())
				{
					facet_gap(population.reaching_state,
							  "sdk.population-cfg-node-population-missing",
							  population.body);
					facet_gap(population.liveness_state,
							  "sdk.population-cfg-node-population-missing",
							  population.body);
				}
			const auto validate_points = [&](const std::vector<std::string>& points,
											 std::string_view facet,
											 std::string& state)
			{
				if (population.program_point_count &&
					points.size() != *population.program_point_count)
					facet_gap(
						state, "sdk.population-point-count-conflicting", std::string{facet}, true);
				std::set<point_key> observed;
				for (const auto& id : points)
				{
					work.work();
					if (!admitted.contains(id))
						facet_gap(state, "sdk.population-point-not-in-facts", id, true);
					const auto found = actual.find(id);
					if (found == actual.end())
					{
						facet_gap(state, "sdk.population-point-missing", id);
						continue;
					}
					const auto& row = *found->second;
					const auto kind = text(row, "kind");
					const std::array<std::string, 4U> kinds{std::string{facet} + "in",
															std::string{facet} + "out",
															std::string{facet} + "before",
															std::string{facet} + "after"};
					const auto found_kind = std::ranges::find(kinds, kind);
					if (found_kind == kinds.end())
					{
						facet_gap(state, "sdk.population-point-kind-conflicting", id, true);
						continue;
					}
					const auto point = number(row, "program_point");
					if (!point)
					{
						facet_gap(state, "sdk.population-point-position-missing", id);
						continue;
					}
					const auto node = text(row, "node");
					const auto size = node_sizes.find(node);
					if (size == node_sizes.end())
					{
						facet_gap(state, "sdk.population-point-node-missing", id);
						continue;
					}
					if (*point > size->second)
						facet_gap(state, "sdk.population-point-position-conflicting", id, true);
					const auto boundary = static_cast<unsigned>(found_kind - kinds.begin());
					const auto event = text(row, "event_fact");
					if ((boundary < 2U && !event.empty()) || (boundary >= 2U && event.empty()))
						facet_gap(state, "sdk.population-point-event-conflicting", id, true);
					const auto label = boundary == 0U ? "in"
						: boundary == 1U			  ? "out"
										 : (boundary == 2U ? "before:" : "after:") + event;
					const point_key value{node, label, *point};
					if (!observed.insert(value).second)
						facet_gap(state, "sdk.population-point-duplicate", id, true);
					if (!expected.contains(value))
						facet_gap(state, "sdk.population-point-extra", id, true);
					const auto* values =
						cell(row, facet == "live_" ? "live_subjects" : "definition_facts");
					if (!values || values->state != cell_state::present)
						facet_gap(state, "sdk.population-point-set-missing", id);
				}
				if (observed != expected)
					facet_gap(
						state, "sdk.population-point-population-incomplete", std::string{facet});
				if (population.program_point_count &&
					*population.program_point_count != expected.size() &&
					population.members.size() == admitted.size() && !node_sizes.empty())
					facet_gap(state,
							  "sdk.population-independent-point-count-conflicting",
							  std::string{facet},
							  true);
			};
			validate_points(population.reaching_points, "reaching_", population.reaching_state);
			validate_points(population.liveness_points, "live_", population.liveness_state);
			if (population.program_point_profile !=
				"clang22-cfg-boundaries-and-written-local-events/1")
			{
				facet_gap(population.reaching_state,
						  "sdk.population-point-profile-unavailable",
						  population.program_point_profile);
				facet_gap(population.liveness_state,
						  "sdk.population-point-profile-unavailable",
						  population.program_point_profile);
			}
		}
		result<finite_population_projection> project_rows(finite_population_input input,
														  spec current,
														  finite_population_limits limits,
														  std::stop_token stop)
		{
			if (auto valid = limits.validate(); !valid)
				return valid.error();
			try
			{
				check(stop);
				projection_work work{limits, stop, {}, {}, {}, {}};
				work.output.compile_units_complete = input.compile_units_complete;
				work.output.inventory_inputs_complete = input.inventory_inputs_complete;
				work.output.member_inputs_complete = input.member_inputs_complete;
				const std::array groups{input.units,
										input.files,
										input.spans,
										input.entities,
										input.details,
										input.bodies,
										input.cfg_nodes,
										input.inventories,
										input.members};
				const auto names = relations(current);
				const auto descriptors = standard_relation_descriptors();
				std::vector<entry> entries;
				for (std::size_t group{}; group < groups.size(); ++group)
				{
					const auto descriptor =
						std::ranges::find(descriptors, names[group], &relation_descriptor::id);
					if (descriptor == descriptors.end())
						fail(std::string{names[group]}, "descriptor-missing");
					for (const auto& row : groups[group])
					{
						work.work();
						if (entries.size() >= limits.maximum_rows)
							fail("rows", "limit-exceeded", "sdk.population-budget");
						if (auto valid = row.validate(); !valid)
							return valid.error();
						for (const auto& column : descriptor->columns)
						{
							const auto* actual = cell(row, column.name);
							if (!actual || actual->type != column.type || !actual->validate())
								fail(column.id, "column-type-or-value-invalid");
						}
						charge(work.used.conditions,
							   row.presence.fragments.size(),
							   limits.maximum_condition_expansions,
							   "condition-expansions");
						auto encoded = row.canonical_form();
						charge(work.used.bytes,
							   encoded.size() * 2U + sizeof(entry),
							   limits.maximum_retained_bytes,
							   "retained-evidence");
						if (encoded.size() > limits.maximum_evidence_bytes)
							fail("evidence", "limit-exceeded", "sdk.population-budget");
						entries.push_back({group, &row, std::move(encoded)});
					}
				}
				std::ranges::sort(entries,
								  {},
								  [](const auto& item)
								  {
									  return std::tie(item.group, item.canonical);
								  });
				std::size_t evidence_bytes{};
				for (const auto& item : entries)
				{
					const auto ref = work.output.evidence.size();
					charge(evidence_bytes,
						   item.canonical.size(),
						   limits.maximum_evidence_bytes,
						   "evidence-bytes");
					work.output.evidence.push_back({std::string{names[item.group]}, *item.row});
					const auto identifier =
						item.group == 8U ? current.member_id : identifiers[item.group];
					const auto id = text(*item.row, identifier);
					if (id.empty())
						fail(std::string{identifier}, "identity-missing");
					for (const auto& variant : item.row->presence.fragments)
					{
						work.work();
						charge(work.used.bytes,
							   id.size() + sizeof(key) + sizeof(std::size_t) + 128U,
							   limits.maximum_retained_bytes,
							   "condition-index");
						work.maps[item.group][world(id, *item.row, variant)].push_back(ref);
						if (item.group == 6U)
						{
							charge(work.used.bytes,
								   sizeof(key) + text(*item.row, "body").size() + 128U,
								   limits.maximum_retained_bytes,
								   "body-node-index");
							work.body_nodes[world(text(*item.row, "body"), *item.row, variant)]
								.push_back(ref);
						}
					}
				}
				std::map<key, std::set<std::string>> observed_members;
				std::map<key, std::set<std::string>> declaration_bindings;
				if (current.domain == "declarations")
					for (const auto& [identity, refs] : work.maps[8U])
						for (const auto ref : refs)
						{
							const auto& row = work.row(ref);
							declaration_bindings[world(text(row, "entity") + '\n' +
														   text(row, "source"),
													   row,
													   identity[2])]
								.insert(identity[0]);
						}
				std::map<key, std::set<std::string>> missing_declarations;
				if (current.domain == "declarations")
					for (const auto& [identity, refs] : work.maps[4U])
						for (const auto ref : refs)
						{
							work.work();
							const auto& detail = work.row(ref);
							const auto flags = work.strings(detail, "flags");
							if (!std::ranges::binary_search(flags,
															"declaration_population_admitted"))
								continue;
							const auto binding =
								text(detail, "entity") + '\n' + text(detail, "source");
							if (!declaration_bindings.contains(world(binding, detail, identity[2])))
								missing_declarations[world(text(detail, "compile_unit"),
														   detail,
														   identity[2])]
									.insert(binding);
						}
				for (const auto& [member_key, refs] : work.maps[8U])
					for (const auto ref : refs)
					{
						work.work();
						const auto& row = work.row(ref);
						if (current.domain == "declarations")
						{
							for (const auto detail :
								 work.lookup(4U, world(text(row, "entity"), row, member_key[2])))
							{
								const auto& candidate = work.row(detail);
								const auto flags = work.strings(candidate, "flags");
								if (text(candidate, "source") == text(row, "source") &&
									std::ranges::binary_search(flags,
															   "declaration_population_admitted"))
									observed_members[world(text(candidate, "compile_unit"),
														   row,
														   member_key[2])]
										.insert(member_key[0]);
							}
						}
						else if (current.domain == "flow")
						{
							for (const auto node :
								 work.lookup(6U, world(text(row, "node"), row, member_key[2])))
								if (text(work.row(node), "compile_unit") ==
										text(row, "compile_unit") &&
									text(work.row(node), "function") == text(row, "function"))
									observed_members[world(text(work.row(node), "body"),
														   row,
														   member_key[2])]
										.insert(member_key[0]);
						}
						else
						{
							if (current.domain == "comments" &&
								text(row, "profile") != current.profile)
								continue;
							for (const auto source :
								 work.lookup(2U, world(text(row, "source"), row, member_key[2])))
								observed_members[world(text(row, "compile_unit") + '\n' +
														   text(work.row(source), "snapshot"),
													   row,
													   member_key[2])]
									.insert(member_key[0]);
						}
					}
				std::set<key> population_owners;
				for (const auto& [identity, refs] : work.maps[7U])
				{
					work.work();
					if (work.output.populations.size() >= limits.maximum_populations)
						fail("populations", "limit-exceeded", "sdk.population-budget");
					const auto& row = work.row(refs.front());
					finite_population population;
					population.id = identity[0];
					population.domain = current.domain;
					population.universe = identity[1];
					population.variant = identity[2];
					population.interpretation = identity[3];
					population.compile_unit = text(row, "compile_unit");
					population.profile = text(row, "profile");
					population.function = text(row, "function");
					population.body = text(row, "body");
					population.source_span = text(row, "source");
					population.file = text(row, "file");
					population.source_snapshot = text(row, "source_snapshot");
					population.declared_count = number(row, current.count);
					population.declared_bytes = number(row, "comment_bytes");
					const auto state = text(row, "enumeration_state");
					population.state = state == "complete" ? finite_population_state::complete
						: state == "partial"			   ? finite_population_state::partial
														   : finite_population_state::unknown;
					work.retain(population.evidence, refs);
					if (!work.equal(refs))
						gap(population,
							"sdk.population-inventory-conflicting",
							population.id,
							true);
					if (population.profile != current.profile)
					{
						if (population.state != finite_population_state::conflicting)
							population.state = finite_population_state::unknown;
						gap(population, "sdk.population-profile-unavailable", population.profile);
					}
					if (state != "complete")
						gap(population,
							"sdk.population-enumeration-incomplete",
							text(row, "reason"));
					const auto unit = work.lookup(0U, world(population.compile_unit, population));
					work.retain(population.evidence, unit);
					if (unit.empty())
						gap(population, "sdk.population-unit-missing", population.compile_unit);
					else if (!work.equal(unit))
						gap(population,
							"sdk.population-unit-conflicting",
							population.compile_unit,
							true);
					work.file(population);
					const auto listed = work.strings(row, current.member_set);
					const std::set<std::string> admitted{listed.begin(), listed.end()};
					if (!population.declared_count ||
						(state == "complete" && *population.declared_count != listed.size()) ||
						(population.declared_count && *population.declared_count < listed.size()))
						gap(population, "sdk.population-count-conflicting", population.id, true);
					population.system_members = work.strings(row, "system_declarations");
					for (const auto& system : population.system_members)
						if (!admitted.contains(system))
							gap(population,
								"sdk.population-system-subset-conflicting",
								system,
								true);
					if (current.domain == "declarations")
					{
						population.parsed_files = work.strings(row, "parsed_files");
						population.parsed_source_snapshots =
							work.strings(row, "parsed_source_snapshots");
						population.file_state = text(row, "file_state");
						std::set<std::string> bound_files;
						for (const auto& snapshot : population.parsed_source_snapshots)
						{
							const auto files = work.lookup(1U, world(snapshot, population));
							work.retain(population.evidence, files);
							if (files.empty())
								gap(population, "sdk.population-parsed-source-missing", snapshot);
							else
							{
								if (!work.equal(files))
									gap(population,
										"sdk.population-parsed-source-conflicting",
										snapshot,
										true);
								for (const auto ref : files)
									bound_files.insert(text(work.row(ref), "file"));
							}
						}
						if (bound_files.size() == population.parsed_source_snapshots.size() &&
							std::vector<std::string>{bound_files.begin(), bound_files.end()} !=
								population.parsed_files)
							gap(population,
								"sdk.population-parsed-files-conflicting",
								population.id,
								true);
					}
					if (current.domain == "comments")
						lines(work, population, row);
					if (current.domain == "includes")
						population.resolution_state = text(row, "resolution_state");
					if (current.domain == "flow")
					{
						population.program_point_profile = text(row, "program_point_profile");
						population.program_point_count = number(row, "program_point_count");
						population.reaching_points = work.strings(row, "reaching_points");
						population.liveness_points = work.strings(row, "liveness_points");
						population.reaching_state = text(row, "reaching_state");
						population.liveness_state = text(row, "liveness_state");
						population.pointer_subjects = work.strings(row, "pointer_subjects");
						population.integer_subjects = work.strings(row, "integer_subjects");
						population.subject_state = text(row, "subject_state");
						population.points_to_state = text(row, "points_to_state");
						population.null_state = text(row, "null_state");
						population.range_state = text(row, "range_state");
						population.summary_state = text(row, "summary_state");
						const auto bodies = work.lookup(5U, world(population.body, population));
						work.retain(population.evidence, bodies);
						if (bodies.empty())
							gap(population, "sdk.population-body-missing", population.body);
						else
						{
							if (!work.equal(bodies))
								gap(population,
									"sdk.population-body-conflicting",
									population.body,
									true);
							for (const auto body : bodies)
								if (text(work.row(body), "compile_unit") !=
										population.compile_unit ||
									text(work.row(body), "function") != population.function ||
									text(work.row(body), "source") != population.source_span)
									gap(population,
										"sdk.population-body-owner-conflicting",
										population.body,
										true);
						}
					}
					std::uint64_t observed_bytes{};
					for (const auto& id : listed)
					{
						const auto member_refs = work.lookup(8U, world(id, population));
						if (member_refs.empty())
						{
							gap(population, "sdk.population-member-missing", id);
							continue;
						}
						if (!work.equal(member_refs))
							gap(population, "sdk.population-member-conflicting", id, true);
						const auto& member_row = work.row(member_refs.front());
						finite_population_member member;
						member.id = id;
						member.entity =
							text(member_row, current.domain == "flow" ? "subject" : "entity");
						member.kind = text(member_row, "kind");
						member.source_span = text(member_row, "source");
						work.retain(member.evidence, member_refs);
						validate_member(work, current, population, member, member_row);
						if (const auto count = number(member_row, "byte_count"))
						{
							if (*count > std::numeric_limits<std::uint64_t>::max() - observed_bytes)
								fail("comment-bytes", "overflow");
							observed_bytes += *count;
						}
						population.members.push_back(std::move(member));
					}
					if (current.domain == "comments" &&
						population.members.size() == listed.size() && population.declared_bytes &&
						*population.declared_bytes != observed_bytes)
						gap(population,
							"sdk.population-comment-total-conflicting",
							population.id,
							true);
					if (current.domain == "flow")
						flow_bindings(work, population, admitted);
					if (!input.member_inputs_complete)
						gap(population,
							"sdk.population-member-scan-incomplete",
							std::string{current.member});
					const auto owner = current.domain == "declarations" ? population.compile_unit
						: current.domain == "flow"
						? population.body
						: population.compile_unit + '\n' + population.source_snapshot;
					if (const auto observed = observed_members.find(world(owner, population));
						observed != observed_members.end())
						for (const auto& id : observed->second)
							if (!admitted.contains(id))
								gap(population, "sdk.population-unlisted-member", id, true);
					if (const auto absent = missing_declarations.find(world(owner, population));
						absent != missing_declarations.end())
						for (const auto& binding : absent->second)
							gap(population, "sdk.population-admitted-declaration-missing", binding);
					population_owners.insert(world(owner, population));
					canonical(population.gaps);
					work.output.unresolved.insert(work.output.unresolved.end(),
												  population.gaps.begin(),
												  population.gaps.end());
					work.output.populations.push_back(std::move(population));
				}
				const auto missing = [&](finite_population value)
				{
					work.work();
					if (work.output.populations.size() >= limits.maximum_populations)
						fail("populations", "limit-exceeded", "sdk.population-budget");
					value.domain = current.domain;
					value.profile = current.profile;
					gap(value,
						"sdk.population-inventory-missing",
						value.body.empty() ? value.source_snapshot : value.body);
					work.output.unresolved.insert(
						work.output.unresolved.end(), value.gaps.begin(), value.gaps.end());
					work.output.populations.push_back(std::move(value));
				};
				if (current.domain == "flow")
				{
					for (const auto& [identity, refs] : work.maps[5U])
						if (!population_owners.contains(identity))
						{
							const auto& row = work.row(refs.front());
							finite_population value;
							value.body = identity[0];
							value.universe = identity[1];
							value.variant = identity[2];
							value.interpretation = identity[3];
							value.compile_unit = text(row, "compile_unit");
							value.function = text(row, "function");
							value.source_span = text(row, "source");
							work.retain(value.evidence, refs);
							missing(std::move(value));
						}
				}
				else
				{
					std::map<std::array<std::string, 3U>, std::vector<std::pair<key, indices>>>
						world_files;
					if (current.domain != "declarations")
						for (const auto& [identity, refs] : work.maps[1U])
							world_files[{identity[1], identity[2], identity[3]}].push_back(
								{identity, refs});
					for (const auto& [identity, refs] : work.maps[0U])
					{
						finite_population value;
						value.compile_unit = identity[0];
						value.universe = identity[1];
						value.variant = identity[2];
						value.interpretation = identity[3];
						work.retain(value.evidence, refs);
						if (current.domain == "declarations")
						{
							if (!population_owners.contains(identity))
								missing(std::move(value));
						}
						else
							for (const auto& [file_identity, file_refs] :
								 world_files[{identity[1], identity[2], identity[3]}])
							{
								work.work();
								if (population_owners.contains(
										world(value.compile_unit + '\n' + file_identity[0], value)))
									continue;
								auto source = value;
								source.source_snapshot = file_identity[0];
								source.file = text(work.row(file_refs.front()), "file");
								source.source_size = number(work.row(file_refs.front()), "size");
								work.retain(source.evidence, file_refs);
								missing(std::move(source));
							}
					}
				}
				std::ranges::sort(work.output.populations,
								  {},
								  [](const auto& v)
								  {
									  return std::tie(v.universe,
													  v.variant,
													  v.interpretation,
													  v.compile_unit,
													  v.body,
													  v.source_snapshot,
													  v.id);
								  });
				for (auto& population : work.output.populations)
				{
					std::ranges::sort(population.evidence);
					population.evidence.erase(std::ranges::unique(population.evidence).begin(),
											  population.evidence.end());
					for (auto& member : population.members)
					{
						std::ranges::sort(member.evidence);
						member.evidence.erase(std::ranges::unique(member.evidence).begin(),
											  member.evidence.end());
					}
				}
				canonical(work.output.unresolved);
				return std::move(work.output);
			}
			catch (const failure& exception)
			{
				return exception.value;
			}
			catch (const std::bad_alloc&)
			{
				return error{"sdk.population-resource-exhausted", "projection", "allocation"};
			}
			catch (const std::length_error&)
			{
				return error{"sdk.population-resource-exhausted", "projection", "length"};
			}
		}
		result<finite_population_projection> project_queries(const application_query_results& input,
															 spec current,
															 finite_population_limits limits,
															 std::stop_token stop)
		{
			if (auto valid = limits.validate(); !valid)
				return valid.error();
			try
			{
				check(stop);
				if (auto bounded =
						detail::check_source_plan_limits(input,
														 limits.maximum_source_queries,
														 limits.maximum_source_plan_bytes,
														 stop,
														 "sdk.population");
					!bounded)
					return bounded.error();
				const auto names = relations(current);
				std::array<std::vector<annotated_row>, 9U> groups;
				std::array<bool, 9U> present{}, complete{};
				complete.fill(true);
				std::size_t rows{}, bytes{};
				for (const auto& scan : input.scans)
				{
					check(stop);
					const auto found = std::ranges::find(names, scan.relation_id);
					if (found == names.end())
						continue;
					const auto group = static_cast<std::size_t>(found - names.begin());
					present[group] = true;
					complete[group] &= scan.result.execution() == execution_status::complete &&
						scan.result.inputs_complete() && scan.result.conflicts().empty() &&
						scan.result.differential_disagreements().empty();
					auto cursor = scan.result.rows();
					while (true)
					{
						check(stop);
						auto next = cursor.next();
						if (!next)
							return next.error();
						if (!*next)
							break;
						charge(rows, 1U, limits.maximum_rows, "scan-rows");
						auto row = (*next)->copy();
						if (!row)
							return row.error();
						charge(bytes,
							   row->canonical_form().size(),
							   limits.maximum_evidence_bytes,
							   "scan-evidence");
						groups[group].push_back(std::move(*row));
					}
				}
				auto output = project_rows({groups[0],
											groups[1],
											groups[2],
											groups[3],
											groups[4],
											groups[5],
											groups[6],
											groups[7],
											groups[8],
											present[0] && complete[0],
											present[7] && complete[7],
											present[8] && complete[8]},
										   current,
										   limits,
										   stop);
				if (!output)
					return output;
				for (std::size_t group{}; group < names.size(); ++group)
					if (!present[group] &&
						(group == 0U || group == 1U || group == 2U || group == 7U || group == 8U ||
						 (current.domain == "declarations" && (group == 3U || group == 4U)) ||
						 (current.domain == "flow" && (group == 3U || group == 5U || group == 6U))))
						output->unresolved.push_back({"sdk.population-scan-missing",
													  std::string{names[group]},
													  "supply-independent-public-scan"});
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
				return error{"sdk.population-resource-exhausted", "projection", "allocation"};
			}
			catch (const std::length_error&)
			{
				return error{"sdk.population-resource-exhausted", "projection", "length"};
			}
		}
	} // namespace

	result<void> finite_population_limits::validate() const
	{
		if (!maximum_rows || !maximum_condition_expansions || !maximum_evidence_bytes ||
			!maximum_retained_bytes || !maximum_evidence_references || !maximum_populations ||
			!maximum_members || !maximum_operations || !maximum_source_queries ||
			!maximum_source_plan_bytes)
			return error{"sdk.population-limit-invalid", "limits", "positive-required"};
		return {};
	}
	result<finite_population_projection> project_declarations(finite_population_input input,
															  finite_population_limits limits,
															  std::stop_token stop)
	{
		return project_rows(input, declarations, limits, stop);
	}
	result<finite_population_projection>
	project_declarations(const application_query_results& input,
						 finite_population_limits limits,
						 std::stop_token stop)
	{
		return project_queries(input, declarations, limits, stop);
	}
	result<finite_population_projection> project_source_comments(finite_population_input input,
																 finite_population_limits limits,
																 std::stop_token stop)
	{
		return project_rows(input, comments, limits, stop);
	}
	result<finite_population_projection>
	project_source_comments(const application_query_results& input,
							finite_population_limits limits,
							std::stop_token stop)
	{
		return project_queries(input, comments, limits, stop);
	}
	result<finite_population_projection> project_source_includes(finite_population_input input,
																 finite_population_limits limits,
																 std::stop_token stop)
	{
		return project_rows(input, includes, limits, stop);
	}
	result<finite_population_projection>
	project_source_includes(const application_query_results& input,
							finite_population_limits limits,
							std::stop_token stop)
	{
		return project_queries(input, includes, limits, stop);
	}
	result<finite_population_projection> project_body_flow(finite_population_input input,
														   finite_population_limits limits,
														   std::stop_token stop)
	{
		return project_rows(input, flow, limits, stop);
	}
	result<finite_population_projection> project_body_flow(const application_query_results& input,
														   finite_population_limits limits,
														   std::stop_token stop)
	{
		return project_queries(input, flow, limits, stop);
	}
} // namespace cxxlens::sdk::query
