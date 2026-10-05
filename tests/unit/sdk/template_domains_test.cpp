#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>

#include <cxxlens/sdk/template_domains.hpp>

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
													"cc.template_subject.v1",
													"cc.constraint_node.v1",
													"cc.lambda_capture.v1",
													"cc.template_instantiation_frame.v1",
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
							 {"end", detached_cell::unsigned_integer(99)}})};
			rows[3] = {fact(3,
							{{"entity", detached_cell::utf8("entity")},
							 {"provider_local_key", detached_cell::bytes({std::byte{'u'}})}})};
			const auto subject = [&](std::string id, std::string kind)
			{
				return fact(
					4,
					{{"subject", detached_cell::utf8(std::move(id))},
					 {"kind", detached_cell::utf8(std::move(kind))},
					 {"compile_unit", detached_cell::utf8("unit")},
					 {"source", detached_cell::utf8("span")},
					 {"profile", detached_cell::utf8("clang22-original-template-domains/2")},
					 {"observation_state", detached_cell::utf8("complete")}});
			};
			rows[4] = {subject("implicit", "implicit_instance"),
					   subject("lambda", "lambda"),
					   subject("primary", "primary"),
					   subject("root", "constraint_root")};
			set(rows[4][0], "argument_state", detached_cell::utf8("complete"));
			set(rows[4][0],
				"argument_profile",
				detached_cell::utf8("clang22-canonical-template-argument-tuple/1"));
			set(rows[4][0], "canonical_arguments", detached_cell::bytes({std::byte{'a'}}));
			set(rows[4][0], "primary", detached_cell::utf8("primary"));
			set(rows[4][1], "capture_state", detached_cell::utf8("complete"));
			set(rows[4][1], "capture_count", detached_cell::unsigned_integer(1));
			set(rows[4][1], "capture_ids", ids({"capture"}));
			set(rows[4][1], "closure_size_bits", detached_cell::unsigned_integer(64));
			set(rows[4][3], "normalization_state", detached_cell::utf8("complete"));
			set(rows[4][3], "constraint_root", detached_cell::utf8("node"));
			rows[5] = {
				fact(5,
					 {{"node", detached_cell::utf8("node")},
					  {"compile_unit", detached_cell::utf8("unit")},
					  {"root_subject", detached_cell::utf8("root")},
					  {"path", detached_cell::utf8("0")},
					  {"kind", detached_cell::utf8("atomic")},
					  {"depth", detached_cell::unsigned_integer(1)},
					  {"profile",
					   detached_cell::utf8("clang22-sema-associated-constraint-normalization/1")},
					  {"child_count", detached_cell::unsigned_integer(0)},
					  {"mapping_state", detached_cell::utf8("complete")},
					  {"observation_state", detached_cell::utf8("complete")},
					  {"source", detached_cell::utf8("span")}})};
			rows[6] = {fact(6,
							{{"capture", detached_cell::utf8("capture")},
							 {"compile_unit", detached_cell::utf8("unit")},
							 {"lambda", detached_cell::utf8("lambda")},
							 {"kind", detached_cell::utf8("by_copy")},
							 {"profile", detached_cell::utf8("clang22-lambda-capture-storage/1")},
							 {"observation_state", detached_cell::utf8("complete")},
							 {"source", detached_cell::utf8("span")},
							 {"index", detached_cell::unsigned_integer(0)},
							 {"field_index", detached_cell::unsigned_integer(0)},
							 {"offset_bits", detached_cell::unsigned_integer(0)},
							 {"size_bits", detached_cell::unsigned_integer(32)}})};
			rows[7] = {
				fact(7,
					 {{"frame", detached_cell::utf8("frame")},
					  {"compile_unit", detached_cell::utf8("unit")},
					  {"source", detached_cell::utf8("span")},
					  {"ordinal", detached_cell::unsigned_integer(0)},
					  {"kind", detached_cell::utf8("synthesis:0")},
					  {"is_instantiation", detached_cell::boolean(true)},
					  {"depth", detached_cell::unsigned_integer(1)},
					  {"profile",
					   detached_cell::utf8("clang22-original-template-instantiation-stack/1")},
					  {"completion_state", detached_cell::utf8("ended")},
					  {"observation_state", detached_cell::utf8("complete")}})};
			rows[8] = {
				fact(8,
					 {{"inventory", detached_cell::utf8("inventory")},
					  {"compile_unit", detached_cell::utf8("unit")},
					  {"profile", detached_cell::utf8("clang22-original-template-domains/2")},
					  {"subject_count", detached_cell::unsigned_integer(4)},
					  {"subject_ids", ids({"implicit", "lambda", "primary", "root"})},
					  {"subject_state", detached_cell::utf8("complete")},
					  {"constraint_count", detached_cell::unsigned_integer(1)},
					  {"constraint_ids", ids({"node"})},
					  {"constraint_state", detached_cell::utf8("complete")},
					  {"capture_count", detached_cell::unsigned_integer(1)},
					  {"capture_ids", ids({"capture"})},
					  {"capture_state", detached_cell::utf8("complete")},
					  {"frame_count", detached_cell::unsigned_integer(1)},
					  {"frame_ids", ids({"frame"})},
					  {"frame_state", detached_cell::utf8("complete")}})};
		}
		q::template_domain_input input(bool complete = true) const
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
					complete,
					complete,
					complete};
		}
		q::application_query_results queries() const
		{
			q::application_query_results result;
			result.snapshot_id = "query:templates";
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
		q::template_domain_projection project(bool complete = true) const
		{
			return take(q::project_template_domains(input(complete)));
		}
	};
} // namespace
int main()
{
	{
		fixture f;
		set(f.rows[4][0], "kind", detached_cell::utf8("dependent_type_use"));
		require(f.project().populations[0].subjects[0].state != state::complete,
				"missing compiler constructor identity stays unavailable");
		set(f.rows[4][0], "occurrence_kind", detached_cell::utf8("type-loc:31"));
		auto p = f.project();
		require(p.populations[0].subjects[0].state == state::complete &&
					p.populations[0].subject_state == state::complete,
				"original compiler discriminator closes its independent occurrence facet");
	}
	{
		fixture f;
		set(f.rows[8][0], "profile", detached_cell::utf8("clang22-original-template-domains/1"));
		for (auto& original : f.rows[4])
			set(original, "profile", detached_cell::utf8("clang22-original-template-domains/1"));
		auto p = f.project();
		require(p.populations[0].subject_state != state::complete,
				"legacy subset cannot close full written subject admission");
		require(p.populations[0].constraint_state == state::complete &&
					p.populations[0].capture_state == state::complete &&
					p.populations[0].frame_state == state::complete &&
					p.populations[0].frames[0].stack_state == state::complete,
				"legacy subject frontier preserves independent compiler families");
		require(p.populations[0].subjects.size() == f.rows[4].size() &&
					p.populations[0].subjects[0].source_state == state::complete,
				"legacy original subject rows and source binding remain readable");
	}
	{
		fixture f;
		set(f.rows[4][0], "profile", detached_cell::utf8("clang22-original-template-domains/1"));
		require(f.project().populations[0].subject_state != state::complete,
				"full inventory cannot promote a legacy subject admission");
	}
	{
		fixture f;
		std::vector<std::byte> original{std::byte{'u'}, std::byte{0}, std::byte{255}};
		set(f.rows[6][0], "captured_usr", detached_cell::bytes(original));
		set(f.rows[6][0], "captured_entity", detached_cell::utf8("entity"));
		std::vector<std::byte> framed;
		for (const char c : std::string_view{"clang-usr:"})
			framed.push_back(static_cast<std::byte>(c));
		framed.insert(framed.end(), original.begin(), original.end());
		set(f.rows[3][0], "provider_local_key", detached_cell::bytes(framed));
		auto p = f.project();
		require(p.populations[0].captures[0].entity_state == state::complete &&
					p.populations[0].captures[0].captured_usr.size() == 3U,
				"native capture joins exact framed provider declaration identity");
		set(f.rows[3][0], "provider_local_key", detached_cell::bytes(original));
		require(f.project().populations[0].captures[0].entity_state == state::conflicting,
				"unframed captured identity does not alias original declaration");
		f.rows[3][0].values.erase("output.provider_local_key");
		require(f.project().populations[0].captures[0].entity_state != state::complete,
				"missing captured declaration identity remains unavailable");
		require(f.project().populations[0].captures[0].layout_state == state::complete,
				"independent original capture storage stays known");
	}

	{
		fixture f;
		set(f.rows[4][1], "owner_entity", detached_cell::utf8("entity"));
		auto p = f.project();
		require(p.populations[0].subjects[1].owner_entity_state == state::complete &&
					p.populations[0].subjects[1].owner_state != state::complete,
				"enclosing function owner independent from optional template owner");
		auto conflicting = f.rows[3][0];
		set(conflicting, "kind", detached_cell::utf8("function"));
		f.rows[3].push_back(conflicting);
		require(f.project().populations[0].subjects[1].owner_entity_state == state::conflicting,
				"contradictory original enclosing entity remains conflicting");
		f.rows[3].clear();
		require(f.project().populations[0].subjects[1].owner_entity_state != state::complete,
				"missing enclosing entity stays unknown");
	}

	{
		fixture f;
		std::vector<std::byte> original{std::byte{'u'}, std::byte{0}, std::byte{255}};
		set(f.rows[4][0], "semantic_usr", detached_cell::bytes(original));
		set(f.rows[4][0], "entity", detached_cell::utf8("entity"));
		std::vector<std::byte> framed;
		for (const char c : std::string_view{"clang-usr:"})
			framed.push_back(static_cast<std::byte>(c));
		framed.insert(framed.end(), original.begin(), original.end());
		set(f.rows[3][0], "provider_local_key", detached_cell::bytes(framed));
		auto p = f.project();
		require(p.populations[0].subjects[0].entity_state == state::complete &&
					p.populations[0].subjects[0].semantic_usr.size() == 3U,
				"native original raw USR joins exact existing framed provider identity");
		set(f.rows[3][0], "provider_local_key", detached_cell::bytes(original));
		require(f.project().populations[0].subjects[0].entity_state == state::conflicting,
				"unframed identity cannot silently alias native original USR");
		f.rows[3][0].values.erase("output.provider_local_key");
		require(f.project().populations[0].subjects[0].entity_state != state::complete,
				"missing original identity differs from known mismatch");
	}

	for (const bool public_queries : {false, true})
	{
		fixture f;
		for (unsigned i{}; i < 512U; ++i)
		{
			auto decoy = f.rows[2][0];
			set(decoy, "span", detached_cell::utf8("unused-" + std::to_string(i)));
			f.rows[2].push_back(std::move(decoy));
		}
		q::finite_population_limits limits;
		limits.maximum_retained_bytes = 1024U * 1024U;
		auto run = [&]
		{
			return public_queries ? q::project_template_domains(f.queries(), limits)
								  : q::project_template_domains(f.input(), limits);
		};
		auto p = take(run());
		require(p.populations[0].subjects[0].source_state == state::complete,
				"unrelated source rows do not consume owned domain storage");
		require(std::ranges::count(p.evidence,
								   std::string{"source.span.v1"},
								   [](const auto& e)
								   {
									   return e.relation_id;
								   }) == 1,
				"retain exact referenced original source rows only");
		if (public_queries)
			require(p.source_queries &&
						q::query_transfer_access::borrow_rows(p.source_queries->scans[2].result)
								.size() == 513U,
					"all original public rows remain available through saved handle");
		f.rows[2].back().values.erase("output.begin");
		require(!run(), "malformed unreferenced source row still fails validation");
	}
	for (const bool public_queries : {false, true})
	{
		fixture f;
		auto duplicate = f.rows[2][0];
		f.rows[2].push_back(duplicate);
		auto run = [&]
		{
			return public_queries ? q::project_template_domains(f.queries())
								  : q::project_template_domains(f.input());
		};
		auto p = take(run());
		require(p.populations[0].subjects[0].source_state == state::complete,
				"equal source duplicate retains independent complete binding");
		require(std::ranges::count(p.evidence,
								   std::string{"source.span.v1"},
								   [](const auto& e)
								   {
									   return e.relation_id;
								   }) == 2,
				"equal source witnesses both retained");
		set(f.rows[2].back(), "end", detached_cell::unsigned_integer(98));
		require(take(run()).populations[0].subjects[0].source_state == state::conflicting,
				"actual relevant source contradiction remains conflicting");
	}
	{
		fixture f;
		auto unused = f.rows[2][0];
		set(unused, "span", detached_cell::utf8("unused"));
		f.rows[2].push_back(unused);
		q::finite_population_limits limits;
		limits.maximum_rows = 1U;
		auto p = q::project_template_domains(f.input(), limits);
		require(!p && p.error().code == "sdk.template-budget", "all input rows charged");
		limits.maximum_rows = 100U;
		limits.maximum_condition_expansions = 1U;
		p = q::project_template_domains(f.input(), limits);
		require(!p && p.error().code == "sdk.template-budget", "all input conditions charged");
	}

	{
		fixture f;
		const auto value = f.project();
		const auto& p = value.populations.at(0);
		require(p.subject_state == state::complete && p.constraint_state == state::complete &&
					p.capture_state == state::complete && p.frame_state == state::complete,
				"complete independent census");
		require(p.subjects.at(0).argument_binding_state == state::complete, "canonical args");
		require(p.constraints.at(0).tree_state == state::complete, "normalized tree");
		require(p.captures.at(0).layout_state == state::complete, "capture storage");
		require(p.frames.at(0).stack_state == state::complete, "original stack");
	}
	{
		fixture f;
		auto p = f.project(false);
		require(p.populations.at(0).subject_state != state::complete,
				"raw closure defaults unknown");
	}
	{
		fixture f;
		f.rows[8].clear();
		auto p = f.project();
		require(p.populations.size() == 1 && p.populations[0].subject_state != state::complete,
				"missing actual inventory retained");
	}
	{
		fixture f;
		f.rows[4].clear();
		auto p = f.project();
		require(p.populations[0].subject_state != state::complete,
				"missing listed members unknown");
	}
	{
		fixture f;
		set(f.rows[8][0], "subject_count", detached_cell::unsigned_integer(3));
		require(f.project().populations[0].subject_state == state::conflicting,
				"count contradiction");
	}
	{
		fixture f;
		auto row = f.rows[4][0];
		set(row, "subject", detached_cell::utf8("extra"));
		f.rows[4].push_back(row);
		require(f.project().populations[0].subject_state == state::conflicting,
				"extra original member");
	}
	{
		fixture f;
		f.rows[4].push_back(f.rows[4][0]);
		auto p = f.project();
		require(p.populations[0].subject_state == state::complete &&
					p.populations[0].subjects[0].evidence.size() > 1,
				"equal duplicates preserve witnesses");
	}
	{
		fixture f;
		auto row = f.rows[4][0];
		set(row, "argument_state", detached_cell::utf8("partial"));
		f.rows[4].push_back(row);
		require(f.project().populations[0].subject_state == state::conflicting,
				"contradictory duplicate");
	}
	{
		fixture f;
		set(f.rows[4][0], "argument_state", detached_cell::utf8("partial"));
		const auto p = f.project();
		require(p.populations[0].subject_state == state::complete &&
					p.populations[0].subjects[0].argument_binding_state != state::complete,
				"argument facet independent");
	}
	{
		fixture f;
		f.rows[4][0].values.erase("output.canonical_arguments");
		require(f.project().populations[0].subjects[0].argument_binding_state != state::complete,
				"missing optional tuple unavailable");
	}
	{
		fixture f;
		set(f.rows[6][0], "size_bits", detached_cell::unsigned_integer(65));
		require(f.project().populations[0].captures[0].layout_state == state::conflicting,
				"extent outside closure");
	}
	{
		fixture f;
		set(f.rows[6][0], "observation_state", detached_cell::utf8("partial"));
		auto p = f.project();
		require(p.populations[0].capture_state == state::complete &&
					p.populations[0].captures[0].layout_state != state::complete,
				"known captures partial layout");
	}
	{
		fixture f;
		set(f.rows[5][0], "child_count", detached_cell::unsigned_integer(1));
		require(f.project().populations[0].constraints[0].tree_state == state::conflicting,
				"atomic arity contradiction");
	}
	{
		fixture f;
		set(f.rows[5][0], "depth", detached_cell::unsigned_integer(2));
		require(f.project().populations[0].subjects.back().normalization_binding_state ==
					state::conflicting,
				"root depth original");
	}
	{
		fixture f;
		set(f.rows[7][0], "completion_state", detached_cell::utf8("partial"));
		auto p = f.project();
		require(p.populations[0].frame_state == state::complete &&
					p.populations[0].frames[0].stack_state != state::complete,
				"known enumeration incomplete stack");
	}
	{
		fixture f;
		set(f.rows[7][0], "parent", detached_cell::utf8("frame"));
		require(f.project().populations[0].frames[0].stack_state == state::conflicting,
				"parent cycle conflict");
	}
	{
		fixture f;
		set(f.rows[0][0], "variant", detached_cell::utf8("release"));
		require(f.project().populations[0].subject_state == state::conflicting,
				"actual world differs");
	}
	{
		fixture f;
		f.rows[4][0].presence.fragments = {"release"};
		f.rows[4][0].contributor_edges[0].condition = f.rows[4][0].presence;
		require(f.project().populations[0].subject_state != state::complete,
				"foreign world cannot fill member");
	}
	{
		fixture f;
		set(f.rows[2][0], "end", detached_cell::unsigned_integer(101));
		auto p = f.project();
		require(p.populations[0].subject_state == state::complete &&
					p.populations[0].subjects[0].source_state == state::conflicting,
				"source bounds independent");
	}
	{
		fixture f;
		q::finite_population_limits limits;
		limits.maximum_rows = 1;
		auto p = q::project_template_domains(f.input(), limits);
		require(!p && p.error().code == "sdk.template-budget", "row budget");
	}
	{
		fixture f;
		unsigned polls{};
		q::finite_population_limits limits;
		limits.cancelled = [&]
		{
			return ++polls == 9;
		};
		auto p = q::project_template_domains(f.input(), limits);
		require(!p && p.error().code == "sdk.template-cancelled", "active cancellation");
	}
	{
		fixture f;
		for (std::size_t group = 4; group < 8; ++group)
			f.rows[group].clear();
		for (const auto prefix : {"subject", "constraint", "capture", "frame"})
		{
			set(f.rows[8][0], std::string(prefix) + "_count", detached_cell::unsigned_integer(0));
			set(f.rows[8][0], std::string(prefix) + "_ids", ids({}));
		}
		const auto p = f.project();
		require(p.populations[0].subject_state == state::complete &&
					p.populations[0].subjects.empty(),
				"explicit known empty");
	}
	{
		fixture f;
		auto input = f.input(false);
		input.compile_units_complete = input.inventory_inputs_complete =
			input.subject_inputs_complete = true;
		auto p = take(q::project_template_domains(input));
		require(p.populations[0].subject_state == state::complete &&
					p.populations[0].constraint_state != state::complete,
				"independent family scan closure");
	}

	{
		fixture f;
		f.rows[5][0].values.erase("output.parameter_mapping");
		const auto p = f.project();
		require(p.populations[0].constraints[0].tree_state == state::complete &&
					p.populations[0].constraints[0].mapping_binding_state != state::complete,
				"unobserved mapping differs from original known absent");
	}
	{
		fixture f;
		set(f.rows[5][0], "root_subject", detached_cell::utf8("primary"));
		require(f.project().populations[0].constraints[0].tree_state == state::conflicting,
				"normalization owner must be actual constraint root");
	}
	{
		fixture f;
		set(f.rows[5][0], "observation_state", detached_cell::utf8("partial"));
		const auto p = f.project();
		require(p.populations[0].constraint_state == state::complete &&
					p.populations[0].subjects.back().normalization_binding_state != state::complete,
				"original partial normalized tree does not erase census");
	}
	{
		fixture f;
		auto member = f.rows[5][0];
		set(member, "node", detached_cell::utf8("orphan"));
		set(member, "path", detached_cell::utf8("0.0"));
		set(member, "depth", detached_cell::unsigned_integer(2));
		f.rows[5].push_back(member);
		set(f.rows[8][0], "constraint_count", detached_cell::unsigned_integer(2));
		set(f.rows[8][0], "constraint_ids", ids({"node", "orphan"}));
		require(f.project().populations[0].constraints[1].tree_state == state::conflicting,
				"original parent must admit the child edge");
	}
	{
		fixture f;
		auto member = f.rows[6][0];
		set(member, "capture", detached_cell::utf8("capture2"));
		f.rows[6].push_back(member);
		set(f.rows[8][0], "capture_count", detached_cell::unsigned_integer(2));
		set(f.rows[8][0], "capture_ids", ids({"capture", "capture2"}));
		set(f.rows[4][1], "capture_count", detached_cell::unsigned_integer(2));
		set(f.rows[4][1], "capture_ids", ids({"capture", "capture2"}));
		const auto p = f.project();
		require(p.populations[0].capture_state == state::complete &&
					p.populations[0].captures[0].layout_state == state::conflicting &&
					p.populations[0].captures[1].layout_state == state::conflicting,
				"two actual IDs cannot claim the same original capture field");
	}
	{
		fixture f;
		set(f.rows[7][0], "depth", detached_cell::unsigned_integer(0));
		require(f.project().populations[0].frames[0].stack_state == state::conflicting,
				"actual root instantiation depth starts at one");
	}
	{
		fixture f;
		set(f.rows[7][0], "ordinal", detached_cell::unsigned_integer(1));
		require(f.project().populations[0].frames[0].stack_state == state::conflicting,
				"ordinal must belong to the original finite frame census");
	}
	{
		fixture f;
		const auto queries = f.queries();
		const auto p = take(q::project_template_domains(queries));
		require(p.source_queries && p.source_queries->snapshot_id == queries.snapshot_id &&
					p.source_queries->scans.size() == queries.scans.size() &&
					p.populations[0].subject_state == state::complete,
				"original public query wrapper retains original source plans and results");
	}
	{
		fixture f;
		auto queries = f.queries();
		std::erase_if(queries.scans,
					  [](const auto& scan)
					  {
						  return scan.relation_id == "cc.lambda_capture.v1";
					  });
		const auto p = take(q::project_template_domains(queries));
		require(p.populations[0].subject_state == state::complete &&
					p.populations[0].capture_state != state::complete,
				"missing unrelated family does not erase known subject census");
	}
	{
		fixture f;
		q::finite_population_limits limits;
		limits.maximum_retained_bytes = 1;
		const auto p = q::project_template_domains(f.queries(), limits);
		require(!p && p.error().code == "sdk.template-budget",
				"wrapper preflights retained input bytes");
	}
	{
		fixture f;
		const auto before = f.project();
		for (auto& group : f.rows)
			std::ranges::reverse(group);
		const auto after = f.project();
		require(before.populations[0].subject_ids == after.populations[0].subject_ids &&
					before.evidence.size() == after.evidence.size() &&
					std::ranges::equal(before.evidence,
									   after.evidence,
									   [](const auto& a, const auto& b)
									   {
										   return a.relation_id == b.relation_id &&
											   a.row.canonical_form() == b.row.canonical_form();
									   }),
				"permutation preserves original evidence and membership");
	}
	std::cout << "template domains 34 cases PASS\n";
}
