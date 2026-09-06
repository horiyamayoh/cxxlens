#pragma once

#include <string_view>

#include <cxxlens/sdk/provider.hpp>

namespace cxxlens::sdk::detail
{
	/** Decode one bounded canonical provider-manifest v1 self-claim. */
	[[nodiscard]] result<provider::manifest>
	decode_provider_manifest(std::string_view canonical_bytes);
} // namespace cxxlens::sdk::detail
