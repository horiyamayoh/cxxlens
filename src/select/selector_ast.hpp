#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/select.hpp>

#include "../core/canonical_json.hpp"

namespace cxxlens::select::detail
{
	enum class selector_domain : std::uint8_t
	{
		file,
		symbol,
		type,
		call
	};

	enum class node_operation : std::uint8_t
	{
		constant,
		predicate,
		all,
		any,
		negate
	};

	struct selector_node
	{
		selector_domain domain{selector_domain::file};
		node_operation operation{node_operation::constant};
		bool constant{true};
		std::string predicate;
		std::string reason_code;
		std::vector<std::pair<std::string, std::string>> arguments;
		std::vector<std::shared_ptr<const selector_node>> operands;
	};

	using node_ptr = std::shared_ptr<const selector_node>;
	enum class selector_truth : std::uint8_t
	{
		matched,
		not_matched,
		ambiguous
	};

	struct selector_access
	{
		[[nodiscard]] static node_ptr get(const file_selector& value)
		{
			return value.value_;
		}
		[[nodiscard]] static node_ptr get(const symbol_selector& value)
		{
			return value.value_;
		}
		[[nodiscard]] static node_ptr get(const type_selector& value)
		{
			return value.value_;
		}
		[[nodiscard]] static node_ptr get(const call_selector& value)
		{
			return value.value_;
		}
		[[nodiscard]] static node_ptr get(const semantic_selector& value)
		{
			return value.value_;
		}
		[[nodiscard]] static file_selector file(node_ptr value)
		{
			return file_selector{std::move(value)};
		}
		[[nodiscard]] static symbol_selector symbol(node_ptr value)
		{
			return symbol_selector{std::move(value)};
		}
		[[nodiscard]] static type_selector type(node_ptr value)
		{
			return type_selector{std::move(value)};
		}
		[[nodiscard]] static call_selector call(node_ptr value)
		{
			return call_selector{std::move(value)};
		}
		[[nodiscard]] static semantic_selector semantic(node_ptr value)
		{
			return semantic_selector{std::move(value)};
		}
	};

	[[nodiscard]] node_ptr default_node(selector_domain domain);
	[[nodiscard]] node_ptr ensure_node(node_ptr value, selector_domain domain);
	[[nodiscard]] node_ptr
	predicate_node(selector_domain domain,
				   std::string predicate,
				   std::vector<std::pair<std::string, std::string>> arguments = {},
				   std::vector<node_ptr> operands = {});
	[[nodiscard]] node_ptr all_node(selector_domain domain, std::vector<node_ptr> operands);
	[[nodiscard]] node_ptr any_node(selector_domain domain, std::vector<node_ptr> operands);
	[[nodiscard]] node_ptr negate_node(selector_domain domain, node_ptr operand);
	[[nodiscard]] node_ptr replace_predicate(node_ptr root, node_ptr replacement);
	[[nodiscard]] std::string serialize_selector(const node_ptr& value);
	[[nodiscard]] std::string serialize_expression(const node_ptr& value);
	[[nodiscard]] std::string explain_selector(const node_ptr& value);
	[[nodiscard]] selector_requirements calculate_requirements(const node_ptr& value);
	[[nodiscard]] selector_truth
	evaluate_truth(const node_ptr& value,
				   const std::map<std::string, selector_truth>& predicate_results);
	[[nodiscard]] result<node_ptr> parse_selector_json(std::string_view input);
} // namespace cxxlens::select::detail
