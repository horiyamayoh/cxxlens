#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>

#include <cxxlens/sdk/object_semantics.hpp>

#include "query_result_internal.hpp"
namespace
{
	using namespace cxxlens::sdk;
	namespace q = cxxlens::sdk::query;
	using state = q::finite_population_state;
	constexpr std::array<std::string_view, 17> names{"build.compile_unit.v1",
													 "source.file.v1",
													 "source.span.v1",
													 "cc.entity.v1",
													 "cc.entity_detail.v1",
													 "cc.declaration.v1",
													 "cc.body.v1",
													 "cc.cfg_node.v1",
													 "cc.cfg_edge.v1",
													 "cc.type.v1",
													 "cc.syntax_node.v1",
													 "cc.sequence_context.v1",
													 "cc.sequence_pair.v1",
													 "cc.object_state_observation.v1",
													 "cc.declaration_inventory.v1",
													 "cc.constant_evaluation_root.v1",
													 "cc.template_inventory.v1"};
	void require(bool v, std::string_view message)
	{
		if (!v)
		{
			std::cerr << message << '\n';
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
	std::vector<std::byte> raw(std::string_view s)
	{
		std::vector<std::byte> out;
		for (char c : s)
			out.push_back(static_cast<std::byte>(c));
		return out;
	}
	detached_cell ids(std::initializer_list<std::string_view> members)
	{
		std::vector<std::byte> v;
		for (auto member : members)
		{
			for (unsigned shift{}; shift < 32U; shift += 8U)
				v.push_back(static_cast<std::byte>((member.size() >> shift) & 255U));
			for (char c : member)
				v.push_back(static_cast<std::byte>(c));
		}
		return detached_cell::bytes(std::move(v));
	}
	q::annotated_row fact(std::size_t group,
						  std::initializer_list<std::pair<std::string, detached_cell>> values)
	{
		q::annotated_row row;
		row.presence = {"object:test", {"debug"}};
		row.interpretation = "clang22";
		row.claim_contributors = {"claim:object"};
		row.producer_contracts = {{"object.fixture", "semantic:original"}};
		row.provenance = {"object:source"};
		row.contributor_guarantees = {
			{"exact", "original-scopes", "fixture", {"schema_validated"}}};
		row.contributor_edges = {{row.claim_contributors.front(),
								  row.producer_contracts.front(),
								  row.provenance.front(),
								  row.contributor_guarantees.front(),
								  row.presence,
								  row.interpretation}};
		const auto descriptors = standard_relation_descriptors();
		const auto d = std::ranges::find(descriptors, names[group], &relation_descriptor::id);
		require(d != descriptors.end(), "original descriptor missing");
		for (const auto& c : d->columns)
		{
			auto v = detached_cell::utf8("fixture");
			if (c.type.optional)
				v = detached_cell::absent(c.type);
			else if (c.type.scalar == scalar_kind::unsigned_integer)
				v = detached_cell::unsigned_integer(0);
			else if (c.type.scalar == scalar_kind::boolean)
				v = detached_cell::boolean(false);
			else if (c.type.scalar == scalar_kind::digest)
				v = detached_cell::utf8(content_digest({}));
			else if (c.type.scalar == scalar_kind::set || c.type.scalar == scalar_kind::bytes)
				v = ids({});
			else if (c.type.scalar == scalar_kind::closed_symbol)
				v = detached_cell::utf8("canonicalized");
			v.type = c.type;
			row.values.emplace("output." + c.name, std::move(v));
		}
		for (const auto& [name, v] : values)
		{
			auto copy = v;
			auto column = row.values.find("output." + name);
			if (column == row.values.end())
			{
				std::cerr << "original fixture column absent: " << names[group] << ':' << name
						  << '\n';
				std::exit(1);
			}
			copy.type = column->second.type;
			row.values["output." + name] = std::move(copy);
		}
		return row;
	}
	void set(q::annotated_row& r, std::string_view name, detached_cell value)
	{
		const auto field = "output." + std::string{name};
		value.type = r.values.at(field).type;
		r.values[field] = std::move(value);
	}
	struct fixture
	{
		std::array<std::vector<q::annotated_row>, 17> rows;
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
			rows[3] = {
				fact(3,
					 {{"entity", detached_cell::utf8("function")},
					  {"provider_local_key", detached_cell::bytes(raw("clang-usr:function"))}}),
				fact(3,
					 {{"entity", detached_cell::utf8("storage")},
					  {"provider_local_key", detached_cell::bytes(raw("clang-usr:storage"))}})};
			rows[5] = {fact(5,
							{{"declaration", detached_cell::utf8("fn-decl")},
							 {"entity", detached_cell::utf8("function")},
							 {"source", detached_cell::utf8("span")}}),
					   fact(5,
							{{"declaration", detached_cell::utf8("var-decl")},
							 {"entity", detached_cell::utf8("storage")},
							 {"source", detached_cell::utf8("span")}})};
			for (auto id : {"left", "right", "invoke", "arg-left", "arg-right"})
				rows[10].push_back(fact(10,
										{{"node", detached_cell::utf8(id)},
										 {"compile_unit", detached_cell::utf8("unit")},
										 {"function", detached_cell::utf8("function")},
										 {"source", detached_cell::utf8("span")}}));
			rows[12] = {fact(
				12,
				{{"pair", detached_cell::utf8("pair")},
				 {"compile_unit", detached_cell::utf8("unit")},
				 {"source", detached_cell::utf8("span")},
				 {"owner_usr", detached_cell::bytes(raw("function"))},
				 {"owner_entity", detached_cell::utf8("function")},
				 {"declaration", detached_cell::utf8("fn-decl")},
				 {"expression", detached_cell::utf8("left")},
				 {"profile", detached_cell::utf8("clang22-original-sequence-checker-candidates/1")},
				 {"membership_state", detached_cell::utf8("complete")},
				 {"binding_state", detached_cell::utf8("complete")},
				 {"checker_root", detached_cell::unsigned_integer(1)},
				 {"left_expression", detached_cell::utf8("left")},
				 {"right_expression", detached_cell::utf8("right")},
				 {"left_source", detached_cell::utf8("span")},
				 {"right_source", detached_cell::utf8("span")},
				 {"left_context_state", detached_cell::utf8("complete")},
				 {"right_context_state", detached_cell::utf8("complete")},
				 {"storage_usr", detached_cell::bytes(raw("storage"))},
				 {"storage_declaration", detached_cell::utf8("var-decl")},
				 {"storage_entity", detached_cell::utf8("storage")},
				 {"left_modifies", detached_cell::boolean(true)},
				 {"right_modifies", detached_cell::boolean(true)},
				 {"scalar_nonreference_storage", detached_cell::boolean(true)},
				 {"jointly_potentially_evaluated", detached_cell::boolean(true)},
				 {"checker_relation", detached_cell::utf8("unsequenced")},
				 {"language", detached_cell::utf8("c++23")}})};
			rows[14] = {
				fact(14,
					 {{"inventory", detached_cell::utf8("inventory")},
					  {"compile_unit", detached_cell::utf8("unit")},
					  {"declarations", ids({"fn-decl", "var-decl"})},
					  {"declaration_count", detached_cell::unsigned_integer(2)},
					  {"sequence_pair_count", detached_cell::unsigned_integer(1)},
					  {"sequence_pair_ids", ids({"pair"})},
					  {"sequence_pair_state", detached_cell::utf8("complete")},
					  {"sequence_pair_profile",
					   detached_cell::utf8("clang22-original-sequence-checker-candidates/1")},
					  {"object_state_count", detached_cell::unsigned_integer(0)},
					  {"object_state_ids", ids({})},
					  {"object_state_state", detached_cell::utf8("complete")},
					  {"object_state_profile",
					   detached_cell::utf8("clang22-legacy-interpreter-object-state/1")}})};
		}
		q::object_semantics_input input()
		{
			return {rows[0],  rows[1],	rows[2], rows[3],  rows[4],	 rows[5],  rows[6],
					rows[7],  rows[8],	rows[9], rows[10], rows[11], rows[12], rows[13],
					rows[14], rows[15], true,	 true,	   true,	 true,	   true,
					true,	  true,		true,	 true,	   true};
		}
		void argument_contexts()
		{
			for (unsigned i{}; i < 2U; ++i)
			{
				const bool left = i == 0U;
				const auto id = left ? "ctx-left" : "ctx-right";
				rows[11].push_back(
					fact(11,
						 {{"context", detached_cell::utf8(id)},
						  {"compile_unit", detached_cell::utf8("unit")},
						  {"source", detached_cell::utf8("span")},
						  {"owner_entity", detached_cell::utf8("function")},
						  {"expression", detached_cell::utf8(left ? "left" : "right")},
						  {"profile",
						   detached_cell::utf8("clang22-original-argument-sequencing-context/1")},
						  {"membership_state", detached_cell::utf8("complete")},
						  {"binding_state", detached_cell::utf8("complete")},
						  {"checker_root", detached_cell::unsigned_integer(1)},
						  {"access_ordinal", detached_cell::unsigned_integer(i + 1U)},
						  {"depth", detached_cell::unsigned_integer(0)},
						  {"invocation", detached_cell::utf8("invoke")},
						  {"argument", detached_cell::utf8(left ? "arg-left" : "arg-right")},
						  {"argument_index", detached_cell::unsigned_integer(i)},
						  {"indeterminately_sequenced", detached_cell::boolean(true)},
						  {"language", detached_cell::utf8("c++23")}}));
				set(rows[12][0], left ? "left_context" : "right_context", detached_cell::utf8(id));
			}
		}
		void exit_fact()
		{
			for (auto id : {"body-span", "prototype-span"})
			{
				auto span = rows[2].front();
				set(span, "span", detached_cell::utf8(id));
				rows[2].push_back(std::move(span));
			}
			rows[5].push_back(fact(5,
								   {{"declaration", detached_cell::utf8("prototype")},
									{"entity", detached_cell::utf8("function")},
									{"source", detached_cell::utf8("prototype-span")}}));
			set(rows[14][0], "declarations", ids({"fn-decl", "prototype", "var-decl"}));
			set(rows[14][0], "declaration_count", detached_cell::unsigned_integer(3));
			rows[4] = {fact(4,
							{{"entity", detached_cell::utf8("function")},
							 {"compile_unit", detached_cell::utf8("unit")},
							 {"source", detached_cell::utf8("span")},
							 {"return_kind", detached_cell::utf8("value")},
							 {"is_main", detached_cell::boolean(false)},
							 {"is_coroutine", detached_cell::boolean(false)},
							 {"function_exit_profile",
							  detached_cell::utf8("clang22-original-function-exits/1")},
							 {"function_exit_state", detached_cell::utf8("complete")}})};
			rows[4].push_back(rows[4].front());
			set(rows[4].back(), "source", detached_cell::utf8("prototype-span"));
			set(rows[4].back(), "return_kind", detached_cell::utf8("void"));
			rows[6] = {fact(6,
							{{"body", detached_cell::utf8("body")},
							 {"function", detached_cell::utf8("function")},
							 {"compile_unit", detached_cell::utf8("unit")},
							 {"source", detached_cell::utf8("body-span")},
							 {"entry", detached_cell::utf8("from")},
							 {"exit", detached_cell::utf8("to")},
							 {"function_exit_declaration", detached_cell::utf8("fn-decl")}})};
			for (auto id : {"from", "to"})
				rows[7].push_back(fact(7,
									   {{"node", detached_cell::utf8(id)},
										{"compile_unit", detached_cell::utf8("unit")},
										{"function", detached_cell::utf8("function")},
										{"body", detached_cell::utf8("body")}}));
			rows[8] = {fact(8,
							{{"edge", detached_cell::utf8("edge")},
							 {"compile_unit", detached_cell::utf8("unit")},
							 {"function", detached_cell::utf8("function")},
							 {"from", detached_cell::utf8("from")},
							 {"to", detached_cell::utf8("to")},
							 {"function_exit_declaration", detached_cell::utf8("fn-decl")},
							 {"function_exit_kind", detached_cell::utf8("fallthrough")},
							 {"function_exit_state", detached_cell::utf8("complete")},
							 {"function_exit_profile",
							  detached_cell::utf8("clang22-original-function-exits/1")}})};
		}
		void value_fact(std::string_view kind, std::string_view profile)
		{
			rows[9] = {fact(9, {{"type", detached_cell::utf8("input-type")}}),
					   fact(9,
							{{"type", detached_cell::utf8("result-type")},
							 {"enum_fixed_underlying", detached_cell::boolean(false)},
							 {"enum_value_lower", detached_cell::utf8("0")},
							 {"enum_value_upper", detached_cell::utf8("7")},
							 {"enum_value_profile",
							  detached_cell::utf8("clang22-original-enum-value-domain/1")},
							 {"enum_value_state", detached_cell::utf8("complete")}})};
			set(rows[10][0], "operand", detached_cell::utf8("right"));
			set(rows[10][0], "operand_type", detached_cell::utf8("input-type"));
			set(rows[10][0], "canonical_type", detached_cell::utf8("result-type"));
			set(rows[10][1], "canonical_type", detached_cell::utf8("input-type"));
			set(rows[10][0], "object_scope_declaration", detached_cell::utf8("fn-decl"));
			set(rows[10][0], "object_fact_kind", detached_cell::utf8(std::string{kind}));
			set(rows[10][0], "object_fact_profile", detached_cell::utf8(std::string{profile}));
			set(rows[10][0], "object_fact_state", detached_cell::utf8("complete"));
			set(rows[10][0], "original_value_lower", detached_cell::utf8("8"));
			set(rows[10][0], "original_value_upper", detached_cell::utf8("8"));
			set(rows[10][0], "canonical_bool", detached_cell::boolean(true));
			if (kind.starts_with("bool_"))
			{
				set(rows[9][1], "builtin_kind", detached_cell::utf8("Bool"));
				set(rows[9][1],
					"builtin_profile",
					detached_cell::utf8("clang22-original-builtin-type/1"));
				set(rows[9][1], "builtin_state", detached_cell::utf8("complete"));
			}
			set(rows[10][0], "representation_state", detached_cell::utf8("complete"));
		}
		void direct_fact()
		{
			value_fact("direct_type_access", "clang22-original-direct-type-access/1");
			set(rows[10][0], "object_declaration", detached_cell::utf8("var-decl"));
			set(rows[10][0], "object_entity", detached_cell::utf8("storage"));
			set(rows[10][0], "declared_object_type", detached_cell::utf8("input-type"));
			set(rows[10][0], "object_address", detached_cell::utf8("invoke"));
			set(rows[10][0], "type_access_permission", detached_cell::boolean(false));
			set(rows[10][0],
				"current_object_profile",
				detached_cell::utf8("clang22-owned-object-initialization-before-address/1"));
			set(rows[10][0], "current_object_state", detached_cell::utf8("live_unreplaced"));
		}

		void lifetime_fact()
		{
			exit_fact();
			rows[9] = {fact(9, {{"type", detached_cell::utf8("record-type")}})};
			set(rows[7][0], "element_count", detached_cell::unsigned_integer(1U));
			auto& use = rows[10][0];
			for (auto [name, value] : std::initializer_list<std::pair<std::string, std::string>>{
					 {"kind", "CXXMemberCallExpr"},
					 {"object_scope_declaration", "fn-decl"},
					 {"object_fact_kind", "direct_member_lifetime"},
					 {"object_fact_profile", "clang22-original-direct-record-member-lifetime/1"},
					 {"object_fact_state", "complete"},
					 {"object_declaration", "var-decl"},
					 {"object_entity", "storage"},
					 {"object_receiver", "right"},
					 {"declared_object_type", "record-type"},
					 {"object_body", "body"},
					 {"object_cfg_node", "from"},
					 {"object_phase", "ended"}})
				set(use, name, detached_cell::utf8(value));
			set(use, "object_cfg_element_index", detached_cell::unsigned_integer(0U));
			set(use, "actual_polymorphic_use", detached_cell::boolean(true));
			auto& receiver = rows[10][1];
			set(receiver, "kind", detached_cell::utf8("DeclRefExpr"));
			set(receiver, "object_declaration", detached_cell::utf8("var-decl"));
			set(receiver, "object_entity", detached_cell::utf8("storage"));
			set(receiver, "canonical_type", detached_cell::utf8("record-type"));
		}
		void object_state()
		{
			auto path = take(q::encode_compiler_object_path({}));
			rows[9] = {fact(9, {{"type", detached_cell::utf8("type")}})};
			rows[15] = {
				fact(15,
					 {{"root", detached_cell::utf8("root")},
					  {"compile_unit", detached_cell::utf8("unit")},
					  {"requested_constant_context", detached_cell::boolean(true)},
					  {"potential_check", detached_cell::boolean(false)},
					  {"mode", detached_cell::utf8("constant_expression")},
					  {"interpreter", detached_cell::utf8("legacy")},
					  {"completion", detached_cell::utf8("complete")},
					  {"profile", detached_cell::utf8("clang22-legacy-constant-evaluation/1")}})};
			rows[13] = {
				fact(13,
					 {{"observation", detached_cell::utf8("state")},
					  {"compile_unit", detached_cell::utf8("unit")},
					  {"source", detached_cell::utf8("span")},
					  {"owner_usr", detached_cell::bytes(raw("function"))},
					  {"owner_entity", detached_cell::utf8("function")},
					  {"declaration", detached_cell::utf8("fn-decl")},
					  {"expression", detached_cell::utf8("left")},
					  {"profile", detached_cell::utf8("clang22-legacy-interpreter-object-state/1")},
					  {"membership_state", detached_cell::utf8("complete")},
					  {"binding_state", detached_cell::utf8("complete")},
					  {"evaluation_root", detached_cell::utf8("root")},
					  {"state_kind", detached_cell::utf8("inactive_union_member")},
					  {"access_kind", detached_cell::utf8("read")},
					  {"storage_kind", detached_cell::utf8("declaration")},
					  {"storage_usr", detached_cell::bytes(raw("storage"))},
					  {"storage_declaration", detached_cell::utf8("var-decl")},
					  {"storage_entity", detached_cell::utf8("storage")},
					  {"call_index", detached_cell::unsigned_integer(1)},
					  {"generation", detached_cell::unsigned_integer(0)},
					  {"storage_path", detached_cell::bytes(std::move(path))},
					  {"complete_type", detached_cell::utf8("type")},
					  {"subobject_type", detached_cell::utf8("type")},
					  {"active_member_state", detached_cell::utf8("active")},
					  {"active_field_usr", detached_cell::bytes(raw("active"))},
					  {"selected_field_usr", detached_cell::bytes(raw("selected"))},
					  {"common_initial_sequence_permitted", detached_cell::boolean(false)},
					  {"storage_state", detached_cell::utf8("complete")},
					  {"semantic_state", detached_cell::utf8("complete")}})};
			set(rows[14][0], "object_state_count", detached_cell::unsigned_integer(1));
			set(rows[14][0], "object_state_ids", ids({"state"}));
		}
	};
	const q::original_sequence_pair& sequence(const q::object_semantics_projection& out)
	{
		return std::get<q::original_sequence_pair>(out.observations.front().fact);
	}
	const q::original_union_access& union_access(const q::object_semantics_projection& out)
	{
		const auto at = std::ranges::find(
			out.observations, "inactive_union_member", &q::original_object_observation::kind);
		require(at != out.observations.end(), "union original absent");
		return std::get<q::original_union_access>(at->fact);
	}
	const q::original_object_observation& observed(const q::object_semantics_projection& out,
												   std::string_view kind)
	{
		const auto at =
			std::ranges::find(out.observations, kind, &q::original_object_observation::kind);
		require(at != out.observations.end(), "original facet absent");
		return *at;
	}
} // namespace
int main()
{
	unsigned cases{};
	{
		fixture f;
		auto out = take(q::project_object_semantics(f.input()));
		require(out.observations.size() == 1 && sequence(out).storage_state == state::complete &&
					sequence(out).sequencing_state == state::complete &&
					out.populations[0].enumeration_state == state::complete,
				"original sequence closure");
		++cases;
	}
	{
		fixture f;
		f.argument_contexts();
		auto out = take(q::project_object_semantics(f.input()));
		require(sequence(out).sequencing == "indeterminately_sequenced" &&
					sequence(out).context_state == state::complete,
				"ordinary arguments mislabeled unsequenced");
		++cases;
	}
	{
		fixture f;
		f.argument_contexts();
		set(f.rows[11][1], "argument_index", detached_cell::unsigned_integer(0));
		auto out = take(q::project_object_semantics(f.input()));
		require(sequence(out).sequencing == "unsequenced", "same argument acquired exclusion");
		++cases;
	}
	{
		fixture f;
		f.argument_contexts();
		set(f.rows[11][0], "parent_context", detached_cell::utf8("ctx-left"));
		auto out = take(q::project_object_semantics(f.input()));
		require(sequence(out).context_state == state::conflicting, "context cycle complete");
		++cases;
	}
	{
		fixture f;
		f.argument_contexts();
		f.rows[11][0].presence.fragments = {"release"};
		f.rows[11][0].contributor_edges.front().condition = f.rows[11][0].presence;
		auto out = take(q::project_object_semantics(f.input()));
		require(sequence(out).context_state != state::complete, "foreign context admitted");
		++cases;
	}
	{
		fixture f;
		f.rows[12].push_back(f.rows[12][0]);
		f.rows[12][1].claim_contributors = {"claim:duplicate"};
		f.rows[12][1].contributor_edges.front().claim_contributor = "claim:duplicate";
		auto out = take(q::project_object_semantics(f.input()));
		require(out.observations.size() == 1 &&
					out.observations[0].membership_state == state::complete &&
					out.evidence.size() >= 9U,
				"equal duplicate lost or contradicted");
		++cases;
	}
	{
		fixture f;
		f.rows[12].push_back(f.rows[12][0]);
		set(f.rows[12][1], "left_modifies", detached_cell::boolean(false));
		auto out = take(q::project_object_semantics(f.input()));
		require(out.observations[0].membership_state == state::conflicting,
				"contradictory original pair admitted");
		++cases;
	}
	{
		fixture f;
		set(f.rows[14][0], "sequence_pair_ids", ids({}));
		auto out = take(q::project_object_semantics(f.input()));
		require(out.populations[0].enumeration_state == state::conflicting,
				"missing census member admitted");
		++cases;
	}
	{
		fixture f;
		f.rows[12].clear();
		set(f.rows[14][0], "sequence_pair_ids", ids({}));
		set(f.rows[14][0], "sequence_pair_count", detached_cell::unsigned_integer(0));
		auto out = take(q::project_object_semantics(f.input()));
		require(out.observations.empty() && out.populations[0].enumeration_state == state::complete,
				"known empty stream is unknown");
		++cases;
	}
	{
		fixture f;
		f.rows[12].clear();
		auto out = take(q::project_object_semantics(f.input()));
		require(out.populations[0].enumeration_state != state::complete,
				"missing returned candidate inferred empty");
		++cases;
	}
	{
		fixture f;
		set(f.rows[14][0], "declarations", ids({"fn-decl"}));
		auto out = take(q::project_object_semantics(f.input()));
		require(sequence(out).storage_state != state::complete,
				"foreign declaration unit admitted");
		++cases;
	}
	{
		fixture f;
		set(f.rows[3][1], "provider_local_key", detached_cell::bytes(raw("storage")));
		auto out = take(q::project_object_semantics(f.input()));
		require(sequence(out).storage_state == state::conflicting,
				"raw USR accepted as framed entity key");
		++cases;
	}
	{
		fixture f;
		set(f.rows[2][0], "end", detached_cell::unsigned_integer(101));
		auto out = take(q::project_object_semantics(f.input()));
		require(out.observations[0].source_state == state::conflicting,
				"outside source bounds admitted");
		++cases;
	}
	{
		fixture f;
		f.object_state();
		auto out = take(q::project_object_semantics(f.input()));
		require(union_access(out).active_state == state::complete &&
					union_access(out).common_initial_sequence_permitted == std::optional{false},
				"original union state unavailable");
		++cases;
	}
	{
		fixture f;
		f.object_state();
		set(f.rows[13][0], "active_member_state", detached_cell::utf8("none"));
		set(f.rows[13][0],
			"active_field_usr",
			detached_cell::absent(f.rows[13][0].values.at("output.active_field_usr").type));
		auto out = take(q::project_object_semantics(f.input()));
		require(union_access(out).active_state == state::complete &&
					!union_access(out).active_field,
				"known no-active union confused with missing");
		++cases;
	}
	{
		fixture f;
		f.object_state();
		set(f.rows[13][0], "active_field_usr", detached_cell::bytes({}));
		auto out = take(q::project_object_semantics(f.input()));
		require(union_access(out).active_state == state::conflicting, "empty active ID admitted");
		++cases;
	}
	{
		fixture f;
		f.object_state();
		set(f.rows[15][0], "potential_check", detached_cell::boolean(true));
		auto out = take(q::project_object_semantics(f.input()));
		require(union_access(out).active_state != state::complete,
				"potential checking became observed invocation");
		++cases;
	}
	{
		fixture f;
		f.object_state();
		set(f.rows[15][0], "fold_failure", detached_cell::boolean(true));
		auto out = take(q::project_object_semantics(f.input()));
		require(union_access(out).active_state == state::complete,
				"failed root erased actual retained point state");
		++cases;
	}
	{
		fixture f;
		f.object_state();
		set(f.rows[13][0],
			"storage_path",
			detached_cell::absent(f.rows[13][0].values.at("output.storage_path").type));
		auto out = take(q::project_object_semantics(f.input()));
		require(union_access(out).active_state != state::complete,
				"missing path became empty root path");
		++cases;
	}
	{
		fixture f;
		q::projection_resource_usage usage{9, 9};
		auto limits = q::finite_population_limits{};
		limits.maximum_operations = 1;
		require(!q::project_object_semantics(f.input(), limits, {}, usage) &&
					usage.operations == 0 && usage.retained_bytes_bound == 0,
				"failed measured output leaked usage");
		++cases;
	}
	{
		fixture f;
		unsigned polls{};
		auto limits = q::finite_population_limits{};
		limits.cancelled = [&]
		{
			return ++polls == 10U;
		};
		auto out = q::project_object_semantics(f.input(), limits);
		require(!out && out.error().code == "sdk.object-cancelled", "nested cancellation absent");
		++cases;
	}
	{
		fixture f;
		q::projection_resource_usage usage;
		auto out = take(q::project_object_semantics(f.input(), {}, {}, usage));
		auto limits = q::finite_population_limits{};
		limits.maximum_operations = usage.operations;
		limits.maximum_retained_bytes = usage.retained_bytes_bound;
		require(static_cast<bool>(q::project_object_semantics(f.input(), limits)),
				"measured bound not reproducible");
		++cases;
	}
	{
		auto path = take(q::encode_compiler_object_path({}));
		require(take(q::decode_compiler_object_path(path)).empty(), "known empty path roundtrip");
		path.push_back(std::byte{0});
		require(!q::decode_compiler_object_path(path), "trailing path bytes admitted");
		++cases;
	}
	{
		q::compiler_object_path_step field;
		field.category = q::compiler_object_path_step::kind::field;
		field.member_usr = {std::byte{'a'}, std::byte{0}, std::byte{255}};
		const std::array original{field};
		auto path = take(q::encode_compiler_object_path(original));
		require(take(q::decode_compiler_object_path(path)) == std::vector{field},
				"binary USR path corrupted");
		++cases;
	}
	{
		q::compiler_object_path_step field;
		field.category = q::compiler_object_path_step::kind::field;
		const std::array original{field};
		require(!q::encode_compiler_object_path(original), "empty field identity accepted");
		++cases;
	}
	{
		fixture f;
		f.exit_fact();
		auto out = take(q::project_object_semantics(f.input()));
		const auto& observation = observed(out, "function_exit");
		const auto& value = std::get<q::original_function_exit>(observation.fact);
		require(value.exit_state == state::complete && value.return_kind == "value" &&
					value.exit_kind == "fallthrough" &&
					observation.owner_state == state::complete &&
					observation.source_state == state::complete && observation.body == "body",
				"actual definition exit lost by independent prototype");
		++cases;
	}
	{
		fixture f;
		f.exit_fact();
		set(f.rows[8][0], "function_exit_declaration", detached_cell::utf8("prototype"));
		auto out = take(q::project_object_semantics(f.input()));
		require(
			std::get<q::original_function_exit>(observed(out, "function_exit").fact).exit_state ==
				state::conflicting,
			"prototype detail borrowed actual definition body");
		++cases;
	}
	{
		fixture f;
		f.exit_fact();
		set(f.rows[6][0],
			"function_exit_declaration",
			detached_cell::absent(f.rows[6][0].values.at("output.function_exit_declaration").type));
		auto out = take(q::project_object_semantics(f.input()));
		require(
			std::get<q::original_function_exit>(observed(out, "function_exit").fact).exit_state !=
				state::complete,
			"missing physical body owner inferred");
		++cases;
	}
	{
		fixture f;
		f.exit_fact();
		set(f.rows[8][0], "function_exit_state", detached_cell::utf8("partial"));
		auto out = take(q::project_object_semantics(f.input()));
		require(
			std::get<q::original_function_exit>(observed(out, "function_exit").fact).exit_state ==
				state::partial,
			"known body source reset incomplete exit");
		++cases;
	}
	{
		fixture f;
		f.value_fact("enum_cast", "clang22-original-enum-cast/1");
		auto out = take(q::project_object_semantics(f.input()));
		const auto& value = std::get<q::original_enum_value>(observed(out, "enum_cast").fact);
		require(value.enumeration_state == state::complete &&
					value.fixed_underlying == std::optional{false} &&
					value.compiler_value_domain->upper == "7" && value.original_value->lower == "8",
				"actual enum compiler domain/value missing");
		++cases;
	}
	{
		fixture f;
		f.value_fact("enum_cast", "clang22-original-enum-cast/1");
		set(f.rows[9][1], "enum_fixed_underlying", detached_cell::boolean(true));
		for (auto field : {"enum_value_lower", "enum_value_upper"})
			set(f.rows[9][1],
				field,
				detached_cell::absent(f.rows[9][1].values.at("output." + std::string{field}).type));
		auto out = take(q::project_object_semantics(f.input()));
		const auto& value = std::get<q::original_enum_value>(observed(out, "enum_cast").fact);
		require(value.enumeration_state == state::complete &&
					value.fixed_underlying == std::optional{true} && !value.compiler_value_domain,
				"fixed enum demanded nonfixed value range");
		++cases;
	}
	{
		fixture f;
		f.value_fact("enum_cast", "clang22-original-enum-cast/1");
		set(f.rows[9][1], "enum_value_state", detached_cell::utf8("partial"));
		auto out = take(q::project_object_semantics(f.input()));
		require(
			std::get<q::original_enum_value>(observed(out, "enum_cast").fact).enumeration_state ==
				state::partial,
			"enum unknown original domain completed");
		++cases;
	}
	{
		fixture f;
		f.value_fact("bool_bit_cast", "clang22-original-bool-representation/1");
		auto out = take(q::project_object_semantics(f.input()));
		const auto& value =
			std::get<q::original_bool_representation>(observed(out, "bool_bit_cast").fact);
		require(value.representation_state == state::complete && value.kind == "bit_cast" &&
					value.original_representation->lower == "8",
				"actual Bool bit representation missing");
		++cases;
	}
	{
		fixture f;
		f.value_fact("bool_bit_cast", "clang22-original-bool-representation/1");
		set(f.rows[10][0], "representation_state", detached_cell::utf8("partial"));
		auto out = take(q::project_object_semantics(f.input()));
		require(std::get<q::original_bool_representation>(observed(out, "bool_bit_cast").fact)
						.representation_state == state::partial,
				"Bool width promoted unavailable bit pattern");
		++cases;
	}
	{
		fixture f;
		f.value_fact("bool_integral_conversion", "clang22-original-bool-representation/1");
		for (auto field : {"original_value_lower", "original_value_upper", "representation_state"})
			set(f.rows[10][0],
				field,
				detached_cell::absent(
					f.rows[10][0].values.at("output." + std::string{field}).type));
		auto out = take(q::project_object_semantics(f.input()));
		const auto& value = std::get<q::original_bool_representation>(
			observed(out, "bool_integral_conversion").fact);
		require(value.kind == "integral_conversion" && !value.original_representation,
				"conversion invented invalid representation");
		++cases;
	}
	{
		fixture f;
		f.direct_fact();
		auto out = take(q::project_object_semantics(f.input()));
		const auto& value =
			std::get<q::original_type_access>(observed(out, "direct_type_access").fact);
		require(value.effective_type_state == state::complete && value.storage == "var-decl" &&
					value.language_type_access_permitted == std::optional{false},
				"current direct type state missing");
		++cases;
	}
	{
		fixture f;
		f.direct_fact();
		set(f.rows[10][0], "current_object_state", detached_cell::utf8("unknown"));
		auto out = take(q::project_object_semantics(f.input()));
		require(std::get<q::original_type_access>(observed(out, "direct_type_access").fact)
						.effective_type_state == state::partial,
				"declared type inferred current effective type");
		++cases;
	}
	{
		fixture f;
		f.direct_fact();
		set(f.rows[10][0], "object_entity", detached_cell::utf8("function"));
		auto out = take(q::project_object_semantics(f.input()));
		require(std::get<q::original_type_access>(observed(out, "direct_type_access").fact)
						.effective_type_state == state::conflicting,
				"object declaration foreign entity admitted");
		++cases;
	}
	{
		fixture f;
		f.object_state();
		set(f.rows[13][0], "state_kind", detached_cell::utf8("absent_subobject"));
		set(f.rows[13][0], "actual_polymorphic_use", detached_cell::boolean(true));
		auto out = take(q::project_object_semantics(f.input()));
		const auto& lifetime =
			std::get<q::original_lifetime_access>(observed(out, "absent_subobject").fact);
		const auto& dynamic = std::get<q::original_dynamic_object_access>(
			observed(out, "dynamic_object_access").fact);
		require(lifetime.phase == "unknown" && lifetime.phase_state != state::complete &&
					dynamic.dynamic_state != state::complete,
				"APValue absence inferred ended lifetime/vptr");
		++cases;
	}

	{
		fixture f;
		f.value_fact("enum_cast", "clang22-original-enum-cast/1");
		set(f.rows[10][0], "type", detached_cell::utf8("result-type"));
		set(f.rows[10][0],
			"canonical_type",
			detached_cell::absent(f.rows[10][0].values.at("output.canonical_type").type));
		auto out = take(q::project_object_semantics(f.input()));
		require(
			std::get<q::original_enum_value>(observed(out, "enum_cast").fact).enumeration_state !=
				state::complete,
			"display spelling became original type ID");
		++cases;
	}
	{
		fixture f;
		f.value_fact("bool_bit_cast", "clang22-original-bool-representation/1");
		set(f.rows[9][1], "builtin_kind", detached_cell::utf8("Int"));
		auto out = take(q::project_object_semantics(f.input()));
		require(std::get<q::original_bool_representation>(observed(out, "bool_bit_cast").fact)
						.representation_state == state::conflicting,
				"non-Bool original type admitted as Bool");
		++cases;
	}
	{
		fixture f;
		f.value_fact("bool_bit_cast", "clang22-original-bool-representation/1");
		set(f.rows[10][1], "function", detached_cell::utf8("storage"));
		auto out = take(q::project_object_semantics(f.input()));
		require(std::get<q::original_bool_representation>(observed(out, "bool_bit_cast").fact)
						.representation_state == state::conflicting,
				"foreign operand owner admitted");
		++cases;
	}
	for (auto value : {"+1", "01", "-0", "x"})
	{
		fixture f;
		f.value_fact("enum_cast", "clang22-original-enum-cast/1");
		set(f.rows[10][0], "original_value_lower", detached_cell::utf8(value));
		auto out = take(q::project_object_semantics(f.input()));
		require(
			std::get<q::original_enum_value>(observed(out, "enum_cast").fact).enumeration_state ==
				state::conflicting,
			"noncanonical original interval admitted");
		++cases;
	}
	{
		fixture f;
		f.value_fact("enum_cast", "clang22-original-enum-cast/1");
		set(f.rows[10][0], "original_value_lower", detached_cell::utf8("9"));
		auto out = take(q::project_object_semantics(f.input()));
		require(
			std::get<q::original_enum_value>(observed(out, "enum_cast").fact).enumeration_state ==
				state::conflicting,
			"reversed original interval admitted");
		++cases;
	}

	{
		fixture f;
		f.lifetime_fact();
		auto out = take(q::project_object_semantics(f.input()));
		const auto& life =
			std::get<q::original_lifetime_access>(observed(out, "direct_member_lifetime").fact);
		require(life.phase_state == state::complete && life.phase == "ended" &&
					life.permitted_in_phase == std::optional{false},
				"original direct ended point lost");
		require(
			std::get<q::original_dynamic_object_access>(observed(out, "dynamic_object_access").fact)
					.dynamic_state == state::complete,
			"original virtual ended point lost");
		++cases;
	}
	{
		fixture f;
		f.lifetime_fact();
		set(f.rows[10][0], "object_phase", detached_cell::utf8("live"));
		auto out = take(q::project_object_semantics(f.input()));
		require(std::get<q::original_lifetime_access>(observed(out, "direct_member_lifetime").fact)
						.permitted_in_phase == std::optional{true},
				"live original point became violation");
		++cases;
	}
	{
		fixture f;
		f.lifetime_fact();
		set(f.rows[10][0], "actual_polymorphic_use", detached_cell::boolean(false));
		auto out = take(q::project_object_semantics(f.input()));
		require(std::ranges::none_of(out.observations,
									 [](const auto& x)
									 {
										 return x.kind == "dynamic_object_access";
									 }),
				"nonvirtual use invented vptr access");
		++cases;
	}
	{
		fixture f;
		f.lifetime_fact();
		set(f.rows[10][0], "object_fact_state", detached_cell::utf8("partial"));
		auto out = take(q::project_object_semantics(f.input()));
		require(std::get<q::original_lifetime_access>(observed(out, "direct_member_lifetime").fact)
						.phase_state == state::partial,
				"unknown original point completed");
		++cases;
	}
	{
		fixture f;
		f.lifetime_fact();
		set(f.rows[10][1], "object_declaration", detached_cell::utf8("fn-decl"));
		auto out = take(q::project_object_semantics(f.input()));
		require(std::get<q::original_lifetime_access>(observed(out, "direct_member_lifetime").fact)
						.phase_state == state::conflicting,
				"foreign receiver storage admitted");
		++cases;
	}
	{
		fixture f;
		f.lifetime_fact();
		set(f.rows[10][0], "object_cfg_element_index", detached_cell::unsigned_integer(1U));
		auto out = take(q::project_object_semantics(f.input()));
		require(std::get<q::original_lifetime_access>(observed(out, "direct_member_lifetime").fact)
						.phase_state == state::conflicting,
				"out of range CFG point admitted");
		++cases;
	}
	{
		fixture f;
		f.lifetime_fact();
		set(f.rows[6][0], "function_exit_declaration", detached_cell::utf8("prototype"));
		auto out = take(q::project_object_semantics(f.input()));
		require(std::get<q::original_lifetime_access>(observed(out, "direct_member_lifetime").fact)
						.phase_state == state::conflicting,
				"prototype lent lifetime body");
		++cases;
	}

	{
		fixture f;
		f.lifetime_fact();
		set(f.rows[6][0],
			"function_exit_declaration",
			detached_cell::absent(f.rows[6][0].values.at("output.function_exit_declaration").type));
		auto out = take(q::project_object_semantics(f.input()));
		require(std::get<q::original_lifetime_access>(observed(out, "direct_member_lifetime").fact)
						.phase_state == state::partial,
				"missing lifetime body owner became contradictory");
		++cases;
	}
	{
		fixture f;
		f.lifetime_fact();
		q::projection_resource_usage usage;
		auto out = take(q::project_object_semantics(f.input(), {}, {}, usage));
		auto limits = q::finite_population_limits{};
		limits.maximum_operations = usage.operations;
		limits.maximum_retained_bytes = usage.retained_bytes_bound;
		require(static_cast<bool>(q::project_object_semantics(f.input(), limits)),
				"lifetime duplicated observation exceeded measured bound");
		++cases;
	}
	for (auto field : {"owner_entity", "expression"})
	{
		fixture f;
		f.argument_contexts();
		set(f.rows[11][0],
			field,
			detached_cell::absent(f.rows[11][0].values.at("output." + std::string{field}).type));
		set(f.rows[11][0], "binding_state", detached_cell::utf8("partial"));
		auto out = take(q::project_object_semantics(f.input()));
		require(sequence(out).context_state == state::partial,
				"missing original context binding became contradiction");
		++cases;
	}
	{
		fixture f;
		f.argument_contexts();
		set(f.rows[11][0], "expression", detached_cell::utf8("right"));
		auto out = take(q::project_object_semantics(f.input()));
		require(sequence(out).context_state == state::conflicting,
				"actual mismatched context expression escaped contradiction");
		++cases;
	}
	{
		fixture f;
		f.argument_contexts();
		set(f.rows[12][0], "language", detached_cell::utf8("c++14"));
		for (auto& context : f.rows[11])
			set(context, "language", detached_cell::utf8("c++14"));
		auto out = take(q::project_object_semantics(f.input()));
		require(sequence(out).context_state == state::conflicting,
				"pre17 invocation falsely supplied indeterminate argument sequencing");
		++cases;
	}
	{
		fixture f;
		f.argument_contexts();
		set(f.rows[12][0], "language", detached_cell::utf8("c++14"));
		for (auto& context : f.rows[11])
		{
			set(context, "language", detached_cell::utf8("c++14"));
			set(context, "indeterminately_sequenced", detached_cell::boolean(false));
		}
		auto out = take(q::project_object_semantics(f.input()));
		require(sequence(out).context_state == state::complete &&
					sequence(out).sequencing == "unsequenced",
				"known pre17 compiler context lost original sequencing");
		++cases;
	}
	{
		fixture f;
		set(f.rows[12][0], "language", detached_cell::utf8("future"));
		auto out = take(q::project_object_semantics(f.input()));
		require(sequence(out).sequencing_state == state::partial,
				"unsupported language promoted pointwise relation");
		++cases;
	}

	{
		fixture f;
		set(f.rows[14][0], "reason", detached_cell::utf8(std::string(65536U, 'x')));
		q::projection_resource_usage usage;
		auto out = take(q::project_object_semantics(f.input(), {}, {}, usage));
		require(out.populations[0].enumeration_state == state::complete &&
					sequence(out).storage_state == state::complete && usage.operations < 32768U,
				"same immutable original payload was repeatedly compared");
		auto limits = q::finite_population_limits{};
		limits.maximum_operations = usage.operations;
		limits.maximum_retained_bytes = usage.retained_bytes_bound;
		require(static_cast<bool>(q::project_object_semantics(f.input(), limits)),
				"identity comparison lost its exact resource bound");
		--limits.maximum_operations;
		auto failed = q::project_object_semantics(f.input(), limits, {}, usage);
		require(!failed && failed.error().code == "sdk.object-budget" &&
					failed.error().field == "operations" && usage.operations == 0U &&
					usage.retained_bytes_bound == 0U,
				"identity comparison bypassed the operation limit");
		++cases;
	}
	{
		fixture f;
		set(f.rows[14][0], "reason", detached_cell::utf8(std::string(65536U, 'x')));
		f.rows[14].push_back(f.rows[14][0]);
		auto limits = q::finite_population_limits{};
		limits.maximum_operations = 32768U;
		auto out = q::project_object_semantics(f.input(), limits);
		require(!out && out.error().code == "sdk.object-budget" &&
					out.error().field == "operations",
				"distinct equal originals skipped their bounded cell comparison");
		++cases;
	}
	{
		fixture f;
		set(f.rows[14][0], "reason", detached_cell::utf8(std::string(65536U, 'x')));
		f.rows[14].push_back(f.rows[14][0]);
		std::string reason(65536U, 'x');
		reason.back() = 'y';
		set(f.rows[14][1], "reason", detached_cell::utf8(std::move(reason)));
		auto out = take(q::project_object_semantics(f.input()));
		require(out.populations[0].enumeration_state == state::conflicting,
				"distinct original disagreement escaped exact comparison");
		++cases;
	}

	{
		fixture f;
		for (auto id : {"pair2", "pair3", "pair4", "pair5", "pair6", "pair7", "pair8"})
		{
			auto original = f.rows[12][0];
			set(original, "pair", detached_cell::utf8(id));
			f.rows[12].push_back(std::move(original));
		}
		set(f.rows[14][0], "sequence_pair_count", detached_cell::unsigned_integer(8U));
		set(f.rows[14][0],
			"sequence_pair_ids",
			ids({"pair", "pair2", "pair3", "pair4", "pair5", "pair6", "pair7", "pair8"}));
		auto limits = q::finite_population_limits{};
		limits.maximum_members = 32U;
		q::projection_resource_usage usage;
		auto out = take(q::project_object_semantics(f.input(), limits, {}, usage));
		require(out.observations.size() == 8U &&
					out.populations[0].enumeration_state == state::complete &&
					std::ranges::all_of(out.observations,
										[](const auto& observation)
										{
											return std::get<q::original_sequence_pair>(
													   observation.fact)
													   .storage_state == state::complete;
										}),
				"repeated references consumed the same declaration members again");
		limits.maximum_operations = usage.operations;
		limits.maximum_retained_bytes = usage.retained_bytes_bound;
		require(static_cast<bool>(q::project_object_semantics(f.input(), limits)),
				"scoped declaration cache leaked into the final retained bound");
		--limits.maximum_operations;
		require(!q::project_object_semantics(f.input(), limits),
				"cached membership lookup bypassed its exact work limit");
		++cases;
	}
	{
		fixture f;
		set(f.rows[14][0], "declarations", ids({"fn-decl"}));
		set(f.rows[14][0], "declaration_count", detached_cell::unsigned_integer(1U));
		auto other = f.rows[14][0];
		set(other, "inventory", detached_cell::utf8("inventory-other"));
		set(other, "declarations", ids({"var-decl"}));
		f.rows[14].push_back(std::move(other));
		auto out = take(q::project_object_semantics(f.input()));
		require(sequence(out).storage_state == state::complete &&
					out.populations[0].enumeration_state == state::conflicting,
				"different original inventory rows reused a declaration list");
		++cases;
	}
	{
		fixture f;
		set(f.rows[14][0], "declarations", ids({"var-decl", "fn-decl"}));
		auto out = q::project_object_semantics(f.input());
		require(!out && out.error().code == "sdk.query-row-invalid" &&
					out.error().field == "output.declarations",
				"cached declaration list accepted noncanonical originals");
		++cases;
	}
	{
		fixture f;
		auto declarations = ids({"fn-decl", "var-decl"});
		auto& raw = std::get<std::vector<std::byte>>(*declarations.value);
		raw.pop_back();
		set(f.rows[14][0], "declarations", std::move(declarations));
		auto out = q::project_object_semantics(f.input());
		require(!out && out.error().code == "sdk.query-row-invalid" &&
					out.error().field == "output.declarations",
				"cached declaration list accepted truncated originals");
		++cases;
	}

	{
		fixture f;
		set(f.rows[14][0], "declarations", ids({"fn-decl"}));
		set(f.rows[14][0], "declaration_count", detached_cell::unsigned_integer(1U));
		auto other = f.rows[14][0];
		set(other, "inventory", detached_cell::utf8("inventory-other"));
		set(other, "declarations", ids({"var-decl"}));
		other.presence.fragments = {"release"};
		other.contributor_edges.front().condition = other.presence;
		f.rows[14].push_back(std::move(other));
		auto out = take(q::project_object_semantics(f.input()));
		require(sequence(out).storage_state != state::complete,
				"cached declaration list bypassed its original world");
		++cases;
	}

	std::cout << cases << " original object SDK cases PASS\n";
}
