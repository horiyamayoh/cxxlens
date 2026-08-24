#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sdk/sqlite_same_process_shm_vfs_alias_registration_internal.hpp"

namespace cxxlens::sdk::test_support
{
	[[nodiscard]] inline sqlite_backend_opaque_identity
	shm_identity(const std::string_view profile, const std::uint8_t marker)
	{
		return {std::string{profile}, {static_cast<std::byte>(marker)}};
	}

	inline void require_shm(const bool condition, const std::string_view message)
	{
		if (!condition)
			throw std::runtime_error{std::string{message}};
	}

	/**
	 * One registered alias and file-family pin created exclusively through production ports.
	 *
	 * The native callbacks are deterministic in-process doubles. They can report only the effects
	 * delegated by the production alias-registration port; no registry state or receipt is forged.
	 */
	class sqlite_same_process_shm_product_fixture final
	{
	  public:
		explicit sqlite_same_process_shm_product_fixture(
			const std::uint8_t marker,
			const std::optional<std::uint8_t> exact_file_marker = std::nullopt)
			: registered_name_{"cxxlens-product-shm-" + std::to_string(marker)}
		{
			previous_native_fixture_ = active_native_fixture_;
			active_native_fixture_ = this;

			auto acquired = sqlite_same_process_shm_process_port::acquire();
			require_shm(acquired.has_value(), "acquire production same-process SHM registry");
			process_.emplace(std::move(*acquired));

			runtime_owner_ = std::make_shared<std::uint8_t>(marker);
			auto sealed = sqlite_same_process_shm_vfs_alias_identity_sealer::seal({
				*process_,
				{&runtime_identity_sentinel_,
				 &runtime_image_sentinel_,
				 runtime_owner_.get(),
				 runtime_owner_,
				 runtime_open,
				 runtime_close,
				 runtime_exec,
				 runtime_errmsg,
				 runtime_free,
				 runtime_source_id,
				 runtime_uri_parameter,
				 runtime_uri_key,
				 native_find,
				 native_register,
				 native_unregister},
				&underlying_vfs_sentinel_,
				&underlying_app_data_sentinel_,
				&underlying_open_sentinel_,
				this,
				registered_name_,
				&vfs_sentinel_,
			});
			require_shm(sealed.has_value(), "seal production VFS alias identity");

			auto registered = sqlite_same_process_shm_vfs_alias_registration_port::register_alias(
				std::move(*sealed));
			require_shm(registered.has_value(), "register production VFS alias");
			alias_.emplace(std::move(*registered));

			family_ = {alias_->process_instance(),
					   alias_->shared_runtime_vfs_cohort(),
					   shm_identity("test.product-shm.file-family",
								 exact_file_marker.value_or(marker))};
			auto family = sqlite_same_process_shm_vfs_alias_registration_port::install_or_join_family(
				*alias_, family_);
			require_shm(family.has_value(), "install production SHM file family");
			family_pin_.emplace(std::move(*family));
		}

		~sqlite_same_process_shm_product_fixture() noexcept
		{
			if (process_ && process_->registry() && family_pin_)
				(void)process_->registry()->release_family(*family_pin_);
			family_pin_.reset();
			if (alias_)
				(void)alias_->unregister_alias();
			alias_.reset();
			active_native_fixture_ = previous_native_fixture_;
		}

		sqlite_same_process_shm_product_fixture(
			const sqlite_same_process_shm_product_fixture&) = delete;
		sqlite_same_process_shm_product_fixture&
		operator=(const sqlite_same_process_shm_product_fixture&) = delete;

		[[nodiscard]] sqlite_shm_process_registry_handle& process() noexcept
		{
			return *process_;
		}
		[[nodiscard]] sqlite_same_process_shm_mapping_registry& registry() noexcept
		{
			return *process_->registry();
		}
		[[nodiscard]] sqlite_shm_registered_vfs_alias& alias() noexcept
		{
			return *alias_;
		}
		[[nodiscard]] sqlite_shm_registry_family_pin& family_pin() noexcept
		{
			return *family_pin_;
		}
		[[nodiscard]] const sqlite_shm_lease_family_binding& family() const noexcept
		{
			return family_;
		}
		[[nodiscard]] std::size_t native_register_calls() const noexcept
		{
			return native_register_calls_;
		}
		[[nodiscard]] std::size_t native_unregister_calls() const noexcept
		{
			return native_unregister_calls_;
		}

	  private:
		using runtime = sqlite_source_shm_runtime_binding;

		static int runtime_open(const char*, void**, int, const char*) { return 0; }
		static int runtime_close(void*) { return 0; }
		static int runtime_exec(void*, const char*, runtime::exec_callback, void*, char**) { return 0; }
		static const char* runtime_errmsg(void*) { return "test product SHM runtime"; }
		static void runtime_free(void*) {}
		static const char* runtime_source_id()
		{
			return "sqlite-source-id-for-product-shm-tests";
		}
		static const char* runtime_uri_parameter(const char*, const char*) { return nullptr; }
		static const char* runtime_uri_key(const char*, int) { return nullptr; }

		static void* native_find(const char* name)
		{
			if (!active_native_fixture_ || name == nullptr ||
				active_native_fixture_->registered_name_ != name)
				return nullptr;
			return active_native_fixture_->installed_vfs_;
		}

		static int native_register(void* vfs, int)
		{
			if (!active_native_fixture_ || vfs != &active_native_fixture_->vfs_sentinel_)
				return 1;
			++active_native_fixture_->native_register_calls_;
			active_native_fixture_->installed_vfs_ = vfs;
			return 0;
		}

		static int native_unregister(void* vfs)
		{
			if (!active_native_fixture_ || vfs != active_native_fixture_->installed_vfs_)
				return 1;
			++active_native_fixture_->native_unregister_calls_;
			active_native_fixture_->installed_vfs_ = nullptr;
			return 0;
		}

		std::string registered_name_;
		std::optional<sqlite_shm_process_registry_handle> process_;
		std::shared_ptr<void> runtime_owner_;
		std::optional<sqlite_shm_registered_vfs_alias> alias_;
		std::optional<sqlite_shm_registry_family_pin> family_pin_;
		sqlite_shm_lease_family_binding family_;
		void* installed_vfs_{};
		std::size_t native_register_calls_{};
		std::size_t native_unregister_calls_{};
		int vfs_sentinel_{};
		sqlite_same_process_shm_product_fixture* previous_native_fixture_{};
		inline static int runtime_identity_sentinel_{};
		inline static int runtime_image_sentinel_{};
		inline static int underlying_vfs_sentinel_{};
		inline static int underlying_app_data_sentinel_{};
		inline static int underlying_open_sentinel_{};
		inline static thread_local sqlite_same_process_shm_product_fixture*
			active_native_fixture_{};
	};
} // namespace cxxlens::sdk::test_support
