#include "source_closure_receiver.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "protocol_v2/closure.hpp"
#include "source_closure_spool.hpp"

namespace cxxlens::detail::clang22
{
	namespace
	{
		using sdk::provider::frame;
		using sdk::provider::message_type;
		namespace protocol = ::cxxlens::protocol_v2;

		constexpr std::size_t wire_header_bytes = 104U;

		[[nodiscard]] sdk::error
		failure(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] std::uint32_t read_u32(const std::span<const std::byte> bytes,
											 const std::size_t offset) noexcept
		{
			return (std::to_integer<std::uint32_t>(bytes[offset]) << 24U) |
				(std::to_integer<std::uint32_t>(bytes[offset + 1U]) << 16U) |
				(std::to_integer<std::uint32_t>(bytes[offset + 2U]) << 8U) |
				std::to_integer<std::uint32_t>(bytes[offset + 3U]);
		}

		[[nodiscard]] std::uint64_t read_u64(const std::span<const std::byte> bytes,
											 const std::size_t offset) noexcept
		{
			std::uint64_t value{};
			for (std::size_t index{}; index < sizeof(value); ++index)
				value = (value << 8U) | std::to_integer<std::uint64_t>(bytes[offset + index]);
			return value;
		}

		[[nodiscard]] sdk::result<bool> read_exact(source_closure_frame_source& source,
												   const std::span<std::byte> destination,
												   const bool clean_eof_allowed)
		{
			std::size_t received{};
			while (received < destination.size())
			{
				auto count = source.read(destination.subspan(received));
				if (!count)
					return sdk::unexpected(std::move(count.error()));
				if (*count > destination.size() - received)
					return sdk::unexpected(failure(
						"source-closure.transport-invalid", "frame-source", "source-overread"));
				if (*count == 0U)
				{
					if (received == 0U && clean_eof_allowed)
						return true;
					return sdk::unexpected(
						failure("source-closure.truncated-stream", "frame", "incomplete-frame"));
				}
				received += *count;
			}
			return false;
		}

		[[nodiscard]] sdk::result<std::optional<frame>>
		read_frame(source_closure_frame_source& source,
				   const sdk::provider::protocol_limits& limits)
		{
			std::array<std::byte, wire_header_bytes> header{};
			auto eof = read_exact(source, header, true);
			if (!eof)
				return sdk::unexpected(std::move(eof.error()));
			if (*eof)
				return std::optional<frame>{};

			const auto control_bytes = read_u32(header, 28U);
			const auto payload_bytes = read_u64(header, 32U);
			if (control_bytes > limits.max_control_bytes ||
				payload_bytes > limits.max_payload_bytes ||
				payload_bytes >
					std::numeric_limits<std::size_t>::max() - wire_header_bytes - control_bytes)
				return sdk::unexpected(
					failure("source-closure.limit-exceeded", "frame", "wire-length"));
			const auto total_bytes = wire_header_bytes + static_cast<std::size_t>(control_bytes) +
				static_cast<std::size_t>(payload_bytes);
			try
			{
				std::vector<std::byte> encoded(total_bytes);
				std::ranges::copy(header, encoded.begin());
				auto complete =
					read_exact(source, std::span{encoded}.subspan(wire_header_bytes), false);
				if (!complete)
					return sdk::unexpected(std::move(complete.error()));
				auto decoded = sdk::provider::decode_frame(encoded, limits);
				if (!decoded)
					return sdk::unexpected(std::move(decoded.error()));
				return std::optional<frame>{std::move(*decoded)};
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(
					failure("source-closure.limit-exceeded", "frame", "allocation"));
			}
		}

