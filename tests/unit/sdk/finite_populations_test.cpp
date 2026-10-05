#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>

#include <cxxlens/sdk/finite_populations.hpp>

#include "../../../src/sdk/query_result_internal.hpp"
namespace
{
	using namespace cxxlens::sdk;
	namespace q = cxxlens::sdk::query;
	void require(bool value, std::string_view message)
	{
		if (!value)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}
	detached_cell ids(std::initializer_list<std::string_view> values)
	{
		std::vector<std::byte> bytes;
		for (const auto value : values)
		{
			for (unsigned shift{}; shift < 32U; shift += 8U)
				bytes.push_back(static_cast<std::byte>((value.size() >> shift) & 255U));
			for (const auto byte : value)
				bytes.push_back(static_cast<std::byte>(byte));
		}
		return detached_cell::bytes(std::move(bytes));
	}
	q::annotated_row row(std::string_view relation,
						 std::initializer_list<std::pair<std::string, detached_cell>> values)
	{
		q::annotated_row result;
		result.presence = {"records:test", {"debug"}};
		result.interpretation = "clang22";
		result.claim_contributors = {"claim:test"};
		result.producer_contracts = {{"records.test", "semantic:hand-labelled-v1"}};
		result.provenance = {"record:evidence"};
		result.contributor_guarantees = {
			{"exact", "finite-local-record", "fixture", {"schema_validated"}}};
		result.contributor_edges = {{result.claim_contributors.front(),
									 result.producer_contracts.front(),
									 result.provenance.front(),
									 result.contributor_guarantees.front(),
									 result.presence,
									 result.interpretation}};
		const auto descriptors = standard_relation_descriptors();
		const auto descriptor = std::ranges::find(descriptors, relation, &relation_descriptor::id);
		require(descriptor != descriptors.end(), "missing relation");
		for (const auto& column : descriptor->columns)
		{
			detached_cell cell = detached_cell::utf8("fixture");
			if (column.type.optional)
				cell = detached_cell::absent(column.type);
			else if (column.type.scalar == scalar_kind::boolean)
				cell = detached_cell::boolean(false);
			else if (column.type.scalar == scalar_kind::unsigned_integer)
				cell = detached_cell::unsigned_integer(0U);
			else if (column.type.scalar == scalar_kind::digest)
				cell = detached_cell::utf8("sha256:" + std::string(64U, 'a'));
			else if (column.type.scalar == scalar_kind::closed_symbol)
				cell = detached_cell::utf8("canonicalized");
			else if (column.type.scalar == scalar_kind::set ||
					 column.type.scalar == scalar_kind::bytes)
				cell = ids({});
			cell.type = column.type;
			result.values.emplace("output." + column.name, std::move(cell));
		}
		for (const auto& [name, value] : values)
		{
			auto actual = value;
			actual.type = result.values.at("output." + name).type;
			result.values.insert_or_assign("output." + name, std::move(actual));
		}
		return result;
	}

