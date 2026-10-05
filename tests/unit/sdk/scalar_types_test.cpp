#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

#include <cxxlens/relations/cc_type.hpp>

namespace
{
	using namespace cxxlens::sdk;
	using type_relation = cxxlens::cc::relations::type;

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
		const auto column = type_relation::descriptor().column("cc.type.v1." + name);
		require(column.has_value(), "original scalar column missing");
		value.type = column->type;
		row.cells.insert_or_assign(column->id, std::move(value));
	}

	detached_row original_type()
	{
		detached_row row;
		row.descriptor_id = type_relation::descriptor().id;
		put(row, "type", detached_cell::typed("cc_type_id", "fixture:type"));
		put(row, "constructor", detached_cell::utf8("builtin"));
		put(row,
			"component_signature_digest",
			detached_cell::utf8("sha256:" + std::string(64U, 'a')));
		put(row, "qualifiers", detached_cell::bytes({}));
		put(row, "dependent", detached_cell::boolean(false));
		return row;
	}
} // namespace

int main()
{
	const auto& descriptor = type_relation::descriptor();
	require(descriptor.validate().has_value(), "scalar descriptor invalid");
	auto legacy = original_type();
	require(validate_row(descriptor, legacy).has_value(), "legacy sparse original type rejected");
	auto missing = type_relation::view{legacy}.get<type_relation::builtin_kind>();
	require(missing && missing->state == cell_state::absent && missing->type.optional,
			"missing legacy builtin facet was invented");
	const auto identity = derive_domain_identity(descriptor, legacy);
	require(identity.has_value(), "original type identity unavailable");

	auto boolean = legacy;
	put(boolean, "builtin_kind", detached_cell::utf8("Bool"));
	put(boolean, "builtin_profile", detached_cell::utf8("clang22-original-builtin-type/1"));
	put(boolean, "builtin_state", detached_cell::utf8("complete"));
	put(boolean, "integer_bit_width", detached_cell::unsigned_integer(8U));
	put(boolean, "integer_signed", detached_cell::boolean(false));
	put(boolean,
		"integer_profile",
		detached_cell::utf8("clang22-original-integer-representation/1"));
	put(boolean, "integer_state", detached_cell::utf8("complete"));
	require(validate_row(descriptor, boolean).has_value(), "actual scalar payload rejected");
	const auto observed = type_relation::view{boolean}.get<type_relation::builtin_kind>();
	require(observed && observed->value && std::get<std::string>(*observed->value) == "Bool",
			"original builtin discriminator lost");
	const auto observed_identity = derive_domain_identity(descriptor, boolean);
	require(observed_identity && *observed_identity == *identity,
			"optional scalar facet changed structural type identity");

	auto malformed = boolean;
	malformed.cells.at("cc.type.v1.integer_signed") = detached_cell::utf8("false");
	require(!validate_row(descriptor, malformed), "foreign scalar type was accepted");
	malformed = boolean;
	malformed.cells.at("cc.type.v1.integer_bit_width").value = std::string{"8"};
	require(!validate_row(descriptor, malformed), "invalid original integer value was accepted");
	malformed = boolean;
	malformed.cells.emplace("foreign.builtin_kind", detached_cell::utf8("Bool"));
	require(!validate_row(descriptor, malformed), "foreign original scalar column was accepted");
	malformed = boolean;
	malformed.cells.erase("cc.type.v1.constructor");
	require(!validate_row(descriptor, malformed), "required original type field was ignored");

	auto future = legacy;
	put(future, "builtin_kind", detached_cell::utf8("FutureCompilerBuiltin"));
	put(future, "builtin_profile", detached_cell::utf8("future-builtin/2"));
	put(future, "builtin_state", detached_cell::utf8("unsupported"));
	require(validate_row(descriptor, future).has_value(), "open original symbols were lost");
	for (const auto* name : {"builtin_kind",
							 "builtin_profile",
							 "builtin_state",
							 "integer_bit_width",
							 "integer_signed",
							 "integer_profile",
							 "integer_state",
							 "integer_underlying_type"})
	{
		const auto column = descriptor.column("cc.type.v1." + std::string{name});
		require(column && !column->required && column->type.optional,
				"scalar facets must remain optional");
		require(std::ranges::find(descriptor.conflict_columns, column->id) !=
					descriptor.conflict_columns.end(),
				"contradictory scalar payload lost conflict membership");
	}
	std::cout << "original scalar type wire contract PASS\n";
}
