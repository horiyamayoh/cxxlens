#include <cstdlib>
#include <iostream>
#include <string>

#include <cxxlens/relations/cc_entity_detail.hpp>

namespace
{
	using namespace cxxlens::sdk;
	using relation = cxxlens::cc::relations::entity_detail;
	void require(bool value, const char* message)
	{
		if (!value)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
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
int main()
{
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
