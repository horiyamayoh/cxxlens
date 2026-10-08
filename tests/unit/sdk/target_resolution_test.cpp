#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <tuple>

#include <cxxlens/sdk/target_resolution.hpp>

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
													 "cc.type.v1",
													 "cc.declaration.v1",
													 "cc.syntax_node.v1",
													 "cc.operation.v1",
													 "source.include.v1",
													 "cc.entity_edge.v1",
													 "cc.call_site.v1",
													 "cc.declaration_inventory.v1",
													 "cc.target_resolution_slot.v1"};
	constexpr std::string_view profile = "clang22-original-consumer-target-relations/1";
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
							{{"span", detached_cell::utf8("span:a")},
							 {"snapshot", detached_cell::utf8("source:a")},
							 {"file", detached_cell::utf8("file:a")},
							 {"begin", detached_cell::unsigned_integer(10)},
							 {"end", detached_cell::unsigned_integer(20)}})};
			rows[3] = {fact(3,
							{{"entity", detached_cell::utf8("entity:owner")},
							 {"kind", detached_cell::utf8("function")}}),
					   fact(3,
							{{"entity", detached_cell::utf8("entity:target")},
							 {"kind", detached_cell::utf8("record")}})};
			rows[4] = {fact(4, {{"type", detached_cell::utf8("type:a")}})};
			rows[5] = {fact(5,
							{{"declaration", detached_cell::utf8("declaration:a")},
							 {"entity", detached_cell::utf8("entity:owner")},
							 {"source", detached_cell::utf8("span:a")}})};
			rows[6] = {fact(6,
							{{"node", detached_cell::utf8("expression:a")},
							 {"compile_unit", detached_cell::utf8("unit:a")},
							 {"function", detached_cell::utf8("entity:owner")},
							 {"source", detached_cell::utf8("span:a")}})};
			rows[7] = {fact(7,
							{{"operation", detached_cell::utf8("operation:a")},
							 {"compile_unit", detached_cell::utf8("unit:a")},
							 {"function", detached_cell::utf8("entity:owner")},
							 {"scope_declaration", detached_cell::utf8("declaration:a")},
							 {"expression", detached_cell::utf8("expression:a")},
							 {"call", detached_cell::utf8("call:a")},
							 {"target", detached_cell::utf8("entity:target")}})};
			rows[8] = {fact(8,
							{{"include", detached_cell::utf8("include:a")},
							 {"compile_unit", detached_cell::utf8("unit:a")},
							 {"source", detached_cell::utf8("span:a")},
							 {"to_file", detached_cell::utf8("file:a")}})};
			rows[9] = {fact(9,
							{{"edge", detached_cell::utf8("edge:a")},
							 {"compile_unit", detached_cell::utf8("unit:a")},
							 {"source_entity", detached_cell::utf8("entity:owner")},
							 {"target_entity", detached_cell::utf8("entity:target")}})};
			rows[10] = {fact(10,
							 {{"call", detached_cell::utf8("call:a")},
							  {"compile_unit", detached_cell::utf8("unit:a")},
							  {"caller", detached_cell::utf8("entity:owner")},
							  {"expression", detached_cell::utf8("expression:a")}})};
			rows[11] = {fact(11,
							 {{"inventory", detached_cell::utf8("inventory:a")},
							  {"compile_unit", detached_cell::utf8("unit:a")},
							  {"enumeration_state", detached_cell::utf8("partial")},
							  {"declarations", symbols({"declaration:a"})},
							  {"target_slot_count", detached_cell::unsigned_integer(1)},
							  {"target_slot_ids", symbols({"slot:a"})},
							  {"target_slot_state", detached_cell::utf8("complete")},
							  {"target_slot_profile", detached_cell::utf8(std::string{profile})}})};
			rows[12] = {fact(12,
							 {{"slot", detached_cell::utf8("slot:a")},
							  {"compile_unit", detached_cell::utf8("unit:a")},
							  {"subject_kind", detached_cell::utf8("QualType")},
							  {"subject_ordinal", detached_cell::unsigned_integer(0)},
							  {"slot_index", detached_cell::unsigned_integer(0)},
							  {"domain", detached_cell::utf8("nominal_type")},
							  {"relation_kind", detached_cell::utf8("uses_type")},
							  {"eligibility", detached_cell::utf8("eligible")},
							  {"resolution", detached_cell::utf8("resolved")},
							  {"observation_state", detached_cell::utf8("complete")},
							  {"profile", detached_cell::utf8(std::string{profile})},
							  {"source", detached_cell::utf8("span:a")},
							  {"declaration", detached_cell::utf8("declaration:a")},
							  {"owner", detached_cell::utf8("entity:owner")},
							  {"target_entity", detached_cell::utf8("entity:target")},
							  {"is_system", detached_cell::boolean(false)}})};
		}
		q::target_resolution_input input() const
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
					true};
		}
		q::application_query_results
		queries(std::size_t incomplete = 99,
				q::execution_status status = q::execution_status::complete) const
		{
			q::application_query_results value;
			value.snapshot_id = "query:targets";
			for (std::size_t i = 0; i < rows.size(); ++i)
			{
				auto data = std::make_shared<q::query_result::data>();
				data->row_values = rows[i];
				data->status = i == incomplete ? status : q::execution_status::complete;
				data->input_complete = i != incomplete;
				data->ordered = true;
				data->snapshot = value.snapshot_id;
				data->unresolved.push_back({"fixture-frontier", std::string{names[i]}, "retained"});
				data->closures = {"original:closure"};
				value.scans.push_back(
					{std::string{names[i]}, {}, q::query_transfer_access::make(std::move(data))});
			}
			return value;
		}
		void empty()
		{
			rows[12].clear();
			set(rows[11][0], "target_slot_count", detached_cell::unsigned_integer(0));
			set(rows[11][0], "target_slot_ids", symbols({}));
		}
	};
	const q::target_resolution_population& only(const q::target_resolution_projection& result)
	{
		require(result.populations.size() == 1, "one actual unit population");
		return result.populations.front();
	}
	const q::observed_target_resolution_slot&
	only_slot(const q::target_resolution_projection& result)
	{
		require(only(result).slots.size() == 1, "one original slot");
		return only(result).slots.front();
	}
	std::string stable(const q::target_resolution_projection& result)
	{
		std::string encoded;
		for (const auto& e : result.evidence)
			encoded += e.relation_id + e.row.canonical_form();
		for (const auto& p : result.populations)
		{
			encoded += p.inventory + p.compile_unit + p.universe + p.variant + p.interpretation +
				std::to_string(static_cast<int>(p.enumeration_state));
			for (const auto& s : p.slots)
			{
				encoded += s.slot + s.domain + s.resolution;
				for (auto e : s.evidence)
					encoded += std::to_string(e) + ',';
			}
		}
		return encoded;
	}
} // namespace
int main()
{
	std::size_t passed{};
	{
		fixture f;
		const auto result = take(q::project_target_resolution(f.input()));
		const auto& s = only_slot(result);
		require(only(result).enumeration_state == state::complete &&
					s.source_state == state::complete && s.subject_state == state::complete &&
					s.target_state == state::complete && s.is_system == false,
				"actual nominal slot independent from partial declaration census");
		++passed;
		require(result.evidence.front().row.provenance == f.rows[0][0].provenance &&
					!s.evidence.empty(),
				"original evidence and annotations retained");
		++passed;
	}
	{
		fixture f;
		f.empty();
		auto result = take(q::project_target_resolution(f.input()));
		require(only(result).enumeration_state == state::complete && only(result).slot_count == 0 &&
					only(result).slots.empty(),
				"independent known empty census");
		++passed;
		f.rows[11].clear();
		result = take(q::project_target_resolution(f.input()));
		require(only(result).enumeration_state == state::unknown &&
					only(result).inventory.empty() && !only(result).slot_count,
				"missing inventory unknown without fabricated ID");
		++passed;
	}
	{
		fixture f;
		for (auto name :
			 {"target_slot_count", "target_slot_ids", "target_slot_state", "target_slot_profile"})
			f.rows[11][0].values.erase("output." + std::string{name});
		auto result = take(q::project_target_resolution(f.input()));
		require(only(result).enumeration_state != state::complete,
				"older compatible optional fields absent stays unknown");
		++passed;
		f.rows[12][0].values.erase("output.subject_ordinal");
		require(!q::project_target_resolution(f.input()), "missing required column rejected");
		++passed;
	}
	{
		fixture f;
		f.rows[12][0].values.at("output.slot_index").type = {scalar_kind::utf8_string, "", false};
		require(!q::project_target_resolution(f.input()), "present wrong descriptor type rejected");
		++passed;
		f = fixture{};
		f.rows[12][0].values.emplace("output.foreign", detached_cell::utf8("bad"));
		require(!q::project_target_resolution(f.input()), "foreign projected column rejected");
		++passed;
	}
	{
		fixture f;
		set(f.rows[12][0], "resolution", detached_cell::utf8("unknown"));
		set(f.rows[12][0],
			"target_entity",
			detached_cell::absent(f.rows[12][0].values.at("output.target_entity").type));
		const auto result = take(q::project_target_resolution(f.input()));
		require(only(result).enumeration_state == state::complete &&
					only_slot(result).resolution == "unknown" &&
					only_slot(result).target_state != state::complete,
				"dependent target unknown with complete original enumeration");
		++passed;
		set(f.rows[12][0], "resolution", detached_cell::utf8("unresolved"));
		require(only_slot(take(q::project_target_resolution(f.input()))).resolution == "unresolved",
				"explicit observed nonresolved preserved");
		++passed;
		set(f.rows[12][0], "eligibility", detached_cell::utf8("excluded"));
		set(f.rows[12][0], "resolution", detached_cell::utf8("not_applicable"));
		require(only_slot(take(q::project_target_resolution(f.input()))).eligibility == "excluded",
				"excluded builtin original retained without target");
		++passed;
	}
	{
		fixture f;
		f.rows[3].pop_back();
		const auto result = take(q::project_target_resolution(f.input()));
		require(only(result).enumeration_state == state::complete &&
					only_slot(result).target_state != state::complete,
				"dangling normalized target independent census");
		++passed;
		f = fixture{};
		f.rows[3].back().presence.fragments = {"release"};
		f.rows[3].back().contributor_edges.front().condition = f.rows[3].back().presence;
		require(only_slot(take(q::project_target_resolution(f.input()))).target_state !=
					state::complete,
				"foreign world cannot bind target");
		++passed;
	}
	{
		fixture f;
		f.rows[12].push_back(f.rows[12][0]);
		f.rows[12].back().provenance = {"other:original"};
		f.rows[12].back().contributor_edges.front().provenance = "other:original";
		const auto result = take(q::project_target_resolution(f.input()));
		require(only(result).enumeration_state == state::complete && result.evidence.size() == 15 &&
					only_slot(result).target_state == state::complete,
				"equal duplicates retain originals without duplicate membership");
		++passed;
		set(f.rows[12].back(), "target_entity", detached_cell::utf8("entity:owner"));
		const auto conflict = take(q::project_target_resolution(f.input()));
		require(only(conflict).enumeration_state == state::complete &&
					only_slot(conflict).target_state == state::conflicting &&
					only_slot(conflict).source_state == state::complete,
				"contradictory targets preserve independent admitted membership");
		++passed;
		set(f.rows[12].back(), "slot_index", detached_cell::unsigned_integer(1));
		require(only(take(q::project_target_resolution(f.input()))).enumeration_state ==
					state::conflicting,
				"contradictory admission slot conflicts census");
		++passed;
	}
	{
		fixture f;
		f.rows[12].clear();
		require(only(take(q::project_target_resolution(f.input()))).enumeration_state !=
					state::complete,
				"missing expected original slot cannot prove complete");
		++passed;
		f = fixture{};
		set(f.rows[11][0], "target_slot_count", detached_cell::unsigned_integer(0));
		set(f.rows[11][0], "target_slot_ids", symbols({}));
		require(only(take(q::project_target_resolution(f.input()))).enumeration_state ==
					state::conflicting,
				"extra actual slot cannot become known empty");
		++passed;
		f = fixture{};
		set(f.rows[11][0], "target_slot_count", detached_cell::unsigned_integer(2));
		require(only(take(q::project_target_resolution(f.input()))).enumeration_state ==
					state::conflicting,
				"count and IDs contradiction");
		++passed;
	}
	{
		fixture f;
		set(f.rows[12][0], "domain", detached_cell::utf8("future_domain"));
		require(only(take(q::project_target_resolution(f.input()))).enumeration_state !=
					state::complete,
				"future domain forbids subset promotion");
		++passed;
		f = fixture{};
		set(f.rows[12][0], "profile", detached_cell::utf8("future/2"));
		require(only(take(q::project_target_resolution(f.input()))).enumeration_state !=
					state::complete,
				"future slot profile unavailable");
		++passed;
		f = fixture{};
		set(f.rows[11][0], "target_slot_profile", detached_cell::utf8("future/2"));
		require(only(take(q::project_target_resolution(f.input()))).enumeration_state !=
					state::complete,
				"future census profile unavailable");
		++passed;
	}
	{
		fixture f;
		set(f.rows[12][0], "expression", detached_cell::utf8("expression:a"));
		set(f.rows[6][0], "compile_unit", detached_cell::utf8("unit:foreign"));
		const auto result = take(q::project_target_resolution(f.input()));
		require(only_slot(result).subject_state == state::conflicting &&
					only(result).enumeration_state == state::complete,
				"present original subject belongs to exact unit");
		++passed;
		f = fixture{};
		set(f.rows[11][0], "declarations", symbols({}));
		require(only_slot(take(q::project_target_resolution(f.input()))).subject_state !=
					state::complete,
				"declaration unit association requires observed same-unit membership");
		++passed;
	}
	{
		fixture f;
		set(f.rows[2][0], "end", detached_cell::unsigned_integer(101));
		require(only_slot(take(q::project_target_resolution(f.input()))).source_state ==
					state::conflicting,
				"source bounds invalid");
		++passed;
		f = fixture{};
		f.rows[12][0].values.erase("output.is_system");
		require(!only_slot(take(q::project_target_resolution(f.input()))).is_system,
				"unobserved actual system facet not false");
		++passed;
	}
	{
		fixture f;
		set(f.rows[12][0], "domain", detached_cell::utf8("include"));
		set(f.rows[12][0], "relation_kind", detached_cell::utf8("includes"));
		set(f.rows[12][0], "include", detached_cell::utf8("include:a"));
		set(f.rows[12][0], "target_file", detached_cell::utf8("file:a"));
		set(f.rows[12][0],
			"target_entity",
			detached_cell::absent(f.rows[12][0].values.at("output.target_entity").type));
		require(only_slot(take(q::project_target_resolution(f.input()))).target_state ==
					state::complete,
				"include target joins actual file ID not snapshot key");
		++passed;
		set(f.rows[8][0], "to_file", detached_cell::utf8("file:foreign"));
		require(only_slot(take(q::project_target_resolution(f.input()))).target_state ==
					state::conflicting,
				"actual include target contradiction");
		++passed;
	}
	{
		fixture f;
		for (const auto& [domain, kind] :
			 std::array<std::pair<std::string_view, std::string_view>, 5>{
				 {{"callable", "calls"},
				  {"state_access", "reads"},
				  {"ownership", "owns"},
				  {"inheritance", "inherits"},
				  {"override", "overrides"}}})
		{
			set(f.rows[12][0], "domain", detached_cell::utf8(std::string{domain}));
			set(f.rows[12][0], "relation_kind", detached_cell::utf8(std::string{kind}));
			require(only(take(q::project_target_resolution(f.input()))).enumeration_state ==
						state::complete,
					"closed supported relation kind admitted");
		}
		++passed;
	}
	{
		fixture f;
		auto result = take(q::project_target_resolution(f.queries(11)));
		require(only(result).enumeration_state == state::complete && result.source_queries &&
					!result.source_queries->scans[11].result.inputs_complete() &&
					!result.source_queries->scans[11].result.unresolved_items().empty(),
				"named census independent of generic declaration partiality; raw flags "
				"and "
				"frontiers retained");
		++passed;
		result = take(q::project_target_resolution(f.queries(12)));
		require(only(result).enumeration_state == state::complete &&
					!result.source_queries->scans[12].result.inputs_complete(),
				"slot target normalization partiality is independent from census");
		++passed;
		result = take(q::project_target_resolution(f.queries(12, q::execution_status::truncated)));
		require(only(result).enumeration_state != state::complete,
				"truncated scan never closed by count alone");
		++passed;
		auto queries = f.queries();
		queries.scans.pop_back();
		result = take(q::project_target_resolution(queries));
		require(only(result).enumeration_state != state::complete &&
					result.source_queries->scans.size() == 12,
				"missing slot scan unavailable and original query shape retained");
		++passed;
	}
	{
		fixture f;
		const auto original = stable(take(q::project_target_resolution(f.input())));
		for (auto& rows : f.rows)
			std::ranges::reverse(rows);
		require(original == stable(take(q::project_target_resolution(f.input()))),
				"original permutation preserves deterministic evidence bindings");
		++passed;
	}
	{
		fixture f;
		q::finite_population_limits limits;
		limits.maximum_operations = 2;
		auto result = q::project_target_resolution(f.input(), limits);
		require(!result && result.error().code == "sdk.target-budget",
				"bounded work stops projection");
		++passed;
		limits = {};
		limits.maximum_retained_bytes = 10;
		result = q::project_target_resolution(f.queries(), limits);
		require(!result && result.error().code == "sdk.target-budget",
				"borrowed original row charged before owning allocation");
		++passed;
		limits = {};
		limits.maximum_evidence_references = 1;
		result = q::project_target_resolution(f.input(), limits);
		require(!result && result.error().code == "sdk.target-budget",
				"derived original evidence reference bound");
		++passed;
		std::stop_source stop;
		stop.request_stop();
		result = q::project_target_resolution(f.input(), {}, stop.get_token());
		require(!result && result.error().code == "sdk.target-cancelled",
				"stop-token cancellation");
		++passed;
		limits = {};
		std::size_t polls{};
		limits.cancelled = [&]
		{
			return ++polls > 20;
		};
		result = q::project_target_resolution(f.queries(), limits);
		require(!result && result.error().code == "sdk.target-cancelled",
				"in-flight caller callback cancellation");
		++passed;
	}

	{
		fixture f;
		f.rows[11].push_back(f.rows[11][0]);
		set(f.rows[11].back(), "target_slot_count", detached_cell::unsigned_integer(0));
		require(only(take(q::project_target_resolution(f.input()))).enumeration_state ==
					state::conflicting,
				"original target4 duplicates must agree");
		++passed;
	}
	{
		fixture f;
		f.rows[12].push_back(f.rows[12][0]);
		set(f.rows[12].back(), "slot", detached_cell::utf8("slot:b"));
		set(f.rows[11][0], "target_slot_count", detached_cell::unsigned_integer(2));
		set(f.rows[11][0], "target_slot_ids", symbols({"slot:a", "slot:b"}));
		require(only(take(q::project_target_resolution(f.input()))).enumeration_state ==
					state::conflicting,
				"distinct slot IDs cannot claim identical native admission tuple");
		++passed;
	}
	{
		fixture f;
		f.rows[0][0].presence.fragments = {"debug", "release"};
		f.rows[0][0].contributor_edges.front().condition = f.rows[0][0].presence;
		const auto result = take(q::project_target_resolution(f.input()));
		require(result.populations.size() == 2 && result.populations[1].variant == "release" &&
					result.populations[1].enumeration_state == state::unknown &&
					result.populations[1].inventory.empty(),
				"selected second world keeps unknown actual unit population");
		++passed;
	}
	{
		fixture f;
		set(f.rows[11][0], "target_slot_ids", detached_cell::bytes({std::byte{8}}));
		require(!q::project_target_resolution(f.input()),
				"truncated target membership set rejected");
		++passed;
	}
	{
		fixture f;
		f.rows[12][0].values.erase("output.declaration");
		require(only_slot(take(q::project_target_resolution(f.input()))).subject_state !=
					state::complete,
				"original native subject slot persists when normalization absent");
		++passed;
	}
	{
		fixture f;
		set(f.rows[12][0], "operation", detached_cell::utf8("operation:a"));
		set(f.rows[12][0], "expression", detached_cell::utf8("expression:a"));
		set(f.rows[12][0], "call", detached_cell::utf8("call:a"));
		require(only_slot(take(q::project_target_resolution(f.input()))).subject_state ==
					state::complete,
				"original operation and call fields bind exact original subject");
		++passed;
		set(f.rows[7][0], "expression", detached_cell::utf8("foreign:expression"));
		require(only_slot(take(q::project_target_resolution(f.input()))).subject_state ==
					state::conflicting,
				"recorded operation and subject expression contradiction");
		++passed;
	}
	{
		fixture f;
		q::finite_population_limits limits;
		limits.maximum_source_queries = 1;
		require(!q::project_target_resolution(f.queries(), limits),
				"original scan plan limits before copying");
		++passed;
		limits = {};
		limits.maximum_rows = 1;
		require(!q::project_target_resolution(f.queries(), limits),
				"total borrowed rows bounded before owning all input groups");
		++passed;
	}

	{
		fixture f;
		set(f.rows[3].back(), "provider_local_key", detached_cell::bytes({std::byte{1}}));
		f.rows[3].push_back(f.rows[3].back());
		f.rows[3].back().values.erase("output.provider_local_key");
		require(only_slot(take(q::project_target_resolution(f.input()))).target_state ==
					state::complete,
				"missing optional target entity facet is not a conflicting value");
		++passed;
		f.rows[12].push_back(f.rows[12][0]);
		f.rows[12].back().values.erase("output.target_entity");
		require(only(take(q::project_target_resolution(f.input()))).enumeration_state ==
						state::complete &&
					only_slot(take(q::project_target_resolution(f.input()))).target_state !=
						state::conflicting,
				"unobserved optional slot target is not a contradictory target");
		++passed;
	}

	{
		fixture f;
		f.rows[3].push_back(f.rows[3].back());
		set(f.rows[3].back(), "kind", detached_cell::utf8("variable"));
		const auto result = take(q::project_target_resolution(f.input()));
		require(only(result).enumeration_state == state::complete &&
					only_slot(result).target_state == state::conflicting,
				"actual normalized entity contradiction remains conflicting");
		++passed;
	}
	{
		fixture f;
		for (std::size_t i{}; i < 64; ++i)
		{
			auto row = f.rows[3].back();
			const auto suffix = std::to_string(i);
			set(row,
				"entity",
				detached_cell::utf8("entity:unused:" + suffix + std::string(4096, 'x')));
			row.claim_contributors = {"claim:" + suffix};
			row.contributor_edges.front().claim_contributor = row.claim_contributors.front();
			f.rows[3].push_back(std::move(row));
		}
		std::size_t serialized_bytes{};
		for (const auto& rows : f.rows)
			for (const auto& row : rows)
				serialized_bytes += row.canonical_form().size();
		q::finite_population_limits limits;
		limits.maximum_operations = serialized_bytes * 4 + 100'000;
		const auto result = take(q::project_target_resolution(f.input(), limits));
		std::size_t position{};
		for (std::size_t group{}; group < f.rows.size(); ++group)
		{
			std::vector<std::string> expected;
			for (const auto& row : f.rows[group])
				expected.push_back(row.canonical_form());
			std::ranges::sort(expected);
			for (const auto& canonical : expected)
			{
				require(result.evidence[position].relation_id == names[group] &&
							result.evidence[position].row.canonical_form() == canonical,
						"full original evidence remains group then binary canonical order");
				++position;
			}
		}
		require(position == result.evidence.size() &&
					only_slot(result).target_state == state::complete,
				"long unrelated evidence uses bounded visited-prefix comparisons");
		++passed;
		for (auto& rows : f.rows)
			std::ranges::reverse(rows);
		require(stable(result) == stable(take(q::project_target_resolution(f.input(), limits))),
				"large original permutation retains exact evidence bindings");
		++passed;
		const auto queries = f.queries();
		const auto retained = take(q::project_target_resolution(queries, limits));
		require(stable(result) == stable(retained) && retained.source_queries &&
					retained.source_queries->scans.size() == queries.scans.size(),
				"query overload retains original scans under the same work cap");
		++passed;
	}
	{
		fixture f;
		for (std::size_t i{}; i < 8; ++i)
		{
			auto row = f.rows[3].back();
			const auto suffix = std::to_string(i);
			set(row, "entity", detached_cell::utf8("entity:prefix:" + suffix));
			row.claim_contributors = {"claim:" + std::string(4096, 'p') + suffix};
			row.contributor_edges.front().claim_contributor = row.claim_contributors.front();
			f.rows[3].push_back(std::move(row));
		}
		q::finite_population_limits limits;
		std::size_t low{1}, high{4'000'000};
		limits.maximum_operations = high;
		const auto expected = stable(take(q::project_target_resolution(f.input(), limits)));
		while (low < high)
		{
			const auto middle = low + (high - low) / 2;
			limits.maximum_operations = middle;
			const auto result = q::project_target_resolution(f.input(), limits);
			if (result)
				high = middle;
			else
			{
				require(result.error().code == "sdk.target-budget" &&
							result.error().field == "operations" &&
							result.error().detail == "limit-exceeded",
						"long common-prefix boundary reports the exact work field");
				low = middle + 1;
			}
		}
		limits.maximum_operations = low;
		require(stable(take(q::project_target_resolution(f.input(), limits))) == expected,
				"exact visited-prefix work cap preserves the complete result");
		++passed;
		limits.maximum_operations = low - 1;
		const auto under = q::project_target_resolution(f.input(), limits);
		require(!under && under.error().code == "sdk.target-budget" &&
					under.error().field == "operations" && under.error().detail == "limit-exceeded",
				"one-under visited-prefix work cap fails before completion");
		++passed;
		limits.maximum_operations = low;
		std::size_t polls{};
		limits.cancelled = [&]
		{
			++polls;
			return false;
		};
		(void)take(q::project_target_resolution(f.input(), limits));
		const auto stop_after = polls / 2;
		polls = 0;
		limits.cancelled = [&]
		{
			return ++polls > stop_after;
		};
		const auto cancelled = q::project_target_resolution(f.input(), limits);
		require(!cancelled && cancelled.error().code == "sdk.target-cancelled" &&
					cancelled.error().field == "projection" &&
					cancelled.error().detail == "stop-requested",
				"in-flight common-prefix work still obeys caller cancellation");
		++passed;
		std::stop_source stop;
		stop.request_stop();
		limits.cancelled = {};
		const auto stopped = q::project_target_resolution(f.input(), limits, stop.get_token());
		require(!stopped && stopped.error().code == "sdk.target-cancelled" &&
					stopped.error().field == "projection",
				"current stop precedes all long-prefix work");
		++passed;
	}
	{
		fixture f;
		const auto prototype = f.rows[0].front();
		for (auto& rows : f.rows)
			rows.clear();
		const std::string prefix = "unit:" + std::string(1024, 'p');
		for (const auto& suffix : std::array<std::string, 5>{"", "a", "aa", "\xc2\x80", "\xc3\xbf"})
		{
			auto row = prototype;
			set(row, "compile_unit", detached_cell::utf8(prefix + suffix));
			f.rows[0].push_back(std::move(row));
		}
		f.rows[0].push_back(f.rows[0].back());
		std::ranges::reverse(f.rows[0]);
		const auto result = take(q::project_target_resolution(f.input()));
		std::vector<q::query_unresolved> expected;
		for (const auto& population : result.populations)
		{
			require(population.enumeration_state != state::complete,
					"missing genuine inventories stay unknown");
			expected.insert(expected.end(), population.gaps.begin(), population.gaps.end());
		}
		std::ranges::sort(expected,
						  [](const auto& a, const auto& b)
						  {
							  return std::tie(a.code, a.subject, a.detail) <
								  std::tie(b.code, b.subject, b.detail);
						  });
		expected.erase(std::ranges::unique(expected).begin(), expected.end());
		require(expected == result.unresolved && result.populations.size() == 5 &&
					result.evidence.size() == 6,
				"long-prefix and high-byte gap subjects preserve full tuple order "
				"and deduplication");
		++passed;
	}
	std::cout << "target resolution oracles " << passed << " PASS\n";
}
