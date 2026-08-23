#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <variant>

#include "sqlite_backend_observation_internal.hpp"

namespace cxxlens::sdk
{
	namespace detail
	{
		struct sqlite_shm_identity_epoch_registry_control;
		struct sqlite_shm_identity_epoch_row;
	} // namespace detail

	/**
	 * Closed failure classes for the source-private identity/epoch helper.
	 *
	 * These values describe why a callback cannot be authenticated.  They are deliberately
	 * separate from SQLite status codes: a caller must finish the corresponding no-effect or
	 * quarantine transition before it can choose an outward status.
	 */
	enum class sqlite_shm_identity_epoch_rejection_reason : std::uint8_t
	{
		invalid_identity,
		invalid_request,
		invalid_process,
		pid_mismatch,
		process_start_mismatch,
		pidfd_mismatch,
		pidfd_not_live,
		fork_epoch_mismatch,
		process_instance_mismatch,
		vfs_mismatch,
		vfs_app_data_mismatch,
		runtime_image_mismatch,
		source_id_mismatch,
		vfs_cohort_mismatch,
		alias_lifetime_mismatch,
		registration_epoch_mismatch,
		file_family_mismatch,
		open_epoch_mismatch,
		callback_cohort_mismatch,
		generation_mismatch,
		stale_generation,
		mapping_epoch_mismatch,
		attachment_epoch_mismatch,
		stale_epoch,
		pointer_aba,
		page_mismatch,
		range_mismatch,
		resize_not_requested,
		resize_regression,
		unmap_delete_flag,
		quarantined,
		generation_exhausted,
	};

	struct sqlite_shm_identity_epoch_rejection
	{
		sqlite_shm_identity_epoch_rejection_reason reason{
			sqlite_shm_identity_epoch_rejection_reason::invalid_request};
	};

	template <class Value>
	class sqlite_shm_identity_epoch_result
	{
	  public:
		sqlite_shm_identity_epoch_result(Value value) : storage_{std::move(value)} {}
		sqlite_shm_identity_epoch_result(sqlite_shm_identity_epoch_rejection failure)
			: storage_{failure}
		{
		}

		[[nodiscard]] bool has_value() const noexcept
		{
			return std::holds_alternative<Value>(storage_);
		}
		[[nodiscard]] explicit operator bool() const noexcept
		{
			return has_value();
		}
		[[nodiscard]] Value& value()
		{
			return std::get<Value>(storage_);
		}
		[[nodiscard]] const Value& value() const
		{
			return std::get<Value>(storage_);
		}
		[[nodiscard]] sqlite_shm_identity_epoch_rejection& error()
		{
			return std::get<sqlite_shm_identity_epoch_rejection>(storage_);
		}
		[[nodiscard]] const sqlite_shm_identity_epoch_rejection& error() const
		{
			return std::get<sqlite_shm_identity_epoch_rejection>(storage_);
		}

	  private:
		std::variant<Value, sqlite_shm_identity_epoch_rejection> storage_;
	};

	template <>
	class sqlite_shm_identity_epoch_result<void>
	{
	  public:
		sqlite_shm_identity_epoch_result() = default;
		sqlite_shm_identity_epoch_result(sqlite_shm_identity_epoch_rejection failure)
			: storage_{failure}
		{
		}

		[[nodiscard]] bool has_value() const noexcept
		{
			return std::holds_alternative<std::monostate>(storage_);
		}
		[[nodiscard]] explicit operator bool() const noexcept
		{
			return has_value();
		}
		[[nodiscard]] sqlite_shm_identity_epoch_rejection& error()
		{
			return std::get<sqlite_shm_identity_epoch_rejection>(storage_);
		}
		[[nodiscard]] const sqlite_shm_identity_epoch_rejection& error() const
		{
			return std::get<sqlite_shm_identity_epoch_rejection>(storage_);
		}

	  private:
		std::variant<std::monostate, sqlite_shm_identity_epoch_rejection> storage_;
	};

	/**
	 * Process identity sampled at one callback boundary.
	 *
	 * PID alone is not an identity.  The helper therefore requires PID, process start identity,
	 * a live pidfd identity, the registered fork epoch, and the process-instance receipt to agree
	 * on every revalidation.  A child inherited across fork cannot reuse the parent receipt.
	 */
	struct sqlite_shm_process_identity_sample
	{
		std::uint64_t pid{};
		std::uint64_t process_start_ticks{};
		std::uint64_t fork_epoch{};
		sqlite_backend_opaque_identity process_instance;
		sqlite_backend_opaque_identity pidfd_identity;
		bool pidfd_live{};

		[[nodiscard]] bool operator==(const sqlite_shm_process_identity_sample&) const = default;
	};

