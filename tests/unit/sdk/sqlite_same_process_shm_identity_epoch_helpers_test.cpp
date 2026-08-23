#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "sdk/sqlite_same_process_shm_identity_epoch_helpers_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;
	using reason = sqlite_shm_identity_epoch_rejection_reason;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
			throw std::runtime_error(std::string{message});
	}

	[[nodiscard]] sqlite_backend_opaque_identity identity(const std::string_view profile,
														  const std::uint64_t value)
	{
		sqlite_backend_opaque_identity output;
		output.profile = std::string{profile};
		for (auto shift = 56U;; shift -= 8U)
		{
			output.bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
			if (shift == 0U)
				break;
		}
		return output;
	}

	struct vfs_fixture
	{
		int forwarding{};
		int underlying{};
		int app_data{};
	};

	[[nodiscard]] sqlite_shm_identity_epoch_anchor anchor(const vfs_fixture& fixture)
	{
		return {
			{42U, 100U, 7U, identity("process", 1U), identity("pidfd", 1U), true},
			{&fixture.forwarding,
			 &fixture.underlying,
			 &fixture.app_data,
			 identity("runtime-image", 1U),
			 identity("sqlite-source-id", 1U),
			 identity("vfs-cohort", 1U),
			 identity("alias", 1U),
			 identity("registration-epoch", 1U)},
			identity("file-family", 1U),
			identity("open-epoch", 1U),
			identity("callback-cohort", 1U),
		};
	}

	[[nodiscard]] sqlite_shm_identity_epoch_reservation_request
	request(const sqlite_shm_identity_epoch_anchor& value,
			const std::uint64_t generation,
			const sqlite_backend_opaque_identity& generation_identity,
			const volatile void* mapping,
			const bool extension_requested = false,
			const std::uint64_t sealed_size = 4096U)
	{
		return {value,
				generation,
				generation_identity,
				0,
				4096,
				0U,
				4096U,
				mapping,
				sealed_size,
				extension_requested};
	}

	[[nodiscard]] sqlite_shm_identity_epoch_observation
	observation(const sqlite_shm_identity_epoch_receipt& receipt,
				const volatile void* mapping,
				const std::uint64_t sealed_size)
	{
		return {receipt.anchor(),
				receipt.generation(),
				receipt.generation_identity(),
				receipt.mapping_epoch(),
				receipt.attachment_epoch(),
				receipt.page_number(),
				receipt.page_size(),
				receipt.byte_offset(),
				receipt.byte_count(),
				mapping,
				sealed_size};
	}

	template <class Result>
	void require_reason(const Result& result, const reason expected, const std::string_view message)
	{
		require(!result.has_value() && result.error().reason == expected, message);
	}

	void verify_process_and_vfs_faults(const vfs_fixture& fixture)
	{
		auto expected = anchor(fixture);
		auto changed_pid = expected.process;
		changed_pid.pid++;
		auto pid_result = validate_sqlite_shm_process_identity(expected.process, changed_pid);
		require_reason(pid_result, reason::pid_mismatch, "PID reuse is rejected");

		auto changed_fork = expected.process;
		changed_fork.fork_epoch++;
		auto fork_result = validate_sqlite_shm_process_identity(expected.process, changed_fork);
		require_reason(
			fork_result, reason::fork_epoch_mismatch, "inherited fork epoch is rejected");

		auto dead_pidfd = expected.process;
		dead_pidfd.pidfd_live = false;
		auto dead_result = validate_sqlite_shm_process_identity(expected.process, dead_pidfd);
		require_reason(dead_result, reason::pidfd_not_live, "dead pidfd is rejected");

		auto changed_app_data = expected.vfs;
		int replacement_app_data{};
		changed_app_data.underlying_app_data = &replacement_app_data;
		auto vfs_result = validate_sqlite_shm_vfs_identity(expected.vfs, changed_app_data);
		require_reason(vfs_result,
					   reason::vfs_app_data_mismatch,
					   "underlying VFS app-data replacement is rejected");

		auto registry_result = sqlite_shm_identity_epoch_registry::create(expected);
		require(registry_result.has_value(), "identity fault registry creates");
		auto registry = std::move(registry_result.value());
		auto child_anchor = expected;
		child_anchor.process.fork_epoch++;
		auto inherited =
			registry.reserve(request(child_anchor, 1U, identity("generation", 1U), nullptr));
		require_reason(inherited,
					   reason::fork_epoch_mismatch,
					   "registry rejects inherited fork state before native delegation");
		require(registry.quarantined(), "fork mismatch sticks quarantine");
	}

	void verify_happy_map_resize_unmap_lifecycle(const vfs_fixture& fixture)
	{
		int mapping{};
		auto registry_result = sqlite_shm_identity_epoch_registry::create(anchor(fixture));
		require(registry_result.has_value(), "valid anchor creates registry");
		auto registry = std::move(registry_result.value());
		auto reserved = registry.reserve(
			request(registry.anchor(), 1U, identity("generation", 1U), nullptr, true));
		require(reserved.has_value(), "predelegate reservation succeeds");
		auto receipt = std::move(reserved.value());
		require(receipt.valid() && receipt.mapping_epoch() != 0U &&
					receipt.attachment_epoch() != 0U,
				"reservation contains issuer epochs");

		auto mapped = observation(receipt, &mapping, 4096U);
		require(registry.validate_map(receipt, mapped).has_value(),
				"exact native map observation succeeds");
		require(receipt.native_mapping() == &mapping, "native pointer is retained only after map");

		auto grown = observation(receipt, &mapping, 8192U);
		require(registry.validate_resize(receipt, grown, 8192U).has_value(),
				"monotonic same-epoch resize succeeds");
		require(receipt.sealed_shm_size() == 8192U, "resize advances high-water size");
		require(registry.validate_unmap(receipt, grown, false).has_value(),
				"non-deleting exact unmap succeeds");
		require(registry.retire(receipt).has_value(), "retire follows validated unmap");
		require(!receipt.valid(), "retirement consumes the receipt");
	}

	void verify_generation_and_pointer_aba(const vfs_fixture& fixture)
	{
		int first_mapping{};
		int replacement_mapping{};
		auto registry_result = sqlite_shm_identity_epoch_registry::create(anchor(fixture));
		require(registry_result.has_value(), "ABA registry creates");
		auto registry = std::move(registry_result.value());
		const auto generation_identity = identity("generation", 1U);
		auto first = registry.reserve(request(registry.anchor(), 1U, generation_identity, nullptr));
		require(first.has_value(), "first generation reservation");
		auto first_receipt = std::move(first.value());
		auto first_observed = observation(first_receipt, &first_mapping, 4096U);
		require(registry.validate_map(first_receipt, first_observed).has_value(),
				"first map validates");
		auto wrong_pointer = observation(first_receipt, &replacement_mapping, 4096U);
		auto pointer_result = registry.validate_unmap(first_receipt, wrong_pointer, false);
		require_reason(pointer_result, reason::pointer_aba, "pointer ABA is rejected before unmap");
		require(registry.quarantined(), "pointer ABA quarantines the coordinator");
		require_reason(
			registry.reserve(request(registry.anchor(), 1U, generation_identity, nullptr)),
			reason::quarantined,
			"quarantine blocks successor reservations");

		auto generation_registry_result =
			sqlite_shm_identity_epoch_registry::create(anchor(fixture));
		require(generation_registry_result.has_value(), "generation registry creates");
		auto generation_registry = std::move(generation_registry_result.value());
		auto generation_one = generation_registry.reserve(
			request(generation_registry.anchor(), 1U, generation_identity, nullptr));
		require(generation_one.has_value(), "generation one is accepted");
		auto mismatch = generation_registry.reserve(
			request(generation_registry.anchor(), 1U, identity("generation", 99U), nullptr));
		require_reason(
			mismatch, reason::generation_mismatch, "generation identity cannot be replaced");
	}

	void verify_fail_closed_operations(const vfs_fixture& fixture)
	{
		int mapping{};
		auto registry_result = sqlite_shm_identity_epoch_registry::create(anchor(fixture));
		require(registry_result.has_value(), "fault registry creates");
		auto registry = std::move(registry_result.value());
		auto reserved =
			registry.reserve(request(registry.anchor(), 1U, identity("generation", 1U), nullptr));
		require(reserved.has_value(), "fault reservation succeeds");
		auto receipt = std::move(reserved.value());
		auto mapped = observation(receipt, &mapping, 4096U);
		require(registry.validate_map(receipt, mapped).has_value(), "fault map succeeds");
		require_reason(registry.validate_unmap(receipt, mapped, true),
					   reason::unmap_delete_flag,
					   "deleting unmap is rejected");
		require(registry.validate_unmap(receipt, mapped, false).has_value(),
				"retry uses the exact non-deleting route");
		require(registry.retire(receipt).has_value(), "fault receipt retires once");
		require_reason(
			registry.retire(receipt), reason::stale_epoch, "retired receipt cannot replay");

		auto invalid = sqlite_shm_identity_epoch_registry::create({});
		require_reason(invalid, reason::invalid_identity, "incomplete anchor fails closed");

		auto extension_registry_result =
			sqlite_shm_identity_epoch_registry::create(anchor(fixture));
		require(extension_registry_result.has_value(), "extension registry creates");
		auto extension_registry = std::move(extension_registry_result.value());
		auto nonextension = extension_registry.reserve(
			request(extension_registry.anchor(), 1U, identity("generation", 1U), nullptr));
		require(nonextension.has_value(), "nonextension reservation succeeds");
		auto nonextension_receipt = std::move(nonextension.value());
		require(extension_registry
					.validate_map(nonextension_receipt,
								  observation(nonextension_receipt, &mapping, 4096U))
					.has_value(),
				"nonextension map succeeds");
		require_reason(
			extension_registry.validate_resize(
				nonextension_receipt, observation(nonextension_receipt, &mapping, 8192U), 8192U),
			reason::resize_not_requested,
			"unrequested resize fails closed");
	}

	void verify_concurrent_epoch_allocation(const vfs_fixture& fixture)
	{
		auto registry_result = sqlite_shm_identity_epoch_registry::create(anchor(fixture));
		require(registry_result.has_value(), "concurrency registry creates");
		auto registry = std::make_shared<sqlite_shm_identity_epoch_registry>(
			std::move(registry_result.value()));
		constexpr std::size_t count = 16U;
		std::array<std::uint64_t, count> epochs{};
		std::array<bool, count> successes{};
		std::vector<std::thread> workers;
		workers.reserve(count);
		for (std::size_t index = 0; index < count; ++index)
		{
			workers.emplace_back(
				[&, index]
				{
					auto result = registry->reserve(
						request(registry->anchor(), 1U, identity("generation", 1U), nullptr));
					if (result)
					{
						successes[index] = true;
						epochs[index] = result.value().attachment_epoch();
					}
				});
		}
		for (auto& worker : workers)
			worker.join();
		for (std::size_t index = 0; index < count; ++index)
		{
			require(successes[index], "concurrent reservation succeeds");
			for (std::size_t other = index + 1U; other < count; ++other)
				require(epochs[index] != epochs[other], "attachment epochs never repeat");
		}
	}
} // namespace

int main()
{
	try
	{
		vfs_fixture fixture;
		verify_process_and_vfs_faults(fixture);
		verify_happy_map_resize_unmap_lifecycle(fixture);
		verify_generation_and_pointer_aba(fixture);
		verify_fail_closed_operations(fixture);
		verify_concurrent_epoch_allocation(fixture);
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
