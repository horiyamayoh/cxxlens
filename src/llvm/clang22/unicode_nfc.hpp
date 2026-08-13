#pragma once

#include <string_view>

#include <cxxlens/sdk/common.hpp>

namespace cxxlens::detail::clang22
{
	/** Return whether one strict UTF-8 byte sequence is already Unicode NFC. */
	[[nodiscard]] sdk::result<bool> is_nfc_utf8(std::string_view value);
} // namespace cxxlens::detail::clang22
