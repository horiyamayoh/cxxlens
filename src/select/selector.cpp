#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <utility>

#include "selector_ast.hpp"

namespace cxxlens::select
{
	namespace detail
	{
		namespace
		{
			using cxxlens::detail::json::json_value;

			[[nodiscard]] std::string_view domain_name(const selector_domain value)
			{
				constexpr std::array names{"file", "symbol", "type", "call"};
				return names.at(static_cast<std::size_t>(value));
			}

			[[nodiscard]] std::string reason_for(const std::string_view predicate)
			{
				std::string result{"select."};
				for (const char value : predicate)
					result.push_back(value == '_' ? '-' : value);
				return result;
			}

			[[nodiscard]] json_value expression_value(const node_ptr& input)
			{
				const auto node = ensure_node(input, selector_domain::file);
				switch (node->operation)
				{
					case node_operation::constant:
						return json_value::object{{"op", "constant"}, {"value", node->constant}};
					case node_operation::predicate:
					{
						json_value::object arguments;
						for (const auto& [key, value] : node->arguments)
							arguments.emplace_back(key, value);
						json_value::array operands;
						for (const auto& operand : node->operands)
							operands.emplace_back(expression_value(operand));
						return json_value::object{{"arguments", std::move(arguments)},
												  {"name", node->predicate},
												  {"op", "predicate"},
												  {"operands", std::move(operands)},
												  {"reason_code", node->reason_code}};
					}
					case node_operation::all:
					case node_operation::any:
					{
						json_value::array operands;
						for (const auto& operand : node->operands)
							operands.emplace_back(expression_value(operand));
						return json_value::object{
							{"op", node->operation == node_operation::all ? "all" : "any"},
							{"operands", std::move(operands)}};
					}
					case node_operation::negate:
						return json_value::object{
							{"op", "negate"},
							{"operand", expression_value(node->operands.front())}};
				}
				return {};
			}

			[[nodiscard]] std::string expression_key(const node_ptr& value)
			{
				return cxxlens::detail::json::write(expression_value(value)).value();
			}

			[[nodiscard]] node_ptr make_constant(const selector_domain domain, const bool value)
			{
				auto node = std::make_shared<selector_node>();
				node->domain = domain;
				node->constant = value;
				return node;
			}

			[[nodiscard]] node_ptr composition(const selector_domain domain,
											   const node_operation operation,
											   std::vector<node_ptr> operands)
			{
				std::vector<node_ptr> flattened;
				for (auto& operand : operands)
				{
					operand = ensure_node(std::move(operand), domain);
					if (operand->operation == operation)
						flattened.insert(
							flattened.end(), operand->operands.begin(), operand->operands.end());
					else
						flattened.push_back(std::move(operand));
				}

				const bool identity = operation == node_operation::all;
				std::vector<node_ptr> retained;
				for (auto& operand : flattened)
				{
					if (operand->operation == node_operation::constant)
					{
						if (operand->constant != identity)
							return make_constant(domain, !identity);
						continue;
					}
					retained.push_back(std::move(operand));
				}
				std::ranges::sort(retained,
								  [](const node_ptr& left, const node_ptr& right)
								  {
									  return expression_key(left) < expression_key(right);
								  });
				retained.erase(std::unique(retained.begin(),
										   retained.end(),
										   [](const node_ptr& left, const node_ptr& right)
										   {
											   return expression_key(left) == expression_key(right);
										   }),
							   retained.end());
				if (retained.empty())
					return make_constant(domain, identity);
				if (retained.size() == 1U)
					return retained.front();
				auto node = std::make_shared<selector_node>();
				node->domain = domain;
				node->operation = operation;
				node->operands = std::move(retained);
				return node;
			}

			[[nodiscard]] std::string join_enum_values(std::vector<std::string> values)
			{
				std::ranges::sort(values);
				values.erase(std::unique(values.begin(), values.end()), values.end());
				std::ostringstream output;
				for (std::size_t index = 0U; index < values.size(); ++index)
				{
					if (index != 0U)
						output << ',';
					output << values.at(index);
				}
				return output.str();
			}

			[[nodiscard]] std::string symbol_kind_name(const symbol_kind value)
			{
				constexpr std::array names{"namespace",		"record",	"class",	  "struct",
										   "union",			"function", "method",	  "constructor",
										   "destructor",	"variable", "field",	  "enum_type",
										   "enum_constant", "typedef",	"type_alias", "template",
										   "concept",		"macro",	"module",	  "parameter",
										   "unknown"};
				return names.at(static_cast<std::size_t>(value));
			}

