#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <locale>
#include <memory>
#include <new>
#include <sstream>
#include <tuple>

#include <cxxlens/sdk/exceptional_exits.hpp>
#include <cxxlens/sdk/query_transfer.hpp>

#include "../../../src/sdk/query_projection_row_copy_internal.hpp"
#include "../../../src/sdk/query_result_internal.hpp"
#include "query_projection_row_copy_controls.hpp"

#if !defined(CXXLENS_TSAN_ALLOCATION_FAULT_TESTS_DISABLED)
namespace
{
	thread_local int copy_allocation_failure = -1;
	thread_local std::stop_source* prefix_allocation_stop = nullptr;
} // namespace
void* operator new(std::size_t size)
{
	if (prefix_allocation_stop && size >= 4096U)
		prefix_allocation_stop->request_stop();
	if (copy_allocation_failure >= 0 && copy_allocation_failure-- == 0)
		throw std::bad_alloc{};
	if (auto* value = std::malloc(size == 0U ? 1U : size))
		return value;
	throw std::bad_alloc{};
}
void* operator new[](std::size_t size)
{
	return ::operator new(size);
}
void operator delete(void* value) noexcept
{
	std::free(value);
}
void operator delete[](void* value) noexcept
{
	::operator delete(value);
}
void operator delete(void* value, std::size_t) noexcept
{
	std::free(value);
}
void operator delete[](void* value, std::size_t) noexcept
{
	::operator delete(value);
}
#endif

