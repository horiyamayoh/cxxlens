#include "source_closure_transport.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <new>
#include <ranges>
#include <string>
#include <utility>

namespace cxxlens::detail::clang22
{
	namespace
	{
		constexpr std::string_view receipts_domain{"cxxlens.source-closure-blob-receipts.v1"};
		constexpr std::string_view transfer_domain{"cxxlens.source-closure-transfer.v1"};
		constexpr std::string_view semantic_prefix{"semantic-v2:sha256:"};
		constexpr std::string_view content_prefix{"sha256:"};
		constexpr std::string_view empty_blob_digest{
			"sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"};

		struct reject_phase_contract
		{
			std::span<const std::string_view> reasons;
			std::span<const std::string_view> counters;
		};

		constexpr std::array<std::string_view, 7U> before_manifest_reasons{
			"source-closure.required-feature-missing",
			"source-closure.protocol-state-invalid",
			"source-closure.task-binding-mismatch",
			"source-closure.session-binding-mismatch",
			"source-closure.limit-exceeded",
			"source-closure.cancelled",
			"source-closure.transfer-timeout"};
		constexpr std::array<std::string_view, 1U> before_manifest_counters{
			"observed-control-frame-count"};
		constexpr std::array<std::string_view, 13U> manifest_streaming_reasons{
			"source-closure.protocol-state-invalid",
			"source-closure.manifest-invalid",
			"source-closure.chunk-order-invalid",
			"source-closure.chunk-overlap",
			"source-closure.chunk-gap",
			"source-closure.digest-mismatch",
			"source-closure.limit-exceeded",
			"source-closure.task-binding-mismatch",
			"source-closure.session-binding-mismatch",
			"source-closure.cancelled",
			"source-closure.transfer-timeout",
			"source-closure.spool-io",
			"source-closure.cleanup-failed"};
		constexpr std::array<std::string_view, 3U> manifest_streaming_counters{
			"declared-manifest-bytes", "next-chunk-index", "received-manifest-bytes"};
		constexpr std::array<std::string_view, 9U> manifest_validated_reasons{
			"source-closure.protocol-state-invalid",
			"source-closure.blob-order-invalid",
			"source-closure.limit-exceeded",
			"source-closure.task-binding-mismatch",
			"source-closure.session-binding-mismatch",
			"source-closure.cancelled",
			"source-closure.transfer-timeout",
			"source-closure.spool-io",
			"source-closure.cleanup-failed"};
		constexpr std::array<std::string_view, 3U> manifest_validated_counters{
			"blob-count", "member-count", "total-blob-bytes"};
		constexpr std::array<std::string_view, 13U> blob_streaming_reasons{
			"source-closure.protocol-state-invalid",
			"source-closure.blob-order-invalid",
			"source-closure.chunk-order-invalid",
			"source-closure.chunk-overlap",
			"source-closure.chunk-gap",
			"source-closure.digest-mismatch",
			"source-closure.limit-exceeded",
			"source-closure.task-binding-mismatch",
			"source-closure.session-binding-mismatch",
			"source-closure.cancelled",
			"source-closure.transfer-timeout",
			"source-closure.spool-io",
			"source-closure.cleanup-failed"};
		constexpr std::array<std::string_view, 4U> blob_streaming_counters{
			"blob-ordinal", "declared-blob-bytes", "next-chunk-index", "received-blob-bytes"};
		constexpr std::array<std::string_view, 8U> closure_sealed_reasons{
			"source-closure.protocol-state-invalid",
			"source-closure.digest-mismatch",
			"source-closure.task-binding-mismatch",
			"source-closure.session-binding-mismatch",
			"source-closure.replay-invalid",
			"source-closure.cancelled",
			"source-closure.transfer-timeout",
			"source-closure.cleanup-failed"};
		constexpr std::array<std::string_view, 2U> closure_sealed_counters{"blob-count",
																		   "total-bytes"};
		constexpr std::array<std::string_view, 6U> acknowledged_reasons{
			"source-closure.protocol-state-invalid",
			"source-closure.task-binding-mismatch",
			"source-closure.session-binding-mismatch",
			"source-closure.replay-invalid",
			"source-closure.cancelled",
			"source-closure.cleanup-failed"};
		constexpr std::array<std::string_view, 2U> acknowledged_counters{"blob-count",
																		 "total-bytes"};

		[[nodiscard]] reject_phase_contract phase_contract(const std::string_view phase) noexcept
		{
			if (phase == "before-manifest")
				return {before_manifest_reasons, before_manifest_counters};
			if (phase == "manifest-streaming")
				return {manifest_streaming_reasons, manifest_streaming_counters};
			if (phase == "manifest-validated")
				return {manifest_validated_reasons, manifest_validated_counters};
			if (phase == "blob-streaming")
				return {blob_streaming_reasons, blob_streaming_counters};
			if (phase == "closure-sealed")
				return {closure_sealed_reasons, closure_sealed_counters};
			if (phase == "acknowledged")
				return {acknowledged_reasons, acknowledged_counters};
			return {};
		}

