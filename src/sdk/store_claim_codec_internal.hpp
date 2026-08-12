#pragma once

#include <span>
#include <vector>

#include <cxxlens/sdk/claim.hpp>

namespace cxxlens::sdk::detail
{
	/**
	 * Encode one validated claim with the Store's private, length-delimited claim codec.
	 *
	 * This header is source-private. It is shared by the bounded materialization ingress and the
	 * Store implementation only; it is not part of the public SDK surface or the claim-batch
	 * commit path.
	 */
	[[nodiscard]] result<std::vector<std::byte>> encode_store_claim(const claim& value);

	/** Decode one complete private claim record and independently validate its identities. */
	[[nodiscard]] result<claim> decode_store_claim(std::span<const std::byte> bytes,
												   const relation_engine& engine);
} // namespace cxxlens::sdk::detail
