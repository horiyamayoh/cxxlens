#include <algorithm>
#include <cctype>
#include <ranges>
#include <set>
#include <sstream>
#include <tuple>

#include <cxxlens/sdk/claim.hpp>

namespace cxxlens::sdk
{
	namespace
	{
		[[nodiscard]] error
		claim_error(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool sorted_unique(const std::vector<std::string>& values)
		{
			return std::ranges::is_sorted(values) &&
				std::ranges::adjacent_find(values) == values.end();
		}

		[[nodiscard]] bool sha256_digest(const std::string_view value)
		{
			if (value.size() != 71U || !value.starts_with("sha256:"))
				return false;
			return std::ranges::all_of(value.substr(7U),
									   [](const char byte)
									   {
										   return std::isdigit(static_cast<unsigned char>(byte)) !=
											   0 ||
											   (byte >= 'a' && byte <= 'f');
									   });
		}

		[[nodiscard]] bool typed_sha256_digest(const std::string_view value)
		{
			const auto separator = value.find(':');
			return separator != std::string_view::npos && separator != 0U &&
				sha256_digest(value.substr(separator + 1U));
		}

		[[nodiscard]] std::string
		typed_digest(std::string_view type, std::string_view domain, std::string_view bytes)
		{
			return std::string{type} + ':' + semantic_digest(domain, bytes);
		}

		[[nodiscard]] result<std::string> row_tuple(const detached_row& row,
													std::span<const std::string> columns)
		{
			std::ostringstream output;
			for (const auto& column : columns)
			{
				const auto found = row.cells.find(column);
				if (found == row.cells.end())
				{
					output << column.size() << ':' << column << "=7:omitted;";
					continue;
				}
				const auto cell = found->second.canonical_form();
				output << column.size() << ':' << column << '=' << cell.size() << ':' << cell
					   << ';';
			}
			return output.str();
		}

		[[nodiscard]] result<std::string> conflict_payload(const relation_descriptor& descriptor,
														   const detached_row& row)
		{
			auto tuple = row_tuple(row, descriptor.conflict_columns);
			if (!tuple)
				return unexpected(std::move(tuple.error()));
			return semantic_digest("cxxlens.conflict-payload.v1", *tuple);
		}

		[[nodiscard]] result<void> validate_basis(const claim_input_basis& basis,
												  const claim_stage stage)
		{
			if (const auto* direct = std::get_if<direct_claim_basis>(&basis))
			{
				if (stage == claim_stage::derived_claim || !sha256_digest(direct->basis_digest))
					return unexpected(claim_error("sdk.claim-basis-invalid", "direct"));
				return {};
			}
			const auto& derived = std::get<derived_claim_basis>(basis);
			if (stage == claim_stage::assertion || derived.input_snapshot.empty() ||
				derived.consumed_partition_content_digests.empty() ||
				!sorted_unique(derived.consumed_partition_content_digests) ||
				!std::ranges::all_of(derived.consumed_partition_content_digests,
									 typed_sha256_digest) ||
				!sha256_digest(derived.transform_semantics))
				return unexpected(claim_error("sdk.claim-basis-invalid", "derived"));
			return {};
		}

		[[nodiscard]] result<claim> encode_claim(const relation_descriptor& descriptor,
												 detached_row row,
												 claim_condition presence,
												 std::string interpretation,
												 claim_stage stage,
												 claim_producer producer,
												 claim_input_basis basis,
												 std::string provenance,
												 claim_guarantee guarantee)
		{
			if (auto valid = validate_row(descriptor, row); !valid)
				return unexpected(std::move(valid.error()));
			if (auto valid = presence.validate(); !valid)
				return unexpected(std::move(valid.error()));
			if (producer.id.empty() || producer.semantic_contract.empty() ||
				interpretation.empty() || provenance.empty())
				return unexpected(claim_error("sdk.claim-envelope-invalid", "identity"));
			if (auto valid = guarantee.validate(); !valid)
				return unexpected(std::move(valid.error()));
			if (auto valid = validate_basis(basis, stage); !valid)
				return unexpected(std::move(valid.error()));

			auto key_tuple = row_tuple(row, descriptor.key_columns);
			if (!key_tuple)
				return unexpected(std::move(key_tuple.error()));
			std::vector<std::string> payload_columns;
			for (const auto& column : descriptor.columns)
				if (column.role == column_role::authoritative_payload)
					payload_columns.push_back(column.id);
			std::ranges::sort(payload_columns);
			auto payload_tuple = row_tuple(row, payload_columns);
			if (!payload_tuple)
				return unexpected(std::move(payload_tuple.error()));

			claim output;
			output.row = std::move(row);
			output.descriptor = descriptor.id;
			output.semantic_key = typed_digest(
				"semantic-key", "cxxlens.semantic-key.v1", descriptor.id + "\n" + *key_tuple);
			output.assertion = typed_digest("assertion",
											"cxxlens.assertion.v1",
											output.semantic_key + "\n" + presence.universe + "\n" +
												presence.canonical_form() + "\n" + interpretation +
												"\n" + producer.semantic_contract);
			output.content = typed_digest(
				"content", "cxxlens.claim-content.v1", output.assertion + "\n" + *payload_tuple);
			output.presence = std::move(presence);
			output.interpretation = std::move(interpretation);
			output.stage = stage;
			output.producer = std::move(producer);
			output.input_basis = std::move(basis);
			output.provenance_root = std::move(provenance);
			output.guarantee = std::move(guarantee);
			return output;
		}

		[[nodiscard]] result<relation_descriptor> descriptor_for(const relation_engine& engine,
																 const claim& value)
		{
			auto relation = engine.require_id(value.descriptor);
			if (!relation)
				return unexpected(std::move(relation.error()));
			return relation->descriptor();
		}

		[[nodiscard]] bool reference_match(const claim& source,
										   const relation_reference_descriptor& reference,
										   const claim& target)
		{
			if (source.interpretation != target.interpretation ||
				source.presence.universe != target.presence.universe ||
				!std::ranges::includes(target.presence.fragments, source.presence.fragments))
				return false;
			for (std::size_t index = 0U; index < reference.source_columns.size(); ++index)
			{
				const auto left = source.row.cells.find(reference.source_columns[index]);
				const auto right = target.row.cells.find(reference.target_columns[index]);
				if (left == source.row.cells.end() || right == target.row.cells.end() ||
					left->second.state != cell_state::present ||
					right->second.state != cell_state::present || !left->second.value ||
					!right->second.value || left->second.value != right->second.value)
					return false;
			}
			return true;
		}

		[[nodiscard]] bool reference_absent(const claim& source,
											const relation_reference_descriptor& reference)
		{
			return std::ranges::any_of(reference.source_columns,
									   [&](const auto& column)
									   {
										   const auto found = source.row.cells.find(column);
										   return found == source.row.cells.end() ||
											   found->second.state == cell_state::absent;
									   });
		}

		[[nodiscard]] bool claim_order(const claim& left, const claim& right)
		{
			return std::tie(left.descriptor,
							left.semantic_key,
							left.interpretation,
							left.assertion,
							left.content) < std::tie(right.descriptor,
													 right.semantic_key,
													 right.interpretation,
													 right.assertion,
													 right.content);
		}
	} // namespace

