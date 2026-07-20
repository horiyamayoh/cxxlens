#include "materialization_store.hpp"

#include <algorithm>
#include <limits>
#include <ranges>
#include <utility>

#include "sdk/store_identity_internal.hpp"

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		class sdk_store_opener final : public materialization_store_opener
		{
		  public:
			sdk::result<sdk::snapshot_store> open_memory(sdk::relation_engine engine) override
			{
				return sdk::make_in_memory_snapshot_store(std::move(engine));
			}

			sdk::result<sdk::snapshot_store> open_sqlite(const std::string& exact_path,
														 sdk::relation_engine engine) override
			{
				return sdk::open_sqlite_snapshot_store(exact_path, std::move(engine));
			}
		};

		[[nodiscard]] sdk::snapshot_partition_binding
		partition_binding(const sdk::partition_manifest& manifest,
						  const sdk::partition_draft& draft)
		{
			return {
				manifest.partition_id,
				draft.relation_descriptor_id,
				draft.scope,
				draft.condition,
				draft.interpretation,
				draft.producer_semantics,
				draft.producer_input_basis_digest,
				draft.precision_profile,
				draft.assumption_set_id,
			};
		}

		[[nodiscard]] sdk::result<sdk::snapshot_manifest>
		build_candidate_manifest(const sdk::relation_engine& engine,
								 const prepared_store_transaction& prepared)
		{
			sdk::snapshot_manifest output;
			output.snapshot_semantics_version = prepared.draft.snapshot_semantics_version;
			output.catalog_semantic_digest = prepared.draft.catalog_semantic_digest;
			output.condition_universe_id = prepared.draft.series.condition_universe_id;
			output.relation_registry_digest = prepared.draft.series.relation_registry_digest;
			output.interpretation_policy_digest =
				prepared.draft.series.interpretation_policy_digest;

			std::vector<std::pair<sdk::partition_manifest, const sdk::partition_draft*>> bindings;
			bindings.reserve(prepared.partitions.size());
			for (const auto& partition : prepared.partitions)
			{
				auto manifest = sdk::make_partition_manifest(engine, partition);
				if (!manifest)
					return sdk::unexpected(std::move(manifest.error()));
				bindings.emplace_back(*manifest, &partition);
				output.partitions.push_back(std::move(*manifest));
			}
			std::ranges::sort(output.partitions, {}, &sdk::partition_manifest::partition_id);

			for (const auto& closure : prepared.closures)
			{
				const auto binding = std::ranges::find_if(bindings,
														  [&](const auto& value)
														  {
															  return value.first.partition_id ==
																  closure.subject_partition_id;
														  });
				if (binding == bindings.end())
					return sdk::unexpected(sdk::error{
						"store.closure-subject-missing", closure.subject_partition_id, {}});
				auto subject = sdk::make_partition_certificate_subject(
					binding->first, partition_binding(binding->first, *binding->second));
				if (!subject)
					return sdk::unexpected(std::move(subject.error()));
				auto certificate = sdk::make_closure_certificate(*subject, closure);
				if (!certificate)
					return sdk::unexpected(std::move(certificate.error()));
				output.closure_ids.push_back(std::move(certificate->id));
			}
			std::ranges::sort(output.closure_ids);
			if (std::ranges::adjacent_find(output.closure_ids) != output.closure_ids.end())
				return sdk::unexpected(sdk::error{"store.closure-duplicate", "closures", {}});
			auto identity = sdk::detail::snapshot_manifest_identity(output);
			if (!identity)
				return sdk::unexpected(std::move(identity.error()));
			output.id = std::move(*identity);
			return output;
		}

		[[nodiscard]] sdk::result<materialization_publication_candidate>
		make_publication_candidate(const std::string& series_id,
								   const std::string& snapshot_id,
								   const std::uint64_t sequence,
								   const std::optional<std::string>& parent)
		{
			auto identity =
				sdk::detail::publication_record_identity(series_id, snapshot_id, sequence, parent);
			if (!identity)
				return sdk::unexpected(std::move(identity.error()));
			return materialization_publication_candidate{
				std::move(*identity), series_id, snapshot_id, sequence, parent};
		}

		[[nodiscard]] materialization_store_path_receipt
		receipt(const materialization_store_path path)
		{
			materialization_store_path_receipt value;
			value.path = path;
			return value;
		}

		[[nodiscard]] materialization_store_observation
		initial_observation(const validated_publication_request& publication)
		{
			materialization_store_observation output;
			output.backend = publication.backend;
			output.selector = publication.selector;
			output.series_id = publication.series_id;
			output.expected_parent_publication = publication.expected_parent_publication;
			output.head_observation = receipt(materialization_store_path::current_selector);
			output.head_observation.selector_lookup = publication.selector;
			output.verification_receipts = {
				receipt(materialization_store_path::current_selector),
				receipt(materialization_store_path::open_publication),
				receipt(materialization_store_path::open_snapshot),
			};
			output.verification_receipts[0U].selector_lookup = publication.selector;
			return output;
		}

		void retain_sdk_failure(materialization_store_observation& output,
								const materialization_store_operation operation,
								std::optional<materialization_store_path> path,
								const sdk::error& error)
		{
			if (!output.first_issue)
				output.first_issue.emplace(
					materialization_store_sdk_failure{operation, path, error});
		}

		void retain_mismatch(materialization_store_observation& output,
							 const materialization_store_operation operation,
							 std::optional<materialization_store_path> path,
							 std::string projection,
							 materialization_store_mismatch_value expected,
							 materialization_store_mismatch_value actual)
		{
			if (!output.first_issue)
				output.first_issue.emplace(materialization_store_mismatch{
					operation,
					path,
					std::move(projection),
					std::move(expected),
					std::move(actual),
				});
		}

		[[nodiscard]] materialization_store_projection
		projection_of(const sdk::snapshot_handle& handle)
		{
			return {
				handle.publication(), handle.manifest(), std::string{handle.physical_backend()}};
		}

		void capture_head(materialization_store_observation& output,
						  sdk::result<sdk::snapshot_handle> current)
		{
			output.head_observation.status = current
				? materialization_store_receipt_status::present
				: materialization_store_receipt_status::sdk_error;
			if (!current)
			{
				output.head_observation.error = current.error();
				return;
			}
			output.head_observation.projection = projection_of(*current);
			output.head_observation.handle.emplace(std::move(*current));
		}

		void capture_verification_receipt(materialization_store_observation& output,
										  materialization_store_path_receipt& receipt_value,
										  const materialization_store_operation operation,
										  sdk::result<sdk::snapshot_handle> opened)
		{
			receipt_value.status = opened ? materialization_store_receipt_status::present
										  : materialization_store_receipt_status::sdk_error;
			if (!opened)
			{
				receipt_value.error = opened.error();
				retain_sdk_failure(output, operation, receipt_value.path, opened.error());
				return;
			}
			receipt_value.projection = projection_of(*opened);
			receipt_value.handle.emplace(std::move(*opened));
		}

		[[nodiscard]] bool same_semantic_publication(const sdk::publication_record& expected,
													 const sdk::publication_record& actual)
		{
			return expected.publication_id == actual.publication_id &&
				expected.series_id == actual.series_id &&
				expected.snapshot_id == actual.snapshot_id &&
				expected.sequence == actual.sequence &&
				expected.parent_publication == actual.parent_publication &&
				expected.state == actual.state && expected.corrupt == actual.corrupt;
		}

		void verify_series_path(materialization_store_observation& output,
								const materialization_store_path_receipt& receipt_value,
								const materialization_store_operation operation,
								const materialization_store_projection& candidate)
		{
			if (!receipt_value.projection)
				return;
			const auto& actual = *receipt_value.projection;
			if (actual.physical_backend != output.backend)
				retain_mismatch(output,
								operation,
								receipt_value.path,
								"physical-backend",
								output.backend,
								actual.physical_backend);
			if (!same_semantic_publication(candidate.publication, actual.publication))
				retain_mismatch(output,
								operation,
								receipt_value.path,
								"semantic-publication-record",
								candidate.publication,
								actual.publication);
			if (actual.publication.physical_generation < candidate.publication.physical_generation)
				retain_mismatch(output,
								operation,
								receipt_value.path,
								"physical-generation-monotonicity",
								candidate.publication.physical_generation,
								actual.publication.physical_generation);
			if (actual.manifest != candidate.manifest)
				retain_mismatch(output,
								operation,
								receipt_value.path,
								"semantic-snapshot-manifest",
								candidate.manifest,
								actual.manifest);
		}

		void verify_snapshot_path(materialization_store_observation& output,
								  const materialization_store_path_receipt& receipt_value,
								  const materialization_store_projection& candidate)
		{
			if (!receipt_value.projection)
				return;
			const auto& actual = *receipt_value.projection;
			if (actual.physical_backend != output.backend)
				retain_mismatch(output,
								materialization_store_operation::verify_open_snapshot,
								receipt_value.path,
								"physical-backend",
								output.backend,
								actual.physical_backend);
			if (actual.publication.snapshot_id != candidate.publication.snapshot_id)
				retain_mismatch(output,
								materialization_store_operation::verify_open_snapshot,
								receipt_value.path,
								"requested-snapshot-id",
								candidate.publication.snapshot_id,
								actual.publication.snapshot_id);
			const bool actual_committed =
				actual.publication.state == sdk::publication_state::committed &&
				!actual.publication.corrupt;
			if (!actual_committed)
				retain_mismatch(output,
								materialization_store_operation::verify_open_snapshot,
								receipt_value.path,
								"committed-noncorrupt",
								true,
								actual_committed);
			if (actual.manifest != candidate.manifest)
				retain_mismatch(output,
								materialization_store_operation::verify_open_snapshot,
								receipt_value.path,
								"semantic-snapshot-manifest",
								candidate.manifest,
								actual.manifest);
		}

		[[nodiscard]] bool validate_configuration(materialization_store_observation& output,
												  const validated_publication_request& publication,
												  const prepared_store_transaction& prepared)
		{
			if (publication.backend != "memory" && publication.backend != "sqlite")
				retain_mismatch(output,
								materialization_store_operation::configuration,
								std::nullopt,
								"backend",
								std::string{"memory-or-sqlite"},
								publication.backend);
			else if (publication.series_id != publication.selector.id())
				retain_mismatch(output,
								materialization_store_operation::configuration,
								std::nullopt,
								"selector-series-id",
								publication.selector.id(),
								publication.series_id);
			else if (prepared.draft.series != publication.selector)
				retain_mismatch(output,
								materialization_store_operation::configuration,
								std::nullopt,
								"draft-selector",
								publication.selector,
								prepared.draft.series);
			else if (prepared.draft.expected_parent_publication !=
					 publication.expected_parent_publication)
				retain_mismatch(output,
								materialization_store_operation::configuration,
								std::nullopt,
								"draft-expected-parent-publication",
								publication.expected_parent_publication,
								prepared.draft.expected_parent_publication);
			else if (publication.genesis != !publication.expected_parent_publication.has_value())
				retain_mismatch(output,
								materialization_store_operation::configuration,
								std::nullopt,
								"genesis-parent-coherence",
								!publication.expected_parent_publication.has_value(),
								publication.genesis);
			else if (publication.backend == "memory" &&
					 (!publication.genesis || publication.sqlite_path.has_value()))
				retain_mismatch(output,
								materialization_store_operation::configuration,
								std::nullopt,
								"memory-fresh-genesis-policy",
								true,
								false);
			else if (publication.backend == "sqlite" && !publication.sqlite_path)
				retain_mismatch(output,
								materialization_store_operation::configuration,
								std::nullopt,
								"sqlite-path-present",
								true,
								false);
			return !output.first_issue.has_value();
		}

		void capture_recovery_lookup(materialization_store_publication_lookup& output,
									 sdk::result<sdk::snapshot_handle> opened,
									 const std::string_view not_found_code)
		{
			if (opened)
			{
				output.status = materialization_store_lookup_status::present;
				output.record = opened->publication();
				return;
			}
			output.error = opened.error();
			output.status = opened.error().code == not_found_code
				? materialization_store_lookup_status::not_found
				: materialization_store_lookup_status::sdk_error;
		}

		void recover_sqlite_publication(materialization_store_observation& output,
										const sdk::relation_engine& engine,
										const validated_publication_request& publication,
										materialization_store_opener& opener)
		{
			materialization_store_recovery_receipt recovery;
			recovery.selector = publication.selector;
			recovery.expected_parent.requested_publication_id =
				publication.expected_parent_publication;
			if (!publication.expected_parent_publication)
				recovery.expected_parent.status =
					materialization_store_lookup_status::not_applicable;
			if (output.candidate_identity)
				recovery.candidate.requested_publication_id =
					output.candidate_identity->publication_id;
			else
				recovery.candidate.status = materialization_store_lookup_status::not_applicable;

			auto reopened = opener.open_sqlite(*publication.sqlite_path, engine);
			if (!reopened)
			{
				recovery.reopen_status = materialization_store_reopen_status::open_failed;
				recovery.open_error = reopened.error();
				output.recovery_receipt.emplace(std::move(recovery));
				return;
			}

			recovery.reopen_status = materialization_store_reopen_status::opened;
			capture_recovery_lookup(recovery.current,
									reopened->current(publication.selector),
									"store.current-not-found");
			if (publication.expected_parent_publication)
				capture_recovery_lookup(
					recovery.expected_parent,
					reopened->open_publication(*publication.expected_parent_publication),
					"store.publication-not-found");
			if (output.candidate_identity)
				capture_recovery_lookup(
					recovery.candidate,
					reopened->open_publication(output.candidate_identity->publication_id),
					"store.publication-not-found");
			output.recovery_receipt.emplace(std::move(recovery));
			output.verification_store.emplace(std::move(*reopened));
		}
	} // namespace

	struct materialization_store_preparation::state
	{
		state(sdk::relation_engine engine_value,
			  validated_publication_request publication_value,
			  materialization_store_observation observation_value,
			  materialization_store_opener& opener_value)
			: observation{std::move(observation_value)}, engine{std::move(engine_value)},
			  publication{std::move(publication_value)}, opener{&opener_value}
		{
		}

		materialization_store_observation observation;
		sdk::relation_engine engine;
		validated_publication_request publication;
		std::optional<sdk::snapshot_store> store;
		std::optional<sdk::snapshot_writer> writer;
		std::unique_ptr<materialization_store_opener> owned_opener;
		materialization_store_opener* opener{};
	};

	materialization_store_preparation::materialization_store_preparation(
		std::unique_ptr<state> state_value)
		: state_{std::move(state_value)}
	{
	}

	materialization_store_preparation::materialization_store_preparation(
		materialization_store_preparation&&) noexcept = default;
	materialization_store_preparation& materialization_store_preparation::operator=(
		materialization_store_preparation&&) noexcept = default;
	materialization_store_preparation::~materialization_store_preparation() = default;

	bool materialization_store_preparation::ready_for_publish() const noexcept
	{
		return state_ && state_->writer && !state_->observation.first_issue &&
			!state_->observation.publication_attempted;
	}

	const materialization_store_observation&
	materialization_store_preparation::observation() const noexcept
	{
		return state_->observation;
	}

	materialization_store_preparation
	prepare_materialization_store(const sdk::relation_engine& engine,
								  const validated_publication_request& publication,
								  prepared_store_transaction prepared,
								  materialization_store_opener& opener)
	{
		auto state_value = std::make_unique<materialization_store_preparation::state>(
			engine, publication, initial_observation(publication), opener);
		auto& output = state_value->observation;
		if (!validate_configuration(output, publication, prepared))
			return materialization_store_preparation{std::move(state_value)};

		auto candidate_manifest = build_candidate_manifest(engine, prepared);

		auto opened = publication.backend == "memory"
			? opener.open_memory(engine)
			: opener.open_sqlite(*publication.sqlite_path, engine);
		if (!opened)
		{
			retain_sdk_failure(
				output, materialization_store_operation::store_open, std::nullopt, opened.error());
			return materialization_store_preparation{std::move(state_value)};
		}
		state_value->store.emplace(std::move(*opened));
		const auto compatibility = state_value->store->compatibility();
		if (compatibility.backend != publication.backend)
		{
			retain_mismatch(output,
							materialization_store_operation::store_open,
							std::nullopt,
							"opened-backend",
							publication.backend,
							compatibility.backend);
			return materialization_store_preparation{std::move(state_value)};
		}

		capture_head(output, state_value->store->current(publication.selector));
		std::optional<std::uint64_t> prior_sequence;
		if (output.head_observation.status == materialization_store_receipt_status::sdk_error)
		{
			const auto& error = *output.head_observation.error;
			if (!publication.genesis || error.code != "store.current-not-found")
				retain_sdk_failure(output,
								   materialization_store_operation::head_current,
								   materialization_store_path::current_selector,
								   error);
		}
		else
		{
			const auto& head = *output.head_observation.projection;
			if (head.physical_backend != publication.backend)
				retain_mismatch(output,
								materialization_store_operation::head_current,
								materialization_store_path::current_selector,
								"physical-backend",
								publication.backend,
								head.physical_backend);
			if (head.publication.series_id != publication.series_id)
				retain_mismatch(output,
								materialization_store_operation::head_current,
								materialization_store_path::current_selector,
								"series-id",
								publication.series_id,
								head.publication.series_id);
			const bool committed_noncorrupt =
				head.publication.state == sdk::publication_state::committed &&
				!head.publication.corrupt;
			if (!committed_noncorrupt)
				retain_mismatch(output,
								materialization_store_operation::head_current,
								materialization_store_path::current_selector,
								"committed-noncorrupt",
								true,
								committed_noncorrupt);
			const auto actual_parent = std::optional<std::string>{head.publication.publication_id};
			if (publication.expected_parent_publication != actual_parent)
				retain_mismatch(output,
								materialization_store_operation::head_current,
								materialization_store_path::current_selector,
								"expected-parent-publication",
								publication.expected_parent_publication,
								actual_parent);
			prior_sequence = head.publication.sequence;
		}
		if (output.first_issue)
			return materialization_store_preparation{std::move(state_value)};

		++output.writer_begin_call_count;
		auto writer = state_value->store->begin(std::move(prepared.draft));
		if (!writer)
		{
			retain_sdk_failure(output,
							   materialization_store_operation::writer_begin,
							   std::nullopt,
							   writer.error());
			return materialization_store_preparation{std::move(state_value)};
		}
		state_value->writer.emplace(std::move(*writer));
		for (auto& partition : prepared.partitions)
		{
			auto staged = state_value->writer->stage(std::move(partition));
			if (!staged)
			{
				retain_sdk_failure(output,
								   materialization_store_operation::partition_stage,
								   std::nullopt,
								   staged.error());
				state_value->writer.reset();
				return materialization_store_preparation{std::move(state_value)};
			}
		}
		for (auto& closure : prepared.closures)
		{
			auto staged = state_value->writer->add_closure(std::move(closure));
			if (!staged)
			{
				retain_sdk_failure(output,
								   materialization_store_operation::closure_stage,
								   std::nullopt,
								   staged.error());
				state_value->writer.reset();
				return materialization_store_preparation{std::move(state_value)};
			}
		}
		auto validated = state_value->writer->validate();
		if (!validated)
		{
			retain_sdk_failure(output,
							   materialization_store_operation::writer_validate,
							   std::nullopt,
							   validated.error());
			state_value->writer.reset();
			return materialization_store_preparation{std::move(state_value)};
		}
		if (!candidate_manifest)
		{
			retain_sdk_failure(output,
							   materialization_store_operation::configuration,
							   std::nullopt,
							   candidate_manifest.error());
			state_value->writer.reset();
			return materialization_store_preparation{std::move(state_value)};
		}
		output.candidate_manifest = std::move(*candidate_manifest);
		if (!prior_sequence || *prior_sequence != std::numeric_limits<std::uint64_t>::max())
		{
			const auto sequence = prior_sequence ? *prior_sequence + 1U : 1U;
			auto candidate = make_publication_candidate(publication.series_id,
														output.candidate_manifest->id,
														sequence,
														publication.expected_parent_publication);
			if (!candidate)
			{
				retain_sdk_failure(output,
								   materialization_store_operation::configuration,
								   std::nullopt,
								   candidate.error());
				state_value->writer.reset();
				return materialization_store_preparation{std::move(state_value)};
			}
			output.candidate_identity = std::move(*candidate);
		}
		return materialization_store_preparation{std::move(state_value)};
	}

	materialization_store_preparation
	prepare_materialization_store(const sdk::relation_engine& engine,
								  const validated_publication_request& publication,
								  prepared_store_transaction prepared)
	{
		auto owned = std::make_unique<sdk_store_opener>();
		auto output =
			prepare_materialization_store(engine, publication, std::move(prepared), *owned);
		output.state_->owned_opener = std::move(owned);
		output.state_->opener = output.state_->owned_opener.get();
		return output;
	}

	materialization_store_observation
	publish_materialization_store(materialization_store_preparation&& prepared)
	{
		auto state_value = std::move(prepared.state_);
		auto& output = state_value->observation;
		if (!state_value->writer || output.first_issue || output.publication_attempted)
			return std::move(output);

		output.publication_attempted = true;
		++output.publish_call_count;
		auto published = state_value->writer->publish();
		state_value->writer.reset();
		if (!published)
		{
			retain_sdk_failure(output,
							   materialization_store_operation::writer_publish,
							   std::nullopt,
							   published.error());
			if (state_value->publication.backend == "sqlite")
			{
				state_value->store.reset();
				recover_sqlite_publication(
					output, state_value->engine, state_value->publication, *state_value->opener);
			}
			return std::move(output);
		}
		output.publish_returned_record = published->publication();
		output.publish_returned_handle.emplace(std::move(*published));

		const auto candidate = projection_of(*output.publish_returned_handle);
		const auto& record = candidate.publication;
		if (!output.candidate_identity)
			retain_mismatch(output,
							materialization_store_operation::writer_publish,
							std::nullopt,
							"candidate-identity-present",
							true,
							false);
		else
		{
			const auto& expected = *output.candidate_identity;
			if (record.publication_id != expected.publication_id)
				retain_mismatch(output,
								materialization_store_operation::writer_publish,
								std::nullopt,
								"returned-publication-id",
								expected.publication_id,
								record.publication_id);
			if (record.series_id != expected.series_id)
				retain_mismatch(output,
								materialization_store_operation::writer_publish,
								std::nullopt,
								"returned-series-id",
								expected.series_id,
								record.series_id);
			if (record.snapshot_id != expected.snapshot_id)
				retain_mismatch(output,
								materialization_store_operation::writer_publish,
								std::nullopt,
								"returned-snapshot-id",
								expected.snapshot_id,
								record.snapshot_id);
			if (record.sequence != expected.sequence)
				retain_mismatch(output,
								materialization_store_operation::writer_publish,
								std::nullopt,
								"returned-sequence",
								expected.sequence,
								record.sequence);
			if (record.parent_publication != expected.parent_publication)
				retain_mismatch(output,
								materialization_store_operation::writer_publish,
								std::nullopt,
								"returned-parent-publication",
								expected.parent_publication,
								record.parent_publication);
		}
		const bool committed_noncorrupt =
			record.state == sdk::publication_state::committed && !record.corrupt;
		if (!committed_noncorrupt)
			retain_mismatch(output,
							materialization_store_operation::writer_publish,
							std::nullopt,
							"returned-committed-noncorrupt",
							true,
							committed_noncorrupt);
		if (output.candidate_manifest && candidate.manifest != *output.candidate_manifest)
			retain_mismatch(output,
							materialization_store_operation::verify_projection,
							std::nullopt,
							"candidate-snapshot-manifest",
							*output.candidate_manifest,
							candidate.manifest);

		if (state_value->publication.backend == "sqlite")
		{
			state_value->store.reset();
			auto reopened = state_value->opener->open_sqlite(*state_value->publication.sqlite_path,
															 state_value->engine);
			if (!reopened)
			{
				retain_sdk_failure(output,
								   materialization_store_operation::store_reopen,
								   std::nullopt,
								   reopened.error());
				return std::move(output);
			}
			state_value->store.emplace(std::move(*reopened));
		}

		output.verification_receipts[1U].id_lookup = record.publication_id;
		output.verification_receipts[2U].id_lookup = record.snapshot_id;
		capture_verification_receipt(
			output,
			output.verification_receipts[0U],
			materialization_store_operation::verify_current,
			state_value->store->current(state_value->publication.selector));
		verify_series_path(output,
						   output.verification_receipts[0U],
						   materialization_store_operation::verify_current,
						   candidate);
		capture_verification_receipt(output,
									 output.verification_receipts[1U],
									 materialization_store_operation::verify_open_publication,
									 state_value->store->open_publication(record.publication_id));
		verify_series_path(output,
						   output.verification_receipts[1U],
						   materialization_store_operation::verify_open_publication,
						   candidate);
		capture_verification_receipt(output,
									 output.verification_receipts[2U],
									 materialization_store_operation::verify_open_snapshot,
									 state_value->store->open(record.snapshot_id));
		verify_snapshot_path(output, output.verification_receipts[2U], candidate);
		output.verification_store.emplace(std::move(*state_value->store));
		return std::move(output);
	}

	materialization_store_observation
	execute_materialization_store(const sdk::relation_engine& engine,
								  const validated_publication_request& publication,
								  prepared_store_transaction prepared)
	{
		return publish_materialization_store(
			prepare_materialization_store(engine, publication, std::move(prepared)));
	}

	materialization_store_observation
	execute_materialization_store(const sdk::relation_engine& engine,
								  const validated_publication_request& publication,
								  prepared_store_transaction prepared,
								  materialization_store_opener& opener)
	{
		return publish_materialization_store(
			prepare_materialization_store(engine, publication, std::move(prepared), opener));
	}
} // namespace cxxlens::detail::clang22::materialization