			[[nodiscard]] std::string call_kind_name(const call_kind value)
			{
				constexpr std::array names{"direct_function",
										   "member",
										   "virtual_member",
										   "constructor",
										   "destructor",
										   "overloaded_operator",
										   "builtin_operator",
										   "function_pointer",
										   "callback",
										   "modeled",
										   "unknown"};
				return names.at(static_cast<std::size_t>(value));
			}

			template <class Enum, std::size_t Size>
			[[nodiscard]] std::string enum_name(const Enum value,
												const std::array<const char*, Size>& names)
			{
				return names.at(static_cast<std::size_t>(value));
			}

			[[nodiscard]] node_ptr add(node_ptr root, node_ptr predicate)
			{
				const auto domain = predicate->domain;
				return all_node(domain,
								{ensure_node(std::move(root), domain), std::move(predicate)});
			}

			[[nodiscard]] node_ptr set(node_ptr root, node_ptr predicate)
			{
				const auto domain = predicate->domain;
				return replace_predicate(ensure_node(std::move(root), domain),
										 std::move(predicate));
			}

			[[nodiscard]] std::vector<std::string> split_values(const std::string_view value)
			{
				std::vector<std::string> output;
				std::size_t begin = 0U;
				while (begin <= value.size())
				{
					const auto end = value.find(',', begin);
					output.emplace_back(value.substr(begin, end - begin));
					if (end == std::string_view::npos)
						break;
					begin = end + 1U;
				}
				return output;
			}

			void collect_requirements(const node_ptr& input,
									  std::set<fact_kind>& facts,
									  precision_level& precision,
									  std::set<std::string>& capabilities)
			{
				const auto& node = input;
				if (node->operation == node_operation::predicate)
				{
					const auto include = [&](const fact_kind value)
					{
						facts.insert(value);
					};
					if (node->predicate.starts_with("file."))
						include(fact_kind::file);
					if (node->predicate.starts_with("symbol."))
						include(fact_kind::symbol);
					if (node->predicate.starts_with("type."))
					{
						include(fact_kind::type);
						precision = std::max(precision, precision_level::local_semantic);
					}
					if (node->predicate.starts_with("call."))
						include(fact_kind::call);
					if (node->predicate == "symbol.derived_from" ||
						node->predicate == "type.derived_from" ||
						node->predicate == "type.including_derived" ||
						node->predicate == "call.include_derived_types")
					{
						include(fact_kind::inheritance);
						precision = std::max(precision, precision_level::workspace_semantic);
					}
					if (node->predicate == "symbol.overrides" ||
						node->predicate == "call.include_virtual_overrides")
					{
						include(fact_kind::override_relation);
						precision = std::max(precision, precision_level::workspace_semantic);
					}
					if (node->predicate == "type.convertible_to")
						include(fact_kind::conversion);
					if (node->predicate.ends_with(".macro_policy"))
					{
						const auto policy = node->arguments.front().second;
						if (policy != "exclude")
							include(fact_kind::macro_expansion);
					}
					if (node->predicate == "call.dispatch" &&
						node->arguments.front().second == "static_and_virtual_candidates")
					{
						include(fact_kind::override_relation);
						precision = std::max(precision, precision_level::workspace_semantic);
					}
					if (node->predicate == "call.precision")
					{
						const auto names = split_values(node->arguments.front().second);
						(void)names;
						const auto requested = static_cast<precision_level>(
							std::stoul(node->arguments.front().second));
						precision = std::max(precision, requested);
					}
				}
				for (const auto& operand : node->operands)
					collect_requirements(operand, facts, precision, capabilities);
			}

			void append_explanation(const node_ptr& node, std::ostringstream& output)
			{
				switch (node->operation)
				{
					case node_operation::constant:
						output << (node->constant ? "true" : "false");
						break;
					case node_operation::predicate:
						output << node->reason_code;
						for (const auto& [key, value] : node->arguments)
							output << ' ' << key << '=' << value;
						for (const auto& operand : node->operands)
						{
							output << " (";
							append_explanation(operand, output);
							output << ')';
						}
						break;
					case node_operation::all:
					case node_operation::any:
						output << (node->operation == node_operation::all ? "all" : "any") << '(';
						for (std::size_t index = 0U; index < node->operands.size(); ++index)
						{
							if (index != 0U)
								output << ", ";
							append_explanation(node->operands.at(index), output);
						}
						output << ')';
						break;
					case node_operation::negate:
						output << "not(";
						append_explanation(node->operands.front(), output);
						output << ')';
						break;
				}
			}
		} // namespace

		node_ptr predicate_node(const selector_domain domain,
								std::string predicate,
								std::vector<std::pair<std::string, std::string>> arguments,
								std::vector<node_ptr> operands)
		{
			auto node = std::make_shared<selector_node>();
			node->domain = domain;
			node->operation = node_operation::predicate;
			node->reason_code = reason_for(predicate);
			node->predicate = std::move(predicate);
			std::ranges::sort(arguments, {}, &std::pair<std::string, std::string>::first);
			node->arguments = std::move(arguments);
			node->operands = std::move(operands);
			return node;
		}

