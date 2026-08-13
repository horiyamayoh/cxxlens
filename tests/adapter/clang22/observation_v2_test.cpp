#include "llvm/clang22/observation_v2.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	using namespace cxxlens::detail::clang22::materialization;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] std::vector<std::byte> unhex(const std::string_view hex)
	{
		const auto nibble = [](const char value) -> unsigned char
		{
			if (value >= '0' && value <= '9')
				return static_cast<unsigned char>(value - '0');
			if (value >= 'a' && value <= 'f')
				return static_cast<unsigned char>(10 + value - 'a');
			return 0xffU;
		};
		require(hex.size() % 2U == 0U, "invalid test hex length");
		std::vector<std::byte> output;
		output.reserve(hex.size() / 2U);
		for (std::size_t index{}; index < hex.size(); index += 2U)
		{
			const auto high = nibble(hex[index]);
			const auto low = nibble(hex[index + 1U]);
			require(high != 0xffU && low != 0xffU, "invalid test hex digit");
			output.push_back(static_cast<std::byte>((high << 4U) | low));
		}
		return output;
	}

	template <class Value>
	[[nodiscard]] const Value& value(const cxxlens::sdk::detached_row& row,
									 const std::string_view suffix)
	{
		const auto found = row.cells.find(row.descriptor_id + '.' + std::string{suffix});
		require(found != row.cells.end(), "required fixture cell is absent");
		require(found->second.state == cxxlens::sdk::cell_state::present &&
					found->second.value.has_value(),
				"fixture cell is not present");
		const auto* output = std::get_if<Value>(&*found->second.value);
		require(output != nullptr, "fixture cell has the wrong scalar alternative");
		return *output;
	}

	[[nodiscard]] bool has(const cxxlens::sdk::detached_row& row, const std::string_view suffix)
	{
		return row.cells.contains(row.descriptor_id + '.' + std::string{suffix});
	}

	[[nodiscard]] const cxxlens::sdk::relation_descriptor&
	descriptor(const cxxlens::sdk::detached_row& row)
	{
		if (row.descriptor_id == entity_observation_v2_descriptor().id)
			return entity_observation_v2_descriptor();
		if (row.descriptor_id == type_observation_v2_descriptor().id)
			return type_observation_v2_descriptor();
		if (row.descriptor_id == call_observation_v2_descriptor().id)
			return call_observation_v2_descriptor();
		require(false, "test row has an unknown descriptor");
		return entity_observation_v2_descriptor();
	}

	[[nodiscard]] cxxlens::sdk::detached_row sealed(cxxlens::sdk::detached_row row)
	{
		for (const auto& column : descriptor(row).columns)
			if (!row.cells.contains(column.id))
				row.cells.emplace(column.id, cxxlens::sdk::detached_cell::absent(column.type));
		return row;
	}

	void rebind_observation_identity(cxxlens::sdk::detached_row& row)
	{
		auto identity = cxxlens::sdk::derive_domain_identity(descriptor(row), row);
		require(identity.has_value(), "test row could not derive a replacement identity");
		auto& result = row.cells.at(row.descriptor_id + ".observation");
		result.state = cxxlens::sdk::cell_state::present;
		result.value = cxxlens::sdk::scalar_value{std::move(*identity)};
		result.unknown_reason.reset();
	}

	template <class Value>
	void set_value(cxxlens::sdk::detached_row& row, const std::string_view suffix, Value value)
	{
		auto& target = row.cells.at(row.descriptor_id + '.' + std::string{suffix});
		target.state = cxxlens::sdk::cell_state::present;
		target.value = cxxlens::sdk::scalar_value{std::move(value)};
		target.unknown_reason.reset();
	}

	void set_absent(cxxlens::sdk::detached_row& row, const std::string_view suffix)
	{
		auto& target = row.cells.at(row.descriptor_id + '.' + std::string{suffix});
		target = cxxlens::sdk::detached_cell::absent(target.type);
	}

	[[nodiscard]] observation_v2_task_authority task()
	{
		return {
			.final_relation_compile_unit_id = "compile-unit:fixture",
			.source_snapshot_id = "source-snapshot:fixture",
			.source_file_id = "file:fixture",
			.source_size_bytes = 64U,
		};
	}

	[[nodiscard]] observation_v2_primary_span span()
	{
		return {
			.span_id = "source-span:sha256:"
					   "4eb3c978dc0916ef88e35cfddb457104ce08d601b9928d726808110b58725049",
			.snapshot = "source-snapshot:fixture",
			.file = "file:fixture",
			.begin = 2U,
			.end = 9U,
			.role = "spelling",
			.read_only = true,
		};
	}

	[[nodiscard]] std::vector<observation_v2_origin> duplicate_origins()
	{
		return {
			{"macro", "project://include/a.hpp", 3, 8, true},
			{"macro", "project://include/a.hpp", 3, 8, true},
		};
	}

	[[nodiscard]] native_observation_v2 fixture(const observation_v2_kind kind)
	{
		return {
			.kind = kind,
			.final_relation_compile_unit_id = "compile-unit:fixture",
			.semantic_key = "関数::f",
			.payload = {{"display", "f(雪)"}, {"kind", "call"}, {"雪", "値"}},
			.primary_span = kind == observation_v2_kind::type
				? std::nullopt
				: std::optional<observation_v2_primary_span>{span()},
			.origin_chain = kind == observation_v2_kind::type ? std::vector<observation_v2_origin>{}
															  : duplicate_origins(),
			.exact_equivalence = false,
			.limitation = "fixture limitation",
		};
	}

	void descriptor_oracle()
	{
		struct expected_binding
		{
			const cxxlens::sdk::relation_descriptor* descriptor;
			std::string_view id;
			std::string_view contract;
			std::string_view runtime;
		};
		const std::array bindings{
			expected_binding{
				&entity_observation_v2_descriptor(),
				"frontend.clang22.entity_observation.v2",
				"sha256:4a5012801fcde26110a9f6350177d74d7d6975edde96337d4d3918ca7a004d51",
				"semantic-v2:sha256:"
				"eb909eec97cec22586f4ac67dc7c56cc29390857df9355186feae5e9ce7700fb"},
			expected_binding{
				&type_observation_v2_descriptor(),
				"frontend.clang22.type_observation.v2",
				"sha256:53c54f967eb041e75ea98463c212d259fed0d3a310038ac9c93209749e72387f",
				"semantic-v2:sha256:"
				"94b6f6efcd46dad74c0cec1c761a2d363c6acdfe135862c37d0b7e28b01b6026"},
			expected_binding{
				&call_observation_v2_descriptor(),
				"frontend.clang22.call_observation.v2",
				"sha256:07ea48a7f00e80972ba59c14ee96f916772ad9ed57fc84e313e3958f08fa548a",
				"semantic-v2:sha256:"
				"8b79a9fb3d59e750c51310d6f32935701a36c68fd5830228516482b0e7d2cd65"},
		};
		for (const auto& binding : bindings)
		{
			require(binding.descriptor->id == binding.id, "descriptor ID differs from Registry");
			require(binding.descriptor->contract_digest == binding.contract,
					"descriptor contract digest differs from Python oracle");
			require(binding.descriptor->descriptor_digest == binding.runtime,
					"runtime descriptor digest differs from Python oracle");
			require(binding.descriptor->validate().has_value(),
					"exact Registry descriptor does not validate");
			require(binding.descriptor->contract_canonical.find("observation-native.v2") !=
						std::string::npos,
					"native v2 contract binding is absent");
		}
		auto invalid = observation_v2_descriptor(static_cast<observation_v2_kind>(255U));
		require(!invalid.has_value() && invalid.error().field == "kind",
				"unknown observation kind was accepted");
	}

	void row_oracle()
	{
		struct expected_row
		{
			observation_v2_kind kind;
			std::string_view payload_digest;
			std::string_view observation_id;
		};
		const std::array rows{
			expected_row{observation_v2_kind::entity,
						 "semantic-v2:sha256:"
						 "b279d787f60c4d394079ee1efe8191fa422b4a28c861039a0850766da9d7ed2a",
						 "clang22-observation:sha256:"
						 "d1a2ee1f2c5f8565342c2bcb6359bcdbed2e88e82adaf7c7bda5fd20efeae854"},
			expected_row{observation_v2_kind::type,
						 "semantic-v2:sha256:"
						 "32b32d9d7cd8f2cfb4ed3745f613066aebf8d6ae643d7dde2f38b6a1a5f44991",
						 "clang22-observation:sha256:"
						 "37ac6f8425163366a02f55ebb68a7d7bb9fe6dda0befc3fcd3ce0347d268654a"},
			expected_row{observation_v2_kind::call,
						 "semantic-v2:sha256:"
						 "ff39f74cf770a67cde14ec5c1a61a30a2a986add382515fd5a2c754e484e8eef",
						 "clang22-observation:sha256:"
						 "a385415d347168009bab77ef7b2f57427a8602efa5bd87304156275d476ec03e"},
		};
		constexpr std::string_view origin_hex =
			"050000000000000002000000000000002f0400000000000000266378786c656e732e636c616e6732322e73"
			"6f757263652d6f726967696e2d636861696e2e763200000000000001070500000000000000020000000000"
			"000077050000000000000005000000000000000e0400000000000000056d6163726f000000000000002004"
			"000000000000001770726f6a6563743a2f2f696e636c7564652f612e687070000000000000000b02000000"
			"00000000000103000000000000000b02000000000000000001080000000000000002010100000000000000"
			"77050000000000000005000000000000000e0400000000000000056d6163726f0000000000000020040000"
			"00000000001770726f6a6563743a2f2f696e636c7564652f612e687070000000000000000b020000000000"
			"0000000103000000000000000b020000000000000000010800000000000000020101";

		for (const auto& expected : rows)
		{
			auto built = make_observation_v2_row(fixture(expected.kind), task());
			require(built.has_value(),
					built ? "" : std::string{"row construction failed: "} + built.error().detail);
			const auto& row = *built;
			require(value<std::string>(row, "payload_digest") == expected.payload_digest,
					"payload digest differs from Python oracle");
			require(value<std::string>(row, "observation") == expected.observation_id,
					"Registry domain identity differs from Python oracle");
			require(value<std::vector<std::byte>>(row, "semantic_key") ==
						unhex("e996a2e695b03a3a66"),
					"semantic key bytes were normalized or hashed");
			require(!value<bool>(row, "exact_equivalence") &&
						value<std::string>(row, "limitation") == "fixture limitation",
					"exact-equivalence limitation coupling was not retained");
			if (expected.kind == observation_v2_kind::type)
			{
				for (const auto suffix : {"source",
										  "source_snapshot",
										  "source_file",
										  "source_begin",
										  "source_end",
										  "source_role",
										  "source_read_only",
										  "source_origin_chain"})
					require(!has(row, suffix), "type observation retained source authority");
			}
			else
			{
				for (const auto suffix : {"source",
										  "source_snapshot",
										  "source_file",
										  "source_begin",
										  "source_end",
										  "source_role",
										  "source_read_only"})
					require(has(row, suffix), "primary span bundle was not all-or-none");
				const auto& origins = value<std::vector<std::byte>>(row, "source_origin_chain");
				require(origins == unhex(origin_hex), "origin bytes differ from Python oracle");
				auto decoded = cxxlens::sdk::canonical_binary_decode(origins);
				require(decoded.has_value() && decoded->tuple.size() == 2U &&
							decoded->tuple[1].tuple.size() == 2U &&
							decoded->tuple[1].tuple[0] == decoded->tuple[1].tuple[1],
						"duplicate origin occurrences were not preserved");
			}
			const auto& descriptor = expected.kind == observation_v2_kind::entity
				? entity_observation_v2_descriptor()
				: expected.kind == observation_v2_kind::type ? type_observation_v2_descriptor()
															 : call_observation_v2_descriptor();
			require(cxxlens::sdk::validate_domain_identity(descriptor, row).has_value(),
					"independent Registry identity validation failed");
			auto tampered = row;
			tampered.cells.at(row.descriptor_id + ".observation").value =
				cxxlens::sdk::scalar_value{std::string{"clang22-observation:tampered"}};
			require(!cxxlens::sdk::validate_domain_identity(descriptor, tampered),
					"tampered observation identity was accepted");
		}
	}

	void strict_inverse_round_trip()
	{
		for (const auto kind :
			 {observation_v2_kind::entity, observation_v2_kind::type, observation_v2_kind::call})
		{
			const auto native = fixture(kind);
			auto built = make_observation_v2_row(native, task());
			require(built.has_value(), "inverse round-trip fixture construction failed");
			auto sealed_row = sealed(std::move(*built));
			auto decoded = decode_observation_v2_row(sealed_row, task());
			require(decoded.has_value(),
					decoded ? ""
							: std::string{"strict inverse failed: "} + decoded.error().code + '/' +
							decoded.error().field + '/' + decoded.error().detail);
			require(decoded->kind == native.kind &&
						decoded->final_relation_compile_unit_id ==
							native.final_relation_compile_unit_id &&
						decoded->semantic_key == native.semantic_key &&
						decoded->payload_digest ==
							value<std::string>(sealed_row, "payload_digest") &&
						decoded->primary_span == native.primary_span &&
						decoded->origin_chain == native.origin_chain &&
						decoded->exact_equivalence == native.exact_equivalence &&
						decoded->limitation == native.limitation,
					"strict inverse lost typed sealed-row authority");
		}
	}

	void strict_inverse_rejections()
	{
		const auto make_row = [](const observation_v2_kind kind)
		{
			auto built = make_observation_v2_row(fixture(kind), task());
			require(built.has_value(), "inverse rejection fixture construction failed");
			return sealed(std::move(*built));
		};
		const auto reject_row =
			[](const cxxlens::sdk::detached_row& row, const std::string_view message)
		{
			require(!decode_observation_v2_row(row, task()).has_value(), message);
		};

		auto row = make_row(observation_v2_kind::call);
		row.descriptor_id = "frontend.clang22.call_observation.v1";
		reject_row(row, "legacy or unknown observation descriptor was accepted");

		row = make_row(observation_v2_kind::call);
		set_value(row, "observation", std::string{"clang22-observation:tampered"});
		reject_row(row, "tampered observation domain identity was accepted");

		row = make_row(observation_v2_kind::call);
		set_value(row, "compile_unit", std::string{"compile-unit:other"});
		rebind_observation_identity(row);
		reject_row(row, "self-consistent cross-task compile unit was accepted");

		row = make_row(observation_v2_kind::call);
		set_value(row, "semantic_key", std::vector<std::byte>{std::byte{0xc0U}});
		rebind_observation_identity(row);
		reject_row(row, "invalid UTF-8 semantic-key bytes were accepted");

		row = make_row(observation_v2_kind::call);
		set_value(row, "payload_digest", std::string{"sha256:"} + std::string(64U, '0'));
		rebind_observation_identity(row);
		reject_row(row, "non-semantic payload digest spelling was accepted");

		row = make_row(observation_v2_kind::call);
		set_absent(row, "source_end");
		rebind_observation_identity(row);
		reject_row(row, "partial primary-span bundle was accepted");

		row = make_row(observation_v2_kind::call);
		set_value(row, "source", std::string{"source-span:other"});
		rebind_observation_identity(row);
		reject_row(row, "source.span base-row identity drift was accepted");

		row = make_row(observation_v2_kind::call);
		set_value(row, "source_snapshot", std::string{"source-snapshot:other"});
		auto rebound_span = cxxlens::sdk::source_span_identity(
			"source-snapshot:other", "file:fixture", 2U, 9U, "spelling");
		require(rebound_span.has_value(), "alternate span identity fixture failed");
		set_value(row, "source", std::move(*rebound_span));
		rebind_observation_identity(row);
		reject_row(row, "self-consistent cross-task source base row was accepted");

		row = make_row(observation_v2_kind::call);
		auto origin_encoding = value<std::vector<std::byte>>(row, "source_origin_chain");
		origin_encoding.push_back(std::byte{});
		set_value(row, "source_origin_chain", std::move(origin_encoding));
		rebind_observation_identity(row);
		reject_row(row, "origin bytes with trailing data were accepted");

		row = make_row(observation_v2_kind::call);
		auto decoded_origins = cxxlens::sdk::canonical_binary_decode(
			value<std::vector<std::byte>>(row, "source_origin_chain"));
		require(decoded_origins.has_value(), "origin mutation fixture did not decode");
		decoded_origins->tuple[0U].text = "cxxlens.clang22.source-origin-chain.v1";
		auto wrong_domain = cxxlens::sdk::canonical_binary(*decoded_origins);
		require(wrong_domain.has_value(), "origin domain mutation did not encode");
		set_value(row, "source_origin_chain", std::move(*wrong_domain));
		rebind_observation_identity(row);
		reject_row(row, "legacy origin projection domain was accepted");

		row = make_row(observation_v2_kind::call);
		decoded_origins = cxxlens::sdk::canonical_binary_decode(
			value<std::vector<std::byte>>(row, "source_origin_chain"));
		require(decoded_origins.has_value(), "empty-origin mutation fixture did not decode");
		decoded_origins->tuple[1U].tuple.clear();
		auto empty_present = cxxlens::sdk::canonical_binary(*decoded_origins);
		require(empty_present.has_value(), "empty-origin mutation did not encode");
		set_value(row, "source_origin_chain", std::move(*empty_present));
		rebind_observation_identity(row);
		reject_row(row, "present encoding of an empty origin chain was accepted");

		row = make_row(observation_v2_kind::call);
		set_value(row, "exact_equivalence", true);
		reject_row(row, "exact row with a limitation was accepted by inverse decoder");

		row = make_row(observation_v2_kind::call);
		set_absent(row, "limitation");
		reject_row(row, "inexact row without a limitation was accepted by inverse decoder");

		row = make_row(observation_v2_kind::call);
		auto& exact = row.cells.at(row.descriptor_id + ".exact_equivalence");
		exact = cxxlens::sdk::detached_cell::unknown(exact.type, "fixture-unknown");
		reject_row(row, "unknown exact-equivalence state was accepted");
	}

	void reject(native_observation_v2 record, const std::string_view message)
	{
		auto built = make_observation_v2_row(record, task());
		require(!built.has_value(), message);
	}

	void native_domain_rejections()
	{
		auto record = fixture(observation_v2_kind::entity);
		record.final_relation_compile_unit_id = "other-unit";
		reject(record, "task-mismatched compile unit was accepted");

		record = fixture(observation_v2_kind::entity);
		record.semantic_key.clear();
		reject(record, "empty semantic key was accepted");
		record.semantic_key = std::string{1U, static_cast<char>(0xc0U)};
		reject(record, "invalid UTF-8 semantic key was accepted");

		record = fixture(observation_v2_kind::entity);
		record.payload = {{"", "value"}};
		reject(record, "empty payload key was accepted");
		record.payload = {{"same", "one"}, {"same", "two"}};
		reject(record, "duplicate payload key was accepted");
		record.payload = {{"雪", "value"}, {"a", "value"}};
		reject(record, "payload entries outside UTF-8 byte order were accepted");
		record.payload = {{std::string{1U, static_cast<char>(0xc0U)}, "value"}};
		reject(record, "invalid UTF-8 payload key was accepted");
		record.payload = {{"key", std::string{1U, static_cast<char>(0xc0U)}}};
		reject(record, "invalid UTF-8 payload value was accepted");

		record = fixture(observation_v2_kind::entity);
		record.exact_equivalence = true;
		reject(record, "exact observation with a limitation was accepted");
		record = fixture(observation_v2_kind::entity);
		record.exact_equivalence = false;
		record.limitation.reset();
		reject(record, "inexact observation without a limitation was accepted");
		record.limitation = "";
		reject(record, "empty limitation was accepted");

		record = fixture(observation_v2_kind::type);
		record.primary_span = span();
		reject(record, "type observation with a primary span was accepted");
		record.primary_span.reset();
		record.origin_chain = duplicate_origins();
		reject(record, "type observation with an origin chain was accepted");
	}

	void span_and_origin_rejections()
	{
		auto record = fixture(observation_v2_kind::call);
		record.primary_span->span_id = "source-span:wrong";
		reject(record, "non-derived source span ID was accepted");
		record = fixture(observation_v2_kind::call);
		record.primary_span->snapshot = "source-snapshot:other";
		reject(record, "task-mismatched source snapshot was accepted");
		record = fixture(observation_v2_kind::call);
		record.primary_span->end = 65U;
		reject(record, "span outside task source was accepted");
		record = fixture(observation_v2_kind::call);
		record.primary_span->begin = 10U;
		record.primary_span->end = 9U;
		reject(record, "reversed primary span was accepted");

		auto large_task = task();
		large_task.source_size_bytes = std::numeric_limits<std::uint64_t>::max();
		record = fixture(observation_v2_kind::call);
		record.primary_span->begin =
			static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U;
		record.primary_span->end = record.primary_span->begin;
		auto built = make_observation_v2_row(record, large_task);
		require(!built.has_value(),
				"primary span outside signed-int64 identity domain was accepted");

		record = fixture(observation_v2_kind::call);
		record.origin_chain.front().kind.clear();
		reject(record, "empty origin kind was accepted");
		record = fixture(observation_v2_kind::call);
		record.origin_chain.front().logical_path.clear();
		reject(record, "empty origin path was accepted");
		record = fixture(observation_v2_kind::call);
		record.origin_chain.front().begin = -1;
		reject(record, "negative origin range was accepted");
		record = fixture(observation_v2_kind::call);
		record.origin_chain.front().end = 2;
		reject(record, "reversed origin range was accepted");
		record = fixture(observation_v2_kind::call);
		record.origin_chain.front().read_only = false;
		reject(record, "writable origin was accepted");
		record = fixture(observation_v2_kind::call);
		record.origin_chain.front().logical_path = std::string{1U, static_cast<char>(0xc0U)};
		reject(record, "invalid UTF-8 origin path was accepted");

		record = fixture(observation_v2_kind::call);
		record.origin_chain = {{"outer", "project://outer.hpp", 0, 1, true},
							   {"inner", "project://inner.hpp", 2, 3, true}};
		auto forward = make_observation_v2_row(record, task());
		require(forward.has_value(), "ordered origin fixture was rejected");
		std::swap(record.origin_chain[0], record.origin_chain[1]);
		auto reversed = make_observation_v2_row(record, task());
		require(reversed.has_value() &&
					value<std::vector<std::byte>>(*forward, "source_origin_chain") !=
						value<std::vector<std::byte>>(*reversed, "source_origin_chain") &&
					value<std::string>(*forward, "observation") !=
						value<std::string>(*reversed, "observation"),
				"origin order was normalized or discarded");

		record = fixture(observation_v2_kind::entity);
		record.primary_span->read_only = false;
		auto writable_primary = make_observation_v2_row(record, task());
		require(writable_primary.has_value() && !value<bool>(*writable_primary, "source_read_only"),
				"boolean primary-span read_only authority was incorrectly forced true");
	}

	void optional_authority()
	{
		auto record = fixture(observation_v2_kind::entity);
		record.primary_span.reset();
		record.origin_chain.clear();
		record.exact_equivalence = true;
		record.limitation.reset();
		record.payload.clear();
		auto built = make_observation_v2_row(record, task());
		require(built.has_value(), "valid absent optional authority was rejected");
		for (const auto suffix : {"source",
								  "source_snapshot",
								  "source_file",
								  "source_begin",
								  "source_end",
								  "source_role",
								  "source_read_only",
								  "source_origin_chain",
								  "limitation"})
			require(!has(*built, suffix), "absent optional field was emitted as a value");
		require(value<bool>(*built, "exact_equivalence"), "exact equivalence was not retained");
		auto decoded_direct = decode_observation_v2_row(*built, task());
		auto decoded_sealed = decode_observation_v2_row(sealed(*built), task());
		require(decoded_direct.has_value() && decoded_sealed.has_value() &&
					!decoded_direct->primary_span && decoded_direct->origin_chain.empty() &&
					decoded_direct->exact_equivalence && !decoded_direct->limitation &&
					*decoded_direct == *decoded_sealed,
				"inverse decoder did not preserve absent optional authority");
	}
} // namespace

int main()
{
	descriptor_oracle();
	row_oracle();
	strict_inverse_round_trip();
	strict_inverse_rejections();
	native_domain_rejections();
	span_and_origin_rejections();
	optional_authority();
	return 0;
}