		[[nodiscard]] sdk::result<protocol::closure_limits>
		protocol_limits(const source_closure_transport_limits& limits)
		{
			const auto fits = [](const std::uint64_t value) noexcept
			{
				return value <= std::numeric_limits<std::size_t>::max();
			};
			if (!fits(limits.maximum_members) || !fits(limits.maximum_unique_blobs) ||
				!fits(limits.maximum_logical_path_bytes) || !fits(limits.maximum_blob_bytes) ||
				!fits(limits.maximum_unique_blob_bytes) || !fits(limits.maximum_manifest_bytes) ||
				!fits(limits.maximum_manifest_chunks) ||
				!fits(limits.maximum_chunk_payload_bytes) ||
				!fits(limits.maximum_chunks_per_blob) || !fits(limits.maximum_blob_chunk_frames) ||
				!fits(limits.maximum_task_spool_bytes) ||
				!fits(limits.maximum_resident_transport_bytes))
				return sdk::unexpected(
					failure("source-closure.limit-exceeded", "limits", "size-type"));
			protocol::closure_limits output;
			output.maximum_members = static_cast<std::size_t>(limits.maximum_members);
			output.maximum_blobs = static_cast<std::size_t>(limits.maximum_unique_blobs);
			output.maximum_manifest_bytes = static_cast<std::size_t>(limits.maximum_manifest_bytes);
			output.maximum_blob_bytes = static_cast<std::size_t>(limits.maximum_blob_bytes);
			output.maximum_unique_blob_bytes =
				static_cast<std::size_t>(limits.maximum_unique_blob_bytes);
			output.maximum_chunk_payload_bytes =
				static_cast<std::size_t>(limits.maximum_chunk_payload_bytes);
			output.maximum_manifest_chunks =
				static_cast<std::size_t>(limits.maximum_manifest_chunks);
			output.maximum_chunks_per_blob =
				static_cast<std::size_t>(limits.maximum_chunks_per_blob);
			output.maximum_blob_chunk_frames =
				static_cast<std::size_t>(limits.maximum_blob_chunk_frames);
			output.maximum_task_spool_bytes =
				static_cast<std::size_t>(limits.maximum_task_spool_bytes);
			output.maximum_resident_transport_bytes =
				static_cast<std::size_t>(limits.maximum_resident_transport_bytes);
			return output;
		}

		[[nodiscard]] sdk::result<std::uint64_t>
		checked_credit_frames(const source_closure_transport_limits& limits)
		{
			std::uint64_t output = 2U;
			for (const auto value : {limits.maximum_manifest_chunks,
									 limits.maximum_unique_blobs,
									 limits.maximum_blob_chunk_frames})
			{
				if (output > std::numeric_limits<std::uint64_t>::max() - value)
					return sdk::unexpected(
						failure("source-closure.limit-exceeded", "credit.frames", "overflow"));
				output += value;
			}
			return output;
		}

		[[nodiscard]] sdk::result<std::uint64_t>
		checked_credit_bytes(const source_closure_transport_limits& limits,
							 const std::uint64_t frame_count)
		{
			constexpr std::uint64_t maximum_control_bytes = 65'536U;
			if (frame_count >
				(std::numeric_limits<std::uint64_t>::max() - limits.maximum_task_spool_bytes) /
					maximum_control_bytes)
				return sdk::unexpected(
					failure("source-closure.limit-exceeded", "credit.bytes", "overflow"));
			return limits.maximum_task_spool_bytes + frame_count * maximum_control_bytes;
		}

		[[nodiscard]] source_closure_manifest_descriptor
		manifest_descriptor(const protocol::source_closure_manifest_descriptor& value)
		{
			return {value.session_id,
					value.task_id,
					value.task_v4_digest,
					value.closure_id,
					value.closure_digest,
					value.manifest_digest,
					value.total_bytes,
					value.chunk_bytes,
					value.chunk_count};
		}

		[[nodiscard]] source_closure_manifest_chunk
		manifest_chunk(const protocol::source_closure_manifest_chunk& value)
		{
			return {value.session_id,
					value.task_id,
					value.manifest_digest,
					value.chunk_index,
					value.offset,
					value.byte_count};
		}

		[[nodiscard]] source_closure_blob_descriptor
		blob_descriptor(const protocol::source_closure_blob_descriptor& value)
		{
			return {value.session_id,
					value.task_id,
					value.closure_digest,
					value.blob_ordinal,
					value.blob_digest,
					value.total_bytes,
					value.chunk_bytes,
					value.chunk_count};
		}

		[[nodiscard]] source_closure_blob_chunk
		blob_chunk(const protocol::source_closure_chunk& value)
		{
			return {value.session_id,
					value.task_id,
					value.blob_ordinal,
					value.blob_digest,
					value.chunk_index,
					value.offset,
					value.byte_count};
		}

		[[nodiscard]] source_closure_seal seal(const protocol::source_closure_seal& value)
		{
			return {value.session_id,
					value.task_id,
					value.task_v4_digest,
					value.manifest_digest,
					value.blob_receipts_digest,
					value.blob_count,
					value.total_bytes,
					value.closure_digest,
					value.transfer_digest};
		}

		[[nodiscard]] sdk::result<void> cleanup(source_closure_spool& spool)
		{
			auto receipt = spool.cleanup();
			if (!receipt)
				return sdk::unexpected(std::move(receipt.error()));
			return {};
		}

		[[nodiscard]] sdk::result<source_closure_receiver_result>
		fail_with_cleanup(source_closure_spool& spool, sdk::error error)
		{
			if (auto cleaned = cleanup(spool); !cleaned)
				return sdk::unexpected(std::move(cleaned.error()));
			return sdk::unexpected(std::move(error));
		}