	result<void> claim_condition::validate() const
	{
		if (universe.empty() || fragments.empty() || !sorted_unique(fragments) ||
			std::ranges::any_of(fragments, &std::string::empty))
			return unexpected(claim_error("sdk.claim-condition-invalid", "condition"));
		return {};
	}

	std::string claim_condition::canonical_form() const
	{
		std::ostringstream output;
		output << universe.size() << ':' << universe;
		for (const auto& fragment : fragments)
			output << ';' << fragment.size() << ':' << fragment;
		return output.str();
	}

	std::string claim_condition::id() const
	{
		return typed_digest("condition", "cxxlens.condition.v1", canonical_form());
	}

	result<std::vector<std::string>> claim_condition::overlap(const claim_condition& other) const
	{
		std::vector<std::string> output;
		if (universe != other.universe)
			return unexpected(
				claim_error("sdk.condition-universe-mismatch", universe, other.universe));
		std::ranges::set_intersection(fragments, other.fragments, std::back_inserter(output));
		return output;
	}

	result<void> claim_guarantee::validate() const
	{
		static const std::set<std::string, std::less<>> approximations{
			"unknown", "under_approximation", "over_approximation", "exact"};
		if (!approximations.contains(approximation) || scope.empty() || assumptions.empty() ||
			!sorted_unique(verification_modalities))
			return unexpected(claim_error("sdk.claim-guarantee-invalid", "guarantee"));
		return {};
	}

