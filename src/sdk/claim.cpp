#include <algorithm>
#include <array>
#include <cctype>
#include <ranges>
#include <set>
#include <sstream>
#include <tuple>
#include <type_traits>

#include <cxxlens/sdk/claim.hpp>

#include "claim_internal.hpp"
#include "json_internal.hpp"

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

		[[nodiscard]] result<void> claim_strong_id(const std::string_view value, std::string field)
		{
			auto valid = validate_strong_id(value);
			if (!valid)
				return unexpected(
					error{"sdk.text-invalid", std::move(field), std::move(valid.error().detail)});
			return {};
		}

		[[nodiscard]] result<void> claim_registered_symbol(const std::string_view value,
														   std::string field)
		{
			auto valid = validate_registered_symbol(value);
			if (!valid)
				return unexpected(
					error{"sdk.text-invalid", std::move(field), std::move(valid.error().detail)});
			return {};
		}

		[[nodiscard]] std::string json_string(const std::string_view value)
		{
			return detail::canonical_json_string(value);
		}

		[[nodiscard]] std::string strings_json(const std::vector<std::string>& values)
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

		[[nodiscard]] bool sha256_digest(const std::string_view value)
		{
			const auto hex = value.starts_with("sha256:")  ? value.substr(7U)
				: value.starts_with("semantic-v2:sha256:") ? value.substr(19U)
														   : std::string_view{};
			if (hex.size() != 64U)
				return false;
			return std::ranges::all_of(hex,
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
				value.substr(separator + 1U).starts_with("sha256:") &&
				sha256_digest(value.substr(separator + 1U));
		}

		[[nodiscard]] bool partition_content_digest(const std::string_view value)
		{
			return value.starts_with("partition-content:") && typed_sha256_digest(value);
		}

		[[nodiscard]] canonical_value cell_value(const detached_cell& cell)
		{
			if (cell.state == cell_state::absent)
				return canonical_value::null();
			if (cell.state == cell_state::unknown)
				return canonical_value::from_tuple(
					{canonical_value::from_string("unknown"),
					 canonical_value::from_string(cell.unknown_reason.value_or("unspecified"))});
			if (!cell.value)
				return canonical_value::null();
			return std::visit(
				[](const auto& value) -> canonical_value
				{
					using value_type = std::decay_t<decltype(value)>;
					if constexpr (std::is_same_v<value_type, bool>)
						return canonical_value::from_boolean(value);
					else if constexpr (std::is_same_v<value_type, std::int64_t>)
						return canonical_value::from_integer(value);
					else if constexpr (std::is_same_v<value_type, std::uint64_t>)
						return canonical_value::from_tuple(
							{canonical_value::from_string("unsigned"),
							 canonical_value::from_string(std::to_string(value))});
					else if constexpr (std::is_same_v<value_type, std::string>)
						return canonical_value::from_string(value);
					else
						return canonical_value::from_bytes(value);
				},
				*cell.value);
		}

		[[nodiscard]] result<canonical_value> row_tuple(const detached_row& row,
														std::span<const std::string> columns)
		{
			std::vector<canonical_value> output;
			output.reserve(columns.size());
			for (const auto& column : columns)
			{
				const auto found = row.cells.find(column);
				if (found == row.cells.end())
				{
					output.push_back(canonical_value::null());
					continue;
				}
				output.push_back(cell_value(found->second));
			}
			return canonical_value::from_tuple(std::move(output));
		}

		[[nodiscard]] result<void> validate_basis(const claim_input_basis& basis,
												  const claim_stage stage)
		{
			if (!is_valid(stage))
				return unexpected(claim_error("sdk.claim-basis-invalid", "stage", "closed-enum"));
			if (const auto* direct = std::get_if<direct_claim_basis>(&basis))
			{
				if (auto valid = claim_strong_id(direct->basis_digest, "basis_digest"); !valid)
					return valid;
				if (stage == claim_stage::derived_claim || !sha256_digest(direct->basis_digest))
					return unexpected(claim_error("sdk.claim-basis-invalid", "direct"));
				return {};
			}
			const auto& derived = std::get<derived_claim_basis>(basis);
			if (stage == claim_stage::assertion)
				return unexpected(claim_error("sdk.claim-basis-invalid", "derived"));
			if (auto valid = claim_strong_id(derived.input_snapshot, "input_snapshot"); !valid)
				return valid;
			for (const auto& digest : derived.consumed_partition_content_digests)
				if (auto valid = claim_strong_id(digest, "consumed_partition_content_digest");
					!valid)
					return valid;
			if (auto valid = claim_strong_id(derived.transform_semantics, "transform_semantics");
				!valid)
				return valid;
			if (derived.consumed_partition_content_digests.empty() ||
				!sorted_unique(derived.consumed_partition_content_digests) ||
				!std::ranges::all_of(derived.consumed_partition_content_digests,
									 partition_content_digest) ||
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
			for (const auto& [text, field] : {
					 std::pair<std::string_view, std::string_view>{producer.id, "producer.id"},
					 {producer.semantic_contract, "producer.semantic_contract"},
					 {interpretation, "interpretation"},
					 {provenance, "provenance_root"},
				 })
				if (auto valid = claim_strong_id(text, std::string{field}); !valid)
					return unexpected(std::move(valid.error()));
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
			const std::array key_fields{
				canonical_value::from_string(descriptor.id),
				canonical_value::from_integer(descriptor.semantic_major),
				*key_tuple,
			};
			auto semantic_key = canonical_identity_digest("semantic-key", key_fields);
			if (!semantic_key)
				return unexpected(std::move(semantic_key.error()));
			output.semantic_key = std::move(*semantic_key);
			const std::array assertion_fields{
				canonical_value::from_string(output.semantic_key),
				canonical_value::from_string(presence.universe),
				canonical_value::from_string(presence.canonical_form()),
				canonical_value::from_string(interpretation),
				canonical_value::from_string(producer.semantic_contract),
			};
			auto assertion = canonical_identity_digest("assertion", assertion_fields);
			if (!assertion)
				return unexpected(std::move(assertion.error()));
			output.assertion = std::move(*assertion);
			const std::array content_fields{
				canonical_value::from_string(output.assertion),
				*payload_tuple,
			};
			auto content = canonical_identity_digest("claim-content", content_fields);
			if (!content)
				return unexpected(std::move(content.error()));
			output.content = std::move(*content);
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

		[[nodiscard]] result<void> validate_claim_inputs(const relation_engine& engine,
														 const std::span<const claim> inputs)
		{
			for (const auto& input : inputs)
				if (auto valid = validate_claim(engine, input); !valid)
					return unexpected(std::move(valid.error()));
			return {};
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
			return detail::claim_occurrence_less(left, right);
		}
	} // namespace

	namespace detail
	{
		result<std::vector<std::byte>> claim_occurrence_projection(const claim& value)
		{
			std::vector<canonical_value> basis;
			if (const auto* direct = std::get_if<direct_claim_basis>(&value.input_basis))
				basis = {canonical_value::from_string("direct"),
						 canonical_value::from_string(direct->basis_digest)};
			else
			{
				const auto& derived = std::get<derived_claim_basis>(value.input_basis);
				std::vector<canonical_value> consumed;
				consumed.reserve(derived.consumed_partition_content_digests.size());
				for (const auto& digest : derived.consumed_partition_content_digests)
					consumed.push_back(canonical_value::from_string(digest));
				basis = {canonical_value::from_string("derived"),
						 canonical_value::from_string(derived.input_snapshot),
						 canonical_value::from_tuple(std::move(consumed)),
						 canonical_value::from_string(derived.transform_semantics)};
			}
			std::vector<canonical_value> modalities;
			modalities.reserve(value.guarantee.verification_modalities.size());
			for (const auto& modality : value.guarantee.verification_modalities)
				modalities.push_back(canonical_value::from_string(modality));
			return *canonical_binary(canonical_value::from_tuple(
				{canonical_value::from_string(value.descriptor),
				 canonical_value::from_string(value.semantic_key),
				 canonical_value::from_string(value.interpretation),
				 canonical_value::from_string(value.assertion),
				 canonical_value::from_string(value.content),
				 canonical_value::from_string(value.row.canonical_form()),
				 canonical_value::from_string(value.presence.canonical_form()),
				 canonical_value::from_integer(static_cast<std::int64_t>(value.stage)),
				 canonical_value::from_string(value.producer.id),
				 canonical_value::from_string(value.producer.semantic_contract),
				 canonical_value::from_tuple(std::move(basis)),
				 canonical_value::from_string(value.provenance_root),
				 canonical_value::from_string(value.guarantee.approximation),
				 canonical_value::from_string(value.guarantee.scope),
				 canonical_value::from_string(value.guarantee.assumptions),
				 canonical_value::from_tuple(std::move(modalities))}));
		}

		bool claim_occurrence_less(const claim& left, const claim& right)
		{
			return *claim_occurrence_projection(left) < *claim_occurrence_projection(right);
		}

		bool same_claim_occurrence(const claim& left, const claim& right)
		{
			return *claim_occurrence_projection(left) == *claim_occurrence_projection(right);
		}

		result<std::string> functional_payload_digest(const relation_descriptor& descriptor,
													  const detached_row& row)
		{
			auto tuple = row_tuple(row, descriptor.conflict_columns);
			if (!tuple)
				return unexpected(std::move(tuple.error()));
			const std::array fields{*tuple};
			return canonical_identity_digest("conflict-payload", fields);
		}

		void canonicalize_claim_conflicts(std::vector<claim_conflict>& values)
		{
			for (auto& value : values)
			{
				std::ranges::sort(value.overlap_fragments);
				value.overlap_fragments.erase(std::ranges::unique(value.overlap_fragments).begin(),
											  value.overlap_fragments.end());
				std::ranges::sort(value.assertions);
				value.assertions.erase(std::ranges::unique(value.assertions).begin(),
									   value.assertions.end());
				std::ranges::sort(value.contents);
				value.contents.erase(std::ranges::unique(value.contents).begin(),
									 value.contents.end());
			}
			std::ranges::sort(values,
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
			values.erase(
				std::ranges::unique(values,
									[](const claim_conflict& left, const claim_conflict& right)
									{
										return left.relation == right.relation &&
											left.semantic_key == right.semantic_key &&
											left.interpretation == right.interpretation &&
											left.overlap_fragments == right.overlap_fragments &&
											left.assertions == right.assertions &&
											left.contents == right.contents;
									})
					.begin(),
				values.end());
		}

		std::string canonical_claim_conflict_json(const claim_conflict& value)
		{
			std::vector canonical{value};
			canonicalize_claim_conflicts(canonical);
			const auto& normalized = canonical.front();
			return "{\"assertions\":" + strings_json(normalized.assertions) +
				",\"contents\":" + strings_json(normalized.contents) +
				",\"interpretation\":" + json_string(normalized.interpretation) +
				",\"overlap_fragments\":" + strings_json(normalized.overlap_fragments) +
				",\"relation\":" + json_string(normalized.relation) +
				",\"semantic_key\":" + json_string(normalized.semantic_key) + "}";
		}
	} // namespace detail

	result<void> claim_condition::validate() const
	{
		if (fragments.empty() || !sorted_unique(fragments))
			return unexpected(claim_error("sdk.claim-condition-invalid", "condition"));
		if (auto valid = claim_strong_id(universe, "condition.universe"); !valid)
			return valid;
		for (const auto& fragment : fragments)
			if (auto valid = claim_strong_id(fragment, "condition.fragment"); !valid)
				return valid;
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
		if (!validate())
			return {};
		const std::array fields{canonical_value::from_string(canonical_form())};
		auto identity = canonical_identity_digest("condition", fields);
		return identity ? std::move(*identity) : std::string{};
	}

	result<std::vector<std::string>> claim_condition::overlap(const claim_condition& other) const
	{
		if (auto valid = validate(); !valid)
			return unexpected(std::move(valid.error()));
		if (auto valid = other.validate(); !valid)
			return unexpected(std::move(valid.error()));
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
		if (auto valid = claim_registered_symbol(approximation, "guarantee.approximation"); !valid)
			return valid;
		if (!approximations.contains(approximation) || !sorted_unique(verification_modalities))
			return unexpected(claim_error("sdk.claim-guarantee-invalid", "guarantee"));
		if (auto valid = claim_strong_id(scope, "guarantee.scope"); !valid)
			return valid;
		if (auto valid = claim_strong_id(assumptions, "guarantee.assumptions"); !valid)
			return valid;
		for (const auto& modality : verification_modalities)
			if (auto valid = claim_registered_symbol(modality, "guarantee.verification_modality");
				!valid)
				return valid;
		return {};
	}

	result<std::string> claim_input_basis_digest(const claim_input_basis& basis)
	{
		if (const auto* direct = std::get_if<direct_claim_basis>(&basis))
		{
			if (auto valid = claim_strong_id(direct->basis_digest, "basis_digest"); !valid)
				return unexpected(std::move(valid.error()));
			if (!sha256_digest(direct->basis_digest))
				return unexpected(claim_error("sdk.claim-basis-invalid", "direct"));
			const std::array fields{canonical_value::from_string(direct->basis_digest)};
			const auto typed = canonical_identity_digest("producer-input-direct", fields);
			if (!typed)
				return unexpected(std::move(typed.error()));
			return typed->substr(typed->find(':') + 1U);
		}
		const auto& derived = std::get<derived_claim_basis>(basis);
		if (auto valid = claim_strong_id(derived.input_snapshot, "input_snapshot"); !valid)
			return unexpected(std::move(valid.error()));
		for (const auto& digest : derived.consumed_partition_content_digests)
			if (auto valid = claim_strong_id(digest, "consumed_partition_content_digest"); !valid)
				return unexpected(std::move(valid.error()));
		if (auto valid = claim_strong_id(derived.transform_semantics, "transform_semantics");
			!valid)
			return unexpected(std::move(valid.error()));
		if (derived.consumed_partition_content_digests.empty() ||
			!sorted_unique(derived.consumed_partition_content_digests) ||
			!std::ranges::all_of(derived.consumed_partition_content_digests,
								 partition_content_digest) ||
			!sha256_digest(derived.transform_semantics))
			return unexpected(claim_error("sdk.claim-basis-invalid", "derived"));
		std::vector<canonical_value> consumed;
		consumed.reserve(derived.consumed_partition_content_digests.size());
		for (const auto& value : derived.consumed_partition_content_digests)
			consumed.push_back(canonical_value::from_string(value));
		const std::array fields{
			canonical_value::from_string(derived.input_snapshot),
			canonical_value::from_tuple(std::move(consumed)),
			canonical_value::from_string(derived.transform_semantics),
		};
		const auto typed = canonical_identity_digest("producer-input-derived", fields);
		if (!typed)
			return unexpected(std::move(typed.error()));
		return typed->substr(typed->find(':') + 1U);
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
		if (auto valid = validate_claim_inputs(engine, std::span<const claim>{&input, 1U}); !valid)
			return unexpected(std::move(valid.error()));
		if (input.stage != claim_stage::assertion || !sha256_digest(transform_semantics))
			return unexpected(claim_error("sdk.claim-stage-invalid", "canonical_claim"));
		auto descriptor = descriptor_for(engine, input);
		if (!descriptor || canonical_row.descriptor_id != input.descriptor)
			return unexpected(descriptor ? claim_error("sdk.row-descriptor-mismatch", "descriptor")
										 : std::move(descriptor.error()));
		direct_claim_basis basis{*semantic_digest("cxxlens.canonical-input-basis.v1",
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
									 std::vector<std::string> consumed_partition_content_digests,
									 std::string transform_semantics)
	{
		if (inputs.empty())
			return unexpected(claim_error("sdk.claim-basis-invalid", "inputs"));
		if (auto valid = validate_claim_inputs(engine, inputs); !valid)
			return unexpected(std::move(valid.error()));
		std::ranges::sort(consumed_partition_content_digests);
		consumed_partition_content_digests.erase(
			std::unique(consumed_partition_content_digests.begin(),
						consumed_partition_content_digests.end()),
			consumed_partition_content_digests.end());
		auto relation = engine.require_id(output.row.descriptor_id);
		if (!relation)
			return unexpected(std::move(relation.error()));
		derived_claim_basis basis{std::move(input_snapshot),
								  std::move(consumed_partition_content_digests),
								  std::move(transform_semantics)};
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

	result<std::vector<std::byte>> claim_batch_content_encoding(
		const std::span<const claim> claims,
		const std::span<const unresolved_reference> unresolved,
		const std::span<const claim_conflict> conflicts,
		const std::span<const differential_disagreement> differential_disagreements)
	{
		const auto strings = [](const auto& values)
		{
			std::vector<canonical_value> output;
			output.reserve(values.size());
			for (const auto& value : values)
				output.push_back(canonical_value::from_string(value));
			return canonical_value::from_tuple(std::move(output));
		};

		std::vector<std::pair<const claim*, std::vector<std::byte>>> ordered_claims;
		ordered_claims.reserve(claims.size());
		for (const auto& value : claims)
		{
			auto occurrence = detail::claim_occurrence_projection(value);
			if (!occurrence)
				return unexpected(std::move(occurrence.error()));
			ordered_claims.emplace_back(&value, std::move(*occurrence));
		}
		std::ranges::sort(ordered_claims,
						  [](const auto& left, const auto& right)
						  {
							  return left.second < right.second;
						  });
		std::vector<canonical_value> claim_records;
		claim_records.reserve(ordered_claims.size());
		for (auto& [value, occurrence] : ordered_claims)
			claim_records.push_back(canonical_value::from_tuple({
				canonical_value::from_string("claim"),
				canonical_value::from_string(value->content),
				canonical_value::from_bytes(std::move(occurrence)),
			}));

		auto ordered_unresolved =
			std::vector<unresolved_reference>{unresolved.begin(), unresolved.end()};
		std::ranges::sort(ordered_unresolved,
						  [](const auto& left, const auto& right)
						  {
							  return std::tie(left.source_assertion,
											  left.source_relation,
											  left.target_relation,
											  left.source_columns,
											  left.reason) < std::tie(right.source_assertion,
																	  right.source_relation,
																	  right.target_relation,
																	  right.source_columns,
																	  right.reason);
						  });
		ordered_unresolved.erase(
			std::ranges::unique(ordered_unresolved,
								[](const auto& left, const auto& right)
								{
									return left.source_assertion == right.source_assertion &&
										left.source_relation == right.source_relation &&
										left.target_relation == right.target_relation &&
										left.source_columns == right.source_columns &&
										left.reason == right.reason;
								})
				.begin(),
			ordered_unresolved.end());
		std::vector<canonical_value> unresolved_records;
		unresolved_records.reserve(ordered_unresolved.size());
		for (const auto& value : ordered_unresolved)
			unresolved_records.push_back(canonical_value::from_tuple({
				canonical_value::from_string("unresolved"),
				canonical_value::from_string(value.source_assertion),
				canonical_value::from_string(value.source_relation),
				canonical_value::from_string(value.target_relation),
				strings(value.source_columns),
				canonical_value::from_string(value.reason),
			}));

		auto ordered_conflicts = std::vector<claim_conflict>{conflicts.begin(), conflicts.end()};
		detail::canonicalize_claim_conflicts(ordered_conflicts);
		std::vector<canonical_value> conflict_records;
		conflict_records.reserve(ordered_conflicts.size());
		for (const auto& value : ordered_conflicts)
			conflict_records.push_back(canonical_value::from_tuple({
				canonical_value::from_string("conflict"),
				canonical_value::from_string(value.relation),
				canonical_value::from_string(value.semantic_key),
				canonical_value::from_string(value.interpretation),
				strings(value.overlap_fragments),
				strings(value.assertions),
				strings(value.contents),
			}));

		auto ordered_differential = std::vector<differential_disagreement>{
			differential_disagreements.begin(), differential_disagreements.end()};
		std::ranges::sort(
			ordered_differential,
			[](const differential_disagreement& left, const differential_disagreement& right)
			{
				return std::tie(left.relation,
								left.semantic_key,
								left.left_interpretation,
								left.right_interpretation,
								left.left_content,
								left.right_content,
								left.overlap_fragments) < std::tie(right.relation,
																   right.semantic_key,
																   right.left_interpretation,
																   right.right_interpretation,
																   right.left_content,
																   right.right_content,
																   right.overlap_fragments);
			});
		ordered_differential.erase(
			std::ranges::unique(
				ordered_differential,
				[](const differential_disagreement& left, const differential_disagreement& right)
				{
					return left.relation == right.relation &&
						left.semantic_key == right.semantic_key &&
						left.left_interpretation == right.left_interpretation &&
						left.right_interpretation == right.right_interpretation &&
						left.left_content == right.left_content &&
						left.right_content == right.right_content &&
						left.overlap_fragments == right.overlap_fragments;
				})
				.begin(),
			ordered_differential.end());
		std::vector<canonical_value> differential_records;
		differential_records.reserve(ordered_differential.size());
		for (const auto& value : ordered_differential)
			differential_records.push_back(canonical_value::from_tuple({
				canonical_value::from_string("differential"),
				canonical_value::from_string(value.relation),
				canonical_value::from_string(value.semantic_key),
				canonical_value::from_string(value.left_interpretation),
				canonical_value::from_string(value.right_interpretation),
				canonical_value::from_string(value.left_content),
				canonical_value::from_string(value.right_content),
				strings(value.overlap_fragments),
			}));

		return canonical_binary(canonical_value::from_tuple({
			canonical_value::from_string("cxxlens.claim-batch.v2"),
			canonical_value::from_tuple(std::move(claim_records)),
			canonical_value::from_tuple(std::move(unresolved_records)),
			canonical_value::from_tuple(std::move(conflict_records)),
			canonical_value::from_tuple(std::move(differential_records)),
		}));
	}

	result<std::string> claim_batch_content_digest(
		const std::span<const claim> claims,
		const std::span<const unresolved_reference> unresolved,
		const std::span<const claim_conflict> conflicts,
		const std::span<const differential_disagreement> differential_disagreements)
	{
		auto encoded =
			claim_batch_content_encoding(claims, unresolved, conflicts, differential_disagreements);
		if (!encoded)
			return unexpected(std::move(encoded.error()));
		std::string payload;
		payload.reserve(encoded->size());
		for (const auto byte : *encoded)
			payload.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
		return semantic_digest("cxxlens.claim-batch.v2", payload);
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
				!detail::same_claim_occurrence(output.claims.back(), value))
				output.claims.push_back(value);
		}

		const auto classify_pair = [&](const claim& left, const claim& right) -> result<void>
		{
			const claim* first = &left;
			const claim* second = &right;
			if (claim_order(*second, *first))
				std::swap(first, second);
			if (first->content == second->content || first->descriptor != second->descriptor ||
				first->semantic_key != second->semantic_key)
				return {};
			auto descriptor = descriptor_for(engine, *first);
			if (!descriptor)
				return unexpected(std::move(descriptor.error()));
			if (descriptor->merge != merge_mode::functional_assertion)
				return {};
			auto first_payload = detail::functional_payload_digest(*descriptor, first->row);
			auto second_payload = detail::functional_payload_digest(*descriptor, second->row);
			if (!first_payload || !second_payload)
				return unexpected(!first_payload ? std::move(first_payload.error())
												 : std::move(second_payload.error()));
			if (*first_payload == *second_payload)
				return {};
			auto overlap = first->presence.overlap(second->presence);
			if (!overlap)
				return unexpected(std::move(overlap.error()));
			if (overlap->empty())
				return {};
			if (first->interpretation == second->interpretation)
				output.conflicts.push_back({descriptor->name,
											first->semantic_key,
											first->interpretation,
											std::move(*overlap),
											{first->assertion, second->assertion},
											{first->content, second->content}});
			else
				output.differential_disagreements.push_back({descriptor->name,
															 first->semantic_key,
															 first->interpretation,
															 second->interpretation,
															 first->content,
															 second->content,
															 std::move(*overlap)});
			return {};
		};

		for (std::size_t left = 0U; left < output.claims.size(); ++left)
			for (std::size_t right = left + 1U; right < output.claims.size(); ++right)
			{
				if (auto classified = classify_pair(output.claims[left], output.claims[right]);
					!classified)
					return unexpected(std::move(classified.error()));
			}

		std::vector<const claim*> ordered_existing;
		ordered_existing.reserve(existing.size());
		for (const auto& value : existing)
			ordered_existing.push_back(&value);
		std::ranges::sort(ordered_existing,
						  [](const claim* left, const claim* right)
						  {
							  return claim_order(*left, *right);
						  });
		ordered_existing.erase(std::unique(ordered_existing.begin(),
										   ordered_existing.end(),
										   [](const claim* left, const claim* right)
										   {
											   return left->content == right->content;
										   }),
							   ordered_existing.end());
		for (const auto& added : output.claims)
			for (const auto* prior : ordered_existing)
				if (auto classified = classify_pair(added, *prior); !classified)
					return unexpected(std::move(classified.error()));

		detail::canonicalize_claim_conflicts(output.conflicts);
		std::ranges::sort(
			output.differential_disagreements,
			[](const differential_disagreement& left, const differential_disagreement& right)
			{
				return std::tie(left.relation,
								left.semantic_key,
								left.left_interpretation,
								left.right_interpretation,
								left.left_content,
								left.right_content,
								left.overlap_fragments) < std::tie(right.relation,
																   right.semantic_key,
																   right.left_interpretation,
																   right.right_interpretation,
																   right.left_content,
																   right.right_content,
																   right.overlap_fragments);
			});
		output.differential_disagreements.erase(
			std::unique(
				output.differential_disagreements.begin(),
				output.differential_disagreements.end(),
				[](const differential_disagreement& left, const differential_disagreement& right)
				{
					return left.relation == right.relation &&
						left.semantic_key == right.semantic_key &&
						left.left_interpretation == right.left_interpretation &&
						left.right_interpretation == right.right_interpretation &&
						left.left_content == right.left_content &&
						left.right_content == right.right_content &&
						left.overlap_fragments == right.overlap_fragments;
				}),
			output.differential_disagreements.end());

		std::ranges::sort(output.unresolved,
						  [](const auto& left, const auto& right)
						  {
							  return std::tie(left.source_assertion,
											  left.source_relation,
											  left.target_relation,
											  left.source_columns,
											  left.reason) < std::tie(right.source_assertion,
																	  right.source_relation,
																	  right.target_relation,
																	  right.source_columns,
																	  right.reason);
						  });
		output.unresolved.erase(
			std::ranges::unique(output.unresolved,
								[](const auto& left, const auto& right)
								{
									return left.source_assertion == right.source_assertion &&
										left.source_relation == right.source_relation &&
										left.target_relation == right.target_relation &&
										left.source_columns == right.source_columns &&
										left.reason == right.reason;
								})
				.begin(),
			output.unresolved.end());
		auto batch_digest = claim_batch_content_digest(
			output.claims, output.unresolved, output.conflicts, output.differential_disagreements);
		if (!batch_digest)
			return unexpected(std::move(batch_digest.error()));
		output.content_digest = std::move(*batch_digest);
		return output;
	}
} // namespace cxxlens::sdk
