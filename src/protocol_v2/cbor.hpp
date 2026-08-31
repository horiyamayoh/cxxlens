#pragma once

/**
 * @file cbor.hpp
 * @brief Small, bounded RFC 8949 deterministic-CBOR value codec for protocol 2.
 *
 * This file deliberately has no dependency on the provider runtime.  It is a
 * closed subset: unsigned and negative integers, byte strings, UTF-8 text,
 * arrays, maps with text keys, booleans, and null.  Tags, floating point,
 * indefinite-length items, and duplicate or non-canonically ordered map keys
 * are rejected.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <cxxlens/sdk/common.hpp>

namespace cxxlens::protocol_v2::cbor
{
	using byte = std::byte;
	using bytes = std::vector<byte>;

	struct value;
	using array = std::vector<value>;
	using map = std::vector<std::pair<std::string, value>>;

	/** @brief Recursive closed-CBOR value used by typed protocol controls. */
	struct value
	{
		using storage = std::variant<std::monostate,
									 bool,
									 std::uint64_t,
									 std::int64_t,
									 bytes,
									 std::string,
									 array,
									 map>;

		storage data;

		value() = default;
		value(std::nullptr_t) : data{std::monostate{}} {}
		value(const bool item) : data{item} {}
		value(const unsigned int item) : data{static_cast<std::uint64_t>(item)} {}
		value(const int item)
			: data{item < 0 ? storage{static_cast<std::int64_t>(item)}
							: storage{static_cast<std::uint64_t>(item)}}
		{
		}
		value(const std::uint64_t item) : data{item} {}
		value(const std::int64_t item) : data{item} {}
		value(bytes item) : data{std::move(item)} {}
		value(std::string item) : data{std::move(item)} {}
		value(const char* item) : data{std::string{item}} {}
		value(array item) : data{std::move(item)} {}
		value(map item) : data{std::move(item)} {}

		[[nodiscard]] bool operator==(const value&) const = default;
	};

	/** @brief Allocation and nesting limits applied before decoder growth. */
	struct limits
	{
		std::size_t max_bytes{65'536U};
		std::size_t max_depth{32U};
		std::size_t max_items{4'096U};
		std::size_t max_text_bytes{4'096U};
		std::size_t max_byte_string_bytes{1'048'576U};
	};

	/** Allocation-free canonical-CBOR scanner bounds. */
	struct scan_limits
	{
		std::size_t max_bytes{65'536U};
		std::size_t max_depth{32U};
		std::size_t max_items{4'096U};
		std::size_t max_array_items{4'096U};
		std::size_t max_map_items{4'096U};
		std::size_t max_text_bytes{4'096U};
		std::size_t max_byte_string_bytes{1'048'576U};
		bool require_root_map{};
	};

	/** Stable, allocation-free scanner outcome. */
	enum class scan_error : std::uint8_t
	{
		none,
		empty_or_limit,
		truncated,
		non_shortest,
		indefinite_or_reserved,
		depth_limit,
		item_limit,
		array_shape_limit,
		map_shape_limit,
		text_limit_or_utf8,
		byte_string_limit,
		map_key_not_text,
		map_order_or_duplicate,
		unsupported,
		trailing_bytes,
	};

	struct scan_result
	{
		scan_error error{scan_error::none};
		std::size_t item_count{};
		std::size_t maximum_depth{};

		[[nodiscard]] explicit operator bool() const noexcept
		{
			return error == scan_error::none;
		}
	};

	inline constexpr std::size_t maximum_scan_depth = 32U;

	/** Fixed scanner state; canonical scanning never recurses or allocates. */
	struct scan_stack_entry
	{
		std::uint64_t remaining{};
		std::size_t item_begin{};
		std::size_t previous_key_offset{};
		std::size_t previous_key_size{};
		std::uint8_t container_kind{};
		bool expecting_key{};
	};

	struct scan_workspace
	{
		std::array<scan_stack_entry, maximum_scan_depth + 1U> stack{};
		std::size_t stack_size{};
		std::size_t item_count{};
		std::size_t maximum_depth{};
	};

	inline constexpr std::size_t canonical_scan_workspace_bytes = sizeof(scan_workspace);

	/** @brief Encode exactly one canonical closed-subset CBOR value. */
	[[nodiscard]] sdk::result<bytes> encode(const value& item, limits bound = {});

	/** @brief Decode exactly one canonical closed-subset CBOR value. */
	[[nodiscard]] sdk::result<value> decode(std::span<const byte> input, limits bound = {});

	/**
	 * Validate one canonical closed-subset CBOR value without allocating a DOM,
	 * canonical output, or key scratch storage.
	 */
	[[nodiscard]] scan_result scan_canonical(std::span<const byte> input,
											 scan_limits bound = {}) noexcept;

	/** @brief Return a text-keyed map field without silently accepting duplicates. */
	[[nodiscard]] const value* find(const map& fields, std::string_view key) noexcept;

	/** @brief Require a map field with the requested scalar type. */
	template <typename T>
	[[nodiscard]] sdk::result<T> field(const map& fields, const std::string_view key)
	{
		const auto* item = find(fields, key);
		if (item == nullptr)
			return sdk::unexpected(
				sdk::error{"protocol-v2.cbor-invalid", std::string{key}, "missing-field"});
		if (const auto* typed = std::get_if<T>(&item->data); typed != nullptr)
			return *typed;
		return sdk::unexpected(
			sdk::error{"protocol-v2.cbor-invalid", std::string{key}, "field-type"});
	}

	/** @brief Require an exact map shape; unknown and duplicate keys are rejected. */
	[[nodiscard]] sdk::result<void> require_keys(const map& fields,
												 std::initializer_list<std::string_view> keys);

	/** @brief Validate a strict UTF-8 scalar sequence without normalizing bytes. */
	[[nodiscard]] bool valid_utf8(std::string_view text) noexcept;

	/** @brief Validate that a value is a canonical closed-subset value before encoding. */
	[[nodiscard]] sdk::result<void> validate(const value& item, limits bound = {});
} // namespace cxxlens::protocol_v2::cbor
