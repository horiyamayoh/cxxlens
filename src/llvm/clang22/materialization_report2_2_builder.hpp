#pragma once

/**
 * @file materialization_report2_2_builder.hpp
 * @brief Bounded two-phase report-v2.2 construction without a report DOM.
 */

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

#include <cxxlens/sdk/common.hpp>

#include "materialization_io.hpp"

namespace cxxlens::detail::clang22::materialization
{
	inline constexpr std::string_view materialization_report2_2_schema =
		"cxxlens.clang22-materialization-report.v2";
	inline constexpr std::string_view materialization_report2_2_version = "2.2.0";

	/** Checked limits for one complete response and its post-publication suffix. */
	struct materialization_report2_2_limits
	{
		std::uint64_t maximum_report_bytes{std::uint64_t{1024U} * 1024U * 1024U};
		std::uint64_t maximum_terminal_bytes{28'321'546U};
	};

	/** Exact Store terminal supplied by the Store boundary. */
	enum class materialization_report2_2_store_terminal : std::uint8_t
	{
		not_attempted,
		rejected_stale,
		rejected_store_failure,
		publication_outcome_unknown,
		committed_unverified,
		committed_verified,
	};

	[[nodiscard]] constexpr bool
	is_valid(const materialization_report2_2_store_terminal value) noexcept
	{
		return value >= materialization_report2_2_store_terminal::not_attempted &&
			value <= materialization_report2_2_store_terminal::committed_verified;
	}

	/**
	 * Publication-independent report authority.  It contains identities and bounded census values,
	 * never task rows, source bytes, frames, or a JSON tree.
	 */
	struct materialization_report2_2_prepublication_projection
	{
		std::string materialization_request_id;
		std::string request_digest;
		std::string semantic_request_digest;
		std::string request_authority_digest;
		std::uint64_t task_count{};
		std::uint64_t closure_count{};
		std::uint64_t unique_blob_bytes{};
		std::string closure_receipt_set_digest;
		std::string worker_result_set_digest;
		std::string base_row_result_set_digest;
		std::string task_receipt_digest;
		std::string store_source_digest;
		std::string store_preparation_digest;
		std::string expected_projection_digest;
		std::string actual_projection_digest;
		std::string journal_digest;

		[[nodiscard]] bool
		operator==(const materialization_report2_2_prepublication_projection&) const = default;
	};

	/** Store-dependent report authority appended only after the publication boundary. */
	struct materialization_report2_2_terminal_projection
	{
		materialization_report2_2_store_terminal terminal{
			materialization_report2_2_store_terminal::not_attempted};
		std::string store_preparation_digest;
		std::string store_result_digest;
		std::optional<std::string> publication_id;
		std::uint32_t publish_call_count{};

		[[nodiscard]] bool
		operator==(const materialization_report2_2_terminal_projection&) const = default;
	};

	/** Recomputed identities for a sealed, schema-validated report byte stream. */
	struct materialization_report2_2_receipt
	{
		std::string schema{materialization_report2_2_schema};
		std::string report_version{materialization_report2_2_version};
		std::uint64_t byte_count{};
		std::string content_digest;
		std::string semantic_digest;
		std::string prepublication_projection_digest;
		std::string terminal_projection_digest;
		materialization_report2_2_store_terminal terminal{
			materialization_report2_2_store_terminal::not_attempted};

		[[nodiscard]] bool operator==(const materialization_report2_2_receipt&) const = default;
	};

	/** Narrow append-only view supplied to a schema-specific streaming encoder. */
	class materialization_report2_2_chunk_sink
	{
	  public:
		virtual ~materialization_report2_2_chunk_sink() = default;
		[[nodiscard]] virtual sdk::result<void> append(std::span<const std::byte> bytes) = 0;
	};

