#include "sdk/provider_protocol_v2_adapter.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <type_traits>

#include "sdk/provider_runtime_internal.hpp"
#include "sdk/provider_validation_internal.hpp"

namespace
{
	using namespace cxxlens::sdk::provider;
	using cxxlens::sdk::provider::detail::encode_provider_protocol_v2_closure_control;
	using cxxlens::sdk::provider::detail::encode_provider_protocol_v2_heartbeat_control;
	using cxxlens::sdk::provider::detail::provider_protocol_v2_ack;
	using cxxlens::sdk::provider::detail::provider_protocol_v2_blob;
	using cxxlens::sdk::provider::detail::provider_protocol_v2_chunk;
	using cxxlens::sdk::provider::detail::provider_protocol_v2_closure_state;
	using cxxlens::sdk::provider::detail::provider_protocol_v2_control;
	using cxxlens::sdk::provider::detail::provider_protocol_v2_heartbeat_control;
	using cxxlens::sdk::provider::detail::provider_protocol_v2_heartbeat_kind;
	using cxxlens::sdk::provider::detail::provider_protocol_v2_manifest;
	using cxxlens::sdk::provider::detail::provider_protocol_v2_manifest_chunk;
	using cxxlens::sdk::provider::detail::provider_protocol_v2_manifest_descriptor;
	using cxxlens::sdk::provider::detail::provider_protocol_v2_manifest_kind;
	using cxxlens::sdk::provider::detail::provider_protocol_v2_phase;
	using cxxlens::sdk::provider::detail::provider_protocol_v2_reject;
	using cxxlens::sdk::provider::detail::provider_protocol_v2_seal;
	using cxxlens::sdk::provider::detail::provider_protocol_v2_session;
	using cxxlens::sdk::provider::detail::provider_protocol_v2_session_binding;
	using cxxlens::sdk::provider::detail::provider_runtime_closure_channel;
	using cxxlens::sdk::provider::detail::validate_provider_protocol_v2_heartbeat;

	void require(const bool value, const std::string_view message)
	{
		if (!value)
		{
			std::cerr << "FAIL: " << message << '\n';
			std::exit(1);
		}
	}

	std::string semantic(const char fill)
	{
		return "semantic-v2:sha256:" + std::string(64U, fill);
	}

