#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <tuple>

#include <cxxlens/relations/cc_declaration_inventory.hpp>
#include <cxxlens/relations/cc_entity_detail.hpp>
#include <cxxlens/relations/cc_exceptional_block.hpp>
#include <cxxlens/relations/cc_exceptional_exit.hpp>
#include <cxxlens/relations/cc_exceptional_successor.hpp>
#include <cxxlens/sdk.hpp>
#include <cxxlens/sdk/exceptional_routes.hpp>

#include "query_projection_row_copy_controls.hpp"
#include "query_result_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;
	namespace q = cxxlens::sdk::query;
	using state = q::finite_population_state;
	constexpr std::array<std::string_view, 12> names{"build.compile_unit.v1",
													 "source.file.v1",
													 "source.span.v1",
													 "cc.entity.v1",
													 "cc.entity_detail.v1",
													 "cc.body.v1",
													 "cc.syntax_node.v1",
													 "cc.declaration.v1",
													 "cc.declaration_inventory.v1",
													 "cc.exceptional_exit.v1",
													 "cc.exceptional_block.v1",
													 "cc.exceptional_successor.v1"};
	auto descriptors_for_routes()
	{
		const auto base = standard_relation_descriptors();
		std::vector<relation_descriptor> out{base.begin(), base.end()};
		std::erase_if(out,
					  [](const auto& d)
					  {
						  return d.id == "cc.exceptional_exit.v1" ||
							  d.id == "cc.entity_detail.v1" ||
							  d.id == "cc.declaration_inventory.v1";
					  });
		out.push_back(cxxlens::cc::relations::exceptional_exit::descriptor());
		out.push_back(cxxlens::cc::relations::entity_detail::descriptor());
		out.push_back(cxxlens::cc::relations::declaration_inventory::descriptor());
		out.push_back(cxxlens::cc::relations::exceptional_block::descriptor());
		out.push_back(cxxlens::cc::relations::exceptional_successor::descriptor());
		return out;
	}
	void require(bool condition, std::string_view label)
	{
		if (!condition)
		{
			std::cerr << label << '\n';
			std::exit(1);
		}
	}
	template <class T>
	T take(result<T> value)
	{
		if (!value)
		{
			std::cerr << value.error().code << ':' << value.error().field << ':'
					  << value.error().detail << '\n';
			std::exit(1);
		}
		return std::move(*value);
	}
	detached_cell symbols(std::initializer_list<std::string_view> values)
	{
		std::vector<std::byte> encoded;
		for (auto value : values)
		{
			for (unsigned shift = 0; shift < 32; shift += 8)
				encoded.push_back(static_cast<std::byte>((value.size() >> shift) & 255));
			for (char c : value)
				encoded.push_back(static_cast<std::byte>(c));
		}
		return detached_cell::bytes(std::move(encoded));
	}
	q::annotated_row fact(std::size_t group,
						  std::initializer_list<std::pair<std::string, detached_cell>> values)
	{
		q::annotated_row row;
		row.presence = {"calls:test", {"debug"}};
		row.interpretation = "clang22";
		row.claim_contributors = {"claim:test"};
		row.producer_contracts = {{"calls.test", "semantic:fixture"}};
		row.provenance = {"calls:evidence"};
		row.contributor_guarantees = {{"exact", "finite-call", "fixture", {"schema_validated"}}};
		row.contributor_edges = {{row.claim_contributors.front(),
								  row.producer_contracts.front(),
								  row.provenance.front(),
								  row.contributor_guarantees.front(),
								  row.presence,
								  row.interpretation}};
		const auto descriptors = descriptors_for_routes();
		auto descriptor = std::ranges::find(descriptors, names[group], &relation_descriptor::id);
		require(descriptor != descriptors.end(), "fixture descriptor missing");
		for (const auto& column : descriptor->columns)
		{
			auto cell = detached_cell::utf8("fixture");
			if (column.type.optional)
				cell = detached_cell::absent(column.type);
			else if (column.type.scalar == scalar_kind::boolean)
				cell = detached_cell::boolean(false);
			else if (column.type.scalar == scalar_kind::unsigned_integer)
				cell = detached_cell::unsigned_integer(0);
			else if (column.type.scalar == scalar_kind::digest)
				cell = detached_cell::utf8(content_digest({}));
			else if (column.type.scalar == scalar_kind::set ||
					 column.type.scalar == scalar_kind::bytes)
				cell = detached_cell::bytes({});
			else if (column.type.scalar == scalar_kind::closed_symbol)
				cell = detached_cell::utf8("canonicalized");
			cell.type = column.type;
			row.values.emplace("output." + column.name, std::move(cell));
		}
		for (const auto& [name, cell] : values)
		{
			auto copy = cell;
			copy.type = row.values.at("output." + name).type;
			row.values["output." + name] = std::move(copy);
		}
		return row;
	}
	void set(q::annotated_row& row, std::string_view name, detached_cell value)
	{
		const std::string key = "output." + std::string{name};
		value.type = row.values.at(key).type;
		row.values[key] = std::move(value);
	}
	detached_cell binary(std::string_view value)
	{
		std::vector<std::byte> out;
		for (char c : value)
			out.push_back(static_cast<std::byte>(c));
		return detached_cell::bytes(std::move(out));
	}

	struct fixture
	{
		std::array<std::vector<q::annotated_row>, 12> rows;
		fixture()
		{
			rows[0] = {fact(0, {{"compile_unit", detached_cell::utf8("unit:a")}})};
			rows[1] = {fact(1,
							{{"snapshot", detached_cell::utf8("snapshot:a")},
							 {"file", detached_cell::utf8("file:a")},
							 {"size", detached_cell::unsigned_integer(100)}})};
			for (auto name : {"span:definition", "span:body"})
				rows[2].push_back(fact(2,
									   {{"span", detached_cell::utf8(name)},
										{"snapshot", detached_cell::utf8("snapshot:a")},
										{"file", detached_cell::utf8("file:a")},
										{"begin", detached_cell::unsigned_integer(0)},
										{"end", detached_cell::unsigned_integer(80)}}));
			rows[3] = {fact(3,
							{{"entity", detached_cell::utf8("function:a")},
							 {"kind", detached_cell::utf8("function")}})};
			rows[4] = {fact(4,
							{{"detail", detached_cell::utf8("detail:a")},
							 {"entity", detached_cell::utf8("function:a")},
							 {"compile_unit", detached_cell::utf8("unit:a")},
							 {"source", detached_cell::utf8("span:definition")},
							 {"exception_spec_kind", detached_cell::utf8("basic_noexcept")},
							 {"exception_spec_nonthrowing", detached_cell::boolean(true)},
							 {"exception_spec_state", detached_cell::utf8("complete")},
							 {"exception_spec_profile",
							  detached_cell::utf8("clang22-function-exception-specification/1")}})};
			rows[5] = {fact(5,
							{{"body", detached_cell::utf8("body:a")},
							 {"function", detached_cell::utf8("function:a")},
							 {"compile_unit", detached_cell::utf8("unit:a")},
							 {"source", detached_cell::utf8("span:body")}})};
			rows[7] = {fact(7,
							{{"declaration", detached_cell::utf8("decl:function")},
							 {"entity", detached_cell::utf8("function:a")},
							 {"source", detached_cell::utf8("span:definition")},
							 {"kind", detached_cell::utf8("Function")}})};
			rows[8] = {fact(
				8,
				{{"inventory", detached_cell::utf8("inventory:a")},
				 {"compile_unit", detached_cell::utf8("unit:a")},
				 {"profile", detached_cell::utf8("clang22-explicit-admitted-named-declarations/1")},
				 {"declaration_count", detached_cell::unsigned_integer(1)},
				 {"declarations", symbols({"decl:function"})},
				 {"enumeration_state", detached_cell::utf8("complete")}})};
			auto carrier = fact(
				9,
				{{"exit", detached_cell::utf8("variant:a")},
				 {"scope_detail", detached_cell::utf8("detail:a")},
				 {"function", detached_cell::utf8("function:a")},
				 {"compile_unit", detached_cell::utf8("unit:a")},
				 {"definition_source", detached_cell::utf8("span:definition")},
				 {"body", detached_cell::utf8("body:a")},
				 {"variant_kind", detached_cell::utf8("function")},
				 {"variant_index", detached_cell::unsigned_integer(0)},
				 {"variant_symbol", binary(std::string_view{"f\0\xff", 3})},
				 {"ordinal", detached_cell::unsigned_integer(0)},
				 {"role", detached_cell::utf8("lowering_variant")},
				 {"eligibility", detached_cell::utf8("excluded")},
				 {"profile", detached_cell::utf8("clang22-original-exceptional-occurrences/1")},
				 {"lowering_profile",
				  detached_cell::utf8("clang22-written-definition-analysis-lowering/1")},
				 {"lowered_entry", detached_cell::utf8("block:entry")},
				 {"lowered_block_count", detached_cell::unsigned_integer(3)},
				 {"lowered_block_ids", symbols({"block:entry", "block:normal", "block:unwind"})},
				 {"lowered_successor_count", detached_cell::unsigned_integer(2)},
				 {"lowered_successor_ids", symbols({"edge:normal", "edge:unwind"})},
				 {"lowered_topology_state", detached_cell::utf8("complete")},
				 {"lowered_topology_profile",
				  detached_cell::utf8("clang22-original-lowering-topology/1")}});
			auto invoke = carrier;
			for (auto name : {"lowered_entry",
							  "lowered_block_count",
							  "lowered_block_ids",
							  "lowered_successor_count",
							  "lowered_successor_ids",
							  "lowered_topology_state",
							  "lowered_topology_profile"})
				invoke.values.erase("output." + std::string{name});
			set(invoke, "exit", detached_cell::utf8("exit:invoke"));
			set(invoke, "variant", detached_cell::utf8("variant:a"));
			set(invoke, "ordinal", detached_cell::unsigned_integer(1));
			set(invoke, "role", detached_cell::utf8("escaping_call"));
			set(invoke, "is_invoke", detached_cell::boolean(true));
			set(invoke, "block_ordinal", detached_cell::unsigned_integer(0));
			set(invoke, "instruction_ordinal", detached_cell::unsigned_integer(0));
			set(invoke, "lowered_block", detached_cell::utf8("block:entry"));
			set(invoke, "normal_successor", detached_cell::utf8("edge:normal"));
			set(invoke, "unwind_successor", detached_cell::utf8("edge:unwind"));
			set(invoke, "eh_boundary_declaration", detached_cell::utf8("decl:function"));
			set(invoke, "eh_selected_scope_kind", detached_cell::utf8("terminate"));
			set(invoke, "eh_disposition", detached_cell::utf8("direct_function_spec_termination"));
			set(invoke, "eh_boundary_state", detached_cell::utf8("complete"));
			set(invoke,
				"eh_boundary_profile",
				detached_cell::utf8("clang22-original-invoke-eh-boundary/1"));
			rows[9] = {carrier, invoke};
			for (std::uint64_t i = 0; i < 3; ++i)
				rows[10].push_back(fact(
					10,
					{{"block",
					  detached_cell::utf8(i == 0	   ? "block:entry"
											  : i == 1 ? "block:normal"
													   : "block:unwind")},
					 {"variant", detached_cell::utf8("variant:a")},
					 {"compile_unit", detached_cell::utf8("unit:a")},
					 {"ordinal", detached_cell::unsigned_integer(i)},
					 {"instruction_count", detached_cell::unsigned_integer(1)},
					 {"is_entry", detached_cell::boolean(i == 0)},
					 {"terminator_kind",
					  detached_cell::utf8(i == 0	   ? "invoke"
											  : i == 1 ? "ret"
													   : "resume")},
					 {"membership_state", detached_cell::utf8("complete")},
					 {"profile", detached_cell::utf8("clang22-original-lowering-topology/1")}}));
			for (std::uint64_t i = 0; i < 2; ++i)
				rows[11].push_back(fact(
					11,
					{{"successor", detached_cell::utf8(i == 0 ? "edge:normal" : "edge:unwind")},
					 {"variant", detached_cell::utf8("variant:a")},
					 {"compile_unit", detached_cell::utf8("unit:a")},
					 {"from_block", detached_cell::utf8("block:entry")},
					 {"to_block", detached_cell::utf8(i == 0 ? "block:normal" : "block:unwind")},
					 {"terminator_instruction_ordinal", detached_cell::unsigned_integer(0)},
					 {"ordinal", detached_cell::unsigned_integer(i)},
					 {"kind", detached_cell::utf8(i == 0 ? "normal" : "unwind")},
					 {"invoke", detached_cell::utf8("exit:invoke")},
					 {"membership_state", detached_cell::utf8("complete")},
					 {"profile", detached_cell::utf8("clang22-original-lowering-topology/1")}}));
		}
		q::exceptional_route_input input() const
		{
			return {rows[0],
					rows[1],
					rows[2],
					rows[3],
					rows[4],
					rows[5],
					rows[6],
					rows[7],
					rows[8],
					rows[9],
					rows[10],
					rows[11],
					true,
					true,
					true,
					true,
					true};
		}
		q::application_query_results queries(bool sizes = false) const
		{
			q::application_query_results out;
			out.snapshot_id = "query:routes";
			for (std::size_t i = 0; i < rows.size(); ++i)
			{
				auto data = std::make_shared<q::query_result::data>();
				data->row_values = rows[i];
				if (sizes)
				{
					for (const auto& row : data->row_values)
					{
						require(bool(row.validate()), "sizing fixture generic row admission");
						std::ostringstream multiplicity;
						multiplicity << row.multiplicity;
						data->row_wire_base_sizes.push_back(row.canonical_form().size() -
															multiplicity.str().size());
					}
					data->rows_validated = true;
				}
				data->status = q::execution_status::complete;
				data->input_complete = false;
				data->snapshot = out.snapshot_id;
				out.scans.push_back(
					{std::string{names[i]}, {}, q::query_transfer_access::make(data)});
			}
			return out;
		}
	};
	const auto& variant(const q::exceptional_route_projection& out)
	{
		require(out.variants.size() == 1, "one physical lowering variant");
		return out.variants.front();
	}
	const auto& invoke(const q::exceptional_route_projection& out)
	{
		require(out.invokes.size() == 1, "one actual Invoke");
		return out.invokes.front();
	}
	void ordered_successor_evidence_controls()
	{
		for (unsigned disposition{}; disposition < 10U; ++disposition)
		{
			fixture original;
			if (disposition == 1U)
				original.rows[11].erase(original.rows[11].begin());
			if (disposition == 2U)
				set(original.rows[11][0], "kind", detached_cell::utf8("unwind"));
			if (disposition == 3U)
				set(original.rows[9][1], "is_invoke", detached_cell::boolean(false));
			if (disposition == 4U)
				set(original.rows[9][1], "unwind_successor", detached_cell::utf8("edge:normal"));
			if (disposition == 5U)
				set(original.rows[11][0], "to_block", detached_cell::utf8("block:dangling"));
			if (disposition == 6U)
				set(original.rows[11][0], "compile_unit", detached_cell::utf8("unit:foreign"));
			if (disposition == 7U)
				set(original.rows[11][1], "ordinal", detached_cell::unsigned_integer(0U));
			if (disposition == 8U)
				set(original.rows[11][0], "membership_state", detached_cell::utf8("partial"));
			if (disposition == 9U)
				original.rows[11].clear();
			const auto out = take(q::project_exceptional_routes(original.input()));
			const auto& value = invoke(out);
			const auto expected_state = disposition == 0U ? state::complete
				: disposition == 9U						  ? state::unknown
				: disposition == 1U || disposition == 3U || disposition == 5U || disposition == 8U
				? state::partial
				: state::conflicting;
			require(value.successors_state == expected_state &&
						value.identity_state == state::complete &&
						value.placement_state == state::complete &&
						value.boundary_state == state::complete &&
						value.boundary_declaration_state == state::complete &&
						value.exception_spec_state == state::complete &&
						value.exception_spec_nonthrowing == true,
					"ordered successors preserve unknown/conflict/partial and independent Invoke "
					"facets");
			require(out.compile_units_complete && out.scope_inputs_complete &&
						out.occurrence_inputs_complete && out.topology_inputs_complete &&
						out.declaration_inputs_complete && out.unresolved.empty() &&
						!out.source_queries,
					"ordered successor evaluation preserves source coverage and query ownership");
			require(value.gaps.size() == (expected_state == state::complete ? 0U : 1U) &&
						(expected_state == state::complete ||
						 value.gaps.front().code ==
							 "sdk.exceptional-route-invoke-successors-unavailable"),
					"ordered successors retain the precise missing/conflicting facet gap");
			using original_reference = std::pair<std::size_t, std::size_t>;
			std::vector<original_reference> expected{{10U, 0U}};
			if (disposition != 3U && disposition != 9U)
			{
				if (disposition != 1U)
				{
					expected.emplace_back(11U, 0U);
					if (disposition != 2U && disposition != 6U)
					{
						expected.emplace_back(10U, 0U);
						if (disposition != 5U)
							expected.emplace_back(10U, 1U);
						expected.emplace_back(10U, 0U);
					}
				}
				expected.emplace_back(11U, disposition == 1U || disposition == 4U ? 0U : 1U);
				if (disposition != 4U && disposition != 7U)
					expected.insert(expected.end(), {{10U, 0U}, {10U, 2U}, {10U, 0U}});
			}
			std::vector<std::size_t> actual;
			for (const auto reference : value.evidence)
			{
				require(reference < out.evidence.size(), "Invoke evidence index remains in owner");
				const auto& evidence = out.evidence[reference];
				if (evidence.relation_id == names[10] || evidence.relation_id == names[11])
					actual.push_back(reference);
			}
			require(actual.size() == expected.size(),
					"ordered successor/block FKs preserve every original reference and duplicate");
			for (std::size_t i{}; i < expected.size(); ++i)
			{
				const auto [group, row] = expected[i];
				const auto& evidence = out.evidence[actual[i]];
				require(
					evidence.relation_id == names[group] &&
						evidence.row.canonical_form() == original.rows[group][row].canonical_form(),
					"normal successor FKs precede unwind successor FKs in exact evidence order");
			}
		}
	}
	auto full_route_record(const q::observed_exceptional_block& value)
	{
		return std::tie(value.block,
						value.variant,
						value.compile_unit,
						value.profile,
						value.membership_state,
						value.universe,
						value.semantic_variant,
						value.interpretation,
						value.ordinal,
						value.instruction_count,
						value.is_entry,
						value.terminator_opcode,
						value.terminator_kind,
						value.identity_state,
						value.variant_state,
						value.state,
						value.evidence,
						value.gaps);
	}
	auto full_route_record(const q::observed_exceptional_successor& value)
	{
		return std::tie(value.successor,
						value.variant,
						value.compile_unit,
						value.profile,
						value.membership_state,
						value.universe,
						value.semantic_variant,
						value.interpretation,
						value.from_block,
						value.to_block,
						value.kind,
						value.invoke,
						value.terminator_instruction_ordinal,
						value.ordinal,
						value.identity_state,
						value.variant_state,
						value.from_state,
						value.to_state,
						value.invoke_state,
						value.state,
						value.evidence,
						value.gaps);
	}
	auto full_route_record(const q::observed_invoke_exceptional_boundary& value)
	{
		return std::tie(value.exit,
						value.variant,
						value.compile_unit,
						value.lowered_block,
						value.normal_successor,
						value.unwind_successor,
						value.universe,
						value.semantic_variant,
						value.interpretation,
						value.block_ordinal,
						value.instruction_ordinal,
						value.is_invoke,
						value.boundary_declaration,
						value.selected_scope_kind,
						value.disposition,
						value.boundary_profile,
						value.boundary_observation_state,
						value.exception_spec_kind,
						value.exception_spec_profile,
						value.exception_spec_nonthrowing,
						value.identity_state,
						value.placement_state,
						value.successors_state,
						value.boundary_state,
						value.boundary_declaration_state,
						value.exception_spec_state,
						value.evidence,
						value.gaps);
	}
	auto full_route_record(const q::exceptional_route_variant& value)
	{
		return std::tie(value.carrier,
						value.kind,
						value.symbol,
						value.index,
						value.detail,
						value.function,
						value.compile_unit,
						value.body,
						value.definition_source,
						value.universe,
						value.variant,
						value.interpretation,
						value.profile,
						value.entry,
						value.block_count,
						value.successor_count,
						value.block_ids,
						value.successor_ids,
						value.carrier_state,
						value.scope_state,
						value.source_state,
						value.enumeration_state,
						value.entry_state,
						value.topology_state,
						value.blocks,
						value.successors,
						value.invokes,
						value.evidence,
						value.gaps);
	}

	void same_routes(const q::exceptional_route_projection& expected,
					 const q::exceptional_route_projection& actual,
					 bool query_owners = true)
	{
		require(std::tie(expected.compile_units_complete,
						 expected.scope_inputs_complete,
						 expected.occurrence_inputs_complete,
						 expected.topology_inputs_complete,
						 expected.declaration_inputs_complete,
						 expected.unresolved) ==
					std::tie(actual.compile_units_complete,
							 actual.scope_inputs_complete,
							 actual.occurrence_inputs_complete,
							 actual.topology_inputs_complete,
							 actual.declaration_inputs_complete,
							 actual.unresolved),
				"immutable Routes preserve full independent coverage and unresolved fields");
		const auto records = [](const auto& left, const auto& right)
		{
			require(left.size() == right.size(), "immutable Routes retain every flat record");
			for (std::size_t i{}; i < left.size(); ++i)
				require(
					full_route_record(left[i]) == full_route_record(right[i]),
					"immutable Routes retain every DTO field, gap and ordered evidence reference");
		};
		records(expected.blocks, actual.blocks);
		records(expected.successors, actual.successors);
		records(expected.invokes, actual.invokes);
		records(expected.variants, actual.variants);
		require(expected.evidence.size() == actual.evidence.size(),
				"immutable Routes retain every raw evidence owner");
		for (std::size_t i{}; i < expected.evidence.size(); ++i)
			require(expected.evidence[i].relation_id == actual.evidence[i].relation_id &&
						expected.evidence[i].original_row().canonical_form() ==
							actual.evidence[i].original_row().canonical_form(),
					"immutable Routes retain every cell and complete annotation in evidence order");
		if (!query_owners)
			return;
		require(bool(expected.source_queries) == bool(actual.source_queries),
				"immutable Routes preserve full query-owner presence");
		if (!expected.source_queries)
			return;
		require(expected.source_queries->snapshot_id == actual.source_queries->snapshot_id &&
					expected.source_queries->scans.size() == actual.source_queries->scans.size(),
				"immutable Routes preserve the complete query set");
		for (std::size_t i{}; i < expected.source_queries->scans.size(); ++i)
		{
			const auto& left = expected.source_queries->scans[i];
			const auto& right = actual.source_queries->scans[i];
			require(left.relation_id == right.relation_id && left.logical_ir == right.logical_ir &&
						left.result.canonical_form() == right.result.canonical_form(),
					"immutable Routes preserve every original query side channel");
		}
	}
	void immutable_evidence_controls()
	{
		q::finite_population_limits shared;
		shared.evidence_ownership = q::projection_evidence_ownership::shared_immutable;
		fixture original;
		set(original.rows[3].front(),
			"provider_local_key",
			detached_cell::bytes(std::vector<std::byte>(32768U, std::byte{0xff})));
		for (auto& group : original.rows)
			for (auto& row : group)
			{
				row.claim_contributors.push_back("claim:z");
				row.producer_contracts.push_back({"z.projected", "semantic:z"});
				row.provenance.push_back("zz:evidence");
				row.contributor_guarantees.push_back(
					{"exact", "zz", "zz", {"native", "schema_validated"}});
				row.contributor_edges.push_back({row.claim_contributors.back(),
												 row.producer_contracts.back(),
												 row.provenance.back(),
												 row.contributor_guarantees.back(),
												 row.presence,
												 row.interpretation});
			}
		auto input = original.queries(true);
		for (auto& scan : input.scans)
		{
			const auto owner = q::query_transfer_access::borrow_evidence_owner(scan.result);
			auto data = std::make_shared<q::query_result::data>(*owner.owner);
			data->closures = {"original-closure"};
			data->unresolved = {{"original-gap", "original-subject", "original-reason"}};
			data->logical = {"original-logical", "original-logical-text"};
			data->physical = {"original-physical", "original-physical-text"};
			data->ir_digest = "original-query-digest";
			data->publication = "original-publication";
			scan.result = q::query_transfer_access::make(std::move(data));
		}
		q::projection_resource_usage detached_usage, shared_usage;
		auto detached = take(q::project_exceptional_routes(input, {}, {}, detached_usage));
		auto alias = take(q::project_exceptional_routes(input, shared, {}, shared_usage));
		same_routes(detached, alias);
		require(shared_usage.operations < detached_usage.operations,
				"shared immutable rows avoid the actual full raw payload clone");
		std::size_t evidence_bytes{};
		for (std::size_t i{}; i < alias.evidence.size(); ++i)
		{
			const auto& evidence = alias.evidence[i];
			const auto group = std::ranges::find(names, evidence.relation_id);
			require(group != names.end() && evidence.row.values.empty(),
					"query-result opt-in keeps the detached field empty without cloning");
			bool exact_owner{};
			for (const auto& scan : input.scans)
				if (scan.relation_id == evidence.relation_id)
					for (const auto& row : q::query_transfer_access::borrow_rows(scan.result))
						exact_owner |= &evidence.original_row() == &row;
			require(exact_owner, "shared evidence aliases the exact immutable original address");
			require(!detached.evidence[i].row.values.empty() &&
						&detached.evidence[i].original_row() == &detached.evidence[i].row,
					"default evidence keeps a complete mutable detached copy");
			evidence_bytes += evidence.original_row().canonical_form().size();
		}
		q::projection_resource_usage raw_default_usage, raw_shared_usage;
		const auto raw_default =
			take(q::project_exceptional_routes(original.input(), {}, {}, raw_default_usage));
		const auto raw_shared =
			take(q::project_exceptional_routes(original.input(), shared, {}, raw_shared_usage));
		same_routes(raw_default, raw_shared);
		same_routes(raw_default, alias, false);
		require(raw_default_usage.operations == raw_shared_usage.operations &&
					raw_default_usage.retained_bytes_bound == raw_shared_usage.retained_bytes_bound,
				"raw-span opt-in preserves the entire detached fallback and original usage");
		for (const auto& evidence : raw_shared.evidence)
			require(&evidence.original_row() == &evidence.row && !evidence.row.values.empty(),
					"raw-span inputs never acquire immutable aliases");
		for (unsigned bound{}; bound < 3U; ++bound)
			for (bool under : {false, true})
			{
				auto exact = shared;
				if (bound == 0U)
					exact.maximum_operations =
						shared_usage.operations - static_cast<std::size_t>(under);
				else if (bound == 1U)
					exact.maximum_retained_bytes =
						shared_usage.retained_bytes_bound - static_cast<std::size_t>(under);
				else
					exact.maximum_evidence_bytes = evidence_bytes - static_cast<std::size_t>(under);
				q::projection_resource_usage usage{1U, 1U};
				const auto bounded = q::project_exceptional_routes(input, exact, {}, usage);
				require(bool(bounded) == !under,
						"shared evidence exact/one-under work/storage/evidence bounds");
				if (under)
					require(!usage.operations && !usage.retained_bytes_bound,
							"shared evidence failed admission revokes output and usage");
				else
					same_routes(detached, *bounded);
			}
		std::vector<std::weak_ptr<const q::query_result::data>> owners;
		for (const auto& scan : input.scans)
			owners.push_back(q::query_transfer_access::borrow_evidence_owner(scan.result).owner);
		alias = {};
		detached = {};
		bool saw_bound_alias{};
		auto interrupt = shared;
		interrupt.cancelled = [&]
		{
			for (const auto& owner : owners)
				if (owner.use_count() > 2)
					return saw_bound_alias = true;
			return false;
		};
		q::projection_resource_usage spent{1U, 1U};
		const auto stopped = q::project_exceptional_routes(input, interrupt, {}, spent);
		require(!stopped && saw_bound_alias &&
					stopped.error().code == "sdk.exceptional-route-cancelled" &&
					!spent.operations && !spent.retained_bytes_bound,
				"actual partial alias ownership is revoked on mid-binding cancellation");
		for (const auto& owner : owners)
			require(owner.use_count() == 1,
					"failed shared projection releases every view and partial alias");
		std::stop_source pre;
		pre.request_stop();
		require(!q::project_exceptional_routes(input, shared, pre.get_token(), spent) &&
					!spent.operations && !spent.retained_bytes_bound,
				"shared evidence pre-stop preserves the existing failure contract");
		alias = take(q::project_exceptional_routes(input, shared));
		auto expected = take(q::project_exceptional_routes(input));
		std::vector<const q::annotated_row*> exact_addresses;
		for (const auto& evidence : alias.evidence)
			exact_addresses.push_back(&evidence.original_row());
		auto copied = alias;
		copied.source_queries.reset();
		alias.source_queries.reset();
		input = {};
		auto moved = std::move(alias);
		same_routes(expected, moved, false);
		same_routes(expected, copied, false);
		for (std::size_t i{}; i < moved.evidence.size(); ++i)
			require(&moved.evidence[i].original_row() == exact_addresses[i] &&
						&copied.evidence[i].original_row() == exact_addresses[i],
					"copied/moved shared evidence owns original rows independently of "
					"input/source_queries");
		// Only the independent detached oracle is needed after this point.
		expected.source_queries.reset();
		moved = {};
		copied = {};
		for (const auto& owner : owners)
			require(owner.expired(), "last evidence owner releases the immutable query backing");
		{
			auto repeated = original.queries();
			require(q::query_transfer_access::borrow_rows(repeated.scans[6].result).empty(),
					"view growth fixture has an empty independently admitted syntax scan");
			for (unsigned i{}; i < 7U; ++i)
				repeated.scans.push_back(repeated.scans[6]);
			std::weak_ptr<const q::query_result::data> owner =
				q::query_transfer_access::borrow_evidence_owner(repeated.scans[6].result).owner;
			const auto baseline_owners = owner.use_count();
			auto before_growth = shared;
			std::size_t growth_checkpoints{};
			before_growth.cancelled = [&]
			{
				return owner.use_count() >= baseline_owners + 2 && ++growth_checkpoints == 3U;
			};
			const auto cancelled =
				q::project_exceptional_routes(repeated, before_growth, {}, spent);
			require(!cancelled && growth_checkpoints == 3U &&
						cancelled.error().code == "sdk.exceptional-route-cancelled" &&
						!spent.operations && !spent.retained_bytes_bound &&
						owner.use_count() == baseline_owners,
					"stop at the next owner-view growth revokes existing capacity and handles");
			const auto repeated_default = take(q::project_exceptional_routes(repeated));
			q::projection_resource_usage grown;
			const auto repeated_shared =
				take(q::project_exceptional_routes(repeated, shared, {}, grown));
			same_routes(repeated_default, repeated_shared);
			for (const bool under : {false, true})
			{
				auto bounded = shared;
				bounded.maximum_retained_bytes =
					grown.retained_bytes_bound - static_cast<std::size_t>(under);
				const auto result = q::project_exceptional_routes(repeated, bounded, {}, spent);
				require(bool(result) == !under,
						"real repeated scan view capacity obeys exact/one-under storage admission");
				if (under)
					require(!spent.operations && !spent.retained_bytes_bound,
							"failed grown-view admission revokes all output and usage");
			}
		}
		// Direct exact-membership negatives cannot acquire ownership from equal detached values.
		{
			const auto source = original.queries();
			const auto view =
				q::query_transfer_access::borrow_evidence_owner(source.scans.front().result);
			auto equal_copy = view.rows.front();
			std::size_t work{};
			const auto miss =
				q::query_transfer_access::share_evidence_row<q::finite_population_evidence>(
					view,
					&equal_copy,
					names.front(),
					[&](std::size_t amount)
					{
						work += amount;
					});
			require(!miss && work > 0U,
					"equal detached row cannot impersonate the immutable owner");
			auto foreign_span = view;
			foreign_span.rows = std::span<const q::annotated_row>{&equal_copy, 1U};
			require(!q::query_transfer_access::share_evidence_row<q::finite_population_evidence>(
						foreign_span,
						&equal_copy,
						names.front(),
						[](std::size_t)
						{
						}),
					"foreign span cannot borrow an unrelated immutable owner");
		}
		for (unsigned disposition{}; disposition < 13U; ++disposition)
		{
			fixture fault;
			if (disposition == 1U)
				fault.rows[10].push_back(fault.rows[10].front());
			if (disposition == 2U)
			{
				fault.rows[10].push_back(fault.rows[10].front());
				set(fault.rows[10].back(),
					"instruction_count",
					detached_cell::unsigned_integer(999U));
			}
			if (disposition == 3U)
				set(fault.rows[11].front(), "compile_unit", detached_cell::utf8("unit:foreign"));
			if (disposition == 4U)
				set(fault.rows[11].front(), "to_block", detached_cell::utf8("block:missing"));
			if (disposition == 5U)
				fault.rows[11].clear();
			if (disposition == 6U)
				fault.rows[10].front().values.erase("output.is_entry");
			if (disposition == 7U)
				fault.rows[11].back().values["output.kind"].value = std::string{"\xc0\x80", 2U};
			if (disposition == 8U)
				set(fault.rows[11].back(), "ordinal", detached_cell::utf8("wrong-type"));
			if (disposition == 9U)
			{
				fault.rows[11].back().presence.universe = "world:foreign";
				fault.rows[11].back().contributor_edges.front().condition.universe =
					"world:foreign";
			}
			if (disposition == 10U)
				fault.rows[6] = {fact(6, {{"node", detached_cell::utf8("unused:node")}})};
			if (disposition == 10U)
				fault.rows[6].front().values["output.extra"] =
					detached_cell::utf8("late-malformed");
			if (disposition == 11U)
			{
				fault.rows[11].back().presence.fragments = {"variant:foreign"};
				fault.rows[11].back().contributor_edges.front().condition.fragments =
					fault.rows[11].back().presence.fragments;
			}
			if (disposition == 12U)
			{
				fault.rows[11].back().interpretation = "interpretation:foreign";
				fault.rows[11].back().contributor_edges.front().interpretation =
					fault.rows[11].back().interpretation;
			}
			const auto query = fault.queries();
			const auto baseline = q::project_exceptional_routes(query);
			const auto candidate = q::project_exceptional_routes(query, shared);
			require(bool(baseline) == bool(candidate),
					"shared mode keeps all late/raw admission outcomes");
			if (baseline)
				same_routes(*baseline, *candidate);
			else
				require(std::tie(baseline.error().code,
								 baseline.error().field,
								 baseline.error().detail) ==
							std::tie(candidate.error().code,
									 candidate.error().field,
									 candidate.error().detail),
						"shared mode retains the exact original malformed-input error");
		}
	}

} // namespace

