#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>

#include <cxxlens/sdk/build_health.hpp>

#include "../../../src/sdk/query_result_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;
	namespace q = cxxlens::sdk::query;
	using state = q::finite_population_state;
	constexpr std::array<std::string_view, 6U> names{"build.project.v1",
													 "build.variant.v1",
													 "build.compile_unit.v1",
													 "source.file.v1",
													 "build.compile_unit_analysis.v1",
													 "build.analysis_inventory.v1"};
	void require(bool value, std::string_view message)
	{
		if (!value)
		{
			std::cerr << message << '\n';
			std::exit(EXIT_FAILURE);
		}
	}
	template <class T>
	T take(result<T> value)
	{
		if (!value)
		{
			std::cerr << value.error().code << ':' << value.error().field << ':'
					  << value.error().detail << '\n';
			std::exit(EXIT_FAILURE);
		}
		return std::move(*value);
	}
	detached_cell ids(std::initializer_list<std::string_view> values)
	{
		std::vector<std::byte> bytes;
		for (auto v : values)
		{
			for (unsigned shift{}; shift < 32U; shift += 8U)
				bytes.push_back(static_cast<std::byte>((v.size() >> shift) & 255U));
			for (char c : v)
				bytes.push_back(static_cast<std::byte>(c));
		}
		return detached_cell::bytes(std::move(bytes));
	}
	q::annotated_row fact(std::size_t group,
						  std::initializer_list<std::pair<std::string, detached_cell>> values)
	{
		q::annotated_row r;
		r.presence = {"health:test", {"debug"}};
		r.interpretation = "clang22";
		r.claim_contributors = {"claim:health"};
		r.producer_contracts = {{"health.fixture", "semantic:original"}};
		r.provenance = {"health:source"};
		r.contributor_guarantees = {{"exact", "selected-units", "fixture", {"schema_validated"}}};
		r.contributor_edges = {{r.claim_contributors.front(),
								r.producer_contracts.front(),
								r.provenance.front(),
								r.contributor_guarantees.front(),
								r.presence,
								r.interpretation}};
		const auto all = standard_relation_descriptors();
		const auto d = std::ranges::find(all, names[group], &relation_descriptor::id);
		require(d != all.end(), "health descriptor missing");
		for (const auto& c : d->columns)
		{
			auto v = detached_cell::utf8("fixture");
			if (c.type.optional)
				v = detached_cell::absent(c.type);
			else if (c.type.scalar == scalar_kind::unsigned_integer)
				v = detached_cell::unsigned_integer(0U);
			else if (c.type.scalar == scalar_kind::boolean)
				v = detached_cell::boolean(false);
			else if (c.type.scalar == scalar_kind::digest)
				v = detached_cell::utf8(content_digest({}));
			else if (c.type.scalar == scalar_kind::set || c.type.scalar == scalar_kind::bytes)
				v = ids({});
			else if (c.type.scalar == scalar_kind::closed_symbol)
				v = detached_cell::utf8("canonicalized");
			v.type = c.type;
			r.values.emplace("output." + c.name, std::move(v));
		}
		for (const auto& [name, v] : values)
		{
			auto copy = v;
			copy.type = r.values.at("output." + name).type;
			r.values["output." + name] = std::move(copy);
		}
		return r;
	}
	void set(q::annotated_row& r, std::string_view name, detached_cell value)
	{
		const auto k = "output." + std::string{name};
		value.type = r.values.at(k).type;
		r.values[k] = std::move(value);
	}
	struct fixture
	{
		std::array<std::vector<q::annotated_row>, 6U> rows;
		fixture()
		{
			rows[0] = {fact(0, {{"project", detached_cell::utf8("project:test")}})};
			rows[1] = {fact(1,
							{{"variant", detached_cell::utf8("debug")},
							 {"project", detached_cell::utf8("project:test")}})};
			rows[2] = {fact(2,
							{{"compile_unit", detached_cell::utf8("unit:test")},
							 {"project", detached_cell::utf8("project:test")},
							 {"variant", detached_cell::utf8("debug")},
							 {"main_source", detached_cell::utf8("source:test")}})};
			rows[3] = {fact(3,
							{{"snapshot", detached_cell::utf8("source:test")},
							 {"file", detached_cell::utf8("file:test")},
							 {"project", detached_cell::utf8("project:test")}})};
			rows[4] = {fact(4,
							{{"compile_unit", detached_cell::utf8("unit:test")},
							 {"project", detached_cell::utf8("project:test")},
							 {"main_source", detached_cell::utf8("source:test")},
							 {"profile", detached_cell::utf8("clang22-selected-unit-analysis/1")},
							 {"parse_outcome", detached_cell::utf8("success")},
							 {"parse_error_count", detached_cell::unsigned_integer(0U)},
							 {"fatal_error_count", detached_cell::unsigned_integer(0U)},
							 {"semantic_output", detached_cell::utf8("produced")}})};
			rows[5] = {fact(5,
							{{"inventory", detached_cell::utf8("inventory:debug")},
							 {"project", detached_cell::utf8("project:test")},
							 {"profile", detached_cell::utf8("clang22-selected-analysis-units/1")},
							 {"compile_unit_count", detached_cell::unsigned_integer(1U)},
							 {"compile_units", ids({"unit:test"})},
							 {"enumeration_state", detached_cell::utf8("complete")},
							 {"selected_variant_count", detached_cell::unsigned_integer(1U)},
							 {"selected_variant_ids", ids({"debug"})},
							 {"selected_variant_state", detached_cell::utf8("complete")}})};
		}
		q::build_health_input input(bool complete = true) const
		{
			return {
				rows[0], rows[1], rows[2], rows[3], rows[4], rows[5], complete, complete, complete};
		}
		q::application_query_results queries() const
		{
			q::application_query_results r;
			r.snapshot_id = "query:health";
			for (std::size_t i{}; i < rows.size(); ++i)
			{
				auto d = std::make_shared<q::query_result::data>();
				d->row_values = rows[i];
				d->status = q::execution_status::complete;
				d->input_complete = true;
				d->snapshot = r.snapshot_id;
				r.scans.push_back(
					{std::string{names[i]}, {}, q::query_transfer_access::make(std::move(d))});
			}
			return r;
		}
		void second_world()
		{
			for (std::size_t i{}; i < rows.size(); ++i)
			{
				auto copy = rows[i].front();
				copy.presence.fragments = {"release"};
				for (auto& e : copy.contributor_edges)
					e.condition = copy.presence;
				if (i == 1U)
					set(copy, "variant", detached_cell::utf8("release"));
				if (i == 2U)
				{
					set(copy, "compile_unit", detached_cell::utf8("unit:release"));
					set(copy, "variant", detached_cell::utf8("release"));
				}
				if (i == 4U)
				{
					set(copy, "compile_unit", detached_cell::utf8("unit:release"));
					set(copy, "parse_outcome", detached_cell::utf8("failed"));
					set(copy, "parse_error_count", detached_cell::unsigned_integer(1U));
					set(copy, "fatal_error_count", detached_cell::unsigned_integer(1U));
					set(copy, "semantic_output", detached_cell::utf8("not_produced"));
				}
				if (i == 5U)
				{
					set(copy, "inventory", detached_cell::utf8("inventory:release"));
					set(copy, "compile_units", ids({"unit:release"}));
				}
				rows[i].push_back(std::move(copy));
			}
			for (auto& r : rows[5])
			{
				set(r, "selected_variant_count", detached_cell::unsigned_integer(2U));
				set(r, "selected_variant_ids", ids({"debug", "release"}));
			}
		}
	};
	q::build_health_projection project(const fixture& f)
	{
		return take(q::project_build_health(f.input()));
	}
} // namespace
int main()
{
	fixture f;
	auto p = project(f);
	require(p.populations.size() == 1U && p.populations[0].state == state::complete &&
				p.populations[0].selection_state == state::complete &&
				p.populations[0].units[0].state == state::complete,
			"original success census not complete");
	set(f.rows[4][0], "semantic_output", detached_cell::utf8("not_produced"));
	p = project(f);
	require(p.populations[0].units[0].parse_outcome == "success" &&
				p.populations[0].units[0].state == state::complete,
			"extraction failure relabeled actual parse success");
	set(f.rows[4][0], "parse_outcome", detached_cell::utf8("recovery"));
	set(f.rows[4][0], "parse_error_count", detached_cell::unsigned_integer(2U));
	set(f.rows[4][0], "semantic_output", detached_cell::utf8("produced"));
	require(project(f).populations[0].units[0].state == state::complete,
			"nonfatal AST recovery unavailable");
	set(f.rows[4][0], "fatal_error_count", detached_cell::unsigned_integer(1U));
	require(project(f).populations[0].units[0].state == state::conflicting,
			"fatal diagnosis accepted as nonfatal recovery");
	set(f.rows[4][0], "parse_outcome", detached_cell::utf8("failed"));
	set(f.rows[4][0], "semantic_output", detached_cell::utf8("not_produced"));
	p = project(f);
	require(p.populations[0].units[0].state == state::complete &&
				p.populations[0].units[0].source_snapshot == "source:test" &&
				p.populations[0].variant == "debug",
			"no-AST selected unit lost actual source or variant");
	f = fixture{};
	f.rows[4].clear();
	p = project(f);
	require(p.populations[0].state == state::complete &&
				p.populations[0].units[0].state == state::partial,
			"missing parser outcome erased finite membership or became zero");
	f = fixture{};
	f.rows[2].clear();
	p = project(f);
	require(p.populations[0].state == state::partial &&
				p.populations[0].units[0].state != state::conflicting,
			"missing original unit became a conflicting payload");
	f = fixture{};
	f.rows[5].clear();
	p = project(f);
	require(p.populations[0].inventory_id.empty() && p.populations[0].state == state::unknown &&
				p.populations[0].variant == "debug",
			"missing census fabricated selection or variant");
	f = fixture{};
	set(f.rows[5][0], "compile_unit_count", detached_cell::unsigned_integer(0U));
	set(f.rows[5][0], "compile_units", ids({}));
	require(project(f).populations[0].state == state::conflicting,
			"independent known selected unit omitted from zero census");
	f = fixture{};
	f.second_world();
	p = project(f);
	require(p.populations.size() == 2U && p.populations[0].selection_state == state::complete &&
				p.populations[1].selection_state == state::complete &&
				p.populations[1].units[0].parse_outcome == "failed",
			"mixed actual selected worlds did not retain failure");
	set(f.rows[5][1], "selected_variant_count", detached_cell::unsigned_integer(1U));
	set(f.rows[5][1], "selected_variant_ids", ids({"release"}));
	p = project(f);
	require(p.populations[0].selection_state == state::conflicting &&
				p.populations[1].selection_state == state::conflicting,
			"cross-world selected census gained first wins");
	f = fixture{};
	set(f.rows[5][0], "selected_variant_count", detached_cell::unsigned_integer(2U));
	set(f.rows[5][0], "selected_variant_ids", ids({"debug", "release"}));
	require(project(f).populations[0].selection_state == state::partial,
			"omitted selected world silently completed global census");
	f = fixture{};
	for (auto& g : f.rows)
	{
		g.push_back(g.front());
		std::ranges::reverse(g);
	}
	p = project(f);
	require(p.evidence.size() == 12U && p.populations[0].units[0].state == state::complete &&
				p.populations[0].units[0].evidence.size() > 5U,
			"equal duplicates lost original witnesses");
	set(f.rows[4].back(), "semantic_output", detached_cell::utf8("not_produced"));
	require(project(f).populations[0].units[0].state == state::conflicting,
			"conflicting original outcome won");
	f = fixture{};
	require(take(q::project_build_health(f.input(false))).populations[0].state == state::unknown,
			"raw closure absence became complete");
	auto queries = f.queries();
	p = take(q::project_build_health(queries));
	require(p.source_queries && p.source_queries->snapshot_id == queries.snapshot_id &&
				p.populations[0].units[0].state == state::complete,
			"public query originals lost");
	queries.scans.erase(queries.scans.begin() + 4U);
	require(take(q::project_build_health(queries)).populations[0].units[0].state == state::partial,
			"missing public analysis scan became zero");
	f = fixture{};
	f.rows[2].clear();
	f.rows[4].clear();
	set(f.rows[5][0], "compile_unit_count", detached_cell::unsigned_integer(0U));
	set(f.rows[5][0], "compile_units", ids({}));
	p = project(f);
	require(p.populations[0].state == state::complete && p.populations[0].units.empty(),
			"explicit original empty unit census became missing");
	f = fixture{};
	set(f.rows[4][0], "main_source", detached_cell::utf8("source:foreign"));
	require(project(f).populations[0].units[0].state == state::conflicting,
			"analysis foreign source joined actual unit");
	f = fixture{};
	set(f.rows[2][0], "variant", detached_cell::utf8("release"));
	require(project(f).populations[0].units[0].state == state::conflicting,
			"foreign unit variant joined actual world");
	f = fixture{};
	f.rows[1].clear();
	require(project(f).populations[0].selection_state == state::partial,
			"missing original variant left selected census exact");
	f = fixture{};
	set(f.rows[4][0], "parse_outcome", detached_cell::utf8("unavailable"));
	set(f.rows[4][0],
		"parse_error_count",
		detached_cell::unknown(f.rows[4][0].values.at("output.parse_error_count").type,
							   "parser-not-attempted"));
	p = project(f);
	require(p.populations[0].units[0].state == state::partial &&
				!p.populations[0].units[0].parse_error_count,
			"unobserved parser counters became zero");
	f = fixture{};
	set(f.rows[5][0], "compile_unit_count", detached_cell::unsigned_integer(2U));
	require(project(f).populations[0].state == state::conflicting,
			"original census count mismatch accepted");
	f = fixture{};
	q::finite_population_limits limits;
	limits.maximum_rows = 1U;
	require(!q::project_build_health(f.input(), limits), "row limit ignored");
	limits = {};
	limits.maximum_retained_bytes = 1U;
	require(!q::project_build_health(f.input(), limits), "retained bound ignored");
	limits = {};
	limits.maximum_operations = 1U;
	require(!q::project_build_health(f.input(), limits), "work bound ignored");
	limits = {};
	std::size_t checks{};
	limits.cancelled = [&]()
	{
		return ++checks > 4U;
	};
	const auto stopped = q::project_build_health(f.input(), limits);
	require(!stopped && stopped.error().code == "sdk.health-cancelled" && checks > 4U,
			"active callback not polled during work");
	checks = 0U;
	const auto query_stopped = q::project_build_health(f.queries(), limits);
	require(!query_stopped && query_stopped.error().code == "sdk.health-cancelled" && checks > 4U,
			"public query plan work did not poll active cancellation");
	std::cout << "build health original outcome, world, census, conflict and bound cases passed\n";
}
