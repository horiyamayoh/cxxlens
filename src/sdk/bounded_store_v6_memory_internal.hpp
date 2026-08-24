#pragma once

/**
 * @file bounded_store_v6_memory_internal.hpp
 * @brief Genesis-only Memory backing used to exercise the v6 phase contract.
 *
 * The common phase core owns every lifecycle token and all compare/report logic.  This file owns
 * only a private canonical frame projection, its authenticated forward cursor, and the exact
 * publication/reopen/cleanup port.  It does not claim to be the public semantic snapshot Store:
 * the Store adapter must provide its own backend-derived semantic observation and lossless typed
 * ingress.  The source helper is intentionally useful only for direct phase tests; it is a
 * move-only cursor, not a callback-owned writer seam.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "bounded_store_v6_internal.hpp"
#include "store_operation_port_internal.hpp"

namespace cxxlens::sdk::detail
{
	/** One small framed input used by the direct Memory source test helper. */
	struct bounded_store_v6_memory_source_frame
	{
		bounded_store_v6_record_kind kind{bounded_store_v6_record_kind::partition_begin};
		std::vector<std::byte> bytes;
	};

	/** Encode the exact 49 + key + payload frame with the DF-0200 domain checksum. */
	[[nodiscard]] result<std::vector<std::byte>>
	encode_bounded_store_v6_memory_frame(bounded_store_v6_record_kind kind,
										 std::span<const std::byte> key,
										 std::span<const std::byte> payload);

	/**
	 * Direct raw task-stream adapter.  It can stage a canonical CXLPEV01 event stream, but cannot
	 * serve as the independent semantic oracle.
	 */
	[[nodiscard]] result<std::unique_ptr<bounded_store_v6_task_frame_source>>
	make_bounded_store_v6_memory_task_frame_source(
		std::vector<bounded_store_v6_memory_source_frame> frames);

	/** Test-only typed oracle decoded into canonical values before traversal. */
	[[nodiscard]] result<std::unique_ptr<bounded_store_v6_expected_semantic_cursor>>
	make_bounded_store_v6_memory_expected_semantic_source(
		std::vector<bounded_store_v6_memory_source_frame> frames);

	/** Process-local genesis-only phase backing. It is not the public snapshot_store authority. */
	class bounded_store_v6_memory_store final
	{
	  public:
		bounded_store_v6_memory_store(snapshot_series_selector selector,
									  std::shared_ptr<store_operation_port> operations);
		~bounded_store_v6_memory_store();
		bounded_store_v6_memory_store(const bounded_store_v6_memory_store&) = delete;
		bounded_store_v6_memory_store& operator=(const bounded_store_v6_memory_store&) = delete;
		bounded_store_v6_memory_store(bounded_store_v6_memory_store&&) noexcept;
		bounded_store_v6_memory_store& operator=(bounded_store_v6_memory_store&&) noexcept;

		[[nodiscard]] result<std::unique_ptr<bounded_store_v6_backend_port>>
		make_backend_port(const bounded_store_v6_session_metadata& metadata);
		[[nodiscard]] result<bounded_store_v6_expected_head> current_head() const;
		[[nodiscard]] std::size_t retained_publication_count() const noexcept;
		[[nodiscard]] std::size_t retained_complete_payload_count() const noexcept;
		[[nodiscard]] std::uint64_t live_staging_payload_count() const noexcept;

	  private:
		struct state;
		std::shared_ptr<state> state_;
		friend class bounded_store_v6_memory_backend_port;
	};

	/**
	 * Memory backend port.  The only complete candidate representation is the segmented immutable
	 * payload owned by this object.  It is sealed before the common core opens the actual cursor;
	 * after sealing no method can append or replace it.
	 */
	class bounded_store_v6_memory_backend_port final : public bounded_store_v6_backend_port
	{
	  public:
		~bounded_store_v6_memory_backend_port() override;
		bounded_store_v6_memory_backend_port(const bounded_store_v6_memory_backend_port&) = delete;
		bounded_store_v6_memory_backend_port&
		operator=(const bounded_store_v6_memory_backend_port&) = delete;

		[[nodiscard]] bounded_store_v6_backend backend() const noexcept override;
		[[nodiscard]] result<void> bind_physical_anchor(
			std::shared_ptr<const bounded_store_v6_physical_anchor> anchor) override;
		[[nodiscard]] std::shared_ptr<const bounded_store_v6_physical_anchor>
		physical_anchor() const noexcept override;
		[[nodiscard]] std::string_view physical_anchor_binding() const noexcept override;
		[[nodiscard]] result<void>
		begin_record(const bounded_store_v6_record_extent& extent) override;
		[[nodiscard]] result<void> append_record_bytes(std::span<const std::byte> bytes) override;
		[[nodiscard]] result<void> finish_record() override;
		[[nodiscard]] result<void> seal_staging() override;
		[[nodiscard]] result<bounded_store_v6_measured_projection>
		measured_projection() const override;
		[[nodiscard]] result<std::unique_ptr<bounded_store_v6_actual_cursor_source>>
		open_actual_cursor() override;
		[[nodiscard]] result<bounded_store_v6_effect_result> publish_once() override;
		[[nodiscard]] result<bounded_store_v6_reopen_observation> reopen() override;
		[[nodiscard]] result<void> abort_staging() override;

	  private:
		bounded_store_v6_memory_backend_port(
			std::shared_ptr<bounded_store_v6_memory_store::state> store,
			bounded_store_v6_session_metadata metadata);
		struct state;
		std::unique_ptr<state> state_;
		friend class bounded_store_v6_memory_store;
	};
} // namespace cxxlens::sdk::detail