namespace
{
	using namespace cxxlens::sdk;
	namespace q = cxxlens::sdk::query;
	using state = q::finite_population_state;
	constexpr std::array<std::string_view, 8> names{"build.compile_unit.v1",
													"source.file.v1",
													"source.span.v1",
													"cc.entity.v1",
													"cc.entity_detail.v1",
													"cc.body.v1",
													"cc.syntax_node.v1",
													"cc.exceptional_exit.v1"};
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
	constexpr std::string_view profile = "clang22-original-exceptional-occurrences/1",
							   lowering = "clang22-written-definition-analysis-lowering/1";
	struct fixture
	{
		std::array<std::vector<q::annotated_row>, 8> rows;
		fixture()
		{
			rows[0] = {fact(0, {{"compile_unit", detached_cell::utf8("unit:a")}})};
			rows[1] = {fact(1,
							{{"snapshot", detached_cell::utf8("source:a")},
							 {"file", detached_cell::utf8("file:a")},
							 {"size", detached_cell::unsigned_integer(100)}})};
			rows[2] = {fact(2,
							{{"span", detached_cell::utf8("span:scope")},
							 {"snapshot", detached_cell::utf8("source:a")},
							 {"file", detached_cell::utf8("file:a")},
							 {"begin", detached_cell::unsigned_integer(0)},
							 {"end", detached_cell::unsigned_integer(90)}}),
					   fact(2,
							{{"span", detached_cell::utf8("span:throw")},
							 {"snapshot", detached_cell::utf8("source:a")},
							 {"file", detached_cell::utf8("file:a")},
							 {"begin", detached_cell::unsigned_integer(10)},
							 {"end", detached_cell::unsigned_integer(20)}})};
			rows[2].push_back(fact(2,
								   {{"span", detached_cell::utf8("span:body")},
									{"snapshot", detached_cell::utf8("source:a")},
									{"file", detached_cell::utf8("file:a")},
									{"begin", detached_cell::unsigned_integer(5)},
									{"end", detached_cell::unsigned_integer(80)}}));

			rows[3] = {fact(3,
							{{"entity", detached_cell::utf8("function:a")},
							 {"kind", detached_cell::utf8("function")}})};
			rows[4] = {fact(4,
							{{"detail", detached_cell::utf8("detail:a")},
							 {"entity", detached_cell::utf8("function:a")},
							 {"compile_unit", detached_cell::utf8("unit:a")},
							 {"source", detached_cell::utf8("span:scope")},
							 {"flags", symbols({"body_written", "finite_function_body_v1"})}})};
			rows[5] = {fact(5,
							{{"body", detached_cell::utf8("body:a")},
							 {"function", detached_cell::utf8("function:a")},
							 {"compile_unit", detached_cell::utf8("unit:a")},
							 {"source", detached_cell::utf8("span:scope")}})};
			set(rows[5][0], "source", detached_cell::utf8("span:body"));

			rows[6] = {fact(6,
							{{"node", detached_cell::utf8("syntax:throw")},
							 {"function", detached_cell::utf8("function:a")},
							 {"compile_unit", detached_cell::utf8("unit:a")},
							 {"source", detached_cell::utf8("span:throw")}})};
			rows[7] = {exit("variant:a", "lowering_variant", 0),
					   exit("exit:throw", "written_throw", 1),
					   exit("exit:helper", "lowering_helper", 2)};
			set(rows[7][1], "variant", detached_cell::utf8("variant:a"));
			set(rows[7][1], "source", detached_cell::utf8("span:throw"));
			set(rows[7][1], "expression", detached_cell::utf8("syntax:throw"));
			set(rows[7][2], "variant", detached_cell::utf8("variant:a"));
			set(rows[7][2], "emitter_methods", detached_cell::unsigned_integer(8));
			set(rows[7][2], "block_ordinal", detached_cell::unsigned_integer(2));
			set(rows[7][2], "instruction_ordinal", detached_cell::unsigned_integer(1));
			set(rows[7][2], "does_not_return", detached_cell::boolean(true));
			census();
		}
		q::annotated_row
		exit(std::string_view id, std::string_view role, std::uint64_t ordinal) const
		{
			return fact(
				7,
				{{"exit", detached_cell::utf8(std::string{id})},
				 {"compile_unit", detached_cell::utf8("unit:a")},
				 {"scope_detail", detached_cell::utf8("detail:a")},
				 {"function", detached_cell::utf8("function:a")},
				 {"definition_source", detached_cell::utf8("span:scope")},
				 {"body", detached_cell::utf8("body:a")},
				 {"variant_kind", detached_cell::utf8("function")},
				 {"variant_index", detached_cell::unsigned_integer(0)},
				 {"variant_symbol", binary(std::string_view{"f\0\xff", 3})},
				 {"ordinal", detached_cell::unsigned_integer(ordinal)},
				 {"profile", detached_cell::utf8(std::string{profile})},
				 {"lowering_profile", detached_cell::utf8(std::string{lowering})},
				 {"role", detached_cell::utf8(std::string{role})},
				 {"eligibility",
				  detached_cell::utf8(role == "lowering_variant" || role == "lowering_helper" ||
											  role == "ordinary_instruction"
										  ? "excluded"
										  : "eligible")},
				 {"observation_state", detached_cell::utf8("complete")}});
		}
		void census()
		{
			std::vector<std::string> ids;
			for (const auto& r : rows[7])
				ids.push_back(std::get<std::string>(*r.values.at("output.exit").value));
			std::ranges::sort(ids);
			ids.erase(std::ranges::unique(ids).begin(), ids.end());
			std::vector<std::byte> out;
			for (const auto& id : ids)
			{
				for (unsigned shift = 0; shift < 32; shift += 8)
					out.push_back(static_cast<std::byte>((id.size() >> shift) & 255U));
				for (char c : id)
					out.push_back(static_cast<std::byte>(c));
			}
			for (auto group : {4U, 5U})
				for (auto& r : rows[group])
				{
					set(r, "exceptional_exit_count", detached_cell::unsigned_integer(ids.size()));
					set(r, "exceptional_exit_ids", detached_cell::bytes(out));
					set(r, "exceptional_exit_state", detached_cell::utf8("complete"));
					set(r, "exceptional_exit_profile", detached_cell::utf8(std::string{profile}));
					set(r,
						"exceptional_lowering_profile",
						detached_cell::utf8(std::string{lowering}));
				}
		}
		q::exceptional_exit_input input() const
		{
			return {rows[0],
					rows[1],
					rows[2],
					rows[3],
					rows[4],
					rows[5],
					rows[6],
					rows[7],
					true,
					true,
					true};
		}
		q::application_query_results queries(bool broad = true, bool sizes = false) const
		{
			q::application_query_results output;
			output.snapshot_id = "query:exceptional";
			for (std::size_t group = 0; group < rows.size(); ++group)
			{
				auto data = std::make_shared<q::query_result::data>();
				data->row_values = rows[group];
				if (sizes)
				{
					for (const auto& row : data->row_values)
					{
						require(bool(row.validate()), "prefix sizing fixture generic admission");
						std::ostringstream multiplicity;
						multiplicity << row.multiplicity;
						data->row_wire_base_sizes.push_back(row.canonical_form().size() -
															multiplicity.str().size());
					}
					data->rows_validated = true;
				}
				data->status = q::execution_status::complete;
				data->input_complete = broad;
				data->ordered = true;
				data->snapshot = output.snapshot_id;
				output.scans.push_back({std::string{names[group]},
										{},
										q::query_transfer_access::make(std::move(data))});
			}
			return output;
		}
	};
	const q::exceptional_exit_population& population(const q::exceptional_exit_projection& result)
	{
		require(result.populations.size() == 1, "one physical population");
		return result.populations.front();
	}
	void claims(q::annotated_row& row, std::vector<std::string> values)
	{
		std::ranges::sort(values);
		row.claim_contributors = std::move(values);
		const auto edge = row.contributor_edges.front();
		row.contributor_edges.clear();
		for (const auto& claim : row.claim_contributors)
		{
			auto next = edge;
			next.claim_contributor = claim;
			row.contributor_edges.push_back(std::move(next));
		}
		require(bool(row.validate()), "prefix fixture annotations stay authoritative");
	}
	void canonical_evidence_order(const q::exceptional_exit_projection& result)
	{
		std::vector<std::pair<std::size_t, std::string>> expected;
		for (const auto& evidence : result.evidence)
		{
			const auto group = std::ranges::find(names, evidence.relation_id);
			require(group != names.end(), "prefix evidence relation remains known");
			expected.emplace_back(static_cast<std::size_t>(group - names.begin()),
								  evidence.row.canonical_form());
		}
		std::ranges::sort(expected);
		for (std::size_t i{}; i < expected.size(); ++i)
			require(result.evidence[i].relation_id == names[expected[i].first] &&
						result.evidence[i].row.canonical_form() == expected[i].second,
					"first-field cohorts changed complete canonical evidence order");
	}
	void canonical_prefix_controls()
	{
		fixture original;
		const std::string long_prefix(4096U, 'x');
		const std::array<std::vector<std::string>, 8> prefixes{
			std::vector<std::string>{"claim:a"},
			std::vector<std::string>{"claim:a", "claim:z"},
			std::vector<std::string>{"claim:a\""},
			std::vector<std::string>{"claim:a\\"},
			std::vector<std::string>{"claim:a\n"},
			std::vector<std::string>{"claim:日本語"},
			std::vector<std::string>{"claim:" + long_prefix + ":a"},
			std::vector<std::string>{"claim:" + long_prefix + ":b"}};
		for (std::size_t i{}; i < prefixes.size(); ++i)
		{
			auto row = fact(3U,
							{{"entity", detached_cell::utf8("decoy:" + std::to_string(i))},
							 {"kind", detached_cell::utf8("function")}});
			claims(row, prefixes[i]);
			original.rows[3].push_back(std::move(row));
		}
		// Equal first fields require every remaining field, including the last cell,
		// to retain the public canonical writer's order.
		auto tied = original.rows[3].back();
		set(tied, "kind", detached_cell::utf8("variable"));
		tied.multiplicity = 1234U;
		original.rows[3].push_back(tied);
		original.rows[3].push_back(tied);
		q::projection_resource_usage raw_usage, fact_usage;
		const auto raw = take(q::project_exceptional_exits(original.input(), {}, {}, raw_usage));
		const auto query = original.queries(true, true);
		const auto admitted = take(q::project_exceptional_exits(query, {}, {}, fact_usage));
		canonical_evidence_order(raw);
		canonical_evidence_order(admitted);
		require(raw.evidence.size() == admitted.evidence.size(),
				"prefix facts omit no original row");
		for (std::size_t i{}; i < raw.evidence.size(); ++i)
			require(raw.evidence[i].row.canonical_form() ==
						admitted.evidence[i].row.canonical_form(),
					"prefix fact/native routes change original evidence bytes");
		for (auto& rows : original.rows)
			std::ranges::reverse(rows);
		const auto reversed = take(q::project_exceptional_exits(original.input()));
		for (std::size_t i{}; i < raw.evidence.size(); ++i)
			require(raw.evidence[i].row.canonical_form() ==
						reversed.evidence[i].row.canonical_form(),
					"prefix tie ordering depends on input order");
		q::finite_population_limits limits;
		limits.maximum_operations = fact_usage.operations;
		limits.maximum_retained_bytes = fact_usage.retained_bytes_bound;
		limits.maximum_evidence_bytes = 0U;
		for (const auto& evidence : admitted.evidence)
			limits.maximum_evidence_bytes += evidence.row.canonical_form().size();
		q::projection_resource_usage repeated;
		require(bool(q::project_exceptional_exits(query, limits, {}, repeated)) &&
					repeated.operations == fact_usage.operations &&
					repeated.retained_bytes_bound == fact_usage.retained_bytes_bound,
				"prefix exact work/storage/full-evidence frontier changed");
		--limits.maximum_evidence_bytes;
		require(!q::project_exceptional_exits(query, limits, {}, repeated) &&
					!repeated.operations && !repeated.retained_bytes_bound,
				"prefix-only length replaced the full evidence limit");
		++limits.maximum_evidence_bytes;
		--limits.maximum_operations;
		require(!q::project_exceptional_exits(query, limits, {}, repeated) &&
					!repeated.operations && !repeated.retained_bytes_bound,
				"prefix one-under work published partial evidence");
		++limits.maximum_operations;
		--limits.maximum_retained_bytes;
		require(!q::project_exceptional_exits(query, limits, {}, repeated) &&
					!repeated.operations && !repeated.retained_bytes_bound,
				"prefix one-under storage published partial evidence");
		std::size_t visits{};
		limits = {};
		limits.cancelled = [&]
		{
			return ++visits == 400U;
		};
		require(!q::project_exceptional_exits(query, limits, {}, repeated) && visits == 400U &&
					!repeated.operations && !repeated.retained_bytes_bound,
				"prefix work ignores actual mid-operation stop");
		require(bool(q::project_exceptional_exits(query, {}, {}, repeated)),
				"prefix fresh retry after stop");
#if !defined(CXXLENS_TSAN_ALLOCATION_FAULT_TESTS_DISABLED)
		std::stop_source stopped;
		prefix_allocation_stop = &stopped;
		const auto interrupted =
			q::project_exceptional_exits(query, {}, stopped.get_token(), repeated);
		prefix_allocation_stop = nullptr;
		require(!interrupted && stopped.stop_requested() && !repeated.operations &&
					!repeated.retained_bytes_bound,
				"actual large prefix allocation stop published partial evidence");
		require(bool(q::project_exceptional_exits(query, {}, {}, repeated)),
				"prefix fresh retry after actual allocation stop");
#endif
		set(original.rows[3].back(),
			"kind",
			detached_cell::utf8(std::string{"function\0late", 13U}));
		require(!q::project_exceptional_exits(original.input(), {}, {}, repeated) &&
					!repeated.operations && !repeated.retained_bytes_bound,
				"prefix ordering bypassed full late NUL cell admission");
		set(original.rows[3].back(), "kind", detached_cell::utf8(std::string{"late\xff", 5U}));
		require(!q::project_exceptional_exits(original.input(), {}, {}, repeated) &&
					!repeated.operations && !repeated.retained_bytes_bound,
				"prefix ordering bypassed full late UTF8 cell admission");
	}
	void canonical_prefix_locale_controls()
	{
		struct grouped : std::numpunct<char>
		{
			char do_thousands_sep() const override
			{
				return ',';
			}
			std::string do_grouping() const override
			{
				return "\3";
			}
		};
		fixture original;
		original.rows[3].front().multiplicity = 1234567U;
		const auto admitted = original.queries(true, true);
		const auto previous = std::locale();
		struct restore
		{
			std::locale previous;
			~restore()
			{
				std::locale::global(previous);
			}
		} guard{previous};
		std::locale::global(std::locale{previous, new grouped});
		q::projection_resource_usage usage;
		const auto projected = take(q::project_exceptional_exits(admitted, {}, {}, usage));
		canonical_evidence_order(projected);
		q::finite_population_limits cap;
		cap.maximum_evidence_bytes = 0U;
		for (const auto& evidence : projected.evidence)
			cap.maximum_evidence_bytes += evidence.row.canonical_form().size();
		require(bool(q::project_exceptional_exits(admitted, cap, {}, usage)),
				"prefix fact froze the multiplicity encoding locale");
		--cap.maximum_evidence_bytes;
		require(!q::project_exceptional_exits(admitted, cap, {}, usage) && !usage.operations,
				"locale evidence one-under cap ignored dynamic multiplicity");
	}
	void transfer_prefix_shape_controls(const char* path)
	{
		relation_registry registry;
		for (const auto& descriptor : standard_relation_descriptors())
			require(bool(registry.add(descriptor)), "prefix transfer registry admission");
		auto engine = take(registry.build("exceptional-prefix-original"));
		std::ifstream file(path, std::ios::binary);
		require(file.good(), "prefix actual saved query present");
		std::string data{std::istreambuf_iterator<char>{file}, {}};
		const auto admitted = take(q::decode_application_queries(engine, data));
		canonical_evidence_order(take(q::project_exceptional_exits(admitted)));
		const std::string_view opening = "\"rows\":[{";
		const auto row = data.find(opening);
		require(row != std::string::npos, "prefix original transfer has a row");
		data.insert(row + opening.size(), "\"aaaa_unknown_first_field\":null,");
		require(!q::decode_application_queries(engine, data),
				"unknown first top-level row field entered the fixed canonical grammar");
	}
	void typed_copy_failure_controls()
	{
		auto row = fixture{}.rows[3].front();
		row.multiplicity = 17U;
		row.values.emplace("copy.long.boolean.field", detached_cell::boolean(true));
		row.values.emplace("copy.long.signed.field",
						   detached_cell::signed_integer(std::numeric_limits<std::int64_t>::min()));
		row.values.emplace(
			"copy.long.unsigned.field",
			detached_cell::unsigned_integer(std::numeric_limits<std::uint64_t>::max()));
		row.values.emplace("copy.long.string.field", detached_cell::utf8("quoted\"\\\n\t日本語"));
		row.values.emplace("copy.long.binary.field",
						   detached_cell::bytes(std::vector<std::byte>(32768U, std::byte{0xff})));
		const value_type optional{scalar_kind::typed_id, "long-original-type-parameter", true};
		row.values.emplace("copy.long.absent.field", detached_cell::absent(optional));
		row.values.emplace(
			"copy.long.unknown.field",
			detached_cell::unknown(optional, "unobserved\"\\日本語-original-reason"));
		row.claim_contributors.push_back("claim:z-long-original-contributor");
		row.producer_contracts.push_back({"z.original-long-producer", "z.original-long-contract"});
		row.provenance.push_back("z.original-long-provenance");
		row.contributor_guarantees.push_back(
			{"exact", "zz-long-scope", "zz-long-assumption", {"native", "schema_validated"}});
		row.contributor_edges.push_back({row.claim_contributors.back(),
										 row.producer_contracts.back(),
										 row.provenance.back(),
										 row.contributor_guarantees.back(),
										 row.presence,
										 row.interpretation});
		const auto expected = row.canonical_form();
		std::size_t work{};
		const auto copied = q::detail::copy_projected_row(row,
														  [&](std::size_t amount)
														  {
															  work += amount;
														  });
		require(copied.canonical_form() == expected,
				"typed copy retained all scalar/optional/nested fields");
		struct interrupted
		{
		};
		for (const auto cap : {work, work - 1U})
		{
			std::size_t used{};
			bool failed{};
			try
			{
				const auto bounded = q::detail::copy_projected_row(row,
																   [&](std::size_t amount)
																   {
																	   if (amount > cap - used)
																		   throw interrupted{};
																	   used += amount;
																   });
				require(bounded.canonical_form() == expected,
						"typed copy exact-bound retry changed input");
			}
			catch (const interrupted&)
			{
				failed = true;
			}
			require(failed == (cap != work) && used > 0U && used <= cap,
					"typed copy did not gate actual work before copying");
		}
		std::size_t callbacks{}, used{};
		bool cancelled{};
		try
		{
			(void)q::detail::copy_projected_row(row,
												[&](std::size_t amount)
												{
													if (++callbacks == 30U)
														throw interrupted{};
													used += amount;
												});
		}
		catch (const interrupted&)
		{
			cancelled = true;
		}
		require(cancelled && used > 0U && row.canonical_form() == expected,
				"typed copy ignored mid-copy cancellation or mutated borrowed input");
#if !defined(CXXLENS_TSAN_ALLOCATION_FAULT_TESTS_DISABLED)
		for (const int point : {0, 1, 4, 16})
		{
			used = 0U;
			bool failed{};
			copy_allocation_failure = point;
			try
			{
				(void)q::detail::copy_projected_row(row,
													[&](std::size_t amount)
													{
														used += amount;
													});
			}
			catch (const std::bad_alloc&)
			{
				failed = true;
			}
			copy_allocation_failure = -1;
			require(failed && used > 0U && row.canonical_form() == expected,
					"typed copy allocation failure lost work or mutated original "
					"ownership");
		}
#endif
		const auto retry = q::detail::copy_projected_row(row,
														 [](std::size_t)
														 {
														 });
		require(retry.canonical_form() == expected,
				"typed copy retry failed after partial-owner unwind");
	}

