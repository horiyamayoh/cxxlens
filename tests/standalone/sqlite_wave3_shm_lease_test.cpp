#include "sqlite_wave3_shm_lease_internal.hpp"

#include <cstddef>
#include <iostream>
#include <stdexcept>

namespace
{
	using namespace cxxlens::sdk;

	sqlite_backend_opaque_identity identity(const char marker)
	{
		return {"sqlite-wave3-test-v1", {static_cast<std::byte>(marker)}};
	}

	sqlite_wave3_identity_binding binding(const std::uint64_t generation = 11)
	{
		return {identity('p'), identity('r'), identity('f'), identity('l'), identity('o'),
			identity('n'), identity('d'), identity('s'), identity('e'), identity('y'), identity('m'),
			7, generation, 0, 4096};
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

	void test_lifecycle_and_tombstone()
	{
		sqlite_wave3_shm_lease_registry registry;
		auto first_result = registry.reserve(binding());
		require(first_result.has_value(), "first reservation failed");
		auto lease = std::move(first_result.value());
		require(lease.valid() && lease.token() == 1 &&
			lease.phase() == sqlite_wave3_lease_phase::reserved,
		"reservation did not mint a lease");
		require(registry.contains(binding()) && registry.size() == 1,
			"registry lost reservation");

		auto duplicate = registry.reserve(binding());
		require(!duplicate && duplicate.error().detail == "duplicate-binding",
			"duplicate generation was admitted");

	require_ok(lease.begin_native(binding()), "native transition failed");
	require_ok(lease.record_pending(binding()), "pending transition failed");
		require_ok(lease.promote(binding()), "promotion failed");
	require_ok(lease.admit_use(), "use owner admission failed");
	require(lease.active_use_owners() == 1, "use owner count missing");
	require_ok(lease.revoke(), "revoke failed");
	require(!lease.admit_use(), "revoked lease admitted a use owner");
	require(!lease.begin_drain(), "drain ignored live use owner");
	require_ok(lease.release_use(), "use owner release failed");
	require_ok(lease.begin_drain(), "drain failed after use release");
	require_ok(lease.retire(), "retire failed");
	require_ok(registry.retire(lease), "registry retirement failed");
	require(!registry.reserve(binding()), "retired generation was resurrected");
	}

	void test_registry_gates()
	{
		sqlite_wave3_shm_lease_registry registry;
		auto first_result = registry.reserve(binding(21));
		require(first_result.has_value(), "second generation reservation failed");
	auto first = std::move(first_result.value());
		require(registry.reserve(binding(22)).has_value(), "distinct generation was rejected");
		require(registry.size() == 2, "registry size is not deterministic");
		require_ok(registry.close_admission(), "close admission failed");
		require(!registry.admission_allowed(), "closed registry still admits");
		require(!registry.reserve(binding(23)) &&
			registry.reserve(binding(24)).error().detail == "registry-closed",
			"closed registry admitted a reservation");
		require_ok(registry.quarantine(), "registry quarantine failed");
		require(registry.quarantined() && !registry.admission_allowed(),
			"registry quarantine is not terminal");
		require(!registry.reserve(binding(25)) &&
			registry.reserve(binding(26)).error().detail == "registry-quarantined",
			"quarantined registry admitted a reservation");

		// Leave this still-reserved lease unretired: the registry keeps it as a live tombstone and
		// no destructor operation can silently manufacture cleanup authority.
		require(first.phase() == sqlite_wave3_lease_phase::reserved, "reserved lease changed phase");
	}
} // namespace

int main()
{
	try
	{
		test_lifecycle_and_tombstone();
		test_registry_gates();
	}
	catch (const std::exception& exception)
	{
		std::cerr << "sqlite_wave3_shm_lease_test: " << exception.what() << '\n';
		return 1;
	}
	return 0;
}
