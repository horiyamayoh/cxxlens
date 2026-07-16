#pragma once

/** @file clang22.hpp @brief Explicit Clang-22-native provider helpers. */

#include <concepts>
#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <cxxlens/sdk/provider.hpp>

namespace clang
{
	class ASTContext;
	class SourceManager;
	class SourceRange;
} // namespace clang

namespace cxxlens::provider::clang22
{
	namespace detail
	{
		struct native_access;
	} // namespace detail

	/** @brief One in-memory Clang translation-unit request with explicit arguments. */
	struct translation_unit_input
	{
		std::string logical_path;
		std::string source;
		std::vector<std::string> arguments;
		[[nodiscard]] sdk::result<void> validate() const;
	};

	/** @brief Callback-scoped native translation unit; never store or move across threads. */
	class borrowed_translation_unit
	{
	  public:
		borrowed_translation_unit(const borrowed_translation_unit&) = delete;
		borrowed_translation_unit& operator=(const borrowed_translation_unit&) = delete;
		borrowed_translation_unit(borrowed_translation_unit&&) = delete;
		borrowed_translation_unit& operator=(borrowed_translation_unit&&) = delete;

		/** @brief Borrow the Clang AST only for the active callback. */
		[[nodiscard]] clang::ASTContext& ast() const noexcept;
		/** @brief Borrow the Clang source manager only for the active callback. */
		[[nodiscard]] clang::SourceManager& source_manager() const noexcept;

	  private:
		borrowed_translation_unit(clang::ASTContext& ast, clang::SourceManager& source_manager);
		clang::ASTContext* ast_{};
		clang::SourceManager* source_manager_{};
		friend struct detail::native_access;
	};

	/** @brief Move-only synchronous native extractor callback. */
	using translation_unit_callback =
		std::move_only_function<sdk::result<void>(borrowed_translation_unit&)>;

	/** @brief Run one fresh in-memory Clang 22 job and detach all output before return. */
	[[nodiscard]] sdk::result<void> with_translation_unit(const translation_unit_input& input,
														  translation_unit_callback callback);

	/** @brief Detached half-open source location independent from Clang object lifetime. */
	struct detached_source_span
	{
		std::string logical_path;
		std::uint64_t begin{};
		std::uint64_t end{};
		bool read_only{};
		std::string id;
		[[nodiscard]] sdk::result<void> validate() const;
	};

	/** @brief Normalize a native source range to a detached half-open source value. */
	[[nodiscard]] sdk::result<detached_source_span>
	normalize_source(borrowed_translation_unit& unit, const clang::SourceRange& range);

	/** @brief Validate that a detached row contains no native pointer/address marker. */
	[[nodiscard]] sdk::result<void> detect_native_escape(const sdk::detached_row& row);

	/** @brief Pointer types are deliberately not detachable through the native SDK. */
	template <class T>
	concept detachable_scalar = !std::is_pointer_v<std::remove_reference_t<T>> &&
		!std::is_member_pointer_v<std::remove_reference_t<T>>;

	template <detachable_scalar T>
	[[nodiscard]] sdk::result<sdk::detached_cell> detach_scalar(T&& value)
	{
		using value_type = std::remove_cvref_t<T>;
		if constexpr (std::same_as<value_type, bool>)
			return sdk::detached_cell::boolean(value);
		else if constexpr (std::signed_integral<value_type>)
			return sdk::detached_cell::signed_integer(static_cast<std::int64_t>(value));
		else if constexpr (std::unsigned_integral<value_type>)
			return sdk::detached_cell::unsigned_integer(static_cast<std::uint64_t>(value));
		else if constexpr (std::same_as<value_type, std::string>)
			return sdk::detached_cell::utf8(std::forward<T>(value));
		else
			return cxxlens::sdk::unexpected(
				sdk::error{"native.unsupported-detachment", "value", {}});
	}
} // namespace cxxlens::provider::clang22
