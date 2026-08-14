#include "materialization_bounded_claim_source.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <ranges>
#include <set>
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

		[[nodiscard]] sdk::result<void> append_record(materialization_replayable_spool& spool,
													  const sdk::canonical_value& value,
													  const std::string_view field)
		{
			auto encoded = sdk::canonical_binary(value);
			if (!encoded || encoded->size() > std::numeric_limits<std::uint64_t>::max())
				return sdk::unexpected(source_error(std::string{field}, "encode"));
			auto length = encode_u64(static_cast<std::uint64_t>(encoded->size()));
			if (auto appended = spool.append(length); !appended)
				return sdk::unexpected(source_error(std::string{field}, "spool-write"));
			if (auto appended = spool.append(*encoded); !appended)
				return sdk::unexpected(source_error(std::string{field}, "spool-write"));
			return {};
		}

		[[nodiscard]] sdk::result<sdk::canonical_value>
		read_record(materialization_replayable_spool& spool,
					std::uint64_t& offset,
					const std::string_view field)
		{
			if (offset > spool.size_bytes() || spool.size_bytes() - offset < sizeof(std::uint64_t))
				return sdk::unexpected(source_error(std::string{field}, "truncated-length"));
			std::array<std::byte, sizeof(std::uint64_t)> length_bytes{};
			if (auto read = read_exact(spool, offset, length_bytes); !read)
				return sdk::unexpected(std::move(read.error()));
			offset += length_bytes.size();
			const auto length = decode_u64(length_bytes);
			if (length > spool.size_bytes() - offset ||
				length > std::numeric_limits<std::size_t>::max())
				return sdk::unexpected(source_error(std::string{field}, "length"));
			std::vector<std::byte> encoded(static_cast<std::size_t>(length));
			if (auto read = read_exact(spool, offset, encoded); !read)
				return sdk::unexpected(std::move(read.error()));
			offset += length;
			auto decoded = sdk::canonical_binary_decode(encoded);
			if (!decoded)
				return sdk::unexpected(source_error(std::string{field}, "decode"));
			return std::move(*decoded);
		}

		[[nodiscard]] sdk::result<std::string> record_string(const sdk::canonical_value& value,
															 const std::string_view field,
															 const bool nonempty = true)
		{
			if (value.type != sdk::canonical_value::kind::utf8_string ||
				(nonempty && value.text.empty()))
				return sdk::unexpected(source_error(std::string{field}, "string"));
			return value.text;
		}

		[[nodiscard]] sdk::result<materialization_claim_envelope>
		decode_envelope(const sdk::canonical_value& value, const sdk::relation_engine& engine)
		{
			if (value.type != sdk::canonical_value::kind::ordered_tuple ||
				value.tuple.size() != 5U ||
				value.tuple[4].type != sdk::canonical_value::kind::bytes)
				return sdk::unexpected(source_error("claim_envelopes", "record-shape"));
			auto role = record_string(value.tuple[0], "claim_envelopes.role");
			auto row_ref = record_string(value.tuple[1], "claim_envelopes.row_ref");
			auto claim_ref = record_string(value.tuple[2], "claim_envelopes.claim_ref");
			auto singleton = record_string(value.tuple[3], "claim_envelopes.singleton-digest");
			if (!role || !row_ref || !claim_ref || !singleton)
				return sdk::unexpected(!role			? std::move(role.error())
										   : !row_ref	? std::move(row_ref.error())
										   : !claim_ref ? std::move(claim_ref.error())
														: std::move(singleton.error()));
			auto claim = sdk::detail::decode_store_claim(value.tuple[4].byte_string, engine);
			if (!claim)
				return sdk::unexpected(std::move(claim.error()));
			return materialization_claim_envelope{std::move(*role),
												  std::move(*row_ref),
												  std::move(*claim_ref),
												  std::move(*singleton),
												  std::move(*claim)};
		}

		[[nodiscard]] sdk::result<materialization_canonicalization_edge>
		decode_edge(const sdk::canonical_value& value)
		{
			if (value.type != sdk::canonical_value::kind::ordered_tuple || value.tuple.size() != 3U)
				return sdk::unexpected(source_error("canonicalization_edges", "record-shape"));
			auto precursor = record_string(value.tuple[0], "canonicalization_edges.precursor");
			auto final = record_string(value.tuple[1], "canonicalization_edges.final");
			auto transform = record_string(value.tuple[2], "canonicalization_edges.transform");
			if (!precursor || !final || !transform)
				return sdk::unexpected(!precursor	? std::move(precursor.error())
										   : !final ? std::move(final.error())
													: std::move(transform.error()));
			return materialization_canonicalization_edge{
				std::move(*precursor), std::move(*final), std::move(*transform)};
		}

		[[nodiscard]] sdk::result<materialization_origin_association>
		decode_association(const sdk::canonical_value& value)
		{
			if (value.type != sdk::canonical_value::kind::ordered_tuple ||
				value.tuple.size() != 5U ||
				value.tuple[2].type != sdk::canonical_value::kind::ordered_tuple ||
				value.tuple[2].tuple.size() != 7U)
				return sdk::unexpected(source_error("origin_associations", "record-shape"));
			auto id = record_string(value.tuple[0], "origin_associations.id");
			auto stored = record_string(value.tuple[1], "origin_associations.stored-ref");
			auto row = record_string(value.tuple[3], "origin_associations.row");
			if (!id || !stored || !row)
				return sdk::unexpected(!id ? std::move(id.error())
										   : !stored ? std::move(stored.error())
													 : std::move(row.error()));
			std::array<std::string, 7U> context_values;
			for (std::size_t index{}; index < context_values.size(); ++index)
			{
				auto item =
					record_string(value.tuple[2].tuple[index], "origin_associations.context");
				if (!item)
					return sdk::unexpected(std::move(item.error()));
				context_values[index] = std::move(*item);
			}
			std::optional<std::string> evidence;
			if (value.tuple[4].type == sdk::canonical_value::kind::utf8_string)
			{
				if (value.tuple[4].text.empty())
					return sdk::unexpected(source_error("origin_associations.evidence", "empty"));
				evidence = value.tuple[4].text;
			}
			else if (value.tuple[4].type != sdk::canonical_value::kind::null_value)
				return sdk::unexpected(source_error("origin_associations.evidence", "optional"));
			return materialization_origin_association{std::move(*id),
													  std::move(*stored),
													  {std::move(context_values[0]),
													   std::move(context_values[1]),
													   std::move(context_values[2]),
													   std::move(context_values[3]),
													   std::move(context_values[4]),
													   std::move(context_values[5]),
													   std::move(context_values[6])},
													  std::move(*row),
													  std::move(evidence)};
		}

		struct bounded_claim_record
		{
			std::string content;
			std::vector<std::byte> occurrence;
		};

		[[nodiscard]] sdk::result<bounded_claim_record>
		decode_bounded_claim_record(const sdk::canonical_value& value)
		{
			if (value.type != sdk::canonical_value::kind::ordered_tuple ||
				value.tuple.size() != 3U ||
				value.tuple[0].type != sdk::canonical_value::kind::utf8_string ||
				value.tuple[0].text != "claim" ||
				value.tuple[1].type != sdk::canonical_value::kind::utf8_string ||
				value.tuple[1].text.empty() ||
				value.tuple[2].type != sdk::canonical_value::kind::bytes)
				return sdk::unexpected(source_error("claim-status", "record-shape"));
			return bounded_claim_record{value.tuple[1].text, value.tuple[2].byte_string};
		}

		[[nodiscard]] bool checked_add(const std::uint64_t left,
									   const std::uint64_t right,
									   std::uint64_t& output) noexcept
		{
			if (right > std::numeric_limits<std::uint64_t>::max() - left)
				return false;
			output = left + right;
			return true;
		}

		[[nodiscard]] bool checked_canonical_string_size(const std::string_view value,
														 std::uint64_t& output) noexcept
		{
			if (value.size() > std::numeric_limits<std::uint64_t>::max() - 9U)
				return false;
			output = 9U + static_cast<std::uint64_t>(value.size());
			return true;
		}

		[[nodiscard]] sdk::result<void> update_digest(materialization_digest_accumulator& digest,
													  const std::span<const std::byte> bytes,
													  const std::string_view field)
		{
			if (auto updated = digest.update(bytes); !updated)
				return sdk::unexpected(source_error(std::string{field}, "digest-update"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		update_canonical_string(materialization_digest_accumulator& digest,
								const std::string_view value,
								const std::string_view field)
		{
			const std::array<std::byte, 1U> tag{std::byte{0x04U}};
			if (auto updated = update_digest(digest, tag, field); !updated)
				return updated;
			const auto length = encode_u64(static_cast<std::uint64_t>(value.size()));
			if (auto updated = update_digest(digest, length, field); !updated)
				return updated;
			const auto* data = reinterpret_cast<const std::byte*>(value.data());
			return update_digest(digest, std::span<const std::byte>{data, value.size()}, field);
		}

		[[nodiscard]] sdk::result<void>
		update_empty_tuple(materialization_digest_accumulator& digest, const std::string_view field)
		{
			const std::array<std::byte, 1U> tag{std::byte{0x05U}};
			if (auto updated = update_digest(digest, tag, field); !updated)
				return updated;
			const auto length = encode_u64(0U);
			return update_digest(digest, length, field);
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
			if (!claim_envelopes_ || !canonicalization_edges_ || !origin_associations_)
			{
				auto envelopes = make_materialization_private_spool();
				auto edges = make_materialization_private_spool();
				auto associations = make_materialization_private_spool();
				if (!envelopes || !edges || !associations)
					return sdk::unexpected(source_error("report-metadata", "spool-create"));
				claim_envelopes_ = std::move(*envelopes);
				canonicalization_edges_ = std::move(*edges);
				origin_associations_ = std::move(*associations);
			}
			for (const auto& envelope : task.claim_envelopes)
			{
				auto encoded = sdk::detail::encode_store_claim(envelope.value);
				if (!encoded)
					return sdk::unexpected(source_error("claim_envelopes", "encode"));
				auto record = sdk::canonical_value::from_tuple({
					sdk::canonical_value::from_string(envelope.role),
					sdk::canonical_value::from_string(envelope.row_ref),
					sdk::canonical_value::from_string(envelope.claim_ref),
					sdk::canonical_value::from_string(envelope.sdk_singleton_claim_batch_digest),
					sdk::canonical_value::from_bytes(std::move(*encoded)),
				});
				if (auto appended = append_record(*claim_envelopes_, record, "claim_envelopes");
					!appended)
					return appended;
			}
			for (const auto& edge : task.canonicalization_edges)
			{
				auto record = sdk::canonical_value::from_tuple({
					sdk::canonical_value::from_string(edge.precursor_claim_ref),
					sdk::canonical_value::from_string(edge.final_claim_ref),
					sdk::canonical_value::from_string(edge.transform_semantics),
				});
				if (auto appended =
						append_record(*canonicalization_edges_, record, "canonicalization_edges");
					!appended)
					return appended;
			}
			for (const auto& association : task.origin_associations)
			{
				auto record = sdk::canonical_value::from_tuple({
					sdk::canonical_value::from_string(association.association_id),
					sdk::canonical_value::from_string(association.stored_claim_ref),
					sdk::canonical_value::from_tuple({
						sdk::canonical_value::from_string(
							association.originating_task.provider_task_id),
						sdk::canonical_value::from_string(
							association.originating_task.task_input_digest),
						sdk::canonical_value::from_string(
							association.originating_task.selected_catalog_compile_unit_id),
						sdk::canonical_value::from_string(
							association.originating_task.compile_unit_id),
						sdk::canonical_value::from_string(
							association.originating_task.condition_universe_id),
						sdk::canonical_value::from_string(
							association.originating_task.condition_id),
						sdk::canonical_value::from_string(
							association.originating_task.interpretation_domain),
					}),
					sdk::canonical_value::from_string(association.sealed_row_digest),
					association.source_evidence_digest
						? sdk::canonical_value::from_string(*association.source_evidence_digest)
						: sdk::canonical_value::null(),
				});
				if (auto appended =
						append_record(*origin_associations_, record, "origin_associations");
					!appended)
					return appended;
			}
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
				for (const auto& ref : partition.stored_claim_refs)
					found->second.stored_claim_refs.insert(ref);
				for (const auto& content : partition.claim_content_ids)
					found->second.claim_content_ids.insert(content);
				if (partition.origin_association_count > std::numeric_limits<std::uint64_t>::max() -
						found->second.origin_association_count)
					return sdk::unexpected(source_error("partition", "association-count-overflow"));
				found->second.origin_association_count += partition.origin_association_count;
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
		if (!claim_envelopes_ || !canonicalization_edges_ || !origin_associations_)
			return sdk::unexpected(source_error("report-metadata", "missing-spool"));
		for (auto* spool :
			 {claim_envelopes_.get(), canonicalization_edges_.get(), origin_associations_.get()})
			if (auto sealed = spool->seal(); !sealed)
				return sdk::unexpected(source_error("report-metadata", "spool-seal"));
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

	sdk::result<void> materialization_bounded_claim_source::replay_claim_envelopes(
		const materialization_claim_envelope_consumer& consumer)
	{
		if (!sealed_ || !claim_envelopes_ || !consumer || engine_ == nullptr)
			return sdk::unexpected(source_error("claim_envelopes", "lifecycle"));
		std::uint64_t offset{};
		while (offset < claim_envelopes_->size_bytes())
		{
			auto record = read_record(*claim_envelopes_, offset, "claim_envelopes");
			if (!record)
				return sdk::unexpected(std::move(record.error()));
			auto envelope = decode_envelope(*record, *engine_);
			if (!envelope)
				return sdk::unexpected(std::move(envelope.error()));
			if (auto consumed = consumer(*envelope); !consumed)
				return consumed;
		}
		return {};
	}

	sdk::result<void> materialization_bounded_claim_source::replay_canonicalization_edges(
		const materialization_canonicalization_edge_consumer& consumer)
	{
		if (!sealed_ || !canonicalization_edges_ || !consumer)
			return sdk::unexpected(source_error("canonicalization_edges", "lifecycle"));
		std::uint64_t offset{};
		while (offset < canonicalization_edges_->size_bytes())
		{
			auto record = read_record(*canonicalization_edges_, offset, "canonicalization_edges");
			if (!record)
				return sdk::unexpected(std::move(record.error()));
			auto edge = decode_edge(*record);
			if (!edge)
				return sdk::unexpected(std::move(edge.error()));
			if (auto consumed = consumer(*edge); !consumed)
				return consumed;
		}
		return {};
	}

	sdk::result<void> materialization_bounded_claim_source::replay_origin_associations(
		const materialization_origin_association_consumer& consumer)
	{
		if (!sealed_ || !origin_associations_ || !consumer)
			return sdk::unexpected(source_error("origin_associations", "lifecycle"));
		std::uint64_t offset{};
		while (offset < origin_associations_->size_bytes())
		{
			auto record = read_record(*origin_associations_, offset, "origin_associations");
			if (!record)
				return sdk::unexpected(std::move(record.error()));
			auto association = decode_association(*record);
			if (!association)
				return sdk::unexpected(std::move(association.error()));
			if (auto consumed = consumer(*association); !consumed)
				return consumed;
		}
		return {};
	}

	sdk::result<materialization_bounded_claim_batch_status>
	materialization_bounded_claim_source::claim_batch_status()
	{
		if (!sealed_ || engine_ == nullptr)
			return sdk::unexpected(source_error("claim-status", "lifecycle"));
		try
		{
			struct claim_run
			{
				std::uint64_t begin{};
				std::uint64_t end{};
			};
			struct claim_cursor
			{
				std::uint64_t offset{};
				std::uint64_t end{};
				std::string content;
				std::vector<std::byte> occurrence;
				bool has_current{};
			};

			auto records_result = make_materialization_private_spool();
			if (!records_result)
				return sdk::unexpected(source_error("claim-status", "spool-create"));
			auto records = std::move(*records_result);
			std::vector<claim_run> runs;
			runs.reserve(partitions_.size());
			std::uint64_t claim_count{};
			std::uint64_t unresolved_count{};

			if (auto replayed = replay(
					[&](sdk::partition_draft draft) -> sdk::result<void>
					{
						runs.push_back({records->size_bytes(), records->size_bytes()});
						if (draft.unresolved.size() >
							std::numeric_limits<std::uint64_t>::max() - unresolved_count)
							return sdk::unexpected(source_error("claims.unresolved", "overflow"));
						unresolved_count += static_cast<std::uint64_t>(draft.unresolved.size());
						if (draft.claims.size() >
							std::numeric_limits<std::uint64_t>::max() - claim_count)
							return sdk::unexpected(source_error("claims", "count-overflow"));
						claim_count += static_cast<std::uint64_t>(draft.claims.size());
						for (const auto& claim : draft.claims)
						{
							auto occurrence = sdk::detail::claim_occurrence_projection(claim);
							if (!occurrence)
								return sdk::unexpected(std::move(occurrence.error()));
							auto record = sdk::canonical_value::from_tuple({
								sdk::canonical_value::from_string("claim"),
								sdk::canonical_value::from_string(claim.content),
								sdk::canonical_value::from_bytes(std::move(*occurrence)),
							});
							if (auto appended = append_record(*records, record, "claim-status");
								!appended)
								return appended;
						}
						runs.back().end = records->size_bytes();
						return {};
					});
				!replayed)
				return sdk::unexpected(std::move(replayed.error()));

			std::vector<claim_cursor> cursors;
			cursors.reserve(runs.size());
			const auto load = [&](claim_cursor& cursor) -> sdk::result<void>
			{
				if (cursor.offset == cursor.end)
				{
					cursor.has_current = false;
					return {};
				}
				if (cursor.offset > cursor.end)
					return sdk::unexpected(source_error("claim-status", "run-boundary"));
				const auto before = cursor.offset;
				auto record = read_record(*records, cursor.offset, "claim-status");
				if (!record)
					return sdk::unexpected(std::move(record.error()));
				if (cursor.offset > cursor.end || cursor.offset == before)
					return sdk::unexpected(source_error("claim-status", "run-boundary"));
				auto decoded = decode_bounded_claim_record(*record);
				if (!decoded)
					return sdk::unexpected(std::move(decoded.error()));
				cursor.content = std::move(decoded->content);
				cursor.occurrence = std::move(decoded->occurrence);
				cursor.has_current = true;
				return {};
			};

			for (const auto& run : runs)
			{
				claim_cursor cursor{run.begin, run.end, {}, {}, false};
				if (auto loaded = load(cursor); !loaded)
					return sdk::unexpected(std::move(loaded.error()));
				cursors.push_back(std::move(cursor));
			}

			const auto occurrence_less = [&cursors](const std::size_t left, const std::size_t right)
			{
				if (cursors[left].occurrence != cursors[right].occurrence)
					return cursors[left].occurrence < cursors[right].occurrence;
				return left < right;
			};
			std::set<std::size_t, decltype(occurrence_less)> ready{occurrence_less};
			for (std::size_t index{}; index < cursors.size(); ++index)
				if (cursors[index].has_current)
					ready.insert(index);

			const auto ordered_begin = records->size_bytes();
			std::uint64_t ordered_count{};
			while (!ready.empty())
			{
				const auto index = *ready.begin();
				ready.erase(ready.begin());
				auto& cursor = cursors[index];
				auto record = sdk::canonical_value::from_tuple({
					sdk::canonical_value::from_string("claim"),
					sdk::canonical_value::from_string(cursor.content),
					sdk::canonical_value::from_bytes(cursor.occurrence),
				});
				if (auto appended = append_record(*records, record, "claim-status-merge");
					!appended)
					return sdk::unexpected(std::move(appended.error()));
				if (ordered_count == std::numeric_limits<std::uint64_t>::max())
					return sdk::unexpected(source_error("claims", "count-overflow"));
				++ordered_count;
				if (auto loaded = load(cursor); !loaded)
					return sdk::unexpected(std::move(loaded.error()));
				if (cursor.has_current)
					ready.insert(index);
			}
			if (ordered_count != claim_count)
				return sdk::unexpected(source_error("claims", "census-mismatch"));

			if (unresolved_count != 0U)
				// Task admission rejects functional conflicts and differential disagreements before
				// they become bounded partitions; zero here is the source contract, not an inferred
				// absence from the streaming census.
				return materialization_bounded_claim_batch_status{
					{}, claim_count, unresolved_count, 0U, 0U, partitions_.size()};

			if (auto sealed = records->seal(); !sealed)
				return sdk::unexpected(source_error("claim-status", "spool-seal"));
			const auto ordered_end = records->size_bytes();
			if (ordered_end < ordered_begin)
				return sdk::unexpected(source_error("claim-status", "size-overflow"));
			const auto ordered_size = ordered_end - ordered_begin;
			std::uint64_t claims_tuple_size{};
			if (!checked_add(9U, ordered_size, claims_tuple_size))
				return sdk::unexpected(source_error("claims", "size-overflow"));

			std::uint64_t claim_batch_marker_size{};
			if (!checked_canonical_string_size("cxxlens.claim-batch.v2", claim_batch_marker_size))
				return sdk::unexpected(source_error("claims", "size-overflow"));
			std::uint64_t child_sum{};
			const auto add_child = [&](const std::uint64_t encoded_size) -> bool
			{
				std::uint64_t framed{};
				return checked_add(8U, encoded_size, framed) &&
					checked_add(child_sum, framed, child_sum);
			};
			if (!add_child(claim_batch_marker_size) || !add_child(claims_tuple_size) ||
				!add_child(9U) || !add_child(9U) || !add_child(9U))
				return sdk::unexpected(source_error("claims", "size-overflow"));
			std::uint64_t inner_payload_size{};
			if (!checked_add(9U, child_sum, inner_payload_size))
				return sdk::unexpected(source_error("claims", "size-overflow"));
			std::uint64_t bytes_element_size{};
			if (!checked_add(9U, inner_payload_size, bytes_element_size))
				return sdk::unexpected(source_error("claims", "size-overflow"));

			auto digest = make_materialization_sha256_accumulator();
			if (!digest)
				return sdk::unexpected(source_error("claims", "digest-create"));
			const std::array<std::byte, 1U> tuple_tag{std::byte{0x05U}};
			if (auto updated = update_digest(*digest, tuple_tag, "claims.digest"); !updated)
				return sdk::unexpected(std::move(updated.error()));
			const auto outer_count = encode_u64(3U);
			if (auto updated = update_digest(*digest, outer_count, "claims.digest"); !updated)
				return sdk::unexpected(std::move(updated.error()));
			const auto semantic_marker = std::string_view{"cxxlens-semantic-digest-v2"};
			const auto semantic_domain = std::string_view{"cxxlens.claim-batch.v2"};
			std::uint64_t semantic_marker_size{};
			std::uint64_t semantic_domain_size{};
			if (!checked_canonical_string_size(semantic_marker, semantic_marker_size) ||
				!checked_canonical_string_size(semantic_domain, semantic_domain_size))
				return sdk::unexpected(source_error("claims", "size-overflow"));
			const auto marker_length = encode_u64(semantic_marker_size);
			if (auto updated = update_digest(*digest, marker_length, "claims.digest"); !updated)
				return sdk::unexpected(std::move(updated.error()));
			if (auto updated = update_canonical_string(*digest, semantic_marker, "claims.digest");
				!updated)
				return sdk::unexpected(std::move(updated.error()));
			const auto domain_length = encode_u64(semantic_domain_size);
			if (auto updated = update_digest(*digest, domain_length, "claims.digest"); !updated)
				return sdk::unexpected(std::move(updated.error()));
			if (auto updated = update_canonical_string(*digest, semantic_domain, "claims.digest");
				!updated)
				return sdk::unexpected(std::move(updated.error()));
			const auto bytes_length = encode_u64(bytes_element_size);
			if (auto updated = update_digest(*digest, bytes_length, "claims.digest"); !updated)
				return sdk::unexpected(std::move(updated.error()));
			const std::array<std::byte, 1U> bytes_tag{std::byte{0x03U}};
			if (auto updated = update_digest(*digest, bytes_tag, "claims.digest"); !updated)
				return sdk::unexpected(std::move(updated.error()));
			const auto payload_length = encode_u64(inner_payload_size);
			if (auto updated = update_digest(*digest, payload_length, "claims.digest"); !updated)
				return sdk::unexpected(std::move(updated.error()));

			if (auto updated = update_digest(*digest, tuple_tag, "claims.digest"); !updated)
				return sdk::unexpected(std::move(updated.error()));
			const auto inner_count = encode_u64(5U);
			if (auto updated = update_digest(*digest, inner_count, "claims.digest"); !updated)
				return sdk::unexpected(std::move(updated.error()));
			const auto claim_marker_length = encode_u64(claim_batch_marker_size);
			if (auto updated = update_digest(*digest, claim_marker_length, "claims.digest");
				!updated)
				return sdk::unexpected(std::move(updated.error()));
			if (auto updated =
					update_canonical_string(*digest, "cxxlens.claim-batch.v2", "claims.digest");
				!updated)
				return sdk::unexpected(std::move(updated.error()));
			const auto claims_length = encode_u64(claims_tuple_size);
			if (auto updated = update_digest(*digest, claims_length, "claims.digest"); !updated)
				return sdk::unexpected(std::move(updated.error()));
			if (auto updated = update_digest(*digest, tuple_tag, "claims.digest"); !updated)
				return sdk::unexpected(std::move(updated.error()));
			const auto claim_count_bytes = encode_u64(claim_count);
			if (auto updated = update_digest(*digest, claim_count_bytes, "claims.digest"); !updated)
				return sdk::unexpected(std::move(updated.error()));

			std::array<std::byte, default_stream_chunk_bytes> buffer{};
			std::uint64_t offset = ordered_begin;
			while (offset < ordered_end)
			{
				const auto remaining = ordered_end - offset;
				const auto requested =
					static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
				auto received = records->read_at(offset, std::span{buffer}.first(requested));
				if (!received || *received == 0U || *received > requested)
					return sdk::unexpected(source_error("claims", "spool-read"));
				if (auto updated =
						update_digest(*digest, std::span{buffer}.first(*received), "claims.digest");
					!updated)
					return sdk::unexpected(std::move(updated.error()));
				offset += static_cast<std::uint64_t>(*received);
			}
			for (std::size_t index{}; index < 3U; ++index)
			{
				const auto empty_length = encode_u64(9U);
				if (auto updated = update_digest(*digest, empty_length, "claims.digest"); !updated)
					return sdk::unexpected(std::move(updated.error()));
				if (auto updated = update_empty_tuple(*digest, "claims.digest"); !updated)
					return sdk::unexpected(std::move(updated.error()));
			}
			auto finished = digest->finish();
			if (!finished || !finished->starts_with("sha256:") || finished->size() != 71U)
				return sdk::unexpected(source_error("claims", "digest-finalize"));
			const auto stream_digest = "semantic-v2:" + *finished;
			// The task contract above is the sole authority for these two zero-valued fields. This
			// bounded status path deliberately does not materialize or recompute their detail rows.
			return materialization_bounded_claim_batch_status{
				stream_digest, claim_count, 0U, 0U, 0U, partitions_.size()};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(source_error("claim-status", "allocation"));
		}
	}

	sdk::result<materialization_bounded_partition_metadata>
	materialization_bounded_claim_source::partition_metadata(
		const std::string_view partition_id) const
	{
		if (!sealed_ || partition_id.empty())
			return sdk::unexpected(source_error("partition", "lifecycle"));
		const auto found = partitions_.find(std::string{partition_id});
		if (found == partitions_.end())
			return sdk::unexpected(source_error("partition", "missing"));
		materialization_bounded_partition_metadata output;
		output.stored_claim_refs.assign(found->second.stored_claim_refs.begin(),
										found->second.stored_claim_refs.end());
		output.claim_content_ids.assign(found->second.claim_content_ids.begin(),
										found->second.claim_content_ids.end());
		output.sdk_claim_occurrence_count =
			static_cast<std::uint64_t>(output.stored_claim_refs.size());
		output.origin_association_count = found->second.origin_association_count;
		output.empty_partition = found->second.empty;
		return output;
	}
} // namespace cxxlens::detail::clang22::materialization
