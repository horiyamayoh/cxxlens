#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <cxxlens/sdk/common.hpp>

#include "materialization_store.hpp"

namespace cxxlens::sdk
{
	class sqlite_backend_observation_capability;
} // namespace cxxlens::sdk

namespace cxxlens::detail::clang22::materialization
{
	/** Move-only close-on-destruction descriptor; no ambient pathname is retained. */
	class materialization_owned_fd
	{
	  public:
		materialization_owned_fd() noexcept = default;
		explicit materialization_owned_fd(int descriptor) noexcept;
		materialization_owned_fd(const materialization_owned_fd&) = delete;
		materialization_owned_fd& operator=(const materialization_owned_fd&) = delete;
		materialization_owned_fd(materialization_owned_fd&& other) noexcept;
		materialization_owned_fd& operator=(materialization_owned_fd&& other) noexcept;
		~materialization_owned_fd();

		[[nodiscard]] int get() const noexcept;
		[[nodiscard]] explicit operator bool() const noexcept;
		[[nodiscard]] int release() noexcept;

	  private:
		int descriptor_{-1};
	};

	/** Exact opened-object identity used for before/after replacement detection. */
	struct materialization_file_identity
	{
		std::uint64_t device{};
		std::uint64_t inode{};
		std::uint64_t size_bytes{};
		std::uint64_t mode{};
		std::int64_t modified_seconds{};
		std::int64_t modified_nanoseconds{};
		std::int64_t changed_seconds{};
		std::int64_t changed_nanoseconds{};

		[[nodiscard]] bool operator==(const materialization_file_identity&) const = default;
	};

	/** Read one descriptor identity, optionally requiring a regular file. */
	[[nodiscard]] sdk::result<materialization_file_identity>
	materialization_fd_identity(int descriptor, bool require_regular);

	/** Closed lexical policy shared by occurrence inventory and rooted SQLite paths. */
	[[nodiscard]] sdk::result<void> validate_materialization_relative_path(
		std::string_view value, std::size_t maximum_utf8_bytes, bool require_nfc);

	/** Exact SQLite request-path policy (1..4095 UTF-8 bytes, canonical relative NFC). */
	[[nodiscard]] sdk::result<void> validate_materialization_sqlite_path(std::string_view value);

	/** Resolve one canonical relative path beneath an already authenticated directory FD. */
	[[nodiscard]] sdk::result<materialization_owned_fd>
	open_materialization_beneath(int directory_descriptor,
								 std::string_view relative_path,
								 int flags,
								 std::uint32_t creation_mode = 0U);

	/**
	 * One immutable startup effect root. Capture must be called before request parsing; every later
	 * lookup is openat2 beneath this descriptor with symlink and magic-link resolution forbidden.
	 */
	class materialization_effect_root
	{
	  public:
		materialization_effect_root(const materialization_effect_root&) = delete;
		materialization_effect_root& operator=(const materialization_effect_root&) = delete;
		materialization_effect_root(materialization_effect_root&&) noexcept = default;
		materialization_effect_root& operator=(materialization_effect_root&&) noexcept = default;

		[[nodiscard]] static sdk::result<materialization_effect_root> capture_startup();
		[[nodiscard]] sdk::result<materialization_owned_fd> open_beneath(
			std::string_view relative_path, int flags, std::uint32_t creation_mode = 0U) const;
		[[nodiscard]] sdk::result<materialization_owned_fd> duplicate_directory() const;
		[[nodiscard]] const materialization_file_identity& identity() const noexcept;
		[[nodiscard]] const std::string& observation_digest() const noexcept;

	  private:
		materialization_effect_root(materialization_owned_fd directory,
									materialization_file_identity identity,
									std::string observation_digest);

		materialization_owned_fd directory_;
		materialization_file_identity identity_;
		std::string observation_digest_;
	};

	/** Internal absolute bootstrap used only to establish an installed prefix from /proc/self/exe.
	 */
	[[nodiscard]] sdk::result<materialization_owned_fd>
	open_materialization_absolute_no_symlinks(std::string_view absolute_path, int flags);

	struct materialization_rooted_vfs_receipt
	{
		std::string schema{"rooted-vfs-v1"};
		std::string mount_device_inode_observation_digest;
		std::string exact_relative_path;
		std::string parent_and_leaf_resolution_verdict{"openat2-beneath-no-symlinks-no-magiclinks"};

		[[nodiscard]] bool operator==(const materialization_rooted_vfs_receipt&) const = default;
	};

	/**
	 * Source-private Store opener backed by a process-local SQLite VFS rooted at the captured
	 * startup directory. Creation fails when the SQLite VFS ABI or openat2 assurance is
	 * unavailable.
	 */
	class materialization_rooted_store_opener final : public materialization_store_opener
	{
	  public:
		materialization_rooted_store_opener(const materialization_rooted_store_opener&) = delete;
		materialization_rooted_store_opener&
		operator=(const materialization_rooted_store_opener&) = delete;
		materialization_rooted_store_opener(materialization_rooted_store_opener&&) = delete;
		materialization_rooted_store_opener&
		operator=(materialization_rooted_store_opener&&) = delete;
		~materialization_rooted_store_opener() override;

		[[nodiscard]] static sdk::result<std::unique_ptr<materialization_rooted_store_opener>>
		create(const materialization_effect_root& root);

		[[nodiscard]] sdk::result<sdk::snapshot_store>
		open_memory(sdk::relation_engine engine) override;
		[[nodiscard]] sdk::result<sdk::snapshot_store>
		open_sqlite(const std::string& exact_path, sdk::relation_engine engine) override;
		[[nodiscard]] const std::optional<materialization_rooted_vfs_receipt>&
		receipt() const noexcept;
		/** Source-private control-plane access used by Store integration and focused adapter tests.
		 */
		[[nodiscard]] sdk::sqlite_backend_observation_capability& observation_capability() noexcept;

	  private:
		struct state;
		explicit materialization_rooted_store_opener(std::unique_ptr<state> state);
		std::unique_ptr<state> state_;
	};
} // namespace cxxlens::detail::clang22::materialization