	struct fixture
	{
		std::array<std::vector<q::annotated_row>, 9U> groups;
		fixture()
		{
			groups[0] = {
				row("build.compile_unit.v1", {{"compile_unit", detached_cell::utf8("tu:test")}})};
			groups[1] = {row("source.file.v1",
							 {{"snapshot", detached_cell::utf8("snapshot:test")},
							  {"file", detached_cell::utf8("file:test")},
							  {"size", detached_cell::unsigned_integer(20U)}})};
			groups[2] = {row("source.span.v1",
							 {{"span", detached_cell::utf8("span:test")},
							  {"snapshot", detached_cell::utf8("snapshot:test")},
							  {"file", detached_cell::utf8("file:test")},
							  {"begin", detached_cell::unsigned_integer(0U)},
							  {"end", detached_cell::unsigned_integer(5U)}})};
			groups[3] = {row("cc.entity.v1",
							 {{"entity", detached_cell::utf8("function:test")},
							  {"kind", detached_cell::utf8("function")}})};
			groups[4] = {row(
				"cc.entity_detail.v1",
				{{"entity", detached_cell::utf8("function:test")},
				 {"compile_unit", detached_cell::utf8("tu:test")},
				 {"source", detached_cell::utf8("span:test")},
				 {"is_definition", detached_cell::boolean(true)},
				 {"flags", ids({"declaration_population_admitted", "finite_declarations_v1"})}})};
		}
		q::finite_population_input input() const
		{
			return {groups[0],
					groups[1],
					groups[2],
					groups[3],
					groups[4],
					groups[5],
					groups[6],
					groups[7],
					groups[8],
					true,
					true,
					true};
		}
		void declaration()
		{
			groups[7] = {row(
				"cc.declaration_inventory.v1",
				{{"inventory", detached_cell::utf8("inventory:test")},
				 {"compile_unit", detached_cell::utf8("tu:test")},
				 {"profile", detached_cell::utf8("clang22-explicit-admitted-named-declarations/1")},
				 {"enumeration_state", detached_cell::utf8("complete")},
				 {"declaration_count", detached_cell::unsigned_integer(1U)},
				 {"declarations", ids({"declaration:test"})},
				 {"system_declarations", ids({})},
				 {"parsed_files", ids({"file:test"})},
				 {"parsed_source_snapshots", ids({"snapshot:test"})},
				 {"file_state", detached_cell::utf8("complete")}})};
			groups[8] = {row("cc.declaration.v1",
							 {{"declaration", detached_cell::utf8("declaration:test")},
							  {"entity", detached_cell::utf8("function:test")},
							  {"source", detached_cell::utf8("span:test")},
							  {"kind", detached_cell::utf8("Function")}})};
		}
		void comment()
		{
			std::vector<std::byte> starts(8U);
			groups[7] = {row("source.comment_inventory.v1",
							 {{"inventory", detached_cell::utf8("inventory:test")},
							  {"compile_unit", detached_cell::utf8("tu:test")},
							  {"file", detached_cell::utf8("file:test")},
							  {"source_snapshot", detached_cell::utf8("snapshot:test")},
							  {"profile", detached_cell::utf8("clang22-frozen-raw-comments/1")},
							  {"enumeration_state", detached_cell::utf8("complete")},
							  {"comment_count", detached_cell::unsigned_integer(1U)},
							  {"comments", ids({"comment:test"})},
							  {"comment_bytes", detached_cell::unsigned_integer(5U)},
							  {"physical_line_starts", detached_cell::bytes(starts)},
							  {"physical_line_profile",
							   detached_cell::utf8("source.physical-lines.crlf-lf-cr/1")}})};
			std::vector<std::byte> spelling;
			for (char c : std::string{"//abc"})
				spelling.push_back(static_cast<std::byte>(c));
			groups[8] = {row("source.comment.v1",
							 {{"comment", detached_cell::utf8("comment:test")},
							  {"compile_unit", detached_cell::utf8("tu:test")},
							  {"source", detached_cell::utf8("span:test")},
							  {"kind", detached_cell::utf8("line")},
							  {"byte_count", detached_cell::unsigned_integer(5U)},
							  {"spelling_bytes", detached_cell::bytes(spelling)},
							  {"ordinal", detached_cell::unsigned_integer(0U)},
							  {"profile", detached_cell::utf8("clang22-frozen-raw-comments/1")}})};
		}
		void include()
		{
			groups[7] = {row(
				"source.include_inventory.v1",
				{{"inventory", detached_cell::utf8("inventory:test")},
				 {"compile_unit", detached_cell::utf8("tu:test")},
				 {"file", detached_cell::utf8("file:test")},
				 {"source_snapshot", detached_cell::utf8("snapshot:test")},
				 {"profile", detached_cell::utf8("clang22-preprocessed-inclusion-directives/1")},
				 {"enumeration_state", detached_cell::utf8("complete")},
				 {"include_count", detached_cell::unsigned_integer(0U)},
				 {"includes", ids({})},
				 {"resolution_state", detached_cell::utf8("complete")}})};
		}
		void flow()
		{
			groups[5] = {row("cc.body.v1",
							 {{"body", detached_cell::utf8("body:test")},
							  {"compile_unit", detached_cell::utf8("tu:test")},
							  {"function", detached_cell::utf8("function:test")},
							  {"source", detached_cell::utf8("span:test")},
							  {"eligibility", detached_cell::utf8("closed")},
							  {"node_count", detached_cell::unsigned_integer(1U)}})};
			groups[6] = {row("cc.cfg_node.v1",
							 {{"node", detached_cell::utf8("node:test")},
							  {"compile_unit", detached_cell::utf8("tu:test")},
							  {"function", detached_cell::utf8("function:test")},
							  {"body", detached_cell::utf8("body:test")},
							  {"kind", detached_cell::utf8("entry")},
							  {"element_count", detached_cell::unsigned_integer(0U)}})};
			groups[7] = {
				row("cc.flow_inventory.v1",
					{{"inventory", detached_cell::utf8("inventory:test")},
					 {"compile_unit", detached_cell::utf8("tu:test")},
					 {"function", detached_cell::utf8("function:test")},
					 {"body", detached_cell::utf8("body:test")},
					 {"source", detached_cell::utf8("span:test")},
					 {"profile", detached_cell::utf8("clang22-local-may-flow-facts/1")},
					 {"enumeration_state", detached_cell::utf8("complete")},
					 {"fact_count", detached_cell::unsigned_integer(4U)},
					 {"facts", ids({"live:in", "live:out", "reaching:in", "reaching:out"})},
					 {"program_point_count", detached_cell::unsigned_integer(2U)},
					 {"program_point_profile",
					  detached_cell::utf8("clang22-cfg-boundaries-and-written-local-events/1")},
					 {"reaching_points", ids({"reaching:in", "reaching:out"})},
					 {"liveness_points", ids({"live:in", "live:out"})},
					 {"reaching_state", detached_cell::utf8("complete")},
					 {"liveness_state", detached_cell::utf8("complete")}})};
			for (auto kind : {"live_in", "live_out", "reaching_in", "reaching_out"})
			{
				std::string id = kind;
				id[id.find('_')] = ':';
				groups[8].push_back(row(
					"cc.flow_fact.v1",
					{{"fact", detached_cell::utf8(id)},
					 {"compile_unit", detached_cell::utf8("tu:test")},
					 {"function", detached_cell::utf8("function:test")},
					 {"node", detached_cell::utf8("node:test")},
					 {"subject", detached_cell::utf8("function:test")},
					 {"kind", detached_cell::utf8(kind)},
					 {"guarantee", detached_cell::utf8("may")},
					 {"program_point", detached_cell::unsigned_integer(0U)},
					 {std::string(kind).starts_with("live") ? "live_subjects" : "definition_facts",
					  ids({})}}));
			}
		}
	};
	void set(q::annotated_row& row, std::string_view field, detached_cell value)
	{
		value.type = row.values.at("output." + std::string{field}).type;
		row.values["output." + std::string{field}] = std::move(value);
	}
	bool state(const result<q::finite_population_projection>& r, q::finite_population_state s)
	{
		return r && r->populations.size() == 1U && r->populations[0].state == s;
	}
} // namespace
int main()
{
	relation_registry registry;
	for (const auto& descriptor : standard_relation_descriptors())
		require(registry.add(descriptor).has_value(), "standard descriptor rejected");
	require(registry.build("finite-population-test").has_value(),
			"finite population references do not match their actual target ID types");
	using s = q::finite_population_state;
	fixture d;
	d.declaration();
	auto result = q::project_declarations(d.input());
	require(state(result, s::complete), "finite declaration did not complete");
	require(result->populations[0].members[0].kind == "function" &&
				result->populations[0].members[0].declaration_kind == "Function",
			"canonical kind lost");
	set(d.groups[7][0], "declaration_count", detached_cell::unsigned_integer(2U));
	require(state(q::project_declarations(d.input()), s::conflicting), "wrong count accepted");
	d.declaration();
	d.groups[4].clear();
	require(state(q::project_declarations(d.input()), s::partial), "missing detail became empty");
	d.declaration();
	d.groups[7].clear();
	require(state(q::project_declarations(d.input()), s::unknown),
			"missing declaration inventory omitted");
	fixture c;
	c.comment();
	result = q::project_source_comments(c.input());
	require(state(result, s::complete) &&
				result->populations[0].physical_line_starts == std::vector<std::uint64_t>{0U} &&
				result->populations[0].members[0].spelling_bytes.size() == 5U,
			"comment spelling/line starts lost");
	set(c.groups[8][0], "byte_count", detached_cell::unsigned_integer(4U));
	require(state(q::project_source_comments(c.input()), s::conflicting),
			"comment bounds contradiction lost");
	c.comment();
	set(c.groups[7][0], "physical_line_starts", detached_cell::bytes({std::byte{1}}));
	require(state(q::project_source_comments(c.input()), s::conflicting),
			"bad line encoding accepted");
	fixture i;
	i.include();
	require(state(q::project_source_includes(i.input()), s::complete),
			"known empty includes failed");
	set(i.groups[7][0], "enumeration_state", detached_cell::utf8("unavailable"));
	require(state(q::project_source_includes(i.input()), s::unknown),
			"unopened includes became empty");
	fixture f;
	f.flow();
	result = q::project_body_flow(f.input());
	require(state(result, s::complete) && result->populations[0].reaching_state == "complete" &&
				result->populations[0].liveness_state == "complete" &&
				result->populations[0].members.size() == 4U,
			"finite CFG points failed");

	fixture zero;
	zero.flow();
	zero.groups[8].clear();
	set(zero.groups[7][0], "fact_count", detached_cell::unsigned_integer(0U));
	set(zero.groups[7][0], "facts", ids({}));
	set(zero.groups[7][0], "program_point_count", detached_cell::unsigned_integer(0U));
	set(zero.groups[7][0], "reaching_points", ids({}));
	set(zero.groups[7][0], "liveness_points", ids({}));
	auto wrong_zero = q::project_body_flow(zero.input());
	require(wrong_zero && wrong_zero->populations[0].reaching_state == "conflicting" &&
				wrong_zero->populations[0].liveness_state == "conflicting",
			"actual CFG points were erased by a false zero inventory");

	set(f.groups[8][0], "node", detached_cell::utf8("node:missing"));
	require(state(q::project_body_flow(f.input()), s::partial), "missing CFG node became complete");
	f.groups[7].clear();
	require(state(q::project_body_flow(f.input()), s::unknown), "missing body inventory omitted");
	q::finite_population_limits limits;
	limits.maximum_rows = 1U;
	require(!q::project_source_comments(c.input(), limits), "row budget not enforced");
	std::stop_source stopped;
	stopped.request_stop();
	require(!q::project_source_comments(c.input(), {}, stopped.get_token()),
			"cancellation not enforced");
	d.declaration();
	auto first = q::project_declarations(d.input());
	std::ranges::reverse(d.groups[3]);
	auto second = q::project_declarations(d.input());
	require(first && second &&
				first->populations[0].members[0].id == second->populations[0].members[0].id,
			"ordering changed identity");
	return 0;
}
