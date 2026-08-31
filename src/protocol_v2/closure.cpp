#include "closure.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <ranges>
#include <set>

namespace cxxlens::protocol_v2
{
	namespace
	{
		[[nodiscard]] sdk::error
		failure(const std::string_view field,
				const std::string_view detail,
				const std::string_view code = "source-closure.protocol-state-invalid")
		{
			return {std::string{code}, std::string{field}, std::string{detail}};
		}

		[[nodiscard]] cbor::value text(const std::string_view value)
		{
			return cbor::value{std::string{value}};
		}

		[[nodiscard]] cbor::value uint(const std::uint64_t value)
		{
			return cbor::value{value};
		}

		void add(cbor::map& output, std::string_view key, cbor::value value)
		{
			output.emplace_back(std::string{key}, std::move(value));
		}

		[[nodiscard]] sdk::result<bytes> encode_map(cbor::map fields)
		{
			return cbor::encode(cbor::value{std::move(fields)});
		}

		// NOLINTBEGIN(bugprone-easily-swappable-parameters): value-field contract order.
		[[nodiscard]] sdk::result<void> valid_text(const std::string_view value,
												   const std::string_view field,
												   const std::size_t maximum = 4'096U)
		{
			// NOLINTEND(bugprone-easily-swappable-parameters)
			if (value.empty() || value.size() > maximum || !cbor::valid_utf8(value))
				return sdk::unexpected(
					failure(field, "typed-id-or-text", "source-closure.manifest-invalid"));
			for (const auto character : value)
			{
				const auto byte_value = static_cast<unsigned char>(character);
				if (byte_value < 0x20U || byte_value == 0x7fU)
					return sdk::unexpected(
						failure(field, "control-character", "source-closure.manifest-invalid"));
			}
			return {};
		}

		[[nodiscard]] bool hex64(const std::string_view value) noexcept
		{
			if (value.size() != 64U)
				return false;
			for (const auto character : value)
				if ((character < '0' || character > '9') && (character < 'a' || character > 'f'))
					return false;
			return true;
		}

		[[nodiscard]] bool semantic_digest(const std::string_view value) noexcept
		{
			constexpr std::string_view prefix{"semantic-v2:sha256:"};
			return value.starts_with(prefix) && hex64(value.substr(prefix.size()));
		}

		[[nodiscard]] bool closure_id(const std::string_view value) noexcept
		{
			constexpr std::string_view prefix{"source-closure:semantic-v2:sha256:"};
			return value.starts_with(prefix) && hex64(value.substr(prefix.size()));
		}

		[[nodiscard]] bool content_digest(const std::string_view value) noexcept
		{
			constexpr std::string_view prefix{"sha256:"};
			return value.starts_with(prefix) && hex64(value.substr(prefix.size()));
		}

		// NOLINTBEGIN(bugprone-easily-swappable-parameters): value-field contract order.
		[[nodiscard]] sdk::result<void> valid_semantic(const std::string_view value,
													   const std::string_view field)
		{
			// NOLINTEND(bugprone-easily-swappable-parameters)
			if (!semantic_digest(value))
				return sdk::unexpected(
					failure(field, "semantic-digest", "source-closure.digest-mismatch"));
			return {};
		}

		// NOLINTBEGIN(bugprone-easily-swappable-parameters): value-field contract order.
		[[nodiscard]] sdk::result<void> valid_content(const std::string_view value,
													  const std::string_view field)
		{
			// NOLINTEND(bugprone-easily-swappable-parameters)
			if (!content_digest(value))
				return sdk::unexpected(
					failure(field, "content-digest", "source-closure.digest-mismatch"));
			return {};
		}

		// NOLINTBEGIN(bugprone-easily-swappable-parameters): closure shape wire order.
		[[nodiscard]] sdk::result<void> valid_shape(const std::uint64_t total_bytes,
													const std::uint64_t chunk_bytes,
													const std::uint64_t chunk_count,
													const std::size_t max_chunk_bytes,
													const std::size_t max_chunks,
													const std::string_view field)
		{
			// NOLINTEND(bugprone-easily-swappable-parameters)
			if (chunk_bytes == 0U || chunk_bytes > max_chunk_bytes)
				return sdk::unexpected(
					failure(field, "chunk-size", "source-closure.limit-exceeded"));
			const auto expected = total_bytes == 0U
				? 0U
				: (total_bytes / chunk_bytes) + (total_bytes % chunk_bytes == 0U ? 0U : 1U);
			if (expected != chunk_count || chunk_count > max_chunks)
				return sdk::unexpected(
					failure(field, "chunk-shape", "source-closure.chunk-order-invalid"));
			return {};
		}

		template <typename T>
		[[nodiscard]] sdk::result<void> validate_manifest_descriptor(const T& value,
																	 const closure_limits bound)
		{
			if (value.kind != manifest_kind::descriptor)
				return sdk::unexpected(
					failure("kind", "descriptor-required", "source-closure.manifest-invalid"));
			for (const auto& item :
				 {std::pair{std::string_view{"session_id"}, std::string_view{value.session_id}},
				  std::pair{std::string_view{"task_id"}, std::string_view{value.task_id}},
				  std::pair{std::string_view{"task_v4_digest"},
							std::string_view{value.task_v4_digest}},
				  std::pair{std::string_view{"closure_id"}, std::string_view{value.closure_id}}})
				if (auto valid = valid_text(item.second, item.first); !valid)
					return valid;
			if (!closure_id(value.closure_id))
				return sdk::unexpected(
					failure("closure_id", "typed-closure-id", "source-closure.manifest-invalid"));
			for (const auto& item : {std::pair{std::string_view{"task_v4_digest"},
											   std::string_view{value.task_v4_digest}},
									 std::pair{std::string_view{"closure_digest"},
											   std::string_view{value.closure_digest}},
									 std::pair{std::string_view{"manifest_digest"},
											   std::string_view{value.manifest_digest}}})
				if (auto valid = valid_semantic(item.second, item.first); !valid)
					return valid;
			if (value.total_bytes > bound.maximum_manifest_bytes)
				return sdk::unexpected(
					failure("total_bytes", "limit-exceeded", "source-closure.limit-exceeded"));
			return valid_shape(value.total_bytes,
							   value.chunk_bytes,
							   value.chunk_count,
							   bound.maximum_chunk_payload_bytes,
							   bound.maximum_manifest_chunks,
							   "manifest");
		}

		template <typename T>
		[[nodiscard]] sdk::result<void> validate_manifest_chunk(const T& value,
																const closure_limits bound)
		{
			if (value.kind != manifest_kind::chunk)
				return sdk::unexpected(
					failure("kind", "chunk-required", "source-closure.manifest-invalid"));
			for (const auto& item :
				 {std::pair{std::string_view{"session_id"}, std::string_view{value.session_id}},
				  std::pair{std::string_view{"task_id"}, std::string_view{value.task_id}}})
				if (auto valid = valid_text(item.second, item.first); !valid)
					return valid;
			if (auto valid = valid_semantic(value.manifest_digest, "manifest_digest"); !valid)
				return valid;
			if (value.byte_count == 0U || value.byte_count > bound.maximum_chunk_payload_bytes)
				return sdk::unexpected(
					failure("byte_count", "limit-exceeded", "source-closure.limit-exceeded"));
			if (value.offset > bound.maximum_manifest_bytes ||
				value.byte_count > bound.maximum_manifest_bytes - value.offset)
				return sdk::unexpected(
					failure("offset", "limit-exceeded", "source-closure.limit-exceeded"));
			return {};
		}

		template <typename T>
		[[nodiscard]] sdk::result<void> validate_blob_descriptor(const T& value,
																 const closure_limits bound)
		{
			for (const auto& item :
				 {std::pair{std::string_view{"session_id"}, std::string_view{value.session_id}},
				  std::pair{std::string_view{"task_id"}, std::string_view{value.task_id}}})
				if (auto valid = valid_text(item.second, item.first); !valid)
					return valid;
			if (auto valid = valid_semantic(value.closure_digest, "closure_digest"); !valid)
				return valid;
			if (auto valid = valid_content(value.blob_digest, "blob_digest"); !valid)
				return valid;
			if (value.blob_ordinal >= bound.maximum_blobs ||
				value.total_bytes > bound.maximum_blob_bytes ||
				value.total_bytes > bound.maximum_unique_blob_bytes)
				return sdk::unexpected(
					failure("blob_ordinal", "limit-exceeded", "source-closure.limit-exceeded"));
			return valid_shape(value.total_bytes,
							   value.chunk_bytes,
							   value.chunk_count,
							   bound.maximum_chunk_payload_bytes,
							   bound.maximum_chunks_per_blob,
							   "blob");
		}

		template <typename T>
		[[nodiscard]] sdk::result<void> validate_blob_chunk(const T& value,
															const closure_limits bound)
		{
			for (const auto& item :
				 {std::pair{std::string_view{"session_id"}, std::string_view{value.session_id}},
				  std::pair{std::string_view{"task_id"}, std::string_view{value.task_id}}})
				if (auto valid = valid_text(item.second, item.first); !valid)
					return valid;
			if (auto valid = valid_content(value.blob_digest, "blob_digest"); !valid)
				return valid;
			if (value.blob_ordinal >= bound.maximum_blobs || value.byte_count == 0U ||
				value.byte_count > bound.maximum_chunk_payload_bytes ||
				value.offset > bound.maximum_blob_bytes ||
				value.byte_count > bound.maximum_blob_bytes - value.offset)
				return sdk::unexpected(
					failure("chunk", "limit-exceeded", "source-closure.limit-exceeded"));
			return {};
		}

		template <typename T>
		[[nodiscard]] sdk::result<void> validate_seal(const T& value, const closure_limits bound)
		{
			for (const auto& item :
				 {std::pair{std::string_view{"session_id"}, std::string_view{value.session_id}},
				  std::pair{std::string_view{"task_id"}, std::string_view{value.task_id}}})
				if (auto valid = valid_text(item.second, item.first); !valid)
					return valid;
			for (const auto& item : {std::pair{std::string_view{"task_v4_digest"},
											   std::string_view{value.task_v4_digest}},
									 std::pair{std::string_view{"manifest_digest"},
											   std::string_view{value.manifest_digest}},
									 std::pair{std::string_view{"blob_receipts_digest"},
											   std::string_view{value.blob_receipts_digest}},
									 std::pair{std::string_view{"closure_digest"},
											   std::string_view{value.closure_digest}},
									 std::pair{std::string_view{"transfer_digest"},
											   std::string_view{value.transfer_digest}}})
				if (auto valid = valid_semantic(item.second, item.first); !valid)
					return valid;
			if (value.blob_count > bound.maximum_blobs ||
				value.total_bytes > bound.maximum_task_spool_bytes)
				return sdk::unexpected(
					failure("seal", "limit-exceeded", "source-closure.limit-exceeded"));
			return {};
		}

		template <typename T>
		[[nodiscard]] sdk::result<void> validate_ack(const T& value)
		{
			for (const auto& item :
				 {std::pair{std::string_view{"session_id"}, std::string_view{value.session_id}},
				  std::pair{std::string_view{"task_id"}, std::string_view{value.task_id}},
				  std::pair{std::string_view{"spool_receipt"},
							std::string_view{value.spool_receipt}},
				  std::pair{std::string_view{"cleanup_owner"},
							std::string_view{value.cleanup_owner}}})
				if (auto valid = valid_text(item.second, item.first); !valid)
					return valid;
			for (const auto& item : {std::pair{std::string_view{"closure_digest"},
											   std::string_view{value.closure_digest}},
									 std::pair{std::string_view{"transfer_digest"},
											   std::string_view{value.transfer_digest}}})
				if (auto valid = valid_semantic(item.second, item.first); !valid)
					return valid;
			return {};
		}

		[[nodiscard]] sdk::result<void> validate_reject(const source_closure_reject& value)
		{
			for (const auto& item :
				 {std::pair{std::string_view{"session_id"}, std::string_view{value.session_id}},
				  std::pair{std::string_view{"task_id"}, std::string_view{value.task_id}},
				  std::pair{std::string_view{"failure_phase"},
							std::string_view{value.failure_phase}},
				  std::pair{std::string_view{"reason_code"}, std::string_view{value.reason_code}},
				  std::pair{std::string_view{"cleanup_receipt"},
							std::string_view{value.cleanup_receipt}}})
				if (auto valid = valid_text(item.second, item.first); !valid)
					return valid;
			const auto valid_phase = [](const std::string_view phase) noexcept
			{
				return phase == "before-manifest" || phase == "manifest-streaming" ||
					phase == "manifest-validated" || phase == "blob-streaming" ||
					phase == "closure-sealed" || phase == "acknowledged" || phase == "local-only";
			};
			const auto valid_reason = [](const std::string_view reason) noexcept
			{
				constexpr std::array<std::string_view, 17U> reasons{
					"source-closure.required-feature-missing",
					"source-closure.protocol-state-invalid",
					"source-closure.manifest-invalid",
					"source-closure.blob-order-invalid",
					"source-closure.chunk-order-invalid",
					"source-closure.chunk-overlap",
					"source-closure.chunk-gap",
					"source-closure.digest-mismatch",
					"source-closure.limit-exceeded",
					"source-closure.task-binding-mismatch",
					"source-closure.session-binding-mismatch",
					"source-closure.replay-invalid",
					"source-closure.cancelled",
					"source-closure.transfer-timeout",
					"source-closure.spool-io",
					"source-closure.cleanup-failed",
					"source-closure.ambient-fallback-denied"};
				return std::ranges::find(reasons, reason) != reasons.end();
			};
			if (!valid_phase(value.failure_phase) || !valid_reason(value.reason_code))
				return sdk::unexpected(failure(
					"failure", "unknown-phase-or-reason", "source-closure.protocol-state-invalid"));
			if (value.observed_counters.size() > 32U)
				return sdk::unexpected(failure(
					"observed_counters", "limit-exceeded", "source-closure.limit-exceeded"));
			std::set<std::string, std::less<>> keys;
			for (const auto& item : value.observed_counters)
			{
				if (!keys.insert(item.first).second ||
					!valid_text(item.first, "observed_counters.key", 128U))
					return sdk::unexpected(failure(
						"observed_counters", "invalid-key", "source-closure.manifest-invalid"));
				if (std::get_if<std::uint64_t>(&item.second.data) == nullptr)
					return sdk::unexpected(failure(
						"observed_counters", "uint-required", "source-closure.manifest-invalid"));
			}
			return {};
		}

		[[nodiscard]] sdk::result<cbor::map>
		map_for(const message_type type, const closure_control& control, const closure_limits bound)
		{
			cbor::map fields;
			if (type == message_type::source_closure_manifest)
			{
				const auto* manifest = std::get_if<source_closure_manifest>(&control);
				if (manifest == nullptr)
					return sdk::unexpected(failure("control", "type-mismatch"));
				if (const auto* descriptor =
						std::get_if<source_closure_manifest_descriptor>(manifest);
					descriptor != nullptr)
				{
					if (auto valid = validate_manifest_descriptor(*descriptor, bound); !valid)
						return sdk::unexpected(valid.error());
					add(fields, "kind", text("descriptor"));
					add(fields, "session_id", text(descriptor->session_id));
					add(fields, "task_id", text(descriptor->task_id));
					add(fields, "task_v4_digest", text(descriptor->task_v4_digest));
					add(fields, "closure_id", text(descriptor->closure_id));
					add(fields, "closure_digest", text(descriptor->closure_digest));
					add(fields, "manifest_digest", text(descriptor->manifest_digest));
					add(fields, "total_bytes", uint(descriptor->total_bytes));
					add(fields, "chunk_bytes", uint(descriptor->chunk_bytes));
					add(fields, "chunk_count", uint(descriptor->chunk_count));
				}
				else if (const auto* chunk = std::get_if<source_closure_manifest_chunk>(manifest);
						 chunk != nullptr)
				{
					if (auto valid = validate_manifest_chunk(*chunk, bound); !valid)
						return sdk::unexpected(valid.error());
					add(fields, "kind", text("chunk"));
					add(fields, "session_id", text(chunk->session_id));
					add(fields, "task_id", text(chunk->task_id));
					add(fields, "manifest_digest", text(chunk->manifest_digest));
					add(fields, "chunk_index", uint(chunk->chunk_index));
					add(fields, "offset", uint(chunk->offset));
					add(fields, "byte_count", uint(chunk->byte_count));
				}
				else
					return sdk::unexpected(failure("control", "empty-manifest-variant"));
				return fields;
			}

			if (type == message_type::source_closure_blob)
			{
				const auto* item = std::get_if<source_closure_blob_descriptor>(&control);
				if (item == nullptr)
					return sdk::unexpected(failure("control", "type-mismatch"));
				if (auto valid = validate_blob_descriptor(*item, bound); !valid)
					return sdk::unexpected(valid.error());
				add(fields, "session_id", text(item->session_id));
				add(fields, "task_id", text(item->task_id));
				add(fields, "closure_digest", text(item->closure_digest));
				add(fields, "blob_ordinal", uint(item->blob_ordinal));
				add(fields, "blob_digest", text(item->blob_digest));
				add(fields, "total_bytes", uint(item->total_bytes));
				add(fields, "chunk_bytes", uint(item->chunk_bytes));
				add(fields, "chunk_count", uint(item->chunk_count));
				return fields;
			}
			if (type == message_type::source_closure_chunk)
			{
				const auto* item = std::get_if<source_closure_chunk>(&control);
				if (item == nullptr)
					return sdk::unexpected(failure("control", "type-mismatch"));
				if (auto valid = validate_blob_chunk(*item, bound); !valid)
					return sdk::unexpected(valid.error());
				add(fields, "session_id", text(item->session_id));
				add(fields, "task_id", text(item->task_id));
				add(fields, "blob_ordinal", uint(item->blob_ordinal));
				add(fields, "blob_digest", text(item->blob_digest));
				add(fields, "chunk_index", uint(item->chunk_index));
				add(fields, "offset", uint(item->offset));
				add(fields, "byte_count", uint(item->byte_count));
				return fields;
			}
			if (type == message_type::source_closure_seal)
			{
				const auto* item = std::get_if<source_closure_seal>(&control);
				if (item == nullptr)
					return sdk::unexpected(failure("control", "type-mismatch"));
				if (auto valid = validate_seal(*item, bound); !valid)
					return sdk::unexpected(valid.error());
				add(fields, "session_id", text(item->session_id));
				add(fields, "task_id", text(item->task_id));
				add(fields, "task_v4_digest", text(item->task_v4_digest));
				add(fields, "manifest_digest", text(item->manifest_digest));
				add(fields, "blob_receipts_digest", text(item->blob_receipts_digest));
				add(fields, "blob_count", uint(item->blob_count));
				add(fields, "total_bytes", uint(item->total_bytes));
				add(fields, "closure_digest", text(item->closure_digest));
				add(fields, "transfer_digest", text(item->transfer_digest));
				return fields;
			}
			if (type == message_type::source_closure_ack)
			{
				const auto* item = std::get_if<source_closure_ack>(&control);
				if (item == nullptr)
					return sdk::unexpected(failure("control", "type-mismatch"));
				if (auto valid = validate_ack(*item); !valid)
					return sdk::unexpected(valid.error());
				add(fields, "session_id", text(item->session_id));
				add(fields, "task_id", text(item->task_id));
				add(fields, "closure_digest", text(item->closure_digest));
				add(fields, "transfer_digest", text(item->transfer_digest));
				add(fields, "spool_receipt", text(item->spool_receipt));
				add(fields, "cleanup_owner", text(item->cleanup_owner));
				return fields;
			}
			if (type == message_type::source_closure_reject)
			{
				const auto* item = std::get_if<source_closure_reject>(&control);
				if (item == nullptr)
					return sdk::unexpected(failure("control", "type-mismatch"));
				if (auto valid = validate_reject(*item); !valid)
					return sdk::unexpected(valid.error());
				add(fields, "session_id", text(item->session_id));
				add(fields, "task_id", text(item->task_id));
				add(fields, "failure_phase", text(item->failure_phase));
				add(fields, "reason_code", text(item->reason_code));
				add(fields, "observed_counters", cbor::value{item->observed_counters});
				add(fields, "cleanup_receipt", text(item->cleanup_receipt));
				return fields;
			}
			return sdk::unexpected(failure("message_type", "not-closure"));
		}

		enum class wire_field : std::uint8_t
		{
			kind,
			session_id,
			task_id,
			task_v4_digest,
			closure_id,
			closure_digest,
			manifest_digest,
			total_bytes,
			chunk_bytes,
			chunk_count,
			chunk_index,
			offset,
			byte_count,
			blob_ordinal,
			blob_digest,
			blob_receipts_digest,
			blob_count,
			transfer_digest,
			spool_receipt,
			cleanup_owner,
			failure_phase,
			reason_code,
			observed_counters,
			cleanup_receipt,
			unknown,
		};

		enum text_slot : std::uint8_t
		{
			session_text,
			task_text,
			task_v4_text,
			closure_id_text,
			closure_digest_text,
			manifest_digest_text,
			blob_digest_text,
			blob_receipts_text,
			transfer_digest_text,
			spool_receipt_text,
			cleanup_owner_text,
			failure_phase_text,
			reason_code_text,
			cleanup_receipt_text,
		};

		enum number_slot : std::uint8_t
		{
			total_number,
			chunk_bytes_number,
			chunk_count_number,
			blob_ordinal_number,
			chunk_index_number,
			offset_number,
			byte_count_number,
			blob_count_number,
		};

		[[nodiscard]] constexpr std::uint32_t field_bit(const wire_field field) noexcept
		{
			return std::uint32_t{1U} << static_cast<std::uint8_t>(field);
		}

		[[nodiscard]] wire_field identify_field(const std::string_view key) noexcept
		{
			if (key == "kind")
				return wire_field::kind;
			if (key == "session_id")
				return wire_field::session_id;
			if (key == "task_id")
				return wire_field::task_id;
			if (key == "task_v4_digest")
				return wire_field::task_v4_digest;
			if (key == "closure_id")
				return wire_field::closure_id;
			if (key == "closure_digest")
				return wire_field::closure_digest;
			if (key == "manifest_digest")
				return wire_field::manifest_digest;
			if (key == "total_bytes")
				return wire_field::total_bytes;
			if (key == "chunk_bytes")
				return wire_field::chunk_bytes;
			if (key == "chunk_count")
				return wire_field::chunk_count;
			if (key == "chunk_index")
				return wire_field::chunk_index;
			if (key == "offset")
				return wire_field::offset;
			if (key == "byte_count")
				return wire_field::byte_count;
			if (key == "blob_ordinal")
				return wire_field::blob_ordinal;
			if (key == "blob_digest")
				return wire_field::blob_digest;
			if (key == "blob_receipts_digest")
				return wire_field::blob_receipts_digest;
			if (key == "blob_count")
				return wire_field::blob_count;
			if (key == "transfer_digest")
				return wire_field::transfer_digest;
			if (key == "spool_receipt")
				return wire_field::spool_receipt;
			if (key == "cleanup_owner")
				return wire_field::cleanup_owner;
			if (key == "failure_phase")
				return wire_field::failure_phase;
			if (key == "reason_code")
				return wire_field::reason_code;
			if (key == "observed_counters")
				return wire_field::observed_counters;
			if (key == "cleanup_receipt")
				return wire_field::cleanup_receipt;
			return wire_field::unknown;
		}

		struct wire_head
		{
			std::uint8_t major{};
			std::uint64_t argument{};
			std::size_t next{};
			bool valid{};
		};

		[[nodiscard]] wire_head read_wire_head(const std::span<const byte> input,
											   const std::size_t offset) noexcept
		{
			if (offset >= input.size())
				return {};
			const auto initial = std::to_integer<std::uint8_t>(input[offset]);
			const auto additional = initial & 0x1fU;
			if (additional >= 28U)
				return {};
			if (additional < 24U)
				return {static_cast<std::uint8_t>(initial >> 5U), additional, offset + 1U, true};
			const auto width = additional == 24U ? 1U
				: additional == 25U				 ? 2U
				: additional == 26U				 ? 4U
												 : 8U;
			if (width > input.size() - offset - 1U)
				return {};
			std::uint64_t argument{};
			for (std::size_t index{}; index < width; ++index)
				argument =
					(argument << 8U) | std::to_integer<std::uint64_t>(input[offset + 1U + index]);
			return {static_cast<std::uint8_t>(initial >> 5U), argument, offset + 1U + width, true};
		}

		[[nodiscard]] bool parse_text_range(const std::span<const byte> input,
											const std::size_t offset,
											closure_text_range& output,
											std::size_t& next) noexcept
		{
			const auto head = read_wire_head(input, offset);
			if (!head.valid || head.major != 3U || head.argument > input.size() - head.next ||
				head.argument > std::numeric_limits<std::uint32_t>::max() ||
				head.next > std::numeric_limits<std::uint32_t>::max())
				return false;
			output = {static_cast<std::uint32_t>(head.next),
					  static_cast<std::uint32_t>(head.argument)};
			next = head.next + static_cast<std::size_t>(head.argument);
			return true;
		}

		[[nodiscard]] std::string_view text_view(const std::span<const byte> input,
												 const closure_text_range range) noexcept
		{
			return {reinterpret_cast<const char*>(input.data() + range.offset), range.size};
		}

		// NOLINTBEGIN(bugprone-easily-swappable-parameters): decoded value-cursor order.
		[[nodiscard]] bool parse_uint(const std::span<const byte> input,
									  const std::size_t offset,
									  std::uint64_t& output,
									  std::size_t& next) noexcept
		{
			// NOLINTEND(bugprone-easily-swappable-parameters)
			const auto head = read_wire_head(input, offset);
			if (!head.valid || head.major != 0U)
				return false;
			output = head.argument;
			next = head.next;
			return true;
		}

		[[nodiscard]] bool parse_counter_map(const std::span<const byte> input,
											 const std::size_t offset,
											 closure_text_range& encoded,
											 std::size_t& next) noexcept
		{
			const auto head = read_wire_head(input, offset);
			if (!head.valid || head.major != 5U || head.argument > 32U)
				return false;
			auto cursor = head.next;
			for (std::uint64_t index{}; index < head.argument; ++index)
			{
				closure_text_range key_range;
				if (!parse_text_range(input, cursor, key_range, cursor))
					return false;
				const auto key = text_view(input, key_range);
				if (!valid_text(key, "observed_counters.key", 128U))
					return false;
				std::uint64_t ignored{};
				if (!parse_uint(input, cursor, ignored, cursor))
					return false;
			}
			if (offset > std::numeric_limits<std::uint32_t>::max() ||
				cursor - offset > std::numeric_limits<std::uint32_t>::max())
				return false;
			encoded = {static_cast<std::uint32_t>(offset),
					   static_cast<std::uint32_t>(cursor - offset)};
			next = cursor;
			return true;
		}

		[[nodiscard]] constexpr std::uint32_t
		mask(const std::initializer_list<wire_field> fields) noexcept
		{
			std::uint32_t output{};
			for (const auto field : fields)
				output |= field_bit(field);
			return output;
		}

		[[nodiscard]] constexpr std::uint32_t expected_mask(const message_type type,
															const manifest_kind kind) noexcept
		{
			if (type == message_type::source_closure_manifest && kind == manifest_kind::descriptor)
				return mask({wire_field::kind,
							 wire_field::session_id,
							 wire_field::task_id,
							 wire_field::task_v4_digest,
							 wire_field::closure_id,
							 wire_field::closure_digest,
							 wire_field::manifest_digest,
							 wire_field::total_bytes,
							 wire_field::chunk_bytes,
							 wire_field::chunk_count});
			if (type == message_type::source_closure_manifest)
				return mask({wire_field::kind,
							 wire_field::session_id,
							 wire_field::task_id,
							 wire_field::manifest_digest,
							 wire_field::chunk_index,
							 wire_field::offset,
							 wire_field::byte_count});
			if (type == message_type::source_closure_blob)
				return mask({wire_field::session_id,
							 wire_field::task_id,
							 wire_field::closure_digest,
							 wire_field::blob_ordinal,
							 wire_field::blob_digest,
							 wire_field::total_bytes,
							 wire_field::chunk_bytes,
							 wire_field::chunk_count});
			if (type == message_type::source_closure_chunk)
				return mask({wire_field::session_id,
							 wire_field::task_id,
							 wire_field::blob_ordinal,
							 wire_field::blob_digest,
							 wire_field::chunk_index,
							 wire_field::offset,
							 wire_field::byte_count});
			if (type == message_type::source_closure_seal)
				return mask({wire_field::session_id,
							 wire_field::task_id,
							 wire_field::task_v4_digest,
							 wire_field::manifest_digest,
							 wire_field::blob_receipts_digest,
							 wire_field::blob_count,
							 wire_field::total_bytes,
							 wire_field::closure_digest,
							 wire_field::transfer_digest});
			if (type == message_type::source_closure_ack)
				return mask({wire_field::session_id,
							 wire_field::task_id,
							 wire_field::closure_digest,
							 wire_field::transfer_digest,
							 wire_field::spool_receipt,
							 wire_field::cleanup_owner});
			if (type == message_type::source_closure_reject)
				return mask({wire_field::session_id,
							 wire_field::task_id,
							 wire_field::failure_phase,
							 wire_field::reason_code,
							 wire_field::observed_counters,
							 wire_field::cleanup_receipt});
			return 0U;
		}

		[[nodiscard]] bool parse_field_value(const wire_field field,
											 const std::span<const byte> input,
											 const std::size_t offset,
											 closure_parsed_control& output,
											 std::size_t& next) noexcept
		{
			const auto parse_text = [&](const text_slot slot)
			{
				return parse_text_range(input, offset, output.text.at(slot), next);
			};
			const auto parse_number = [&](const number_slot slot)
			{
				return parse_uint(input, offset, output.number.at(slot), next);
			};
			switch (field)
			{
				case wire_field::kind:
				{
					closure_text_range range;
					if (!parse_text_range(input, offset, range, next))
						return false;
					const auto kind = text_view(input, range);
					if (kind == "descriptor")
						output.kind = manifest_kind::descriptor;
					else if (kind == "chunk")
						output.kind = manifest_kind::chunk;
					else
						return false;
					return true;
				}
				case wire_field::session_id:
					return parse_text(session_text);
				case wire_field::task_id:
					return parse_text(task_text);
				case wire_field::task_v4_digest:
					return parse_text(task_v4_text);
				case wire_field::closure_id:
					return parse_text(closure_id_text);
				case wire_field::closure_digest:
					return parse_text(closure_digest_text);
				case wire_field::manifest_digest:
					return parse_text(manifest_digest_text);
				case wire_field::blob_digest:
					return parse_text(blob_digest_text);
				case wire_field::blob_receipts_digest:
					return parse_text(blob_receipts_text);
				case wire_field::transfer_digest:
					return parse_text(transfer_digest_text);
				case wire_field::spool_receipt:
					return parse_text(spool_receipt_text);
				case wire_field::cleanup_owner:
					return parse_text(cleanup_owner_text);
				case wire_field::failure_phase:
					return parse_text(failure_phase_text);
				case wire_field::reason_code:
					return parse_text(reason_code_text);
				case wire_field::cleanup_receipt:
					return parse_text(cleanup_receipt_text);
				case wire_field::total_bytes:
					return parse_number(total_number);
				case wire_field::chunk_bytes:
					return parse_number(chunk_bytes_number);
				case wire_field::chunk_count:
					return parse_number(chunk_count_number);
				case wire_field::blob_ordinal:
					return parse_number(blob_ordinal_number);
				case wire_field::chunk_index:
					return parse_number(chunk_index_number);
				case wire_field::offset:
					return parse_number(offset_number);
				case wire_field::byte_count:
					return parse_number(byte_count_number);
				case wire_field::blob_count:
					return parse_number(blob_count_number);
				case wire_field::observed_counters:
					return parse_counter_map(input, offset, output.observed_counters, next);
				case wire_field::unknown:
					return false;
			}
			return false;
		}

		[[nodiscard]] closure_control_view
		make_control_view(const message_type type,
						  const std::span<const byte> control,
						  const closure_parsed_control& parsed) noexcept
		{
			const auto text = [&](const text_slot slot)
			{
				return text_view(control, parsed.text.at(slot));
			};
			const auto number = [&](const number_slot slot)
			{
				return parsed.number.at(slot);
			};
			if (type == message_type::source_closure_manifest &&
				parsed.kind == manifest_kind::descriptor)
				return source_closure_manifest_view{
					source_closure_manifest_descriptor_view{parsed.kind,
															text(session_text),
															text(task_text),
															text(task_v4_text),
															text(closure_id_text),
															text(closure_digest_text),
															text(manifest_digest_text),
															number(total_number),
															number(chunk_bytes_number),
															number(chunk_count_number)}};
			if (type == message_type::source_closure_manifest)
				return source_closure_manifest_view{
					source_closure_manifest_chunk_view{parsed.kind,
													   text(session_text),
													   text(task_text),
													   text(manifest_digest_text),
													   number(chunk_index_number),
													   number(offset_number),
													   number(byte_count_number)}};
			if (type == message_type::source_closure_blob)
				return source_closure_blob_descriptor_view{text(session_text),
														   text(task_text),
														   text(closure_digest_text),
														   number(blob_ordinal_number),
														   text(blob_digest_text),
														   number(total_number),
														   number(chunk_bytes_number),
														   number(chunk_count_number)};
			if (type == message_type::source_closure_chunk)
				return source_closure_chunk_view{text(session_text),
												 text(task_text),
												 number(blob_ordinal_number),
												 text(blob_digest_text),
												 number(chunk_index_number),
												 number(offset_number),
												 number(byte_count_number)};
			if (type == message_type::source_closure_seal)
				return source_closure_seal_view{text(session_text),
												text(task_text),
												text(task_v4_text),
												text(manifest_digest_text),
												text(blob_receipts_text),
												number(blob_count_number),
												number(total_number),
												text(closure_digest_text),
												text(transfer_digest_text)};
			if (type == message_type::source_closure_ack)
				return source_closure_ack_view{text(session_text),
											   text(task_text),
											   text(closure_digest_text),
											   text(transfer_digest_text),
											   text(spool_receipt_text),
											   text(cleanup_owner_text)};
			return source_closure_reject_view{
				text(session_text),
				text(task_text),
				text(failure_phase_text),
				text(reason_code_text),
				control.subspan(parsed.observed_counters.offset, parsed.observed_counters.size),
				text(cleanup_receipt_text)};
		}

		[[nodiscard]] sdk::result<void>
		validate_reject_view(const source_closure_reject_view& value)
		{
			for (const auto& item :
				 {std::pair{std::string_view{"session_id"}, value.session_id},
				  std::pair{std::string_view{"task_id"}, value.task_id},
				  std::pair{std::string_view{"failure_phase"}, value.failure_phase},
				  std::pair{std::string_view{"reason_code"}, value.reason_code},
				  std::pair{std::string_view{"cleanup_receipt"}, value.cleanup_receipt}})
				if (auto valid = valid_text(item.second, item.first); !valid)
					return valid;
			const auto valid_phase = [](const std::string_view phase) noexcept
			{
				return phase == "before-manifest" || phase == "manifest-streaming" ||
					phase == "manifest-validated" || phase == "blob-streaming" ||
					phase == "closure-sealed" || phase == "acknowledged" || phase == "local-only";
			};
			const auto valid_reason = [](const std::string_view reason) noexcept
			{
				constexpr std::array<std::string_view, 17U> reasons{
					"source-closure.required-feature-missing",
					"source-closure.protocol-state-invalid",
					"source-closure.manifest-invalid",
					"source-closure.blob-order-invalid",
					"source-closure.chunk-order-invalid",
					"source-closure.chunk-overlap",
					"source-closure.chunk-gap",
					"source-closure.digest-mismatch",
					"source-closure.limit-exceeded",
					"source-closure.task-binding-mismatch",
					"source-closure.session-binding-mismatch",
					"source-closure.replay-invalid",
					"source-closure.cancelled",
					"source-closure.transfer-timeout",
					"source-closure.spool-io",
					"source-closure.cleanup-failed",
					"source-closure.ambient-fallback-denied"};
				return std::ranges::find(reasons, reason) != reasons.end();
			};
			if (!valid_phase(value.failure_phase) || !valid_reason(value.reason_code))
				return sdk::unexpected(failure(
					"failure", "unknown-phase-or-reason", "source-closure.protocol-state-invalid"));
			return {};
		}

		[[nodiscard]] sdk::result<closure_parsed_control>
		parse_closure_control(const message_type type,
							  const std::span<const byte> control,
							  const closure_limits bound)
		{
			if (!is_closure_message(type))
				return sdk::unexpected(failure("message_type", "not-closure"));
			cbor::scan_limits scan_bound;
			scan_bound.max_bytes = max_control_bytes;
			scan_bound.max_depth = 2U;
			scan_bound.max_items = 96U;
			scan_bound.max_array_items = 0U;
			scan_bound.max_map_items = 32U;
			scan_bound.max_text_bytes = 4'096U;
			scan_bound.max_byte_string_bytes = 0U;
			scan_bound.require_root_map = true;
			if (const auto scanned = cbor::scan_canonical(control, scan_bound); !scanned)
				return sdk::unexpected(
					failure("control", "canonical-shape", "source-closure.manifest-invalid"));

			const auto root = read_wire_head(control, 0U);
			if (!root.valid || root.major != 5U || root.argument > 10U)
				return sdk::unexpected(
					failure("control", "map-shape", "source-closure.manifest-invalid"));
			closure_parsed_control output;
			std::uint32_t seen{};
			auto cursor = root.next;
			for (std::uint64_t index{}; index < root.argument; ++index)
			{
				closure_text_range key_range;
				if (!parse_text_range(control, cursor, key_range, cursor))
					return sdk::unexpected(
						failure("control", "map-key", "source-closure.manifest-invalid"));
				const auto field = identify_field(text_view(control, key_range));
				if (field == wire_field::unknown || (seen & field_bit(field)) != 0U)
					return sdk::unexpected(failure(
						"control", "unknown-or-duplicate", "source-closure.manifest-invalid"));
				seen |= field_bit(field);
				if (!parse_field_value(field, control, cursor, output, cursor))
					return sdk::unexpected(
						failure("control", "field-type", "source-closure.manifest-invalid"));
			}
			if (cursor != control.size() || seen != expected_mask(type, output.kind))
				return sdk::unexpected(
					failure("control", "closed-map-shape", "source-closure.manifest-invalid"));

			const auto view = make_control_view(type, control, output);
			if (type == message_type::source_closure_manifest)
			{
				const auto& manifest = std::get<source_closure_manifest_view>(view);
				if (const auto* descriptor =
						std::get_if<source_closure_manifest_descriptor_view>(&manifest);
					descriptor != nullptr)
				{
					if (auto valid = validate_manifest_descriptor(*descriptor, bound); !valid)
						return sdk::unexpected(valid.error());
				}
				else if (auto valid = validate_manifest_chunk(
							 std::get<source_closure_manifest_chunk_view>(manifest), bound);
						 !valid)
					return sdk::unexpected(valid.error());
			}
			else if (type == message_type::source_closure_blob)
			{
				if (auto valid = validate_blob_descriptor(
						std::get<source_closure_blob_descriptor_view>(view), bound);
					!valid)
					return sdk::unexpected(valid.error());
			}
			else if (type == message_type::source_closure_chunk)
			{
				if (auto valid =
						validate_blob_chunk(std::get<source_closure_chunk_view>(view), bound);
					!valid)
					return sdk::unexpected(valid.error());
			}
			else if (type == message_type::source_closure_seal)
			{
				if (auto valid = validate_seal(std::get<source_closure_seal_view>(view), bound);
					!valid)
					return sdk::unexpected(valid.error());
			}
			else if (type == message_type::source_closure_ack)
			{
				if (auto valid = validate_ack(std::get<source_closure_ack_view>(view)); !valid)
					return sdk::unexpected(valid.error());
			}
			else if (auto valid = validate_reject_view(std::get<source_closure_reject_view>(view));
					 !valid)
				return sdk::unexpected(valid.error());
			return output;
		}

		[[nodiscard]] source_closure_reject own_reject(const source_closure_reject_view& value)
		{
			source_closure_reject output;
			output.session_id = value.session_id;
			output.task_id = value.task_id;
			output.failure_phase = value.failure_phase;
			output.reason_code = value.reason_code;
			output.cleanup_receipt = value.cleanup_receipt;
			const auto root = read_wire_head(value.observed_counters, 0U);
			output.observed_counters.reserve(static_cast<std::size_t>(root.argument));
			auto cursor = root.next;
			for (std::uint64_t index{}; index < root.argument; ++index)
			{
				closure_text_range key_range;
				const auto parsed_key =
					parse_text_range(value.observed_counters, cursor, key_range, cursor);
				std::uint64_t counter{};
				const auto parsed_counter =
					parse_uint(value.observed_counters, cursor, counter, cursor);
				if (!parsed_key || !parsed_counter)
					break;
				output.observed_counters.emplace_back(
					std::string{text_view(value.observed_counters, key_range)},
					cbor::value{counter});
			}
			return output;
		}

		[[nodiscard]] closure_control own_control_view(const closure_control_view& value)
		{
			if (const auto* manifest = std::get_if<source_closure_manifest_view>(&value);
				manifest != nullptr)
			{
				if (const auto* descriptor =
						std::get_if<source_closure_manifest_descriptor_view>(manifest);
					descriptor != nullptr)
					return source_closure_manifest{
						source_closure_manifest_descriptor{descriptor->kind,
														   std::string{descriptor->session_id},
														   std::string{descriptor->task_id},
														   std::string{descriptor->task_v4_digest},
														   std::string{descriptor->closure_id},
														   std::string{descriptor->closure_digest},
														   std::string{descriptor->manifest_digest},
														   descriptor->total_bytes,
														   descriptor->chunk_bytes,
														   descriptor->chunk_count}};
				const auto& chunk = std::get<source_closure_manifest_chunk_view>(*manifest);
				return source_closure_manifest{
					source_closure_manifest_chunk{chunk.kind,
												  std::string{chunk.session_id},
												  std::string{chunk.task_id},
												  std::string{chunk.manifest_digest},
												  chunk.chunk_index,
												  chunk.offset,
												  chunk.byte_count}};
			}
			if (const auto* item = std::get_if<source_closure_blob_descriptor_view>(&value);
				item != nullptr)
				return source_closure_blob_descriptor{std::string{item->session_id},
													  std::string{item->task_id},
													  std::string{item->closure_digest},
													  item->blob_ordinal,
													  std::string{item->blob_digest},
													  item->total_bytes,
													  item->chunk_bytes,
													  item->chunk_count};
			if (const auto* item = std::get_if<source_closure_chunk_view>(&value); item != nullptr)
				return source_closure_chunk{std::string{item->session_id},
											std::string{item->task_id},
											item->blob_ordinal,
											std::string{item->blob_digest},
											item->chunk_index,
											item->offset,
											item->byte_count};
			if (const auto* item = std::get_if<source_closure_seal_view>(&value); item != nullptr)
				return source_closure_seal{std::string{item->session_id},
										   std::string{item->task_id},
										   std::string{item->task_v4_digest},
										   std::string{item->manifest_digest},
										   std::string{item->blob_receipts_digest},
										   item->blob_count,
										   item->total_bytes,
										   std::string{item->closure_digest},
										   std::string{item->transfer_digest}};
			if (const auto* item = std::get_if<source_closure_ack_view>(&value); item != nullptr)
				return source_closure_ack{std::string{item->session_id},
										  std::string{item->task_id},
										  std::string{item->closure_digest},
										  std::string{item->transfer_digest},
										  std::string{item->spool_receipt},
										  std::string{item->cleanup_owner}};
			return own_reject(std::get<source_closure_reject_view>(value));
		}

		// NOLINTBEGIN(bugprone-exception-escape): admitted variant is never valueless.
		[[nodiscard]] std::size_t owned_control_resident(const closure_control& value) noexcept
		{
			// NOLINTEND(bugprone-exception-escape)
			std::size_t output = sizeof(closure_control);
			const auto add_string = [&output](const std::string& item) noexcept
			{
				if (item.capacity() > std::numeric_limits<std::size_t>::max() - output)
				{
					output = std::numeric_limits<std::size_t>::max();
					return;
				}
				output += item.capacity();
			};
			const auto add_common = [&add_string](const auto& item) noexcept
			{
				add_string(item.session_id);
				add_string(item.task_id);
			};
			std::visit(
				[&](const auto& item)
				{
					using item_type = std::remove_cvref_t<decltype(item)>;
					if constexpr (std::is_same_v<item_type, source_closure_manifest>)
						std::visit(
							[&](const auto& manifest)
							{
								add_common(manifest);
								add_string(manifest.manifest_digest);
								if constexpr (std::is_same_v<
												  std::remove_cvref_t<decltype(manifest)>,
												  source_closure_manifest_descriptor>)
								{
									add_string(manifest.task_v4_digest);
									add_string(manifest.closure_id);
									add_string(manifest.closure_digest);
								}
							},
							item);
					else
					{
						add_common(item);
						if constexpr (std::is_same_v<item_type, source_closure_blob_descriptor>)
						{
							add_string(item.closure_digest);
							add_string(item.blob_digest);
						}
						else if constexpr (std::is_same_v<item_type, source_closure_chunk>)
							add_string(item.blob_digest);
						else if constexpr (std::is_same_v<item_type, source_closure_seal>)
						{
							add_string(item.task_v4_digest);
							add_string(item.manifest_digest);
							add_string(item.blob_receipts_digest);
							add_string(item.closure_digest);
							add_string(item.transfer_digest);
						}
						else if constexpr (std::is_same_v<item_type, source_closure_ack>)
						{
							add_string(item.closure_digest);
							add_string(item.transfer_digest);
							add_string(item.spool_receipt);
							add_string(item.cleanup_owner);
						}
						else
						{
							add_string(item.failure_phase);
							add_string(item.reason_code);
							add_string(item.cleanup_receipt);
							const auto capacity_bytes =
								item.observed_counters.capacity() * sizeof(cbor::map::value_type);
							if (capacity_bytes > std::numeric_limits<std::size_t>::max() - output)
								output = std::numeric_limits<std::size_t>::max();
							else
								output += capacity_bytes;
							for (const auto& counter : item.observed_counters)
								add_string(counter.first);
						}
					}
				},
				value);
			return output;
		}
	} // namespace

