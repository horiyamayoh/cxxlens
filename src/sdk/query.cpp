#include <algorithm>
#include <iomanip>
#include <map>
#include <ranges>
#include <set>
#include <sstream>

#include <cxxlens/sdk/query.hpp>

#include "json_internal.hpp"
#include "query_internal.hpp"

namespace cxxlens::sdk::query
{
	namespace
	{
		[[nodiscard]] error
		query_error(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] std::string json_string(const std::string_view value)
		{
			return cxxlens::sdk::detail::canonical_json_string(value);
		}

		[[nodiscard]] std::string column_json(const column_ref& column)
		{
			return R"({"availability":"require","column_id":)" + json_string(column.column_id) +
				"}";
		}

		[[nodiscard]] std::string scalar_json(const scalar_value& value)
		{
			return std::visit(
				[](const auto& item) -> std::string
				{
					using item_type = std::remove_cvref_t<decltype(item)>;
					if constexpr (std::same_as<item_type, bool>)
						return item ? "true" : "false";
					else if constexpr (std::integral<item_type>)
						return std::to_string(item);
					else if constexpr (std::same_as<item_type, std::string>)
						return json_string(item);
					else
					{
						std::ostringstream output;
						output << '"' << std::hex << std::setfill('0');
						for (const auto byte : item)
							output << std::setw(2) << std::to_integer<unsigned int>(byte);
						output << '"';
						return output.str();
					}
				},
				value);
		}

		[[nodiscard]] std::string literal_json(const detached_cell& cell)
		{
			auto present_type = cell.type;
			present_type.optional = false;
			return "{\"type\":" + json_string(present_type.canonical_name()) +
				",\"value\":" + scalar_json(*cell.value) + "}";
		}

		[[nodiscard]] std::string plain_digest(const std::string& value)
		{
			return content_digest(std::as_bytes(std::span{value.data(), value.size()}));
		}

		[[nodiscard]] bool valid_identifier(const std::string_view value)
		{
			return !value.empty() && value.front() >= 'a' && value.front() <= 'z' &&
				std::ranges::all_of(value,
									[](const char byte)
									{
										return (byte >= 'a' && byte <= 'z') ||
											(byte >= '0' && byte <= '9') || byte == '_';
									});
		}

		[[nodiscard]] std::string default_alias(const relation_descriptor& descriptor)
		{
			auto output = descriptor.name;
			std::ranges::replace(output, '.', '_');
			std::ranges::replace(output, '-', '_');
			return output;
		}

		[[nodiscard]] std::string projections_json(const std::span<const column_ref> columns)
		{
			const auto aliases = detail::output_aliases(columns);
			std::ostringstream output;
			output << '[';
			for (std::size_t index = 0U; index < columns.size(); ++index)
			{
				if (index != 0U)
					output << ',';
				output << "{\"column\":" << column_json(columns[index]) << R"(,"output":"output.)"
					   << aliases[index] << "\"}";
			}
			output << ']';
			return output.str();
		}

		[[nodiscard]] std::string order_keys_json(const std::span<const column_ref> columns)
		{
			std::ostringstream output;
			output << '[';
			for (std::size_t index = 0U; index < columns.size(); ++index)
			{
				if (index != 0U)
					output << ',';
				output << "{\"cell_state_order\":[\"absent\",\"present\",\"unknown\"],"
						  "\"column\":"
					   << column_json(columns[index]) << R"(,"direction":"ascending"})";
			}
			output << ']';
			return output.str();
		}

		[[nodiscard]] std::string column_json(const ir_column_ref& column)
		{
			const auto availability = column.availability == column_availability::require
				? "require"
				: "absent_if_schema_missing";
			return "{\"availability\":" + json_string(availability) +
				",\"column_id\":" + json_string(column.column_id) + "}";
		}