		node_ptr default_node(const selector_domain domain)
		{
			std::vector<node_ptr> defaults{
				predicate_node(domain, std::string{domain_name(domain)} + ".any")};
			if (domain == selector_domain::symbol)
			{
				defaults.push_back(
					predicate_node(domain, "symbol.macro_policy", {{"value", "exclude"}}));
				defaults.push_back(predicate_node(
					domain, "symbol.variant_policy", {{"value", "reject_disagreement"}}));
			}
			if (domain == selector_domain::call)
			{
				defaults.push_back(
					predicate_node(domain, "call.dispatch", {{"value", "direct_only"}}));
				defaults.push_back(
					predicate_node(domain, "call.implicit_policy", {{"value", "spelled_only"}}));
				defaults.push_back(
					predicate_node(domain, "call.macro_policy", {{"value", "exclude"}}));
				defaults.push_back(
					predicate_node(domain, "call.template_policy", {{"value", "patterns"}}));
				defaults.push_back(predicate_node(
					domain, "call.variant_policy", {{"value", "reject_disagreement"}}));
			}
			return all_node(domain, std::move(defaults));
		}

		node_ptr ensure_node(node_ptr value, const selector_domain domain)
		{
			if (!value)
				return default_node(domain);
			return value;
		}

		node_ptr all_node(const selector_domain domain, std::vector<node_ptr> operands)
		{
			return composition(domain, node_operation::all, std::move(operands));
		}

		node_ptr any_node(const selector_domain domain, std::vector<node_ptr> operands)
		{
			return composition(domain, node_operation::any, std::move(operands));
		}

		node_ptr negate_node(const selector_domain domain, node_ptr operand)
		{
			operand = ensure_node(std::move(operand), domain);
			if (operand->operation == node_operation::constant)
				return make_constant(domain, !operand->constant);
			if (operand->operation == node_operation::negate)
				return operand->operands.front();
			auto node = std::make_shared<selector_node>();
			node->domain = domain;
			node->operation = node_operation::negate;
			node->operands.push_back(std::move(operand));
			return node;
		}

		node_ptr replace_predicate(node_ptr root, node_ptr replacement)
		{
			std::vector<node_ptr> operands;
			if (root->operation == node_operation::all)
				operands = root->operands;
			else
				operands.push_back(std::move(root));
			std::erase_if(operands,
						  [&](const node_ptr& value)
						  {
							  return value->operation == node_operation::predicate &&
								  value->predicate == replacement->predicate;
						  });
			operands.push_back(std::move(replacement));
			const auto domain = operands.front()->domain;
			return all_node(domain, std::move(operands));
		}

		std::string serialize_expression(const node_ptr& value)
		{
			return cxxlens::detail::json::write(expression_value(value)).value();
		}

		std::string serialize_selector(const node_ptr& value)
		{
			json_value document{cxxlens::detail::json::envelope(
				{"cxxlens.selector.v1"},
				{{"domain", std::string{domain_name(value->domain)}},
				 {"expression", expression_value(value)},
				 {"normalization_version", std::uint64_t{1U}}})};
			return cxxlens::detail::json::write(document).value();
		}

		std::string explain_selector(const node_ptr& value)
		{
			std::ostringstream output;
			output << domain_name(value->domain) << ": ";
			append_explanation(value, output);
			return output.str();
		}

		selector_requirements calculate_requirements(const node_ptr& value)
		{
			std::set<fact_kind> concrete;
			std::set<std::string> capability_set;
			precision_level precision = precision_level::ast_structural;
			collect_requirements(value, concrete, precision, capability_set);
			selector_requirements output;
			for (const auto kind : concrete)
				output.facts = output.facts.include(kind);
			output.facts = output.facts.precision(precision);
			output.minimum_precision = precision;
			output.capabilities.assign(capability_set.begin(), capability_set.end());
			return output;
		}

