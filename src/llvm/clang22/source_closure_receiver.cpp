#include "source_closure_receiver.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "protocol_v2/closure.hpp"
#include "sdk/provider_protocol_v2_adapter.hpp"
#include "source_closure_spool.hpp"

namespace cxxlens::detail::clang22
{
	namespace
	{
		using sdk::provider::frame;
		using sdk::provider::message_type;
		namespace protocol = ::cxxlens::protocol_v2;

		constexpr std::size_t wire_header_bytes = protocol::fixed_header_bytes;

		[[nodiscard]] sdk::error
		failure(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
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
				   const sdk::provider::protocol_limits& limits,
				   const std::uint64_t expected_stream_id,
				   const std::uint64_t expected_sequence,
				   const std::uint64_t maximum_resident_bytes)
		{
			std::array<std::byte, wire_header_bytes> header{};
			auto eof = read_exact(source, header, true);
			if (!eof)
				return sdk::unexpected(std::move(eof.error()));
			if (*eof)
				return std::optional<frame>{};

			auto prepared =
				sdk::provider::detail::prepare_provider_protocol_v2_frame(header, limits);
			if (!prepared)
				return sdk::unexpected(std::move(prepared.error()));
			if (!sdk::provider::is_source_closure_message(prepared->type()))
				return sdk::unexpected(failure(
					"source-closure.protocol-state-invalid", "message-type", "closure-required"));
			if (auto valid = validate_source_closure_frame_header(
					static_cast<std::uint16_t>(prepared->type()), prepared->flags());
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			if (prepared->stream_id() != expected_stream_id)
				return sdk::unexpected(
					failure("source-closure.session-binding-mismatch", "stream-id"));
			if (prepared->sequence() != expected_sequence)
				return sdk::unexpected(
					failure("source-closure.protocol-state-invalid", "sequence", "unexpected"));
			if (prepared->body_resident_bytes() > maximum_resident_bytes)
				return sdk::unexpected(
					failure("source-closure.limit-exceeded", "frame-resident", "contract-cap"));
			try
			{
				std::vector<std::byte> control(prepared->control_bytes());
				if (control.capacity() > maximum_resident_bytes)
					return sdk::unexpected(failure(
						"source-closure.limit-exceeded", "frame-resident", "allocator-capacity"));
				const auto remaining_resident_bytes = maximum_resident_bytes - control.capacity();
				if (prepared->payload_bytes() > remaining_resident_bytes)
					return sdk::unexpected(
						failure("source-closure.limit-exceeded", "frame-resident", "contract-cap"));
				std::vector<std::byte> payload(prepared->payload_bytes());
				if (payload.capacity() > remaining_resident_bytes)
					return sdk::unexpected(failure(
						"source-closure.limit-exceeded", "frame-resident", "allocator-capacity"));
				if (auto complete = read_exact(source, control, false); !complete)
					return sdk::unexpected(std::move(complete.error()));
				if (auto complete = read_exact(source, payload, false); !complete)
					return sdk::unexpected(std::move(complete.error()));
				auto decoded =
					std::move(*prepared).finalize(std::move(control), std::move(payload));
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
			if (!source_closure_transport_limits_within_contract(limits))
				return sdk::unexpected(
					failure("source-closure.limit-exceeded", "limits", "contract-cap"));
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
			constexpr auto maximum_control_bytes =
				static_cast<std::uint64_t>(protocol::max_control_bytes);
			constexpr auto maximum_frame_overhead =
				maximum_control_bytes + static_cast<std::uint64_t>(wire_header_bytes);
			if (frame_count >
				(std::numeric_limits<std::uint64_t>::max() - limits.maximum_task_spool_bytes) /
					maximum_frame_overhead)
				return sdk::unexpected(
					failure("source-closure.limit-exceeded", "credit.bytes", "overflow"));
			return limits.maximum_task_spool_bytes + frame_count * maximum_frame_overhead;
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

		[[nodiscard]] sdk::result<void> cleanup(source_closure_spool_relay& relay)
		{
			auto receipt = relay.cleanup();
			if (!receipt)
				return sdk::unexpected(std::move(receipt.error()));
			return {};
		}

		[[nodiscard]] sdk::result<source_closure_receiver_result>
		fail_with_cleanup(source_closure_spool_relay& relay, sdk::error error)
		{
			if (auto cleaned = cleanup(relay); !cleaned)
				return sdk::unexpected(std::move(cleaned.error()));
			return sdk::unexpected(std::move(error));
		}

		[[nodiscard]] sdk::result<std::uint64_t> now_ns(const source_closure_monotonic_clock* clock)
		{
			if (clock != nullptr)
				return clock->now_ns();
			const auto now = std::chrono::steady_clock::now().time_since_epoch();
			const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
			if (count < 0)
				return sdk::unexpected(
					failure("source-closure.channel-clock-invalid", "clock", "negative"));
			return static_cast<std::uint64_t>(count);
		}

		[[nodiscard]] sdk::result<source_closure_receiver_result>
		fail_liveness(source_closure_spool_relay& relay,
					  source_closure_transfer_validator& validator,
					  sdk::error error)
		{
			const auto code = error.code;
			if (code == "source-closure.channel-cancelled")
			{
				auto rejected = validator.cancel();
				if (!rejected)
					return sdk::unexpected(std::move(rejected.error()));
				if (auto cleaned = relay.cleanup(); !cleaned)
					return sdk::unexpected(std::move(cleaned.error()));
				return sdk::unexpected(
					failure("source-closure.cancelled", std::move(error.field), "stop-requested"));
			}
			if (code == "source-closure.channel-timeout")
			{
				auto rejected = validator.timeout();
				if (!rejected)
					return sdk::unexpected(std::move(rejected.error()));
				if (auto cleaned = relay.cleanup(); !cleaned)
					return sdk::unexpected(std::move(cleaned.error()));
				return sdk::unexpected(failure("source-closure.transfer-timeout",
											   std::move(error.field),
											   "progress-deadline"));
			}
			if (code == "source-closure.channel-closed" ||
				code == "source-closure.channel-invalid" || code == "source-closure.channel-io" ||
				code == "source-closure.truncated-stream")
			{
				if (auto lost = validator.connection_lost(false); !lost)
					return sdk::unexpected(std::move(lost.error()));
				if (auto cleaned = relay.connection_lost(false); !cleaned)
					return sdk::unexpected(std::move(cleaned.error()));
				return sdk::unexpected(std::move(error));
			}
			return fail_with_cleanup(relay, std::move(error));
		}

		[[nodiscard]] sdk::result<void>
		accept_typed_frame_impl(frame value,
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
			protocol_frame.control = std::move(value.control);
			protocol_frame.payload = std::move(value.payload);
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
													protocol_frame.sequence);
				if (const auto* chunk =
						std::get_if<protocol::source_closure_manifest_chunk>(manifest);
					chunk != nullptr)
					return validator.manifest_chunk(
						manifest_chunk(*chunk), protocol_frame.payload, protocol_frame.sequence);
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
				return validator.begin_blob(blob_descriptor(*descriptor), protocol_frame.sequence);
			}
			if (type == message_type::source_closure_chunk)
			{
				const auto* chunk = std::get_if<protocol::source_closure_chunk>(&*decoded);
				if (chunk == nullptr)
					return sdk::unexpected(
						failure("source-closure.protocol-state-invalid", "blob-chunk", "variant"));
				return validator.blob_chunk(
					blob_chunk(*chunk), protocol_frame.payload, protocol_frame.sequence);
			}
			if (type == message_type::source_closure_seal)
			{
				const auto* value_seal = std::get_if<protocol::source_closure_seal>(&*decoded);
				if (value_seal == nullptr)
					return sdk::unexpected(
						failure("source-closure.protocol-state-invalid", "seal", "variant"));
				auto sealed_value = validator.seal(seal(*value_seal), protocol_frame.sequence);
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

		[[nodiscard]] sdk::result<void>
		accept_typed_frame(frame value,
						   source_closure_transfer_validator& validator,
						   protocol::closure_transfer& protocol_state,
						   const protocol::closure_limits& limits,
						   bool& sealed)
		{
			try
			{
				return accept_typed_frame_impl(
					std::move(value), validator, protocol_state, limits, sealed);
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(
					failure("source-closure.limit-exceeded", "frame-semantic", "allocation"));
			}
		}
	} // namespace

	sdk::result<source_closure_receiver_credit>
	source_closure_receiver_initial_credit(const source_closure_transport_limits& limits)
	{
		auto frames = checked_credit_frames(limits);
		if (!frames)
			return sdk::unexpected(std::move(frames.error()));
		auto bytes = checked_credit_bytes(limits, *frames);
		if (!bytes)
			return sdk::unexpected(std::move(bytes.error()));
		return source_closure_receiver_credit{*bytes, *frames};
	}

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
		auto initial_credit = source_closure_receiver_initial_credit(options.limits);
		if (!initial_credit)
			return sdk::unexpected(std::move(initial_credit.error()));
		if (initial_credit->frames > options.maximum_frames)
			return sdk::unexpected(
				failure("source-closure.limit-exceeded", "receiver-options", "frame-credit"));
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
		session.initial_credit = {initial_credit->bytes, initial_credit->frames};
		session.limits = *closure_limits;
		auto protocol_state = protocol::closure_transfer::create(std::move(session));
		if (!protocol_state)
			return sdk::unexpected(std::move(protocol_state.error()));

		auto relay = std::make_shared<source_closure_spool_relay>(options.limits);
		auto& spool = relay->sink();
		source_closure_transfer_validator validator{
			options.binding, *options.authority, spool, options.limits};
		sdk::provider::protocol_limits wire_limits;
		wire_limits.max_payload_bytes = std::min<std::uint64_t>(
			options.limits.maximum_chunk_payload_bytes, std::numeric_limits<std::uint64_t>::max());
		if (wire_limits.max_payload_bytes > std::numeric_limits<std::size_t>::max())
			return fail_with_cleanup(
				*relay, failure("source-closure.limit-exceeded", "limits", "payload-size"));
		wire_limits.max_payload_bytes = std::min<std::uint64_t>(
			wire_limits.max_payload_bytes, sdk::provider::protocol_limits{}.max_payload_bytes);

		std::uint64_t frame_count{};
		auto last_progress = now_ns(options.clock);
		if (!last_progress)
			return fail_with_cleanup(*relay,
									 failure("source-closure.channel-clock-invalid",
											 "clock",
											 last_progress.error().detail));
		for (;;)
		{
			if (options.cancellation.stop_requested())
				return fail_liveness(
					*relay,
					validator,
					failure("source-closure.channel-cancelled", "transfer", "stop-requested"));
			if (options.progress_timeout_ns != 0U)
			{
				auto current = now_ns(options.clock);
				if (!current)
					return fail_with_cleanup(*relay,
											 failure("source-closure.channel-clock-invalid",
													 "clock",
													 current.error().detail));
				if (*current < *last_progress)
					return fail_with_cleanup(
						*relay,
						failure("source-closure.channel-clock-invalid", "clock", "backwards"));
				if (*current - *last_progress >= options.progress_timeout_ns)
					return fail_liveness(
						*relay,
						validator,
						failure("source-closure.channel-timeout", "transfer", "progress-deadline"));
			}
			if (frame_count >= options.maximum_frames)
				return fail_with_cleanup(
					*relay, failure("source-closure.limit-exceeded", "frames", "maximum"));
			auto next = read_frame(source,
								   wire_limits,
								   options.stream_id,
								   validator.next_sequence(),
								   options.limits.maximum_resident_transport_bytes);
			if (!next)
				return fail_liveness(*relay, validator, std::move(next.error()));
			if (!next->has_value())
				return fail_liveness(*relay,
									 validator,
									 failure("source-closure.truncated-stream", "ack", "missing"));
			++frame_count;
			auto current = now_ns(options.clock);
			if (!current)
				return fail_with_cleanup(*relay,
										 failure("source-closure.channel-clock-invalid",
												 "clock",
												 current.error().detail));
			if (*current < *last_progress)
				return fail_with_cleanup(
					*relay, failure("source-closure.channel-clock-invalid", "clock", "backwards"));
			last_progress = *current;
			bool sealed{};
			auto accepted = accept_typed_frame(
				std::move(**next), validator, *protocol_state, *closure_limits, sealed);
			if (!accepted)
				return fail_with_cleanup(*relay, std::move(accepted.error()));
			if (!sealed)
				continue;
			if (options.cancellation.stop_requested())
				return fail_liveness(
					*relay,
					validator,
					failure("source-closure.channel-cancelled", "transfer", "stop-requested"));
			auto credentials = spool.ack_credentials();
			if (!credentials)
				return fail_with_cleanup(*relay, std::move(credentials.error()));
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
				return fail_with_cleanup(*relay, std::move(ack_control.error()));
			frame ack_frame;
			ack_frame.type = message_type::source_closure_ack;
			ack_frame.stream_id = options.stream_id;
			ack_frame.sequence = validator.next_sequence();
			ack_frame.control = std::move(*ack_control);
			auto encoded_ack = sdk::provider::encode_frame(ack_frame, wire_limits);
			if (!encoded_ack)
				return fail_with_cleanup(*relay, std::move(encoded_ack.error()));
			if (auto emitted = sink.write(*encoded_ack); !emitted)
				return fail_liveness(*relay, validator, std::move(emitted.error()));
			auto snapshot = spool.snapshot();
			if (!snapshot)
				return fail_with_cleanup(*relay, std::move(snapshot.error()));
			if (auto marked = relay->mark_sealed(); !marked)
				return fail_with_cleanup(*relay, std::move(marked.error()));
			const auto transfer_digest = credentials->transfer_digest;
			auto credentials_value = std::move(*credentials);
			return source_closure_receiver_result{std::move(*snapshot),
												  std::move(credentials_value),
												  transfer_digest,
												  std::move(relay)};
		}
	}
} // namespace cxxlens::detail::clang22
