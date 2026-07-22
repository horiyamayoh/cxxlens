#include "llvm/clang22/materialization_report.hpp"

#include <array>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <cxxlens/sdk.hpp>

#include "llvm/clang22/materialization_json.hpp"

namespace
{
	using namespace cxxlens::detail::clang22::materialization;
	namespace sdk = cxxlens::sdk;

	void require(const bool condition, const std::string& message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	template <class Journal>
	concept compact_failure_capable = requires(Journal&& journal) {
		std::move(journal).issue_compact_failure(
			compact_report_error{"materialization.report-invalid", "report", "test"});
	};

	static_assert(!std::default_initializable<compact_failure_authority>);
	static_assert(!std::copy_constructible<compact_failure_authority>);
	static_assert(!std::is_copy_assignable_v<compact_failure_authority>);
	static_assert(std::move_constructible<compact_failure_authority>);
	static_assert(compact_failure_capable<materialization_execution_journal>);
	static_assert(!compact_failure_capable<materialization_postpublication_journal>);

	[[nodiscard]] raw_input_observation complete_input()
	{
		const std::string raw_request{"{invalid-request}"};
		return {
			maximum_raw_request_bytes,
			raw_request.size(),
			cxxlens::sdk::content_digest(std::as_bytes(std::span{raw_request})),
			true,
		};
	}

	[[nodiscard]] compact_request_binding request_binding()
	{
		return {
			"materialization-request:test",
			"semantic-v2:sha256:1111111111111111111111111111111111111111111111111111111111111111",
			"semantic-v2:sha256:2222222222222222222222222222222222222222222222222222222222222222",
		};
	}

	[[nodiscard]] materialization_execution_journal bound_journal(const std::uint64_t task_count)
	{
		auto started = materialization_execution_journal::begin(complete_input());
		require(started.has_value(), "request-bound journal did not start");
		auto journal = std::move(*started);
		require(journal.pass_input_limit().has_value() && journal.pass_json_decode().has_value() &&
					journal.pass_request_envelope().has_value() &&
					journal.pass_request_version().has_value() &&
					journal.pass_request_schema().has_value() &&
					journal.authenticate_request(request_binding(), task_count).has_value() &&
					journal.complete_installation_binding().has_value(),
				"request-bound journal rejected the exact phase order");
		return journal;
	}

	void complete_worker_census(materialization_execution_journal& journal,
								const std::uint64_t task_count)
	{
		for (std::uint64_t index{}; index < task_count; ++index)
			require(journal.record_worker_launch_attempt().has_value() &&
						journal.record_worker_launch_success().has_value(),
					"journal rejected one authenticated worker launch");
		require(journal.complete_worker_launches().has_value(),
				"journal rejected the complete worker census");
	}

	[[nodiscard]] const json_value& required_member(const json_value& object,
													const std::string_view name)
	{
		const auto* value = object.member(name);
		require(value != nullptr, "compact report omitted " + std::string{name});
		return *value;
	}

	[[nodiscard]] std::uint64_t required_unsigned(const json_value& object,
												  const std::string_view name)
	{
		const auto* value = required_member(object, name).as_unsigned_integer();
		require(value != nullptr, "compact report member is not unsigned: " + std::string{name});
		return *value;
	}

	[[nodiscard]] std::string_view required_string(const json_value& object,
												   const std::string_view name)
	{
		const auto* value = required_member(object, name).as_string();
		require(value != nullptr, "compact report member is not a string: " + std::string{name});
		return *value;
	}

	[[nodiscard]] sdk::relation_descriptor descriptor()
	{
		sdk::relation_descriptor value;
		value.id = "company.test.compact_item.v1";
		value.name = "company.test.compact_item";
		value.version = {1U, 0U, 0U};
		value.semantic_major = 1U;
		value.semantics = "company.test.compact-item/1";
		value.owner_namespace = "company.test";
		value.columns = {
			{"company.test.compact_item.v1.key",
			 "key",
			 {sdk::scalar_kind::typed_id, "company_compact_item_id", false},
			 true,
			 sdk::column_role::claim_key},
		};
		value.key_columns = {"company.test.compact_item.v1.key"};
		value.merge = sdk::merge_mode::set;
		value.descriptor_digest =
			*sdk::semantic_digest("cxxlens.relation-descriptor-binding.v2",
								  value.contract_digest + "\n" + value.canonical_form());
		return value;
	}

	[[nodiscard]] sdk::relation_engine engine()
	{
		sdk::relation_registry registry;
		require(registry.add(descriptor()).has_value(), "compact test descriptor rejected");
		auto built = registry.build("engine-materialization-compact-test");
		require(built.has_value(), "compact test engine build failed");
		return std::move(*built);
	}

	[[nodiscard]] sdk::snapshot_series_selector selector(const sdk::relation_engine& value)
	{
		return {
			"catalog-materialization-compact-test",
			"stable",
			std::string{value.generation()},
			"universe-materialization-compact-test",
			std::string{value.registry_digest()},
			"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
			"sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
		};
	}

	[[nodiscard]] sdk::claim claim(const sdk::relation_engine& value)
	{
		const auto relation = descriptor();
		sdk::row_builder builder{relation};
		require(builder
					.set({relation.id, relation.columns.front().id, relation.columns.front().type},
						 sdk::detached_cell::typed("company_compact_item_id", "item:compact"))
					.has_value(),
				"compact test row rejected");
		auto row = std::move(builder).finish();
		require(row.has_value(), "compact test row did not finish");
		sdk::observation observation{
			std::move(*row),
			{"universe-materialization-compact-test", {"all"}},
			"company.test.canonical-1",
			{"company.test.provider",
			 "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
			{"sha256:9999999999999999999999999999999999999999999999999999999999999999"},
			"evidence:materialization-compact-test",
			{"exact", "partition", "assumptions:none", {"schema_validated"}},
		};
		auto result = sdk::make_assertion(value, std::move(observation));
		require(result.has_value(), "compact test claim rejected");
		return std::move(*result);
	}

	[[nodiscard]] sdk::partition_draft partition(const sdk::relation_engine& value,
												 std::string universe)
	{
		sdk::partition_draft result;
		result.relation_descriptor_id = descriptor().id;
		result.scope = "compile-unit-materialization-compact-test";
		result.condition = {std::move(universe), {"all"}};
		result.interpretation = "company.test.canonical-1";
		result.producer_semantics =
			"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
		result.precision_profile = "exact";
		result.assumption_set_id = "assumptions-empty";
		result.claims = {claim(value)};
		auto basis = sdk::claim_input_basis_digest(result.claims.front().input_basis);
		require(basis.has_value(), "compact test partition basis rejected");
		result.producer_input_basis_digest = std::move(*basis);
		result.coverage = {{"compile-unit", result.scope, "covered", ""}};
		return result;
	}

	[[nodiscard]] validated_publication_request
	publication_request(const sdk::snapshot_series_selector& selector_value)
	{
		return {
			"memory",
			selector_value,
			selector_value.id(),
			true,
			std::nullopt,
			std::nullopt,
		};
	}

	[[nodiscard]] prepared_store_transaction
	store_plan(const sdk::relation_engine& value,
			   const validated_publication_request& publication,
			   std::string universe = "universe-materialization-compact-test")
	{
		return {
			{publication.selector,
			 {1U, 0U, 0U},
			 "sha256:6666666666666666666666666666666666666666666666666666666666666666",
			 publication.expected_parent_publication},
			{partition(value, std::move(universe))},
			{},
		};
	}

	class failing_store_opener final : public materialization_store_opener
	{
	  public:
		sdk::result<sdk::snapshot_store> open_memory(sdk::relation_engine) override
		{
			return sdk::unexpected(error);
		}

		sdk::result<sdk::snapshot_store> open_sqlite(const std::string&,
													 sdk::relation_engine) override
		{
			return sdk::unexpected(error);
		}

		sdk::error error{"store.sqlite-failure", "open", "secret diagnostic prose"};
	};

	class corrupt_head_store_opener final : public materialization_store_opener
	{
	  public:
		explicit corrupt_head_store_opener(validated_publication_request publication)
			: publication_{std::move(publication)}
		{
		}

		sdk::result<sdk::snapshot_store> open_memory(sdk::relation_engine value) override
		{
			auto store = sdk::make_in_memory_snapshot_store(value);
			if (!store)
				return sdk::unexpected(std::move(store.error()));
			auto plan = store_plan(value, publication_);
			auto writer = store->begin(std::move(plan.draft));
			if (!writer)
				return sdk::unexpected(std::move(writer.error()));
			for (auto& staged : plan.partitions)
				if (auto result = writer->stage(std::move(staged)); !result)
					return sdk::unexpected(std::move(result.error()));
			if (auto valid = writer->validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			auto published = writer->publish();
			if (!published)
				return sdk::unexpected(std::move(published.error()));
			if (auto corrupted = sdk::mark_publication_corrupt_for_testing(
					*store, published->publication().publication_id);
				!corrupted)
				return sdk::unexpected(std::move(corrupted.error()));
			return std::move(*store);
		}

		sdk::result<sdk::snapshot_store> open_sqlite(const std::string&,
													 sdk::relation_engine) override
		{
			return sdk::unexpected(
				sdk::error{"store.sqlite-failure", "open", "unexpected-sqlite-open"});
		}

	  private:
		validated_publication_request publication_;
	};

	class temporary_working_directory final
	{
	  public:
		temporary_working_directory()
			: original_{std::filesystem::current_path()},
			  directory_{std::filesystem::temp_directory_path() /
						 ("cxxlens-materialization-report-" +
						  std::to_string(reinterpret_cast<std::uintptr_t>(this)))}
		{
			std::error_code error;
			std::filesystem::remove_all(directory_, error);
			error.clear();
			std::filesystem::create_directories(directory_, error);
			require(!error, "temporary report directory creation failed");
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

	void input_limit_failure_is_phase_authentic()
	{
		const std::string observed_prefix{"bounded-prefix"};
		auto journal = materialization_execution_journal::begin({
			maximum_raw_request_bytes,
			maximum_raw_request_bytes + 1U,
			sdk::content_digest(std::as_bytes(std::span{observed_prefix})),
			false,
		});
		require(journal.has_value() && !journal->pass_input_limit(),
				"over-limit observation crossed the input-limit boundary");
		auto authority = std::move(*journal).issue_compact_failure(
			{"materialization.request-invalid", "input", "request-size-limit"});
		require(authority.has_value() && authority->phase() == compact_failure_phase::input_limit &&
					!authority->request_binding() &&
					authority->worker_launch_attempt_count() == 0U &&
					authority->worker_launch_success_count() == 0U &&
					authority->store_draft_state() == compact_store_draft_state::not_created &&
					authority->head_observation() == compact_head_observation::not_observed &&
					!authority->store_failure_cause(),
				"input-limit token did not preserve the exact zero-effect phase");
	}

	void spool_failure_phases_are_closed()
	{
		{
			auto journal = materialization_execution_journal::begin(complete_input());
			require(journal && journal->pass_input_limit(),
					"json-decode spool journal setup failed");
			auto authority = std::move(*journal).issue_compact_failure(
				{"materialization.spool-failure", "task-index", "create"});
			require(authority && authority->phase() == compact_failure_phase::json_decode,
					"json-decode spool failure was rejected or rephased");
		}
		{
			auto journal = materialization_execution_journal::begin(complete_input());
			require(journal && journal->pass_input_limit() && journal->pass_json_decode() &&
						journal->pass_request_envelope() && journal->pass_request_version(),
					"request-schema spool journal setup failed");
			auto authority = std::move(*journal).issue_compact_failure(
				{"materialization.spool-failure", "task-unique-index", "seal"});
			require(authority && authority->phase() == compact_failure_phase::request_schema,
					"request-schema spool failure was rejected or rephased");
		}
		{
			auto journal = materialization_execution_journal::begin(complete_input());
			require(journal && journal->pass_input_limit() && journal->pass_json_decode() &&
						journal->pass_request_envelope() && journal->pass_request_version() &&
						journal->pass_request_schema(),
					"request-binding spool journal setup failed");
			auto authority = std::move(*journal).issue_compact_failure(
				{"materialization.spool-failure", "execution-unique-index", "read"});
			require(authority && authority->phase() == compact_failure_phase::request_binding,
					"request-binding spool failure was rejected or rephased");
		}
	}

	void exact_worker_census_is_journal_owned()
	{
		auto journal = bound_journal(3U);
		require(journal.record_worker_launch_attempt().has_value() &&
					journal.record_worker_launch_success().has_value() &&
					!journal.complete_worker_launches(),
				"journal accepted an incomplete worker census");
		require(journal.record_worker_launch_attempt().has_value() &&
					journal.record_worker_launch_success().has_value() &&
					journal.record_worker_launch_attempt().has_value(),
				"journal rejected the third worker launch attempt");
		auto authority = std::move(journal).issue_compact_failure(
			{"materialization.worker-failure", "worker", "third-launch-failed"});
		require(authority.has_value(), "journal rejected the exact failed-launch census");
		auto report = encode_compact_failure_report(*authority, "2026-07-20T12:34:56Z");
		require(report.has_value(), "worker compact report was rejected");
		auto parsed = parse_json_object(report->substr(0U, report->size() - 1U));
		require(parsed.has_value(), "worker compact report did not parse");
		const auto& effects = required_member(parsed->root(), "effects");
		require(required_unsigned(effects, "worker_launch_attempt_count") == 3U &&
					required_unsigned(effects, "worker_launch_success_count") == 2U &&
					required_string(effects, "store_draft_state") == "not-created" &&
					required_string(effects, "head_observation") == "not-observed",
				"worker compact report did not preserve the exact task/effect census");
		require(!journal.record_worker_launch_attempt(),
				"consumed journal issued more than one compact authority");

		auto moved = std::move(*authority);
		require(moved.valid() && !authority->valid() &&
					!encode_compact_failure_report(*authority, "2026-07-20T12:34:56Z"),
				"compact authority was copy-like or a consumed token remained usable");
	}

	[[nodiscard]] std::string typed_store_cause_preserves_exact_detail()
	{
		constexpr std::uint64_t task_count = 3U;
		const auto value = engine();
		const auto selector_value = selector(value);
		const auto publication = publication_request(selector_value);
		failing_store_opener opener;
		auto prepared = prepare_materialization_store(
			value, publication, store_plan(value, publication), opener);
		require(!prepared.ready_for_publish(), "failing Store opener produced a draft");

		auto journal = bound_journal(task_count);
		complete_worker_census(journal, task_count);
		require(journal.complete_transcript_validation().has_value() &&
					journal.complete_materialization_validation().has_value() &&
					journal.record_store_preparation(std::move(prepared)).has_value(),
				"journal rejected the first typed Store-open failure");
		auto authority = std::move(journal).issue_compact_failure(
			{"materialization.store-failure", "store", "store-open-failed"});
		require(authority.has_value() && authority->store_failure_cause(),
				"Store-open failure did not issue compact authority");
		const auto& cause = *authority->store_failure_cause();
		const auto detail = std::as_bytes(
			std::span<const char>{opener.error.detail.data(), opener.error.detail.size()});
		require(cause.operation == materialization_store_operation::store_open && !cause.path &&
					cause.code == opener.error.code && cause.field == opener.error.field &&
					cause.detail == opener.error.detail &&
					cause.detail_byte_count == opener.error.detail.size() &&
					cause.detail_digest == sdk::content_digest(detail),
				"Store cause did not retain the exact typed SDK observation");
		auto report = encode_compact_failure_report(*authority, "2026-07-20T12:34:56Z");
		require(report.has_value() && report->find(opener.error.detail) != std::string::npos,
				"compact report lost the exact Store diagnostic occurrence");
		auto parsed = parse_json_object(report->substr(0U, report->size() - 1U));
		require(parsed.has_value(), "Store compact report did not parse");
		const auto& effects = required_member(parsed->root(), "effects");
		const auto& encoded_cause = required_member(effects, "store_failure_cause");
		const auto& encoded_detail = required_member(encoded_cause, "detail");
		require(required_string(encoded_cause, "operation") == "store_open" &&
					required_member(encoded_cause, "access_path").is_null() &&
					required_string(encoded_detail, "kind") == "opaque" &&
					required_unsigned(encoded_detail, "byte_count") == opener.error.detail.size() &&
					required_string(encoded_detail, "digest") == cause.detail_digest &&
					required_string(encoded_detail, "diagnostic") == opener.error.detail,
				"compact report did not serialize the bounded typed Store cause");
		return std::move(*report);
	}

	[[nodiscard]] std::string failed_head_observation_is_four_state_and_path_bound()
	{
		constexpr std::uint64_t task_count = 1U;
		const auto value = engine();
		const auto selector_value = selector(value);
		const auto genesis = publication_request(selector_value);
		corrupt_head_store_opener corrupt_opener{genesis};
		auto failed = prepare_materialization_store(
			value, genesis, store_plan(value, genesis), corrupt_opener);
		require(!failed.ready_for_publish() && failed.observation().writer_begin_call_count == 0U,
				"failed head lookup reached writer construction");

		auto journal = bound_journal(task_count);
		complete_worker_census(journal, task_count);
		require(journal.complete_transcript_validation().has_value() &&
					journal.complete_materialization_validation().has_value() &&
					journal.record_store_preparation(std::move(failed)).has_value(),
				"journal rejected the exact failed head receipt");
		auto authority = std::move(journal).issue_compact_failure(
			{"materialization.store-failure", "store", "head-current-failed"});
		require(authority && authority->phase() == compact_failure_phase::store_stage &&
					authority->store_draft_state() == compact_store_draft_state::discarded &&
					authority->head_observation() == compact_head_observation::sdk_error &&
					!authority->observed_head_publication() && authority->store_failure_cause() &&
					authority->store_failure_cause()->operation ==
						materialization_store_operation::head_current &&
					authority->store_failure_cause()->path ==
						materialization_store_path::current_selector &&
					authority->store_failure_cause()->code != "store.current-not-found",
				"failed head receipt was relabeled or lost its exact path");
		auto report = encode_compact_failure_report(*authority, "2026-07-20T12:34:56Z");
		require(report && report->find("\"head_observation\":\"sdk-error\"") != report->npos &&
					report->find("\"access_path\":\"current-selector\"") != report->npos,
				"failed head compact report lost its closed state or path");

		temporary_working_directory working_directory;
		auto missing_parent = genesis;
		missing_parent.backend = "sqlite";
		missing_parent.genesis = false;
		missing_parent.expected_parent_publication = "publication:missing-parent";
		missing_parent.sqlite_path = "missing.sqlite";
		auto absent =
			prepare_materialization_store(value, missing_parent, store_plan(value, missing_parent));
		require(!absent.ready_for_publish() && absent.observation().writer_begin_call_count == 0U,
				"missing head reached writer construction");
		const auto* absent_failure = absent.observation().first_issue
			? std::get_if<materialization_store_sdk_failure>(&*absent.observation().first_issue)
			: nullptr;
		require(absent_failure != nullptr, "missing head did not retain an SDK failure");
		require(!absent_failure->error.field.empty(),
				"current-not-found SDK error has an empty field: " + absent_failure->error.code +
					":" + absent_failure->error.detail);
		auto absent_journal = bound_journal(task_count);
		complete_worker_census(absent_journal, task_count);
		require(absent_journal.complete_transcript_validation().has_value() &&
					absent_journal.complete_materialization_validation().has_value(),
				"journal did not reach Store preparation for current-not-found");
		auto absent_recorded = absent_journal.record_store_preparation(std::move(absent));
		require(absent_recorded.has_value(),
				"journal rejected exact current-not-found receipt: " +
					(absent_recorded ? std::string{} : absent_recorded.error().detail));
		auto absent_authority =
			std::move(absent_journal)
				.issue_compact_failure(
					{"materialization.store-failure", "store", "head-current-not-found"});
		require(absent_authority &&
					absent_authority->head_observation() == compact_head_observation::absent &&
					absent_authority->store_draft_state() == compact_store_draft_state::discarded &&
					absent_authority->store_failure_cause()->code == "store.current-not-found" &&
					absent_authority->store_failure_cause()->path ==
						materialization_store_path::current_selector,
				"current-not-found was not kept distinct from an SDK error");
		return std::move(*report);
	}

	void store_stage_and_publication_boundary_are_closed()
	{
		constexpr std::uint64_t task_count = 2U;
		const auto value = engine();
		const auto selector_value = selector(value);
		const auto publication = publication_request(selector_value);

		auto stage_failed = prepare_materialization_store(
			value, publication, store_plan(value, publication, "wrong-universe"));
		auto stage_journal = bound_journal(task_count);
		complete_worker_census(stage_journal, task_count);
		require(stage_journal.complete_transcript_validation().has_value() &&
					stage_journal.complete_materialization_validation().has_value() &&
					stage_journal.record_store_preparation(std::move(stage_failed)).has_value(),
				"journal rejected the exact Store-stage failure");
		auto stage_authority =
			std::move(stage_journal)
				.issue_compact_failure(
					{"materialization.store-failure", "store", "partition-stage-failed"});
		require(stage_authority.has_value() &&
					stage_authority->phase() == compact_failure_phase::store_stage &&
					stage_authority->store_draft_state() == compact_store_draft_state::discarded &&
					stage_authority->head_observation() == compact_head_observation::absent &&
					stage_authority->store_failure_cause() &&
					stage_authority->store_failure_cause()->operation ==
						materialization_store_operation::partition_stage &&
					!stage_authority->store_failure_cause()->path,
				"Store-stage compact token lost its exact prepublication effects");

		auto report_ready =
			prepare_materialization_store(value, publication, store_plan(value, publication));
		require(report_ready.ready_for_publish(),
				"report-construction fixture did not reach readiness");
		auto report_journal = bound_journal(task_count);
		complete_worker_census(report_journal, task_count);
		require(report_journal.complete_transcript_validation().has_value() &&
					report_journal.complete_materialization_validation().has_value() &&
					report_journal.record_store_preparation(std::move(report_ready)).has_value() &&
					report_journal.complete_store_preparation().has_value(),
				"journal did not reach report construction");
		auto report_authority =
			std::move(report_journal)
				.issue_compact_failure(
					{"materialization.report-invalid", "report", "capacity-reservation-failed"});
		require(report_authority.has_value() &&
					report_authority->phase() == compact_failure_phase::report_construction &&
					report_authority->worker_launch_attempt_count() == task_count &&
					report_authority->worker_launch_success_count() == task_count &&
					report_authority->store_draft_state() == compact_store_draft_state::discarded &&
					report_authority->head_observation() == compact_head_observation::absent &&
					!report_authority->store_failure_cause(),
				"report-construction token did not discard the draft with exact effects");

		auto publication_ready =
			prepare_materialization_store(value, publication, store_plan(value, publication));
		require(publication_ready.ready_for_publish(),
				"publication fixture did not reach readiness");
		auto publication_journal = bound_journal(task_count);
		complete_worker_census(publication_journal, task_count);
		require(publication_journal.complete_transcript_validation().has_value() &&
					publication_journal.complete_materialization_validation().has_value() &&
					publication_journal.record_store_preparation(std::move(publication_ready))
						.has_value() &&
					publication_journal.complete_store_preparation().has_value(),
				"journal did not reach the final prepublication boundary");
		auto postpublication = std::move(publication_journal).begin_publication();
		require(postpublication.has_value() &&
					postpublication->store_observation().publication_attempted &&
					postpublication->store_observation().publish_call_count == 1U &&
					postpublication->store_observation().publish_returned_record,
				"journal did not cross exactly one irreversible publication boundary");
	}
} // namespace

int main(const int argument_count, const char* const* arguments)
{
	using namespace cxxlens::detail::clang22::materialization;
	if (argument_count == 2 && std::string_view{arguments[1]} == "--emit-head-error")
	{
		std::cout << failed_head_observation_is_four_state_and_path_bound();
		return 0;
	}
	if (argument_count == 2 && std::string_view{arguments[1]} == "--emit-store-open-error")
	{
		std::cout << typed_store_cause_preserves_exact_detail();
		return 0;
	}
	auto journal = materialization_execution_journal::begin(complete_input());
	require(journal.has_value() && journal->pass_input_limit().has_value(),
			"raw execution journal did not authenticate the input-limit boundary");
	auto authority = std::move(*journal).issue_compact_failure(
		{"materialization.request-invalid", "input", "strict-json-invalid"});
	require(authority.has_value(), "journal did not issue raw compact authority");
	auto raw = encode_compact_failure_report(*authority, "2026-07-20T12:34:56Z");
	require(raw.has_value(), "raw compact report was rejected");
	if (argument_count == 2 && std::string_view{arguments[1]} == "--emit-raw")
	{
		std::cout << *raw;
		return 0;
	}
	require(raw->ends_with('\n') &&
				raw->find("\"response_kind\":\"compact_failure\"") != std::string::npos &&
				raw->find("\"state\":\"raw-input-only\"") != std::string::npos &&
				raw->find("\"request\":null") != std::string::npos &&
				raw->find("\"store_failure_cause\":null") != std::string::npos,
			"raw compact report omitted a closed branch field");
	auto parsed = parse_json_object(raw->substr(0U, raw->size() - 1U));
	require(
		parsed.has_value() &&
			parsed->root().has_exact_members(std::array{std::string_view{"binding"},
														std::string_view{"effects"},
														std::string_view{"error"},
														std::string_view{"generated_at"},
														std::string_view{"process_exit_status"},
														std::string_view{"raw_input_observation"},
														std::string_view{"report_version"},
														std::string_view{"response_kind"},
														std::string_view{"result"},
														std::string_view{"schema"}}),
		"compact report is not one exact JSON object");

	require(!encode_compact_failure_report(*authority, "2026-02-30T12:34:56Z"),
			"compact report accepted a non-existent UTC date");

	input_limit_failure_is_phase_authentic();
	spool_failure_phases_are_closed();
	exact_worker_census_is_journal_owned();
	static_cast<void>(typed_store_cause_preserves_exact_detail());
	static_cast<void>(failed_head_observation_is_four_state_and_path_bound());
	store_stage_and_publication_boundary_are_closed();

	return 0;
}