	/**
	 * Exact VFS/runtime coordinates.  Pointers are only one coordinate; no pointer or VFS name is
	 * accepted as a stand-alone authority.  Opaque runtime/source/alias/registration receipts are
	 * retained so an unload, unregister, or replacement cannot pass an endpoint-equality check.
	 */
	struct sqlite_shm_vfs_identity_sample
	{
		const void* forwarding_vfs{};
		const void* underlying_vfs{};
		const void* underlying_app_data{};
		sqlite_backend_opaque_identity runtime_image;
		sqlite_backend_opaque_identity sqlite_source_id;
		sqlite_backend_opaque_identity shared_runtime_vfs_cohort;
		sqlite_backend_opaque_identity alias_lifetime;
		sqlite_backend_opaque_identity registration_epoch;

		[[nodiscard]] bool operator==(const sqlite_shm_vfs_identity_sample&) const = default;
	};

	/** Immutable process/runtime/VFS/file-family anchor for one mapping coordinator. */
	struct sqlite_shm_identity_epoch_anchor
	{
		sqlite_shm_process_identity_sample process;
		sqlite_shm_vfs_identity_sample vfs;
		sqlite_backend_opaque_identity exact_file_family;
		sqlite_backend_opaque_identity open_epoch;
		sqlite_backend_opaque_identity callback_cohort;

		[[nodiscard]] bool operator==(const sqlite_shm_identity_epoch_anchor&) const = default;
	};

	/** Caller-side coordinates captured before one native map attempt. */
	struct sqlite_shm_identity_epoch_reservation_request
	{
		sqlite_shm_identity_epoch_anchor anchor;
		std::uint64_t generation{};
		sqlite_backend_opaque_identity generation_identity;
		int page_number{};
		int page_size{};
		std::uint64_t byte_offset{};
		std::uint64_t byte_count{};
		const volatile void* native_mapping{};
		std::uint64_t sealed_shm_size{};
		bool extension_requested{};

		[[nodiscard]] bool
		operator==(const sqlite_shm_identity_epoch_reservation_request&) const = default;
	};

	/**
	 * Coordinates observed after the native callback.  Mapping and attachment epochs must be
	 * copied from the exact predelegate reservation; the native callback cannot invent either.
	 */
	struct sqlite_shm_identity_epoch_observation
	{
		sqlite_shm_identity_epoch_anchor anchor;
		std::uint64_t generation{};
		sqlite_backend_opaque_identity generation_identity;
		std::uint64_t mapping_epoch{};
		std::uint64_t attachment_epoch{};
		int page_number{};
		int page_size{};
		std::uint64_t byte_offset{};
		std::uint64_t byte_count{};
		const volatile void* native_mapping{};
		std::uint64_t sealed_shm_size{};

		[[nodiscard]] bool operator==(const sqlite_shm_identity_epoch_observation&) const = default;
	};

	/**
	 * Exact coordinates sealed by the registry.  Epoch fields are issuer-assigned; callers cannot
	 * choose them through the reservation request.
	 */
	struct sqlite_shm_identity_epoch_binding
	{
		sqlite_shm_identity_epoch_anchor anchor;
		std::uint64_t generation{};
		sqlite_backend_opaque_identity generation_identity;
		std::uint64_t mapping_epoch{};
		std::uint64_t attachment_epoch{};
		int page_number{};
		int page_size{};
		std::uint64_t byte_offset{};
		std::uint64_t byte_count{};
		const volatile void* native_mapping{};
		std::uint64_t sealed_shm_size{};
		bool extension_requested{};

		[[nodiscard]] bool operator==(const sqlite_shm_identity_epoch_binding&) const = default;
	};

	/** Closed identity retained from one exact predelegate reservation. */
	class sqlite_shm_identity_epoch_receipt final
	{
	  public:
		~sqlite_shm_identity_epoch_receipt() noexcept = default;
		sqlite_shm_identity_epoch_receipt(sqlite_shm_identity_epoch_receipt&&) noexcept = default;
		sqlite_shm_identity_epoch_receipt&
		operator=(sqlite_shm_identity_epoch_receipt&&) noexcept = default;
		sqlite_shm_identity_epoch_receipt(const sqlite_shm_identity_epoch_receipt&) = delete;
		sqlite_shm_identity_epoch_receipt&
		operator=(const sqlite_shm_identity_epoch_receipt&) = delete;

		[[nodiscard]] bool valid() const noexcept;
		[[nodiscard]] std::uint64_t sequence() const noexcept;
		[[nodiscard]] const sqlite_shm_identity_epoch_anchor& anchor() const noexcept;
		[[nodiscard]] std::uint64_t generation() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& generation_identity() const noexcept;
		[[nodiscard]] std::uint64_t mapping_epoch() const noexcept;
		[[nodiscard]] std::uint64_t attachment_epoch() const noexcept;
		[[nodiscard]] int page_number() const noexcept;
		[[nodiscard]] int page_size() const noexcept;
		[[nodiscard]] std::uint64_t byte_offset() const noexcept;
		[[nodiscard]] std::uint64_t byte_count() const noexcept;
		[[nodiscard]] const volatile void* native_mapping() const noexcept;
		[[nodiscard]] std::uint64_t sealed_shm_size() const noexcept;
		[[nodiscard]] bool extension_requested() const noexcept;

	  private:
		friend class sqlite_shm_identity_epoch_registry;

