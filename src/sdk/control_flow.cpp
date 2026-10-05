#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

#include <cxxlens/sdk/control_flow.hpp>

#include "query_projection_plan_limits_internal.hpp"
#include "query_result_internal.hpp"

namespace cxxlens::sdk::query
{
	namespace
	{
		using key = std::array<std::string, 4U>;
		using function_key = std::array<std::string, 5U>;
		struct projection_failure
		{
			error value;
		};
		[[noreturn]] void fail(std::string code, std::string field, std::string detail)
		{
			throw projection_failure{{std::move(code), std::move(field), std::move(detail)}};
		}
		void cancelled(std::stop_token token)
		{
			if (token.stop_requested())
				fail("sdk.cfg-cancelled", "projection", "stop-requested");
		}
		void bounded_add(std::size_t& target,
						 std::size_t amount,
						 std::size_t maximum,
						 std::string_view field)
		{
			if (target > maximum || amount > maximum - target)
				fail("sdk.cfg-budget", std::string{field}, "limit-exceeded");
			target += amount;
		}
		void usage_add(std::size_t& target, std::size_t amount)
		{
			bounded_add(target, amount, std::numeric_limits<std::size_t>::max(), "usage-overflow");
		}
		void usage_product(std::size_t& target, std::size_t count, std::size_t width)
		{
			if (count > std::numeric_limits<std::size_t>::max() / width)
				fail("sdk.cfg-budget", "usage-overflow", "limit-exceeded");
			usage_add(target, count * width);
		}
		void checkpoint(std::stop_token token, std::size_t& operations)
		{
			cancelled(token);
			usage_add(operations, 1U);
		}

		std::size_t
		row_geometry(const annotated_row& row, std::stop_token stop, std::size_t& operations)
		{
			std::size_t total = sizeof(annotated_row) + 2176U;
			const auto dynamic = [&](std::size_t bytes)
			{
				usage_product(total, bytes, 16U);
			};
			const auto strings = [&](const auto& values)
			{
				for (const auto& value : values)
				{
					checkpoint(stop, operations);
					usage_add(total, sizeof(value) + 128U);
					dynamic(value.size());
				}
			};
			const auto producer = [&](const auto& value)
			{
				checkpoint(stop, operations);
				usage_add(total, sizeof(value) + 128U);
				dynamic(value.id.size());
				dynamic(value.semantic_contract.size());
			};
			const auto guarantee = [&](const auto& value)
			{
				checkpoint(stop, operations);
				usage_add(total, sizeof(value) + 128U);
				dynamic(value.approximation.size());
				dynamic(value.scope.size());
				dynamic(value.assumptions.size());
				strings(value.verification_modalities);
			};
			for (const auto& [name, value] : row.values)
			{
				checkpoint(stop, operations);
				usage_add(total, sizeof(decltype(row.values)::value_type) + 512U);
				dynamic(name.size());
				dynamic(value.type.parameter.size());
				if (value.unknown_reason)
					dynamic(value.unknown_reason->size());
				if (value.value)
				{
					if (const auto* text = std::get_if<std::string>(&*value.value))
						dynamic(text->size());
					if (const auto* bytes = std::get_if<std::vector<std::byte>>(&*value.value))
						dynamic(bytes->size());
				}
			}
			strings(row.claim_contributors);
			strings(row.provenance);
			strings(row.presence.fragments);
			dynamic(row.interpretation.size());
			dynamic(row.presence.universe.size());
			for (const auto& value : row.producer_contracts)
				producer(value);
			for (const auto& value : row.contributor_guarantees)
				guarantee(value);
			for (const auto& value : row.contributor_edges)
			{
				checkpoint(stop, operations);
				usage_add(total, sizeof(value) + 128U);
				dynamic(value.claim_contributor.size());
				dynamic(value.provenance.size());
				dynamic(value.interpretation.size());
				dynamic(value.condition.universe.size());
				producer(value.producer);
				guarantee(value.guarantee);
				strings(value.condition.fragments);
			}
			return total;
		}