		selector_truth
		evaluate_truth(const node_ptr& value,
					   const std::map<std::string, selector_truth>& predicate_results)
		{
			if (value->operation == node_operation::constant)
				return value->constant ? selector_truth::matched : selector_truth::not_matched;
			if (value->operation == node_operation::predicate)
			{
				const auto position = predicate_results.find(value->reason_code);
				return position == predicate_results.end() ? selector_truth::ambiguous
														   : position->second;
			}
			if (value->operation == node_operation::negate)
			{
				const auto result = evaluate_truth(value->operands.front(), predicate_results);
				if (result == selector_truth::matched)
					return selector_truth::not_matched;
				if (result == selector_truth::not_matched)
					return selector_truth::matched;
				return selector_truth::ambiguous;
			}
			const bool conjunction = value->operation == node_operation::all;
			bool ambiguous = false;
			for (const auto& operand : value->operands)
			{
				const auto result = evaluate_truth(operand, predicate_results);
				if (conjunction && result == selector_truth::not_matched)
					return selector_truth::not_matched;
				if (!conjunction && result == selector_truth::matched)
					return selector_truth::matched;
				ambiguous = ambiguous || result == selector_truth::ambiguous;
			}
			if (ambiguous)
				return selector_truth::ambiguous;
			return conjunction ? selector_truth::matched : selector_truth::not_matched;
		}
	} // namespace detail

	namespace
	{
		using detail::node_ptr;
		using detail::selector_domain;
		constexpr std::array name_match_names{
			"exact", "qualified_exact", "unqualified_exact", "glob"};
		constexpr std::array macro_policy_names{"exclude",
												"include_with_origin",
												"only_macro_arguments",
												"only_macro_bodies",
												"only_expansions"};
		constexpr std::array implicit_policy_names{
			"spelled_only", "include_language_implicit", "implicit_only"};
		constexpr std::array template_policy_names{
			"patterns", "observed_instantiations", "patterns_and_observed_instantiations"};
		constexpr std::array variant_policy_names{
			"any_variant", "all_variants", "report_per_variant", "reject_disagreement"};
		constexpr std::array dispatch_policy_names{"direct_only",
												   "static_target",
												   "static_and_virtual_candidates",
												   "include_indirect_candidates"};

		template <class Enum, std::size_t Size>
		[[nodiscard]] std::string enum_value(const Enum value,
											 const std::array<const char*, Size>& names)
		{
			return names.at(static_cast<std::size_t>(value));
		}

		[[nodiscard]] std::string bool_value(const bool value)
		{
			return value ? "true" : "false";
		}

		[[nodiscard]] node_ptr nested(const selector_domain domain,
									  std::string name,
									  node_ptr operand,
									  const selector_domain operand_domain)
		{
			return detail::predicate_node(
				domain,
				std::move(name),
				{},
				{detail::ensure_node(std::move(operand), operand_domain)});
		}
	} // namespace

	file_selector::file_selector(std::shared_ptr<const detail::selector_node> value)
		: value_{std::move(value)}
	{
	}
	file_selector file_selector::path_exact(
		path value) const // NOLINT(performance-unnecessary-value-param) exact public value API
	{
		return detail::selector_access::file(detail::add(
			value_,
			detail::predicate_node(
				selector_domain::file, "file.path_exact", {{"value", value.generic_string()}})));
	}
	file_selector file_selector::path_glob(std::string value) const
	{
		return detail::selector_access::file(detail::add(
			value_,
			detail::predicate_node(
				selector_domain::file, "file.path_glob", {{"value", std::move(value)}})));
	}
	file_selector file_selector::generated(const bool value) const
	{
		return detail::selector_access::file(detail::set(
			value_,
			detail::predicate_node(
				selector_domain::file, "file.generated", {{"value", bool_value(value)}})));
	}
	file_selector file_selector::system(const bool value) const
	{
		return detail::selector_access::file(
			detail::set(value_,
						detail::predicate_node(
							selector_domain::file, "file.system", {{"value", bool_value(value)}})));
	}
	// Exact immutable builder API intentionally accepts a detached value sequence.
	// NOLINTNEXTLINE(performance-unnecessary-value-param)
	file_selector file_selector::any_of(std::vector<file_selector> values) const
	{
		std::vector<node_ptr> alternatives;
		alternatives.reserve(values.size());
		for (const auto& value : values)
			alternatives.push_back(
				detail::ensure_node(detail::selector_access::get(value), selector_domain::file));
		return detail::selector_access::file(
			detail::all_node(selector_domain::file,
							 {detail::ensure_node(value_, selector_domain::file),
							  detail::any_node(selector_domain::file, std::move(alternatives))}));
	}
	file_selector file_selector::negate() const
	{
		return detail::selector_access::file(detail::negate_node(selector_domain::file, value_));
	}
	std::string file_selector::to_json() const
	{
		return detail::serialize_selector(detail::ensure_node(value_, selector_domain::file));
	}

