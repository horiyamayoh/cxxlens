#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <source_location>

#include <cxxlens/sdk/call_operands.hpp>

#include "../../../src/sdk/query_result_internal.hpp"

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
													 "cc.type.v1",
													 "cc.type_component.v1",
													 "cc.body.v1",
													 "cc.syntax_node.v1",
													 "cc.call_site.v1",
													 "cc.call_direct_target.v1",
													 "cc.call_operand.v1"};
	void require(bool condition, std::string_view label)
	{
		if (!condition)
		{
			std::cerr << label << '\n';
			std::exit(1);
		}
	}
	template <class T>
	T take(result<T> value, const std::source_location caller = std::source_location::current())
	{
		if (!value)
		{
			std::cerr << caller.file_name() << ':' << caller.line() << ' ' << value.error().code
					  << ':' << value.error().field << ':' << value.error().detail << '\n';
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
		std::array<std::vector<q::annotated_row>, 12> rows;
		fixture()
		{
			rows[0] = {fact(0, {{"compile_unit", detached_cell::utf8("unit:a")}})};
			rows[1] = {fact(1,
							{{"snapshot", detached_cell::utf8("source:a")},
							 {"file", detached_cell::utf8("file:a")},
							 {"size", detached_cell::unsigned_integer(100)}})};
			rows[2] = {fact(2,
							{{"span", detached_cell::utf8("span:a")},
							 {"snapshot", detached_cell::utf8("source:a")},
							 {"file", detached_cell::utf8("file:a")},
							 {"begin", detached_cell::unsigned_integer(2)},
							 {"end", detached_cell::unsigned_integer(8)}})};
			rows[3] = {fact(3,
							{{"entity", detached_cell::utf8("function:a")},
							 {"kind", detached_cell::utf8("function")}})};
			rows[4] = {fact(4,
							{{"detail", detached_cell::utf8("detail:a")},
							 {"entity", detached_cell::utf8("function:a")},
							 {"compile_unit", detached_cell::utf8("unit:a")},
							 {"source", detached_cell::utf8("span:a")},
							 {"call_site_count", detached_cell::unsigned_integer(1)},
							 {"call_site_ids", symbols({"call:a"})},
							 {"call_site_state", detached_cell::utf8("complete")},
							 {"call_site_profile",
							  detached_cell::utf8("clang22-function-emitted-call-sites/1")}})};
			rows[5] = {
				fact(5,
					 {{"type", detached_cell::utf8("type:int")},
					  {"constructor", detached_cell::utf8("builtin")},
					  {"structure_profile", detached_cell::utf8("clang22-structural-type/1")},
					  {"structure_preimage", detached_cell::utf8("builtin:Int")},
					  {"structure_state", detached_cell::utf8("complete")},
					  {"component_signature_digest",
					   detached_cell::utf8(take(
						   semantic_digest("cc.clang22.type-components.v1", "builtin:Int")))}})};
			rows[8] = {
				fact(8,
					 {{"node", detached_cell::utf8("syntax:a")},
					  {"compile_unit", detached_cell::utf8("unit:a")},
					  {"function", detached_cell::utf8("function:a")},
					  {"source", detached_cell::utf8("span:a")},
					  {"kind", detached_cell::utf8("CallExpr")},
					  {"flags", symbols({"admitted_call_site", "finite_call_admission_v1"})}})};
			rows[9] = {
				fact(9,
					 {{"call", detached_cell::utf8("call:a")},
					  {"compile_unit", detached_cell::utf8("unit:a")},
					  {"caller", detached_cell::utf8("function:a")},
					  {"source", detached_cell::utf8("span:a")},
					  {"expression", detached_cell::utf8("syntax:a")},
					  {"kind", detached_cell::utf8("direct_function")},
					  {"argument_count", detached_cell::unsigned_integer(1)},
					  {"operand_count", detached_cell::unsigned_integer(1)},
					  {"operand_population_state", detached_cell::utf8("complete")},
					  {"operand_profile", detached_cell::utf8("clang22-actual-call-operands/1")}})};
			rows[11] = {fact(11,
							 {{"operand", detached_cell::utf8("operand:a")},
							  {"call", detached_cell::utf8("call:a")},
							  {"compile_unit", detached_cell::utf8("unit:a")},
							  {"kind", detached_cell::utf8("argument")},
							  {"index", detached_cell::unsigned_integer(0)},
							  {"actual_argument_index", detached_cell::unsigned_integer(0)},
							  {"formal_parameter_index", detached_cell::unsigned_integer(0)},
							  {"written_argument_index", detached_cell::unsigned_integer(0)},
							  {"origin", detached_cell::utf8("written")},
							  {"source", detached_cell::utf8("span:a")},
							  {"expression", detached_cell::utf8("syntax:a")},
							  {"type", detached_cell::utf8("type:int")},
							  {"value_category", detached_cell::utf8("prvalue")},
							  {"evaluation_state", detached_cell::utf8("evaluated")},
							  {"observation_state", detached_cell::utf8("complete")}})};
		}
		q::call_operand_input input(bool closed = true) const
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
					closed,
					closed,
					closed,
					closed,
					closed};
		}
		q::application_query_results queries() const
		{
			q::application_query_results value;
			value.snapshot_id = "query:calls";
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
	};
	q::call_operand_projection project(const fixture& value)
	{
		return take(q::project_call_operands(value.input()));
	}
	void check_state(const fixture& value, state expected, std::string_view label)
	{
		auto result = project(value);
		require(result.calls.size() == 1 && result.calls[0].state == expected, label);
	}
} // namespace
int main()
{
	{
		fixture original;
		const auto check_scopes = [&](const auto& input)
		{
			q::projection_resource_usage usage{777U, 888U};
			auto measured = q::project_function_call_scopes(input, {}, {}, usage);
			auto ordinary = q::project_function_call_scopes(input);
			require(measured && ordinary && usage.operations > 0U &&
						usage.retained_bytes_bound > 0U && measured->calls.empty(),
					"scope-only successful usage missing");
			require(usage.operations < q::finite_population_limits{}.maximum_operations &&
						usage.retained_bytes_bound <
							q::finite_population_limits{}.maximum_retained_bytes,
					"scope-only configured maxima reported as actual usage");
			require(measured->evidence.size() == ordinary->evidence.size() &&
						measured->function_scopes.size() == ordinary->function_scopes.size(),
					"scope-only usage changed original population");
			for (std::size_t i{}; i < measured->evidence.size(); ++i)
				require(measured->evidence[i].row.canonical_form() ==
							ordinary->evidence[i].row.canonical_form(),
						"scope-only usage changed original evidence");
			for (std::size_t i{}; i < measured->function_scopes.size(); ++i)
			{
				const auto& left = measured->function_scopes[i];
				const auto& right = ordinary->function_scopes[i];
				require(left.function == right.function &&
							left.compile_unit == right.compile_unit && left.body == right.body &&
							left.state == right.state && left.call_count == right.call_count &&
							left.call_ids == right.call_ids,
						"scope-only usage changed original scope admission");
			}
			q::finite_population_limits exact;
			exact.maximum_operations = usage.operations;
			exact.maximum_retained_bytes = usage.retained_bytes_bound;
			q::projection_resource_usage repeated{777U, 888U};
			require(q::project_function_call_scopes(input, exact, {}, repeated) &&
						repeated.operations == usage.operations &&
						repeated.retained_bytes_bound == usage.retained_bytes_bound,
					"scope-only exact bounds or deterministic charges changed");
			for (const bool reduce_work : {false, true})
			{
				auto below = exact;
				if (reduce_work)
					--below.maximum_operations;
				else
					--below.maximum_retained_bytes;
				repeated = {777U, 888U};
				require(!q::project_function_call_scopes(input, below, {}, repeated) &&
							repeated.operations == 0U && repeated.retained_bytes_bound == 0U,
						"scope-only below-bound failure leaked successful usage");
			}
			std::stop_source stopped;
			stopped.request_stop();
			repeated = {777U, 888U};
			require(!q::project_function_call_scopes(input, {}, stopped.get_token(), repeated) &&
						repeated.operations == 0U && repeated.retained_bytes_bound == 0U,
					"scope-only cancellation leaked successful usage");
		};
		check_scopes(original.input());
		check_scopes(original.queries());
	}
	{
		q::projection_resource_usage charged{777U, 888U};
		fixture original;
		auto with_usage = q::project_call_operands(original.input(), {}, {}, charged);
		auto without_usage = q::project_call_operands(original.input());
		require(with_usage && without_usage && charged.operations > 0U &&
					charged.retained_bytes_bound > 0U,
				"successful raw usage missing");
		require(charged.operations < q::finite_population_limits{}.maximum_operations &&
					charged.retained_bytes_bound <
						q::finite_population_limits{}.maximum_retained_bytes,
				"configured maxima were reported as actual usage");
		require(with_usage->evidence.size() == without_usage->evidence.size() &&
					with_usage->calls.size() == without_usage->calls.size(),
				"usage changed original payload");
		charged = {777U, 888U};
		auto query_usage = q::project_call_operands(original.queries(), {}, {}, charged);
		require(query_usage && charged.operations > 0U && charged.retained_bytes_bound > 0U,
				"successful public-query usage missing");
		q::finite_population_limits tiny;
		tiny.maximum_rows = 1U;
		charged = {777U, 888U};
		require(!q::project_call_operands(original.input(), tiny, {}, charged) &&
					charged.operations == 0U && charged.retained_bytes_bound == 0U,
				"failed projection leaked usage");
		std::stop_source usage_stop;
		usage_stop.request_stop();
		charged = {777U, 888U};
		require(!q::project_call_operands(original.input(), {}, usage_stop.get_token(), charged) &&
					charged.operations == 0U && charged.retained_bytes_bound == 0U,
				"cancelled projection leaked usage");
		// The generated syntax row has many absent, short cells. Their owned
		// scalar/map storage still needs a bound even with little value text.
		q::finite_population_limits cell_storage;
		cell_storage.maximum_retained_bytes =
			original.rows[8].front().values.size() * sizeof(detached_cell);
		charged = {777U, 888U};
		require(!q::project_call_operands(original.input(), cell_storage, {}, charged) &&
					charged.operations == 0U && charged.retained_bytes_bound == 0U,
				"short/absent raw cells escaped owned storage limits");
		charged = {777U, 888U};
		require(!q::project_call_operands(original.queries(), cell_storage, {}, charged) &&
					charged.operations == 0U && charged.retained_bytes_bound == 0U,
				"short/absent query cells escaped owned storage limits");
	}

	{
		fixture f;
		for (std::size_t i{}; i < 512U; ++i)
		{
			const auto id = "excluded:" + std::to_string(i);
			f.rows[8].push_back(fact(8,
									 {{"node", detached_cell::utf8(id)},
									  {"compile_unit", detached_cell::utf8("unit:a")},
									  {"function", detached_cell::utf8("function:a")},
									  {"source", detached_cell::utf8(id)},
									  {"kind", detached_cell::utf8("IntegerLiteral")},
									  {"flags", symbols({"finite_call_admission_v1"})}}));
			f.rows[2].push_back(fact(2,
									 {{"span", detached_cell::utf8(id)},
									  {"snapshot", detached_cell::utf8("source:a")},
									  {"file", detached_cell::utf8("file:a")},
									  {"begin", detached_cell::unsigned_integer(2U)},
									  {"end", detached_cell::unsigned_integer(3U)}}));
			f.rows[3].push_back(fact(
				3,
				{{"entity", detached_cell::utf8(id)}, {"kind", detached_cell::utf8("variable")}}));
			f.rows[4].push_back(fact(4,
									 {{"entity", detached_cell::utf8(id)},
									  {"compile_unit", detached_cell::utf8("unit:a")},
									  {"source", detached_cell::utf8(id)}}));
			f.rows[5].push_back(fact(5,
									 {{"type", detached_cell::utf8(id)},
									  {"constructor", detached_cell::utf8("builtin")}}));
		}
		q::finite_population_limits limits;
		limits.maximum_retained_bytes = 512U * 1024U;
		const auto raw = take(q::project_call_operands(f.input(), limits));
		require(raw.calls.size() == 1U && raw.function_scopes.size() == 1U &&
					raw.function_scopes.front().state == state::complete,
				"raw independently excluded syntax/variables/types must not own "
				"unrelated evidence");
		const auto query = take(q::project_call_operands(f.queries(), limits));
		require(query.calls.size() == 1U && query.function_scopes.size() == 1U &&
					query.function_scopes.front().state == state::complete &&
					query.source_queries && query.source_queries->scans.size() == 12U,
				"query selective semantic retention preserves all original handles");
		require(raw.evidence.size() == query.evidence.size() && raw.evidence.size() < 30U,
				"only original referenced/admitted semantic carriers retained");
		set(f.rows[8].back(), "ordinal", detached_cell::utf8("malformed"));
		require(!q::project_call_operands(f.input(), limits) &&
					!q::project_call_operands(f.queries(), limits),
				"excluded syntax still gets exact descriptor validation");
	}
	{
		fixture f;
		auto excluded = f.rows[8].front();
		set(excluded, "flags", symbols({"finite_call_admission_v1"}));
		f.rows[8].push_back(excluded);
		const auto p = project(f);
		require(p.function_scopes.front().state == state::conflicting,
				"same original admitted ID conflicting exclusion remains retained");
	}
	{
		fixture f;
		auto syntax = f.rows[8].front();
		set(syntax, "node", detached_cell::utf8("unknown:syntax"));
		set(syntax, "flags", symbols({}));
		f.rows[8].push_back(syntax);
		require(project(f).function_scopes.front().state != state::complete,
				"missing independent admission marker cannot be dropped as noncall");
		set(f.rows[8].back(), "flags", symbols({"admitted_call_site", "finite_call_admission_v1"}));
		require(project(f).function_scopes.front().state != state::complete,
				"actual admitted original syntax without original call remains "
				"unknown");
		set(f.rows[8].back(), "flags", symbols({"finite_call_admission_v1"}));
		set(f.rows[8].back(), "source", detached_cell::utf8("missing:irrelevant"));
		require(project(f).function_scopes.front().state == state::complete,
				"actual known excluded syntax missing source does not poison call "
				"census");
	}
	{
		fixture f;
		auto query = f.queries();
		q::finite_population_limits limits;
		limits.maximum_rows = 1U;
		require(!q::project_call_operands(f.input(), limits) &&
					!q::project_call_operands(query, limits),
				"all original rows still consume row quota");
		limits = {};
		limits.maximum_condition_expansions = 1U;
		require(!q::project_call_operands(f.input(), limits) &&
					!q::project_call_operands(query, limits),
				"all original rows still consume world quota");
	}

	{
		fixture f;
		f.rows[3].clear();
		f.rows[7] = {fact(7,
						  {{"body", detached_cell::utf8("body:known")},
						   {"compile_unit", detached_cell::utf8("unit:a")},
						   {"function", detached_cell::utf8("function:a")},
						   {"source", detached_cell::utf8("span:a")}})};
		const auto raw = project(f);
		const auto query = take(q::project_call_operands(f.queries()));
		require(raw.calls.front().body == "body:known" && query.calls.front().body == "body:known",
				"actual source-bound call body survives missing independent caller "
				"entity");
	}
	{
		fixture f;
		auto syntax = f.rows[8].front();
		set(syntax, "node", detached_cell::utf8(""));
		set(syntax, "flags", symbols({"finite_call_admission_v1"}));
		f.rows[8].push_back(syntax);
		require(!q::project_call_operands(f.input()) && !q::project_call_operands(f.queries()),
				"excluded original semantic carrier must still have its actual "
				"identity");
	}

	fixture input;
	auto result = project(input);
	require(result.calls.size() == 1 && result.calls[0].state == state::complete,
			"actual call operands did not close");
	require(result.function_scopes.size() == 1 &&
				result.function_scopes[0].state == state::complete &&
				result.function_scopes[0].body.empty(),
			"bodyless original scope lost");
	require(result.calls[0].expression == "syntax:a" &&
				result.calls[0].operands[0].referenced_entity.empty() &&
				result.calls[0].operands[0].reference_state == state::complete,
			"opaque argument became named storage");
	require(result.calls[0].operands[0].source_state == state::complete &&
				result.calls[0].operands[0].type_state == state::complete,
			"independent operand facets lost");
	auto owned = input.queries();
	auto public_result = take(q::project_call_operands(owned));
	require(public_result.source_queries &&
				public_result.source_queries->scans.size() == owned.scans.size() &&
				public_result.calls[0].state == state::complete,
			"owned queries lost original scans");

	// An independent source scan also carries lexical token spans. Its retained
	// query handle keeps every original row, while the projection owns only
	// actual referenced source carriers and still validates all input cells.
	fixture lexical;
	for (std::size_t i = 0; i < 512; ++i)
	{
		auto span = lexical.rows[2][0];
		set(span, "span", detached_cell::utf8("token:unrelated:" + std::to_string(i)));
		lexical.rows[2].push_back(std::move(span));
	}
	q::finite_population_limits source_limits;
	source_limits.maximum_retained_bytes = 512U * 1024U;
	const auto lexical_queries = lexical.queries();
	auto lexical_result = take(q::project_call_operands(lexical_queries, source_limits));
	require(lexical_result.calls[0].state == state::complete &&
				lexical_result.function_scopes[0].state == state::complete,
			"unrelated token sources exhausted the relevant call projection");
	require(std::ranges::count(lexical_result.evidence,
							   "source.span.v1",
							   &q::finite_population_evidence::relation_id) == 1,
			"unreferenced token sources were copied into call evidence");
	const auto saved_span = std::ranges::find(lexical_result.source_queries->scans,
											  "source.span.v1",
											  &q::application_relation_scan::relation_id);
	require(saved_span != lexical_result.source_queries->scans.end() &&
				q::query_transfer_access::borrow_rows(saved_span->result).size() == 513 &&
				saved_span->result.inputs_complete(),
			"original complete source query handle/rows were lost");
	auto source_scope = take(q::project_function_call_scopes(lexical_queries, source_limits));
	require(source_scope.function_scopes[0].state == state::complete &&
				source_scope.source_queries->scans.size() == lexical_queries.scans.size(),
			"scope-only source borrowing lost original handles");
	auto malformed_lexical = lexical;
	malformed_lexical.rows[2].back().values.at("output.begin").type = {
		scalar_kind::utf8_string, "", false};
	require(!q::project_call_operands(malformed_lexical.queries(), source_limits),
			"unreferenced malformed source row escaped validation");
	auto too_many_sources = source_limits;
	too_many_sources.maximum_rows = 100;
	require(!q::project_call_operands(lexical_queries, too_many_sources),
			"source borrowing bypassed original row quota");
	too_many_sources = source_limits;
	too_many_sources.maximum_condition_expansions = 100;
	require(!q::project_call_operands(lexical_queries, too_many_sources),
			"unreferenced sources bypassed condition quota");
	auto related_duplicate = lexical;
	auto duplicate_source = related_duplicate.rows[2][0];
	set(duplicate_source, "end", detached_cell::unsigned_integer(9));
	related_duplicate.rows[2].push_back(std::move(duplicate_source));
	auto conflicted_source =
		take(q::project_call_operands(related_duplicate.queries(), source_limits));
	require(conflicted_source.calls[0].state == state::conflicting,
			"borrowed matching source contradiction gained a convenient winner");
	auto duplicate_scan = lexical_queries;
	const auto span_scan = std::ranges::find(
		duplicate_scan.scans, "source.span.v1", &q::application_relation_scan::relation_id);
	duplicate_scan.scans.push_back(*span_scan);
	auto duplicates = take(q::project_call_operands(duplicate_scan, source_limits));
	require(duplicates.calls[0].state == state::complete &&
				std::ranges::count(duplicates.evidence,
								   "source.span.v1",
								   &q::finite_population_evidence::relation_id) == 2,
			"multiple original scans lost identical matching evidence");

	// The raw borrowing entry path has the same finite source-carrier domain.
	// Model binders already own all TU spans; unrelated lexical spans must not
	// become another owned copy inside their nested projection budget.
	auto raw_lexical = take(q::project_call_operands(lexical.input(), source_limits));
	require(raw_lexical.calls[0].state == state::complete &&
				raw_lexical.function_scopes[0].state == state::complete &&
				std::ranges::count(raw_lexical.evidence,
								   "source.span.v1",
								   &q::finite_population_evidence::relation_id) == 1,
			"raw input copied unrelated source carriers");
	auto raw_scope = take(q::project_function_call_scopes(lexical.input(), source_limits));
	require(raw_scope.function_scopes[0].state == state::complete && !raw_scope.source_queries,
			"raw scope source selection invented original query handles");
	require(!q::project_call_operands(malformed_lexical.input(), source_limits),
			"raw unreferenced malformed source row escaped validation");
	auto unannotated_source = lexical;
	unannotated_source.rows[2].back().contributor_edges.clear();
	require(!q::project_call_operands(unannotated_source.input(), source_limits),
			"raw unreferenced source annotation escaped validation");
	auto raw_rows = source_limits;
	raw_rows.maximum_rows = 100;
	auto exhausted_raw_rows = q::project_call_operands(lexical.input(), raw_rows);
	require(!exhausted_raw_rows && exhausted_raw_rows.error().code == "sdk.call-budget",
			"raw source selection bypassed the original row quota");
	auto raw_conditions = source_limits;
	raw_conditions.maximum_condition_expansions = 100;
	auto exhausted_raw_conditions = q::project_call_operands(lexical.input(), raw_conditions);
	require(!exhausted_raw_conditions && exhausted_raw_conditions.error().code == "sdk.call-budget",
			"raw source selection bypassed the original condition quota");
	auto raw_source_conflict =
		take(q::project_call_operands(related_duplicate.input(), source_limits));
	require(raw_source_conflict.calls[0].state == state::conflicting &&
				std::ranges::count(raw_source_conflict.evidence,
								   "source.span.v1",
								   &q::finite_population_evidence::relation_id) == 2,
			"raw referenced source conflict or original evidence was discarded");

	auto missing_syntax = owned;
	missing_syntax.scans.erase(std::ranges::find(
		missing_syntax.scans, "cc.syntax_node.v1", &q::application_relation_scan::relation_id));
	auto incomplete_scope = take(q::project_function_call_scopes(missing_syntax));
	require(!incomplete_scope.call_inputs_complete &&
				incomplete_scope.function_scopes[0].state == state::unknown &&
				incomplete_scope.source_queries &&
				incomplete_scope.source_queries->scans.size() == missing_syntax.scans.size(),
			"missing independent syntax query became a complete call scope or "
			"lost original inputs");
	auto scope_result = take(q::project_function_call_scopes(input.input()));
	require(scope_result.calls.empty() && scope_result.function_scopes[0].state == state::complete,
			"scope-only API coupled to operands");
	input.rows[11].clear();
	set(input.rows[9][0], "argument_count", detached_cell::unsigned_integer(0));
	set(input.rows[9][0], "operand_count", detached_cell::unsigned_integer(0));
	check_state(input, state::complete, "explicit empty operand population not distinguished");
	auto open = input.input();
	open.operand_inputs_complete = false;
	require(take(q::project_call_operands(open)).calls[0].state == state::unknown,
			"unclosed empty inputs became zero");
	set(input.rows[9][0],
		"operand_count",
		detached_cell::absent(input.rows[9][0].values.at("output.operand_count").type));
	check_state(input, state::unknown, "missing original count became zero");
	input = fixture{};
	input.rows[11].clear();
	check_state(input, state::partial, "missing operand row became complete");
	input = fixture{};
	input.rows[11].push_back(input.rows[11][0]);
	input.rows[11].back().provenance = {"duplicate:source"};
	input.rows[11].back().contributor_edges.front().provenance = "duplicate:source";
	result = project(input);
	require(result.calls[0].state == state::complete && result.calls[0].operands.size() == 1 &&
				result.calls[0].operands[0].evidence.size() > 4,
			"equal duplicate annotations lost or duplicated count");
	set(input.rows[11].back(), "type", detached_cell::utf8("type:foreign"));
	check_state(input, state::conflicting, "duplicate operand disagreement gained a winner");
	input = fixture{};
	input.rows[11].push_back(input.rows[11][0]);
	set(input.rows[11].back(), "operand", detached_cell::utf8("operand:other"));
	check_state(input, state::conflicting, "duplicate original kind/index domain accepted");
	input = fixture{};
	set(input.rows[11][0], "compile_unit", detached_cell::utf8("unit:other"));
	check_state(input, state::conflicting, "cross-unit operand joined");
	for (const auto axis : {"variant", "universe", "interpretation"})
	{
		input = fixture{};
		if (std::string_view{axis} == "variant")
			input.rows[11][0].presence.fragments = {"release"};
		if (std::string_view{axis} == "universe")
			input.rows[11][0].presence.universe = "other";
		if (std::string_view{axis} == "interpretation")
			input.rows[11][0].interpretation = "other";
		input.rows[11][0].contributor_edges.front().condition = input.rows[11][0].presence;
		input.rows[11][0].contributor_edges.front().interpretation =
			input.rows[11][0].interpretation;
		check_state(input, state::partial, "cross-world operand joined");
	}
	input = fixture{};
	input.rows[2][0].presence.fragments = {"release"};
	input.rows[2][0].contributor_edges.front().condition = input.rows[2][0].presence;
	check_state(input, state::partial, "foreign world source joined");
	input = fixture{};
	set(input.rows[2][0], "end", detached_cell::unsigned_integer(101));
	check_state(input, state::conflicting, "source outside actual snapshot accepted");
	input = fixture{};
	set(input.rows[8][0], "compile_unit", detached_cell::utf8("unit:other"));
	check_state(input, state::conflicting, "syntax from another unit joined");
	input = fixture{};
	input.rows[1].push_back(fact(1,
								 {{"snapshot", detached_cell::utf8("source:decl")},
								  {"file", detached_cell::utf8("file:decl")},
								  {"size", detached_cell::unsigned_integer(20)}}));
	input.rows[2].push_back(fact(2,
								 {{"span", detached_cell::utf8("span:default")},
								  {"snapshot", detached_cell::utf8("source:decl")},
								  {"file", detached_cell::utf8("file:decl")},
								  {"end", detached_cell::unsigned_integer(5)}}));
	input.rows[8].push_back(fact(8,
								 {{"node", detached_cell::utf8("syntax:default")},
								  {"compile_unit", detached_cell::utf8("unit:a")},
								  {"function", detached_cell::utf8("function:declaration")},
								  {"source", detached_cell::utf8("span:default")}}));
	set(input.rows[11][0], "default_argument", detached_cell::boolean(true));
	set(input.rows[11][0], "origin", detached_cell::utf8("default_argument"));
	set(input.rows[11][0],
		"written_argument_index",
		detached_cell::absent(input.rows[11][0].values.at("output.written_argument_index").type));
	set(input.rows[11][0], "source", detached_cell::utf8("span:default"));
	set(input.rows[11][0], "expression", detached_cell::utf8("syntax:default"));
	result = project(input);
	require(result.calls[0].state == state::complete &&
				result.calls[0].operands[0].file == "file:decl",
			"default argument declaration source lost");
	set(input.rows[11][0], "written_argument_index", detached_cell::unsigned_integer(0));
	check_state(input, state::conflicting, "default argument incorrectly marked written");
	input = fixture{};
	set(input.rows[11][0],
		"formal_parameter_index",
		detached_cell::absent(input.rows[11][0].values.at("output.formal_parameter_index").type));
	set(input.rows[11][0], "evaluation_state", detached_cell::utf8("unevaluated"));
	result = project(input);
	require(result.calls[0].state == state::complete &&
				result.calls[0].operands[0].evaluation_state == "unevaluated" &&
				!result.calls[0].operands[0].formal_parameter_index,
			"variadic/unevaluated original operands lost");
	input = fixture{};
	set(input.rows[9][0], "kind", detached_cell::utf8("operator"));
	set(input.rows[11][0], "actual_argument_index", detached_cell::unsigned_integer(1));
	check_state(input,
				state::complete,
				"static operator original AST argument axis was forced to formal index");
	input = fixture{};
	set(input.rows[11][0],
		"referenced_entity",
		detached_cell::unknown(input.rows[11][0].values.at("output.referenced_entity").type,
							   "dependent-reference"));
	set(input.rows[11][0], "observation_state", detached_cell::utf8("partial"));
	result = project(input);
	require(result.calls[0].state == state::complete &&
				result.calls[0].operands[0].reference_state == state::unknown &&
				result.calls[0].operands[0].referenced_entity.empty(),
			"unknown named binding erased independent finite slots or became "
			"known absence");
	input = fixture{};
	set(input.rows[4][0], "call_site_ids", symbols({}));
	set(input.rows[4][0], "call_site_count", detached_cell::unsigned_integer(0));
	require(project(input).function_scopes[0].state == state::conflicting,
			"false empty function scope erased actual call");
	input.rows[9].clear();
	input.rows[11].clear();
	require(project(input).function_scopes[0].state == state::partial,
			"remaining admitted syntax became a false empty call scope");
	input.rows[8].clear();
	require(project(input).function_scopes[0].state == state::complete,
			"true bodyless empty scope lost");
	input = fixture{};
	input.rows[9].clear();
	input.rows[11].clear();
	require(project(input).function_scopes[0].state == state::partial,
			"missing original call in scope became complete");
	input = fixture{};
	set(input.rows[4][0], "call_site_count", detached_cell::unsigned_integer(2));
	require(project(input).function_scopes[0].state == state::conflicting,
			"scope original count contradiction accepted");
	input = fixture{};
	input.rows[4].push_back(input.rows[4][0]);
	set(input.rows[4].back(), "call_site_ids", symbols({}));
	require(project(input).function_scopes[0].state == state::conflicting,
			"conflicting scope gained first-wins");
	input = fixture{};
	set(input.rows[4][0], "flags", symbols({"body_written", "finite_function_body_v1"}));
	input.rows[7] = {fact(7,
						  {{"body", detached_cell::utf8("body:definition")},
						   {"compile_unit", detached_cell::utf8("unit:a")},
						   {"function", detached_cell::utf8("function:a")},
						   {"source", detached_cell::utf8("span:a")}})};
	input.rows[2].push_back(fact(2,
								 {{"span", detached_cell::utf8("span:prototype")},
								  {"snapshot", detached_cell::utf8("source:a")},
								  {"file", detached_cell::utf8("file:a")},
								  {"begin", detached_cell::unsigned_integer(20)},
								  {"end", detached_cell::unsigned_integer(30)}}));
	auto prototype = input.rows[4][0];
	set(prototype, "detail", detached_cell::utf8("detail:prototype"));
	set(prototype, "source", detached_cell::utf8("span:prototype"));
	set(prototype, "call_site_ids", symbols({}));
	set(prototype, "call_site_count", detached_cell::unsigned_integer(0));
	set(prototype, "flags", symbols({"body_not_written", "finite_function_body_v1"}));
	input.rows[4].push_back(prototype);
	result = project(input);
	require(result.function_scopes.size() == 2 &&
				result.function_scopes[0].state == state::complete &&
				result.function_scopes[1].state == state::complete,
			"definition call poisoned separate prototype scope");
	for (const auto& scope : result.function_scopes)
		require(scope.source_span == "span:a" ? scope.body == "body:definition"
											  : scope.body.empty(),
				"prototype inherited a different declaration body");
	input.rows[2].push_back(fact(2,
								 {{"span", detached_cell::utf8("span:prototype-call")},
								  {"snapshot", detached_cell::utf8("source:a")},
								  {"file", detached_cell::utf8("file:a")},
								  {"begin", detached_cell::unsigned_integer(22)},
								  {"end", detached_cell::unsigned_integer(24)}}));
	auto default_call = input.rows[9][0];
	set(default_call, "call", detached_cell::utf8("call:prototype"));
	set(default_call, "source", detached_cell::utf8("span:prototype-call"));
	set(default_call, "expression", detached_cell::utf8("syntax:prototype-call"));
	input.rows[8].push_back(
		fact(8,
			 {{"node", detached_cell::utf8("syntax:prototype-call")},
			  {"compile_unit", detached_cell::utf8("unit:a")},
			  {"function", detached_cell::utf8("function:a")},
			  {"source", detached_cell::utf8("span:prototype-call")},
			  {"kind", detached_cell::utf8("CallExpr")},
			  {"flags", symbols({"admitted_call_site", "finite_call_admission_v1"})}}));
	set(default_call, "argument_count", detached_cell::unsigned_integer(0));
	set(default_call, "operand_count", detached_cell::unsigned_integer(0));
	input.rows[9].push_back(default_call);
	set(input.rows[4].back(), "call_site_ids", symbols({"call:prototype"}));
	set(input.rows[4].back(), "call_site_count", detached_cell::unsigned_integer(1));
	result = project(input);
	require(result.function_scopes[1].state == state::complete &&
				result.function_scopes[1].body.empty(),
			"actual bodyless default-call scope lost");
	for (const auto& call : result.calls)
		if (call.call == "call:prototype")
			require(call.body.empty(), "bodyless default call inherited definition body");

	// The same physical definition's optional body mirror must agree, while
	// the separate bodyless prototype keeps its independent enumeration.
	set(input.rows[7][0], "call_site_count", detached_cell::unsigned_integer(0));
	set(input.rows[7][0], "call_site_ids", symbols({}));
	set(input.rows[7][0], "call_site_state", detached_cell::utf8("complete"));
	set(input.rows[7][0],
		"call_site_profile",
		detached_cell::utf8("clang22-function-emitted-call-sites/1"));
	result = project(input);
	require(result.function_scopes[0].state == state::conflicting &&
				result.function_scopes[1].state == state::complete,
			"contradictory definition body mirror won or poisoned the prototype");
	set(input.rows[7][0], "call_site_count", detached_cell::unsigned_integer(1));
	set(input.rows[7][0], "call_site_ids", symbols({"call:a"}));
	require(project(input).function_scopes[0].state == state::complete,
			"agreeing original definition body mirror failed");
	auto second_body = input.rows[7][0];
	set(second_body, "body", detached_cell::utf8("body:ambiguous"));
	input.rows[7].push_back(second_body);
	require(project(input).function_scopes[0].state == state::conflicting,
			"multiple original body IDs won a complete physical definition scope");
	input.rows[7].pop_back();

	input = fixture{};
	auto duplicate_site = input.rows[9][0];
	set(duplicate_site, "call", detached_cell::utf8("call:duplicate"));
	set(duplicate_site, "argument_count", detached_cell::unsigned_integer(0));
	set(duplicate_site, "operand_count", detached_cell::unsigned_integer(0));
	input.rows[9].push_back(duplicate_site);
	set(input.rows[4][0], "call_site_count", detached_cell::unsigned_integer(2));
	set(input.rows[4][0], "call_site_ids", symbols({"call:a", "call:duplicate"}));
	require(project(input).function_scopes[0].state == state::conflicting,
			"two original call IDs mapped to one admitted syntax expression");
	input = fixture{};
	auto extra_syntax = input.rows[8][0];
	set(extra_syntax, "node", detached_cell::utf8("syntax:missing-call"));
	input.rows[8].push_back(extra_syntax);
	require(project(input).function_scopes[0].state == state::partial,
			"admitted syntax missing from call rows and inventory became complete");
	input = fixture{};
	set(input.rows[8][0], "flags", symbols({"finite_call_admission_v1"}));
	require(project(input).function_scopes[0].state == state::conflicting,
			"original call mapped to a known non-admitted syntax node");
	input = fixture{};
	set(input.rows[8][0], "flags", symbols({}));
	require(project(input).function_scopes[0].state == state::partial,
			"absent admission classification became complete");
	input = fixture{};
	set(input.rows[8][0], "compile_unit", detached_cell::utf8("unit:foreign"));
	require(project(input).function_scopes[0].state == state::conflicting,
			"call expression from foreign compile unit joined");
	input = fixture{};
	input.rows[8][0].presence.fragments = {"release"};
	for (auto& edge : input.rows[8][0].contributor_edges)
		edge.condition = input.rows[8][0].presence;
	require(project(input).function_scopes[0].state == state::partial,
			"cross-world syntax expression became complete");
	input = fixture{};
	auto contradictory_syntax = input.rows[8][0];
	set(contradictory_syntax, "flags", symbols({"finite_call_admission_v1"}));
	input.rows[8].push_back(contradictory_syntax);
	require(project(input).function_scopes[0].state == state::conflicting,
			"conflicting syntax admission gained first-wins");
	input = fixture{};
	const std::vector<std::byte> usr{std::byte{'f'}, std::byte{0}, std::byte{255}};
	const auto signature = take(semantic_digest("call-test-signature", "original"));
	input.rows[10] = {fact(
		10,
		{{"call", detached_cell::utf8("call:a")},
		 {"target", detached_cell::utf8("external:f")},
		 {"resolution", detached_cell::utf8("resolved")},
		 {"target_signature_state", detached_cell::utf8("complete")},
		 {"target_signature_profile", detached_cell::utf8("clang22-original-target-signature/1")},
		 {"target_usr", detached_cell::bytes(usr)},
		 {"target_canonical_type", detached_cell::utf8("type:int")},
		 {"target_structural_signature_digest", detached_cell::utf8(signature)},
		 {"target_canonical_type_digest",
		  input.rows[5][0].values.at("output.component_signature_digest")},
		 {"target_canonical_type_profile", detached_cell::utf8("clang22-structural-type/1")},
		 {"target_language", detached_cell::utf8("cxx")},
		 {"target_linkage", detached_cell::utf8("external")},
		 {"target_module_domain", detached_cell::utf8("<none>")}})};
	result = project(input);
	require(result.calls[0].signatures.size() == 1 &&
				result.calls[0].signatures[0].state == state::complete &&
				result.calls[0].signatures[0].usr == usr,
			"original external signature/non-text USR lost");

	// Missing optional target facets cannot contradict the independent,
	// original declaration identity retained by the entity/detail route.
	const auto complete_target = input.rows[10][0];
	input.rows[3].push_back(
		fact(3,
			 {{"entity", detached_cell::utf8("external:f")},
			  {"kind", detached_cell::utf8("function")},
			  {"provider_local_key", detached_cell::bytes(usr)},
			  {"structural_signature_digest", detached_cell::utf8(signature)}}));
	input.rows[4].push_back(fact(4,
								 {{"detail", detached_cell::utf8("detail:target")},
								  {"entity", detached_cell::utf8("external:f")},
								  {"compile_unit", detached_cell::utf8("unit:a")},
								  {"source", detached_cell::utf8("span:a")},
								  {"canonical_type", detached_cell::utf8("type:int")}}));
	for (const auto name : {"target_signature_state",
							"target_signature_profile",
							"target_usr",
							"target_canonical_type",
							"target_structural_signature_digest",
							"target_canonical_type_digest",
							"target_canonical_type_profile",
							"target_language",
							"target_linkage",
							"target_module_domain"})
	{
		const auto type = input.rows[10][0].values.at("output." + std::string{name}).type;
		set(input.rows[10][0], name, detached_cell::absent(type));
	}
	result = project(input);
	require(result.calls[0].signatures[0].state == state::unknown &&
				result.calls[0].state == state::complete &&
				input.rows[3].back().values.at("output.provider_local_key").value ==
					detached_cell::bytes(usr).value,
			"absent target signature conflicted with independent entity/detail "
			"identity");
	input.rows[10][0] = complete_target;
	for (const auto name : {"target_canonical_type_digest", "target_canonical_type_profile"})
		set(input.rows[10][0],
			name,
			detached_cell::absent(input.rows[10][0].values.at("output." + std::string{name}).type));
	require(project(input).calls[0].signatures[0].state == state::partial,
			"absent target type facets became contradictory against known actual "
			"type");
	input.rows[10][0] = complete_target;
	set(input.rows[3].back(), "provider_local_key", detached_cell::bytes({std::byte{'x'}}));
	require(project(input).calls[0].signatures[0].state == state::conflicting,
			"present contradictory original target USR was accepted");
	input.rows[3].pop_back();
	input.rows[4].pop_back();

	auto child = input.rows[5][0];
	set(child, "type", detached_cell::utf8("type:child"));
	auto leaf = child;
	set(leaf, "type", detached_cell::utf8("type:leaf"));
	set(leaf, "structure_state", detached_cell::utf8("partial"));
	input.rows[5].push_back(child);
	input.rows[5].push_back(leaf);
	input.rows[6].push_back(fact(6,
								 {{"owner_type", detached_cell::utf8("type:int")},
								  {"role", detached_cell::utf8("return")},
								  {"ordinal", detached_cell::unsigned_integer(0)},
								  {"component_type", detached_cell::utf8("type:child")}}));
	input.rows[6].push_back(fact(6,
								 {{"owner_type", detached_cell::utf8("type:child")},
								  {"role", detached_cell::utf8("pointee")},
								  {"ordinal", detached_cell::unsigned_integer(0)},
								  {"component_type", detached_cell::utf8("type:leaf")}}));
	result = project(input);
	require(result.calls[0].signatures[0].state == state::partial &&
				result.calls[0].signatures[0].evidence.size() > 4,
			"partial nested original type node became a complete target signature");
	set(input.rows[5].back(), "structure_state", detached_cell::utf8("complete"));
	require(project(input).calls[0].signatures[0].state == state::complete,
			"available original type-node facets were not retained");
	set(input.rows[5].back(), "structure_state", detached_cell::utf8("unsupported"));
	require(project(input).calls[0].signatures[0].state == state::partial,
			"unsupported nested original type node became a complete target "
			"signature");
	set(input.rows[10][0], "target_canonical_type_digest", detached_cell::utf8(content_digest({})));
	require(project(input).calls[0].signatures[0].state == state::conflicting,
			"target type signature disagreement accepted");
	input = fixture{};
	auto changed = input.rows[11][0];
	changed.values.at("output.index").type = {scalar_kind::utf8_string, "", false};
	input.rows[11][0] = changed;
	require(!q::project_call_operands(input.input()), "raw descriptor type validation bypassed");
	input = fixture{};
	q::finite_population_limits limits;
	limits.maximum_rows = 1;
	require(!q::project_call_operands(input.input(), limits), "row limit not enforced");
	limits = {};
	limits.maximum_retained_bytes = 1;
	require(!q::project_call_operands(input.input(), limits), "retained budget not enforced");
	limits = {};
	limits.maximum_operations = 1;
	require(!q::project_call_operands(input.input(), limits), "operation budget not enforced");
	std::stop_source stopped;
	stopped.request_stop();
	require(!q::project_call_operands(input.input(), {}, stopped.get_token()),
			"cancellation not enforced");
	limits = {};
	std::size_t checkpoints{};
	limits.cancelled = [&]
	{
		return ++checkpoints > 4U;
	};
	auto callback_cancelled = q::project_call_operands(input.input(), limits);
	require(!callback_cancelled && callback_cancelled.error().code == "sdk.call-cancelled" &&
				checkpoints > 4U,
			"active callback cancellation not enforced");
	input = fixture{};
	auto first = project(input);
	for (auto& rows : input.rows)
		std::ranges::reverse(rows);
	auto second = project(input);
	require(first.calls[0].call == second.calls[0].call &&
				first.evidence[0].row.canonical_form() == second.evidence[0].row.canonical_form(),
			"permutation changed original projection");
	std::cout << "call operand and function scope cases passed\n";
}