		const detached_cell* cell(const annotated_row& row, std::string_view name)
		{
			const auto found = row.values.find("output." + std::string{name});
			return found == row.values.end() || found->second.state != cell_state::present ||
					!found->second.value
				? nullptr
				: &found->second;
		}
		std::optional<std::string> text(const annotated_row& row, std::string_view name)
		{
			const auto* found = cell(row, name);
			const auto* value = found ? std::get_if<std::string>(&*found->value) : nullptr;
			return value && !value->empty() ? std::optional{*value} : std::nullopt;
		}
		std::optional<std::uint64_t> number(const annotated_row& row, std::string_view name)
		{
			const auto* found = cell(row, name);
			const auto* value = found ? std::get_if<std::uint64_t>(&*found->value) : nullptr;
			return value ? std::optional{*value} : std::nullopt;
		}
		std::string payload(const annotated_row& row)
		{
			std::string value;
			for (const auto& [name, item] : row.values)
				value += name + "=" + item.canonical_form() + "\n";
			return value;
		}
		void gap(control_flow_body& body,
				 std::string code,
				 std::string detail = {},
				 bool conflict = false)
		{
			body.gaps.push_back({std::move(code), body.id, std::move(detail)});
			if (conflict)
				body.state = control_flow_state::conflicting;
			else if (body.state != control_flow_state::conflicting)
				body.state = control_flow_state::partial;
		}
		void canonical_gaps(std::vector<query_unresolved>& gaps)
		{
			std::ranges::sort(gaps,
							  {},
							  [](const auto& value)
							  {
								  return std::tie(value.code, value.subject, value.detail);
							  });
			gaps.erase(std::ranges::unique(gaps).begin(), gaps.end());
		}
		key world_key(std::string id, const annotated_row& row, const std::string& variant)
		{
			return {std::move(id), row.presence.universe, variant, row.interpretation};
		}
		struct work_body
		{
			control_flow_body value;
			std::string payload;
			std::map<std::string, std::pair<std::size_t, std::string>, std::less<>> nodes, edges;
		};
		struct entry
		{
			unsigned int kind{};
			const annotated_row* row{};
			std::string canonical;
		};

	} // namespace

	result<void> control_flow_limits::validate() const
	{
		if (maximum_rows == 0U || maximum_condition_expansions == 0U ||
			maximum_evidence_bytes == 0U || maximum_expanded_row_bytes == 0U ||
			maximum_bodies == 0U || maximum_body_nodes == 0U || maximum_body_edges == 0U ||
			maximum_source_queries == 0U || maximum_source_plan_bytes == 0U)
			return error{"sdk.cfg-limit-invalid", "limits", "positive-required"};
		return {};
	}