	symbol_selector::symbol_selector(std::shared_ptr<const detail::selector_node> value)
		: value_{std::move(value)}
	{
	}
	symbol_selector symbol_selector::kind(const symbol_kind value) const
	{
		return kinds({value});
	}
	// Exact immutable builder API intentionally accepts a detached value sequence.
	// NOLINTNEXTLINE(performance-unnecessary-value-param)
	symbol_selector symbol_selector::kinds(std::vector<symbol_kind> values) const
	{
		if (values.empty())
			return detail::selector_access::symbol(
				detail::all_node(selector_domain::symbol,
								 {detail::ensure_node(value_, selector_domain::symbol),
								  detail::any_node(selector_domain::symbol, {})}));
		std::vector<std::string> names;
		names.reserve(values.size());
		for (const auto value : values)
			names.push_back(detail::symbol_kind_name(value));
		return detail::selector_access::symbol(detail::add(
			value_,
			detail::predicate_node(selector_domain::symbol,
								   "symbol.kinds",
								   {{"values", detail::join_enum_values(std::move(names))}})));
	}
	symbol_selector symbol_selector::name(std::string value, const name_match policy) const
	{
		return detail::selector_access::symbol(
			detail::add(value_,
						detail::predicate_node(selector_domain::symbol,
											   "symbol.name",
											   {{"policy", enum_value(policy, name_match_names)},
												{"value", std::move(value)}})));
	}
	// NOLINTNEXTLINE(performance-unnecessary-value-param) exact public value API
	symbol_selector symbol_selector::declared_in(file_selector value) const
	{
		return detail::selector_access::symbol(
			detail::add(value_,
						nested(selector_domain::symbol,
							   "symbol.declared_in",
							   detail::selector_access::get(value),
							   selector_domain::file)));
	}
	symbol_selector symbol_selector::defined(const bool value) const
	{
		return detail::selector_access::symbol(detail::set(
			value_,
			detail::predicate_node(
				selector_domain::symbol, "symbol.defined", {{"value", bool_value(value)}})));
	}
	// NOLINTNEXTLINE(performance-unnecessary-value-param) exact public value API
	symbol_selector symbol_selector::member_of(symbol_selector value) const
	{
		return detail::selector_access::symbol(
			detail::add(value_,
						nested(selector_domain::symbol,
							   "symbol.member_of",
							   detail::selector_access::get(value),
							   selector_domain::symbol)));
	}
	// NOLINTNEXTLINE(performance-unnecessary-value-param) exact public value API
	symbol_selector symbol_selector::derived_from(symbol_selector value) const
	{
		return detail::selector_access::symbol(
			detail::add(value_,
						nested(selector_domain::symbol,
							   "symbol.derived_from",
							   detail::selector_access::get(value),
							   selector_domain::symbol)));
	}
	// NOLINTNEXTLINE(performance-unnecessary-value-param) exact public value API
	symbol_selector symbol_selector::overrides(symbol_selector value) const
	{
		return detail::selector_access::symbol(
			detail::add(value_,
						nested(selector_domain::symbol,
							   "symbol.overrides",
							   detail::selector_access::get(value),
							   selector_domain::symbol)));
	}
	symbol_selector symbol_selector::public_surface(const bool value) const
	{
		return detail::selector_access::symbol(detail::set(
			value_,
			detail::predicate_node(
				selector_domain::symbol, "symbol.public_surface", {{"value", bool_value(value)}})));
	}
	symbol_selector symbol_selector::macro(const macro_match_policy value) const
	{
		return detail::selector_access::symbol(detail::set(
			value_,
			detail::predicate_node(selector_domain::symbol,
								   "symbol.macro_policy",
								   {{"value", enum_value(value, macro_policy_names)}})));
	}
	symbol_selector symbol_selector::variants(const variant_match_policy value) const
	{
		return detail::selector_access::symbol(detail::set(
			value_,
			detail::predicate_node(selector_domain::symbol,
								   "symbol.variant_policy",
								   {{"value", enum_value(value, variant_policy_names)}})));
	}
	// Exact immutable builder API intentionally accepts a detached value sequence.
	// NOLINTNEXTLINE(performance-unnecessary-value-param)
	symbol_selector symbol_selector::any_of(std::vector<symbol_selector> values) const
	{
		std::vector<node_ptr> alternatives;
		alternatives.reserve(values.size());
		for (const auto& value : values)
			alternatives.push_back(
				detail::ensure_node(detail::selector_access::get(value), selector_domain::symbol));
		return detail::selector_access::symbol(
			detail::all_node(selector_domain::symbol,
							 {detail::ensure_node(value_, selector_domain::symbol),
							  detail::any_node(selector_domain::symbol, std::move(alternatives))}));
	}
	// Exact immutable builder API intentionally accepts a detached value sequence.
	// NOLINTNEXTLINE(performance-unnecessary-value-param)
	symbol_selector symbol_selector::all_of(std::vector<symbol_selector> values) const
	{
		std::vector<node_ptr> operands{detail::ensure_node(value_, selector_domain::symbol)};
		for (const auto& value : values)
			operands.push_back(
				detail::ensure_node(detail::selector_access::get(value), selector_domain::symbol));
		return detail::selector_access::symbol(
			detail::all_node(selector_domain::symbol, std::move(operands)));
	}
	symbol_selector symbol_selector::negate() const
	{
		return detail::selector_access::symbol(
			detail::negate_node(selector_domain::symbol, value_));
	}
	selector_requirements symbol_selector::requirements() const
	{
		return detail::calculate_requirements(detail::ensure_node(value_, selector_domain::symbol));
	}
	std::string symbol_selector::explain() const
	{
		return detail::explain_selector(detail::ensure_node(value_, selector_domain::symbol));
	}
	std::string symbol_selector::to_json() const
	{
		return detail::serialize_selector(detail::ensure_node(value_, selector_domain::symbol));
	}

