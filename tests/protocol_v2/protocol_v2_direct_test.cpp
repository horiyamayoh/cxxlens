#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>

#include "protocol_v2/cbor.hpp"
#include "protocol_v2/closure.hpp"
#include "protocol_v2/codec.hpp"

namespace
{
	thread_local bool fail_next_allocation{};
} // namespace

void* operator new(const std::size_t size)
{
	if (fail_next_allocation)
	{
		fail_next_allocation = false;
		throw std::bad_alloc{};
	}
	if (auto* allocation = std::malloc(size == 0U ? 1U : size); allocation != nullptr)
		return allocation;
	throw std::bad_alloc{};
}

void* operator new[](const std::size_t size)
{
	return ::operator new(size);
}

void operator delete(void* allocation) noexcept
{
	std::free(allocation);
}

void operator delete[](void* allocation) noexcept
{
	::operator delete(allocation);
}

void operator delete(void* allocation, std::size_t) noexcept
{
	std::free(allocation);
}

void operator delete[](void* allocation, std::size_t) noexcept
{
	::operator delete(allocation);
}

namespace
{
	using cxxlens::protocol_v2::byte;
	using cxxlens::protocol_v2::bytes;
	using cxxlens::protocol_v2::closure_control;
	using cxxlens::protocol_v2::closure_control_token;
	using cxxlens::protocol_v2::closure_session;
	using cxxlens::protocol_v2::closure_transfer;
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

	cxxlens::protocol_v2::cbor::scan_limits closure_scan_bound()
	{
		cxxlens::protocol_v2::cbor::scan_limits output;
		output.max_bytes = cxxlens::protocol_v2::max_control_bytes;
		output.max_depth = 2U;
		output.max_items = 96U;
		output.max_array_items = 0U;
		output.max_map_items = 32U;
		output.max_text_bytes = 4'096U;
		output.max_byte_string_bytes = 0U;
		output.require_root_map = true;
		return output;
	}

	void write_u32_be(std::array<byte, cxxlens::protocol_v2::fixed_header_bytes>& header,
					  const std::size_t offset,
					  const std::uint32_t value)
	{
		for (std::size_t index{}; index < 4U; ++index)
			header[offset + index] = static_cast<byte>(value >> (24U - index * 8U));
	}