		[[nodiscard]] std::string predicate_json(const decoded_predicate& predicate)
		{
			switch (predicate.kind)
			{
				case predicate_kind::equals_present:
					return "{\"column\":" + column_json(*predicate.column) +
						R"(,"kind":"equals_present","literal":{"type":)" +
						json_string(predicate.literal_value->type) +
						",\"value\":" + scalar_json(predicate.literal_value->value) + "}}";
				case predicate_kind::column_equals_present:
					return R"({"kind":"column_equals_present","left":)" +
						column_json(*predicate.left) +
						",\"right\":" + column_json(*predicate.right) + "}";
				case predicate_kind::is_present:
				case predicate_kind::is_absent:
				case predicate_kind::is_unknown:
				{
					const auto kind = predicate.kind == predicate_kind::is_present ? "is_present"
						: predicate.kind == predicate_kind::is_absent			   ? "is_absent"
																				   : "is_unknown";
					return "{\"column\":" + column_json(*predicate.column) +
						",\"kind\":" + json_string(kind) + "}";
				}
				case predicate_kind::all:
				case predicate_kind::any:
				{
					std::vector<std::string> operands;
					operands.reserve(predicate.operands.size());
					for (const auto& operand : predicate.operands)
						operands.push_back(predicate_json(operand));
					std::ranges::sort(operands,
									  [](const std::string& left, const std::string& right)
									  {
										  const auto left_digest = plain_digest(left);
										  const auto right_digest = plain_digest(right);
										  return left_digest != right_digest
											  ? left_digest < right_digest
											  : left < right;
									  });
					operands.erase(std::ranges::unique(operands).begin(), operands.end());
					std::ostringstream output;
					output << "{\"kind\":"
						   << json_string(predicate.kind == predicate_kind::all ? "and" : "or")
						   << ",\"operands\":[";
					for (std::size_t index = 0U; index < operands.size(); ++index)
					{
						if (index != 0U)
							output << ',';
						output << operands[index];
					}
					output << "]}";
					return output.str();
				}
			}
			return "{}";
		}

		[[nodiscard]] std::string canonical_arguments(const ir_node& node)
		{
			auto decoded = decode_arguments(node);
			if (!decoded)
				return "{}";
			return std::visit(
				[](const auto& value) -> std::string
				{
					using value_type = std::remove_cvref_t<decltype(value)>;
					if constexpr (std::same_as<value_type, scan_arguments>)
						return "{\"alias\":" + json_string(value.alias) +
							",\"descriptor_id\":" + json_string(value.descriptor_id) + "}";
					else if constexpr (std::same_as<value_type, predicate_arguments>)
						return "{\"predicate\":" + predicate_json(value.predicate) + "}";
					else if constexpr (std::same_as<value_type, project_arguments>)
					{
						std::ostringstream output;
						output << "{\"columns\":[";
						for (std::size_t index = 0U; index < value.columns.size(); ++index)
						{
							if (index != 0U)
								output << ',';
							output << "{\"column\":" << column_json(value.columns[index].column)
								   << ",\"output\":" << json_string(value.columns[index].output)
								   << '}';
						}
						output << "]}";
						return output.str();
					}
					else if constexpr (std::same_as<value_type, empty_arguments>)
						return "{}";
					else if constexpr (std::same_as<value_type, order_arguments>)
					{
						auto state_name = [](const cell_state state)
						{
							switch (state)
							{
								case cell_state::present:
									return "present";
								case cell_state::absent:
									return "absent";
								case cell_state::unknown:
									return "unknown";
							}
							return "unknown";
						};
						std::ostringstream output;
						output << "{\"keys\":[";
						for (std::size_t index = 0U; index < value.keys.size(); ++index)
						{
							if (index != 0U)
								output << ',';
							const auto& key = value.keys[index];
							output << "{\"cell_state_order\":[";
							for (std::size_t state = 0U; state < key.cell_state_order.size();
								 ++state)
							{
								if (state != 0U)
									output << ',';
								output << json_string(state_name(key.cell_state_order[state]));
							}
							output
								<< "],\"column\":" << column_json(key.column) << ",\"direction\":"
								<< json_string(key.ascending ? "ascending" : "descending") << '}';
						}
						output << "]}";
						return output.str();
					}
					else if constexpr (std::same_as<value_type, limit_arguments>)
						return "{\"count\":" + std::to_string(value.count) + "}";
					else if constexpr (std::same_as<value_type, condition_arguments>)
					{
						std::ostringstream output;
						output << "{\"alternatives\":[";
						for (std::size_t index = 0U; index < value.alternatives.size(); ++index)
						{
							if (index != 0U)
								output << ',';
							output << json_string(value.alternatives[index]);
						}
						output << "],\"universe\":" << json_string(value.universe) << '}';
						return output.str();
					}
					else
						return "{\"interpretation\":" + json_string(value.interpretation) + "}";
				},
				*decoded);
		}

		[[nodiscard]] std::string
		nested_node_json(const std::string_view id,
						 const std::map<std::string, const ir_node*, std::less<>>& nodes)
		{
			const auto found = nodes.find(id);
			if (found == nodes.end())
				return "{}";
			const auto& node = *found->second;
			std::vector<std::string> inputs;
			inputs.reserve(node.inputs.size());
			for (const auto& input : node.inputs)
				inputs.push_back(nested_node_json(input, nodes));
			if (node.operator_id == "query.union.v1")
				std::ranges::sort(inputs,
								  [](const std::string& left, const std::string& right)
								  {
									  const auto left_digest = plain_digest(left);
									  const auto right_digest = plain_digest(right);
									  return left_digest != right_digest
										  ? left_digest < right_digest
										  : left < right;
								  });
			std::ostringstream output;
			output << "{\"arguments\":" << canonical_arguments(node) << ",\"inputs\":[";
			for (std::size_t index = 0U; index < inputs.size(); ++index)
			{
				if (index != 0U)
					output << ',';
				output << inputs[index];
			}
			output << "],\"operator\":" << json_string(node.operator_id) << '}';
			return output.str();
		}

