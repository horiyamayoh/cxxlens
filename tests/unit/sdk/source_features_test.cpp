#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>

#include <cxxlens/sdk/source_features.hpp>

#include "../../../src/sdk/query_result_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;
	namespace q = cxxlens::sdk::query;
	using state = q::finite_population_state;
	constexpr std::string_view profile = "clang22-original-static-source-features/1";
	constexpr std::array<std::string_view, 10> names{"build.compile_unit.v1",
													 "source.file.v1",
													 "source.span.v1",
													 "cc.entity.v1",
													 "cc.declaration.v1",
													 "cc.declaration_inventory.v1",
													 "cc.type.v1",
													 "cc.syntax_node.v1",
													 "cc.source_feature.v1",
													 "cc.source_feature_inventory.v1"};
	void require(bool condition, std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}
	template <class T>
	T take(result<T> result)
	{
		if (!result)
		{
			std::cerr << result.error().code << ':' << result.error().field << ':'
					  << result.error().detail << '\n';
			std::exit(1);
		}
		return std::move(*result);
	}
	detached_cell symbols(std::initializer_list<std::string_view> ids)
	{
		std::vector<std::byte> encoded;
		for (const auto id : ids)
		{
			for (unsigned shift{}; shift < 32U; shift += 8U)
				encoded.push_back(static_cast<std::byte>((id.size() >> shift) & 255U));
			for (char c : id)
				encoded.push_back(static_cast<std::byte>(c));
		}
		return detached_cell::bytes(std::move(encoded));
	}
	void set(q::annotated_row& row, std::string_view name, detached_cell value)
	{
		const auto key = "output." + std::string{name};
		if (!row.values.contains(key))
		{
			std::cerr << "fixture column missing: " << name << '\n';
			std::exit(1);
		}
		value.type = row.values.at(key).type;
		row.values[key] = std::move(value);
	}
	q::annotated_row fact(std::size_t group,
						  std::initializer_list<std::pair<std::string, detached_cell>> fields)
	{
		q::annotated_row row;
		row.presence = {"features:test", {"debug"}};
		row.interpretation = "clang22";
		row.claim_contributors = {"claim:test"};
		row.producer_contracts = {{"source.test", "semantic:fixture"}};
		row.provenance = {"features:evidence"};
		row.contributor_guarantees = {
			{"exact", "original-feature", "fixture", {"schema_validated"}}};
		row.contributor_edges = {{row.claim_contributors.front(),
								  row.producer_contracts.front(),
								  row.provenance.front(),
								  row.contributor_guarantees.front(),
								  row.presence,
								  row.interpretation}};
		const auto descriptors = standard_relation_descriptors();
		const auto d = std::ranges::find(descriptors, names[group], &relation_descriptor::id);
		require(d != descriptors.end(), "fixture descriptor missing");
		for (const auto& column : d->columns)
		{
			auto value = detached_cell::utf8("fixture");
			if (column.type.optional)
				value = detached_cell::absent(column.type);
			else if (column.type.scalar == scalar_kind::boolean)
				value = detached_cell::boolean(false);
			else if (column.type.scalar == scalar_kind::unsigned_integer)
				value = detached_cell::unsigned_integer(0U);
			else if (column.type.scalar == scalar_kind::digest)
				value = detached_cell::utf8(content_digest({}));
			else if (column.type.scalar == scalar_kind::set ||
					 column.type.scalar == scalar_kind::bytes)
				value = detached_cell::bytes({});
			else if (column.type.scalar == scalar_kind::closed_symbol)
				value = detached_cell::utf8("canonicalized");
			value.type = column.type;
			row.values.emplace("output." + column.name, std::move(value));
		}
		for (const auto& [name, value] : fields)
			set(row, name, value);
		return row;
	}
	detached_cell txt(std::string_view text)
	{
		return detached_cell::utf8(std::string{text});
	}
	detached_cell num(std::uint64_t n)
	{
		return detached_cell::unsigned_integer(n);
	}
	struct fixture
	{
		std::array<std::vector<q::annotated_row>, 10> rows;
		fixture()
		{
			rows[0].push_back(
				fact(0,
					 {{"compile_unit", txt("U")},
					  {"freestanding", detached_cell::boolean(false)},
					  {"freestanding_profile", txt("clang22-original-language-environment/1")},
					  {"freestanding_state", txt("complete")}}));
			rows[1].push_back(
				fact(1, {{"file", txt("F")}, {"snapshot", txt("P")}, {"size", num(32)}}));
			rows[2].push_back(fact(2,
								   {{"span", txt("S")},
									{"file", txt("F")},
									{"snapshot", txt("P")},
									{"begin", num(0)},
									{"end", num(4)}}));
			rows[3].push_back(fact(3, {{"entity", txt("E")}, {"kind", txt("function")}}));
			rows[4].push_back(fact(4,
								   {{"declaration", txt("D")},
									{"entity", txt("E")},
									{"source", txt("S")},
									{"kind", txt("Function")}}));
			rows[5].push_back(
				fact(5,
					 {{"inventory", txt("DI")},
					  {"compile_unit", txt("U")},
					  {"profile", txt("clang22-explicit-admitted-named-declarations/1")},
					  {"enumeration_state", txt("complete")},
					  {"declaration_count", num(1)},
					  {"declarations", symbols({"D"})}}));
			rows[6].push_back(fact(6, {{"type", txt("T")}, {"constructor", txt("builtin")}}));
			rows[7].push_back(fact(7,
								   {{"node", txt("N")},
									{"compile_unit", txt("U")},
									{"function", txt("E")},
									{"kind", txt("CallExpr")},
									{"source", txt("S")}}));
			rows[8].push_back(fact(8,
								   {{"feature", txt("X")},
									{"compile_unit", txt("U")},
									{"profile", txt(profile)},
									{"ordinal", num(0)},
									{"original_node_ordinal", num(0)},
									{"feature_class", txt("statement")},
									{"kind", txt("CallExpr")},
									{"compiler_kind", num(0)},
									{"origin", txt("written")},
									{"evaluation", txt("potentially_evaluated")},
									{"state", txt("complete")},
									{"source_binding_state", txt("complete")},
									{"file", txt("F")},
									{"source_snapshot", txt("P")},
									{"source", txt("S")},
									{"declaration_binding_state", txt("none")},
									{"context_binding_state", txt("complete")},
									{"context_declaration", txt("D")},
									{"entity_binding_state", txt("complete")},
									{"subject_entity", txt("E")},
									{"call_binding_state", txt("complete")},
									{"call_target", txt("E")},
									{"reference_kind", txt("direct_callee")},
									{"type_binding_state", txt("complete")},
									{"subject_type", txt("T")},
									{"type_role", txt("expression")},
									{"syntax", txt("N")}}));
			rows[9].push_back(fact(9,
								   {{"inventory", txt("I")},
									{"compile_unit", txt("U")},
									{"profile", txt(profile)},
									{"scope", txt("translation_unit")},
									{"feature_count", num(1)},
									{"feature_ids", symbols({"X"})},
									{"enumeration_state", txt("complete")},
									{"traversal_state", txt("complete")},
									{"entry_state", txt("complete")},
									{"source_binding_state", txt("complete")},
									{"unbound_feature_count", num(0)},
									{"unbound_feature_ids", symbols({})},
									{"entered_file_count", num(1)},
									{"entered_file_ids", symbols({"F"})},
									{"entered_source_snapshots", symbols({"P"})},
									{"entered_file_state", txt("complete")}}));
			rows[9].push_back(fact(9,
								   {{"inventory", txt("FI")},
									{"compile_unit", txt("U")},
									{"profile", txt(profile)},
									{"scope", txt("entered_file")},
									{"file", txt("F")},
									{"source_snapshot", txt("P")},
									{"feature_count", num(1)},
									{"feature_ids", symbols({"X"})},
									{"enumeration_state", txt("complete")},
									{"traversal_state", txt("complete")},
									{"entry_state", txt("complete")},
									{"source_binding_state", txt("complete")},
									{"unbound_feature_count", num(0)},
									{"unbound_feature_ids", symbols({})}}));
		}
		q::source_feature_input input() const
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
					true,
					true,
					true};
		}
		q::application_query_results queries(bool validated = false) const
		{
			q::application_query_results queries;
			queries.snapshot_id = "original:test";
			for (std::size_t group{}; group < rows.size(); ++group)
			{
				auto data = std::make_shared<q::query_result::data>();
				data->row_values = rows[group];
				if (validated)
				{
					for (const auto& row : data->row_values)
					{
						const auto valid = row.validate();
						if (!valid)
							std::cerr << valid.error().code << ':' << valid.error().field << ':'
									  << valid.error().detail << '\n';
						require(bool(valid), "fixture generic row validation failed");
					}
					data->rows_validated = true;
				}
				data->status = q::execution_status::complete;
				// Generic optional metadata health does not erase an independent scan.
				data->input_complete = false;
				queries.scans.push_back({std::string{names[group]},
										 {},
										 q::query_transfer_access::make(std::move(data))});
			}
			return queries;
		}
	};
} // namespace
int main()
{
	fixture f;
	{
		const auto unvalidated = f.queries();
		for (const auto& scan : unvalidated.scans)
			require(!q::query_transfer_access::rows_validated(scan.result),
					"default query owner silently skipped generic validation");
		q::projection_resource_usage checked_usage, borrowed_usage;
		const auto checked = take(q::project_source_features(unvalidated, {}, {}, checked_usage));
		const auto borrowed =
			take(q::project_source_features(f.queries(true), {}, {}, borrowed_usage));
		require(checked.features.size() == borrowed.features.size() &&
					checked.evidence.size() == borrowed.evidence.size() &&
					borrowed_usage.operations < checked_usage.operations &&
					borrowed_usage.retained_bytes_bound == checked_usage.retained_bytes_bound,
				"same admitted row ownership changed output/storage or repeated validation");
		for (std::size_t i{}; i < checked.evidence.size(); ++i)
			require(checked.evidence[i].relation_id == borrowed.evidence[i].relation_id &&
						checked.evidence[i].row.canonical_form() ==
							borrowed.evidence[i].row.canonical_form(),
					"generic validation reuse changed original evidence");
		auto malformed = f;
		malformed.rows[8].front().multiplicity = 0U;
		require(!q::project_source_features(malformed.queries()),
				"default owner bypassed malformed original annotations");
		auto mixed = f.queries(true);
		mixed.scans[8].result = malformed.queries().scans[8].result;
		require(!q::project_source_features(mixed),
				"mixed admitted owners bypassed an unvalidated malformed row");
		malformed = f;
		set(malformed.rows[8].front(),
			"feature",
			detached_cell::utf8(std::string(1U, static_cast<char>(0xffU))));
		require(!q::project_source_features(malformed.queries()),
				"default owner bypassed malformed UTF-8 values");
		malformed = f;
		malformed.rows[8].front().values.emplace("output.foreign", detached_cell::boolean(true));
		require(!q::project_source_features(malformed.queries(true)),
				"generic admitted rows bypassed relation-specific foreign columns");
		auto missing = f;
		missing.rows[8].front().values.erase("output.feature");
		require(!q::project_source_features(missing.queries(true)),
				"generic admitted rows bypassed a required relation column");
	}
	{
		auto alternatives = f;
		for (auto& group : alternatives.rows)
			for (auto& row : group)
			{
				row.presence.fragments.push_back("release");
				for (auto& edge : row.contributor_edges)
					edge.condition = row.presence;
			}
		auto other = alternatives.rows[8].front();
		set(alternatives.rows[8].front(), "kind", txt("\xc3\xa9"));
		set(other, "kind", txt("z"));
		alternatives.rows[8].push_back(std::move(other));
		const auto claim = "claim:" + std::string(65536U, 'x');
		for (auto& row : alternatives.rows[8])
		{
			row.claim_contributors = {claim};
			row.contributor_edges.front().claim_contributor = claim;
		}
		q::projection_resource_usage measured, repeated;
		const auto original = alternatives.queries(true);
		const auto ordered = take(q::project_source_features(original, {}, {}, measured));
		require(ordered.features.size() == 2U, "multi-world original alternatives were dropped");
		std::ranges::reverse(alternatives.rows[8]);
		const auto reversed = take(q::project_source_features(alternatives.queries(true)));
		require(ordered.evidence.size() == reversed.evidence.size(),
				"alternative order changed original evidence cardinality");
		for (std::size_t i{}; i < ordered.evidence.size(); ++i)
			require(ordered.evidence[i].relation_id == reversed.evidence[i].relation_id &&
						ordered.evidence[i].row.canonical_form() ==
							reversed.evidence[i].row.canonical_form(),
					"non-ASCII alternatives changed canonical evidence order");
		q::finite_population_limits exact;
		exact.maximum_operations = measured.operations;
		exact.maximum_retained_bytes = measured.retained_bytes_bound;
		require(bool(q::project_source_features(original, exact, {}, repeated)) &&
					repeated.operations == measured.operations &&
					repeated.retained_bytes_bound == measured.retained_bytes_bound,
				"live ordering scratch rejected its exact measured peak");
		--exact.maximum_retained_bytes;
		require(!q::project_source_features(original, exact, {}, repeated) &&
					!repeated.operations && !repeated.retained_bytes_bound,
				"ordering scratch bypassed one-less storage or published failed usage");
		std::size_t callbacks{};
		q::finite_population_limits cancel;
		cancel.cancelled = [&]
		{
			return ++callbacks > 65536U;
		};
		const auto stopped = q::project_source_features(original, cancel, {}, repeated);
		require(!stopped && stopped.error().code == "sdk.source-feature-cancelled" &&
					!repeated.operations && !repeated.retained_bytes_bound,
				"long common-prefix ordering ignored cancellation");
	}
	{
		auto identities = f;
		const auto id = "feature:" + std::string(65536U, 'i');
		set(identities.rows[8].front(), "feature", txt(id));
		for (auto& inventory : identities.rows[9])
			set(inventory, "feature_ids", symbols({id}));
		for (auto& group : identities.rows)
			for (auto& row : group)
			{
				row.presence.fragments.push_back("release");
				for (auto& edge : row.contributor_edges)
					edge.condition = row.presence;
			}
		const auto original = identities.queries(true);
		const auto full = take(q::project_source_features(original));
		require(full.features.size() == 2U, "long identities lost their distinct original worlds");
		std::size_t callbacks{};
		q::finite_population_limits limits;
		limits.cancelled = [&]
		{
			return ++callbacks > 65536U;
		};
		q::projection_resource_usage failed;
		const auto stopped = q::project_source_features(original, limits, {}, failed);
		require(!stopped && stopped.error().code == "sdk.source-feature-cancelled" &&
					!failed.operations && !failed.retained_bytes_bound,
				"long identity-prefix comparison ignored cancellation");
	}
	q::projection_resource_usage usage;
	const auto full = take(q::project_source_features(f.input(), {}, {}, usage));
	require(full.features.size() == 1 && full.populations.size() == 2 &&
				full.environments.size() == 1,
			"original populations omitted");
	const auto& feature = full.features.front();
	require(feature.observation == state::complete && feature.source_state == state::complete &&
				feature.context_state == state::complete &&
				feature.entity_state == state::complete && feature.call_state == state::complete &&
				feature.type_state == state::complete && feature.syntax_state == state::complete,
			"independent bindings lost");
	require(full.environments.front().freestanding == false &&
				full.environments.front().state == state::complete,
			"actual hosted false was dropped");
	require(std::ranges::all_of(full.populations,
								[](const auto& p)
								{
									return p.enumeration_state == state::complete &&
										p.membership_state == state::complete;
								}),
			"complete inventory lost");
	require(usage.operations > 0 && usage.retained_bytes_bound > 0, "measured usage missing");
	const auto public_full = take(q::project_source_features(f.queries()));
	require(public_full.feature_inputs_complete && public_full.inventory_inputs_complete &&
				public_full.populations.front().membership_state == state::complete,
			"generic scan-health erased original census");
	{
		auto missing = f;
		missing.rows[6].clear();
		missing.rows[3].clear();
		const auto p = take(q::project_source_features(missing.input()));
		require(p.features.front().type_state == state::unknown &&
					p.features.front().call_state == state::unknown &&
					p.populations.front().membership_state == state::complete,
				"optional bindings erased independent membership");
	}
	{
		auto foreign = f;
		foreign.rows[5].front().values.at("output.compile_unit") =
			detached_cell::typed("compile_unit_id", "other");
		const auto p = take(q::project_source_features(foreign.input()));
		require(p.features.front().context_state != state::complete,
				"foreign unit lent declaration membership");
	}
	{
		auto extra = f;
		set(extra.rows[9][0], "feature_ids", symbols({}));
		set(extra.rows[9][0], "feature_count", num(0));
		const auto p = take(q::project_source_features(extra.input()));
		require(std::ranges::any_of(p.populations,
									[](const auto& v)
									{
										return v.scope == "translation_unit" &&
											v.membership_state == state::conflicting;
									}),
				"extra observed feature fabricated zero");
	}
	{
		auto unknown = f;
		set(unknown.rows[8][0], "profile", txt("future/1"));
		const auto p = take(q::project_source_features(unknown.input()));
		require(p.features.front().observation == state::unknown,
				"future compiler profile interpreted");
	}
	{
		auto duplicate = f;
		duplicate.rows[8].push_back(duplicate.rows[8].front());
		set(duplicate.rows[8].back(), "kind", txt("OtherExpr"));
		const auto p = take(q::project_source_features(duplicate.input()));
		require(p.features.size() == 1 && p.features.front().observation == state::conflicting,
				"contrary original observations collapsed");
	}
	{
		auto repeated = f;
		repeated.rows[8].push_back(repeated.rows[8][0]);
		set(repeated.rows[8][1], "feature", txt("X2"));
		set(repeated.rows[8][1], "ordinal", num(1));
		for (auto& inventory : repeated.rows[9])
		{
			set(inventory, "feature_ids", symbols({"X", "X2"}));
			set(inventory, "feature_count", num(2));
		}
		const auto positive = take(q::project_source_features(repeated.input()));
		require(std::ranges::all_of(positive.features,
									[](const auto& v)
									{
										return v.identity_state == state::complete;
									}),
				"repeated original node lost distinct traversal occurrences");
		set(repeated.rows[8][1], "ordinal", num(0));
		const auto collision = take(q::project_source_features(repeated.input()));
		require(std::ranges::all_of(collision.features,
									[](const auto& v)
									{
										return v.identity_state == state::conflicting;
									}),
				"different IDs lent two identities to one original occurrence");
	}
	{
		auto noncallable = f;
		set(noncallable.rows[3][0], "kind", txt("variable"));
		const auto projected = take(q::project_source_features(noncallable.input()));
		require(projected.features[0].call_state != state::complete &&
					projected.features[0].entity_state == state::complete,
				"noncallable subject became an original selected call target");
	}
	{
		auto none = f;
		set(none.rows[8][0], "source_binding_state", txt("none"));
		set(none.rows[8][0], "source_none_reason", txt("future-none/1"));
		const auto p = take(q::project_source_features(none.input()));
		require(p.features.front().source_state == state::conflicting,
				"unsupported source-none grammar accepted");
	}
	{
		auto none = f;
		auto& original = none.rows[8][0];
		set(original, "source_binding_state", txt("none"));
		for (const auto field : {"file", "source_snapshot", "source"})
			set(original, field, detached_cell::absent({}));
		set(original, "source_none_reason", txt("clang22-invalid-range-implicit-origin/1"));
		set(original, "feature_class", txt("declaration"));
		set(original, "origin", txt("implicit"));
		set(original, "is_implicit", detached_cell::boolean(true));
		require(take(q::project_source_features(none.input())).features[0].source_state ==
					state::complete,
				"actual implicit declaration grammar rejected");
		set(original, "is_implicit", detached_cell::boolean(false));
		set(original, "origin", txt("unknown"));
		require(take(q::project_source_features(none.input())).features[0].source_state ==
					state::conflicting,
				"forged implicit-origin reason accepted");
		set(original, "origin", txt("implicit"));
		set(original, "is_implicit", detached_cell::boolean(true));
		set(original, "feature_class", txt("statement"));
		require(take(q::project_source_features(none.input())).features[0].source_state ==
					state::conflicting,
				"implicit context lent source-none to a statement");
		set(original,
			"source_none_reason",
			txt("clang22-invalid-range-default-activation-wrapper/1"));
		set(original, "origin", txt("activated"));
		set(original, "kind", txt("CXXDefaultArgExpr"));
		set(original, "is_implicit", detached_cell::absent({}));
		require(take(q::project_source_features(none.input())).features[0].source_state ==
					state::complete,
				"actual default activation wrapper rejected");
		set(original, "kind", txt("CallExpr"));
		require(take(q::project_source_features(none.input())).features[0].source_state ==
					state::conflicting,
				"ordinary child accepted default-wrapper grammar");
		set(original, "feature_class", txt("type_location"));
		set(original, "origin", txt("implicit"));
		set(original,
			"source_none_reason",
			txt("clang22-invalid-range-implicit-type-source-info/1"));
		require(take(q::project_source_features(none.input())).features[0].source_state ==
					state::complete,
				"actual implicit TypeSourceInfo grammar rejected");
		set(original, "is_implicit", detached_cell::boolean(false));
		require(take(q::project_source_features(none.input())).features[0].source_state ==
					state::conflicting,
				"contradictory TypeSourceInfo implicit flag accepted");
	}
	{
		auto empty = f;
		empty.rows[8].clear();
		for (auto& inventory : empty.rows[9])
		{
			set(inventory, "feature_count", num(0));
			set(inventory, "feature_ids", symbols({}));
		}
		empty.rows[1].push_back(
			fact(1, {{"file", txt("F2")}, {"snapshot", txt("P2")}, {"size", num(16)}}));
		set(empty.rows[9][0], "entered_file_count", num(2));
		set(empty.rows[9][0], "entered_file_ids", symbols({"F", "F2"}));
		set(empty.rows[9][0], "entered_source_snapshots", symbols({"P", "P2"}));
		empty.rows[9].push_back(empty.rows[9][1]);
		set(empty.rows[9][2], "inventory", txt("FI2"));
		set(empty.rows[9][2], "file", txt("F2"));
		set(empty.rows[9][2], "source_snapshot", txt("P2"));
		const auto complete = take(q::project_source_features(empty.input()));
		require(std::ranges::all_of(complete.populations,
									[](const auto& p)
									{
										return p.entry_state == state::complete &&
											p.identity_state == state::complete;
									}),
				"actual two-file empty populations rejected");
		set(empty.rows[9][1], "source_snapshot", txt("P2"));
		const auto crossed = take(q::project_source_features(empty.input()));
		require(std::ranges::any_of(crossed.populations,
									[](const auto& p)
									{
										return p.inventory == "FI" &&
											p.identity_state == state::conflicting &&
											p.entry_state == state::conflicting &&
											p.source_state == state::conflicting;
									}),
				"crossed file/snapshot invented complete empty population");
		set(empty.rows[9][1], "source_snapshot", txt("P"));
		set(empty.rows[1][1], "file", txt("F"));
		const auto duplicate_file = take(q::project_source_features(empty.input()));
		require(std::ranges::any_of(duplicate_file.populations,
									[](const auto& p)
									{
										return p.scope == "translation_unit" &&
											p.entered_file_state == state::conflicting;
									}),
				"duplicate file snapshots hid an unrepresented entered file");
	}
	{
		auto no_scan = f.queries();
		no_scan.scans.erase(no_scan.scans.begin() + 8);
		const auto p = take(q::project_source_features(no_scan));
		require(!p.feature_inputs_complete &&
					p.populations.front().membership_state != state::complete,
				"absent scan became known empty");
	}
	{
		auto bad = f;
		bad.rows[8].front().values.erase("output.kind");
		usage = {1, 1};
		const auto p = q::project_source_features(bad.input(), {}, {}, usage);
		require(!p && !usage.operations && !usage.retained_bytes_bound,
				"invalid original row or failure usage accepted");
	}
	{
		q::finite_population_limits limit;
		limit.maximum_operations = usage.operations;
		const auto measured = take(q::project_source_features(f.input(), {}, {}, usage));
		(void)measured;
		limit.maximum_operations = usage.operations;
		require(bool(q::project_source_features(f.input(), limit)),
				"exact measured work bound rejected");
		--limit.maximum_operations;
		usage = {1, 1};
		require(!q::project_source_features(f.input(), limit, {}, usage) && !usage.operations &&
					!usage.retained_bytes_bound,
				"work frontier not enforced/reset");
	}
	{
		q::finite_population_limits limit;
		limit.cancelled = []()
		{
			return true;
		};
		usage = {1, 1};
		require(!q::project_source_features(f.input(), limit, {}, usage) && !usage.operations &&
					!usage.retained_bytes_bound,
				"cancellation failed");
	}
	std::cout << "original source feature projection controls passed\n";
}
