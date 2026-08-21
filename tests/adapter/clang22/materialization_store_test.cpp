#include "llvm/clang22/materialization_store.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/sdk.hpp>

#include "../../support/sqlite_store_fixture.hpp"

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::detail::clang22::materialization;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] sdk::relation_descriptor descriptor()
	{
		sdk::relation_descriptor value;
		value.id = "company.test.item.v1";
		value.name = "company.test.item";
		value.version = {1U, 0U, 0U};
		value.semantic_major = 1U;
		value.semantics = "company.test.item/1";
		value.owner_namespace = "company.test";
		value.columns = {
			{"company.test.item.v1.key",
			 "key",
			 {sdk::scalar_kind::typed_id, "company_item_id", false},
			 true,
			 sdk::column_role::claim_key},
			{"company.test.item.v1.value",
			 "value",
			 {sdk::scalar_kind::utf8_string, {}, false},
			 true,
			 sdk::column_role::authoritative_payload},
		};
		value.key_columns = {"company.test.item.v1.key"};
		value.merge = sdk::merge_mode::set;
		value.descriptor_digest =
			*sdk::semantic_digest("cxxlens.relation-descriptor-binding.v2",
								  value.contract_digest + "\n" + value.canonical_form());
		return value;
	}

	[[nodiscard]] sdk::relation_engine engine()
	{
		sdk::relation_registry registry;
		require(registry.add(descriptor()).has_value(), "test descriptor registration failed");
		auto built = registry.build("engine-materialization-store-test");
		require(built.has_value(), "test relation engine build failed");
		return std::move(*built);
	}

	[[nodiscard]] sdk::snapshot_series_selector selector(const sdk::relation_engine& value)
	{
		return {
			"catalog-materialization-store-test",
			"stable",
			std::string{value.generation()},
			"universe-materialization-store-test",
			std::string{value.registry_digest()},
			"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
			"sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
		};
	}

	[[nodiscard]] sdk::detached_row row(std::string key, std::string payload)
	{
		const auto relation = descriptor();
		sdk::row_builder builder{relation};
		require(builder
					.set({relation.id, relation.columns[0U].id, relation.columns[0U].type},
						 sdk::detached_cell::typed("company_item_id", std::move(key)))
					.has_value(),
				"test row key rejected");
		require(builder
					.set({relation.id, relation.columns[1U].id, relation.columns[1U].type},
						 sdk::detached_cell::utf8(std::move(payload)))
					.has_value(),
				"test row payload rejected");
		auto finished = std::move(builder).finish();
		require(finished.has_value(), "test row did not finish");
		return std::move(*finished);
	}

	[[nodiscard]] sdk::claim
	make_claim(const sdk::relation_engine& value, std::string key, std::string payload)
	{
		constexpr std::string_view producer_digest =
			"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
		sdk::observation observation{
			row(std::move(key), std::move(payload)),
			{"universe-materialization-store-test", {"all"}},
			"company.test.canonical-1",
			{"company.test.provider", std::string{producer_digest}},
			{"sha256:9999999999999999999999999999999999999999999999999999999999999999"},
			"evidence:materialization-store-test",
			{"exact", "partition", "assumptions:none", {"schema_validated"}},
		};
		auto claim = sdk::make_assertion(value, std::move(observation));
		require(claim.has_value(), "test Store claim rejected");
		return std::move(*claim);
	}

	[[nodiscard]] sdk::partition_draft
	partition(const sdk::relation_engine& value,
			  std::string key,
			  std::string scope,
			  std::string universe = "universe-materialization-store-test")
	{
		constexpr std::string_view producer_digest =
			"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
		sdk::partition_draft draft;
		draft.relation_descriptor_id = descriptor().id;
		draft.scope = std::move(scope);
		draft.condition = {std::move(universe), {"all"}};
		draft.interpretation = "company.test.canonical-1";
		draft.producer_semantics = producer_digest;
		draft.precision_profile = "exact";
		draft.assumption_set_id = "assumptions-empty";
		draft.claims = {make_claim(value, std::move(key), "payload")};
		auto basis = sdk::claim_input_basis_digest(draft.claims.front().input_basis);
		require(basis.has_value(), "test partition basis rejected");
		draft.producer_input_basis_digest = std::move(*basis);
		draft.coverage = {{"compile-unit", draft.scope, "covered", ""}};
		return draft;
	}

	[[nodiscard]] validated_publication_request
	publication_request(const sdk::snapshot_series_selector& selector_value,
						std::string backend,
						std::optional<std::string> parent,
						std::optional<std::string> sqlite_path = std::nullopt)
	{
		return {
			std::move(backend),
			selector_value,
			selector_value.id(),
			!parent.has_value(),
			std::move(parent),
			std::move(sqlite_path),
		};
	}

	[[nodiscard]] prepared_store_transaction plan(const sdk::relation_engine&,
												  const validated_publication_request& publication,
												  std::vector<sdk::partition_draft> partitions)
	{
		return {
			{publication.selector,
			 {1U, 0U, 0U},
			 "sha256:6666666666666666666666666666666666666666666666666666666666666666",
			 publication.expected_parent_publication},
			std::move(partitions),
			{},
		};
	}

	[[nodiscard]] prepared_store_transaction plan(const sdk::relation_engine& value,
												  const validated_publication_request& publication,
												  std::string key = "item:one")
	{
		return plan(value,
					publication,
					{partition(value, std::move(key), "compile-unit-materialization-store-test")});
	}

	class recording_opener final : public materialization_store_opener
	{
	  public:
		sdk::result<sdk::snapshot_store> open_memory(sdk::relation_engine value) override
		{
			++memory_call_count;
			return sdk::make_in_memory_snapshot_store(std::move(value));
		}

		sdk::result<sdk::snapshot_store> open_sqlite(const std::string& exact_path,
													 sdk::relation_engine value) override
		{
			const auto call = sqlite_call_count++;
			sqlite_paths.push_back(exact_path);
			if (failing_sqlite_call && call == *failing_sqlite_call)
				return injected_error;
			return sdk::open_sqlite_snapshot_store(exact_path, std::move(value));
		}

		std::size_t memory_call_count{};
		std::size_t sqlite_call_count{};
		std::vector<std::string> sqlite_paths;
		std::optional<std::size_t> failing_sqlite_call;
		sdk::error injected_error{"store.sqlite-failure", "open", "injected-test-boundary"};
	};

	class replayable_partition_source final : public materialization_store_partition_replay_source
	{
	  public:
		explicit replayable_partition_source(std::vector<sdk::partition_draft> partitions)
			: partitions_{std::move(partitions)}
		{
		}

		sdk::result<void> replay(const materialization_store_partition_consumer& consumer) override
		{
			if (!consumer)
				return sdk::unexpected(sdk::error{"store.partition-source", "consumer", "missing"});
			++replay_count;
			for (const auto& partition : partitions_)
			{
				auto copy = partition;
				if (auto consumed = consumer(std::move(copy)); !consumed)
					return consumed;
			}
			return {};
		}

		std::size_t replay_count{};

	  private:
		std::vector<sdk::partition_draft> partitions_;
	};

	class temporary_working_directory
	{
	  public:
		temporary_working_directory()
			: original_{std::filesystem::current_path()},
			  directory_{std::filesystem::temp_directory_path() /
						 ("cxxlens-materialization-store-" +
						  std::to_string(reinterpret_cast<std::uintptr_t>(this)))}
		{
			std::error_code error;
			std::filesystem::remove_all(directory_, error);
			error.clear();
			std::filesystem::create_directories(directory_, error);
			require(!error, "temporary Store directory creation failed");
			std::filesystem::current_path(directory_);
		}

		temporary_working_directory(const temporary_working_directory&) = delete;
		temporary_working_directory& operator=(const temporary_working_directory&) = delete;

		~temporary_working_directory()
		{
			std::error_code error;
			std::filesystem::current_path(original_, error);
			error.clear();
			std::filesystem::remove_all(directory_, error);
		}

	  private:
		std::filesystem::path original_;
		std::filesystem::path directory_;
	};

	void require_three_present_paths(const materialization_store_observation& observation)
	{
		const std::array expected_paths{
			materialization_store_path::current_selector,
			materialization_store_path::open_publication,
			materialization_store_path::open_snapshot,
		};
		for (std::size_t index{}; index < expected_paths.size(); ++index)
		{
			const auto& receipt = observation.verification_receipts[index];
			require(receipt.path == expected_paths[index] &&
						receipt.status == materialization_store_receipt_status::present &&
						receipt.projection.has_value() && receipt.handle.has_value() &&
						!receipt.error,
					"fixed-order Store verification receipt was not present");
		}
		require(observation.verification_receipts[0U].selector_lookup == observation.selector,
				"current receipt lost the exact selector lookup");
		require(observation.verification_receipts[1U].id_lookup ==
					observation.publish_returned_record->publication_id,
				"publication receipt lost the exact candidate lookup");
		require(observation.verification_receipts[2U].id_lookup ==
					observation.publish_returned_record->snapshot_id,
				"snapshot receipt lost the exact candidate lookup");
	}

	void require_three_paths_query_the_same_admitted_snapshot(
		const materialization_store_observation& observation, const sdk::relation_engine& value)
	{
		auto relation = value.require_id(descriptor().id);
		require(relation.has_value(), "query regression relation was not admitted");
		const auto expected_descriptor = descriptor();
		const auto expected_row = row("item:one", "payload").canonical_form();
		for (const auto& receipt : observation.verification_receipts)
		{
			require(receipt.handle.has_value(), "query regression lost a reopened handle");
			auto persisted_descriptor = receipt.handle->descriptor(expected_descriptor.id);
			require(persisted_descriptor && *persisted_descriptor == expected_descriptor,
					"reopened Store descriptor admission differs from the engine descriptor");

			auto claims = receipt.handle->open_claims(expected_descriptor.id);
			require(claims.has_value(), "reopened Store claim query was unavailable");
			auto claim = claims->next();
			require(claim && claim->has_value() && (*claim)->copy(),
					"reopened Store claim query lost its occurrence");
			auto claim_end = claims->next();
			require(claim_end && !*claim_end, "reopened Store claim query was not finite");

			auto rows = receipt.handle->open(*relation);
			require(rows.has_value(), "reopened Store row query was unavailable");
			auto first = rows->next();
			require(first && first->has_value(), "reopened Store row query lost its row");
			auto copied = (*first)->copy();
			require(copied && copied->canonical_form() == expected_row,
					"reopened Store row query returned a different canonical row");
			auto row_end = rows->next();
			require(row_end && !*row_end, "reopened Store row query was not finite");
		}
	}

	void memory_fresh_genesis()
	{
		const auto value = engine();
		const auto selector_value = selector(value);
		const auto publication = publication_request(selector_value, "memory", std::nullopt);
		auto prepared = prepare_materialization_store(value, publication, plan(value, publication));
		require(prepared.ready_for_publish() && !prepared.observation().first_issue &&
					prepared.observation().writer_begin_call_count == 1U &&
					!prepared.observation().publication_attempted &&
					prepared.observation().publish_call_count == 0U &&
					prepared.observation().candidate_manifest &&
					prepared.observation().candidate_identity,
				"memory preparation crossed publish before report construction could run");
		auto observed = publish_materialization_store(std::move(prepared));
		require(!observed.first_issue && observed.writer_begin_call_count == 1U &&
					observed.publication_attempted && observed.publish_call_count == 1U &&
					observed.publish_returned_handle && observed.publish_returned_record &&
					observed.verification_store &&
					observed.semantic_graph_validation ==
						materialization_store_semantic_graph_validation::candidate,
				"memory genesis did not cross exactly one publication boundary");
		require(observed.head_observation.status ==
						materialization_store_receipt_status::sdk_error &&
					observed.head_observation.error->code == "store.current-not-found",
				"memory fresh genesis did not retain the absent-head SDK receipt");
		require(observed.publish_returned_record->sequence == 1U &&
					!observed.publish_returned_record->parent_publication &&
					observed.publish_returned_record->state == sdk::publication_state::committed &&
					!observed.publish_returned_record->corrupt,
				"memory genesis returned record differs from SDK authority");
		require_three_present_paths(observed);
		for (const auto& receipt : observed.verification_receipts)
			require(receipt.projection->publication.publication_id ==
						observed.publish_returned_record->publication_id,
					"memory same-Store paths did not resolve the invocation publication");
		require_three_paths_query_the_same_admitted_snapshot(observed, value);
	}

	void authority_registry_digest_alias_is_rejected_before_store_open()
	{
		const auto value = engine();
		constexpr std::string_view authority_registry_digest =
			"sha256:4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f";
		const auto engine_registry_digest = std::string{value.registry_digest()};
		require(engine_registry_digest.starts_with("semantic-v2:sha256:") &&
					engine_registry_digest != authority_registry_digest,
				"test relation engine did not expose a distinct admitted registry digest");

		auto authority_bound_selector = selector(value);
		authority_bound_selector.relation_registry_digest = std::string{authority_registry_digest};
		const auto publication =
			publication_request(authority_bound_selector, "memory", std::nullopt);
		recording_opener opener;
		auto observed =
			execute_materialization_store(value, publication, plan(value, publication), opener);
		const auto* mismatch = observed.first_issue
			? std::get_if<materialization_store_mismatch>(&*observed.first_issue)
			: nullptr;
		require(mismatch && mismatch->operation == materialization_store_operation::configuration &&
					mismatch->projection == "engine-registry-digest" &&
					std::get<std::string>(mismatch->expected) == authority_registry_digest &&
					std::get<std::string>(mismatch->actual) == engine_registry_digest &&
					observed.head_observation.selector_lookup == authority_bound_selector &&
					observed.writer_begin_call_count == 0U && opener.memory_call_count == 0U &&
					!observed.publication_attempted && observed.publish_call_count == 0U &&
					!observed.publish_returned_record && !observed.verification_store,
				"authority registry digest was admitted before Store open");
	}

	void sqlite_genesis_append_and_stale()
	{
		temporary_working_directory working_directory;
		const auto value = engine();
		const auto selector_value = selector(value);
		recording_opener opener;

		const auto genesis_request =
			publication_request(selector_value, "sqlite", std::nullopt, "positive.sqlite");
		auto genesis = execute_materialization_store(
			value, genesis_request, plan(value, genesis_request), opener);
		require(!genesis.first_issue && genesis.publish_call_count == 1U &&
					genesis.publish_returned_record && opener.sqlite_call_count == 2U &&
					genesis.semantic_graph_validation ==
						materialization_store_semantic_graph_validation::reopened &&
					opener.sqlite_paths ==
						std::vector<std::string>{"positive.sqlite", "positive.sqlite"},
				"SQLite genesis did not use one publish and exact close/reopen path");
		require_three_present_paths(genesis);
		const auto genesis_record = *genesis.publish_returned_record;
		genesis.verification_store.reset();

		const auto append_request = publication_request(
			selector_value, "sqlite", genesis_record.publication_id, "positive.sqlite");
		auto append = execute_materialization_store(
			value, append_request, plan(value, append_request, "item:two"), opener);
		if (append.first_issue)
		{
			if (const auto* failure =
					std::get_if<materialization_store_sdk_failure>(&*append.first_issue))
				std::cerr << "append sdk failure operation=" << static_cast<int>(failure->operation)
						  << " code=" << failure->error.code << " field=" << failure->error.field
						  << " detail=" << failure->error.detail << '\n';
			else
				std::cerr << "append mismatch\n";
		}
		require(!append.first_issue && append.publish_call_count == 1U &&
					append.publish_returned_record &&
					append.publish_returned_record->sequence == 2U &&
					append.publish_returned_record->parent_publication ==
						genesis_record.publication_id &&
					opener.sqlite_call_count == 4U,
				"SQLite exact-parent append did not preserve SDK sequence/CAS authority");
		require(append.head_observation.projection &&
					append.head_observation.projection->publication == genesis_record,
				"SQLite append did not retain the exact observed parent record");
		require_three_present_paths(append);
		append.verification_store.reset();

		const auto stale_request = publication_request(
			selector_value, "sqlite", genesis_record.publication_id, "positive.sqlite");
		auto stale = execute_materialization_store(
			value, stale_request, plan(value, stale_request, "item:stale"), opener);
		require(stale.first_issue && stale.writer_begin_call_count == 0U &&
					!stale.publication_attempted && stale.publish_call_count == 0U &&
					opener.sqlite_call_count == 5U,
				"stale observed parent reached a writer or publish call");
		const auto* mismatch = std::get_if<materialization_store_mismatch>(&*stale.first_issue);
		require(mismatch && mismatch->operation == materialization_store_operation::head_current &&
					mismatch->projection == "expected-parent-publication" &&
					std::get<std::optional<std::string>>(mismatch->expected) ==
						genesis_record.publication_id &&
					std::get<std::optional<std::string>>(mismatch->actual) ==
						append.publish_returned_record->publication_id,
				"stale head was mapped to a fabricated SDK error instead of exact mismatch values");
	}

	void sqlite_publish_race_recovers_exact_receipts()
	{
		temporary_working_directory working_directory;
		const auto value = engine();
		const auto selector_value = selector(value);
		const auto genesis_request =
			publication_request(selector_value, "sqlite", std::nullopt, "race.sqlite");
		auto genesis = execute_materialization_store(
			value, genesis_request, plan(value, genesis_request, "item:genesis"));
		if (genesis.first_issue)
		{
			if (const auto* failure =
					std::get_if<materialization_store_sdk_failure>(&*genesis.first_issue))
				std::cerr << "genesis sdk failure operation="
						  << static_cast<int>(failure->operation) << " code=" << failure->error.code
						  << " field=" << failure->error.field
						  << " detail=" << failure->error.detail << '\n';
		}
		require(!genesis.first_issue && genesis.publish_returned_record,
				"race fixture genesis failed");
		const auto genesis_record = *genesis.publish_returned_record;
		genesis.verification_store.reset();

		const auto append_request = publication_request(
			selector_value, "sqlite", genesis_record.publication_id, "race.sqlite");
		recording_opener opener;
		auto prepared = prepare_materialization_store(
			value, append_request, plan(value, append_request, "item:losing-candidate"), opener);
		require(prepared.ready_for_publish() && opener.sqlite_call_count == 1U &&
					prepared.observation().candidate_identity &&
					!prepared.observation().publication_attempted,
				"SQLite race candidate was not held at the prepublication boundary");
		const auto losing_candidate = *prepared.observation().candidate_identity;

		auto competitor = execute_materialization_store(
			value, append_request, plan(value, append_request, "item:competing-winner"));
		if (competitor.first_issue)
		{
			if (const auto* failure =
					std::get_if<materialization_store_sdk_failure>(&*competitor.first_issue))
				std::cerr << "competitor sdk failure operation="
						  << static_cast<int>(failure->operation) << " code=" << failure->error.code
						  << " field=" << failure->error.field
						  << " detail=" << failure->error.detail << '\n';
			else
				std::cerr << "competitor mismatch\n";
		}
		if (competitor.first_issue)
		{
			const auto* unavailable =
				std::get_if<materialization_store_sdk_failure>(&*competitor.first_issue);
			require(unavailable &&
						unavailable->operation == materialization_store_operation::store_open &&
						unavailable->error.code == "store.backend-unavailable" &&
						unavailable->error.field == "sqlite" &&
						unavailable->error.detail == "source-shm-readonly-qualification",
					"SQLite race competitor fabricated a non-qualification failure while "
					"source-SHM was disabled");
			return;
		}
		require(!competitor.first_issue && competitor.publish_returned_record,
				"race fixture competitor did not publish");
		const auto competitor_record = *competitor.publish_returned_record;
		competitor.verification_store.reset();

		auto rejected = publish_materialization_store(std::move(prepared));
		const auto* publish_error = rejected.first_issue
			? std::get_if<materialization_store_sdk_failure>(&*rejected.first_issue)
			: nullptr;
		require(publish_error &&
					publish_error->operation == materialization_store_operation::writer_publish &&
					publish_error->error.code == "store.publication-conflict" &&
					rejected.publication_attempted && rejected.publish_call_count == 1U &&
					!rejected.publish_returned_record && rejected.recovery_receipt &&
					rejected.verification_store && opener.sqlite_call_count == 2U,
				"SQLite publish conflict did not close/reopen with the exact first SDK error");
		const auto& recovery = *rejected.recovery_receipt;
		require(
			recovery.reopen_status == materialization_store_reopen_status::opened &&
				recovery.current.status == materialization_store_lookup_status::present &&
				recovery.current.record == competitor_record &&
				recovery.expected_parent.status == materialization_store_lookup_status::present &&
				recovery.expected_parent.record == genesis_record &&
				recovery.candidate.status == materialization_store_lookup_status::not_found &&
				recovery.candidate.requested_publication_id == losing_candidate.publication_id &&
				recovery.candidate.error &&
				recovery.candidate.error->code == "store.publication-not-found",
			"SQLite recovery did not retain current/parent/candidate lookup authority");

		const auto second_append = publication_request(
			selector_value, "sqlite", competitor_record.publication_id, "race.sqlite");
		recording_opener failing_opener;
		auto prepared_for_failed_reopen = prepare_materialization_store(
			value,
			second_append,
			plan(value, second_append, "item:second-losing-candidate"),
			failing_opener);
		require(prepared_for_failed_reopen.ready_for_publish(),
				"second race candidate did not reach prepublication readiness");
		auto second_competitor = execute_materialization_store(
			value, second_append, plan(value, second_append, "item:second-winner"));
		require(!second_competitor.first_issue && second_competitor.publish_returned_record,
				"second race fixture competitor did not publish");
		second_competitor.verification_store.reset();
		failing_opener.failing_sqlite_call = 1U;
		auto unknown = publish_materialization_store(std::move(prepared_for_failed_reopen));
		require(unknown.first_issue && unknown.recovery_receipt &&
					unknown.recovery_receipt->reopen_status ==
						materialization_store_reopen_status::open_failed &&
					unknown.recovery_receipt->open_error == failing_opener.injected_error &&
					unknown.recovery_receipt->current.status ==
						materialization_store_lookup_status::not_attempted &&
					unknown.recovery_receipt->expected_parent.status ==
						materialization_store_lookup_status::not_attempted &&
					unknown.recovery_receipt->candidate.status ==
						materialization_store_lookup_status::not_attempted &&
					!unknown.verification_store,
				"SQLite publish conflict open failure invented recovery lookup results");
	}

	void typed_prepublication_failures()
	{
		const auto value = engine();
		const auto selector_value = selector(value);
		const auto memory_request = publication_request(selector_value, "memory", std::nullopt);
		auto valid = partition(value, "item:valid", "compile-unit-valid");
		auto invalid = partition(value, "item:invalid", "compile-unit-invalid", "wrong-universe");
		auto failed = execute_materialization_store(
			value,
			memory_request,
			plan(value, memory_request, {std::move(valid), std::move(invalid)}));
		const auto* stage_error = failed.first_issue
			? std::get_if<materialization_store_sdk_failure>(&*failed.first_issue)
			: nullptr;
		require(stage_error &&
					stage_error->operation == materialization_store_operation::partition_stage &&
					stage_error->error.code == "store.condition-universe-mismatch" &&
					failed.writer_begin_call_count == 1U && !failed.publication_attempted &&
					failed.publish_call_count == 0U,
				"stage failure was not retained as the first exact prepublication SDK error");

		const auto sqlite_request =
			publication_request(selector_value, "sqlite", std::nullopt, "open-failure.sqlite");
		recording_opener opener;
		opener.failing_sqlite_call = 0U;
		auto open_failed = execute_materialization_store(
			value, sqlite_request, plan(value, sqlite_request), opener);
		const auto* open_error = open_failed.first_issue
			? std::get_if<materialization_store_sdk_failure>(&*open_failed.first_issue)
			: nullptr;
		require(
			open_error && open_error->operation == materialization_store_operation::store_open &&
				open_error->error == opener.injected_error &&
				open_failed.writer_begin_call_count == 0U && open_failed.publish_call_count == 0U,
			"Store-open failure did not retain the injected typed SDK error");
	}

	void streaming_store_replays_and_rechecks_exact_partitions()
	{
		const auto value = engine();
		const auto selector_value = selector(value);
		const auto publication = publication_request(selector_value, "memory", std::nullopt);
		auto draft = partition(value, "item:streaming", "compile-unit-streaming");
		const auto expected =
			execute_materialization_store(value, publication, plan(value, publication, {draft}));
		require(!expected.first_issue && expected.publish_returned_record &&
					expected.candidate_manifest,
				"reference Store publication for streaming comparison failed");

		replayable_partition_source source{{draft}};
		streaming_prepared_store_transaction prepared{
			{publication.selector,
			 {1U, 0U, 0U},
			 "sha256:6666666666666666666666666666666666666666666666666666666666666666",
			 publication.expected_parent_publication},
			{},
			{}};
		auto streamed = execute_materialization_store_streaming(
			value, publication, std::move(prepared), source);
		require(!streamed.first_issue && streamed.publish_returned_record &&
					streamed.candidate_manifest && source.replay_count == 2U,
				"streaming Store did not complete the two replay publication boundary");
		require(streamed.semantic_graph_validation ==
					materialization_store_semantic_graph_validation::candidate,
				"streaming Store did not retain the SDK candidate graph-validation phase");
		require(*streamed.publish_returned_record == *expected.publish_returned_record &&
					*streamed.candidate_manifest == *expected.candidate_manifest,
				"streaming Store changed the exact publication or manifest identity");
	}

	void sqlite_streaming_store_reopens_and_revalidates_graph()
	{
		temporary_working_directory working_directory;
		const auto value = engine();
		const auto selector_value = selector(value);
		const auto publication =
			publication_request(selector_value, "sqlite", std::nullopt, "streaming.sqlite");
		const auto draft =
			partition(value, "item:streaming-sqlite", "compile-unit-streaming-sqlite");
		replayable_partition_source source{{draft}};
		recording_opener opener;
		streaming_prepared_store_transaction prepared{
			{publication.selector,
			 {1U, 0U, 0U},
			 "sha256:6666666666666666666666666666666666666666666666666666666666666666",
			 publication.expected_parent_publication},
			{},
			{}};
		auto streamed = execute_materialization_store_streaming(
			value, publication, std::move(prepared), source, opener);
		require(!streamed.first_issue && streamed.publish_returned_record &&
					streamed.verification_store && source.replay_count == 2U &&
					opener.sqlite_call_count == 2U &&
					streamed.semantic_graph_validation ==
						materialization_store_semantic_graph_validation::reopened,
				"SQLite streaming Store did not complete close/reopen semantic validation");
		require_three_present_paths(streamed);
		auto relation = value.require_id(descriptor().id);
		require(relation.has_value(), "SQLite streaming query relation was not admitted");
		const auto expected_row = row("item:streaming-sqlite", "payload").canonical_form();
		for (const auto& receipt : streamed.verification_receipts)
		{
			require(receipt.handle->query_annotations_available(),
					"SQLite streaming reopen lost v5 query annotations");
				require(receipt.handle->input_coverage().size() == 1U &&
						receipt.handle->partition_bindings().size() == 1U &&
						receipt.handle->unresolved_items().empty(),
					"SQLite streaming reopen lost semantic graph side projections");
			auto claims = receipt.handle->open_claims(descriptor().id);
			require(claims.has_value(), "SQLite streaming reopen lost claim annotations");
			auto claim = claims->next();
			require(claim && claim->has_value() && (*claim)->copy(),
					"SQLite streaming reopen lost the persisted claim occurrence");
			auto claim_end = claims->next();
			require(claim_end && !*claim_end,
					"SQLite streaming reopen claim cursor was not finite");
			auto rows = receipt.handle->open(*relation);
			require(rows.has_value(), "SQLite streaming reopen lost row query");
			auto first = rows->next();
			require(first && first->has_value(),
					"SQLite streaming reopen lost the persisted row");
			auto copied = (*first)->copy();
			require(copied && copied->canonical_form() == expected_row,
					"SQLite streaming reopen changed the persisted row projection");
			auto row_end = rows->next();
			require(row_end && !*row_end,
					"SQLite streaming reopen row cursor was not finite");
		}
	}

	void sqlite_reopen_failure_retains_commit()
	{
		temporary_working_directory working_directory;
		const auto value = engine();
		const auto selector_value = selector(value);
		const auto publication =
			publication_request(selector_value, "sqlite", std::nullopt, "reopen-failure.sqlite");
		recording_opener opener;
		opener.failing_sqlite_call = 1U;
		auto observed =
			execute_materialization_store(value, publication, plan(value, publication), opener);
		const auto* reopen_error = observed.first_issue
			? std::get_if<materialization_store_sdk_failure>(&*observed.first_issue)
			: nullptr;
		require(reopen_error &&
					reopen_error->operation == materialization_store_operation::store_reopen &&
					reopen_error->error == opener.injected_error &&
					observed.semantic_graph_validation ==
						materialization_store_semantic_graph_validation::candidate &&
					observed.publication_attempted && observed.publish_call_count == 1U &&
					observed.publish_returned_record && !observed.verification_store &&
					std::ranges::all_of(observed.verification_receipts,
										[](const materialization_store_path_receipt& receipt)
										{
											return receipt.status ==
												materialization_store_receipt_status::not_attempted;
										}),
				"postcommit reopen error lost the actual commit or invented path receipts");

		auto recovered = sdk::open_sqlite_snapshot_store("reopen-failure.sqlite", value);
		require(recovered.has_value(), "independent recovery could not reopen committed SQLite");
		auto current = recovered->current(selector_value);
		require(current && current->publication() == *observed.publish_returned_record,
				"publish-returned record was not the committed recovery authority");
	}

	void sqlite_v2_begin_is_phase_authentic_and_does_not_migrate()
	{
		using namespace cxxlens::test::sqlite_fixture;
		temporary_working_directory working_directory;
		const auto value = engine();
		const auto selector_value = selector(value);
		const auto path = std::filesystem::path{"migration-required.sqlite"};
		const auto genesis_request =
			publication_request(selector_value, "sqlite", std::nullopt, path.string());
		auto genesis = execute_materialization_store(
			value, genesis_request, plan(value, genesis_request, "item:v2-genesis"));
		if (genesis.first_issue)
		{
			if (const auto* failure =
					std::get_if<materialization_store_sdk_failure>(&*genesis.first_issue))
				std::cerr << "v2 genesis sdk failure operation="
						  << static_cast<int>(failure->operation) << " code=" << failure->error.code
						  << " field=" << failure->error.field
						  << " detail=" << failure->error.detail << '\n';
			else
				std::cerr << "v2 genesis non-sdk issue\n";
		}
		require(!genesis.first_issue && genesis.publish_returned_record &&
					genesis.verification_store,
				"materializer v2 fixture genesis failed");
		const auto genesis_record = *genesis.publish_returned_record;
		genesis.verification_store.reset();
		downgrade_v3_to_exact_v2(path);
		require_wal_header_and_quiescent_sidecars(path);
		const auto before = capture_files(path);

		const auto append_request = publication_request(
			selector_value, "sqlite", genesis_record.publication_id, path.string());
		recording_opener opener;
		auto observed = execute_materialization_store(
			value, append_request, plan(value, append_request, "item:v2-append"), opener);
		const auto* failure = observed.first_issue
			? std::get_if<materialization_store_sdk_failure>(&*observed.first_issue)
			: nullptr;
		require(failure && failure->operation == materialization_store_operation::writer_begin &&
					failure->error.code == "store.migration-required" &&
					failure->error.field == "sqlite-physical-format" &&
					failure->error.detail == "cxxlens.sqlite-semantic-store.v2-to-v3" &&
					observed.head_observation.status ==
						materialization_store_receipt_status::present &&
					observed.head_observation.projection &&
					observed.head_observation.projection->publication == genesis_record &&
					opener.sqlite_call_count == 1U && observed.writer_begin_call_count == 1U &&
					!observed.publication_attempted && observed.publish_call_count == 0U &&
					!observed.publish_returned_record && !observed.recovery_receipt &&
					!observed.verification_store &&
					std::ranges::all_of(observed.verification_receipts,
										[](const materialization_store_path_receipt& receipt)
										{
											return receipt.status ==
												materialization_store_receipt_status::not_attempted;
										}),
				"v2 materializer failure was not the exact one-shot writer_begin result");
		const auto after = capture_files(path);
		require(after == before,
				"v2 materializer writer_begin implicitly migrated or changed source bytes");
	}

	void bounded_projection_streams_compare_without_resident_graph()
	{
		const materialization_store_projection_limits limits{256U, 64U};
		auto actual = materialization_store_projection_writer::create(limits);
		auto expected = materialization_store_projection_writer::create(limits);
		require(actual && expected, "projection writer creation failed");
		const auto append = [](materialization_store_projection_writer& writer,
										const std::string_view value) -> sdk::result<void>
		{
			return writer.append(std::as_bytes(std::span{value.data(), value.size()}));
		};
		require(append(*actual, "record:a") && append(*actual, "record:b") &&
					append(*expected, "record:a") && append(*expected, "record:b"),
				"projection record append failed");
		auto actual_receipt = actual->seal();
		auto expected_receipt = expected->seal();
		require(actual_receipt && expected_receipt && actual->sealed() && expected->sealed() &&
					actual_receipt->record_count == 2U &&
					expected_receipt->record_count == 2U &&
					actual_receipt->payload_bytes == expected_receipt->payload_bytes,
				"projection stream seal receipt was not exact");
		auto equal = compare_materialization_store_projections(*actual, *expected);
		require(equal && equal->equal && equal->actual_record_count == 2U &&
					equal->expected_record_count == 2U && !equal->first_mismatch_offset,
				"identical independent projection streams did not compare equal");

		auto reordered = materialization_store_projection_writer::create(limits);
		require(reordered && append(*reordered, "record:b") && append(*reordered, "record:a"),
				"reordered projection append failed");
		const auto reordered_receipt = reordered->seal();
		require(static_cast<bool>(reordered_receipt), "reordered projection seal failed");
		auto mismatch = compare_materialization_store_projections(*actual, *reordered);
		require(mismatch && !mismatch->equal && mismatch->actual_record_count == 2U &&
					mismatch->expected_record_count == 2U && mismatch->first_mismatch_offset,
				"record reordering was not retained as a byte-level projection mismatch");

		auto oversized = materialization_store_projection_writer::create(limits);
		require(static_cast<bool>(oversized), "oversized projection writer creation failed");
		const std::string oversized_record(65U, 'x');
		require(!append(*oversized, oversized_record) && !oversized->seal(),
				"projection writer accepted a record beyond its declared bound");

		const materialization_store_projection_limits aggregate_limits{32U, 24U};
		auto aggregate = materialization_store_projection_writer::create(aggregate_limits);
		require(aggregate && append(*aggregate, std::string(24U, 'a')) &&
				!append(*aggregate, "b") && !aggregate->seal(),
				"projection writer crossed its aggregate framed-byte bound");

		const materialization_store_projection_limits window_limits{256U * 1024U, 128U * 1024U};
		auto window_actual = materialization_store_projection_writer::create(window_limits);
		auto window_expected = materialization_store_projection_writer::create(window_limits);
		require(window_actual && window_expected, "large projection writer creation failed");
		std::string large_record(96U * 1024U, 'p');
		std::string altered_record = large_record;
		altered_record[80U * 1024U] = 'q';
		require(append(*window_actual, large_record) && append(*window_expected, altered_record),
				"large projection record append failed");
		require(window_actual->seal() && window_expected->seal(),
				"large projection stream seal failed");
		auto window_mismatch =
			compare_materialization_store_projections(*window_actual, *window_expected);
		require(window_mismatch && !window_mismatch->equal &&
				window_mismatch->first_mismatch_offset &&
				*window_mismatch->first_mismatch_offset == 8U + 80U * 1024U,
				"multi-window projection mismatch was not byte-exact");
	}
} // namespace

int main()
{
	sqlite_v2_begin_is_phase_authentic_and_does_not_migrate();
	memory_fresh_genesis();
	authority_registry_digest_alias_is_rejected_before_store_open();
	sqlite_genesis_append_and_stale();
	sqlite_publish_race_recovers_exact_receipts();
	typed_prepublication_failures();
	streaming_store_replays_and_rechecks_exact_partitions();
	sqlite_streaming_store_reopens_and_revalidates_graph();
	sqlite_reopen_failure_retains_commit();
	bounded_projection_streams_compare_without_resident_graph();
	return 0;
}