	/**
	 * Schema-specific encoder/validator port.
	 *
	 * The concrete production instance is configured with the validated request, occurrence,
	 * worker transcripts, claims, base rows, and Store preparation. The three ordered set digests
	 * in the projection bind those streamed details without placing a report DOM at this boundary.
	 * `write_prepublication` must leave the document open at the single Store-terminal insertion
	 * point. `write_terminal` closes it. `validate_sealed` independently validates the complete
	 * report-v2.2 stream and must prove its detailed leaves match every supplied projection digest;
	 * it must not retain the spool reference.
	 */
	class materialization_report2_2_projection_port
	{
	  public:
		virtual ~materialization_report2_2_projection_port() = default;
		[[nodiscard]] virtual sdk::result<void>
		write_prepublication(const materialization_report2_2_prepublication_projection& projection,
							 materialization_report2_2_chunk_sink& sink,
							 std::stop_token cancellation) = 0;
		[[nodiscard]] virtual sdk::result<void>
		write_terminal(const materialization_report2_2_terminal_projection& projection,
					   materialization_report2_2_chunk_sink& sink) = 0;
		[[nodiscard]] virtual sdk::result<void>
		validate_sealed(const materialization_report2_2_prepublication_projection& prepublication,
						const materialization_report2_2_terminal_projection& terminal,
						materialization_replayable_spool& bytes,
						const materialization_report2_2_receipt& receipt) = 0;
	};

	/**
	 * Replayable storage which can physically reserve the post-publication suffix after the prefix
	 * is written. A normal spool cannot cross this boundary. Production implementations must make
	 * ENOSPC or quota failure observable before returning success; sparse logical length alone is
	 * not a reservation.
	 */
	class materialization_report2_2_reserved_spool : public materialization_replayable_spool
	{
	  public:
		~materialization_report2_2_reserved_spool() override = default;
		[[nodiscard]] virtual materialization_io_result<void>
		reserve_terminal_bytes(std::uint64_t bytes) = 0;
		[[nodiscard]] virtual std::uint64_t reserved_terminal_bytes() const noexcept = 0;
	};

	/** Complete report bytes; no in-memory report object exists. */
	class sealed_materialization_report2_2
	{
	  public:
		sealed_materialization_report2_2(const sealed_materialization_report2_2&) = delete;
		sealed_materialization_report2_2&
		operator=(const sealed_materialization_report2_2&) = delete;
		sealed_materialization_report2_2(sealed_materialization_report2_2&&) noexcept = default;
		sealed_materialization_report2_2&
		operator=(sealed_materialization_report2_2&&) noexcept = default;
		~sealed_materialization_report2_2() = default;

		[[nodiscard]] const materialization_report2_2_receipt& receipt() const noexcept;
		[[nodiscard]] materialization_replayable_spool& bytes() noexcept;
		[[nodiscard]] std::unique_ptr<materialization_replayable_spool> take_bytes() && noexcept;

	  private:
		sealed_materialization_report2_2(std::unique_ptr<materialization_replayable_spool> bytes,
										 materialization_report2_2_receipt receipt);

		std::unique_ptr<materialization_replayable_spool> bytes_;
		materialization_report2_2_receipt receipt_;

		friend class materialization_report2_2_builder;
	};

	/** Move-only prepublication state. Destruction before `finalize` performs no Store effect. */
	class materialization_report2_2_builder
	{
	  public:
		// Source-private implementation state is exposed only so the bounded sink in the
		// translation unit can enforce the two independent byte budgets.
		struct state;

		materialization_report2_2_builder(const materialization_report2_2_builder&) = delete;
		materialization_report2_2_builder&
		operator=(const materialization_report2_2_builder&) = delete;
		materialization_report2_2_builder(materialization_report2_2_builder&&) noexcept;
		materialization_report2_2_builder& operator=(materialization_report2_2_builder&&) noexcept;
		~materialization_report2_2_builder();

		[[nodiscard]] static sdk::result<materialization_report2_2_builder>
		prepare(std::unique_ptr<materialization_report2_2_reserved_spool> storage,
				materialization_report2_2_prepublication_projection projection,
				materialization_report2_2_projection_port& port,
				materialization_report2_2_limits limits = {},
				const std::stop_token& cancellation = {});

		[[nodiscard]] bool terminal_space_reserved() const noexcept;
		[[nodiscard]] std::uint64_t prepublication_bytes() const noexcept;

		/** Append the Store terminal, seal, hash, and independently validate the byte stream. */
		[[nodiscard]] sdk::result<sealed_materialization_report2_2>
		finalize(const materialization_report2_2_terminal_projection& terminal) &&;

	  private:
		explicit materialization_report2_2_builder(std::unique_ptr<state> state);
		std::unique_ptr<state> state_;
	};

	[[nodiscard]] sdk::result<std::string> materialization_report2_2_prepublication_digest(
		const materialization_report2_2_prepublication_projection& projection);
	[[nodiscard]] sdk::result<std::string> materialization_report2_2_terminal_digest(
		const materialization_report2_2_terminal_projection& projection);
} // namespace cxxlens::detail::clang22::materialization
