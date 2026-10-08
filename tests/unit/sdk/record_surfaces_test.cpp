#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>

#include <cxxlens/sdk/record_surfaces.hpp>

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
		std::array<std::vector<q::annotated_row>, 8> groups;
		fixture()
		{
			groups[0] = {
				row("build.compile_unit.v1", {{"compile_unit", detached_cell::utf8("tu:test")}})};
			groups[1] = {row("source.file.v1",
							 {{"snapshot", detached_cell::utf8("snapshot:test")},
							  {"file", detached_cell::utf8("file:test")},
							  {"size", detached_cell::unsigned_integer(100U)}})};
			groups[2] = {row("source.span.v1",
							 {{"span", detached_cell::utf8("span:record")},
							  {"file", detached_cell::utf8("file:test")},
							  {"snapshot", detached_cell::utf8("snapshot:test")},
							  {"end", detached_cell::unsigned_integer(100U)}})};
			for (const auto& [entity, kind] : std::array{std::pair{"record", "struct"},
														 std::pair{"method", "method"},
														 std::pair{"field", "field"}})
			{
				groups[3].push_back(row("cc.entity.v1",
										{{"entity", detached_cell::utf8(entity)},
										 {"kind", detached_cell::utf8(kind)}}));
				groups[4].push_back(row("cc.entity_detail.v1",
										{{"entity", detached_cell::utf8(entity)},
										 {"compile_unit", detached_cell::utf8("tu:test")},
										 {"source", detached_cell::utf8("span:record")},
										 {"is_definition", detached_cell::boolean(true)},
										 {"access", detached_cell::utf8("public")},
										 {"canonical_type", detached_cell::utf8("type:test")}}));
				if (std::string_view{entity} != "record")
					groups[5].push_back(row("cc.entity_edge.v1",
											{{"compile_unit", detached_cell::utf8("tu:test")},
											 {"source_entity", detached_cell::utf8("record")},
											 {"target_entity", detached_cell::utf8(entity)},
											 {"kind", detached_cell::utf8("owns")},
											 {"resolution", detached_cell::utf8("resolved")}}));
			}
			groups[6] = {row("cc.record_surface.v1",
							 {{"surface", detached_cell::utf8("surface:record")},
							  {"compile_unit", detached_cell::utf8("tu:test")},
							  {"entity", detached_cell::utf8("record")},
							  {"source", detached_cell::utf8("span:record")},
							  {"is_definition", detached_cell::boolean(true)},
							  {"profile", detached_cell::utf8("clang22-explicit-record-surface/1")},
							  {"enumeration_state", detached_cell::utf8("complete")},
							  {"method_count", detached_cell::unsigned_integer(1U)},
							  {"field_count", detached_cell::unsigned_integer(1U)},
							  {"methods", ids({"method"})},
							  {"fields", ids({"field"})}})};
		}
		q::record_surface_input input(bool complete = true) const
		{
			return {groups[0],
					groups[1],
					groups[2],
					groups[3],
					groups[4],
					groups[5],
					groups[6],
					complete,
					groups[7],
					complete,
					complete};
		}
	};
	q::record_surface_projection project(const fixture& data)
	{
		auto result = q::project_record_surfaces(data.input());
		if (!result)
			std::cerr << result.error().code << ':' << result.error().field << ':'
					  << result.error().detail << '\n';
		require(result.has_value(), "projection failed");
		return std::move(*result);
	}
	bool gap(const q::record_surface& value, std::string_view code)
	{
		return std::ranges::any_of(value.gaps,
								   [&](const auto& g)
								   {
									   return g.code == code;
								   });
	}
	void replace(q::annotated_row& row, std::string name, detached_cell value)
	{
		value.type = row.values.at("output." + name).type;
		row.values["output." + name] = std::move(value);
	}
	fixture type_fixture()
	{
		fixture data;
		for (const auto& [entity, kind] :
			 std::array{std::pair{"target:enum", "enum"}, std::pair{"target:record", "class"}})
		{
			data.groups[3].push_back(row(
				"cc.entity.v1",
				{{"entity", detached_cell::utf8(entity)}, {"kind", detached_cell::utf8(kind)}}));
		}
		for (const auto target : {"record", "target:enum", "target:record"})
			data.groups[5].push_back(row("cc.entity_edge.v1",
										 {{"compile_unit", detached_cell::utf8("tu:test")},
										  {"source", detached_cell::utf8("span:record")},
										  {"source_entity", detached_cell::utf8("record")},
										  {"target_entity", detached_cell::utf8(target)},
										  {"kind", detached_cell::utf8("uses_type")},
										  {"resolution", detached_cell::utf8("resolved")}}));
		auto& surface = data.groups[6].front();
		replace(surface, "type_reference_count", detached_cell::unsigned_integer(3U));
		replace(surface, "type_reference_state", detached_cell::utf8("complete"));
		replace(surface,
				"type_reference_profile",
				detached_cell::utf8("clang22-explicit-nonsystem-nominal-type-uses/1"));
		replace(surface, "type_reference_targets", ids({"record", "target:enum", "target:record"}));
		return data;
	}
	void type_reference_tests()
	{
		auto data = type_fixture();
		const auto actual = project(data);
		const auto& surface = actual.surfaces.front();
		require(surface.state == q::record_surface_state::complete &&
					surface.type_reference_state == q::record_surface_state::complete &&
					surface.declared_type_references == 3U &&
					surface.type_reference_targets ==
						std::vector<std::string>{"record", "target:enum", "target:record"},
				"nominal references, enum, or explicit self reference lost");
		for (auto& group : data.groups)
			std::ranges::reverse(group);
		const auto reordered = project(data);
		require(reordered.surfaces.front().evidence == surface.evidence &&
					reordered.surfaces.front().type_reference_targets ==
						surface.type_reference_targets,
				"type reference ordering changed evidence or target set");
		data = type_fixture();
		replace(data.groups[5].back(), "source", detached_cell::utf8("span:other"));
		const auto missing_edge = project(data).surfaces.front();
		require(missing_edge.state == q::record_surface_state::complete &&
					missing_edge.type_reference_state == q::record_surface_state::partial &&
					gap(missing_edge, "sdk.record-type-reference-edge-missing"),
				"missing reference source proof corrupted member counts or asserted completeness");
		data = type_fixture();
		data.groups[3].back().presence.fragments = {"release"};
		data.groups[3].back().contributor_edges.front().condition = data.groups[3].back().presence;
		require(gap(project(data).surfaces.front(), "sdk.record-type-reference-missing"),
				"type reference borrowed another variant's nominal identity");
		data = type_fixture();
		replace(data.groups[3].back(), "kind", detached_cell::utf8("field"));
		require(project(data).surfaces.front().type_reference_state ==
					q::record_surface_state::conflicting,
				"non-nominal target accepted");
		data = type_fixture();
		replace(
			data.groups[6].front(), "type_reference_count", detached_cell::unsigned_integer(2U));
		replace(
			data.groups[6].front(), "type_reference_profile", detached_cell::utf8("unsupported/1"));
		const auto bad_count = project(data).surfaces.front();
		require(bad_count.state == q::record_surface_state::complete &&
					bad_count.type_reference_state == q::record_surface_state::conflicting &&
					gap(bad_count, "sdk.record-type-reference-conflicting"),
				"unsupported profile downgraded a contradictory count or changed member state");
		data = type_fixture();
		replace(
			data.groups[6].front(), "type_reference_profile", detached_cell::utf8("unsupported/1"));
		require(project(data).surfaces.front().type_reference_state ==
					q::record_surface_state::unknown,
				"unrecognized policy asserted reference completeness");
		data = type_fixture();
		auto alternate = data.groups[6].front();
		replace(alternate, "type_reference_state", detached_cell::utf8("partial"));
		replace(alternate, "type_reference_reason", detached_cell::utf8("dependent-type"));
		data.groups[6].push_back(alternate);
		const auto candidates = project(data);
		require(candidates.surfaces.size() == 2U &&
					std::ranges::all_of(
						candidates.surfaces,
						[](const auto& s)
						{
							return s.type_reference_state == q::record_surface_state::conflicting &&
								gap(s, "sdk.record-type-reference-candidates-conflicting");
						}),
				"incompatible reference candidates selected a winner");
		data = type_fixture();
		replace(
			data.groups[6].front(), "type_reference_count", detached_cell::unsigned_integer(0U));
		replace(data.groups[6].front(), "type_reference_targets", ids({}));
		require(gap(project(data).surfaces.front(), "sdk.record-type-reference-unlisted"),
				"unlisted outgoing nominal references asserted an empty set");
		data.groups[5].erase(data.groups[5].begin() + 2, data.groups[5].end());
		const auto known_empty = project(data).surfaces.front();
		require(known_empty.type_reference_state == q::record_surface_state::complete &&
					known_empty.type_reference_targets.empty(),
				"complete empty reference set unavailable");
		data = type_fixture();
		const auto& property_type =
			data.groups[6].front().values.at("output.type_reference_state").type;
		replace(
			data.groups[6].front(), "type_reference_state", detached_cell::absent(property_type));
		const auto malformed = q::project_record_surfaces(data.input());
		require(!malformed && malformed.error().code == "sdk.record-input-invalid" &&
					malformed.error().field == "type_references",
				"incomplete type payload accepted");
	}
	q::application_query_results queries(const fixture& fixture)
	{
		q::application_query_results result;
		result.snapshot_id = "snapshot:query";
		const std::array<std::string_view, 8> relations{"build.compile_unit.v1",
														"source.file.v1",
														"source.span.v1",
														"cc.entity.v1",
														"cc.entity_detail.v1",
														"cc.entity_edge.v1",
														"cc.record_surface.v1",
														"cc.record_inventory.v1"};
		for (std::size_t group{}; group < relations.size(); ++group)
		{
			auto data = std::make_shared<q::query_result::data>();
			data->row_values = fixture.groups[group];
			data->status = q::execution_status::complete;
			data->input_complete = true;
			data->snapshot = result.snapshot_id;
			data->unresolved = {{"fixture.frontier", "external", "model-unavailable"}};
			data->coverage = {
				{std::string{relations[group]}, {"fixture", "tu:test", "covered", {}}}};
			const auto descriptors = standard_relation_descriptors();
			const auto descriptor =
				std::ranges::find(descriptors, relations[group], &relation_descriptor::id);
			auto plan = q::builder::from(*descriptor);
			require(plan.has_value(), "cannot build independent scan");
			result.scans.push_back({std::string{relations[group]},
									std::move(*plan).finish(),
									q::query_transfer_access::make(data)});
		}
		return result;
	}
	void optional_bitfield_compatibility_tests()
	{
		constexpr std::array<std::string_view, 5> columns{"field_bitfield_profile",
														  "field_bitfield_state",
														  "field_is_bitfield",
														  "field_bit_width",
														  "field_bitfield_reason"};
		fixture data;
		const auto current = project(data);
		for (auto& detail : data.groups[4])
			for (const auto column : columns)
				detail.values.erase("output." + std::string{column});
		const auto sparse = project(data);
		require(sparse.surfaces.size() == 1U &&
					sparse.surfaces.front().state == current.surfaces.front().state &&
					sparse.surfaces.front().fields.size() == 1U &&
					sparse.surfaces.front().methods.size() == 1U,
				"unconsumed absent bit-field facts changed record membership");
		for (const auto& evidence : sparse.evidence)
		{
			if (evidence.relation_id != "cc.entity_detail.v1")
				continue;
			for (const auto column : columns)
				require(!evidence.row.values.contains("output." + std::string{column}),
						"sparse original detail acquired invented bit-field cells");
		}
		for (const auto column : columns)
		{
			auto malformed = data;
			malformed.groups[4].front().values.emplace(
				"output." + std::string{column}, detached_cell::utf8("wrong descriptor type"));
			const auto rejected = q::project_record_surfaces(malformed.input());
			require(!rejected && rejected.error().code == "sdk.record-input-invalid",
					"present mistyped bit-field fact escaped record input validation");
		}
		for (const auto column : {"detail", "canonical_type"})
		{
			auto missing = data;
			missing.groups[4].front().values.erase("output." + std::string{column});
			const auto rejected = q::project_record_surfaces(missing.input());
			require(!rejected && rejected.error().code == "sdk.record-input-invalid" &&
						rejected.error().detail == "column-missing",
					"bit-field compatibility relaxed an existing column check");
		}
		for (const auto bound : {&q::record_surface_limits::maximum_rows,
								 &q::record_surface_limits::maximum_operations,
								 &q::record_surface_limits::maximum_retained_bytes})
		{
			q::record_surface_limits limits;
			limits.*bound = 1U;
			const auto rejected = q::project_record_surfaces(data.input(), limits);
			require(!rejected && rejected.error().code == "sdk.record-budget",
					"sparse record inputs escaped a resource bound");
		}
		std::stop_source stopped;
		stopped.request_stop();
		const auto cancelled = q::project_record_surfaces(data.input(), {}, stopped.get_token());
		require(!cancelled && cancelled.error().code == "sdk.record-cancelled",
				"sparse record inputs escaped cancellation");
	}
	fixture inventory_fixture()
	{
		fixture data;
		data.groups[7] = {
			row("cc.record_inventory.v1",
				{{"inventory", detached_cell::utf8("inventory:test")},
				 {"compile_unit", detached_cell::utf8("tu:test")},
				 {"profile", detached_cell::utf8("clang22-explicit-admitted-record-definitions/1")},
				 {"enumeration_state", detached_cell::utf8("complete")},
				 {"definition_count", detached_cell::unsigned_integer(1U)},
				 {"definitions", ids({"surface:record"})},
				 {"system_definitions", ids({})}})};
		return data;
	}
	void inventory_tests()
	{
		auto data = inventory_fixture();
		const auto actual = project(data);
		require(actual.inventories.size() == 1U &&
					actual.inventories.front().state == q::record_surface_state::complete &&
					actual.inventories.front().declared_definitions == 1U &&
					actual.inventories.front().definitions ==
						std::vector<std::string>{"surface:record"} &&
					actual.inventories.front().system_definitions.empty() &&
					!actual.inventories.front().evidence.empty(),
				"finite record inventory lost");
		require(actual.surfaces.front().type_reference_state == q::record_surface_state::unknown,
				"inventory supplied missing type-reference completion");
		auto raw = data.input();
		raw.compile_units_complete = false;
		raw.inventory_inputs_complete = false;
		const auto scoped = q::project_record_surfaces(raw);
		require(scoped && !scoped->compile_units_complete && !scoped->inventory_inputs_complete &&
					scoped->inventories.front().state == q::record_surface_state::complete,
				"local population granted selected-input completion");
		for (auto& group : data.groups)
			std::ranges::reverse(group);
		const auto reordered = project(data);
		require(actual.inventories.front().evidence == reordered.inventories.front().evidence,
				"input order changed inventory evidence");
		data.groups[7].push_back(data.groups[7].front());
		require(project(data).inventories.size() == 1U,
				"equal inventory observations were not merged");
		data = inventory_fixture();
		replace(data.groups[7].front(), "system_definitions", ids({"surface:record"}));
		require(project(data).inventories.front().system_definitions ==
					std::vector<std::string>{"surface:record"},
				"compiler system classification was lost");
		replace(data.groups[7].front(), "system_definitions", ids({"surface:other"}));
		require(project(data).inventories.front().state == q::record_surface_state::conflicting,
				"system classification outside inventory accepted");
		data = inventory_fixture();
		replace(data.groups[7].front(), "definition_count", detached_cell::unsigned_integer(2U));
		require(project(data).inventories.front().state == q::record_surface_state::conflicting,
				"complete wrong cardinality accepted");
		replace(data.groups[7].front(), "enumeration_state", detached_cell::utf8("partial"));
		replace(data.groups[7].front(), "reason", detached_cell::utf8("missing-definition-source"));
		require(project(data).inventories.front().state == q::record_surface_state::partial,
				"declared partial subset became a contradiction or completion");
		data = inventory_fixture();
		data.groups[6].clear();
		require(project(data).inventories.front().state == q::record_surface_state::partial,
				"missing actual definition surface acquired completion");
		replace(data.groups[7].front(), "definition_count", detached_cell::unsigned_integer(0U));
		replace(data.groups[7].front(), "definitions", ids({}));
		require(project(data).inventories.front().state == q::record_surface_state::conflicting,
				"known declaration with an omitted surface became zero definitions");
		replace(data.groups[4].front(), "is_definition", detached_cell::boolean(false));
		require(project(data).inventories.front().state == q::record_surface_state::complete,
				"forward-only domain did not establish zero definitions");
		data = inventory_fixture();
		replace(data.groups[6].front(), "is_definition", detached_cell::boolean(false));
		replace(data.groups[6].front(), "enumeration_state", detached_cell::utf8("unknown"));
		replace(data.groups[4].front(), "is_definition", detached_cell::boolean(false));
		require(project(data).inventories.front().state == q::record_surface_state::conflicting,
				"forward declaration counted as a definition");
		data = inventory_fixture();
		replace(data.groups[7].front(), "definition_count", detached_cell::unsigned_integer(0U));
		replace(data.groups[7].front(), "definitions", ids({}));
		require(project(data).inventories.front().state == q::record_surface_state::conflicting,
				"unlisted observed definition accepted");
		data = inventory_fixture();
		data.groups[6].front().presence.fragments = {"release"};
		data.groups[6].front().contributor_edges.front().condition =
			data.groups[6].front().presence;
		require(project(data).inventories.front().state == q::record_surface_state::partial,
				"definition from another variant satisfied inventory");
		data = inventory_fixture();
		replace(data.groups[6].front(), "compile_unit", detached_cell::utf8("tu:other"));
		require(project(data).inventories.front().state == q::record_surface_state::conflicting,
				"definition from another unit satisfied inventory");
		data = inventory_fixture();
		data.groups[0].clear();
		require(project(data).inventories.front().state == q::record_surface_state::partial,
				"unbound compile unit acquired completion");
		data = inventory_fixture();
		replace(data.groups[2].front(), "end", detached_cell::unsigned_integer(101U));
		require(project(data).inventories.front().state == q::record_surface_state::conflicting,
				"invalid declaration source acquired completion");
		data = inventory_fixture();
		replace(data.groups[7].front(), "profile", detached_cell::utf8("future-record-domain/2"));
		require(project(data).inventories.front().state == q::record_surface_state::unknown,
				"unknown population profile acquired completion");
		replace(data.groups[7].front(), "definition_count", detached_cell::unsigned_integer(0U));
		require(project(data).inventories.front().state == q::record_surface_state::conflicting,
				"unknown profile hid a cardinality contradiction");
		data = inventory_fixture();
		auto alternate = data.groups[7].front();
		replace(alternate, "system_definitions", ids({"surface:record"}));
		data.groups[7].push_back(alternate);
		const auto conflicting = project(data);
		require(conflicting.inventories.size() == 2U &&
					std::ranges::all_of(conflicting.inventories,
										[](const auto& inventory)
										{
											return inventory.state ==
												q::record_surface_state::conflicting;
										}),
				"conflicting system classifications chose a first candidate");
		q::record_surface_limits limits;
		limits.maximum_inventories = 1U;
		const auto bounded = q::project_record_surfaces(data.input(), limits);
		require(!bounded && bounded.error().code == "sdk.record-budget" &&
					bounded.error().field == "inventories",
				"inventory count bound not enforced");
		data = inventory_fixture();
		const auto transferred = q::project_record_surfaces(queries(data));
		require(transferred && transferred->compile_units_complete &&
					transferred->inventory_inputs_complete &&
					transferred->inventories.front().state == q::record_surface_state::complete &&
					transferred->source_queries &&
					!transferred->source_queries->scans.front().result.closed(),
				"independent finite scans were lost or acquired whole-project closure");
		auto incomplete = queries(data);
		incomplete.scans.erase(incomplete.scans.begin());
		const auto missing_unit = q::project_record_surfaces(incomplete);
		require(missing_unit && !missing_unit->compile_units_complete &&
					missing_unit->inventory_inputs_complete,
				"missing build scan acquired a complete inverse domain");
	}
} // namespace