		[[nodiscard]] bool compatible_literal(const value_type& column, const value_type& value)
		{
			return column.scalar == value.scalar && column.parameter == value.parameter;
		}

		[[nodiscard]] std::string literal_comparison_type(const value_type& column)
		{
			auto canonical = column.canonical_name();
			constexpr std::string_view prefix = "optional<";
			if (canonical.starts_with(prefix) && canonical.ends_with('>'))
				return canonical.substr(prefix.size(), canonical.size() - prefix.size() - 1U);
			return canonical;
		}

		void merge_requirements(std::vector<relation_descriptor>& destination,
								const std::span<const relation_descriptor> additions)
		{
			for (const auto& descriptor : additions)
			{
				if (std::ranges::find(destination, descriptor.id, &relation_descriptor::id) ==
					destination.end())
					destination.push_back(descriptor);
			}
			std::ranges::sort(destination, {}, &relation_descriptor::id);
		}

		struct node_shape
		{
			std::map<std::string, value_type, std::less<>> columns;
			bool ordered{};
			std::vector<std::string> order_keys;
		};

		void collect_columns(const decoded_predicate& predicate,
							 std::vector<ir_column_ref>& columns)
		{
			if (predicate.column)
				columns.push_back(*predicate.column);
			if (predicate.left)
				columns.push_back(*predicate.left);
			if (predicate.right)
				columns.push_back(*predicate.right);
			for (const auto& operand : predicate.operands)
				collect_columns(operand, columns);
		}
	} // namespace

	namespace detail
	{
		std::string
		canonical_subtree_form(const std::string_view node_id,
							   const std::map<std::string, const ir_node*, std::less<>>& nodes)
		{
			return nested_node_json(node_id, nodes);
		}

		std::string
		canonical_subtree_digest(const std::string_view node_id,
								 const std::map<std::string, const ir_node*, std::less<>>& nodes)
		{
			return plain_digest(nested_node_json(node_id, nodes));
		}
	} // namespace detail

	literal literal::boolean(const bool value)
	{
		return {detached_cell::boolean(value)};
	}
	literal literal::signed_integer(const std::int64_t value)
	{
		return {detached_cell::signed_integer(value)};
	}
	literal literal::unsigned_integer(const std::uint64_t value)
	{
		return {detached_cell::unsigned_integer(value)};
	}
	literal literal::utf8(std::string value)
	{
		return {detached_cell::utf8(std::move(value))};
	}
	literal literal::typed(std::string type, std::string value)
	{
		return {detached_cell::typed(std::move(type), std::move(value))};
	}
	literal literal::exact(value_type type, scalar_value value)
	{
		type.optional = false;
		return {{std::move(type), cell_state::present, std::move(value), std::nullopt}};
	}

	result<expression> equals_present(column_ref column, const literal& value)
	{
		if (auto valid = value.cell.validate(); !valid)
			return cxxlens::sdk::unexpected(std::move(valid.error()));
		if (value.cell.state != cell_state::present ||
			!compatible_literal(column.type, value.cell.type))
			return cxxlens::sdk::unexpected(query_error("sdk.query-literal-type-mismatch",
														column.column_id,
														value.cell.type.canonical_name()));
		return expression{"{\"column\":" + column_json(column) +
							  R"(,"kind":"equals_present","literal":)" + literal_json(value.cell) +
							  "}",
						  {std::move(column)}};
	}

	result<expression> equals_present(column_ref left, column_ref right)
	{
		if (!compatible_literal(left.type, right.type))
			return cxxlens::sdk::unexpected(
				query_error("sdk.query-column-type-mismatch", left.column_id, right.column_id));
		return expression{R"({"kind":"column_equals_present","left":)" + column_json(left) +
							  ",\"right\":" + column_json(right) + "}",
						  {std::move(left), std::move(right)}};
	}

	expression is_present(column_ref column)
	{
		return {"{\"column\":" + column_json(column) + R"(,"kind":"is_present"})",
				{std::move(column)}};
	}
	expression is_absent(column_ref column)
	{
		return {"{\"column\":" + column_json(column) + R"(,"kind":"is_absent"})",
				{std::move(column)}};
	}
	expression is_unknown(column_ref column)
	{
		return {"{\"column\":" + column_json(column) + R"(,"kind":"is_unknown"})",
				{std::move(column)}};
	}

	result<expression> all(const std::span<const expression> expressions)
	{
		if (expressions.empty())
			return cxxlens::sdk::unexpected(query_error("sdk.query-empty-expression", "all"));
		std::vector<std::string> children;
		std::vector<column_ref> columns;
		for (const auto& value : expressions)
		{
			if (value.canonical.empty())
				return cxxlens::sdk::unexpected(query_error("sdk.query-empty-expression", "all"));
			children.push_back(value.canonical);
			columns.insert(
				columns.end(), value.referenced_columns.begin(), value.referenced_columns.end());
		}
		std::ranges::sort(children,
						  [](const std::string& left, const std::string& right)
						  {
							  return plain_digest(left) < plain_digest(right);
						  });
		std::ranges::sort(columns, {}, &column_ref::column_id);
		columns.erase(std::ranges::unique(columns).begin(), columns.end());
		std::ostringstream output;
		output << R"({"kind":"and","operands":[)";
		for (std::size_t index = 0U; index < children.size(); ++index)
		{
			if (index != 0U)
				output << ',';
			output << children[index];
		}
		output << "]}";
		return expression{output.str(), std::move(columns)};
	}

	result<expression> any(const std::span<const expression> expressions)
	{
		auto combined = all(expressions);
		if (!combined)
			return combined;
		combined->canonical.replace(9U, 3U, "or");
		return combined;
	}

	result<void> logical_query_ir::validate() const
	{
		if (version != semantic_version{1U, 0U, 0U} || relation_requirements.empty() ||
			nodes.empty() || root.empty() || output_schema.empty())
			return cxxlens::sdk::unexpected(query_error("sdk.query-ir-invalid", "structure"));
		std::map<std::string, const relation_descriptor*, std::less<>> descriptors;
		for (const auto& descriptor : relation_requirements)
		{
			if (auto valid = descriptor.validate(); !valid)
				return cxxlens::sdk::unexpected(std::move(valid.error()));
			if (!descriptors.emplace(descriptor.id, &descriptor).second)
				return cxxlens::sdk::unexpected(
					query_error("sdk.query-duplicate-relation", descriptor.id));
		}
		std::set<std::string> node_ids;
		std::set<std::string, std::less<>> scanned_descriptors;
		std::map<std::string, node_shape, std::less<>> shapes;
		std::map<std::string, column_descriptor, std::less<>> columns;
		for (const auto& descriptor : relation_requirements)
			for (const auto& column : descriptor.columns)
				columns.emplace(column.id, column);
		static const std::map<std::string, std::size_t, std::less<>> arities{
			{"query.scan.v1", 0U},
			{"query.filter.v1", 1U},
			{"query.project.v1", 1U},
			{"query.inner_join.v1", 2U},
			{"query.semi_join.v1", 2U},
			{"query.union.v1", 2U},
			{"query.distinct.v1", 1U},
			{"query.order_by.v1", 1U},
			{"query.limit.v1", 1U},
			{"query.condition_restrict.v1", 1U},
			{"query.interpretation_restrict.v1", 1U},
		};
		for (const auto& node : nodes)
		{
			for (const auto& input : node.inputs)
				if (!node_ids.contains(input))
					return cxxlens::sdk::unexpected(
						query_error("sdk.query-nontopological-node", node.id, input));
			const auto arity = arities.find(node.operator_id);
			if (node.id.empty() || arity == arities.end() || node.inputs.size() != arity->second ||
				!node_ids.insert(node.id).second)
				return cxxlens::sdk::unexpected(query_error("sdk.query-node-invalid", node.id));
			auto arguments = decode_arguments(node);
			if (!arguments)
				return cxxlens::sdk::unexpected(std::move(arguments.error()));
			std::vector<node_shape> inputs;
			inputs.reserve(node.inputs.size());
			for (const auto& input : node.inputs)
				inputs.push_back(shapes.at(input));
			node_shape shape;
			if (node.operator_id == "query.scan.v1")
			{
				const auto& scan = std::get<scan_arguments>(*arguments);
				const auto descriptor = std::ranges::find(
					relation_requirements, scan.descriptor_id, &relation_descriptor::id);
				if (descriptor == relation_requirements.end())
					return cxxlens::sdk::unexpected(
						query_error("sdk.query-scan-requirement-missing", scan.descriptor_id));
				scanned_descriptors.insert(scan.descriptor_id);
				for (const auto& column : descriptor->columns)
					shape.columns.emplace(column.id, column.type);
			}
			else
			{
				shape = inputs.front();
				if (const auto* predicate = std::get_if<predicate_arguments>(&*arguments))
				{
					if ((node.operator_id == "query.inner_join.v1" ||
						 node.operator_id == "query.semi_join.v1") &&
						predicate->predicate.kind != predicate_kind::column_equals_present)
						return cxxlens::sdk::unexpected(
							query_error("sdk.query-join-present-equality-required", node.id));
					if (node.operator_id == "query.filter.v1" &&
						predicate->predicate.kind == predicate_kind::column_equals_present)
						return cxxlens::sdk::unexpected(
							query_error("sdk.query-filter-join-predicate", node.id));
					std::vector<ir_column_ref> references;
					collect_columns(predicate->predicate, references);
					auto available = inputs.front().columns;
					if (inputs.size() == 2U)
						available.insert(inputs[1].columns.begin(), inputs[1].columns.end());
					for (const auto& reference : references)
						if (!available.contains(reference.column_id) &&
							reference.availability == column_availability::require)
							return cxxlens::sdk::unexpected(
								query_error("sdk.query-column-not-in-input", reference.column_id));
					if (predicate->predicate.literal_value && predicate->predicate.column)
					{
						const auto found = columns.find(predicate->predicate.column->column_id);
						if (found == columns.end() ||
							literal_comparison_type(found->second.type) !=
								predicate->predicate.literal_value->type)
							return cxxlens::sdk::unexpected(
								query_error("sdk.query-literal-type-mismatch",
											predicate->predicate.column->column_id));
					}
				}
				if (node.operator_id == "query.inner_join.v1" ||
					node.operator_id == "query.semi_join.v1")
				{
					if (node.operator_id == "query.inner_join.v1")
						shape.columns.insert(inputs[1].columns.begin(), inputs[1].columns.end());
					shape.ordered =
						node.operator_id == "query.semi_join.v1" && inputs.front().ordered;
					if (shape.ordered)
						shape.order_keys = inputs.front().order_keys;
					else
						shape.order_keys.clear();
				}
				else if (node.operator_id == "query.union.v1")
				{
					if (inputs.front().columns != inputs[1].columns)
						return cxxlens::sdk::unexpected(
							query_error("sdk.query-union-schema-mismatch", node.id));
					shape.ordered = false;
					shape.order_keys.clear();
				}
				else if (node.operator_id == "query.distinct.v1")
				{
					shape.ordered = false;
					shape.order_keys.clear();
				}
				else if (node.operator_id == "query.project.v1")
				{
					const auto& project = std::get<project_arguments>(*arguments);
					node_shape projected;
					std::map<std::string, std::string, std::less<>> mapping;
					for (const auto& item : project.columns)
					{
						if (!inputs.front().columns.contains(item.column.column_id) &&
							item.column.availability == column_availability::require)
							return cxxlens::sdk::unexpected(query_error(
								"sdk.query-column-not-in-input", item.column.column_id));
						const auto source = inputs.front().columns.find(item.column.column_id);
						if (source == inputs.front().columns.end())
							return cxxlens::sdk::unexpected(query_error(
								"sdk.query-output-schema-mismatch", item.column.column_id));
						if (!projected.columns.emplace(item.output, source->second).second)
							return cxxlens::sdk::unexpected(
								query_error("sdk.query-output-schema-mismatch", item.output));
						mapping.emplace(item.column.column_id, item.output);
					}
					projected.ordered = inputs.front().ordered &&
						std::ranges::all_of(inputs.front().order_keys,
											[&](const std::string& key)
											{
												return mapping.contains(key);
											});
					if (projected.ordered)
						for (const auto& key : inputs.front().order_keys)
							projected.order_keys.push_back(mapping.at(key));
					shape = std::move(projected);
				}
				else if (node.operator_id == "query.order_by.v1")
				{
					const auto& order = std::get<order_arguments>(*arguments);
					shape.ordered = true;
					shape.order_keys.clear();
					for (const auto& key : order.keys)
					{
						if (!shape.columns.contains(key.column.column_id))
							return cxxlens::sdk::unexpected(
								query_error("sdk.query-column-not-in-input", key.column.column_id));
						shape.order_keys.push_back(key.column.column_id);
					}
				}
				else if (node.operator_id == "query.limit.v1" && !inputs.front().ordered)
					return cxxlens::sdk::unexpected(
						query_error("sdk.query-limit-unordered", node.id));
			}
			shapes.emplace(node.id, std::move(shape));
		}
		if (!node_ids.contains(root))
			return cxxlens::sdk::unexpected(query_error("sdk.query-root-missing", root));
		std::map<std::string, const ir_node*, std::less<>> node_index;
		for (const auto& node : nodes)
			node_index.emplace(node.id, &node);
		std::set<std::string, std::less<>> reachable;
		std::vector<std::string> pending{root};
		while (!pending.empty())
		{
			auto current = std::move(pending.back());
			pending.pop_back();
			if (!reachable.insert(current).second)
				continue;
			const auto* node = node_index.at(current);
			pending.insert(pending.end(), node->inputs.begin(), node->inputs.end());
		}
		if (reachable.size() != nodes.size())
		{
			const auto unreachable = std::ranges::find_if(nodes,
														  [&](const ir_node& node)
														  {
															  return !reachable.contains(node.id);
														  });
			return cxxlens::sdk::unexpected(
				query_error("sdk.query-unreachable-node", unreachable->id));
		}
		for (const auto& column : output_schema)
		{
			const auto descriptor = descriptors.find(column.descriptor_id);
			if (descriptor == descriptors.end())
				return cxxlens::sdk::unexpected(
					query_error("sdk.query-output-column-foreign", column.column_id));
			const auto declared = descriptor->second->column(column.column_id);
			if (!declared || declared->type != column.type)
				return cxxlens::sdk::unexpected(
					query_error("sdk.query-output-schema-mismatch", column.column_id));
		}
		const bool projected =
			std::ranges::any_of(nodes,
								[](const ir_node& node)
								{
									return node.operator_id == "query.project.v1";
								});
		std::map<std::string, value_type, std::less<>> expected;
		if (projected)
		{
			const auto aliases = detail::output_aliases(output_schema);
			for (std::size_t index = 0U; index < output_schema.size(); ++index)
				if (!expected.emplace("output." + aliases[index], output_schema[index].type).second)
					return cxxlens::sdk::unexpected(query_error("sdk.query-output-schema-mismatch",
																output_schema[index].column_id));
		}
		else
			for (const auto& column : output_schema)
				if (!expected.emplace(column.column_id, column.type).second)
					return cxxlens::sdk::unexpected(
						query_error("sdk.query-output-schema-mismatch", column.column_id));
		if (shapes.at(root).columns != expected)
			return cxxlens::sdk::unexpected(query_error("sdk.query-output-schema-mismatch", root));
		for (const auto& descriptor : relation_requirements)
			if (!scanned_descriptors.contains(descriptor.id))
				return cxxlens::sdk::unexpected(
					query_error("sdk.query-unused-relation-requirement", descriptor.id));
		return {};
	}

	std::string logical_query_ir::canonical_form() const
	{
		auto descriptors = relation_requirements;
		std::ranges::sort(descriptors, {}, &relation_descriptor::id);
		std::map<std::string, const ir_node*, std::less<>> node_index;
		for (const auto& node : nodes)
			node_index.emplace(node.id, &node);
		auto root_json = nested_node_json(root, node_index);
		if (std::ranges::none_of(nodes,
								 [](const ir_node& node)
								 {
									 return node.operator_id == "query.project.v1";
								 }))
			root_json = R"({"arguments":{"columns":)" + projections_json(output_schema) +
				"},\"inputs\":[" + root_json + R"(],"operator":"query.project.v1"})";
		const auto aliases = detail::output_aliases(output_schema);
		std::ostringstream output;
		output << "{\"condition_policy\":{\"empty_intersection\":\"discard\","
				  "\"require_same_universe\":true},\"interpretation_policy\":{"
				  "\"join_compatibility\":\"exact-id-equality\"},\"ir_version\":\""
			   << version.string() << R"(","output_schema":[)";
		for (std::size_t index = 0U; index < output_schema.size(); ++index)
		{
			if (index != 0U)
				output << ',';
			const auto& column = output_schema[index];
			output << R"({"id":"output.)" << aliases[index] << R"(","required":)"
				   << (column.type.optional ? "false" : "true")
				   << ",\"type\":" << json_string(column.type.canonical_name()) << '}';
		}
		output << "],\"relation_requirements\":[";
		for (std::size_t index = 0U; index < descriptors.size(); ++index)
		{
			if (index != 0U)
				output << ',';
			output << "{\"descriptor_id\":" << json_string(descriptors[index].id)
				   << R"(,"maximum_minor":"any-compatible","minimum_minor":)"
				   << descriptors[index].version.minor << '}';
		}
		output << "],\"root\":" << root_json << R"(,"schema":"cxxlens.logical-query-ir.v1"})";
		return output.str();
	}

	std::string logical_query_ir::digest() const
	{
		return *semantic_digest("cxxlens.logical-query-ir.v1", canonical_form());
	}

	builder::builder(logical_query_ir ir) : ir_{std::move(ir)} {}

	result<builder> builder::from(const relation_descriptor& descriptor,
								  const std::string_view alias)
	{
		if (auto valid = descriptor.validate(); !valid)
			return cxxlens::sdk::unexpected(std::move(valid.error()));
		const auto resolved_alias = alias.empty() ? default_alias(descriptor) : std::string{alias};
		if (!valid_identifier(resolved_alias))
			return cxxlens::sdk::unexpected(query_error("sdk.query-alias-invalid", "alias"));
		logical_query_ir ir;
		for (const auto& column : descriptor.columns)
			ir.output_schema.push_back({descriptor.id, column.id, column.type});
		ir.relation_requirements.push_back(descriptor);
		ir.nodes.push_back({"n0",
							"query.scan.v1",
							{},
							"{\"alias\":" + json_string(resolved_alias) +
								",\"descriptor_id\":" + json_string(descriptor.id) + "}"});
		ir.root = "n0";
		return builder{std::move(ir)};
	}

	result<void> builder::require_columns(const std::span<const column_ref> columns) const
	{
		for (const auto& column : columns)
		{
			const auto descriptor = std::ranges::find(
				ir_.relation_requirements, column.descriptor_id, &relation_descriptor::id);
			if (descriptor == ir_.relation_requirements.end())
				return cxxlens::sdk::unexpected(
					query_error("sdk.query-foreign-column", column.column_id));
			auto expected = descriptor->column(column.column_id);
			if (!expected || !(expected->type == column.type))
				return cxxlens::sdk::unexpected(
					query_error("sdk.query-column-mismatch", column.column_id));
		}
		return {};
	}

	std::string
	builder::append(std::string operator_id, std::vector<std::string> inputs, std::string arguments)
	{
		const auto id = "n" + std::to_string(ir_.nodes.size());
		ir_.nodes.push_back({id, std::move(operator_id), std::move(inputs), std::move(arguments)});
		ir_.root = id;
		return id;
	}

	result<builder> builder::where(expression predicate) &&
	{
		if (projected_)
			return cxxlens::sdk::unexpected(query_error("sdk.query-project-terminal", "where"));
		if (auto valid = require_columns(predicate.referenced_columns); !valid)
			return cxxlens::sdk::unexpected(std::move(valid.error()));
		(void)append("query.filter.v1",
					 {ir_.root},
					 "{\"predicate\":" + std::move(predicate.canonical) + "}");
		return std::move(*this);
	}

	result<builder> builder::project(const std::span<const column_ref> columns) &&
	{
		if (projected_)
			return cxxlens::sdk::unexpected(query_error("sdk.query-project-terminal", "project"));
		if (columns.empty())
			return cxxlens::sdk::unexpected(query_error("sdk.query-empty-projection", "columns"));
		if (auto valid = require_columns(columns); !valid)
			return cxxlens::sdk::unexpected(std::move(valid.error()));
		(void)append(
			"query.project.v1", {ir_.root}, "{\"columns\":" + projections_json(columns) + "}");
		ir_.output_schema.assign(columns.begin(), columns.end());
		projected_ = true;
		if (total_ordered_)
			for (const auto& key : order_keys_)
				if (std::ranges::find(columns, key, &column_ref::column_id) == columns.end())
				{
					total_ordered_ = false;
					order_keys_.clear();
					break;
				}
		return std::move(*this);
	}

	result<builder> builder::inner_join(builder right, expression predicate) &&
	{
		if (projected_ || right.projected_)
			return cxxlens::sdk::unexpected(
				query_error("sdk.query-project-terminal", "inner_join"));
		const auto left_root = ir_.root;
		merge_requirements(ir_.relation_requirements, right.ir_.relation_requirements);
		std::map<std::string, std::string, std::less<>> remap;
		for (const auto& node : right.ir_.nodes)
		{
			std::vector<std::string> inputs;
			inputs.reserve(node.inputs.size());
			for (const auto& input : node.inputs)
				inputs.push_back(remap.at(input));
			remap.emplace(node.id, append(node.operator_id, std::move(inputs), node.arguments));
		}
		if (auto valid = require_columns(predicate.referenced_columns); !valid)
			return cxxlens::sdk::unexpected(std::move(valid.error()));
		const auto right_root = remap.at(right.ir_.root);
		(void)append("query.inner_join.v1",
					 {left_root, right_root},
					 "{\"predicate\":" + std::move(predicate.canonical) + "}");
		ir_.output_schema.insert(ir_.output_schema.end(),
								 right.ir_.output_schema.begin(),
								 right.ir_.output_schema.end());
		total_ordered_ = false;
		order_keys_.clear();
		return std::move(*this);
	}

	result<builder> builder::semi_join(builder right, expression predicate) &&
	{
		const auto left_schema = ir_.output_schema;
		auto joined = std::move(*this).inner_join(std::move(right), std::move(predicate));
		if (!joined)
			return joined;
		joined->ir_.nodes.back().operator_id = "query.semi_join.v1";
		joined->ir_.output_schema = left_schema;
		return joined;
	}

	result<builder> builder::union_with(const builder& right) &&
	{
		if (projected_ != right.projected_)
			return cxxlens::sdk::unexpected(
				query_error("sdk.query-union-schema-mismatch", "projection_state"));
		if (ir_.output_schema != right.ir_.output_schema)
			return cxxlens::sdk::unexpected(
				query_error("sdk.query-union-schema-mismatch", "output_schema"));
		const auto left_root = ir_.root;
		merge_requirements(ir_.relation_requirements, right.ir_.relation_requirements);
		std::map<std::string, std::string, std::less<>> remap;
		for (const auto& node : right.ir_.nodes)
		{
			std::vector<std::string> inputs;
			inputs.reserve(node.inputs.size());
			for (const auto& input : node.inputs)
				inputs.push_back(remap.at(input));
			remap.emplace(node.id, append(node.operator_id, std::move(inputs), node.arguments));
		}
		(void)append("query.union.v1", {left_root, remap.at(right.ir_.root)}, "{}");
		total_ordered_ = false;
		order_keys_.clear();
		return std::move(*this);
	}

	result<builder> builder::distinct() &&
	{
		(void)append("query.distinct.v1", {ir_.root}, "{}");
		total_ordered_ = false;
		order_keys_.clear();
		return std::move(*this);
	}

	result<builder> builder::order_by(const std::span<const column_ref> columns) &&
	{
		if (projected_)
			return cxxlens::sdk::unexpected(query_error("sdk.query-project-terminal", "order_by"));
		if (columns.empty())
			return cxxlens::sdk::unexpected(query_error("sdk.query-empty-order", "columns"));
		if (auto valid = require_columns(columns); !valid)
			return cxxlens::sdk::unexpected(std::move(valid.error()));
		(void)append(
			"query.order_by.v1", {ir_.root}, "{\"keys\":" + order_keys_json(columns) + "}");
		total_ordered_ = true;
		order_keys_.clear();
		for (const auto& column : columns)
			order_keys_.push_back(column.column_id);
		return std::move(*this);
	}

	result<builder> builder::limit(const std::uint64_t count) &&
	{
		if (!total_ordered_)
			return cxxlens::sdk::unexpected(query_error("sdk.query-limit-requires-order", "limit"));
		(void)append("query.limit.v1", {ir_.root}, "{\"count\":" + std::to_string(count) + "}");
		return std::move(*this);
	}

	result<builder> builder::condition_restrict(const std::string_view universe,
												const std::span<const std::string> alternatives) &&
	{
		if (universe.empty() || alternatives.empty())
			return cxxlens::sdk::unexpected(
				query_error("sdk.query-condition-missing", "condition"));
		auto ordered = std::vector<std::string>{alternatives.begin(), alternatives.end()};
		if (std::ranges::any_of(ordered, &std::string::empty))
			return cxxlens::sdk::unexpected(
				query_error("sdk.query-condition-missing", "alternative"));
		std::ranges::sort(ordered);
		ordered.erase(std::ranges::unique(ordered).begin(), ordered.end());
		std::ostringstream arguments;
		arguments << "{\"alternatives\":[";
		for (std::size_t index = 0U; index < ordered.size(); ++index)
		{
			if (index != 0U)
				arguments << ',';
			arguments << json_string(ordered[index]);
		}
		arguments << "],\"universe\":" << json_string(universe) << '}';
		(void)append("query.condition_restrict.v1", {ir_.root}, arguments.str());
		return std::move(*this);
	}

	result<builder> builder::interpretation_restrict(const std::string_view interpretation) &&
	{
		if (interpretation.empty())
			return cxxlens::sdk::unexpected(
				query_error("sdk.query-interpretation-missing", "interpretation"));
		(void)append("query.interpretation_restrict.v1",
					 {ir_.root},
					 "{\"interpretation\":" + json_string(interpretation) + "}");
		return std::move(*this);
	}

	const logical_query_ir& builder::ir() const noexcept
	{
		return ir_;
	}

	logical_query_ir builder::finish() &&
	{
		return std::move(ir_);
	}
} // namespace cxxlens::sdk::query
