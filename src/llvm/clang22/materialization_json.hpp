#pragma once

#include <string>
#include <utility>

#include "sdk/bounded_json_internal.hpp"

namespace cxxlens::detail::clang22::materialization
{
	using utf8_byte_less = sdk::detail::utf8_byte_less;
	using json_limits = sdk::detail::json_limits;
	using json_value = sdk::detail::json_value;
	using json_document = sdk::detail::json_document;

	/** Preserve the accepted Clang v2.2 JSON boundary over the shared parser authority. */
	[[nodiscard]] inline sdk::result<json_document>
	parse_json_object(std::string raw_bytes, const json_limits& limits = {})
	{
		const sdk::detail::json_parse_contract contract{
			.error_code = "materialization.json-invalid",
			.error_field = "input",
			.include_byte_offset = true,
			.require_top_level_object = true,
			.reject_utf8_bom = true,
			.numbers = sdk::detail::json_number_syntax::exact_integer_values,
			.depth = sdk::detail::json_depth_semantics::containers,
		};
		return sdk::detail::parse_json_document(std::move(raw_bytes), limits, contract);
	}

	using sdk::detail::canonical_json;
	using sdk::detail::canonical_json_line;
} // namespace cxxlens::detail::clang22::materialization