	result<claim> make_assertion(const relation_engine& engine, observation value)
	{
		auto relation = engine.require_id(value.row.descriptor_id);
		if (!relation)
			return unexpected(std::move(relation.error()));
		return encode_claim(relation->descriptor(),
							std::move(value.row),
							std::move(value.presence),
							std::move(value.interpretation),
							claim_stage::assertion,
							std::move(value.producer),
							claim_input_basis{std::move(value.input_basis)},
							std::move(value.provenance_root),
							std::move(value.guarantee));
	}

	result<claim> make_canonical_claim(const relation_engine& engine,
									   const claim& input,
									   claim_producer canonicalizer,
									   detached_row canonical_row,
									   const std::string& transform_semantics)
	{
		if (input.stage != claim_stage::assertion || !sha256_digest(transform_semantics))
			return unexpected(claim_error("sdk.claim-stage-invalid", "canonical_claim"));
		auto descriptor = descriptor_for(engine, input);
		if (!descriptor || canonical_row.descriptor_id != input.descriptor)
			return unexpected(descriptor ? claim_error("sdk.row-descriptor-mismatch", "descriptor")
										 : std::move(descriptor.error()));
		direct_claim_basis basis{semantic_digest("cxxlens.canonical-input-basis.v1",
												 input.content + "\n" + transform_semantics)};
		return encode_claim(*descriptor,
							std::move(canonical_row),
							input.presence,
							input.interpretation,
							claim_stage::canonical_claim,
							std::move(canonicalizer),
							std::move(basis),
							input.provenance_root,
							input.guarantee);
	}

	result<claim> make_derived_claim(const relation_engine& engine,
									 std::span<const claim> inputs,
									 observation output,
									 std::string input_snapshot,
									 std::string transform_semantics)
	{
		if (inputs.empty())
			return unexpected(claim_error("sdk.claim-basis-invalid", "inputs"));
		std::vector<std::string> contents;
		contents.reserve(inputs.size());
		for (const auto& input : inputs)
		{
			if (auto valid = validate_claim(engine, input); !valid)
				return unexpected(std::move(valid.error()));
			contents.push_back(input.content);
		}
		std::ranges::sort(contents);
		contents.erase(std::unique(contents.begin(), contents.end()), contents.end());
		auto relation = engine.require_id(output.row.descriptor_id);
		if (!relation)
			return unexpected(std::move(relation.error()));
		derived_claim_basis basis{
			std::move(input_snapshot), std::move(contents), std::move(transform_semantics)};
		return encode_claim(relation->descriptor(),
							std::move(output.row),
							std::move(output.presence),
							std::move(output.interpretation),
							claim_stage::derived_claim,
							std::move(output.producer),
							std::move(basis),
							std::move(output.provenance_root),
							std::move(output.guarantee));
	}

	result<void> validate_claim(const relation_engine& engine, const claim& value)
	{
		auto descriptor = descriptor_for(engine, value);
		if (!descriptor)
			return unexpected(std::move(descriptor.error()));
		auto rebuilt = encode_claim(*descriptor,
									value.row,
									value.presence,
									value.interpretation,
									value.stage,
									value.producer,
									value.input_basis,
									value.provenance_root,
									value.guarantee);
		if (!rebuilt)
			return unexpected(std::move(rebuilt.error()));
		if (rebuilt->semantic_key != value.semantic_key || rebuilt->assertion != value.assertion ||
			rebuilt->content != value.content || value.descriptor != value.row.descriptor_id)
			return unexpected(claim_error("sdk.claim-identity-mismatch", "claim"));
		return {};
	}

	result<void> claim_batch::add(claim value)
	{
		claims_.push_back(std::move(value));
		return {};
	}

	result<void> claim_batch::add_observation(const relation_engine& engine, observation value)
	{
		auto assertion = make_assertion(engine, std::move(value));
		if (!assertion)
			return unexpected(std::move(assertion.error()));
		return add(std::move(*assertion));
	}

