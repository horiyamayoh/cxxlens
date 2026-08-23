#include "closure.hpp"

#include <algorithm>
#include <array>
#include <limits>
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

		[[nodiscard]] sdk::result<cbor::map> decode_map_owned(const std::span<const byte> input)
		{
			auto decoded = cbor::decode(input);
			if (!decoded)
				return sdk::unexpected(
					failure("control", decoded.error().detail, "source-closure.manifest-invalid"));
			if (auto* fields = std::get_if<cbor::map>(&decoded->data); fields != nullptr)
				return std::move(*fields);
			return sdk::unexpected(
				failure("control", "map-required", "source-closure.manifest-invalid"));
		}

		[[nodiscard]] sdk::result<void>
		exact_keys(const cbor::map& fields, const std::initializer_list<std::string_view> keys)
		{
			return cbor::require_keys(fields, keys);
		}

		[[nodiscard]] sdk::result<void> valid_text(const std::string_view value,
												   const std::string_view field,
												   const std::size_t maximum = 4'096U)
		{
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
				if (!((character >= '0' && character <= '9') ||
					  (character >= 'a' && character <= 'f')))
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

		[[nodiscard]] sdk::result<void> valid_semantic(const std::string_view value,
													   const std::string_view field)
		{
			if (!semantic_digest(value))
				return sdk::unexpected(
					failure(field, "semantic-digest", "source-closure.digest-mismatch"));
			return {};
		}

		[[nodiscard]] sdk::result<void> valid_content(const std::string_view value,
													  const std::string_view field)
		{
			if (!content_digest(value))
				return sdk::unexpected(
					failure(field, "content-digest", "source-closure.digest-mismatch"));
			return {};
		}

		[[nodiscard]] sdk::result<void> valid_shape(const std::uint64_t total_bytes,
													const std::uint64_t chunk_bytes,
													const std::uint64_t chunk_count,
													const std::size_t max_chunk_bytes,
													const std::size_t max_chunks,
													const std::string_view field)
		{
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

		[[nodiscard]] sdk::result<void>
		validate_manifest_descriptor(const source_closure_manifest_descriptor& value,
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

		[[nodiscard]] sdk::result<void>
		validate_manifest_chunk(const source_closure_manifest_chunk& value,
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

		[[nodiscard]] sdk::result<void>
		validate_blob_descriptor(const source_closure_blob_descriptor& value,
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

		[[nodiscard]] sdk::result<void> validate_blob_chunk(const source_closure_chunk& value,
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

		[[nodiscard]] sdk::result<void> validate_seal(const source_closure_seal& value,
													  const closure_limits bound)
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

		[[nodiscard]] sdk::result<void> validate_ack(const source_closure_ack& value)
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

		template <typename T>
		[[nodiscard]] sdk::result<T> get(const cbor::map& fields, const std::string_view key)
		{
			return cbor::field<T>(fields, key);
		}

		[[nodiscard]] sdk::result<manifest_kind> parse_manifest_kind(const std::string& value)
		{
			if (value == "descriptor")
				return manifest_kind::descriptor;
			if (value == "chunk")
				return manifest_kind::chunk;
			return sdk::unexpected(failure("kind", "unknown", "source-closure.manifest-invalid"));
		}

		[[nodiscard]] sdk::result<std::string> string_field(const cbor::map& fields,
															const std::string_view key)
		{
			return get<std::string>(fields, key);
		}

		[[nodiscard]] sdk::result<std::uint64_t> uint_field(const cbor::map& fields,
															const std::string_view key)
		{
			return get<std::uint64_t>(fields, key);
		}

		[[nodiscard]] sdk::result<closure_control>
		decode_owned(const message_type type, cbor::map fields, const closure_limits bound)
		{
			const auto require = [&fields](const std::initializer_list<std::string_view> keys)
			{
				return exact_keys(fields, keys);
			};
			if (type == message_type::source_closure_manifest)
			{
				auto kind = string_field(fields, "kind");
				if (!kind)
					return sdk::unexpected(kind.error());
				auto parsed = parse_manifest_kind(*kind);
				if (!parsed)
					return sdk::unexpected(parsed.error());
				if (*parsed == manifest_kind::descriptor)
				{
					if (auto valid = require({"kind",
											  "session_id",
											  "task_id",
											  "task_v4_digest",
											  "closure_id",
											  "closure_digest",
											  "manifest_digest",
											  "total_bytes",
											  "chunk_bytes",
											  "chunk_count"});
						!valid)
						return sdk::unexpected(valid.error());
					source_closure_manifest_descriptor item;
					item.kind = *parsed;
					for (const auto key : {"session_id",
										   "task_id",
										   "task_v4_digest",
										   "closure_id",
										   "closure_digest",
										   "manifest_digest"})
					{
						auto field_value = string_field(fields, key);
						if (!field_value)
							return sdk::unexpected(field_value.error());
						if (std::string_view{key} == "session_id")
							item.session_id = std::move(*field_value);
						else if (std::string_view{key} == "task_id")
							item.task_id = std::move(*field_value);
						else if (std::string_view{key} == "task_v4_digest")
							item.task_v4_digest = std::move(*field_value);
						else if (std::string_view{key} == "closure_id")
							item.closure_id = std::move(*field_value);
						else if (std::string_view{key} == "closure_digest")
							item.closure_digest = std::move(*field_value);
						else
							item.manifest_digest = std::move(*field_value);
					}
					auto total = uint_field(fields, "total_bytes");
					auto chunk = uint_field(fields, "chunk_bytes");
					auto count = uint_field(fields, "chunk_count");
					if (!total || !chunk || !count)
						return sdk::unexpected(!total		? total.error()
												   : !chunk ? chunk.error()
															: count.error());
					item.total_bytes = *total;
					item.chunk_bytes = *chunk;
					item.chunk_count = *count;
					if (auto valid = validate_manifest_descriptor(item, bound); !valid)
						return sdk::unexpected(valid.error());
					return closure_control{source_closure_manifest{std::move(item)}};
				}
				if (auto valid = require({"kind",
										  "session_id",
										  "task_id",
										  "manifest_digest",
										  "chunk_index",
										  "offset",
										  "byte_count"});
					!valid)
					return sdk::unexpected(valid.error());
				source_closure_manifest_chunk item;
				item.kind = *parsed;
				auto session = string_field(fields, "session_id");
				auto task = string_field(fields, "task_id");
				auto digest = string_field(fields, "manifest_digest");
				auto index = uint_field(fields, "chunk_index");
				auto offset = uint_field(fields, "offset");
				auto count = uint_field(fields, "byte_count");
				if (!session || !task || !digest || !index || !offset || !count)
					return sdk::unexpected(!session		 ? session.error()
											   : !task	 ? task.error()
											   : !digest ? digest.error()
											   : !index	 ? index.error()
											   : !offset ? offset.error()
														 : count.error());
				item.session_id = std::move(*session);
				item.task_id = std::move(*task);
				item.manifest_digest = std::move(*digest);
				item.chunk_index = *index;
				item.offset = *offset;
				item.byte_count = *count;
				if (auto valid = validate_manifest_chunk(item, bound); !valid)
					return sdk::unexpected(valid.error());
				return closure_control{source_closure_manifest{std::move(item)}};
			}
			if (type == message_type::source_closure_blob)
			{
				if (auto valid = require({"session_id",
										  "task_id",
										  "closure_digest",
										  "blob_ordinal",
										  "blob_digest",
										  "total_bytes",
										  "chunk_bytes",
										  "chunk_count"});
					!valid)
					return sdk::unexpected(valid.error());
				source_closure_blob_descriptor item;
				auto session = string_field(fields, "session_id");
				auto task = string_field(fields, "task_id");
				auto closure = string_field(fields, "closure_digest");
				auto ordinal = uint_field(fields, "blob_ordinal");
				auto digest = string_field(fields, "blob_digest");
				auto total = uint_field(fields, "total_bytes");
				auto chunk = uint_field(fields, "chunk_bytes");
				auto count = uint_field(fields, "chunk_count");
				if (!session || !task || !closure || !ordinal || !digest || !total || !chunk ||
					!count)
					return sdk::unexpected(!session		  ? session.error()
											   : !task	  ? task.error()
											   : !closure ? closure.error()
											   : !ordinal ? ordinal.error()
											   : !digest  ? digest.error()
											   : !total	  ? total.error()
											   : !chunk	  ? chunk.error()
														  : count.error());
				item.session_id = std::move(*session);
				item.task_id = std::move(*task);
				item.closure_digest = std::move(*closure);
				item.blob_ordinal = *ordinal;
				item.blob_digest = std::move(*digest);
				item.total_bytes = *total;
				item.chunk_bytes = *chunk;
				item.chunk_count = *count;
				if (auto valid = validate_blob_descriptor(item, bound); !valid)
					return sdk::unexpected(valid.error());
				return closure_control{std::move(item)};
			}
			if (type == message_type::source_closure_chunk)
			{
				if (auto valid = require({"session_id",
										  "task_id",
										  "blob_ordinal",
										  "blob_digest",
										  "chunk_index",
										  "offset",
										  "byte_count"});
					!valid)
					return sdk::unexpected(valid.error());
				source_closure_chunk item;
				auto session = string_field(fields, "session_id");
				auto task = string_field(fields, "task_id");
				auto ordinal = uint_field(fields, "blob_ordinal");
				auto digest = string_field(fields, "blob_digest");
				auto index = uint_field(fields, "chunk_index");
				auto offset = uint_field(fields, "offset");
				auto count = uint_field(fields, "byte_count");
				if (!session || !task || !ordinal || !digest || !index || !offset || !count)
					return sdk::unexpected(!session		  ? session.error()
											   : !task	  ? task.error()
											   : !ordinal ? ordinal.error()
											   : !digest  ? digest.error()
											   : !index	  ? index.error()
											   : !offset  ? offset.error()
														  : count.error());
				item.session_id = std::move(*session);
				item.task_id = std::move(*task);
				item.blob_ordinal = *ordinal;
				item.blob_digest = std::move(*digest);
				item.chunk_index = *index;
				item.offset = *offset;
				item.byte_count = *count;
				if (auto valid = validate_blob_chunk(item, bound); !valid)
					return sdk::unexpected(valid.error());
				return closure_control{std::move(item)};
			}
			if (type == message_type::source_closure_seal)
			{
				if (auto valid = require({"session_id",
										  "task_id",
										  "task_v4_digest",
										  "manifest_digest",
										  "blob_receipts_digest",
										  "blob_count",
										  "total_bytes",
										  "closure_digest",
										  "transfer_digest"});
					!valid)
					return sdk::unexpected(valid.error());
				source_closure_seal item;
				for (const auto key : {"session_id",
									   "task_id",
									   "task_v4_digest",
									   "manifest_digest",
									   "blob_receipts_digest",
									   "closure_digest",
									   "transfer_digest"})
				{
					auto field_value = string_field(fields, key);
					if (!field_value)
						return sdk::unexpected(field_value.error());
					if (std::string_view{key} == "session_id")
						item.session_id = std::move(*field_value);
					else if (std::string_view{key} == "task_id")
						item.task_id = std::move(*field_value);
					else if (std::string_view{key} == "task_v4_digest")
						item.task_v4_digest = std::move(*field_value);
					else if (std::string_view{key} == "manifest_digest")
						item.manifest_digest = std::move(*field_value);
					else if (std::string_view{key} == "blob_receipts_digest")
						item.blob_receipts_digest = std::move(*field_value);
					else if (std::string_view{key} == "closure_digest")
						item.closure_digest = std::move(*field_value);
					else
						item.transfer_digest = std::move(*field_value);
				}
				auto blobs = uint_field(fields, "blob_count");
				auto total = uint_field(fields, "total_bytes");
				if (!blobs || !total)
					return sdk::unexpected(!blobs ? blobs.error() : total.error());
				item.blob_count = *blobs;
				item.total_bytes = *total;
				if (auto valid = validate_seal(item, bound); !valid)
					return sdk::unexpected(valid.error());
				return closure_control{std::move(item)};
			}
			if (type == message_type::source_closure_ack)
			{
				if (auto valid = require({"session_id",
										  "task_id",
										  "closure_digest",
										  "transfer_digest",
										  "spool_receipt",
										  "cleanup_owner"});
					!valid)
					return sdk::unexpected(valid.error());
				source_closure_ack item;
				for (const auto key : {"session_id",
									   "task_id",
									   "closure_digest",
									   "transfer_digest",
									   "spool_receipt",
									   "cleanup_owner"})
				{
					auto field_value = string_field(fields, key);
					if (!field_value)
						return sdk::unexpected(field_value.error());
					if (std::string_view{key} == "session_id")
						item.session_id = std::move(*field_value);
					else if (std::string_view{key} == "task_id")
						item.task_id = std::move(*field_value);
					else if (std::string_view{key} == "closure_digest")
						item.closure_digest = std::move(*field_value);
					else if (std::string_view{key} == "transfer_digest")
						item.transfer_digest = std::move(*field_value);
					else if (std::string_view{key} == "spool_receipt")
						item.spool_receipt = std::move(*field_value);
					else
						item.cleanup_owner = std::move(*field_value);
				}
				if (auto valid = validate_ack(item); !valid)
					return sdk::unexpected(valid.error());
				return closure_control{std::move(item)};
			}
			if (type == message_type::source_closure_reject)
			{
				if (auto valid = require({"session_id",
										  "task_id",
										  "failure_phase",
										  "reason_code",
										  "observed_counters",
										  "cleanup_receipt"});
					!valid)
					return sdk::unexpected(valid.error());
				source_closure_reject item;
				for (const auto key :
					 {"session_id", "task_id", "failure_phase", "reason_code", "cleanup_receipt"})
				{
					auto field_value = string_field(fields, key);
					if (!field_value)
						return sdk::unexpected(field_value.error());
					if (std::string_view{key} == "session_id")
						item.session_id = std::move(*field_value);
					else if (std::string_view{key} == "task_id")
						item.task_id = std::move(*field_value);
					else if (std::string_view{key} == "failure_phase")
						item.failure_phase = std::move(*field_value);
					else if (std::string_view{key} == "reason_code")
						item.reason_code = std::move(*field_value);
					else
						item.cleanup_receipt = std::move(*field_value);
				}
				auto counters = cbor::field<cbor::map>(fields, "observed_counters");
				if (!counters)
					return sdk::unexpected(counters.error());
				item.observed_counters = std::move(*counters);
				if (auto valid = validate_reject(item); !valid)
					return sdk::unexpected(valid.error());
				return closure_control{std::move(item)};
			}
			return sdk::unexpected(failure("message_type", "not-closure"));
		}
	} // namespace

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
		if (!is_closure_message(type))
			return sdk::unexpected(failure("message_type", "not-closure"));
		auto fields = decode_map_owned(control);
		if (!fields)
			return sdk::unexpected(fields.error());
		return decode_owned(type, std::move(*fields), bound);
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
		const auto stream_id = session.stream_id;
		return closure_transfer{std::move(session),
								sequence_guard{stream_id, 0U},
								credit_window{session.initial_credit}};
	}

	sdk::result<void> closure_transfer::bind_common(const std::string_view session_id,
													const std::string_view task_id,
													const std::string_view closure_digest) const
	{
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

	sdk::result<void> closure_transfer::accept_impl(const frame& value)
	{
		if (value.protocol_major != protocol_major || value.protocol_minor != 0U)
			return sdk::unexpected(
				failure("protocol", "downgrade-or-unnegotiated", "source-closure.replay-invalid"));
		if (value.flags != 0U)
			return sdk::unexpected(failure(
				"flags", "closure-flags-must-be-zero", "source-closure.protocol-state-invalid"));
		if (!is_closure_message(value.type))
			return sdk::unexpected(failure("message_type", "not-closure"));
		if (value.control.size() > std::numeric_limits<std::size_t>::max() - value.payload.size() ||
			value.control.size() + value.payload.size() >
				session_.limits.maximum_resident_transport_bytes)
			return sdk::unexpected(failure(
				"resident_transport_bytes", "limit-exceeded", "source-closure.limit-exceeded"));
		const auto observed_control_digest = sha256(value.control);
		const auto observed_payload_digest = sha256(value.payload);
		if ((!digest_is_zero(value.control_digest) &&
			 !digest_equal(value.control_digest, observed_control_digest)) ||
			(!digest_is_zero(value.payload_digest) &&
			 !digest_equal(value.payload_digest, observed_payload_digest)))
			return sdk::unexpected(failure("digest", "mismatch", "source-closure.digest-mismatch"));
		auto decoded = decode_closure_control(value.type, value.control, session_.limits);
		if (!decoded)
			return sdk::unexpected(decoded.error());
		if (auto valid =
				validate_closure_payload(value.type, *decoded, value.payload, session_.limits);
			!valid)
			return valid;
		if (auto valid = sequence_.accept(value); !valid)
			return valid;
		if (auto valid = credit_.consume(value); !valid)
			return valid;

		if (value.type == message_type::source_closure_manifest)
		{
			const auto& manifest = std::get<source_closure_manifest>(*decoded);
			if (const auto* descriptor = std::get_if<source_closure_manifest_descriptor>(&manifest);
				descriptor != nullptr)
			{
				if (phase_ != closure_phase::task_v4_sealed)
					return sdk::unexpected(failure(
						"phase", "manifest-reopen", "source-closure.protocol-state-invalid"));
				if (auto valid = bind_common(
						descriptor->session_id, descriptor->task_id, descriptor->closure_digest);
					!valid)
					return valid;
				if (!session_.task_v4_digest.empty() &&
					descriptor->task_v4_digest != session_.task_v4_digest)
					return sdk::unexpected(failure(
						"task_v4_digest", "mismatch", "source-closure.task-binding-mismatch"));
				if (!session_.manifest_digest.empty() &&
					descriptor->manifest_digest != session_.manifest_digest)
					return sdk::unexpected(
						failure("manifest_digest", "mismatch", "source-closure.digest-mismatch"));
				manifest_total_ = descriptor->total_bytes;
				manifest_chunk_bytes_ = descriptor->chunk_bytes;
				manifest_chunk_count_ = descriptor->chunk_count;
				manifest_next_chunk_ = 0U;
				manifest_observed_ = 0U;
				phase_ = manifest_chunk_count_ == 0U ? closure_phase::manifest_validated
													 : closure_phase::manifest_streaming;
				return {};
			}
			const auto* chunk = std::get_if<source_closure_manifest_chunk>(&manifest);
			if (chunk == nullptr ||
				(phase_ != closure_phase::manifest_streaming &&
				 phase_ != closure_phase::manifest_open))
				return sdk::unexpected(failure(
					"phase", "manifest-chunk-order", "source-closure.protocol-state-invalid"));
			if (auto valid = bind_common(chunk->session_id, chunk->task_id); !valid)
				return valid;
			if (chunk->manifest_digest != session_.manifest_digest &&
				!session_.manifest_digest.empty())
				return sdk::unexpected(
					failure("manifest_digest", "mismatch", "source-closure.digest-mismatch"));
			if (manifest_observed_ > manifest_total_ ||
				chunk->chunk_index != manifest_next_chunk_ || chunk->offset != manifest_observed_ ||
				chunk->byte_count > manifest_total_ - manifest_observed_ ||
				(chunk->chunk_index + 1U < manifest_chunk_count_ &&
				 chunk->byte_count != manifest_chunk_bytes_) ||
				(chunk->chunk_index + 1U == manifest_chunk_count_ &&
				 chunk->byte_count != manifest_total_ - manifest_observed_))
				return sdk::unexpected(failure(
					"chunk", "gap-overlap-or-reorder", "source-closure.chunk-order-invalid"));
			manifest_observed_ += chunk->byte_count;
			++manifest_next_chunk_;
			if (manifest_next_chunk_ == manifest_chunk_count_)
				phase_ = closure_phase::manifest_validated;
			return {};
		}

		if (value.type == message_type::source_closure_blob)
		{
			if (phase_ != closure_phase::manifest_validated)
				return sdk::unexpected(failure(
					"phase", "blob-before-manifest", "source-closure.protocol-state-invalid"));
			const auto& item = std::get<source_closure_blob_descriptor>(*decoded);
			if (auto valid = bind_common(item.session_id, item.task_id, item.closure_digest);
				!valid)
				return valid;
			if (item.blob_ordinal != next_blob_ordinal_)
				return sdk::unexpected(
					failure("blob_ordinal", "gap-or-replay", "source-closure.blob-order-invalid"));
			current_blob_ordinal_ = item.blob_ordinal;
			current_blob_digest_ = item.blob_digest;
			current_blob_total_ = item.total_bytes;
			current_blob_chunk_bytes_ = item.chunk_bytes;
			current_blob_chunk_count_ = item.chunk_count;
			current_blob_next_chunk_ = 0U;
			current_blob_observed_ = 0U;
			phase_ = current_blob_chunk_count_ == 0U ? closure_phase::manifest_validated
													 : closure_phase::blob_streaming;
			if (current_blob_chunk_count_ == 0U)
			{
				++next_blob_ordinal_;
				if (item.total_bytes > session_.limits.maximum_unique_blob_bytes ||
					blob_observed_total_ >
						session_.limits.maximum_unique_blob_bytes - item.total_bytes)
					return sdk::unexpected(
						failure("blob_bytes", "limit-exceeded", "source-closure.limit-exceeded"));
				blob_observed_total_ += item.total_bytes;
			}
			return {};
		}

		if (value.type == message_type::source_closure_chunk)
		{
			if (phase_ != closure_phase::blob_streaming)
				return sdk::unexpected(
					failure("phase", "blob-chunk-order", "source-closure.protocol-state-invalid"));
			const auto& item = std::get<source_closure_chunk>(*decoded);
			if (auto valid = bind_common(item.session_id, item.task_id); !valid)
				return valid;
			if (item.blob_ordinal != current_blob_ordinal_ ||
				item.blob_digest != current_blob_digest_ ||
				item.chunk_index != current_blob_next_chunk_ ||
				item.offset != current_blob_observed_ ||
				item.byte_count > current_blob_total_ - current_blob_observed_ ||
				(item.chunk_index + 1U < current_blob_chunk_count_ &&
				 item.byte_count != current_blob_chunk_bytes_) ||
				(item.chunk_index + 1U == current_blob_chunk_count_ &&
				 item.byte_count != current_blob_total_ - current_blob_observed_))
				return sdk::unexpected(failure(
					"chunk", "gap-overlap-or-reorder", "source-closure.chunk-order-invalid"));
			current_blob_observed_ += item.byte_count;
			++current_blob_next_chunk_;
			if (item.byte_count > session_.limits.maximum_unique_blob_bytes ||
				item.byte_count > session_.limits.maximum_task_spool_bytes ||
				blob_observed_total_ >
					session_.limits.maximum_unique_blob_bytes - item.byte_count ||
				blob_observed_total_ > session_.limits.maximum_task_spool_bytes - item.byte_count)
				return sdk::unexpected(
					failure("blob_bytes", "limit-exceeded", "source-closure.limit-exceeded"));
			blob_observed_total_ += item.byte_count;
			if (current_blob_next_chunk_ == current_blob_chunk_count_)
			{
				if (next_blob_ordinal_ >= session_.limits.maximum_blobs)
					return sdk::unexpected(
						failure("blob_count", "limit-exceeded", "source-closure.limit-exceeded"));
				++next_blob_ordinal_;
				phase_ = closure_phase::manifest_validated;
			}
			return {};
		}

		if (value.type == message_type::source_closure_seal)
		{
			if (phase_ != closure_phase::manifest_validated ||
				current_blob_next_chunk_ != current_blob_chunk_count_)
				return sdk::unexpected(
					failure("phase", "seal-order", "source-closure.protocol-state-invalid"));
			const auto& item = std::get<source_closure_seal>(*decoded);
			if (auto valid = bind_common(item.session_id, item.task_id, item.closure_digest);
				!valid)
				return valid;
			if ((!session_.task_v4_digest.empty() &&
				 item.task_v4_digest != session_.task_v4_digest) ||
				(!session_.manifest_digest.empty() &&
				 item.manifest_digest != session_.manifest_digest) ||
				item.blob_count != next_blob_ordinal_ || item.total_bytes != blob_observed_total_)
				return sdk::unexpected(failure(
					"seal", "binding-or-counter-mismatch", "source-closure.digest-mismatch"));
			phase_ = closure_phase::closure_sealed;
			return {};
		}

		if (value.type == message_type::source_closure_ack)
		{
			if (phase_ != closure_phase::closure_sealed)
				return sdk::unexpected(
					failure("phase", "ack-order", "source-closure.protocol-state-invalid"));
			const auto& item = std::get<source_closure_ack>(*decoded);
			if (auto valid = bind_common(item.session_id, item.task_id, item.closure_digest);
				!valid)
				return valid;
			phase_ = closure_phase::acknowledged;
			return {};
		}

		const auto& item = std::get<source_closure_reject>(*decoded);
		if (phase_ == closure_phase::acknowledged || phase_ == closure_phase::rejected)
			return sdk::unexpected(
				failure("phase", "terminal-replay", "source-closure.replay-invalid"));
		if (auto valid = bind_common(item.session_id, item.task_id); !valid)
			return valid;
		phase_ = closure_phase::rejected;
		return {};
	}

	sdk::result<void> closure_transfer::accept(const frame& value)
	{
		auto trial = *this;
		if (auto valid = trial.accept_impl(value); !valid)
			return valid;
		*this = std::move(trial);
		return {};
	}
} // namespace cxxlens::protocol_v2
