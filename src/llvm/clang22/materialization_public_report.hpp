#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "materialization_bounded_claim_source.hpp"
#include "materialization_claims.hpp"
#include "materialization_json.hpp"
#include "materialization_occurrence.hpp"
#include "materialization_report.hpp"
#include "materialization_request_v2_1.hpp"
#include "materialization_rooted_vfs.hpp"
#include "materialization_store.hpp"

namespace cxxlens::detail::clang22::materialization
{
	/** Closed category used by the public success-report construction boundary. */
	enum class public_materialization_report_error_kind : std::uint8_t
	{
		missing_authority,
		invalid_projection,
		publication_unverified,
		limit_exceeded,
	};

	/**
	 * Report-construction failure retaining every missing authority field.
	 *
	 * The public SDK result carries the same information as `materialization.report-invalid`; this
	 * value is provided so callers can retain the typed classification before crossing that result
	 * boundary. `missing_fields` is deterministic and never contains a guessed replacement value.
	 */
	struct public_materialization_report_error
	{
		public_materialization_report_error_kind kind{
			public_materialization_report_error_kind::invalid_projection};
		std::vector<std::string> missing_fields;
		std::string field;
		std::string detail;

		[[nodiscard]] sdk::error as_sdk_error() const;
	};

	/**
	 * Already-authorized projections which are not derivable from the four execution authorities.
	 *
	 * Values must be produced by the corresponding schema/contract validators. This type does not
	 * claim to validate arbitrary JSON against the complete YAML schema; it only checks the
	 * required top-level shape and the publication invariants before emitting a passed report.
	 */
	struct public_materialization_authority_projections
	{
		std::map<std::string, json_value, utf8_byte_less> values;
	};

	/**
	 * Checked response-capacity reservation minted from the accepted report-limit profile.
	 *
	 * The constructor and stored proof are source-private. Callers can only obtain this value from
	 * `check_public_materialization_capacity_reservation`, so a projection cannot claim that an
	 * arbitrary byte count was checked by copying its own `reserved_bytes` member into the
	 * consumption call.
	 */
	class public_materialization_capacity_reservation final
	{
	  public:
		public_materialization_capacity_reservation(
			const public_materialization_capacity_reservation&) = delete;
		public_materialization_capacity_reservation&
		operator=(const public_materialization_capacity_reservation&) = delete;
		public_materialization_capacity_reservation(
			public_materialization_capacity_reservation&&) noexcept = default;
		public_materialization_capacity_reservation&
		operator=(public_materialization_capacity_reservation&&) noexcept = default;
		~public_materialization_capacity_reservation() = default;

		[[nodiscard]] std::size_t reserved_bytes() const noexcept
		{
			return reserved_bytes_;
		}

		[[nodiscard]] std::string_view proof_digest() const noexcept
		{
			return proof_digest_;
		}

	  private:
		public_materialization_capacity_reservation(std::size_t reserved_bytes,
													std::string proof_digest) noexcept;

		std::size_t reserved_bytes_{};
		std::string proof_digest_;

		friend sdk::result<public_materialization_capacity_reservation>
		check_public_materialization_capacity_reservation(const detailed_report_limits& limits);
	};

	/** Validate and mint the source-private checked tail-capacity proof. */
	[[nodiscard]] sdk::result<public_materialization_capacity_reservation>
	check_public_materialization_capacity_reservation(const detailed_report_limits& limits);

	/**
	 * Publication-independent report authority reserved before the Store publish call.
	 *
	 * This projection deliberately contains no claim, publication, or query value.  It binds the
	 * authenticated request/input/installation census and reserves the complete report byte budget;
	 * the post-publication builder must recompute and match it before emitting success.
	 */
	class public_materialization_prepublication_projection final
	{
	  public:
		public_materialization_prepublication_projection(std::string binding_digest,
														 std::string request_digest,
														 std::string semantic_request_digest,
														 std::string occurrence_inventory_digest,
														 std::uint64_t task_count,
														 std::size_t reserved_bytes,
														 std::string capacity_proof_digest);
		public_materialization_prepublication_projection(
			const public_materialization_prepublication_projection&) = delete;
		public_materialization_prepublication_projection&
		operator=(const public_materialization_prepublication_projection&) = delete;
		public_materialization_prepublication_projection(
			public_materialization_prepublication_projection&&) noexcept = default;
		public_materialization_prepublication_projection&
		operator=(public_materialization_prepublication_projection&&) noexcept = default;
		~public_materialization_prepublication_projection() = default;

		/** Consume exactly the independently checked response budget before publication. */
		[[nodiscard]] sdk::result<void>
		consume_reserved_capacity(const public_materialization_capacity_reservation& capacity);

		[[nodiscard]] bool reservation_consumed() const noexcept
		{
			return state_ == lifecycle_state::consumed;
		}

		/** Validate the post-publication lifecycle state without mutating it. */
		[[nodiscard]] sdk::result<void>
		validate_reserved_capacity(const public_materialization_capacity_reservation& capacity,
								   std::size_t maximum_report_bytes) const;

		/** Compare only publication-independent authority; consumption is lifecycle state. */
		[[nodiscard]] bool
		operator==(const public_materialization_prepublication_projection& other) const noexcept
		{
			return binding_digest_ == other.binding_digest_ &&
				request_digest_ == other.request_digest_ &&
				semantic_request_digest_ == other.semantic_request_digest_ &&
				occurrence_inventory_digest_ == other.occurrence_inventory_digest_ &&
				task_count_ == other.task_count_ && reserved_bytes_ == other.reserved_bytes_ &&
				capacity_proof_digest_ == other.capacity_proof_digest_;
		}