	void indexed_prefix_controls()
	{
		constexpr std::size_t common_bytes = 120U, decoy_count = 64U;
		fixture long_prefix;
		const std::array old_ids{"span:scope", "span:throw", "span:body"};
		const std::array new_ids{std::string(common_bytes, 's') + "source:A",
								 std::string(common_bytes, 's') + "source:B",
								 std::string(common_bytes, 's') + "source:C"};
		for (auto& group : long_prefix.rows)
			for (auto& row : group)
				for (auto& [name, cell] : row.values)
				{
					(void)name;
					if (!cell.value)
						continue;
					if (auto* value = std::get_if<std::string>(&*cell.value))
						for (std::size_t i{}; i < old_ids.size(); ++i)
							if (*value == old_ids[i])
								*value = new_ids[i];
				}
		for (std::size_t i{}; i < decoy_count; ++i)
		{
			auto row = long_prefix.rows[2].front();
			auto suffix = std::to_string(i);
			if (suffix.size() == 1U)
				suffix.insert(suffix.begin(), '0');
			set(row,
				"span",
				detached_cell::utf8(std::string(common_bytes - 2U, 's') + suffix + "source:A"));
			long_prefix.rows[2].push_back(std::move(row));
		}
		auto early_prefix = long_prefix;
		for (std::size_t i = old_ids.size(); i < early_prefix.rows[2].size(); ++i)
		{
			auto id =
				std::get<std::string>(*early_prefix.rows[2][i].values.at("output.span").value);
			id.front() = 'z';
			set(early_prefix.rows[2][i], "span", detached_cell::utf8(std::move(id)));
		}
		std::size_t visits{};
		q::finite_population_limits limits;
		limits.cancelled = [&]
		{
			++visits;
			return false;
		};
		q::projection_resource_usage measured, early_usage, repeated;
		const auto full =
			take(q::project_exceptional_exits(long_prefix.input(), limits, {}, measured));
		const auto early =
			take(q::project_exceptional_exits(early_prefix.input(), {}, {}, early_usage));
		require(population(full).state == state::complete &&
					population(early).state == state::complete &&
					population(full).occurrence_ids == population(early).occurrence_ids &&
					full.evidence.size() == early.evidence.size(),
				"prefix-only unreferenced originals changed finite exit closure");
		for (std::size_t i{}; i < full.evidence.size(); ++i)
			require(full.evidence[i].row.canonical_form() == early.evidence[i].row.canonical_form(),
					"unreferenced prefix decoys changed selected original evidence");
		require(measured.retained_bytes_bound == early_usage.retained_bytes_bound,
				"visited-prefix work changed selected owned/index/temporary storage");
		// Every decoy deliberately has the same size and final eight bytes as a
		// needed source. Candidate filtering must still compare complete IDs before
		// excluding it; equal-size decoys keep validation and output work equal.
		require(measured.operations > early_usage.operations &&
					measured.operations - early_usage.operations >=
						2U * (common_bytes - 2U) * decoy_count,
				"needed-source lookup did not charge its actually visited identity "
				"prefix");
		limits.cancelled = {};
		limits.maximum_operations = measured.operations;
		limits.maximum_retained_bytes = measured.retained_bytes_bound;
		require(bool(q::project_exceptional_exits(long_prefix.input(), limits, {}, repeated)) &&
					repeated.operations == measured.operations &&
					repeated.retained_bytes_bound == measured.retained_bytes_bound,
				"prefix lookup rejected its exact deterministic work/storage bound");
		--limits.maximum_operations;
		auto failed = q::project_exceptional_exits(long_prefix.input(), limits, {}, repeated);
		require(!failed && failed.error().code == "sdk.exceptional-exit-budget" &&
					!repeated.operations && !repeated.retained_bytes_bound,
				"prefix lookup ignored one-under work or published failed usage");
		limits.maximum_operations = measured.operations;
		--limits.maximum_retained_bytes;
		failed = q::project_exceptional_exits(long_prefix.input(), limits, {}, repeated);
		require(!failed && failed.error().code == "sdk.exceptional-exit-budget" &&
					!repeated.operations && !repeated.retained_bytes_bound,
				"prefix lookup ignored one-under storage or published failed usage");
		require(visits > 2U, "long prefix fixture did not perform cancellable work");
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
		failed = q::project_exceptional_exits(
			long_prefix.input(), interrupted, stop.get_token(), repeated);
		require(!failed && failed.error().code == "sdk.exceptional-exit-cancelled" &&
					prefix == midpoint && !repeated.operations && !repeated.retained_bytes_bound,
				"long prefix work ignored an actual mid-projection stop");

		fixture worlds;
		const std::array world_ids{std::string(96U, 'v') + "a", std::string(96U, 'v') + "b"};
		for (auto& group : worlds.rows)
			for (auto& row : group)
			{
				row.presence.universe = std::string(96U, 'u');
				row.presence.fragments = {world_ids[0], world_ids[1]};
				row.interpretation = std::string(96U, 'i');
				for (auto& edge : row.contributor_edges)
				{
					edge.condition = row.presence;
					edge.interpretation = row.interpretation;
				}
			}
		const auto ordered = take(q::project_exceptional_exits(worlds.input()));
		for (auto& group : worlds.rows)
			std::ranges::reverse(group);
		const auto permuted = take(q::project_exceptional_exits(worlds.input()));
		require(ordered.populations.size() == 2U && permuted.populations.size() == 2U &&
					ordered.evidence.size() == permuted.evidence.size(),
				"long exact world keys erased or mixed independent populations");
		for (std::size_t i{}; i < world_ids.size(); ++i)
			require(ordered.populations[i].variant == world_ids[i] &&
						permuted.populations[i].variant == world_ids[i] &&
						ordered.populations[i].state == state::complete &&
						permuted.populations[i].state == state::complete &&
						ordered.populations[i].occurrence_ids ==
							permuted.populations[i].occurrence_ids,
					"identity ordering changed actual world closure or output order");
		for (std::size_t i{}; i < ordered.evidence.size(); ++i)
			require(ordered.evidence[i].row.canonical_form() ==
						permuted.evidence[i].row.canonical_form(),
					"long world ordering changed canonical original evidence");
	}
	void scope_world_prefix_controls()
	{
		const std::array base_world{std::string(96U, 'u') + "日本語:a",
									std::string(96U, 'v') + ":a",
									std::string(96U, 'i') + ":a"};
		for (std::size_t axis{}; axis < base_world.size(); ++axis)
		{
			auto other_world = base_world;
			other_world[axis].back() = 'b';
			const auto rebind = [](fixture& value, const auto& world)
			{
				for (auto& rows : value.rows)
					for (auto& row : rows)
					{
						row.presence = {world[0], {world[1]}};
						row.interpretation = world[2];
						for (auto& edge : row.contributor_edges)
						{
							edge.condition = row.presence;
							edge.interpretation = row.interpretation;
						}
					}
			};
			fixture original, foreign;
			rebind(original, base_world);
			rebind(foreign, other_world);
			for (std::size_t group{}; group < original.rows.size(); ++group)
				original.rows[group].insert(original.rows[group].end(),
											foreign.rows[group].begin(),
											foreign.rows[group].end());
			const auto input = original.queries();
			q::projection_resource_usage usage;
			const auto projected = take(q::project_exceptional_exits(input, {}, {}, usage));
			require(projected.populations.size() == 2U,
					"late world axis merged same-ID physical scopes");
			for (std::size_t i{}; i < projected.populations.size(); ++i)
			{
				const auto& scope = projected.populations[i];
				const auto& expected = i ? other_world : base_world;
				require(scope.universe == expected[0] && scope.variant == expected[1] &&
							scope.interpretation == expected[2] && scope.detail == "detail:a" &&
							scope.compile_unit == "unit:a" && scope.body == "body:a" &&
							scope.definition_source == "span:scope" &&
							scope.state == state::complete && scope.occurrence_count == 3U &&
							scope.variants.size() == 1U &&
							scope.variants.front().occurrences.size() == 2U,
						"matched ID skipped full world/body/source closure");
			}
			require(projected.source_queries && projected.source_queries->scans.size() == 8U,
					"world prefix comparison lost original query side channels");
			for (std::size_t i{}; i < input.scans.size(); ++i)
				require(projected.source_queries->scans[i].result.canonical_form() ==
							input.scans[i].result.canonical_form(),
						"world prefix comparison changed complete raw originals");
			set(original.rows[5].back(), "function", detached_cell::utf8("function:foreign"));
			const auto conflict = take(q::project_exceptional_exits(original.input()));
			const auto found = std::ranges::find_if(conflict.populations,
													[&](const auto& scope)
													{
														return scope.detail == "detail:a" &&
															scope.universe == other_world[0] &&
															scope.variant == other_world[1] &&
															scope.interpretation == other_world[2];
													});
			require(found != conflict.populations.end() && found->scope_state != state::complete,
					"late matched-world foreign body was treated as closed");
		}
	}
	void self_reference_agreement_controls()
	{
		for (unsigned control{}; control < 8U; ++control)
		{
			fixture original;
			auto& row = original.rows[7][1];
			const auto optional = row.values.at("output.original_expression_ordinal").type;
			if (control == 1U)
				set(row,
					"original_expression_ordinal",
					detached_cell::unsigned_integer(std::numeric_limits<std::uint64_t>::max()));
			if (control >= 2U && control <= 4U)
				set(row,
					"original_expression_ordinal",
					detached_cell::unknown(optional, "not-observed-日本語"));
			if (control == 5U)
				set(row, "does_not_return", detached_cell::boolean(true));
			if (control >= 6U)
				set(row, "target_usr", binary(std::string_view{"u\0\xff", 3U}));
			original.rows[7].push_back(row);
			auto& distinct = original.rows[7].back();
			if (control == 3U)
				set(distinct,
					"original_expression_ordinal",
					detached_cell::unknown(optional, "different-reason"));
			if (control == 4U)
				set(distinct, "original_expression_ordinal", detached_cell::absent(optional));
			if (control == 5U)
				set(distinct, "does_not_return", detached_cell::boolean(false));
			if (control == 7U)
				set(distinct, "target_usr", binary(std::string_view{"u\0\xfe", 3U}));
			const auto result = take(q::project_exceptional_exits(original.input()));
			const auto& scope = population(result);
			require(scope.enumeration_state == state::complete,
					"distinct duplicate changed original membership");
			const auto& occurrences = scope.variants[0U].occurrences;
			const auto found =
				std::ranges::find(occurrences, "exit:throw", &q::observed_exceptional_exit::exit);
			require(found != occurrences.end(), "duplicate occurrence disappeared");
			const auto expected =
				control >= 3U && control <= 5U ? state::conflicting : state::complete;
			require(found->state == expected,
					"distinct state/value/unknown-reason disagreement was skipped");
			if (control == 1U)
				require(found->original_expression_ordinal ==
							std::numeric_limits<std::uint64_t>::max(),
						"equal extreme optional ordinal was changed");
			if (control == 7U)
				require(found->target_state == state::conflicting,
						"distinct opaque byte disagreement was skipped");
			for (auto& rows : original.rows)
				std::ranges::reverse(rows);
			const auto permuted = take(q::project_exceptional_exits(original.input()));
			require(result.evidence.size() == permuted.evidence.size(),
					"agreement changed reordered original evidence count");
			for (std::size_t i{}; i < result.evidence.size(); ++i)
				require(result.evidence[i].row.canonical_form() ==
							permuted.evidence[i].row.canonical_form(),
						"agreement changed reordered full original metadata");
		}
		fixture measured;
		const auto optional =
			measured.rows[7][1].values.at("output.original_expression_ordinal").type;
		set(measured.rows[7][1],
			"original_expression_ordinal",
			detached_cell::unknown(optional, std::string(512U, 'r')));
		measured.rows[7].push_back(measured.rows[7][1]);
		std::size_t visits{};
		q::finite_population_limits cap;
		cap.cancelled = [&]
		{
			++visits;
			return false;
		};
		q::projection_resource_usage used, repeated;
		require(bool(q::project_exceptional_exits(measured.input(), cap, {}, used)),
				"equal unknown optional duplicate was not admitted");
		cap.cancelled = {};
		cap.maximum_operations = used.operations;
		cap.maximum_retained_bytes = used.retained_bytes_bound;
		require(bool(q::project_exceptional_exits(measured.input(), cap, {}, repeated)) &&
					repeated.operations == used.operations &&
					repeated.retained_bytes_bound == used.retained_bytes_bound,
				"agreement exact work/storage frontier changed");
		--cap.maximum_operations;
		auto failed = q::project_exceptional_exits(measured.input(), cap, {}, repeated);
		require(!failed && failed.error().code == "sdk.exceptional-exit-budget" &&
					!repeated.operations && !repeated.retained_bytes_bound,
				"agreement one-under work published a partial result");
		std::size_t prefix{};
		std::stop_source stopped;
		cap = {};
		cap.cancelled = [&]
		{
			if (++prefix == visits / 2U)
				stopped.request_stop();
			return false;
		};
		failed = q::project_exceptional_exits(measured.input(), cap, stopped.get_token(), repeated);
		require(!failed && failed.error().code == "sdk.exceptional-exit-cancelled" &&
					!repeated.operations && !repeated.retained_bytes_bound,
				"agreement actual traversal ignored cancellation");
		fixture malformed;
		malformed.rows[7][1].values.at("output.ordinal").state = static_cast<cell_state>(99);
		require(!q::project_exceptional_exits(malformed.input()),
				"self reference bypassed original cell validation");
	}
	void complete_projection_parity(const q::exceptional_exit_projection& left,
									const q::exceptional_exit_projection& right)
	{
		require(std::tie(left.compile_units_complete,
						 left.scope_inputs_complete,
						 left.occurrence_inputs_complete,
						 left.unresolved) ==
						std::tie(right.compile_units_complete,
								 right.scope_inputs_complete,
								 right.occurrence_inputs_complete,
								 right.unresolved) &&
					left.evidence.size() == right.evidence.size() &&
					left.populations.size() == right.populations.size(),
				"immutable ownership changes complete projection header");
		for (std::size_t i{}; i < left.evidence.size(); ++i)
			require(left.evidence[i].relation_id == right.evidence[i].relation_id &&
						left.evidence[i].original_row().canonical_form() ==
							right.evidence[i].original_row().canonical_form(),
					"immutable ownership changes full original annotations/cells/order");
		const auto scope_fields = [](const auto& v)
		{
			return std::tie(v.detail,
							v.function,
							v.compile_unit,
							v.body,
							v.definition_source,
							v.file,
							v.source_snapshot,
							v.universe,
							v.variant,
							v.interpretation,
							v.profile,
							v.lowering_profile,
							v.occurrence_count,
							v.occurrence_ids,
							v.enumeration_state,
							v.scope_state,
							v.state,
							v.evidence,
							v.gaps);
		};
		const auto occurrence_fields = [](const auto& v)
		{
			return std::tie(v.exit,
							v.variant,
							v.role,
							v.eligibility,
							v.profile,
							v.lowering_profile,
							v.observation_state,
							v.source_span,
							v.expression,
							v.target,
							v.target_usr,
							v.ordinal,
							v.original_expression_ordinal,
							v.block_ordinal,
							v.instruction_ordinal,
							v.successor_ordinal,
							v.intrinsic_id,
							v.compiler_route,
							v.emitter_methods,
							v.is_invoke,
							v.does_not_throw,
							v.does_not_return,
							v.source_state,
							v.expression_state,
							v.target_state,
							v.state,
							v.evidence,
							v.gaps);
		};
		for (std::size_t i{}; i < left.populations.size(); ++i)
		{
			const auto& a = left.populations[i];
			const auto& b = right.populations[i];
			require(scope_fields(a) == scope_fields(b) && a.variants.size() == b.variants.size(),
					"immutable ownership changes complete physical population");
			for (std::size_t j{}; j < a.variants.size(); ++j)
			{
				const auto& x = a.variants[j];
				const auto& y = b.variants[j];
				require(
					std::tie(x.variant, x.kind, x.symbol, x.index, x.state, x.evidence, x.gaps) ==
							std::tie(y.variant,
									 y.kind,
									 y.symbol,
									 y.index,
									 y.state,
									 y.evidence,
									 y.gaps) &&
						x.occurrences.size() == y.occurrences.size(),
					"immutable ownership changes complete lowering variant");
				for (std::size_t k{}; k < x.occurrences.size(); ++k)
					require(occurrence_fields(x.occurrences[k]) ==
								occurrence_fields(y.occurrences[k]),
							"immutable ownership changes complete exceptional occurrence");
			}
		}
		require(bool(left.source_queries) == bool(right.source_queries),
				"immutable ownership changes source query availability");
		if (left.source_queries)
		{
			require(left.source_queries->snapshot_id == right.source_queries->snapshot_id &&
						left.source_queries->scans.size() == right.source_queries->scans.size(),
					"immutable ownership changes source query domain");
			for (std::size_t i{}; i < left.source_queries->scans.size(); ++i)
			{
				const auto& x = left.source_queries->scans[i];
				const auto& y = right.source_queries->scans[i];
				require(x.relation_id == y.relation_id && x.logical_ir == y.logical_ir &&
							x.result.canonical_form() == y.result.canonical_form(),
						"immutable ownership changes complete source query side channels");
			}
		}
	}