	closure_control_token::closure_control_token(closure_control_token&& other) noexcept
		: type_{other.type_}, control_{std::move(other.control_)}, parsed_{other.parsed_},
		  resident_bytes_{other.resident_bytes_}, control_digest_{other.control_digest_},
		  consumed_{other.consumed_}
	{
		other.control_.clear();
		other.parsed_ = {};
		other.control_digest_ = {};
		other.consumed_ = true;
		other.resident_bytes_ = 0U;
	}

	closure_control_token& closure_control_token::operator=(closure_control_token&& other) noexcept
	{
		if (this == &other)
			return *this;
		type_ = other.type_;
		control_ = std::move(other.control_);
		parsed_ = other.parsed_;
		resident_bytes_ = other.resident_bytes_;
		control_digest_ = other.control_digest_;
		consumed_ = other.consumed_;
		other.control_.clear();
		other.parsed_ = {};
		other.control_digest_ = {};
		other.consumed_ = true;
		other.resident_bytes_ = 0U;
		return *this;
	}

	std::optional<closure_control_view> closure_control_token::value() const noexcept
	{
		if (consumed_ || control_.empty())
			return std::nullopt;
		return make_control_view(type_, control_, parsed_);
	}

	sdk::result<closure_control_view> closure_control_token::consume() &&
	{
		if (consumed_)
			return sdk::unexpected(
				failure("control", "already-consumed", "source-closure.replay-invalid"));
		auto output = value();
		if (!output)
			return sdk::unexpected(
				failure("control", "moved-from", "source-closure.replay-invalid"));
		consumed_ = true;
		return *output;
	}