		[[nodiscard]] sdk::result<void>
		accept_typed_frame(const frame& value,
						   source_closure_transfer_validator& validator,
						   protocol::closure_transfer& protocol_state,
						   const protocol::closure_limits& limits,
						   bool& sealed)
		{
			if (!sdk::provider::is_source_closure_message(value.type))
				return sdk::unexpected(failure(
					"source-closure.protocol-state-invalid", "message-type", "closure-required"));
			if (auto valid = validate_source_closure_frame_header(
					static_cast<std::uint16_t>(value.type), value.flags);
				!valid)
				return valid;
			const auto protocol_message =
				static_cast<protocol::message_type>(static_cast<std::uint16_t>(value.type));
			auto decoded =
				protocol::decode_closure_control(protocol_message, value.control, limits);
			if (!decoded)
				return sdk::unexpected(std::move(decoded.error()));
			if (auto valid = protocol::validate_closure_payload(
					protocol_message, *decoded, value.payload, limits);
				!valid)
				return valid;
			protocol::frame protocol_frame;
			protocol_frame.type = protocol_message;
			protocol_frame.protocol_major = value.protocol_major;
			protocol_frame.protocol_minor = value.protocol_minor;
			protocol_frame.flags = value.flags;
			protocol_frame.stream_id = value.stream_id;
			protocol_frame.sequence = value.sequence;
			protocol_frame.control = value.control;
			protocol_frame.payload = value.payload;
			if (auto valid = protocol_state.accept(protocol_frame); !valid)
				return valid;

			const auto type = value.type;
			if (type == message_type::source_closure_manifest)
			{
				const auto* manifest = std::get_if<protocol::source_closure_manifest>(&*decoded);
				if (manifest == nullptr)
					return sdk::unexpected(
						failure("source-closure.protocol-state-invalid", "manifest", "type"));
				if (const auto* descriptor =
						std::get_if<protocol::source_closure_manifest_descriptor>(manifest);
					descriptor != nullptr)
					return validator.begin_manifest(manifest_descriptor(*descriptor),
													value.sequence);
				if (const auto* chunk =
						std::get_if<protocol::source_closure_manifest_chunk>(manifest);
					chunk != nullptr)
					return validator.manifest_chunk(
						manifest_chunk(*chunk), value.payload, value.sequence);
				return sdk::unexpected(
					failure("source-closure.protocol-state-invalid", "manifest", "variant"));
			}
			if (type == message_type::source_closure_blob)
			{
				const auto* descriptor =
					std::get_if<protocol::source_closure_blob_descriptor>(&*decoded);
				if (descriptor == nullptr)
					return sdk::unexpected(
						failure("source-closure.protocol-state-invalid", "blob", "variant"));
				return validator.begin_blob(blob_descriptor(*descriptor), value.sequence);
			}
			if (type == message_type::source_closure_chunk)
			{
				const auto* chunk = std::get_if<protocol::source_closure_chunk>(&*decoded);
				if (chunk == nullptr)
					return sdk::unexpected(
						failure("source-closure.protocol-state-invalid", "blob-chunk", "variant"));
				return validator.blob_chunk(blob_chunk(*chunk), value.payload, value.sequence);
			}
			if (type == message_type::source_closure_seal)
			{
				const auto* value_seal = std::get_if<protocol::source_closure_seal>(&*decoded);
				if (value_seal == nullptr)
					return sdk::unexpected(
						failure("source-closure.protocol-state-invalid", "seal", "variant"));
				auto sealed_value = validator.seal(seal(*value_seal), value.sequence);
				if (sealed_value)
					sealed = true;
				return sealed_value;
			}
			if (type == message_type::source_closure_ack)
				return sdk::unexpected(
					failure("source-closure.protocol-state-invalid", "ack", "receiver-owns-ack"));

			const auto* value_reject = std::get_if<protocol::source_closure_reject>(&*decoded);
			if (value_reject == nullptr)
				return sdk::unexpected(
					failure("source-closure.protocol-state-invalid", "reject", "variant"));
			return sdk::unexpected(
				failure("source-closure.remote-reject", "reject", value_reject->reason_code));
		}
	} // namespace

