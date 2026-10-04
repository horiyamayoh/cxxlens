#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

#include <cxxlens/sdk/query_transfer.hpp>

#include "bounded_json_internal.hpp"
#include "query_internal.hpp"
#include "query_result_internal.hpp"

namespace cxxlens::sdk::query
{
	namespace
	{
		using json = sdk::detail::json_value;
		struct decode_failure
		{
			error value;
		};
		[[noreturn]] void fail(std::string field, std::string detail)
		{
			throw decode_failure{
				{"sdk.query-transfer-invalid", std::move(field), std::move(detail)}};
		}
		template <class T>
		T take(result<T> value)
		{
			if (!value)
				throw decode_failure{std::move(value.error())};
			return std::move(*value);
		}
		void check(result<void> value)
		{
			if (!value)
				throw decode_failure{std::move(value.error())};
		}
		const json& member(const json& value, std::string_view name)
		{
			const auto* found = value.member(name);
			if (!found)
				fail(std::string{name}, "missing-member");
			return *found;
		}
		void shape(const json& value, std::initializer_list<std::string_view> names)
		{
			if (!value.has_exact_members(std::span{names.begin(), names.size()}))
				fail("object", "member-set");
		}
		std::string text_value(const json& value, bool nonempty = true)
		{
			const auto* result = value.as_string();
			if (!result || (nonempty && result->empty()))
				fail("string", "string-required");
			return *result;
		}
		std::string string(const json& value, std::string_view name, bool nonempty = true)
		{
			return text_value(member(value, name), nonempty);
		}
		bool boolean(const json& value)
		{
			const auto* result = value.as_boolean();
			if (!result)
				fail("boolean", "boolean-required");
			return *result;
		}
		bool boolean(const json& value, std::string_view name)
		{
			return boolean(member(value, name));
		}
		std::uint64_t number(const json& value)
		{
			const auto* result = value.as_unsigned_integer();
			if (!result)
				fail("integer", "unsigned-required");
			return *result;
		}
		const json::array_type& array(const json& value)
		{
			const auto* result = value.as_array();
			if (!result)
				fail("array", "array-required");
			return *result;
		}
		std::vector<std::string> strings(const json& value, bool sorted = true)
		{
			std::vector<std::string> result;
			for (const auto& item : array(value))
				result.push_back(text_value(item));
			if (sorted &&
				(!std::ranges::is_sorted(result) ||
				 std::ranges::adjacent_find(result) != result.end()))
				fail("set", "canonical-set-required");
			return result;
		}
		bool digest(std::string_view value, bool typed)
		{
			const auto marker = value.rfind(":sha256:");
			if (marker == std::string_view::npos || value.size() - marker != 72U)
				return false;
			const auto prefix = value.substr(0, marker);
			if (typed)
			{
				if (prefix.empty() || prefix.front() < 'a' || prefix.front() > 'z')
					return false;
				for (const char c : prefix)
					if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
						  c == '_' || c == '-'))
						return false;
			}
			else if (prefix != "semantic-v2")
				return false;
			return std::ranges::all_of(value.substr(marker + 8U),
									   [](char c)
									   {
										   return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
									   });
		}
		bool semantic_digest_value(std::string_view value)
		{
			if (value.starts_with("sha256:") && value.size() == 71U)
				return std::ranges::all_of(value.substr(7),
										   [](char c)
										   {
											   return (c >= '0' && c <= '9') ||
												   (c >= 'a' && c <= 'f');
										   });
			return digest(value, false);
		}
		claim_producer producer(const json& value)
		{
			shape(value, {"id", "semantic_contract"});
			claim_producer result{string(value, "id"), string(value, "semantic_contract")};
			if (!semantic_digest_value(result.semantic_contract))
				fail("producer", "contract-digest");
			return result;
		}
		std::vector<claim_producer> producers(const json& value)
		{
			std::vector<claim_producer> result;
			std::string previous;
			for (const auto& item : array(value))
			{
				auto decoded = producer(item);
				auto key = decoded.id + "\n" + decoded.semantic_contract;
				if (!previous.empty() && previous >= key)
					fail("producers", "canonical-set-required");
				previous = std::move(key);
				result.push_back(std::move(decoded));
			}
			return result;
		}
		claim_guarantee guarantee(const json& value)
		{
			shape(value, {"approximation", "scope", "assumptions", "verification_modalities"});
			claim_guarantee result{string(value, "approximation"),
								   string(value, "scope"),
								   string(value, "assumptions"),
								   strings(member(value, "verification_modalities"))};
			check(result.validate());
			return result;
		}
		claim_condition condition(const json& value)
		{
			claim_condition result{string(value, "condition_universe"),
								   strings(member(value, "condition_fragments"))};
			if (!result.fragments.empty())
				check(result.validate());
			return result;
		}
		query_contributor_edge edge(const json& value)
		{
			shape(value,
				  {"claim_contributor",
				   "condition_universe",
				   "condition_fragments",
				   "guarantee",
				   "interpretation",
				   "producer",
				   "provenance"});
			query_contributor_edge result{string(value, "claim_contributor"),
										  producer(member(value, "producer")),
										  string(value, "provenance"),
										  guarantee(member(value, "guarantee")),
										  condition(value),
										  string(value, "interpretation")};
			check(result.validate());
			return result;
		}
		std::vector<std::byte> bytes(std::string_view input)
		{
			if (input.size() % 2U)
				fail("bytes", "odd-hex-length");
			std::vector<std::byte> result;
			result.reserve(input.size() / 2U);
			auto nibble = [](char c) -> unsigned
			{
				if (c >= '0' && c <= '9')
					return static_cast<unsigned>(c - '0');
				if (c >= 'a' && c <= 'f')
					return static_cast<unsigned>(c - 'a') + 10U;
				fail("bytes", "lowercase-hex-required");
			};
			for (std::size_t i{}; i < input.size(); i += 2U)
				result.push_back(
					static_cast<std::byte>((nibble(input[i]) << 4U) | nibble(input[i + 1U])));
			return result;
		}
		detached_cell cell(const json& value, const value_type& type)
		{
			const auto state = string(value, "state");
			if (state == "absent")
			{
				shape(value, {"state"});
				auto result = detached_cell::absent(type);
				check(result.validate());
				return result;
			}
			if (state == "unknown")
			{
				shape(value, {"reason", "state"});
				auto result = detached_cell::unknown(type, string(value, "reason"));
				check(result.validate());
				return result;
			}
			if (state != "present")
				fail("cell.state", "unsupported-state");
			shape(value, {"state", "type", "value"});
			if (string(value, "type") != type.canonical_name())
				fail("cell.type", "descriptor-mismatch");
			const auto& scalar = member(value, "value");
			scalar_value output;
			switch (type.scalar)
			{
				case scalar_kind::boolean:
					output = boolean(scalar);
					break;
				case scalar_kind::unsigned_integer:
					output = number(scalar);
					break;
				case scalar_kind::signed_integer:
					if (const auto* signed_value = scalar.as_signed_integer())
						output = *signed_value;
					else if (const auto* unsigned_value = scalar.as_unsigned_integer();
							 unsigned_value &&
							 *unsigned_value <= static_cast<std::uint64_t>(
													std::numeric_limits<std::int64_t>::max()))
						output = static_cast<std::int64_t>(*unsigned_value);
					else
						fail("cell.value", "signed-integer-required");
					break;
				case scalar_kind::bytes:
				case scalar_kind::set:
					output = bytes(text_value(scalar, false));
					break;
				default:
					output = text_value(scalar, false);
					break;
			}
			detached_cell result{type, cell_state::present, std::move(output), {}};
			check(result.validate());
			return result;
		}
		annotated_row row(const json& value, const logical_query_ir& ir)
		{
			shape(value,
				  {"values",
				   "multiplicity",
				   "condition_universe",
				   "condition_fragments",
				   "interpretation",
				   "claim_contributors",
				   "producer_contracts",
				   "provenance",
				   "contributor_guarantees",
				   "contributor_edges"});
			annotated_row result;
			result.multiplicity = number(member(value, "multiplicity"));
			result.presence = condition(value);
			result.interpretation = string(value, "interpretation");
			result.claim_contributors = strings(member(value, "claim_contributors"));
			result.producer_contracts = producers(member(value, "producer_contracts"));
			result.provenance = strings(member(value, "provenance"));
			for (const auto& item : array(member(value, "contributor_guarantees")))
				result.contributor_guarantees.push_back(guarantee(item));
			for (const auto& item : array(member(value, "contributor_edges")))
				result.contributor_edges.push_back(edge(item));
			const auto& values = member(value, "values");
			const auto aliases = detail::output_aliases(ir.output_schema);
			if (!values.as_object() || values.as_object()->size() != aliases.size())
				fail("row.values", "column-set");
			for (std::size_t i{}; i < aliases.size(); ++i)
			{
				const auto key = "output." + aliases[i];
				result.values.emplace(key, cell(member(values, key), ir.output_schema[i].type));
			}
			check(result.validate());
			return result;
		}
		query_guarantee_fragment fragment(const json& value)
		{
			shape(value,
				  {"guarantee",
				   "condition_universe",
				   "condition_fragments",
				   "interpretation",
				   "assumptions",
				   "claim_contributors",
				   "producer_contracts",
				   "provenance",
				   "coverage_states",
				   "closure_ids",
				   "condition_partition_complete",
				   "conflicting",
				   "unresolved",
				   "requires_closure"});
			return {guarantee(member(value, "guarantee")),
					condition(value),
					string(value, "interpretation"),
					strings(member(value, "assumptions")),
					strings(member(value, "claim_contributors")),
					producers(member(value, "producer_contracts")),
					strings(member(value, "provenance")),
					strings(member(value, "coverage_states")),
					strings(member(value, "closure_ids")),
					boolean(value, "condition_partition_complete"),
					boolean(value, "conflicting"),
					boolean(value, "unresolved"),
					boolean(value, "requires_closure")};
		}
		query_summary_guarantee summary(const json& value)
		{
			shape(value,
				  {"approximation",
				   "scope",
				   "condition_partition",
				   "interpretation_partitions",
				   "assumptions",
				   "verification_modalities",
				   "fragment_count",
				   "fragment_set_digest",
				   "drill_down_ref",
				   "fragments"});
			query_summary_guarantee result;
			result.approximation = string(value, "approximation");
			if (result.approximation != "exact" && result.approximation != "unknown" &&
				result.approximation != "under_approximation" &&
				result.approximation != "over_approximation")
				fail("summary.approximation", "unsupported-state");
			result.scope = string(value, "scope");
			const auto& partition = member(value, "condition_partition");
			shape(partition, {"universe", "alternatives"});
			result.condition_partition = {string(partition, "universe"),
										  strings(member(partition, "alternatives"))};
			result.interpretation_partitions = strings(member(value, "interpretation_partitions"));
			result.assumptions = strings(member(value, "assumptions"));
			result.verification_modalities = strings(member(value, "verification_modalities"));
			result.fragment_count = number(member(value, "fragment_count"));
			result.fragment_set_digest = string(value, "fragment_set_digest");
			result.drill_down_ref = string(value, "drill_down_ref");
			std::string projection, previous;
			for (const auto& item : array(member(value, "fragments")))
			{
				const auto encoded = sdk::detail::canonical_json(item);
				if (!previous.empty() && previous >= encoded)
					fail("fragments", "canonical-set-required");
				previous = encoded;
				projection += encoded + "\n";
				result.fragments.push_back(fragment(item));
			}
			if (result.fragments.empty() || result.fragment_count != result.fragments.size() ||
				result.fragment_set_digest !=
					take(sdk::semantic_digest("cxxlens.query-guarantee-fragment-set.v1",
											  projection)) ||
				result.drill_down_ref != "fragments:" + result.fragment_set_digest)
				fail("summary.fragments", "digest-or-count-mismatch");
			if (result.interpretation_partitions.empty())
				fail("summary", "empty-interpretations");
			return result;
		}
		query_result decode_result(const json& value,
								   const logical_query_ir& ir,
								   std::string_view relation,
								   std::string_view snapshot,
								   std::size_t& retained_rows,
								   const transfer_limits& limits)
		{
			shape(value,
				  {"schema",
				   "logical_ir_digest",
				   "snapshot_id",
				   "publication_id",
				   "status",
				   "ordered",
				   "rows",
				   "input_coverage",
				   "closure_ids",
				   "unresolved",
				   "conflicts",
				   "differential_disagreements",
				   "producer_contracts",
				   "summary_guarantee",
				   "inputs_complete",
				   "closed",
				   "logical_explanation",
				   "physical_explanation"});
			if (string(value, "schema") != "cxxlens.query-execution-result.v1")
				fail("result.schema", "unsupported-schema");
			auto result = std::make_shared<query_result::data>();
			result->snapshot = string(value, "snapshot_id");
			result->publication = string(value, "publication_id");
			result->ir_digest = string(value, "logical_ir_digest");
			if (result->snapshot != snapshot || !digest(result->publication, true) ||
				result->ir_digest != ir.digest())
				fail("result.binding", "snapshot-publication-or-plan-mismatch");
			const auto status = string(value, "status");
			if (status == "complete")
				result->status = execution_status::complete;
			else if (status == "truncated")
				result->status = execution_status::truncated;
			else if (status == "cancelled_with_partial")
				result->status = execution_status::cancelled_with_partial;
			else if (status == "failed_before_result")
				result->status = execution_status::failed_before_result;
			else
				fail("result.status", "unsupported-status");
			result->ordered = boolean(value, "ordered");
			result->input_complete = boolean(value, "inputs_complete");
			result->closed_world = boolean(value, "closed");
			result->closures = strings(member(value, "closure_ids"));
			for (const auto& closure : result->closures)
				if (!digest(closure, true))
					fail("closure_ids", "typed-digest-required");
			for (const auto& item : array(member(value, "input_coverage")))
			{
				shape(item, {"relation", "domain", "key", "state", "reason"});
				auto relation_id = string(item, "relation");
				if (relation_id != relation)
					fail("coverage.relation", "scan-mismatch");
				result->coverage.push_back({std::move(relation_id),
											{string(item, "domain"),
											 string(item, "key"),
											 string(item, "state"),
											 string(item, "reason", false)}});
			}
			for (const auto& item : array(member(value, "unresolved")))
			{
				shape(item, {"code", "subject", "detail"});
				result->unresolved.push_back({string(item, "code"),
											  string(item, "subject", false),
											  string(item, "detail", false)});
			}
			for (const auto& item : array(member(value, "conflicts")))
			{
				shape(item,
					  {"relation",
					   "semantic_key",
					   "interpretation",
					   "overlap_fragments",
					   "assertions",
					   "contents"});
				claim_conflict conflict{string(item, "relation"),
										string(item, "semantic_key"),
										string(item, "interpretation"),
										strings(member(item, "overlap_fragments")),
										strings(member(item, "assertions")),
										strings(member(item, "contents"))};
				if ((conflict.relation != relation &&
					 conflict.relation != ir.relation_requirements.front().name) ||
					conflict.overlap_fragments.empty() || conflict.assertions.empty() ||
					conflict.contents.empty())
					fail("conflict", "scan-or-empty-evidence");
				result->conflict_values.push_back(std::move(conflict));
			}
			for (const auto& item : array(member(value, "differential_disagreements")))
			{
				shape(item,
					  {"relation",
					   "semantic_key",
					   "left_interpretation",
					   "right_interpretation",
					   "left_content",
					   "right_content",
					   "overlap_fragments"});
				differential_disagreement disagreement{string(item, "relation"),
													   string(item, "semantic_key"),
													   string(item, "left_interpretation"),
													   string(item, "right_interpretation"),
													   string(item, "left_content"),
													   string(item, "right_content"),
													   strings(member(item, "overlap_fragments"))};
				if (disagreement.relation != relation &&
					disagreement.relation != ir.relation_requirements.front().name)
					fail("disagreement.relation", "scan-mismatch");
				result->disagreement_values.push_back(std::move(disagreement));
			}
			result->producers = producers(member(value, "producer_contracts"));
			result->guarantee = summary(member(value, "summary_guarantee"));
			auto explanation = [](const json& item) -> query_explanation
			{
				shape(item, {"id", "text"});
				return {string(item, "id"), string(item, "text")};
			};
			result->logical = explanation(member(value, "logical_explanation"));
			result->physical = explanation(member(value, "physical_explanation"));
			const auto& rows = array(member(value, "rows"));
			if (rows.size() > limits.maximum_rows - retained_rows)
				fail("rows", "row-limit");
			retained_rows += rows.size();
			result->row_values.reserve(rows.size());
			for (const auto& item : rows)
				result->row_values.push_back(row(item, ir));
			if (result->status == execution_status::failed_before_result && !rows.empty())
				fail("status", "failed-result-has-rows");
			if (result->closed_world &&
				(!result->input_complete || result->status != execution_status::complete ||
				 result->closures.empty()))
				fail("closed", "closure-binding-missing");
			if (result->guarantee.approximation == "exact" &&
				(!result->input_complete || result->status != execution_status::complete ||
				 !result->conflict_values.empty() || !result->unresolved.empty() ||
				 (rows.empty() && !result->closed_world)))
				fail("summary", "inconsistent-exact-guarantee");
			return query_transfer_access::make(std::move(result));
		}
	} // namespace

	result<void> transfer_limits::validate() const
	{
		if (!maximum_bytes || maximum_bytes > 512U * 1024U * 1024U || !maximum_depth ||
			maximum_depth > 256U || !maximum_scans || maximum_scans > 4096U || !maximum_rows ||
			maximum_rows > 10000000U || !maximum_values || maximum_values > 64000000U)
			return unexpected(error{"sdk.query-transfer-limit-invalid", "limits", "out-of-range"});
		return {};
	}

	result<application_query_results> decode_application_queries(const relation_engine& engine,
																 std::string_view input,
																 transfer_limits limits)
	{
		try
		{
			check(limits.validate());
			sdk::detail::json_limits json_limits;
			json_limits.max_input_bytes = limits.maximum_bytes;
			json_limits.max_depth = limits.maximum_depth;
			json_limits.max_array_elements = limits.maximum_values;
			json_limits.max_object_members = limits.maximum_values;
			json_limits.max_string_bytes = limits.maximum_bytes;
			json_limits.max_total_string_bytes = limits.maximum_bytes;
			json_limits.max_total_values = limits.maximum_values;
			sdk::detail::json_parse_contract contract;
			contract.error_code = "sdk.query-transfer-invalid";
			const auto root = take(sdk::detail::parse_json_value(input, json_limits, contract));
			shape(root, {"schema", "snapshot_id", "queries"});
			if (string(root, "schema") != "cxxlens.application-query-results.v1")
				fail("schema", "unsupported-schema");
			application_query_results result{string(root, "snapshot_id"), {}};
			if (!digest(result.snapshot_id, true))
				fail("snapshot", "typed-digest-required");
			const auto& queries = array(member(root, "queries"));
			if (queries.empty() || queries.size() > limits.maximum_scans)
				fail("queries", "scan-limit");
			std::size_t retained_rows{};
			std::string previous, publication;
			for (const auto& scan : queries)
			{
				shape(scan, {"relation_id", "logical_ir", "result"});
				auto relation = string(scan, "relation_id");
				if (!previous.empty() && relation <= previous)
					fail("queries", "sorted-unique-scans-required");
				previous = relation;
				const auto descriptor = take(engine.require_id(relation));
				auto builder = take(query::builder::from(descriptor.descriptor()));
				auto ir = std::move(builder).finish();
				const auto expected_plan =
					take(sdk::detail::parse_json_value(ir.canonical_form(), json_limits));
				if (member(scan, "logical_ir") != expected_plan)
					fail("logical_ir", "independent-scan-required");
				auto decoded = decode_result(member(scan, "result"),
											 ir,
											 relation,
											 result.snapshot_id,
											 retained_rows,
											 limits);
				if (!publication.empty() && publication != decoded.publication_id())
					fail("publication", "scan-mismatch");
				publication = decoded.publication_id();
				result.scans.push_back({std::move(relation), std::move(ir), std::move(decoded)});
			}
			return result;
		}
		catch (const decode_failure& failure)
		{
			return unexpected(failure.value);
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(
				error{"sdk.query-transfer-resource-exhausted", "memory", "allocation"});
		}
		catch (const std::length_error&)
		{
			return unexpected(
				error{"sdk.query-transfer-resource-exhausted", "memory", "allocation-length"});
		}
	}
} // namespace cxxlens::sdk::query
