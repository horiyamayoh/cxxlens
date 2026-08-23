#include "unicode_nfc.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <unicode/stringoptions.h>
#include <unicode/unorm2.h>
#include <unicode/ustring.h>
#include <unicode/utypes.h>

namespace cxxlens::detail::clang22
{
	namespace
	{
		[[nodiscard]] sdk::error unicode_error(std::string detail)
		{
			return {
				"materialization.unicode-normalization-failed", "logical_path", std::move(detail)};
		}

		[[nodiscard]] sdk::result<std::vector<UChar>> to_utf16(const std::string_view value)
		{
			if (value.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
				return sdk::unexpected(unicode_error("input-too-large"));
			const auto input_size = static_cast<std::int32_t>(value.size());
			UErrorCode status = U_ZERO_ERROR;
			std::int32_t utf16_size{};
			// ICU receives the explicit byte count; this view is intentionally not required
			// to be NUL-terminated.
			// NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
			u_strFromUTF8(nullptr, 0, &utf16_size, value.data(), input_size, &status);
			if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status))
				return sdk::unexpected(unicode_error("invalid-utf8"));

			status = U_ZERO_ERROR;
			std::vector<UChar> utf16(static_cast<std::size_t>(utf16_size));
			std::int32_t converted_size{};
			// ICU receives the explicit byte count; this view is intentionally not required
			// to be NUL-terminated.
			// NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
			u_strFromUTF8(
				// NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
				utf16.data(),
				utf16_size,
				&converted_size,
				// NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
				value.data(),
				input_size,
				&status);
			if (U_FAILURE(status) || converted_size != utf16_size)
				return sdk::unexpected(unicode_error("utf8-conversion"));
			return utf16;
		}

		[[nodiscard]] sdk::result<std::vector<UChar>> normalize_nfc(const std::vector<UChar>& utf16)
		{
			if (utf16.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
				return sdk::unexpected(unicode_error("input-too-large"));
			UErrorCode status = U_ZERO_ERROR;
			const UNormalizer2* normalizer = unorm2_getNFCInstance(&status);
			if (U_FAILURE(status) || normalizer == nullptr)
				return sdk::unexpected(unicode_error("nfc-instance"));
			const auto input_size = static_cast<std::int32_t>(utf16.size());
			status = U_ZERO_ERROR;
			const auto normalized_size =
				unorm2_normalize(normalizer, utf16.data(), input_size, nullptr, 0, &status);
			if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status))
				return sdk::unexpected(unicode_error("nfc-size"));
			status = U_ZERO_ERROR;
			std::vector<UChar> normalized(static_cast<std::size_t>(normalized_size));
			const auto written = unorm2_normalize(
				normalizer, utf16.data(), input_size, normalized.data(), normalized_size, &status);
			if (U_FAILURE(status) || written != normalized_size)
				return sdk::unexpected(unicode_error("nfc-normalize"));
			return normalized;
		}

		[[nodiscard]] sdk::result<std::string> to_utf8(const std::vector<UChar>& utf16)
		{
			if (utf16.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
				return sdk::unexpected(unicode_error("input-too-large"));
			const auto input_size = static_cast<std::int32_t>(utf16.size());
			UErrorCode status = U_ZERO_ERROR;
			std::int32_t utf8_size{};
			u_strToUTF8(nullptr, 0, &utf8_size, utf16.data(), input_size, &status);
			if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status))
				return sdk::unexpected(unicode_error("utf8-size"));
			status = U_ZERO_ERROR;
			std::string output(static_cast<std::size_t>(utf8_size), '\0');
			std::int32_t converted_size{};
			u_strToUTF8(
				output.data(), utf8_size, &converted_size, utf16.data(), input_size, &status);
			if (U_FAILURE(status) || converted_size != utf8_size)
				return sdk::unexpected(unicode_error("utf16-conversion"));
			return output;
		}
	} // namespace

	sdk::result<bool> is_nfc_utf8(const std::string_view value)
	{
		auto utf16 = to_utf16(value);
		if (!utf16)
			return sdk::unexpected(std::move(utf16.error()));
		UErrorCode status = U_ZERO_ERROR;
		const UNormalizer2* normalizer = unorm2_getNFCInstance(&status);
		if (U_FAILURE(status) || normalizer == nullptr)
			return sdk::unexpected(unicode_error("nfc-instance"));
		const UBool normalized = unorm2_isNormalized(
			normalizer, utf16->data(), static_cast<std::int32_t>(utf16->size()), &status);
		if (U_FAILURE(status))
			return sdk::unexpected(unicode_error("nfc-check"));
		return normalized != 0;
	}

	sdk::result<std::string> nfc_casefold_utf8(const std::string_view value)
	{
		auto utf16 = to_utf16(value);
		if (!utf16)
			return sdk::unexpected(std::move(utf16.error()));
		if (utf16->size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
			return sdk::unexpected(unicode_error("input-too-large"));
		const auto input_size = static_cast<std::int32_t>(utf16->size());
		UErrorCode status = U_ZERO_ERROR;
		const auto folded_size =
			u_strFoldCase(nullptr, 0, utf16->data(), input_size, U_FOLD_CASE_DEFAULT, &status);
		if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status))
			return sdk::unexpected(unicode_error("casefold-size"));
		status = U_ZERO_ERROR;
		std::vector<UChar> folded(static_cast<std::size_t>(folded_size));
		const auto written = u_strFoldCase(
			folded.data(), folded_size, utf16->data(), input_size, U_FOLD_CASE_DEFAULT, &status);
		if (U_FAILURE(status) || written != folded_size)
			return sdk::unexpected(unicode_error("casefold"));
		auto normalized = normalize_nfc(folded);
		if (!normalized)
			return sdk::unexpected(std::move(normalized.error()));
		return to_utf8(*normalized);
	}
} // namespace cxxlens::detail::clang22