	result<claim_batch_result> claim_batch::commit(const relation_engine& engine,
												   std::span<const claim> existing) &&
	{
		for (const auto& value : claims_)
			if (auto valid = validate_claim(engine, value); !valid)
				return unexpected(std::move(valid.error()));
		for (const auto& value : existing)
			if (auto valid = validate_claim(engine, value); !valid)
				return unexpected(std::move(valid.error()));

		claim_batch_result output;
		std::vector<const claim*> reference_space;
		reference_space.reserve(claims_.size() + existing.size());
		for (const auto& value : existing)
			reference_space.push_back(&value);
		for (const auto& value : claims_)
			reference_space.push_back(&value);

		for (const auto& value : claims_)
		{
			auto descriptor = descriptor_for(engine, value);
			if (!descriptor)
				return unexpected(std::move(descriptor.error()));
			for (const auto& reference : descriptor->references)
			{
				if (reference_absent(value, reference))
					continue;
				const bool resolved = std::ranges::any_of(
					reference_space,
					[&](const claim* target)
					{
						auto target_descriptor = descriptor_for(engine, *target);
						return target_descriptor &&
							target_descriptor->name == reference.target_relation &&
							reference_match(value, reference, *target);
					});
				if (!resolved && reference.strength == reference_strength::hard)
					return unexpected(claim_error(
						"sdk.hard-reference-missing", value.assertion, reference.target_relation));
				if (!resolved)
					output.unresolved.push_back({value.assertion,
												 value.descriptor,
												 reference.target_relation,
												 reference.source_columns,
												 "soft-reference-missing"});
			}
		}

		std::ranges::sort(claims_, claim_order);
		for (const auto& value : claims_)
		{
			auto descriptor = descriptor_for(engine, value);
			if (!descriptor)
				return unexpected(std::move(descriptor.error()));
			const bool deduplicate = descriptor->merge != merge_mode::multiset;
			if (!deduplicate || output.claims.empty() ||
				output.claims.back().content != value.content)
				output.claims.push_back(value);
		}

		for (std::size_t left = 0U; left < output.claims.size(); ++left)
			for (std::size_t right = left + 1U; right < output.claims.size(); ++right)
			{
				const auto& first = output.claims[left];
				const auto& second = output.claims[right];
				if (first.descriptor != second.descriptor ||
					first.semantic_key != second.semantic_key)
					continue;
				auto descriptor = descriptor_for(engine, first);
				if (!descriptor || descriptor->merge != merge_mode::functional_assertion)
					continue;
				auto first_payload = conflict_payload(*descriptor, first.row);
				auto second_payload = conflict_payload(*descriptor, second.row);
				if (!first_payload || !second_payload)
					return unexpected(!first_payload ? std::move(first_payload.error())
													 : std::move(second_payload.error()));
				if (*first_payload == *second_payload)
					continue;
				auto overlap = first.presence.overlap(second.presence);
				if (!overlap)
					return unexpected(std::move(overlap.error()));
				if (overlap->empty())
					continue;
				if (first.interpretation == second.interpretation)
					output.conflicts.push_back({descriptor->name,
												first.semantic_key,
												first.interpretation,
												std::move(*overlap),
												{first.assertion, second.assertion},
												{first.content, second.content}});
				else
					output.differential_disagreements.push_back({descriptor->name,
																 first.semantic_key,
																 first.interpretation,
																 second.interpretation,
																 first.content,
																 second.content,
																 std::move(*overlap)});
			}

		std::ranges::sort(
			output.unresolved,
			[](const auto& left, const auto& right)
			{
				return std::tie(left.source_assertion, left.target_relation, left.source_columns) <
					std::tie(right.source_assertion, right.target_relation, right.source_columns);
			});
		std::string canonical;
		for (const auto& value : output.claims)
			canonical += value.content + "\n";
		for (const auto& value : output.unresolved)
			canonical += value.source_assertion + "->" + value.target_relation + "\n";
		for (const auto& value : output.conflicts)
		{
			canonical += "conflict:" + value.relation + ':' + value.semantic_key + ':' +
				value.interpretation + '\n';
			for (const auto& fragment : value.overlap_fragments)
				canonical += "condition:" + fragment + '\n';
			for (const auto& content : value.contents)
				canonical += "content:" + content + '\n';
		}
		for (const auto& value : output.differential_disagreements)
		{
			canonical += "differential:" + value.relation + ':' + value.semantic_key + ':' +
				value.left_interpretation + ':' + value.right_interpretation + ':' +
				value.left_content + ':' + value.right_content + '\n';
			for (const auto& fragment : value.overlap_fragments)
				canonical += "condition:" + fragment + '\n';
		}
		output.content_digest = semantic_digest("cxxlens.claim-batch.v1", canonical);
		return output;
	}
} // namespace cxxlens::sdk
