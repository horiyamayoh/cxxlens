#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "../../../src/sdk/protocol2/protocol2_codec.hpp"

namespace
{
	using cxxlens::sdk::protocol2::control_field;
	using cxxlens::sdk::protocol2::control_value;

	void control_round_trip_and_canonical_rejection()
	{
		const std::vector<control_field> fields{
			{"task_id", control_value{std::string{"task:semantic-v2:sha256:" + std::string(64U, 'a')}}},
			{"kind", control_value{std::string{"source_closure_manifest"}}},
			{"chunk_count", control_value{std::uint64_t{2U}}},
		};
		auto encoded = cxxlens::sdk::protocol2::encode_control(fields);
		assert(encoded);
		auto decoded = cxxlens::sdk::protocol2::decode_control(*encoded);
		assert(decoded);
		assert(decoded->size() == fields.size());
		assert((*decoded)[0].key == "kind");
		assert((*decoded)[1].key == "task_id");
		assert((*decoded)[2].key == "chunk_count");

		const std::vector<std::byte> non_shortest{std::byte{0xa1}, std::byte{0x61}, std::byte{'x'},
			std::byte{0x18}, std::byte{0x01}};
		assert(!cxxlens::sdk::protocol2::decode_control(non_shortest));

		const std::vector<std::byte> duplicate_key{std::byte{0xa2}, std::byte{0x61}, std::byte{'x'},
			std::byte{0x01}, std::byte{0x61}, std::byte{'x'}, std::byte{0x02}};
		assert(!cxxlens::sdk::protocol2::decode_control(duplicate_key));
	}

	void frame_round_trip_and_tamper_rejection()
	{
		cxxlens::sdk::protocol2::frame input;
		input.type = cxxlens::sdk::protocol2::message_type::source_closure_chunk;
		input.sequence = 7U;
		input.control = {std::byte{0xa1}, std::byte{0x63}, std::byte{'o'}, std::byte{'f'}, std::byte{'f'},
			std::byte{0x00}};
		input.payload = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
		auto encoded = cxxlens::sdk::protocol2::encode_frame(input);
		assert(encoded);
		assert(encoded->size() == cxxlens::sdk::protocol2::frame_header_bytes + input.control.size() +
			input.payload.size());
		auto decoded = cxxlens::sdk::protocol2::decode_frame(*encoded);
		assert(decoded);
		assert(decoded->type == input.type);
		assert(decoded->sequence == input.sequence);
		assert(decoded->control == input.control);
		assert(decoded->payload == input.payload);

		auto tampered = *encoded;
		tampered.back() ^= std::byte{0x01};
		assert(!cxxlens::sdk::protocol2::decode_frame(tampered));
		auto wrong_version = *encoded;
		wrong_version[4] = std::byte{0x00};
		wrong_version[5] = std::byte{0x01};
		assert(!cxxlens::sdk::protocol2::decode_frame(wrong_version));
	}
} // namespace

int main()
{
	control_round_trip_and_canonical_rejection();
	frame_round_trip_and_tamper_rejection();
}