	sdk::result<source_closure_receiver_result>
	receive_source_closure_frames(source_closure_frame_source& source,
								  source_closure_frame_sink& sink,
								  source_closure_receiver_options options)
	{
		if (options.authority == nullptr)
			return sdk::unexpected(
				failure("source-closure.worker-input-invalid", "authority", "missing"));
		if (options.stream_id == 0U || options.maximum_frames == 0U ||
			options.binding.first_sequence != 0U)
			return sdk::unexpected(
				failure("source-closure.protocol-state-invalid", "receiver-options", "sequence"));
		auto closure_limits = protocol_limits(options.limits);
		if (!closure_limits)
			return sdk::unexpected(std::move(closure_limits.error()));
		auto frame_credit = checked_credit_frames(options.limits);
		if (!frame_credit || *frame_credit > options.maximum_frames)
			return sdk::unexpected(
				failure("source-closure.limit-exceeded", "receiver-options", "frame-credit"));
		auto byte_credit = checked_credit_bytes(options.limits, *frame_credit);
		if (!byte_credit)
			return sdk::unexpected(std::move(byte_credit.error()));
		if (!options.binding.task_v4_digest.starts_with("semantic-v2:sha256:") ||
			!options.binding.closure_digest.starts_with("semantic-v2:sha256:") ||
			!options.binding.manifest_digest.starts_with("semantic-v2:sha256:"))
			return sdk::unexpected(
				failure("source-closure.task-binding-mismatch", "receiver-options", "digest"));

		protocol::closure_session session;
		session.session_id = options.binding.session_id;
		session.task_id = options.binding.task_id;
		session.task_v4_digest = options.binding.task_v4_digest;
		session.closure_digest = options.binding.closure_digest;
		session.manifest_digest = options.binding.manifest_digest;
		session.stream_id = options.stream_id;
		session.initial_credit = {*byte_credit, *frame_credit};
		session.limits = *closure_limits;
		auto protocol_state = protocol::closure_transfer::create(std::move(session));
		if (!protocol_state)
			return sdk::unexpected(std::move(protocol_state.error()));

		source_closure_spool spool{options.limits};
		source_closure_transfer_validator validator{
			options.binding, *options.authority, spool, options.limits};
		sdk::provider::protocol_limits wire_limits;
		wire_limits.max_payload_bytes = std::min<std::uint64_t>(
			options.limits.maximum_chunk_payload_bytes, std::numeric_limits<std::uint64_t>::max());
		if (wire_limits.max_payload_bytes > std::numeric_limits<std::size_t>::max())
			return fail_with_cleanup(
				spool, failure("source-closure.limit-exceeded", "limits", "payload-size"));
		wire_limits.max_payload_bytes = std::min<std::uint64_t>(
			wire_limits.max_payload_bytes, sdk::provider::protocol_limits{}.max_payload_bytes);

		std::uint64_t frame_count{};
		for (;;)
		{
			if (frame_count >= options.maximum_frames)
				return fail_with_cleanup(
					spool, failure("source-closure.limit-exceeded", "frames", "maximum"));
			auto next = read_frame(source, wire_limits);
			if (!next)
				return fail_with_cleanup(spool, std::move(next.error()));
			if (!next->has_value())
				return fail_with_cleanup(
					spool, failure("source-closure.truncated-stream", "ack", "missing"));
			++frame_count;
			const auto& value = **next;
			if (value.stream_id != options.stream_id)
				return fail_with_cleanup(
					spool, failure("source-closure.session-binding-mismatch", "stream-id"));
			bool sealed{};
			auto accepted =
				accept_typed_frame(**next, validator, *protocol_state, *closure_limits, sealed);
			if (!accepted)
				return fail_with_cleanup(spool, std::move(accepted.error()));
			if (!sealed)
				continue;
			auto credentials = spool.ack_credentials();
			if (!credentials)
				return fail_with_cleanup(spool, std::move(credentials.error()));
			protocol::source_closure_ack ack_value{options.binding.session_id,
												   options.binding.task_id,
												   options.binding.closure_digest,
												   credentials->transfer_digest,
												   credentials->spool_receipt,
												   credentials->cleanup_owner};
			auto ack_control =
				protocol::encode_closure_control(protocol::message_type::source_closure_ack,
												 protocol::closure_control{std::move(ack_value)},
												 *closure_limits);
			if (!ack_control)
				return fail_with_cleanup(spool, std::move(ack_control.error()));
			frame ack_frame;
			ack_frame.type = message_type::source_closure_ack;
			ack_frame.stream_id = options.stream_id;
			ack_frame.sequence = validator.next_sequence();
			ack_frame.control = std::move(*ack_control);
			auto encoded_ack = sdk::provider::encode_frame(ack_frame, wire_limits);
			if (!encoded_ack)
				return fail_with_cleanup(spool, std::move(encoded_ack.error()));
			if (auto emitted = sink.write(*encoded_ack); !emitted)
				return fail_with_cleanup(spool, std::move(emitted.error()));
			auto snapshot = spool.snapshot();
			if (!snapshot)
				return fail_with_cleanup(spool, std::move(snapshot.error()));
			const auto transfer_digest = credentials->transfer_digest;
			auto credentials_value = std::move(*credentials);
			return source_closure_receiver_result{
				std::move(*snapshot), std::move(credentials_value), transfer_digest};
		}
	}
} // namespace cxxlens::detail::clang22