	sdk::result<closure_control_token>
	decode_closure_control_token(const message_type type,
								 bytes&& control,
								 const bytes& payload,
								 const closure_limits bound,
								 const std::size_t caller_fixed_resident_bytes)
	{
		try
		{
			if (control.empty() || control.size() > max_control_bytes)
				return sdk::unexpected(
					failure("control", "empty-or-limit-exceeded", "source-closure.limit-exceeded"));
			const auto payload_limit =
				std::min(bound.maximum_chunk_payload_bytes, max_closure_chunk_payload_bytes);
			if (payload.size() > payload_limit)
				return sdk::unexpected(
					failure("payload", "limit-exceeded", "source-closure.limit-exceeded"));
			std::size_t token_resident{};
			const auto add = [](std::size_t& total, const std::size_t value) noexcept
			{
				if (value > std::numeric_limits<std::size_t>::max() - total)
					return false;
				total += value;
				return true;
			};
			if (!add(token_resident, control.capacity()) ||
				!add(token_resident, closure_control_decode_fixed_workspace_bytes))
				return sdk::unexpected(
					failure("resident", "overflow", "source-closure.limit-exceeded"));
			std::size_t peak_resident{};
			if (!add(peak_resident, fixed_header_bytes) ||
				!add(peak_resident, caller_fixed_resident_bytes) ||
				!add(peak_resident, payload.capacity()) || !add(peak_resident, token_resident))
				return sdk::unexpected(
					failure("resident", "overflow", "source-closure.limit-exceeded"));
			const auto resident_limit = std::min(bound.maximum_resident_transport_bytes,
												 max_closure_resident_transport_bytes);
			if (peak_resident > resident_limit)
				return sdk::unexpected(
					failure("resident", "limit-exceeded", "source-closure.limit-exceeded"));
			auto parsed = parse_closure_control(type, control, bound);
			if (!parsed)
				return sdk::unexpected(parsed.error());
			const auto digest = sha256(control);
			return closure_control_token{type, std::move(control), *parsed, token_resident, digest};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(
				failure("control", "allocation", "source-closure.limit-exceeded"));
		}
	}

