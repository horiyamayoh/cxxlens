#include "materialization_store_outcome.hpp"

#include <array>
#include <string_view>
#include <variant>

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		using outcome_kind = materialization_store_publication_outcome_kind;
		using failure_category = materialization_store_rejected_failure_category;

		[[nodiscard]] sdk::result<materialization_store_publication_outcome> invariant_breach()
		{
			return sdk::unexpected(sdk::error{"store.transaction-state", "publish", {}});
		}

		[[nodiscard]] bool
		is_prepublication_operation(const materialization_store_operation operation) noexcept
		{
			switch (operation)
			{
				case materialization_store_operation::configuration:
				case materialization_store_operation::store_open:
				case materialization_store_operation::head_current:
				case materialization_store_operation::writer_begin:
				case materialization_store_operation::partition_stage:
				case materialization_store_operation::closure_stage:
				case materialization_store_operation::writer_validate:
					return true;
				case materialization_store_operation::writer_publish:
				case materialization_store_operation::store_reopen:
				case materialization_store_operation::verify_current:
				case materialization_store_operation::verify_open_publication:
				case materialization_store_operation::verify_open_snapshot:
				case materialization_store_operation::verify_projection:
					return false;
			}
			return false;
		}

		[[nodiscard]] bool
		is_postpublication_operation(const materialization_store_operation operation) noexcept
		{
			switch (operation)
			{
				case materialization_store_operation::store_reopen:
				case materialization_store_operation::verify_current:
				case materialization_store_operation::verify_open_publication:
				case materialization_store_operation::verify_open_snapshot:
				case materialization_store_operation::verify_projection:
					return true;
				case materialization_store_operation::configuration:
				case materialization_store_operation::store_open:
				case materialization_store_operation::head_current:
				case materialization_store_operation::writer_begin:
				case materialization_store_operation::partition_stage:
				case materialization_store_operation::closure_stage:
				case materialization_store_operation::writer_validate:
				case materialization_store_operation::writer_publish:
					return false;
			}
			return false;
		}

		[[nodiscard]] bool is_publication_invariant_error(const sdk::error& error) noexcept
		{
			return (error.code == "store.transaction-state" && error.field == "publish" &&
					error.detail.empty()) ||
				(error.code == "store.corrupt" && error.field == "publication" &&
				 error.detail == "identity") ||
				error.code == "store.publish-stale-parent";
		}

		[[nodiscard]] bool is_committed_record(const sdk::publication_record& record) noexcept
		{
			return record.state == sdk::publication_state::committed && !record.corrupt;
		}

		[[nodiscard]] bool
		same_publication_identity(const materialization_publication_candidate& candidate,
								  const sdk::publication_record& record) noexcept
		{
			return candidate.publication_id == record.publication_id &&
				candidate.series_id == record.series_id &&
				candidate.snapshot_id == record.snapshot_id &&
				candidate.sequence == record.sequence &&
				candidate.parent_publication == record.parent_publication;
		}

		[[nodiscard]] bool
		valid_recovery_receipt(const materialization_store_observation& observation) noexcept
		{
			if (!observation.recovery_receipt ||
				observation.recovery_receipt->reopen_status ==
					materialization_store_reopen_status::not_attempted ||
				observation.recovery_receipt->selector != observation.selector)
				return false;
			if (!observation.candidate_identity)
				return observation.recovery_receipt->candidate.status ==
					materialization_store_lookup_status::not_applicable &&
					!observation.recovery_receipt->candidate.requested_publication_id &&
					!observation.recovery_receipt->candidate.record &&
					!observation.recovery_receipt->candidate.error;
			return observation.recovery_receipt->candidate.requested_publication_id ==
				observation.candidate_identity->publication_id;
		}

		[[nodiscard]] bool
		valid_success_receipts(const materialization_store_observation& observation,
							   const sdk::publication_record& record,
							   const sdk::snapshot_manifest& manifest) noexcept
		{
			constexpr std::array expected_paths{
				materialization_store_path::current_selector,
				materialization_store_path::open_publication,
				materialization_store_path::open_snapshot,
			};
			for (std::size_t index{}; index < expected_paths.size(); ++index)
			{
				const auto& receipt = observation.verification_receipts[index];
				if (receipt.path != expected_paths[index] ||
					receipt.status != materialization_store_receipt_status::present ||
					receipt.error || !receipt.projection)
					return false;
				const auto& projection = *receipt.projection;
				if (projection.physical_backend != observation.backend ||
					projection.publication != record || projection.manifest != manifest)
					return false;
			}
			return true;
		}

		[[nodiscard]] bool matches_corrupt_tuple(const sdk::error& error) noexcept
		{
			constexpr std::array sqlite_details{
				"backend",
				"column-count",
				"publication-row",
				"series-head-count",
				"series-head",
				"series-head-sequence",
			};
			constexpr std::array publication_details{
				"authority-record",
				"duplicate-publication-id",
				"parent",
				"parent-sequence",
			};
			constexpr std::array series_details{
				"duplicate-sequence",
				"series-roots",
				"series-head-cas",
			};
			if (error.code != "store.corrupt")
				return false;
			const auto contains = [](const auto& values, const std::string_view value)
			{
				for (const auto candidate : values)
					if (candidate == value)
						return true;
				return false;
			};
			return (error.field == "sqlite" && contains(sqlite_details, error.detail)) ||
				(error.field == "exact-publication-id" &&
				 contains(publication_details, error.detail)) ||
				(error.field == "exact-series-id" && contains(series_details, error.detail));
		}

		[[nodiscard]] std::optional<failure_category>
		classify_rejected_failure(const sdk::error& error) noexcept
		{
			if (error.code == "store.counter-overflow" && error.detail.empty() &&
				(error.field == "publication_sequence" || error.field == "physical_generation"))
				return failure_category::counter_overflow;
			if (error.code == "store.hash-collision" &&
				error.field == "exact-candidate-snapshot-id" && error.detail.empty())
				return failure_category::hash_collision;
			if (error.code == "store.snapshot-ambiguous" && error.field == "exact-snapshot-id" &&
				error.detail.empty())
				return failure_category::persistence_corrupt;
			if (matches_corrupt_tuple(error))
				return failure_category::persistence_corrupt;
			return std::nullopt;
		}

		[[nodiscard]] sdk::result<materialization_store_publication_outcome>
		classify_writer_publish_failure(const materialization_store_observation& observation,
										const materialization_store_sdk_failure& failure)
		{
			if (observation.backend != "sqlite" ||
				failure.operation != materialization_store_operation::writer_publish ||
				!valid_recovery_receipt(observation) || observation.publish_returned_record ||
				observation.publish_returned_handle)
				return invariant_breach();
			if (is_publication_invariant_error(failure.error))
				return sdk::unexpected(failure.error);

			materialization_store_publication_outcome output;
			output.first_issue = failure;
			output.recovery = observation.recovery_receipt;
			if (failure.error.code == "store.publication-conflict" &&
				failure.error.field == "exact-series-id" && failure.error.detail.empty())
			{
				output.kind = outcome_kind::rejected_stale;
				return output;
			}
			if (failure.error.code == "store.sqlite-failure" && failure.error.field == "database" &&
				!failure.error.detail.empty())
			{
				output.kind = outcome_kind::publication_outcome_unknown;
				output.unknown_cause = materialization_store_unknown_category::persistence_io;
				return output;
			}
			if (const auto category = classify_rejected_failure(failure.error))
			{
				output.kind = outcome_kind::rejected_store_failure;
				output.rejected_failure = *category;
				return output;
			}
			return sdk::unexpected(failure.error);
		}

		[[nodiscard]] sdk::result<materialization_store_publication_outcome>
		classify_postpublication(const materialization_store_observation& observation)
		{
			if (!observation.publish_returned_record ||
				!is_committed_record(*observation.publish_returned_record))
				return invariant_breach();
			if (!observation.first_issue)
			{
				if (!observation.candidate_identity || !observation.candidate_manifest ||
					!same_publication_identity(*observation.candidate_identity,
											   *observation.publish_returned_record) ||
					!valid_success_receipts(observation,
											*observation.publish_returned_record,
											*observation.candidate_manifest))
					return invariant_breach();
				materialization_store_publication_outcome output;
				output.kind = outcome_kind::committed_verified;
				return output;
			}

			const auto& issue = *observation.first_issue;
			if (const auto* failure = std::get_if<materialization_store_sdk_failure>(&issue))
			{
				if (!is_postpublication_operation(failure->operation) ||
					is_publication_invariant_error(failure->error))
					return sdk::unexpected(failure->error);
			}
			else if (const auto* mismatch = std::get_if<materialization_store_mismatch>(&issue))
			{
				if (mismatch->operation != materialization_store_operation::writer_publish &&
					!is_postpublication_operation(mismatch->operation))
					return invariant_breach();
			}
			else
				return invariant_breach();

			materialization_store_publication_outcome output;
			output.kind = outcome_kind::committed_unverified;
			output.first_issue = issue;
			return output;
		}
	} // namespace

	sdk::result<materialization_store_publication_outcome>
	classify_materialization_store_publication_outcome(
		const materialization_store_observation& observation)
	{
		if (observation.backend != "memory" && observation.backend != "sqlite")
			return invariant_breach();
		if (observation.publication_attempted != (observation.publish_call_count != 0U) ||
			observation.publish_call_count > 1U)
			return invariant_breach();

		if (!observation.publication_attempted)
		{
			const auto* failure = observation.first_issue
				? std::get_if<materialization_store_sdk_failure>(&*observation.first_issue)
				: nullptr;
			if (failure == nullptr || observation.publish_returned_record ||
				observation.publish_returned_handle || observation.candidate_identity ||
				observation.candidate_manifest || observation.recovery_receipt ||
				!is_prepublication_operation(failure->operation))
				return invariant_breach();
			if (observation.backend == "memory")
				return sdk::unexpected(sdk::error{"store.transaction-state", "publish", {}});
			materialization_store_publication_outcome output;
			output.kind = outcome_kind::prepublication_zero_effect;
			output.first_issue = observation.first_issue;
			return output;
		}

		if (observation.publish_call_count != 1U)
			return invariant_breach();
		if (observation.first_issue)
		{
			if (const auto* failure =
					std::get_if<materialization_store_sdk_failure>(&*observation.first_issue);
				failure && failure->operation == materialization_store_operation::writer_publish)
				return classify_writer_publish_failure(observation, *failure);
		}
		if (!observation.publish_returned_record)
			return invariant_breach();
		return classify_postpublication(observation);
	}
} // namespace cxxlens::detail::clang22::materialization
