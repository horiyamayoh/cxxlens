#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "sqlite_payload_streaming_internal.hpp"
#include "sqlite_wave3_wal_recovery_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;

	sqlite_backend_opaque_identity identity(const char marker)
	{
		return {"sqlite-wave3-test-v1", {static_cast<std::byte>(marker)}};
	}

	sqlite_wave3_wal_source_identity source()
	{
		return {identity('p'), identity('r'), identity('f'), identity('o'), identity('n'), 7};
	}

	std::string digest(const char marker)
	{
		return std::string(64, marker);
	}

	std::string prefix_digest(const std::size_t count, const std::byte value)
	{
		std::vector<std::byte> bytes(count, value);
		sqlite_incremental_sha256 checksum;
		if (!checksum.update(bytes))
			throw std::runtime_error{"prefix digest update failed"};
		auto finished = checksum.finish();
		if (!finished)
			throw std::runtime_error{"prefix digest finish failed"};
		return finished->substr(7U);
	}

	sqlite_wave3_wal_recovery_input active_input()
	{
		return {source(),
				4096,
				7,
				digest('a'),
				prefix_digest(7U, std::byte{0x5a}),
				sqlite_wave3_wal_state::valid_nonzero,
				0,
				true,
				false,
				false,
				true,
				7,
				64U * 1024U};
	}

	void require(const bool condition, const char* message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	void require_ok(const result<void>& value, const char* message)
	{
		require(value.has_value(), message);
	}

	void test_private_and_session()
	{
		auto input = active_input();
		auto plan_result = plan_sqlite_wave3_wal_recovery(input);
		require(plan_result.has_value(), "private WAL plan failed");
		require(plan_result->route == sqlite_wave3_wal_recovery_route::private_heap_index &&
					plan_result->source_zero_effect_required &&
					plan_result->eager_decode_required && plan_result->close_before_receipt,
				"private WAL route lost safety flags");

		auto session_result = sqlite_wave3_wal_recovery_session::open(input);
		require(session_result.has_value(), "private session open failed");
		auto session = std::move(session_result.value());
		std::vector<std::byte> prefix(7, std::byte{0x5a});
		require_ok(session.seal_prefix(prefix), "prefix seal failed");
		require(session.sealed_prefix().size() == 7 &&
					session.phase() == sqlite_wave3_wal_recovery_phase::prefix_sealed,
				"prefix was not retained");
		require_ok(session.mark_decoded_candidate(), "decode candidate failed");
		require_ok(session.revoke(), "session revoke failed");
		require_ok(session.close(), "session close failed");
		require(session.phase() == sqlite_wave3_wal_recovery_phase::closed,
				"closed session is not terminal");
		require(!session.quarantine("late-fault"), "closed session was quarantined");

		auto chunks = chunk_sqlite_wave3_wal_prefix(prefix, 3);
		require(chunks.has_value() && chunks->size() == 3 &&
					(*chunks)[0] == sqlite_wave3_wal_recovery_chunk{0, 3} &&
					(*chunks)[1] == sqlite_wave3_wal_recovery_chunk{3, 3} &&
					(*chunks)[2] == sqlite_wave3_wal_recovery_chunk{6, 1},
				"WAL chunking is not deterministic");
	}

	void test_mapped_and_main_only_routes()
	{
		auto mapped = active_input();
		mapped.native_readonly_cantinit = false;
		mapped.native_readonly_mapping = true;
		mapped.read_lock_index = 2;
		auto mapped_plan = plan_sqlite_wave3_wal_recovery(mapped);
		require(mapped_plan.has_value() &&
					mapped_plan->route == sqlite_wave3_wal_recovery_route::native_readonly_mapping,
				"native read-only mapping route failed");
		auto out_of_range_lock = mapped;
		out_of_range_lock.read_lock_index = 5;
		auto out_of_range_plan = plan_sqlite_wave3_wal_recovery(out_of_range_lock);
		require(!out_of_range_plan &&
					out_of_range_plan.error().detail == "native-mapping-lock-missing",
				"out-of-range native mapping lock was accepted");

		sqlite_wave3_wal_recovery_input main_only{source(),
												  4096,
												  0,
												  digest('a'),
												  {},
												  sqlite_wave3_wal_state::absent,
												  -1,
												  false,
												  false,
												  false,
												  true,
												  0,
												  64U * 1024U};
		auto main_plan = plan_sqlite_wave3_wal_recovery(main_only);
		require(main_plan.has_value() &&
					main_plan->route == sqlite_wave3_wal_recovery_route::main_only,
				"main-only route failed");
		auto main_session_result = sqlite_wave3_wal_recovery_session::open(main_only);
		require(main_session_result.has_value(), "main-only session failed");
		auto main_session = std::move(main_session_result.value());
		require_ok(main_session.seal_prefix({}), "empty prefix seal failed");
		require_ok(main_session.mark_decoded_candidate(), "main-only candidate failed");
		require_ok(main_session.revoke(), "main-only revoke failed");
		require_ok(main_session.close(), "main-only close failed");
	}

	void test_negative_and_resource_paths()
	{
		auto mutation = active_input();
		mutation.source_mutation_permitted = true;
		auto mutation_result = plan_sqlite_wave3_wal_recovery(mutation);
		require(!mutation_result && mutation_result.error().detail == "source-mutation-permitted",
				"source mutation was admitted");

		auto lock_drift = active_input();
		lock_drift.read_lock_index = 1;
		auto lock_result = plan_sqlite_wave3_wal_recovery(lock_drift);
		require(!lock_result && lock_result.error().detail == "private-index-lock-mismatch",
				"private index lock drift was admitted");

		auto no_route = active_input();
		no_route.native_readonly_cantinit = false;
		no_route.read_lock_index = -1;
		auto no_route_result = plan_sqlite_wave3_wal_recovery(no_route);
		require(!no_route_result && no_route_result.error().detail == "native-route-missing",
				"unproven WAL route was admitted");

		auto bad_digest = active_input();
		bad_digest.wal_digest = "not-a-digest";
		auto bad_digest_result = plan_sqlite_wave3_wal_recovery(bad_digest);
		require(!bad_digest_result && bad_digest_result.error().detail == "valid-wal-inconsistent",
				"invalid digest was admitted");

		auto oversized = active_input();
		oversized.max_copy_bytes = 4;
		auto oversized_result = plan_sqlite_wave3_wal_recovery(oversized);
		require(!oversized_result && oversized_result.error().detail == "copy-bound-exceeded",
				"copy bound was ignored");

		auto prefix_session_result = sqlite_wave3_wal_recovery_session::open(active_input());
		require(prefix_session_result.has_value(), "prefix digest setup failed");
		auto prefix_session = std::move(prefix_session_result.value());
		std::vector<std::byte> tampered_prefix(7, std::byte{0x5b});
		auto tampered = prefix_session.seal_prefix(tampered_prefix);
		require(!tampered && tampered.error().detail == "prefix-digest-mismatch",
				"tampered WAL prefix was accepted");

		auto quarantine_result = sqlite_wave3_wal_recovery_session::open(active_input());
		require(quarantine_result.has_value(), "quarantine setup failed");
		auto quarantined = std::move(quarantine_result.value());
		require_ok(quarantined.quarantine("native-indeterminate"), "quarantine failed");
		require(quarantined.phase() == sqlite_wave3_wal_recovery_phase::quarantined &&
					!quarantined.close() && !quarantined.seal_prefix({}),
				"quarantine allowed a later transition");

		std::vector<std::byte> too_many_chunks(4097, std::byte{0});
		auto chunks = chunk_sqlite_wave3_wal_prefix(too_many_chunks, 1);
		require(!chunks && chunks.error().detail == "chunk-count-exceeded",
				"chunk resource bound was ignored");
		require(!chunk_sqlite_wave3_wal_prefix({}, 0), "zero chunk bound was accepted");
	}
} // namespace

int main()
{
	try
	{
		test_private_and_session();
		test_mapped_and_main_only_routes();
		test_negative_and_resource_paths();
	}
	catch (const std::exception& exception)
	{
		std::cerr << "sqlite_wave3_wal_recovery_test: " << exception.what() << '\n';
		return 1;
	}
	return 0;
}
