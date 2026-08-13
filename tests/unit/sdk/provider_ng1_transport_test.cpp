#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "sdk/provider_ng1_transport_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;
	using namespace cxxlens::sdk::provider::detail;

	template <typename T>
	void require(const result<T>& outcome, const std::string_view message)
	{
		if (!outcome.has_value())
		{
			std::cerr << message << " (" << outcome.error().code << ")\n";
			std::exit(1);
		}
	}

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] std::string digest(const std::string_view value)
	{
		auto output = semantic_digest("test.ng1.transport", value);
		require(output, "test semantic digest construction failed");
		return *output;
	}

	[[nodiscard]] ng1_heartbeat_control heartbeat_control()
	{
		return {"cxxlens.provider-control.heartbeat.v1",
				ng1_heartbeat_kind::ack,
				"provider:test",
				{1U, 2U, 3U},
				"session:test",
				"task:test",
				7U,
				0U,
				123U,
				0U,
				digest("staged")};
	}

	[[nodiscard]] ng1_progress_control progress_control()
	{
		return {"cxxlens.provider-control.progress.v2",
				"task:test",
				"dependency:test",
				4U,
				456U,
				5U,
				10U};
	}

	[[nodiscard]] ng1_resume_binding resume_binding()
	{
		return {"provider:test",
				{1U, 2U, 3U},
				digest("binary"),
				digest("contract"),
				"session:test",
				"task:test",
				digest("input"),
				digest("invocation"),
				digest("toolchain"),
				digest("environment"),
				digest("sandbox"),
				"dependency:test",
				"atomic:test",
				"batch:test",
				7U};
	}

	[[nodiscard]] ng1_resume_control resume_control()
	{
		ng1_resume_control output;
		output.kind = ng1_resume_kind::accepted;
		output.binding = resume_binding();
		output.highest_contiguous_acked_sequence = 4U;
		output.staged_digest = digest("staged");
		output.token_generation = 2U;
		const ng1_resume_token token{output.schema,
									 output.kind,
									 output.binding,
									 output.highest_contiguous_acked_sequence,
									 output.staged_digest,
									 output.token_generation,
									 {}};
		auto token_digest = ng1_resume_token_digest(token);
		require(token_digest, "resume token digest construction failed");
		output.token_digest = *token_digest;
		return output;
	}

	[[nodiscard]] std::vector<std::byte> cbor_text(const std::string_view value)
	{
		std::vector<std::byte> output;
		if (value.size() < 24U)
			output.push_back(static_cast<std::byte>(0x60U | value.size()));
		else
		{
			output.push_back(std::byte{0x78});
			output.push_back(static_cast<std::byte>(value.size()));
		}
		for (const auto byte : value)
			output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
		return output;
	}

	void append_text(std::vector<std::byte>& output, const std::string_view value)
	{
		const auto encoded = cbor_text(value);
		output.insert(output.end(), encoded.begin(), encoded.end());
	}

	void append_uint(std::vector<std::byte>& output, const std::uint64_t value)
	{
		require(value < 24U, "test helper only encodes small unsigned values");
		output.push_back(static_cast<std::byte>(value));
	}

	[[nodiscard]] std::size_t encoded_text_start(const std::vector<std::byte>& control,
												 const std::string_view value)
	{
		const auto encoded = cbor_text(value);
		const auto position =
			std::search(control.begin(), control.end(), encoded.begin(), encoded.end());
		require(position != control.end(), "encoded text was not found");
		return static_cast<std::size_t>(std::distance(control.begin(), position));
	}

	[[nodiscard]] std::vector<std::byte> noncanonical_progress_control()
	{
		std::vector<std::byte> output{std::byte{0xa7}};
		append_text(output, "task_id");
		append_text(output, "task:test");
		append_text(output, "schema");
		append_text(output, "cxxlens.provider-control.progress.v2");
		append_text(output, "dependency_group_id");
		append_text(output, "dependency:test");
		append_text(output, "progress_sequence");
		append_uint(output, 0U);
		append_text(output, "monotonic_time_ns");
		append_uint(output, 0U);
		append_text(output, "completed_units");
		append_uint(output, 0U);
		append_text(output, "total_units");
		append_uint(output, 1U);
		return output;
	}

	[[nodiscard]] std::vector<std::byte> duplicate_progress_control()
	{
		std::vector<std::byte> output{std::byte{0xa7}};
		append_text(output, "schema");
		append_text(output, "cxxlens.provider-control.progress.v2");
		append_text(output, "task_id");
		append_text(output, "task:test");
		append_text(output, "task_id");
		append_text(output, "task:test");
		append_text(output, "total_units");
		append_uint(output, 1U);
		append_text(output, "completed_units");
		append_uint(output, 0U);
		append_text(output, "progress_sequence");
		append_uint(output, 0U);
		append_text(output, "dependency_group_id");
		append_text(output, "dependency:test");
		return output;
	}

	void test_heartbeat_round_trip_and_validator_bridge()
	{
		const auto input = heartbeat_control();
		auto encoded = encode_ng1_heartbeat_control(input);
		require(encoded, "heartbeat encoding failed");
		auto encoded_again = encode_ng1_heartbeat_control(input);
		require(encoded_again, "heartbeat second encoding failed");
		require(*encoded == *encoded_again, "heartbeat encoding was not deterministic");
		require(!encoded->empty() && encoded->front() == std::byte{0xab},
				"heartbeat field count was not encoded exactly");

		auto decoded = decode_ng1_heartbeat_control(*encoded);
		require(decoded, "heartbeat decoding failed");
		require(*decoded == input, "heartbeat round trip changed typed values");
		auto sample = decoded->to_validation_sample(1'000U);
		require(sample, "heartbeat validator bridge failed");
		require(sample->host_receipt_time_ns == 1'000U &&
					sample->provider_monotonic_time_ns == 123U,
				"heartbeat bridge confused host receipt and provider timestamp");
		auto state = ng1_heartbeat_state::create(sample->binding, 1'000U);
		require(state, "heartbeat state creation from codec value failed");
		auto probe = *decoded;
		probe.kind = ng1_heartbeat_kind::probe;
		probe.monotonic_time_ns = 122U;
		auto probe_sample = probe.to_validation_sample(1'000U);
		require(probe_sample, "heartbeat probe validator bridge failed");
		require(state->accept(*probe_sample, 0U, input.staged_digest),
				"decoded heartbeat probe was not accepted by validator core");
		auto ack_sample = decoded->to_validation_sample(1'001U);
		require(ack_sample, "heartbeat ACK validator bridge failed");
		require(state->accept(*ack_sample, 0U, input.staged_digest),
				"decoded heartbeat was not accepted by validator core");

		auto invalid_kind = input;
		invalid_kind.kind = static_cast<ng1_heartbeat_kind>(255U);
		auto invalid_kind_result = encode_ng1_heartbeat_control(invalid_kind);
		require(!invalid_kind_result, "unknown heartbeat kind was encoded");
	}

	void test_progress_round_trip_and_validator_bridge()
	{
		const auto input = progress_control();
		auto encoded = encode_ng1_progress_control(input);
		require(encoded, "progress encoding failed");
		auto decoded = decode_ng1_progress_control(*encoded);
		require(decoded, "progress decoding failed");
		require(*decoded == input, "progress round trip changed typed values");
		auto sample = decoded->to_validation_sample(2'000U);
		require(sample, "progress validator bridge failed");
		require(sample->host_receipt_time_ns == 2'000U && sample->completed_units == 5U,
				"progress bridge lost host receipt or work count");

		auto invalid_range = input;
		invalid_range.completed_units = 11U;
		auto invalid_range_result = encode_ng1_progress_control(invalid_range);
		require(!invalid_range_result, "out-of-range progress was encoded");
	}

	void test_resume_round_trip_digest_and_validator_bridge()
	{
		const auto input = resume_control();
		auto encoded = encode_ng1_resume_control(input);
		require(encoded, "resume encoding failed");
		require(!encoded->empty() && encoded->front() == std::byte{0xb5},
				"resume field count was not encoded exactly");
		auto decoded = decode_ng1_resume_control(*encoded);
		require(decoded, "resume decoding failed");
		require(*decoded == input, "resume round trip changed typed values");
		auto token = decoded->to_validation_token();
		require(token, "resume validator bridge failed");
		require(token->token_digest == input.token_digest,
				"resume bridge did not preserve projection digest");

		auto mutated = input;
		mutated.staged_digest = digest("different-staged-prefix");
		auto mutated_result = encode_ng1_resume_control(mutated);
		require(!mutated_result, "resume projection mutation was encoded");
	}

	void test_strict_cbor_rejections()
	{
		const auto encoded = encode_ng1_progress_control(progress_control());
		require(encoded, "strict-CBOR setup encoding failed");

		auto wrong_count = *encoded;
		require(wrong_count.front() == std::byte{0xa7}, "unexpected progress map header");
		wrong_count.front() = std::byte{0xa6};
		auto wrong_count_result = decode_ng1_progress_control(wrong_count);
		require(!wrong_count_result, "wrong field count was accepted");

		auto wrong_type = *encoded;
		const auto total_key = cbor_text("total_units");
		const auto total_key_position =
			std::search(wrong_type.begin(), wrong_type.end(), total_key.begin(), total_key.end());
		require(total_key_position != wrong_type.end(), "total_units key not found");
		const auto total_value_position =
			static_cast<std::size_t>(std::distance(wrong_type.begin(), total_key_position)) +
			total_key.size();
		require(wrong_type[total_value_position] == std::byte{0x0a},
				"test did not locate the total_units unsigned value");
		wrong_type[total_value_position] = std::byte{0x60};
		auto wrong_type_result = decode_ng1_progress_control(wrong_type);
		require(!wrong_type_result, "wrong CBOR field type was accepted");

		auto duplicate = duplicate_progress_control();
		auto duplicate_result = decode_ng1_progress_control(duplicate);
		require(!duplicate_result, "duplicate CBOR map key was accepted");

		auto noncanonical_order = noncanonical_progress_control();
		auto noncanonical_order_result = decode_ng1_progress_control(noncanonical_order);
		require(!noncanonical_order_result, "non-canonical CBOR map order was accepted");

		auto nonshortest_integer = *encoded;
		const auto total_key_start = encoded_text_start(nonshortest_integer, "total_units");
		nonshortest_integer.insert(
			nonshortest_integer.begin() +
				static_cast<std::ptrdiff_t>(total_key_start + total_key.size()),
			std::byte{0x18});
		auto nonshortest_integer_result = decode_ng1_progress_control(nonshortest_integer);
		require(!nonshortest_integer_result, "non-shortest CBOR integer was accepted");

		auto nonshortest_text = *encoded;
		const auto task_value_start = encoded_text_start(nonshortest_text, "task:test");
		nonshortest_text.insert(nonshortest_text.begin() +
									static_cast<std::ptrdiff_t>(task_value_start),
								{std::byte{0x78}, std::byte{0x09}});
		auto nonshortest_text_result = decode_ng1_progress_control(nonshortest_text);
		require(!nonshortest_text_result, "non-shortest CBOR text was accepted");

		auto invalid_utf8 = *encoded;
		const auto task_value_start_for_utf8 = encoded_text_start(invalid_utf8, "task:test") + 1U;
		invalid_utf8[task_value_start_for_utf8] = std::byte{0xc0};
		auto invalid_utf8_result = decode_ng1_progress_control(invalid_utf8);
		require(!invalid_utf8_result, "invalid UTF-8 CBOR text was accepted");

		const auto heartbeat = encode_ng1_heartbeat_control(heartbeat_control());
		require(heartbeat, "heartbeat negative-vector setup encoding failed");
		auto malformed_version = *heartbeat;
		const auto version_value_start = encoded_text_start(malformed_version, "1.2.3") + 1U;
		malformed_version[version_value_start] = std::byte{'x'};
		auto malformed_version_result = decode_ng1_heartbeat_control(malformed_version);
		require(!malformed_version_result, "malformed semantic version was accepted");

		auto invalid_digest = *heartbeat;
		const auto digest_value_start =
			encoded_text_start(invalid_digest, heartbeat_control().staged_digest) + 2U;
		invalid_digest[digest_value_start + std::string_view{"semantic-v2:sha256:"}.size()] =
			std::byte{'G'};
		auto invalid_digest_result = decode_ng1_heartbeat_control(invalid_digest);
		require(!invalid_digest_result, "invalid semantic digest was accepted");

		auto invalid_identity = *heartbeat;
		const auto provider_value_start =
			encoded_text_start(invalid_identity, "provider:test") + 1U;
		invalid_identity[provider_value_start] = std::byte{0x01};
		auto invalid_identity_result = decode_ng1_heartbeat_control(invalid_identity);
		require(!invalid_identity_result, "invalid typed identity was accepted");

		auto trailing = *encoded;
		trailing.push_back(std::byte{0x00});
		auto trailing_result = decode_ng1_progress_control(trailing);
		require(!trailing_result, "trailing CBOR bytes were accepted");
	}
} // namespace

int main()
{
	test_heartbeat_round_trip_and_validator_bridge();
	test_progress_round_trip_and_validator_bridge();
	test_resume_round_trip_digest_and_validator_bridge();
	test_strict_cbor_rejections();
	return 0;
}
