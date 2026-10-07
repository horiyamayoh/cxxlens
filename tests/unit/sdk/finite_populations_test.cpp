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
			groups[0] = {row("build.compile_unit.v1",
							 {{"compile_unit", detached_cell::utf8("tu:test")},
							  {"main_source", detached_cell::utf8("snapshot:test")}})};
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
	{
		q::projection_resource_usage charged{777U, 888U};
		fixture original;
		original.declaration();
		auto with_usage = q::project_declarations(original.input(), {}, {}, charged);
		auto without_usage = q::project_declarations(original.input());
		require(with_usage && without_usage && charged.operations > 0U &&
					charged.retained_bytes_bound > 0U,
				"successful raw usage missing");
		require(charged.operations < q::finite_population_limits{}.maximum_operations &&
					charged.retained_bytes_bound <
						q::finite_population_limits{}.maximum_retained_bytes,
				"configured maxima were reported as actual usage");
		require(with_usage->evidence.size() == without_usage->evidence.size() &&
					with_usage->populations[0].members[0].id ==
						without_usage->populations[0].members[0].id,
				"usage changed original declaration payload");
		original.groups[8].clear();
		original.flow();
		charged = {777U, 888U};
		auto flow_usage = q::project_body_flow(original.input(), {}, {}, charged);
		require(flow_usage && charged.operations > 0U && charged.retained_bytes_bound > 0U,
				"successful original flow usage missing");
		q::application_query_results flow_queries;
		flow_queries.snapshot_id = "query:original-flow";
		constexpr std::array<std::string_view, 9> flow_names{"build.compile_unit.v1",
															 "source.file.v1",
															 "source.span.v1",
															 "cc.entity.v1",
															 "cc.entity_detail.v1",
															 "cc.body.v1",
															 "cc.cfg_node.v1",
															 "cc.flow_inventory.v1",
															 "cc.flow_fact.v1"};
		for (std::size_t group = 0; group < flow_names.size(); ++group)
		{
			auto data = std::make_shared<q::query_result::data>();
			data->row_values = original.groups[group];
			data->status = q::execution_status::complete;
			data->input_complete = true;
			data->snapshot = flow_queries.snapshot_id;
			flow_queries.scans.push_back(
				{std::string{flow_names[group]}, {}, q::query_transfer_access::make(data)});
		}
		charged = {777U, 888U};
		auto flow_query_usage = q::project_body_flow(flow_queries, {}, {}, charged);
		require(flow_query_usage && charged.operations > 0U && charged.retained_bytes_bound > 0U,
				"successful original flow query usage missing");
		require(flow_query_usage->evidence.size() == flow_usage->evidence.size() &&
					flow_query_usage->populations[0].state == flow_usage->populations[0].state,
				"query usage changed original flow evidence or membership state");
		std::size_t visited{};
		q::finite_population_limits late;
		late.cancelled = [&]
		{
			return ++visited == 50U;
		};
		charged = {777U, 888U};
		auto stopped_flow = q::project_body_flow(flow_queries, late, {}, charged);
		require(!stopped_flow && charged.operations == 0U && charged.retained_bytes_bound == 0U,
				"late public-query cancellation leaked usage");
		q::finite_population_limits tiny;
		tiny.maximum_rows = 1U;
		charged = {777U, 888U};
		require(!q::project_declarations(original.input(), tiny, {}, charged) &&
					charged.operations == 0U && charged.retained_bytes_bound == 0U,
				"failed projection leaked usage");
		std::stop_source usage_stop;
		usage_stop.request_stop();
		charged = {777U, 888U};
		require(!q::project_declarations(original.input(), {}, usage_stop.get_token(), charged) &&
					charged.operations == 0U && charged.retained_bytes_bound == 0U,
				"cancelled projection leaked usage");
	}

	{
		fixture f;
		f.declaration();
		constexpr std::array<std::string_view, 9> names{"build.compile_unit.v1",
														"source.file.v1",
														"source.span.v1",
														"cc.entity.v1",
														"cc.entity_detail.v1",
														"cc.body.v1",
														"cc.cfg_node.v1",
														"cc.declaration_inventory.v1",
														"cc.declaration.v1"};
		const auto public_queries = [&](bool execution_complete = true,
										bool member_execution_complete = true,
										bool member_conflict = false,
										bool member_disagreement = false)
		{
			q::application_query_results input;
			input.snapshot_id = "snapshot:original-public";
			for (std::size_t group{}; group < names.size(); ++group)
			{
				auto data = std::make_shared<q::query_result::data>();
				data->row_values = f.groups[group];
				data->status = (group == 7U && !execution_complete) ||
						(group == 8U && !member_execution_complete)
					? q::execution_status::truncated
					: q::execution_status::complete;
				data->input_complete = group != 7U && group != 8U;
				if (group == 8U && member_conflict)
					data->conflict_values.emplace_back();
				if (group == 8U && member_disagreement)
					data->disagreement_values.emplace_back();
				data->snapshot = input.snapshot_id;
				input.scans.push_back(
					{std::string{names[group]}, {}, q::query_transfer_access::make(data)});
			}
			return input;
		};
		q::projection_resource_usage public_usage{9U, 9U};
		auto with_public_usage = q::project_declarations(public_queries(), {}, {}, public_usage);
		require(with_public_usage && public_usage.operations > 0U &&
					public_usage.retained_bytes_bound > 0U,
				"successful declaration query usage missing");
		q::finite_population_limits exact;
		exact.maximum_retained_bytes = public_usage.retained_bytes_bound;
		require(q::project_declarations(public_queries(), exact, {}, public_usage).has_value(),
				"caller could not settle and reuse actual source bound");
		exact.maximum_retained_bytes = 1U;
		public_usage = {9U, 9U};
		require(!q::project_declarations(public_queries(), exact, {}, public_usage) &&
					public_usage.operations == 0U && public_usage.retained_bytes_bound == 0U,
				"public declaration failure leaked usage");
		auto complete = q::project_declarations(public_queries());
		require(complete && complete->inventory_inputs_complete &&
					complete->member_inputs_complete &&
					complete->populations[0].state == q::finite_population_state::complete,
				"independent original declaration facet survives generic optional "
				"frontier");
		require(complete->source_queries &&
					!complete->source_queries->scans[8U].result.inputs_complete(),
				"independent named membership erased the original generic frontier");
		auto partial_members = q::project_declarations(public_queries(true, false));
		require(partial_members && !partial_members->member_inputs_complete &&
					partial_members->populations[0].state != q::finite_population_state::complete,
				"truncated member execution closed original named membership");
		for (const bool disagreement : {false, true})
		{
			auto contradicted =
				q::project_declarations(public_queries(true, true, !disagreement, disagreement));
			require(contradicted && !contradicted->member_inputs_complete &&
						contradicted->populations[0].state != q::finite_population_state::complete,
					"contradicted member scan closed original named membership");
		}
		auto no_member_scan = public_queries();
		no_member_scan.scans.erase(no_member_scan.scans.begin() + 8);
		auto absent_members = q::project_declarations(no_member_scan);
		require(absent_members && !absent_members->member_inputs_complete &&
					absent_members->populations[0].state != q::finite_population_state::complete,
				"missing member scan closed original named membership");
		auto missing_execution = q::project_declarations(public_queries(false));
		require(missing_execution && !missing_execution->inventory_inputs_complete,
				"incomplete original inventory execution remains unavailable");
		auto inventory = public_queries();
		inventory.scans.erase(inventory.scans.begin() + 7);
		require(!q::project_declarations(inventory)->inventory_inputs_complete,
				"missing original inventory scan cannot close");
		for (const std::string_view field : {"output.source", "output.entity"})
		{
			auto& binding = f.groups[8U][0U].values.at(std::string{field});
			const auto original = binding;
			binding.value = std::string{"foreign:original-member"};
			auto unbound = q::project_declarations(public_queries());
			require(unbound &&
						unbound->populations[0U].state != q::finite_population_state::complete,
					"independent member scan fabricated an original source/entity binding");
			binding = original;
		}
		f.groups[8].clear();
		auto missing = q::project_declarations(public_queries());
		require(missing && missing->populations[0].state != q::finite_population_state::complete,
				"missing listed original declaration is not known zero");
		f.declaration();
		auto& count = f.groups[7][0].values.at("output.declaration_count");
		count.value = std::uint64_t{2U};
		auto conflict = q::project_declarations(public_queries());
		require(conflict &&
					conflict->populations[0].state == q::finite_population_state::conflicting,
				"original independent declaration count contradiction survives");
	}

	relation_registry registry;
	for (const auto& descriptor : standard_relation_descriptors())
	{
		auto added = registry.add(descriptor);
		if (!added)
			std::cerr << descriptor.id << " / " << added.error().code << " / "
					  << added.error().field << " / " << added.error().detail << "\n";
		require(added.has_value(), "standard descriptor rejected");
	}
	auto built = registry.build("finite-population-test");
	if (!built)
		std::cerr << built.error().code << " / " << built.error().field << " / "
				  << built.error().detail << "\n";
	require(built.has_value(),
			"finite population references do not match their actual target ID types");
	using s = q::finite_population_state;
	fixture d;
	d.declaration();
	auto result = q::project_declarations(d.input());
	require(state(result, s::complete), "finite declaration did not complete");
	require(result->populations[0].members[0].kind == "function" &&
				result->populations[0].members[0].declaration_kind == "Function",
			"canonical kind lost");
	// A saved older declaration scan lacks newly added optional identifier and
	// target-census cells. It retains the original declaration population, and
	// the missing fields remain unobserved in owned evidence rather than padded.
	auto older = d;
	for (auto iterator = older.groups[8][0].values.begin();
		 iterator != older.groups[8][0].values.end();)
		if (iterator->first.starts_with("output.identifier_"))
			iterator = older.groups[8][0].values.erase(iterator);
		else
			++iterator;
	for (auto iterator = older.groups[7][0].values.begin();
		 iterator != older.groups[7][0].values.end();)
		if (iterator->first.starts_with("output.target_slot_"))
			iterator = older.groups[7][0].values.erase(iterator);
		else
			++iterator;
	auto compatible = q::project_declarations(older.input());
	require(state(compatible, s::complete), "optional saved columns made declarations unavailable");
	require(std::ranges::any_of(compatible->evidence,
								[](const auto& evidence)
								{
									return evidence.relation_id == "cc.declaration.v1" &&
										!evidence.row.values.contains("output.identifier_profile");
								}),
			"optional saved fields were fabricated in original evidence");
	older.groups[8][0].values.erase("output.entity");
	require(!q::project_declarations(older.input()), "missing required saved field accepted");
	older = d;
	older.groups[8][0].values.emplace("output.foreign", detached_cell::utf8("foreign"));
	require(!q::project_declarations(older.input()), "foreign projected field accepted");
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
	// Two actual main-source occurrences in one world never establish a
	// Cartesian unit/file comment domain.
	fixture independent;
	independent.comment();
	set(independent.groups[0][0], "main_source", detached_cell::utf8("snapshot:test"));
	auto other_unit = independent.groups[0][0];
	set(other_unit, "compile_unit", detached_cell::utf8("tu:other"));
	set(other_unit, "main_source", detached_cell::utf8("snapshot:other"));
	independent.groups[0].push_back(other_unit);
	auto other_file = independent.groups[1][0];
	set(other_file, "snapshot", detached_cell::utf8("snapshot:other"));
	set(other_file, "file", detached_cell::utf8("file:other"));
	independent.groups[1].push_back(other_file);
	auto other_inventory = independent.groups[7][0];
	set(other_inventory, "inventory", detached_cell::utf8("inventory:other"));
	set(other_inventory, "compile_unit", detached_cell::utf8("tu:other"));
	set(other_inventory, "source_snapshot", detached_cell::utf8("snapshot:other"));
	set(other_inventory, "file", detached_cell::utf8("file:other"));
	set(other_inventory, "comment_count", detached_cell::unsigned_integer(0U));
	set(other_inventory, "comment_bytes", detached_cell::unsigned_integer(0U));
	set(other_inventory, "comments", ids({}));
	independent.groups[7].push_back(other_inventory);
	auto original_comments = q::project_source_comments(independent.input());
	require(original_comments && original_comments->populations.size() == 2U &&
				std::ranges::all_of(original_comments->populations,
									[](const auto& population)
									{
										return population.state == s::complete &&
											!population.id.empty();
									}),
			"unrelated unit/file Cartesian comment populations invented");
	independent.groups[7].pop_back();
	auto missing_main_comments = q::project_source_comments(independent.input());
	require(missing_main_comments && missing_main_comments->populations.size() == 2U &&
				std::ranges::any_of(missing_main_comments->populations,
									[](const auto& population)
									{
										return population.compile_unit == "tu:other" &&
											population.file == "file:other" &&
											population.source_snapshot == "snapshot:other" &&
											population.id.empty() && population.state == s::unknown;
									}),
			"genuine main-source missing comment census omitted or became zero");
	fixture member_only;
	member_only.comment();
	set(member_only.groups[0][0],
		"main_source",
		detached_cell::unknown(member_only.groups[0][0].values.at("output.main_source").type,
							   "fixture-original-boundary-unavailable"));
	member_only.groups[7].clear();
	auto member_comments = q::project_source_comments(member_only.input());
	require(member_comments && member_comments->populations.size() == 1U &&
				member_comments->populations[0].file == "file:test" &&
				member_comments->populations[0].state == s::unknown,
			"actual member/source association lost when inventory is missing");
	member_only.groups[8].clear();
	auto unassociated_comments = q::project_source_comments(member_only.input());
	require(unassociated_comments && unassociated_comments->populations.size() == 1U &&
				unassociated_comments->populations[0].file.empty() &&
				unassociated_comments->populations[0].source_snapshot.empty() &&
				unassociated_comments->populations[0].state == s::unknown &&
				std::ranges::any_of(unassociated_comments->unresolved,
									[](const auto& item)
									{
										return item.code ==
											"sdk.population-unit-source-membership-missing";
									}),
			"missing original association became fabricated file membership");

	fixture i;
	i.include();
	require(state(q::project_source_includes(i.input()), s::complete),
			"known empty includes failed");
	set(i.groups[7][0], "enumeration_state", detached_cell::utf8("unavailable"));
	require(state(q::project_source_includes(i.input()), s::unknown),
			"unopened includes became empty");
	{
		fixture original;
		original.include();
		const auto add_source = [&](std::string snapshot, std::string file)
		{
			auto observed = original.groups[1][0];
			set(observed, "snapshot", detached_cell::utf8(std::move(snapshot)));
			set(observed, "file", detached_cell::utf8(std::move(file)));
			original.groups[1].push_back(std::move(observed));
		};
		add_source("snapshot:worker", "file:worker");
		add_source("snapshot:header", "file:header");
		add_source("snapshot:variant", "file:variant");
		add_source("snapshot:unentered", "file:unentered");
		const auto add_unit = [&](std::string unit, std::string main)
		{
			auto observed = original.groups[0][0];
			set(observed, "compile_unit", detached_cell::utf8(std::move(unit)));
			set(observed, "main_source", detached_cell::utf8(std::move(main)));
			original.groups[0].push_back(std::move(observed));
		};
		add_unit("tu:worker", "snapshot:worker");
		add_unit("tu:variant-a", "snapshot:variant");
		add_unit("tu:variant-b", "snapshot:variant");
		const auto add_inventory =
			[&](std::string id, std::string unit, std::string snapshot, std::string file)
		{
			auto observed = original.groups[7][0];
			set(observed, "inventory", detached_cell::utf8(std::move(id)));
			set(observed, "compile_unit", detached_cell::utf8(std::move(unit)));
			set(observed, "source_snapshot", detached_cell::utf8(std::move(snapshot)));
			set(observed, "file", detached_cell::utf8(std::move(file)));
			original.groups[7].push_back(std::move(observed));
		};
		add_inventory("inventory:main-header", "tu:test", "snapshot:header", "file:header");
		add_inventory("inventory:worker", "tu:worker", "snapshot:worker", "file:worker");
		add_inventory("inventory:worker-header", "tu:worker", "snapshot:header", "file:header");
		add_inventory("inventory:variant-a", "tu:variant-a", "snapshot:variant", "file:variant");
		add_inventory("inventory:variant-b", "tu:variant-b", "snapshot:variant", "file:variant");
		auto projected = q::project_source_includes(original.input());
		require(projected && projected->populations.size() == 6U &&
					std::ranges::all_of(projected->populations,
										[](const auto& population)
										{
											return population.state == s::complete &&
												!population.id.empty();
										}),
				"same-world files fabricated unit/include census alternatives");
		q::application_query_results public_input;
		public_input.snapshot_id = "query:original-includes";
		constexpr std::array<std::string_view, 9U> names{"build.compile_unit.v1",
														 "source.file.v1",
														 "source.span.v1",
														 "cc.entity.v1",
														 "cc.entity_detail.v1",
														 "cc.body.v1",
														 "cc.cfg_node.v1",
														 "source.include_inventory.v1",
														 "source.include.v1"};
		for (std::size_t group{}; group < names.size(); ++group)
		{
			auto data = std::make_shared<q::query_result::data>();
			data->row_values = original.groups[group];
			data->status = q::execution_status::complete;
			data->input_complete = true;
			data->snapshot = public_input.snapshot_id;
			public_input.scans.push_back(
				{std::string{names[group]}, {}, q::query_transfer_access::make(data)});
		}
		auto public_projection = q::project_source_includes(public_input);
		require(public_projection && public_projection->populations.size() == 6U &&
					public_projection->unresolved.empty(),
				"public include query invented missing cross-unit files");
		original.groups[7].pop_back();
		auto missing_main = q::project_source_includes(original.input());
		require(missing_main && missing_main->populations.size() == 6U &&
					std::ranges::any_of(missing_main->populations,
										[](const auto& population)
										{
											return population.compile_unit == "tu:variant-b" &&
												population.source_snapshot == "snapshot:variant" &&
												population.id.empty() &&
												population.state == s::unknown;
										}),
				"actual main-source missing include census omitted or became zero");
	}
	{
		fixture member;
		member.include();
		member.groups[7].clear();
		set(member.groups[0][0],
			"main_source",
			detached_cell::unknown(member.groups[0][0].values.at("output.main_source").type,
								   "fixture-original-main-source-unavailable"));
		member.groups[8] = {row("source.include.v1",
								{{"include", detached_cell::utf8("include:test")},
								 {"compile_unit", detached_cell::utf8("tu:test")},
								 {"source", detached_cell::utf8("span:test")},
								 {"from_file", detached_cell::utf8("file:test")},
								 {"resolution", detached_cell::utf8("external")}})};
		auto missing_member = q::project_source_includes(member.input());
		require(missing_member && missing_member->populations.size() == 1U &&
					missing_member->populations[0].file == "file:test" &&
					missing_member->populations[0].state == s::unknown,
				"original include member/source membership lost without inventory");
		set(member.groups[8][0], "from_file", detached_cell::utf8("file:foreign"));
		require(state(q::project_source_includes(member.input()), s::conflicting),
				"contradictory original include source association accepted");
		member.groups[8].clear();
		auto missing_association = q::project_source_includes(member.input());
		require(missing_association && missing_association->populations.size() == 1U &&
					missing_association->populations[0].source_snapshot.empty() &&
					missing_association->populations[0].file.empty() &&
					missing_association->populations[0].state == s::unknown,
				"missing include source membership fabricated a global file association");
	}
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
	limits = {};
	std::size_t checkpoints{};
	limits.cancelled = [&]
	{
		return ++checkpoints == 4U;
	};
	auto callback_cancelled = q::project_source_comments(c.input(), limits);
	require(!callback_cancelled && callback_cancelled.error().code == "sdk.population-cancelled" &&
				checkpoints == 4U,
			"caller cancellation was not polled during projection");
	d.declaration();
	auto first = q::project_declarations(d.input());
	std::ranges::reverse(d.groups[3]);
	auto second = q::project_declarations(d.input());
	require(first && second &&
				first->populations[0].members[0].id == second->populations[0].members[0].id,
			"ordering changed identity");
	return 0;
}
