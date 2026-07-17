#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
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

namespace cxxlens::sdk::query
{
	namespace
	{
		[[nodiscard]] error
		query_error(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] std::string escape_json(const std::string_view input)
		{
			std::ostringstream output;
			for (const auto byte : input)
			{
				switch (byte)
				{
					case '\\':
						output << "\\\\";
						break;
					case '"':
						output << "\\\"";
						break;
					case '\b':
						output << "\\b";
						break;
					case '\f':
						output << "\\f";
						break;
					case '\n':
						output << "\\n";
						break;
					case '\r':
						output << "\\r";
						break;
					case '\t':
						output << "\\t";
						break;
					default:
						output << byte;
				}
			}
			return output.str();
		}

		[[nodiscard]] std::string json_string(const std::string_view value)
		{
			return "\"" + escape_json(value) + "\"";
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

		[[nodiscard]] std::string guarantee_key(const claim_guarantee& value)
		{
			return value.approximation + "\n" + value.scope + "\n" + value.assumptions + "\n" +
				strings_json(value.verification_modalities);
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

		[[nodiscard]] detached_cell
		cell_for(const annotated_row& row,
				 const std::string_view column,
				 const std::map<std::string, value_type, std::less<>>& column_types)
		{
			const auto found = row.values.find(column);
			if (found != row.values.end())
				return found->second;
			const auto type = column_types.find(column);
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
					return present_equal(cell_for(row, predicate.column->column_id, column_types),
										 literal_cell(*predicate.literal_value));
				case predicate_kind::column_equals_present:
					return present_equal(cell_for(row, predicate.left->column_id, column_types),
										 cell_for(row, predicate.right->column_id, column_types));
				case predicate_kind::is_present:
					return cell_for(row, predicate.column->column_id, column_types).state ==
						cell_state::present;
				case predicate_kind::is_absent:
					return cell_for(row, predicate.column->column_id, column_types).state ==
						cell_state::absent;
				case predicate_kind::is_unknown:
					return cell_for(row, predicate.column->column_id, column_types).state ==
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
			output.claim_contributors.insert(output.claim_contributors.end(),
											 right.claim_contributors.begin(),
											 right.claim_contributors.end());
			output.producer_contracts.insert(output.producer_contracts.end(),
											 right.producer_contracts.begin(),
											 right.producer_contracts.end());
			output.provenance.insert(
				output.provenance.end(), right.provenance.begin(), right.provenance.end());
			output.contributor_guarantees.insert(output.contributor_guarantees.end(),
												 right.contributor_guarantees.begin(),
												 right.contributor_guarantees.end());
			canonical_set(output.claim_contributors);
			canonical_producers(output.producer_contracts);
			canonical_set(output.provenance);
			canonical_guarantees(output.contributor_guarantees);
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

		[[nodiscard]] std::vector<annotated_row>
		distinct_rows(const std::vector<annotated_row>& rows)
		{
			std::map<std::string, annotated_row, std::less<>> groups;
			for (const auto& row : rows)
			{
				const auto key = distinct_key(row);
				const auto found = groups.find(key);
				if (found == groups.end())
				{
					auto inserted = row;
					inserted.multiplicity = 1U;
					groups.emplace(key, std::move(inserted));
					continue;
				}
				auto& current = found->second;
				current.presence.fragments.insert(current.presence.fragments.end(),
												  row.presence.fragments.begin(),
												  row.presence.fragments.end());
				current.claim_contributors.insert(current.claim_contributors.end(),
												  row.claim_contributors.begin(),
												  row.claim_contributors.end());
				current.producer_contracts.insert(current.producer_contracts.end(),
												  row.producer_contracts.begin(),
												  row.producer_contracts.end());
				current.provenance.insert(
					current.provenance.end(), row.provenance.begin(), row.provenance.end());
				current.contributor_guarantees.insert(current.contributor_guarantees.end(),
													  row.contributor_guarantees.begin(),
													  row.contributor_guarantees.end());
				canonical_set(current.presence.fragments);
				canonical_set(current.claim_contributors);
				canonical_producers(current.producer_contracts);
				canonical_set(current.provenance);
				canonical_guarantees(current.contributor_guarantees);
			}
			std::vector<annotated_row> output;
			output.reserve(groups.size());
			for (auto& [key, row] : groups)
			{
				(void)key;
				output.push_back(std::move(row));
			}
			return output;
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
						auto left_payload = detail::functional_payload_digest(descriptor->second,
																			  values[left]->row);
						auto right_payload = detail::functional_payload_digest(descriptor->second,
																			   values[right]->row);
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
				const auto left_cell = cell_for(left, key.column.column_id, column_types);
				const auto right_cell = cell_for(right, key.column.column_id, column_types);
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
			bool ordered{};
			std::vector<order_key> order_keys;
			bool limited{};
		};

		struct runtime_state
		{
			execution_budget budget;
			std::uint64_t scanned{};
			std::string failure_code;
			std::string failure_subject;
			std::vector<snapshot_claim_annotation> source_annotations;

			[[nodiscard]] bool failed() const noexcept
			{
				return !failure_code.empty();
			}
		};

		void check_intermediate(runtime_state& state,
								const std::string_view subject,
								const std::vector<annotated_row>& rows)
		{
			if (rows.size() > state.budget.max_intermediate_rows)
			{
				state.failure_code = "sdk.query-intermediate-budget";
				state.failure_subject = std::string{subject};
				return;
			}
			std::uint64_t bytes{};
			for (const auto& row : rows)
			{
				const auto size = row.canonical_form().size();
				if (size > std::numeric_limits<std::uint64_t>::max() - bytes)
				{
					state.failure_code = "sdk.query-memory-budget";
					state.failure_subject = std::string{subject};
					return;
				}
				bytes += size;
				if (bytes > state.budget.max_memory_bytes)
				{
					state.failure_code = "sdk.query-memory-budget";
					state.failure_subject = std::string{subject};
					return;
				}
			}
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
						auto left_payload = detail::functional_payload_digest(descriptor->second,
																			  values[left]->row);
						auto right_payload = detail::functional_payload_digest(descriptor->second,
																			   values[right]->row);
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
			std::ranges::sort(output,
							  [](const claim_conflict& left, const claim_conflict& right)
							  {
								  return std::tie(left.relation,
												  left.semantic_key,
												  left.interpretation,
												  left.overlap_fragments,
												  left.assertions,
												  left.contents) < std::tie(right.relation,
																			right.semantic_key,
																			right.interpretation,
																			right.overlap_fragments,
																			right.assertions,
																			right.contents);
							  });
			output.erase(std::unique(output.begin(),
									 output.end(),
									 [](const claim_conflict& left, const claim_conflict& right)
									 {
										 return left.relation == right.relation &&
											 left.semantic_key == right.semantic_key &&
											 left.interpretation == right.interpretation &&
											 left.overlap_fragments == right.overlap_fragments &&
											 left.assertions == right.assertions &&
											 left.contents == right.contents;
									 }),
						 output.end());
			return output;
		}

		[[nodiscard]] claim_guarantee
		summarize_guarantee(const std::vector<snapshot_claim_annotation>& annotations,
							const bool inputs_complete,
							const bool execution_complete,
							const bool limited)
		{
			if (annotations.empty())
				return {"unknown", "query-empty", "assumptions:unknown", {}};
			bool sound = true;
			bool complete = inputs_complete && execution_complete && !limited;
			std::set<std::string, std::less<>> scopes;
			std::set<std::string, std::less<>> assumptions;
			std::set<std::string, std::less<>> modalities(
				annotations.front().guarantee.verification_modalities.begin(),
				annotations.front().guarantee.verification_modalities.end());
			for (const auto& annotation : annotations)
			{
				const auto& guarantee = annotation.guarantee;
				sound = sound &&
					(guarantee.approximation == "exact" ||
					 guarantee.approximation == "under_approximation");
				complete = complete &&
					(guarantee.approximation == "exact" ||
					 guarantee.approximation == "over_approximation");
				scopes.insert(guarantee.scope);
				assumptions.insert(guarantee.assumptions);
				std::set<std::string, std::less<>> current(
					guarantee.verification_modalities.begin(),
					guarantee.verification_modalities.end());
				std::set<std::string, std::less<>> intersection;
				std::ranges::set_intersection(
					modalities, current, std::inserter(intersection, intersection.end()));
				modalities = std::move(intersection);
			}
			std::string approximation{"unknown"};
			if (sound && complete)
				approximation = "exact";
			else if (sound)
				approximation = "under_approximation";
			else if (complete)
				approximation = "over_approximation";
			std::string scope;
			if (scopes.size() == 1U)
				scope = *scopes.begin();
			else
			{
				std::ostringstream joined;
				for (const auto& value : scopes)
					joined << value << '\n';
				scope = "query:" + *semantic_digest("query.scope.v1", joined.str());
			}
			std::ostringstream assumption_text;
			for (auto iterator = assumptions.begin(); iterator != assumptions.end(); ++iterator)
			{
				if (iterator != assumptions.begin())
					assumption_text << ',';
				assumption_text << *iterator;
			}
			return {std::move(approximation),
					std::move(scope),
					assumption_text.str(),
					{modalities.begin(), modalities.end()}};
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
		claim_guarantee guarantee{"unknown", "query-empty", "assumptions:unknown", {}};
		query_explanation logical;
		query_explanation physical;
		std::string ir_digest;
		std::string snapshot;
		std::string publication;
	};

	result<void> annotated_row::validate() const
	{
		if (multiplicity == 0U || interpretation.empty() || claim_contributors.empty() ||
			producer_contracts.empty() || provenance.empty() ||
			!sorted_unique(claim_contributors) || !producers_are_canonical(producer_contracts) ||
			!sorted_unique(provenance))
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
		return {};
	}

	std::string annotated_row::canonical_form() const
	{
		std::ostringstream output;
		output << "{\"claim_contributors\":" << strings_json(claim_contributors)
			   << ",\"condition_fragments\":" << strings_json(presence.fragments)
			   << ",\"condition_universe\":" << json_string(presence.universe)
			   << ",\"interpretation\":" << json_string(interpretation)
			   << ",\"multiplicity\":" << multiplicity << ",\"producer_contracts\":[";
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

	const claim_guarantee& query_result::summary_guarantee() const noexcept
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
			const auto& conflict = data_->conflict_values[index];
			output << json_string(conflict.relation + "|" + conflict.semantic_key + "|" +
								  conflict.interpretation + "|" +
								  strings_json(conflict.assertions));
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
			   << ",\"assumptions\":" << json_string(data_->guarantee.assumptions)
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
			for (const auto& column : query.output_schema)
				if (column.descriptor_id == requirement.id)
				{
					auto snapshot_column = snapshot_descriptor->column(column.column_id);
					if (!snapshot_column || snapshot_column->type != column.type)
						return unexpected(
							query_error("sdk.query-snapshot-schema-mismatch", column.column_id));
				}
			descriptors.emplace(requirement.id, *snapshot_descriptor);
			requirements.insert(requirement.id);
			for (const auto& column : requirement.columns)
				column_types.emplace(column.id, column.type);
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
			result_data->guarantee =
				summarize_guarantee({}, result_data->input_complete, false, false);
			result_data->physical = {"cxxlens.reference-query-planner.v1",
									 "backend=" + std::string{snapshot_.physical_backend()} +
										 ";cancelled=before-execution"};
			return query_result{std::move(result_data)};
		}

		runtime_state state{request.budget, 0U, {}, {}, {}};
		std::map<std::string, evaluation, std::less<>> evaluations;
		std::ostringstream physical;
		physical << "backend=" << snapshot_.physical_backend();
		for (const auto& node : query.nodes)
		{
			auto arguments = decode_arguments(node);
			if (!arguments)
				return unexpected(std::move(arguments.error()));
			physical << ";node=" << node.id << ':' << node.operator_id << ':'
					 << operator_strategy(node.operator_id);
			std::vector<const evaluation*> inputs;
			inputs.reserve(node.inputs.size());
			for (const auto& input : node.inputs)
				inputs.push_back(&evaluations.at(input));
			evaluation output;
			if (node.operator_id == "query.scan.v1")
			{
				const auto& scan = std::get<scan_arguments>(*arguments);
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
					state.source_annotations.push_back(*annotation);
					annotated_row row;
					row.values = annotation->row.cells;
					row.presence = annotation->presence;
					row.interpretation = annotation->interpretation;
					row.claim_contributors = {annotation->assertion};
					row.producer_contracts = {annotation->producer};
					row.provenance = {annotation->provenance_root};
					row.contributor_guarantees = {annotation->guarantee};
					output.rows.push_back(std::move(row));
				}
			}
			else if (node.operator_id == "query.filter.v1")
			{
				const auto& predicate = std::get<predicate_arguments>(*arguments).predicate;
				for (const auto& row : inputs.front()->rows)
					if (evaluate_predicate(row, predicate, column_types))
						output.rows.push_back(row);
				output.ordered = inputs.front()->ordered;
				output.order_keys = inputs.front()->order_keys;
				output.limited = inputs.front()->limited;
			}
			else if (node.operator_id == "query.project.v1")
			{
				const auto& project = std::get<project_arguments>(*arguments);
				std::map<std::string, std::string, std::less<>> mapping;
				for (const auto& item : project.columns)
					mapping.emplace(item.column.column_id, item.output);
				for (const auto& row : inputs.front()->rows)
				{
					auto projected = row;
					projected.values.clear();
					for (const auto& item : project.columns)
						projected.values.emplace(
							item.output, cell_for(row, item.column.column_id, column_types));
					output.rows.push_back(std::move(projected));
				}
				output.ordered = inputs.front()->ordered &&
					std::ranges::all_of(inputs.front()->order_keys,
										[&](const order_key& key)
										{
											return mapping.contains(key.column.column_id);
										});
				if (output.ordered)
					for (const auto& key : inputs.front()->order_keys)
					{
						auto projected = key;
						projected.column.column_id = mapping.at(key.column.column_id);
						output.order_keys.push_back(std::move(projected));
					}
				output.limited = inputs.front()->limited;
			}
			else if (node.operator_id == "query.inner_join.v1")
			{
				const auto& predicate = std::get<predicate_arguments>(*arguments).predicate;
				for (const auto& left : inputs[0]->rows)
					for (const auto& right : inputs[1]->rows)
					{
						auto combined = combine(left, right);
						if (!combined)
							return unexpected(std::move(combined.error()));
						if (*combined && evaluate_predicate(**combined, predicate, column_types))
							output.rows.push_back(std::move(**combined));
					}
				output.limited = inputs[0]->limited || inputs[1]->limited;
			}
			else if (node.operator_id == "query.semi_join.v1")
			{
				const auto& predicate = std::get<predicate_arguments>(*arguments).predicate;
				for (const auto& left : inputs[0]->rows)
				{
					std::vector<annotated_row> witnesses;
					for (const auto& right : inputs[1]->rows)
					{
						auto combined = combine(left, right);
						if (!combined)
							return unexpected(std::move(combined.error()));
						if (*combined && evaluate_predicate(**combined, predicate, column_types))
							witnesses.push_back(std::move(**combined));
					}
					if (witnesses.empty())
						continue;
					auto selected = left;
					selected.presence.fragments.clear();
					for (const auto& witness : witnesses)
					{
						selected.presence.fragments.insert(selected.presence.fragments.end(),
														   witness.presence.fragments.begin(),
														   witness.presence.fragments.end());
						selected.claim_contributors.insert(selected.claim_contributors.end(),
														   witness.claim_contributors.begin(),
														   witness.claim_contributors.end());
						selected.producer_contracts.insert(selected.producer_contracts.end(),
														   witness.producer_contracts.begin(),
														   witness.producer_contracts.end());
						selected.provenance.insert(selected.provenance.end(),
												   witness.provenance.begin(),
												   witness.provenance.end());
						selected.contributor_guarantees.insert(
							selected.contributor_guarantees.end(),
							witness.contributor_guarantees.begin(),
							witness.contributor_guarantees.end());
					}
					canonical_set(selected.presence.fragments);
					canonical_set(selected.claim_contributors);
					canonical_producers(selected.producer_contracts);
					canonical_set(selected.provenance);
					canonical_guarantees(selected.contributor_guarantees);
					output.rows.push_back(std::move(selected));
				}
				output.ordered = inputs[0]->ordered;
				output.order_keys = inputs[0]->order_keys;
				output.limited = inputs[0]->limited || inputs[1]->limited;
			}
			else if (node.operator_id == "query.union.v1")
			{
				output.rows = inputs[0]->rows;
				output.rows.insert(
					output.rows.end(), inputs[1]->rows.begin(), inputs[1]->rows.end());
				output.limited = inputs[0]->limited || inputs[1]->limited;
			}
			else if (node.operator_id == "query.distinct.v1")
			{
				output.rows = distinct_rows(inputs.front()->rows);
				output.limited = inputs.front()->limited;
			}
			else if (node.operator_id == "query.order_by.v1")
			{
				output.rows = inputs.front()->rows;
				output.order_keys = std::get<order_arguments>(*arguments).keys;
				std::ranges::sort(output.rows,
								  [&](const annotated_row& left, const annotated_row& right)
								  {
									  return compare_rows(
												 left, right, output.order_keys, column_types) < 0;
								  });
				output.ordered = true;
				output.limited = inputs.front()->limited;
			}
			else if (node.operator_id == "query.limit.v1")
			{
				output = *inputs.front();
				const auto count = std::get<limit_arguments>(*arguments).count;
				if (output.rows.size() > count)
				{
					output.rows.resize(static_cast<std::size_t>(count));
					output.limited = true;
				}
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
						output.rows.push_back(std::move(restricted));
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
						output.rows.push_back(row);
				output.ordered = inputs.front()->ordered;
				output.order_keys = inputs.front()->order_keys;
				output.limited = inputs.front()->limited;
			}
			check_intermediate(state, node.id, output.rows);
			evaluations.emplace(node.id, std::move(output));
			if (state.failed())
				break;
		}

		result_data->physical = {"cxxlens.reference-query-planner.v1", physical.str()};
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
		if (state.failed())
		{
			result_data->status = execution_status::failed_before_result;
			result_data->unresolved.push_back(
				{state.failure_code, state.failure_subject, "no sealed rows"});
			result_data->guarantee = summarize_guarantee(
				state.source_annotations, result_data->input_complete, false, false);
			return query_result{std::move(result_data)};
		}

		auto evaluated = evaluations.at(query.root);
		if (!evaluated.ordered)
			std::ranges::sort(evaluated.rows,
							  [](const annotated_row& left, const annotated_row& right)
							  {
								  return left.canonical_form() < right.canonical_form();
							  });
		result_data->ordered = evaluated.ordered;
		result_data->status = execution_status::complete;
		if (evaluated.rows.size() > request.budget.max_rows_output)
		{
			evaluated.rows.resize(static_cast<std::size_t>(request.budget.max_rows_output));
			result_data->status = execution_status::truncated;
			result_data->unresolved.push_back(
				{"sdk.query-output-budget", query.root, "canonical sealed prefix returned"});
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
												   query.root,
												   result_data->row_values.empty()
													   ? "no sealed rows"
													   : "sealed canonical prefix returned"});
				break;
			}
			result_data->row_values.push_back(std::move(evaluated.rows[index]));
		}
		const bool execution_complete = result_data->status == execution_status::complete;
		result_data->guarantee = summarize_guarantee(
			state.source_annotations,
			result_data->input_complete,
			execution_complete,
			evaluated.limited || result_data->status != execution_status::complete);
		return query_result{std::move(result_data)};
	}
} // namespace cxxlens::sdk::query
