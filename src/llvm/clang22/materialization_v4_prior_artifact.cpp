#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>

#include "materialization_io.hpp"
#include "materialization_v4_prior_artifact.hpp"
#include "materialization_prior_artifact_storage_internal.hpp"
#include "materialization_rooted_vfs.hpp"

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		[[nodiscard]] sdk::error v4_artifact_error(std::string field, std::string detail = {})
		{
			return {
				"materialization.v4-prior-artifact-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::canonical_value v4_text(const std::string_view value)
		{
			return sdk::canonical_value::from_string(std::string{value});
		}

		[[nodiscard]] sdk::result<sdk::canonical_value> v4_integer(const std::uint64_t value,
																   const std::string_view field)
		{
			if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
				return sdk::unexpected(v4_artifact_error(std::string{field}, "signed-bound"));
			return sdk::canonical_value::from_integer(static_cast<std::int64_t>(value));
		}

		[[nodiscard]] sdk::result<std::uint64_t>
		v4_decode_integer(const sdk::canonical_value& value, const std::string_view field)
		{
			if (value.type != sdk::canonical_value::kind::signed_integer || value.integer < 0)
				return sdk::unexpected(v4_artifact_error(std::string{field}, "integer"));
			return static_cast<std::uint64_t>(value.integer);
		}

		[[nodiscard]] sdk::result<std::string> v4_decode_string(const sdk::canonical_value& value,
																const std::string_view field,
																const bool allow_empty = false)
		{
			if (value.type != sdk::canonical_value::kind::utf8_string ||
				(!allow_empty && value.text.empty()) || value.text.contains('\0') ||
				!sdk::validate_utf8_text(value.text))
				return sdk::unexpected(v4_artifact_error(std::string{field}, "string"));
			return value.text;
		}

		[[nodiscard]] sdk::result<std::span<const sdk::canonical_value>>
		v4_tuple(const sdk::canonical_value& value,
				 const std::size_t expected,
				 const std::string_view field)
		{
			if (value.type != sdk::canonical_value::kind::ordered_tuple ||
				value.tuple.size() != expected)
				return sdk::unexpected(v4_artifact_error(std::string{field}, "tuple-shape"));
			return std::span<const sdk::canonical_value>{value.tuple};
		}

		[[nodiscard]] sdk::result<std::optional<std::string>>
		v4_optional_string(const sdk::canonical_value& value, const std::string_view field)
		{
			if (value.type == sdk::canonical_value::kind::null_value)
				return std::optional<std::string>{};
			auto decoded = v4_decode_string(value, field);
			if (!decoded)
				return sdk::unexpected(std::move(decoded.error()));
			return std::optional<std::string>{std::move(*decoded)};
		}

		[[nodiscard]] sdk::result<bool> v4_decode_bool(const sdk::canonical_value& value,
													   const std::string_view field)
		{
			if (value.type != sdk::canonical_value::kind::boolean)
				return sdk::unexpected(v4_artifact_error(std::string{field}, "boolean"));
			return value.boolean;
		}

		[[nodiscard]] bool v4_id(const std::string_view value) noexcept
		{
			return static_cast<bool>(sdk::validate_strong_id(value));
		}

		[[nodiscard]] sdk::canonical_value
		v4_receipt_task_value(const materialization_v4_claim_receipt& value)
		{
			return sdk::canonical_value::from_tuple({
				v4_text(value.schema),
				v4_text(value.binding_digest),
				v4_text(value.materialization_request_id),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.task_index)),
				v4_text(value.task_id),
				v4_text(value.task_v4_digest),
				v4_text(value.provider_execution_id),
				v4_text(value.source_closure_id),
				v4_text(value.source_closure_digest),
				v4_text(value.manifest_digest),
				v4_text(value.task_input_digest),
				v4_text(value.claim_batch_content_digest),
				v4_text(value.partition_id),
				v4_text(value.partition_content_digest),
				v4_text(value.coverage_digest),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.claim_count)),
				sdk::canonical_value::from_integer(
					static_cast<std::int64_t>(value.unresolved_count)),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.conflict_count)),
				sdk::canonical_value::from_integer(
					static_cast<std::int64_t>(value.differential_disagreement_count)),
				sdk::canonical_value::from_boolean(value.complete),
				v4_text(value.receipt_digest),
			});
		}

		[[nodiscard]] sdk::result<std::string>
		v4_receipt_digest(const materialization_v4_incremental_receipt& value)
		{
			std::vector<sdk::canonical_value> fields;
			fields.reserve(10U);
			fields.push_back(v4_text(value.schema));
			fields.push_back(v4_text(value.materialization_request_id));
			auto task_count = v4_integer(value.task_count, "receipt.task-count");
			if (!task_count)
				return sdk::unexpected(std::move(task_count.error()));
			fields.push_back(std::move(*task_count));
			std::vector<sdk::canonical_value> tasks;
			tasks.reserve(value.task_receipts.size());
			for (const auto& task : value.task_receipts)
				tasks.push_back(v4_receipt_task_value(task));
			fields.push_back(sdk::canonical_value::from_tuple(std::move(tasks)));
			const std::array<std::uint64_t, 4U> counts{value.claim_count,
													   value.unresolved_count,
													   value.conflict_count,
													   value.differential_disagreement_count};
			for (const auto count : counts)
			{
				auto encoded = v4_integer(count, "receipt.count");
				if (!encoded)
					return sdk::unexpected(std::move(encoded.error()));
				fields.push_back(std::move(*encoded));
			}
			fields.push_back(sdk::canonical_value::from_boolean(value.complete));
			return sdk::canonical_identity_digest(materialization_v4_incremental_receipt_schema,
												  fields);
		}

		[[nodiscard]] sdk::result<void>
		v4_validate_task_receipt(const materialization_v4_claim_receipt& value)
		{
			const std::array<std::string_view, 15U> ids{
				value.binding_digest,
				value.materialization_request_id,
				value.task_id,
				value.task_v4_digest,
				value.provider_execution_id,
				value.source_closure_id,
				value.source_closure_digest,
				value.manifest_digest,
				value.task_input_digest,
				value.claim_batch_content_digest,
				value.partition_id,
				value.partition_content_digest,
				value.coverage_digest,
				value.receipt_digest,
				value.schema,
			};
			for (const auto id : ids)
				if (!v4_id(id))
					return sdk::unexpected(v4_artifact_error("task-receipt", "strong-id"));
			if (value.schema != materialization_v4_claim_receipt_schema || value.task_index > 4095U)
				return sdk::unexpected(v4_artifact_error("task-receipt", "schema-or-bound"));
			const std::array<std::uint64_t, 4U> counts{value.claim_count,
													   value.unresolved_count,
													   value.conflict_count,
													   value.differential_disagreement_count};
			for (const auto count : counts)
				if (count > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
					return sdk::unexpected(v4_artifact_error("task-receipt", "signed-bound"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		v4_validate_receipt(const materialization_v4_incremental_receipt& value,
							const bool require_complete)
		{
			if (value.schema != materialization_v4_incremental_receipt_schema ||
				value.task_count == 0U ||
				value.task_count > materialization_v4_incremental_max_tasks ||
				value.task_count != value.task_receipts.size() ||
				!v4_id(value.materialization_request_id) || !v4_id(value.receipt_digest) ||
				(require_complete && !value.complete))
				return sdk::unexpected(v4_artifact_error("receipt", "shape"));
			std::uint64_t claims{};
			std::uint64_t unresolved{};
			std::uint64_t conflicts{};
			std::uint64_t differential{};
			bool complete = true;
			for (std::size_t index{}; index < value.task_receipts.size(); ++index)
			{
				const auto& task = value.task_receipts[index];
				if (auto valid = v4_validate_task_receipt(task); !valid)
					return valid;
				if (task.task_index != index ||
					task.materialization_request_id != value.materialization_request_id)
					return sdk::unexpected(v4_artifact_error("receipt", "order-or-request"));
				for (std::size_t previous{}; previous < index; ++previous)
					if (task.task_id == value.task_receipts[previous].task_id)
						return sdk::unexpected(v4_artifact_error("receipt", "duplicate"));
				const std::array<std::pair<std::uint64_t, std::uint64_t*>, 4U> additions{{
					{task.claim_count, &claims},
					{task.unresolved_count, &unresolved},
					{task.conflict_count, &conflicts},
					{task.differential_disagreement_count, &differential},
				}};
				for (const auto [number, total] : additions)
				{
					if (number > std::numeric_limits<std::uint64_t>::max() - *total)
						return sdk::unexpected(v4_artifact_error("receipt", "overflow"));
					*total += number;
				}
				complete = complete && task.complete;
			}
			if (value.claim_count != claims || value.unresolved_count != unresolved ||
				value.conflict_count != conflicts ||
				value.differential_disagreement_count != differential || value.complete != complete)
				return sdk::unexpected(v4_artifact_error("receipt", "census"));
			auto digest = v4_receipt_digest(value);
			if (!digest || *digest != value.receipt_digest)
				return sdk::unexpected(v4_artifact_error("receipt", "digest"));
			return {};
		}
	} // namespace

	namespace
	{
		[[nodiscard]] sdk::result<sdk::canonical_value>
		v4_publication_value(const materialization_v4_prior_publication_identity& value)
		{
			auto sequence = v4_integer(value.sequence, "publication.sequence");
			auto generation = v4_integer(value.physical_generation, "publication.generation");
			if (!sequence || !generation)
				return sdk::unexpected(v4_artifact_error("publication", "integer"));
			if (!v4_id(value.series_id) || !v4_id(value.publication_id) ||
				!v4_id(value.snapshot_id) ||
				(value.parent_publication && !v4_id(*value.parent_publication)) ||
				!value.committed || value.corrupt)
				return sdk::unexpected(v4_artifact_error("publication", "identity-or-state"));
			return sdk::canonical_value::from_tuple({
				v4_text(value.series_id),
				v4_text(value.publication_id),
				v4_text(value.snapshot_id),
				std::move(*sequence),
				std::move(*generation),
				value.parent_publication ? v4_text(*value.parent_publication)
										 : sdk::canonical_value::null(),
				sdk::canonical_value::from_boolean(value.committed),
				sdk::canonical_value::from_boolean(value.corrupt),
			});
		}

		[[nodiscard]] sdk::result<materialization_v4_prior_publication_identity>
		v4_decode_publication(const sdk::canonical_value& value)
		{
			auto fields = v4_tuple(value, 8U, "publication");
			if (!fields)
				return sdk::unexpected(std::move(fields.error()));
			auto series = v4_decode_string((*fields)[0U], "publication.series");
			auto publication = v4_decode_string((*fields)[1U], "publication.id");
			auto snapshot = v4_decode_string((*fields)[2U], "publication.snapshot");
			auto sequence = v4_decode_integer((*fields)[3U], "publication.sequence");
			auto generation = v4_decode_integer((*fields)[4U], "publication.generation");
			auto parent = v4_optional_string((*fields)[5U], "publication.parent");
			auto committed = v4_decode_bool((*fields)[6U], "publication.committed");
			auto corrupt = v4_decode_bool((*fields)[7U], "publication.corrupt");
			if (!series || !publication || !snapshot || !sequence || !generation || !parent ||
				!committed || !corrupt)
				return sdk::unexpected(v4_artifact_error("publication", "field"));
			materialization_v4_prior_publication_identity output{
				std::move(*series),
				std::move(*publication),
				std::move(*snapshot),
				std::move(*parent),
				*sequence,
				*generation,
				*committed,
				*corrupt,
			};
			if (auto encoded = v4_publication_value(output); !encoded)
				return sdk::unexpected(std::move(encoded.error()));
			return output;
		}

		[[nodiscard]] sdk::result<sdk::canonical_value>
		v4_authority_value(const materialization_v4_store_publication_authority& value)
		{
			if (!v4_id(value.analysis_recipe_digest) || !v4_id(value.output_plan_digest) ||
				!v4_id(value.publication_target))
				return sdk::unexpected(v4_artifact_error("authority", "strong-id"));
			return sdk::canonical_value::from_tuple({
				v4_text(value.analysis_recipe_digest),
				v4_text(value.output_plan_digest),
				v4_text(value.publication_target),
			});
		}

		[[nodiscard]] sdk::result<materialization_v4_store_publication_authority>
		v4_decode_authority(const sdk::canonical_value& value)
		{
			auto fields = v4_tuple(value, 3U, "authority");
			if (!fields)
				return sdk::unexpected(std::move(fields.error()));
			auto recipe = v4_decode_string((*fields)[0U], "authority.recipe");
			auto plan = v4_decode_string((*fields)[1U], "authority.plan");
			auto target = v4_decode_string((*fields)[2U], "authority.target");
			if (!recipe || !plan || !target)
				return sdk::unexpected(v4_artifact_error("authority", "field"));
			materialization_v4_store_publication_authority output{
				std::move(*recipe), std::move(*plan), std::move(*target)};
			if (auto encoded = v4_authority_value(output); !encoded)
				return sdk::unexpected(std::move(encoded.error()));
			return output;
		}

		[[nodiscard]] sdk::result<sdk::canonical_value>
		v4_receipt_value(const materialization_v4_incremental_receipt& value)
		{
			auto task_count = v4_integer(value.task_count, "receipt.task-count");
			auto claims = v4_integer(value.claim_count, "receipt.claim-count");
			auto unresolved = v4_integer(value.unresolved_count, "receipt.unresolved-count");
			auto conflicts = v4_integer(value.conflict_count, "receipt.conflict-count");
			auto differential =
				v4_integer(value.differential_disagreement_count, "receipt.differential-count");
			if (!task_count || !claims || !unresolved || !conflicts || !differential)
				return sdk::unexpected(v4_artifact_error("receipt", "integer"));
			std::vector<sdk::canonical_value> tasks;
			tasks.reserve(value.task_receipts.size());
			for (const auto& task : value.task_receipts)
				tasks.push_back(v4_receipt_task_value(task));
			return sdk::canonical_value::from_tuple({
				v4_text(value.schema),
				v4_text(value.materialization_request_id),
				std::move(*task_count),
				sdk::canonical_value::from_tuple(std::move(tasks)),
				std::move(*claims),
				std::move(*unresolved),
				std::move(*conflicts),
				std::move(*differential),
				sdk::canonical_value::from_boolean(value.complete),
				v4_text(value.receipt_digest),
			});
		}

		[[nodiscard]] sdk::result<materialization_v4_claim_receipt>
		v4_decode_task_receipt(const sdk::canonical_value& value)
		{
			auto fields = v4_tuple(value, 21U, "task-receipt");
			if (!fields)
				return sdk::unexpected(std::move(fields.error()));
			auto schema = v4_decode_string((*fields)[0U], "task.schema");
			auto binding = v4_decode_string((*fields)[1U], "task.binding");
			auto request = v4_decode_string((*fields)[2U], "task.request");
			auto index = v4_decode_integer((*fields)[3U], "task.index");
			auto task_id = v4_decode_string((*fields)[4U], "task.id");
			auto task_digest = v4_decode_string((*fields)[5U], "task.digest");
			auto execution = v4_decode_string((*fields)[6U], "task.execution");
			auto closure_id = v4_decode_string((*fields)[7U], "task.closure-id");
			auto closure_digest = v4_decode_string((*fields)[8U], "task.closure-digest");
			auto manifest = v4_decode_string((*fields)[9U], "task.manifest");
			auto input = v4_decode_string((*fields)[10U], "task.input");
			auto batch = v4_decode_string((*fields)[11U], "task.batch");
			auto partition = v4_decode_string((*fields)[12U], "task.partition");
			auto partition_digest = v4_decode_string((*fields)[13U], "task.partition-digest");
			auto coverage = v4_decode_string((*fields)[14U], "task.coverage");
			auto claims = v4_decode_integer((*fields)[15U], "task.claim-count");
			auto unresolved = v4_decode_integer((*fields)[16U], "task.unresolved-count");
			auto conflicts = v4_decode_integer((*fields)[17U], "task.conflict-count");
			auto differential = v4_decode_integer((*fields)[18U], "task.differential-count");
			auto complete = v4_decode_bool((*fields)[19U], "task.complete");
			auto receipt_digest = v4_decode_string((*fields)[20U], "task.receipt-digest");
			if (!schema || !binding || !request || !index || !task_id || !task_digest ||
				!execution || !closure_id || !closure_digest || !manifest || !input || !batch ||
				!partition || !partition_digest || !coverage || !claims || !unresolved ||
				!conflicts || !differential || !complete || !receipt_digest)
				return sdk::unexpected(v4_artifact_error("task-receipt", "field"));
			materialization_v4_claim_receipt output{
				std::move(*schema),
				std::move(*binding),
				std::move(*request),
				*index,
				std::move(*task_id),
				std::move(*task_digest),
				std::move(*execution),
				std::move(*closure_id),
				std::move(*closure_digest),
				std::move(*manifest),
				std::move(*input),
				std::move(*batch),
				std::move(*partition),
				std::move(*partition_digest),
				std::move(*coverage),
				*claims,
				*unresolved,
				*conflicts,
				*differential,
				*complete,
				std::move(*receipt_digest),
			};
			if (auto valid = v4_validate_task_receipt(output); !valid)
				return sdk::unexpected(std::move(valid.error()));
			return output;
		}

		[[nodiscard]] sdk::result<materialization_v4_incremental_receipt>
		v4_decode_receipt(const sdk::canonical_value& value)
		{
			auto fields = v4_tuple(value, 10U, "receipt");
			if (!fields)
				return sdk::unexpected(std::move(fields.error()));
			auto schema = v4_decode_string((*fields)[0U], "receipt.schema");
			auto request = v4_decode_string((*fields)[1U], "receipt.request");
			auto task_count = v4_decode_integer((*fields)[2U], "receipt.task-count");
			if (!schema || !request || !task_count)
				return sdk::unexpected(v4_artifact_error("receipt", "header"));
			if ((*fields)[3U].type != sdk::canonical_value::kind::ordered_tuple ||
				(*fields)[3U].tuple.empty() ||
				(*fields)[3U].tuple.size() > materialization_v4_incremental_max_tasks)
				return sdk::unexpected(v4_artifact_error("receipt.tasks", "bounded-tuple"));
			auto claims = v4_decode_integer((*fields)[4U], "receipt.claim-count");
			auto unresolved = v4_decode_integer((*fields)[5U], "receipt.unresolved-count");
			auto conflicts = v4_decode_integer((*fields)[6U], "receipt.conflict-count");
			auto differential = v4_decode_integer((*fields)[7U], "receipt.differential-count");
			auto complete = v4_decode_bool((*fields)[8U], "receipt.complete");
			auto digest = v4_decode_string((*fields)[9U], "receipt.digest");
			if (!claims || !unresolved || !conflicts || !differential || !complete || !digest)
				return sdk::unexpected(v4_artifact_error("receipt", "field"));
			materialization_v4_incremental_receipt output;
			output.schema = std::move(*schema);
			output.materialization_request_id = std::move(*request);
			output.task_count = *task_count;
			output.claim_count = *claims;
			output.unresolved_count = *unresolved;
			output.conflict_count = *conflicts;
			output.differential_disagreement_count = *differential;
			output.complete = *complete;
			output.receipt_digest = std::move(*digest);
			output.task_receipts.reserve((*fields)[3U].tuple.size());
			for (const auto& task : (*fields)[3U].tuple)
			{
				auto decoded = v4_decode_task_receipt(task);
				if (!decoded)
					return sdk::unexpected(std::move(decoded.error()));
				output.task_receipts.push_back(std::move(*decoded));
			}
			if (auto valid = v4_validate_receipt(output, true); !valid)
				return sdk::unexpected(std::move(valid.error()));
			return output;
		}

		[[nodiscard]] sdk::result<void>
		v4_validate_artifact(const materialization_v4_prior_artifact& value)
		{
			if (value.version != 1U || !v4_id(value.materialization_request_id) ||
				value.receipt.materialization_request_id != value.materialization_request_id)
				return sdk::unexpected(v4_artifact_error("artifact", "version-or-request"));
			if (auto valid = v4_authority_value(value.authority); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (auto valid = v4_publication_value(value.publication); !valid)
				return sdk::unexpected(std::move(valid.error()));
			return v4_validate_receipt(value.receipt, true);
		}

		[[nodiscard]] sdk::result<sdk::canonical_value>
		v4_artifact_body_value(const materialization_v4_prior_artifact& value)
		{
			if (auto valid = v4_validate_artifact(value); !valid)
				return sdk::unexpected(std::move(valid.error()));
			auto receipt = v4_receipt_value(value.receipt);
			auto publication = v4_publication_value(value.publication);
			auto authority = v4_authority_value(value.authority);
			auto version = v4_integer(value.version, "artifact.version");
			if (!receipt || !publication || !authority || !version)
				return sdk::unexpected(v4_artifact_error("artifact", "encode"));
			return sdk::canonical_value::from_tuple({
				v4_text(materialization_v4_prior_artifact::schema),
				std::move(*version),
				v4_text(value.materialization_request_id),
				std::move(*authority),
				std::move(*publication),
				std::move(*receipt),
			});
		}

		[[nodiscard]] sdk::result<std::string>
		v4_sidecar_path(const std::string_view sqlite_path,
						const std::string_view expected_parent_publication)
		{
			if (auto valid = validate_materialization_sqlite_path(sqlite_path); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (!v4_id(expected_parent_publication))
				return sdk::unexpected(v4_artifact_error("sidecar.parent", "strong-id"));
			const auto bytes = std::as_bytes(std::span<const char>{
				expected_parent_publication.data(), expected_parent_publication.size()});
			const auto digest = sdk::content_digest(bytes);
			if (digest.size() != 71U || !digest.starts_with("sha256:"))
				return sdk::unexpected(v4_artifact_error("sidecar.parent", "digest"));
			std::string path{sqlite_path};
			constexpr std::string_view suffix{".cxxlens-materialization-v4-"};
			path += suffix;
			path.append(digest.substr(7U));
			if (auto valid = validate_materialization_relative_path(path, 4095U, true); !valid)
				return sdk::unexpected(std::move(valid.error()));
			return path;
		}

		[[nodiscard]] sdk::result<std::vector<std::byte>>
		v4_read_spool(materialization_replayable_spool& spool,
					  const materialization_prior_artifact_limits& limits)
		{
			if (!spool.sealed() || spool.size_bytes() == 0U ||
				spool.size_bytes() > limits.max_bytes ||
				spool.size_bytes() > std::numeric_limits<std::size_t>::max())
				return sdk::unexpected(v4_artifact_error("sidecar", "bounds"));
			try
			{
				std::vector<std::byte> bytes(static_cast<std::size_t>(spool.size_bytes()));
				std::size_t offset{};
				while (offset < bytes.size())
				{
					auto read = spool.read_at(offset, std::span{bytes}.subspan(offset));
					if (!read || *read == 0U)
						return sdk::unexpected(v4_artifact_error("sidecar", "read"));
					offset += *read;
				}
				return bytes;
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(v4_artifact_error("sidecar", "allocation"));
			}
		}
	} // namespace

	sdk::result<std::vector<std::byte>>
	encode_materialization_v4_prior_artifact(const materialization_v4_prior_artifact& artifact,
											 const materialization_prior_artifact_limits& limits)
	{
		if (!prior_artifact_limits_valid(limits))
			return sdk::unexpected(v4_artifact_error("limits", "zero"));
		auto body_value = v4_artifact_body_value(artifact);
		if (!body_value)
			return sdk::unexpected(std::move(body_value.error()));
		auto body = sdk::canonical_binary(*body_value);
		if (!body || body->size() > limits.max_bytes)
			return sdk::unexpected(v4_artifact_error("body", "canonical-or-bound"));
		const auto digest = sdk::content_digest(*body);
		if (!artifact.artifact_digest.empty() && artifact.artifact_digest != digest)
			return sdk::unexpected(v4_artifact_error("artifact.digest", "mismatch"));
		auto envelope = sdk::canonical_binary(sdk::canonical_value::from_tuple({
			v4_text(materialization_v4_prior_artifact::schema),
			sdk::canonical_value::from_bytes(std::move(*body)),
			v4_text(digest),
		}));
		if (!envelope || envelope->size() > limits.max_bytes)
			return sdk::unexpected(v4_artifact_error("envelope", "canonical-or-bound"));
		return envelope;
	}

	sdk::result<materialization_v4_prior_artifact>
	decode_materialization_v4_prior_artifact(const std::span<const std::byte> bytes,
											 const materialization_prior_artifact_limits& limits)
	{
		if (!prior_artifact_limits_valid(limits) || bytes.empty() ||
			bytes.size() > limits.max_bytes)
			return sdk::unexpected(v4_artifact_error("envelope", "bounds"));
		auto decoded = sdk::canonical_binary_decode(bytes);
		if (!decoded)
			return sdk::unexpected(v4_artifact_error("envelope", "canonical"));
		auto canonical = sdk::canonical_binary(*decoded);
		if (!canonical || canonical->size() != bytes.size() ||
			!std::ranges::equal(*canonical, bytes))
			return sdk::unexpected(v4_artifact_error("envelope", "noncanonical"));
		auto envelope = v4_tuple(*decoded, 3U, "envelope");
		if (!envelope)
			return sdk::unexpected(std::move(envelope.error()));
		auto schema = v4_decode_string((*envelope)[0U], "envelope.schema");
		if (!schema || *schema != materialization_v4_prior_artifact::schema ||
			(*envelope)[1U].type != sdk::canonical_value::kind::bytes)
			return sdk::unexpected(v4_artifact_error("envelope", "schema-or-body"));
		auto expected_digest = v4_decode_string((*envelope)[2U], "envelope.digest");
		if (!expected_digest)
			return sdk::unexpected(std::move(expected_digest.error()));
		const auto& body_bytes = (*envelope)[1U].byte_string;
		if (!v4_id(*expected_digest) || sdk::content_digest(body_bytes) != *expected_digest)
			return sdk::unexpected(v4_artifact_error("envelope", "digest"));
		auto body_decoded = sdk::canonical_binary_decode(body_bytes);
		if (!body_decoded)
			return sdk::unexpected(v4_artifact_error("body", "canonical"));
		auto body = v4_tuple(*body_decoded, 6U, "body");
		if (!body)
			return sdk::unexpected(std::move(body.error()));
		auto body_schema = v4_decode_string((*body)[0U], "body.schema");
		auto version = v4_decode_integer((*body)[1U], "body.version");
		auto request = v4_decode_string((*body)[2U], "body.request");
		auto authority = v4_decode_authority((*body)[3U]);
		auto publication = v4_decode_publication((*body)[4U]);
		auto receipt = v4_decode_receipt((*body)[5U]);
		if (!body_schema || *body_schema != materialization_v4_prior_artifact::schema || !version ||
			*version != 1U || !request || !authority || !publication || !receipt)
			return sdk::unexpected(v4_artifact_error("body", "field"));
		materialization_v4_prior_artifact output{
			1U,
			std::move(*request),
			std::move(*authority),
			std::move(*publication),
			std::move(*receipt),
			*expected_digest,
		};
		if (auto valid = v4_validate_artifact(output); !valid)
			return sdk::unexpected(std::move(valid.error()));
		return output;
	}

	sdk::result<materialization_v4_prior_artifact_reuse> match_materialization_v4_prior_artifact(
		const materialization_v4_prior_artifact& artifact,
		const std::string_view materialization_request_id,
		const materialization_v4_store_publication_authority& authority,
		const materialization_v4_prior_publication_identity& publication,
		const std::span<const materialization_v4_claim_receipt> current_task_receipts)
	{
		if (auto valid = v4_validate_artifact(artifact); !valid)
			return sdk::unexpected(std::move(valid.error()));
		if (artifact.materialization_request_id != materialization_request_id ||
			artifact.authority != authority || artifact.publication != publication ||
			current_task_receipts.size() != artifact.receipt.task_receipts.size())
			return sdk::unexpected(v4_artifact_error("reuse", "authority-or-publication"));
		for (std::size_t index{}; index < current_task_receipts.size(); ++index)
		{
			if (auto valid = v4_validate_task_receipt(current_task_receipts[index]); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (current_task_receipts[index] != artifact.receipt.task_receipts[index])
				return sdk::unexpected(v4_artifact_error("reuse", "task-identity-or-order"));
		}
		return materialization_v4_prior_artifact_reuse{
			artifact.receipt, 0U, artifact.receipt.task_count};
	}

	sdk::result<void>
	persist_materialization_v4_prior_artifact(const materialization_effect_root& root,
											  const std::string_view sqlite_path,
											  const std::string_view expected_parent_publication,
											  const materialization_v4_prior_artifact& artifact,
											  const materialization_prior_artifact_limits& limits)
	{
		auto path = v4_sidecar_path(sqlite_path, expected_parent_publication);
		if (!path)
			return sdk::unexpected(std::move(path.error()));
		if (!artifact.publication.parent_publication ||
			*artifact.publication.parent_publication != expected_parent_publication)
			return sdk::unexpected(v4_artifact_error("persist.parent", "mismatch"));
		auto encoded = encode_materialization_v4_prior_artifact(artifact, limits);
		if (!encoded)
			return sdk::unexpected(std::move(encoded.error()));
		auto storage = make_materialization_private_spool();
		if (!storage)
			return sdk::unexpected(v4_artifact_error("persist.spool", "create"));
		if (auto appended = (*storage)->append(*encoded); !appended)
			return sdk::unexpected(v4_artifact_error("persist.spool", "append"));
		if (auto sealed = (*storage)->seal(); !sealed)
			return sdk::unexpected(v4_artifact_error("persist.spool", "seal"));
		return install_prior_artifact_sidecar(root, *path, **storage, limits);
	}

	sdk::result<std::optional<materialization_v4_prior_artifact>>
	load_materialization_v4_prior_artifact(
		const materialization_effect_root& root,
		const std::string_view sqlite_path,
		const std::string_view expected_parent_publication,
		const std::string_view materialization_request_id,
		const materialization_v4_store_publication_authority& authority,
		const materialization_v4_prior_publication_identity& publication,
		const materialization_prior_artifact_limits& limits)
	{
		auto path = v4_sidecar_path(sqlite_path, expected_parent_publication);
		if (!path)
			return sdk::unexpected(std::move(path.error()));
		auto opened = root.open_beneath(*path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		if (!opened)
		{
			if (prior_artifact_sidecar_missing(opened.error()))
				return std::optional<materialization_v4_prior_artifact>{};
			return sdk::unexpected(std::move(opened.error()));
		}
		auto spool = spool_prior_artifact_sidecar(*opened, limits);
		if (!spool)
			return sdk::unexpected(std::move(spool.error()));
		auto bytes = v4_read_spool(**spool, limits);
		if (!bytes)
			return sdk::unexpected(std::move(bytes.error()));
		auto artifact = decode_materialization_v4_prior_artifact(*bytes, limits);
		if (!artifact)
			return sdk::unexpected(std::move(artifact.error()));
		if (artifact->materialization_request_id != materialization_request_id ||
			artifact->authority != authority || artifact->publication != publication ||
			!artifact->publication.parent_publication ||
			*artifact->publication.parent_publication != expected_parent_publication)
			return sdk::unexpected(v4_artifact_error("load", "stale-authority-or-publication"));
		return std::optional<materialization_v4_prior_artifact>{std::move(*artifact)};
	}
} // namespace cxxlens::detail::clang22::materialization
