#include "llvm/clang22/materialization_store_outcome.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "llvm/clang22/materialization_v4_execution_journal.hpp"
#include "llvm/clang22/materialization_v4_prior_artifact.hpp"

namespace
{
	using namespace cxxlens::detail::clang22::materialization;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] cxxlens::sdk::snapshot_series_selector selector()
	{
		return {
			"catalog:outcome-test",
			"stable",
			"engine:outcome-test",
			"universe:outcome-test",
			"semantic-v2:sha256:" + std::string(64U, 'a'),
			"semantic-v2:sha256:" + std::string(64U, 'b'),
			"semantic-v2:sha256:" + std::string(64U, 'c'),
		};
	}

	[[nodiscard]] cxxlens::sdk::snapshot_manifest manifest()
	{
		cxxlens::sdk::snapshot_manifest value;
		value.id = "snapshot:outcome";
		return value;
	}

	[[nodiscard]] cxxlens::sdk::publication_record
	record(const std::string_view publication_id,
		   const std::string_view series_id,
		   const std::string_view snapshot_id,
		   const std::uint64_t sequence,
		   const std::optional<std::string>& parent = std::nullopt)
	{
		cxxlens::sdk::publication_record value;
		value.publication_id = publication_id;
		value.series_id = series_id;
		value.snapshot_id = snapshot_id;
		value.sequence = sequence;
		value.physical_generation = sequence;
		value.parent_publication = parent;
		value.state = cxxlens::sdk::publication_state::committed;
		return value;
	}

	[[nodiscard]] materialization_store_observation verified_observation()
	{
		materialization_store_observation value;
		value.backend = "sqlite";
		value.selector = selector();
		value.series_id = value.selector.id();
		value.publication_attempted = true;
		value.publish_call_count = 1U;
		value.candidate_manifest = manifest();
		value.candidate_identity = materialization_publication_candidate{
			"publication:outcome", value.series_id, value.candidate_manifest->id, 1U, std::nullopt};
		value.publish_returned_record =
			record("publication:outcome", value.series_id, value.candidate_manifest->id, 1U);
		const std::array paths{
			materialization_store_path::current_selector,
			materialization_store_path::open_publication,
			materialization_store_path::open_snapshot,
		};
		for (std::size_t index{}; index < paths.size(); ++index)
		{
			value.verification_receipts[index].path = paths[index];
			value.verification_receipts[index].status =
				materialization_store_receipt_status::present;
			value.verification_receipts[index].projection = materialization_store_projection{
				*value.publish_returned_record, *value.candidate_manifest, value.backend};
		}
		return value;
	}

	[[nodiscard]] materialization_store_observation
	writer_failure(const std::string_view code,
				   const std::string_view field,
				   const std::string_view detail,
				   const std::string_view backend = "sqlite")
	{
		materialization_store_observation value;
		value.backend = backend;
		value.selector = selector();
		value.series_id = value.selector.id();
		value.publication_attempted = true;
		value.publish_call_count = 1U;
		value.candidate_identity = materialization_publication_candidate{
			"publication:candidate", value.series_id, "snapshot:candidate", 1U, std::nullopt};
		value.recovery_receipt.emplace();
		value.recovery_receipt->selector = value.selector;
		value.recovery_receipt->reopen_status = materialization_store_reopen_status::open_failed;
		value.recovery_receipt->candidate.requested_publication_id =
			value.candidate_identity->publication_id;
		value.first_issue = materialization_store_sdk_failure{
			materialization_store_operation::writer_publish,
			std::nullopt,
			{std::string{code}, std::string{field}, std::string{detail}},
		};
		return value;
	}

	void exact_writer_publish_tuples_map_without_prose_parsing()
	{
		const auto stale = classify_materialization_store_publication_outcome(
			writer_failure("store.publication-conflict", "exact-series-id", ""));
		require(stale &&
					stale->kind == materialization_store_publication_outcome_kind::rejected_stale,
				"exact stale-parent tuple was not classified as rejected_stale");

		const auto overflow = classify_materialization_store_publication_outcome(
			writer_failure("store.counter-overflow", "physical_generation", ""));
		require(overflow &&
					overflow->kind ==
						materialization_store_publication_outcome_kind::rejected_store_failure &&
					*overflow->rejected_failure ==
						materialization_store_rejected_failure_category::counter_overflow,
				"counter overflow tuple was not classified exactly");

		const auto collision = classify_materialization_store_publication_outcome(
			writer_failure("store.hash-collision", "exact-candidate-snapshot-id", ""));
		require(collision &&
					*collision->rejected_failure ==
						materialization_store_rejected_failure_category::hash_collision,
				"hash collision tuple was not classified exactly");

		const auto ambiguous = classify_materialization_store_publication_outcome(
			writer_failure("store.snapshot-ambiguous", "exact-snapshot-id", ""));
		require(ambiguous &&
					*ambiguous->rejected_failure ==
						materialization_store_rejected_failure_category::persistence_corrupt,
				"snapshot ambiguity tuple was not classified as persistence corruption");

		const auto corrupt = classify_materialization_store_publication_outcome(
			writer_failure("store.corrupt", "exact-publication-id", "parent-sequence"));
		require(corrupt &&
					*corrupt->rejected_failure ==
						materialization_store_rejected_failure_category::persistence_corrupt,
				"typed persisted-authority corruption tuple was not classified exactly");

		const auto unknown = classify_materialization_store_publication_outcome(
			writer_failure("store.sqlite-failure", "database", "diagnostic text is retained only"));
		require(
			unknown &&
				unknown->kind ==
					materialization_store_publication_outcome_kind::publication_outcome_unknown &&
				*unknown->unknown_cause == materialization_store_unknown_category::persistence_io,
			"SQLite database failure was not kept phase-opaque");
		require(std::get<materialization_store_sdk_failure>(*unknown->first_issue).error.detail ==
					"diagnostic text is retained only",
				"SQLite diagnostic detail was not retained as evidence");
	}

	void prepublication_and_postpublication_states_are_separate()
	{
		materialization_store_observation prepublication;
		prepublication.backend = "sqlite";
		prepublication.first_issue = materialization_store_sdk_failure{
			materialization_store_operation::writer_begin,
			std::nullopt,
			{"store.migration-required",
			 "sqlite-physical-format",
			 "cxxlens.sqlite-semantic-store.v2-to-v3"},
		};
		const auto compact = classify_materialization_store_publication_outcome(prepublication);
		require(compact &&
					compact->kind ==
						materialization_store_publication_outcome_kind::prepublication_zero_effect,
				"prepublication Store failure was promoted to a publication outcome");

		auto unverified = verified_observation();
		unverified.first_issue = materialization_store_sdk_failure{
			materialization_store_operation::verify_open_snapshot,
			materialization_store_path::open_snapshot,
			{"store.snapshot-corrupt", "snapshot", "payload"},
		};
		const auto failed = classify_materialization_store_publication_outcome(unverified);
		require(failed &&
					failed->kind ==
						materialization_store_publication_outcome_kind::committed_unverified &&
					std::get<materialization_store_sdk_failure>(*failed->first_issue).error.code ==
						"store.snapshot-corrupt",
				"postpublication verification failure did not retain committed_unverified");

		const auto verified =
			classify_materialization_store_publication_outcome(verified_observation());
		require(verified &&
					verified->kind ==
						materialization_store_publication_outcome_kind::committed_verified,
				"fully verified Store observation was not classified as committed_verified");

		auto compacted_snapshot = verified_observation();
		compacted_snapshot.verification_receipts[2U].projection->publication =
			record("publication:compacted",
				   "series:compacted",
				   compacted_snapshot.candidate_manifest->id,
				   7U,
				   "publication:prior");
		compacted_snapshot.verification_receipts[2U].projection->publication.physical_generation =
			99U;
		const auto compacted =
			classify_materialization_store_publication_outcome(compacted_snapshot);
		require(compacted &&
					compacted->kind ==
						materialization_store_publication_outcome_kind::committed_verified,
				"open(snapshot) rejected a semantically matching compacted publication");

		auto wrong_snapshot = verified_observation();
		wrong_snapshot.verification_receipts[2U].projection->publication.snapshot_id =
			"snapshot:other";
		const auto rejected_snapshot =
			classify_materialization_store_publication_outcome(wrong_snapshot);
		require(!rejected_snapshot &&
					rejected_snapshot.error() ==
						cxxlens::sdk::error{"store.transaction-state", "publish", {}},
				"open(snapshot) accepted a publication for a different snapshot");
	}

	void unknown_or_forged_tuples_fail_closed()
	{
		const auto unknown = classify_materialization_store_publication_outcome(
			writer_failure("store.sqlite-failure-after-reopen", "database", "opaque"));
		require(
			!unknown &&
				unknown.error() ==
					cxxlens::sdk::error{"store.sqlite-failure-after-reopen", "database", "opaque"},
			"invented Store error code was silently classified");

		const auto empty_detail = classify_materialization_store_publication_outcome(
			writer_failure("store.sqlite-failure", "database", ""));
		require(!empty_detail && empty_detail.error().code == "store.sqlite-failure",
				"empty opaque SQLite diagnostic was accepted");

		auto memory =
			writer_failure("store.sqlite-failure", "database", "memory must not map", "memory");
		const auto memory_result = classify_materialization_store_publication_outcome(memory);
		require(!memory_result &&
					memory_result.error() ==
						cxxlens::sdk::error{"store.transaction-state", "publish", {}},
				"memory writer failure did not take the exit-two invariant path");

		auto bad_receipt = writer_failure("store.publication-conflict", "exact-series-id", "");
		bad_receipt.recovery_receipt.reset();
		const auto rejected = classify_materialization_store_publication_outcome(bad_receipt);
		require(!rejected &&
					rejected.error() ==
						cxxlens::sdk::error{"store.transaction-state", "publish", {}},
				"publish conflict without recovery evidence was accepted");
	}

	[[nodiscard]] std::string sha(const char value)
	{
		return "sha256:" + std::string(64U, value);
	}

	[[nodiscard]] std::string semantic(const char value)
	{
		return "semantic-v2:sha256:" + std::string(64U, value);
	}

	[[nodiscard]] cxxlens::sdk::canonical_value text(const std::string_view value)
	{
		return cxxlens::sdk::canonical_value::from_string(std::string{value});
	}

	[[nodiscard]] cxxlens::sdk::canonical_value
	receipt_projection(const materialization_v4_claim_receipt& value)
	{
		return cxxlens::sdk::canonical_value::from_tuple({
			text(value.schema),
			text(value.binding_digest),
			text(value.materialization_request_id),
			cxxlens::sdk::canonical_value::from_integer(
				static_cast<std::int64_t>(value.task_index)),
			text(value.task_id),
			text(value.task_v4_digest),
			text(value.provider_execution_id),
			text(value.source_closure_id),
			text(value.source_closure_digest),
			text(value.manifest_digest),
			text(value.task_input_digest),
			text(value.claim_batch_content_digest),
			text(value.partition_id),
			text(value.partition_content_digest),
			text(value.coverage_digest),
			cxxlens::sdk::canonical_value::from_integer(
				static_cast<std::int64_t>(value.claim_count)),
			cxxlens::sdk::canonical_value::from_integer(
				static_cast<std::int64_t>(value.unresolved_count)),
			cxxlens::sdk::canonical_value::from_integer(
				static_cast<std::int64_t>(value.conflict_count)),
			cxxlens::sdk::canonical_value::from_integer(
				static_cast<std::int64_t>(value.differential_disagreement_count)),
			cxxlens::sdk::canonical_value::from_boolean(value.complete),
			text(value.receipt_digest),
		});
	}

	[[nodiscard]] std::string task_receipt_digest(const materialization_v4_claim_receipt& value)
	{
		const auto result = cxxlens::sdk::canonical_identity_digest(
			"cxxlens.clang22.materialization-claim-receipt.v4",
			std::array<cxxlens::sdk::canonical_value, 20U>{
				text(value.schema),
				text(value.binding_digest),
				text(value.materialization_request_id),
				cxxlens::sdk::canonical_value::from_integer(
					static_cast<std::int64_t>(value.task_index)),
				text(value.task_id),
				text(value.task_v4_digest),
				text(value.provider_execution_id),
				text(value.source_closure_id),
				text(value.source_closure_digest),
				text(value.manifest_digest),
				text(value.task_input_digest),
				text(value.claim_batch_content_digest),
				text(value.partition_id),
				text(value.partition_content_digest),
				text(value.coverage_digest),
				cxxlens::sdk::canonical_value::from_integer(
					static_cast<std::int64_t>(value.claim_count)),
				cxxlens::sdk::canonical_value::from_integer(
					static_cast<std::int64_t>(value.unresolved_count)),
				cxxlens::sdk::canonical_value::from_integer(
					static_cast<std::int64_t>(value.conflict_count)),
				cxxlens::sdk::canonical_value::from_integer(
					static_cast<std::int64_t>(value.differential_disagreement_count)),
				cxxlens::sdk::canonical_value::from_boolean(value.complete),
			});
		require(result.has_value(), "test receipt digest derivation failed");
		return *result;
	}

	[[nodiscard]] materialization_v4_claim_receipt task_receipt(const std::uint64_t index)
	{
		materialization_v4_claim_receipt value;
		value.materialization_request_id = "materialization-request:v4-reuse";
		value.task_index = index;
		value.binding_digest = sha(static_cast<char>('a' + index));
		value.task_id = "task:v4-" + std::to_string(index);
		value.task_v4_digest = semantic(static_cast<char>('b' + index));
		value.provider_execution_id = "provider-execution:v4-" + std::to_string(index);
		value.source_closure_id = "source-closure:v4-" + std::to_string(index);
		value.source_closure_digest = sha(static_cast<char>('c' + index));
		value.manifest_digest = sha(static_cast<char>('d' + index));
		value.task_input_digest = sha(static_cast<char>('e' + index));
		value.claim_batch_content_digest = sha(static_cast<char>('f' + index));
		value.partition_id = "partition:v4-" + std::to_string(index);
		value.partition_content_digest = sha(static_cast<char>('1' + index));
		value.coverage_digest = sha(static_cast<char>('2' + index));
		value.claim_count = 1U;
		value.complete = true;
		value.receipt_digest = task_receipt_digest(value);
		return value;
	}

	[[nodiscard]] materialization_v4_incremental_receipt
	incremental_receipt(std::vector<materialization_v4_claim_receipt> tasks)
	{
		materialization_v4_incremental_receipt value;
		value.materialization_request_id = tasks.front().materialization_request_id;
		value.task_count = tasks.size();
		value.task_receipts = std::move(tasks);
		for (const auto& task : value.task_receipts)
		{
			value.claim_count += task.claim_count;
			value.unresolved_count += task.unresolved_count;
			value.conflict_count += task.conflict_count;
			value.differential_disagreement_count += task.differential_disagreement_count;
			value.complete = value.complete || task.complete;
		}
		value.complete = true;
		std::vector<cxxlens::sdk::canonical_value> fields{
			text(value.schema),
			text(value.materialization_request_id),
			cxxlens::sdk::canonical_value::from_integer(
				static_cast<std::int64_t>(value.task_count)),
		};
		std::vector<cxxlens::sdk::canonical_value> task_values;
		for (const auto& task : value.task_receipts)
			task_values.push_back(receipt_projection(task));
		fields.push_back(cxxlens::sdk::canonical_value::from_tuple(std::move(task_values)));
		fields.push_back(cxxlens::sdk::canonical_value::from_integer(
			static_cast<std::int64_t>(value.claim_count)));
		fields.push_back(cxxlens::sdk::canonical_value::from_integer(
			static_cast<std::int64_t>(value.unresolved_count)));
		fields.push_back(cxxlens::sdk::canonical_value::from_integer(
			static_cast<std::int64_t>(value.conflict_count)));
		fields.push_back(cxxlens::sdk::canonical_value::from_integer(
			static_cast<std::int64_t>(value.differential_disagreement_count)));
		fields.push_back(cxxlens::sdk::canonical_value::from_boolean(value.complete));
		const auto digest = cxxlens::sdk::canonical_identity_digest(
			materialization_v4_incremental_receipt_schema, fields);
		require(digest.has_value(), "test incremental digest derivation failed");
		value.receipt_digest = *digest;
		return value;
	}

	void v4_execution_and_prior_reuse_are_exact()
	{
		auto receipt = incremental_receipt({task_receipt(0U), task_receipt(1U)});
		auto journal = materialization_v4_execution_journal::begin(
			receipt.materialization_request_id, receipt.task_count);
		require(journal.has_value(), "v4 execution journal did not begin");
		require(journal->record(receipt.task_receipts[0U], true, 0U).has_value(),
				"valid zero-call reuse was rejected");
		require(journal->record(receipt.task_receipts[1U], true, 0U).has_value(),
				"second zero-call reuse was rejected");
		auto execution = std::move(*journal).finish(receipt);
		require(execution.has_value() && execution->provider_call_count == 0U &&
					execution->reused_task_count == receipt.task_count,
				"valid reuse did not produce a zero-provider execution receipt");

		auto invalid_calls = materialization_v4_execution_journal::begin(
			receipt.materialization_request_id, receipt.task_count);
		require(invalid_calls.has_value(), "invalid-call journal did not begin");
		require(!invalid_calls->record(receipt.task_receipts[0U], true, 1U),
				"reuse with provider call was accepted");
		require(!materialization_v4_execution_journal::validate_exact_reuse(
					receipt,
					incremental_receipt({receipt.task_receipts[1U], receipt.task_receipts[0U]})),
				"reordered v4 receipt was accepted");

		materialization_v4_prior_artifact artifact;
		artifact.materialization_request_id = receipt.materialization_request_id;
		artifact.authority = {"recipe:v4", "output-plan:v4", "publication-target:v4"};
		artifact.publication = {"series:v4",
								"publication:v4",
								"snapshot:v4",
								std::string{"publication:parent"},
								4U,
								4U,
								true,
								false};
		artifact.receipt = receipt;
		auto encoded = encode_materialization_v4_prior_artifact(artifact);
		require(encoded.has_value(), "v4 prior artifact did not encode");
		auto decoded = decode_materialization_v4_prior_artifact(*encoded);
		require(decoded.has_value(), "v4 prior artifact did not decode");
		auto reuse = match_materialization_v4_prior_artifact(*decoded,
															 receipt.materialization_request_id,
															 artifact.authority,
															 artifact.publication,
															 receipt.task_receipts);
		require(reuse.has_value() && reuse->provider_call_count == 0U,
				"v4 prior artifact exact match did not issue zero-call reuse");
		(*encoded)[encoded->size() - 1U] ^= std::byte{1};
		require(!decode_materialization_v4_prior_artifact(*encoded),
				"tampered v4 prior artifact was accepted");
	}
} // namespace

int main()
{
	exact_writer_publish_tuples_map_without_prose_parsing();
	prepublication_and_postpublication_states_are_separate();
	unknown_or_forged_tuples_fail_closed();
	v4_execution_and_prior_reuse_are_exact();
	return 0;
}
