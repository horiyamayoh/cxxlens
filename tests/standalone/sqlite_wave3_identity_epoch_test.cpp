#include "sqlite_wave3_identity_epoch_internal.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using namespace cxxlens::sdk;

	sqlite_backend_opaque_identity identity(const char marker)
	{
		return {"sqlite-wave3-test-v1", {static_cast<std::byte>(marker)}};
	}

	sqlite_wave3_identity_binding binding()
	{
		return {identity('p'), identity('r'), identity('f'), identity('l'), identity('o'),
			identity('n'), identity('d'), identity('s'), identity('e'), identity('y'), identity('m'),
			7, 11, 0, 4096};
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

	void test_identity_validation()
	{
		auto valid = binding();
		require(inspect_sqlite_wave3_identity(valid) == sqlite_wave3_identity_failure::none,
			"valid identity rejected");
		require_ok(validate_sqlite_wave3_identity(valid), "valid identity failed");

		auto missing = valid;
		missing.shm_entry = {};
		auto missing_result = validate_sqlite_wave3_identity(missing);
		require(!missing_result && missing_result.error().detail == "missing-identity",
			"missing identity accepted");

		auto too_large = valid;
		too_large.mount.bytes.resize(4097U);
		auto too_large_result = validate_sqlite_wave3_identity(too_large);
		require(!too_large_result && too_large_result.error().detail == "identity-too-large",
			"oversized identity accepted");

		auto invalid_page = valid;
		invalid_page.page_number = std::numeric_limits<std::uint64_t>::max();
		invalid_page.page_size = 2;
		auto invalid_page_result = validate_sqlite_wave3_identity(invalid_page);
		require(!invalid_page_result && invalid_page_result.error().detail == "invalid-page",
			"overflowing page accepted");

		auto changed = valid;
		changed.open_epoch = identity('x');
		auto continuity = validate_sqlite_wave3_identity_continuity(valid, changed);
		require(!continuity && continuity.error().detail == "continuity-mismatch",
			"changed epoch accepted");
}

	void test_epoch_ordering()
	{
		const auto sealed_binding = binding();
		sqlite_wave3_epoch_controller controller;
		require(controller.phase() == sqlite_wave3_epoch_phase::unresolved,
			"initial phase is not unresolved");
		require_ok(controller.arm(sealed_binding), "arm failed");
		require(controller.phase() == sqlite_wave3_epoch_phase::armed &&
			controller.admission_allowed(), "armed phase not admitting");

		auto stale = sealed_binding;
		stale.fork_generation++;
		auto stale_result = controller.start_native(stale);
		require(!stale_result && stale_result.error().detail == "continuity-mismatch",
			"fork generation drift accepted");
		require(controller.phase() == sqlite_wave3_epoch_phase::armed,
			"failed native start changed phase");

		require_ok(controller.start_native(sealed_binding), "native start failed");
		require(controller.phase() == sqlite_wave3_epoch_phase::native_started,
			"native phase missing");
	require_ok(controller.seal(sealed_binding), "seal failed");
		require(controller.phase() == sqlite_wave3_epoch_phase::sealed && controller.terminal() &&
			!controller.admission_allowed(), "sealed phase has live admission");
	require(!controller.revoke(), "sealed epoch revoked");

		sqlite_wave3_epoch_controller quarantined;
		require_ok(quarantined.arm(binding()), "quarantine setup failed");
		require_ok(quarantined.quarantine("native-indeterminate"), "quarantine failed");
		require(quarantined.phase() == sqlite_wave3_epoch_phase::quarantined &&
			quarantined.terminal() && !quarantined.admission_allowed() &&
			quarantined.quarantine_detail() == "native-indeterminate",
			"quarantine not terminal");
		require(!quarantined.start_native(binding()), "quarantined epoch admitted native callback");
	}
} // namespace

int main()
{
	try
	{
		test_identity_validation();
		test_epoch_ordering();
	}
	catch (const std::exception& exception)
	{
		std::cerr << "sqlite_wave3_identity_epoch_test: " << exception.what() << '\n';
		return 1;
	}
	return 0;
}
