#include "unicode_nfc.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

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
	} // namespace

	sdk::result<bool> is_nfc_utf8(const std::string_view value)
	{
		if (value.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
			return sdk::unexpected(unicode_error("input-too-large"));

		const auto input_size = static_cast<std::int32_t>(value.size());
		UErrorCode status = U_ZERO_ERROR;
		std::int32_t utf16_size{};
		u_strFromUTF8(nullptr, 0, &utf16_size, value.data(), input_size, &status);
		if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status))
			return sdk::unexpected(unicode_error("invalid-utf8"));

		status = U_ZERO_ERROR;
		std::vector<UChar> utf16(static_cast<std::size_t>(utf16_size));
		std::int32_t converted_size{};
		u_strFromUTF8(utf16.data(), utf16_size, &converted_size, value.data(), input_size, &status);
		if (U_FAILURE(status) || converted_size != utf16_size)
			return sdk::unexpected(unicode_error("utf8-conversion"));

		status = U_ZERO_ERROR;
		const UNormalizer2* normalizer = unorm2_getNFCInstance(&status);
		if (U_FAILURE(status) || normalizer == nullptr)
			return sdk::unexpected(unicode_error("nfc-instance"));
		const UBool normalized =
			unorm2_isNormalized(normalizer, utf16.data(), converted_size, &status);
		if (U_FAILURE(status))
			return sdk::unexpected(unicode_error("nfc-check"));
		return normalized != 0;
	}
} // namespace cxxlens::detail::clang22
