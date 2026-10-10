#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <tuple>
#include <type_traits>

#include <cxxlens/sdk/source_features.hpp>

#include "../../../src/sdk/query_projected_row_encoding_internal.hpp"
#include "../../../src/sdk/query_result_internal.hpp"
#include "query_projection_row_copy_controls.hpp"

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
		q::application_query_results queries(bool validated = false, bool sizes = false) const
		{
			q::application_query_results queries;
			queries.snapshot_id = "original:test";
			for (std::size_t group{}; group < rows.size(); ++group)
			{
				auto data = std::make_shared<q::query_result::data>();
				data->row_values = rows[group];
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

namespace
{
	detached_cell symbol_values(std::vector<std::string> ids)
	{
		std::ranges::sort(ids);
		std::vector<std::byte> encoded;
		for (const auto& id : ids)
		{
			for (unsigned shift{}; shift < 32U; shift += 8U)
				encoded.push_back(static_cast<std::byte>((id.size() >> shift) & 255U));
			for (char c : id)
				encoded.push_back(static_cast<std::byte>(c));
		}
		return detached_cell::bytes(std::move(encoded));
	}
	fixture many_members()
	{
		fixture value;
		const auto original = value.rows[8].front();
		value.rows[8].clear();
		std::vector<std::string> ids;
		for (std::size_t ordinal{}; ordinal < 96U; ++ordinal)
		{
			const auto id =
				(ordinal % 2U ? "feature:z:" : "feature:\xc3\xa9:") + std::to_string(ordinal);
			auto row = original;
			set(row, "feature", txt(id));
			set(row, "ordinal", num(ordinal));
			set(row, "original_node_ordinal", num(ordinal));
			value.rows[8].push_back(std::move(row));
			ids.push_back(id);
		}
		std::ranges::reverse(ids);
		for (auto& row : value.rows[9])
		{
			set(row, "feature_ids", symbol_values(ids));
			set(row, "feature_count", num(ids.size()));
		}
		return value;
	}
	void retained_membership_controls()
	{
		const auto original = many_members();
		q::projection_resource_usage measured;
		const auto full =
			take(q::project_source_features(original.queries(true), {}, {}, measured));
		require(full.features.size() == 96U && full.populations.size() == 2U &&
					std::ranges::all_of(full.populations,
										[](const auto& population)
										{
											return population.membership_state == state::complete &&
												population.feature_count == 96U &&
												population.features.size() == 96U;
										}),
				"retained ID membership lost an original or its exact census");
		for (const auto& population : full.populations)
		{
			require(population.feature_ids.size() == 96U &&
						std::ranges::is_sorted(population.feature_ids) &&
						population.feature_ids.front() == "feature:z:1",
					"membership lookup reordered the original inventory array");
		}
		for (const std::string_view mutation :
			 {"omitted", "duplicate", "foreign", "absent", "partial"})
		{
			auto changed = original;
			std::vector<std::string> ids = full.populations.front().feature_ids;
			if (mutation == "omitted")
				ids.pop_back();
			if (mutation == "duplicate")
				ids.push_back(ids.front());
			if (mutation == "foreign")
				ids.push_back("feature:foreign");
			for (auto& row : changed.rows[9])
			{
				set(row, "feature_ids", symbol_values(ids));
				set(row, "feature_count", num(ids.size()));
				if (mutation == "absent")
					row.values.erase("output.feature_ids");
				if (mutation == "partial")
					set(row, "enumeration_state", txt("partial"));
			}
			if (mutation == "absent" || mutation == "duplicate")
			{
				q::projection_resource_usage failed;
				require(!q::project_source_features(changed.queries(), {}, {}, failed) &&
							!failed.operations && !failed.retained_bytes_bound,
						"missing or duplicate original inventory IDs bypassed input guards");
				continue;
			}
			const auto out = take(q::project_source_features(changed.queries(true)));
			require(out.features.size() == 96U, "membership failure erased observed originals");
			for (const auto& population : out.populations)
			{
				if (mutation == "partial")
					require(population.enumeration_state == state::partial,
							"partial native inventory became complete");
				else
					require(population.membership_state != state::complete,
							"missing/duplicate/foreign original inventory became complete");
			}
		}
		for (const std::string_view mutation : {"unit", "world", "profile", "source"})
		{
			auto changed = original;
			auto& row = changed.rows[8].front();
			if (mutation == "unit")
				set(row, "compile_unit", txt("foreign-unit"));
			if (mutation == "profile")
				set(row, "profile", txt("foreign-profile"));
			if (mutation == "source")
				set(row, "source_snapshot", txt("foreign-snapshot"));
			if (mutation == "world")
			{
				row.presence.fragments = {"foreign-world"};
				row.contributor_edges.front().condition = row.presence;
			}
			const auto out = take(q::project_source_features(changed.queries(true)));
			require(out.features.size() == 96U,
					"foreign member erased original feature population");
			require(std::ranges::any_of(out.populations,
										[](const auto& population)
										{
											return population.membership_state != state::complete ||
												population.source_state != state::complete;
										}),
					"foreign unit/world/profile/source lent complete original membership");
		}
		q::finite_population_limits exact;
		exact.maximum_operations = measured.operations;
		exact.maximum_retained_bytes = measured.retained_bytes_bound;
		q::projection_resource_usage repeated;
		require(bool(q::project_source_features(original.queries(true), exact, {}, repeated)) &&
					repeated.operations == measured.operations &&
					repeated.retained_bytes_bound == measured.retained_bytes_bound,
				"retained membership rejected exact measured work/storage peak");
		--exact.maximum_operations;
		require(!q::project_source_features(original.queries(true), exact, {}, repeated) &&
					!repeated.operations && !repeated.retained_bytes_bound,
				"retained membership ignored one-under work or published failed usage");
		exact.maximum_operations = measured.operations;
		--exact.maximum_retained_bytes;
		require(!q::project_source_features(original.queries(true), exact, {}, repeated),
				"retained membership ignored one-under storage peak");
		std::stop_source stop;
		stop.request_stop();
		const auto stopped =
			q::project_source_features(original.queries(true), {}, stop.get_token(), repeated);
		require(!stopped && stopped.error().code == "sdk.source-feature-cancelled" &&
					!repeated.operations && !repeated.retained_bytes_bound,
				"retained membership ignored current cancellation or published "
				"failed usage");
	}
	void retained_unbound_and_declaration_controls()
	{
		fixture unknown_source;
		set(unknown_source.rows[8].front(), "source_binding_state", txt("unknown"));
		set(unknown_source.rows[9].front(), "unbound_feature_ids", symbols({"X"}));
		set(unknown_source.rows[9].front(), "unbound_feature_count", num(1));
		set(unknown_source.rows[9].front(), "source_binding_state", txt("unknown"));
		const auto unbound = take(q::project_source_features(unknown_source.queries(true)));
		const auto unit_population = std::ranges::find(
			unbound.populations, "translation_unit", &q::source_feature_population::scope);
		require(unit_population != unbound.populations.end(),
				"unbound translation-unit inventory missing");
		const auto& unit = *unit_population;
		require(unit.membership_state == state::complete &&
					unit.unbound_feature_ids == std::vector<std::string>{"X"} &&
					unit.source_state != state::complete,
				"authentic unbound subset became absent or a complete source binding");
		for (const bool foreign : {false, true})
		{
			auto changed = unknown_source;
			set(changed.rows[9].front(),
				"unbound_feature_ids",
				foreign ? symbols({"foreign"}) : symbols({}));
			set(changed.rows[9].front(), "unbound_feature_count", num(foreign ? 1U : 0U));
			const auto out = take(q::project_source_features(changed.queries(true)));
			const auto changed_unit = std::ranges::find(
				out.populations, "translation_unit", &q::source_feature_population::scope);
			require(changed_unit != out.populations.end() &&
						changed_unit->membership_state == state::conflicting,
					"omitted or foreign unbound original bypassed exact subset closure");
		}

		auto originals = many_members();
		auto& inventory = originals.rows[5].front();
		set(inventory,
			"physical_definition_profile",
			txt("clang22-original-physical-definitions/1"));
		set(inventory, "physical_definition_state", txt("complete"));
		set(inventory, "physical_definition_count", num(1));
		set(inventory, "physical_definition_ids", symbols({"D"}));
		const auto complete_context = [](const auto& out)
		{
			return out.features.size() == 96U &&
				std::ranges::all_of(out.features,
									[](const auto& feature)
									{
										return feature.context_state == state::complete;
									});
		};
		require(complete_context(take(q::project_source_features(originals.queries(true)))),
				"authentic named and physical inventories lost context closure");
		for (const bool physical_only : {false, true})
		{
			auto changed = originals;
			set(changed.rows[5].front(),
				physical_only ? "declarations" : "physical_definition_ids",
				symbols({"unused"}));
			require(complete_context(take(q::project_source_features(changed.queries(true)))),
					"named and physical decode caches aliased different original fields");
		}
		for (const bool physical : {false, true})
		{
			auto changed = originals;
			set(changed.rows[5].front(),
				physical ? "physical_definition_count" : "declaration_count",
				num(0));
			const auto out = take(q::project_source_features(changed.queries(true)));
			require(std::ranges::all_of(out.features,
										[](const auto& feature)
										{
											return feature.context_state == state::conflicting;
										}),
					"decode reuse bypassed the current inventory count guard");
		}
		for (const std::string_view identity : {"same", "unit", "world", "profile"})
		{
			auto changed = originals;
			auto sibling = changed.rows[5].front();
			set(sibling, "inventory", txt("DI:sibling"));
			set(sibling, "declaration_count", num(0));
			if (identity == "unit")
				set(sibling, "compile_unit", txt("foreign-unit"));
			if (identity == "world")
			{
				sibling.presence.fragments = {"foreign-world"};
				sibling.contributor_edges.front().condition = sibling.presence;
			}
			if (identity == "profile")
			{
				set(sibling, "profile", txt("future-named-profile"));
				set(sibling, "physical_definition_profile", txt("future-physical-profile"));
			}
			changed.rows[5].push_back(std::move(sibling));
			const auto out = take(q::project_source_features(changed.queries(true)));
			if (identity == "same")
				require(std::ranges::all_of(out.features,
											[](const auto& feature)
											{
												return feature.context_state == state::conflicting;
											}),
						"contradictory sibling inventory inherited first-row closure");
			else
				require(complete_context(out),
						"foreign inventory lent or replaced exact context closure");
		}
		set(inventory, "declarations", symbols({"unused"}));
		set(inventory, "physical_definition_ids", symbols({"unused"}));
		const auto changed_owner = take(q::project_source_features(originals.queries(true)));
		require(std::ranges::all_of(changed_owner.features,
									[](const auto& feature)
									{
										return feature.context_state != state::complete;
									}),
				"declaration memo persisted across projections of changed original rows");
		q::source_feature_projection detached;
		{
			auto owner = many_members();
			detached = take(q::project_source_features(owner.input()));
		}
		require(complete_context(detached) && !detached.evidence.empty(),
				"output context depended on expired memo/input owners");
		for (const auto& row : detached.evidence)
			require(bool(row.row.validate()) && !row.row.canonical_form().empty(),
					"detached evidence retained a borrowed inventory buffer");
	}
	void indexed_membership_prefix_controls()
	{
		auto originals = many_members();
		for (auto& group : originals.rows)
			for (auto& row : group)
			{
				row.presence.universe = "world:" + std::string(128U, 'u');
				row.presence.fragments = {"variant:" + std::string(128U, 'v')};
				row.interpretation = "interpretation:" + std::string(128U, 'i');
				for (auto& edge : row.contributor_edges)
				{
					edge.condition = row.presence;
					edge.interpretation = row.interpretation;
				}
			}
		const auto input = originals.queries(true);
		std::size_t visits{};
		q::finite_population_limits limits;
		limits.cancelled = [&]
		{
			++visits;
			return false;
		};
		q::projection_resource_usage measured, repeated;
		const auto full = take(q::project_source_features(input, limits, {}, measured));
		require(full.features.size() == 96U &&
					std::ranges::all_of(full.populations,
										[](const auto& population)
										{
											return population.membership_state == state::complete;
										}),
				"long exact world keys changed the closed original feature census");
		auto empty_listed = originals;
		for (auto& inventory : empty_listed.rows[9])
		{
			set(inventory, "feature_ids", symbols({}));
			set(inventory, "feature_count", num(0));
		}
		q::projection_resource_usage unlisted_usage;
		const auto unlisted =
			take(q::project_source_features(empty_listed.queries(true), {}, {}, unlisted_usage));
		require(unlisted.features.size() == full.features.size() &&
					std::ranges::all_of(unlisted.populations,
										[](const auto& population)
										{
											return population.membership_state ==
												state::conflicting;
										}),
				"empty listed inventory erased actual originals or fabricated closed zero");
		// Every listed key must inspect its complete physical ID. Equal Worlds may
		// reuse an admitted local identity; the empty census omits these ID reads.
		std::size_t minimum_equal_id_work{};
		for (const auto& population : full.populations)
			for (const auto& id : population.feature_ids)
				minimum_equal_id_work += 2U * id.size() + 1U;
		require(measured.operations > unlisted_usage.operations &&
					measured.operations - unlisted_usage.operations >= minimum_equal_id_work,
				"listed membership did not charge its actual full-ID comparisons");
		limits.cancelled = {};
		limits.maximum_operations = measured.operations;
		limits.maximum_retained_bytes = measured.retained_bytes_bound;
		require(bool(q::project_source_features(input, limits, {}, repeated)) &&
					repeated.operations == measured.operations &&
					repeated.retained_bytes_bound == measured.retained_bytes_bound,
				"actual index comparisons changed exact charged work/storage");
		--limits.maximum_operations;
		require(!q::project_source_features(input, limits, {}, repeated) && !repeated.operations &&
					!repeated.retained_bytes_bound,
				"index comparison bypassed one-under work or published failed usage");
		limits.maximum_operations = measured.operations;
		--limits.maximum_retained_bytes;
		require(!q::project_source_features(input, limits, {}, repeated) && !repeated.operations &&
					!repeated.retained_bytes_bound,
				"index comparison bypassed one-under storage or published failed usage");

		auto foreign = originals;
		std::vector<std::string> ids = full.populations.front().feature_ids;
		ids.front() = "!foreign-first-byte";
		for (auto& inventory : foreign.rows[9])
			set(inventory, "feature_ids", symbol_values(ids));
		const auto unknown = take(q::project_source_features(foreign.queries(true)));
		require(unknown.features.size() == 96U &&
					std::ranges::all_of(unknown.populations,
										[](const auto& population)
										{
											return population.membership_state != state::complete;
										}),
				"first-byte mismatch borrowed long matching world keys for foreign membership");

		require(visits > 65536U, "fixture did not reach long original-key comparisons");
		const auto midpoint = visits / 2U;
		std::size_t prefix{};
		std::stop_source stop;
		q::finite_population_limits interrupted;
		interrupted.cancelled = [&]
		{
			if (++prefix == midpoint)
				stop.request_stop();
			return false;
		};
		const auto stopped =
			q::project_source_features(input, interrupted, stop.get_token(), repeated);
		require(
			!stopped && stopped.error().code == "sdk.source-feature-cancelled" &&
				prefix == midpoint && !repeated.operations && !repeated.retained_bytes_bound,
			"long comparison failed to observe a real mid-projection stop before the next visit");
	}
	void canonical_size_controls()
	{
		auto row = fact(8U, {{"feature", txt("feature:size")}});
		row.multiplicity = std::numeric_limits<std::uint64_t>::max();
		row.values.emplace("corner.signed",
						   detached_cell::signed_integer(std::numeric_limits<std::int64_t>::min()));
		row.values.emplace(
			"corner.unsigned",
			detached_cell::unsigned_integer(std::numeric_limits<std::uint64_t>::max()));
		row.values.emplace(
			"corner.bytes",
			detached_cell::bytes({std::byte{0x00}, std::byte{0x80}, std::byte{0xff}}));
		row.values.emplace("corner.string",
						   txt(std::string{"quote\"\\\b\f\n\r\t"} + char(0x01) + "日本語"));
		require(bool(row.validate()), "canonical size fixture is not an admitted row");
		const auto wire = row.canonical_form();
		require(wire.find("-9223372036854775808") != std::string::npos &&
					wire.find("18446744073709551615") != std::string::npos &&
					wire.find("\"0080ff\"") != std::string::npos &&
					wire.find("\\u0001日本語") != std::string::npos,
				"canonical size fixture lost integer, lowercase hex or escaped UTF-8 spelling");
		std::size_t work{};
		const auto measured = q::detail::admitted_projected_row_size(
			row,
			[&]
			{
				++work;
			},
			[]
			{
				throw std::length_error("size");
			});
		require(measured == wire.size() && work > 0U && work < measured,
				"canonical size differs from complete wire or serializes framing bytes");
		const auto exact_work = work;
		std::size_t visited{};
		require(q::detail::admitted_projected_row_size(
					row,
					[&]
					{
						if (visited == exact_work)
							throw std::runtime_error("work");
						++visited;
					},
					[]
					{
						throw std::length_error("size");
					}) == measured &&
					visited == exact_work,
				"canonical size rejected the exact visited-work frontier");
		visited = 0U;
		bool stopped = false;
		try
		{
			(void)q::detail::admitted_projected_row_size(
				row,
				[&]
				{
					if (visited == exact_work - 1U)
						throw std::runtime_error("work");
					++visited;
				},
				[]
				{
					throw std::length_error("size");
				});
		}
		catch (const std::runtime_error&)
		{
			stopped = true;
		}
		require(stopped && visited == exact_work - 1U,
				"canonical size bypassed one-under work frontier");
		visited = 0U;
		stopped = false;
		try
		{
			(void)q::detail::admitted_projected_row_size(
				row,
				[&]
				{
					if (visited == 90U)
						throw std::runtime_error("stop");
					++visited;
				},
				[]
				{
					throw std::length_error("size");
				});
		}
		catch (const std::runtime_error&)
		{
			stopped = true;
		}
		require(stopped && visited == 90U, "canonical size ignored a stop during nested text");
		// Invalid UTF-8 keeps the original encoder's empty quoted spelling.
		for (const std::string& text :
			 {std::string{"valid\"\\日本語"},
			  std::string{"prefix"} + char(0xc2),
			  std::string{"prefix"} + char(0xe0) + char(0x80) + char(0x80),
			  std::string{"prefix"} + char(0xf4) + char(0x90) + char(0x80) + char(0x80),
			  std::string{"prefix"} + char(0xc2) + 'x'})
		{
			q::detail::projected_row_size_sink sink{[]
													{
													},
													[]
													{
														throw std::length_error("size");
													}};
			sink.string(text);
			require(sink.size() == cxxlens::sdk::detail::canonical_json_string(text).size(),
					"canonical size changed invalid UTF-8 or escaped text admission bytes");
		}
	}
	void whole_evidence_clone_controls()
	{
		fixture original;
		constexpr std::size_t payload_bytes = 32768U;
		std::vector<std::byte> payload(payload_bytes, std::byte{0xff});
		payload.back() = std::byte{0x80};
		set(original.rows[3].front(), "provider_local_key", detached_cell::bytes(payload));
		set(original.rows[8].front(), "kind", txt("quoted\"\\\n\t日本語"));
		set(original.rows[8].front(),
			"compiler_kind",
			num(std::numeric_limits<std::uint64_t>::max()));
		set(original.rows[8].front(), "is_implicit", detached_cell::boolean(true));
		auto& feature = original.rows[8].front();
		feature.multiplicity = 17U;
		set(feature,
			"source_none_reason",
			detached_cell::unknown(feature.values.at("output.source_none_reason").type,
								   "unobserved\"\\日本語"));
		for (auto& group : original.rows)
			for (auto& row : group)
			{
				row.claim_contributors.push_back("claim:z");
				row.producer_contracts.push_back({"source.z", "semantic:z"});
				row.provenance.push_back("features:z");
				row.contributor_guarantees.push_back(
					{"exact", "z", "z", {"native", "schema_validated"}});
				row.contributor_edges.push_back({row.claim_contributors.back(),
												 row.producer_contracts.back(),
												 row.provenance.back(),
												 row.contributor_guarantees.back(),
												 row.presence,
												 row.interpretation});
			}
		const auto input = original.queries(true);
		q::projection_resource_usage measured;
		const auto out = take(q::project_source_features(input, {}, {}, measured));
		require(out.features.size() == 1U &&
					out.features.front().compiler_kind ==
						std::numeric_limits<std::uint64_t>::max() &&
					out.features.front().is_implicit == true,
				"typed evidence copy altered scalar values");
		require(out.evidence.size() == 11U, "typed evidence copy lost an original row");
		std::size_t evidence_bytes{};
		for (const auto& evidence : out.evidence)
		{
			const auto group = std::ranges::find(names, evidence.relation_id);
			require(group != names.end(), "typed evidence copy lost relation identity");
			const auto group_index = static_cast<std::size_t>(group - names.begin());
			const auto canonical = evidence.row.canonical_form();
			require(std::ranges::any_of(original.rows[group_index],
										[&](const auto& row)
										{
											return row.canonical_form() == canonical;
										}),
					"typed evidence copy changed a cell or nested original annotation");
			evidence_bytes += canonical.size();
		}
		require(out.source_queries.has_value() &&
					out.source_queries->snapshot_id == input.snapshot_id,
				"typed evidence copy omitted the source query owner");
		for (std::size_t i{}; i < input.scans.size(); ++i)
			require(out.source_queries->scans[i].result.canonical_form() ==
						input.scans[i].result.canonical_form(),
					"typed evidence copy changed a query sidechannel");
		q::finite_population_limits exact;
		exact.maximum_operations = measured.operations;
		exact.maximum_retained_bytes = measured.retained_bytes_bound;
		exact.maximum_evidence_bytes = evidence_bytes;
		q::projection_resource_usage repeated;
		require(bool(q::project_source_features(input, exact, {}, repeated)) &&
					repeated.operations == measured.operations &&
					repeated.retained_bytes_bound == measured.retained_bytes_bound,
				"typed evidence copy rejected exact work/storage/evidence bounds");
		--exact.maximum_evidence_bytes;
		require(!q::project_source_features(input, exact, {}, repeated) && !repeated.operations &&
					!repeated.retained_bytes_bound,
				"typed evidence copy bypassed canonical evidence-byte cap");
		exact.maximum_evidence_bytes = evidence_bytes;
		--exact.maximum_operations;
		require(!q::project_source_features(input, exact, {}, repeated) && !repeated.operations &&
					!repeated.retained_bytes_bound,
				"typed evidence copy bypassed work cap or exposed failed usage");
		exact.maximum_operations = measured.operations;
		--exact.maximum_retained_bytes;
		require(!q::project_source_features(input, exact, {}, repeated) && !repeated.operations &&
					!repeated.retained_bytes_bound,
				"typed evidence copy bypassed retained cap or exposed failed usage");
		exact.maximum_retained_bytes = measured.retained_bytes_bound;
		std::size_t callbacks{};
		exact.cancelled = [&]
		{
			return ++callbacks > 3000U;
		};
		const auto stopped = q::project_source_features(input, exact, {}, repeated);
		require(!stopped && stopped.error().code == "sdk.source-feature-cancelled" &&
					!repeated.operations && !repeated.retained_bytes_bound,
				"typed evidence copy ignored cancellation during projection");
	}
	void first_present_comparison_controls()
	{
		constexpr std::size_t bytes = 65536U;
		fixture short_rows;
		q::projection_resource_usage short_usage;
		(void)take(q::project_source_features(short_rows.queries(true), {}, {}, short_usage));
		auto large = short_rows;
		const std::string kind(bytes, 'k'), toolchain(bytes, 't');
		set(large.rows[8].front(), "kind", txt(kind));
		set(large.rows[0].front(), "toolchain", txt(toolchain));
		const auto input = large.queries(true);
		q::projection_resource_usage measured;
		const auto full = take(q::project_source_features(input, {}, {}, measured));
		require(full.features.size() == 1U && full.features.front().kind == kind &&
					full.features.front().observation == state::complete &&
					full.features.front().identity_state == state::complete,
				"large first observations lost their original payload or identity");
		// Evidence serialization and output copies still visit the actual bytes.
		// Initial observations have no other cell whose bytes must be compared.
		require(measured.operations > short_usage.operations + 4U * bytes &&
					measured.operations < short_usage.operations + 6U * bytes,
				"first observations charged payload equality without a second cell");
		q::finite_population_limits exact;
		exact.maximum_operations = measured.operations;
		exact.maximum_retained_bytes = measured.retained_bytes_bound;
		q::projection_resource_usage repeated;
		require(bool(q::project_source_features(input, exact, {}, repeated)) &&
					repeated.operations == measured.operations &&
					repeated.retained_bytes_bound == measured.retained_bytes_bound,
				"first-observation exact work or storage bound rejected");
		--exact.maximum_operations;
		require(!q::project_source_features(input, exact, {}, repeated) && !repeated.operations &&
					!repeated.retained_bytes_bound,
				"first-observation work frontier accepted or leaked usage");
		exact.maximum_operations = measured.operations;
		--exact.maximum_retained_bytes;
		require(!q::project_source_features(input, exact, {}, repeated) && !repeated.operations &&
					!repeated.retained_bytes_bound,
				"first-observation storage frontier accepted or leaked usage");
		auto duplicate = large;
		duplicate.rows[8].push_back(duplicate.rows[8].front());
		const auto equal = take(q::project_source_features(duplicate.queries(true)));
		require(equal.features.size() == 1U && equal.features.front().kind == kind &&
					equal.features.front().observation == state::complete,
				"distinct equal original cells became contrary observations");
		auto late_kind = kind;
		late_kind.back() = 'z';
		set(duplicate.rows[8].back(), "kind", txt(late_kind));
		const auto contrary = take(q::project_source_features(duplicate.queries(true)));
		require(contrary.features.size() == 1U &&
					contrary.features.front().observation == state::conflicting,
				"distinct late-byte payload disagreement borrowed first-cell equality");
		duplicate.rows[8].back().presence.fragments = {"foreign-world"};
		duplicate.rows[8].back().contributor_edges.front().condition =
			duplicate.rows[8].back().presence;
		const auto worlds = take(q::project_source_features(duplicate.queries(true)));
		require(worlds.features.size() == 2U &&
					std::ranges::all_of(worlds.features,
										[](const auto& feature)
										{
											return feature.observation == state::complete;
										}) &&
					worlds.features[0].kind != worlds.features[1].kind,
				"first-cell comparison shortcut crossed original worlds");
	}
} // namespace

void supporting_lookup_controls()
{
	fixture indexed;
	constexpr std::array<std::string_view, 10U> ids{
		"", "snapshot", "span", "entity", "declaration", "", "type", "node", "", ""};
	const auto complete = [](const auto& out)
	{
		if (out.features.size() != 1U)
			return false;
		const auto& feature = out.features.front();
		return feature.source_state == state::complete &&
			feature.context_state == state::complete && feature.entity_state == state::complete &&
			feature.call_state == state::complete && feature.type_state == state::complete &&
			feature.syntax_state == state::complete;
	};
	for (const std::size_t group : {1U, 2U, 3U, 4U, 6U, 7U})
	{
		auto alternative = indexed.rows[group].front();
		alternative.provenance = {"provenance:alternative-source"};
		alternative.contributor_edges.front().provenance = alternative.provenance.front();
		indexed.rows[group].push_back(std::move(alternative));
		for (const unsigned axis : {0U, 1U, 2U})
		{
			auto foreign = indexed.rows[group].front();
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
			if (group == 1U || group == 2U)
				set(foreign, group == 1U ? "size" : "begin", num(99U));
			else if (group == 3U)
				set(foreign, "kind", txt("variable"));
			else if (group == 4U)
				set(foreign, "source", txt("source:foreign"));
			else if (group == 6U)
				set(foreign, "constructor", txt("pointer"));
			else
				set(foreign, "kind", txt("DeclRefExpr"));
			indexed.rows[group].push_back(std::move(foreign));
		}
		for (unsigned i{}; i < 24U; ++i)
		{
			auto unrelated = indexed.rows[group].front();
			set(unrelated, ids[group], txt(std::string(4096U, 's') + std::to_string(i)));
			indexed.rows[group].push_back(std::move(unrelated));
		}
	}
	const auto out = take(q::project_source_features(indexed.queries(true)));
	require(complete(out), "supporting identity buckets merged a foreign world");
	std::vector<std::string> expected;
	for (const auto& evidence : out.evidence)
		expected.push_back(evidence.relation_id + evidence.row.canonical_form());
	for (const std::size_t group : {1U, 2U, 3U, 4U, 6U, 7U})
		std::ranges::reverse(indexed.rows[group]);
	// Exact work bounds apply to this immutable original owner, including the
	// metered pointer-key alternative ordering comparisons.
	const auto reordered_input = indexed.queries(true);
	q::projection_resource_usage baseline;
	const auto reordered = take(q::project_source_features(reordered_input, {}, {}, baseline));
	std::vector<std::string> actual;
	for (const auto& evidence : reordered.evidence)
		actual.push_back(evidence.relation_id + evidence.row.canonical_form());
	require(actual == expected && complete(reordered),
			"supporting alternative order changed complete original evidence");
	for (const bool storage : {false, true})
		for (const bool one_under : {false, true})
		{
			q::finite_population_limits limits;
			if (storage)
				limits.maximum_retained_bytes =
					baseline.retained_bytes_bound - static_cast<std::size_t>(one_under);
			else
				limits.maximum_operations =
					baseline.operations - static_cast<std::size_t>(one_under);
			q::projection_resource_usage usage{1U, 1U};
			const auto bounded = q::project_source_features(reordered_input, limits, {}, usage);
			require(static_cast<bool>(bounded) == !one_under,
					"supporting lookup exact and one-under quota");
			if (one_under)
				require(!usage.operations && !usage.retained_bytes_bound,
						"failed supporting lookup published success usage");
		}
	std::size_t checkpoints{};
	q::finite_population_limits limits;
	limits.cancelled = [&]
	{
		return ++checkpoints == 1000U;
	};
	q::projection_resource_usage usage{1U, 1U};
	const auto stopped = q::project_source_features(reordered_input, limits, {}, usage);
	require(!stopped && stopped.error().code == "sdk.source-feature-cancelled" &&
				checkpoints == 1000U && !usage.operations && !usage.retained_bytes_bound,
			"long supporting ID hashing ignored cancellation or published failure usage");
}

void occurrence_index_controls()
{
	for (const unsigned axis : {0U, 1U, 2U, 3U, 4U, 5U, 6U})
	{
		fixture original;
		auto sibling = original.rows[8].front();
		set(sibling, "feature", txt("Y"));
		auto unit = original.rows[0].front();
		if (axis == 0U)
		{
			set(sibling, "compile_unit", txt("U:foreign"));
			set(unit, "compile_unit", txt("U:foreign"));
		}
		if (axis == 1U)
			set(sibling, "profile", txt("future-feature-profile"));
		if (axis == 2U)
			sibling.presence.universe = unit.presence.universe = "foreign-universe";
		if (axis == 3U)
			sibling.presence.fragments = unit.presence.fragments = {"foreign-variant"};
		if (axis == 4U)
			sibling.interpretation = unit.interpretation = "foreign-interpretation";
		if (axis == 5U)
			set(sibling, "ordinal", num(1U));
		sibling.contributor_edges.front().condition = sibling.presence;
		sibling.contributor_edges.front().interpretation = sibling.interpretation;
		unit.contributor_edges.front().condition = unit.presence;
		unit.contributor_edges.front().interpretation = unit.interpretation;
		if (axis == 0U || axis == 2U || axis == 3U || axis == 4U)
			original.rows[0].push_back(std::move(unit));
		original.rows[8].push_back(std::move(sibling));
		original.rows[9].clear();
		const auto output = take(q::project_source_features(original.queries(true)));
		require(output.features.size() == 2U &&
					std::ranges::all_of(output.features,
										[&](const auto& feature)
										{
											return feature.identity_state ==
												(axis == 6U ? state::conflicting : state::complete);
										}),
				"occurrence duplicate identity omitted an exact axis or merged foreign keys");
		require(output.features[0].feature == "X" && output.features[1].feature == "Y",
				"private occurrence lookup changed public feature ordering");
	}

	fixture long_prefix;
	long_prefix.rows[9].clear();
	const auto unit_id = std::string(4096U, 'u');
	for (auto& group : long_prefix.rows)
		for (auto& row : group)
			if (row.values.contains("output.compile_unit"))
				set(row, "compile_unit", txt(unit_id));
	set(long_prefix.rows[8].front(), "profile", txt(std::string(4096U, 'p') + "a"));
	auto sibling = long_prefix.rows[8].front();
	set(sibling, "feature", txt("Y"));
	set(sibling, "profile", txt(std::string(4096U, 'p') + "b"));
	long_prefix.rows[8].push_back(std::move(sibling));
	const auto input = long_prefix.queries(true);
	std::size_t checkpoints{};
	q::finite_population_limits observed;
	observed.cancelled = [&]
	{
		++checkpoints;
		return false;
	};
	q::projection_resource_usage baseline;
	const auto complete = take(q::project_source_features(input, observed, {}, baseline));
	require(complete.features.size() == 2U &&
				complete.features[0].identity_state == state::complete &&
				complete.features[1].identity_state == state::complete && checkpoints > 512U,
			"late profile byte changed exact occurrence attribution");
	for (const bool storage : {false, true})
		for (const bool one_under : {false, true})
		{
			q::finite_population_limits limits;
			if (storage)
				limits.maximum_retained_bytes =
					baseline.retained_bytes_bound - static_cast<std::size_t>(one_under);
			else
				limits.maximum_operations =
					baseline.operations - static_cast<std::size_t>(one_under);
			q::projection_resource_usage usage{1U, 1U};
			const auto bounded = q::project_source_features(input, limits, {}, usage);
			require(static_cast<bool>(bounded) == !one_under &&
						(!one_under || (!usage.operations && !usage.retained_bytes_bound)),
					"occurrence comparator ignored exact/one-under work or storage");
		}
	// With no populations, the final long common-prefix comparison is the last work.
	const auto stop_at = checkpoints - 512U;
	std::size_t visited{};
	q::finite_population_limits cancelled;
	cancelled.cancelled = [&]
	{
		return ++visited == stop_at;
	};
	q::projection_resource_usage failed{1U, 1U};
	const auto stopped = q::project_source_features(input, cancelled, {}, failed);
	require(!stopped && stopped.error().code == "sdk.source-feature-cancelled" &&
				visited == stop_at && !failed.operations && !failed.retained_bytes_bound,
			"occurrence long-prefix comparison ignored cancellation or exposed partial output");
}

void derived_feature_lookup_controls()
{
	const fixture original;
	auto cohorts = original;
	for (const unsigned axis : {0U, 1U, 2U})
		for (const std::size_t group : {0U, 8U, 9U})
			for (const auto& saved : original.rows[group])
			{
				auto sibling = saved;
				if (axis == 0U)
					sibling.presence.universe = "world:foreign";
				else if (axis == 1U)
					sibling.presence.fragments = {"variant:foreign"};
				else
					sibling.interpretation = "interpretation:foreign";
				sibling.contributor_edges.front().condition = sibling.presence;
				sibling.contributor_edges.front().interpretation = sibling.interpretation;
				if (axis == 0U && group == 8U)
					set(sibling, "source_binding_state", txt("unknown"));
				if (axis == 0U && group == 9U &&
					std::get<std::string>(*sibling.values.at("output.scope").value) ==
						"translation_unit")
				{
					set(sibling, "unbound_feature_ids", symbols({"X"}));
					set(sibling, "unbound_feature_count", num(1U));
				}
				cohorts.rows[group].push_back(std::move(sibling));
			}
	const auto check = [](const auto& output)
	{
		require(output.features.size() == 4U && output.populations.size() == 8U,
				"same-ID worlds changed owning feature/population cardinality");
		std::vector<std::size_t> positions;
		for (const auto& population : output.populations)
		{
			require(population.features.size() == 1U &&
						population.features.front() < output.features.size(),
					"derived feature lookup lost an exact population member");
			const auto position = population.features.front();
			const auto& feature = output.features[position];
			require(feature.feature == "X" && feature.universe == population.universe &&
						feature.variant == population.variant &&
						feature.interpretation == population.interpretation,
					"derived numeric feature position crossed a same-ID world");
			if (population.scope == "translation_unit" && population.universe == "world:foreign")
				require(population.membership_state == state::complete &&
							feature.source_binding_state == "unknown",
						"same-ID unbound lookup borrowed a complete source from another world");
			positions.push_back(position);
		}
		return positions;
	};
	const auto full = take(q::project_source_features(cohorts.queries(true)));
	const auto expected = check(full);
	for (const std::size_t group : {0U, 8U, 9U})
		std::ranges::reverse(cohorts.rows[group]);
	const auto reordered = take(q::project_source_features(cohorts.queries(true)));
	require(check(reordered) == expected,
			"private derived feature lookup reordered owning population positions");
	require(full.evidence.size() == reordered.evidence.size(), "reordered world evidence lost");
	for (std::size_t at{}; at < full.evidence.size(); ++at)
		require(full.evidence[at].relation_id == reordered.evidence[at].relation_id &&
					full.evidence[at].row.canonical_form() ==
						reordered.evidence[at].row.canonical_form(),
				"private derived feature lookup reordered complete original evidence");
}

void declaration_closure_reuse_controls()
{
	auto originals = many_members();
	const auto input = originals.queries(true);
	q::projection_resource_usage measured;
	const auto full = take(q::project_source_features(input, {}, {}, measured));
	require(full.features.size() == 96U &&
				std::ranges::all_of(full.features,
									[](const auto& feature)
									{
										return feature.context_state == state::complete;
									}),
			"repeated declaration contexts lost exact inventory closure");
	std::size_t references{};
	for (const auto& environment : full.environments)
		references += environment.evidence.size();
	for (const auto& feature : full.features)
		references += feature.evidence.size();
	for (const auto& population : full.populations)
		references += population.evidence.size();
	q::finite_population_limits exact;
	exact.maximum_operations = measured.operations;
	exact.maximum_retained_bytes = measured.retained_bytes_bound;
	exact.maximum_evidence_references = references;
	q::projection_resource_usage repeated;
	require(bool(q::project_source_features(input, exact, {}, repeated)) &&
				repeated.operations == measured.operations &&
				repeated.retained_bytes_bound == measured.retained_bytes_bound,
			"repeated declaration closure rejected exact work/storage/reference bounds");
	--exact.maximum_evidence_references;
	const auto missing_reference = q::project_source_features(input, exact, {}, repeated);
	require(!missing_reference && missing_reference.error().code == "sdk.source-feature-budget" &&
				missing_reference.error().field == "evidence-references" && !repeated.operations &&
				!repeated.retained_bytes_bound,
			"cached declaration evidence bypassed the per-output reference bound");
	exact.maximum_evidence_references = references;
	--exact.maximum_operations;
	require(!q::project_source_features(input, exact, {}, repeated) && !repeated.operations &&
				!repeated.retained_bytes_bound,
			"declaration closure reuse bypassed one-under actual work");
	exact.maximum_operations = measured.operations;
	--exact.maximum_retained_bytes;
	require(!q::project_source_features(input, exact, {}, repeated) && !repeated.operations &&
				!repeated.retained_bytes_bound,
			"declaration closure reuse bypassed one-under live storage");

	for (const std::string_view axis : {"compile_unit", "universe", "variant", "interpretation"})
	{
		auto changed = originals;
		const std::string foreign_unit = "U:foreign:" + std::string(128U, 'u');
		if (axis == "compile_unit")
		{
			auto unit = changed.rows[0].front();
			set(unit, "compile_unit", txt(foreign_unit));
			changed.rows[0].push_back(std::move(unit));
		}
		auto declaration = changed.rows[4].front();
		if (axis == "universe")
			declaration.presence.universe = "foreign:universe";
		if (axis == "variant")
			declaration.presence.fragments = {"foreign:variant"};
		if (axis == "interpretation")
			declaration.interpretation = "foreign:interpretation";
		declaration.contributor_edges.front().condition = declaration.presence;
		declaration.contributor_edges.front().interpretation = declaration.interpretation;
		if (axis != "compile_unit")
			changed.rows[4].push_back(declaration);
		for (std::size_t at{}; at < changed.rows[8].size(); ++at)
		{
			if (at % 2U == 0U)
				continue;
			auto& feature = changed.rows[8][at];
			if (axis == "compile_unit")
				set(feature, "compile_unit", txt(foreign_unit));
			else
			{
				feature.presence = declaration.presence;
				feature.interpretation = declaration.interpretation;
				feature.contributor_edges.front().condition = feature.presence;
				feature.contributor_edges.front().interpretation = feature.interpretation;
			}
		}
		const auto out = take(q::project_source_features(changed.queries(true)));
		require(out.features.size() == 96U, "closure reuse erased a foreign original context");
		for (const auto& feature : out.features)
			require(feature.context_state ==
						(feature.ordinal % 2U ? state::partial : state::complete),
					"declaration closure reuse crossed compile-unit or exact-world axes");
	}

	auto contradictory = originals.rows[4].front();
	set(contradictory, "kind", txt("ContraryKind"));
	originals.rows[4].push_back(std::move(contradictory));
	const auto conflict = take(q::project_source_features(originals.queries(true)));
	require(std::ranges::all_of(conflict.features,
								[](const auto& feature)
								{
									return feature.context_state == state::conflicting;
								}),
			"completed closure persisted across projection of contradictory original rows");

	// Repeated original declarations and contexts fill the small memo. The
	// extra exact key must use the same complete cold resolver when it is full.
	fixture overflow;
	const auto native_declaration = overflow.rows[4].front();
	const auto native_feature = overflow.rows[8].front();
	overflow.rows[4].clear();
	overflow.rows[8].clear();
	std::vector<std::string> declaration_ids, feature_ids;
	for (std::size_t at{}; at < 129U; ++at)
	{
		const auto id = "declaration:closure:" + std::to_string(at);
		auto declaration = native_declaration;
		set(declaration, "declaration", txt(id));
		overflow.rows[4].push_back(std::move(declaration));
		declaration_ids.push_back(id);
		for (std::size_t repeat{}; repeat < 2U; ++repeat)
		{
			const auto ordinal = at * 2U + repeat;
			const auto feature_id = "feature:closure:" + std::to_string(ordinal);
			auto feature = native_feature;
			set(feature, "feature", txt(feature_id));
			set(feature, "ordinal", num(ordinal));
			set(feature, "original_node_ordinal", num(ordinal));
			set(feature, "declaration", txt(id));
			set(feature, "declaration_binding_state", txt("complete"));
			set(feature, "context_declaration", txt(id));
			overflow.rows[8].push_back(std::move(feature));
			feature_ids.push_back(feature_id);
		}
	}
	set(overflow.rows[5].front(), "declarations", symbol_values(declaration_ids));
	set(overflow.rows[5].front(), "declaration_count", num(declaration_ids.size()));
	for (auto& inventory : overflow.rows[9])
	{
		set(inventory, "feature_ids", symbol_values(feature_ids));
		set(inventory, "feature_count", num(feature_ids.size()));
	}
	const auto overflow_input = overflow.queries(true);
	q::projection_resource_usage overflow_usage;
	const auto complete_overflow =
		take(q::project_source_features(overflow_input, {}, {}, overflow_usage));
	require(complete_overflow.features.size() == 258U &&
				std::ranges::all_of(complete_overflow.features,
									[](const auto& feature)
									{
										return feature.declaration_state == state::complete &&
											feature.context_state == state::complete &&
											feature.source_state == state::complete &&
											feature.entity_state == state::complete &&
											feature.call_state == state::complete &&
											feature.type_state == state::complete &&
											feature.syntax_state == state::complete;
									}),
			"full closure memo changed cold declaration/context resolution or other original axes");
	q::finite_population_limits overflow_exact;
	overflow_exact.maximum_operations = overflow_usage.operations;
	overflow_exact.maximum_retained_bytes = overflow_usage.retained_bytes_bound;
	require(bool(q::project_source_features(overflow_input, overflow_exact, {}, repeated)) &&
				repeated.operations == overflow_usage.operations &&
				repeated.retained_bytes_bound == overflow_usage.retained_bytes_bound,
			"full closure memo rejected exact cold-fallback work/storage");
	--overflow_exact.maximum_operations;
	require(!q::project_source_features(overflow_input, overflow_exact, {}, repeated) &&
				!repeated.operations && !repeated.retained_bytes_bound,
			"cold closure fallback ignored one-under work or exposed failed usage");
	overflow_exact.maximum_operations = overflow_usage.operations;
	--overflow_exact.maximum_retained_bytes;
	require(!q::project_source_features(overflow_input, overflow_exact, {}, repeated),
			"cold closure fallback ignored one-under storage");

	std::size_t calls{};
	q::finite_population_limits observed;
	observed.cancelled = [&]
	{
		++calls;
		return false;
	};
	require(bool(q::project_source_features(input, observed)), "closure callback census failed");
	for (const auto divisor : {2U, 3U})
	{
		std::size_t visited{};
		q::finite_population_limits interrupted;
		interrupted.cancelled = [&]
		{
			return ++visited >= calls / divisor;
		};
		const auto stopped = q::project_source_features(input, interrupted, {}, repeated);
		require(!stopped && stopped.error().code == "sdk.source-feature-cancelled" &&
					!repeated.operations && !repeated.retained_bytes_bound,
				"closure reuse ignored current cancellation or exposed failed usage");
	}
}

namespace
{
	void immutable_evidence_controls()
	{
		static_assert(!std::is_constructible_v<q::finite_population_evidence,
											   std::string,
											   std::shared_ptr<const q::annotated_row>>);
		q::finite_population_evidence default_empty{"fixture", {}};
		require(&default_empty.original_row() == &default_empty.row,
				"default brace construction retained detached access");
		const auto observed_language_environment_fields =
			[](const q::observed_language_environment& r)
		{
			return std::tie(r.compile_unit,
							r.universe,
							r.variant,
							r.interpretation,
							r.profile,
							r.observation_state,
							r.freestanding,
							r.state,
							r.evidence,
							r.gaps);
		};
		const auto observed_source_feature_fields = [](const q::observed_source_feature& r)
		{
			return std::tie(r.feature,
							r.compile_unit,
							r.universe,
							r.variant,
							r.interpretation,
							r.profile,
							r.ordinal,
							r.original_node_ordinal,
							r.compiler_kind,
							r.feature_class,
							r.kind,
							r.origin,
							r.evaluation,
							r.observation_state,
							r.file,
							r.source_snapshot,
							r.source_span,
							r.source_none_reason,
							r.is_implicit,
							r.is_system,
							r.declaration,
							r.context_declaration,
							r.subject_entity,
							r.call_target,
							r.subject_type,
							r.syntax,
							r.reference_kind,
							r.type_role,
							r.source_binding_state,
							r.declaration_binding_state,
							r.context_binding_state,
							r.entity_binding_state,
							r.call_binding_state,
							r.type_binding_state,
							r.identity_state,
							r.observation,
							r.source_state,
							r.declaration_state,
							r.context_state,
							r.entity_state,
							r.call_state,
							r.type_state,
							r.syntax_state,
							r.evidence,
							r.gaps);
		};
		const auto source_feature_population_fields = [](const q::source_feature_population& r)
		{
			return std::tie(r.inventory,
							r.compile_unit,
							r.scope,
							r.file,
							r.source_snapshot,
							r.profile,
							r.universe,
							r.variant,
							r.interpretation,
							r.feature_count,
							r.unbound_feature_count,
							r.feature_ids,
							r.unbound_feature_ids,
							r.entered_file_count,
							r.entered_file_ids,
							r.entered_source_snapshots,
							r.enumeration_observation,
							r.traversal_observation,
							r.entry_observation,
							r.source_binding_observation,
							r.entered_file_observation,
							r.identity_state,
							r.enumeration_state,
							r.membership_state,
							r.traversal_state,
							r.entry_state,
							r.source_state,
							r.entered_file_state,
							r.features,
							r.evidence,
							r.gaps);
		};
		const auto compare =
			[&](const q::source_feature_projection& a, const q::source_feature_projection& z)
		{
			require(a.compile_units_complete == z.compile_units_complete &&
						a.feature_inputs_complete == z.feature_inputs_complete &&
						a.inventory_inputs_complete == z.inventory_inputs_complete &&
						a.unresolved == z.unresolved,
					"shared Source changed scan health or unresolved metadata");
			require(std::ranges::equal(a.environments,
									   z.environments,
									   [&](const auto& x, const auto& y)
									   {
										   return observed_language_environment_fields(x) ==
											   observed_language_environment_fields(y);
									   }),
					"shared Source changed complete language environments");
			require(std::ranges::equal(a.features,
									   z.features,
									   [&](const auto& x, const auto& y)
									   {
										   return observed_source_feature_fields(x) ==
											   observed_source_feature_fields(y);
									   }),
					"shared Source changed complete feature DTOs");
			require(std::ranges::equal(a.populations,
									   z.populations,
									   [&](const auto& x, const auto& y)
									   {
										   return source_feature_population_fields(x) ==
											   source_feature_population_fields(y);
									   }),
					"shared Source changed complete population DTOs");
			require(a.evidence.size() == z.evidence.size(),
					"shared Source changed raw evidence cardinality");
			for (std::size_t i{}; i < a.evidence.size(); ++i)
				require(a.evidence[i].relation_id == z.evidence[i].relation_id &&
							a.evidence[i].original_row().canonical_form() ==
								z.evidence[i].original_row().canonical_form(),
						"shared Source changed a complete raw cell or side channel");
		};
		fixture original;
		q::finite_population_limits shared_limits;
		shared_limits.evidence_ownership = q::projection_evidence_ownership::shared_immutable;
		const auto queries = original.queries(true, true);
		q::projection_resource_usage detached_usage, shared_usage;
		const auto detached = take(q::project_source_features(queries, {}, {}, detached_usage));
		const auto shared =
			take(q::project_source_features(queries, shared_limits, {}, shared_usage));
		compare(detached, shared);
		require(shared_usage.operations < detached_usage.operations,
				"immutable Source did not eliminate actual copies");
		for (const auto& e : detached.evidence)
			require(&e.original_row() == &e.row && !e.row.values.empty(),
					"default Source lost detached public row");
		for (const auto& e : shared.evidence)
		{
			require(e.row.values.empty(), "shared Source copied raw rows before discarding them");
			bool exact{};
			for (const auto& scan : queries.scans)
				if (scan.relation_id == e.relation_id)
					for (const auto& row : q::query_transfer_access::borrow_rows(scan.result))
						exact |= &row == &e.original_row();
			require(exact, "shared Source did not retain the exact immutable row owner");
		}
		require(shared.source_queries && detached.source_queries &&
					shared.source_queries->snapshot_id == queries.snapshot_id &&
					shared.source_queries->scans.size() == queries.scans.size(),
				"shared Source lost source queries");
		for (std::size_t i{}; i < queries.scans.size(); ++i)
			require(shared.source_queries->scans[i].logical_ir.canonical_form() ==
							queries.scans[i].logical_ir.canonical_form() &&
						shared.source_queries->scans[i].result.canonical_form() ==
							queries.scans[i].result.canonical_form(),
					"shared Source lost complete original query side channels");
		q::projection_resource_usage raw_shared_usage, raw_detached_usage;
		const auto raw_shared =
			take(q::project_source_features(original.input(), shared_limits, {}, raw_shared_usage));
		const auto raw_detached =
			take(q::project_source_features(original.input(), {}, {}, raw_detached_usage));
		compare(raw_shared, raw_detached);
		require(raw_shared_usage.operations == raw_detached_usage.operations &&
					raw_shared_usage.retained_bytes_bound ==
						raw_detached_usage.retained_bytes_bound,
				"raw Source changed copy charges in shared mode");
		for (const auto& e : raw_shared.evidence)
			require(&e.original_row() == &e.row && !e.row.values.empty(),
					"raw spans borrowed mutable caller rows");
		{
			q::source_feature_projection survived;
			std::weak_ptr<const q::query_result::data> owner;
			{
				const auto temporary = original.queries(true, true);
				owner = q::query_transfer_access::borrow_evidence_owner(temporary.scans[8].result)
							.owner;
				survived = take(q::project_source_features(temporary, shared_limits));
			}
			compare(detached, survived);
			survived.source_queries.reset();
			require(!owner.expired(), "resetting Source queries destroyed a retained evidence row");
			auto copied = survived;
			survived = {};
			auto moved = std::move(copied);
			compare(detached, moved);
			moved.evidence.clear();
			require(owner.expired(),
					"Source evidence aliases kept expired owners after destruction");
		}
		std::size_t evidence_bytes{};
		for (const auto& e : shared.evidence)
			evidence_bytes += e.original_row().canonical_form().size();
		auto exact = shared_limits;
		exact.maximum_operations = shared_usage.operations;
		exact.maximum_retained_bytes = shared_usage.retained_bytes_bound;
		exact.maximum_evidence_bytes = evidence_bytes;
		q::projection_resource_usage repeated;
		require(bool(q::project_source_features(queries, exact, {}, repeated)) &&
					repeated.operations == shared_usage.operations &&
					repeated.retained_bytes_bound == shared_usage.retained_bytes_bound,
				"shared Source rejected exact work/storage/evidence caps");
		for (int axis{}; axis < 3; ++axis)
		{
			auto under = exact;
			if (axis == 0)
				--under.maximum_operations;
			else if (axis == 1)
				--under.maximum_retained_bytes;
			else
				--under.maximum_evidence_bytes;
			repeated = {1, 1};
			require(!q::project_source_features(queries, under, {}, repeated) &&
						!repeated.operations && !repeated.retained_bytes_bound,
					"shared Source bypassed one-under cap or retained failed usage");
		}
		std::size_t calls{};
		auto counted = shared_limits;
		counted.cancelled = [&]
		{
			++calls;
			return false;
		};
		require(bool(q::project_source_features(queries, counted)),
				"shared Source callback census");
		std::size_t visited{};
		std::stop_source stopped;
		auto interrupted = shared_limits;
		interrupted.cancelled = [&]
		{
			if (++visited == calls / 2U)
				stopped.request_stop();
			return false;
		};
		require(!q::project_source_features(queries, interrupted, stopped.get_token(), repeated) &&
					!repeated.operations && !repeated.retained_bytes_bound && visited == calls / 2U,
				"shared Source real stop failed to revoke usage");
		require(bool(q::project_source_features(queries, shared_limits, {}, repeated)) &&
					repeated.operations == shared_usage.operations,
				"shared Source failed fresh retry");
		stopped.request_stop();
		require(
			!q::project_source_features(queries, shared_limits, stopped.get_token(), repeated) &&
				!repeated.operations && !repeated.retained_bytes_bound,
			"shared Source ignored pre-stop");
		{
			auto bad = original;
			bad.rows[8].front().multiplicity = 0;
			require(!q::project_source_features(bad.queries(), shared_limits),
					"shared Source bypassed malformed original annotations");
			bad = original;
			bad.rows[8].front().values.erase("output.kind");
			require(!q::project_source_features(bad.queries(), shared_limits),
					"shared Source bypassed missing original field");
			bad = original;
			bad.rows[8].push_back(bad.rows[8].front());
			const auto duplicate = bad.queries(true, true);
			compare(take(q::project_source_features(duplicate)),
					take(q::project_source_features(duplicate, shared_limits)));
			bad = original;
			bad.rows[8].front().presence.universe = "foreign-world";
			bad.rows[8].front().contributor_edges.front().condition = bad.rows[8].front().presence;
			const auto foreign = bad.queries(true, true);
			compare(take(q::project_source_features(foreign)),
					take(q::project_source_features(foreign, shared_limits)));
		}
		auto invalid_mode = shared_limits;
		invalid_mode.evidence_ownership = static_cast<q::projection_evidence_ownership>(255U);
		require(!q::project_source_features(queries, invalid_mode),
				"unsupported evidence ownership mode accepted");
	}
} // namespace

namespace
{
	template <class T>
	void membership_append(std::string& wire, const T& value);
	void membership_append(std::string& wire, const q::query_unresolved& value)
	{
		membership_append(wire, std::tie(value.code, value.subject, value.detail));
	}
	template <class T>
	void membership_append(std::string& wire, const T& value)
	{
		if constexpr (std::is_convertible_v<T, std::string_view>)
		{
			const std::string_view text{value};
			wire += std::to_string(text.size()) + ":";
			wire.append(text);
		}
		else if constexpr (std::is_integral_v<T>)
			wire += std::to_string(value) + ";";
		else if constexpr (std::is_enum_v<T>)
			membership_append(wire, static_cast<std::underlying_type_t<T>>(value));
		else if constexpr (requires { value.has_value(); })
		{
			membership_append(wire, value.has_value());
			if (value)
				membership_append(wire, *value);
		}
		else if constexpr (requires { std::tuple_size<T>::value; })
			std::apply(
				[&](const auto&... parts)
				{
					(membership_append(wire, parts), ...);
				},
				value);
		else if constexpr (requires {
							   value.begin();
							   value.end();
							   value.size();
						   })
		{
			membership_append(wire, value.size());
			for (const auto& member : value)
				membership_append(wire, member);
		}
		else
			static_assert(!sizeof(T), "missing complete Source oracle field");
	}
	std::string membership_projection_form(const q::source_feature_projection& output,
										   bool include_queries = true)
	{
		const auto observed_language_environment_fields =
			[](const q::observed_language_environment& r)
		{
			return std::tie(r.compile_unit,
							r.universe,
							r.variant,
							r.interpretation,
							r.profile,
							r.observation_state,
							r.freestanding,
							r.state,
							r.evidence,
							r.gaps);
		};
		const auto observed_source_feature_fields = [](const q::observed_source_feature& r)
		{
			return std::tie(r.feature,
							r.compile_unit,
							r.universe,
							r.variant,
							r.interpretation,
							r.profile,
							r.ordinal,
							r.original_node_ordinal,
							r.compiler_kind,
							r.feature_class,
							r.kind,
							r.origin,
							r.evaluation,
							r.observation_state,
							r.file,
							r.source_snapshot,
							r.source_span,
							r.source_none_reason,
							r.is_implicit,
							r.is_system,
							r.declaration,
							r.context_declaration,
							r.subject_entity,
							r.call_target,
							r.subject_type,
							r.syntax,
							r.reference_kind,
							r.type_role,
							r.source_binding_state,
							r.declaration_binding_state,
							r.context_binding_state,
							r.entity_binding_state,
							r.call_binding_state,
							r.type_binding_state,
							r.identity_state,
							r.observation,
							r.source_state,
							r.declaration_state,
							r.context_state,
							r.entity_state,
							r.call_state,
							r.type_state,
							r.syntax_state,
							r.evidence,
							r.gaps);
		};
		const auto source_feature_population_fields = [](const q::source_feature_population& r)
		{
			return std::tie(r.inventory,
							r.compile_unit,
							r.scope,
							r.file,
							r.source_snapshot,
							r.profile,
							r.universe,
							r.variant,
							r.interpretation,
							r.feature_count,
							r.unbound_feature_count,
							r.feature_ids,
							r.unbound_feature_ids,
							r.entered_file_count,
							r.entered_file_ids,
							r.entered_source_snapshots,
							r.enumeration_observation,
							r.traversal_observation,
							r.entry_observation,
							r.source_binding_observation,
							r.entered_file_observation,
							r.identity_state,
							r.enumeration_state,
							r.membership_state,
							r.traversal_state,
							r.entry_state,
							r.source_state,
							r.entered_file_state,
							r.features,
							r.evidence,
							r.gaps);
		};

		std::string wire;
		membership_append(wire,
						  std::tie(output.compile_units_complete,
								   output.feature_inputs_complete,
								   output.inventory_inputs_complete,
								   output.unresolved));
		membership_append(wire, output.environments.size());
		for (const auto& value : output.environments)
			membership_append(wire, observed_language_environment_fields(value));
		membership_append(wire, output.features.size());
		for (const auto& value : output.features)
			membership_append(wire, observed_source_feature_fields(value));
		membership_append(wire, output.populations.size());
		for (const auto& value : output.populations)
			membership_append(wire, source_feature_population_fields(value));
		membership_append(wire, output.evidence.size());
		for (const auto& value : output.evidence)
			membership_append(wire,
							  std::tuple{value.relation_id, value.original_row().canonical_form()});
		if (include_queries)
		{
			membership_append(wire, output.source_queries.has_value());
			if (output.source_queries)
			{
				membership_append(wire, output.source_queries->snapshot_id);
				membership_append(wire, output.source_queries->scans.size());
				for (const auto& scan : output.source_queries->scans)
					membership_append(wire,
									  std::tuple{scan.relation_id,
												 scan.logical_ir.canonical_form(),
												 scan.result.canonical_form()});
			}
		}
		return wire;
	}
} // namespace

namespace
{
	std::uint64_t membership_fixture_hash(std::string_view id)
	{
		std::uint64_t hash = 14695981039346656037ULL;
		for (const char byte : id)
		{
			hash ^= static_cast<unsigned char>(byte);
			hash *= 1099511628211ULL;
		}
		return hash ? hash : 1U;
	}
	fixture colliding_members()
	{
		fixture original;
		std::vector<std::string> ids;
		for (std::size_t candidate{}; candidate < 200000U && ids.size() < 96U; ++candidate)
		{
			auto id =
				"feature:membership:\xc3\xa9:" + std::to_string(100000U + candidate) + ":01234567";
			if ((membership_fixture_hash(id) & 511U) == 511U)
				ids.push_back(std::move(id));
		}
		require(ids.size() == 96U, "fixture did not find real same-bucket original IDs");
		require(std::ranges::all_of(ids,
									[&](const auto& id)
									{
										return id.size() == ids.front().size() &&
											id.ends_with(":01234567");
									}),
				"fixture did not retain equal-length full-ID suffix collisions");
		const auto feature = original.rows[8].front();
		original.rows[8].clear();
		for (std::size_t index{}; index < ids.size(); ++index)
		{
			auto row = feature;
			set(row, "feature", txt(ids[index]));
			set(row, "ordinal", num(index));
			set(row, "original_node_ordinal", num(index));
			original.rows[8].push_back(std::move(row));
		}
		for (auto& inventory : original.rows[9])
		{
			set(inventory, "feature_ids", symbol_values(ids));
			set(inventory, "feature_count", num(ids.size()));
		}
		return original;
	}
	void collision_membership_controls()
	{
		const auto original = colliding_members();
		const auto input = original.queries(true, true);
		q::projection_resource_usage measured;
		const auto detached = take(q::project_source_features(input, {}, {}, measured));
		const auto full = membership_projection_form(detached);
		std::vector<std::string> expected;
		for (const auto& row : original.rows[8])
			expected.push_back(std::get<std::string>(*row.values.at("output.feature").value));
		std::ranges::sort(expected);
		require(std::ranges::adjacent_find(expected) == expected.end() &&
					detached.features.size() == expected.size() &&
					detached.populations.size() == 2U,
				"colliding fixture lost distinct originals or introduced duplicate bytes");
		for (const auto& population : detached.populations)
		{
			require(population.feature_ids == expected &&
						population.features.size() == expected.size() &&
						population.membership_state == state::complete,
					"same-bucket membership changed the independent sorted-set census");
			for (std::size_t index{}; index < expected.size(); ++index)
				require(detached.features[population.features[index]].feature == expected[index],
						"membership probes changed ordered feature references");
		}
		const auto raw = take(q::project_source_features(original.input()));
		require(!raw.source_queries &&
					membership_projection_form(raw, false) ==
						membership_projection_form(detached, false),
				"raw membership changed complete DTO/raw evidence");
		q::finite_population_limits shared_limits;
		shared_limits.evidence_ownership = q::projection_evidence_ownership::shared_immutable;
		q::source_feature_projection survived;
		{
			const auto temporary = original.queries(true, true);
			survived = take(q::project_source_features(temporary, shared_limits));
		}
		require(membership_projection_form(survived) == full,
				"shared membership changed any DTO, complete raw row or source query");
		survived.source_queries.reset();
		auto copied = survived;
		survived = {};
		auto moved = std::move(copied);
		require(membership_projection_form(moved, false) ==
					membership_projection_form(detached, false),
				"membership depended on expired bucket/original-query owners");
		for (const auto& evidence : moved.evidence)
			require(evidence.row.values.empty() && bool(evidence.original_row().validate()),
					"shared evidence copied or lost the independently retained original row");
		for (const std::string_view mutation : {"duplicate", "missing", "foreign"})
		{
			auto changed = original;
			auto ids = expected;
			if (mutation == "duplicate")
				ids.push_back(std::string{ids.front()});
			if (mutation == "missing")
				ids.pop_back();
			if (mutation == "foreign")
			{
				ids.back().front() = '!';
			}
			for (auto& inventory : changed.rows[9])
			{
				set(inventory, "feature_ids", symbol_values(ids));
				set(inventory, "feature_count", num(ids.size()));
			}
			q::projection_resource_usage usage{1U, 1U};
			const auto result = q::project_source_features(changed.queries(), {}, {}, usage);
			if (mutation == "duplicate")
			{
				require(
					!result && !usage.operations && !usage.retained_bytes_bound,
					"equal bytes from separately owned cells bypassed full duplicate admission");
			}
			else
			{
				require(
					bool(result) && result->features.size() == expected.size() &&
						std::ranges::all_of(result->populations,
											[](const auto& population)
											{
												return population.membership_state !=
													state::complete;
											}),
					"missing/foreign membership erased originals or promoted incomplete census");
			}
		}
		for (const bool storage : {false, true})
			for (const bool one_under : {false, true})
			{
				q::finite_population_limits limits;
				if (storage)
					limits.maximum_retained_bytes =
						measured.retained_bytes_bound - static_cast<std::size_t>(one_under);
				else
					limits.maximum_operations =
						measured.operations - static_cast<std::size_t>(one_under);
				q::projection_resource_usage usage{1U, 1U};
				const auto result = q::project_source_features(input, limits, {}, usage);
				require(static_cast<bool>(result) == !one_under,
						"colliding membership ignored exact/one-under work or storage");
				if (result)
					require(membership_projection_form(*result) == full &&
								usage.operations == measured.operations &&
								usage.retained_bytes_bound == measured.retained_bytes_bound,
							"bounded membership changed complete output or actual usage");
				else
					require(!usage.operations && !usage.retained_bytes_bound,
							"failed membership exposed successful usage");
			}
		std::size_t checkpoints{};
		q::finite_population_limits counted;
		counted.cancelled = [&]
		{
			++checkpoints;
			return false;
		};
		require(bool(q::project_source_features(input, counted)), "collision callback census");
		require(checkpoints > 100U, "colliding membership did not perform bounded actual visits");
		for (const std::size_t stop_at : {std::size_t{1U}, checkpoints / 2U, checkpoints - 1U})
		{
			std::size_t visited{};
			std::stop_source stop;
			q::finite_population_limits limits;
			limits.cancelled = [&]
			{
				if (++visited == stop_at)
					stop.request_stop();
				return false;
			};
			q::projection_resource_usage usage{1U, 1U};
			const auto result = q::project_source_features(input, limits, stop.get_token(), usage);
			require(!result && result.error().code == "sdk.source-feature-cancelled" &&
						visited == stop_at && !usage.operations && !usage.retained_bytes_bound,
					"real collision-path stop bypassed checkpoints or retained partial usage");
		}
		q::projection_resource_usage retried;
		const auto retry = take(q::project_source_features(input, {}, {}, retried));
		require(membership_projection_form(retry) == full &&
					retried.operations == measured.operations &&
					retried.retained_bytes_bound == measured.retained_bytes_bound,
				"fresh retry inherited discarded membership buckets or partial output");
		// A sampled bucket never authorizes unseen leading bytes. Ordinary row
		// admission still validates every member in every encoded original set.
		for (const auto& invalid :
			 {std::string{"bad\0:01234567", 13U}, std::string(1U, char(0xff)) + ":01234567"})
			for (const auto& [group, field] :
				 std::array<std::pair<std::size_t, std::string_view>, 6U>{
					 {{5U, "declarations"},
					  {5U, "physical_definition_ids"},
					  {9U, "feature_ids"},
					  {9U, "unbound_feature_ids"},
					  {9U, "entered_file_ids"},
					  {9U, "entered_source_snapshots"}}})
			{
				auto bad = original;
				set(bad.rows[group].front(), field, symbol_values({invalid}));
				for (unsigned ownership{}; ownership < 3U; ++ownership)
				{
					q::projection_resource_usage usage{1U, 1U};
					const auto result = ownership == 0U
						? q::project_source_features(bad.input(), {}, {}, usage)
						: q::project_source_features(bad.queries(),
													 ownership == 1U ? q::finite_population_limits{}
																	 : shared_limits,
													 {},
													 usage);
					require(
						!result && !usage.operations && !usage.retained_bytes_bound,
						"unread NUL/UTF-8 member prefix bypassed ordinary full original admission");
				}
			}
	}
} // namespace

namespace
{
	fixture sampled_supporting_fixture(std::string_view prefix)
	{
		const auto original_id = [&](std::string_view group, char variant)
		{
			if (prefix == "short")
				return std::string(1U, variant);
			return std::string{group} + ":" + std::string{prefix} + ":" + variant + ":01234567";
		};
		std::array<fixture, 2U> members;
		std::array<std::map<std::string, std::string, std::less<>>, 2U> ids;
		for (std::size_t member{}; member < members.size(); ++member)
		{
			const auto variant = member ? 'z' : 'a';
			for (const auto& [short_id, group] :
				 std::array<std::pair<std::string_view, std::string_view>, 8U>{
					 {{"F", "file"},
					  {"P", "snapshot"},
					  {"S", "span"},
					  {"E", "entity"},
					  {"D", "declaration"},
					  {"T", "type"},
					  {"N", "node"},
					  {"X", "feature"}}})
				ids[member].emplace(short_id, original_id(group, variant));
			for (auto& group : members[member].rows)
				for (auto& row : group)
					for (auto& [name, cell] : row.values)
					{
						(void)name;
						if (!cell.value)
							continue;
						if (auto* value = std::get_if<std::string>(&*cell.value))
							if (const auto changed = ids[member].find(*value);
								changed != ids[member].end())
								*value = changed->second;
					}
			set(members[member].rows[8].front(), "ordinal", num(member));
			set(members[member].rows[8].front(), "original_node_ordinal", num(member));
		}
		fixture result = std::move(members.front());
		for (const std::size_t group : {1U, 2U, 3U, 4U, 6U, 7U, 8U})
			result.rows[group].push_back(std::move(members[1U].rows[group].front()));
		set(result.rows[5].front(), "declaration_count", num(2U));
		set(result.rows[5].front(),
			"declarations",
			symbol_values({ids[0U].at("D"), ids[1U].at("D")}));
		set(result.rows[5].front(),
			"physical_definition_profile",
			txt("clang22-original-physical-definitions/1"));
		set(result.rows[5].front(), "physical_definition_state", txt("complete"));
		set(result.rows[5].front(), "physical_definition_count", num(2U));
		set(result.rows[5].front(),
			"physical_definition_ids",
			symbol_values({ids[0U].at("D"), ids[1U].at("D")}));
		set(result.rows[9].front(), "feature_count", num(2U));
		set(result.rows[9].front(),
			"feature_ids",
			symbol_values({ids[0U].at("X"), ids[1U].at("X")}));
		set(result.rows[9].front(), "entered_file_count", num(2U));
		set(result.rows[9].front(),
			"entered_file_ids",
			symbol_values({ids[0U].at("F"), ids[1U].at("F")}));
		set(result.rows[9].front(),
			"entered_source_snapshots",
			symbol_values({ids[0U].at("P"), ids[1U].at("P")}));
		set(result.rows[9][1U], "feature_ids", symbol_values({ids[0U].at("X")}));
		set(members[1U].rows[9][1U], "feature_ids", symbol_values({ids[1U].at("X")}));
		set(members[1U].rows[9][1U], "inventory", txt("FI:second"));
		result.rows[9].push_back(std::move(members[1U].rows[9][1U]));
		return result;
	}

	void sampled_supporting_lookup_controls()
	{
		const auto identifier = [](const q::annotated_row& row, std::string_view column)
		{
			return std::get<std::string>(*row.values.at("output." + std::string{column}).value);
		};
		constexpr std::array<std::pair<std::size_t, std::string_view>, 7U> groups{
			{{1U, "snapshot"},
			 {2U, "span"},
			 {3U, "entity"},
			 {4U, "declaration"},
			 {6U, "type"},
			 {7U, "node"},
			 {8U, "feature"}}};
		for (const auto& prefix :
			 {std::string{"short"}, std::string{}, std::string{"日本語"}, std::string(1024U, 'p')})
		{
			auto original = sampled_supporting_fixture(prefix);
			for (const auto& [group, column] : groups)
			{
				const auto left = identifier(original.rows[group][0U], column);
				const auto right = identifier(original.rows[group][1U], column);
				require(left != right && left.size() == right.size() &&
							(left.size() <= 8U ||
							 left.substr(left.size() - 8U) == right.substr(right.size() - 8U)),
						"sampled lookup fixture lacks distinct full IDs in the same suffix bucket");
				if (group == 8U)
					continue;
				for (const unsigned axis : {0U, 1U, 2U})
				{
					auto foreign = original.rows[group].front();
					if (axis == 0U)
					{
						foreign.presence.universe = "foreign:universe";
						foreign.contributor_edges.front().condition.universe =
							foreign.presence.universe;
					}
					else if (axis == 1U)
					{
						foreign.presence.fragments = {"foreign:variant"};
						foreign.contributor_edges.front().condition.fragments =
							foreign.presence.fragments;
					}
					else
					{
						foreign.interpretation = "foreign:interpretation";
						foreign.contributor_edges.front().interpretation = foreign.interpretation;
					}
					if (group == 1U)
						set(foreign, "size", num(8U));
					else if (group == 2U)
						set(foreign, "begin", num(1U));
					else if (group == 3U)
						set(foreign, "kind", txt("variable"));
					else if (group == 4U)
						set(foreign, "source", txt("foreign:source"));
					else if (group == 6U)
						set(foreign, "constructor", txt("pointer"));
					else
						set(foreign, "kind", txt("DeclRefExpr"));
					original.rows[group].push_back(std::move(foreign));
				}
			}
			const auto input = original.queries(true, true);
			q::projection_resource_usage measured;
			const auto detached = take(q::project_source_features(input, {}, {}, measured));
			require(detached.features.size() == 2U && detached.populations.size() == 3U &&
						std::ranges::all_of(detached.features,
											[](const auto& feature)
											{
												return feature.identity_state == state::complete &&
													feature.source_state == state::complete &&
													feature.context_state == state::complete &&
													feature.entity_state == state::complete &&
													feature.call_state == state::complete &&
													feature.type_state == state::complete &&
													feature.syntax_state == state::complete;
											}) &&
						std::ranges::all_of(detached.populations,
											[](const auto& population)
											{
												return population.membership_state ==
													state::complete &&
													population.entry_state == state::complete &&
													population.source_state == state::complete;
											}),
					"sample collisions or foreign worlds changed fully bound original tuples");
			for (const auto& feature : detached.features)
				for (const auto& [group, column, expected] :
					 std::array<std::tuple<std::size_t, std::string_view, std::string_view>, 6U>{
						 {{1U, "snapshot", feature.source_snapshot},
						  {2U, "span", feature.source_span},
						  {3U, "entity", feature.subject_entity},
						  {4U, "declaration", feature.context_declaration},
						  {6U, "type", feature.subject_type},
						  {7U, "node", feature.syntax}}})
				{
					bool found{};
					for (const auto index : feature.evidence)
					{
						const auto& evidence = detached.evidence.at(index);
						if (evidence.relation_id != names[group])
							continue;
						const auto& row = evidence.original_row();
						require(identifier(row, column) == expected &&
									row.presence.universe == feature.universe &&
									row.presence.fragments ==
										std::vector<std::string>{feature.variant} &&
									row.interpretation == feature.interpretation,
								"sample collision borrowed another full ID or World as supporting "
								"evidence");
						found = true;
					}
					require(found,
							"sample collision lost the requested original supporting evidence");
				}
			for (const auto& population : detached.populations)
			{
				std::vector<std::string> members;
				for (const auto index : population.features)
				{
					const auto& feature = detached.features.at(index);
					members.push_back(feature.feature);
					require(
						std::ranges::find(population.feature_ids, feature.feature) !=
								population.feature_ids.end() &&
							feature.universe == population.universe &&
							feature.variant == population.variant &&
							feature.interpretation == population.interpretation &&
							(population.scope == "translation_unit" ||
							 (feature.file == population.file &&
							  feature.source_snapshot == population.source_snapshot)),
						"sampled feature index borrowed an unrelated population member or World");
				}
				std::ranges::sort(members);
				require(
					members == population.feature_ids,
					"sampled feature index duplicated or omitted an original population member");
			}
			const auto complete = membership_projection_form(detached);
			const auto raw = take(q::project_source_features(original.input()));
			require(membership_projection_form(raw, false) ==
						membership_projection_form(detached, false),
					"sample collisions changed raw/default complete DTO or original evidence");
			q::finite_population_limits shared_limits;
			shared_limits.evidence_ownership = q::projection_evidence_ownership::shared_immutable;
			q::source_feature_projection survived;
			{
				const auto temporary = original.queries(true, true);
				survived = take(q::project_source_features(temporary, shared_limits));
			}
			require(
				membership_projection_form(survived) == complete,
				"sample collisions changed shared DTO/evidence/source queries after owner expiry");
			survived.source_queries.reset();
			auto copied = survived;
			survived = {};
			auto moved = std::move(copied);
			require(membership_projection_form(moved, false) ==
						membership_projection_form(detached, false),
					"sampled lookup depended on expired query, copied or moved projection owners");
			for (auto& group : original.rows)
				std::ranges::reverse(group);
			const auto reordered_input = original.queries(true, true);
			const auto reordered = take(q::project_source_features(reordered_input));
			require(
				membership_projection_form(reordered, false) ==
					membership_projection_form(detached, false),
				"sampled buckets changed deterministic complete DTO or canonical evidence order");
			require(reordered.source_queries &&
						reordered.source_queries->scans.size() == reordered_input.scans.size(),
					"sampled lookup lost reordered original source queries");
			for (std::size_t i{}; i < reordered_input.scans.size(); ++i)
				require(reordered.source_queries->scans[i].result.canonical_form() ==
							reordered_input.scans[i].result.canonical_form(),
						"sampled lookup altered complete reordered original queries");
			auto missing = sampled_supporting_fixture(prefix);
			const auto absent = prefix == "short"
				? std::string{"m"}
				: std::string{"feature:"} + prefix + ":m:01234567";
			for (auto& inventory : missing.rows[9])
			{
				const bool unit = identifier(inventory, "scope") == "translation_unit";
				set(inventory,
					"feature_ids",
					symbol_values(
						unit ? std::vector<std::string>{absent,
														identifier(missing.rows[8][1U], "feature")}
							 : std::vector<std::string>{absent}));
			}
			const auto incomplete = take(q::project_source_features(missing.queries(true, true)));
			require(incomplete.features.size() == 2U &&
						std::ranges::all_of(incomplete.populations,
											[](const auto& population)
											{
												return population.membership_state !=
													state::complete;
											}),
					"unlisted full ID borrowed membership from a same-suffix original feature");
			for (const auto& population : incomplete.populations)
			{
				require(
					population.features.size() ==
						(population.scope == "translation_unit" ? 1U : 0U),
					"absent sampled feature key retained an unrelated original feature reference");
				for (const auto index : population.features)
					require(std::ranges::find(population.feature_ids,
											  incomplete.features.at(index).feature) !=
								population.feature_ids.end(),
							"same-suffix absent key borrowed an unlisted feature reference");
			}
			for (const bool storage : {false, true})
				for (const bool one_under : {false, true})
				{
					q::finite_population_limits limits;
					if (storage)
						limits.maximum_retained_bytes =
							measured.retained_bytes_bound - static_cast<std::size_t>(one_under);
					else
						limits.maximum_operations =
							measured.operations - static_cast<std::size_t>(one_under);
					q::projection_resource_usage usage{1U, 1U};
					const auto bounded = q::project_source_features(input, limits, {}, usage);
					require(static_cast<bool>(bounded) == !one_under,
							"sample collision lookup ignored exact/one-under work or storage");
					if (bounded)
						require(membership_projection_form(*bounded) == complete &&
									usage.operations == measured.operations &&
									usage.retained_bytes_bound == measured.retained_bytes_bound,
								"bounded sampled lookup changed complete DTO or measured usage");
					else
						require(!usage.operations && !usage.retained_bytes_bound,
								"failed sampled lookup published successful usage");
				}
			std::size_t checkpoints{};
			q::finite_population_limits counted;
			counted.cancelled = [&]
			{
				++checkpoints;
				return false;
			};
			require(bool(q::project_source_features(input, counted)) && checkpoints > 2U,
					"sample collision checkpoint census failed");
			std::size_t visited{};
			std::stop_source stop;
			q::finite_population_limits interrupted;
			interrupted.cancelled = [&]
			{
				if (++visited == checkpoints / 2U)
					stop.request_stop();
				return false;
			};
			q::projection_resource_usage failed{1U, 1U};
			const auto stopped =
				q::project_source_features(input, interrupted, stop.get_token(), failed);
			require(!stopped && stopped.error().code == "sdk.source-feature-cancelled" &&
						visited == checkpoints / 2U && !failed.operations &&
						!failed.retained_bytes_bound,
					"real sampled-lookup stop escaped checkpoints or published failed usage");
			q::projection_resource_usage retried;
			const auto retry = take(q::project_source_features(input, {}, {}, retried));
			require(membership_projection_form(retry) == complete &&
						retried.operations == measured.operations &&
						retried.retained_bytes_bound == measured.retained_bytes_bound,
					"sampled lookup fresh retry inherited partial buckets or failed output");
		}
	}
} // namespace

namespace
{
	void sampled_member_inventory_controls()
	{
		auto original = sampled_supporting_fixture(std::string(128U, 'p') + "日本語");
		original.rows[9].resize(1U);
		std::vector<std::string> ids;
		for (auto& feature : original.rows[8])
		{
			ids.push_back(std::get<std::string>(*feature.values.at("output.feature").value));
			set(feature, "source_binding_state", txt("unknown"));
			for (const auto field : {"file", "source_snapshot", "source"})
				set(feature, field, detached_cell::absent({}));
		}
		std::ranges::sort(ids);
		set(original.rows[9].front(), "unbound_feature_ids", symbol_values(ids));
		set(original.rows[9].front(), "unbound_feature_count", num(ids.size()));
		set(original.rows[9].front(), "source_binding_state", txt("partial"));
		const auto input = original.queries(true, true);
		q::projection_resource_usage measured;
		const auto detached = take(q::project_source_features(input, {}, {}, measured));
		require(detached.features.size() == 2U && detached.populations.size() == 1U &&
					detached.populations.front().membership_state == state::complete &&
					detached.populations.front().unbound_feature_ids == ids &&
					detached.populations.front().source_state != state::complete,
				"same-suffix unbound membership merged full IDs or fabricated complete source "
				"bindings");
		const auto full = membership_projection_form(detached);
		q::finite_population_limits shared;
		shared.evidence_ownership = q::projection_evidence_ownership::shared_immutable;
		require(
			membership_projection_form(take(q::project_source_features(input, shared))) == full &&
				membership_projection_form(take(q::project_source_features(original.input())),
										   false) == membership_projection_form(detached, false),
			"same-suffix unbound sets changed complete DTO, evidence or source query ownership");
		for (const bool duplicate : {false, true})
		{
			auto changed = original;
			auto values = ids;
			if (duplicate)
				values.push_back(std::string{values.front()});
			else
				values.front().front() = '!';
			set(changed.rows[9].front(), "unbound_feature_ids", symbol_values(values));
			q::projection_resource_usage usage{1U, 1U};
			const auto output = q::project_source_features(changed.queries(), {}, {}, usage);
			if (duplicate)
				require(!output && !usage.operations && !usage.retained_bytes_bound,
						"duplicate full unbound ID bypassed original set admission");
			else
				require(bool(output) && output->features.size() == 2U &&
							output->populations.front().membership_state == state::conflicting,
						"same-suffix foreign unbound ID borrowed original subset membership");
		}
		q::projection_resource_usage retried;
		require(membership_projection_form(
					take(q::project_source_features(input, {}, {}, retried))) == full &&
					retried.operations == measured.operations &&
					retried.retained_bytes_bound == measured.retained_bytes_bound,
				"unbound membership fresh retry inherited foreign or failed sets");
	}

	fixture pooled_world_originals()
	{
		const fixture original;
		fixture pooled;
		for (auto& group : pooled.rows)
			group.clear();
		for (std::size_t at{}; at < 12U; ++at)
			for (std::size_t group{}; group < original.rows.size(); ++group)
				for (const auto& saved : original.rows[group])
				{
					auto row = saved;
					row.presence.universe =
						"universe:" + std::string(1U, at % 6U < 3U ? 'a' : 'z') + ":same-end";
					row.presence.fragments = {
						"variant:" + std::string(1U, static_cast<char>('a' + at % 3U)) +
						":same-end"};
					row.interpretation =
						"interpretation:" + std::string(1U, at < 6U ? 'a' : 'z') + ":same-end";
					row.contributor_edges.front().condition = row.presence;
					row.contributor_edges.front().interpretation = row.interpretation;
					if (group == 3U)
						set(row,
							"provider_local_key",
							detached_cell::bytes({static_cast<std::byte>(at)}));
					pooled.rows[group].push_back(std::move(row));
				}
		return pooled;
	}
	void pooled_world_lookup_controls()
	{
		auto originals = pooled_world_originals();
		const auto input = originals.queries(true, true);
		q::projection_resource_usage measured;
		const auto output = take(q::project_source_features(input, {}, {}, measured));
		require(output.environments.size() == 12U && output.features.size() == 12U &&
					output.populations.size() == 24U,
				"same-ID sampled World collisions changed full observation cardinality");
		for (const auto& feature : output.features)
		{
			require(feature.identity_state == state::complete &&
						feature.source_state == state::complete &&
						feature.context_state == state::complete &&
						feature.entity_state == state::complete &&
						feature.call_state == state::complete &&
						feature.type_state == state::complete &&
						feature.syntax_state == state::complete,
					"same-ID World collisions borrowed conflicting original facts");
			for (const auto reference : feature.evidence)
			{
				const auto& row = output.evidence.at(reference).original_row();
				require(row.presence.universe == feature.universe &&
							row.presence.fragments == std::vector<std::string>{feature.variant} &&
							row.interpretation == feature.interpretation,
						"a supporting or unit reference crossed a complete original World");
			}
		}
		for (const auto& population : output.populations)
		{
			require(population.features.size() == 1U &&
						population.membership_state == state::complete,
					"pooled World original inventory lost an exact member");
			const auto& feature = output.features.at(population.features.front());
			require(feature.universe == population.universe &&
						feature.variant == population.variant &&
						feature.interpretation == population.interpretation,
					"feature-index growth moved a World identity away from its original member");
		}
		const auto full = membership_projection_form(output);
		require(membership_projection_form(take(q::project_source_features(originals.input())),
										   false) == membership_projection_form(output, false),
				"raw and admitted World originals changed complete DTO or evidence fields");
		q::finite_population_limits shared;
		shared.evidence_ownership = q::projection_evidence_ownership::shared_immutable;
		q::source_feature_projection survived;
		{
			const auto donor = originals.queries(true, true);
			survived = take(q::project_source_features(donor, shared));
		}
		require(membership_projection_form(survived) == full,
				"private World contexts depended on expired query owners");
		survived.source_queries.reset();
		auto copied = survived;
		survived = {};
		auto moved = std::move(copied);
		require(membership_projection_form(moved, false) ==
					membership_projection_form(output, false),
				"copy or move retained a temporary World context");
		for (auto& group : originals.rows)
			std::ranges::reverse(group);
		require(membership_projection_form(
					take(q::project_source_features(originals.queries(true, true))), false) ==
					membership_projection_form(output, false),
				"World bucket order changed canonical observation or evidence order");
		for (const bool storage : {false, true})
			for (const bool under : {false, true})
			{
				q::finite_population_limits limits;
				if (storage)
					limits.maximum_retained_bytes =
						measured.retained_bytes_bound - static_cast<std::size_t>(under);
				else
					limits.maximum_operations =
						measured.operations - static_cast<std::size_t>(under);
				q::projection_resource_usage usage{1U, 1U};
				const auto bounded = q::project_source_features(input, limits, {}, usage);
				require(static_cast<bool>(bounded) == !under,
						"pooled World lookup ignored exact or one-under resource bounds");
				if (bounded)
					require(membership_projection_form(*bounded) == full &&
								usage.operations == measured.operations &&
								usage.retained_bytes_bound == measured.retained_bytes_bound,
							"bounded World admission changed complete output or usage");
				else
					require(!usage.operations && !usage.retained_bytes_bound,
							"failed World admission published successful usage");
			}
		std::size_t checkpoints{};
		q::finite_population_limits counted;
		counted.cancelled = [&]
		{
			++checkpoints;
			return false;
		};
		require(bool(q::project_source_features(input, counted)) && checkpoints > 4U,
				"pooled World checkpoint census failed");
		std::stop_source stop;
		std::size_t visited{};
		q::finite_population_limits interrupted;
		interrupted.cancelled = [&]
		{
			if (++visited == checkpoints * 3U / 4U)
				stop.request_stop();
			return false;
		};
		q::projection_resource_usage failed{1U, 1U};
		const auto stopped =
			q::project_source_features(input, interrupted, stop.get_token(), failed);
		require(!stopped && stopped.error().code == "sdk.source-feature-cancelled" &&
					!failed.operations && !failed.retained_bytes_bound,
				"real stop during pooled World admission escaped bounded checkpoints");
		require(membership_projection_form(take(q::project_source_features(input))) == full,
				"fresh projection inherited a partial World registry");
		{
			auto missing = pooled_world_originals();
			const auto removed = missing.rows[3].at(7U);
			missing.rows[3].erase(missing.rows[3].begin() + 7U);
			const auto incomplete = take(q::project_source_features(missing.input()));
			for (const auto& feature : incomplete.features)
			{
				const bool foreign = feature.universe == removed.presence.universe &&
					feature.variant == removed.presence.fragments.front() &&
					feature.interpretation == removed.interpretation;
				require(feature.entity_state == (foreign ? state::unknown : state::complete) &&
							feature.call_state == (foreign ? state::unknown : state::complete),
						"absent exact World borrowed a same-ID entity from another pooled World");
			}
		}
		for (const bool nul : {false, true})
		{
			auto malformed = pooled_world_originals();
			auto unused = malformed.rows[2].front();
			set(unused, "span", txt("unreferenced-span"));
			unused.presence.universe +=
				nul ? std::string(1U, '\0') : std::string(1U, static_cast<char>(0xc3));
			unused.contributor_edges.front().condition = unused.presence;
			malformed.rows[2].push_back(std::move(unused));
			q::projection_resource_usage usage{1U, 1U};
			require(!q::project_source_features(malformed.input(), {}, {}, usage) &&
						!usage.operations && !usage.retained_bytes_bound,
					"unused original World bytes bypassed full raw admission");
		}
	}
} // namespace

int main()
{
	sampled_member_inventory_controls();
	pooled_world_lookup_controls();
	sampled_supporting_lookup_controls();
	collision_membership_controls();
	immutable_evidence_controls();
	{
		fixture sized;
		query_copy_controls::projection(
			sized.rows,
			names,
			[&]
			{
				return sized.queries(true, true);
			},
			[](const auto& input, auto limits, auto& usage)
			{
				return q::project_source_features(input, limits, {}, usage);
			},
			require);
		std::size_t calls{};
		q::finite_population_limits measured;
		measured.cancelled = [&]
		{
			++calls;
			return false;
		};
		const auto admitted = sized.queries(true, true);
		require(bool(q::project_source_features(admitted, measured)),
				"immutable sizing current callback census");
		q::finite_population_limits interrupted;
		std::size_t visited{};
		interrupted.cancelled = [&]
		{
			return ++visited >= calls / 2U;
		};
		q::projection_resource_usage spent;
		const auto stopped = q::project_source_features(admitted, interrupted, {}, spent);
		require(!stopped && !spent.operations && !spent.retained_bytes_bound,
				"immutable sizing real stop revokes all usage");
		require(bool(q::project_source_features(admitted, {}, {}, spent)),
				"immutable sizing fresh retry");
	}
	declaration_closure_reuse_controls();
	derived_feature_lookup_controls();
	occurrence_index_controls();
	supporting_lookup_controls();
	canonical_size_controls();
	whole_evidence_clone_controls();
	first_present_comparison_controls();
	indexed_membership_prefix_controls();
	retained_membership_controls();
	retained_unbound_and_declaration_controls();
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
				"same admitted row ownership changed output/storage or repeated "
				"validation");
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
