#include "materialization_bounded_claim_source.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "materialization_incremental_receipt.hpp"
#include "sdk/claim_internal.hpp"
#include "sdk/store_claim_codec_internal.hpp"

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		[[nodiscard]] sdk::error source_error(std::string field, std::string detail = {})
		{
			return {"materialization.claim-source-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] std::array<std::byte, 8U> encode_u64(const std::uint64_t value) noexcept
		{
			std::array<std::byte, 8U> output{};
			for (std::size_t index{}; index < output.size(); ++index)
				output[index] = static_cast<std::byte>((value >> (56U - index * 8U)) & 0xffU);
			return output;
		}

		[[nodiscard]] std::uint64_t decode_u64(const std::span<const std::byte> bytes) noexcept
		{
			std::uint64_t output{};
			for (const auto byte : bytes)
				output = (output << 8U) | std::to_integer<unsigned char>(byte);
			return output;
		}

		[[nodiscard]] sdk::result<void> read_exact(materialization_replayable_spool& spool,
												   const std::uint64_t offset,
												   const std::span<std::byte> destination)
		{
			std::size_t copied{};
			while (copied < destination.size())
			{
				auto read = spool.read_at(offset + copied, destination.subspan(copied));
				if (!read || *read == 0U || *read > destination.size() - copied)
					return sdk::unexpected(source_error("claims", "truncated-record"));
				copied += *read;
			}
			return {};
		}

		[[nodiscard]] sdk::result<void>
		validate_partition_metadata(const sdk::relation_engine& engine,
									const materialization_claim_partition& partition)
		{
			if (partition.manifest.partition_id != partition.binding.partition_id ||
				partition.draft.relation_descriptor_id !=
					partition.manifest.relation_descriptor_id ||
				partition.binding.relation_descriptor_id !=
					partition.manifest.relation_descriptor_id ||
				partition.draft.condition != partition.binding.condition ||
				partition.draft.interpretation != partition.binding.interpretation ||
				partition.draft.producer_semantics != partition.binding.producer_semantics ||
				partition.draft.producer_input_basis_digest !=
					partition.binding.producer_input_basis_digest ||
				partition.draft.precision_profile != partition.binding.precision_profile ||
				partition.draft.assumption_set_id != partition.binding.assumption_set_id)
				return sdk::unexpected(source_error("partition", "identity-binding"));
			if (auto valid = partition.draft.condition.validate(); !valid)
				return sdk::unexpected(source_error("partition", "condition"));
			if (auto relation = engine.require_id(partition.draft.relation_descriptor_id);
				!relation)
				return sdk::unexpected(std::move(relation.error()));
			for (const auto& coverage : partition.draft.coverage)
				if (auto valid = coverage.validate(); !valid)
					return sdk::unexpected(source_error("partition", "coverage"));
			for (const auto& unresolved : partition.draft.unresolved)
				if (unresolved.source_assertion.empty() || unresolved.source_relation.empty() ||
					unresolved.target_relation.empty())
					return sdk::unexpected(source_error("partition", "unresolved"));
			return {};
		}

		[[nodiscard]] bool same_identity(const sdk::partition_draft& left,
										 const sdk::partition_draft& right) noexcept
		{
			return left.relation_descriptor_id == right.relation_descriptor_id &&
				left.scope == right.scope && left.condition == right.condition &&
				left.interpretation == right.interpretation &&
				left.producer_semantics == right.producer_semantics &&
				left.producer_input_basis_digest == right.producer_input_basis_digest &&
				left.precision_profile == right.precision_profile &&
				left.assumption_set_id == right.assumption_set_id;
		}
	} // namespace

	sdk::result<materialization_bounded_claim_source>
	materialization_bounded_claim_source::begin(const validated_materialization_request& request)
	{
		if (request.tasks.empty())
			return sdk::unexpected(source_error("request", "empty-task-set"));
		auto request_id = materialization_incremental_request_id(request);
		if (!request_id)
			return sdk::unexpected(std::move(request_id.error()));
		return materialization_bounded_claim_source{std::move(*request_id), request.engine};
	}

	sdk::result<void>
	materialization_bounded_claim_source::consume_task(materialization_bounded_task_claims task)
	{
		if (sealed_ || engine_ == nullptr || task.partitions.empty())
			return sdk::unexpected(source_error("lifecycle", "sealed-or-empty"));
		try
		{
			if (materializer_semantics_digest_.empty())
			{
				materializer_semantics_digest_ = std::move(task.materializer_semantics_digest);
				direct_basis_digest_ = std::move(task.direct_basis_digest);
				canonical_adoption_transform_digest_ =
					std::move(task.canonical_adoption_transform_digest);
				base_ingestion_transform_digest_ = std::move(task.base_ingestion_transform_digest);
				assumption_set_id_ = std::move(task.assumption_set_id);
			}
			else if (materializer_semantics_digest_ != task.materializer_semantics_digest ||
					 direct_basis_digest_ != task.direct_basis_digest ||
					 canonical_adoption_transform_digest_ !=
						 task.canonical_adoption_transform_digest ||
					 base_ingestion_transform_digest_ != task.base_ingestion_transform_digest ||
					 assumption_set_id_ != task.assumption_set_id)
				return sdk::unexpected(source_error("basis", "task-mismatch"));

			for (const auto& partition : task.partitions)
			{
				if (auto valid = validate_partition_metadata(*engine_, partition); !valid)
					return valid;
				auto [found, inserted] = partitions_.try_emplace(partition.manifest.partition_id);
				if (inserted)
				{
					found->second.identity = partition.draft;
					found->second.identity.claims.clear();
					found->second.identity.coverage.clear();
					found->second.identity.unresolved.clear();
					found->second.empty = partition.empty_partition;
					auto spool = make_materialization_private_spool();
					if (!spool)
						return sdk::unexpected(source_error("claims", "spool-create"));
					found->second.claims = std::move(*spool);
				}
				else if (!same_identity(found->second.identity, partition.draft) ||
						 found->second.empty != partition.empty_partition)
					return sdk::unexpected(source_error("partition", "identity-collision"));

				for (const auto& coverage : partition.draft.coverage)
				{
					const auto key = coverage.canonical_form();
					auto [coverage_entry, added] = found->second.coverage.emplace(key, coverage);
					if (!added && coverage_entry->second != coverage)
						return sdk::unexpected(source_error("coverage", "identity-collision"));
				}
				for (const auto& unresolved : partition.draft.unresolved)
					found->second.unresolved.push_back(unresolved);
				for (const auto& claim : partition.draft.claims)
				{
					auto encoded = sdk::detail::encode_store_claim(claim);
					if (!encoded || encoded->size() > std::numeric_limits<std::uint64_t>::max())
						return sdk::unexpected(source_error("claims", "encode"));
					auto length = encode_u64(static_cast<std::uint64_t>(encoded->size()));
					if (auto appended = found->second.claims->append(length); !appended)
						return sdk::unexpected(source_error("claims", "spool-write"));
					if (auto appended = found->second.claims->append(*encoded); !appended)
						return sdk::unexpected(source_error("claims", "spool-write"));
					if (found->second.appended_claim_count ==
						std::numeric_limits<std::uint64_t>::max())
						return sdk::unexpected(source_error("claims", "count-overflow"));
					++found->second.appended_claim_count;
				}
			}
			return {};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(source_error("claims", "allocation"));
		}
	}

	sdk::result<materialization_bounded_claim_source>
	materialization_bounded_claim_source::finalize() &&
	{
		if (sealed_ || partitions_.empty())
			return sdk::unexpected(source_error("lifecycle", "empty-or-already-sealed"));
		for (auto& [partition_id, state] : partitions_)
		{
			(void)partition_id;
			if (!state.claims)
				return sdk::unexpected(source_error("claims", "missing-spool"));
			if (auto sealed = state.claims->seal(); !sealed)
				return sdk::unexpected(source_error("claims", "spool-seal"));
		}
		sealed_ = true;
		return std::move(*this);
	}

	sdk::result<void> materialization_bounded_claim_source::replay(
		const materialization_store_partition_consumer& consumer)
	{
		if (!sealed_ || engine_ == nullptr || !consumer)
			return sdk::unexpected(source_error("lifecycle", "unsealed-or-consumer"));
		try
		{
			for (auto& [partition_id, state] : partitions_)
			{
				std::vector<sdk::claim> claims;
				std::uint64_t offset{};
				while (offset < state.claims->size_bytes())
				{
					if (state.claims->size_bytes() - offset < 8U)
						return sdk::unexpected(source_error("claims", "truncated-length"));
					std::array<std::byte, 8U> length_bytes{};
					if (auto read = read_exact(*state.claims, offset, length_bytes); !read)
						return read;
					offset += length_bytes.size();
					const auto length = decode_u64(length_bytes);
					if (length > state.claims->size_bytes() - offset ||
						length > std::numeric_limits<std::size_t>::max())
						return sdk::unexpected(source_error("claims", "length"));
					std::vector<std::byte> encoded(static_cast<std::size_t>(length));
					if (auto read = read_exact(*state.claims, offset, encoded); !read)
						return read;
					offset += length;
					auto claim = sdk::detail::decode_store_claim(encoded, *engine_);
					if (!claim)
						return sdk::unexpected(std::move(claim.error()));
					claims.push_back(std::move(*claim));
				}
				std::ranges::sort(claims, sdk::detail::claim_occurrence_less);
				auto relation = engine_->require_id(state.identity.relation_descriptor_id);
				if (!relation)
					return sdk::unexpected(std::move(relation.error()));
				if (relation->descriptor().merge != sdk::merge_mode::multiset)
				{
					claims.erase(std::unique(claims.begin(),
											 claims.end(),
											 [](const sdk::claim& left, const sdk::claim& right)
											 {
												 return sdk::detail::same_claim_occurrence(left,
																						   right);
											 }),
								 claims.end());
				}
				sdk::partition_draft draft = state.identity;
				draft.claims = std::move(claims);
				for (const auto& [coverage_id, coverage] : state.coverage)
				{
					(void)coverage_id;
					draft.coverage.push_back(coverage);
				}
				draft.unresolved = state.unresolved;
				if (auto valid = sdk::make_partition_manifest(*engine_, draft); !valid)
					return sdk::unexpected(std::move(valid.error()));
				if (auto consumed = consumer(std::move(draft)); !consumed)
					return consumed;
			}
			return {};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(source_error("replay", "allocation"));
		}
	}
} // namespace cxxlens::detail::clang22::materialization
