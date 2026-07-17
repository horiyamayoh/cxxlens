#pragma once

#include <string>
#include <vector>

#include <cxxlens/sdk/claim.hpp>

namespace cxxlens::sdk::detail
{
	[[nodiscard]] result<std::string>
	functional_payload_digest(const relation_descriptor& descriptor, const detached_row& row);
	void canonicalize_claim_conflicts(std::vector<claim_conflict>& values);
	[[nodiscard]] std::string canonical_claim_conflict_json(const claim_conflict& value);
} // namespace cxxlens::sdk::detail