	static result<control_flow_projection> project_control_flow_rows(
		control_flow_input input,
		control_flow_limits limits,
		std::stop_token cancellation,
		projection_resource_usage& usage,
		const std::array<std::vector<const annotated_row*>, 3U>* borrowed = nullptr,
		std::size_t query_operations = 0U,
		std::size_t query_bytes = 0U,
		std::size_t maximum_storage = std::numeric_limits<std::size_t>::max())
	{
		if (auto valid = limits.validate(); !valid)
			return valid.error();
		try
		{
			std::size_t operations = query_operations, storage{};
			bounded_add(storage, query_bytes, maximum_storage, "retained-bytes");
			checkpoint(cancellation, operations);
			std::size_t row_count{}, bytes{}, expansions{}, expanded_bytes{};
			std::vector<entry> entries;
			const std::array groups{input.bodies, input.nodes, input.edges};
			for (unsigned int kind{}; kind < groups.size(); ++kind)
			{
				const auto count = borrowed ? (*borrowed)[kind].size() : groups[kind].size();
				bounded_add(row_count, count, limits.maximum_rows, "rows");
				for (std::size_t index{}; index < count; ++index)
				{
					const auto& row = borrowed ? *(*borrowed)[kind][index] : groups[kind][index];
					checkpoint(cancellation, operations);
					if (!row.presence.validate() || row.interpretation.empty())
						fail("sdk.cfg-input-invalid", "condition", "validated-world-required");
					bounded_add(expansions,
								row.presence.fragments.size(),
								limits.maximum_condition_expansions,
								"condition-expansions");
					// Borrowed-row geometry covers row ownership, canonical payloads and
					// conditioned body/node/edge keys before any of those allocations.
					const auto geometry = row_geometry(row, cancellation, operations);
					bounded_add(storage, geometry, maximum_storage, "retained-bytes");
					std::size_t conditioned = geometry;
					usage_add(conditioned, 4096U);
					std::size_t indexed{};
					usage_product(indexed, row.presence.fragments.size(), conditioned);
					bounded_add(storage, indexed, maximum_storage, "retained-bytes");
					auto canonical = row.canonical_form();
					bounded_add(
						bytes, canonical.size(), limits.maximum_evidence_bytes, "evidence-bytes");
					if (canonical.size() > limits.maximum_expanded_row_bytes /
							std::max(std::size_t{1U}, row.presence.fragments.size()))
						fail("sdk.cfg-budget", "expanded-row-bytes", "limit-exceeded");
					bounded_add(expanded_bytes,
								canonical.size() * row.presence.fragments.size(),
								limits.maximum_expanded_row_bytes,
								"expanded-row-bytes");
					entries.push_back({kind, &row, std::move(canonical)});
				}
			}
			std::ranges::sort(entries,
							  {},
							  [](const auto& value)
							  {
								  return std::tie(value.kind, value.canonical);
							  });
			control_flow_projection output;
			std::map<key, work_body> bodies;
			std::map<key, std::set<key>> node_owners;
			for (const auto& item : entries)
			{
				checkpoint(cancellation, operations);
				const auto ref = output.evidence_rows.size();
				output.evidence_rows.push_back(*item.row);
				const auto& row = *item.row;
				if (item.kind != 0U)
					continue;
				const auto id = text(row, "body"), function = text(row, "function"),
						   unit = text(row, "compile_unit");
				if (!id || !function || !unit)
					fail("sdk.cfg-input-invalid", "body", "subject-owner-required");
				for (const auto& variant : row.presence.fragments)
				{
					checkpoint(cancellation, operations);
					const auto subject = world_key(*id, row, variant);
					if (!bodies.contains(subject) && bodies.size() >= limits.maximum_bodies)
						fail("sdk.cfg-budget", "bodies", "limit-exceeded");
					auto [where, inserted] = bodies.try_emplace(subject);
					auto& work = where->second;
					auto& body = work.value;
					body.evidence.push_back(ref);
					const auto candidate = payload(row);
					if (!inserted)
					{
						if (work.payload != candidate)
							gap(body, "sdk.cfg-body-conflicting", "overlapping-payloads", true);
						continue;
					}
					work.payload = candidate;
					body.id = *id;
					body.function = *function;
					body.compile_unit = *unit;
					body.universe = row.presence.universe;
					body.variant = variant;
					body.interpretation = row.interpretation;
					body.source_span = text(row, "source").value_or("");
					body.analysis_profile = text(row, "analysis_profile").value_or("");
					body.declared_nodes = number(row, "node_count");
					body.declared_edges = number(row, "edge_count");
					body.entry = text(row, "entry");
					body.exit = text(row, "exit");
					body.state = control_flow_state::complete;
					if (text(row, "eligibility") != "closed")
						gap(body,
							"sdk.cfg-body-not-closed",
							text(row, "reason").value_or("body-eligibility-required"));
					if (!body.declared_nodes || !body.declared_edges)
						gap(body,
							"sdk.cfg-cardinality-unavailable",
							"declared-node-and-edge-counts-required");
				}
			}
			for (std::size_t ref{}; ref < entries.size(); ++ref)
			{
				checkpoint(cancellation, operations);
				const auto& item = entries[ref];
				const auto& row = *item.row;
				if (item.kind != 1U)
					continue;
				const auto id = text(row, "node"), owner = text(row, "body");
				if (!id || !owner)
					fail("sdk.cfg-input-invalid", "node", "subject-body-required");
				for (const auto& variant : row.presence.fragments)
				{
					checkpoint(cancellation, operations);
					const auto subject = world_key(*owner, row, variant);
					const auto found = bodies.find(subject);
					if (found == bodies.end())
					{
						output.unresolved.push_back({"sdk.cfg-body-missing", *id, *owner});
						continue;
					}
					auto& work = found->second;
					auto& body = work.value;
					node_owners[world_key(*id, row, variant)].insert(subject);
					if (text(row, "compile_unit") != body.compile_unit ||
						text(row, "function") != body.function)
						gap(body, "sdk.cfg-node-owner-mismatch", *id);
					const auto candidate = payload(row);
					const auto existing = work.nodes.find(*id);
					if (existing != work.nodes.end())
					{
						body.nodes[existing->second.first].evidence.push_back(ref);
						if (existing->second.second != candidate)
							gap(body, "sdk.cfg-node-conflicting", *id, true);
						continue;
					}
					if (body.nodes.size() >= limits.maximum_body_nodes)
						fail("sdk.cfg-budget", "body-nodes", body.id);
					work.nodes.emplace(*id, std::pair{body.nodes.size(), candidate});
					body.nodes.push_back({*id,
										  text(row, "kind").value_or(""),
										  number(row, "ordinal"),
										  text(row, "source"),
										  number(row, "statement_count"),
										  {ref},
										  text(row, "terminator"),
										  text(row, "terminator_state"),
										  text(row, "terminator_profile")});
				}
			}
			for (const auto& [node, owners] : node_owners)
			{
				checkpoint(cancellation, operations);
				if (owners.size() > 1U)
					for (const auto& owner : owners)
					{
						checkpoint(cancellation, operations);
						gap(bodies.at(owner).value,
							"sdk.cfg-node-owner-conflicting",
							node[0U],
							true);
					}
			}
			std::map<function_key, std::vector<key>> function_bodies;
			for (const auto& [subject, work] : bodies)
				function_bodies[{work.value.function,
								 work.value.compile_unit,
								 work.value.universe,
								 work.value.variant,
								 work.value.interpretation}]
					.push_back(subject);
			std::map<function_key, std::size_t> orphan_edges;
			for (std::size_t ref{}; ref < entries.size(); ++ref)
			{
				checkpoint(cancellation, operations);
				const auto& item = entries[ref];
				const auto& row = *item.row;
				if (item.kind != 2U)
					continue;
				const auto id = text(row, "edge"), from = text(row, "from");
				if (!id || !from)
					fail("sdk.cfg-input-invalid", "edge", "subject-source-required");
				for (const auto& variant : row.presence.fragments)
				{
					checkpoint(cancellation, operations);
					const auto source = node_owners.find(world_key(*from, row, variant));
					if (source == node_owners.end() || source->second.size() != 1U)
					{
						output.unresolved.push_back(
							{"sdk.cfg-edge-source-missing-or-ambiguous", *id, *from});
						// A stray edge in a declared function's world is a local gap,
						// even if the other edges happen to match its claimed count.
						if (source == node_owners.end())
						{
							++orphan_edges[{text(row, "function").value_or(""),
											text(row, "compile_unit").value_or(""),
											row.presence.universe,
											variant,
											row.interpretation}];
						}
						// Ambiguous owners were marked once above; every edge still
						// retains its own unresolved item and original evidence row.
						continue;
					}
					const auto& owner = *source->second.begin();
					auto& work = bodies.at(owner);
					auto& body = work.value;
					if (text(row, "compile_unit") != body.compile_unit ||
						text(row, "function") != body.function)
						gap(body, "sdk.cfg-edge-owner-mismatch", *id);
					const auto target_id = text(row, "to");
					const auto target = target_id
						? node_owners.find(world_key(*target_id, row, variant))
						: node_owners.end();
					if (!target_id || target == node_owners.end() || target->second.size() != 1U ||
						*target->second.begin() != owner || text(row, "kind") == "frontier")
						gap(body, "sdk.cfg-edge-frontier", *id);
					const auto candidate = payload(row);
					const auto existing = work.edges.find(*id);
					if (existing != work.edges.end())
					{
						body.edges[existing->second.first].evidence.push_back(ref);
						if (existing->second.second != candidate)
							gap(body, "sdk.cfg-edge-conflicting", *id, true);
						continue;
					}
					if (body.edges.size() >= limits.maximum_body_edges)
						fail("sdk.cfg-budget", "body-edges", body.id);
					work.edges.emplace(*id, std::pair{body.edges.size(), candidate});
					body.edges.push_back({*id,
										  *from,
										  target_id,
										  text(row, "kind").value_or(""),
										  number(row, "ordinal"),
										  {ref},
										  text(row, "condition"),
										  text(row, "condition_state"),
										  text(row, "condition_profile"),
										  text(row, "outcome"),
										  text(row, "outcome_state"),
										  text(row, "outcome_profile"),
										  text(row, "outcome_expression")});
				}
			}
			for (const auto& [function, count] : orphan_edges)
			{
				checkpoint(cancellation, operations);
				const auto candidates = function_bodies.find(function);
				if (candidates != function_bodies.end())
					for (const auto& subject : candidates->second)
					{
						checkpoint(cancellation, operations);
						gap(bodies.at(subject).value,
							"sdk.cfg-edge-source-missing-or-ambiguous",
							"unmatched-edge-count=" + std::to_string(count));
					}
			}
			for (auto& [subject, work] : bodies)
			{
				checkpoint(cancellation, operations);
				(void)subject;
				auto& body = work.value;
				if (!body.declared_nodes || *body.declared_nodes != body.nodes.size())
					gap(body,
						"sdk.cfg-node-cardinality-mismatch",
						"observed=" + std::to_string(body.nodes.size()));
				if (!body.declared_edges || *body.declared_edges != body.edges.size())
					gap(body,
						"sdk.cfg-edge-cardinality-mismatch",
						"observed=" + std::to_string(body.edges.size()));
				if (!body.entry || !body.exit || !work.nodes.contains(*body.entry) ||
					!work.nodes.contains(*body.exit))
					gap(body,
						"sdk.cfg-entry-exit-missing",
						"declared-entry-and-exit-must-be-enumerated");
				std::ranges::sort(body.nodes, {}, &control_flow_node::id);
				std::ranges::sort(body.edges, {}, &control_flow_edge::id);
				canonical_gaps(body.gaps);
				output.bodies.push_back(std::move(body));
			}
			canonical_gaps(output.unresolved);
			checkpoint(cancellation, operations);
			usage = {operations, storage};
			return output;
		}
		catch (const projection_failure& failure)
		{
			return failure.value;
		}
		catch (const std::bad_alloc&)
		{
			return error{"sdk.cfg-resource-exhausted", "projection", "allocation"};
		}
		catch (const std::length_error&)
		{
			return error{"sdk.cfg-resource-exhausted", "projection", "length"};
		}
	}

