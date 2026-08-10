#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "materialization_claims.hpp"
#include "materialization_json.hpp"
#include "materialization_occurrence.hpp"
#include "materialization_report.hpp"
#include "materialization_request_v2_1.hpp"
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

	/** Inputs to the fail-closed public v2.1 success-report builder. */
	struct public_materialization_success_report_input
	{
		const validated_materialization_request_v2_1* request{};
		const json_document* request_globals{};
		const raw_input_observation* raw_input{};
		const materialization_occurrence_manifest* occurrence_manifest{};
		const materialization_occurrence_receipt* occurrence_receipt{};
		const sealed_materialization_claims* claims{};
		const materialization_store_observation* store{};
		std::string generated_at;
		public_materialization_authority_projections projections;
		std::size_t maximum_report_bytes{64U * 1024U * 1024U};
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

	/** Encode the model as one bounded canonical JSON line of report schema v2.1. */
	[[nodiscard]] sdk::result<std::string> encode_public_materialization_success_report(
		const public_materialization_success_report_model& model);
} // namespace cxxlens::detail::clang22::materialization
