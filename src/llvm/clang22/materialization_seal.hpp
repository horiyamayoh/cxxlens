#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "materialization_request.hpp"
#include "materialization_task_spool.hpp"
#include "observation_v2.hpp"
#include "sdk/provider_validation_internal.hpp"

namespace cxxlens::detail::clang22::materialization
{
	/** One observation row rebound from the generic seal to the installed task authority. */
	struct sealed_observation_v2_row
	{
		std::size_t batch_index{};
		std::size_t row_index{};
		decoded_observation_v2_row observation;
	};

	/**
	 * Source-private v2.1 task authority passed to the streaming seal boundary. The task.v3
	 * occurrence is retained in one sealed spool and is consumed exactly once by validation.
	 */
	struct streamed_validated_materialization_task_request
	{
		clang22_task_input worker_input;
		/** Non-owning request authority; the v2.1 admission token outlives this seal call. */
		const sdk::project_catalog* catalog{};
		clang22_task_source_receipt source_receipt;
		std::string provider_task_id;
		std::string provider_execution_id;
		std::string task_input_digest;
		sdk::provider::sandbox_requirement sandbox;
		std::unique_ptr<clang22_task_input_spool> worker_payload;
	};

	/**
	 * Tool-private immutable claim-adoption boundary for one Clang 22 task.
	 *
	 * The value owns the shared validator's generic seal. Base and span claims are reconstructed
	 * only from the already validated task authority and rows decoded from that seal. Raw provider
	 * frames and report prose are therefore not representable at this boundary.
	 */
	class sealed_materialization_result
	{
	  public:
		sealed_materialization_result(const sealed_materialization_result&) = delete;
		sealed_materialization_result& operator=(const sealed_materialization_result&) = delete;
		sealed_materialization_result(sealed_materialization_result&&) noexcept = default;
		sealed_materialization_result&
		operator=(sealed_materialization_result&&) noexcept = default;
		~sealed_materialization_result() = default;

		[[nodiscard]] std::string_view provider_task_id() const noexcept;
		[[nodiscard]] std::string_view task_input_digest() const noexcept;
		/** Physical correlation only; excluded from semantic claim/base identities. */
		[[nodiscard]] std::string_view provider_execution_id() const noexcept;
		[[nodiscard]] std::string_view selected_catalog_compile_unit_id() const noexcept;
		[[nodiscard]] std::string_view final_relation_compile_unit_id() const noexcept;
		[[nodiscard]] const sdk::provider::detail::sealed_provider_transcript&
		provider_seal() const noexcept;
		/** Exactly project, toolchain, variant, source.file, and compile_unit in dependency order.
		 */
		[[nodiscard]] std::span<const sdk::detached_row> base_claim_rows() const noexcept;
		/** Canonically ordered, full-bundle-deduplicated source.span rows. */
		[[nodiscard]] std::span<const sdk::detached_row> source_span_claim_rows() const noexcept;
		[[nodiscard]] std::span<const sealed_observation_v2_row> observation_rows() const noexcept;

	  private:
		sealed_materialization_result(
			std::string provider_task_id,
			std::string task_input_digest,
			std::string provider_execution_id,
			std::string selected_catalog_compile_unit_id,
			std::string final_relation_compile_unit_id,
			sdk::provider::detail::sealed_provider_transcript provider_seal,
			std::vector<sdk::detached_row> base_claim_rows,
			std::vector<sdk::detached_row> source_span_claim_rows,
			std::vector<sealed_observation_v2_row> observation_rows);

		std::string provider_task_id_;
		std::string task_input_digest_;
		std::string provider_execution_id_;
		std::string selected_catalog_compile_unit_id_;
		std::string final_relation_compile_unit_id_;
		sdk::provider::detail::sealed_provider_transcript provider_seal_;
		std::vector<sdk::detached_row> base_claim_rows_;
		std::vector<sdk::detached_row> source_span_claim_rows_;
		std::vector<sealed_observation_v2_row> observation_rows_;

		friend sdk::result<sealed_materialization_result> validate_and_seal_materialization(
			const validated_task_request& request,
			sdk::provider::detail::sealed_provider_transcript&& provider_seal);
		friend sdk::result<sealed_materialization_result> seal_validated_provider_result(
			const clang22_task_input& worker_input,
			std::string_view provider_task_id,
			std::string_view task_input_digest,
			std::string_view provider_execution_id,
			sdk::provider::detail::sealed_provider_transcript&& provider_seal);
	};

	/** Consume one generic immutable seal and independently establish the higher task seal. */
	[[nodiscard]] sdk::result<sealed_materialization_result> validate_and_seal_materialization(
		const validated_task_request& request,
		sdk::provider::detail::sealed_provider_transcript&& provider_seal);

	/** Consume one v2.1 task.v3 spool without constructing a resident payload vector. */
	[[nodiscard]] sdk::result<sealed_materialization_result> validate_and_seal_materialization(
		streamed_validated_materialization_task_request request,
		sdk::provider::detail::sealed_provider_transcript&& provider_seal);
} // namespace cxxlens::detail::clang22::materialization