		[[nodiscard]] sdk::error
		failure(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool exceeds(const std::uint64_t current,
								   const std::uint64_t added,
								   const std::uint64_t maximum) noexcept
		{
			return current > maximum || added > maximum - current;
		}

		[[nodiscard]] bool checked_chunk_count(const std::uint64_t total_bytes,
											   const std::uint64_t chunk_bytes,
											   std::uint64_t& output) noexcept
		{
			if (chunk_bytes == 0U)
				return false;
			if (total_bytes == 0U)
			{
				output = 0U;
				return true;
			}
			if (total_bytes > std::numeric_limits<std::uint64_t>::max() - (chunk_bytes - 1U))
				return false;
			output = (total_bytes + chunk_bytes - 1U) / chunk_bytes;
			return true;
		}

		[[nodiscard]] bool exceeds_three(const std::uint64_t first,
										 const std::uint64_t second,
										 const std::uint64_t added,
										 const std::uint64_t maximum) noexcept
		{
			if (exceeds(first, second, maximum))
				return true;
			return exceeds(first + second, added, maximum);
		}

		[[nodiscard]] bool hex_lower(const std::string_view value) noexcept
		{
			return std::ranges::all_of(value,
									   [](const char byte)
									   {
										   return (byte >= '0' && byte <= '9') ||
											   (byte >= 'a' && byte <= 'f');
									   });
		}

		[[nodiscard]] bool typed_digest(const std::string_view value,
										const std::string_view prefix) noexcept
		{
			return value.size() == prefix.size() + 64U && value.starts_with(prefix) &&
				hex_lower(value.substr(prefix.size()));
		}

		[[nodiscard]] bool typed_id(const std::string_view value,
									const std::string_view prefix) noexcept
		{
			return typed_digest(value, prefix);
		}

		[[nodiscard]] bool valid_binding(const source_closure_transfer_binding& value) noexcept
		{
			return typed_id(value.session_id, "provider-session:sha256:") &&
				typed_id(value.task_id, "task:semantic-v2:sha256:") &&
				typed_digest(value.task_v4_digest, semantic_prefix) &&
				value.task_id == "task:" + value.task_v4_digest &&
				typed_id(value.closure_id, "source-closure:semantic-v2:sha256:") &&
				typed_digest(value.closure_digest, semantic_prefix) &&
				value.closure_id == "source-closure:" + value.closure_digest &&
				typed_digest(value.manifest_digest, semantic_prefix);
		}

		[[nodiscard]] bool valid_spool_id(const std::string_view value,
										  const std::string_view prefix) noexcept
		{
			return typed_id(value, prefix);
		}

		[[nodiscard]] std::string json_quote(const std::string_view value)
		{
			std::string output;
			output.reserve(value.size() + 2U);
			output.push_back('"');
			constexpr char hex[] = "0123456789abcdef";
			for (const char value_byte : value)
			{
				const auto byte = static_cast<unsigned char>(value_byte);
				switch (byte)
				{
					case '"':
						output.append("\\\"");
						break;
					case '\\':
						output.append("\\\\");
						break;
					case '\b':
						output.append("\\b");
						break;
					case '\f':
						output.append("\\f");
						break;
					case '\n':
						output.append("\\n");
						break;
					case '\r':
						output.append("\\r");
						break;
					case '\t':
						output.append("\\t");
						break;
					default:
						if (byte < 0x20U)
						{
							output.append("\\u00");
							output.push_back(hex[(byte >> 4U) & 0x0fU]);
							output.push_back(hex[byte & 0x0fU]);
						}
						else
							output.push_back(static_cast<char>(byte));
				}
			}
			output.push_back('"');
			return output;
		}

		[[nodiscard]] std::string json_u64(const std::uint64_t value)
		{
			std::array<char, 32U> buffer{};
			const auto converted =
				std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
			return std::string{buffer.data(), converted.ptr};
		}

		[[nodiscard]] sdk::result<std::string> semantic_json_digest(const std::string_view domain,
																	const std::string_view json)
		{
			return sdk::semantic_digest(domain, json);
		}

		constexpr std::array<std::uint32_t, 64U> sha_k{
			0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
			0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
			0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
			0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
			0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
			0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
			0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
			0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
			0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
			0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
			0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

		[[nodiscard]] constexpr std::uint32_t rotr(const std::uint32_t value,
												   const unsigned shift) noexcept
		{
			return (value >> shift) | (value << (32U - shift));
		}

		void sha_transform(std::array<std::uint32_t, 8U>& state, const std::byte* block)
		{
			std::array<std::uint32_t, 64U> words{};
			for (std::size_t index{}; index < 16U; ++index)
			{
				const auto offset = index * 4U;
				words[index] = (std::to_integer<std::uint32_t>(block[offset]) << 24U) |
					(std::to_integer<std::uint32_t>(block[offset + 1U]) << 16U) |
					(std::to_integer<std::uint32_t>(block[offset + 2U]) << 8U) |
					std::to_integer<std::uint32_t>(block[offset + 3U]);
			}
			for (std::size_t index = 16U; index < words.size(); ++index)
			{
				const auto s0 = rotr(words[index - 15U], 7U) ^ rotr(words[index - 15U], 18U) ^
					(words[index - 15U] >> 3U);
				const auto s1 = rotr(words[index - 2U], 17U) ^ rotr(words[index - 2U], 19U) ^
					(words[index - 2U] >> 10U);
				words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
			}
			auto [a, b, c, d, e, f, g, h] = state;
			for (std::size_t index{}; index < words.size(); ++index)
			{
				const auto t1 = h + (rotr(e, 6U) ^ rotr(e, 11U) ^ rotr(e, 25U)) +
					((e & f) ^ (~e & g)) + sha_k[index] + words[index];
				const auto t2 =
					(rotr(a, 2U) ^ rotr(a, 13U) ^ rotr(a, 22U)) + ((a & b) ^ (a & c) ^ (b & c));
				h = g;
				g = f;
				f = e;
				e = d + t1;
				d = c;
				c = b;
				b = a;
				a = t1 + t2;
			}
			state[0U] += a;
			state[1U] += b;
			state[2U] += c;
			state[3U] += d;
			state[4U] += e;
			state[5U] += f;
			state[6U] += g;
			state[7U] += h;
		}

		void sha_reset(std::array<std::uint32_t, 8U>& state,
					   std::array<std::byte, 64U>& buffer,
					   std::size_t& buffer_size,
					   std::uint64_t& total_bytes)
		{
			state = {0x6a09e667U,
					 0xbb67ae85U,
					 0x3c6ef372U,
					 0xa54ff53aU,
					 0x510e527fU,
					 0x9b05688cU,
					 0x1f83d9abU,
					 0x5be0cd19U};
			buffer.fill(std::byte{});
			buffer_size = 0U;
			total_bytes = 0U;
		}

		void sha_update(std::array<std::uint32_t, 8U>& state,
						std::array<std::byte, 64U>& buffer,
						std::size_t& buffer_size,
						std::uint64_t& total_bytes,
						const std::span<const std::byte> bytes)
		{
			std::size_t offset{};
			if (buffer_size != 0U)
			{
				const auto copied = std::min(buffer.size() - buffer_size, bytes.size());
				std::copy_n(bytes.data(), copied, buffer.data() + buffer_size);
				buffer_size += copied;
				offset += copied;
				if (buffer_size == buffer.size())
				{
					sha_transform(state, buffer.data());
					buffer_size = 0U;
				}
			}
			while (bytes.size() - offset >= buffer.size())
			{
				sha_transform(state, bytes.data() + offset);
				offset += buffer.size();
			}
			if (const auto remaining = bytes.size() - offset; remaining != 0U)
			{
				std::copy_n(bytes.data() + offset, remaining, buffer.data());
				buffer_size = remaining;
			}
			total_bytes += bytes.size();
		}

		[[nodiscard]] std::string sha_finish(const std::array<std::uint32_t, 8U>& state,
											 const std::array<std::byte, 64U>& buffer,
											 const std::size_t buffer_size,
											 const std::uint64_t total_bytes)
		{
			auto final_state = state;
			auto final_buffer = buffer;
			auto final_size = buffer_size;
			const auto marker = std::byte{0x80U};
			final_buffer[final_size++] = marker;
			if (final_size > 56U)
			{
				std::fill(final_buffer.begin() + static_cast<std::ptrdiff_t>(final_size),
						  final_buffer.end(),
						  std::byte{});
				sha_transform(final_state, final_buffer.data());
				final_size = 0U;
			}
			std::fill(final_buffer.begin() + static_cast<std::ptrdiff_t>(final_size),
					  final_buffer.begin() + 56,
					  std::byte{});
			const auto bit_length = total_bytes * 8U;
			for (std::size_t index{}; index < 8U; ++index)
				final_buffer[56U + index] =
					static_cast<std::byte>((bit_length >> ((7U - index) * 8U)) & 0xffU);
			sha_transform(final_state, final_buffer.data());
			constexpr char hex[] = "0123456789abcdef";
			std::string output{"sha256:"};
			output.reserve(71U);
			for (const auto word : final_state)
				for (int shift = 28; shift >= 0; shift -= 4)
					output.push_back(hex[(word >> shift) & 0x0fU]);
			return output;
		}

		void sha_byte(std::array<std::uint32_t, 8U>& state,
					  std::array<std::byte, 64U>& buffer,
					  std::size_t& buffer_size,
					  std::uint64_t& total_bytes,
					  const std::byte value)
		{
			sha_update(state, buffer, buffer_size, total_bytes, std::span{&value, 1U});
		}

		void sha_u64(std::array<std::uint32_t, 8U>& state,
					 std::array<std::byte, 64U>& buffer,
					 std::size_t& buffer_size,
					 std::uint64_t& total_bytes,
					 const std::uint64_t value)
		{
			std::array<std::byte, 8U> encoded{};
			for (std::size_t index{}; index < encoded.size(); ++index)
				encoded[index] = static_cast<std::byte>((value >> ((7U - index) * 8U)) & 0xffU);
			sha_update(state, buffer, buffer_size, total_bytes, encoded);
		}

		void sha_text(std::array<std::uint32_t, 8U>& state,
					  std::array<std::byte, 64U>& buffer,
					  std::size_t& buffer_size,
					  std::uint64_t& total_bytes,
					  const std::string_view value)
		{
			sha_update(state,
					   buffer,
					   buffer_size,
					   total_bytes,
					   std::as_bytes(std::span{value.data(), value.size()}));
		}

		[[nodiscard]] std::string semantic_stream_digest(const std::string_view domain,
														 const std::uint64_t payload_bytes,
														 const auto& emit_payload)
		{
			std::array<std::uint32_t, 8U> state{};
			std::array<std::byte, 64U> buffer{};
			std::size_t buffer_size{};
			std::uint64_t total_bytes{};
			sha_reset(state, buffer, buffer_size, total_bytes);
			sha_byte(state, buffer, buffer_size, total_bytes, std::byte{0x05U});
			sha_u64(state, buffer, buffer_size, total_bytes, 3U);
			const auto emit_string = [&](const std::string_view value)
			{
				sha_u64(state, buffer, buffer_size, total_bytes, 1U + 8U + value.size());
				sha_byte(state, buffer, buffer_size, total_bytes, std::byte{0x04U});
				sha_u64(state, buffer, buffer_size, total_bytes, value.size());
				sha_text(state, buffer, buffer_size, total_bytes, value);
			};
			emit_string("cxxlens-semantic-digest-v2");
			emit_string(domain);
			sha_u64(state, buffer, buffer_size, total_bytes, 1U + 8U + payload_bytes);
			sha_byte(state, buffer, buffer_size, total_bytes, std::byte{0x03U});
			sha_u64(state, buffer, buffer_size, total_bytes, payload_bytes);
			emit_payload(state, buffer, buffer_size, total_bytes);
			const auto raw = sha_finish(state, buffer, buffer_size, total_bytes);
			return "semantic-v2:sha256:" + raw.substr(std::string_view{"sha256:"}.size());
		}

		[[nodiscard]] bool add_size(std::uint64_t& total, const std::uint64_t value) noexcept
		{
			if (value > std::numeric_limits<std::uint64_t>::max() - total)
				return false;
			total += value;
			return true;
		}

		[[nodiscard]] std::string phase_name(const source_closure_transfer_state state)
		{
			switch (state)
			{
				case source_closure_transfer_state::task_v4_sealed:
					return "before-manifest";
				case source_closure_transfer_state::manifest_open:
				case source_closure_transfer_state::manifest_streaming:
					return "manifest-streaming";
				case source_closure_transfer_state::manifest_validated:
					return "manifest-validated";
				case source_closure_transfer_state::blob_open:
				case source_closure_transfer_state::blob_streaming:
				case source_closure_transfer_state::blob_sealed:
					return "blob-streaming";
				case source_closure_transfer_state::closure_sealed:
					return "closure-sealed";
				case source_closure_transfer_state::closure_acknowledged:
				case source_closure_transfer_state::rejected:
					return "acknowledged";
				case source_closure_transfer_state::local_terminal:
					return "before-manifest";
			}
			return "before-manifest";
		}

	} // namespace

	sdk::result<std::string>
	source_closure_blob_receipts_digest(const std::span<const source_closure_blob_receipt> receipts)
	{
		std::uint64_t expected_ordinal{};
		std::uint64_t payload_bytes{2U};
		for (const auto& receipt : receipts)
		{
			if (receipt.blob_ordinal != expected_ordinal++ ||
				!typed_digest(receipt.blob_digest, content_prefix))
				return sdk::unexpected(failure("source-closure.digest-mismatch", "blob-receipts"));
			const auto digest = json_quote(receipt.blob_digest);
			const auto ordinal = json_u64(receipt.blob_ordinal);
			const auto size = json_u64(receipt.size_bytes);
			// RFC 8785-compatible key order is blob_digest, blob_ordinal, size_bytes.
			const auto item_bytes = std::string_view{"{\"blob_digest\":"}.size() + digest.size() +
				std::string_view{",\"blob_ordinal\":"}.size() + ordinal.size() +
				std::string_view{",\"size_bytes\":"}.size() + size.size() + 1U;
			if ((receipt.blob_ordinal != 0U && !add_size(payload_bytes, 1U)) ||
				!add_size(payload_bytes, item_bytes))
				return sdk::unexpected(failure("source-closure.limit-exceeded", "blob-receipts"));
		}
		const auto emit_payload =
			[&](auto& state, auto& buffer, auto& buffer_size, auto& total_bytes)
		{
			sha_text(state, buffer, buffer_size, total_bytes, "[");
			for (std::size_t index{}; index < receipts.size(); ++index)
			{
				if (index != 0U)
					sha_text(state, buffer, buffer_size, total_bytes, ",");
				const auto& receipt = receipts[index];
				sha_text(state, buffer, buffer_size, total_bytes, "{\"blob_digest\":");
				sha_text(state, buffer, buffer_size, total_bytes, json_quote(receipt.blob_digest));
				sha_text(state, buffer, buffer_size, total_bytes, ",\"blob_ordinal\":");
				sha_text(state, buffer, buffer_size, total_bytes, json_u64(receipt.blob_ordinal));
				sha_text(state, buffer, buffer_size, total_bytes, ",\"size_bytes\":");
				sha_text(state, buffer, buffer_size, total_bytes, json_u64(receipt.size_bytes));
				sha_text(state, buffer, buffer_size, total_bytes, "}");
			}
			sha_text(state, buffer, buffer_size, total_bytes, "]");
		};
		return semantic_stream_digest(receipts_domain, payload_bytes, emit_payload);
	}

	sdk::result<std::string>
	source_closure_transfer_digest(const source_closure_transfer_binding& binding,
								   const std::string_view blob_receipts_digest_value,
								   const std::uint64_t blob_count,
								   const std::uint64_t total_bytes)
	{
		if (!valid_binding(binding) || !typed_digest(blob_receipts_digest_value, semantic_prefix))
			return sdk::unexpected(failure("source-closure.task-binding-mismatch", "transfer"));
		// Canonical JSON object key order is blob_count, blob_receipts_digest, closure_digest,
		// manifest_digest, session_id, task_id, task_v4_digest, total_bytes.
		const auto projection = "{\"blob_count\":" + json_u64(blob_count) +
			",\"blob_receipts_digest\":" + json_quote(blob_receipts_digest_value) +
			",\"closure_digest\":" + json_quote(binding.closure_digest) +
			",\"manifest_digest\":" + json_quote(binding.manifest_digest) +
			",\"session_id\":" + json_quote(binding.session_id) +
			",\"task_id\":" + json_quote(binding.task_id) +
			",\"task_v4_digest\":" + json_quote(binding.task_v4_digest) +
			",\"total_bytes\":" + json_u64(total_bytes) + "}";
		return semantic_json_digest(transfer_domain, projection);
	}

	sdk::result<void>
	validate_source_closure_capability(const std::uint16_t protocol_minor,
									   const std::span<const std::string_view> capabilities)
	{
		if (protocol_minor != source_closure_protocol_minor)
			return sdk::unexpected(
				failure("source-closure.protocol-state-invalid", "protocol-minor"));
		if (std::ranges::find(capabilities, source_closure_capability) == capabilities.end())
			return sdk::unexpected(
				failure("source-closure.required-feature-missing", "capability"));
		return {};
	}

	sdk::result<void> validate_source_closure_frame_header(const std::uint16_t message_id,
														   const std::uint16_t flags)
	{
		if (!is_source_closure_message_id(message_id))
			return sdk::unexpected(failure("source-closure.protocol-state-invalid", "message-id"));
		if (flags != 0U)
			return sdk::unexpected(failure("source-closure.protocol-state-invalid", "flags"));
		return {};
	}

	source_closure_transfer_validator::source_closure_transfer_validator(
		source_closure_transfer_binding binding,
		source_closure_task_v4_authority& authority,
		source_closure_transfer_sink& sink,
		const source_closure_transport_limits limits)
		: binding_{std::move(binding)}, authority_{&authority}, sink_{&sink}, limits_{limits},
		  next_sequence_{binding_.first_sequence}
	{
		sha_reset(
			blob_hash_state_, blob_hash_buffer_, blob_hash_buffer_size_, blob_hash_total_bytes_);
	}

	sdk::result<void> source_closure_transfer_validator::sequence(const std::uint64_t value)
	{
		if (value != next_sequence_)
			return sdk::unexpected(failure(
				"source-closure.protocol-state-invalid", "sequence", std::to_string(value)));
		if (next_sequence_ == std::numeric_limits<std::uint64_t>::max())
			return sdk::unexpected(failure("source-closure.limit-exceeded", "sequence"));
		++next_sequence_;
		return {};
	}

	sdk::result<void>
	source_closure_transfer_validator::ensure_identity(const std::string_view session_id,
													   const std::string_view task_id) const
	{
		if (!typed_id(binding_.session_id, "provider-session:sha256:"))
			return sdk::unexpected(
				failure("source-closure.task-binding-mismatch", "outer-task", "session"));
		if (!typed_id(binding_.task_id, "task:semantic-v2:sha256:") ||
			!typed_digest(binding_.task_v4_digest, semantic_prefix) ||
			binding_.task_id != "task:" + binding_.task_v4_digest)
			return sdk::unexpected(
				failure("source-closure.task-binding-mismatch", "outer-task", "task"));
		if (!typed_id(binding_.closure_id, "source-closure:semantic-v2:sha256:") ||
			!typed_digest(binding_.closure_digest, semantic_prefix) ||
			binding_.closure_id != "source-closure:" + binding_.closure_digest)
			return sdk::unexpected(
				failure("source-closure.task-binding-mismatch", "outer-task", "closure"));
		if (!typed_digest(binding_.manifest_digest, semantic_prefix))
			return sdk::unexpected(
				failure("source-closure.task-binding-mismatch", "outer-task", "manifest"));
		if (session_id != binding_.session_id)
			return sdk::unexpected(
				failure("source-closure.session-binding-mismatch", "session-id"));
		if (task_id != binding_.task_id)
			return sdk::unexpected(failure("source-closure.task-binding-mismatch", "task-id"));
		return {};
	}

	sdk::result<void> source_closure_transfer_validator::fail(std::string code,
															  const std::string field,
															  std::string detail)
	{
		state_ = source_closure_transfer_state::rejected;
		auto cleanup = cleanup_once();
		if (!cleanup)
			return sdk::unexpected(
				failure("source-closure.cleanup-failed", "cleanup", cleanup.error().detail));
		return sdk::unexpected(failure(std::move(code), field, std::move(detail)));
	}

	sdk::result<std::string> source_closure_transfer_validator::cleanup_once()
	{
		if (cleanup_done_)
			return cleanup_receipt_;
		auto receipt = sink_->cleanup();
		if (!receipt)
			return sdk::unexpected(std::move(receipt.error()));
		if (!valid_spool_id(*receipt, "cleanup-receipt:semantic-v2:"))
			return sdk::unexpected(failure("source-closure.cleanup-failed", "cleanup-receipt"));
		cleanup_done_ = true;
		cleanup_receipt_ = *receipt;
		return cleanup_receipt_;
	}

	sdk::result<void> source_closure_transfer_validator::begin_manifest(
		const source_closure_manifest_descriptor& descriptor, const std::uint64_t sequence_value)
	{
		if (state_ != source_closure_transfer_state::task_v4_sealed)
			return sdk::unexpected(failure("source-closure.protocol-state-invalid", "manifest"));
		if (auto valid = ensure_identity(descriptor.session_id, descriptor.task_id); !valid)
			return valid;
		if (descriptor.task_v4_digest != binding_.task_v4_digest ||
			descriptor.closure_id != binding_.closure_id ||
			descriptor.closure_digest != binding_.closure_digest ||
			descriptor.manifest_digest != binding_.manifest_digest)
			return sdk::unexpected(failure("source-closure.task-binding-mismatch", "manifest"));
		if (authority_->task_id() != binding_.task_id ||
			authority_->task_v4_digest() != binding_.task_v4_digest)
			return fail("source-closure.task-binding-mismatch", "outer-task", "authority");
		if (auto valid = authority_->revalidate(); !valid)
			return fail("source-closure.task-binding-mismatch", "outer-task", valid.error().detail);
		if (descriptor.total_bytes == 0U ||
			descriptor.total_bytes > limits_.maximum_manifest_bytes ||
			descriptor.chunk_bytes == 0U ||
			descriptor.chunk_bytes > limits_.maximum_chunk_payload_bytes ||
			descriptor.total_bytes > limits_.maximum_task_spool_bytes)
			return sdk::unexpected(failure("source-closure.limit-exceeded", "manifest"));
		std::uint64_t expected_chunks{};
		if (!checked_chunk_count(descriptor.total_bytes, descriptor.chunk_bytes, expected_chunks) ||
			descriptor.chunk_count != expected_chunks ||
			descriptor.chunk_count > limits_.maximum_manifest_chunks)
			return sdk::unexpected(failure("source-closure.chunk-order-invalid", "manifest"));
		if (auto valid = sequence(sequence_value); !valid)
			return valid;
		if (auto valid = sink_->begin_manifest(descriptor); !valid)
			return fail("source-closure.spool-io", "manifest", valid.error().detail);
		declared_bytes_ = descriptor.total_bytes;
		manifest_bytes_ = descriptor.total_bytes;
		declared_chunk_bytes_ = descriptor.chunk_bytes;
		declared_chunk_count_ = descriptor.chunk_count;
		next_chunk_index_ = 0U;
		next_offset_ = 0U;
		state_ = source_closure_transfer_state::manifest_open;
		return {};
	}

	sdk::result<void>
	source_closure_transfer_validator::manifest_chunk(const source_closure_manifest_chunk& control,
													  const std::span<const std::byte> payload,
													  const std::uint64_t sequence_value)
	{
		if (state_ != source_closure_transfer_state::manifest_open &&
			state_ != source_closure_transfer_state::manifest_streaming)
			return sdk::unexpected(
				failure("source-closure.protocol-state-invalid", "manifest-chunk"));
		if (auto valid = ensure_identity(control.session_id, control.task_id); !valid)
			return fail(valid.error().code, valid.error().field, valid.error().detail);
		if (control.manifest_digest != binding_.manifest_digest)
			return fail("source-closure.task-binding-mismatch", "manifest-chunk");
		if (control.chunk_index < next_chunk_index_ || control.offset < next_offset_)
			return fail("source-closure.chunk-overlap", "manifest-chunk");
		if (control.chunk_index != next_chunk_index_ || control.offset != next_offset_)
			return fail("source-closure.chunk-order-invalid", "manifest-chunk");
		const auto remaining = declared_bytes_ - next_offset_;
		const auto expected_bytes = std::min(declared_chunk_bytes_, remaining);
		if (control.byte_count != payload.size() || payload.size() != expected_bytes ||
			payload.empty() || payload.size() > limits_.maximum_chunk_payload_bytes)
			return fail("source-closure.chunk-gap", "manifest-chunk");
		if (auto valid = sequence(sequence_value); !valid)
			return fail(valid.error().code, valid.error().field, valid.error().detail);
		if (auto valid = sink_->append_manifest(payload); !valid)
			return fail("source-closure.spool-io", "manifest-chunk", valid.error().detail);
		next_offset_ += payload.size();
		++next_chunk_index_;
		if (next_offset_ != declared_bytes_)
		{
			state_ = source_closure_transfer_state::manifest_streaming;
			return {};
		}
		if (next_chunk_index_ != declared_chunk_count_)
			return fail("source-closure.chunk-order-invalid", "manifest-census");
		auto summary = sink_->finish_manifest(binding_.manifest_digest);
		if (!summary)
			return fail("source-closure.manifest-invalid", "manifest", summary.error().detail);
		if (summary->closure_id != binding_.closure_id ||
			summary->closure_digest != binding_.closure_digest ||
			summary->manifest_digest != binding_.manifest_digest || summary->member_count == 0U ||
			summary->member_count > limits_.maximum_members || summary->blob_count == 0U ||
			summary->blob_count > limits_.maximum_unique_blobs ||
			summary->total_blob_bytes > limits_.maximum_unique_blob_bytes ||
			exceeds(manifest_bytes_, summary->total_blob_bytes, limits_.maximum_task_spool_bytes))
			return fail("source-closure.manifest-invalid", "manifest-census");
		manifest_member_count_ = summary->member_count;
		manifest_blob_count_ = summary->blob_count;
		manifest_total_blob_bytes_ = summary->total_blob_bytes;
		if (manifest_blob_count_ >
			limits_.maximum_resident_transport_bytes / sizeof(source_closure_blob_receipt))
			return fail("source-closure.limit-exceeded", "receipt-census", "resident-bound");
		try
		{
			blob_receipts_.reserve(static_cast<std::size_t>(manifest_blob_count_));
		}
		catch (const std::bad_alloc&)
		{
			return fail("source-closure.limit-exceeded", "receipt-census", "resident-bound");
		}
		state_ = source_closure_transfer_state::manifest_validated;
		return {};
	}

	sdk::result<void>
	source_closure_transfer_validator::begin_blob(const source_closure_blob_descriptor& descriptor,
												  const std::uint64_t sequence_value)
	{
		if (state_ != source_closure_transfer_state::manifest_validated &&
			state_ != source_closure_transfer_state::blob_sealed)
			return sdk::unexpected(failure("source-closure.protocol-state-invalid", "blob"));
		if (auto valid = ensure_identity(descriptor.session_id, descriptor.task_id); !valid)
			return fail(valid.error().code, valid.error().field, valid.error().detail);
		if (descriptor.closure_digest != binding_.closure_digest ||
			descriptor.blob_ordinal != completed_blobs_ ||
			descriptor.blob_ordinal >= manifest_blob_count_ ||
			!typed_digest(descriptor.blob_digest, content_prefix))
			return fail("source-closure.blob-order-invalid", "blob");
		if (descriptor.total_bytes > limits_.maximum_blob_bytes || descriptor.chunk_bytes == 0U ||
			descriptor.chunk_bytes > limits_.maximum_chunk_payload_bytes)
			return fail("source-closure.limit-exceeded", "blob");
		std::uint64_t expected_chunks{};
		if (!checked_chunk_count(descriptor.total_bytes, descriptor.chunk_bytes, expected_chunks) ||
			descriptor.chunk_count != expected_chunks ||
			descriptor.chunk_count > limits_.maximum_chunks_per_blob ||
			completed_blobs_ >= limits_.maximum_unique_blobs ||
			exceeds(total_blob_bytes_, descriptor.total_bytes, limits_.maximum_unique_blob_bytes) ||
			exceeds(
				blob_chunk_frames_, descriptor.chunk_count, limits_.maximum_blob_chunk_frames) ||
			exceeds_three(manifest_bytes_,
						  total_blob_bytes_,
						  descriptor.total_bytes,
						  limits_.maximum_task_spool_bytes))
			return fail("source-closure.limit-exceeded", "blob");
		if (descriptor.total_bytes == 0U && descriptor.blob_digest != empty_blob_digest)
			return fail("source-closure.digest-mismatch", "blob");
		if (auto valid = sequence(sequence_value); !valid)
			return fail(valid.error().code, valid.error().field, valid.error().detail);
		if (auto valid = sink_->begin_blob(descriptor); !valid)
			return fail("source-closure.spool-io", "blob", valid.error().detail);
		current_blob_ordinal_ = descriptor.blob_ordinal;
		current_blob_digest_ = descriptor.blob_digest;
		declared_bytes_ = descriptor.total_bytes;
		declared_chunk_bytes_ = descriptor.chunk_bytes;
		declared_chunk_count_ = descriptor.chunk_count;
		next_chunk_index_ = 0U;
		next_offset_ = 0U;
		sha_reset(
			blob_hash_state_, blob_hash_buffer_, blob_hash_buffer_size_, blob_hash_total_bytes_);
		if (descriptor.total_bytes == 0U)
		{
			const source_closure_blob_receipt receipt{
				descriptor.blob_ordinal, descriptor.blob_digest, 0U};
			if (auto valid = sink_->finish_blob(receipt); !valid)
				return fail("source-closure.spool-io", "blob", valid.error().detail);
			try
			{
				blob_receipts_.push_back(receipt);
			}
			catch (const std::bad_alloc&)
			{
				return fail("source-closure.limit-exceeded", "receipt-census", "resident-bound");
			}
			++completed_blobs_;
			state_ = source_closure_transfer_state::blob_sealed;
		}
		else
			state_ = source_closure_transfer_state::blob_open;
		return {};
	}

	sdk::result<void>
	source_closure_transfer_validator::blob_chunk(const source_closure_blob_chunk& control,
												  const std::span<const std::byte> payload,
												  const std::uint64_t sequence_value)
	{
		if (state_ != source_closure_transfer_state::blob_open &&
			state_ != source_closure_transfer_state::blob_streaming)
			return sdk::unexpected(failure("source-closure.protocol-state-invalid", "blob-chunk"));
		if (auto valid = ensure_identity(control.session_id, control.task_id); !valid)
			return fail(valid.error().code, valid.error().field, valid.error().detail);
		if (control.blob_ordinal != current_blob_ordinal_ ||
			control.blob_digest != current_blob_digest_)
			return fail("source-closure.blob-order-invalid", "blob-chunk");
		if (control.chunk_index < next_chunk_index_ || control.offset < next_offset_)
			return fail("source-closure.chunk-overlap", "blob-chunk");
		if (control.chunk_index != next_chunk_index_ || control.offset != next_offset_)
			return fail("source-closure.chunk-order-invalid", "blob-chunk");
		const auto remaining = declared_bytes_ - next_offset_;
		const auto expected_bytes = std::min(declared_chunk_bytes_, remaining);
		if (control.byte_count != payload.size() || payload.size() != expected_bytes ||
			payload.empty() || payload.size() > limits_.maximum_chunk_payload_bytes)
			return fail("source-closure.chunk-gap", "blob-chunk");
		if (blob_chunk_frames_ >= limits_.maximum_blob_chunk_frames)
			return fail("source-closure.limit-exceeded", "blob-chunk");
		if (auto valid = sequence(sequence_value); !valid)
			return fail(valid.error().code, valid.error().field, valid.error().detail);
		sha_update(blob_hash_state_,
				   blob_hash_buffer_,
				   blob_hash_buffer_size_,
				   blob_hash_total_bytes_,
				   payload);
		if (auto valid = sink_->append_blob(payload); !valid)
			return fail("source-closure.spool-io", "blob-chunk", valid.error().detail);
		next_offset_ += payload.size();
		++next_chunk_index_;
		++blob_chunk_frames_;
		if (next_offset_ != declared_bytes_)
		{
			state_ = source_closure_transfer_state::blob_streaming;
			return {};
		}
		if (next_chunk_index_ != declared_chunk_count_)
			return fail("source-closure.chunk-order-invalid", "blob-census");
		const auto observed_digest = sha_finish(
			blob_hash_state_, blob_hash_buffer_, blob_hash_buffer_size_, blob_hash_total_bytes_);
		if (observed_digest != current_blob_digest_)
			return fail("source-closure.digest-mismatch", "blob-content");
		const source_closure_blob_receipt receipt{
			current_blob_ordinal_, observed_digest, declared_bytes_};
		if (auto valid = sink_->finish_blob(receipt); !valid)
			return fail("source-closure.spool-io", "blob", valid.error().detail);
		try
		{
			blob_receipts_.push_back(receipt);
		}
		catch (const std::bad_alloc&)
		{
			return fail("source-closure.limit-exceeded", "receipt-census", "resident-bound");
		}
		++completed_blobs_;
		total_blob_bytes_ += declared_bytes_;
		state_ = source_closure_transfer_state::blob_sealed;
		return {};
	}

	sdk::result<void> source_closure_transfer_validator::seal(const source_closure_seal& value,
															  const std::uint64_t sequence_value)
	{
		if (state_ != source_closure_transfer_state::blob_sealed)
			return sdk::unexpected(failure("source-closure.protocol-state-invalid", "seal"));
		if (auto valid = ensure_identity(value.session_id, value.task_id); !valid)
			return fail(valid.error().code, valid.error().field, valid.error().detail);
		if (value.task_v4_digest != binding_.task_v4_digest ||
			value.manifest_digest != binding_.manifest_digest ||
			value.closure_digest != binding_.closure_digest ||
			completed_blobs_ != manifest_blob_count_ || value.blob_count != completed_blobs_ ||
			value.total_bytes != total_blob_bytes_ ||
			total_blob_bytes_ != manifest_total_blob_bytes_)
			return fail("source-closure.digest-mismatch", "seal-census");
		auto receipts = source_closure_blob_receipts_digest(blob_receipts_);
		if (!receipts || *receipts != value.blob_receipts_digest)
			return fail("source-closure.digest-mismatch", "blob-receipts");
		auto expected_transfer = source_closure_transfer_digest(
			binding_, value.blob_receipts_digest, value.blob_count, value.total_bytes);
		if (!expected_transfer || *expected_transfer != value.transfer_digest)
			return fail("source-closure.digest-mismatch", "transfer");
		if (auto valid = sequence(sequence_value); !valid)
			return fail(valid.error().code, valid.error().field, valid.error().detail);
		auto credentials = sink_->finish_closure(value.transfer_digest);
		if (!credentials)
			return fail("source-closure.spool-io", "closure", credentials.error().detail);
		if (!valid_spool_id(credentials->spool_receipt, "spool-receipt:semantic-v2:") ||
			!valid_spool_id(credentials->cleanup_owner, "cleanup-owner:semantic-v2:") ||
			credentials->transfer_digest != value.transfer_digest)
			return fail("source-closure.spool-io", "ack-credentials", "issuer-binding");
		transfer_digest_ = value.transfer_digest;
		ack_credentials_ = std::move(*credentials);
		state_ = source_closure_transfer_state::closure_sealed;
		return {};
	}

	sdk::result<void>
	source_closure_transfer_validator::acknowledge(const source_closure_ack& value,
												   const std::uint64_t sequence_value)
	{
		if (state_ != source_closure_transfer_state::closure_sealed)
			return sdk::unexpected(failure("source-closure.protocol-state-invalid", "ack"));
		if (auto valid = ensure_identity(value.session_id, value.task_id); !valid)
			return valid;
		if (value.closure_digest != binding_.closure_digest ||
			value.transfer_digest != transfer_digest_ ||
			value.spool_receipt != ack_credentials_.spool_receipt ||
			value.cleanup_owner != ack_credentials_.cleanup_owner)
			return sdk::unexpected(failure("source-closure.replay-invalid", "ack"));
		if (auto valid = sequence(sequence_value); !valid)
			return valid;
		state_ = source_closure_transfer_state::closure_acknowledged;
		return {};
	}

	sdk::result<void>
	source_closure_transfer_validator::validate_reject(const source_closure_reject& value) const
	{
		if (!valid_binding(binding_) || value.session_id != binding_.session_id ||
			value.task_id != binding_.task_id ||
			!valid_spool_id(value.cleanup_receipt, "cleanup-receipt:semantic-v2:"))
			return sdk::unexpected(failure("source-closure.task-binding-mismatch", "reject"));
		const auto phase = phase_name(state_);
		if (value.failure_phase != phase)
			return sdk::unexpected(
				failure("source-closure.protocol-state-invalid", "failure-phase"));
		const auto contract = phase_contract(phase);
		if (contract.reasons.empty() ||
			std::ranges::find(contract.reasons, value.reason_code) == contract.reasons.end())
			return sdk::unexpected(failure("source-closure.protocol-state-invalid", "reason-code"));
		if (value.observed_counters.size() != contract.counters.size())
			return sdk::unexpected(
				failure("source-closure.protocol-state-invalid", "observed-counters"));
		for (std::size_t index{}; index < value.observed_counters.size(); ++index)
		{
			if (value.observed_counters[index].first != contract.counters[index])
				return sdk::unexpected(
					failure("source-closure.protocol-state-invalid", "observed-counters"));
		}
		if (value.observed_counters != phase_counters())
			return sdk::unexpected(failure(
				"source-closure.protocol-state-invalid", "observed-counters", "phase-value"));
		return {};
	}

	sdk::result<void> source_closure_transfer_validator::reject(const source_closure_reject& value,
																const std::uint64_t sequence_value)
	{
		if (state_ == source_closure_transfer_state::closure_acknowledged ||
			state_ == source_closure_transfer_state::rejected ||
			state_ == source_closure_transfer_state::local_terminal)
			return sdk::unexpected(failure("source-closure.protocol-state-invalid", "reject"));
		if (auto valid = validate_reject(value); !valid)
			return valid;
		if (auto valid = sequence(sequence_value); !valid)
			return valid;
		auto cleanup = cleanup_once();
		if (!cleanup)
		{
			state_ = source_closure_transfer_state::rejected;
			return sdk::unexpected(
				failure("source-closure.cleanup-failed", "cleanup", cleanup.error().detail));
		}
		if (*cleanup != value.cleanup_receipt)
			return fail("source-closure.cleanup-failed", "cleanup-receipt", "issuer-binding");
		state_ = source_closure_transfer_state::rejected;
		return {};
	}

	std::vector<std::pair<std::string, std::uint64_t>>
	source_closure_transfer_validator::phase_counters() const
	{
		const auto phase = phase_name(state_);
		if (phase == "before-manifest")
			return {{"observed-control-frame-count", next_sequence_ - binding_.first_sequence}};
		if (phase == "manifest-streaming")
			return {{"declared-manifest-bytes", declared_bytes_},
					{"next-chunk-index", next_chunk_index_},
					{"received-manifest-bytes", next_offset_}};
		if (phase == "manifest-validated")
			return {{"blob-count", manifest_blob_count_},
					{"member-count", manifest_member_count_},
					{"total-blob-bytes", manifest_total_blob_bytes_}};
		if (phase == "blob-streaming")
			return {{"blob-ordinal", current_blob_ordinal_},
					{"declared-blob-bytes", declared_bytes_},
					{"next-chunk-index", next_chunk_index_},
					{"received-blob-bytes", next_offset_}};
		if (phase == "closure-sealed" || phase == "acknowledged")
			return {{"blob-count", completed_blobs_}, {"total-bytes", total_blob_bytes_}};
		return {};
	}

	sdk::result<source_closure_reject>
	source_closure_transfer_validator::make_terminal_reject(std::string reason)
	{
		if (state_ == source_closure_transfer_state::closure_acknowledged ||
			state_ == source_closure_transfer_state::rejected ||
			state_ == source_closure_transfer_state::local_terminal)
			return sdk::unexpected(failure("source-closure.protocol-state-invalid", "terminal"));
		const auto phase = phase_name(state_);
		const auto contract = phase_contract(phase);
		if (std::ranges::find(contract.reasons, reason) == contract.reasons.end())
			return sdk::unexpected(failure("source-closure.protocol-state-invalid", "reason-code"));
		auto cleanup = cleanup_once();
		if (!cleanup)
		{
			state_ = source_closure_transfer_state::rejected;
			return sdk::unexpected(
				failure("source-closure.cleanup-failed", "cleanup", cleanup.error().detail));
		}
		source_closure_reject value{binding_.session_id,
									binding_.task_id,
									phase,
									std::move(reason),
									phase_counters(),
									*cleanup};
		state_ = source_closure_transfer_state::rejected;
		return value;
	}

	sdk::result<source_closure_reject> source_closure_transfer_validator::cancel()
	{
		cancel_observed_ = true;
		return make_terminal_reject("source-closure.cancelled");
	}

	sdk::result<source_closure_reject> source_closure_transfer_validator::timeout()
	{
		return make_terminal_reject("source-closure.transfer-timeout");
	}

	sdk::result<void> source_closure_transfer_validator::connection_lost(const bool cancel_observed)
	{
		if (state_ == source_closure_transfer_state::closure_acknowledged ||
			state_ == source_closure_transfer_state::rejected ||
			state_ == source_closure_transfer_state::local_terminal)
			return sdk::unexpected(failure("source-closure.protocol-state-invalid", "connection"));
		cancel_observed_ = cancel_observed;
		auto cleanup = cleanup_once();
		if (!cleanup)
		{
			local_terminal_ = source_closure_local_terminal::connection_lost;
			state_ = source_closure_transfer_state::local_terminal;
			return sdk::unexpected(
				failure("source-closure.cleanup-failed", "cleanup", cleanup.error().detail));
		}
		local_terminal_ = source_closure_local_terminal::connection_lost;
		state_ = source_closure_transfer_state::local_terminal;
		return {};
	}

	sdk::result<void> source_closure_transfer_validator::worker_crashed(const bool cancel_observed)
	{
		if (state_ == source_closure_transfer_state::closure_acknowledged ||
			state_ == source_closure_transfer_state::rejected ||
			state_ == source_closure_transfer_state::local_terminal)
			return sdk::unexpected(failure("source-closure.protocol-state-invalid", "crash"));
		cancel_observed_ = cancel_observed;
		auto cleanup = cleanup_once();
		if (!cleanup)
		{
			local_terminal_ = source_closure_local_terminal::worker_crashed;
			state_ = source_closure_transfer_state::local_terminal;
			return sdk::unexpected(
				failure("source-closure.cleanup-failed", "cleanup", cleanup.error().detail));
		}
		local_terminal_ = source_closure_local_terminal::worker_crashed;
		state_ = source_closure_transfer_state::local_terminal;
		return {};
	}
} // namespace cxxlens::detail::clang22