	void colliding_identity_controls()
	{
		fixture original;
		const std::array old_ids{"span:scope",
								 "span:throw",
								 "span:body",
								 "syntax:throw",
								 "function:a",
								 "detail:a",
								 "body:a",
								 "exit:throw",
								 "exit:helper"};
		std::array<std::string, old_ids.size()> ids;
		for (std::size_t i{}; i < ids.size(); ++i)
			ids[i] = std::string(120U, static_cast<char>('a' + i)) + "same:end";
		for (auto& group : original.rows)
			for (auto& row : group)
				for (auto& [name, cell] : row.values)
				{
					(void)name;
					if (!cell.value)
						continue;
					if (auto* value = std::get_if<std::string>(&*cell.value))
						for (std::size_t i{}; i < ids.size(); ++i)
							if (*value == old_ids[i])
								*value = ids[i];
				}
		// Census byte sets must refer to the remapped complete exit identities.
		original.census();
		const auto raw = take(q::project_exceptional_exits(original.input()));
		require(population(raw).state == state::complete && raw.populations.size() == 1U,
				"equal-size/equal-suffix IDs merged independent source or exit membership");
		q::finite_population_limits shared;
		shared.evidence_ownership = q::projection_evidence_ownership::shared_immutable;
		q::projection_resource_usage used, repeated;
		const auto native = original.queries(true, true);
		const auto projected = take(q::project_exceptional_exits(native, shared, {}, used));
		complete_projection_parity(take(q::project_exceptional_exits(native)), projected);
		for (auto& group : original.rows)
			std::ranges::reverse(group);
		complete_projection_parity(raw, take(q::project_exceptional_exits(original.input())));
		shared.maximum_operations = used.operations;
		shared.maximum_retained_bytes = used.retained_bytes_bound;
		complete_projection_parity(
			projected, take(q::project_exceptional_exits(native, shared, {}, repeated)));
		--shared.maximum_operations;
		const auto failed = q::project_exceptional_exits(native, shared, {}, repeated);
		require(!failed && failed.error().code == "sdk.exceptional-exit-budget" &&
					!repeated.operations && !repeated.retained_bytes_bound,
				"colliding candidates ignored one-under work or exposed a partial projection");
	}

