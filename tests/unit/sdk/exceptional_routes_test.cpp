#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>

#include <cxxlens/relations/cc_declaration_inventory.hpp>
#include <cxxlens/relations/cc_entity_detail.hpp>
#include <cxxlens/relations/cc_exceptional_block.hpp>
#include <cxxlens/relations/cc_exceptional_exit.hpp>
#include <cxxlens/relations/cc_exceptional_successor.hpp>
#include <cxxlens/sdk.hpp>
#include <cxxlens/sdk/exceptional_routes.hpp>

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
		q::application_query_results queries() const
		{
			q::application_query_results out;
			out.snapshot_id = "query:routes";
			for (std::size_t i = 0; i < rows.size(); ++i)
			{
				auto data = std::make_shared<q::query_result::data>();
				data->row_values = rows[i];
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
} // namespace

int main()
{
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
