#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sqlite_backend_observation_internal.hpp"
#include "sqlite_connection_lifecycle_internal.hpp"
#include "store_operation_port_internal.hpp"

namespace cxxlens::sdk
{
	/** Extra runtime callbacks required by the production accepted-empty normalizer. */
	struct sqlite_exact_empty_normalization_runtime
	{
		using libversion_number_function = int (*)();
		using compile_option_get_function = const char* (*)(int);
		using db_readonly_function = int (*)(void*, const char*);

		sqlite_source_shm_runtime_binding sqlite;
		libversion_number_function libversion_number{};
		compile_option_get_function compile_option_get{};
		db_readonly_function db_readonly{};
	};

	/**
	 * Source input for one live #202 attempt.  The namespace census owns the retained main and
	 * authenticated parent guard.  The logical-read receipt must pin that exact held main object;
	 * a caller-provided path, identity, digest, or success boolean is never accepted instead.
	 */
	struct sqlite_exact_empty_normalization_input
	{
		sqlite_exact_empty_normalization_runtime runtime;
		sqlite_backend_namespace_census source_census;
		std::string canonical_vfs_locator;
		std::shared_ptr<store_operation_port> operation_port;
		std::uint64_t maximum_main_bytes{std::uint64_t{512U} * 1024U * 1024U};
	};

	enum class sqlite_exact_empty_effect_callback_kind : std::uint8_t
	{
		open,
		write,
		truncate,
		sync,
		lock,
		unlock,
		file_control,
		close,
		delete_relative,
		parent_sync,
		sector_size,
		device_characteristics,
		shm_effect,
	};

	/** One callback emitted by the forwarding VFS itself, never supplied by the Store caller. */
	struct sqlite_exact_empty_effect_callback
	{
		std::uint64_t sequence{};
		sqlite_exact_empty_effect_callback_kind kind{sqlite_exact_empty_effect_callback_kind::open};
		sqlite_backend_file_role role{sqlite_backend_file_role::main_database};
		std::int64_t offset{};
		std::uint64_t byte_count{};
		int argument{};
		int result{};
		int output{};
		std::uint64_t payload_offset{};

		[[nodiscard]] bool operator==(const sqlite_exact_empty_effect_callback&) const = default;
	};

	struct sqlite_exact_empty_effect_device_profile
	{
		int raw_sector_size{};
		std::uint32_t effective_sector_size{};
		int device_characteristics{};
		std::uint32_t page_size{};
		std::uint32_t database_page_count{};
		std::vector<std::uint32_t> journal_record_pages;
		sqlite_backend_opaque_identity vfs_backend_token;
		std::string sqlite_source_id;
		std::vector<std::string> sqlite_build_options;

		[[nodiscard]] bool
		operator==(const sqlite_exact_empty_effect_device_profile&) const = default;
	};

	/**
	 * Nonforgeable internal completed edge.  Only the bound forwarding-VFS factory can instantiate
	 * or logically seal it.  This type deliberately has no Store/public-success method.
	 */
	class sqlite_exact_empty_normalization_completed_edge final
	{
	  public:
		[[nodiscard]] const sqlite_backend_opaque_identity& operation_token() const noexcept;
		[[nodiscard]] const sqlite_exact_empty_effect_device_profile&
		device_profile() const noexcept;
		[[nodiscard]] std::span<const sqlite_exact_empty_effect_callback>
		callbacks() const noexcept;
		[[nodiscard]] std::span<const std::byte> callback_payload() const noexcept;
		[[nodiscard]] std::span<const std::byte> pre_main_bytes() const noexcept;
		[[nodiscard]] std::span<const std::byte> expected_post_main_bytes() const noexcept;
		/** Namespace census captured by the effect after classifier close and cleanup. */
		[[nodiscard]] const sqlite_backend_namespace_census* post_namespace_census() const noexcept;
		[[nodiscard]] bool confirmed_close() const noexcept;
		[[nodiscard]] bool post_close_exact_projection() const noexcept;
		[[nodiscard]] bool post_close_logical_empty() const noexcept;
		[[nodiscard]] bool post_close_sidecars_absent() const noexcept;
		[[nodiscard]] bool custody_drained() const noexcept;

	  private:
		friend struct sqlite_exact_empty_normalization_edge_factory;
		sqlite_exact_empty_normalization_completed_edge(
			sqlite_backend_opaque_identity operation_token,
			sqlite_exact_empty_effect_device_profile device_profile,
			std::vector<sqlite_exact_empty_effect_callback> callbacks,
			std::vector<std::byte> callback_payload,
			std::vector<std::byte> pre_main,
			std::vector<std::byte> expected_post_main,
			bool confirmed_close,
			bool exact_projection,
			bool sidecars_absent,
			bool custody_drained);

		sqlite_backend_opaque_identity operation_token_;
		sqlite_exact_empty_effect_device_profile device_profile_;
		std::vector<sqlite_exact_empty_effect_callback> callbacks_;
		std::vector<std::byte> callback_payload_;
		std::vector<std::byte> pre_main_;
		std::vector<std::byte> expected_post_main_;
		std::optional<sqlite_backend_namespace_census> post_namespace_census_;
		bool confirmed_close_{};
		bool exact_projection_{};
		bool logical_empty_{};
		bool sidecars_absent_{};
		bool custody_drained_{};
	};

	/**
	 * Execute exactly one transaction-free WAL-to-DELETE transition through the bound forwarding
	 * VFS.  The result is only the internal physical edge; the Store owner must independently run
	 * the post-close logical-empty classifier before ordinary fresh initialization.
	 */
	[[nodiscard]] result<std::shared_ptr<const sqlite_exact_empty_normalization_completed_edge>>
	run_sqlite_exact_empty_normalization_effect(
		sqlite_logical_read_receipt logical_read_receipt,
		const sqlite_exact_empty_normalization_input& input,
		const std::shared_ptr<sqlite_backend_observation_capability>& observation);
} // namespace cxxlens::sdk