int main()
{
	ordered_successor_evidence_controls();
	immutable_evidence_controls();
	{
		fixture sized;
		query_copy_controls::projection(
			sized.rows,
			names,
			[&]
			{
				return sized.queries(true);
			},
			[](const auto& input, auto limits, auto& usage)
			{
				return q::project_exceptional_routes(input, limits, {}, usage);
			},
			require);
		std::size_t calls{};
		q::finite_population_limits measured;
		measured.cancelled = [&]
		{
			++calls;
			return false;
		};
		const auto admitted = sized.queries(true);
		require(bool(q::project_exceptional_routes(admitted, measured)),
				"immutable sizing current callback census");
		q::finite_population_limits interrupted;
		std::size_t visited{};
		interrupted.cancelled = [&]
		{
			return ++visited >= calls / 2U;
		};
		q::projection_resource_usage spent;
		const auto stopped = q::project_exceptional_routes(admitted, interrupted, {}, spent);
		require(!stopped && !spent.operations && !spent.retained_bytes_bound,
				"immutable sizing real stop revokes all usage");
		require(bool(q::project_exceptional_routes(admitted, {}, {}, spent)),
				"immutable sizing fresh retry");
	}
	// Shared span IDs deliberately hash to one bucket; worlds still stay distinct.
	{
		fixture indexed;
		auto alternative = indexed.rows[2].front();
		alternative.provenance = {"provenance:alternative-span"};
		alternative.contributor_edges.front().provenance = alternative.provenance.front();
		indexed.rows[2].push_back(std::move(alternative));
		for (const unsigned axis : {0U, 1U, 2U})
		{
			auto foreign = indexed.rows[2].front();
			if (axis == 0U)
			{
				foreign.presence.universe = "world:foreign";
				foreign.contributor_edges.front().condition.universe = foreign.presence.universe;
			}
			else if (axis == 1U)
			{
				foreign.presence.fragments = {"variant:foreign"};
				foreign.contributor_edges.front().condition.fragments = foreign.presence.fragments;
			}
			else
			{
				foreign.interpretation = "interpretation:foreign";
				foreign.contributor_edges.front().interpretation = foreign.interpretation;
			}
			set(foreign, "begin", detached_cell::unsigned_integer(99U));
			set(foreign, "end", detached_cell::unsigned_integer(100U));
			indexed.rows[2].push_back(std::move(foreign));
		}
		for (unsigned i{}; i < 24U; ++i)
		{
			auto unrelated = indexed.rows[2].front();
			set(unrelated,
				"span",
				detached_cell::utf8(std::string(4096U, 's') + std::to_string(i)));
			indexed.rows[2].push_back(std::move(unrelated));
		}
		q::projection_resource_usage measured;
		auto out = take(q::project_exceptional_routes(indexed.input(), {}, {}, measured));
		require(variant(out).source_state == state::complete,
				"span ID collision retains all exact world axes");
		std::vector<std::string> expected_evidence;
		for (const auto& evidence : out.evidence)
			expected_evidence.push_back(evidence.relation_id + evidence.row.canonical_form());
		std::ranges::reverse(indexed.rows[2]);
		out = take(q::project_exceptional_routes(indexed.input()));
		std::vector<std::string> reordered_evidence;
		for (const auto& evidence : out.evidence)
			reordered_evidence.push_back(evidence.relation_id + evidence.row.canonical_form());
		require(variant(out).source_state == state::complete &&
					reordered_evidence == expected_evidence,
				"span collision and long-prefix input order preserve canonical evidence");
		q::projection_resource_usage baseline;
		take(q::project_exceptional_routes(indexed.input(), {}, {}, baseline));
		for (const bool storage : {false, true})
			for (const bool one_under : {false, true})
			{
				q::finite_population_limits bounded;
				if (storage)
					bounded.maximum_retained_bytes =
						baseline.retained_bytes_bound - static_cast<std::size_t>(one_under);
				else
					bounded.maximum_operations =
						baseline.operations - static_cast<std::size_t>(one_under);
				q::projection_resource_usage spent{1U, 1U};
				const auto result =
					q::project_exceptional_routes(indexed.input(), bounded, {}, spent);
				require(static_cast<bool>(result) == !one_under,
						"span lookup exact and one-under quota");
				if (one_under)
					require(spent.operations == 0U && spent.retained_bytes_bound == 0U,
							"failed span lookup keeps the existing failure usage contract");
			}
		q::finite_population_limits cancelled;
		std::size_t checkpoints{};
		cancelled.cancelled = [&]
		{
			return ++checkpoints == 1000U;
		};
		q::projection_resource_usage spent{1U, 1U};
		const auto stopped = q::project_exceptional_routes(indexed.input(), cancelled, {}, spent);
		require(!stopped && stopped.error().code == "sdk.exceptional-route-cancelled" &&
					checkpoints == 1000U && spent.operations == 0U &&
					spent.retained_bytes_bound == 0U,
				"late span lookup stop preserves cancellation and failure usage");
	}
	fixture copied;
	query_copy_controls::projection(
		copied.rows,
		names,
		[&]
		{
			return copied.queries();
		},
		[](const auto& input, const auto& limits, auto& usage)
		{
			return q::project_exceptional_routes(input, limits, {}, usage);
		},
		require);

	std::size_t cases = 0;
	{
		fixture f;
		auto out = take(q::project_exceptional_routes(f.input()));
		require(variant(out).topology_state == state::complete, "complete exact original topology");
		require(variant(out).scope_state == state::complete &&
					variant(out).source_state == state::complete,
				"distinct body and definition source bind");
		require(variant(out).symbol == std::string_view{"f\0\xff", 3},
				"raw ABI symbol bytes preserved");
		require(invoke(out).identity_state == state::complete &&
					invoke(out).placement_state == state::complete &&
					invoke(out).successors_state == state::complete &&
					invoke(out).boundary_state == state::complete &&
					invoke(out).boundary_declaration_state == state::complete &&
					invoke(out).exception_spec_state == state::complete,
				"zero ordinals and genuine boundary/spec independently close");
		++cases;
	}
	{
		fixture f;
		auto out = take(q::project_exceptional_routes(f.queries()));
		require(variant(out).topology_state == state::complete && out.source_queries &&
					!out.source_queries->scans.front().result.inputs_complete(),
				"typed topology independent from unrelated generic inputs");
		++cases;
	}
	{
		fixture f;
		f.rows[11].clear();
		f.rows[9].resize(1);
		f.rows[10].resize(1);
		set(f.rows[10][0], "terminator_kind", detached_cell::utf8("ret"));
		set(f.rows[9][0], "lowered_block_count", detached_cell::unsigned_integer(1));
		set(f.rows[9][0], "lowered_block_ids", symbols({"block:entry"}));
		set(f.rows[9][0], "lowered_successor_count", detached_cell::unsigned_integer(0));
		set(f.rows[9][0], "lowered_successor_ids", symbols({}));
		auto out = take(q::project_exceptional_routes(f.input()));
		require(variant(out).topology_state == state::complete && variant(out).successors.empty(),
				"genuine known empty successors");
		f.rows[9][0].values.erase("output.lowered_successor_ids");
		out = take(q::project_exceptional_routes(f.input()));
		require(variant(out).enumeration_state != state::complete,
				"missing old optional set is not empty");
		++cases;
	}
	{
		fixture f;
		auto query = f.queries();
		query.scans.pop_back();
		auto out = take(q::project_exceptional_routes(query));
		require(variant(out).enumeration_state != state::complete &&
					variant(out).enumeration_state != state::conflicting,
				"missing scan remains unknown not mismatch");
		require(!out.unresolved.empty(), "missing scan diagnostic retained");
		++cases;
	}
	{
		fixture f;
		f.rows[11].pop_back();
		auto out = take(q::project_exceptional_routes(f.input()));
		require(variant(out).enumeration_state == state::conflicting,
				"complete scan missing named member conflicts");
		++cases;
	}
	{
		fixture f;
		f.rows[10].push_back(f.rows[10][1]);
		auto out = take(q::project_exceptional_routes(f.input()));
		require(variant(out).topology_state == state::complete && out.evidence.size() > 10,
				"equal duplicate evidence retained");
		set(f.rows[10].back(), "instruction_count", detached_cell::unsigned_integer(2));
		out = take(q::project_exceptional_routes(f.input()));
		require(variant(out).topology_state == state::conflicting,
				"contradictory duplicates conflict");
		++cases;
	}
	{
		fixture f;
		set(f.rows[11][0], "compile_unit", detached_cell::utf8("unit:foreign"));
		auto out = take(q::project_exceptional_routes(f.input()));
		require(out.successors.front().state == state::conflicting &&
					variant(out).topology_state == state::conflicting,
				"foreign unit edge cannot bind");
		++cases;
	}
	{
		fixture f;
		set(f.rows[11][0], "to_block", detached_cell::utf8("block:dangling"));
		auto out = take(q::project_exceptional_routes(f.input()));
		require(invoke(out).successors_state != state::complete &&
					variant(out).topology_state != state::complete &&
					variant(out).enumeration_state == state::complete,
				"target FK frontier independent from membership");
		++cases;
	}
	{
		fixture f;
		set(f.rows[9][1], "body", detached_cell::utf8("body:foreign"));
		auto out = take(q::project_exceptional_routes(f.input()));
		require(invoke(out).identity_state == state::conflicting &&
					invoke(out).boundary_state == state::complete,
				"wrong physical body conflicts only identity");
		++cases;
	}
	{
		fixture f;
		f.rows[9][1].values.erase("output.instruction_ordinal");
		auto out = take(q::project_exceptional_routes(f.input()));
		require(invoke(out).placement_state != state::complete &&
					invoke(out).successors_state != state::complete &&
					invoke(out).boundary_state == state::complete,
				"unobserved placement is not ordinal zero");
		++cases;
	}
	{
		fixture f;
		set(f.rows[10][0], "instruction_count", detached_cell::unsigned_integer(2));
		auto out = take(q::project_exceptional_routes(f.input()));
		require(invoke(out).successors_state == state::conflicting &&
					variant(out).topology_state == state::conflicting,
				"successor must belong to final terminator instruction");
		++cases;
	}
	{
		fixture f;
		set(f.rows[9][1], "eh_selected_scope_kind", detached_cell::utf8("catch"));
		set(f.rows[9][1], "eh_disposition", detached_cell::utf8("catch_dispatch"));
		auto out = take(q::project_exceptional_routes(f.input()));
		require(invoke(out).boundary_state == state::complete &&
					invoke(out).disposition == "catch_dispatch" &&
					invoke(out).exception_spec_nonthrowing == true,
				"caught unwind remains catch despite noexcept specification");
		set(f.rows[9][1],
			"eh_disposition",
			detached_cell::utf8("direct_function_spec_termination"));
		out = take(q::project_exceptional_routes(f.input()));
		require(invoke(out).boundary_state == state::conflicting,
				"contradictory closed boundary symbols");
		++cases;
	}
	{
		fixture f;
		set(f.rows[9][1], "eh_disposition", detached_cell::utf8("future-disposition"));
		auto out = take(q::project_exceptional_routes(f.input()));
		require(invoke(out).boundary_state != state::complete &&
					variant(out).topology_state == state::complete,
				"future boundary does not alter topology");
		set(f.rows[9][0], "lowered_topology_profile", detached_cell::utf8("future/1"));
		out = take(q::project_exceptional_routes(f.input()));
		require(variant(out).enumeration_state != state::complete,
				"future topology profile unavailable");
		++cases;
	}
	{
		fixture f;
		f.rows[9].erase(f.rows[9].begin());
		auto out = take(q::project_exceptional_routes(f.input()));
		require(out.variants.empty() && out.blocks.size() == 3 && out.successors.size() == 2 &&
					out.invokes.size() == 1 && out.blocks.front().variant_state != state::complete,
				"orphans retained without invented carrier");
		++cases;
	}
	{
		fixture f;
		set(f.rows[4][0], "exception_spec_kind", detached_cell::utf8("dependent_noexcept"));
		set(f.rows[4][0], "exception_spec_state", detached_cell::utf8("partial"));
		f.rows[4][0].values.erase("output.exception_spec_nonthrowing");
		auto out = take(q::project_exceptional_routes(f.input()));
		require(invoke(out).exception_spec_state == state::partial &&
					!invoke(out).exception_spec_nonthrowing &&
					invoke(out).boundary_state == state::complete,
				"lazy semantic specification remains unknown independently");
		++cases;
	}
	{
		fixture f;
		set(f.rows[11][0], "ordinal", detached_cell::unsigned_integer(1));
		auto out = take(q::project_exceptional_routes(f.input()));
		require(invoke(out).successors_state == state::conflicting,
				"Invoke normal ordinal is actual successor zero");
		++cases;
	}
	{
		fixture f;
		set(f.rows[11][0], "to_block", detached_cell::utf8("block:entry"));
		auto out = take(q::project_exceptional_routes(f.input()));
		require(variant(out).topology_state == state::complete,
				"self successor is preserved without route exploration");
		set(f.rows[11][0], "to_block", detached_cell::utf8("block:unwind"));
		out = take(q::project_exceptional_routes(f.input()));
		require(variant(out).topology_state == state::complete && out.successors.size() == 2,
				"parallel successor occurrences retained");
		++cases;
	}
	{
		fixture f;
		set(f.rows[10][1], "ordinal", detached_cell::unsigned_integer(0));
		auto out = take(q::project_exceptional_routes(f.input()));
		require(variant(out).enumeration_state == state::conflicting,
				"distinct blocks cannot share actual ordinal");
		++cases;
	}
	{
		fixture f;
		set(f.rows[9][0], "lowered_entry", detached_cell::utf8("block:normal"));
		auto out = take(q::project_exceptional_routes(f.input()));
		require(variant(out).entry_state == state::conflicting,
				"entry FK binds actual entry flag not ordinal guess");
		++cases;
	}
	{
		fixture f;
		auto out = take(q::project_exceptional_routes(f.input()));
		const auto original = out.evidence.size();
		f.rows[7][0].presence.fragments = {"foreign"};
		out = take(q::project_exceptional_routes(f.input()));
		require(invoke(out).boundary_declaration_state != state::complete &&
					out.evidence.size() <= original,
				"declaration world cannot be guessed");
		++cases;
	}
	{
		fixture f;
		q::projection_resource_usage a, b;
		take(q::project_exceptional_routes(f.input(), {}, {}, a));
		take(q::project_exceptional_routes(f.input(), {}, {}, b));
		require(a.operations == b.operations && a.retained_bytes_bound == b.retained_bytes_bound &&
					a.operations > 0 && a.retained_bytes_bound > 0,
				"usage deterministic actual charged bound");
		q::finite_population_limits limit;
		limit.maximum_retained_bytes = a.retained_bytes_bound;
		take(q::project_exceptional_routes(f.input(), limit, {}, b));
		require(b.retained_bytes_bound <= limit.maximum_retained_bytes,
				"exact reported peak reservation admits projection");
		limit.maximum_retained_bytes = 1;
		b = {100, 100};
		auto failed = q::project_exceptional_routes(f.input(), limit, {}, b);
		require(!failed && b.operations == 0 && b.retained_bytes_bound == 0,
				"failure zeroes usage");
		std::stop_source stop;
		stop.request_stop();
		b = {100, 100};
		failed = q::project_exceptional_routes(f.input(), {}, stop.get_token(), b);
		require(!failed && failed.error().code == "sdk.exceptional-route-cancelled" &&
					b.operations == 0 && b.retained_bytes_bound == 0,
				"cancel zeroes usage");
		++cases;
	}
	{
		fixture f;
		f.rows[6] = {fact(6, {{"node", detached_cell::utf8("syntax:unused")}})};
		f.rows[6][0].values["output.foreign"] = detached_cell::utf8("malformed");
		auto out = q::project_exceptional_routes(f.input());
		require(!out && out.error().code == "sdk.exceptional-route-input-invalid",
				"unused original rows still validated");
		++cases;
	}
	{
		fixture f;
		set(f.rows[8][0], "declarations", symbols({}));
		set(f.rows[8][0], "declaration_count", detached_cell::unsigned_integer(0));
		set(f.rows[8][0], "physical_definition_count", detached_cell::unsigned_integer(1));
		set(f.rows[8][0], "physical_definition_ids", symbols({"decl:function"}));
		set(f.rows[8][0], "physical_definition_state", detached_cell::utf8("complete"));
		set(f.rows[8][0],
			"physical_definition_profile",
			detached_cell::utf8("clang22-original-physical-definitions/1"));
		set(f.rows[8][0], "enumeration_state", detached_cell::utf8("partial"));
		auto out = take(q::project_exceptional_routes(f.input()));
		require(invoke(out).boundary_declaration_state == state::complete,
				"actual lambda physical membership is independent of named cohort");
		set(f.rows[8][0], "physical_definition_count", detached_cell::unsigned_integer(2));
		out = take(q::project_exceptional_routes(f.input()));
		require(invoke(out).boundary_declaration_state == state::conflicting,
				"physical membership cardinality contradiction retained");
		++cases;
	}
	{
		fixture f;
		set(f.rows[9][1],
			"function",
			detached_cell::unknown(f.rows[9][1].values.at("output.function").type,
								   "original-owner-unobserved"));
		auto out = take(q::project_exceptional_routes(f.input()));
		require(invoke(out).identity_state != state::complete &&
					invoke(out).boundary_declaration_state != state::conflicting,
				"missing normalized owner is not foreign contradiction");
		++cases;
	}
	{
		fixture f;
		f.rows[10][0].values.erase("output.is_entry");
		auto out = q::project_exceptional_routes(f.input());
		require(!out && out.error().code == "sdk.exceptional-route-input-invalid",
				"missing required topology column is invalid");
		++cases;
	}
	{
		fixture f;
		auto input = f.input();
		input.topology_inputs_complete = false;
		f.rows[10].clear();
		f.rows[11].clear();
		input = f.input();
		input.topology_inputs_complete = false;
		auto out = take(q::project_exceptional_routes(input));
		require(variant(out).entry_state == state::unknown &&
					variant(out).enumeration_state != state::conflicting,
				"unavailable roster cannot invent wrong entry conflict");
		++cases;
	}
	{
		fixture f;
		set(f.rows[8][0], "declarations", symbols({}));
		set(f.rows[8][0], "declaration_count", detached_cell::unsigned_integer(0));
		set(f.rows[8][0], "physical_definition_count", detached_cell::unsigned_integer(3));
		set(f.rows[8][0], "physical_definition_ids", symbols({"decl:function"}));
		set(f.rows[8][0], "physical_definition_state", detached_cell::utf8("partial"));
		set(f.rows[8][0],
			"physical_definition_profile",
			detached_cell::utf8("clang22-original-physical-definitions/1"));
		auto out = take(q::project_exceptional_routes(f.input()));
		require(invoke(out).boundary_declaration_state == state::complete,
				"positive physical member binds despite independently unbound "
				"counted members");
		set(f.rows[8][0], "physical_definition_count", detached_cell::unsigned_integer(0));
		out = take(q::project_exceptional_routes(f.input()));
		require(invoke(out).boundary_declaration_state == state::conflicting,
				"partial count below actual members is contradictory");
		++cases;
	}
	{
		fixture f;
		set(f.rows[8][0], "physical_definition_count", detached_cell::unsigned_integer(1));
		set(f.rows[8][0], "physical_definition_ids", symbols({"decl:function"}));
		set(f.rows[8][0], "physical_definition_state", detached_cell::utf8("future-state"));
		set(f.rows[8][0],
			"physical_definition_profile",
			detached_cell::utf8("clang22-original-physical-definitions/1"));
		auto out = take(q::project_exceptional_routes(f.input()));
		require(invoke(out).boundary_declaration_state == state::complete,
				"independent named membership survives unavailable physical facet");
		set(f.rows[8][0], "declarations", symbols({}));
		set(f.rows[8][0], "declaration_count", detached_cell::unsigned_integer(0));
		out = take(q::project_exceptional_routes(f.input()));
		require(invoke(out).boundary_declaration_state == state::partial,
				"future physical state cannot assert membership alone");
		++cases;
	}
	{
		fixture f;
		set(f.rows[9][1], "block_ordinal", detached_cell::unsigned_integer(9));
		auto out = take(q::project_exceptional_routes(f.input()));
		require(invoke(out).placement_state == state::conflicting &&
					variant(out).topology_state == state::conflicting &&
					variant(out).enumeration_state == state::complete,
				"Invoke successor backlink retains exact original block ordinal "
				"frontier");
		++cases;
	}
	{
		fixture f;
		set(f.rows[5][0],
			"compile_unit",
			detached_cell::unknown(f.rows[5][0].values.at("output.compile_unit").type,
								   "original-body-unit-unobserved"));
		auto out = take(q::project_exceptional_routes(f.input()));
		require(variant(out).scope_state == state::partial &&
					variant(out).topology_state == state::complete,
				"missing body owner is unavailable independently of closed topology");
		++cases;
	}
	{
		fixture f;
		set(f.rows[11][0], "kind", detached_cell::utf8("ordinary"));
		f.rows[11][0].values.erase("output.invoke");
		auto out = take(q::project_exceptional_routes(f.input()));
		require(out.successors.front().state == state::conflicting &&
					variant(out).enumeration_state == state::complete,
				"actual Invoke terminator cannot be reclassified ordinary");
		++cases;
	}
	{
		fixture f;
		const auto original = f.rows;
		for (std::size_t i = 0; i < original[9].size(); ++i)
		{
			auto row = original[9][i];
			set(row, "exit", detached_cell::utf8(i == 0 ? "variant:b" : "exit:invoke-b"));
			set(row, "variant_kind", detached_cell::utf8("constructor"));
			set(row, "variant_index", detached_cell::unsigned_integer(1));
			set(row, "variant_symbol", binary("original-constructor-variant"));
			if (i == 0)
			{
				set(row, "lowered_entry", detached_cell::utf8("block:b-entry"));
				set(row,
					"lowered_block_ids",
					symbols({"block:b-entry", "block:b-normal", "block:b-unwind"}));
				set(row, "lowered_successor_ids", symbols({"edge:b-normal", "edge:b-unwind"}));
			}
			else
			{
				set(row, "variant", detached_cell::utf8("variant:b"));
				set(row, "lowered_block", detached_cell::utf8("block:b-entry"));
				set(row, "normal_successor", detached_cell::utf8("edge:b-normal"));
				set(row, "unwind_successor", detached_cell::utf8("edge:b-unwind"));
			}
			f.rows[9].push_back(std::move(row));
		}
		const std::array block_ids{"block:b-entry", "block:b-normal", "block:b-unwind"};
		for (std::size_t i = 0; i < original[10].size(); ++i)
		{
			auto row = original[10][i];
			set(row, "block", detached_cell::utf8(block_ids[i]));
			set(row, "variant", detached_cell::utf8("variant:b"));
			f.rows[10].push_back(std::move(row));
		}
		for (std::size_t i = 0; i < original[11].size(); ++i)
		{
			auto row = original[11][i];
			set(row, "successor", detached_cell::utf8(i == 0 ? "edge:b-normal" : "edge:b-unwind"));
			set(row, "variant", detached_cell::utf8("variant:b"));
			set(row, "from_block", detached_cell::utf8("block:b-entry"));
			set(row, "to_block", detached_cell::utf8(i == 0 ? "block:b-normal" : "block:b-unwind"));
			set(row, "invoke", detached_cell::utf8("exit:invoke-b"));
			f.rows[11].push_back(std::move(row));
		}
		auto out = take(q::project_exceptional_routes(f.input()));
		require(out.variants.size() == 2 && out.blocks.size() == 6 && out.successors.size() == 4 &&
					out.invokes.size() == 2,
				"ABI variants are independent physical topologies");
		for (const auto& v : out.variants)
			require(v.topology_state == state::complete && v.blocks.size() == 3 &&
						v.successors.size() == 2 && v.invokes.size() == 1,
					"no constructor alternative summing or crossbinding");
		++cases;
	}
	{
		fixture f;
		for (auto& rows : f.rows)
			for (auto& row : rows)
				row.presence.fragments.push_back("release");
		auto out = take(q::project_exceptional_routes(f.input()));
		require(out.variants.size() == 2 && out.blocks.size() == 6 && out.successors.size() == 4 &&
					out.invokes.size() == 2,
				"original condition worlds expand independently");
		for (const auto& v : out.variants)
			require(v.topology_state == state::complete && v.blocks.size() == 3 &&
						v.successors.size() == 2,
					"same raw IDs cannot mix original condition worlds");
		auto limits = q::finite_population_limits{};
		limits.maximum_condition_expansions = 1;
		auto failed = q::project_exceptional_routes(f.input(), limits);
		require(!failed && failed.error().code == "sdk.exceptional-route-budget",
				"original condition expansions remain bounded");
		++cases;
	}
	std::cout << cases << " exceptional route tests PASS\n";
}
