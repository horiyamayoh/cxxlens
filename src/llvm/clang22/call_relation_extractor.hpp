#pragma once

#include <vector>

#include <cxxlens/core/failure.hpp>

#include "../common/frontend_port.hpp"

namespace clang
{
	class ASTContext;
} // namespace clang

namespace cxxlens::detail::clang22
{
	class semantic_identity_adapter;

	[[nodiscard]] result<std::vector<facts::observation_record>>
	extract_call_relation_observations(clang::ASTContext& context,
									   semantic_identity_adapter& identities);
} // namespace cxxlens::detail::clang22