	sdk::result<bytes> encode_closure_control(const message_type type,
											  const closure_control& control,
											  const closure_limits bound)
	{
		auto fields = map_for(type, control, bound);
		if (!fields)
			return sdk::unexpected(fields.error());
		return encode_map(std::move(*fields));
	}

	sdk::result<closure_control> decode_closure_control(const message_type type,
														const std::span<const byte> control,
														const closure_limits bound)
	{
		try
		{
			if (!is_closure_message(type))
				return sdk::unexpected(failure("message_type", "not-closure"));
			if (control.empty() || control.size() > max_control_bytes)
				return sdk::unexpected(
					failure("control", "empty-or-limit-exceeded", "source-closure.limit-exceeded"));

			// The owning API is compatibility-only. Reserve its complete worst-case
			// materialization before allocating the adopted vector or any strings. The
			// production receiver keeps the token/view and does not pay this reserve.
			constexpr std::size_t compatibility_dynamic_reserve =
				std::size_t{4U} * max_control_bytes +
				std::size_t{2U} * 32U * sizeof(cbor::map::value_type);
			std::size_t projected_resident = fixed_header_bytes + sizeof(frame);
			const auto add = [](std::size_t& total, const std::size_t increment) noexcept
			{
				if (increment > std::numeric_limits<std::size_t>::max() - total)
					return false;
				total += increment;
				return true;
			};
			if (!add(projected_resident, control.size()) ||
				!add(projected_resident, closure_control_decode_fixed_workspace_bytes) ||
				!add(projected_resident, compatibility_dynamic_reserve) ||
				!add(projected_resident, sizeof(closure_control)) ||
				!add(projected_resident, sizeof(sdk::result<closure_control>)))
				return sdk::unexpected(
					failure("resident", "overflow", "source-closure.limit-exceeded"));
			const auto resident_limit = std::min(bound.maximum_resident_transport_bytes,
												 max_closure_resident_transport_bytes);
			if (projected_resident > resident_limit)
				return sdk::unexpected(
					failure("resident", "limit-exceeded", "source-closure.limit-exceeded"));

			bytes adopted(control.begin(), control.end());
			const bytes payload;
			auto token = decode_closure_control_token(
				type, std::move(adopted), payload, bound, sizeof(frame) + control.size());
			if (!token)
				return sdk::unexpected(token.error());
			auto view = std::move(*token).consume();
			if (!view)
				return sdk::unexpected(view.error());
			auto output = own_control_view(*view);
			std::size_t resident = fixed_header_bytes + sizeof(frame) + control.size();
			if (!add(resident, token->resident_bytes()) ||
				!add(resident, owned_control_resident(output)) ||
				!add(resident, sizeof(sdk::result<closure_control>)))
				return sdk::unexpected(
					failure("resident", "overflow", "source-closure.limit-exceeded"));
			if (resident > resident_limit)
				return sdk::unexpected(
					failure("resident", "limit-exceeded", "source-closure.limit-exceeded"));
			return output;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(
				failure("control", "allocation", "source-closure.limit-exceeded"));
		}
	}

