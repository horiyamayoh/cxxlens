#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>

#include <cxxlens/sdk/function_compiler_facets.hpp>

#include "query_result_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;
	namespace q = cxxlens::sdk::query;
	using state = q::finite_population_state;
	constexpr std::array<std::string_view, 13> names{"build.compile_unit.v1",
													 "source.file.v1",
													 "source.span.v1",
													 "cc.entity.v1",
													 "cc.entity_detail.v1",
													 "cc.declaration.v1",
													 "cc.type.v1",
													 "cc.type_component.v1",
													 "cc.body.v1",
													 "cc.syntax_node.v1",
													 "cc.cfg_node.v1",
													 "cc.call_site.v1",
													 "cc.operation.v1"};
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
		const auto descriptors = standard_relation_descriptors();
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
			if (!row.values.contains("output." + name))
			{
				std::cerr << "missing fixture column: " << names[group] << ":" << name << std::endl;
				std::exit(1);
			}
			copy.type = row.values.at("output." + name).type;
			row.values["output." + name] = std::move(copy);
		}
		return row;
	}
	void set(q::annotated_row& row, std::string_view name, detached_cell value)
	{
		const std::string key = "output." + std::string{name};
		if (!row.values.contains(key))
		{
			std::cerr << "missing fixture column: " << key << std::endl;
			std::exit(1);
		}
		value.type = row.values.at(key).type;
		row.values[key] = std::move(value);
	}
	struct fixture
	{
		std::array<std::vector<q::annotated_row>, 13> rows;
		fixture()
		{
			rows[0] = {fact(0, {{"compile_unit", detached_cell::utf8("unit:a")}})};
			rows[1] = {fact(1,
							{{"snapshot", detached_cell::utf8("source:a")},
							 {"file", detached_cell::utf8("file:a")},
							 {"size", detached_cell::unsigned_integer(100)}})};
			rows[2] = {fact(2,
							{{"span", detached_cell::utf8("span:scope")},
							 {"snapshot", detached_cell::utf8("source:a")},
							 {"file", detached_cell::utf8("file:a")},
							 {"begin", detached_cell::unsigned_integer(0)},
							 {"end", detached_cell::unsigned_integer(90)}}),
					   fact(2,
							{{"span", detached_cell::utf8("span:action")},
							 {"snapshot", detached_cell::utf8("source:a")},
							 {"file", detached_cell::utf8("file:a")},
							 {"begin", detached_cell::unsigned_integer(20)},
							 {"end", detached_cell::unsigned_integer(25)}})};
			rows[3] = {fact(3,
							{{"entity", detached_cell::utf8("function:a")},
							 {"kind", detached_cell::utf8("function")}})};
			rows[4] = {fact(4,
							{{"detail", detached_cell::utf8("detail:a")},
							 {"entity", detached_cell::utf8("function:a")},
							 {"compile_unit", detached_cell::utf8("unit:a")},
							 {"source", detached_cell::utf8("span:scope")},
							 {"flags",
							  symbols({"body_absent",
									   "declaration_population_admitted",
									   "finite_function_body_v1",
									   "finite_type_use_admission_v1",
									   "type_use_return_type"})},
							 {"parameter_count", detached_cell::unsigned_integer(0)},
							 {"capture_count", detached_cell::unsigned_integer(0)},
							 {"operation_count", detached_cell::unsigned_integer(1)},
							 {"operation_ids", symbols({"op:return"})},
							 {"operation_state", detached_cell::utf8("complete")},
							 {"operation_profile",
							  detached_cell::utf8("clang22-function-compiler-actions/1")}})};
			rows[5] = {fact(5,
							{{"declaration", detached_cell::utf8("declaration:a")},
							 {"entity", detached_cell::utf8("function:a")},
							 {"source", detached_cell::utf8("span:scope")},
							 {"kind", detached_cell::utf8("Function")}})};
			rows[6] = {
				fact(6,
					 {{"type", detached_cell::utf8("type:void")},
					  {"structure_state", detached_cell::utf8("complete")},
					  {"structure_profile", detached_cell::utf8("clang22-structural-type/1")},
					  {"structure_preimage", detached_cell::utf8("original:void")}})};
			rows[12] = {operation("op:return", "type_use", "declaration", {}, {}, "return_type")};
		}
		q::annotated_row operation(std::string_view id,
								   std::string_view kind,
								   std::string_view origin,
								   std::string_view expression = {},
								   std::string_view node = {},
								   std::string_view role = {}) const
		{
			auto result =
				fact(12,
					 {{"operation", detached_cell::utf8(std::string{id})},
					  {"site", detached_cell::utf8("site:" + std::string{kind})},
					  {"site_state", detached_cell::utf8("complete")},
					  {"compile_unit", detached_cell::utf8("unit:a")},
					  {"scope_declaration", detached_cell::utf8("declaration:a")},
					  {"function", detached_cell::utf8("function:a")},
					  {"kind", detached_cell::utf8(std::string{kind})},
					  {"origin", detached_cell::utf8(std::string{origin})},
					  {"profile", detached_cell::utf8("clang22-function-compiler-actions/1")},
					  {"source",
					   detached_cell::utf8(role == "return_type" ? "span:scope" : "span:action")},
					  {"evaluation",
					   detached_cell::utf8(origin == "declaration" ? "declarative"
																   : "potentially_evaluated")},
					  {"outcome", detached_cell::utf8("ordinary")},
					  {"observation_state", detached_cell::utf8("complete")}});
			if (!expression.empty())
				set(result, "expression", detached_cell::utf8(std::string{expression}));
			if (!node.empty())
			{
				set(result, "node", detached_cell::utf8(std::string{node}));
				set(result, "body", detached_cell::utf8("body:a"));
				set(result, "element_index", detached_cell::unsigned_integer(0));
				set(result, "element_kind", detached_cell::utf8("new_allocator"));
			}
			if (!role.empty())
			{
				set(result, "type_use_kind", detached_cell::utf8(std::string{role}));
				set(result, "object_type", detached_cell::utf8("type:void"));
				set(result, "object_state", detached_cell::utf8("complete"));
				set(result, "subject_index", detached_cell::unsigned_integer(0));
			}
			return result;
		}
		q::function_action_input input() const
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
					rows[12],
					true,
					true,
					true,
					true,
					true,
					true};
		}
		q::application_query_results queries() const
		{
			q::application_query_results value;
			value.snapshot_id = "query:actions";
			for (std::size_t i = 0; i < rows.size(); ++i)
			{
				auto data = std::make_shared<q::query_result::data>();
				data->row_values = rows[i];
				data->status = q::execution_status::complete;
				data->input_complete = true;
				data->ordered = true;
				data->snapshot = value.snapshot_id;
				value.scans.push_back(
					{std::string{names[i]}, {}, q::query_transfer_access::make(std::move(data))});
			}
			return value;
		}
		void written()
		{
			set(rows[4][0],
				"flags",
				symbols({"body_written",
						 "declaration_population_admitted",
						 "finite_function_body_v1",
						 "finite_type_use_admission_v1",
						 "type_use_return_type"}));
			set(rows[4][0], "operation_count", detached_cell::unsigned_integer(3));
			set(rows[4][0], "operation_ids", symbols({"op:ast", "op:cfg", "op:return"}));
			rows[8] = {fact(8,
							{{"body", detached_cell::utf8("body:a")},
							 {"compile_unit", detached_cell::utf8("unit:a")},
							 {"function", detached_cell::utf8("function:a")},
							 {"source", detached_cell::utf8("span:scope")},
							 {"eligibility", detached_cell::utf8("closed")},
							 {"node_count", detached_cell::unsigned_integer(1)},
							 {"operation_count", detached_cell::unsigned_integer(3)},
							 {"operation_ids", symbols({"op:ast", "op:cfg", "op:return"})},
							 {"operation_state", detached_cell::utf8("complete")},
							 {"operation_profile",
							  detached_cell::utf8("clang22-function-compiler-actions/1")}})};
			rows[9] = {fact(
				9,
				{{"node", detached_cell::utf8("syntax:new")},
				 {"compile_unit", detached_cell::utf8("unit:a")},
				 {"function", detached_cell::utf8("function:a")},
				 {"source", detached_cell::utf8("span:action")},
				 {"flags", symbols({"finite_operation_admission_v1", "operation_allocation"})}})};
			rows[10] = {fact(10,
							 {{"node", detached_cell::utf8("cfg:a")},
							  {"body", detached_cell::utf8("body:a")},
							  {"compile_unit", detached_cell::utf8("unit:a")},
							  {"function", detached_cell::utf8("function:a")},
							  {"element_count", detached_cell::unsigned_integer(1)},
							  {"implicit_operation_count", detached_cell::unsigned_integer(1)},
							  {"implicit_operation_state", detached_cell::utf8("complete")},
							  {"implicit_operation_profile",
							   detached_cell::utf8("clang22-function-compiler-actions/1")}})};
			set(rows[12][0], "body", detached_cell::utf8("body:a"));
			rows[12].push_back(operation("op:ast", "allocation", "ast", "syntax:new"));
			set(rows[12].back(), "body", detached_cell::utf8("body:a"));
			rows[12].push_back(operation("op:cfg", "allocation", "cfg", "syntax:new", "cfg:a"));
		}
	};

	void automatic(fixture& f, bool include_local)
	{
		const auto profile = detached_cell::utf8("clang22-written-automatic-local-storage/1");
		set(f.rows[4][0], "automatic_storage_profile", profile);
		set(f.rows[4][0], "automatic_storage_state", detached_cell::utf8("complete"));
		set(f.rows[4][0],
			"automatic_storage_count",
			detached_cell::unsigned_integer(include_local ? 1U : 0U));
		set(f.rows[4][0],
			"automatic_storage_ids",
			include_local ? symbols({"op:local"}) : symbols({}));
		if (!include_local)
			return;
		f.written();
		f.rows[9].clear();
		f.rows[12].resize(1U);
		set(f.rows[10][0], "implicit_operation_count", detached_cell::unsigned_integer(0U));
		set(f.rows[4][0], "operation_count", detached_cell::unsigned_integer(2U));
		set(f.rows[4][0], "operation_ids", symbols({"op:local", "op:return"}));
		set(f.rows[8][0], "operation_count", detached_cell::unsigned_integer(2U));
		set(f.rows[8][0], "operation_ids", symbols({"op:local", "op:return"}));
		for (const auto name : {"automatic_storage_profile",
								"automatic_storage_state",
								"automatic_storage_count",
								"automatic_storage_ids"})
			f.rows[8][0].values["output." + std::string{name}] =
				f.rows[4][0].values.at("output." + std::string{name});
		f.rows[3].push_back(fact(3,
								 {{"entity", detached_cell::utf8("variable:a")},
								  {"kind", detached_cell::utf8("variable")},
								  {"semantic_owner", detached_cell::utf8("function:a")}}));
		f.rows[4].push_back(fact(4,
								 {{"detail", detached_cell::utf8("detail:local")},
								  {"entity", detached_cell::utf8("variable:a")},
								  {"compile_unit", detached_cell::utf8("unit:a")},
								  {"source", detached_cell::utf8("span:action")},
								  {"flags",
								   symbols({"automatic_storage",
											"finite_type_use_admission_v1",
											"finite_variable_storage_v1",
											"storage_automatic",
											"type_use_local"})}}));
		f.rows[5].push_back(fact(5,
								 {{"declaration", detached_cell::utf8("declaration:local")},
								  {"entity", detached_cell::utf8("variable:a")},
								  {"source", detached_cell::utf8("span:action")},
								  {"kind", detached_cell::utf8("Var")}}));
		f.rows[6].push_back(
			fact(6,
				 {{"type", detached_cell::utf8("type:int")},
				  {"structure_state", detached_cell::utf8("complete")},
				  {"structure_profile", detached_cell::utf8("clang22-structural-type/1")},
				  {"structure_preimage", detached_cell::utf8("original:int")}}));
		auto op = f.operation("op:local", "type_use", "declaration", {}, {}, "local");
		set(op, "object_type", detached_cell::utf8("type:int"));
		set(op, "body", detached_cell::utf8("body:a"));
		set(op, "object_entity", detached_cell::utf8("variable:a"));
		set(op, "object_declaration", detached_cell::utf8("declaration:local"));
		set(op, "object_source", detached_cell::utf8("span:action"));
		set(op, "storage_duration", detached_cell::utf8("automatic"));
		set(op, "storage_state", detached_cell::utf8("complete"));
		set(op,
			"storage_profile",
			detached_cell::utf8("clang22-declared-automatic-object-layout/1"));
		set(op, "storage_size_bytes", detached_cell::unsigned_integer(4U));
		set(op, "storage_alignment_bytes", detached_cell::unsigned_integer(4U));
		set(op, "storage_abi_context", detached_cell::utf8(content_digest({})));
		set(op, "storage_target_triple", detached_cell::utf8("x86_64-unknown-linux-gnu"));
		f.rows[12].push_back(std::move(op));
	}
	void virtual_call(fixture& f)
	{
		f.written();
		f.rows[12].resize(2U);
		set(f.rows[12][1], "kind", detached_cell::utf8("invocation"));
		set(f.rows[12][1], "call", detached_cell::utf8("call:a"));
		set(f.rows[10][0], "implicit_operation_count", detached_cell::unsigned_integer(0U));
		for (auto* r : {&f.rows[4][0], &f.rows[8][0]})
		{
			set(*r, "operation_count", detached_cell::unsigned_integer(2U));
			set(*r, "operation_ids", symbols({"op:ast", "op:return"}));
		}
		set(f.rows[9][0], "kind", detached_cell::utf8("CXXMemberCallExpr"));
		set(f.rows[9][0],
			"flags",
			symbols({"admitted_call_site",
					 "finite_call_admission_v1",
					 "finite_operation_admission_v1",
					 "operation_invocation"}));
		f.rows[11].push_back(fact(11,
								  {{"call", detached_cell::utf8("call:a")},
								   {"caller", detached_cell::utf8("function:a")},
								   {"compile_unit", detached_cell::utf8("unit:a")},
								   {"source", detached_cell::utf8("span:action")},
								   {"expression", detached_cell::utf8("syntax:new")},
								   {"kind", detached_cell::utf8("member_call")}}));
		f.rows[3].push_back(fact(3,
								 {{"entity", detached_cell::utf8("method:a")},
								  {"kind", detached_cell::utf8("method")}}));
		for (auto* r : {&f.rows[12][1], &f.rows[11][0]})
		{
			set(*r, "dispatch_kind", detached_cell::utf8("virtual"));
			set(*r, "dispatch_state", detached_cell::utf8("complete"));
			set(*r, "dispatch_profile", detached_cell::utf8("clang22-original-call-dispatch/1"));
			set(*r, "candidate_presence", detached_cell::utf8("present"));
			set(*r, "candidate_count", detached_cell::unsigned_integer(1U));
			set(*r, "candidate_targets", symbols({"method:a"}));
			set(*r, "candidate_state", detached_cell::utf8("complete"));
			set(*r,
				"candidate_profile",
				detached_cell::utf8("clang22-materialized-static-override-candidates/1"));
		}
	}
	q::function_compiler_facet_projection facets(const fixture& f)
	{
		return take(q::project_function_compiler_facets({f.input(), true, true}));
	}
} // namespace
int main()
{
	fixture f;
	automatic(f, false);
	auto out = facets(f);
	require(out.populations.size() == 1U && out.populations[0].enumeration_state == state::complete,
			"independent original bodyless enum");
	require(out.populations[0].storage_enumeration_state == state::complete &&
				out.populations[0].automatic_storage_count == 0U,
			"closed actual empty storage census");
	f = fixture{};
	automatic(f, true);
	out = facets(f);
	require(out.populations[0].enumeration_state == state::complete,
			"independent actual local admission");
	require(out.populations[0].storage_enumeration_state == state::complete &&
				out.populations[0].automatic_storage.size() == 1U,
			"actual local member joins");
	require(out.populations[0].automatic_storage[0].layout_state == state::complete &&
				out.populations[0].automatic_storage[0].size_bytes == 4U &&
				out.populations[0].automatic_storage[0].binding_state == state::complete,
			"actual target layout bytes");
	set(f.rows[12][1], "storage_state", detached_cell::utf8("unknown"));
	set(f.rows[12][1],
		"storage_size_bytes",
		detached_cell::absent(f.rows[12][1].values.at("output.storage_size_bytes").type));
	out = facets(f);
	require(out.populations[0].storage_enumeration_state == state::complete &&
				out.populations[0].automatic_storage[0].layout_state == state::unknown,
			"layout absence does not erase census");
	f = fixture{};
	virtual_call(f);
	out = facets(f);
	require(out.populations[0].enumeration_state == state::complete &&
				out.populations[0].dispatches.size() == 1U,
			"actual virtual call enumeration");
	require(out.populations[0].dispatches[0].dispatch_state == state::complete &&
				out.populations[0].dispatches[0].candidate_state == state::complete &&
				out.populations[0].dispatches[0].candidate_presence == "present",
			"typed virtual candidate facet");
	set(f.rows[11][0], "dispatch_kind", detached_cell::utf8("direct"));
	out = facets(f);
	require(out.populations[0].dispatches[0].dispatch_state == state::conflicting,
			"contradictory dispatch facets");
	f = fixture{};
	virtual_call(f);
	f.rows[3][1].presence.fragments = {"release"};
	out = facets(f);
	require(out.populations[0].dispatches[0].candidate_state != state::complete,
			"foreign world candidate");
	f = fixture{};
	virtual_call(f);
	for (auto* r : {&f.rows[12][1], &f.rows[11][0]})
		set(*r, "candidate_count", detached_cell::unsigned_integer(2U));
	out = facets(f);
	require(out.populations[0].dispatches[0].candidate_state == state::conflicting,
			"complete candidate count mismatch");
	f = fixture{};
	automatic(f, true);
	set(f.rows[4][0], "automatic_storage_ids", symbols({}));
	set(f.rows[4][0], "automatic_storage_count", detached_cell::unsigned_integer(0U));
	out = facets(f);
	require(out.populations[0].storage_enumeration_state != state::complete,
			"independent body inventory disagreement");
	f = fixture{};
	virtual_call(f);
	for (auto* r : {&f.rows[12][1], &f.rows[11][0]})
	{
		set(*r, "candidate_targets", symbols({}));
		set(*r, "candidate_count", detached_cell::unsigned_integer(0U));
	}
	out = facets(f);
	require(out.populations[0].dispatches[0].candidate_state == state::complete &&
				out.populations[0].dispatches[0].candidate_presence == "present",
			"present empty is preserved");
	for (auto* r : {&f.rows[12][1], &f.rows[11][0]})
	{
		set(*r, "dispatch_kind", detached_cell::utf8("direct"));
		set(*r, "candidate_presence", detached_cell::utf8("absent"));
	}
	out = facets(f);
	require(out.populations[0].dispatches[0].dispatch_state == state::complete &&
				out.populations[0].dispatches[0].candidate_presence == "absent",
			"actual direct absent candidate");
	f = fixture{};
	virtual_call(f);
	set(f.rows[11][0], "candidate_count", detached_cell::unsigned_integer(2U));
	out = facets(f);
	require(out.populations[0].dispatches[0].dispatch_state == state::complete &&
				out.populations[0].dispatches[0].candidate_state == state::conflicting,
			"candidate conflict does not erase dispatch");
	f = fixture{};
	virtual_call(f);
	set(f.rows[11][0],
		"candidate_count",
		detached_cell::absent(f.rows[11][0].values.at("output.candidate_count").type));
	out = facets(f);
	require(out.populations[0].dispatches[0].dispatch_state == state::complete &&
				out.populations[0].dispatches[0].candidate_state != state::conflicting &&
				out.populations[0].dispatches[0].candidate_state != state::complete,
			"candidate absence is unavailable rather than conflict");
	f = fixture{};
	automatic(f, true);
	set(f.rows[4][1],
		"flags",
		symbols({"finite_type_use_admission_v1",
				 "finite_variable_storage_v1",
				 "storage_static",
				 "type_use_local"}));
	out = facets(f);
	require(out.populations[0].storage_enumeration_state == state::conflicting,
			"original storage classification contradicts census");
	f = fixture{};
	automatic(f, true);
	set(f.rows[4][1], "flags", symbols({"finite_type_use_admission_v1", "type_use_local"}));
	out = facets(f);
	require(out.populations[0].storage_enumeration_state != state::complete &&
				out.populations[0].storage_enumeration_state != state::conflicting,
			"missing storage classification is unknown");
	f = fixture{};
	automatic(f, true);
	for (auto* r : {&f.rows[4][0], &f.rows[8][0]})
	{
		set(*r, "automatic_storage_count", detached_cell::unsigned_integer(0U));
		set(*r, "automatic_storage_ids", symbols({}));
	}
	out = facets(f);
	require(out.populations[0].storage_enumeration_state == state::conflicting,
			"independent actual automatic object cannot disappear");
	f = fixture{};
	automatic(f, true);
	set(f.rows[4][1],
		"flags",
		symbols({"finite_type_use_admission_v1",
				 "finite_variable_storage_v1",
				 "storage_automatic",
				 "storage_non_object",
				 "type_use_local"}));
	for (auto* r : {&f.rows[4][0], &f.rows[8][0]})
	{
		set(*r, "automatic_storage_count", detached_cell::unsigned_integer(0U));
		set(*r, "automatic_storage_ids", symbols({}));
	}
	out = facets(f);
	require(out.populations[0].storage_enumeration_state == state::complete &&
				out.populations[0].automatic_storage.empty(),
			"actual references explicitly excluded from object census");
	f = fixture{};
	automatic(f, true);
	for (auto* r : {&f.rows[4][0], &f.rows[8][0]})
	{
		set(*r, "automatic_storage_count", detached_cell::unsigned_integer(2U));
		set(*r, "automatic_storage_state", detached_cell::utf8("partial"));
	}
	out = facets(f);
	require(out.populations[0].storage_enumeration_state == state::partial &&
				out.populations[0].automatic_storage_count == 2U,
			"partial admitted total preserves surviving members");
	f = fixture{};
	automatic(f, true);
	for (const auto name : {"automatic_storage_count",
							"automatic_storage_ids",
							"automatic_storage_state",
							"automatic_storage_profile"})
		set(f.rows[8][0],
			name,
			detached_cell::absent(f.rows[8][0].values.at("output." + std::string{name}).type));
	out = facets(f);
	require(out.populations[0].storage_enumeration_state != state::complete &&
				out.populations[0].storage_enumeration_state != state::conflicting,
			"missing body facet is unavailable rather than contradictory");
	f = fixture{};
	automatic(f, true);
	f.rows[12].pop_back();
	out = facets(f);
	require(out.populations[0].enumeration_state != state::complete &&
				out.populations[0].storage_enumeration_state != state::complete,
			"missing original operation cannot prove empty");
	f = fixture{};
	virtual_call(f);
	const auto before = facets(f);
	for (auto& group : f.rows)
		std::ranges::reverse(group);
	out = facets(f);
	require(out.populations[0].dispatches[0].candidate_targets ==
					before.populations[0].dispatches[0].candidate_targets &&
				out.unresolved == before.unresolved,
			"permuted original rows deterministic");
	out = take(q::project_function_compiler_facets(f.queries()));
	require(out.populations[0].dispatches[0].dispatch_state == state::complete &&
				out.original_actions.source_queries.has_value(),
			"public application query preserves originals");
	f = fixture{};
	automatic(f, true);
	auto input = f.input();
	input.admission_inputs_complete = false;
	out = take(q::project_function_compiler_facets({input, true, true}));
	require(!out.storage_inputs_complete &&
				out.populations[0].storage_enumeration_state != state::complete,
			"missing independent admission cannot close storage");
	q::finite_population_limits callback_limits;
	callback_limits.cancelled = []
	{
		return true;
	};
	require(!q::project_function_compiler_facets({f.input(), true, true}, callback_limits),
			"caller callback cancellation");
	std::stop_source stopped;
	stopped.request_stop();
	auto cancelled =
		q::project_function_compiler_facets({f.input(), true, true}, {}, stopped.get_token());
	require(!cancelled && cancelled.error().code == "sdk.function-facet-cancelled",
			"stop cancellation");
	q::finite_population_limits limits;
	limits.maximum_operations = 1U;
	require(!q::project_function_compiler_facets({f.input(), true, true}, limits),
			"bounded total nested work");
	f = fixture{};
	automatic(f, true);
	q::projection_resource_usage usage{999U, 999U};
	auto raw_measured =
		take(q::project_function_compiler_facets({f.input(), true, true}, {}, {}, usage));
	const auto raw_usage = usage;
	require(usage.operations > 0U && usage.retained_bytes_bound > 0U &&
				usage.operations < q::finite_population_limits{}.maximum_operations / 2U &&
				usage.retained_bytes_bound <
					q::finite_population_limits{}.maximum_retained_bytes / 2U,
			"measured raw usage contains actual charges rather than reserved maxima");
	out = take(q::project_function_compiler_facets(f.queries(), {}, {}, usage));
	require(usage.operations > raw_usage.operations &&
				usage.retained_bytes_bound > raw_usage.retained_bytes_bound &&
				out.original_actions.source_queries.has_value() &&
				out.populations[0].storage_enumeration_state ==
					raw_measured.populations[0].storage_enumeration_state,
			"query measurement retains bounded source plans and independent storage state");
	auto bounded = q::finite_population_limits{};
	bounded.maximum_operations = 2U * usage.operations + 4U;
	bounded.maximum_retained_bytes = 2U * usage.retained_bytes_bound + 4096U;
	auto repeated = q::project_function_compiler_facets(f.queries(), bounded, {}, usage);
	if (!repeated)
		std::cerr << repeated.error().code << ":" << repeated.error().field << ":"
				  << repeated.error().detail << "\n";
	require(repeated.has_value(),
			"actual charged budget can be reused without phantom maximum consumption");
	usage = {999U, 999U};
	require(!q::project_function_compiler_facets({f.input(), true, true}, limits, {}, usage) &&
				usage.operations == 0U && usage.retained_bytes_bound == 0U,
			"failed measurement is zero");
	usage = {999U, 999U};
	require(!q::project_function_compiler_facets(f.queries(), {}, stopped.get_token(), usage) &&
				usage.operations == 0U && usage.retained_bytes_bound == 0U,
			"cancelled query measurement is zero");
	input = f.input();
	input.admission_inputs_complete = false;
	out = take(q::project_function_compiler_facets({input, true, true}, {}, {}, usage));
	require(!out.storage_inputs_complete &&
				out.populations[0].storage_enumeration_state != state::complete,
			"successful measurement cannot invent admission closure");
	std::cout << "function compiler facets 35 focused checks PASS\n";
}
