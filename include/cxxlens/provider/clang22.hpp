#pragma once

/** @file clang22.hpp @brief Explicit Clang-22-native provider helpers. */

#include <concepts>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

#include <cxxlens/interop/clang.hpp>
#include <cxxlens/sdk/provider.hpp>

namespace clang
{
	class SourceRange;
} // namespace clang

namespace cxxlens::provider::clang22
{
	/** @brief Callback-scoped native translation unit; never store or move across threads. */
	using borrowed_translation_unit = interop::borrowed_clang_tu;
	/** @brief Move-only synchronous native extractor callback. */
	using translation_unit_callback = interop::clang_tu_callback;

	/** @brief Run one fresh Clang 22 job with the existing process/lifetime boundary. */
	[[nodiscard]] inline result<void> with_translation_unit(workspace& workspace,
															compile_unit_id unit,
															translation_unit_callback callback,
															execution_context context = {})
	{
		return interop::with_translation_unit(
			workspace, std::move(unit), std::move(callback), std::move(context));
	}

	/** @brief Normalize a native source range to a detached, half-open source value. */
	[[nodiscard]] result<source_span> normalize_source(borrowed_translation_unit& unit,
													   const clang::SourceRange& range);

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
