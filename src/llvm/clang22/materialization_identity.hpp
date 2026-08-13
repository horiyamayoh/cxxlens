#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "materialization_json.hpp"

namespace cxxlens::detail::clang22::materialization
{
	/** Encode one JSON value with the machine contract's sorted-key canonical tuple projection. */
	[[nodiscard]] sdk::result<sdk::canonical_value>
	canonical_projection_value(const json_value& value);

	/** Return the exact cxxlens-canonical-tuple-v1 bytes for one JSON projection. */
	[[nodiscard]] sdk::result<std::vector<std::byte>>
	canonical_projection_bytes(const json_value& value);

	/** Hash one JSON projection with semantic-digest-v2 in the supplied domain. */
	[[nodiscard]] sdk::result<std::string> projection_digest(std::string_view domain,
															 const json_value& value);

	/** Derive a canonical v1 strong identity from already typed JSON fields. */
	[[nodiscard]] sdk::result<std::string> projection_identity(std::string_view kind,
															   std::span<const json_value> fields);

	/** Construct a validated JSON string value without losing SDK error context. */
	[[nodiscard]] sdk::result<json_value> json_string(std::string value);

	/** Construct a duplicate-free, UTF-8-valid JSON object. */
	[[nodiscard]] sdk::result<json_value> json_object(json_value::object_type value);

	/** Copy an object while removing an exact set of members. */
	[[nodiscard]] sdk::result<json_value> object_without(const json_value& value,
														 std::span<const std::string_view> removed);
} // namespace cxxlens::detail::clang22::materialization