	sdk::result<void> validate_closure_payload(const message_type type,
											   const closure_control& control,
											   const std::span<const byte> payload,
											   const closure_limits bound)
	{
		if (!is_closure_message(type))
			return sdk::unexpected(failure("message_type", "not-closure"));
		if (type == message_type::source_closure_manifest)
		{
			const auto* item = std::get_if<source_closure_manifest>(&control);
			if (item == nullptr)
				return sdk::unexpected(failure("control", "type-mismatch"));
			if (const auto* chunk = std::get_if<source_closure_manifest_chunk>(item);
				chunk != nullptr)
			{
				if (payload.size() != chunk->byte_count ||
					payload.size() > bound.maximum_chunk_payload_bytes)
					return sdk::unexpected(failure(
						"payload", "byte-count-mismatch", "source-closure.chunk-order-invalid"));
				return {};
			}
		}
		else if (type == message_type::source_closure_chunk)
		{
			const auto* item = std::get_if<source_closure_chunk>(&control);
			if (item == nullptr)
				return sdk::unexpected(failure("control", "type-mismatch"));
			if (payload.size() != item->byte_count ||
				payload.size() > bound.maximum_chunk_payload_bytes)
				return sdk::unexpected(failure(
					"payload", "byte-count-mismatch", "source-closure.chunk-order-invalid"));
			return {};
		}
		else if (!payload.empty())
			return sdk::unexpected(
				failure("payload", "must-be-empty", "source-closure.protocol-state-invalid"));
		return {};
	}

