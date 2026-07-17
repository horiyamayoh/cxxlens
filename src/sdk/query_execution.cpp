#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <cxxlens/sdk/query.hpp>

#include "claim_internal.hpp"
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
						static constexpr std::string_view digits{"0123456789abcdef"};
						std::string output{"\""};
						output.reserve(item.size() * 2U + 2U);
						for (const auto byte : item)
						{
							const auto value = std::to_integer<unsigned char>(byte);
							output.push_back(digits[value >> 4U]);
							output.push_back(digits[value & 0x0fU]);
						}
						output.push_back('"');
						return output;
					}
				},
				value);
		}

		[[nodiscard]] std::string cell_json(const detached_cell& cell)
		{
			switch (cell.state)
			{
				case cell_state::present:
					return R"({"state":"present","type":)" +
						json_string(cell.type.canonical_name()) +
						",\"value\":" + scalar_json(*cell.value) + "}";
				case cell_state::absent:
					return R"({"state":"absent"})";
				case cell_state::unknown:
					return "{\"reason\":" + json_string(*cell.unknown_reason) +
						R"(,"state":"unknown"})";
			}
			return "{}";
		}

		[[nodiscard]] std::string strings_json(const std::span<const std::string> values)
		{
			std::ostringstream output;
			output << '[';
			for (std::size_t index = 0U; index < values.size(); ++index)
			{
				if (index != 0U)
					output << ',';
				output << json_string(values[index]);
			}
			output << ']';
			return output.str();
		}

		template <class T>
		[[nodiscard]] bool sorted_unique(const std::vector<T>& values)
		{
			return std::ranges::is_sorted(values) &&
				std::ranges::adjacent_find(values) == values.end();
		}

		template <class T>
		void canonical_set(std::vector<T>& values)
		{
			std::ranges::sort(values);
			values.erase(std::ranges::unique(values).begin(), values.end());
		}

		struct canonical_execution_plan
		{
			std::vector<const ir_node*> nodes;
			std::map<std::string, std::string, std::less<>> stable_ids;
			std::map<std::string, std::vector<std::string>, std::less<>> inputs;
		};

		[[nodiscard]] std::vector<std::string>
		canonical_inputs(const ir_node& node,
						 const std::map<std::string, const ir_node*, std::less<>>& nodes)
		{
			auto inputs = node.inputs;
			if (node.operator_id != "query.union.v1")
				return inputs;
			std::ranges::sort(inputs,
							  [&](const std::string& left, const std::string& right)
							  {
								  const auto left_digest =
									  detail::canonical_subtree_digest(left, nodes);
								  const auto right_digest =
									  detail::canonical_subtree_digest(right, nodes);
								  if (left_digest != right_digest)
									  return left_digest < right_digest;
								  return detail::canonical_subtree_form(left, nodes) <
									  detail::canonical_subtree_form(right, nodes);
							  });
			return inputs;
		}

		void append_canonical_execution_node(
			const std::string& node_id,
			const std::map<std::string, const ir_node*, std::less<>>& index,
			std::set<std::string, std::less<>>& visited,
			canonical_execution_plan& plan)
		{
			if (visited.contains(node_id))
				return;
			const auto& node = *index.at(node_id);
			auto inputs = canonical_inputs(node, index);
			for (const auto& input : inputs)
				append_canonical_execution_node(input, index, visited, plan);
			visited.insert(node_id);
			plan.inputs.emplace(node_id, std::move(inputs));
			plan.stable_ids.emplace(node_id, "n" + std::to_string(plan.nodes.size()));
			plan.nodes.push_back(&node);
		}

		canonical_execution_plan make_canonical_execution_plan(const logical_query_ir& query)
		{
			std::map<std::string, const ir_node*, std::less<>> index;
			for (const auto& node : query.nodes)
				index.emplace(node.id, &node);
			canonical_execution_plan plan;
			std::set<std::string, std::less<>> visited;
			append_canonical_execution_node(query.root, index, visited, plan);
			return plan;
		}

		struct schema_column_access
		{
			std::string descriptor_id;
			column_descriptor expected;
			column_availability availability{column_availability::absent_if_schema_missing};
		};

		void collect_schema_references(const decoded_predicate& predicate,
									   std::vector<ir_column_ref>& output)
		{
			if (predicate.column)
				output.push_back(*predicate.column);
			if (predicate.left)
				output.push_back(*predicate.left);
			if (predicate.right)
				output.push_back(*predicate.right);
			for (const auto& operand : predicate.operands)
				collect_schema_references(operand, output);
		}

		[[nodiscard]] result<std::map<std::string, schema_column_access, std::less<>>>
		make_schema_compatibility_plan(const logical_query_ir& query,
									   const bool implicit_terminal_projection)
		{
			std::map<std::string, schema_column_access, std::less<>> plan;
			auto add = [&](const ir_column_ref& reference) -> result<void>
			{
				const relation_descriptor* owner{};
				const column_descriptor* expected{};
				for (const auto& descriptor : query.relation_requirements)
				{
					const auto column = std::ranges::find(
						descriptor.columns, reference.column_id, &column_descriptor::id);
					if (column == descriptor.columns.end())
						continue;
					if (owner != nullptr)
						return unexpected(
							query_error("sdk.query-snapshot-schema-mismatch", reference.column_id));
					owner = &descriptor;
					expected = &*column;
				}
				if (owner == nullptr || expected == nullptr)
					return unexpected(
						query_error("sdk.query-snapshot-schema-mismatch", reference.column_id));
				auto [position, inserted] = plan.emplace(
					reference.column_id,
					schema_column_access{owner->id, *expected, reference.availability});
				if (!inserted && reference.availability == column_availability::require)
					position->second.availability = column_availability::require;
				return {};
			};

			for (const auto& node : query.nodes)
			{
				auto arguments = decode_arguments(node);
				if (!arguments)
					return unexpected(std::move(arguments.error()));
				std::vector<ir_column_ref> references;
				if (const auto* predicate = std::get_if<predicate_arguments>(&*arguments))
					collect_schema_references(predicate->predicate, references);
				else if (const auto* project = std::get_if<project_arguments>(&*arguments))
					for (const auto& item : project->columns)
						references.push_back(item.column);
				else if (const auto* order = std::get_if<order_arguments>(&*arguments))
					for (const auto& key : order->keys)
						references.push_back(key.column);
				for (const auto& reference : references)
					if (auto added = add(reference); !added)
						return unexpected(std::move(added.error()));
			}
			if (implicit_terminal_projection)
				for (const auto& column : query.output_schema)
					if (auto added = add({column.column_id, column_availability::require}); !added)
						return unexpected(std::move(added.error()));
			return plan;
		}

		[[nodiscard]] std::string guarantee_json(const claim_guarantee& value)
		{
			return "{\"approximation\":" + json_string(value.approximation) +
				",\"assumptions\":" + json_string(value.assumptions) +
				",\"scope\":" + json_string(value.scope) +
				",\"verification_modalities\":" + strings_json(value.verification_modalities) + "}";
		}

		[[nodiscard]] std::string guarantee_key(const claim_guarantee& value)
		{
			return guarantee_json(value);
		}

		void canonical_guarantees(std::vector<claim_guarantee>& values)
		{
			std::ranges::sort(values,
							  [](const claim_guarantee& left, const claim_guarantee& right)
							  {
								  return guarantee_key(left) < guarantee_key(right);
							  });
			values.erase(std::unique(values.begin(),
									 values.end(),
									 [](const claim_guarantee& left, const claim_guarantee& right)
									 {
										 return guarantee_key(left) == guarantee_key(right);
									 }),
						 values.end());
		}

		[[nodiscard]] std::string producer_key(const claim_producer& value)
		{
			return value.id + "\n" + value.semantic_contract;
		}

		void canonical_producers(std::vector<claim_producer>& values)
		{
			std::ranges::sort(values,
							  [](const claim_producer& left, const claim_producer& right)
							  {
								  return producer_key(left) < producer_key(right);
							  });
			values.erase(std::unique(values.begin(),
									 values.end(),
									 [](const claim_producer& left, const claim_producer& right)
									 {
										 return producer_key(left) == producer_key(right);
									 }),
						 values.end());
		}

		void canonical_contributor_edges(std::vector<query_contributor_edge>& values)
		{
			std::ranges::sort(
				values,
				[](const query_contributor_edge& left, const query_contributor_edge& right)
				{
					return left.canonical_form() < right.canonical_form();
				});
			values.erase(std::ranges::unique(values,
											 [](const query_contributor_edge& left,
												const query_contributor_edge& right)
											 {
												 return left.canonical_form() ==
													 right.canonical_form();
											 })
							 .begin(),
						 values.end());
		}

		[[nodiscard]] bool
		contributor_edges_are_canonical(const std::vector<query_contributor_edge>& values)
		{
			return std::ranges::is_sorted(
					   values,
					   [](const query_contributor_edge& left, const query_contributor_edge& right)
					   {
						   return left.canonical_form() < right.canonical_form();
					   }) &&
				std::ranges::adjacent_find(
					values,
					[](const query_contributor_edge& left, const query_contributor_edge& right)
					{
						return left.canonical_form() == right.canonical_form();
					}) == values.end();
		}

		void refresh_contributor_projections(annotated_row& row)
		{
			row.claim_contributors.clear();
			row.producer_contracts.clear();
			row.provenance.clear();
			row.contributor_guarantees.clear();
			for (const auto& edge : row.contributor_edges)
			{
				row.claim_contributors.push_back(edge.claim_contributor);
				row.producer_contracts.push_back(edge.producer);
				row.provenance.push_back(edge.provenance);
				row.contributor_guarantees.push_back(edge.guarantee);
			}
			canonical_set(row.claim_contributors);
			canonical_producers(row.producer_contracts);
			canonical_set(row.provenance);
			canonical_guarantees(row.contributor_guarantees);
		}

		void canonicalize_contributor_edges(annotated_row& row)
		{
			canonical_contributor_edges(row.contributor_edges);
			refresh_contributor_projections(row);
		}

		void bind_contributor_condition(annotated_row& row, const claim_condition& condition)
		{
			for (auto& edge : row.contributor_edges)
				edge.condition = condition;
			canonicalize_contributor_edges(row);
		}

		[[nodiscard]] bool producers_are_canonical(const std::vector<claim_producer>& values)
		{
			return std::ranges::is_sorted(
					   values,
					   [](const claim_producer& left, const claim_producer& right)
					   {
						   return producer_key(left) < producer_key(right);
					   }) &&
				std::ranges::adjacent_find(
					values,
					[](const claim_producer& left, const claim_producer& right)
					{
						return producer_key(left) == producer_key(right);
					}) == values.end();
		}

		[[nodiscard]] detached_cell absent_cell(const value_type& type)
		{
			return detached_cell::absent(type);
		}

		[[nodiscard]] std::optional<std::string>
		resolve_occurrence(const std::map<std::string, value_type, std::less<>>& column_types,
						   const ir_column_ref& reference)
		{
			if (!reference.source_alias.empty())
			{
				auto key =
					detail::occurrence_column_id(reference.source_alias, reference.column_id);
				return column_types.contains(key) ? std::optional<std::string>{std::move(key)}
												  : std::nullopt;
			}
			if (column_types.contains(reference.column_id))
				return reference.column_id;
			std::optional<std::string> match;
			const auto suffix = std::string{detail::occurrence_separator} + reference.column_id;
			for (const auto& [key, type] : column_types)
			{
				(void)type;
				if (!key.ends_with(suffix))
					continue;
				if (match)
					return std::nullopt;
				match = key;
			}
			return match;
		}

		[[nodiscard]] detached_cell
		cell_for(const annotated_row& row,
				 const ir_column_ref& reference,
				 const std::map<std::string, value_type, std::less<>>& column_types)
		{
			const auto column = resolve_occurrence(column_types, reference);
			if (!column)
				return detached_cell::absent({scalar_kind::utf8_string, {}, true});
			const auto found = row.values.find(*column);
			if (found != row.values.end())
				return found->second;
			const auto type = column_types.find(*column);
			if (type != column_types.end())
				return absent_cell(type->second);
			return detached_cell::absent({scalar_kind::utf8_string, {}, true});
		}

		[[nodiscard]] bool present_equal(const detached_cell& left, const detached_cell& right)
		{
			if (left.state != cell_state::present || right.state != cell_state::present ||
				left.type.scalar != right.type.scalar ||
				left.type.parameter != right.type.parameter || !left.value || !right.value)
				return false;
			return *left.value == *right.value;
		}

		[[nodiscard]] detached_cell literal_cell(const ir_literal& literal)
		{
			value_type type;
			if (literal.type == "bool")
				type.scalar = scalar_kind::boolean;
			else if (literal.type == "int64")
				type.scalar = scalar_kind::signed_integer;
			else if (literal.type == "uint64")
				type.scalar = scalar_kind::unsigned_integer;
			else if (literal.type == "bytes")
				type.scalar = scalar_kind::bytes;
			else
			{
				static const std::map<std::string, scalar_kind, std::less<>> names{
					{"closed_symbol", scalar_kind::closed_symbol},
					{"condition_ref", scalar_kind::condition_ref},
					{"digest", scalar_kind::digest},
					{"evidence_id", scalar_kind::evidence_id},
					{"open_symbol", scalar_kind::open_symbol},
					{"semantic_version", scalar_kind::semantic_version},
					{"set", scalar_kind::set},
					{"source_span_id", scalar_kind::source_span_id},
					{"typed_id", scalar_kind::typed_id},
					{"utf8_string", scalar_kind::utf8_string},
				};
				auto base = literal.type;
				const auto open = base.find('<');
				if (open != std::string::npos && base.back() == '>')
				{
					type.parameter = base.substr(open + 1U, base.size() - open - 2U);
					base.resize(open);
				}
				const auto found = names.find(base);
				type.scalar = found == names.end() ? scalar_kind::utf8_string : found->second;
			}
			return {std::move(type), cell_state::present, literal.value, std::nullopt};
		}

		[[nodiscard]] bool
		evaluate_predicate(const annotated_row& row,
						   const decoded_predicate& predicate,
						   const std::map<std::string, value_type, std::less<>>& column_types)
		{
			switch (predicate.kind)
			{
				case predicate_kind::all:
					return std::ranges::all_of(predicate.operands,
											   [&](const decoded_predicate& child)
											   {
												   return evaluate_predicate(
													   row, child, column_types);
											   });
				case predicate_kind::any:
					return std::ranges::any_of(predicate.operands,
											   [&](const decoded_predicate& child)
											   {
												   return evaluate_predicate(
													   row, child, column_types);
											   });
				case predicate_kind::equals_present:
					return present_equal(cell_for(row, *predicate.column, column_types),
										 literal_cell(*predicate.literal_value));
				case predicate_kind::column_equals_present:
					return present_equal(cell_for(row, *predicate.left, column_types),
										 cell_for(row, *predicate.right, column_types));
				case predicate_kind::is_present:
					return cell_for(row, *predicate.column, column_types).state ==
						cell_state::present;
				case predicate_kind::is_absent:
					return cell_for(row, *predicate.column, column_types).state ==
						cell_state::absent;
				case predicate_kind::is_unknown:
					return cell_for(row, *predicate.column, column_types).state ==
						cell_state::unknown;
			}
			return false;
		}

		[[nodiscard]] result<std::optional<claim_condition>>
		intersect_condition(const claim_condition& left, const claim_condition& right)
		{
			auto overlap = left.overlap(right);
			if (!overlap)
				return unexpected(std::move(overlap.error()));
			if (overlap->empty())
				return std::optional<claim_condition>{};
			return std::optional<claim_condition>{
				claim_condition{left.universe, std::move(*overlap)}};
		}

		[[nodiscard]] result<std::optional<annotated_row>> combine(const annotated_row& left,
																   const annotated_row& right)
		{
			if (left.interpretation != right.interpretation)
				return std::optional<annotated_row>{};
			auto presence = intersect_condition(left.presence, right.presence);
			if (!presence)
				return unexpected(std::move(presence.error()));
			if (!*presence)
				return std::optional<annotated_row>{};
			if (right.multiplicity != 0U &&
				left.multiplicity > std::numeric_limits<std::uint64_t>::max() / right.multiplicity)
				return unexpected(query_error("sdk.query-multiplicity-overflow", "inner_join"));
			annotated_row output = left;
			for (const auto& [column, cell] : right.values)
			{
				const auto found = output.values.find(column);
				if (found != output.values.end() &&
					found->second.canonical_form() != cell.canonical_form())
					return std::optional<annotated_row>{};
				output.values[column] = cell;
			}
			output.multiplicity *= right.multiplicity;
			output.presence = std::move(**presence);
			output.contributor_edges.insert(output.contributor_edges.end(),
											right.contributor_edges.begin(),
											right.contributor_edges.end());
			bind_contributor_condition(output, output.presence);
			return std::optional<annotated_row>{std::move(output)};
		}

		[[nodiscard]] std::string distinct_key(const annotated_row& row)
		{
			std::ostringstream output;
			for (const auto& [column, cell] : row.values)
				output << column << '=' << cell.canonical_form() << '\n';
			output << "universe=" << row.presence.universe << '\n';
			output << "interpretation=" << row.interpretation << '\n';
			return output.str();
		}

		[[nodiscard]] bool annotation_order(const snapshot_claim_annotation* left,
											const snapshot_claim_annotation* right)
		{
			return std::tie(left->row.descriptor_id,
							left->semantic_key,
							left->interpretation,
							left->assertion,
							left->content) < std::tie(right->row.descriptor_id,
													  right->semantic_key,
													  right->interpretation,
													  right->assertion,
													  right->content);
		}

		[[nodiscard]] result<std::vector<differential_disagreement>> disagreements_for(
			const std::vector<snapshot_claim_annotation>& annotations,
			const std::map<std::string, relation_descriptor, std::less<>>& descriptors)
		{
			std::map<std::string, std::vector<const snapshot_claim_annotation*>, std::less<>>
				groups;
			for (const auto& annotation : annotations)
			{
				const auto descriptor = descriptors.find(annotation.row.descriptor_id);
				if (descriptor == descriptors.end() ||
					descriptor->second.merge != merge_mode::functional_assertion)
					continue;
				groups[annotation.row.descriptor_id + "\n" + annotation.semantic_key].push_back(
					&annotation);
			}
			std::vector<differential_disagreement> output;
			for (auto& [key, values] : groups)
			{
				(void)key;
				std::ranges::sort(values, annotation_order);
				const auto descriptor = descriptors.find(values.front()->row.descriptor_id);
				for (std::size_t left = 0U; left < values.size(); ++left)
					for (std::size_t right = left + 1U; right < values.size(); ++right)
					{
						if (values[left]->interpretation == values[right]->interpretation)
							continue;
						auto left_payload = cxxlens::sdk::detail::functional_payload_digest(
							descriptor->second, values[left]->row);
						auto right_payload = cxxlens::sdk::detail::functional_payload_digest(
							descriptor->second, values[right]->row);
						if (!left_payload || !right_payload)
							return unexpected(!left_payload ? std::move(left_payload.error())
															: std::move(right_payload.error()));
						if (*left_payload == *right_payload)
							continue;
						auto overlap = values[left]->presence.overlap(values[right]->presence);
						if (!overlap || overlap->empty())
							continue;
						const auto* first = values[left];
						const auto* second = values[right];
						output.push_back({descriptor->second.name,
										  first->semantic_key,
										  first->interpretation,
										  second->interpretation,
										  first->content,
										  second->content,
										  std::move(*overlap)});
					}
			}
			std::ranges::sort(
				output,
				[](const differential_disagreement& left, const differential_disagreement& right)
				{
					return std::tie(left.relation,
									left.semantic_key,
									left.left_interpretation,
									left.right_interpretation,
									left.left_content,
									left.right_content) < std::tie(right.relation,
																   right.semantic_key,
																   right.left_interpretation,
																   right.right_interpretation,
																   right.left_content,
																   right.right_content);
				});
			output.erase(
				std::unique(output.begin(),
							output.end(),
							[](const differential_disagreement& left,
							   const differential_disagreement& right)
							{
								return left.relation == right.relation &&
									left.semantic_key == right.semantic_key &&
									left.left_interpretation == right.left_interpretation &&
									left.right_interpretation == right.right_interpretation &&
									left.left_content == right.left_content &&
									left.right_content == right.right_content &&
									left.overlap_fragments == right.overlap_fragments;
							}),
				output.end());
			return output;
		}

		[[nodiscard]] int
		compare_rows(const annotated_row& left,
					 const annotated_row& right,
					 const std::vector<order_key>& keys,
					 const std::map<std::string, value_type, std::less<>>& column_types)
		{
			for (const auto& key : keys)
			{
				const auto left_cell = cell_for(left, key.column, column_types);
				const auto right_cell = cell_for(right, key.column, column_types);
				auto state_rank = [&](const cell_state state)
				{
					const auto found = std::ranges::find(key.cell_state_order, state);
					return static_cast<std::size_t>(
						std::distance(key.cell_state_order.begin(), found));
				};
				const auto left_value =
					std::pair{state_rank(left_cell.state), left_cell.canonical_form()};
				const auto right_value =
					std::pair{state_rank(right_cell.state), right_cell.canonical_form()};
				if (left_value != right_value)
				{
					const auto comparison = left_value < right_value ? -1 : 1;
					return key.ascending ? comparison : -comparison;
				}
			}
			const auto left_value = left.canonical_form();
			const auto right_value = right.canonical_form();
			return left_value == right_value ? 0 : (left_value < right_value ? -1 : 1);
		}

		struct evaluation
		{
			std::vector<annotated_row> rows;
			std::uint64_t memory_bytes{};
			bool ordered{};
			std::vector<order_key> order_keys;
			bool limited{};
		};

		[[nodiscard]] annotated_row
		project_terminal_row(const annotated_row& source,
							 const std::span<const column_ref> output_schema,
							 const std::map<std::string, value_type, std::less<>>& column_types)
		{
			const auto aliases = detail::output_aliases(output_schema);
			auto output = source;
			output.values.clear();
			for (std::size_t index = 0U; index < output_schema.size(); ++index)
				output.values.emplace("output." + aliases[index],
									  cell_for(source,
											   {output_schema[index].column_id,
												column_availability::require,
												output_schema[index].source_alias},
											   column_types));
			return output;
		}

		struct runtime_state
		{
			execution_budget budget;
			std::uint64_t scanned{};
			std::uint64_t retained_memory_bytes{};
			std::uint64_t peak_memory_bytes{};
			std::uint64_t peak_intermediate_rows{};
			std::string failure_code;
			std::string failure_subject;
			std::vector<snapshot_claim_annotation> source_annotations;

			[[nodiscard]] bool failed() const noexcept
			{
				return !failure_code.empty();
			}

			[[nodiscard]] bool reserve_memory(const std::string_view subject,
											  const std::uint64_t bytes)
			{
				if (bytes > std::numeric_limits<std::uint64_t>::max() - retained_memory_bytes ||
					retained_memory_bytes + bytes > budget.max_memory_bytes)
				{
					failure_code = "sdk.query-memory-budget";
					failure_subject = std::string{subject};
					return false;
				}
				retained_memory_bytes += bytes;
				peak_memory_bytes = std::max(peak_memory_bytes, retained_memory_bytes);
				return true;
			}
		};

		[[nodiscard]] std::optional<std::uint64_t>
		row_accounting_bytes(const annotated_row& row) noexcept
		{
			try
			{
				const auto size = row.canonical_form().size();
				if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t))
					if (size > std::numeric_limits<std::uint64_t>::max())
						return std::nullopt;
				return static_cast<std::uint64_t>(size);
			}
			catch (const std::bad_alloc&)
			{
				return std::nullopt;
			}
		}

		[[nodiscard]] bool append_row(runtime_state& state,
									  const std::string_view subject,
									  evaluation& output,
									  annotated_row row)
		{
			if (output.rows.size() >= state.budget.max_intermediate_rows)
			{
				state.failure_code = "sdk.query-intermediate-budget";
				state.failure_subject = std::string{subject};
				return false;
			}
			const auto bytes = row_accounting_bytes(row);
			if (!bytes)
			{
				state.failure_code = "sdk.query-memory-budget";
				state.failure_subject = std::string{subject};
				return false;
			}
			if (!state.reserve_memory(subject, *bytes))
				return false;
			try
			{
				output.rows.push_back(std::move(row));
			}
			catch (const std::bad_alloc&)
			{
				state.retained_memory_bytes -= *bytes;
				state.failure_code = "sdk.query-memory-budget";
				state.failure_subject = std::string{subject};
				return false;
			}
			output.memory_bytes += *bytes;
			state.peak_intermediate_rows =
				std::max<std::uint64_t>(state.peak_intermediate_rows, output.rows.size());
			return true;
		}

		[[nodiscard]] bool replace_row(runtime_state& state,
									   const std::string_view subject,
									   evaluation& output,
									   const std::size_t index,
									   annotated_row row)
		{
			const auto previous = row_accounting_bytes(output.rows[index]);
			const auto replacement = row_accounting_bytes(row);
			if (!previous || !replacement)
			{
				state.failure_code = "sdk.query-memory-budget";
				state.failure_subject = std::string{subject};
				return false;
			}
			if (*replacement > *previous &&
				!state.reserve_memory(subject,
									  static_cast<std::uint64_t>(*replacement - *previous)))
				return false;
			if (*previous > *replacement)
				state.retained_memory_bytes -= static_cast<std::uint64_t>(*previous - *replacement);
			output.memory_bytes = output.memory_bytes - *previous + *replacement;
			output.rows[index] = std::move(row);
			return true;
		}

		[[nodiscard]] bool candidate_fits_memory(runtime_state& state,
												 const std::string_view subject,
												 const annotated_row& row)
		{
			const auto bytes = row_accounting_bytes(row);
			if (!bytes ||
				*bytes > std::numeric_limits<std::uint64_t>::max() - state.retained_memory_bytes ||
				state.retained_memory_bytes + *bytes > state.budget.max_memory_bytes)
			{
				state.failure_code = "sdk.query-memory-budget";
				state.failure_subject = std::string{subject};
				return false;
			}
			return true;
		}

		[[nodiscard]] std::optional<std::uint64_t>
		annotation_accounting_bytes(const snapshot_claim_annotation& annotation) noexcept
		{
			try
			{
				std::uint64_t bytes{};
				auto add = [&](const std::size_t size)
				{
					if (size > std::numeric_limits<std::uint64_t>::max() - bytes)
						return false;
					bytes += static_cast<std::uint64_t>(size);
					return true;
				};
				if (!add(annotation.row.canonical_form().size()) ||
					!add(annotation.assertion.size()) || !add(annotation.semantic_key.size()) ||
					!add(annotation.interpretation.size()) || !add(annotation.producer.id.size()) ||
					!add(annotation.producer.semantic_contract.size()) ||
					!add(annotation.content.size()) || !add(annotation.provenance_root.size()) ||
					!add(annotation.presence.universe.size()) ||
					!add(annotation.guarantee.approximation.size()) ||
					!add(annotation.guarantee.scope.size()) ||
					!add(annotation.guarantee.assumptions.size()))
					return std::nullopt;
				for (const auto& fragment : annotation.presence.fragments)
					if (!add(fragment.size()))
						return std::nullopt;
				for (const auto& modality : annotation.guarantee.verification_modalities)
					if (!add(modality.size()))
						return std::nullopt;
				return bytes;
			}
			catch (const std::bad_alloc&)
			{
				return std::nullopt;
			}
		}

		[[nodiscard]] bool append_source_annotation(runtime_state& state,
													const std::string_view subject,
													const snapshot_claim_annotation& annotation)
		{
			const auto bytes = annotation_accounting_bytes(annotation);
			if (!bytes || !state.reserve_memory(subject, bytes.value_or(0U)))
			{
				if (!bytes)
				{
					state.failure_code = "sdk.query-memory-budget";
					state.failure_subject = std::string{subject};
				}
				return false;
			}
			try
			{
				state.source_annotations.push_back(annotation);
			}
			catch (const std::bad_alloc&)
			{
				state.retained_memory_bytes -= *bytes;
				state.failure_code = "sdk.query-memory-budget";
				state.failure_subject = std::string{subject};
				return false;
			}
			return true;
		}

		[[nodiscard]] std::string operator_strategy(const std::string_view operator_id)
		{
			static const std::map<std::string, std::string, std::less<>> strategies{
				{"query.condition_restrict.v1", "canonical-condition-intersection"},
				{"query.distinct.v1", "canonical-key-reduction"},
				{"query.filter.v1", "typed-predicate"},
				{"query.inner_join.v1", "bounded-reference-nested-loop"},
				{"query.interpretation_restrict.v1", "exact-interpretation-filter"},
				{"query.limit.v1", "sealed-total-order-prefix"},
				{"query.order_by.v1", "stable-total-order"},
				{"query.project.v1", "lossless-column-projection"},
				{"query.scan.v1", "canonical-claim-content-scan"},
				{"query.semi_join.v1", "all-compatible-witness-reduction"},
				{"query.union.v1", "bag-addition"},
			};
			return strategies.find(operator_id)->second;
		}

		[[nodiscard]] result<std::vector<claim_conflict>>
		conflicts_for(const std::vector<snapshot_claim_annotation>& annotations,
					  const std::map<std::string, relation_descriptor, std::less<>>& descriptors)
		{
			std::map<std::string, std::vector<const snapshot_claim_annotation*>, std::less<>>
				groups;
			for (const auto& annotation : annotations)
			{
				const auto descriptor = descriptors.find(annotation.row.descriptor_id);
				if (descriptor == descriptors.end() ||
					descriptor->second.merge != merge_mode::functional_assertion)
					continue;
				groups[annotation.row.descriptor_id + "\n" + annotation.semantic_key + "\n" +
					   annotation.interpretation]
					.push_back(&annotation);
			}
			std::vector<claim_conflict> output;
			for (auto& [key, values] : groups)
			{
				(void)key;
				std::ranges::sort(values, annotation_order);
				const auto descriptor = descriptors.find(values.front()->row.descriptor_id);
				for (std::size_t left = 0U; left < values.size(); ++left)
					for (std::size_t right = left + 1U; right < values.size(); ++right)
					{
						auto left_payload = cxxlens::sdk::detail::functional_payload_digest(
							descriptor->second, values[left]->row);
						auto right_payload = cxxlens::sdk::detail::functional_payload_digest(
							descriptor->second, values[right]->row);
						if (!left_payload || !right_payload)
							return unexpected(!left_payload ? std::move(left_payload.error())
															: std::move(right_payload.error()));
						if (*left_payload == *right_payload)
							continue;
						auto overlap = values[left]->presence.overlap(values[right]->presence);
						if (!overlap || overlap->empty())
							continue;
						claim_conflict conflict;
						conflict.relation = descriptor->second.name;
						conflict.semantic_key = values[left]->semantic_key;
						conflict.interpretation = values[left]->interpretation;
						conflict.overlap_fragments = std::move(*overlap);
						conflict.assertions = {values[left]->assertion, values[right]->assertion};
						conflict.contents = {values[left]->content, values[right]->content};
						output.push_back(std::move(conflict));
					}
			}
			cxxlens::sdk::detail::canonicalize_claim_conflicts(output);
			return output;
		}

		[[nodiscard]] std::vector<std::string> assumption_ids(const std::string_view value)
		{
			std::vector<std::string> output;
			std::size_t begin{};
			while (begin <= value.size())
			{
				const auto end = value.find(',', begin);
				const auto item = value.substr(
					begin, end == std::string_view::npos ? value.size() - begin : end - begin);
				if (!item.empty())
					output.emplace_back(item);
				if (end == std::string_view::npos)
					break;
				begin = end + 1U;
			}
			canonical_set(output);
			return output;
		}

		[[nodiscard]] std::vector<std::string>
		modality_closure(const std::span<const std::string> values)
		{
			std::vector<std::string> output{values.begin(), values.end()};
			if (std::ranges::any_of(output,
									[](const std::string& value)
									{
										return value == "frontend_replayed" ||
											value == "compiler_verified" ||
											value == "link_verified" ||
											value == "runtime_observed" ||
											value == "differentially_verified";
									}))
				output.emplace_back("schema_validated");
			canonical_set(output);
			return output;
		}

		[[nodiscard]] std::string fragment_json(const query_guarantee_fragment& fragment)
		{
			std::ostringstream output;
			output << "{\"assumptions\":" << strings_json(fragment.assumptions)
				   << ",\"claim_contributors\":" << strings_json(fragment.claim_contributors)
				   << ",\"closure_ids\":" << strings_json(fragment.closure_ids)
				   << ",\"condition_fragments\":" << strings_json(fragment.condition.fragments)
				   << ",\"condition_partition_complete\":"
				   << (fragment.condition_partition_complete ? "true" : "false")
				   << ",\"condition_universe\":" << json_string(fragment.condition.universe)
				   << ",\"conflicting\":" << (fragment.conflicting ? "true" : "false")
				   << ",\"coverage_states\":" << strings_json(fragment.coverage_states)
				   << ",\"guarantee\":" << guarantee_json(fragment.guarantee)
				   << ",\"interpretation\":" << json_string(fragment.interpretation)
				   << ",\"producer_contracts\":[";
			for (std::size_t index = 0U; index < fragment.producer_contracts.size(); ++index)
			{
				if (index != 0U)
					output << ',';
				output << "{\"id\":" << json_string(fragment.producer_contracts[index].id)
					   << ",\"semantic_contract\":"
					   << json_string(fragment.producer_contracts[index].semantic_contract) << '}';
			}
			output << ']' << ",\"provenance\":" << strings_json(fragment.provenance)
				   << ",\"requires_closure\":" << (fragment.requires_closure ? "true" : "false")
				   << ",\"unresolved\":" << (fragment.unresolved ? "true" : "false") << '}';
			return output.str();
		}

		[[nodiscard]] bool fragment_conflicts(const query_guarantee_fragment& fragment,
											  const std::span<const claim_conflict> conflicts)
		{
			return std::ranges::any_of(
				conflicts,
				[&](const claim_conflict& conflict)
				{
					return conflict.interpretation == fragment.interpretation &&
						std::ranges::any_of(fragment.claim_contributors,
											[&](const std::string& contributor)
											{
												return std::ranges::binary_search(
													conflict.assertions, contributor);
											}) &&
						std::ranges::any_of(fragment.condition.fragments,
											[&](const std::string& condition)
											{
												return std::ranges::binary_search(
													conflict.overlap_fragments, condition);
											});
				});
		}

		[[nodiscard]] query_summary_guarantee
		summarize_guarantee(const std::span<const annotated_row> rows,
							const std::span<const snapshot_claim_annotation> empty_result_sources,
							const std::span<const snapshot_query_coverage> coverage,
							const std::span<const std::string> closures,
							const std::span<const query_unresolved> unresolved,
							const std::span<const claim_conflict> conflicts,
							const bool inputs_complete,
							const bool execution_complete,
							const bool closed_world,
							const bool limited)
		{
			std::vector<std::string> coverage_states;
			for (const auto& item : coverage)
				coverage_states.push_back(item.unit.state);
			canonical_set(coverage_states);

			std::vector<query_guarantee_fragment> fragments;
			auto append_fragment = [&](const claim_guarantee& guarantee,
									   const claim_condition& condition,
									   const std::string_view interpretation,
									   const std::span<const std::string> contributors,
									   const std::span<const claim_producer> producers,
									   const std::span<const std::string> provenance)
			{
				query_guarantee_fragment fragment{guarantee,
												  condition,
												  std::string{interpretation},
												  assumption_ids(guarantee.assumptions),
												  {contributors.begin(), contributors.end()},
												  {producers.begin(), producers.end()},
												  {provenance.begin(), provenance.end()},
												  coverage_states,
												  {closures.begin(), closures.end()},
												  !condition.fragments.empty(),
												  false,
												  false,
												  true};
				fragment.conflicting = fragment_conflicts(fragment, conflicts);
				fragment.unresolved = std::ranges::any_of(
					unresolved,
					[&](const query_unresolved& item)
					{
						return item.code != "sdk.query-input-unresolved" ||
							std::ranges::binary_search(fragment.claim_contributors, item.subject);
					});
				fragments.push_back(std::move(fragment));
			};
			for (const auto& row : rows)
				for (const auto& edge : row.contributor_edges)
				{
					const std::array contributors{edge.claim_contributor};
					const std::array producers{edge.producer};
					const std::array provenance{edge.provenance};
					append_fragment(edge.guarantee,
									edge.condition,
									edge.interpretation,
									contributors,
									producers,
									provenance);
				}
			if (fragments.empty())
				for (const auto& annotation : empty_result_sources)
				{
					const std::array contributors{annotation.assertion};
					const std::array producers{annotation.producer};
					const std::array provenance{annotation.provenance_root};
					append_fragment(annotation.guarantee,
									annotation.presence,
									annotation.interpretation,
									contributors,
									producers,
									provenance);
				}
			if (fragments.empty())
				append_fragment({"unknown", "query-empty", "assumptions:unknown", {}},
								{"query-empty", {}},
								"query-empty",
								{},
								{},
								{});

			std::ranges::sort(
				fragments,
				[](const query_guarantee_fragment& left, const query_guarantee_fragment& right)
				{
					return fragment_json(left) < fragment_json(right);
				});
			fragments.erase(std::ranges::unique(fragments,
												[](const query_guarantee_fragment& left,
												   const query_guarantee_fragment& right)
												{
													return fragment_json(left) ==
														fragment_json(right);
												})
								.begin(),
							fragments.end());

			std::vector<std::string> scopes;
			std::vector<std::string> interpretations;
			std::vector<std::string> assumptions;
			std::vector<std::string> condition_fragments;
			std::vector<std::string> condition_universes;
			std::vector<std::string> modalities =
				modality_closure(fragments.front().guarantee.verification_modalities);
			bool sound = true;
			bool complete = inputs_complete && execution_complete && !limited;
			bool exact = complete && closed_world;
			static const std::set<std::string, std::less<>> blocking{
				"failed", "unresolved", "unsupported", "stale", "truncated"};
			for (const auto& fragment : fragments)
			{
				const auto& approximation = fragment.guarantee.approximation;
				sound =
					sound && (approximation == "exact" || approximation == "under_approximation");
				complete =
					complete && (approximation == "exact" || approximation == "over_approximation");
				exact = exact && approximation == "exact" &&
					fragment.condition_partition_complete && !fragment.conflicting &&
					!fragment.unresolved &&
					(!fragment.requires_closure || !fragment.closure_ids.empty()) &&
					std::ranges::none_of(fragment.coverage_states,
										 [&](const std::string& state)
										 {
											 return blocking.contains(state);
										 });
				if (fragment.conflicting || fragment.unresolved)
					sound = complete = false;
				scopes.push_back(fragment.guarantee.scope);
				interpretations.push_back(fragment.interpretation);
				assumptions.insert(
					assumptions.end(), fragment.assumptions.begin(), fragment.assumptions.end());
				condition_universes.push_back(fragment.condition.universe);
				condition_fragments.insert(condition_fragments.end(),
										   fragment.condition.fragments.begin(),
										   fragment.condition.fragments.end());
				auto current = modality_closure(fragment.guarantee.verification_modalities);
				std::vector<std::string> intersection;
				std::ranges::set_intersection(
					modalities, current, std::back_inserter(intersection));
				modalities = std::move(intersection);
			}
			canonical_set(scopes);
			canonical_set(interpretations);
			canonical_set(assumptions);
			canonical_set(condition_universes);
			canonical_set(condition_fragments);
			if (scopes.size() != 1U || condition_universes.size() != 1U)
				exact = complete = false;

			std::ostringstream fragment_projection;
			for (const auto& fragment : fragments)
				fragment_projection << fragment_json(fragment) << '\n';
			const auto fragment_digest = *semantic_digest("cxxlens.query-guarantee-fragment-set.v1",
														  fragment_projection.str());
			std::string scope = scopes.front();
			if (scopes.size() != 1U)
			{
				std::ostringstream projection;
				for (const auto& value : scopes)
					projection << value << '\n';
				scope = "query:" + *semantic_digest("query.scope.v1", projection.str());
			}
			std::string universe = condition_universes.front();
			if (condition_universes.size() != 1U)
				universe = "query:" +
					*semantic_digest("query.condition-universe.v1", fragment_projection.str());

			std::string approximation{"unknown"};
			if (exact)
				approximation = "exact";
			else if (sound)
				approximation = "under_approximation";
			else if (complete)
				approximation = "over_approximation";
			return {std::move(approximation),
					std::move(scope),
					{std::move(universe), std::move(condition_fragments)},
					std::move(interpretations),
					std::move(assumptions),
					std::move(modalities),
					static_cast<std::uint64_t>(fragments.size()),
					fragment_digest,
					"fragments:" + fragment_digest,
					std::move(fragments)};
		}

		[[nodiscard]] std::string execution_name(const execution_status status)
		{
			switch (status)
			{
				case execution_status::complete:
					return "complete";
				case execution_status::truncated:
					return "truncated";
				case execution_status::cancelled_with_partial:
					return "cancelled_with_partial";
				case execution_status::failed_before_result:
					return "failed_before_result";
			}
			return "failed_before_result";
		}

		struct closure_selector
		{
			std::optional<claim_condition> condition;
			std::optional<std::string> interpretation;
		};

		using closure_selectors = std::map<std::string, std::vector<closure_selector>, std::less<>>;

		[[nodiscard]] result<void>
		collect_closure_selectors(const std::string_view node_id,
								  closure_selector selector,
								  const std::map<std::string, const ir_node*, std::less<>>& nodes,
								  closure_selectors& output)
		{
			const auto& node = *nodes.find(node_id)->second;
			auto arguments = decode_arguments(node);
			if (!arguments)
				return unexpected(std::move(arguments.error()));
			if (node.operator_id == "query.condition_restrict.v1")
			{
				const auto& restriction = std::get<condition_arguments>(*arguments);
				claim_condition requested{restriction.universe, restriction.alternatives};
				if (selector.condition)
				{
					auto intersection = intersect_condition(*selector.condition, requested);
					if (!intersection)
						return unexpected(std::move(intersection.error()));
					if (!*intersection)
						return {};
					selector.condition = std::move(**intersection);
				}
				else
					selector.condition = std::move(requested);
			}
			else if (node.operator_id == "query.interpretation_restrict.v1")
			{
				const auto& restriction = std::get<interpretation_arguments>(*arguments);
				if (selector.interpretation &&
					*selector.interpretation != restriction.interpretation)
					return {};
				selector.interpretation = restriction.interpretation;
			}
			else if (node.operator_id == "query.scan.v1")
			{
				const auto& scan = std::get<scan_arguments>(*arguments);
				output[scan.descriptor_id].push_back(std::move(selector));
				return {};
			}
			for (const auto& input : node.inputs)
				if (auto collected = collect_closure_selectors(input, selector, nodes, output);
					!collected)
					return unexpected(std::move(collected.error()));
			return {};
		}

		[[nodiscard]] result<std::pair<bool, std::vector<std::string>>>
		applicable_closures(const logical_query_ir& query,
							const snapshot_handle& snapshot,
							const std::set<std::string, std::less<>>& requirements)
		{
			std::map<std::string, const ir_node*, std::less<>> nodes;
			for (const auto& node : query.nodes)
				nodes.emplace(node.id, &node);
			closure_selectors selectors;
			if (auto collected = collect_closure_selectors(query.root, {}, nodes, selectors);
				!collected)
				return unexpected(std::move(collected.error()));

			std::vector<std::string> applied;
			for (const auto& relation : requirements)
			{
				const auto relation_selectors = selectors.find(relation);
				if (relation_selectors == selectors.end() || relation_selectors->second.empty())
					return std::pair{false, std::vector<std::string>{}};
				for (const auto& selector : relation_selectors->second)
				{
					std::size_t relevant{};
					std::vector<std::string> covered_fragments;
					for (const auto& binding : snapshot.partition_bindings())
					{
						if (binding.relation_descriptor_id != relation ||
							(selector.interpretation &&
							 binding.interpretation != *selector.interpretation))
							continue;
						if (selector.condition)
						{
							auto overlap = binding.condition.overlap(*selector.condition);
							if (!overlap)
								return unexpected(std::move(overlap.error()));
							if (overlap->empty())
								continue;
							covered_fragments.insert(
								covered_fragments.end(), overlap->begin(), overlap->end());
						}
						++relevant;
						const auto certificate = std::ranges::find_if(
							snapshot.closure_certificates(),
							[&](const closure_certificate& value)
							{
								return value.subject.subject_partition_id == binding.partition_id &&
									value.subject.relation_descriptor_id == relation &&
									value.subject.condition == binding.condition &&
									value.subject.interpretation == binding.interpretation &&
									value.subject.assumption_set_id == binding.assumption_set_id &&
									value.subject.producer_semantics ==
									binding.producer_semantics &&
									value.subject.closure_kind == "relation-key-enumeration";
							});
						if (certificate == snapshot.closure_certificates().end())
							return std::pair{false, std::vector<std::string>{}};
						applied.push_back(certificate->id);
					}
					if (relevant == 0U)
						return std::pair{false, std::vector<std::string>{}};
					if (selector.condition)
					{
						canonical_set(covered_fragments);
						if (!std::ranges::includes(covered_fragments,
												   selector.condition->fragments))
							return std::pair{false, std::vector<std::string>{}};
					}
				}
			}
			canonical_set(applied);
			return std::pair{true, std::move(applied)};
		}
	} // namespace

	struct query_result::data
	{
		std::vector<annotated_row> row_values;
		execution_status status{execution_status::failed_before_result};
		bool ordered{};
		bool input_complete{};
		bool closed_world{};
		std::vector<snapshot_query_coverage> coverage;
		std::vector<std::string> closures;
		std::vector<query_unresolved> unresolved;
		std::vector<claim_conflict> conflict_values;
		std::vector<differential_disagreement> disagreement_values;
		std::vector<claim_producer> producers;
		query_summary_guarantee guarantee{"unknown",
										  "query-empty",
										  {"query-empty", {}},
										  {"query-empty"},
										  {"assumptions:unknown"},
										  {},
										  0U,
										  {},
										  {},
										  {}};
		query_explanation logical;
		query_explanation physical;
		std::string ir_digest;
		std::string snapshot;
		std::string publication;
	};

	result<void> query_contributor_edge::validate() const
	{
		if (claim_contributor.empty() || producer.id.empty() ||
			producer.semantic_contract.empty() || provenance.empty() || interpretation.empty())
			return unexpected(query_error("sdk.query-row-invalid", "contributor_edges"));
		if (auto valid = guarantee.validate(); !valid)
			return unexpected(std::move(valid.error()));
		return condition.validate();
	}

	std::string query_contributor_edge::canonical_form() const
	{
		std::ostringstream output;
		output << "{\"claim_contributor\":" << json_string(claim_contributor)
			   << ",\"condition_fragments\":" << strings_json(condition.fragments)
			   << ",\"condition_universe\":" << json_string(condition.universe)
			   << ",\"guarantee\":" << guarantee_json(guarantee)
			   << ",\"interpretation\":" << json_string(interpretation)
			   << ",\"producer\":{\"id\":" << json_string(producer.id)
			   << ",\"semantic_contract\":" << json_string(producer.semantic_contract)
			   << "},\"provenance\":" << json_string(provenance) << '}';
		return output.str();
	}

	result<void> annotated_row::validate() const
	{
		if (multiplicity == 0U || interpretation.empty() || claim_contributors.empty() ||
			producer_contracts.empty() || provenance.empty() || contributor_guarantees.empty() ||
			contributor_edges.empty() || !sorted_unique(claim_contributors) ||
			!producers_are_canonical(producer_contracts) || !sorted_unique(provenance) ||
			!contributor_edges_are_canonical(contributor_edges))
			return unexpected(query_error("sdk.query-row-invalid", "annotation"));
		if (auto valid = presence.validate(); !valid)
			return unexpected(std::move(valid.error()));
		for (const auto& [column, cell] : values)
			if (column.empty() || !cell.validate())
				return unexpected(query_error("sdk.query-row-invalid", column));
		if (std::ranges::any_of(producer_contracts,
								[](const claim_producer& producer)
								{
									return producer.id.empty() ||
										producer.semantic_contract.empty();
								}))
			return unexpected(query_error("sdk.query-row-invalid", "producer_contracts"));
		for (const auto& guarantee : contributor_guarantees)
			if (auto valid = guarantee.validate(); !valid)
				return unexpected(std::move(valid.error()));
		for (const auto& edge : contributor_edges)
			if (edge.interpretation != interpretation)
				return unexpected(query_error("sdk.query-row-invalid", "contributor_edges"));
			else if (auto valid = edge.validate(); !valid)
				return unexpected(std::move(valid.error()));
		auto projected = *this;
		refresh_contributor_projections(projected);
		if (projected.claim_contributors != claim_contributors ||
			projected.producer_contracts != producer_contracts ||
			projected.provenance != provenance ||
			projected.contributor_guarantees.size() != contributor_guarantees.size() ||
			!std::ranges::equal(projected.contributor_guarantees,
								contributor_guarantees,
								[](const claim_guarantee& left, const claim_guarantee& right)
								{
									return guarantee_key(left) == guarantee_key(right);
								}))
			return unexpected(query_error("sdk.query-row-invalid", "contributor_projections"));
		return {};
	}

	std::string annotated_row::canonical_form() const
	{
		auto guarantees = contributor_guarantees;
		canonical_guarantees(guarantees);
		auto edges = contributor_edges;
		canonical_contributor_edges(edges);
		std::ostringstream output;
		output << "{\"claim_contributors\":" << strings_json(claim_contributors)
			   << ",\"condition_fragments\":" << strings_json(presence.fragments)
			   << ",\"condition_universe\":" << json_string(presence.universe)
			   << ",\"contributor_guarantees\":[";
		for (std::size_t index = 0U; index < guarantees.size(); ++index)
		{
			if (index != 0U)
				output << ',';
			output << guarantee_json(guarantees[index]);
		}
		output << ']' << ",\"interpretation\":" << json_string(interpretation)
			   << ",\"contributor_edges\":[";
		for (std::size_t edge = 0U; edge < edges.size(); ++edge)
		{
			if (edge != 0U)
				output << ',';
			output << edges[edge].canonical_form();
		}
		output << ']' << ",\"multiplicity\":" << multiplicity << ",\"producer_contracts\":[";
		for (std::size_t producer = 0U; producer < producer_contracts.size(); ++producer)
		{
			if (producer != 0U)
				output << ',';
			output << "{\"id\":" << json_string(producer_contracts[producer].id)
				   << ",\"semantic_contract\":"
				   << json_string(producer_contracts[producer].semantic_contract) << '}';
		}
		output << ']' << ",\"provenance\":" << strings_json(provenance) << ",\"values\":{";
		std::size_t index{};
		for (const auto& [column, cell] : values)
		{
			if (index++ != 0U)
				output << ',';
			output << json_string(column) << ':' << cell_json(cell);
		}
		output << "}}";
		return output.str();
	}

	stop_token_cancellation::stop_token_cancellation(std::stop_token token) noexcept
		: token_{std::move(token)}
	{
	}

	bool
	stop_token_cancellation::stop_requested(const execution_checkpoint& checkpoint) const noexcept
	{
		(void)checkpoint;
		return token_.stop_requested();
	}

	query_result::query_result(std::shared_ptr<const data> data) : data_{std::move(data)} {}

	result_row_cursor query_result::rows() const
	{
		return result_row_cursor{data_};
	}

	execution_status query_result::execution() const noexcept
	{
		return data_->status;
	}

	bool query_result::ordered() const noexcept
	{
		return data_->ordered;
	}

	bool query_result::inputs_complete() const noexcept
	{
		return data_->input_complete;
	}

	bool query_result::closed() const noexcept
	{
		return data_->closed_world;
	}

	std::span<const snapshot_query_coverage> query_result::input_coverage() const noexcept
	{
		return data_->coverage;
	}

	std::span<const std::string> query_result::closure_ids() const noexcept
	{
		return data_->closures;
	}

	std::span<const query_unresolved> query_result::unresolved_items() const noexcept
	{
		return data_->unresolved;
	}

	std::span<const claim_conflict> query_result::conflicts() const noexcept
	{
		return data_->conflict_values;
	}

	std::span<const differential_disagreement>
	query_result::differential_disagreements() const noexcept
	{
		return data_->disagreement_values;
	}

	std::span<const claim_producer> query_result::producer_contracts() const noexcept
	{
		return data_->producers;
	}

	const query_summary_guarantee& query_result::summary_guarantee() const noexcept
	{
		return data_->guarantee;
	}

	const query_explanation& query_result::explain_logical() const noexcept
	{
		return data_->logical;
	}

	const query_explanation& query_result::explain_physical() const noexcept
	{
		return data_->physical;
	}

	std::string_view query_result::logical_ir_digest() const noexcept
	{
		return data_->ir_digest;
	}

	std::string_view query_result::snapshot_id() const noexcept
	{
		return data_->snapshot;
	}

	std::string query_result::canonical_form() const
	{
		std::ostringstream output;
		output << "{\"closed\":" << (data_->closed_world ? "true" : "false")
			   << ",\"closure_ids\":" << strings_json(data_->closures) << ",\"conflicts\":[";
		for (std::size_t index = 0U; index < data_->conflict_values.size(); ++index)
		{
			if (index != 0U)
				output << ',';
			output << cxxlens::sdk::detail::canonical_claim_conflict_json(
				data_->conflict_values[index]);
		}
		output << "],\"differential_disagreements\":[";
		for (std::size_t index = 0U; index < data_->disagreement_values.size(); ++index)
		{
			if (index != 0U)
				output << ',';
			const auto& disagreement = data_->disagreement_values[index];
			output << "{\"left_content\":" << json_string(disagreement.left_content)
				   << ",\"left_interpretation\":" << json_string(disagreement.left_interpretation)
				   << ",\"overlap_fragments\":" << strings_json(disagreement.overlap_fragments)
				   << ",\"relation\":" << json_string(disagreement.relation)
				   << ",\"right_content\":" << json_string(disagreement.right_content)
				   << ",\"right_interpretation\":" << json_string(disagreement.right_interpretation)
				   << ",\"semantic_key\":" << json_string(disagreement.semantic_key) << '}';
		}
		output << "],\"input_coverage\":[";
		for (std::size_t index = 0U; index < data_->coverage.size(); ++index)
		{
			if (index != 0U)
				output << ',';
			const auto& coverage = data_->coverage[index];
			output << "{\"domain\":" << json_string(coverage.unit.domain)
				   << ",\"key\":" << json_string(coverage.unit.key)
				   << ",\"reason\":" << json_string(coverage.unit.reason)
				   << ",\"relation\":" << json_string(coverage.relation_descriptor_id)
				   << ",\"state\":" << json_string(coverage.unit.state) << '}';
		}
		output << "],\"inputs_complete\":" << (data_->input_complete ? "true" : "false")
			   << R"(,"logical_explanation":{"id":)" << json_string(data_->logical.id)
			   << ",\"text\":" << json_string(data_->logical.text)
			   << "},\"logical_ir_digest\":" << json_string(data_->ir_digest)
			   << ",\"ordered\":" << (data_->ordered ? "true" : "false")
			   << R"(,"physical_explanation":{"id":)" << json_string(data_->physical.id)
			   << ",\"text\":" << json_string(data_->physical.text) << "},\"producer_contracts\":[";
		for (std::size_t index = 0U; index < data_->producers.size(); ++index)
		{
			if (index != 0U)
				output << ',';
			output << "{\"id\":" << json_string(data_->producers[index].id)
				   << ",\"semantic_contract\":"
				   << json_string(data_->producers[index].semantic_contract) << '}';
		}
		output << "],\"publication_id\":" << json_string(data_->publication) << ",\"rows\":[";
		for (std::size_t index = 0U; index < data_->row_values.size(); ++index)
		{
			if (index != 0U)
				output << ',';
			output << data_->row_values[index].canonical_form();
		}
		output << R"(],"schema":"cxxlens.query-execution-result.v1","snapshot_id":)"
			   << json_string(data_->snapshot)
			   << ",\"status\":" << json_string(execution_name(data_->status))
			   << R"(,"summary_guarantee":{"approximation":)"
			   << json_string(data_->guarantee.approximation)
			   << ",\"assumptions\":" << strings_json(data_->guarantee.assumptions)
			   << R"(,"condition_partition":{"alternatives":)"
			   << strings_json(data_->guarantee.condition_partition.fragments)
			   << ",\"universe\":" << json_string(data_->guarantee.condition_partition.universe)
			   << "},\"drill_down_ref\":" << json_string(data_->guarantee.drill_down_ref)
			   << ",\"fragment_count\":" << data_->guarantee.fragment_count
			   << ",\"fragment_set_digest\":" << json_string(data_->guarantee.fragment_set_digest)
			   << ",\"fragments\":[";
		for (std::size_t index = 0U; index < data_->guarantee.fragments.size(); ++index)
		{
			if (index != 0U)
				output << ',';
			output << fragment_json(data_->guarantee.fragments[index]);
		}
		output << "],\"interpretation_partitions\":"
			   << strings_json(data_->guarantee.interpretation_partitions)
			   << ",\"scope\":" << json_string(data_->guarantee.scope)
			   << ",\"verification_modalities\":"
			   << strings_json(data_->guarantee.verification_modalities) << "},\"unresolved\":[";
		for (std::size_t index = 0U; index < data_->unresolved.size(); ++index)
		{
			if (index != 0U)
				output << ',';
			const auto& unresolved = data_->unresolved[index];
			output << "{\"code\":" << json_string(unresolved.code)
				   << ",\"detail\":" << json_string(unresolved.detail)
				   << ",\"subject\":" << json_string(unresolved.subject) << '}';
		}
		output << "]}";
		return output.str();
	}

	result_row_view::result_row_view(const annotated_row* row,
									 std::weak_ptr<const std::uint64_t> generation,
									 const std::uint64_t expected)
		: row_{row}, generation_{std::move(generation)}, expected_{expected}
	{
	}

	result<annotated_row> result_row_view::copy() const
	{
		const auto current = generation_.lock();
		if (!current || *current != expected_ || row_ == nullptr)
			return unexpected(query_error("sdk.query-row-view-expired", "row_view"));
		return *row_;
	}

	result_row_cursor::result_row_cursor(std::shared_ptr<const query_result::data> result)
		: result_{std::move(result)}, owner_{std::this_thread::get_id()},
		  generation_{std::make_shared<std::uint64_t>(0U)}
	{
	}

	result<std::optional<result_row_view>> result_row_cursor::next()
	{
		if (owner_ != std::this_thread::get_id())
			return unexpected(query_error("sdk.query-cursor-thread-violation", "cursor"));
		++*generation_;
		if (!result_ || index_ >= result_->row_values.size())
			return std::optional<result_row_view>{};
		return std::optional<result_row_view>{
			result_row_view{&result_->row_values[index_++], generation_, *generation_}};
	}

	reference_engine::reference_engine(snapshot_handle snapshot) : snapshot_{std::move(snapshot)} {}

	result<reference_engine> reference_engine::bind(snapshot_handle snapshot)
	{
		if (snapshot.empty())
			return unexpected(query_error("sdk.query-snapshot-empty", "snapshot"));
		if (!snapshot.query_annotations_available())
			return unexpected(
				query_error("sdk.query-annotations-unavailable", std::string{snapshot.id()}));
		return reference_engine{std::move(snapshot)};
	}

	result<query_result> reference_engine::execute(const logical_query_ir& query,
												   const execution_request request) const
	{
		if (auto valid = query.validate(); !valid)
			return unexpected(std::move(valid.error()));
		const bool implicit_terminal_projection =
			std::ranges::none_of(query.nodes,
								 [](const ir_node& node)
								 {
									 return node.operator_id == "query.project.v1";
								 });
		auto schema_plan = make_schema_compatibility_plan(query, implicit_terminal_projection);
		if (!schema_plan)
			return unexpected(std::move(schema_plan.error()));
		auto result_data = std::make_shared<query_result::data>();
		result_data->ir_digest = query.digest();
		result_data->snapshot = snapshot_.id();
		result_data->publication = snapshot_.publication().publication_id;
		result_data->logical = {result_data->ir_digest, query.canonical_form()};

		std::map<std::string, relation_descriptor, std::less<>> descriptors;
		std::map<std::string, value_type, std::less<>> column_types;
		std::set<std::string, std::less<>> requirements;
		for (const auto& requirement : query.relation_requirements)
		{
			auto snapshot_descriptor = snapshot_.descriptor(requirement.id);
			if (!snapshot_descriptor)
				return unexpected(
					query_error("sdk.query-snapshot-schema-mismatch", requirement.id));
			if (snapshot_descriptor->semantic_major != requirement.semantic_major ||
				snapshot_descriptor->version.minor < requirement.version.minor)
				return unexpected(
					query_error("sdk.query-snapshot-schema-mismatch", requirement.id));
			for (const auto& [column_id, access] : *schema_plan)
				if (access.descriptor_id == requirement.id)
				{
					auto snapshot_column = snapshot_descriptor->column(column_id);
					if (!snapshot_column)
					{
						const bool permitted_absence =
							access.availability == column_availability::absent_if_schema_missing &&
							!access.expected.required && access.expected.type.optional;
						if (!permitted_absence)
							return unexpected(
								query_error("sdk.query-snapshot-schema-mismatch", column_id));
						continue;
					}
					if (snapshot_column->type != access.expected.type ||
						snapshot_column->required != access.expected.required ||
						snapshot_column->role != access.expected.role)
						return unexpected(
							query_error("sdk.query-snapshot-schema-mismatch", column_id));
				}
			descriptors.emplace(requirement.id, *snapshot_descriptor);
			requirements.insert(requirement.id);
		}
		for (const auto& node : query.nodes)
			if (node.operator_id == "query.scan.v1")
			{
				auto arguments = decode_arguments(node);
				if (!arguments)
					return unexpected(std::move(arguments.error()));
				const auto& scan = std::get<scan_arguments>(*arguments);
				const auto requirement = std::ranges::find(
					query.relation_requirements, scan.descriptor_id, &relation_descriptor::id);
				for (const auto& column : requirement->columns)
					column_types.emplace(detail::occurrence_column_id(scan.alias, column.id),
										 column.type);
			}
		for (const auto& coverage : snapshot_.input_coverage())
			if (requirements.contains(coverage.relation_descriptor_id))
				result_data->coverage.push_back(coverage);
		std::ranges::sort(
			result_data->coverage,
			[](const snapshot_query_coverage& left, const snapshot_query_coverage& right)
			{
				return std::tie(left.relation_descriptor_id,
								left.unit.domain,
								left.unit.key,
								left.unit.state,
								left.unit.reason) < std::tie(right.relation_descriptor_id,
															 right.unit.domain,
															 right.unit.key,
															 right.unit.state,
															 right.unit.reason);
			});
		for (const auto& unresolved : snapshot_.unresolved_items())
			if (requirements.contains(unresolved.source_relation))
				result_data->unresolved.push_back(
					{"sdk.query-input-unresolved", unresolved.source_assertion, unresolved.reason});
		result_data->input_complete = !requirements.empty() &&
			std::ranges::all_of(
				requirements,
				[&](const std::string& relation)
				{
					const auto units =
						std::ranges::count(result_data->coverage,
										   relation,
										   &snapshot_query_coverage::relation_descriptor_id);
					return units > 0 &&
						std::ranges::all_of(result_data->coverage,
											[&](const snapshot_query_coverage& coverage)
											{
												return coverage.relation_descriptor_id !=
													relation ||
													coverage.unit.state == "covered";
											});
				}) &&
			result_data->unresolved.empty();
		auto closure_proof = applicable_closures(query, snapshot_, requirements);
		if (!closure_proof)
			return unexpected(std::move(closure_proof.error()));
		result_data->closed_world = result_data->input_complete && closure_proof->first;
		if (closure_proof->first)
			result_data->closures = std::move(closure_proof->second);

		if (request.cancellation != nullptr &&
			request.cancellation->stop_requested(
				{execution_checkpoint::phase::before_execution, 0U}))
		{
			result_data->status = execution_status::failed_before_result;
			result_data->unresolved.push_back(
				{"sdk.query-cancelled", "before-execution", "no sealed rows"});
			result_data->guarantee = summarize_guarantee({},
														 {},
														 result_data->coverage,
														 result_data->closures,
														 result_data->unresolved,
														 {},
														 result_data->input_complete,
														 false,
														 result_data->closed_world,
														 false);
			result_data->physical = {"cxxlens.reference-query-planner.v1",
									 "backend=" + std::string{snapshot_.physical_backend()} +
										 ";cancelled=before-execution"};
			return query_result{std::move(result_data)};
		}

		runtime_state state;
		state.budget = request.budget;
		const auto execution_plan = make_canonical_execution_plan(query);
		const auto& normalized_root = execution_plan.stable_ids.at(query.root);
		std::map<std::string, evaluation, std::less<>> evaluations;
		std::ostringstream physical;
		physical << "backend=" << snapshot_.physical_backend();
		if (implicit_terminal_projection)
			physical << ";terminal=canonical-implicit-project";
		for (const auto* planned_node : execution_plan.nodes)
		{
			const auto& node = *planned_node;
			const auto& stable_id = execution_plan.stable_ids.at(node.id);
			auto arguments = decode_arguments(node);
			if (!arguments)
				return unexpected(std::move(arguments.error()));
			physical << ";node=" << stable_id << ':' << node.operator_id << ':'
					 << operator_strategy(node.operator_id);
			std::vector<const evaluation*> inputs;
			inputs.reserve(node.inputs.size());
			for (const auto& input : execution_plan.inputs.at(node.id))
				inputs.push_back(&evaluations.at(input));
			evaluation output;
			if (node.operator_id == "query.scan.v1")
			{
				const auto& scan = std::get<scan_arguments>(*arguments);
				const bool preserve_multiplicity =
					descriptors.at(scan.descriptor_id).merge == merge_mode::multiset;
				std::map<std::string, std::size_t, std::less<>> semantic_rows;
				auto cursor = snapshot_.open_claims(scan.descriptor_id);
				if (!cursor)
					return unexpected(std::move(cursor.error()));
				while (true)
				{
					auto next = cursor->next();
					if (!next)
						return unexpected(std::move(next.error()));
					if (!*next)
						break;
					if (state.retained_memory_bytes >= state.budget.max_memory_bytes)
					{
						state.failure_code = "sdk.query-memory-budget";
						state.failure_subject = stable_id;
						break;
					}
					if (state.scanned == state.budget.max_rows_scanned)
					{
						state.failure_code = "sdk.query-scan-budget";
						state.failure_subject = scan.descriptor_id;
						break;
					}
					auto annotation = (*next)->copy();
					if (!annotation)
						return unexpected(std::move(annotation.error()));
					++state.scanned;
					annotated_row row;
					for (const auto& [column, cell] : annotation->row.cells)
						row.values.emplace(detail::occurrence_column_id(scan.alias, column), cell);
					row.presence = annotation->presence;
					row.interpretation = annotation->interpretation;
					row.claim_contributors = {annotation->assertion};
					row.producer_contracts = {annotation->producer};
					row.provenance = {annotation->provenance_root};
					row.contributor_guarantees = {annotation->guarantee};
					row.contributor_edges = {{annotation->assertion,
											  annotation->producer,
											  annotation->provenance_root,
											  annotation->guarantee,
											  annotation->presence,
											  annotation->interpretation}};
					const auto existing = semantic_rows.find(annotation->content);
					if (!preserve_multiplicity && existing != semantic_rows.end())
					{
						auto merged = output.rows[existing->second];
						merged.contributor_edges.push_back({annotation->assertion,
															annotation->producer,
															annotation->provenance_root,
															annotation->guarantee,
															annotation->presence,
															annotation->interpretation});
						canonicalize_contributor_edges(merged);
						if (!replace_row(
								state, stable_id, output, existing->second, std::move(merged)))
							break;
					}
					else
					{
						if (output.rows.size() >= state.budget.max_intermediate_rows)
						{
							state.failure_code = "sdk.query-intermediate-budget";
							state.failure_subject = stable_id;
							break;
						}
						const auto index = output.rows.size();
						if (!append_row(state, stable_id, output, std::move(row)))
							break;
						if (!preserve_multiplicity)
							semantic_rows.emplace(annotation->content, index);
					}
					if (!append_source_annotation(state, stable_id, *annotation))
						break;
				}
			}
			else if (node.operator_id == "query.filter.v1")
			{
				const auto& predicate = std::get<predicate_arguments>(*arguments).predicate;
				for (const auto& row : inputs.front()->rows)
					if (evaluate_predicate(row, predicate, column_types))
						if (!append_row(state, stable_id, output, row))
							break;
				output.ordered = inputs.front()->ordered;
				output.order_keys = inputs.front()->order_keys;
				output.limited = inputs.front()->limited;
			}
			else if (node.operator_id == "query.project.v1")
			{
				const auto& project = std::get<project_arguments>(*arguments);
				std::map<std::string, std::string, std::less<>> mapping;
				for (const auto& item : project.columns)
				{
					auto source = resolve_occurrence(column_types, item.column);
					if (!source)
						return unexpected(
							query_error("sdk.query-column-mismatch", item.column.column_id));
					mapping.emplace(*source, item.output);
					column_types.emplace(item.output, column_types.at(*source));
				}
				for (const auto& row : inputs.front()->rows)
				{
					auto projected = row;
					projected.values.clear();
					for (const auto& item : project.columns)
						projected.values.emplace(item.output,
												 cell_for(row, item.column, column_types));
					if (!append_row(state, stable_id, output, std::move(projected)))
						break;
				}
				output.ordered = inputs.front()->ordered &&
					std::ranges::all_of(inputs.front()->order_keys,
										[&](const order_key& key)
										{
											auto source =
												resolve_occurrence(column_types, key.column);
											return source && mapping.contains(*source);
										});
				if (output.ordered)
					for (const auto& key : inputs.front()->order_keys)
					{
						auto projected = key;
						const auto source = resolve_occurrence(column_types, key.column);
						projected.column.column_id = mapping.at(*source);
						projected.column.source_alias.clear();
						output.order_keys.push_back(std::move(projected));
					}
				output.limited = inputs.front()->limited;
			}
			else if (node.operator_id == "query.inner_join.v1")
			{
				const auto& predicate = std::get<predicate_arguments>(*arguments).predicate;
				for (const auto& left : inputs[0]->rows)
				{
					for (const auto& right : inputs[1]->rows)
					{
						auto combined = combine(left, right);
						if (!combined)
							return unexpected(std::move(combined.error()));
						if (*combined && evaluate_predicate(**combined, predicate, column_types))
							if (!append_row(state, stable_id, output, std::move(**combined)))
								break;
					}
					if (state.failed())
						break;
				}
				output.limited = inputs[0]->limited || inputs[1]->limited;
			}
			else if (node.operator_id == "query.semi_join.v1")
			{
				const auto& predicate = std::get<predicate_arguments>(*arguments).predicate;
				for (const auto& left : inputs[0]->rows)
				{
					std::optional<annotated_row> selected;
					for (const auto& right : inputs[1]->rows)
					{
						auto combined = combine(left, right);
						if (!combined)
							return unexpected(std::move(combined.error()));
						if (!*combined || !evaluate_predicate(**combined, predicate, column_types))
							continue;
						if (!selected)
						{
							if (output.rows.size() >= state.budget.max_intermediate_rows)
							{
								state.failure_code = "sdk.query-intermediate-budget";
								state.failure_subject = stable_id;
								break;
							}
							selected = left;
							selected->presence.fragments.clear();
							selected->contributor_edges.clear();
						}
						const auto& witness = **combined;
						selected->presence.fragments.insert(selected->presence.fragments.end(),
															witness.presence.fragments.begin(),
															witness.presence.fragments.end());
						selected->contributor_edges.insert(selected->contributor_edges.end(),
														   witness.contributor_edges.begin(),
														   witness.contributor_edges.end());
						canonical_set(selected->presence.fragments);
						canonicalize_contributor_edges(*selected);
						if (!candidate_fits_memory(state, stable_id, *selected))
							break;
					}
					if (state.failed())
						break;
					if (!selected)
						continue;
					canonical_set(selected->presence.fragments);
					canonicalize_contributor_edges(*selected);
					if (!append_row(state, stable_id, output, std::move(*selected)))
						break;
				}
				output.ordered = inputs[0]->ordered;
				output.order_keys = inputs[0]->order_keys;
				output.limited = inputs[0]->limited || inputs[1]->limited;
			}
			else if (node.operator_id == "query.union.v1")
			{
				for (const auto* input : inputs)
				{
					for (const auto& row : input->rows)
						if (!append_row(state, stable_id, output, row))
							break;
					if (state.failed())
						break;
				}
				output.limited = inputs[0]->limited || inputs[1]->limited;
			}
			else if (node.operator_id == "query.distinct.v1")
			{
				std::map<std::string, std::size_t, std::less<>> groups;
				std::uint64_t scratch_bytes{};
				for (const auto& row : inputs.front()->rows)
				{
					std::string key;
					try
					{
						key = distinct_key(row);
					}
					catch (const std::bad_alloc&)
					{
						state.failure_code = "sdk.query-memory-budget";
						state.failure_subject = stable_id;
						break;
					}
					const auto found = groups.find(key);
					if (found == groups.end())
					{
						auto inserted = row;
						inserted.multiplicity = 1U;
						if (!append_row(state, stable_id, output, std::move(inserted)))
							break;
						const auto key_bytes = static_cast<std::uint64_t>(key.size());
						if (!state.reserve_memory(stable_id, key_bytes))
							break;
						try
						{
							groups.emplace(std::move(key), output.rows.size() - 1U);
						}
						catch (const std::bad_alloc&)
						{
							state.retained_memory_bytes -= key_bytes;
							state.failure_code = "sdk.query-memory-budget";
							state.failure_subject = stable_id;
							break;
						}
						scratch_bytes += key_bytes;
						continue;
					}
					auto& current = output.rows[found->second];
					auto merged = current;
					merged.presence.fragments.insert(merged.presence.fragments.end(),
													 row.presence.fragments.begin(),
													 row.presence.fragments.end());
					merged.contributor_edges.insert(merged.contributor_edges.end(),
													row.contributor_edges.begin(),
													row.contributor_edges.end());
					canonical_set(merged.presence.fragments);
					canonicalize_contributor_edges(merged);
					if (!replace_row(state, stable_id, output, found->second, std::move(merged)))
						break;
				}
				state.retained_memory_bytes -= scratch_bytes;
				output.limited = inputs.front()->limited;
			}
			else if (node.operator_id == "query.order_by.v1")
			{
				for (const auto& row : inputs.front()->rows)
					if (!append_row(state, stable_id, output, row))
						break;
				output.order_keys = std::get<order_arguments>(*arguments).keys;
				if (!state.failed())
					try
					{
						std::ranges::sort(
							output.rows,
							[&](const annotated_row& left, const annotated_row& right)
							{
								return compare_rows(left, right, output.order_keys, column_types) <
									0;
							});
					}
					catch (const std::bad_alloc&)
					{
						state.failure_code = "sdk.query-memory-budget";
						state.failure_subject = stable_id;
					}
				output.ordered = true;
				output.limited = inputs.front()->limited;
			}
			else if (node.operator_id == "query.limit.v1")
			{
				const auto count = std::get<limit_arguments>(*arguments).count;
				const auto retained = std::min<std::uint64_t>(inputs.front()->rows.size(), count);
				for (std::size_t index = 0U; index < retained; ++index)
					if (!append_row(state, stable_id, output, inputs.front()->rows[index]))
						break;
				output.ordered = inputs.front()->ordered;
				output.order_keys = inputs.front()->order_keys;
				output.limited = inputs.front()->limited || inputs.front()->rows.size() > count;
			}
			else if (node.operator_id == "query.condition_restrict.v1")
			{
				const auto& condition = std::get<condition_arguments>(*arguments);
				const claim_condition restriction{condition.universe, condition.alternatives};
				for (const auto& row : inputs.front()->rows)
				{
					auto presence = intersect_condition(row.presence, restriction);
					if (!presence)
						return unexpected(std::move(presence.error()));
					if (*presence)
					{
						auto restricted = row;
						restricted.presence = std::move(**presence);
						bind_contributor_condition(restricted, restricted.presence);
						if (!append_row(state, stable_id, output, std::move(restricted)))
							break;
					}
				}
				output.ordered = inputs.front()->ordered;
				output.order_keys = inputs.front()->order_keys;
				output.limited = inputs.front()->limited;
			}
			else if (node.operator_id == "query.interpretation_restrict.v1")
			{
				const auto& interpretation =
					std::get<interpretation_arguments>(*arguments).interpretation;
				for (const auto& row : inputs.front()->rows)
					if (row.interpretation == interpretation)
						if (!append_row(state, stable_id, output, row))
							break;
				output.ordered = inputs.front()->ordered;
				output.order_keys = inputs.front()->order_keys;
				output.limited = inputs.front()->limited;
			}
			evaluations.emplace(node.id, std::move(output));
			physical << ",rows=" << evaluations.at(node.id).rows.size()
					 << ",retained-bytes=" << state.retained_memory_bytes;
			if (state.failed())
				break;
		}

		if (state.failed())
		{
			physical << ";peak-logical-bytes=" << state.peak_memory_bytes
					 << ";peak-intermediate-rows=" << state.peak_intermediate_rows;
			result_data->physical = {"cxxlens.reference-query-planner.v1", physical.str()};
			result_data->status = execution_status::failed_before_result;
			result_data->unresolved.push_back(
				{state.failure_code, state.failure_subject, "no sealed rows"});
			result_data->guarantee = summarize_guarantee({},
														 state.source_annotations,
														 result_data->coverage,
														 result_data->closures,
														 result_data->unresolved,
														 {},
														 result_data->input_complete,
														 false,
														 result_data->closed_world,
														 false);
			return query_result{std::move(result_data)};
		}

		auto conflicts = conflicts_for(state.source_annotations, descriptors);
		if (!conflicts)
			return unexpected(std::move(conflicts.error()));
		result_data->conflict_values = std::move(*conflicts);
		auto disagreements = disagreements_for(state.source_annotations, descriptors);
		if (!disagreements)
			return unexpected(std::move(disagreements.error()));
		result_data->disagreement_values = std::move(*disagreements);
		for (const auto& annotation : state.source_annotations)
			result_data->producers.push_back(annotation.producer);
		canonical_producers(result_data->producers);
		auto evaluated = std::move(evaluations.at(query.root));
		if (implicit_terminal_projection)
		{
			const auto aliases = detail::output_aliases(query.output_schema);
			std::map<std::string, std::string, std::less<>> mapping;
			for (std::size_t index = 0U; index < query.output_schema.size(); ++index)
				mapping.emplace(
					detail::occurrence_column_id(query.output_schema[index].source_alias,
												 query.output_schema[index].column_id),
					"output." + aliases[index]);
			for (std::size_t index = 0U; index < evaluated.rows.size(); ++index)
				if (!replace_row(state,
								 "implicit-terminal-project",
								 evaluated,
								 index,
								 project_terminal_row(
									 evaluated.rows[index], query.output_schema, column_types)))
					break;
			if (!state.failed())
				for (auto& key : evaluated.order_keys)
				{
					const auto source = resolve_occurrence(column_types, key.column);
					key.column.column_id = mapping.at(*source);
					key.column.source_alias.clear();
				}
		}
		physical << ";peak-logical-bytes=" << state.peak_memory_bytes
				 << ";peak-intermediate-rows=" << state.peak_intermediate_rows;
		result_data->physical = {"cxxlens.reference-query-planner.v1", physical.str()};
		if (state.failed())
		{
			result_data->status = execution_status::failed_before_result;
			result_data->unresolved.push_back(
				{state.failure_code, state.failure_subject, "no sealed rows"});
			result_data->guarantee = summarize_guarantee({},
														 state.source_annotations,
														 result_data->coverage,
														 result_data->closures,
														 result_data->unresolved,
														 result_data->conflict_values,
														 result_data->input_complete,
														 false,
														 result_data->closed_world,
														 false);
			return query_result{std::move(result_data)};
		}
		if (!evaluated.ordered)
			try
			{
				std::ranges::sort(evaluated.rows,
								  [](const annotated_row& left, const annotated_row& right)
								  {
									  return left.canonical_form() < right.canonical_form();
								  });
			}
			catch (const std::bad_alloc&)
			{
				result_data->status = execution_status::failed_before_result;
				result_data->unresolved.push_back(
					{"sdk.query-memory-budget", "canonical-output-order", "no sealed rows"});
				result_data->guarantee = summarize_guarantee({},
															 state.source_annotations,
															 result_data->coverage,
															 result_data->closures,
															 result_data->unresolved,
															 result_data->conflict_values,
															 result_data->input_complete,
															 false,
															 result_data->closed_world,
															 false);
				return query_result{std::move(result_data)};
			}
		result_data->ordered = evaluated.ordered;
		result_data->status = execution_status::complete;
		if (evaluated.rows.size() > request.budget.max_rows_output)
		{
			evaluated.rows.resize(static_cast<std::size_t>(request.budget.max_rows_output));
			result_data->status = execution_status::truncated;
			result_data->unresolved.push_back(
				{"sdk.query-output-budget", normalized_root, "canonical sealed prefix returned"});
		}
		for (std::size_t index = 0U; index < evaluated.rows.size(); ++index)
		{
			if (request.cancellation != nullptr &&
				request.cancellation->stop_requested(
					{execution_checkpoint::phase::before_publish_row, index}))
			{
				result_data->status = result_data->row_values.empty()
					? execution_status::failed_before_result
					: execution_status::cancelled_with_partial;
				result_data->unresolved.push_back({"sdk.query-cancelled",
												   normalized_root,
												   result_data->row_values.empty()
													   ? "no sealed rows"
													   : "sealed canonical prefix returned"});
				break;
			}
			result_data->row_values.push_back(std::move(evaluated.rows[index]));
		}
		const bool execution_complete = result_data->status == execution_status::complete;
		result_data->guarantee = summarize_guarantee(
			result_data->row_values,
			state.source_annotations,
			result_data->coverage,
			result_data->closures,
			result_data->unresolved,
			result_data->conflict_values,
			result_data->input_complete,
			execution_complete,
			result_data->closed_world,
			evaluated.limited || result_data->status != execution_status::complete);
		return query_result{std::move(result_data)};
	}
} // namespace cxxlens::sdk::query
