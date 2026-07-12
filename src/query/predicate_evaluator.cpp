#include "predicate_evaluator.hpp"

#include <array>
#include <cstddef>
#include <ranges>
#include <string_view>
#include <utility>

namespace cxxlens::detail::query
{
	namespace
	{
		using select::detail::node_operation;
		using select::detail::node_ptr;

		[[nodiscard]] std::string argument(const node_ptr& node, const std::string_view key)
		{
			const auto found = std::ranges::find(
				node->arguments, key, &std::pair<std::string, std::string>::first);
			return found == node->arguments.end() ? std::string{} : found->second;
		}

		[[nodiscard]] bool contains_csv(const std::string_view csv, const std::string_view expected)
		{
			std::size_t begin{};
			while (begin < csv.size())
			{
				const auto end = csv.find(',', begin);
				if (csv.substr(begin, end - begin) == expected)
					return true;
				if (end == std::string_view::npos)
					break;
				begin = end + 1U;
			}
			return false;
		}

		[[nodiscard]] std::string_view symbol_kind_name(const symbol_kind value)
		{
			constexpr std::array names{
				"namespace",	 "record",		"class",	  "struct",	  "union",	 "function",
				"method",		 "constructor", "destructor", "variable", "field",	 "enum_type",
				"enum_constant", "typedef",		"type_alias", "template", "concept", "macro",
				"module",		 "parameter",	"unknown"};
			return names.at(static_cast<std::size_t>(value));
		}

		[[nodiscard]] predicate_evaluation unresolved(const node_ptr& node)
		{
			return {predicate_outcome::unresolved, node->reason_code};
		}

		[[nodiscard]] predicate_evaluation boolean(const node_ptr& node, const bool value)
		{
			return {value ? predicate_outcome::matched : predicate_outcome::rejected,
					node->reason_code};
		}
	} // namespace

	predicate_evaluation evaluate_symbol_predicate(const select::detail::node_ptr& node,
												   const std::optional<symbol_id>& id,
												   const symbol_index& symbols)
	{
		if (node->operation == select::detail::node_operation::constant)
			return boolean(node, node->constant);
		if (node->operation == select::detail::node_operation::negate)
		{
			auto result = evaluate_symbol_predicate(node->operands.front(), id, symbols);
			if (result.outcome == predicate_outcome::matched)
				result.outcome = predicate_outcome::rejected;
			else if (result.outcome == predicate_outcome::rejected)
				result.outcome = predicate_outcome::matched;
			return result;
		}
		if (node->operation == select::detail::node_operation::all ||
			node->operation == select::detail::node_operation::any)
		{
			const bool conjunction = node->operation == select::detail::node_operation::all;
			std::optional<predicate_evaluation> unresolved_result;
			for (const auto& operand : node->operands)
			{
				auto result = evaluate_symbol_predicate(operand, id, symbols);
				if (conjunction && result.outcome == predicate_outcome::rejected)
					return result;
				if (!conjunction && result.outcome == predicate_outcome::matched)
					return result;
				if (result.outcome == predicate_outcome::unresolved && !unresolved_result)
					unresolved_result = std::move(result);
			}
			if (unresolved_result)
				return *unresolved_result;
			return {conjunction ? predicate_outcome::matched : predicate_outcome::rejected,
					conjunction ? "select.all" : "select.any"};
		}
		if (node->predicate == "symbol.any" || node->predicate == "symbol.macro_policy" ||
			node->predicate == "symbol.variant_policy")
			return boolean(node, id.has_value());
		if (!id)
			return unresolved(node);
		const auto found = symbols.find(std::string{id->value()});
		if (found == symbols.end())
			return unresolved(node);
		const auto& symbol = found->second;
		if (node->predicate == "symbol.kinds")
			return boolean(node,
						   contains_csv(argument(node, "values"), symbol_kind_name(symbol.kind())));
		if (node->predicate == "symbol.name")
		{
			const auto policy = argument(node, "policy");
			const auto expected = argument(node, "value");
			if (policy == "unqualified_exact")
				return boolean(node, symbol.name() == expected);
			if (policy == "glob")
				return unresolved(node);
			return boolean(node, symbol.qualified_name() == expected);
		}
		if (node->predicate == "symbol.defined")
			return boolean(node,
						   symbol.definition().has_value() == (argument(node, "value") == "true"));
		return unresolved(node);
	}
} // namespace cxxlens::detail::query