int main()
{
	type_reference_tests();
	optional_bitfield_compatibility_tests();
	inventory_tests();
	fixture data;
	const auto actual = project(data);
	require(actual.surfaces.size() == 1U, "wrong surface count");
	const auto& surface = actual.surfaces.front();
	require(surface.state == q::record_surface_state::complete && surface.methods.size() == 1U &&
				surface.fields.size() == 1U && surface.methods.front().access == "public",
			"finite member surface lost");
	require(surface.type_reference_state == q::record_surface_state::unknown &&
				!surface.declared_type_references && !surface.type_reference_reason.empty(),
			"missing type-reference capability became an exact empty population");
	require(surface.file == "file:test" && surface.source_snapshot == "snapshot:test" &&
				!surface.evidence.empty(),
			"source binding lost");
	const auto original = queries(data);
	const auto transferred = q::project_record_surfaces(original);
	require(transferred &&
				transferred->surfaces.front().state == q::record_surface_state::complete &&
				transferred->source_queries &&
				transferred->source_queries->scans.front().result.unresolved_items().size() == 1U &&
				transferred->source_queries->scans.front().result.input_coverage().size() == 1U &&
				!transferred->source_queries->scans.front().result.closed(),
			"query side channels lost or acquired closure");
	q::record_surface_limits plan_limit;
	plan_limit.maximum_source_plan_bytes = 1U;
	require(!q::project_record_surfaces(original, plan_limit), "source plan limit ignored");
	for (auto& group : data.groups)
		std::ranges::reverse(group);
	const auto reordered = project(data);
	require(reordered.surfaces.front().evidence == surface.evidence,
			"input order changed evidence indices");
	for (std::size_t i{}; i < actual.evidence.size(); ++i)
		require(actual.evidence[i].row.canonical_form() ==
					reordered.evidence[i].row.canonical_form(),
				"input order changed retained rows");
	data = fixture{};
	data.groups[6].push_back(data.groups[6].front());
	require(project(data).surfaces.size() == 1U, "equal candidates did not merge evidence");
	data = fixture{};
	auto conflicting = data.groups[6].front();
	replace(conflicting, "method_count", detached_cell::unsigned_integer(2U));
	data.groups[6].push_back(conflicting);
	const auto conflicts = project(data);
	require(conflicts.surfaces.size() == 2U &&
				std::ranges::all_of(conflicts.surfaces,
									[](const auto& s)
									{
										return s.state == q::record_surface_state::conflicting;
									}),
			"conflicting surface selected winner");
	data = fixture{};
	data.groups[5].clear();
	require(gap(project(data).surfaces.front(), "sdk.record-member-owner-missing"),
			"unowned member became complete");
	data = fixture{};
	replace(data.groups[2].front(), "end", detached_cell::unsigned_integer(101U));
	require(gap(project(data).surfaces.front(), "sdk.record-source-binding-conflicting"),
			"wrong source range ignored");
	data = fixture{};
	auto alternate_span = data.groups[2].front();
	replace(alternate_span, "end", detached_cell::unsigned_integer(99U));
	data.groups[2].push_back(alternate_span);
	require(gap(project(data).surfaces.front(), "sdk.record-source-conflicting"),
			"two individually valid conflicting spans acquired completeness");
	data = fixture{};
	auto alternate_source = data.groups[1].front();
	replace(alternate_source, "size", detached_cell::unsigned_integer(101U));
	data.groups[1].push_back(alternate_source);
	require(gap(project(data).surfaces.front(), "sdk.record-source-conflicting"),
			"two conflicting source snapshots acquired completeness");
	data = fixture{};
	replace(data.groups[4].back(), "source", detached_cell::utf8("span:absent"));
	require(gap(project(data).surfaces.front(), "sdk.record-source-missing"),
			"member detail with an unbound source acquired completeness");
	data = fixture{};
	auto redeclaration = data.groups[4][1];
	replace(redeclaration, "flags", ids({"inline"}));
	data.groups[4].push_back(redeclaration);
	const auto inline_redeclaration = project(data);
	require(inline_redeclaration.surfaces.front().state == q::record_surface_state::complete &&
				inline_redeclaration.surfaces.front().methods.front().flags ==
					std::vector<std::string>{"inline"},
			"declaration-only inline spelling became a member conflict or was lost");
	replace(data.groups[4].back(), "flags", ids({"static"}));
	require(gap(project(data).surfaces.front(), "sdk.record-member-detail-conflicting"),
			"static member contradiction was discarded");
	data = fixture{};
	replace(data.groups[6].front(), "method_count", detached_cell::unsigned_integer(0U));
	require(gap(project(data).surfaces.front(), "sdk.record-cardinality-conflicting"),
			"cardinality contradiction hidden");
	data = fixture{};
	for (auto& row : data.groups[4])
	{
		row.presence.fragments = {"release"};
		row.contributor_edges.front().condition = row.presence;
	}
	require(gap(project(data).surfaces.front(), "sdk.record-declaration-missing"),
			"variant details crossed");
	data = fixture{};
	data.groups[3].back().interpretation = "gcc";
	data.groups[3].back().contributor_edges.front().interpretation = "gcc";
	require(gap(project(data).surfaces.front(), "sdk.record-entity-missing"),
			"interpretations crossed");
	data = fixture{};
	for (const auto name : {"methods", "fields", "base_targets"})
		replace(data.groups[6].front(), name, ids({}));
	for (const auto name : {"method_count", "field_count", "base_specifier_count"})
		replace(data.groups[6].front(), name, detached_cell::unsigned_integer(0U));
	require(gap(project(data).surfaces.front(), "sdk.record-unlisted-member-conflicting"),
			"unlisted direct members became empty surface");
	data.groups[5].clear();
	require(project(data).surfaces.front().state == q::record_surface_state::complete,
			"known empty definition unavailable");
	replace(data.groups[6].front(), "is_definition", detached_cell::boolean(false));
	replace(data.groups[6].front(), "enumeration_state", detached_cell::utf8("unknown"));
	replace(data.groups[4].front(), "is_definition", detached_cell::boolean(false));
	require(project(data).surfaces.front().state == q::record_surface_state::unknown,
			"forward record became empty definition");
	data = fixture{};
	for (const auto bound : {"rows", "evidence", "bytes", "references", "members", "operations"})
	{
		q::record_surface_limits limit;
		const std::string name{bound};
		if (name == "rows")
			limit.maximum_rows = 1U;
		if (name == "evidence")
			limit.maximum_evidence_bytes = 1U;
		if (name == "bytes")
			limit.maximum_retained_bytes = 1U;
		if (name == "references")
			limit.maximum_evidence_references = 1U;
		if (name == "members")
			limit.maximum_members = 1U;
		if (name == "operations")
			limit.maximum_operations = 1U;
		const auto rejected = q::project_record_surfaces(data.input(), limit);
		require(!rejected && rejected.error().code == "sdk.record-budget",
				"resource failure not typed");
	}
	std::stop_source stop;
	stop.request_stop();
	const auto cancelled = q::project_record_surfaces(data.input(), {}, stop.get_token());
	require(!cancelled && cancelled.error().code == "sdk.record-cancelled",
			"cancellation not typed");
	data.groups[6].front().values["output.method_count"] = detached_cell::utf8("1");
	require(!q::project_record_surfaces(data.input()), "numeric type coercion accepted");
	q::application_query_results missing;
	missing.snapshot_id = "snapshot:missing";
	const auto no_scans = q::project_record_surfaces(missing);
	require(no_scans && no_scans->surfaces.empty() && no_scans->unresolved.size() == 9U &&
				!no_scans->compile_units_complete && !no_scans->inventory_inputs_complete &&
				no_scans->source_queries,
			"missing scans became empty closed project");
	return 0;
}