	static result<control_flow_projection> project_control_flow_queries(
		const application_query_results& input,
		control_flow_limits limits,
		std::stop_token cancellation,
		projection_resource_usage& usage,
		std::size_t maximum_storage = std::numeric_limits<std::size_t>::max())
	{
		if (auto valid = limits.validate(); !valid)
			return valid.error();
		try
		{
			std::size_t operations{}, plan_bytes{}, staging_bytes{};
			checkpoint(cancellation, operations);
			if (auto bounded = detail::check_source_plan_limits(
					input,
					limits.maximum_source_queries,
					std::min(limits.maximum_source_plan_bytes, maximum_storage),
					cancellation,
					"sdk.cfg",
					&plan_bytes,
					[&]()
					{
						checkpoint(cancellation, operations);
						return false;
					});
				!bounded)
				return bounded.error();
			bounded_add(staging_bytes, plan_bytes, maximum_storage, "retained-bytes");
			std::size_t handle_bytes{};
			usage_product(handle_bytes, input.scans.size(), 1024U);
			bounded_add(staging_bytes, handle_bytes, maximum_storage, "retained-bytes");
			std::array<std::vector<const annotated_row*>, 3U> groups;
			const std::array<std::string_view, 3U> ids{
				"cc.body.v1", "cc.cfg_node.v1", "cc.cfg_edge.v1"};
			std::array<bool, 3U> present{};
			std::size_t rows{};
			for (const auto& scan : input.scans)
			{
				checkpoint(cancellation, operations);
				const auto found = std::ranges::find(ids, scan.relation_id);
				if (found == ids.end())
					continue;
				const auto index = static_cast<std::size_t>(found - ids.begin());
				present[index] = true;
				const auto borrowed = query_transfer_access::borrow_rows(scan.result);
				bounded_add(rows, borrowed.size(), limits.maximum_rows, "rows");
				std::size_t pointers{};
				usage_product(pointers, borrowed.size(), sizeof(const annotated_row*));
				bounded_add(staging_bytes, pointers, maximum_storage, "retained-bytes");
				groups[index].reserve(groups[index].size() + borrowed.size());
				for (const auto& row : borrowed)
				{
					checkpoint(cancellation, operations);
					groups[index].push_back(&row);
				}
			}
			projection_resource_usage completed;

			auto projected = project_control_flow_rows(control_flow_input{},
													   limits,
													   cancellation,
													   completed,
													   &groups,
													   operations,
													   staging_bytes,
													   maximum_storage);
			if (!projected)
				return projected;
			for (std::size_t index{}; index < ids.size(); ++index)
				if (!present[index])
				{
					std::size_t gap_bytes{};
					usage_product(gap_bytes, projected->bodies.size() + 1U, 1024U);
					bounded_add(completed.retained_bytes_bound,
								gap_bytes,
								maximum_storage,
								"retained-bytes");
					projected->unresolved.push_back({"sdk.cfg-scan-missing",
													 std::string{ids[index]},
													 "independent-scan-required"});
					for (auto& body : projected->bodies)
						gap(body, "sdk.cfg-scan-missing", std::string{ids[index]});
				}
			// Query-result data remains shared; account for the owned scan handles,
			// relation metadata and final missing-scan gaps without copying rows.
			checkpoint(cancellation, completed.operations);
			projected->source_queries = input;
			usage = completed;
			return projected;
		}
		catch (const projection_failure& failure)
		{
			return failure.value;
		}
		catch (const std::bad_alloc&)
		{
			return error{"sdk.cfg-resource-exhausted", "projection", "allocation"};
		}
		catch (const std::length_error&)
		{
			return error{"sdk.cfg-resource-exhausted", "projection", "length"};
		}
	}
	result<control_flow_projection> project_control_flow(control_flow_input input,
														 control_flow_limits limits,
														 std::stop_token cancellation)
	{
		projection_resource_usage ignored;
		return project_control_flow_rows(input, limits, cancellation, ignored);
	}
	result<control_flow_projection> project_control_flow(const application_query_results& input,
														 control_flow_limits limits,
														 std::stop_token cancellation)
	{
		projection_resource_usage ignored;
		return project_control_flow_queries(input, limits, cancellation, ignored);
	}
	result<control_flow_projection> project_control_flow(control_flow_input input,
														 control_flow_limits limits,
														 std::stop_token cancellation,
														 projection_resource_usage& usage)
	{
		usage = {};
		return project_control_flow_rows(input, limits, cancellation, usage);
	}
	result<control_flow_projection> project_control_flow(const application_query_results& input,
														 control_flow_limits limits,
														 std::stop_token cancellation,
														 projection_resource_usage& usage)
	{
		usage = {};
		return project_control_flow_queries(input, limits, cancellation, usage);
	}

	result<control_flow_projection> project_control_flow(control_flow_input input,
														 control_flow_limits limits,
														 std::stop_token stop,
														 projection_resource_usage& usage,
														 std::size_t maximum_storage)
	{
		usage = {};
		return project_control_flow_rows(
			input, limits, stop, usage, nullptr, 0U, 0U, maximum_storage);
	}
	result<control_flow_projection> project_control_flow(const application_query_results& input,
														 control_flow_limits limits,
														 std::stop_token stop,
														 projection_resource_usage& usage,
														 std::size_t maximum_storage)
	{
		usage = {};
		return project_control_flow_queries(input, limits, stop, usage, maximum_storage);
	}

} // namespace cxxlens::sdk::query