	type_selector::type_selector(std::shared_ptr<const detail::selector_node> value)
		: value_{std::move(value)}
	{
	}
	type_selector type_selector::canonical(std::string value) const
	{
		return detail::selector_access::type(detail::add(
			value_,
			detail::predicate_node(
				selector_domain::type, "type.canonical", {{"value", std::move(value)}})));
	}
	type_selector type_selector::spelling(std::string value) const
	{
		return detail::selector_access::type(detail::add(
			value_,
			detail::predicate_node(
				selector_domain::type, "type.spelling", {{"value", std::move(value)}})));
	}
	// NOLINTNEXTLINE(performance-unnecessary-value-param) exact public value API
	type_selector type_selector::declared_as(symbol_selector value) const
	{
		return detail::selector_access::type(detail::add(value_,
														 nested(selector_domain::type,
																"type.declared_as",
																detail::selector_access::get(value),
																selector_domain::symbol)));
	}
	// NOLINTNEXTLINE(performance-unnecessary-value-param) exact public value API
	type_selector type_selector::pointer_to(type_selector value) const
	{
		return detail::selector_access::type(detail::add(value_,
														 nested(selector_domain::type,
																"type.pointer_to",
																detail::selector_access::get(value),
																selector_domain::type)));
	}
	// NOLINTNEXTLINE(performance-unnecessary-value-param) exact public value API
	type_selector type_selector::reference_to(type_selector value) const
	{
		return detail::selector_access::type(detail::add(value_,
														 nested(selector_domain::type,
																"type.reference_to",
																detail::selector_access::get(value),
																selector_domain::type)));
	}
	type_selector type_selector::const_qualified(const bool value) const
	{
		return detail::selector_access::type(detail::set(
			value_,
			detail::predicate_node(
				selector_domain::type, "type.const_qualified", {{"value", bool_value(value)}})));
	}
	// NOLINTNEXTLINE(performance-unnecessary-value-param) exact public value API
	type_selector type_selector::derived_from(symbol_selector value) const
	{
		return detail::selector_access::type(detail::add(value_,
														 nested(selector_domain::type,
																"type.derived_from",
																detail::selector_access::get(value),
																selector_domain::symbol)));
	}
	type_selector type_selector::including_derived(const bool value) const
	{
		return detail::selector_access::type(detail::set(
			value_,
			detail::predicate_node(
				selector_domain::type, "type.including_derived", {{"value", bool_value(value)}})));
	}
	// NOLINTNEXTLINE(performance-unnecessary-value-param) exact public value API
	type_selector type_selector::convertible_to(type_selector value) const
	{
		return detail::selector_access::type(detail::add(value_,
														 nested(selector_domain::type,
																"type.convertible_to",
																detail::selector_access::get(value),
																selector_domain::type)));
	}
	type_selector type_selector::specialization_of(std::string value) const
	{
		return detail::selector_access::type(detail::add(
			value_,
			detail::predicate_node(
				selector_domain::type, "type.specialization_of", {{"value", std::move(value)}})));
	}
	type_selector type_selector::any_cvref() const
	{
		return detail::selector_access::type(detail::set(
			value_,
			detail::predicate_node(selector_domain::type, "type.any_cvref", {{"value", "true"}})));
	}
	selector_requirements type_selector::requirements() const
	{
		return detail::calculate_requirements(detail::ensure_node(value_, selector_domain::type));
	}
	std::string type_selector::explain() const
	{
		return detail::explain_selector(detail::ensure_node(value_, selector_domain::type));
	}
	std::string type_selector::to_json() const
	{
		return detail::serialize_selector(detail::ensure_node(value_, selector_domain::type));
	}

