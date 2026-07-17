#pragma once

/** @file common.hpp @brief LLVM-free value and failure primitives for the author SDK. */

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace cxxlens::sdk
{
	/** @brief Stable machine-readable SDK failure. */
	struct error
	{
		std::string code;
		std::string field;
		std::string detail;
		[[nodiscard]] bool operator==(const error&) const = default;
	};

	/** @brief Value-or-structured-error result used at every SDK boundary. */
	template <class T>
	class result
	{
	  public:
		result(T value) : storage_{std::move(value)} {}
		result(sdk::error failure) : storage_{std::move(failure)} {}
		[[nodiscard]] bool has_value() const noexcept
		{
			return std::holds_alternative<T>(storage_);
		}
		[[nodiscard]] explicit operator bool() const noexcept
		{
			return has_value();
		}
		[[nodiscard]] T& value()
		{
			return std::get<T>(storage_);
		}
		[[nodiscard]] const T& value() const
		{
			return std::get<T>(storage_);
		}
		[[nodiscard]] T& operator*()
		{
			return value();
		}
		[[nodiscard]] const T& operator*() const
		{
			return value();
		}
		[[nodiscard]] T* operator->()
		{
			return &value();
		}
		[[nodiscard]] const T* operator->() const
		{
			return &value();
		}
		[[nodiscard]] sdk::error& error()
		{
			return std::get<sdk::error>(storage_);
		}
		[[nodiscard]] const sdk::error& error() const
		{
			return std::get<sdk::error>(storage_);
		}

	  private:
		std::variant<T, sdk::error> storage_;
	};

	/** @brief Value-less SDK result specialization. */
	template <>
	class result<void>
	{
	  public:
		result() = default;
		result(sdk::error failure) : storage_{std::move(failure)} {}
		[[nodiscard]] bool has_value() const noexcept
		{
			return std::holds_alternative<std::monostate>(storage_);
		}
		[[nodiscard]] explicit operator bool() const noexcept
		{
			return has_value();
		}
		[[nodiscard]] sdk::error& error()
		{
			return std::get<sdk::error>(storage_);
		}
		[[nodiscard]] const sdk::error& error() const
		{
			return std::get<sdk::error>(storage_);
		}

	  private:
		std::variant<std::monostate, sdk::error> storage_;
	};

	/** @brief Explicit failure helper mirroring expected-style call sites. */
	[[nodiscard]] inline error unexpected(error failure)
	{
		return failure;
	}

	/** @brief Semantic version value with exact major/minor/patch components. */
	struct semantic_version
	{
		std::uint32_t major{};
		std::uint32_t minor{};
		std::uint32_t patch{};
		[[nodiscard]] auto operator<=>(const semantic_version&) const = default;
		[[nodiscard]] std::string string() const;
	};

	/** @brief One value in the versioned cxxlens canonical tuple algebra. */
	struct canonical_value
	{
		/** @brief Closed wire kind; values are encoded with an explicit one-byte tag. */
		enum class kind : std::uint8_t
		{
			null_value,
			boolean,
			signed_integer,
			bytes,
			utf8_string,
			ordered_tuple,
		};

		kind type{kind::null_value};
		bool boolean{};
		std::int64_t integer{};
		std::vector<std::byte> byte_string;
		std::string text;
		std::vector<canonical_value> tuple;

		[[nodiscard]] static canonical_value null();
		[[nodiscard]] static canonical_value from_boolean(bool value);
		[[nodiscard]] static canonical_value from_integer(std::int64_t value);
		[[nodiscard]] static canonical_value from_bytes(std::vector<std::byte> value);
		[[nodiscard]] static canonical_value from_string(std::string value);
		[[nodiscard]] static canonical_value from_tuple(std::vector<canonical_value> value);
		/** @brief Reject invalid kinds, inactive payload, invalid UTF-8, and invalid children. */
		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] bool operator==(const canonical_value&) const = default;
	};
	/** @brief Return whether a canonical tuple kind is a defined closed-enum member. */
	[[nodiscard]] constexpr bool is_valid(const canonical_value::kind value) noexcept
	{
		return value >= canonical_value::kind::null_value &&
			value <= canonical_value::kind::ordered_tuple;
	}

	/** @brief Encode one recursively validated value using cxxlens-canonical-tuple-v1. */
	[[nodiscard]] result<std::vector<std::byte>> canonical_binary(const canonical_value& value);
	/** @brief Strictly decode one complete, canonical cxxlens-canonical-tuple-v1 value. */
	[[nodiscard]] result<canonical_value> canonical_binary_decode(std::span<const std::byte> bytes);
	/** @brief Full typed SHA-256 identity using the exact cxxlens domain prefix. */
	[[nodiscard]] result<std::string>
	canonical_identity_digest(std::string_view identity_kind,
							  std::span<const canonical_value> fields);
	/** @brief Derive the accepted source.span snapshot/file/range/role domain identity. */
	[[nodiscard]] result<std::string> source_span_identity(std::string_view source_snapshot,
														   std::string_view file,
														   std::uint64_t begin,
														   std::uint64_t end,
														   std::string_view role);

	/**
	 * @brief Full v2 semantic SHA-256 digest over a typed, length-prefixed domain/payload tuple.
	 * @return `sdk.semantic-domain-invalid` when domain is not a non-empty lowercase identifier.
	 */
	[[nodiscard]] result<std::string> semantic_digest(std::string_view domain,
													  std::string_view bytes);
	/** @brief Full SHA-256 content digest without a semantic domain prefix. */
	[[nodiscard]] std::string content_digest(std::span<const std::byte> bytes);
} // namespace cxxlens::sdk
