#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <tuple>

#include <cxxlens/sdk.hpp>
#include <cxxlens/sdk/exception_cleanup_facets.hpp>

#include "../../../src/sdk/query_result_internal.hpp"
#include "query_projection_row_copy_controls.hpp"

namespace
{
	using namespace cxxlens::sdk;
	namespace q = cxxlens::sdk::query;
	using state = q::finite_population_state;
	constexpr std::array<std::string_view, 9> names{"build.compile_unit.v1",
													"source.file.v1",
													"source.span.v1",
													"cc.entity.v1",
													"cc.entity_detail.v1",
													"cc.body.v1",
													"cc.exceptional_exit.v1",
													"cc.declaration.v1",
													"cc.declaration_inventory.v1"};
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
			copy.type = row.values.at("output." + name).type;
			row.values["output." + name] = std::move(copy);
		}
		return row;
	}
	void set(q::annotated_row& row, std::string_view name, detached_cell value)
	{
		const std::string key = "output." + std::string{name};
		value.type = row.values.at(key).type;
		row.values[key] = std::move(value);
	}
	detached_cell binary(std::string_view value)
	{
		std::vector<std::byte> out;
		for (char c : value)
			out.push_back(static_cast<std::byte>(c));
		return detached_cell::bytes(std::move(out));
	}
	struct fixture
	{
		std::array<std::vector<q::annotated_row>, 9> rows;
		fixture()
		{
			rows[0] = {fact(0, {{"compile_unit", detached_cell::utf8("unit:a")}})};
			rows[1] = {fact(1,
							{{"snapshot", detached_cell::utf8("snapshot:a")},
							 {"file", detached_cell::utf8("file:a")},
							 {"size", detached_cell::unsigned_integer(100)}})};
			for (auto name : {"span:definition", "span:body", "span:object"})
				rows[2].push_back(fact(2,
									   {{"span", detached_cell::utf8(name)},
										{"snapshot", detached_cell::utf8("snapshot:a")},
										{"file", detached_cell::utf8("file:a")},
										{"begin", detached_cell::unsigned_integer(10)},
										{"end", detached_cell::unsigned_integer(80)}}));
			rows[3] = {fact(3,
							{{"entity", detached_cell::utf8("function:a")},
							 {"kind", detached_cell::utf8("function")}}),
					   fact(3,
							{{"entity", detached_cell::utf8("object:a")},
							 {"kind", detached_cell::utf8("variable")},
							 {"semantic_owner", detached_cell::utf8("function:a")}}),
					   fact(3,
							{{"entity", detached_cell::utf8("target:dtor")},
							 {"kind", detached_cell::utf8("destructor")},
							 {"provider_local_key", binary("opaque:not-USR")}})};
			rows[4] = {fact(4,
							{{"detail", detached_cell::utf8("detail:a")},
							 {"entity", detached_cell::utf8("function:a")},
							 {"compile_unit", detached_cell::utf8("unit:a")},
							 {"source", detached_cell::utf8("span:definition")},
							 {"exception_spec_kind", detached_cell::utf8("basic_noexcept")},
							 {"exception_spec_nonthrowing", detached_cell::boolean(true)},
							 {"exception_spec_state", detached_cell::utf8("complete")},
							 {"exception_spec_profile",
							  detached_cell::utf8("clang22-function-exception-specification/1")}})};
			rows[5] = {fact(5,
							{{"body", detached_cell::utf8("body:a")},
							 {"function", detached_cell::utf8("function:a")},
							 {"compile_unit", detached_cell::utf8("unit:a")},
							 {"source", detached_cell::utf8("span:body")}})};
			auto variant = fact(
				6,
				{{"exit", detached_cell::utf8("variant:a")},
				 {"scope_detail", detached_cell::utf8("detail:a")},
				 {"function", detached_cell::utf8("function:a")},
				 {"compile_unit", detached_cell::utf8("unit:a")},
				 {"definition_source", detached_cell::utf8("span:definition")},
				 {"body", detached_cell::utf8("body:a")},
				 {"variant_kind", detached_cell::utf8("function")},
				 {"variant_index", detached_cell::unsigned_integer(0)},
				 {"variant_symbol", binary(std::string_view{"f\0\xff", 3})},
				 {"ordinal", detached_cell::unsigned_integer(0)},
				 {"role", detached_cell::utf8("lowering_variant")},
				 {"eligibility", detached_cell::utf8("excluded")},
				 {"observation_state", detached_cell::utf8("complete")},
				 {"profile", detached_cell::utf8("clang22-original-exceptional-occurrences/1")},
				 {"lowering_profile",
				  detached_cell::utf8("clang22-written-definition-analysis-lowering/1")}});
			auto cleanup = variant;
			set(cleanup, "exit", detached_cell::utf8("exit:cleanup"));
			set(cleanup, "variant", detached_cell::utf8("variant:a"));
			set(cleanup, "ordinal", detached_cell::unsigned_integer(1));
			set(cleanup, "role", detached_cell::utf8("escaping_call"));
			set(cleanup, "cleanup_declaration", detached_cell::utf8("decl:object"));
			set(cleanup, "cleanup_registration_ordinal", detached_cell::unsigned_integer(1));
			set(cleanup, "cleanup_emission_ordinal", detached_cell::unsigned_integer(1));
			set(cleanup, "cleanup_route", detached_cell::utf8("exceptional"));
			set(cleanup,
				"cleanup_profile",
				detached_cell::utf8("clang22-destroy-object-cleanup-emission/1"));
			set(cleanup, "cleanup_target", detached_cell::utf8("target:dtor"));
			set(cleanup, "cleanup_target_usr", binary(std::string_view{"usr\0\xff", 5}));
			set(cleanup, "cleanup_target_dtor_type", detached_cell::unsigned_integer(0));
			set(cleanup,
				"cleanup_target_profile",
				detached_cell::utf8("clang22-destructor-emission-target/1"));
			set(cleanup, "emitter_methods", detached_cell::unsigned_integer(1));
			rows[6] = {variant, cleanup};
			rows[7] = {fact(7,
							{{"declaration", detached_cell::utf8("decl:object")},
							 {"entity", detached_cell::utf8("object:a")},
							 {"source", detached_cell::utf8("span:object")},
							 {"kind", detached_cell::utf8("Var")}})};
			rows[8] = {fact(
				8,
				{{"inventory", detached_cell::utf8("inventory:a")},
				 {"compile_unit", detached_cell::utf8("unit:a")},
				 {"profile", detached_cell::utf8("clang22-explicit-admitted-named-declarations/1")},
				 {"enumeration_state", detached_cell::utf8("complete")},
				 {"declaration_count", detached_cell::unsigned_integer(1)},
				 {"declarations", symbols({"decl:object"})}})};
		}
		q::exception_cleanup_input input() const
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
					true};
		}
		q::application_query_results queries(bool broad = false, bool sizes = false) const
		{
			q::application_query_results out;
			out.snapshot_id = "query:facet";
			for (std::size_t group = 0; group < rows.size(); ++group)
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
				data->status = q::execution_status::complete;
				data->input_complete = broad;
				data->snapshot = out.snapshot_id;
				out.scans.push_back(
					{std::string{names[group]}, {}, q::query_transfer_access::make(data)});
			}
			return out;
		}
	};
	const q::observed_cleanup_emission& cleanup(const q::exception_cleanup_projection& out)
	{
		const auto at =
			std::ranges::find(out.cleanups, "exit:cleanup", &q::observed_cleanup_emission::exit);
		require(at != out.cleanups.end(), "cleanup observation retained");
		return *at;
	}
	const q::observed_function_exception_specification&
	spec(const q::exception_cleanup_projection& out)
	{
		require(out.specifications.size() == 1U, "one callable detail");
		return out.specifications.front();
	}

	auto specification_fields(const q::observed_function_exception_specification& value)
	{
		return std::tie(value.detail,
						value.function,
						value.compile_unit,
						value.source_span,
						value.universe,
						value.variant,
						value.interpretation,
						value.kind,
						value.profile,
						value.observation_state,
						value.nonthrowing,
						value.specification_state,
						value.identity_state,
						value.source_state,
						value.evidence,
						value.gaps);
	}
	void specification_parity(const q::exception_cleanup_projection& full,
							  const q::function_exception_specification_projection& narrow)
	{
		require(full.specifications.size() == narrow.specifications.size(),
				"specification-only retains every original detail");
		for (std::size_t i{}; i < narrow.specifications.size(); ++i)
		{
			require(specification_fields(full.specifications[i]) ==
						specification_fields(narrow.specifications[i]),
					"specification-only all fields/states/ordered references match full API");
			for (const auto reference : narrow.specifications[i].evidence)
			{
				require(reference < narrow.evidence.size() && reference < full.evidence.size(),
						"specification-only evidence index remains in its owner");
				require(narrow.evidence[reference].relation_id ==
								full.evidence[reference].relation_id &&
							narrow.evidence[reference].row.canonical_form() ==
								full.evidence[reference].row.canonical_form(),
						"specification-only lossless original evidence matches full API");
			}
		}
		require(std::tie(full.compile_units_complete,
						 full.detail_inputs_complete,
						 full.exit_inputs_complete,
						 full.declaration_inputs_complete,
						 full.unresolved) ==
					std::tie(narrow.compile_units_complete,
							 narrow.detail_inputs_complete,
							 narrow.exit_inputs_complete,
							 narrow.declaration_inputs_complete,
							 narrow.unresolved),
				"specification-only preserves all source coverage/missing-scan axes");
		require(full.source_queries.has_value() == narrow.source_queries.has_value(),
				"specification-only preserves query owner presence");
		if (full.source_queries)
		{
			require(full.source_queries->snapshot_id == narrow.source_queries->snapshot_id &&
						full.source_queries->scans.size() == narrow.source_queries->scans.size(),
					"specification-only retains the complete source query");
			for (std::size_t i{}; i < full.source_queries->scans.size(); ++i)
				require(full.source_queries->scans[i].relation_id ==
								narrow.source_queries->scans[i].relation_id &&
							full.source_queries->scans[i].logical_ir.canonical_form() ==
								narrow.source_queries->scans[i].logical_ir.canonical_form() &&
							full.source_queries->scans[i].result.canonical_form() ==
								narrow.source_queries->scans[i].result.canonical_form(),
						"specification-only query side channels match full API");
		}
	}
	void ordered_identity_evidence_controls()
	{
		for (unsigned disposition{}; disposition < 5U; ++disposition)
		{
			fixture original;
			if (disposition == 1U || disposition == 4U)
				original.rows[0].clear();
			if (disposition == 2U || disposition == 4U)
			{
				auto& function = original.rows[3].front();
				function.presence.universe = "world:foreign";
				function.contributor_edges.front().condition.universe = "world:foreign";
			}
			if (disposition == 3U)
			{
				auto conflicting = original.rows[3].front();
				set(conflicting, "kind", detached_cell::utf8("variable"));
				original.rows[3].push_back(std::move(conflicting));
			}
			const auto out = take(q::project_exception_cleanup_facets(original.input()));
			const auto expected_state = disposition == 0U ? state::complete
				: disposition == 3U						  ? state::conflicting
				: disposition == 4U						  ? state::unknown
														  : state::partial;
			require(spec(out).identity_state == expected_state &&
						cleanup(out).scope_state == expected_state &&
						spec(out).specification_state == state::complete &&
						spec(out).source_state == state::complete &&
						spec(out).nonthrowing == true &&
						cleanup(out).emission_state == state::complete &&
						cleanup(out).registration_state == state::complete &&
						cleanup(out).definition_source_state == state::complete &&
						cleanup(out).declaration_state == state::complete &&
						cleanup(out).declaration_source_state == state::complete &&
						cleanup(out).target_attribution_state == state::complete &&
						cleanup(out).target_state == state::complete,
					"ordered identity evidence preserves every independent cleanup/specification "
					"state");
			require(
				out.compile_units_complete && out.detail_inputs_complete &&
					out.exit_inputs_complete && out.declaration_inputs_complete &&
					out.unresolved.empty() && !out.source_queries,
				"ordered identity evidence preserves original input coverage and query ownership");
			if (expected_state == state::complete)
				require(spec(out).gaps.empty() && cleanup(out).gaps.empty(),
						"complete identity ordering invented a missing facet");
			else
				require(spec(out).gaps.size() == 1U && cleanup(out).gaps.size() == 1U &&
							spec(out).gaps.front().code ==
								"sdk.exception-cleanup-function-identity-unavailable" &&
							cleanup(out).gaps.front().code ==
								"sdk.exception-cleanup-cleanup-scope-unavailable",
						"unknown/conflicting identity retains its precise original gaps");
			using original_reference = std::pair<std::size_t, std::size_t>;
			const auto append_identity = [&](std::vector<original_reference>& references)
			{
				if (disposition != 1U && disposition != 4U)
					references.emplace_back(0U, 0U);
				if (disposition != 2U && disposition != 4U)
					references.emplace_back(3U, 0U);
				if (disposition == 3U)
					references.emplace_back(3U, 3U);
			};
			std::vector<original_reference> specification_references{{4U, 0U}};
			append_identity(specification_references);
			specification_references.insert(specification_references.end(), {{2U, 0U}, {1U, 0U}});
			std::vector<original_reference> cleanup_references{{6U, 1U}};
			append_identity(cleanup_references);
			cleanup_references.insert(cleanup_references.end(),
									  {{4U, 0U},
									   {5U, 0U},
									   {6U, 0U},
									   {5U, 0U},
									   {2U, 0U},
									   {1U, 0U},
									   {7U, 0U},
									   {2U, 2U},
									   {1U, 0U},
									   {3U, 1U},
									   {8U, 0U},
									   {3U, 2U}});
			std::vector<original_reference> owner_order;
			for (const auto& references : {specification_references, cleanup_references})
				for (const auto& reference : references)
					if (std::ranges::find(owner_order, reference) == owner_order.end())
						owner_order.push_back(reference);
			require(out.evidence.size() == owner_order.size(),
					"ordered identity binding omitted or invented original evidence");
			for (std::size_t i{}; i < owner_order.size(); ++i)
			{
				const auto [group, row] = owner_order[i];
				require(out.evidence[i].relation_id == names[group] &&
							out.evidence[i].row.canonical_form() ==
								original.rows[group][row].canonical_form(),
						"compile unit precedes entity in the lossless original evidence owner");
			}
			const auto check_references = [&](const std::vector<original_reference>& expected,
											  const std::vector<std::size_t>& actual)
			{
				require(expected.size() == actual.size(), "ordered evidence reference cardinality");
				for (std::size_t i{}; i < expected.size(); ++i)
				{
					const auto at = std::ranges::find(owner_order, expected[i]);
					require(
						at != owner_order.end() &&
							actual[i] == static_cast<std::size_t>(at - owner_order.begin()),
						"ordered evidence references preserve duplicates and exact owner indices");
				}
			};
			check_references(specification_references, spec(out).evidence);
			check_references(cleanup_references, cleanup(out).evidence);
			specification_parity(
				out, take(q::project_function_exception_specifications(original.input())));
		}
	}
	void specification_only_controls()
	{
		fixture original;
		q::projection_resource_usage full_usage, narrow_usage;
		auto full = take(q::project_exception_cleanup_facets(original.input(), {}, {}, full_usage));
		auto narrow = take(
			q::project_function_exception_specifications(original.input(), {}, {}, narrow_usage));
		specification_parity(full, narrow);
		require(!full.cleanups.empty() && narrow_usage.operations < full_usage.operations,
				"purpose-specific API avoids actual cleanup construction");
		specification_parity(
			take(q::project_exception_cleanup_facets(original.queries())),
			take(q::project_function_exception_specifications(original.queries())));
		for (unsigned disposition{}; disposition < 4U; ++disposition)
		{
			auto changed = original;
			if (disposition == 0U)
				changed.rows[4].front().values.erase("output.exception_spec_nonthrowing");
			else if (disposition == 1U)
			{
				set(changed.rows[4].front(),
					"exception_spec_kind",
					detached_cell::utf8("dependent_noexcept"));
				set(changed.rows[4].front(),
					"exception_spec_state",
					detached_cell::utf8("partial"));
				changed.rows[4].front().values.erase("output.exception_spec_nonthrowing");
			}
			else if (disposition == 2U)
				set(changed.rows[4].front(),
					"exception_spec_nonthrowing",
					detached_cell::boolean(false));
			else
			{
				changed.rows[3].front().presence.universe = "world:foreign";
				changed.rows[3].front().contributor_edges.front().condition.universe =
					"world:foreign";
			}
			specification_parity(
				take(q::project_exception_cleanup_facets(changed.input())),
				take(q::project_function_exception_specifications(changed.input())));
		}
		auto missing = original.queries();
		missing.scans.pop_back();
		specification_parity(take(q::project_exception_cleanup_facets(missing)),
							 take(q::project_function_exception_specifications(missing)));
		for (std::size_t group = 5U; group < 9U; ++group)
		{
			auto malformed = original;
			malformed.rows[group].front().values.erase("output." +
													   std::string{group == 5U		 ? "body"
																	   : group == 6U ? "exit"
																	   : group == 7U
																	   ? "declaration"
																	   : "inventory"});
			auto full_error = q::project_exception_cleanup_facets(malformed.input());
			narrow_usage = {1U, 1U};
			auto narrow_error = q::project_function_exception_specifications(
				malformed.input(), {}, {}, narrow_usage);
			require(!full_error && !narrow_error && full_error.error() == narrow_error.error() &&
						!narrow_usage.operations && !narrow_usage.retained_bytes_bound,
					"unrequested body/exit/declaration/inventory rows still admitted");
		}
		auto malformed_members = original;
		set(malformed_members.rows[8].front(),
			"declarations",
			symbols({"decl:object", "decl:object"}));
		require(!q::project_function_exception_specifications(malformed_members.input()),
				"unrequested cleanup inventory retains duplicate-member rejection");
		set(malformed_members.rows[8].front(),
			"declarations",
			detached_cell::bytes({std::byte{1}}));
		require(!q::project_function_exception_specifications(malformed_members.input()),
				"unrequested cleanup inventory retains detached-set framing rejection");
		q::finite_population_limits member_cap;
		member_cap.maximum_members = 1U;
		set(malformed_members.rows[8].front(),
			"declarations",
			symbols({"decl:object", "decl:other"}));
		require(
			!q::project_function_exception_specifications(malformed_members.input(), member_cap),
			"unrequested cleanup inventory retains member ceiling");
		fixture rich;
		query_copy_controls::projection(
			rich.rows,
			names,
			[&]
			{
				return rich.queries();
			},
			[](const auto& query,
			   q::finite_population_limits cap,
			   q::projection_resource_usage& usage)
			{
				return q::project_function_exception_specifications(query, cap, {}, usage);
			},
			require);
		std::stop_source stopped;
		stopped.request_stop();
		narrow_usage = {1U, 1U};
		auto cancelled = q::project_function_exception_specifications(
			original.input(), {}, stopped.get_token(), narrow_usage);
		require(!cancelled && cancelled.error().code == "sdk.exception-cleanup-cancelled" &&
					!narrow_usage.operations && !narrow_usage.retained_bytes_bound,
				"specification-only stop returns existing error and zero failure usage");
		q::finite_population_limits late_stop;
		std::size_t calls{};
		late_stop.cancelled = [&]
		{
			return ++calls == 50U;
		};
		require(!q::project_function_exception_specifications(
					original.input(), late_stop, {}, narrow_usage) &&
					!narrow_usage.operations && !narrow_usage.retained_bytes_bound,
				"specification-only active cancellation retains failure usage contract");
	}

} // namespace
int main(int argc, char** argv)
{
	{
		fixture sized;
		query_copy_controls::projection(
			sized.rows,
			names,
			[&]
			{
				return sized.queries(false, true);
			},
			[](const auto& input, auto limits, auto& usage)
			{
				return q::project_exception_cleanup_facets(input, limits, {}, usage);
			},
			require);
		std::size_t calls{};
		q::finite_population_limits measured;
		measured.cancelled = [&]
		{
			++calls;
			return false;
		};
		const auto admitted = sized.queries(false, true);
		require(bool(q::project_exception_cleanup_facets(admitted, measured)),
				"immutable sizing current callback census");
		q::finite_population_limits interrupted;
		std::size_t visited{};
		interrupted.cancelled = [&]
		{
			return ++visited >= calls / 2U;
		};
		q::projection_resource_usage spent;
		const auto stopped = q::project_exception_cleanup_facets(admitted, interrupted, {}, spent);
		require(!stopped && !spent.operations && !spent.retained_bytes_bound,
				"immutable sizing real stop revokes all usage");
		require(bool(q::project_exception_cleanup_facets(admitted, {}, {}, spent)),
				"immutable sizing fresh retry");
	}
	if (argc == 2 || argc == 3)
	{
		relation_registry registry;
		for (const auto& descriptor : standard_relation_descriptors())
			require(registry.add(descriptor).has_value(), "original registry admission");
		auto engine = take(registry.build("exception-cleanup-original"));
		std::ifstream file(argv[1], std::ios::binary);
		require(file.good(), "actual saved query present");
		std::string data{std::istreambuf_iterator<char>{file}, {}};
		auto query = take(q::decode_application_queries(engine, data));
		q::projection_resource_usage observed;
		auto projected = take(q::project_exception_cleanup_facets(query, {}, {}, observed));
		q::projection_resource_usage specification_usage;
		auto specifications =
			take(q::project_function_exception_specifications(query, {}, {}, specification_usage));
		specification_parity(projected, specifications);
		std::cerr << "actual narrow-specification work " << specification_usage.operations
				  << " bytes " << specification_usage.retained_bytes_bound << " full work "
				  << observed.operations << "\n";
		if (argc == 3)
		{
			require(std::string_view{argv[2]} == "--specification-parity",
					"known genuine specification mode");
			return 0;
		}
		std::size_t known_true{}, known_false{}, partial{}, emissions{}, normal{}, exceptional{},
			targets{}, bound{};
		for (const auto& s : projected.specifications)
		{
			if (s.specification_state == state::complete && s.nonthrowing)
			{
				if (*s.nonthrowing)
					++known_true;
				else
					++known_false;
			}
			if (s.specification_state == state::partial)
				++partial;
		}
		for (const auto& c : projected.cleanups)
			if (c.emission_state == state::complete)
			{
				++emissions;
				normal += c.route == "normal" ? 1U : 0U;
				exceptional += c.route == "exceptional" ? 1U : 0U;
				bound += c.declaration_state == state::complete ? 1U : 0U;
				targets += c.target_attribution_state == state::complete &&
						c.target_state == state::complete
					? 1U
					: 0U;
				if (c.scope_state != state::complete || c.declaration_state != state::complete)
					std::cerr << "frontier " << c.exit << " scope "
							  << static_cast<unsigned>(c.scope_state) << " decl "
							  << static_cast<unsigned>(c.declaration_state) << "\n";
			}
		std::cout << "actual specs " << projected.specifications.size() << " known true "
				  << known_true << " false " << known_false << " partial " << partial
				  << " cleanups " << emissions << " normal " << normal << " exceptional "
				  << exceptional << " declarations " << bound << " targets " << targets << " work "
				  << observed.operations << " bytes " << observed.retained_bytes_bound << "\n";
		require(known_true > 0U && known_false > 0U && partial > 0U && normal > 0U &&
					exceptional > 0U && bound > 0U && bound < emissions && targets > 0U,
				"actual typed spec/normal+EH cleanup original binding");
		return 0;
	}

	ordered_identity_evidence_controls();
	specification_only_controls();

	// Shared span IDs deliberately hash to one bucket; worlds still stay distinct.
	{
		fixture indexed;
		auto alternative = indexed.rows[2].front();
		alternative.provenance = {"provenance:alternative-span"};
		alternative.contributor_edges.front().provenance = alternative.provenance.front();
		indexed.rows[2].push_back(std::move(alternative));
		for (const unsigned axis : {0U, 1U, 2U})
		{
			auto foreign = indexed.rows[2].front();
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
			set(foreign, "begin", detached_cell::unsigned_integer(99U));
			set(foreign, "end", detached_cell::unsigned_integer(100U));
			indexed.rows[2].push_back(std::move(foreign));
		}
		for (unsigned i{}; i < 24U; ++i)
		{
			auto unrelated = indexed.rows[2].front();
			set(unrelated,
				"span",
				detached_cell::utf8(std::string(4096U, 's') + std::to_string(i)));
			indexed.rows[2].push_back(std::move(unrelated));
		}
		q::projection_resource_usage measured;
		auto out = take(q::project_exception_cleanup_facets(indexed.input(), {}, {}, measured));
		require(spec(out).source_state == state::complete,
				"span ID collision retains all exact world axes");
		std::vector<std::string> expected_evidence;
		for (const auto& evidence : out.evidence)
			expected_evidence.push_back(evidence.relation_id + evidence.row.canonical_form());
		std::ranges::reverse(indexed.rows[2]);
		out = take(q::project_exception_cleanup_facets(indexed.input()));
		std::vector<std::string> reordered_evidence;
		for (const auto& evidence : out.evidence)
			reordered_evidence.push_back(evidence.relation_id + evidence.row.canonical_form());
		require(spec(out).source_state == state::complete &&
					reordered_evidence == expected_evidence,
				"span collision and long-prefix input order preserve canonical evidence");
		q::projection_resource_usage baseline;
		take(q::project_exception_cleanup_facets(indexed.input(), {}, {}, baseline));
		for (const bool storage : {false, true})
			for (const bool one_under : {false, true})
			{
				q::finite_population_limits bounded;
				if (storage)
					bounded.maximum_retained_bytes =
						baseline.retained_bytes_bound - static_cast<std::size_t>(one_under);
				else
					bounded.maximum_operations =
						baseline.operations - static_cast<std::size_t>(one_under);
				q::projection_resource_usage spent{1U, 1U};
				const auto result =
					q::project_exception_cleanup_facets(indexed.input(), bounded, {}, spent);
				require(static_cast<bool>(result) == !one_under,
						"span lookup exact and one-under quota");
				if (one_under)
					require(spent.operations == 0U && spent.retained_bytes_bound == 0U,
							"failed span lookup keeps the existing failure usage contract");
			}
		q::finite_population_limits cancelled;
		std::size_t checkpoints{};
		cancelled.cancelled = [&]
		{
			return ++checkpoints == 1000U;
		};
		q::projection_resource_usage spent{1U, 1U};
		const auto stopped =
			q::project_exception_cleanup_facets(indexed.input(), cancelled, {}, spent);
		require(!stopped && stopped.error().code == "sdk.exception-cleanup-cancelled" &&
					checkpoints == 1000U && spent.operations == 0U &&
					spent.retained_bytes_bound == 0U,
				"late span lookup stop preserves cancellation and failure usage");
	}
	fixture copied;
	query_copy_controls::projection(
		copied.rows,
		names,
		[&]
		{
			return copied.queries();
		},
		[](const auto& input, const auto& limits, auto& usage)
		{
			return q::project_exception_cleanup_facets(input, limits, {}, usage);
		},
		require);

	fixture original;
	q::projection_resource_usage usage;
	auto out = take(q::project_exception_cleanup_facets(original.input(), {}, {}, usage));
	require(spec(out).specification_state == state::complete && spec(out).nonthrowing == true &&
				spec(out).identity_state == state::complete,
			"stored known noexcept and identity");
	require(cleanup(out).emission_state == state::complete &&
				cleanup(out).scope_state == state::complete &&
				cleanup(out).declaration_state == state::complete &&
				cleanup(out).declaration_source_state == state::complete &&
				cleanup(out).target_attribution_state == state::complete &&
				cleanup(out).target_state == state::complete &&
				cleanup(out).target_dtor_type == 0U && cleanup(out).target_usr.size() == 5U,
			"exact cleanup/body/variant/VarDecl/destructor zero enum raw USR");
	require(out.cleanups.size() == 2U && out.cleanups.back().emission_state != state::complete,
			"absent optional cleanup is not complete zero");
	require(usage.operations > 0U && usage.retained_bytes_bound > 0U, "measured usage");
	auto queried = take(q::project_exception_cleanup_facets(original.queries()));
	require(queried.source_queries && queried.detail_inputs_complete &&
				queried.exit_inputs_complete && queried.declaration_inputs_complete &&
				cleanup(queried).declaration_state == state::complete,
			"public independent scans ignore unrelated broad partiality");
	fixture absent = original;
	for (auto name : {"exception_spec_kind",
					  "exception_spec_nonthrowing",
					  "exception_spec_state",
					  "exception_spec_profile"})
		absent.rows[4][0].values.erase("output." + std::string{name});
	require(spec(take(q::project_exception_cleanup_facets(absent.input()))).specification_state ==
				state::unknown,
			"old saved optional spec fields remain unobserved");
	fixture bodyless = original;
	bodyless.rows[5].clear();
	bodyless.rows[6].clear();
	require(spec(take(q::project_exception_cleanup_facets(bodyless.input()))).specification_state ==
				state::complete,
			"stored exception spec independent bodyless declarations");
	for (auto kind : {"dependent_noexcept", "unevaluated", "uninstantiated", "unparsed"})
	{
		fixture partial = original;
		set(partial.rows[4][0], "exception_spec_kind", detached_cell::utf8(kind));
		set(partial.rows[4][0], "exception_spec_state", detached_cell::utf8("partial"));
		set(partial.rows[4][0],
			"exception_spec_nonthrowing",
			detached_cell::absent(
				partial.rows[4][0].values.at("output.exception_spec_nonthrowing").type));
		require(
			spec(take(q::project_exception_cleanup_facets(partial.input()))).specification_state ==
				state::partial,
			"stored lazy spec unknown truth");
		set(partial.rows[4][0], "exception_spec_nonthrowing", detached_cell::boolean(true));
		require(
			spec(take(q::project_exception_cleanup_facets(partial.input()))).specification_state ==
				state::conflicting,
			"lazy spec cannot claim truth");
	}
	fixture false_spec = original;
	set(false_spec.rows[4][0], "exception_spec_kind", detached_cell::utf8("noexcept_false"));
	set(false_spec.rows[4][0], "exception_spec_nonthrowing", detached_cell::boolean(false));
	require(
		spec(take(q::project_exception_cleanup_facets(false_spec.input()))).specification_state ==
			state::complete,
		"known throwing specification");
	set(false_spec.rows[4][0], "exception_spec_nonthrowing", detached_cell::boolean(true));
	require(
		spec(take(q::project_exception_cleanup_facets(false_spec.input()))).specification_state ==
			state::conflicting,
		"kind truth contradiction");
	fixture future = original;
	set(future.rows[4][0], "exception_spec_kind", detached_cell::utf8("future_kind"));
	require(spec(take(q::project_exception_cleanup_facets(future.input()))).specification_state ==
				state::partial,
			"future spec kind frontier");

	fixture future_entity = original;
	set(future_entity.rows[3][0], "kind", detached_cell::utf8("future_callable"));
	require(spec(take(q::project_exception_cleanup_facets(future_entity.input()))).identity_state !=
				state::complete,
			"future callable identity cannot silently disappear or close");
	fixture future_variant = original;
	for (auto& exit : future_variant.rows[6])
		set(exit, "variant_kind", detached_cell::utf8("future_variant"));
	require(
		cleanup(take(q::project_exception_cleanup_facets(future_variant.input()))).scope_state !=
			state::complete,
		"future lowering variant stays unavailable");
	fixture future_dtor = original;
	set(future_dtor.rows[6][1], "cleanup_target_dtor_type", detached_cell::unsigned_integer(99));
	require(cleanup(take(q::project_exception_cleanup_facets(future_dtor.input())))
					.target_attribution_state != state::complete,
			"future destructor ABI value retains frontier");
	fixture duplicate = original;
	auto unrelated = duplicate.rows[4][0];
	set(unrelated, "flags", symbols({"const"}));
	duplicate.rows[4].push_back(unrelated);
	auto same = take(q::project_exception_cleanup_facets(duplicate.input()));
	require(spec(same).specification_state == state::complete &&
				same.evidence.size() > out.evidence.size(),
			"equal consumed facet preserves unrelated original duplicate evidence");
	set(duplicate.rows[4].back(), "exception_spec_nonthrowing", detached_cell::boolean(false));
	require(
		spec(take(q::project_exception_cleanup_facets(duplicate.input()))).specification_state ==
			state::conflicting,
		"contradictory original stored facet");
	fixture foreign = original;
	set(foreign.rows[8][0], "compile_unit", detached_cell::utf8("unit:foreign"));
	require(cleanup(take(q::project_exception_cleanup_facets(foreign.input()))).declaration_state !=
				state::complete,
			"declaration cannot borrow foreign unit membership");
	fixture owner = original;
	set(owner.rows[3][1], "semantic_owner", detached_cell::utf8("function:foreign"));
	require(cleanup(take(q::project_exception_cleanup_facets(owner.input()))).declaration_state ==
				state::conflicting,
			"present foreign declaration owner contradiction");
	fixture missing_decl = original;
	missing_decl.rows[7].clear();
	auto missing = take(q::project_exception_cleanup_facets(missing_decl.input()));
	require(cleanup(missing).emission_state == state::complete &&
				cleanup(missing).declaration_state == state::unknown,
			"emission independent missing VarDecl binding");

	fixture unbound = original;
	for (auto field : {"cleanup_declaration", "cleanup_registration_ordinal", "cleanup_target"})
		unbound.rows[6][1].values.erase("output." + std::string{field});
	auto independent = take(q::project_exception_cleanup_facets(unbound.input()));
	require(cleanup(independent).emission_state == state::complete &&
				cleanup(independent).registration_state == state::unknown &&
				cleanup(independent).declaration_state == state::unknown &&
				cleanup(independent).target_attribution_state == state::complete &&
				cleanup(independent).target_state == state::unknown,
			"original emitter and destructor attribution independent missing normalized FKs");

	fixture wrong_carrier = original;
	auto another_body = wrong_carrier.rows[5][0];
	set(another_body, "body", detached_cell::utf8("body:other"));
	wrong_carrier.rows[5].push_back(another_body);
	set(wrong_carrier.rows[6][0], "body", detached_cell::utf8("body:other"));
	require(cleanup(take(q::project_exception_cleanup_facets(wrong_carrier.input()))).scope_state ==
				state::conflicting,
			"present carrier body must match exact original cleanup body");
	fixture zero = original;
	set(zero.rows[6][1], "cleanup_emission_ordinal", detached_cell::unsigned_integer(0));
	require(cleanup(take(q::project_exception_cleanup_facets(zero.input()))).emission_state ==
				state::conflicting,
			"emission ordinal starts one");
	fixture helper = original;
	set(helper.rows[6][1], "emitter_methods", detached_cell::unsigned_integer(8));
	require(cleanup(take(q::project_exception_cleanup_facets(helper.input())))
					.target_attribution_state == state::conflicting,
			"runtime helper cannot claim destructor target");
	for (auto name : {"cleanup_target",
					  "cleanup_target_usr",
					  "cleanup_target_dtor_type",
					  "cleanup_target_profile"})
		helper.rows[6][1].values.erase("output." + std::string{name});
	require(cleanup(take(q::project_exception_cleanup_facets(helper.input())))
						.destructor_target_excluded == true &&
				cleanup(take(q::project_exception_cleanup_facets(helper.input())))
						.target_attribution_state == state::complete,
			"known runtime emitter exclusion independent unknown target");

	fixture malformed_set = original;
	set(malformed_set.rows[8][0], "declarations", symbols({"decl:object", "decl:object"}));
	set(malformed_set.rows[8][0], "declaration_count", detached_cell::unsigned_integer(2));
	require(!q::project_exception_cleanup_facets(malformed_set.input()),
			"duplicate original set members cannot close declaration binding");
	fixture wrong_members = original;
	auto conflicting_inventory = wrong_members.rows[8][0];
	set(conflicting_inventory, "declarations", symbols({}));
	set(conflicting_inventory, "declaration_count", detached_cell::unsigned_integer(0));
	wrong_members.rows[8].push_back(conflicting_inventory);
	require(cleanup(take(q::project_exception_cleanup_facets(wrong_members.input())))
					.declaration_state == state::conflicting,
			"contradictory same original inventory cannot choose an admitted winner");
	fixture target_world = original;
	target_world.rows[3][2].presence.universe = "foreign:world";
	target_world.rows[3][2].contributor_edges[0].condition.universe = "foreign:world";
	require(cleanup(take(q::project_exception_cleanup_facets(target_world.input()))).target_state ==
				state::unknown,
			"target exact original world");
	fixture malformed = original;
	malformed.rows[2].push_back(malformed.rows[2][0]);
	malformed.rows[2].back().values.erase("output.begin");
	require(!q::project_exception_cleanup_facets(malformed.input()),
			"malformed unused original row still validated");
	auto missing_scan = original.queries();
	missing_scan.scans.pop_back();
	auto scan = take(q::project_exception_cleanup_facets(missing_scan));
	require(!scan.declaration_inputs_complete && !scan.unresolved.empty() &&
				cleanup(scan).declaration_state != state::complete,
			"missing original scan preserved");
	q::finite_population_limits cap;
	cap.maximum_retained_bytes = usage.retained_bytes_bound;
	auto exact = take(q::project_exception_cleanup_facets(original.input(), cap));
	require(cleanup(exact).emission_state == state::complete,
			"exact successful reported peak reservation");
	cap.maximum_retained_bytes = 1U;
	usage = {1U, 1U};
	require(!q::project_exception_cleanup_facets(original.input(), cap, {}, usage) &&
				usage.operations == 0U && usage.retained_bytes_bound == 0U,
			"bounded storage failure resets usage");
	std::stop_source stopped;
	stopped.request_stop();
	usage = {1U, 1U};
	require(
		!q::project_exception_cleanup_facets(original.input(), {}, stopped.get_token(), usage) &&
			usage.operations == 0U && usage.retained_bytes_bound == 0U,
		"cancellation resets usage");
	std::cout << "exception-cleanup facets original positive/negative/resource tests PASS\n";
}
