#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include <cxxlens/relations/cc_entity_detail.hpp>
#include <cxxlens/sdk.hpp>
#include <cxxlens/sdk/abi_surfaces.hpp>
#include <cxxlens/sdk/record_surfaces.hpp>

namespace
{
	using namespace cxxlens::sdk;
	using relation = cxxlens::cc::relations::entity_detail;
	namespace q = cxxlens::sdk::query;
	void require(bool value, const char* message)
	{
		if (!value)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}
	void legacy_projection_tests(const char* path)
	{
		// This independent scan is unchanged output from the earlier exact adapter.
		relation_registry registry;
		for (const auto& descriptor : standard_relation_descriptors())
			require(registry.add(descriptor).has_value(), "legacy registry failed");
		auto engine = registry.build("legacy-field-projection-test");
		require(engine.has_value(), "legacy engine failed");
		std::ifstream file{path};
		require(file.good(), "legacy original scan fixture missing");
		const std::string text{std::istreambuf_iterator<char>{file}, {}};
		auto decoded = q::decode_application_queries(*engine, text);
		require(decoded && decoded->scans.size() == 1U, "genuine legacy scan rejected");
		std::vector<q::annotated_row> rows;
		auto cursor = decoded->scans.front().result.rows();
		for (;;)
		{
			auto next = cursor.next();
			require(next.has_value(), "legacy original row cursor failed");
			if (!*next)
				break;
			auto row = (*next)->copy();
			require(row.has_value(), "legacy original row copy failed");
			require(!row->values.contains("output.field_is_bitfield"),
					"fixture contains a newer bit-field classification");
			rows.push_back(std::move(*row));
		}
		require(rows.size() == 18U, "genuine legacy row population changed");
		q::abi_surface_input abi;
		abi.details = rows;
		q::record_surface_input records;
		records.details = rows;
		const auto projected_abi = q::project_abi_surfaces(abi);
		const auto projected_records = q::project_record_surfaces(records);
		require(projected_abi && projected_records &&
					projected_abi->evidence.size() == rows.size() &&
					projected_records->evidence.size() == rows.size(),
				"ABI/record projection rejected genuine older original detail rows");
		for (const auto& evidence : projected_abi->evidence)
			require(!evidence.row.values.contains("output.field_is_bitfield"),
					"ABI projection interpreted a missing old observation as false");
		for (const auto& evidence : projected_records->evidence)
			require(!evidence.row.values.contains("output.field_is_bitfield"),
					"record projection interpreted a missing old observation as false");
		auto missing = rows;
		missing.front().values.erase("output.detail");
		abi.details = missing;
		records.details = missing;
		require(!q::project_abi_surfaces(abi) && !q::project_record_surfaces(records),
				"legacy compatibility relaxed a required original column");
		auto malformed = rows;
		malformed.front().values.emplace("output.field_is_bitfield", detached_cell::utf8("false"));
		abi.details = malformed;
		records.details = malformed;
		require(!q::project_abi_surfaces(abi) && !q::project_record_surfaces(records),
				"legacy compatibility accepted a present wrong field type");
		abi.details = rows;
		records.details = rows;
		std::stop_source stopped;
		stopped.request_stop();
		require(!q::project_abi_surfaces(abi, {}, stopped.get_token()) &&
					!q::project_record_surfaces(records, {}, stopped.get_token()),
				"legacy compatibility bypassed cancellation");
		q::abi_surface_limits abi_limits;
		abi_limits.maximum_rows = 1U;
		q::record_surface_limits record_limits;
		record_limits.maximum_rows = 1U;
		require(!q::project_abi_surfaces(abi, abi_limits) &&
					!q::project_record_surfaces(records, record_limits),
				"legacy compatibility bypassed original row bounds");
	}
	void put(detached_row& row, std::string name, detached_cell value)
	{
		const auto column = relation::descriptor().column("cc.entity_detail.v1." + name);
		require(column.has_value(), "original field column missing");
		value.type = column->type;
		row.cells.insert_or_assign(column->id, std::move(value));
	}
	detached_row original_detail()
	{
		detached_row row;
		row.descriptor_id = relation::descriptor().id;
		put(row, "detail", detached_cell::typed("entity_detail_id", "fixture:detail"));
		put(row, "compile_unit", detached_cell::typed("compile_unit_id", "fixture:unit"));
		put(row, "entity", detached_cell::typed("cc_entity_id", "fixture:field"));
		put(row, "source", detached_cell::typed("source_span_id", "fixture:source"));
		put(row, "signature", detached_cell::utf8("unused"));
		put(row, "access", detached_cell::utf8("public"));
		put(row, "linkage", detached_cell::utf8("external"));
		put(row, "is_definition", detached_cell::boolean(true));
		put(row, "flags", detached_cell::bytes({}));
		put(row, "parameter_count", detached_cell::unsigned_integer(0U));
		return row;
	}
} // namespace
int main(int argc, char** argv)
{
	require(argc == 2, "legacy fixture argument missing");
	legacy_projection_tests(argv[1]);
	const auto& descriptor = relation::descriptor();
	require(descriptor.validate().has_value(), "field descriptor invalid");
	auto legacy = original_detail();
	require(validate_row(descriptor, legacy).has_value(), "legacy detail rejected");
	const auto absent = relation::view{legacy}.get<relation::field_is_bitfield>();
	require(absent && absent->state == cell_state::absent && absent->type.optional,
			"missing old field classification became false");
	const auto identity = derive_domain_identity(descriptor, legacy);
	require(identity.has_value(), "original detail identity unavailable");
	auto plain = legacy;
	put(plain, "field_bitfield_profile", detached_cell::utf8("clang22-original-field-bitfield/1"));
	put(plain, "field_bitfield_state", detached_cell::utf8("complete"));
	put(plain, "field_is_bitfield", detached_cell::boolean(false));
	require(validate_row(descriptor, plain).has_value(), "actual non-bitfield rejected");
	const auto false_value = relation::view{plain}.get<relation::field_is_bitfield>();
	require(false_value && false_value->value && !std::get<bool>(*false_value->value),
			"original false lost");
	const auto no_width = relation::view{plain}.get<relation::field_bit_width>();
	require(no_width && no_width->state == cell_state::absent, "non-bitfield width invented");
	auto zero = plain;
	put(zero, "field_is_bitfield", detached_cell::boolean(true));
	put(zero, "field_bit_width", detached_cell::unsigned_integer(0U));
	require(validate_row(descriptor, zero).has_value(), "actual zero-width field rejected");
	const auto width = relation::view{zero}.get<relation::field_bit_width>();
	require(width && width->value && std::get<std::uint64_t>(*width->value) == 0U,
			"actual zero conflated with absence");
	auto dependent = plain;
	put(dependent, "field_is_bitfield", detached_cell::boolean(true));
	put(dependent, "field_bitfield_state", detached_cell::utf8("partial"));
	put(dependent,
		"field_bitfield_reason",
		detached_cell::utf8("original-bitfield-width-requires-specialization"));
	require(validate_row(descriptor, dependent).has_value(),
			"dependent width without a value rejected");
	const auto unknown_width = relation::view{dependent}.get<relation::field_bit_width>();
	require(unknown_width && unknown_width->state == cell_state::absent,
			"dependent width became zero");
	for (const auto* row : {&plain, &zero, &dependent})
	{
		const auto observed_identity = derive_domain_identity(descriptor, *row);
		require(observed_identity && *observed_identity == *identity,
				"optional primitive altered physical identity");
	}
	auto malformed = zero;
	malformed.cells.at("cc.entity_detail.v1.field_is_bitfield") = detached_cell::utf8("true");
	require(!validate_row(descriptor, malformed), "mistyped classification accepted");
	malformed = zero;
	malformed.cells.at("cc.entity_detail.v1.field_bit_width").value = std::string{"0"};
	require(!validate_row(descriptor, malformed), "mistyped original width accepted");
	malformed = zero;
	malformed.cells.emplace("foreign.field_bit_width", detached_cell::unsigned_integer(0U));
	require(!validate_row(descriptor, malformed), "foreign bit-field column accepted");
}
