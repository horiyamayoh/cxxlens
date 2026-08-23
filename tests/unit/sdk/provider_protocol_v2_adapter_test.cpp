#include "sdk/provider_protocol_v2_adapter.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace
{
	using namespace cxxlens::sdk::provider;
	using cxxlens::sdk::provider::detail::encode_provider_protocol_v2_closure_control;
	using cxxlens::sdk::provider::detail::encode_provider_protocol_v2_heartbeat_control;
	using cxxlens::sdk::provider::detail::provider_protocol_v2_ack;
	using cxxlens::sdk::provider::detail::provider_protocol_v2_closure_state;
	using cxxlens::sdk::provider::detail::provider_protocol_v2_control;
	using cxxlens::sdk::provider::detail::provider_protocol_v2_heartbeat_control;
	using cxxlens::sdk::provider::detail::provider_protocol_v2_heartbeat_kind;
	using cxxlens::sdk::provider::detail::provider_protocol_v2_manifest;
	using cxxlens::sdk::provider::detail::provider_protocol_v2_manifest_descriptor;
	using cxxlens::sdk::provider::detail::provider_protocol_v2_manifest_kind;
	using cxxlens::sdk::provider::detail::provider_protocol_v2_session;
	using cxxlens::sdk::provider::detail::provider_protocol_v2_session_binding;
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
} // namespace

int main()
{
	test_registry_and_heartbeat();
	test_closure_state();
	std::cout << "provider Protocol 2 adapter tests passed\n";
}
