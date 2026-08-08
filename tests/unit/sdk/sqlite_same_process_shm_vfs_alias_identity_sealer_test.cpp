#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "sdk/sqlite_same_process_shm_vfs_alias_registration_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;

	constexpr std::string_view alias_name_a{"cxxlens-sealed-alias-a"};
	constexpr std::string_view alias_name_b{"cxxlens-sealed-alias-b"};
	void* registered_vfs{};
	std::string registered_name;
	int runtime_sentinel{};
	int runtime_image_sentinel{};
	int underlying_vfs_sentinel{};
	int underlying_app_data_sentinel{};
	int underlying_open_sentinel{};

	int runtime_open(const char*, void**, int, const char*)
	{
		return 0;
	}

	int runtime_close(void*)
	{
		return 0;
	}

	int runtime_exec(void*, const char*, sqlite_source_shm_runtime_binding::exec_callback,
					 void*, char**)
	{
		return 0;
	}

	const char* runtime_errmsg(void*)
	{
		return "test";
	}

	void runtime_free(void*) {}

	const char* runtime_source_id()
	{
		return "sqlite-source-id-for-sealer-test";
	}

	const char* runtime_uri_parameter(const char*, const char*)
	{
		return nullptr;
	}

	const char* runtime_uri_key(const char*, int)
	{
		return nullptr;
	}

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
			throw std::runtime_error(std::string{message});
	}

	void* find_vfs(const char* name)
	{
		return name != nullptr && registered_vfs != nullptr && registered_name == name
			? registered_vfs
			: nullptr;
	}

	int register_vfs(void* value, int)
	{
		registered_vfs = value;
		return 0;
	}

	int unregister_vfs(void* value)
	{
		if (value == nullptr || value != registered_vfs)
			return 1;
		registered_vfs = nullptr;
		registered_name.clear();
		return 0;
	}

	sqlite_shm_vfs_alias_identity_sealing_input input_for(
		sqlite_shm_process_registry_handle process,
		const std::shared_ptr<void>& runtime,
		const std::string_view name,
		const void* vfs,
		const void* backend)
	{
		return {
			std::move(process),
			{&runtime_sentinel,
			 &runtime_image_sentinel,
			 runtime.get(),
			 runtime,
			 runtime_open,
			 runtime_close,
			 runtime_exec,
			 runtime_errmsg,
			 runtime_free,
			 runtime_source_id,
			 runtime_uri_parameter,
			 runtime_uri_key,
			 find_vfs,
			 register_vfs,
			 unregister_vfs},
			&underlying_vfs_sentinel,
			&underlying_app_data_sentinel,
			&underlying_open_sentinel,
			backend,
			std::string{name},
			const_cast<void*>(vfs),
		};
	}

	void verify_complete_tuple_registers_and_groups_aliases()
	{
		auto process = sqlite_same_process_shm_process_port::acquire();
#if defined(__linux__)
		require(process.has_value(), "qualified process port for identity sealer");
#else
		require(!process, "unsupported process port fails closed");
		return;
#endif
		auto runtime = std::static_pointer_cast<void>(std::make_shared<std::uint64_t>(9U));
		int vfs_a{};
		int backend_a{};
		registered_name = std::string{alias_name_a};
		auto first_binding = sqlite_same_process_shm_vfs_alias_identity_sealer::seal(
			input_for(std::move(process.value()), runtime, alias_name_a, &vfs_a, &backend_a));
		require(first_binding.has_value(), "complete identity tuple seals");
		require(first_binding->valid(), "sealed binding is valid");
		auto first = sqlite_same_process_shm_vfs_alias_registration_port::register_alias(
			std::move(first_binding.value()));
		require(first.has_value(), "sealed binding registers through closed port");
		require(first->valid(), "registered sealed alias is valid");
		const auto first_cohort = first->shared_runtime_vfs_cohort();
		const auto first_runtime = first->runtime_lifetime_identity();
		const auto first_alias = first->alias_lifetime();
		const auto first_pin = first->runtime_lifetime_pin_identity();
		require(first->unregister_alias().has_value(), "first sealed alias unregisters");

		auto second_process = sqlite_same_process_shm_process_port::acquire();
		require(second_process.has_value(), "same process port remains available");
		int vfs_b{};
		int backend_b{};
		registered_name = std::string{alias_name_b};
		auto second_binding = sqlite_same_process_shm_vfs_alias_identity_sealer::seal(
			input_for(std::move(second_process.value()), runtime, alias_name_b, &vfs_b, &backend_b));
		require(second_binding.has_value(), "second complete identity tuple seals");
		auto second = sqlite_same_process_shm_vfs_alias_registration_port::register_alias(
			std::move(second_binding.value()));
		if (!second)
			throw std::runtime_error(
				"second sealed binding registers, rejection=" +
				std::to_string(static_cast<int>(second.error().reason)));
		require(second->shared_runtime_vfs_cohort() == first_cohort,
				"same runtime and original VFS share one cohort");
		require(second->runtime_lifetime_identity() != first_runtime,
				"distinct aliases have distinct runtime identities");
		require(second->alias_lifetime() != first_alias,
				"distinct forwarding aliases have distinct lifetimes");
		require(second->runtime_lifetime_pin_identity() != first_pin,
				"distinct aliases have distinct runtime pins");
		require(second->unregister_alias().has_value(), "second sealed alias unregisters");
		require(registered_vfs == nullptr, "native registry is empty after exact unregistration");
	}

	void verify_incomplete_tuple_fails_before_native_effect()
	{
		auto process = sqlite_same_process_shm_process_port::acquire();
#if defined(__linux__)
		require(process.has_value(), "qualified process port for negative sealer cases");
#else
		return;
#endif
		auto runtime = std::static_pointer_cast<void>(std::make_shared<std::uint64_t>(11U));
		int vfs{};
		int backend{};
		auto incomplete = input_for(
			std::move(process.value()), runtime, alias_name_a, &vfs, &backend);
		incomplete.runtime.source_id = nullptr;
		auto rejected = sqlite_same_process_shm_vfs_alias_identity_sealer::seal(std::move(incomplete));
		require(!rejected.has_value() &&
					rejected.error().reason == sqlite_shm_lease_rejection_reason::invalid_identity,
				"missing source identity rejects before registration");

		auto second_process = sqlite_same_process_shm_process_port::acquire();
		require(second_process.has_value(), "process port remains available after rejection");
		auto mismatch = input_for(
			std::move(second_process.value()), runtime, alias_name_a, &vfs, &backend);
		mismatch.pinned_underlying_vfs_identity = mismatch.vfs_implementation;
		auto mismatched_rejected =
			sqlite_same_process_shm_vfs_alias_identity_sealer::seal(std::move(mismatch));
		require(!mismatched_rejected.has_value() &&
				mismatched_rejected.error().reason ==
					sqlite_shm_lease_rejection_reason::invalid_identity,
				"alias equal to underlying VFS rejects before registration");
		require(registered_vfs == nullptr, "negative sealer cases have zero native effect");
	}
} // namespace

int main()
{
	try
	{
		verify_complete_tuple_registers_and_groups_aliases();
		verify_incomplete_tuple_fails_before_native_effect();
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
