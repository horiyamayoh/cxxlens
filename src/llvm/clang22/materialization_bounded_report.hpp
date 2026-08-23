#pragma once

/**
 * @file materialization_bounded_report.hpp
 * @brief Publication-independent bounded report-tail writer.
 *
 * This source-private port reserves the maximum post-publication tail before an irreversible
 * Store call.  It retains only sealed bytes in the anonymous private spool and exposes semantic
 * and content digests after finalization.  Publication attribution remains outside this writer.
 */

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <cxxlens/sdk/common.hpp>

#include "materialization_io.hpp"

namespace cxxlens::detail::clang22::materialization
{
	struct materialization_bounded_report_limits
	{
		static constexpr std::uint64_t default_max_report_bytes = 1U * 1024U * 1024U * 1024U;
		static constexpr std::uint64_t default_max_tail_bytes = 28'321'546U;

		std::uint64_t max_report_bytes{default_max_report_bytes};
		std::uint64_t max_tail_bytes{default_max_tail_bytes};
	};

	/** Store terminal values are data supplied by the Store boundary, never inferred by the report.
	 */
	enum class materialization_bounded_report_terminal : std::uint8_t
	{
		not_attempted,
		rejected_stale,
		rejected_store_failure,
		publication_outcome_unknown,
		committed_unverified,
		committed_verified,
	};

	[[nodiscard]] constexpr bool
	is_valid(const materialization_bounded_report_terminal value) noexcept
	{
		return value >= materialization_bounded_report_terminal::not_attempted &&
			value <= materialization_bounded_report_terminal::committed_verified;
	}

	/**
	 * Move-only report-tail writer.  The writer does not produce a report DOM or retain an
	 * unbounded byte vector; append is limited by the checked tail and report budgets.
	 */
	class materialization_bounded_report_writer final
	{
	  public:
		materialization_bounded_report_writer(const materialization_bounded_report_writer&) =
			delete;
		materialization_bounded_report_writer&
		operator=(const materialization_bounded_report_writer&) = delete;
		materialization_bounded_report_writer(materialization_bounded_report_writer&&) noexcept;
		materialization_bounded_report_writer&
		operator=(materialization_bounded_report_writer&&) noexcept;
		~materialization_bounded_report_writer();

		[[nodiscard]] static sdk::result<materialization_bounded_report_writer>
		create(std::unique_ptr<materialization_private_spool> storage,
			   materialization_bounded_report_limits limits = {});

		/** Reserve the checked maximum tail before publication. */
		[[nodiscard]] sdk::result<void> reserve();
		[[nodiscard]] sdk::result<void> append(std::span<const std::byte> bytes);
		[[nodiscard]] sdk::result<void> finalize(materialization_bounded_report_terminal terminal);

		[[nodiscard]] bool reserved() const noexcept;
		[[nodiscard]] bool finalized() const noexcept;
		[[nodiscard]] std::uint64_t bytes_written() const noexcept;
		[[nodiscard]] std::optional<materialization_bounded_report_terminal>
		terminal() const noexcept;

		/** Exact content and semantic digests of the sealed report-tail bytes. */
		[[nodiscard]] sdk::result<std::string> content_digest() const;
		[[nodiscard]] sdk::result<std::string> semantic_digest() const;

	  private:
		struct state;
		explicit materialization_bounded_report_writer(std::unique_ptr<state> state);
		std::unique_ptr<state> state_;
	};
} // namespace cxxlens::detail::clang22::materialization
