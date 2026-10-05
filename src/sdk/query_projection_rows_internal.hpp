#pragma once

#include <cxxlens/sdk/query.hpp>

namespace cxxlens::sdk::query::detail
{
	/** Validate original projected cells without inventing optional observations.
	 * A compatible saved scan can predate optional descriptor additions. */
	template <class Checkpoint>
	result<void> validate_projected_relation_row(const annotated_row& row,
												 const relation_descriptor& descriptor,
												 std::string_view error_code,
												 Checkpoint checkpoint)
	{
		if (auto valid = row.validate(); !valid)
			return valid.error();
		std::size_t recognized{};
		for (const auto& column : descriptor.columns)
		{
			checkpoint();
			const auto value = row.values.find("output." + column.name);
			if (value == row.values.end())
			{
				if (!column.type.optional)
					return error{std::string{error_code}, column.id, "required-column-missing"};
				continue;
			}
			++recognized;
			if (value->second.type != column.type || !value->second.validate())
				return error{std::string{error_code}, column.id, "column-type-or-value-invalid"};
		}
		if (recognized != row.values.size())
			return error{std::string{error_code}, descriptor.id, "foreign-column"};
		return {};
	}
} // namespace cxxlens::sdk::query::detail
