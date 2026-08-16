#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "provider_ng1_validation_internal.hpp"
#include "provider_visibility_internal.hpp"

namespace cxxlens::sdk::provider::detail
{
	/**
	 * Source-private effect boundary for one NG1 append-only spill object.
	 *
	 * The port owns all filesystem/platform effects.  Callers receive only bounded bytes and a
	 * host-observed fsync sequence; no pathname or ambient descriptor is part of the contract.
	 */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_spill_storage_port
	{
	  public:
		virtual ~ng1_spill_storage_port() = default;

		/** Append one already-framed record occurrence or fail closed. */
		[[nodiscard]] virtual result<void> append(std::span<const std::byte> bytes) = 0;
		/** Fsync the complete append prefix and return a positive host sequence. */
		[[nodiscard]] virtual result<std::uint64_t> fsync() = 0;
		/** Read the complete bounded object for source-private recovery. */
		[[nodiscard]] virtual result<std::vector<std::byte>> read_all() const = 0;
		/** Dispose the private object; an unknown cleanup effect is terminal. */
		[[nodiscard]] virtual result<void> cleanup() = 0;
	};

	/** Create the Linux anonymous private spill port; unsupported platforms fail closed. */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<std::unique_ptr<ng1_spill_storage_port>>
	make_system_ng1_spill_storage_port();

	/**
	 * Source-private staging transaction for one exact NG1 spill prefix.
	 *
	 * Each append validates a candidate prefix before the port is mutated, and commits the value
	 * state only after the port reports the complete framed write.  A port effect failure poisons
	 * the transaction: callers may recover or classify it, but may not retry an unknown effect.
	 */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_spill_staging_session
	{
	  public:
		ng1_spill_staging_session() = delete;
		~ng1_spill_staging_session() noexcept;
		ng1_spill_staging_session(const ng1_spill_staging_session&) = delete;
		ng1_spill_staging_session& operator=(const ng1_spill_staging_session&) = delete;
		ng1_spill_staging_session(ng1_spill_staging_session&&) noexcept;
		ng1_spill_staging_session& operator=(ng1_spill_staging_session&&) noexcept;

		[[nodiscard]] static result<ng1_spill_staging_session>
		create(ng1_spill_binding binding, std::unique_ptr<ng1_spill_storage_port> storage);

		/** Append one exact record, preserving prefix state on pre-write rejection. */
		[[nodiscard]] result<void> append(const ng1_spill_record& record);
		/** Fsync and construct the host-observed receipt needed by resume. */
		[[nodiscard]] result<ng1_spill_fsync_receipt>
		fsync(std::uint64_t highest_contiguous_acked_sequence,
			  std::uint64_t highest_observed_sequence,
			  std::string staged_digest);
		/** Re-read and validate the complete stored prefix from the private port. */
		[[nodiscard]] result<ng1_spill_prefix_state> recover();
		/**
		 * Rehydrate a fresh staging transaction from one exact host-observed fsync receipt.
		 *
		 * The receipt is checked against bytes re-read through the injected port; this method does
		 * not itself prove persistence or launch/restart a worker.  A caller must keep worker
		 * termination and resume-token acceptance as separate lifecycle observations.
		 */
		[[nodiscard]] result<void>
		restore_from_fsync_receipt(const ng1_spill_fsync_receipt& receipt);

		/** Cleanup after final report/token disposal or after recovery classification. */
		[[nodiscard]] result<void> cleanup();

		[[nodiscard]] std::uint64_t total_bytes() const noexcept
		{
			return prefix_.total_bytes();
		}
		[[nodiscard]] std::uint64_t total_records() const noexcept
		{
			return prefix_.total_records();
		}
		[[nodiscard]] bool poisoned() const noexcept
		{
			return poisoned_;
		}
		[[nodiscard]] bool cleaned() const noexcept
		{
			return cleaned_;
		}

	  private:
		explicit ng1_spill_staging_session(
			ng1_spill_prefix_state prefix,
			ng1_spill_binding binding,
			std::unique_ptr<ng1_spill_storage_port> storage) noexcept;

		ng1_spill_prefix_state prefix_;
		ng1_spill_binding binding_;
		std::unique_ptr<ng1_spill_storage_port> storage_;
		std::uint64_t last_fsync_sequence_{};
		bool has_fsync_sequence_{};
		bool poisoned_{};
		bool cleaned_{};
	};
} // namespace cxxlens::sdk::provider::detail