	sdk::result<void> validate_closure_payload(const message_type type,
											   const closure_control_view& control,
											   const std::span<const byte> payload,
											   const closure_limits bound)
	{
		if (!is_closure_message(type))
			return sdk::unexpected(failure("message_type", "not-closure"));
		if (type == message_type::source_closure_manifest)
		{
			const auto* item = std::get_if<source_closure_manifest_view>(&control);
			if (item == nullptr)
				return sdk::unexpected(failure("control", "type-mismatch"));
			if (const auto* chunk = std::get_if<source_closure_manifest_chunk_view>(item);
				chunk != nullptr)
			{
				if (payload.size() != chunk->byte_count ||
					payload.size() > bound.maximum_chunk_payload_bytes)
					return sdk::unexpected(failure(
						"payload", "byte-count-mismatch", "source-closure.chunk-order-invalid"));
				return {};
			}
		}
		else if (type == message_type::source_closure_chunk)
		{
			const auto* item = std::get_if<source_closure_chunk_view>(&control);
			if (item == nullptr)
				return sdk::unexpected(failure("control", "type-mismatch"));
			if (payload.size() != item->byte_count ||
				payload.size() > bound.maximum_chunk_payload_bytes)
				return sdk::unexpected(failure(
					"payload", "byte-count-mismatch", "source-closure.chunk-order-invalid"));
			return {};
		}
		else if (!payload.empty())
			return sdk::unexpected(
				failure("payload", "must-be-empty", "source-closure.protocol-state-invalid"));
		return {};
	}

	bool closure_transfer::fixed_text::assign(const std::string_view value) noexcept
	{
		if (value.size() > storage.size())
			return false;
		std::copy(value.begin(), value.end(), storage.begin());
		size = static_cast<std::uint8_t>(value.size());
		return true;
	}

	closure_transfer::closure_transfer(closure_transfer&& other) noexcept
		: session_{std::move(other.session_)}, state_{other.state_}, generation_{other.generation_},
		  active_{other.active_}
	{
		other.active_ = false;
		++other.generation_;
	}

	closure_transfer& closure_transfer::operator=(closure_transfer&& other) noexcept
	{
		if (this == &other)
			return *this;
		const auto invalidated_generation = generation_ + 1U;
		session_ = std::move(other.session_);
		state_ = other.state_;
		generation_ = invalidated_generation;
		active_ = other.active_;
		other.active_ = false;
		++other.generation_;
		return *this;
	}

	closure_transfer::prepared_ack_transition::prepared_ack_transition(
		prepared_ack_transition&& other) noexcept
		: owner_{other.owner_}, generation_{other.generation_}, next_{other.next_},
		  wire_{std::move(other.wire_)}, consumed_{other.consumed_}
	{
		other.abort();
	}