	  private:
		enum class lifecycle_state : std::uint8_t
		{
			reserved,
			consumed,
		};

		std::string binding_digest_;
		std::string request_digest_;
		std::string semantic_request_digest_;
		std::string occurrence_inventory_digest_;
		std::uint64_t task_count_{};
		std::size_t reserved_bytes_{};
		std::string capacity_proof_digest_;
		lifecycle_state state_{lifecycle_state::reserved};
	};

	/** Observable result of the source-private prior-artifact write after Store commit. */
	struct public_materialization_prior_artifact_persistence
	{
		bool committed{};
		std::string error_code;
		std::string error_field;
		std::string error_detail;
	};

	/**
	 * Re-bind request installation assertions to the measured installed occurrence.
	 *
	 * This is a source-private report-construction validator. It does not introduce a public Store
	 * or SDK error: the occurrence manifest and rooted receipt remain the measured authorities.
	 */
	[[nodiscard]] sdk::result<void> validate_materialization_public_report_occurrence_binding(
		const materialization_v2_1_tool_authority& tool,
		const materialization_v2_1_worker_authority& worker,
		const materialization_occurrence_manifest& occurrence_manifest,
		const materialization_occurrence_receipt& occurrence_receipt);

	/** Build the bounded publication-independent projection before publication is attempted. */
	[[nodiscard]] sdk::result<public_materialization_prepublication_projection>
	prepare_public_materialization_prepublication_projection(
		const validated_materialization_request_v2_1& request,
		const raw_input_observation& raw_input,
		const materialization_occurrence_manifest& occurrence_manifest,
		const materialization_occurrence_receipt& occurrence_receipt,
		const public_materialization_capacity_reservation& capacity);

	/** Inputs to the fail-closed public v2.1 success-report builder. */
	struct public_materialization_success_report_input
	{
		const validated_materialization_request_v2_1* request{};
		const json_document* request_globals{};
		const detailed_task_report_accumulator* task_reports{};
		/** Replayable production task-report source; mutually exclusive with task_reports. */
		const detailed_task_report_replayable_spool* task_report_spool{};
		const raw_input_observation* raw_input{};
		const materialization_occurrence_manifest* occurrence_manifest{};
		const materialization_occurrence_receipt* occurrence_receipt{};
		/** Production report authority; mutually exclusive with the qualification-only claims view.
		 */
		materialization_bounded_claim_source* bounded_claims{};
		/** Qualification-only resident oracle retained for adapter/reference tests. */
		const sealed_materialization_claims* claims{};
		const materialization_store_observation* store{};
		const public_materialization_capacity_reservation* capacity_reservation{};
		const public_materialization_prepublication_projection* prepublication{};
		const public_materialization_prior_artifact_persistence* prior_artifact_persistence{};
		/** Exact rooted-VFS receipt retained by the SQLite opener, when the backend is SQLite. */
		const materialization_rooted_vfs_receipt* rooted_vfs_receipt{};
		std::string generated_at;
		public_materialization_authority_projections projections;
		std::size_t maximum_report_bytes{detailed_report_limits::maximum_report_bytes};
	};

	/**
	 * Complete immutable model for one public `detailed/passed` response.
	 *
	 * Construction is private: a model can only be obtained after the builder has checked the
	 * actual request, occurrence, claims, and post-publication Store observation.
	 */
	class public_materialization_success_report_model
	{
	  public:
		public_materialization_success_report_model(
			const public_materialization_success_report_model&) = delete;
		public_materialization_success_report_model&
		operator=(const public_materialization_success_report_model&) = delete;
		public_materialization_success_report_model(
			public_materialization_success_report_model&&) noexcept = default;
		public_materialization_success_report_model&
		operator=(public_materialization_success_report_model&&) noexcept = default;
		~public_materialization_success_report_model() = default;

		[[nodiscard]] std::string_view generated_at() const noexcept;
		[[nodiscard]] const std::map<std::string, json_value, utf8_byte_less>&
		fields() const noexcept;

	  private:
		public_materialization_success_report_model(
			std::string generated_at,
			std::map<std::string, json_value, utf8_byte_less> fields,
			std::size_t maximum_report_bytes) noexcept;

		std::string generated_at_;
		std::map<std::string, json_value, utf8_byte_less> fields_;
		std::size_t maximum_report_bytes_{};

		friend sdk::result<public_materialization_success_report_model>
		build_public_materialization_success_report(
			const public_materialization_success_report_input& input);
		friend sdk::result<std::string> encode_public_materialization_success_report(
			const public_materialization_success_report_model& model);
	};

	/** Build a passed model only after exact post-publication Store verification succeeds. */
	[[nodiscard]] sdk::result<public_materialization_success_report_model>
	build_public_materialization_success_report(
		const public_materialization_success_report_input& input);

	/** Encode the model as bounded canonical JSON without a trailing LF. */
	[[nodiscard]] sdk::result<std::string> encode_public_materialization_success_report(
		const public_materialization_success_report_model& model);

	/**
	 * Stage the exact detailed response bytes in one sealed private spool before stdout
	 * publication.
	 *
	 * This is a source-private transport boundary: it does not alter the v2.1 response shape or
	 * promote any Store observation.  The returned spool has been size-checked, sealed, and
	 * re-digested from its immutable bytes; callers must stream it only after the remaining
	 * post-publication validation has succeeded.
	 */
	[[nodiscard]] sdk::result<std::unique_ptr<materialization_replayable_spool>>
	stage_public_materialization_final_response(std::string response,
												std::size_t maximum_report_bytes);
} // namespace cxxlens::detail::clang22::materialization