		sqlite_shm_identity_epoch_receipt(
			std::shared_ptr<detail::sqlite_shm_identity_epoch_registry_control> control,
			std::uint64_t sequence,
			sqlite_shm_identity_epoch_binding binding) noexcept;

		std::shared_ptr<detail::sqlite_shm_identity_epoch_registry_control> control_;
		std::uint64_t sequence_{};
		sqlite_shm_identity_epoch_binding binding_;
	};

	/**
	 * Process-only comparison used by the registry and direct fault tests.
	 *
	 * This function performs no recovery and never refreshes the expected identity.  A mismatch
	 * must be treated as inherited/forked or stale process state by the caller.
	 */
	[[nodiscard]] sqlite_shm_identity_epoch_result<void> validate_sqlite_shm_process_identity(
		const sqlite_shm_process_identity_sample& expected,
		const sqlite_shm_process_identity_sample& observed) noexcept;

	/** Exact VFS/runtime comparison independent of the mapping registry. */
	[[nodiscard]] sqlite_shm_identity_epoch_result<void>
	validate_sqlite_shm_vfs_identity(const sqlite_shm_vfs_identity_sample& expected,
									 const sqlite_shm_vfs_identity_sample& observed) noexcept;

	/**
	 * Small source-private coordinator for identity/epoch custody.
	 *
	 * The coordinator owns process-lifetime tombstones.  It does not reclaim old sequence,
	 * mapping, or attachment epochs, so a stale token cannot become valid after pointer storage is
	 * reused (ABA).  Any process/PID/fork/VFS drift permanently quarantines this coordinator.
	 * It has no SQLite callbacks and no native side effects; production wiring remains an explicit
	 * caller responsibility.
	 */
	class sqlite_shm_identity_epoch_registry final
	{
	  public:
		~sqlite_shm_identity_epoch_registry() noexcept = default;
		sqlite_shm_identity_epoch_registry(sqlite_shm_identity_epoch_registry&&) noexcept = default;
		sqlite_shm_identity_epoch_registry&
		operator=(sqlite_shm_identity_epoch_registry&&) noexcept = default;
		sqlite_shm_identity_epoch_registry(const sqlite_shm_identity_epoch_registry&) = delete;
		sqlite_shm_identity_epoch_registry&
		operator=(const sqlite_shm_identity_epoch_registry&) = delete;

		[[nodiscard]] static sqlite_shm_identity_epoch_result<sqlite_shm_identity_epoch_registry>
		create(sqlite_shm_identity_epoch_anchor anchor);

		[[nodiscard]] bool valid() const noexcept;
		[[nodiscard]] bool quarantined() const noexcept;
		[[nodiscard]] const sqlite_shm_identity_epoch_anchor& anchor() const noexcept;

		[[nodiscard]] sqlite_shm_identity_epoch_result<sqlite_shm_identity_epoch_receipt>
		reserve(const sqlite_shm_identity_epoch_reservation_request& request);

		/** Validate and commit one exact mapped callback observation. */
		[[nodiscard]] sqlite_shm_identity_epoch_result<void>
		validate_map(sqlite_shm_identity_epoch_receipt& receipt,
					 const sqlite_shm_identity_epoch_observation& observation) noexcept;

		/** Validate monotonic same-epoch growth; pointer or identity drift is never resized in
		 * place. */
		[[nodiscard]] sqlite_shm_identity_epoch_result<void>
		validate_resize(sqlite_shm_identity_epoch_receipt& receipt,
						const sqlite_shm_identity_epoch_observation& observation,
						std::uint64_t requested_range_end) noexcept;

		/** Validate one non-deleting native unmap before the receipt can be retired. */
		[[nodiscard]] sqlite_shm_identity_epoch_result<void>
		validate_unmap(sqlite_shm_identity_epoch_receipt& receipt,
					   const sqlite_shm_identity_epoch_observation& observation,
					   bool delete_flag) noexcept;

		/** Consume a reserved-but-never-delegated attempt without permitting replay. */
		[[nodiscard]] sqlite_shm_identity_epoch_result<void>
		cancel(sqlite_shm_identity_epoch_receipt& receipt) noexcept;

		/** Retire only after validate_unmap succeeded. */
		[[nodiscard]] sqlite_shm_identity_epoch_result<void>
		retire(sqlite_shm_identity_epoch_receipt& receipt) noexcept;

		/** Sticky fail-closed transition for fork, unload, unknown native outcome, or custody loss.
		 */
		void quarantine() noexcept;

	  private:
		explicit sqlite_shm_identity_epoch_registry(
			std::shared_ptr<detail::sqlite_shm_identity_epoch_registry_control> control) noexcept;
		[[nodiscard]] sqlite_shm_identity_epoch_result<void>
		check_receipt(detail::sqlite_shm_identity_epoch_registry_control& state,
					  sqlite_shm_identity_epoch_receipt& receipt,
					  detail::sqlite_shm_identity_epoch_row*& row) noexcept;
		std::shared_ptr<detail::sqlite_shm_identity_epoch_registry_control> control_;
	};
} // namespace cxxlens::sdk
