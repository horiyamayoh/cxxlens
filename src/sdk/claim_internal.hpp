#pragma once

#include <string>
#include <vector>

#include <cxxlens/sdk/claim.hpp>

namespace cxxlens::sdk::detail
{
	[[nodiscard]] std::vector<std::byte> claim_occurrence_projection(const claim& value);
	[[nodiscard]] bool claim_occurrence_less(const claim& left, const claim& right);
	[[nodiscard]] bool same_claim_occurrence(const claim& left, const claim& right);
	[[nodiscard]] result<std::string>
	functional_payload_digest(const relation_descriptor& descriptor, const detached_row& row);
	void canonicalize_claim_conflicts(std::vector<claim_conflict>& values);
	[[nodiscard]] std::string canonical_claim_conflict_json(const claim_conflict& value);
} // namespace cxxlens::sdk::detail