	call_selector::call_selector(std::shared_ptr<const detail::selector_node> value)
		: value_{std::move(value)}
	{
	}
	call_selector call_selector::kind(const call_kind value) const
	{
		return kinds({value});
	}
	// Exact immutable builder API intentionally accepts a detached value sequence.
	// NOLINTNEXTLINE(performance-unnecessary-value-param)
	call_selector call_selector::kinds(std::vector<call_kind> values) const
	{
		if (values.empty())
			return detail::selector_access::call(
				detail::all_node(selector_domain::call,
								 {detail::ensure_node(value_, selector_domain::call),
								  detail::any_node(selector_domain::call, {})}));
		std::vector<std::string> names;
		names.reserve(values.size());
		for (const auto value : values)
			names.push_back(detail::call_kind_name(value));
		return detail::selector_access::call(detail::add(
			value_,
			detail::predicate_node(selector_domain::call,
								   "call.kinds",
								   {{"values", detail::join_enum_values(std::move(names))}})));
	}
	// NOLINTNEXTLINE(performance-unnecessary-value-param) exact public value API
	call_selector call_selector::callee(symbol_selector value) const
	{
		return detail::selector_access::call(detail::add(value_,
														 nested(selector_domain::call,
																"call.callee",
																detail::selector_access::get(value),
																selector_domain::symbol)));
	}
	call_selector call_selector::callee_name(std::string value) const
	{
		return detail::selector_access::call(detail::add(
			value_,
			detail::predicate_node(
				selector_domain::call, "call.callee_name", {{"value", std::move(value)}})));
	}
	call_selector call_selector::function_name(std::string value) const
	{
		return detail::selector_access::call(detail::add(
			value_,
			detail::predicate_node(
				selector_domain::call, "call.function_name", {{"value", std::move(value)}})));
	}
	call_selector call_selector::method_name(std::string value) const
	{
		return detail::selector_access::call(detail::add(
			value_,
			detail::predicate_node(
				selector_domain::call, "call.method_name", {{"value", std::move(value)}})));
	}
	// NOLINTNEXTLINE(performance-unnecessary-value-param) exact public value API
	call_selector call_selector::receiver_type(type_selector value) const
	{
		return detail::selector_access::call(detail::add(value_,
														 nested(selector_domain::call,
																"call.receiver_type",
																detail::selector_access::get(value),
																selector_domain::type)));
	}
	call_selector call_selector::include_derived_types(const bool value) const
	{
		return detail::selector_access::call(
			detail::set(value_,
						detail::predicate_node(selector_domain::call,
											   "call.include_derived_types",
											   {{"value", bool_value(value)}})));
	}
	call_selector call_selector::include_virtual_overrides(const bool value) const
	{
		return detail::selector_access::call(
			detail::set(value_,
						detail::predicate_node(selector_domain::call,
											   "call.include_virtual_overrides",
											   {{"value", bool_value(value)}})));
	}
	call_selector call_selector::dispatch(const dispatch_policy value) const
	{
		return detail::selector_access::call(detail::set(
			value_,
			detail::predicate_node(selector_domain::call,
								   "call.dispatch",
								   {{"value", enum_value(value, dispatch_policy_names)}})));
	}
	// NOLINTNEXTLINE(performance-unnecessary-value-param) exact public value API
	call_selector call_selector::argument_type(const std::size_t index, type_selector value) const
	{
		return detail::selector_access::call(detail::add(
			value_,
			detail::predicate_node(selector_domain::call,
								   "call.argument_type",
								   {{"index", std::to_string(index)}},
								   {detail::ensure_node(detail::selector_access::get(value),
														selector_domain::type)})));
	}
	// NOLINTNEXTLINE(performance-unnecessary-value-param) exact public value API
	call_selector call_selector::inside(symbol_selector value) const
	{
		return detail::selector_access::call(detail::add(value_,
														 nested(selector_domain::call,
																"call.inside",
																detail::selector_access::get(value),
																selector_domain::symbol)));
	}
	// NOLINTNEXTLINE(performance-unnecessary-value-param) exact public value API
	call_selector call_selector::in_file(file_selector value) const
	{
		return detail::selector_access::call(detail::add(value_,
														 nested(selector_domain::call,
																"call.in_file",
																detail::selector_access::get(value),
																selector_domain::file)));
	}
	call_selector call_selector::implicit(const implicit_node_policy value) const
	{
		return detail::selector_access::call(detail::set(
			value_,
			detail::predicate_node(selector_domain::call,
								   "call.implicit_policy",
								   {{"value", enum_value(value, implicit_policy_names)}})));
	}
	call_selector call_selector::macro(const macro_match_policy value) const
	{
		return detail::selector_access::call(detail::set(
			value_,
			detail::predicate_node(selector_domain::call,
								   "call.macro_policy",
								   {{"value", enum_value(value, macro_policy_names)}})));
	}
	call_selector call_selector::templates(const template_selection_policy value) const
	{
		return detail::selector_access::call(detail::set(
			value_,
			detail::predicate_node(selector_domain::call,
								   "call.template_policy",
								   {{"value", enum_value(value, template_policy_names)}})));
	}
	call_selector call_selector::variants(const variant_match_policy value) const
	{
		return detail::selector_access::call(detail::set(
			value_,
			detail::predicate_node(selector_domain::call,
								   "call.variant_policy",
								   {{"value", enum_value(value, variant_policy_names)}})));
	}
	call_selector call_selector::precision(const precision_level value) const
	{
		return detail::selector_access::call(detail::set(
			value_,
			detail::predicate_node(selector_domain::call,
								   "call.precision",
								   {{"value", std::to_string(static_cast<std::uint8_t>(value))}})));
	}
	selector_requirements call_selector::requirements() const
	{
		return detail::calculate_requirements(detail::ensure_node(value_, selector_domain::call));
	}
	std::string call_selector::explain() const
	{
		return detail::explain_selector(detail::ensure_node(value_, selector_domain::call));
	}
	std::string call_selector::to_json() const
	{
		return detail::serialize_selector(detail::ensure_node(value_, selector_domain::call));
	}

