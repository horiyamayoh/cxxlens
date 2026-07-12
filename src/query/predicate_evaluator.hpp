#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include <cxxlens/facts.hpp>

#include "../select/selector_ast.hpp"

namespace cxxlens::detail::query
{
	enum class predicate_outcome : std::uint8_t
	{
		matched,
		rejected,
		unresolved
	};

	struct predicate_evaluation
	{
		predicate_outcome outcome{predicate_outcome::unresolved};
		std::string reason_code;
	};

	using symbol_index = std::map<std::string, symbol>;

	[[nodiscard]] predicate_evaluation
	evaluate_symbol_predicate(const select::detail::node_ptr& node,
							  const std::optional<symbol_id>& id,
							  const symbol_index& symbols);
} // namespace cxxlens::detail::query
