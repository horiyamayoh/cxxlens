#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <cxxlens/sdk/common.hpp>

namespace cxxlens::sdk::detail
{
	/** Canonical Unicode-scalar order for validated UTF-8 strings. */
	struct utf8_byte_less
	{
		using is_transparent = void;

		[[nodiscard]] bool operator()(std::string_view left, std::string_view right) const noexcept;
	};

	/** Bounded lexical/DOM limits. Callers select limits for the authenticated phase. */
	struct json_limits
	{
		std::size_t max_input_bytes{static_cast<std::size_t>(16U * 1024U * 1024U)};
		std::size_t max_depth{64U};
		std::size_t max_array_elements{static_cast<std::size_t>(16U * 1024U)};
		std::size_t max_object_members{static_cast<std::size_t>(16U * 1024U)};
		std::size_t max_string_bytes{static_cast<std::size_t>(8U * 1024U * 1024U)};
		std::size_t max_total_string_bytes{static_cast<std::size_t>(16U * 1024U * 1024U)};
		std::size_t max_total_values{static_cast<std::size_t>(256U * 1024U)};
	};

	enum class json_number_syntax : std::uint8_t
	{
		integer_tokens,
		exact_integer_values,
	};

	enum class json_depth_semantics : std::uint8_t
	{
		all_values,
		containers,
	};

	/** Caller-owned policy for one external JSON boundary. */
	struct json_parse_contract
	{
		std::string_view error_code{"sdk.json-invalid"};
		std::string_view error_field{"input"};
		bool include_byte_offset{true};
		bool require_top_level_object{true};
		bool reject_utf8_bom{true};
		json_number_syntax numbers{json_number_syntax::exact_integer_values};
		json_depth_semantics depth{json_depth_semantics::containers};
	};

	/** Immutable, value-owned JSON algebra. Floating-point values are intentionally absent. */
	class json_value
	{
	  public:
		enum class kind : std::uint8_t
		{
			null_value,
			boolean,
			signed_integer,
			unsigned_integer,
			string,
			array,
			object,
		};

		using array_type = std::vector<json_value>;
		using object_type = std::map<std::string, json_value, utf8_byte_less>;

		[[nodiscard]] static json_value null();
		[[nodiscard]] static json_value boolean(bool value);
		[[nodiscard]] static json_value signed_integer(std::int64_t value);
		[[nodiscard]] static json_value unsigned_integer(std::uint64_t value);
		[[nodiscard]] static result<json_value> string(std::string value);
		[[nodiscard]] static json_value array(array_type value);
		[[nodiscard]] static result<json_value> object(object_type value);

		[[nodiscard]] kind type() const noexcept;
		[[nodiscard]] bool is_null() const noexcept;
		[[nodiscard]] const bool* as_boolean() const noexcept;
		[[nodiscard]] const std::int64_t* as_signed_integer() const noexcept;
		[[nodiscard]] const std::uint64_t* as_unsigned_integer() const noexcept;
		[[nodiscard]] const std::string* as_string() const noexcept;
		[[nodiscard]] const array_type* as_array() const noexcept;
		[[nodiscard]] const object_type* as_object() const noexcept;

		/** Return an exact decoded member name, or null for a non-object/missing member. */
		[[nodiscard]] const json_value* member(std::string_view name) const noexcept;
		/** Match one exact, duplicate-free decoded member-name set. */
		[[nodiscard]] bool
		has_exact_members(std::span<const std::string_view> names) const noexcept;

		[[nodiscard]] bool operator==(const json_value&) const = default;

	  private:
		using storage_type = std::variant<std::monostate,
										  bool,
										  std::int64_t,
										  std::uint64_t,
										  std::string,
										  array_type,
										  object_type>;

		explicit json_value(storage_type value);
		storage_type value_;
	};

	/** Accepted transport document retaining raw occurrence bytes independently of its DOM. */
	class json_document
	{
	  public:
		[[nodiscard]] const std::string& raw_bytes() const noexcept;
		[[nodiscard]] const json_value& root() const noexcept;

	  private:
		json_document(std::string raw_bytes, json_value root);

		std::string raw_bytes_;
		json_value root_;

		friend result<json_document> parse_json_document(std::string raw_bytes,
														 const json_limits& limits,
														 const json_parse_contract& contract);
	};

	/** Parse exactly one bounded, BOM-free strict UTF-8 JSON value. */
	[[nodiscard]] result<json_value> parse_json_value(std::string_view raw_bytes,
													  const json_limits& limits = {},
													  const json_parse_contract& contract = {});

	/** Parse one value while retaining its raw occurrence bytes. */
	[[nodiscard]] result<json_document>
	parse_json_document(std::string raw_bytes,
						const json_limits& limits = {},
						const json_parse_contract& contract = {});

	/** Python-checker-compatible compact JSON (sorted object keys, no final newline). */
	[[nodiscard]] std::string canonical_json(const json_value& value);
	/** Response transport form: canonical_json(value) followed by exactly one LF. */
	[[nodiscard]] std::string canonical_json_line(const json_value& value);
} // namespace cxxlens::sdk::detail