	std::array<byte, cxxlens::protocol_v2::fixed_header_bytes>
	header_for_control(const frame& valid_frame, const bytes& replacement)
	{
		auto wire = cxxlens::protocol_v2::encode_frame(valid_frame);
		require_ok(wire, "closure header fixture encoding");
		std::array<byte, cxxlens::protocol_v2::fixed_header_bytes> header{};
		std::copy_n(wire->begin(), header.size(), header.begin());
		write_u32_be(header, 28U, static_cast<std::uint32_t>(replacement.size()));
		const auto digest = cxxlens::protocol_v2::sha256(replacement);
		std::copy(digest.begin(), digest.end(), header.begin() + 40);
		return header;
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

		const auto scan_bound = closure_scan_bound();
		require(cxxlens::protocol_v2::cbor::canonical_scan_workspace_bytes ==
					sizeof(cxxlens::protocol_v2::cbor::scan_workspace),
				"scanner workspace accounting is exact");
		auto scanned_unordered =
			cxxlens::protocol_v2::cbor::scan_canonical(unordered_map, scan_bound);
		auto scanned_duplicate =
			cxxlens::protocol_v2::cbor::scan_canonical(duplicate_map, scan_bound);
		require(!scanned_unordered &&
					scanned_unordered.error ==
						cxxlens::protocol_v2::cbor::scan_error::map_order_or_duplicate,
				"allocation-free scanner rejected unordered keys");
		require(!scanned_duplicate &&
					scanned_duplicate.error ==
						cxxlens::protocol_v2::cbor::scan_error::map_order_or_duplicate,
				"allocation-free scanner rejected duplicate keys");

		map maximum_map;
		for (std::size_t index{}; index < 32U; ++index)
			maximum_map.emplace_back(
				"k" + std::to_string(index / 10U) + std::to_string(index % 10U), value{index});
		auto maximum_map_bytes = cxxlens::protocol_v2::cbor::encode(value{maximum_map});
		require_ok(maximum_map_bytes, "maximum closure scanner map encoding");
		require(static_cast<bool>(
					cxxlens::protocol_v2::cbor::scan_canonical(*maximum_map_bytes, scan_bound)),
				"maximum 32-entry closure map scanned without DOM");

		map oversized_map = maximum_map;
		oversized_map.emplace_back("k32", value{std::uint64_t{32U}});
		auto oversized_map_bytes = cxxlens::protocol_v2::cbor::encode(value{oversized_map});
		require_ok(oversized_map_bytes, "oversized closure scanner map encoding");
		auto oversized_map_scan =
			cxxlens::protocol_v2::cbor::scan_canonical(*oversized_map_bytes, scan_bound);
		require(!oversized_map_scan &&
					oversized_map_scan.error ==
						cxxlens::protocol_v2::cbor::scan_error::map_shape_limit,
				"33-entry map rejected before DOM growth");

		map maximum_key_map;
		maximum_key_map.emplace_back(std::string(4'096U, 'k'), value{std::uint64_t{0U}});
		auto maximum_key_bytes = cxxlens::protocol_v2::cbor::encode(value{maximum_key_map});
		require_ok(maximum_key_bytes, "maximum scanner key encoding");
		require(static_cast<bool>(
					cxxlens::protocol_v2::cbor::scan_canonical(*maximum_key_bytes, scan_bound)),
				"maximum 4096-byte key scanned");

		bytes too_deep{byte{0xa1},
					   byte{0x61},
					   byte{'a'},
					   byte{0xa1},
					   byte{0x61},
					   byte{'b'},
					   byte{0xa1},
					   byte{0x61},
					   byte{'c'},
					   byte{0x00}};
		auto depth_scan = cxxlens::protocol_v2::cbor::scan_canonical(too_deep, scan_bound);
		require(!depth_scan &&
					depth_scan.error == cxxlens::protocol_v2::cbor::scan_error::depth_limit,
				"scanner depth bound rejected before recursion");
		bytes invalid_map_utf8{byte{0xa1}, byte{0x61}, byte{0xc0}, byte{0x00}};
		auto utf8_scan = cxxlens::protocol_v2::cbor::scan_canonical(invalid_map_utf8, scan_bound);
		require(!utf8_scan &&
					utf8_scan.error == cxxlens::protocol_v2::cbor::scan_error::text_limit_or_utf8,
				"scanner invalid UTF-8 rejection");
		bytes trailing_map{byte{0xa0}, byte{0x00}};
		auto trailing_scan = cxxlens::protocol_v2::cbor::scan_canonical(trailing_map, scan_bound);
		require(!trailing_scan &&
					trailing_scan.error == cxxlens::protocol_v2::cbor::scan_error::trailing_bytes,
				"scanner trailing byte rejection");
		bytes byte_string_value{byte{0xa1}, byte{0x61}, byte{'a'}, byte{0x41}, byte{0x00}};
		auto byte_string_scan =
			cxxlens::protocol_v2::cbor::scan_canonical(byte_string_value, scan_bound);
		require(!byte_string_scan &&
					byte_string_scan.error ==
						cxxlens::protocol_v2::cbor::scan_error::byte_string_limit,
				"closure scanner byte-string rejection");
		bytes array_value{byte{0xa1}, byte{0x61}, byte{'a'}, byte{0x81}, byte{0xf4}};
		auto array_scan = cxxlens::protocol_v2::cbor::scan_canonical(array_value, scan_bound);
		require(!array_scan &&
					array_scan.error == cxxlens::protocol_v2::cbor::scan_error::array_shape_limit,
				"closure scanner array rejection");

		bytes canonical_false_array{byte{0x99}, byte{0xff}, byte{0xf7}};
		canonical_false_array.insert(canonical_false_array.end(), 65'527U, byte{0xf4});
		auto false_array_scan =
			cxxlens::protocol_v2::cbor::scan_canonical(canonical_false_array, scan_bound);
		require(!false_array_scan && false_array_scan.item_count == 1U &&
					false_array_scan.error ==
						cxxlens::protocol_v2::cbor::scan_error::array_shape_limit,
				"real canonical 65527-false array rejected from declared shape");
		bytes nonshortest_false_array{byte{0x9a}, byte{0x00}, byte{0x00}, byte{0xff}, byte{0xf7}};
		auto nonshortest_scan =
			cxxlens::protocol_v2::cbor::scan_canonical(nonshortest_false_array, scan_bound);
		require(!nonshortest_scan &&
					nonshortest_scan.error == cxxlens::protocol_v2::cbor::scan_error::non_shortest,
				"32-bit spelling of 65527 distinguished as non-shortest");

		auto tiny_item_bound = scan_bound;
		tiny_item_bound.max_items = 4U;
		auto item_scan =
			cxxlens::protocol_v2::cbor::scan_canonical(*maximum_map_bytes, tiny_item_bound);
		require(!item_scan && item_scan.error == cxxlens::protocol_v2::cbor::scan_error::item_limit,
				"declared map cardinality rejected before item allocation");

		map exact_maximum_control;
		for (std::size_t index{}; index < 15U; ++index)
			exact_maximum_control.emplace_back(
				"m" + std::to_string(index / 10U) + std::to_string(index % 10U),
				value{std::string(4'096U, static_cast<char>('a' + index))});
		exact_maximum_control.emplace_back("m15", value{std::string(3'983U, 'z')});
		cxxlens::protocol_v2::cbor::limits exact_encode_bound;
		exact_encode_bound.max_bytes = cxxlens::protocol_v2::max_control_bytes;
		exact_encode_bound.max_depth = 2U;
		exact_encode_bound.max_items = 96U;
		exact_encode_bound.max_text_bytes = 4'096U;
		exact_encode_bound.max_byte_string_bytes = 0U;
		auto exact_control = cxxlens::protocol_v2::cbor::encode(
			value{std::move(exact_maximum_control)}, exact_encode_bound);
		require_ok(exact_control, "exact 65536-byte closure control encoding");
		require(exact_control->size() == cxxlens::protocol_v2::max_control_bytes,
				"exact closure control byte boundary");
		frame exact_control_frame;
		exact_control_frame.type = message_type::source_closure_chunk;
		exact_control_frame.stream_id = 1U;
		exact_control_frame.control = *exact_control;
		auto exact_wire = cxxlens::protocol_v2::encode_frame(exact_control_frame);
		require_ok(exact_wire, "exact maximum closure control frame encoding");
		auto exact_header = header_for_control(exact_control_frame, *exact_control);
		auto exact_prepared = cxxlens::protocol_v2::prepare_frame_header(exact_header);
		require_ok(exact_prepared, "exact maximum closure control header admission");
		bytes no_payload;
		auto exact_finalized =
			std::move(*exact_prepared).finalize(std::move(*exact_control), std::move(no_payload));
		require_ok(exact_finalized, "exact maximum closure control staged finalization");
		bytes oversized_control = exact_control_frame.control;
		oversized_control.push_back(byte{0xf4});
		auto oversized_header = header_for_control(exact_control_frame, oversized_control);
		require_error(cxxlens::protocol_v2::prepare_frame_header(oversized_header),
					  "65537-byte closure control rejected from header before body allocation");
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

		frame optional = input;
		optional.type = static_cast<message_type>(65'000U);
		optional.flags = cxxlens::protocol_v2::flag_optional_extension;
		optional.control = {byte{0x61}, byte{'a'}};
		optional.payload = payload("optional-payload");
		auto optional_wire = cxxlens::protocol_v2::encode_frame(optional);
		require_ok(optional_wire, "canonical unknown optional frame encoding");
		auto optional_decoded = cxxlens::protocol_v2::decode_frame(*optional_wire);
		require_ok(optional_decoded, "canonical unknown optional frame decoding");
		auto optional_stream = cxxlens::protocol_v2::decode_frame_stream(*optional_wire);
		require_ok(optional_stream, "canonical unknown optional stream decoding");
		require(static_cast<std::uint16_t>(optional_decoded->type) == 65'000U &&
					optional_decoded->flags == cxxlens::protocol_v2::flag_optional_extension,
				"unknown optional identity retained for skip accounting");
		require(optional_stream->size() == 1U &&
					static_cast<std::uint16_t>(optional_stream->front().type) == 65'000U,
				"stream codec did not retain unknown optional frame for accounting");

		auto optional_tampered = *optional_wire;
		optional_tampered.back() ^= byte{0x01};
		auto optional_digest_failure = cxxlens::protocol_v2::decode_frame(optional_tampered);
		require_error(optional_digest_failure, "tampered unknown optional frame rejected");
		require(optional_digest_failure.error().code == "protocol-v2.digest-mismatch",
				"unknown optional checksum rejection reason");

		auto noncanonical_optional = *optional_wire;
		noncanonical_optional[cxxlens::protocol_v2::fixed_header_bytes] = byte{0x78};
		noncanonical_optional[cxxlens::protocol_v2::fixed_header_bytes + 1U] = byte{0x00};
		const auto noncanonical_digest =
			cxxlens::protocol_v2::sha256(std::span<const byte>{noncanonical_optional}.subspan(
				cxxlens::protocol_v2::fixed_header_bytes, 2U));
		std::copy(noncanonical_digest.begin(),
				  noncanonical_digest.end(),
				  noncanonical_optional.begin() + 40);
		auto noncanonical_direct = cxxlens::protocol_v2::decode_frame(noncanonical_optional);
		auto noncanonical_stream = cxxlens::protocol_v2::decode_frame_stream(noncanonical_optional);
		require_error(noncanonical_direct, "noncanonical unknown optional control rejected");
		require_error(noncanonical_stream, "stream rejected noncanonical unknown optional control");
		require(noncanonical_direct.error() == noncanonical_stream.error() &&
					noncanonical_direct.error().code == "protocol-v2.cbor-invalid",
				"direct and stream codecs returned the same stable canonical-CBOR reason");
		auto noncanonical_value = optional;
		noncanonical_value.control = {byte{0x78}, byte{0x00}};
		require_error(cxxlens::protocol_v2::encode_frame(noncanonical_value),
					  "encoder accepted noncanonical unknown optional control");

		auto known_optional = input;
		known_optional.flags = cxxlens::protocol_v2::flag_optional_extension;
		require_error(cxxlens::protocol_v2::encode_frame(known_optional),
					  "known message with optional flag rejected");
		auto heartbeat = input;
		heartbeat.type = cxxlens::protocol_v2::message_type::heartbeat;
		heartbeat.flags = cxxlens::protocol_v2::flag_optional_extension;
		auto heartbeat_flags = cxxlens::protocol_v2::encode_frame(heartbeat);
		require_error(heartbeat_flags, "heartbeat with optional flag rejected");
		require(heartbeat_flags.error().code == "protocol-v2.header-invalid" &&
					heartbeat_flags.error().field == "payload" &&
					heartbeat_flags.error().detail == "heartbeat-flags-or-payload",
				"heartbeat flag rejection reason");
		heartbeat.flags = 0U;
		heartbeat.payload.push_back(byte{0x01});
		auto heartbeat_payload = cxxlens::protocol_v2::encode_frame(heartbeat);
		require_error(heartbeat_payload, "heartbeat with payload rejected");
		require(heartbeat_payload.error() == heartbeat_flags.error(),
				"heartbeat flags and payload share one exact failure contract");
		auto unknown_required = optional;
		unknown_required.flags = cxxlens::protocol_v2::flag_required_extension;
		auto required_failure = cxxlens::protocol_v2::encode_frame(unknown_required);
		require_error(required_failure, "unknown required extension rejected");
		require(required_failure.error().code == "protocol-v2.unknown-required-extension",
				"unknown required extension reason");
		auto compressed = optional;
		compressed.flags = cxxlens::protocol_v2::flag_compressed_payload;
		require_error(cxxlens::protocol_v2::encode_frame(compressed),
					  "compressed unknown frame rejected");
		auto reserved = optional;
		reserved.flags = 0x10U;
		require_error(cxxlens::protocol_v2::encode_frame(reserved),
					  "reserved frame flags rejected");
		auto extension = *encoded;
		extension[10U] = byte{0x80};
		extension[11U] = byte{0x00};
		require_error(cxxlens::protocol_v2::decode_frame(extension),
					  "unknown extension flags rejected");

		cxxlens::protocol_v2::sequence_guard sequence;
		require_ok(sequence.accept(*decoded), "sequence zero accepted");
		require_error(sequence.accept(*decoded), "replayed sequence rejected");
		auto foreign_stream = *decoded;
		foreign_stream.stream_id = 2U;
		foreign_stream.sequence = 1U;
		require_error(sequence.accept(foreign_stream), "cross-stream frame rejected");
		auto gap = *decoded;
		gap.sequence = 2U;
		require_error(sequence.accept(gap), "sequence gap rejected");
		auto next = *decoded;
		next.sequence = 1U;
		require_ok(sequence.accept(next),
				   "failed replay/gap/cross-stream checks did not advance state");

		const auto original_wire_bytes =
			cxxlens::protocol_v2::fixed_header_bytes + input.control.size() + input.payload.size();
		cxxlens::protocol_v2::credit_window credit{{original_wire_bytes, 1U}};
		require_ok(credit.consume(*decoded), "credit consumed");
		require_error(credit.consume(*decoded), "credit exhaustion rejected");
		cxxlens::protocol_v2::credit_window short_credit{{optional_wire->size() - 1U, 1U}};
		require_error(short_credit.consume(*optional_decoded),
					  "unknown optional frame rejected at exact encoded-byte boundary");
		cxxlens::protocol_v2::credit_window exact_credit{{optional_wire->size(), 1U}};
		require_ok(exact_credit.consume(*optional_decoded),
				   "unknown optional frame accounted with its 104-byte header");
		require(exact_credit.available().bytes == 0U && exact_credit.available().frames == 0U,
				"unknown optional credit account was exact");
	}

	void test_prepared_frame_decode()
	{
		static_assert(!std::is_copy_constructible_v<cxxlens::protocol_v2::prepared_frame_decode>);
		frame input;
		input.type = message_type::source_closure_chunk;
		input.stream_id = 9U;
		input.sequence = 17U;
		source_closure_chunk prepared_chunk{
			"prepared-session", "prepared-task", 0U, content_digest('a'), 0U, 0U, 1'048'576U};
		input.control = *cxxlens::protocol_v2::encode_closure_control(
			message_type::source_closure_chunk, closure_control{prepared_chunk});
		input.payload.assign(1'048'576U, byte{0x5a});
		auto wire = cxxlens::protocol_v2::encode_frame(input);
		require_ok(wire, "prepared decode 1 MiB frame encoding");
		std::array<byte, cxxlens::protocol_v2::fixed_header_bytes> header{};
		std::copy_n(wire->begin(), header.size(), header.begin());
		auto prepared = cxxlens::protocol_v2::prepare_frame_header(header);
		require_ok(prepared, "prepared decode header validation");
		require(
			prepared->control_bytes() == input.control.size() &&
				prepared->payload_bytes() == input.payload.size() &&
				prepared->body_resident_bytes() == input.control.size() + input.payload.size() &&
				prepared->type() == input.type && prepared->flags() == input.flags &&
				prepared->stream_id() == input.stream_id && prepared->sequence() == input.sequence,
			"prepared decode body accounting");

		bytes control(wire->begin() + static_cast<std::ptrdiff_t>(header.size()),
					  wire->begin() +
						  static_cast<std::ptrdiff_t>(header.size() + input.control.size()));
		bytes body(wire->begin() +
					   static_cast<std::ptrdiff_t>(header.size() + input.control.size()),
				   wire->end());
		const auto* const body_allocation = body.data();
		auto decoded = std::move(*prepared).finalize(std::move(control), std::move(body));
		require_ok(decoded, "prepared decode body finalization");
		require(decoded->payload.data() == body_allocation && decoded->payload == input.payload,
				"prepared decode copied the caller-owned 1 MiB payload");

		auto tampered_prepared = cxxlens::protocol_v2::prepare_frame_header(header);
		require_ok(tampered_prepared, "tampered prepared decode setup");
		control = input.control;
		body = input.payload;
		body.back() ^= byte{0x01};
		auto tampered = std::move(*tampered_prepared).finalize(std::move(control), std::move(body));
		require_error(tampered, "prepared decode accepted tampered payload");
		require(tampered.error().code == "protocol-v2.digest-mismatch",
				"prepared decode tamper reason");

		auto short_prepared = cxxlens::protocol_v2::prepare_frame_header(header);
		require_ok(short_prepared, "short prepared decode setup");
		control = input.control;
		body = input.payload;
		body.pop_back();
		auto short_body = std::move(*short_prepared).finalize(std::move(control), std::move(body));
		require_error(short_body, "prepared decode accepted a short body");

		auto invalid_flags_header = header;
		invalid_flags_header[11U] = byte{cxxlens::protocol_v2::flag_optional_extension};
		require_error(cxxlens::protocol_v2::prepare_frame_header(invalid_flags_header),
					  "prepared decode accepted optional flag on a known message");
		auto oversized_header = header;
		std::fill(oversized_header.begin() + 32, oversized_header.begin() + 40, byte{});
		oversized_header[36U] = byte{0x01};
		oversized_header[39U] = byte{0x01};
		require_error(cxxlens::protocol_v2::prepare_frame_header(oversized_header),
					  "prepared decode accepted an oversized declared payload");

		frame small = input;
		prepared_chunk.byte_count = 1U;
		small.control = *cxxlens::protocol_v2::encode_closure_control(
			message_type::source_closure_chunk, closure_control{prepared_chunk});
		small.payload = {byte{'b'}};
		auto small_wire = cxxlens::protocol_v2::encode_frame(small);
		require_ok(small_wire, "prepared one-shot setup encoding");
		std::copy_n(small_wire->begin(), header.size(), header.begin());
		auto move_source = cxxlens::protocol_v2::prepare_frame_header(header);
		require_ok(move_source, "prepared one-shot header validation");
		auto moved = std::move(*move_source);
		auto moved_from =
			std::move(*move_source).finalize(bytes{small.control}, bytes{small.payload});
		require_error(moved_from, "moved-from prepared frame remained usable");
		auto first = std::move(moved).finalize(bytes{small.control}, bytes{small.payload});
		require_ok(first, "prepared frame first finalization");
		auto duplicate = std::move(moved).finalize(bytes{small.control}, bytes{small.payload});
		require_error(duplicate, "prepared frame finalized twice");

		cxxlens::protocol_v2::sequence_guard maximum_sequence{
			9U, std::numeric_limits<std::uint64_t>::max()};
		frame terminal_sequence = small;
		terminal_sequence.stream_id = 9U;
		terminal_sequence.sequence = std::numeric_limits<std::uint64_t>::max();
		require_ok(maximum_sequence.accept(terminal_sequence),
				   "maximum sequence was not accepted once");
		require_error(maximum_sequence.accept(terminal_sequence),
					  "maximum sequence duplicate was accepted");
		terminal_sequence.sequence = 0U;
		require_error(maximum_sequence.accept(terminal_sequence), "sequence wrapped after maximum");

		bytes canonical_false_array{byte{0x99}, byte{0xff}, byte{0xf7}};
		canonical_false_array.insert(canonical_false_array.end(), 65'527U, byte{0xf4});
		frame closure_header_fixture = small;
		closure_header_fixture.payload.clear();
		auto huge_header = header_for_control(closure_header_fixture, canonical_false_array);
		auto huge_prepared = cxxlens::protocol_v2::prepare_frame_header(huge_header);
		require_ok(huge_prepared, "65527 array header passed allocation preflight");
		auto huge_rejected =
			std::move(*huge_prepared).finalize(std::move(canonical_false_array), bytes{});
		require_error(huge_rejected, "prepared closure decoder accepted 65527 false array");
		require(huge_rejected.error().code == "protocol-v2.cbor-invalid" &&
					huge_rejected.error().detail == "array-shape-limit",
				"prepared closure decoder used declared-shape scanner rejection");

		bytes nonshortest_false_array{byte{0x9a}, byte{0x00}, byte{0x00}, byte{0xff}, byte{0xf7}};
		auto nonshort_header = header_for_control(closure_header_fixture, nonshortest_false_array);
		auto nonshort_prepared = cxxlens::protocol_v2::prepare_frame_header(nonshort_header);
		require_ok(nonshort_prepared, "non-shortest array header passed allocation preflight");
		auto nonshort_rejected =
			std::move(*nonshort_prepared).finalize(std::move(nonshortest_false_array), bytes{});
		require_error(nonshort_rejected, "prepared closure decoder accepted non-shortest array");
		require(nonshort_rejected.error().detail == "non-shortest",
				"prepared closure decoder distinguished non-shortest array head");
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

	cxxlens::sdk::result<void> accept_adopted(closure_transfer& transfer, frame value)
	{
		auto token = cxxlens::protocol_v2::decode_closure_control_token(
			value.type, std::move(value.control), value.payload, transfer.session().limits);
		if (!token)
			return cxxlens::sdk::unexpected(token.error());
		return transfer.accept_decoded(value, std::move(*token));
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

		static_assert(!std::is_copy_constructible_v<closure_control_token>);
		static_assert(!std::is_copy_constructible_v<closure_transfer>);
		bytes empty_payload;
		bytes adopted_control = *descriptor_bytes;
		const auto* adopted_allocation = adopted_control.data();
		fail_next_allocation = true;
		auto token_result = cxxlens::protocol_v2::decode_closure_control_token(
			message_type::source_closure_manifest, std::move(adopted_control), empty_payload);
		const auto token_decode_was_allocation_free = fail_next_allocation;
		fail_next_allocation = false;
		require_ok(token_result, "bounded closure token decoding");
		require(token_decode_was_allocation_free,
				"production closure token decode performs no dynamic allocation");
		require(token_result->control_bytes().data() == adopted_allocation &&
					token_result->resident_bytes() ==
						token_result->control_capacity() +
							cxxlens::protocol_v2::closure_control_decode_fixed_workspace_bytes,
				"closure token adopted bytes and reported concrete fixed workspaces");
		require(token_result->value().has_value(), "fresh closure token exposes one valid view");
		closure_control_token moved_token{std::move(*token_result)};
		require(!token_result->value().has_value(), "moved-from token view is inaccessible");
		auto consumed_view = std::move(moved_token).consume();
		require_ok(consumed_view, "closure token consumed once");
		require(!moved_token.value().has_value(), "consumed token view is inaccessible");
		require_error(std::move(moved_token).consume(), "closure token replay rejected");

		bytes overflow_control = *descriptor_bytes;
		auto overflow_token = cxxlens::protocol_v2::decode_closure_control_token(
			message_type::source_closure_manifest,
			std::move(overflow_control),
			empty_payload,
			{},
			std::numeric_limits<std::size_t>::max());
		require_error(overflow_token, "closure token resident addition overflow rejected");
		require(overflow_token.error().field == "resident" &&
					overflow_token.error().detail == "overflow",
				"closure token overflow reason");
		bytes capacity_payload;
		capacity_payload.reserve(cxxlens::protocol_v2::max_closure_resident_transport_bytes);
		bytes capacity_control = *descriptor_bytes;
		auto capacity_token = cxxlens::protocol_v2::decode_closure_control_token(
			message_type::source_closure_manifest, std::move(capacity_control), capacity_payload);
		require_error(capacity_token, "actual payload capacity participates in resident budget");

		closure_control maximum_payload_control{
			source_closure_chunk{session,
								 task,
								 0U,
								 blob_digest,
								 0U,
								 0U,
								 cxxlens::protocol_v2::max_closure_chunk_payload_bytes}};
		auto maximum_payload_control_bytes = cxxlens::protocol_v2::encode_closure_control(
			message_type::source_closure_chunk, maximum_payload_control);
		require_ok(maximum_payload_control_bytes, "maximum-payload control encoding");
		bytes maximum_payload(cxxlens::protocol_v2::max_closure_chunk_payload_bytes, byte{0x5a});
		auto maximum_payload_token = cxxlens::protocol_v2::decode_closure_control_token(
			message_type::source_closure_chunk,
			std::move(*maximum_payload_control_bytes),
			maximum_payload);
		require_ok(maximum_payload_token, "maximum closure payload admitted within resident bound");
		bytes oversized_payload(cxxlens::protocol_v2::max_closure_chunk_payload_bytes + 1U,
								byte{0x5a});
		auto oversized_payload_control_bytes = cxxlens::protocol_v2::encode_closure_control(
			message_type::source_closure_chunk, maximum_payload_control);
		require_ok(oversized_payload_control_bytes, "oversized-payload control encoding");
		auto oversized_payload_token = cxxlens::protocol_v2::decode_closure_control_token(
			message_type::source_closure_chunk,
			std::move(*oversized_payload_control_bytes),
			oversized_payload);
		require_error(oversized_payload_token,
					  "oversized closure payload rejected directly by bounded token decoder");
		require(oversized_payload_token.error().field == "payload" &&
					oversized_payload_token.error().code == "source-closure.limit-exceeded",
				"oversized closure payload stable resource error");

		fail_next_allocation = true;
		auto owning_allocation_failure = cxxlens::protocol_v2::decode_closure_control(
			message_type::source_closure_manifest, *descriptor_bytes);
		const auto owning_allocation_was_injected = !fail_next_allocation;
		fail_next_allocation = false;
		require(owning_allocation_was_injected,
				"compatibility owning decoder reached its adopted-vector allocation");
		require_error(owning_allocation_failure,
					  "compatibility owning decoder translated allocation failure");
		require(owning_allocation_failure.error().field == "control" &&
					owning_allocation_failure.error().detail == "allocation",
				"compatibility owning decoder allocation failure is stable");

		closure_session session_config{
			session, task, task_digest, closure_digest, manifest_digest, 1U};
		session_config.first_sequence = 41U;
		auto transfer = cxxlens::protocol_v2::closure_transfer::create(session_config);
		require_ok(transfer, "closure transfer creation");
		require(transfer->next_sequence() == 41U, "nonzero first sequence retained");
		require_error(
			accept_adopted(
				*transfer,
				closure_frame(message_type::source_closure_manifest, 0U, descriptor_control)),
			"implicit zero sequence rejected");
		auto wrong_stream =
			closure_frame(message_type::source_closure_manifest, 41U, descriptor_control);
		wrong_stream.stream_id = 2U;
		require_error(accept_adopted(*transfer, std::move(wrong_stream)),
					  "cross-stream closure descriptor rejected");
		auto foreign_session_descriptor = descriptor;
		foreign_session_descriptor.session_id = "provider-session:sha256:" + std::string(64U, '9');
		require_error(accept_adopted(*transfer,
									 closure_frame(message_type::source_closure_manifest,
												   41U,
												   closure_control{source_closure_manifest{
													   foreign_session_descriptor}})),
					  "cross-session closure descriptor rejected");
		auto foreign_task_descriptor = descriptor;
		foreign_task_descriptor.task_id = "task:semantic-v2:sha256:" + std::string(64U, '9');
		require_error(accept_adopted(*transfer,
									 closure_frame(message_type::source_closure_manifest,
												   41U,
												   closure_control{source_closure_manifest{
													   foreign_task_descriptor}})),
					  "cross-task closure descriptor rejected");
		auto unknown_flags =
			closure_frame(message_type::source_closure_manifest, 41U, descriptor_control);
		unknown_flags.flags = 0x10U;
		require_error(accept_adopted(*transfer, std::move(unknown_flags)),
					  "closure unknown flags rejected transactionally");
		require_ok(
			accept_adopted(
				*transfer,
				closure_frame(message_type::source_closure_manifest, 41U, descriptor_control)),
			"manifest descriptor accepted");

		source_closure_manifest_chunk manifest_chunk_a{
			manifest_kind::chunk, session, task, manifest_digest, 0U, 0U, 2U};
		require_ok(
			accept_adopted(*transfer,
						   closure_frame(message_type::source_closure_manifest,
										 42U,
										 closure_control{source_closure_manifest{manifest_chunk_a}},
										 payload("ab"))),
			"manifest first chunk accepted");
		source_closure_manifest_chunk manifest_chunk_b{
			manifest_kind::chunk, session, task, manifest_digest, 1U, 2U, 1U};
		require_ok(
			accept_adopted(*transfer,
						   closure_frame(message_type::source_closure_manifest,
										 43U,
										 closure_control{source_closure_manifest{manifest_chunk_b}},
										 payload("c"))),
			"manifest final chunk accepted");

		source_closure_blob_descriptor blob{
			session, task, closure_digest, 0U, blob_digest, 3U, 2U, 2U};
		require_ok(
			accept_adopted(*transfer, closure_frame(message_type::source_closure_blob, 44U, blob)),
			"blob descriptor accepted");
		source_closure_chunk blob_chunk_a{session, task, 0U, blob_digest, 0U, 0U, 2U};
		require_ok(accept_adopted(
					   *transfer,
					   closure_frame(
						   message_type::source_closure_chunk, 45U, blob_chunk_a, payload("xy"))),
				   "blob first chunk accepted");
		source_closure_chunk blob_chunk_b{session, task, 0U, blob_digest, 1U, 2U, 1U};
		require_ok(
			accept_adopted(
				*transfer,
				closure_frame(message_type::source_closure_chunk, 46U, blob_chunk_b, payload("z"))),
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
		require_ok(
			accept_adopted(*transfer, closure_frame(message_type::source_closure_seal, 47U, seal)),
			"closure seal accepted");
		source_closure_ack ack{
			session, task, closure_digest, transfer_digest, "spool-receipt:1", "cleanup-owner:1"};
		auto forged_ack = ack;
		forged_ack.transfer_digest = semantic_digest('9');
		require_error(transfer->prepare_acknowledgement(forged_ack, 48U),
					  "ACK cross-bound to sealed transfer digest");
		fail_next_allocation = true;
		auto allocation_failed_ack = transfer->prepare_acknowledgement(ack, 48U);
		const auto ack_allocation_was_injected = !fail_next_allocation;
		fail_next_allocation = false;
		require(ack_allocation_was_injected, "ACK prepare reached a pre-wire allocation");
		require_error(allocation_failed_ack, "ACK prepare translated allocation failure");
		require(allocation_failed_ack.error().field == "ack" &&
					allocation_failed_ack.error().detail == "allocation" &&
					transfer->phase() == cxxlens::protocol_v2::closure_phase::closure_sealed,
				"ACK prepare allocation failure leaves live state unchanged");

		auto aborted_ack = transfer->prepare_acknowledgement(ack, 48U);
		require_ok(aborted_ack, "ACK fully prepared before wire emission");
		require(!aborted_ack->wire_bytes().empty() &&
					transfer->phase() == cxxlens::protocol_v2::closure_phase::closure_sealed,
				"ACK prepare owns wire without phase mutation");
		auto decoded_ack_frame = cxxlens::protocol_v2::decode_frame(aborted_ack->wire_bytes());
		require_ok(decoded_ack_frame, "prepared ACK wire decodes");
		require(decoded_ack_frame->sequence == 48U &&
					decoded_ack_frame->type == message_type::source_closure_ack,
				"prepared ACK wire identity");
		aborted_ack->abort();
		transfer->commit_acknowledgement(std::move(*aborted_ack));
		require(transfer->phase() == cxxlens::protocol_v2::closure_phase::closure_sealed,
				"failed-write abort leaves ACK phase unchanged");

		auto pre_move_ack = transfer->prepare_acknowledgement(ack, 48U);
		require_ok(pre_move_ack, "pre-move ACK preparation");
		closure_transfer moved_transfer{std::move(*transfer)};
		moved_transfer.commit_acknowledgement(std::move(*pre_move_ack));
		require(moved_transfer.phase() == cxxlens::protocol_v2::closure_phase::closure_sealed,
				"ACK capability is not transplantable across transfer move");
		auto stale_ack = moved_transfer.prepare_acknowledgement(ack, 48U);
		auto committed_ack = moved_transfer.prepare_acknowledgement(ack, 48U);
		require_ok(stale_ack, "stale ACK candidate preparation");
		require_ok(committed_ack, "committed ACK candidate preparation");
		fail_next_allocation = true;
		moved_transfer.commit_acknowledgement(std::move(*committed_ack));
		const auto ack_commit_was_allocation_free = fail_next_allocation;
		fail_next_allocation = false;
		require(ack_commit_was_allocation_free,
				"ACK commit is a fixed-state allocation-free phase flip");
		require(moved_transfer.phase() == cxxlens::protocol_v2::closure_phase::acknowledged &&
					moved_transfer.next_sequence() == 49U,
				"successful wire ACK commits by fixed no-allocation phase flip");
		moved_transfer.commit_acknowledgement(std::move(*stale_ack));
		moved_transfer.commit_acknowledgement(std::move(*committed_ack));
		require(moved_transfer.phase() == cxxlens::protocol_v2::closure_phase::acknowledged,
				"stale and replayed ACK capabilities are no-ops");
		require_error(moved_transfer.prepare_acknowledgement(ack, 49U),
					  "post-terminal ACK replay rejected");

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
#if defined(CXXLENS_PROTOCOL_V2_TSAN_UNSUPPORTED)
	// This harness deliberately replaces process-wide new/delete to inject
	// deterministic allocation faults. TSan's allocator is a competing global
	// authority; ordinary and ASan/UBSan runs cover this contract, while the
	// TSan graph records the incompatibility as an explicit skip.
	return 77;
#else
	test_sha256_and_cbor();
	test_frame_codec_and_guards();
	test_prepared_frame_decode();
	test_closure_codec_and_state();
	std::cout << "protocol-v2 direct tests passed\n";
#endif
}