	closure_transfer::prepared_ack_transition&
	closure_transfer::prepared_ack_transition::operator=(prepared_ack_transition&& other) noexcept
	{
		if (this == &other)
			return *this;
		abort();
		owner_ = other.owner_;
		generation_ = other.generation_;
		next_ = other.next_;
		wire_ = std::move(other.wire_);
		consumed_ = other.consumed_;
		other.abort();
		return *this;
	}

	void closure_transfer::prepared_ack_transition::abort() noexcept
	{
		owner_ = nullptr;
		generation_ = 0U;
		consumed_ = true;
		wire_.clear();
	}

	sdk::result<closure_transfer> closure_transfer::create(closure_session session)
	{
		if (session.session_id.empty() || session.task_id.empty() || session.stream_id == 0U)
			return sdk::unexpected(
				failure("session", "binding-invalid", "source-closure.session-binding-mismatch"));
		for (const auto& item :
			 {std::pair{std::string_view{"session_id"}, std::string_view{session.session_id}},
			  std::pair{std::string_view{"task_id"}, std::string_view{session.task_id}}})
			if (auto valid = valid_text(item.second, item.first); !valid)
				return sdk::unexpected(valid.error());
		for (const auto& item : {std::pair{std::string_view{"task_v4_digest"},
										   std::string_view{session.task_v4_digest}},
								 std::pair{std::string_view{"closure_digest"},
										   std::string_view{session.closure_digest}},
								 std::pair{std::string_view{"manifest_digest"},
										   std::string_view{session.manifest_digest}}})
			if (!item.second.empty())
				if (auto valid = valid_semantic(item.second, item.first); !valid)
					return sdk::unexpected(valid.error());
		if (session.initial_credit.bytes == 0U || session.initial_credit.frames == 0U)
			return sdk::unexpected(failure("credit", "zero", "source-closure.limit-exceeded"));
		mutable_state initial;
		initial.sequence = sequence_guard{session.stream_id, session.first_sequence};
		initial.credit = credit_window{session.initial_credit};
		return closure_transfer{std::move(session), initial};
	}

	// NOLINTBEGIN(bugprone-easily-swappable-parameters): protocol binding field order.
	sdk::result<void> closure_transfer::bind_common(const std::string_view session_id,
													const std::string_view task_id,
													const std::string_view closure_digest) const
	{
		// NOLINTEND(bugprone-easily-swappable-parameters)
		if (session_id != session_.session_id)
			return sdk::unexpected(
				failure("session_id", "mismatch", "source-closure.session-binding-mismatch"));
		if (task_id != session_.task_id)
			return sdk::unexpected(
				failure("task_id", "mismatch", "source-closure.task-binding-mismatch"));
		if (!closure_digest.empty() && !session_.closure_digest.empty() &&
			closure_digest != session_.closure_digest)
			return sdk::unexpected(
				failure("closure_digest", "mismatch", "source-closure.task-binding-mismatch"));
		return {};
	}

	sdk::result<closure_transfer::mutable_state>
	closure_transfer::prepare_transition(const frame& value,
										 const closure_control_view& decoded,
										 const std::size_t control_bytes,
										 const digest32& control_digest,
										 const bool consume_credit) const
	{
		if (!active_)
			return sdk::unexpected(
				failure("transfer", "moved-from", "source-closure.replay-invalid"));
		if (value.protocol_major != protocol_major || value.protocol_minor != 0U)
			return sdk::unexpected(
				failure("protocol", "downgrade-or-unnegotiated", "source-closure.replay-invalid"));
		if (value.flags != 0U)
			return sdk::unexpected(failure(
				"flags", "closure-flags-must-be-zero", "source-closure.protocol-state-invalid"));
		if (!is_closure_message(value.type))
			return sdk::unexpected(failure("message_type", "not-closure"));
		if (control_bytes > std::numeric_limits<std::size_t>::max() - value.payload.size() ||
			control_bytes + value.payload.size() > session_.limits.maximum_resident_transport_bytes)
			return sdk::unexpected(failure(
				"resident_transport_bytes", "limit-exceeded", "source-closure.limit-exceeded"));
		const auto observed_payload_digest = sha256(value.payload);
		if ((!digest_is_zero(value.control_digest) &&
			 !digest_equal(value.control_digest, control_digest)) ||
			(!digest_is_zero(value.payload_digest) &&
			 !digest_equal(value.payload_digest, observed_payload_digest)))
			return sdk::unexpected(failure("digest", "mismatch", "source-closure.digest-mismatch"));
		if (auto valid =
				validate_closure_payload(value.type, decoded, value.payload, session_.limits);
			!valid)
			return sdk::unexpected(valid.error());

		auto next = state_;
		if (auto valid = next.sequence.accept(value); !valid)
			return sdk::unexpected(valid.error());
		if (consume_credit)
			if (auto valid = next.credit.consume_encoded(control_bytes, value.payload.size());
				!valid)
				return sdk::unexpected(valid.error());

		if (value.type == message_type::source_closure_manifest)
		{
			const auto& manifest = std::get<source_closure_manifest_view>(decoded);
			if (const auto* descriptor =
					std::get_if<source_closure_manifest_descriptor_view>(&manifest);
				descriptor != nullptr)
			{
				if (next.phase != closure_phase::task_v4_sealed)
					return sdk::unexpected(failure(
						"phase", "manifest-reopen", "source-closure.protocol-state-invalid"));
				if (auto valid = bind_common(
						descriptor->session_id, descriptor->task_id, descriptor->closure_digest);
					!valid)
					return sdk::unexpected(valid.error());
				if (!session_.task_v4_digest.empty() &&
					descriptor->task_v4_digest != session_.task_v4_digest)
					return sdk::unexpected(failure(
						"task_v4_digest", "mismatch", "source-closure.task-binding-mismatch"));
				if (!session_.manifest_digest.empty() &&
					descriptor->manifest_digest != session_.manifest_digest)
					return sdk::unexpected(
						failure("manifest_digest", "mismatch", "source-closure.digest-mismatch"));
				next.manifest_total = descriptor->total_bytes;
				next.manifest_chunk_bytes = descriptor->chunk_bytes;
				next.manifest_chunk_count = descriptor->chunk_count;
				next.manifest_next_chunk = 0U;
				next.manifest_observed = 0U;
				next.phase = next.manifest_chunk_count == 0U ? closure_phase::manifest_validated
															 : closure_phase::manifest_streaming;
				return next;
			}
			const auto* chunk = std::get_if<source_closure_manifest_chunk_view>(&manifest);
			if (chunk == nullptr ||
				(next.phase != closure_phase::manifest_streaming &&
				 next.phase != closure_phase::manifest_open))
				return sdk::unexpected(failure(
					"phase", "manifest-chunk-order", "source-closure.protocol-state-invalid"));
			if (auto valid = bind_common(chunk->session_id, chunk->task_id); !valid)
				return sdk::unexpected(valid.error());
			if (chunk->manifest_digest != session_.manifest_digest &&
				!session_.manifest_digest.empty())
				return sdk::unexpected(
					failure("manifest_digest", "mismatch", "source-closure.digest-mismatch"));
			if (next.manifest_observed > next.manifest_total ||
				chunk->chunk_index != next.manifest_next_chunk ||
				chunk->offset != next.manifest_observed ||
				chunk->byte_count > next.manifest_total - next.manifest_observed ||
				(chunk->chunk_index + 1U < next.manifest_chunk_count &&
				 chunk->byte_count != next.manifest_chunk_bytes) ||
				(chunk->chunk_index + 1U == next.manifest_chunk_count &&
				 chunk->byte_count != next.manifest_total - next.manifest_observed))
				return sdk::unexpected(failure(
					"chunk", "gap-overlap-or-reorder", "source-closure.chunk-order-invalid"));
			next.manifest_observed += chunk->byte_count;
			++next.manifest_next_chunk;
			if (next.manifest_next_chunk == next.manifest_chunk_count)
				next.phase = closure_phase::manifest_validated;
			return next;
		}

		if (value.type == message_type::source_closure_blob)
		{
			if (next.phase != closure_phase::manifest_validated)
				return sdk::unexpected(failure(
					"phase", "blob-before-manifest", "source-closure.protocol-state-invalid"));
			const auto& item = std::get<source_closure_blob_descriptor_view>(decoded);
			if (auto valid = bind_common(item.session_id, item.task_id, item.closure_digest);
				!valid)
				return sdk::unexpected(valid.error());
			if (item.blob_ordinal != next.next_blob_ordinal)
				return sdk::unexpected(
					failure("blob_ordinal", "gap-or-replay", "source-closure.blob-order-invalid"));
			if (!next.current_blob_digest.assign(item.blob_digest))
				return sdk::unexpected(
					failure("blob_digest", "fixed-bound", "source-closure.limit-exceeded"));
			next.current_blob_ordinal = item.blob_ordinal;
			next.current_blob_total = item.total_bytes;
			next.current_blob_chunk_bytes = item.chunk_bytes;
			next.current_blob_chunk_count = item.chunk_count;
			next.current_blob_next_chunk = 0U;
			next.current_blob_observed = 0U;
			next.phase = next.current_blob_chunk_count == 0U ? closure_phase::manifest_validated
															 : closure_phase::blob_streaming;
			if (next.current_blob_chunk_count == 0U)
			{
				if (item.total_bytes > session_.limits.maximum_unique_blob_bytes ||
					next.blob_observed_total >
						session_.limits.maximum_unique_blob_bytes - item.total_bytes)
					return sdk::unexpected(
						failure("blob_bytes", "limit-exceeded", "source-closure.limit-exceeded"));
				++next.next_blob_ordinal;
				next.blob_observed_total += item.total_bytes;
			}
			return next;
		}

		if (value.type == message_type::source_closure_chunk)
		{
			if (next.phase != closure_phase::blob_streaming)
				return sdk::unexpected(
					failure("phase", "blob-chunk-order", "source-closure.protocol-state-invalid"));
			const auto& item = std::get<source_closure_chunk_view>(decoded);
			if (auto valid = bind_common(item.session_id, item.task_id); !valid)
				return sdk::unexpected(valid.error());
			if (next.current_blob_observed > next.current_blob_total ||
				item.blob_ordinal != next.current_blob_ordinal ||
				item.blob_digest != next.current_blob_digest.view() ||
				item.chunk_index != next.current_blob_next_chunk ||
				item.offset != next.current_blob_observed ||
				item.byte_count > next.current_blob_total - next.current_blob_observed ||
				(item.chunk_index + 1U < next.current_blob_chunk_count &&
				 item.byte_count != next.current_blob_chunk_bytes) ||
				(item.chunk_index + 1U == next.current_blob_chunk_count &&
				 item.byte_count != next.current_blob_total - next.current_blob_observed))
				return sdk::unexpected(failure(
					"chunk", "gap-overlap-or-reorder", "source-closure.chunk-order-invalid"));
			if (item.byte_count > session_.limits.maximum_unique_blob_bytes ||
				item.byte_count > session_.limits.maximum_task_spool_bytes ||
				next.blob_observed_total >
					session_.limits.maximum_unique_blob_bytes - item.byte_count ||
				next.blob_observed_total >
					session_.limits.maximum_task_spool_bytes - item.byte_count)
				return sdk::unexpected(
					failure("blob_bytes", "limit-exceeded", "source-closure.limit-exceeded"));
			next.current_blob_observed += item.byte_count;
			++next.current_blob_next_chunk;
			next.blob_observed_total += item.byte_count;
			if (next.current_blob_next_chunk == next.current_blob_chunk_count)
			{
				if (next.next_blob_ordinal >= session_.limits.maximum_blobs)
					return sdk::unexpected(
						failure("blob_count", "limit-exceeded", "source-closure.limit-exceeded"));
				++next.next_blob_ordinal;
				next.phase = closure_phase::manifest_validated;
			}
			return next;
		}

		if (value.type == message_type::source_closure_seal)
		{
			if (next.phase != closure_phase::manifest_validated ||
				next.current_blob_next_chunk != next.current_blob_chunk_count)
				return sdk::unexpected(
					failure("phase", "seal-order", "source-closure.protocol-state-invalid"));
			const auto& item = std::get<source_closure_seal_view>(decoded);
			if (auto valid = bind_common(item.session_id, item.task_id, item.closure_digest);
				!valid)
				return sdk::unexpected(valid.error());
			if ((!session_.task_v4_digest.empty() &&
				 item.task_v4_digest != session_.task_v4_digest) ||
				(!session_.manifest_digest.empty() &&
				 item.manifest_digest != session_.manifest_digest) ||
				item.blob_count != next.next_blob_ordinal ||
				item.total_bytes != next.blob_observed_total)
				return sdk::unexpected(failure(
					"seal", "binding-or-counter-mismatch", "source-closure.digest-mismatch"));
			if (!next.sealed_transfer_digest.assign(item.transfer_digest))
				return sdk::unexpected(
					failure("transfer_digest", "fixed-bound", "source-closure.limit-exceeded"));
			next.phase = closure_phase::closure_sealed;
			return next;
		}

		if (value.type == message_type::source_closure_ack)
		{
			if (next.phase != closure_phase::closure_sealed)
				return sdk::unexpected(
					failure("phase", "ack-order", "source-closure.protocol-state-invalid"));
			const auto& item = std::get<source_closure_ack_view>(decoded);
			if (auto valid = bind_common(item.session_id, item.task_id, item.closure_digest);
				!valid)
				return sdk::unexpected(valid.error());
			if (item.transfer_digest != next.sealed_transfer_digest.view())
				return sdk::unexpected(
					failure("transfer_digest", "mismatch", "source-closure.replay-invalid"));
			next.phase = closure_phase::acknowledged;
			return next;
		}

		const auto& item = std::get<source_closure_reject_view>(decoded);
		if (next.phase == closure_phase::acknowledged || next.phase == closure_phase::rejected)
			return sdk::unexpected(
				failure("phase", "terminal-replay", "source-closure.replay-invalid"));
		if (auto valid = bind_common(item.session_id, item.task_id); !valid)
			return sdk::unexpected(valid.error());
		next.phase = closure_phase::rejected;
		return next;
	}

