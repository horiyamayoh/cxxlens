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

namespace cxxlens::detail::clang22::materialization
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
		std::size_t max_input_bytes{16U * 1024U * 1024U};
		std::size_t max_depth{64U};
		std::size_t max_array_elements{16U * 1024U};
		std::size_t max_object_members{16U * 1024U};
		std::size_t max_string_bytes{8U * 1024U * 1024U};
		std::size_t max_total_string_bytes{16U * 1024U * 1024U};
		std::size_t max_total_values{256U * 1024U};
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
		[[nodiscard]] static sdk::result<json_value> string(std::string value);
		[[nodiscard]] static json_value array(array_type value);
		[[nodiscard]] static sdk::result<json_value> object(object_type value);

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

		friend sdk::result<json_document> parse_json_object(std::string raw_bytes,
															const json_limits& limits);
	};

	/**
	 * Parse exactly one BOM-free strict UTF-8 top-level JSON object.
	 * The private `materialization.json-invalid` failure must be mapped by the request/report
	 * phase.
	 */
	[[nodiscard]] sdk::result<json_document> parse_json_object(std::string raw_bytes,
															   const json_limits& limits = {});

	/** Python-checker-compatible compact JSON (sorted object keys, no final newline). */
	[[nodiscard]] std::string canonical_json(const json_value& value);
	/** Response transport form: canonical_json(value) followed by exactly one LF. */
	[[nodiscard]] std::string canonical_json_line(const json_value& value);
} // namespace cxxlens::detail::clang22::materialization
