#include "materialization_identity.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <ranges>
#include <utility>

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		[[nodiscard]] sdk::error identity_error(std::string field, std::string detail = {})
		{
			return {"materialization.identity-mismatch", std::move(field), std::move(detail)};
		}
	} // namespace

	sdk::result<sdk::canonical_value> canonical_projection_value(const json_value& value)
	{
		using sdk::canonical_value;
		switch (value.type())
		{
			case json_value::kind::null_value:
				return canonical_value::null();
			case json_value::kind::boolean:
				return canonical_value::from_boolean(*value.as_boolean());
			case json_value::kind::signed_integer:
				return canonical_value::from_integer(*value.as_signed_integer());
			case json_value::kind::unsigned_integer:
			{
				const auto integer = *value.as_unsigned_integer();
				if (integer > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
					return sdk::unexpected(identity_error("integer", "signed-int64-domain"));
				return canonical_value::from_integer(static_cast<std::int64_t>(integer));
			}
			case json_value::kind::string:
			{
				auto text = sdk::canonical_utf8_string(*value.as_string());
				if (!text)
					return sdk::unexpected(std::move(text.error()));
				return std::move(*text);
			}
			case json_value::kind::array:
			{
				std::vector<canonical_value> items;
				items.reserve(value.as_array()->size());
				for (const auto& item : *value.as_array())
				{
					auto projected = canonical_projection_value(item);
					if (!projected)
						return sdk::unexpected(std::move(projected.error()));
					items.push_back(std::move(*projected));
				}
				return canonical_value::from_tuple(std::move(items));
			}
			case json_value::kind::object:
			{
				std::vector<canonical_value> entries;
				entries.reserve(value.as_object()->size());
				for (const auto& [key, item] : *value.as_object())
				{
					auto canonical_key = sdk::canonical_utf8_string(key);
					auto canonical_item = canonical_projection_value(item);
					if (!canonical_key)
						return sdk::unexpected(std::move(canonical_key.error()));
					if (!canonical_item)
						return sdk::unexpected(std::move(canonical_item.error()));
					entries.push_back(canonical_value::from_tuple(
						{std::move(*canonical_key), std::move(*canonical_item)}));
				}
				return canonical_value::from_tuple(std::move(entries));
			}
		}
		return sdk::unexpected(identity_error("json", "unknown-kind"));
	}

	sdk::result<std::vector<std::byte>> canonical_projection_bytes(const json_value& value)
	{
		auto projected = canonical_projection_value(value);
		if (!projected)
			return sdk::unexpected(std::move(projected.error()));
		return sdk::canonical_binary(*projected);
	}

	sdk::result<std::string> projection_digest(const std::string_view domain,
											   const json_value& value)
	{
		auto bytes = canonical_projection_bytes(value);
		if (!bytes)
			return sdk::unexpected(std::move(bytes.error()));
		return sdk::semantic_digest(
			domain, std::string_view{reinterpret_cast<const char*>(bytes->data()), bytes->size()});
	}

	sdk::result<std::string> projection_identity(const std::string_view kind,
												 const std::span<const json_value> fields)
	{
		std::vector<sdk::canonical_value> projected;
		projected.reserve(fields.size());
		for (const auto& field : fields)
		{
			auto value = canonical_projection_value(field);
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			projected.push_back(std::move(*value));
		}
		return sdk::canonical_identity_digest(kind, projected);
	}

	sdk::result<json_value> json_string(std::string value)
	{
		return json_value::string(std::move(value));
	}

	sdk::result<json_value> json_object(json_value::object_type value)
	{
		return json_value::object(std::move(value));
	}

	sdk::result<json_value> object_without(const json_value& value,
										   const std::span<const std::string_view> removed)
	{
		const auto* object = value.as_object();
		if (object == nullptr)
			return sdk::unexpected(identity_error("json", "object-required"));
		auto output = *object;
		for (const auto member : removed)
			output.erase(std::string{member});
		return json_object(std::move(output));
	}
} // namespace cxxlens::detail::clang22::materialization