	sdk::result<void> closure_transfer::accept(const frame& value)
	{
		try
		{
			bytes adopted = value.control;
			auto token = decode_closure_control_token(value.type,
													  std::move(adopted),
													  value.payload,
													  session_.limits,
													  sizeof(frame) + value.control.capacity());
			if (!token)
				return sdk::unexpected(token.error());
			const auto control_bytes = token->control_bytes().size();
			const auto control_digest = token->control_digest();
			auto view = std::move(*token).consume();
			if (!view)
				return sdk::unexpected(view.error());
			auto next = prepare_transition(value, *view, control_bytes, control_digest, true);
			if (!next)
				return sdk::unexpected(next.error());
			state_ = *next;
			++generation_;
			return {};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(
				failure("control", "allocation", "source-closure.limit-exceeded"));
		}
	}

	sdk::result<void> closure_transfer::accept_decoded(const frame& value,
													   closure_control_token&& control)
	{
		if (!value.control.empty())
			return sdk::unexpected(
				failure("control", "must-be-adopted", "source-closure.protocol-state-invalid"));
		if (control.type() != value.type)
			return sdk::unexpected(
				failure("control", "type-mismatch", "source-closure.protocol-state-invalid"));
		const auto control_bytes = control.control_bytes().size();
		const auto control_digest = control.control_digest();
		auto view = std::move(control).consume();
		if (!view)
			return sdk::unexpected(view.error());
		auto next = prepare_transition(value, *view, control_bytes, control_digest, true);
		if (!next)
			return sdk::unexpected(next.error());
		state_ = *next;
		++generation_;
		return {};
	}

	sdk::result<closure_transfer::prepared_ack_transition>
	closure_transfer::prepare_acknowledgement(const source_closure_ack& value,
											  const std::uint64_t sequence) const
	{
		try
		{
			if (!active_ || state_.phase != closure_phase::closure_sealed)
				return sdk::unexpected(
					failure("phase", "ack-order", "source-closure.protocol-state-invalid"));
			constexpr std::size_t fixed_workspace = closure_control_decode_fixed_workspace_bytes +
				sizeof(frame) + sizeof(closure_control_view) + sizeof(digest32) +
				2U * sizeof(sdk::result<bytes>) + sizeof(sdk::result<mutable_state>) +
				sizeof(prepared_ack_transition) + sizeof(sdk::result<prepared_ack_transition>);
			constexpr std::size_t encoding_dynamic_reserve = 4U * max_control_bytes +
				fixed_header_bytes + std::size_t{2U} * 32U * sizeof(cbor::map::value_type);
			const auto add = [](std::size_t& total, const std::size_t increment) noexcept
			{
				if (increment > std::numeric_limits<std::size_t>::max() - total)
					return false;
				total += increment;
				return true;
			};
			std::size_t preflight_resident = fixed_workspace;
			if (!add(preflight_resident, encoding_dynamic_reserve))
				return sdk::unexpected(
					failure("resident", "overflow", "source-closure.limit-exceeded"));
			const auto resident_limit = std::min(session_.limits.maximum_resident_transport_bytes,
												 max_closure_resident_transport_bytes);
			if (preflight_resident > resident_limit)
				return sdk::unexpected(
					failure("resident", "limit-exceeded", "source-closure.limit-exceeded"));

			auto encoded_control = encode_closure_control(
				message_type::source_closure_ack, closure_control{value}, session_.limits);
			if (!encoded_control)
				return sdk::unexpected(encoded_control.error());
			frame acknowledgement;
			acknowledgement.type = message_type::source_closure_ack;
			acknowledgement.stream_id = session_.stream_id;
			acknowledgement.sequence = sequence;
			acknowledgement.control = std::move(*encoded_control);
			const closure_control_view view{source_closure_ack_view{value.session_id,
																	value.task_id,
																	value.closure_digest,
																	value.transfer_digest,
																	value.spool_receipt,
																	value.cleanup_owner}};
			const auto observed_control_digest = sha256(acknowledgement.control);
			auto next = prepare_transition(acknowledgement,
										   view,
										   acknowledgement.control.size(),
										   observed_control_digest,
										   false);
			if (!next)
				return sdk::unexpected(next.error());
			auto wire = encode_frame(acknowledgement);
			if (!wire)
				return sdk::unexpected(wire.error());
			std::size_t resident = fixed_workspace;
			if (!add(resident, acknowledgement.control.capacity()) ||
				!add(resident, wire->capacity()))
				return sdk::unexpected(
					failure("resident", "overflow", "source-closure.limit-exceeded"));
			if (resident > resident_limit)
				return sdk::unexpected(
					failure("resident", "limit-exceeded", "source-closure.limit-exceeded"));
			return prepared_ack_transition{this, generation_, *next, std::move(*wire)};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(failure("ack", "allocation", "source-closure.limit-exceeded"));
		}
	}

	void closure_transfer::commit_acknowledgement(prepared_ack_transition&& transition) noexcept
	{
		if (!active_ || transition.owner_ != this || transition.consumed_ ||
			transition.generation_ != generation_ ||
			state_.phase != closure_phase::closure_sealed ||
			transition.next_.phase != closure_phase::acknowledged)
			return;
		state_ = transition.next_;
		++generation_;
		transition.abort();
	}
} // namespace cxxlens::protocol_v2