	semantic_selector::semantic_selector(std::shared_ptr<const detail::selector_node> value)
		: value_{std::move(value)}
	{
	}
	result<semantic_selector> semantic_selector::from_json(const std::string_view input)
	{
		auto parsed = detail::parse_selector_json(input);
		if (!parsed)
			return std::move(parsed.error());
		return detail::selector_access::semantic(std::move(parsed.value()));
	}
	selector_requirements semantic_selector::requirements() const
	{
		return detail::calculate_requirements(value_);
	}
	std::string semantic_selector::explain() const
	{
		return detail::explain_selector(value_);
	}
	std::string semantic_selector::to_json() const
	{
		return detail::serialize_selector(value_);
	}

	symbol_selector any_symbol()
	{
		return symbol_selector{};
	}
	symbol_selector function(std::string qualified_name)
	{
		auto result = any_symbol().kind(symbol_kind::function);
		return qualified_name.empty() ? result : result.name(std::move(qualified_name));
	}
	symbol_selector method(std::string qualified_name)
	{
		auto result = any_symbol().kind(symbol_kind::method);
		return qualified_name.empty() ? result : result.name(std::move(qualified_name));
	}
	symbol_selector record(std::string qualified_name)
	{
		auto result = any_symbol().kinds(
			{symbol_kind::record, symbol_kind::class_, symbol_kind::struct_, symbol_kind::union_});
		return qualified_name.empty() ? result : result.name(std::move(qualified_name));
	}
	symbol_selector variable(std::string qualified_name)
	{
		auto result = any_symbol().kind(symbol_kind::variable);
		return qualified_name.empty() ? result : result.name(std::move(qualified_name));
	}
	symbol_selector macro(std::string name)
	{
		auto result =
			any_symbol().kind(symbol_kind::macro).macro(macro_match_policy::include_with_origin);
		return name.empty() ? result : result.name(std::move(name), name_match::exact);
	}
	type_selector type(std::string canonical)
	{
		auto result = type_selector{};
		return canonical.empty() ? result : result.canonical(std::move(canonical));
	}
	call_selector any_call()
	{
		return call_selector{};
	}
	call_selector calls_to(symbol_selector callee)
	{
		return any_call().callee(std::move(callee));
	}
	call_selector calls_to_function(std::string qualified_name)
	{
		return any_call().callee(function(std::move(qualified_name)));
	}
	call_selector calls_to_method(std::string receiver_type, std::string method_name)
	{
		return any_call()
			.receiver_type(type(std::move(receiver_type)))
			.method_name(std::move(method_name))
			.kind(call_kind::member);
	}
	semantic_selector semantic(
		file_selector value) // NOLINT(performance-unnecessary-value-param) exact type-erasure API
	{
		return detail::selector_access::semantic(
			detail::ensure_node(detail::selector_access::get(value), selector_domain::file));
	}
	semantic_selector semantic(
		symbol_selector value) // NOLINT(performance-unnecessary-value-param) exact type-erasure API
	{
		return detail::selector_access::semantic(
			detail::ensure_node(detail::selector_access::get(value), selector_domain::symbol));
	}
	semantic_selector semantic(
		type_selector value) // NOLINT(performance-unnecessary-value-param) exact type-erasure API
	{
		return detail::selector_access::semantic(
			detail::ensure_node(detail::selector_access::get(value), selector_domain::type));
	}
	semantic_selector semantic(
		call_selector value) // NOLINT(performance-unnecessary-value-param) exact type-erasure API
	{
		return detail::selector_access::semantic(
			detail::ensure_node(detail::selector_access::get(value), selector_domain::call));
	}
} // namespace cxxlens::select
