#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>

#include <cxxlens/relations/build_compile_unit.hpp>
#include <cxxlens/relations/cc_syntax_node.hpp>
#include <cxxlens/relations/source_file.hpp>
#include <cxxlens/relations/source_preprocessor_event.hpp>
#include <cxxlens/relations/source_preprocessor_inventory.hpp>
#include <cxxlens/relations/source_span.hpp>
#include <cxxlens/relations/source_token.hpp>
#include <cxxlens/relations/source_token_inventory.hpp>
#include <cxxlens/sdk/preprocessor.hpp>

#include "query_result_internal.hpp"

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
		result.presence = {"pp:test", {"debug"}};
		result.interpretation = "clang22";
		result.claim_contributors = {"claim:pp"};
		result.provenance = {"source:pp"};
		result.producer_contracts = {{"preprocessor.fixture", "original-owned/1"}};
		result.contributor_guarantees = {
			{"exact", "finite-local-population", "fixture", {"schema_validated"}}};
		result.contributor_edges = {{result.claim_contributors.front(),
									 result.producer_contracts.front(),
									 result.provenance.front(),
									 result.contributor_guarantees.front(),
									 result.presence,
									 result.interpretation}};
		const auto standard = standard_relation_descriptors();
		std::vector<relation_descriptor> descriptors{standard.begin(), standard.end()};
		for (const auto& current :
			 {cxxlens::build::relations::compile_unit::descriptor(),
			  cxxlens::source::relations::file::descriptor(),
			  cxxlens::source::relations::span::descriptor(),
			  cxxlens::source::relations::token_inventory::descriptor(),
			  cxxlens::source::relations::token::descriptor(),
			  cxxlens::cc::relations::syntax_node::descriptor(),
			  cxxlens::source::relations::preprocessor_inventory::descriptor(),
			  cxxlens::source::relations::preprocessor_event::descriptor()})
		{
			const auto found = std::ranges::find(descriptors, current.id, &relation_descriptor::id);
			if (found == descriptors.end())
				descriptors.push_back(current);
			else
				*found = current;
		}
		const auto d = std::ranges::find(descriptors, relation, &relation_descriptor::id);
		require(d != descriptors.end(), "relation descriptor required");
		for (const auto& column : d->columns)
		{
			detached_cell value = detached_cell::utf8("fixture");
			if (column.type.optional)
				value = detached_cell::absent(column.type);
			else if (column.type.scalar == scalar_kind::boolean)
				value = detached_cell::boolean(false);
			else if (column.type.scalar == scalar_kind::unsigned_integer)
				value = detached_cell::unsigned_integer(0U);
			else if (column.type.scalar == scalar_kind::digest)
				value = detached_cell::utf8("sha256:" + std::string(64U, 'a'));
			else if (column.type.scalar == scalar_kind::closed_symbol)
				value = detached_cell::utf8("canonicalized");
			else if (column.type.scalar == scalar_kind::bytes ||
					 column.type.scalar == scalar_kind::set)
				value = ids({});
			value.type = column.type;
			result.values.emplace("output." + column.name, std::move(value));
		}
		for (const auto& [name, value] : values)
		{
			auto actual = value;
			actual.type = result.values.at("output." + name).type;
			result.values.insert_or_assign("output." + name, std::move(actual));
		}
		return result;
	}
	void set(q::annotated_row& row, std::string_view name, detached_cell value)
	{
		value.type = row.values.at("output." + std::string{name}).type;
		row.values.insert_or_assign("output." + std::string{name}, std::move(value));
	}
	struct fixture
	{
		std::array<std::vector<q::annotated_row>, 8U> g;
		fixture()
		{
			g[0] = {row("build.compile_unit.v1", {{"compile_unit", detached_cell::utf8("tu:pp")}})};
			g[1] = {row("source.file.v1",
						{{"snapshot", detached_cell::utf8("snapshot:pp")},
						 {"file", detached_cell::utf8("file:pp")},
						 {"size", detached_cell::unsigned_integer(100U)}})};
			span("span:directive", 0U, 10U);
			span("span:token", 0U, 1U);
			g[5] = {row("source.token_inventory.v1",
						{{"inventory", detached_cell::utf8("tokens:pp")},
						 {"compile_unit", detached_cell::utf8("tu:pp")},
						 {"source_snapshot", detached_cell::utf8("snapshot:pp")},
						 {"file", detached_cell::utf8("file:pp")},
						 {"phase", detached_cell::utf8("raw")},
						 {"profile", detached_cell::utf8("clang22.tokens.lexical-pp-template.v1")},
						 {"token_count", detached_cell::unsigned_integer(2U)},
						 {"directive_count", detached_cell::unsigned_integer(1U)},
						 {"enumeration_state", detached_cell::utf8("complete")}})};
			g[6] = {row("source.token.v1",
						{{"token", detached_cell::utf8("token:hash")},
						 {"compile_unit", detached_cell::utf8("tu:pp")},
						 {"phase", detached_cell::utf8("raw")},
						 {"profile", detached_cell::utf8("clang22.tokens.lexical-pp-template.v1")},
						 {"source", detached_cell::utf8("span:token")},
						 {"kind", detached_cell::utf8("hash")},
						 {"directive_start", detached_cell::boolean(true)}})};
			g[4] = {event("raw:event", "raw_define", "raw")};
			set(g[4][0], "function_like", detached_cell::boolean(true));
			set(g[4][0], "variadic", detached_cell::boolean(false));
			set(g[4][0], "parameter_count", detached_cell::unsigned_integer(1U));
			set(g[4][0], "replacement_count", detached_cell::unsigned_integer(2U));
			set(g[4][0], "raw_token_ids", ids({"token:hash", "token:parameter"}));
			set(g[4][0], "stringify_token_ids", ids({}));
			set(g[4][0], "paste_token_ids", ids({}));
			set(g[4][0], "macro_state", detached_cell::utf8("complete"));
			span("span:parameter", 2U, 3U);
			g[6].push_back(
				row("source.token.v1",
					{{"token", detached_cell::utf8("token:parameter")},
					 {"compile_unit", detached_cell::utf8("tu:pp")},
					 {"phase", detached_cell::utf8("raw")},
					 {"profile", detached_cell::utf8("clang22.tokens.lexical-pp-template.v1")},
					 {"source", detached_cell::utf8("span:parameter")},
					 {"kind", detached_cell::utf8("identifier")},
					 {"directive_start", detached_cell::boolean(false)}}));
			g[4].push_back(event("raw:parameter", "macro_parameter", "raw"));
			set(g[4].back(), "source", detached_cell::utf8("span:parameter"));
			set(g[4].back(), "raw_event", detached_cell::utf8("raw:event"));
			set(g[4].back(), "parent_event", detached_cell::utf8("raw:event"));
			set(g[4].back(), "parameter_index", detached_cell::unsigned_integer(0U));
			set(g[4].back(), "parameter_symbol", detached_cell::utf8("pp-symbol:x"));
			set(g[4].back(), "raw_token_ids", ids({"token:parameter"}));
			set(g[4].back(), "macro_state", detached_cell::utf8("complete"));
			g[3] = {inventory("raw:inventory", "raw", {"raw:event", "raw:parameter"})};
		}
		void span(std::string id, std::uint64_t begin, std::uint64_t end)
		{
			g[2].push_back(row("source.span.v1",
							   {{"span", detached_cell::utf8(std::move(id))},
								{"snapshot", detached_cell::utf8("snapshot:pp")},
								{"file", detached_cell::utf8("file:pp")},
								{"begin", detached_cell::unsigned_integer(begin)},
								{"end", detached_cell::unsigned_integer(end)}}));
		}
		q::annotated_row event(std::string id, std::string kind, std::string phase)
		{
			return row(
				"source.preprocessor_event.v1",
				{{"event", detached_cell::utf8(std::move(id))},
				 {"compile_unit", detached_cell::utf8("tu:pp")},
				 {"source", detached_cell::utf8("span:directive")},
				 {"kind", detached_cell::utf8(std::move(kind))},
				 {"state", detached_cell::utf8("observed")},
				 {"phase", detached_cell::utf8(phase)},
				 {"profile",
				  detached_cell::utf8(phase == "raw" ? "clang22-frozen-raw-directives/1"
													 : "clang22-evaluated-preprocessor/1")}});
		}
		q::annotated_row
		inventory(std::string id, std::string phase, std::initializer_list<std::string_view> events)
		{
			return row("source.preprocessor_inventory.v1",
					   {{"inventory", detached_cell::utf8(std::move(id))},
						{"compile_unit", detached_cell::utf8("tu:pp")},
						{"file", detached_cell::utf8("file:pp")},
						{"source_snapshot", detached_cell::utf8("snapshot:pp")},
						{"phase", detached_cell::utf8(phase)},
						{"profile",
						 detached_cell::utf8(phase == "raw" ? "clang22-frozen-raw-directives/1"
															: "clang22-evaluated-preprocessor/1")},
						{"event_count", detached_cell::unsigned_integer(events.size())},
						{"event_ids", ids(events)},
						{"enumeration_state", detached_cell::utf8("complete")},
						{"macro_state", detached_cell::utf8("complete")},
						{"structure_state", detached_cell::utf8("unsupported")},
						{"activity_state", detached_cell::utf8("unsupported")},
						{"candidate_state", detached_cell::utf8("unsupported")},
						{"expansion_state", detached_cell::utf8("unsupported")},
						{"argument_effect_state", detached_cell::utf8("unsupported")}});
		}
		q::preprocessor_input input() const
		{
			return {g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], true, true, true, true, true};
		}
		result<q::preprocessor_projection> run(q::finite_population_limits limits = {},
											   std::stop_token stop = {}) const
		{
			return q::project_preprocessor(input(), limits, stop);
		}
		q::application_query_results queries() const
		{
			constexpr std::array<std::string_view, 8U> names{"build.compile_unit.v1",
															 "source.file.v1",
															 "source.span.v1",
															 "source.preprocessor_inventory.v1",
															 "source.preprocessor_event.v1",
															 "source.token_inventory.v1",
															 "source.token.v1",
															 "cc.syntax_node.v1"};
			q::application_query_results value;
			value.snapshot_id = "query:pp";
			for (std::size_t i{}; i < g.size(); ++i)
			{
				auto data = std::make_shared<q::query_result::data>();
				data->row_values = g[i];
				data->status = q::execution_status::complete;
				data->input_complete = true;
				data->ordered = true;
				data->snapshot = value.snapshot_id;
				data->unresolved.push_back(
					{"fixture-original-gap", std::string{names[i]}, "original-side-channel"});
				value.scans.push_back(
					{std::string{names[i]}, {}, q::query_transfer_access::make(std::move(data))});
			}
			return value;
		}
	};
	const q::preprocessor_population& population(const q::preprocessor_projection& result,
												 std::string_view phase)
	{
		const auto found =
			std::ranges::find(result.populations, phase, &q::preprocessor_population::phase);
		require(found != result.populations.end(), "population missing");
		return *found;
	}
	bool has_gap(const q::preprocessor_projection& result, std::string_view code)
	{
		return std::ranges::any_of(result.unresolved,
								   [&](const auto& g)
								   {
									   return g.code == code;
								   });
	}
} // namespace
int main()
{
	using state = q::preprocessor_state;
	fixture actual;
	auto projected = actual.run();
	if (!projected)
		std::cerr << projected.error().code << ":" << projected.error().field << ":"
				  << projected.error().detail << "\n";
	require(bool(projected), "original projection");
	require(population(*projected, "raw").state == state::complete, "raw complete");
	require(population(*projected, "raw").macro_state == state::complete, "raw macro complete");
	require(population(*projected, "raw").events.front().function_like == true,
			"actual function-like facet");
	require(population(*projected, "evaluated").state == state::unknown,
			"missing evaluated inventory is unknown");
	require(!projected->evidence.empty(), "original evidence retained");
	fixture empty;
	empty.g[4].clear();
	empty.g[6].clear();
	empty.g[3][0] = empty.inventory("empty:raw", "raw", {});
	set(empty.g[5][0], "token_count", detached_cell::unsigned_integer(0U));
	set(empty.g[5][0], "directive_count", detached_cell::unsigned_integer(0U));
	auto zero = empty.run();
	require(bool(zero) && population(*zero, "raw").state == state::complete &&
				population(*zero, "raw").events.empty(),
			"complete empty raw is not absent");
	fixture two_files = empty;
	two_files.g[0].push_back(
		row("build.compile_unit.v1", {{"compile_unit", detached_cell::utf8("tu:other")}}));
	two_files.g[1].push_back(row("source.file.v1",
								 {{"snapshot", detached_cell::utf8("snapshot:other")},
								  {"file", detached_cell::utf8("file:other")},
								  {"size", detached_cell::unsigned_integer(100U)}}));
	auto second_tokens = two_files.g[5].front();
	set(second_tokens, "inventory", detached_cell::utf8("tokens:other"));
	set(second_tokens, "compile_unit", detached_cell::utf8("tu:other"));
	set(second_tokens, "file", detached_cell::utf8("file:other"));
	set(second_tokens, "source_snapshot", detached_cell::utf8("snapshot:other"));
	two_files.g[5].push_back(std::move(second_tokens));
	auto second_raw = two_files.g[3].front();
	set(second_raw, "inventory", detached_cell::utf8("raw:other"));
	set(second_raw, "compile_unit", detached_cell::utf8("tu:other"));
	set(second_raw, "file", detached_cell::utf8("file:other"));
	set(second_raw, "source_snapshot", detached_cell::utf8("snapshot:other"));
	two_files.g[3].push_back(std::move(second_raw));
	auto independent = two_files.run();
	require(bool(independent) && independent->populations.size() == 4U,
			"two independent TUs retain only their original file memberships");
	for (const auto& p : independent->populations)
		require((p.compile_unit == "tu:pp" && p.file == "file:pp") ||
					(p.compile_unit == "tu:other" && p.file == "file:other"),
				"no Cartesian TU-file population invented");
	two_files.g[0].push_back(
		row("build.compile_unit.v1", {{"compile_unit", detached_cell::utf8("tu:unobserved")}}));
	independent = two_files.run();
	require(bool(independent) && independent->populations.size() == 4U &&
				has_gap(*independent, "sdk.preprocessor-unit-files-unobserved"),
			"missing all original file associations stays unit-level unknown");
	empty.g[3].clear();
	zero = empty.run();
	require(bool(zero) && population(*zero, "raw").state == state::unknown,
			"absent raw is unknown");
	fixture missing;
	missing.g[4].clear();
	auto out = missing.run();
	require(bool(out) && population(*out, "raw").state != state::complete &&
				has_gap(*out, "sdk.preprocessor-event-missing"),
			"missing event never zero");
	fixture unlisted;
	unlisted.g[3][0] = unlisted.inventory("empty:raw", "raw", {});
	out = unlisted.run();
	require(bool(out) && has_gap(*out, "sdk.preprocessor-event-unlisted"),
			"unlisted original event conflict");
	fixture false_zero;
	false_zero.g[4].clear();
	false_zero.g[3][0] = false_zero.inventory("empty:raw", "raw", {});
	out = false_zero.run();
	require(bool(out) && has_gap(*out, "sdk.preprocessor-directive-population-conflicting"),
			"independent directive admission defeats false empty");
	fixture tokenless;
	tokenless.g[5].clear();
	out = tokenless.run();
	require(bool(out) && population(*out, "raw").state != state::complete,
			"missing token inventory prevents raw closure");
	fixture foreign;
	set(foreign.g[4][0], "compile_unit", detached_cell::utf8("tu:foreign"));
	out = foreign.run();
	require(bool(out) && has_gap(*out, "sdk.preprocessor-event-owner-conflicting"),
			"foreign unit never joined");
	fixture count;
	set(count.g[3][0], "event_count", detached_cell::unsigned_integer(3U));
	out = count.run();
	require(bool(out) && population(*out, "raw").state == state::conflicting,
			"independent count mismatch");
	fixture facet;
	set(facet.g[4][0],
		"function_like",
		detached_cell::absent(facet.g[4][0].values.at("output.function_like").type));
	out = facet.run();
	require(bool(out) && population(*out, "raw").state == state::complete &&
				population(*out, "raw").macro_state != state::complete,
			"macro unknown independent of enumeration");
	fixture prose;
	set(prose.g[4][0], "value", detached_cell::utf8("function_like variadic # x ## y"));
	set(prose.g[4][0], "function_like", detached_cell::boolean(false));
	out = prose.run();
	require(bool(out) && population(*out, "raw").events.front().function_like == false,
			"display prose never interpreted");
	fixture duplicate;
	auto changed = duplicate.g[4][0];
	set(changed, "variadic", detached_cell::boolean(true));
	duplicate.g[4].push_back(changed);
	out = duplicate.run();
	require(bool(out) && population(*out, "raw").state == state::conflicting,
			"conflict never first wins");
	fixture unsupported;
	set(unsupported.g[3][0], "profile", detached_cell::utf8("future/1"));
	out = unsupported.run();
	require(bool(out) && has_gap(*out, "sdk.preprocessor-profile-unsupported"),
			"unknown profile remains explicit");
	fixture crossworld;
	crossworld.g[4][0].presence.fragments = {"release"};
	crossworld.g[4][0].contributor_edges.front().condition.fragments = {"release"};
	out = crossworld.run();
	require(bool(out) && has_gap(*out, "sdk.preprocessor-event-missing"),
			"different variants never merged");
	fixture evaluated;
	evaluated.g[4].push_back(evaluated.event("eval:event", "macro_expansion", "evaluated"));
	set(evaluated.g[4].back(), "raw_event", detached_cell::utf8("raw:event"));
	set(evaluated.g[4].back(), "expansion_depth", detached_cell::unsigned_integer(1U));
	evaluated.g[3].push_back(evaluated.inventory("eval:inventory", "evaluated", {"eval:event"}));
	out = evaluated.run();
	require(bool(out) && population(*out, "evaluated").state == state::complete,
			"evaluated phase independent original admission");
	set(evaluated.g[4].back(), "definition_event", detached_cell::utf8("raw:foreign"));
	out = evaluated.run();
	require(bool(out) && has_gap(*out, "sdk.preprocessor-link-unbound"),
			"foreign definition link unknown");
	fixture cycle;
	set(cycle.g[4][0], "parent_event", detached_cell::utf8("raw:event"));
	set(cycle.g[3][0], "structure_state", detached_cell::utf8("complete"));
	out = cycle.run();
	require(bool(out) && population(*out, "raw").structure_state == state::conflicting,
			"typed parent cycle conflict");
	q::finite_population_limits tiny;
	tiny.maximum_operations = 1U;
	out = actual.run(tiny);
	require(!out && out.error().code == "sdk.preprocessor-budget", "operation bound");
	tiny = {};
	tiny.maximum_evidence_bytes = 1U;
	out = actual.run(tiny);
	require(!out && out.error().code == "sdk.preprocessor-budget", "evidence bound");
	tiny = {};
	tiny.maximum_retained_bytes = 1U;
	out = actual.run(tiny);
	require(!out && out.error().code == "sdk.preprocessor-budget", "retained bound");
	tiny = {};
	tiny.maximum_members = 0U;
	require(!actual.run(tiny), "invalid limits");
	std::stop_source stop;
	stop.request_stop();
	out = actual.run({}, stop.get_token());
	require(!out && out.error().code == "sdk.preprocessor-cancelled", "stop token");
	tiny = {};
	tiny.cancelled = []
	{
		return true;
	};
	out = actual.run(tiny);
	require(!out && out.error().code == "sdk.preprocessor-cancelled", "active cancellation");
	fixture ordered;
	std::ranges::reverse(ordered.g[2]);
	out = ordered.run();
	require(bool(out) && out->evidence.size() == projected->evidence.size() &&
				population(*out, "raw").events.front().id ==
					population(*projected, "raw").events.front().id,
			"deterministic order");
	fixture auxiliary_missing;
	auxiliary_missing.g[4].pop_back();
	auxiliary_missing.g[3][0] = auxiliary_missing.inventory("raw:inventory", "raw", {"raw:event"});
	out = auxiliary_missing.run();
	require(bool(out) && population(*out, "raw").state == state::complete &&
				population(*out, "raw").macro_state == state::conflicting,
			"parameter auxiliary closure independent of raw directives");
	fixture orphan = empty;
	orphan.g[4] = {orphan.event("orphan:event", "raw_define", "raw")};
	set(orphan.g[4].front(), "source", detached_cell::utf8("span:uncaptured"));
	orphan.g[3] = {orphan.inventory("empty:raw", "raw", {})};
	out = orphan.run();
	require(bool(out) && population(*out, "raw").state != state::complete &&
				has_gap(*out, "sdk.preprocessor-event-owner-unbound"),
			"dangling event never disappears into known empty");
	fixture effects;
	effects.span("span:argument", 20U, 27U);
	effects.g[7] = {row("cc.syntax_node.v1",
						{{"node", detached_cell::utf8("syntax:argument")},
						 {"compile_unit", detached_cell::utf8("tu:pp")},
						 {"function", detached_cell::utf8("function:pp")},
						 {"source", detached_cell::utf8("span:argument")},
						 {"kind", detached_cell::utf8("UnaryOperator")}})};
	effects.g[4].push_back(effects.event("eval:argument", "macro_argument", "evaluated"));
	auto& argument = effects.g[4].back();
	set(argument, "argument_source", detached_cell::utf8("span:argument"));
	set(argument, "effect_expression", detached_cell::utf8("syntax:argument"));
	set(argument, "effect_function", detached_cell::utf8("function:pp"));
	set(argument, "effect_state", detached_cell::utf8("complete"));
	set(argument, "argument_may_have_side_effects", detached_cell::boolean(true));
	set(argument, "argument_empty", detached_cell::boolean(false));
	set(argument, "substitution_count", detached_cell::unsigned_integer(2U));
	set(argument, "evaluating_substitution_count", detached_cell::unsigned_integer(2U));
	effects.g[3].push_back(effects.inventory("eval:inventory", "evaluated", {"eval:argument"}));
	set(effects.g[3].back(), "argument_effect_state", detached_cell::utf8("complete"));
	out = effects.run();
	require(bool(out) && population(*out, "evaluated").state == state::complete &&
				population(*out, "evaluated").argument_effect_state == state::complete &&
				population(*out, "evaluated").events.front().argument_may_have_side_effects == true,
			"actual Expr HasSideEffects and exact owner retained");
	set(argument, "substitution_count", detached_cell::unsigned_integer(1U));
	out = effects.run();
	require(bool(out) && population(*out, "evaluated").argument_effect_state == state::complete &&
				population(*out, "evaluated").events.front().substitution_count == 1U &&
				population(*out, "evaluated").events.front().evaluating_substitution_count == 2U,
			"direct replacement and expanded AST evaluation multiplicities are independent");
	set(effects.g[7][0], "function", detached_cell::utf8("function:foreign"));
	out = effects.run();
	require(bool(out) && population(*out, "evaluated").state == state::complete &&
				population(*out, "evaluated").argument_effect_state != state::complete,
			"foreign expression function affects only actual effect facet");
	auto queries = actual.queries();
	out = q::project_preprocessor(queries);
	require(bool(out) && out->source_queries && out->source_queries->scans.size() == 8U &&
				out->source_queries->scans[4].result.unresolved_items().front().code ==
					"fixture-original-gap" &&
				population(*out, "raw").state == state::complete,
			"original plans and side channels preserved");
	auto partial = queries;
	auto data = std::make_shared<q::query_result::data>();
	data->row_values = actual.g[4];
	data->status = q::execution_status::truncated;
	data->input_complete = false;
	partial.scans[4].result = q::query_transfer_access::make(std::move(data));
	out = q::project_preprocessor(partial);
	require(bool(out) && !out->event_inputs_complete &&
				population(*out, "raw").state != state::complete,
			"partial original scan cannot close raw domain");
	queries.scans.erase(queries.scans.begin() + 6);
	out = q::project_preprocessor(queries);
	require(bool(out) && !out->token_inputs_complete &&
				population(*out, "raw").state != state::complete,
			"missing independent token scan cannot close raw domain");
	tiny = {};
	tiny.maximum_source_queries = 1U;
	out = q::project_preprocessor(actual.queries(), tiny);
	require(!out && out.error().code == "sdk.preprocessor-budget", "source query count bound");
	tiny = {};
	tiny.maximum_source_plan_bytes = 1U;
	out = q::project_preprocessor(actual.queries(), tiny);
	require(!out && out.error().code == "sdk.preprocessor-budget", "source plan bytes bound");
	tiny = {};
	tiny.cancelled = []
	{
		return true;
	};
	out = q::project_preprocessor(actual.queries(), tiny);
	require(!out && out.error().code == "sdk.preprocessor-cancelled",
			"query plan active cancellation");
	fixture invalid_annotation;
	invalid_annotation.g[0][0].producer_contracts.clear();
	require(!invalid_annotation.run(), "invalid original row annotation rejected");
	fixture invalid_cell;
	invalid_cell.g[4][0].values.at("output.kind").type = detached_cell::boolean(false).type;
	require(!invalid_cell.run(), "foreign typed original cell rejected");
	std::cout << "preprocessor original/facet/closure/bounds oracles passed\n";
}
