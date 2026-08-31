#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "source_closure.hpp"
#include "source_closure_transport.hpp"

namespace cxxlens::detail::clang22
{
	/**
	 * A task-local source-closure sink with explicit transport bounds.
	 *
	 * The sink accepts only the canonical manifest emitted by the task-v4 source-closure
	 * builder.  It keeps manifest metadata and content-addressed blob bytes in one bounded
	 * task-local value backed by a private sealed spool; it never resolves a path from the ambient
	 * filesystem.  A validated snapshot is available only after every manifest-declared blob has
	 * been sealed.
	 */
	class source_closure_spool final : public source_closure_transfer_sink
	{
	  public:
		explicit source_closure_spool(source_closure_transport_limits limits = {});
		~source_closure_spool() override;

		source_closure_spool(const source_closure_spool&) = delete;
		source_closure_spool& operator=(const source_closure_spool&) = delete;
		source_closure_spool(source_closure_spool&&) = delete;
		source_closure_spool& operator=(source_closure_spool&&) = delete;

		[[nodiscard]] sdk::result<void>
		begin_manifest(const source_closure_manifest_descriptor& descriptor) override;
		[[nodiscard]] sdk::result<void>
		append_manifest(std::span<const std::byte> payload) override;
		[[nodiscard]] sdk::result<source_closure_manifest_summary>
		finish_manifest(std::string_view manifest_digest) override;
		[[nodiscard]] sdk::result<void>
		begin_blob(const source_closure_blob_descriptor& descriptor) override;
		[[nodiscard]] sdk::result<void> append_blob(std::span<const std::byte> payload) override;
		[[nodiscard]] sdk::result<void>
		finish_blob(const source_closure_blob_receipt& receipt) override;
		[[nodiscard]] sdk::result<source_closure_ack_credentials>
		finish_closure(std::string_view transfer_digest) override;
		[[nodiscard]] sdk::result<std::string> cleanup() override;

		/** Return the immutable snapshot after a successful closure seal. */
		[[nodiscard]] sdk::result<source_closure_snapshot> snapshot() const;
		/** Return bytes currently retained by this task-local spool. */
		[[nodiscard]] std::uint64_t retained_bytes() const noexcept;
		/** Return whether the closure has been sealed and not cleaned up. */
		[[nodiscard]] bool sealed() const noexcept;

	  private:
		enum class state : std::uint8_t
		{
			fresh,
			manifest_open,
			manifest_validated,
			blob_open,
			blob_sealed,
			closure_sealed,
			cleaned,
		};

		[[nodiscard]] sdk::result<void> reject(std::string field, std::string detail = {}) const;
		[[nodiscard]] sdk::result<void> reject_limit(std::string field) const;
		[[nodiscard]] sdk::result<std::string> credential(std::string_view prefix,
														  std::string_view transfer_digest) const;
		[[nodiscard]] sdk::result<void> ensure_private_spool();
		[[nodiscard]] sdk::result<void> append_private_spool(std::span<const std::byte> payload);
		[[nodiscard]] sdk::result<std::string> read_private_blob(std::uint64_t offset,
																 std::uint64_t size) const;
		[[nodiscard]] sdk::result<void> seal_private_spool() const;
		[[nodiscard]] sdk::result<void> validate_sealed_metadata() const;
		void close_private_spool() noexcept;

		source_closure_transport_limits limits_;
		state state_{state::fresh};
		source_closure_manifest_descriptor manifest_descriptor_;
		std::string manifest_bytes_;
		source_closure_snapshot snapshot_;
		std::uint64_t manifest_received_bytes_{};
		std::uint64_t completed_blob_count_{};
		std::uint64_t completed_blob_bytes_{};
		std::size_t current_blob_ordinal_{};
		std::string current_blob_digest_;
		std::uint64_t current_blob_size_{};
		std::uint64_t current_blob_offset_{};
		std::uint64_t current_blob_received_bytes_{};
		std::uint64_t private_spool_bytes_{};
		int private_spool_fd_{-1};
		std::vector<std::uint64_t> blob_offsets_;
		std::string transfer_digest_;
		source_closure_ack_credentials credentials_;
		std::string cleanup_receipt_;
	};

	/**
	 * Task-local ownership of a sealed source-closure spool.
	 *
	 * The receiver returns this lease only after it has emitted the authenticated ACK.  A worker
	 * crash or connection loss can therefore clean the private backend without fabricating a peer
	 * reject.  The lease is also best-effort RAII cleanup for callers which abandon the result.
	 */
	enum class source_closure_relay_terminal : std::uint8_t
	{
		open,
		sealed,
		cancelled,
		cleaned,
		connection_lost,
		worker_crashed,
	};

	class source_closure_spool_relay final
	{
	  public:
		explicit source_closure_spool_relay(source_closure_transport_limits limits = {});
		~source_closure_spool_relay();

		source_closure_spool_relay(const source_closure_spool_relay&) = delete;
		source_closure_spool_relay& operator=(const source_closure_spool_relay&) = delete;

		[[nodiscard]] source_closure_spool& sink() noexcept
		{
			return spool_;
		}
		[[nodiscard]] const source_closure_spool& sink() const noexcept
		{
			return spool_;
		}
		[[nodiscard]] sdk::result<void> mark_sealed() noexcept;
		[[nodiscard]] sdk::result<std::string> cancel();
		[[nodiscard]] sdk::result<std::string> cleanup();
		[[nodiscard]] sdk::result<std::string> connection_lost(bool cancel_observed = false);
		[[nodiscard]] sdk::result<std::string> worker_crashed(bool cancel_observed = false);
		[[nodiscard]] source_closure_relay_terminal terminal() const noexcept
		{
			return terminal_;
		}
		[[nodiscard]] bool cancel_observed() const noexcept
		{
			return cancel_observed_;
		}

	  private:
		[[nodiscard]] sdk::result<std::string> terminate(source_closure_relay_terminal terminal,
														 bool cancel_observed);

		source_closure_spool spool_;
		source_closure_relay_terminal terminal_{source_closure_relay_terminal::open};
		bool cancel_observed_{};
	};
} // namespace cxxlens::detail::clang22
