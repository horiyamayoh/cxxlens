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

	/** @brief Full SHA-256 digest with a caller-supplied domain separator. */
	[[nodiscard]] std::string semantic_digest(std::string_view domain, std::string_view bytes);
	/** @brief Full SHA-256 content digest without a semantic domain prefix. */
	[[nodiscard]] std::string content_digest(std::span<const std::byte> bytes);
} // namespace cxxlens::sdk