	void immutable_evidence_controls()
	{
		q::finite_population_limits shared_limits;
		shared_limits.evidence_ownership = q::projection_evidence_ownership::shared_immutable;
		for (bool sizes : {false, true})
		{
			fixture original;
			const auto input = original.queries(true, sizes);
			q::projection_resource_usage detached_usage, shared_usage;
			const auto detached = take(q::project_exceptional_exits(input, {}, {}, detached_usage));
			const auto shared =
				take(q::project_exceptional_exits(input, shared_limits, {}, shared_usage));
			complete_projection_parity(detached, shared);
			canonical_evidence_order(detached);
			for (const auto& evidence : shared.evidence)
			{
				bool found{};
				for (const auto& scan : input.scans)
					if (scan.relation_id == evidence.relation_id)
						for (const auto& row : q::query_transfer_access::borrow_rows(scan.result))
							found |= &row == &evidence.original_row();
				require(found && evidence.row.values.empty(),
						"opt-in evidence must share the exact admitted row without cloning");
			}
			for (const auto& evidence : detached.evidence)
				require(&evidence.original_row() == &evidence.row && !evidence.row.values.empty(),
						"default evidence keeps full mutable detached originals");
			require(shared_usage.operations < detached_usage.operations,
					"immutable ownership still performs full original payload copies");
			q::projection_resource_usage raw_shared_usage, raw_default_usage;
			const auto raw_shared = take(q::project_exceptional_exits(
				original.input(), shared_limits, {}, raw_shared_usage));
			const auto raw_default =
				take(q::project_exceptional_exits(original.input(), {}, {}, raw_default_usage));
			complete_projection_parity(raw_default, raw_shared);
			require(raw_shared_usage.operations == raw_default_usage.operations &&
						raw_shared_usage.retained_bytes_bound ==
							raw_default_usage.retained_bytes_bound,
					"raw-span opt-in must preserve actual detached copy charges");
			for (const auto& evidence : raw_shared.evidence)
				require(&evidence.original_row() == &evidence.row && !evidence.row.values.empty(),
						"raw-span opt-in must retain detached fallback");
			const auto original_bytes = raw_shared.evidence.front().row.canonical_form();
			original.rows[0][0].values.clear();
			require(raw_shared.evidence.front().row.canonical_form() == original_bytes,
					"raw-span fallback aliases caller mutations");

			auto exact = shared_limits;
			exact.maximum_operations = shared_usage.operations;
			exact.maximum_retained_bytes = shared_usage.retained_bytes_bound;
			exact.maximum_evidence_bytes = 0U;
			for (const auto& evidence : shared.evidence)
				exact.maximum_evidence_bytes += evidence.original_row().canonical_form().size();
			q::projection_resource_usage repeated;
			const auto bounded = take(q::project_exceptional_exits(input, exact, {}, repeated));
			complete_projection_parity(shared, bounded);
			require(repeated.operations == shared_usage.operations &&
						repeated.retained_bytes_bound == shared_usage.retained_bytes_bound,
					"immutable evidence exact measured work/storage/full-evidence boundary");
			--exact.maximum_operations;
			auto failed = q::project_exceptional_exits(input, exact, {}, repeated);
			require(!failed && failed.error().code == "sdk.exceptional-exit-budget" &&
						!repeated.operations && !repeated.retained_bytes_bound,
					"immutable evidence one-under work returns no partial owner");
			exact.maximum_operations = shared_usage.operations;
			--exact.maximum_retained_bytes;
			failed = q::project_exceptional_exits(input, exact, {}, repeated);
			require(!failed && failed.error().code == "sdk.exceptional-exit-budget" &&
						!repeated.operations && !repeated.retained_bytes_bound,
					"immutable evidence one-under storage returns no partial owner");
			exact.maximum_retained_bytes = shared_usage.retained_bytes_bound;
			--exact.maximum_evidence_bytes;
			failed = q::project_exceptional_exits(input, exact, {}, repeated);
			require(!failed && failed.error().field == "evidence-bytes" && !repeated.operations &&
						!repeated.retained_bytes_bound,
					"immutable ownership one-under full-evidence cap returns no partial owner");

			std::stop_source stopped;
			stopped.request_stop();
			failed =
				q::project_exceptional_exits(input, shared_limits, stopped.get_token(), repeated);
			require(!failed && failed.error().code == "sdk.exceptional-exit-cancelled" &&
						!repeated.operations && !repeated.retained_bytes_bound,
					"immutable ownership honors pre-stop");
			std::size_t visits{};
			auto counted = shared_limits;
			counted.cancelled = [&]
			{
				++visits;
				return false;
			};
			complete_projection_parity(
				shared, take(q::project_exceptional_exits(input, counted, {}, repeated)));
			require(visits > 2U, "immutable ownership has real checkpoints");
			std::stop_source mid_stop;
			std::size_t prefix{};
			counted.cancelled = [&]
			{
				if (++prefix == visits / 2U)
					mid_stop.request_stop();
				return false;
			};
			failed = q::project_exceptional_exits(input, counted, mid_stop.get_token(), repeated);
			require(!failed && failed.error().code == "sdk.exceptional-exit-cancelled" &&
						prefix == visits / 2U && !repeated.operations &&
						!repeated.retained_bytes_bound,
					"immutable ownership ignores charged real-prefix stop");
			complete_projection_parity(
				shared, take(q::project_exceptional_exits(input, shared_limits, {}, repeated)));
		}

		fixture lifetime_fixture;
		auto lifetime_input = lifetime_fixture.queries();
		std::weak_ptr<const q::query_result::data> weak =
			q::query_transfer_access::borrow_evidence_owner(lifetime_input.scans.front().result)
				.owner;
		auto held = take(q::project_exceptional_exits(lifetime_input, shared_limits));
		std::vector<std::string> originals;
		for (const auto& evidence : held.evidence)
			originals.push_back(evidence.original_row().canonical_form());
		lifetime_input = {};
		lifetime_fixture = {};
		held.source_queries.reset();
		require(!weak.expired(), "evidence loses immutable owner when source_queries resets");
		auto copied = held;
		auto moved = std::move(held);
		complete_projection_parity(copied, moved);
		auto isolated = copied.evidence;
		copied = {};
		moved = {};
		require(!weak.expired(), "standalone evidence copy loses original owner");
		for (std::size_t i{}; i < isolated.size(); ++i)
			require(isolated[i].original_row().canonical_form() == originals[i],
					"copy/move/owner death changes immutable original bytes");
		isolated.clear();
		require(weak.expired(), "shared evidence leaks query owner after final release");

		fixture duplicate;
		duplicate.rows[7].push_back(duplicate.rows[7][1]);
		for (bool contradictory : {false, true})
		{
			if (contradictory)
				set(duplicate.rows[7].back(), "eligibility", detached_cell::utf8("excluded"));
			const auto input = duplicate.queries();
			const auto detached = take(q::project_exceptional_exits(input));
			const auto shared = take(q::project_exceptional_exits(input, shared_limits));
			complete_projection_parity(detached, shared);
			require(contradictory ? population(shared).state != state::complete
								  : population(shared).state == state::complete,
					"immutable aliases change duplicate agreement/conflict");
		}
		for (std::size_t axis{}; axis < 3U; ++axis)
		{
			fixture foreign;
			auto& row = foreign.rows[7][1];
			if (axis == 0U)
				row.presence.universe = "foreign:universe";
			else if (axis == 1U)
				row.presence.fragments = {"foreign:variant"};
			else
				row.interpretation = "foreign:interpretation";
			for (auto& edge : row.contributor_edges)
			{
				edge.condition = row.presence;
				edge.interpretation = row.interpretation;
			}
			const auto input = foreign.queries();
			const auto shared = take(q::project_exceptional_exits(input, shared_limits));
			complete_projection_parity(take(q::project_exceptional_exits(input)), shared);
			require(std::ranges::any_of(shared.populations,
										[&](const auto& scope)
										{
											return scope.universe == row.presence.universe &&
												scope.variant == row.presence.fragments.front() &&
												scope.interpretation == row.interpretation;
										}) &&
						std::ranges::none_of(shared.populations,
											 [](const auto& scope)
											 {
												 return scope.state == state::complete;
											 }),
					"shared foreign World cannot borrow independent occurrence closure");
		}
		for (bool foreign_unit : {false, true})
		{
			fixture foreign_body;
			set(foreign_body.rows[5].front(),
				foreign_unit ? "compile_unit" : "function",
				detached_cell::utf8("foreign:owner"));
			const auto input = foreign_body.queries();
			const auto shared = take(q::project_exceptional_exits(input, shared_limits));
			complete_projection_parity(take(q::project_exceptional_exits(input)), shared);
			require(std::ranges::any_of(shared.populations,
										[&](const auto& scope)
										{
											return scope.detail.empty() && scope.body == "body:a" &&
												(foreign_unit ? scope.compile_unit
															  : scope.function) == "foreign:owner";
										}) &&
						std::ranges::none_of(shared.populations,
											 [](const auto& scope)
											 {
												 return scope.scope_state == state::complete;
											 }),
					"shared immutable body cannot borrow a foreign owner or unit");
		}
		fixture sidechannels;
		auto partial_input = sidechannels.queries();
		auto partial_data = std::make_shared<q::query_result::data>();
		partial_data->row_values = sidechannels.rows[7];
		partial_data->status = q::execution_status::complete;
		partial_data->input_complete = true;
		partial_data->snapshot = partial_input.snapshot_id;
		partial_data->conflict_values.push_back({std::string{names[7]},
												 "slot:original",
												 "clang22",
												 {"debug"},
												 {"claim:left", "claim:right"},
												 {"content:left", "content:right"}});
		partial_input.scans[7].result = q::query_transfer_access::make(std::move(partial_data));
		const auto partial_shared =
			take(q::project_exceptional_exits(partial_input, shared_limits));
		complete_projection_parity(take(q::project_exceptional_exits(partial_input)),
								   partial_shared);
		require(!partial_shared.occurrence_inputs_complete &&
					partial_shared.source_queries->scans[7].result.conflicts().size() == 1U,
				"shared evidence loses original query conflict sidechannels");
		fixture malformed;
		malformed.rows[2].back().values.erase("output.begin");
		q::projection_resource_usage refused{777U, 888U};
		require(!q::project_exceptional_exits(malformed.queries(), shared_limits, {}, refused) &&
					!refused.operations && !refused.retained_bytes_bound,
				"shared immutable evidence bypasses malformed original validation");
	}

} // namespace
int main(int argc, char** argv)
{
	immutable_evidence_controls();
	colliding_identity_controls();
	canonical_prefix_controls();
	canonical_prefix_locale_controls();
	if (argc == 2)
		transfer_prefix_shape_controls(argv[1]);
	scope_world_prefix_controls();
	self_reference_agreement_controls();
	typed_copy_failure_controls();
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
			return q::project_exceptional_exits(input, limits, {}, usage);
		},
		require);

	indexed_prefix_controls();
	fixture original;
	auto raw = take(q::project_exceptional_exits(original.input()));
	const auto& p = population(raw);
	require(p.state == state::complete && p.enumeration_state == state::complete &&
				p.scope_state == state::complete && p.occurrence_count == 3U,
			"complete physical scope and atomic full census");
	require(p.variants.size() == 1 && p.variants[0].occurrences.size() == 2 &&
				p.variants[0].symbol == std::string("f\0\xff", 3),
			"carrier not counted as qualifying occurrence; raw symbol lossless");
	require(p.variants[0].occurrences[0].role == "written_throw" &&
				p.variants[0].occurrences[0].source_state == state::complete &&
				p.variants[0].occurrences[0].expression_state == state::complete,
			"exact written throw bindings");
	require(p.variants[0].occurrences[1].emitter_methods == 8U &&
				p.variants[0].occurrences[1].state == state::complete &&
				p.variants[0].occurrences[1].source_state == state::unknown,
			"excluded helper absence remains independent");
	auto query = take(q::project_exceptional_exits(original.queries(false)));
	require(population(query).state == state::complete && query.source_queries &&
				query.source_queries->scans.size() == 8,
			"typed domain ignores unrelated broad partiality and retains query");
	fixture empty;
	empty.rows[7].resize(1);
	empty.census();
	auto known_empty = take(q::project_exceptional_exits(empty.input()));
	require(population(known_empty).state == state::complete &&
				population(known_empty).variants[0].occurrences.empty() &&
				population(known_empty).occurrence_count == 1U,
			"known empty lowering has original carrier");
	fixture no_carrier;
	no_carrier.rows[7].clear();
	no_carrier.census();
	require(population(take(q::project_exceptional_exits(no_carrier.input()))).state !=
				state::complete,
			"zero rows cannot invent completed lowering");
	fixture variants = empty;
	auto alternative = variants.exit("variant:b", "lowering_variant", 0);
	set(alternative, "variant_kind", detached_cell::utf8("constructor"));
	set(alternative, "variant_index", detached_cell::unsigned_integer(1));
	variants.rows[7].push_back(alternative);
	variants.census();
	require(population(take(q::project_exceptional_exits(variants.input()))).variants.size() == 2,
			"actual variants retained separately");
	fixture absent = original;
	absent.rows[7].pop_back();
	require(population(take(q::project_exceptional_exits(absent.input()))).enumeration_state ==
				state::conflicting,
			"missing original member rejects closure");
	fixture extra = original;
	extra.rows[7].push_back(extra.exit("exit:extra", "termination", 3));
	set(extra.rows[7].back(), "variant", detached_cell::utf8("variant:a"));
	require(population(take(q::project_exceptional_exits(extra.input()))).enumeration_state ==
				state::conflicting,
			"extra original member rejects closure");
	fixture foreign = original;
	foreign.rows[7][1].presence.fragments = {"foreign"};
	auto foreign_out = take(q::project_exceptional_exits(foreign.input()));
	require(std::ranges::none_of(foreign_out.populations,
								 [](const auto& scope)
								 {
									 return scope.state == state::complete;
								 }),
			"foreign world cannot satisfy original member");
	fixture duplicate = original;
	duplicate.rows[7].push_back(duplicate.rows[7][1]);
	require(population(take(q::project_exceptional_exits(duplicate.input()))).state ==
				state::complete,
			"equal duplicate preserves finite closure");
	set(duplicate.rows[7].back(), "eligibility", detached_cell::utf8("excluded"));
	auto contradictory = take(q::project_exceptional_exits(duplicate.input()));
	require(population(contradictory).enumeration_state == state::complete &&
				population(contradictory).state == state::conflicting,
			"classification conflict separate from membership");
	fixture source_conflict = original;
	source_conflict.rows[2].push_back(source_conflict.rows[2][1]);
	set(source_conflict.rows[2].back(), "end", detached_cell::unsigned_integer(30));
	auto source_out = take(q::project_exceptional_exits(source_conflict.input()));
	require(population(source_out).state == state::complete &&
				population(source_out).variants[0].occurrences[0].source_state ==
					state::conflicting,
			"source conflict cannot erase observed eligibility");
	fixture owner = original;
	set(owner.rows[6][0], "compile_unit", detached_cell::utf8("unit:foreign"));
	require(population(take(q::project_exceptional_exits(owner.input())))
					.variants[0]
					.occurrences[0]
					.expression_state == state::conflicting,
			"foreign original expression unit");
	fixture future = original;
	set(future.rows[7][1], "role", detached_cell::utf8("future_exit"));
	require(population(take(q::project_exceptional_exits(future.input()))).state != state::complete,
			"future role cannot close subset");
	fixture unknown = original;
	set(unknown.rows[7][1], "eligibility", detached_cell::utf8("unknown"));
	set(unknown.rows[7][1], "observation_state", detached_cell::utf8("partial"));
	require(population(take(q::project_exceptional_exits(unknown.input()))).enumeration_state ==
					state::complete &&
				population(take(q::project_exceptional_exits(unknown.input()))).state ==
					state::partial,
			"unknown eligibility independent complete census");
	fixture ordinal = original;
	set(ordinal.rows[7][2], "ordinal", detached_cell::unsigned_integer(1));
	require(population(take(q::project_exceptional_exits(ordinal.input()))).enumeration_state ==
				state::conflicting,
			"duplicate original ordinal");
	auto missing = original.queries();
	missing.scans.erase(missing.scans.begin() + 7);
	auto missing_result = take(q::project_exceptional_exits(missing));
	require(!missing_result.occurrence_inputs_complete &&
				population(missing_result).state != state::complete &&
				!missing_result.unresolved.empty(),
			"missing scan unavailable, not known zero");
	fixture optional = original;
	for (auto& d : optional.rows[4])
		for (auto it = d.values.begin(); it != d.values.end();)
			if (it->first.starts_with("output.exceptional_"))
				it = d.values.erase(it);
			else
				++it;
	for (auto& d : optional.rows[5])
		for (auto it = d.values.begin(); it != d.values.end();)
			if (it->first.starts_with("output.exceptional_"))
				it = d.values.erase(it);
			else
				++it;
	require(population(take(q::project_exceptional_exits(optional.input()))).enumeration_state !=
				state::complete,
			"old optional facets stay unobserved");
	fixture invalid = original;
	invalid.rows[2][0].values.erase("output.begin");
	auto bad = q::project_exceptional_exits(invalid.input());
	require(!bad, "missing required original cell rejected");
	q::projection_resource_usage usage{9, 9};
	auto success = q::project_exceptional_exits(original.queries(), {}, {}, usage);
	require(success && usage.operations > 0 && usage.retained_bytes_bound > 0,
			"measured usage success");
	const auto measured = usage;
	q::finite_population_limits exact;
	exact.maximum_operations = measured.operations;
	exact.maximum_retained_bytes = measured.retained_bytes_bound;
	auto bounded = q::project_exceptional_exits(original.queries(), exact, {}, usage);
	require(bounded && usage.operations == measured.operations &&
				usage.retained_bytes_bound == measured.retained_bytes_bound,
			"exact measured reservation deterministic");
	exact.maximum_operations = measured.operations - 1U;
	auto work_fail = q::project_exceptional_exits(original.queries(), exact, {}, usage);
	require(!work_fail && usage.operations == 0 && usage.retained_bytes_bound == 0,
			"work failure zeroes usage");
	q::finite_population_limits cap;
	cap.maximum_rows = 1;
	auto row_fail = q::project_exceptional_exits(original.input(), cap, {}, usage);
	require(!row_fail && usage.operations == 0, "row cap before ownership");
	std::stop_source stop;
	stop.request_stop();
	auto cancelled = q::project_exceptional_exits(original.input(), {}, stop.get_token(), usage);
	require(!cancelled && cancelled.error().code == "sdk.exceptional-exit-cancelled" &&
				usage.operations == 0,
			"stop cancellation zeroes usage");
	q::finite_population_limits callback;
	callback.cancelled = []
	{
		return true;
	};
	auto callback_fail = q::project_exceptional_exits(original.input(), callback, {}, usage);
	require(!callback_fail && callback_fail.error().code == "sdk.exceptional-exit-cancelled",
			"caller cancellation");
	auto reversed = original;
	for (auto& rows : reversed.rows)
		std::ranges::reverse(rows);
	auto permutation = take(q::project_exceptional_exits(reversed.input()));
	require(population(permutation).state == p.state &&
				permutation.evidence.size() == raw.evidence.size() &&
				population(permutation).variants[0].occurrences[0].evidence ==
					p.variants[0].occurrences[0].evidence,
			"permutation canonical original evidence");
	auto implicit_flags = original.input();
	implicit_flags.compile_units_complete = false;
	implicit_flags.scope_inputs_complete = false;
	implicit_flags.occurrence_inputs_complete = false;
	require(population(take(q::project_exceptional_exits(implicit_flags))).state != state::complete,
			"raw flags default unavailable");
	fixture target_bound = original;
	target_bound.rows[3].push_back(
		fact(3,
			 {{"entity", detached_cell::utf8("function:target")},
			  {"kind", detached_cell::utf8("function")},
			  {"provider_local_key", binary("opaque-original-framing")}}));
	set(target_bound.rows[7][1], "target", detached_cell::utf8("function:target"));
	set(target_bound.rows[7][1], "target_usr", binary(std::string_view{"u\0\xff", 3}));
	auto target_result = take(q::project_exceptional_exits(target_bound.input()));
	require(population(target_result).variants[0].occurrences[0].target_state == state::complete &&
				population(target_result).variants[0].occurrences[0].target_usr ==
					std::string("u\0\xff", 3),
			"opaque entity key is independent from raw target USR");
	fixture missing_ordinal = empty;
	auto ordinal_type = missing_ordinal.rows[7][0].values.at("output.ordinal").type;
	set(missing_ordinal.rows[7][0],
		"ordinal",
		detached_cell::unknown(ordinal_type, "not-observed"));
	require(population(take(q::project_exceptional_exits(missing_ordinal.input()))).state !=
				state::complete,
			"unknown required ordinal cannot become known ordinal zero");
	fixture future_profile = original;
	for (auto group : {4U, 5U})
		set(future_profile.rows[group][0],
			"exceptional_exit_profile",
			detached_cell::utf8("future-exception-profile/2"));
	require(
		population(take(q::project_exceptional_exits(future_profile.input()))).enumeration_state !=
			state::complete,
		"future census profile not recognized subset");
	fixture unknown_variant = empty;
	set(unknown_variant.rows[7][0], "variant_kind", detached_cell::utf8("future_variant"));
	require(population(take(q::project_exceptional_exits(unknown_variant.input()))).state !=
				state::complete,
			"future variant unavailable");
	auto sidechannels = original.queries();
	auto data = std::make_shared<q::query_result::data>();
	data->row_values = original.rows[7];
	data->status = q::execution_status::complete;
	data->input_complete = true;
	data->ordered = true;
	data->snapshot = sidechannels.snapshot_id;
	data->conflict_values.push_back({std::string{names[7]},
									 "original-slot",
									 "clang22",
									 {"debug"},
									 {"claim:left", "claim:right"},
									 {"content:left", "content:right"}});
	sidechannels.scans[7].result = q::query_transfer_access::make(std::move(data));
	auto conflict_scan = take(q::project_exceptional_exits(sidechannels));
	require(!conflict_scan.occurrence_inputs_complete &&
				population(conflict_scan).state != state::complete &&
				conflict_scan.source_queries->scans[7].result.conflicts().size() == 1,
			"original query conflict sidechannel preserved");
	q::finite_population_limits evidence_cap;
	evidence_cap.maximum_evidence_bytes = 1;
	auto evidence_failure = q::project_exceptional_exits(original.input(), evidence_cap, {}, usage);
	require(!evidence_failure && usage.operations == 0, "evidence cap before output ownership");
	q::finite_population_limits zero_pop;
	zero_pop.maximum_populations = 1;
	fixture two_scopes = original;
	auto second = two_scopes.rows[4][0];
	set(second, "detail", detached_cell::utf8("detail:b"));
	two_scopes.rows[4].push_back(second);
	auto population_failure = q::project_exceptional_exits(two_scopes.input(), zero_pop, {}, usage);
	require(!population_failure && population_failure.error().field == "populations" &&
				usage.operations == 0,
			"population quota independently bounded");
	q::projection_resource_usage raw_usage;
	require(q::project_exceptional_exits(original.input(), {}, {}, raw_usage).has_value() &&
				raw_usage.operations > 0 && raw_usage.retained_bytes_bound > 0,
			"raw measured usage success");
	fixture missing_body = original;
	missing_body.rows[5].clear();
	auto missing_body_result = take(q::project_exceptional_exits(missing_body.input()));
	require(population(missing_body_result).body == "body:a" &&
				population(missing_body_result).scope_state != state::complete,
			"original body FK retained but missing body cannot close binding");
	fixture foreign_body = original;
	set(foreign_body.rows[5][0], "function", detached_cell::utf8("function:foreign"));
	auto foreign_body_result = take(q::project_exceptional_exits(foreign_body.input()));
	require(std::ranges::none_of(foreign_body_result.populations,
								 [](const auto& scope)
								 {
									 return scope.scope_state == state::complete;
								 }),
			"actual foreign body owner cannot satisfy scope");
	require(p.body == "body:a" && p.definition_source == "span:scope" &&
				p.scope_state == state::complete,
			"distinct exact declaration and lexical body sources joined by "
			"actual body FK");

	fixture decoys = original;
	std::string large(512U * 1024U, 'x');
	auto unused_span = decoys.rows[2][0];
	set(unused_span, "span", detached_cell::utf8("span:unrelated"));
	set(unused_span, "role", detached_cell::utf8(large));
	decoys.rows[2].push_back(unused_span);
	auto unused_syntax = decoys.rows[6][0];
	set(unused_syntax, "node", detached_cell::utf8("syntax:unrelated"));
	set(unused_syntax, "source", detached_cell::utf8("span:unrelated"));
	set(unused_syntax, "kind", detached_cell::utf8(large));
	decoys.rows[6].push_back(unused_syntax);
	q::finite_population_limits selective;
	selective.maximum_retained_bytes = raw_usage.retained_bytes_bound * 2U;
	selective.maximum_evidence_bytes = selective.maximum_retained_bytes;
	q::projection_resource_usage decoy_usage;
	auto decoy_result = q::project_exceptional_exits(decoys.input(), selective, {}, decoy_usage);
	require(decoy_result && population(*decoy_result).state == state::complete &&
				decoy_result->evidence.size() == raw.evidence.size(),
			"unreferenced large syntax/source not copied under actual evidence cap");
	decoys.rows[2].back().values.erase("output.begin");
	require(!q::project_exceptional_exits(decoys.input(), selective, {}, decoy_usage) &&
				decoy_usage.operations == 0,
			"malformed unreferenced source still validates and resets usage");
	fixture unknown_body = original;
	set(unknown_body.rows[5][0],
		"compile_unit",
		detached_cell::unknown(unknown_body.rows[5][0].values.at("output.compile_unit").type,
							   "unobserved-original-unit"));
	auto unknown_body_result = take(q::project_exceptional_exits(unknown_body.input()));
	require(std::ranges::none_of(unknown_body_result.populations,
								 [](const auto& scope)
								 {
									 return scope.scope_state == state::complete;
								 }),
			"unobserved original body unit is a frontier");

	std::cout << "exceptional exits original projection: PASS\n";
}
