#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>

#include <cxxlens/sdk/template_events.hpp>

#include "../../../src/sdk/query_result_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;
	namespace q = cxxlens::sdk::query;
	using state = q::finite_population_state;
	constexpr std::array<std::string_view, 9> names{"build.compile_unit.v1",
													"source.file.v1",
													"source.span.v1",
													"cc.entity.v1",
													"cc.syntax_node.v1",
													"cc.template_candidate.v1",
													"cc.constant_evaluation_root.v1",
													"cc.constant_evaluated_call.v1",
													"cc.template_inventory.v1"};
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
		r.presence = {"templates:test", {"debug"}};
		r.interpretation = "clang22";
		r.claim_contributors = {"claim:health"};
		r.producer_contracts = {{"template.fixture", "semantic:original"}};
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
		require(d != all.end(), "template descriptor missing");
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
		std::array<std::vector<q::annotated_row>, 9> rows;
		fixture()
		{
			rows[0] = {fact(0,
							{{"compile_unit", detached_cell::utf8("unit")},
							 {"variant", detached_cell::utf8("debug")}})};
			rows[1] = {fact(1,
							{{"snapshot", detached_cell::utf8("snapshot")},
							 {"file", detached_cell::utf8("file")},
							 {"size", detached_cell::unsigned_integer(100)}})};
			rows[2] = {fact(2,
							{{"span", detached_cell::utf8("span")},
							 {"snapshot", detached_cell::utf8("snapshot")},
							 {"file", detached_cell::utf8("file")},
							 {"begin", detached_cell::unsigned_integer(1)},
							 {"end", detached_cell::unsigned_integer(90)}})};
			std::vector<std::byte> key;
			for (char c : std::string("clang-usr:original"))
				key.push_back(static_cast<std::byte>(c));
			rows[3] = {fact(3,
							{{"entity", detached_cell::utf8("entity")},
							 {"provider_local_key", detached_cell::bytes(key)}})};
			rows[8] = {
				fact(8,
					 {{"inventory", detached_cell::utf8("inventory")},
					  {"compile_unit", detached_cell::utf8("unit")},
					  {"profile", detached_cell::utf8("clang22-original-template-domains/2")}})};
			std::vector<std::byte> raw;
			for (char c : std::string_view("original"))
				raw.push_back(static_cast<std::byte>(c));
			rows[5] = {fact(
				5,
				{{"candidate", detached_cell::utf8("candidate")},
				 {"compile_unit", detached_cell::utf8("unit")},
				 {"source", detached_cell::utf8("span")},
				 {"candidate_entity", detached_cell::utf8("entity")},
				 {"candidate_usr", detached_cell::bytes(raw)},
				 {"route", detached_cell::utf8("function_overload")},
				 {"deduction_result", detached_cell::utf8("substitution_failure")},
				 {"exclusion_disposition", detached_cell::utf8("substitution_exclusion")},
				 {"completed", detached_cell::boolean(true)},
				 {"binding_state", detached_cell::utf8("complete")},
				 {"profile", detached_cell::utf8("clang22-final-candidate-substitution-events/1")},
				 {"is_system", detached_cell::boolean(false)}})};
			rows[6] = {
				fact(6,
					 {{"root", detached_cell::utf8("root")},
					  {"compile_unit", detached_cell::utf8("unit")},
					  {"mode", detached_cell::utf8("constant_expression")},
					  {"interpreter", detached_cell::utf8("legacy")},
					  {"requested_constant_context", detached_cell::boolean(true)},
					  {"completion", detached_cell::utf8("complete")},
					  {"call_count", detached_cell::unsigned_integer(1)},
					  {"call_ids", ids({"call"})},
					  {"call_state", detached_cell::utf8("complete")},
					  {"profile", detached_cell::utf8("clang22-legacy-constant-evaluation/1")}})};
			rows[7] = {
				fact(7,
					 {{"call", detached_cell::utf8("call")},
					  {"compile_unit", detached_cell::utf8("unit")},
					  {"source", detached_cell::utf8("span")},
					  {"owner_entity", detached_cell::utf8("entity")},
					  {"owner_usr", detached_cell::bytes(raw)},
					  {"invocation_kind", detached_cell::utf8("function")},
					  {"expression_kind", detached_cell::utf8("CallExpr")},
					  {"root_count", detached_cell::unsigned_integer(1)},
					  {"root_ids", ids({"root"})},
					  {"callee_usr_hexes", ids({"63616c6c6565"})},
					  {"membership_state", detached_cell::utf8("complete")},
					  {"binding_state", detached_cell::utf8("complete")},
					  {"profile", detached_cell::utf8("clang22-legacy-constant-evaluation/1")},
					  {"is_system", detached_cell::boolean(false)}})};
			set(rows[8][0], "candidate_count", detached_cell::unsigned_integer(1));
			set(rows[8][0], "candidate_ids", ids({"candidate"}));
			set(rows[8][0], "candidate_state", detached_cell::utf8("complete"));
			set(rows[8][0],
				"candidate_profile",
				detached_cell::utf8("clang22-final-candidate-substitution-events/1"));
			set(rows[8][0], "evaluation_root_count", detached_cell::unsigned_integer(1));
			set(rows[8][0], "evaluation_root_ids", ids({"root"}));
			set(rows[8][0], "evaluation_root_state", detached_cell::utf8("complete"));
			set(rows[8][0],
				"evaluation_root_profile",
				detached_cell::utf8("clang22-legacy-constant-evaluation/1"));
		}
		q::template_event_input input()
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
					true,
					true,
					true,
					true,
					true};
		}
		q::application_query_results queries() const
		{
			q::application_query_results result;
			result.snapshot_id = "query:template-events";
			for (std::size_t i = 0; i < rows.size(); ++i)
			{
				auto data = std::make_shared<q::query_result::data>();
				data->row_values = rows[i];
				data->status = q::execution_status::complete;
				data->input_complete = true;
				data->snapshot = result.snapshot_id;
				result.scans.push_back(
					{std::string(names[i]), {}, q::query_transfer_access::make(data)});
			}
			return result;
		}
		q::template_event_projection project()
		{
			return take(q::project_template_events(input()));
		}
	};
} // namespace
int main()
{
	fixture original;
	const auto positive = original.project();
	require(positive.populations.size() == 1, "single actual population");
	const auto& p = positive.populations.front();
	require(p.candidate_state == state::complete && p.invocation_state == state::complete,
			"independent full event census");
	require(p.candidates.size() == 1 && p.calls.size() == 1 && p.roots.size() == 1,
			"original event members");
	require(p.candidates[0].subject_state == state::complete &&
				p.candidates[0].source_state == state::complete,
			"actual raw/framed subject/source join");
	require(p.calls[0].owner_state == state::complete &&
				p.calls[0].expression_state == state::unknown,
			"missing optional syntax independent census");
	{
		auto f = original;
		f.rows[5].clear();
		require(f.project().populations[0].candidate_state != state::complete,
				"missing candidate not zero");
	}
	{
		auto f = original;
		f.rows[6].clear();
		require(f.project().populations[0].invocation_state != state::complete,
				"missing actual root");
	}
	{
		auto f = original;
		f.rows[7].clear();
		require(f.project().populations[0].invocation_state != state::complete,
				"missing actual reached call");
	}
	{
		auto f = original;
		set(f.rows[8][0], "candidate_count", detached_cell::unsigned_integer(0));
		require(f.project().populations[0].candidate_state == state::conflicting, "count conflict");
	}
	{
		auto f = original;
		f.rows[5].push_back(f.rows[5][0]);
		require(f.project().populations[0].candidate_state == state::complete,
				"identical duplicate retained");
		require(f.project().evidence.size() > positive.evidence.size(),
				"duplicate original evidence retained");
	}
	{
		auto f = original;
		f.rows[5].push_back(f.rows[5][0]);
		set(f.rows[5][1], "exclusion_disposition", detached_cell::utf8("other_exclusion"));
		require(f.project().populations[0].candidate_state == state::conflicting,
				"contradictory duplicate");
	}
	{
		auto f = original;
		set(f.rows[7][0], "compile_unit", detached_cell::utf8("foreign"));
		require(f.project().populations[0].invocation_state != state::complete,
				"foreign original unit");
	}
	{
		auto f = original;
		f.rows[7][0].presence.fragments = {"release"};
		f.rows[7][0].contributor_edges[0].condition.fragments = {"release"};
		require(f.project().populations[0].invocation_state != state::complete,
				"foreign world call");
	}
	{
		auto f = original;
		set(f.rows[6][0], "call_ids", ids({}));
		set(f.rows[6][0], "call_count", detached_cell::unsigned_integer(0));
		require(f.project().populations[0].invocation_state == state::conflicting,
				"unlisted actual call");
	}
	{
		auto f = original;
		set(f.rows[7][0], "root_ids", ids({}));
		set(f.rows[7][0], "root_count", detached_cell::unsigned_integer(0));
		require(f.project().populations[0].invocation_state == state::conflicting,
				"reverse original membership");
	}
	{
		auto f = original;
		set(f.rows[2][0], "end", detached_cell::unsigned_integer(101));
		const auto v = f.project();
		require(v.populations[0].candidates[0].source_state == state::conflicting, "source bounds");
		require(v.populations[0].candidate_state == state::complete,
				"source binding independent enumeration");
	}
	{
		auto f = original;
		set(f.rows[3][0], "provider_local_key", detached_cell::bytes({std::byte{'x'}}));
		require(f.project().populations[0].candidates[0].subject_state == state::conflicting,
				"original USR contradiction");
	}
	{
		auto f = original;
		set(f.rows[6][0], "interpreter", detached_cell::utf8("bytecode"));
		set(f.rows[6][0], "call_state", detached_cell::utf8("unsupported"));
		require(f.project().populations[0].invocation_state != state::complete,
				"bytecode not false complete");
	}
	{
		auto f = original;
		set(f.rows[6][0], "fold_failure", detached_cell::boolean(true));
		require(f.project().populations[0].invocation_state != state::complete,
				"actual failed qualifying evaluation");
	}
	{
		auto f = original;
		set(f.rows[6][0], "fold_failure", detached_cell::boolean(true));
		set(f.rows[6][0], "requested_constant_context", detached_cell::boolean(false));
		require(f.project().populations[0].invocation_state == state::complete,
				"unrelated fold failure independent");
	}
	{
		auto f = original;
		auto input = f.input();
		input.candidate_inputs_complete = false;
		const auto v = take(q::project_template_events(input));
		require(v.populations[0].candidate_state != state::complete &&
					v.populations[0].invocation_state == state::complete,
				"independent scan closure");
	}
	{
		auto f = original;
		set(f.rows[8][0],
			"candidate_count",
			detached_cell::absent(f.rows[8][0].values.at("output.candidate_count").type));
		require(f.project().populations[0].candidate_state != state::complete,
				"absent atomic facet");
	}
	{
		auto f = original;
		set(f.rows[8][0], "candidate_profile", detached_cell::utf8("future/1"));
		require(f.project().populations[0].candidate_state != state::complete,
				"future original profile");
	}
	{
		auto f = original;
		for (auto& rows : f.rows)
			std::ranges::reverse(rows);
		require(std::ranges::equal(f.project().evidence,
								   positive.evidence,
								   [](const auto& a, const auto& b)
								   {
									   return a.row.canonical_form() == b.row.canonical_form();
								   }),
				"canonical input permutation");
	}
	{
		auto f = original;
		for (int i = 0; i < 512; ++i)
		{
			auto r = f.rows[2][0];
			set(r, "span", detached_cell::utf8("decoy-" + std::to_string(i)));
			f.rows[2].push_back(std::move(r));
		}
		const auto v = f.project();
		require(v.evidence.size() == positive.evidence.size(),
				"validate unreferenced source without retention");
	}
	{
		auto f = original;
		auto r = f.rows[2][0];
		set(r, "span", detached_cell::utf8("decoy"));
		r.values.at("output.begin").type = detached_cell::utf8("bad").type;
		f.rows[2].push_back(std::move(r));
		require(!q::project_template_events(f.input()), "invalid decoy validated");
	}
	{
		q::finite_population_limits l;
		l.maximum_retained_bytes = 1;
		require(!q::project_template_events(original.input(), l), "retained quota");
	}
	{
		q::finite_population_limits l;
		l.maximum_rows = 1;
		require(!q::project_template_events(original.input(), l), "row quota");
	}
	{
		q::finite_population_limits l;
		l.maximum_members = 1;
		require(!q::project_template_events(original.input(), l), "membership quota");
	}
	{
		q::finite_population_limits l;
		l.maximum_operations = 1;
		require(!q::project_template_events(original.input(), l), "work quota");
	}
	{
		q::finite_population_limits l;
		unsigned checkpoint{};
		l.cancelled = [&]
		{
			return ++checkpoint == 7;
		};
		const auto r = q::project_template_events(original.input(), l);
		require(!r && r.error().code == "sdk.template-event-cancelled",
				"active nested cancellation");
	}
	{
		auto f = original;
		f.rows[5].clear();
		f.rows[6].clear();
		f.rows[7].clear();
		set(f.rows[8][0], "candidate_count", detached_cell::unsigned_integer(0));
		set(f.rows[8][0], "candidate_ids", ids({}));
		set(f.rows[8][0], "evaluation_root_count", detached_cell::unsigned_integer(0));
		set(f.rows[8][0], "evaluation_root_ids", ids({}));
		const auto v = f.project();
		require(v.populations[0].candidate_state == state::complete &&
					v.populations[0].invocation_state == state::complete,
				"actual finite empty distinct missing");
	}
	{
		const auto queries = original.queries();
		const auto v = take(q::project_template_events(queries));
		require(v.source_queries && v.source_queries->snapshot_id == queries.snapshot_id &&
					v.source_queries->scans.size() == 9,
				"original public query handles retained");
		require(v.populations[0].candidate_state == state::complete &&
					v.populations[0].invocation_state == state::complete,
				"public original query projection");
	}
	{
		auto queries = original.queries();
		queries.scans.erase(queries.scans.begin() + 5);
		const auto v = take(q::project_template_events(queries));
		require(v.populations[0].candidate_state != state::complete &&
					v.populations[0].invocation_state == state::complete,
				"missing independent public candidate scan");
	}
	{
		auto queries = original.queries();
		auto data = std::make_shared<q::query_result::data>();
		data->row_values = original.rows[8];
		data->status = q::execution_status::complete;
		data->input_complete = false;
		queries.scans[8].result = q::query_transfer_access::make(data);
		const auto v = take(q::project_template_events(queries));
		require(v.populations[0].candidate_state == state::complete &&
					v.populations[0].invocation_state == state::complete,
				"unrelated optional reference gaps do not erase actual atomic "
				"event census");
	}
	{
		auto queries = original.queries();
		auto data = std::make_shared<q::query_result::data>();
		data->row_values = original.rows[6];
		data->status = q::execution_status::truncated;
		data->input_complete = true;
		queries.scans[6].result = q::query_transfer_access::make(data);
		const auto v = take(q::project_template_events(queries));
		require(v.populations[0].invocation_state != state::complete &&
					v.populations[0].candidate_state == state::complete,
				"actual root scan frontier independent candidates");
	}
	{
		auto queries = original.queries();
		q::finite_population_limits l;
		l.maximum_source_queries = 1;
		require(!q::project_template_events(queries, l), "public query plan cap");
	}
	{
		auto f = original;
		const std::vector<std::byte> raw{std::byte{'u'}, std::byte{0}, std::byte{255}};
		set(f.rows[5][0], "candidate_usr", detached_cell::bytes(raw));
		std::vector<std::byte> framed;
		for (char c : std::string_view("clang-usr:"))
			framed.push_back(static_cast<std::byte>(c));
		framed.insert(framed.end(), raw.begin(), raw.end());
		set(f.rows[3][0], "provider_local_key", detached_cell::bytes(framed));
		const auto v = f.project();
		require(v.populations[0].candidates[0].candidate_usr == raw &&
					v.populations[0].candidates[0].subject_state == state::complete,
				"lossless nonUTF8 embedded NUL USR");
	}
	{
		auto f = original;
		set(f.rows[7][0], "callee_usr_hexes", ids({"bad"}));
		require(f.project().populations[0].calls[0].target_state == state::conflicting,
				"invalid raw target hex");
	}
	{
		q::finite_population_limits l;
		l.maximum_condition_expansions = 1;
		require(!q::project_template_events(original.input(), l),
				"original condition expansion quota");
	}
	{
		q::finite_population_limits l;
		l.maximum_evidence_bytes = 1;
		require(!q::project_template_events(original.input(), l), "original evidence bytes quota");
	}
	{
		const auto v = original.project();
		require(v.populations[0].calls[0].is_system == std::optional{false},
				"actual invocation source classification retained");
	}
	{
		auto f = original;
		set(f.rows[5][0], "completed", detached_cell::boolean(false));
		const auto v = f.project();
		require(v.populations[0].candidate_state != state::complete &&
					v.populations[0].invocation_state == state::complete,
				"incomplete original candidate terminal independent invocation domain");
	}
	{
		auto f = original;
		set(f.rows[8][0], "evaluation_root_state", detached_cell::utf8("partial"));
		const auto v = f.project();
		require(v.populations[0].invocation_state != state::complete &&
					v.populations[0].roots[0].invocation_state == state::complete,
				"original local root closure survives global frontier");
	}
	std::cout << "template events SDK 41 original state/binding/resource cases PASS\n";
}
