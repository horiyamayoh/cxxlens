#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>

#include <cxxlens/sdk/function_actions.hpp>

#include "../../../src/sdk/query_result_internal.hpp"

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
		q::application_query_results
		queries(bool broad_complete = true, bool conflict = false, bool disagreement = false) const
		{
			q::application_query_results value;
			value.snapshot_id = "query:actions";
			for (std::size_t i = 0; i < rows.size(); ++i)
			{
				auto data = std::make_shared<q::query_result::data>();
				data->row_values = rows[i];
				data->status = q::execution_status::complete;
				data->input_complete = broad_complete;
				if (i == 4U && conflict)
					data->conflict_values.push_back({std::string{names[i]},
													 "unrelated-original-key",
													 "clang22",
													 {"debug"},
													 {"claim:left", "claim:right"},
													 {"content:left", "content:right"}});
				if (i == 4U && disagreement)
					data->disagreement_values.push_back({std::string{names[i]},
														 "unrelated-original-key",
														 "clang22",
														 "other",
														 "content:left",
														 "content:right",
														 {"debug"}});
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
	q::function_action_projection project(const fixture& fixture)
	{
		return take(q::project_function_actions(fixture.input()));
	}
	void check(const fixture& fixture, state wanted, std::string_view label)
	{
		const auto output = project(fixture);
		require(!output.populations.empty(), "missing scope");
		if (output.populations.front().state != wanted)
			for (const auto& gap : output.populations.front().gaps)
				std::cerr << gap.code << ':' << gap.subject << '\n';
		require(output.populations.front().state == wanted, label);
	}
} // namespace
int main()
{
	{
		q::projection_resource_usage charged{777U, 888U};
		fixture original;
		auto with_usage = q::project_function_actions(original.input(), {}, {}, charged);
		auto without_usage = q::project_function_actions(original.input());
		require(with_usage && without_usage && charged.operations > 0U &&
					charged.retained_bytes_bound > 0U,
				"successful raw usage missing");
		require(charged.operations < q::finite_population_limits{}.maximum_operations &&
					charged.retained_bytes_bound <
						q::finite_population_limits{}.maximum_retained_bytes,
				"configured maxima were reported as actual usage");
		require(with_usage->evidence.size() == without_usage->evidence.size() &&
					with_usage->populations.size() == without_usage->populations.size(),
				"usage changed original payload");
		charged = {777U, 888U};
		auto query_usage = q::project_function_actions(original.queries(), {}, {}, charged);
		require(query_usage && charged.operations > 0U && charged.retained_bytes_bound > 0U,
				"successful public-query usage missing");
		q::finite_population_limits tiny;
		tiny.maximum_rows = 1U;
		charged = {777U, 888U};
		require(!q::project_function_actions(original.input(), tiny, {}, charged) &&
					charged.operations == 0U && charged.retained_bytes_bound == 0U,
				"failed projection leaked usage");
		std::stop_source usage_stop;
		usage_stop.request_stop();
		charged = {777U, 888U};
		require(
			!q::project_function_actions(original.input(), {}, usage_stop.get_token(), charged) &&
				charged.operations == 0U && charged.retained_bytes_bound == 0U,
			"cancelled projection leaked usage");
	}

	fixture input;
	check(input, state::complete, "bodyless return type population");
	auto output = project(input);
	require(output.populations.size() == 1 &&
				output.populations[0].declaration == "declaration:a" &&
				output.populations[0].actions[0].type_state == state::complete,
			"original bodyless type facet");
	input.written();
	check(input, state::complete, "AST plus actual CFG allocation views");
	output = project(input);
	require(output.populations[0].actions.size() == 3, "original occurrences collapsed by site");
	require(output.populations[0].actions[0].site == output.populations[0].actions[1].site &&
				!output.populations[0].actions[1].program_point,
			"actual shared site and absent flow point");
	input.rows[12].erase(input.rows[12].begin() + 1);
	check(input, state::partial, "missing independent AST admission must stay partial");
	input = fixture{};
	input.written();
	set(input.rows[4][0], "operation_count", detached_cell::unsigned_integer(1));
	set(input.rows[4][0], "operation_ids", symbols({"op:return"}));
	input.rows[12].resize(1);
	set(input.rows[8][0], "operation_count", detached_cell::unsigned_integer(1));
	set(input.rows[8][0], "operation_ids", symbols({"op:return"}));
	check(input, state::partial, "forged shrunken inventory must not erase syntax/CFG admissions");
	input = fixture{};
	input.written();
	set(input.rows[12][2], "element_index", detached_cell::unsigned_integer(1));
	check(input, state::conflicting, "CFG element bound");
	input = fixture{};
	input.written();
	set(input.rows[12][2], "compile_unit", detached_cell::utf8("unit:wrong"));
	output = project(input);
	require(output.populations[0].state != state::complete, "cross-unit occurrence closure");
	input = fixture{};
	input.written();
	set(input.rows[9][0], "function", detached_cell::utf8("function:wrong"));
	check(input, state::conflicting, "contradictory syntax owner");
	input = fixture{};
	input.written();
	input.rows[12].push_back(input.rows[12][2]);
	check(input, state::complete, "agreeing duplicate evidence");
	{
		fixture many;
		for (unsigned copy = 0; copy < 32U; ++copy)
			many.rows[12].push_back(many.rows[12].front());
		q::finite_population_limits compact;
		compact.maximum_retained_bytes = 2U * 1024U * 1024U;
		const auto retained = take(q::project_function_actions(many.input(), compact));
		require(retained.populations.size() == 1U &&
					retained.populations[0].state == state::complete &&
					retained.populations[0].actions.size() == 1U,
				"original agreeing row evidence should not require a second "
				"equality payload copy");
		require(std::ranges::count(retained.evidence,
								   "cc.operation.v1",
								   [](const auto& e)
								   {
									   return e.relation_id;
								   }) == 33,
				"bounded storage must preserve every agreeing original operation row");
	}
	set(input.rows[12].back(), "element_kind", detached_cell::utf8("constructor"));
	check(input, state::conflicting, "contradictory occurrence evidence");
	input = fixture{};
	input.rows[6].clear();
	output = project(input);
	require(output.populations[0].state == state::complete &&
				output.populations[0].actions[0].type_state != state::complete,
			"missing type does not erase finite membership");
	input = fixture{};
	set(input.rows[4][0], "capture_count", detached_cell::unsigned_integer(1));
	check(input, state::partial, "missing actual capture slot");
	input = fixture{};
	set(input.rows[4][0], "parameter_count", detached_cell::unsigned_integer(1));
	check(input, state::partial, "missing actual parameter slot");
	input = fixture{};
	set(input.rows[12][0], "subject_index", detached_cell::unsigned_integer(2));
	check(input, state::conflicting, "return slot identity");
	input = fixture{};
	set(input.rows[12][0],
		"site",
		detached_cell::absent(input.rows[12][0].values.at("output.site").type));
	check(input, state::conflicting, "complete site without original ID");
	input = fixture{};
	input.rows[2][0].presence.fragments = {"release"};
	check(input, state::partial, "source world mismatch");
	input = fixture{};
	set(input.rows[4][0], "operation_count", detached_cell::unsigned_integer(2));
	check(input, state::conflicting, "count/member contradiction");
	input = fixture{};
	auto raw = input.input();
	raw.admission_inputs_complete = false;
	require(take(q::project_function_actions(raw)).populations[0].state == state::unknown,
			"incomplete admission scan");
	input.written();
	auto first = project(input);
	for (auto& rows : input.rows)
		std::ranges::reverse(rows);
	auto second = project(input);
	require(first.evidence.size() == second.evidence.size() &&
				first.populations[0].evidence == second.populations[0].evidence &&
				first.populations[0].actions[0].evidence ==
					second.populations[0].actions[0].evidence,
			"permutation determinism");
	q::finite_population_limits limits;
	limits.maximum_members = 1;
	require(!q::project_function_actions(input.input(), limits), "member budget");
	limits = {};
	limits.maximum_operations = 1;
	require(!q::project_function_actions(input.input(), limits), "work budget");
	limits = {};
	limits.maximum_retained_bytes = 1;
	require(!q::project_function_actions(input.input(), limits), "retained byte budget");
	std::stop_source stop;
	stop.request_stop();
	require(!q::project_function_actions(input.input(), {}, stop.get_token()), "stop token");
	limits = {};
	limits.cancelled = []
	{
		return true;
	};
	require(!q::project_function_actions(input.input(), limits), "caller cancellation");
	input = fixture{};
	const auto owned = input.queries();
	output = take(q::project_function_actions(owned));
	require(output.source_queries && output.source_queries->scans.size() == 13 &&
				output.populations[0].state == state::complete,
			"original public query preservation");
	// Source scans can also contain lexical token spans unrelated to any
	// action. Validate them all, retain only exact original referenced sources.
	fixture lexical;
	for (std::size_t i = 0; i < 512; ++i)
	{
		auto span = lexical.rows[2][0];
		set(span, "span", detached_cell::utf8("token:unrelated:" + std::to_string(i)));
		lexical.rows[2].push_back(std::move(span));
	}
	q::finite_population_limits source_limits;
	source_limits.maximum_retained_bytes = 512U * 1024U;
	const auto raw_lexical_result =
		take(q::project_function_actions(lexical.input(), source_limits));
	require(raw_lexical_result.populations[0].enumeration_state == state::complete &&
				raw_lexical_result.populations[0].state == state::complete &&
				std::ranges::count(raw_lexical_result.evidence,
								   "source.span.v1",
								   &q::finite_population_evidence::relation_id) == 1,
			"raw independent input copied unrelated source spans");
	const auto lexical_queries = lexical.queries(false);
	const auto lexical_result = take(q::project_function_actions(lexical_queries, source_limits));
	require(lexical_result.populations[0].enumeration_state == state::complete &&
				lexical_result.populations[0].state == state::complete,
			"unrelated token spans exhausted original action projection");
	require(std::ranges::count(lexical_result.evidence,
							   "source.span.v1",
							   &q::finite_population_evidence::relation_id) == 1,
			"unreferenced token spans copied into action evidence");
	const auto saved_span = std::ranges::find(lexical_result.source_queries->scans,
											  "source.span.v1",
											  &q::application_relation_scan::relation_id);
	require(saved_span != lexical_result.source_queries->scans.end() &&
				q::query_transfer_access::borrow_rows(saved_span->result).size() == 514 &&
				!saved_span->result.inputs_complete(),
			"original source query handle or broad flags lost");
	auto malformed_lexical = lexical;
	malformed_lexical.rows[2].back().values.at("output.begin").type = {
		scalar_kind::utf8_string, "", false};
	require(!q::project_function_actions(malformed_lexical.queries(), source_limits),
			"unreferenced malformed source row escaped validation");
	require(!q::project_function_actions(malformed_lexical.input(), source_limits),
			"raw unreferenced malformed source row escaped validation");
	auto too_many_sources = source_limits;
	too_many_sources.maximum_rows = 100;
	require(!q::project_function_actions(lexical_queries, too_many_sources),
			"source borrowing bypassed original row quota");
	too_many_sources = source_limits;
	too_many_sources.maximum_condition_expansions = 100;
	require(!q::project_function_actions(lexical_queries, too_many_sources),
			"unreferenced source spans bypassed condition quota");
	require(!q::project_function_actions(lexical.input(), too_many_sources),
			"raw unreferenced source rows bypassed condition quota");
	auto related_duplicate = lexical;
	auto duplicate_source = related_duplicate.rows[2][0];
	set(duplicate_source, "end", detached_cell::unsigned_integer(89));
	related_duplicate.rows[2].push_back(std::move(duplicate_source));
	const auto raw_conflicted_source =
		take(q::project_function_actions(related_duplicate.input(), source_limits));
	require(raw_conflicted_source.populations[0].state == state::conflicting,
			"raw matching source contradiction gained a convenient winner");
	const auto conflicted_source =
		take(q::project_function_actions(related_duplicate.queries(), source_limits));
	require(conflicted_source.populations[0].state == state::conflicting,
			"matching source contradiction gained a convenient winner");
	auto duplicate_scan = lexical_queries;
	const auto span_scan = std::ranges::find(
		duplicate_scan.scans, "source.span.v1", &q::application_relation_scan::relation_id);
	duplicate_scan.scans.push_back(*span_scan);
	const auto duplicates = take(q::project_function_actions(duplicate_scan, source_limits));
	require(duplicates.populations[0].enumeration_state == state::complete &&
				std::ranges::count(duplicates.evidence,
								   "source.span.v1",
								   &q::finite_population_evidence::relation_id) == 2,
			"multiple original scans lost agreeing source evidence");
	// Known non-action syntax is independently excluded by the actual compiler
	// admission marker. Unknown admission and every original row remain checked.
	auto syntax_limits = source_limits;
	syntax_limits.maximum_retained_bytes = 1024U * 1024U;
	fixture nonactions;
	nonactions.written();
	for (std::size_t i = 0; i < 512; ++i)
	{
		const auto suffix = std::to_string(i);
		auto syntax = nonactions.rows[9][0];
		set(syntax, "node", detached_cell::utf8("syntax:nonaction:" + suffix));
		set(syntax, "source", detached_cell::utf8("span:nonaction:" + suffix));
		set(syntax, "flags", symbols({"finite_operation_admission_v1"}));
		nonactions.rows[9].push_back(std::move(syntax));
		auto span = nonactions.rows[2][1];
		set(span, "span", detached_cell::utf8("span:nonaction:" + suffix));
		nonactions.rows[2].push_back(std::move(span));
	}
	const auto bounded_nonactions =
		take(q::project_function_actions(nonactions.input(), syntax_limits));
	require(bounded_nonactions.populations[0].enumeration_state == state::complete &&
				bounded_nonactions.populations[0].state == state::complete &&
				std::ranges::count(bounded_nonactions.evidence,
								   "cc.syntax_node.v1",
								   &q::finite_population_evidence::relation_id) == 1 &&
				std::ranges::count(bounded_nonactions.evidence,
								   "source.span.v1",
								   &q::finite_population_evidence::relation_id) == 2,
			"raw known excluded syntax/source exhausted original action storage");
	const auto public_nonactions =
		take(q::project_function_actions(nonactions.queries(false), syntax_limits));
	require(public_nonactions.populations[0].enumeration_state == state::complete &&
				public_nonactions.source_queries &&
				public_nonactions.source_queries->scans.size() == 13,
			"public excluded syntax lost original scan handles or independent "
			"closure");
	auto malformed_syntax = nonactions;
	malformed_syntax.rows[9].back().values.at("output.compile_unit").type = {
		scalar_kind::boolean, "", false};
	require(!q::project_function_actions(malformed_syntax.input(), syntax_limits),
			"excluded malformed syntax escaped raw validation");
	require(!q::project_function_actions(malformed_syntax.queries(), syntax_limits),
			"excluded malformed syntax escaped public validation");
	auto empty_identity = nonactions;
	set(empty_identity.rows[9].back(), "node", detached_cell::utf8(""));
	require(!q::project_function_actions(empty_identity.input(), syntax_limits),
			"excluded empty syntax identity escaped validation");
	auto excluded_limits = syntax_limits;
	excluded_limits.maximum_rows = 100;
	require(!q::project_function_actions(nonactions.input(), excluded_limits),
			"excluded syntax bypassed raw row quota");
	excluded_limits = syntax_limits;
	excluded_limits.maximum_condition_expansions = 100;
	require(!q::project_function_actions(nonactions.queries(), excluded_limits),
			"excluded syntax bypassed public condition quota");
	auto unknown_admission = fixture{};
	unknown_admission.written();
	auto unknown_node = nonactions.rows[9].back();
	set(unknown_node, "source", detached_cell::utf8("span:action"));
	set(unknown_node, "flags", symbols({}));
	unknown_admission.rows[9].push_back(unknown_node);
	require(project(unknown_admission).populations[0].enumeration_state != state::complete,
			"missing syntax admission was treated as independently excluded");
	set(unknown_admission.rows[9].back(),
		"flags",
		symbols({"finite_operation_admission_v1", "operation_invocation"}));
	require(project(unknown_admission).populations[0].enumeration_state != state::complete,
			"admitted missing action was discarded as a nonaction");
	auto repeated_nonaction = nonactions;
	repeated_nonaction.rows[9].push_back(repeated_nonaction.rows[9].back());
	require(take(q::project_function_actions(repeated_nonaction.input(), syntax_limits))
					.populations[0]
					.enumeration_state == state::complete,
			"agreeing original excluded duplicates lost known admission");
	set(repeated_nonaction.rows[9].back(),
		"flags",
		symbols({"finite_operation_admission_v1", "operation_invocation"}));
	require(take(q::project_function_actions(repeated_nonaction.input(), syntax_limits))
					.populations[0]
					.enumeration_state != state::complete,
			"conflicting excluded/admitted duplicate gained a convenient winner");
	auto referenced_nonaction = fixture{};
	referenced_nonaction.written();
	set(referenced_nonaction.rows[9][0], "flags", symbols({"finite_operation_admission_v1"}));
	const auto retained_reference = project(referenced_nonaction);
	require(std::ranges::count(retained_reference.evidence,
							   "cc.syntax_node.v1",
							   &q::finite_population_evidence::relation_id) == 1,
			"actual action expression lost its independently referenced original "
			"syntax");
	std::ranges::reverse(nonactions.rows[9]);
	std::ranges::reverse(nonactions.rows[2]);
	const auto reordered_nonactions =
		take(q::project_function_actions(nonactions.input(), syntax_limits));
	require(reordered_nonactions.populations.size() == bounded_nonactions.populations.size() &&
				reordered_nonactions.populations[0].state ==
					bounded_nonactions.populations[0].state &&
				reordered_nonactions.populations[0].operation_ids ==
					bounded_nonactions.populations[0].operation_ids &&
				std::ranges::equal(reordered_nonactions.evidence,
								   bounded_nonactions.evidence,
								   [](const auto& a, const auto& b)
								   {
									   return a.relation_id == b.relation_id &&
										   a.row.canonical_form() == b.row.canonical_form();
								   }),
			"excluded syntax retention depended on row order");

	auto missing = owned;
	missing.scans.erase(std::ranges::find(
		missing.scans, "cc.syntax_node.v1", &q::application_relation_scan::relation_id));
	output = take(q::project_function_actions(missing));
	require(!output.admission_inputs_complete && output.populations[0].state == state::unknown &&
				output.source_queries->scans.size() == 12,
			"missing independent query stays unknown");

	input = fixture{};
	input.written();
	const auto unrelated = input.queries(false);
	output = take(q::project_function_actions(unrelated));
	require(output.populations[0].enumeration_state == state::complete &&
				output.populations[0].state == state::complete,
			"original typed action and admission census is independent of "
			"unrelated broad input "
			"frontier");
	require(output.source_queries &&
				std::ranges::all_of(output.source_queries->scans,
									[](const auto& scan)
									{
										return !scan.result.inputs_complete();
									}),
			"original broad input flags remain unmodified");
	auto absent_scope = unrelated;
	absent_scope.scans.erase(std::ranges::find(
		absent_scope.scans, "cc.declaration.v1", &q::application_relation_scan::relation_id));
	output = take(q::project_function_actions(absent_scope));
	require(!output.scope_inputs_complete &&
				output.populations[0].enumeration_state != state::complete,
			"absent original declaration scan cannot close a function action "
			"population");
	output = take(q::project_function_actions(input.queries(false, true)));
	require(output.populations[0].enumeration_state != state::complete &&
				output.source_queries->scans[4].result.conflicts().size() == 1U,
			"original query conflict still blocks independent scope scan "
			"availability and is retained");
	output = take(q::project_function_actions(input.queries(false, false, true)));
	require(output.populations[0].enumeration_state != state::complete &&
				output.source_queries->scans[4].result.differential_disagreements().size() == 1U,
			"original differential disagreement still blocks availability and is "
			"retained");

	limits = {};
	limits.maximum_rows = 1U;
	require(!q::project_function_actions(unrelated, limits),
			"original borrowed row count is bounded before wrapper copies");
	limits = {};
	limits.maximum_retained_bytes = 1U;
	require(!q::project_function_actions(unrelated, limits),
			"original borrowed payload is charged before wrapper copies");
	limits = {};
	limits.maximum_source_queries = 1;
	require(!q::project_function_actions(owned, limits), "source query bounds");
	input = fixture{};
	set(input.rows[4][0], "capture_count", detached_cell::unsigned_integer(1));
	set(input.rows[4][0], "operation_count", detached_cell::unsigned_integer(2));
	set(input.rows[4][0], "operation_ids", symbols({"op:capture", "op:return"}));
	input.rows[12].push_back(
		input.operation("op:capture", "type_use", "declaration", {}, {}, "capture"));
	set(input.rows[12].back(), "site", detached_cell::utf8("site:capture0"));
	set(input.rows[12].back(), "object_state", detached_cell::utf8("partial"));
	check(input, state::complete, "actual this capture slot does not require a named object");
	output = project(input);
	require(output.populations[0].actions[0].object_state == state::partial &&
				output.populations[0].actions[0].type_state == state::complete,
			"capture type and named object axes");
	input = fixture{};
	set(input.rows[4][0], "parameter_count", detached_cell::unsigned_integer(1));
	set(input.rows[4][0], "operation_count", detached_cell::unsigned_integer(2));
	set(input.rows[4][0], "operation_ids", symbols({"op:parameter", "op:return"}));
	input.rows[3].push_back(fact(3,
								 {{"entity", detached_cell::utf8("parameter:a")},
								  {"kind", detached_cell::utf8("parameter")},
								  {"semantic_owner", detached_cell::utf8("function:a")}}));
	input.rows[4].push_back(
		fact(4,
			 {{"detail", detached_cell::utf8("detail:parameter")},
			  {"entity", detached_cell::utf8("parameter:a")},
			  {"source", detached_cell::utf8("span:action")},
			  {"compile_unit", detached_cell::utf8("unit:a")},
			  {"flags", symbols({"finite_type_use_admission_v1", "type_use_parameter"})}}));
	input.rows[5].push_back(fact(5,
								 {{"declaration", detached_cell::utf8("declaration:parameter")},
								  {"entity", detached_cell::utf8("parameter:a")},
								  {"source", detached_cell::utf8("span:action")}}));
	input.rows[12].push_back(
		input.operation("op:parameter", "type_use", "declaration", {}, {}, "parameter"));
	set(input.rows[12].back(), "object_declaration", detached_cell::utf8("declaration:parameter"));
	set(input.rows[12].back(), "object_entity", detached_cell::utf8("parameter:a"));
	set(input.rows[12].back(), "parameter_has_default_argument", detached_cell::boolean(true));
	output = project(input);
	require(output.populations[0].actions[0].parameter_has_default_argument == true,
			"actual original parameter default is retained");
	set(input.rows[12].back(), "parameter_has_default_argument", detached_cell::boolean(false));
	output = project(input);
	require(output.populations[0].actions[0].parameter_has_default_argument == false,
			"actual original parameter without a default is retained");
	set(input.rows[12].back(),
		"parameter_has_default_argument",
		detached_cell::absent(
			input.rows[12].back().values.at("output.parameter_has_default_argument").type));
	output = project(input);
	require(!output.populations[0].actions[0].parameter_has_default_argument,
			"missing default classification remains unknown");
	check(input, state::complete, "actual parameter declaration and slot admission");
	set(input.rows[3].back(),
		"semantic_owner",
		detached_cell::absent(input.rows[3].back().values.at("output.semantic_owner").type));
	check(input,
		  state::partial,
		  "independently admitted named parameter with unobserved owner is "
		  "unavailable");
	output = project(input);
	require(std::ranges::any_of(output.populations[0].gaps,
								[](const auto& value)
								{
									return value.code ==
										"sdk.action-type-admission-owner-unavailable";
								}),
			"missing original owner retains an explicit admission frontier");
	set(input.rows[3].back(), "semantic_owner", detached_cell::utf8("function:foreign"));
	check(
		input, state::conflicting, "actual present foreign parameter owner remains contradictory");
	set(input.rows[3].back(), "semantic_owner", detached_cell::utf8("function:a"));
	check(input, state::complete, "actual matching owner closes original declaration admission");
	input.rows[5].pop_back();
	check(input, state::partial, "missing original parameter declaration");
	input = fixture{};
	set(input.rows[4][0], "parameter_count", detached_cell::unsigned_integer(2));
	set(input.rows[4][0], "operation_count", detached_cell::unsigned_integer(3));
	set(input.rows[4][0],
		"operation_ids",
		symbols({"op:parameter0", "op:parameter1", "op:return"}));
	for (std::uint64_t index{}; index < 2U; ++index)
	{
		input.rows[12].push_back(input.operation("op:parameter" + std::to_string(index),
												 "type_use",
												 "declaration",
												 {},
												 {},
												 "parameter"));
		set(input.rows[12].back(), "subject_index", detached_cell::unsigned_integer(index));
		set(input.rows[12].back(),
			"site",
			detached_cell::utf8("site:parameter" + std::to_string(index)));
		set(input.rows[12].back(), "object_state", detached_cell::utf8("partial"));
		set(input.rows[12].back(),
			"parameter_has_default_argument",
			detached_cell::boolean(index == 1U));
	}
	check(input,
		  state::complete,
		  "actual unnamed parameter slots close independently of named objects");
	output = project(input);
	require(output.populations[0].actions[0].object_state == state::partial &&
				output.populations[0].actions[0].type_state == state::complete &&
				output.populations[0].actions[1].parameter_has_default_argument == true,
			"unnamed parameter type/default classifications retain independent axes");
	set(input.rows[12][2], "subject_index", detached_cell::unsigned_integer(0));
	check(input, state::partial, "missing independent unnamed parameter slot is unavailable");
	input = fixture{};
	set(input.rows[4][0], "operation_count", detached_cell::unsigned_integer(2));
	set(input.rows[4][0], "operation_ids", symbols({"op:call", "op:return"}));
	input.rows[9] = {
		fact(9,
			 {{"node", detached_cell::utf8("syntax:call")},
			  {"compile_unit", detached_cell::utf8("unit:a")},
			  {"function", detached_cell::utf8("function:a")},
			  {"source", detached_cell::utf8("span:action")},
			  {"flags", symbols({"finite_operation_admission_v1", "operation_invocation"})}})};
	input.rows[11] = {fact(11,
						   {{"call", detached_cell::utf8("call:a")},
							{"caller", detached_cell::utf8("function:a")},
							{"compile_unit", detached_cell::utf8("unit:a")},
							{"expression", detached_cell::utf8("syntax:call")},
							{"source", detached_cell::utf8("span:action")}})};
	input.rows[12].push_back(input.operation("op:call", "invocation", "ast", "syntax:call"));
	set(input.rows[12].back(), "call", detached_cell::utf8("call:a"));
	check(input, state::complete, "targetless actual call keeps finite invocation membership");
	output = project(input);
	require(output.populations[0].actions[0].target_signature.state == state::unknown &&
				output.populations[0].actions[0].call_state == state::complete,
			"target resolution independent of actual call");
	input.rows[11].clear();
	check(input, state::partial, "missing reused actual call");
	input = fixture{};
	input.written();
	auto& action = input.rows[12][2];
	const auto digest = content_digest({});
	set(action, "target", detached_cell::utf8("external:implicit-constructor"));
	set(action, "target_kind", detached_cell::utf8("constructor"));
	set(action, "target_signature_state", detached_cell::utf8("complete"));
	set(action,
		"target_signature_profile",
		detached_cell::utf8("clang22-original-target-signature/1"));
	set(action, "target_usr", detached_cell::bytes({std::byte{1}}));
	set(action, "target_canonical_type", detached_cell::utf8("type:void"));
	set(action, "target_structural_signature_digest", detached_cell::utf8(digest));
	set(action, "target_canonical_type_digest", detached_cell::utf8(digest));
	set(action, "target_canonical_type_profile", detached_cell::utf8("clang22-structural-type/1"));
	set(action, "target_language", detached_cell::utf8("c++"));
	set(action, "target_linkage", detached_cell::utf8("external"));
	set(action, "target_module_domain", detached_cell::utf8("<none>"));
	set(input.rows[6][0], "component_signature_digest", detached_cell::utf8(digest));
	output = project(input);
	require(output.populations[0].state == state::complete &&
				output.populations[0].actions[1].target_signature.state == state::complete &&
				output.populations[0].actions[1].target_kind == "constructor",
			"actual external constructor signature without entity row");
	set(input.rows[6][0],
		"component_signature_digest",
		detached_cell::utf8("sha256:" + std::string(64, 'a')));
	output = project(input);
	require(output.populations[0].state == state::complete &&
				output.populations[0].actions[1].target_signature.state == state::conflicting,
			"conflicting target type does not erase occurrence membership");

	input = fixture{};
	input.written();
	auto& optional_target = input.rows[12][2];
	set(optional_target, "target", detached_cell::utf8("target:constructor"));
	set(optional_target, "target_kind", detached_cell::utf8("constructor"));
	input.rows[3].push_back(fact(3,
								 {{"entity", detached_cell::utf8("target:constructor")},
								  {"kind", detached_cell::utf8("constructor")},
								  {"provider_local_key", detached_cell::bytes({std::byte{1}})},
								  {"structural_signature_digest", detached_cell::utf8(digest)}}));
	output = project(input);
	require(output.populations[0].state == state::complete &&
				output.populations[0].actions[1].target_signature.state == state::unknown,
			"absent original optional target facets remain unknown with actual "
			"entity present");
	set(optional_target, "target_canonical_type", detached_cell::utf8("type:void"));
	set(input.rows[6][0], "component_signature_digest", detached_cell::utf8(digest));
	output = project(input);
	require(output.populations[0].actions[1].target_signature.state == state::unknown,
			"absent original type signature facets remain unknown with actual "
			"structural type present");
	set(optional_target, "target_usr", detached_cell::bytes({std::byte{2}}));
	output = project(input);
	require(output.populations[0].state == state::complete &&
				output.populations[0].actions[1].target_signature.state == state::conflicting,
			"present wrong original target USR stays conflicting independently "
			"of occurrence "
			"membership");
	input = fixture{};
	input.written();
	input.rows[12].erase(input.rows[12].begin() + 1);
	set(input.rows[4][0], "operation_count", detached_cell::unsigned_integer(2));
	set(input.rows[4][0], "operation_ids", symbols({"op:cfg", "op:return"}));
	set(input.rows[8][0], "operation_count", detached_cell::unsigned_integer(2));
	set(input.rows[8][0], "operation_ids", symbols({"op:cfg", "op:return"}));
	set(input.rows[9][0],
		"function",
		detached_cell::absent(input.rows[9][0].values.at("output.function").type));
	set(input.rows[9][0],
		"flags",
		symbols({"finite_operation_admission_v1", "operation_construction"}));
	set(input.rows[12][1], "kind", detached_cell::utf8("construction"));
	set(input.rows[12][1], "element_kind", detached_cell::utf8("constructor"));
	check(input,
		  state::complete,
		  "CFG-owned default initializer syntax does not need body containment "
		  "or guessed syntax "
		  "owner");
	set(input.rows[12][1], "program_point", detached_cell::unsigned_integer(0));
	set(input.rows[12][1], "program_point_profile", detached_cell::utf8("future:point"));
	check(input, state::partial, "unsupported flow point profile is not a known binding");
	input = fixture{};
	input.written();
	set(input.rows[4][0], "operation_count", detached_cell::unsigned_integer(4));
	set(input.rows[4][0], "operation_ids", symbols({"op:ast", "op:cfg", "op:cfg2", "op:return"}));
	set(input.rows[8][0], "operation_count", detached_cell::unsigned_integer(4));
	set(input.rows[8][0], "operation_ids", symbols({"op:ast", "op:cfg", "op:cfg2", "op:return"}));
	set(input.rows[10][0], "element_count", detached_cell::unsigned_integer(2));
	set(input.rows[10][0], "implicit_operation_count", detached_cell::unsigned_integer(2));
	input.rows[12].push_back(input.rows[12][2]);
	set(input.rows[12].back(), "operation", detached_cell::utf8("op:cfg2"));
	set(input.rows[12].back(), "element_index", detached_cell::unsigned_integer(1));
	check(input, state::complete, "distinct actual CFG exits may share a static site");
	output = project(input);
	require(output.populations[0].actions[2].site_binding_state == state::complete,
			"validated shared static-site group");
	set(input.rows[12].back(), "element_index", detached_cell::unsigned_integer(0));
	check(input, state::conflicting, "two occurrence IDs cannot claim one original CFG element");
	set(input.rows[12].back(), "element_index", detached_cell::unsigned_integer(1));
	set(input.rows[12].back(), "object_entity", detached_cell::utf8("object:second"));
	set(input.rows[12][2], "object_entity", detached_cell::utf8("object:first"));
	output = project(input);
	require(output.populations[0].state == state::complete &&
				output.populations[0].actions[2].site_binding_state == state::conflicting,
			"contradictory shared static-site subjects stay separate from "
			"occurrence completeness");
	input = fixture{};
	input.rows[4].clear();
	input.rows[12].clear();
	output = project(input);
	require(output.populations.size() == 1 && output.populations[0].state == state::unknown &&
				output.populations[0].compile_unit.empty(),
			"surviving function declaration without original detail is unknown "
			"and has no "
			"fabricated unit");
	const auto default_cfg = [](bool field)
	{
		fixture value;
		value.written();
		value.rows[12].erase(value.rows[12].begin() + 1);
		set(value.rows[4][0], "operation_count", detached_cell::unsigned_integer(2));
		set(value.rows[4][0], "operation_ids", symbols({"op:cfg", "op:return"}));
		set(value.rows[8][0], "operation_count", detached_cell::unsigned_integer(2));
		set(value.rows[8][0], "operation_ids", symbols({"op:cfg", "op:return"}));
		set(value.rows[2][1], "begin", detached_cell::unsigned_integer(94));
		set(value.rows[2][1], "end", detached_cell::unsigned_integer(96));
		set(value.rows[9][0],
			"function",
			field ? detached_cell::absent(value.rows[9][0].values.at("output.function").type)
				  : detached_cell::utf8("function:default-owner"));
		set(value.rows[9][0],
			"flags",
			symbols({"finite_operation_admission_v1",
					 field ? "operation_construction" : "operation_invocation"}));
		set(value.rows[12][1], "kind", detached_cell::utf8(field ? "construction" : "invocation"));
		set(value.rows[12][1],
			"element_kind",
			detached_cell::utf8(field ? "constructor" : "statement"));
		set(value.rows[10][0],
			"implicit_operation_count",
			detached_cell::unsigned_integer(field ? 1 : 0));
		set(value.rows[12][1],
			"expression_context",
			detached_cell::utf8(field ? "default_initializer" : "default_argument"));
		set(value.rows[12][1], "context_declaration", detached_cell::utf8("declaration:default"));
		value.rows[3].push_back(
			fact(3,
				 {{"entity", detached_cell::utf8(field ? "record:a" : "function:default-owner")},
				  {"kind", detached_cell::utf8(field ? "record" : "function")}}));
		value.rows[3].push_back(
			fact(3,
				 {{"entity", detached_cell::utf8("subject:default")},
				  {"kind", detached_cell::utf8(field ? "field" : "parameter")},
				  {"semantic_owner",
				   detached_cell::utf8(field ? "record:a" : "function:default-owner")}}));
		value.rows[5].push_back(fact(5,
									 {{"declaration", detached_cell::utf8("declaration:default")},
									  {"entity", detached_cell::utf8("subject:default")},
									  {"source", detached_cell::utf8("span:action")},
									  {"kind", detached_cell::utf8(field ? "Field" : "ParmVar")}}));
		value.rows[4].push_back(fact(4,
									 {{"detail", detached_cell::utf8("detail:default")},
									  {"entity", detached_cell::utf8("subject:default")},
									  {"compile_unit", detached_cell::utf8("unit:a")},
									  {"source", detached_cell::utf8("span:action")},
									  {"flags",
									   field ? symbols({"finite_type_use_admission_v1"})
											 : symbols({"default_argument",
														"finite_type_use_admission_v1",
														"type_use_parameter"})}}));
		if (field)
		{
			set(value.rows[3][0], "kind", detached_cell::utf8("constructor"));
			set(value.rows[3][0], "semantic_owner", detached_cell::utf8("record:a"));
		}
		else
		{
			value.rows[11] = {fact(11,
								   {{"call", detached_cell::utf8("call:default")},
									{"caller", detached_cell::utf8("function:default-owner")},
									{"compile_unit", detached_cell::utf8("unit:a")},
									{"expression", detached_cell::utf8("syntax:new")},
									{"source", detached_cell::utf8("span:action")}})};
			set(value.rows[12][1], "call", detached_cell::utf8("call:default"));
		}
		return value;
	};
	input = default_cfg(false);
	check(input,
		  state::complete,
		  "actual default-argument CFG activation retains its lexical expression "
		  "and original call "
		  "owner");
	output = project(input);
	require(output.populations[0].actions[0].context_state == state::complete &&
				output.populations[0].actions[0].scope_declaration == "declaration:a" &&
				output.populations[0].actions[0].context_declaration == "declaration:default",
			"execution scope is distinct from original default context");
	set(input.rows[12][1],
		"context_declaration",
		detached_cell::absent(input.rows[12][1].values.at("output.context_declaration").type));
	check(input, state::partial, "missing original default context remains unknown");
	input = default_cfg(false);
	set(input.rows[11][0], "caller", detached_cell::utf8("function:foreign"));
	check(input, state::conflicting, "foreign original default call owner");
	input = default_cfg(false);
	set(input.rows[9][0], "function", detached_cell::utf8("function:foreign"));
	check(input, state::conflicting, "foreign default syntax owner");
	input = default_cfg(false);
	set(input.rows[9][0],
		"function",
		detached_cell::absent(input.rows[9][0].values.at("output.function").type));
	check(input, state::partial, "missing original default syntax owner remains unknown");
	input = default_cfg(false);
	set(input.rows[11][0],
		"caller",
		detached_cell::absent(input.rows[11][0].values.at("output.caller").type));
	check(input, state::partial, "missing original default call owner remains unknown");
	input = default_cfg(false);
	set(input.rows[4][1], "flags", symbols({"finite_type_use_admission_v1", "type_use_parameter"}));
	check(input, state::conflicting, "context parameter without an actual default");
	input = default_cfg(false);
	input.rows[4][1].presence.fragments = {"release"};
	check(input, state::partial, "default context detail world mismatch");
	input = default_cfg(true);
	check(input,
		  state::complete,
		  "actual field-default CFG context is bound to the executing "
		  "constructor record");
	set(input.rows[3][0], "semantic_owner", detached_cell::utf8("record:foreign"));
	check(input, state::conflicting, "foreign field-default executing record");
	input = default_cfg(true);
	set(input.rows[9][0], "function", detached_cell::utf8("constructor:unavailable"));
	check(input, state::partial, "missing original field-default lexical owner remains unknown");
	input = default_cfg(false);
	set(input.rows[12][1], "expression_context", detached_cell::utf8("unknown"));
	check(input, state::partial, "unknown compiler activation context");
	std::cout << "function actions focused checks PASS\n";
}
