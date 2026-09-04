#include "sdk/detached_provider_run_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace
{
	template <class value_type>
	void require(const value_type& condition)
	{
		if (!static_cast<bool>(condition))
			std::abort();
	}

	[[nodiscard]] std::string digest(const char digit)
	{
		return "sha256:" + std::string(64U, digit);
	}

	[[nodiscard]] cxxlens::sdk::detail::detached_provider_run_draft draft()
	{
		using namespace cxxlens::sdk::detail;
		detached_provider_run_draft value;
		value.task_id = "task:detached-msvc-main";
		value.task_input_digest = digest('1');
		value.replay_plan_digest = digest('2');
		value.provider = {"cxxlens.clangcl23-msvc-replay",
						  {1U, 0U, 0U},
						  digest('3'),
						  "semantic-v2:" + digest('4'),
						  digest('5'),
						  "not-revoked",
						  digest('6')};
		value.protocol_transcript = {std::byte{0x43}, std::byte{0x58}, std::byte{0x4c}};
		value.terminal = detached_provider_terminal::partial;
		value.partitions = {
			{"source.span.v1",
			 "relation-descriptor:" + digest('7'),
			 "clangcl23-msvc-replay",
			 "atomic:source-span",
			 "batch:source-span",
			 "batch-digest:" + digest('8'),
			 4U},
			{"cc.entity.v1",
			 "relation-descriptor:" + digest('9'),
			 "clangcl23-msvc-replay",
			 "atomic:cc-entity",
			 "batch:cc-entity",
			 "batch-digest:" + digest('a'),
			 3U},
		};
		value.coverage = {
			{"relation", "source.span.v1", "covered", "frontend-observed"},
			{"relation", "cc.entity.v1", "unresolved", "replay-gap"},
		};
		value.unresolved = {
			{"application-analysis.capture-gap", "cc.entity.v1", "unsupported-option"},
			{"application-analysis.capture-gap", "build.variant.v1", "environment-redacted"},
		};
		value.provenance = {
			{"application-analysis.replay", "compile-unit:main", "clang-cl-23.1.0", digest('b')},
			{"application-analysis.capture", "compile-unit:main", "cl.exe-14.51", digest('c')},
		};
		value.runtime_receipt_digest = "provider-runtime-receipt:" + digest('d');
		return value;
	}

	void canonical_round_trip_is_deterministic()
	{
		using namespace cxxlens::sdk::detail;
		auto first = validate_detached_provider_run(draft());
		auto permuted = draft();
		std::ranges::reverse(permuted.partitions);
		std::ranges::reverse(permuted.coverage);
		std::ranges::reverse(permuted.unresolved);
		std::ranges::reverse(permuted.provenance);
		auto second = validate_detached_provider_run(std::move(permuted));
		require(first && second && std::ranges::equal(first->bytes(), second->bytes()) &&
				first->digest() == second->digest());
		auto decoded = decode_detached_provider_run(first->bytes());
		require(decoded && decoded->value() == first->value() &&
				decoded->digest() == first->digest() &&
				decoded->value().partitions.front().descriptor_id == "cc.entity.v1" &&
				decoded->value().coverage.front().id == "cc.entity.v1");
	}

	void malformed_and_noncanonical_wire_fail_closed()
	{
		using namespace cxxlens::sdk;
		using namespace cxxlens::sdk::detail;
		auto value = validate_detached_provider_run(draft());
		require(value);
		auto truncated = std::vector<std::byte>{value->bytes().begin(), value->bytes().end()};
		truncated.pop_back();
		require(!decode_detached_provider_run(truncated));
		auto trailing = std::vector<std::byte>{value->bytes().begin(), value->bytes().end()};
		trailing.push_back(std::byte{});
		require(!decode_detached_provider_run(trailing));

		auto root = canonical_binary_decode(value->bytes());
		require(root);
		std::ranges::reverse(root->tuple[7].tuple);
		auto reordered = canonical_binary(*root);
		require(reordered);
		auto rejected = decode_detached_provider_run(*reordered);
		require(!rejected && rejected.error().detail == "noncanonical-projection-order");
		root = canonical_binary_decode(value->bytes());
		require(root);
		root->tuple[0] = canonical_value::from_string("cxxlens.detached-provider-run.v2");
		auto wrong_schema = canonical_binary(*root);
		require(wrong_schema && !decode_detached_provider_run(*wrong_schema));
	}

	void terminal_projection_and_identity_rules_are_fail_closed()
	{
		using namespace cxxlens::sdk::detail;
		auto failed = draft();
		failed.terminal = detached_provider_terminal::failed;
		auto publication = validate_detached_provider_run(std::move(failed));
		require(!publication && publication.error().detail == "publication-candidate-forbidden");

		auto terminal_only = draft();
		terminal_only.terminal = detached_provider_terminal::failed;
		terminal_only.partitions.clear();
		terminal_only.runtime_receipt_digest.reset();
		auto admitted_terminal = validate_detached_provider_run(std::move(terminal_only));
		require(admitted_terminal);

		auto empty_partial = draft();
		empty_partial.partitions.clear();
		empty_partial.unresolved.clear();
		auto partial = validate_detached_provider_run(std::move(empty_partial));
		require(!partial && partial.error().detail == "empty-partial");

		auto forged_digest = draft();
		forged_digest.provider.binary_digest.back() = 'x';
		auto digest_result = validate_detached_provider_run(std::move(forged_digest));
		require(!digest_result && digest_result.error().field == "digest");

		auto duplicate = draft();
		duplicate.coverage.push_back(duplicate.coverage.front());
		auto duplicate_result = validate_detached_provider_run(std::move(duplicate));
		require(!duplicate_result && duplicate_result.error().detail == "duplicate");
	}

	void resource_limits_are_checked_before_canonical_allocation()
	{
		using namespace cxxlens::sdk;
		using namespace cxxlens::sdk::detail;
		auto value = validate_detached_provider_run(draft());
		require(value);
		import_limits byte_limits;
		byte_limits.maximum_bundle_bytes = value->bytes().size() - 1U;
		auto bounded = decode_detached_provider_run(value->bytes(), byte_limits);
		require(!bounded && bounded.error().field == "detached_provider_run");

		auto rows = draft();
		rows.partitions.front().row_count = 100000U;
		auto row_limit = validate_detached_provider_run(std::move(rows));
		require(!row_limit && row_limit.error().field == "partitions.rows");

		auto protocol = draft();
		protocol.protocol_transcript.resize(std::size_t{16U} * 1024U * 1024U + 1U);
		auto protocol_limit = validate_detached_provider_run(std::move(protocol));
		require(!protocol_limit && protocol_limit.error().field == "protocol_transcript");

		auto root = canonical_binary_decode(value->bytes());
		require(root);
		root->tuple[8] = canonical_value::from_tuple(
			std::vector<canonical_value>(10001U, canonical_value::null()));
		auto oversized_count = canonical_binary(*root);
		require(oversized_count);
		auto preflight = decode_detached_provider_run(*oversized_count);
		require(!preflight && preflight.error().field == "binary" &&
				preflight.error().detail == "tuple-items");
	}
} // namespace

int main()
{
	canonical_round_trip_is_deterministic();
	malformed_and_noncanonical_wire_fail_closed();
	terminal_projection_and_identity_rules_are_fail_closed();
	resource_limits_are_checked_before_canonical_allocation();
}
