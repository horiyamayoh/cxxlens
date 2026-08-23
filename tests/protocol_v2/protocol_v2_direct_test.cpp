#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include "protocol_v2/cbor.hpp"
#include "protocol_v2/closure.hpp"
#include "protocol_v2/codec.hpp"

namespace
{
	using cxxlens::protocol_v2::byte;
	using cxxlens::protocol_v2::bytes;
	using cxxlens::protocol_v2::closure_control;
	using cxxlens::protocol_v2::closure_session;
	using cxxlens::protocol_v2::frame;
	using cxxlens::protocol_v2::manifest_kind;
	using cxxlens::protocol_v2::message_type;
	using cxxlens::protocol_v2::source_closure_ack;
	using cxxlens::protocol_v2::source_closure_blob_descriptor;
	using cxxlens::protocol_v2::source_closure_chunk;
	using cxxlens::protocol_v2::source_closure_manifest;
	using cxxlens::protocol_v2::source_closure_manifest_chunk;
	using cxxlens::protocol_v2::source_closure_manifest_descriptor;
	using cxxlens::protocol_v2::source_closure_seal;
	using cxxlens::protocol_v2::cbor::array;
	using cxxlens::protocol_v2::cbor::map;
	using cxxlens::protocol_v2::cbor::value;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << "FAIL: " << message << '\n';
			std::exit(1);
		}
	}

	template <typename T>
	void require_ok(const cxxlens::sdk::result<T>& result, const std::string_view message)
	{
		if (!result)
		{
			std::cerr << "FAIL: " << message << " (" << result.error().code << ':'
					  << result.error().detail << ")\n";
			std::exit(1);
		}
	}

	void require_error(const auto& result, const std::string_view message)
	{
		require(!result, message);
	}

	bytes payload(const std::string_view text)
	{
		bytes output;
		for (const auto character : text)
			output.push_back(static_cast<byte>(static_cast<unsigned char>(character)));
		return output;
	}

	std::string semantic_digest(const char fill)
	{
		return std::string{"semantic-v2:sha256:"} + std::string(64U, fill);
	}

	std::string content_digest(const char fill)
	{
		return std::string{"sha256:"} + std::string(64U, fill);
	}

	bytes hello_control()
	{
		map fields;
		fields.emplace_back("schema", value{"cxxlens.protocol-control.hello.v2"});
		fields.emplace_back("kind", value{"hello"});
		auto encoded = cxxlens::protocol_v2::cbor::encode(value{std::move(fields)});
		require_ok(encoded, "hello control encoding");
		return std::move(*encoded);
	}

	void test_sha256_and_cbor()
	{
		const auto empty = cxxlens::protocol_v2::sha256({});
		const std::string expected_hex =
			"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
		std::string actual;
		constexpr char hex[] = "0123456789abcdef";
		for (const auto item : empty)
		{
			const auto byte_value = std::to_integer<unsigned char>(item);
			actual.push_back(hex[byte_value >> 4U]);
			actual.push_back(hex[byte_value & 0x0fU]);
		}
		require(actual == expected_hex, "SHA-256 empty vector");

		map fields;
		fields.emplace_back("z", value{std::uint64_t{23U}});
		fields.emplace_back("a", value{std::string{"NUL\0ok", 6U}});
		fields.emplace_back("nested", value{array{value{true}, value{std::int64_t{-2}}}});
		auto encoded = cxxlens::protocol_v2::cbor::encode(value{fields});
		require_ok(encoded, "canonical CBOR encoding");
		auto decoded = cxxlens::protocol_v2::cbor::decode(*encoded);
		require_ok(decoded, "canonical CBOR decoding");
		auto reencoded = cxxlens::protocol_v2::cbor::encode(*decoded);
		require_ok(reencoded, "canonical CBOR re-encoding");
		require(*reencoded == *encoded, "CBOR round trip is canonical");

		bytes non_shortest{byte{0x18}, byte{0x17}};
		require_error(cxxlens::protocol_v2::cbor::decode(non_shortest),
					  "non-shortest CBOR integer rejected");
		bytes indefinite{byte{0x9f}, byte{0xff}};
		require_error(cxxlens::protocol_v2::cbor::decode(indefinite), "indefinite CBOR rejected");
		bytes invalid_utf8{byte{0x61}, byte{0xc0}};
		require_error(cxxlens::protocol_v2::cbor::decode(invalid_utf8), "invalid UTF-8 rejected");
		bytes unordered_map{
			byte{0xa2}, byte{0x61}, byte{'z'}, byte{0x01}, byte{0x61}, byte{'a'}, byte{0x02}};
		require_error(cxxlens::protocol_v2::cbor::decode(unordered_map),
					  "noncanonical map order rejected");
		bytes duplicate_map{
			byte{0xa2}, byte{0x61}, byte{'a'}, byte{0x01}, byte{0x61}, byte{'a'}, byte{0x02}};
		require_error(cxxlens::protocol_v2::cbor::decode(duplicate_map),
					  "duplicate map key rejected");
	}

	void test_frame_codec_and_guards()
	{
		frame input;
		input.type = message_type::hello;
		input.protocol_minor = 0U;
		input.stream_id = 1U;
		input.sequence = 0U;
		input.control = hello_control();
		input.payload = payload("payload");
		auto encoded = cxxlens::protocol_v2::encode_frame(input);
		require_ok(encoded, "protocol 2 frame encoding");
		require(encoded->size() ==
					cxxlens::protocol_v2::fixed_header_bytes + input.control.size() +
						input.payload.size(),
				"104-byte header size");
		auto decoded = cxxlens::protocol_v2::decode_frame(*encoded);
		require_ok(decoded, "protocol 2 frame decoding");
		require(decoded->type == input.type && decoded->sequence == input.sequence &&
					decoded->control == input.control && decoded->payload == input.payload,
				"protocol 2 frame round trip");
		require(!cxxlens::protocol_v2::digest_is_zero(decoded->control_digest) &&
					!cxxlens::protocol_v2::digest_is_zero(decoded->payload_digest),
				"frame digests present");

		auto tampered = *encoded;
		tampered.back() ^= byte{0x01};
		auto digest_failure = cxxlens::protocol_v2::decode_frame(tampered);
		require_error(digest_failure, "payload digest mismatch rejected");
		auto downgraded = *encoded;
		downgraded[4U] = byte{0x00};
		downgraded[5U] = byte{0x01};
		require_error(cxxlens::protocol_v2::decode_frame(downgraded), "major downgrade rejected");
		auto caller_downgrade = input;
		caller_downgrade.protocol_major = 1U;
		require_error(cxxlens::protocol_v2::encode_frame(caller_downgrade),
					  "caller major downgrade rejected");
		auto unknown = *encoded;
		unknown[8U] = byte{0x00};
		unknown[9U] = byte{0x7f};
		require_error(cxxlens::protocol_v2::decode_frame(unknown), "unknown message rejected");
		auto extension = *encoded;
		extension[10U] = byte{0x80};
		extension[11U] = byte{0x00};
		require_error(cxxlens::protocol_v2::decode_frame(extension),
					  "unknown extension flags rejected");

		cxxlens::protocol_v2::sequence_guard sequence;
		require_ok(sequence.accept(*decoded), "sequence zero accepted");
		require_error(sequence.accept(*decoded), "replayed sequence rejected");
		auto gap = *decoded;
		gap.sequence = 2U;
		require_error(sequence.accept(gap), "sequence gap rejected");
		cxxlens::protocol_v2::credit_window credit{
			{input.control.size() + input.payload.size(), 1U}};
		require_ok(credit.consume(*decoded), "credit consumed");
		require_error(credit.consume(*decoded), "credit exhaustion rejected");
	}

	frame closure_frame(const message_type type,
						const std::uint64_t sequence,
						const closure_control& control,
						const bytes data = {})
	{
		frame output;
		output.type = type;
		output.sequence = sequence;
		output.control = *cxxlens::protocol_v2::encode_closure_control(type, control);
		output.payload = data;
		return output;
	}

	void test_closure_codec_and_state()
	{
		const auto task_digest = semantic_digest('1');
		const auto closure_digest = semantic_digest('2');
		const auto manifest_digest = semantic_digest('3');
		const auto transfer_digest = semantic_digest('4');
		const auto receipts_digest = semantic_digest('5');
		const auto blob_digest = content_digest('a');
		const std::string session{"provider-session:sha256:" + std::string(64U, '6')};
		const std::string task{"task:semantic-v2:sha256:" + std::string(64U, '7')};

		source_closure_manifest_descriptor descriptor{manifest_kind::descriptor,
													  session,
													  task,
													  task_digest,
													  "source-closure:semantic-v2:sha256:" +
														  std::string(64U, '8'),
													  closure_digest,
													  manifest_digest,
													  3U,
													  2U,
													  2U};
		closure_control descriptor_control{source_closure_manifest{descriptor}};
		auto descriptor_bytes = cxxlens::protocol_v2::encode_closure_control(
			message_type::source_closure_manifest, descriptor_control);
		require_ok(descriptor_bytes, "closure manifest descriptor encoding");
		auto descriptor_decoded = cxxlens::protocol_v2::decode_closure_control(
			message_type::source_closure_manifest, *descriptor_bytes);
		require_ok(descriptor_decoded, "closure manifest descriptor decoding");
		require(std::get<source_closure_manifest>(*descriptor_decoded) ==
					source_closure_manifest{descriptor},
				"closure descriptor round trip");

		closure_session session_config{
			session, task, task_digest, closure_digest, manifest_digest, 1U};
		auto transfer = cxxlens::protocol_v2::closure_transfer::create(session_config);
		require_ok(transfer, "closure transfer creation");
		require_ok(transfer->accept(closure_frame(
					   message_type::source_closure_manifest, 0U, descriptor_control)),
				   "manifest descriptor accepted");

		source_closure_manifest_chunk manifest_chunk_a{
			manifest_kind::chunk, session, task, manifest_digest, 0U, 0U, 2U};
		require_ok(transfer->accept(
					   closure_frame(message_type::source_closure_manifest,
									 1U,
									 closure_control{source_closure_manifest{manifest_chunk_a}},
									 payload("ab"))),
				   "manifest first chunk accepted");
		source_closure_manifest_chunk manifest_chunk_b{
			manifest_kind::chunk, session, task, manifest_digest, 1U, 2U, 1U};
		require_ok(transfer->accept(
					   closure_frame(message_type::source_closure_manifest,
									 2U,
									 closure_control{source_closure_manifest{manifest_chunk_b}},
									 payload("c"))),
				   "manifest final chunk accepted");

		source_closure_blob_descriptor blob{
			session, task, closure_digest, 0U, blob_digest, 3U, 2U, 2U};
		require_ok(transfer->accept(closure_frame(message_type::source_closure_blob, 3U, blob)),
				   "blob descriptor accepted");
		source_closure_chunk blob_chunk_a{session, task, 0U, blob_digest, 0U, 0U, 2U};
		require_ok(transfer->accept(closure_frame(
					   message_type::source_closure_chunk, 4U, blob_chunk_a, payload("xy"))),
				   "blob first chunk accepted");
		source_closure_chunk blob_chunk_b{session, task, 0U, blob_digest, 1U, 2U, 1U};
		require_ok(transfer->accept(closure_frame(
					   message_type::source_closure_chunk, 5U, blob_chunk_b, payload("z"))),
				   "blob final chunk accepted");

		source_closure_seal seal{session,
								 task,
								 task_digest,
								 manifest_digest,
								 receipts_digest,
								 1U,
								 3U,
								 closure_digest,
								 transfer_digest};
		require_ok(transfer->accept(closure_frame(message_type::source_closure_seal, 6U, seal)),
				   "closure seal accepted");
		source_closure_ack ack{
			session, task, closure_digest, transfer_digest, "spool-receipt:1", "cleanup-owner:1"};
		require_ok(transfer->accept(closure_frame(message_type::source_closure_ack, 7U, ack)),
				   "closure ack accepted");
		require(transfer->phase() == cxxlens::protocol_v2::closure_phase::acknowledged,
				"closure transfer reached acknowledged");
		require_error(transfer->accept(closure_frame(message_type::source_closure_ack, 8U, ack)),
					  "post-terminal replay rejected");

		auto extra = *descriptor_bytes;
		// A map with an extra field is rejected by the typed closed-map decoder.
		auto map_value = cxxlens::protocol_v2::cbor::decode(extra);
		require_ok(map_value, "decode descriptor for extra-field mutation");
		std::get<map>(map_value->data).emplace_back("extra", value{"nope"});
		auto extra_bytes = cxxlens::protocol_v2::cbor::encode(*map_value);
		require_ok(extra_bytes, "encode extra-field mutation");
		require_error(cxxlens::protocol_v2::decode_closure_control(
						  message_type::source_closure_manifest, *extra_bytes),
					  "unknown closure field rejected");
	}
} // namespace

int main()
{
	test_sha256_and_cbor();
	test_frame_codec_and_guards();
	test_closure_codec_and_state();
	std::cout << "protocol-v2 direct tests passed\n";
}