	void test_single_codec_facade()
	{
		static_assert(!std::is_copy_constructible_v<
					  cxxlens::sdk::provider::detail::prepared_provider_protocol_v2_frame>);
		frame optional{static_cast<message_type>(65'000U),
					   7U,
					   11U,
					   {std::byte{0x61}, std::byte{'a'}},
					   {std::byte{'p'}},
					   protocol_v2_major,
					   protocol_v2_minor,
					   static_cast<std::uint16_t>(frame_flag::optional_extension)};
		auto public_wire = encode_frame(optional);
		require(public_wire.has_value(), "public facade encoded canonical unknown optional frame");

		cxxlens::protocol_v2::frame native;
		native.type = static_cast<cxxlens::protocol_v2::message_type>(65'000U);
		native.stream_id = optional.stream_id;
		native.sequence = optional.sequence;
		native.control = optional.control;
		native.payload = optional.payload;
		native.flags = cxxlens::protocol_v2::flag_optional_extension;
		auto native_wire = cxxlens::protocol_v2::encode_frame(native);
		require(native_wire && *native_wire == *public_wire,
				"public facade wire bytes diverged from the sole Protocol 2 codec");

		auto large = optional;
		large.payload.assign(1'048'576U, std::byte{0x5a});
		auto large_wire = encode_frame(large);
		require(large_wire.has_value(), "prepared public facade 1 MiB frame encoding");
		std::array<std::byte, cxxlens::protocol_v2::fixed_header_bytes> prepared_header{};
		std::copy_n(large_wire->begin(), prepared_header.size(), prepared_header.begin());
		auto prepared =
			cxxlens::sdk::provider::detail::prepare_provider_protocol_v2_frame(prepared_header, {});
		require(
			prepared && prepared->control_bytes() == large.control.size() &&
				prepared->payload_bytes() == large.payload.size() &&
				prepared->body_resident_bytes() == large.control.size() + large.payload.size() &&
				prepared->type() == large.type && prepared->flags() == large.flags &&
				prepared->stream_id() == large.stream_id && prepared->sequence() == large.sequence,
			"prepared public facade body accounting");
		std::vector<std::byte> prepared_control(
			large_wire->begin() + static_cast<std::ptrdiff_t>(prepared_header.size()),
			large_wire->begin() +
				static_cast<std::ptrdiff_t>(prepared_header.size() + large.control.size()));
		std::vector<std::byte> prepared_payload(
			large_wire->begin() +
				static_cast<std::ptrdiff_t>(prepared_header.size() + large.control.size()),
			large_wire->end());
		const auto* const prepared_payload_allocation = prepared_payload.data();
		auto prepared_decoded =
			std::move(*prepared).finalize(std::move(prepared_control), std::move(prepared_payload));
		require(prepared_decoded &&
					prepared_decoded->payload.data() == prepared_payload_allocation &&
					prepared_decoded->payload == large.payload,
				"prepared public facade copied the caller-owned 1 MiB payload");
		prepared_control = large.control;
		prepared_payload = large.payload;
		auto replayed_finalize =
			std::move(*prepared).finalize(std::move(prepared_control), std::move(prepared_payload));
		require(!replayed_finalize,
				"prepared public facade allowed the one-shot token to be replayed");

		auto tampered_prepared =
			cxxlens::sdk::provider::detail::prepare_provider_protocol_v2_frame(prepared_header, {});
		require(tampered_prepared.has_value(), "prepared public tamper setup");
		prepared_control = large.control;
		prepared_payload = large.payload;
		prepared_payload.back() ^= std::byte{0x01};
		auto tampered_prepared_result =
			std::move(*tampered_prepared)
				.finalize(std::move(prepared_control), std::move(prepared_payload));
		require(!tampered_prepared_result &&
					tampered_prepared_result.error().code == "provider.checksum-mismatch",
				"prepared public facade accepted a tampered body");

		auto invalid_prepared_header = prepared_header;
		invalid_prepared_header[8U] = std::byte{};
		invalid_prepared_header[9U] = std::byte{0x01};
		auto invalid_prepared = cxxlens::sdk::provider::detail::prepare_provider_protocol_v2_frame(
			invalid_prepared_header, {});
		require(!invalid_prepared &&
					invalid_prepared.error().code == "provider.invalid-frame-flags",
				"prepared public facade accepted optional flag on a known message");
		auto short_prepared =
			cxxlens::sdk::provider::detail::prepare_provider_protocol_v2_frame(prepared_header, {});
		require(short_prepared.has_value(), "prepared public short-body setup");
		prepared_control = large.control;
		prepared_payload = large.payload;
		prepared_payload.pop_back();
		auto short_prepared_result =
			std::move(*short_prepared)
				.finalize(std::move(prepared_control), std::move(prepared_payload));
		require(!short_prepared_result &&
					short_prepared_result.error().code == "provider.truncated-stream",
				"prepared public facade accepted a short body");

		auto decoded = decode_frame(*public_wire);
		auto decoded_stream = decode_frame_stream(*public_wire);
		require(decoded && static_cast<std::uint16_t>(decoded->type) == 65'000U &&
					decoded->stream_id == optional.stream_id &&
					decoded->sequence == optional.sequence &&
					decoded->control == optional.control && decoded->payload == optional.payload,
				"public facade did not retain unknown optional accounting fields");
		require(decoded_stream && decoded_stream->size() == 1U &&
					static_cast<std::uint16_t>(decoded_stream->front().type) == 65'000U,
				"public stream facade did not preserve unknown optional skip accounting");
		auto zero_frame_limit = decode_frame_stream(*public_wire, {}, 0U);
		require(!zero_frame_limit && zero_frame_limit.error().code == "provider.stream-invalid",
				"public facade changed the zero transcript-frame limit reason");
		zero_frame_limit = decode_frame_stream({}, {}, 0U);
		require(!zero_frame_limit && zero_frame_limit.error().code == "provider.stream-invalid",
				"empty input bypassed the zero transcript-frame limit reason");

		auto tampered = *public_wire;
		tampered.back() ^= std::byte{0x01};
		auto tamper_failure = decode_frame(tampered);
		require(!tamper_failure && tamper_failure.error().code == "provider.checksum-mismatch",
				"public facade skipped checksum validation for unknown optional frame");

		auto noncanonical = *public_wire;
		noncanonical[cxxlens::protocol_v2::fixed_header_bytes] = std::byte{0x78};
		noncanonical[cxxlens::protocol_v2::fixed_header_bytes + 1U] = std::byte{0x00};
		const auto digest =
			cxxlens::protocol_v2::sha256(std::span<const std::byte>{noncanonical}.subspan(
				cxxlens::protocol_v2::fixed_header_bytes, 2U));
		std::copy(digest.begin(), digest.end(), noncanonical.begin() + 40);
		auto direct_failure = decode_frame(noncanonical);
		auto stream_failure = decode_frame_stream(noncanonical);
		require(!direct_failure && !stream_failure &&
					direct_failure.error() == stream_failure.error() &&
					direct_failure.error().code == "provider.malformed-frame",
				"public direct/stream facade did not share the canonical-CBOR rejection reason");
		std::copy_n(noncanonical.begin(), prepared_header.size(), prepared_header.begin());
		auto noncanonical_prepared =
			cxxlens::sdk::provider::detail::prepare_provider_protocol_v2_frame(prepared_header, {});
		require(noncanonical_prepared.has_value(),
				"noncanonical prepared public header validation");
		prepared_control.assign(
			noncanonical.begin() + static_cast<std::ptrdiff_t>(prepared_header.size()),
			noncanonical.begin() + static_cast<std::ptrdiff_t>(prepared_header.size() + 2U));
		prepared_payload.assign(noncanonical.begin() +
									static_cast<std::ptrdiff_t>(prepared_header.size() + 2U),
								noncanonical.end());
		auto noncanonical_finalize =
			std::move(*noncanonical_prepared)
				.finalize(std::move(prepared_control), std::move(prepared_payload));
		require(!noncanonical_finalize &&
					noncanonical_finalize.error().code == "provider.malformed-frame",
				"prepared public facade skipped canonical-CBOR finalization");
		auto noncanonical_value = optional;
		noncanonical_value.control = {std::byte{0x78}, std::byte{0x00}};
		auto encode_canonical_failure = encode_frame(noncanonical_value);
		require(!encode_canonical_failure &&
					encode_canonical_failure.error().code == "provider.malformed-frame",
				"public encoder accepted noncanonical unknown optional control");

		auto known_optional = optional;
		known_optional.type = message_type::hello;
		require(!encode_frame(known_optional), "public facade accepted known optional message");
		auto required = optional;
		required.flags = static_cast<std::uint16_t>(frame_flag::required_extension);
		auto required_failure = encode_frame(required);
		require(!required_failure &&
					required_failure.error().code == "provider.unknown-required-extension",
				"public facade changed unknown-required rejection");
		auto compressed = optional;
		compressed.flags = static_cast<std::uint16_t>(frame_flag::compressed_payload);
		auto compressed_failure = encode_frame(compressed);
		require(!compressed_failure &&
					compressed_failure.error().code == "provider.unsupported-compression",
				"public facade accepted compressed payload");
		auto reserved = optional;
		reserved.flags = 0x10U;
		auto reserved_failure = encode_frame(reserved);
		require(!reserved_failure &&
					reserved_failure.error().code == "provider.invalid-frame-flags",
				"public facade accepted reserved flags");

		protocol_limits small;
		small.max_payload_bytes = 1U;
		auto oversized = optional;
		oversized.payload.push_back(std::byte{'q'});
		auto encode_bound_failure = encode_frame(oversized, small);
		require(!encode_bound_failure &&
					encode_bound_failure.error().code == "provider.oversized-payload",
				"public facade did not reject payload before wire allocation");

		auto declared_oversized = *public_wire;
		std::fill(declared_oversized.begin() + 32, declared_oversized.begin() + 40, std::byte{});
		declared_oversized[36U] = std::byte{0x01};
		declared_oversized[39U] = std::byte{0x01};
		auto decode_bound_failure = decode_frame(declared_oversized);
		auto stream_bound_failure = decode_frame_stream(declared_oversized);
		require(!decode_bound_failure && !stream_bound_failure &&
					decode_bound_failure.error() == stream_bound_failure.error() &&
					decode_bound_failure.error().code == "provider.oversized-frame",
				"declared oversized payload was not rejected consistently before allocation");
	}

	void test_registry_and_heartbeat()
	{
		require(static_cast<std::uint16_t>(message_type::heartbeat) == 23U,
				"heartbeat registry id");
		require(static_cast<std::uint16_t>(message_type::source_closure_manifest) == 24U &&
					static_cast<std::uint16_t>(message_type::source_closure_reject) == 29U,
				"source closure registry ids");
		require(is_known_message_type(message_type::source_closure_ack) &&
					is_source_closure_message(message_type::source_closure_chunk),
				"source closure registry membership");
		frame source_frame{message_type::source_closure_ack,
						   1U,
						   0U,
						   {std::byte{0xa0}},
						   {},
						   protocol_v2_major,
						   protocol_v2_minor,
						   0U};
		auto source_wire = encode_frame(source_frame);
		require(source_wire.has_value(), "source closure frame encoding");
		auto source_decoded = decode_frame(*source_wire);
		require(source_decoded && source_decoded->type == message_type::source_closure_ack,
				"source closure frame decoding");
		source_frame.flags = static_cast<std::uint16_t>(frame_flag::optional_extension);
		require(!encode_frame(source_frame), "optional extension flag on known closure rejected");
		source_frame.flags = static_cast<std::uint16_t>(frame_flag::required_extension);
		require(!encode_frame(source_frame), "required extension flag on closure rejected");
		provider_protocol_v2_heartbeat_control heartbeat{
			"cxxlens.provider-control.heartbeat.v1",
			provider_protocol_v2_heartbeat_kind::ack,
			"cxxlens.provider:sha256:" + std::string(64U, 'a'),
			{2U, 0U, 0U},
			"provider-session:sha256:" + std::string(64U, 'b'),
			"task:semantic-v2:sha256:" + std::string(64U, 'c'),
			1U,
			0U,
			12U,
			0U,
			semantic('d')};
		auto encoded = encode_provider_protocol_v2_heartbeat_control(heartbeat);
		require(encoded.has_value(), "typed heartbeat encoding");
		frame wire{message_type::heartbeat,
				   1U,
				   0U,
				   *encoded,
				   {},
				   protocol_v2_major,
				   protocol_v2_minor,
				   0U};
		provider_protocol_v2_session_binding binding{heartbeat.provider_id,
													 heartbeat.provider_version,
													 heartbeat.protocol_session_id,
													 heartbeat.task_id,
													 heartbeat.stream_id};
		require(validate_provider_protocol_v2_heartbeat(wire, &binding).has_value(),
				"typed heartbeat validation");
		wire.flags = static_cast<std::uint16_t>(frame_flag::optional_extension);
		auto prohibited_flags = encode_frame(wire);
		require(!prohibited_flags &&
					prohibited_flags.error().code == "provider.protocol-state-invalid" &&
					prohibited_flags.error().field == "heartbeat",
				"reserved heartbeat flags use the stable state failure");
		wire.flags = 0U;
		wire.payload.push_back(std::byte{0x01});
		require(!validate_provider_protocol_v2_heartbeat(wire, &binding),
				"heartbeat payload is rejected");
	}

	void test_closure_state()
	{
		const std::string session{"provider-session:sha256:" + std::string(64U, '1')};
		const std::string task{"task:semantic-v2:sha256:" + std::string(64U, '2')};
		const std::string closure_id{"source-closure:semantic-v2:sha256:" + std::string(64U, '3')};
		const std::string closure_digest = semantic('4');
		const std::string manifest_digest = semantic('5');
		provider_protocol_v2_session session_config{
			session, task, semantic('6'), closure_digest, manifest_digest, 1U, {1'048'576U, 64U}};
		auto state = provider_protocol_v2_closure_state::create(session_config);
		require(state.has_value(), "closure state creation");
		provider_protocol_v2_manifest_descriptor descriptor{
			provider_protocol_v2_manifest_kind::descriptor,
			session,
			task,
			semantic('6'),
			closure_id,
			closure_digest,
			manifest_digest,
			0U,
			1U,
			0U};
		provider_protocol_v2_control control{provider_protocol_v2_manifest{descriptor}};
		auto control_bytes = encode_provider_protocol_v2_closure_control(
			message_type::source_closure_manifest, control);
		require(control_bytes.has_value(), "closure descriptor encoding");
		frame descriptor_frame{message_type::source_closure_manifest,
							   1U,
							   0U,
							   *control_bytes,
							   {},
							   protocol_v2_major,
							   protocol_v2_minor,
							   0U};
		require(state->accept(descriptor_frame).has_value(), "closure descriptor accepted");
		provider_protocol_v2_ack ack{
			session, task, closure_digest, semantic('7'), "spool-receipt:1", "cleanup-owner:1"};
		control = ack;
		control_bytes =
			encode_provider_protocol_v2_closure_control(message_type::source_closure_ack, control);
		require(control_bytes.has_value(), "closure ack encoding");
		frame ack_frame{message_type::source_closure_ack,
						1U,
						1U,
						*control_bytes,
						{},
						protocol_v2_major,
						protocol_v2_minor,
						0U};
		require(!state->accept(ack_frame), "ack before seal is rejected");
		require(!state->accept(descriptor_frame), "replayed descriptor is rejected");
	}

	void test_runtime_closure_channel()
	{
		const std::string session{"provider-session:sha256:" + std::string(64U, '1')};
		const std::string task{"task:semantic-v2:sha256:" + std::string(64U, '2')};
		const std::string closure_digest = semantic('4');
		const std::string manifest_digest = semantic('5');
		const std::string task_digest = semantic('6');
		const std::string transfer_digest = semantic('7');
		const std::string receipts_digest = semantic('8');
		const std::string blob_digest = "sha256:" + std::string(64U, '9');

		const auto closure_frame = [&](const message_type type,
									   const std::uint64_t sequence,
									   provider_protocol_v2_control control,
									   std::vector<std::byte> payload = {})
		{
			auto encoded = encode_provider_protocol_v2_closure_control(type, control);
			require(encoded.has_value(), "runtime closure control encoding");
			return frame{type,
						 2U,
						 sequence,
						 std::move(*encoded),
						 std::move(payload),
						 protocol_v2_major,
						 protocol_v2_minor,
						 0U};
		};

		const provider_protocol_v2_manifest_descriptor descriptor{
			provider_protocol_v2_manifest_kind::descriptor,
			session,
			task,
			task_digest,
			"source-closure:" + closure_digest,
			closure_digest,
			manifest_digest,
			1U,
			1U,
			1U};
		const provider_protocol_v2_manifest_chunk manifest_chunk{
			provider_protocol_v2_manifest_kind::chunk, session, task, manifest_digest, 0U, 0U, 1U};
		const provider_protocol_v2_blob blob{
			session, task, closure_digest, 0U, blob_digest, 1U, 1U, 1U};
		const provider_protocol_v2_chunk blob_chunk{session, task, 0U, blob_digest, 0U, 0U, 1U};
		const provider_protocol_v2_seal seal{session,
											 task,
											 task_digest,
											 manifest_digest,
											 receipts_digest,
											 1U,
											 1U,
											 closure_digest,
											 transfer_digest};
		const provider_protocol_v2_ack ack{
			session,
			task,
			closure_digest,
			transfer_digest,
			"spool-receipt:semantic-v2:sha256:" + std::string(64U, 'a'),
			"cleanup-owner:semantic-v2:sha256:" + std::string(64U, 'b')};
		const provider_protocol_v2_reject reject{session,
												 task,
												 "before-manifest",
												 "source-closure.required-feature-missing",
												 {},
												 "cleanup-receipt:semantic-v2:sha256:" +
													 std::string(64U, 'c')};
		std::vector<frame> closure_frames;
		closure_frames.push_back(closure_frame(
			message_type::source_closure_manifest, 0U, provider_protocol_v2_manifest{descriptor}));
		closure_frames.push_back(closure_frame(message_type::source_closure_manifest,
											   1U,
											   provider_protocol_v2_manifest{manifest_chunk},
											   {std::byte{'m'}}));
		closure_frames.push_back(closure_frame(message_type::source_closure_blob, 2U, blob));
		closure_frames.push_back(
			closure_frame(message_type::source_closure_chunk, 3U, blob_chunk, {std::byte{'b'}}));
		closure_frames.push_back(closure_frame(message_type::source_closure_seal, 4U, seal));
		closure_frames.push_back(closure_frame(message_type::source_closure_ack, 5U, ack));
		provider_protocol_v2_session session_config{
			session, task, task_digest, closure_digest, manifest_digest, 2U};
		auto channel = provider_runtime_closure_channel::create(session_config);
		require(channel.has_value(), "runtime closure channel creation");
		for (const auto& value : closure_frames)
			require(channel->accept(value).has_value(), "runtime closure channel state transition");
		require(channel->acknowledged() &&
					channel->phase() == provider_protocol_v2_phase::acknowledged,
				"runtime closure channel did not expose acknowledged authority");
		auto replay = channel->accept(closure_frames.back());
		require(!replay && replay.error().code == "protocol-v2.replay-rejected",
				"runtime closure channel accepted a replayed ack");

		const auto provider_id = std::string{"test.provider"};
		const cxxlens::sdk::semantic_version provider_version{1U, 0U, 0U};
		auto accepted =
			encode_task_accepted_metadata({provider_id, provider_version.string(), task});
		const std::vector<coverage_unit> coverage_values{{"task", task, "covered", {}}};
		auto coverage = encode_coverage_metadata(coverage_values);
		auto unresolved = encode_unresolved_metadata({});
		auto evidence = encode_evidence_metadata({});
		auto complete = encode_task_complete_metadata({task});
		require(accepted && coverage && unresolved && evidence && complete,
				"runtime transcript metadata encoding");
		std::vector<frame> output_frames;
		output_frames.push_back({message_type::task_accepted,
								 1U,
								 0U,
								 std::move(*accepted),
								 {},
								 protocol_v2_major,
								 protocol_v2_minor,
								 0U});
		output_frames.push_back({message_type::coverage_chunk,
								 1U,
								 1U,
								 std::move(*coverage),
								 {},
								 protocol_v2_major,
								 protocol_v2_minor,
								 0U});
		output_frames.push_back({message_type::unresolved_chunk,
								 1U,
								 2U,
								 std::move(*unresolved),
								 {},
								 protocol_v2_major,
								 protocol_v2_minor,
								 0U});
		output_frames.push_back({message_type::progress,
								 1U,
								 3U,
								 std::move(*evidence),
								 {},
								 protocol_v2_major,
								 protocol_v2_minor,
								 0U});
		output_frames.push_back({message_type::task_complete,
								 1U,
								 4U,
								 std::move(*complete),
								 {},
								 protocol_v2_major,
								 protocol_v2_minor,
								 0U});

		cxxlens::sdk::provider::detail::transcript_validation_request request{
			task,
			provider_id,
			provider_version,
			nullptr,
			{},
			{16U * 1024U * 1024U, 64U},
			nullptr,
			false,
			nullptr,
			1U,
			false,
			nullptr};
		auto validated = cxxlens::sdk::provider::detail::validate_provider_transcript(
			request, output_frames, {});
		require(validated &&
					validated->kind ==
						cxxlens::sdk::provider::detail::transcript_terminal_kind::complete,
				"runtime output transcript validation changed after closure split");

		auto mixed_output = output_frames;
		mixed_output.push_back(closure_frames.front());
		validated =
			cxxlens::sdk::provider::detail::validate_provider_transcript(request, mixed_output, {});
		require(!validated && validated.error().code == "source-closure.channel-required",
				"runtime mixed closure frames into the provider output transcript");

		auto rejection_channel = provider_runtime_closure_channel::create(session_config);
		require(rejection_channel.has_value(), "runtime rejection channel creation");
		auto rejected = rejection_channel->accept(
			closure_frame(message_type::source_closure_reject, 0U, reject));
		require(rejected && rejection_channel->rejected() &&
					rejection_channel->phase() == provider_protocol_v2_phase::rejected,
				"runtime did not preserve a typed closure rejection");
	}
} // namespace

int main()
{
	test_single_codec_facade();
	test_registry_and_heartbeat();
	test_closure_state();
	test_runtime_closure_channel();
	std::cout << "provider Protocol 2 adapter tests passed\n";
}
