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
#include <fcntl.h>
#include <unistd.h>

#include "llvm/clang22/materialization_json.hpp"
#include "llvm/clang22/materialization_prior_artifact.hpp"
#include "llvm/clang22/materialization_public_report.hpp"
#include "llvm/clang22/materialization_rooted_vfs.hpp"
#include "sdk/provider_runtime_internal.hpp"

namespace cxxlens::detail::clang22::materialization
{
	class public_materialization_prepublication_projection_test_peer final
	{
	  public:
		[[nodiscard]] static public_materialization_prepublication_projection
		make(std::string binding_digest,
			 std::string request_digest,
			 std::string semantic_request_digest,
			 std::string occurrence_inventory_digest,
			 const std::uint64_t task_count,
			 const std::size_t reserved_bytes,
			 std::string capacity_proof_digest,
			 const bool issue_capability)
		{
			public_materialization_prepublication_projection projection{
				std::move(binding_digest),
				std::move(request_digest),
				std::move(semantic_request_digest),
				std::move(occurrence_inventory_digest),
				task_count,
				reserved_bytes,
				std::move(capacity_proof_digest)};
			if (issue_capability)
				projection.issue_capability();
			return projection;
		}
	};
} // namespace cxxlens::detail::clang22::materialization

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

	class artifact_transcript_sink final : public sdk::provider::frame_sink
	{
	  public:
		sdk::result<void> write(const std::span<const std::byte> bytes) override
		{
			transcript.insert(transcript.end(), bytes.begin(), bytes.end());
			return {};
		}

		std::vector<std::byte> transcript;
	};

	template <class Journal>
	concept compact_failure_capable = requires(Journal&& journal) {
		std::move(journal).issue_compact_failure(
			compact_report_error{"materialization.report-invalid", "report", "test"});
	};

	static_assert(!std::default_initializable<compact_failure_authority>);
	static_assert(!std::copy_constructible<compact_failure_authority>);
	static_assert(!std::is_copy_assignable_v<compact_failure_authority>);
	static_assert(std::move_constructible<compact_failure_authority>);
	static_assert(!std::default_initializable<materialization_postpublication_failure_authority>);
	static_assert(!std::copy_constructible<materialization_postpublication_failure_authority>);
	static_assert(std::move_constructible<materialization_postpublication_failure_authority>);
	static_assert(compact_failure_capable<materialization_execution_journal>);
	static_assert(!compact_failure_capable<materialization_postpublication_journal>);
	static_assert(!std::copy_constructible<detailed_task_report_replayable_spool>);
	static_assert(std::move_constructible<detailed_task_report_replayable_spool>);
	static_assert(!std::default_initializable<public_materialization_capacity_reservation>);
	static_assert(!std::copy_constructible<public_materialization_capacity_reservation>);
	static_assert(std::move_constructible<public_materialization_capacity_reservation>);
	static_assert(!std::copy_constructible<public_materialization_prepublication_projection>);
	static_assert(std::move_constructible<public_materialization_prepublication_projection>);
	static_assert(!std::constructible_from<public_materialization_prepublication_projection,
										   std::string,
										   std::string,
										   std::string,
										   std::string,
										   std::uint64_t,
										   std::size_t,
										   std::string>);

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

	struct occurrence_binding_fixture
	{
		materialization_v2_1_tool_authority tool;
		materialization_v2_1_worker_authority worker;
		materialization_occurrence_manifest manifest;
		materialization_occurrence_receipt receipt;
	};

	[[nodiscard]] occurrence_binding_fixture valid_occurrence_binding_fixture()
	{
		occurrence_binding_fixture fixture;
		fixture.tool.source_revision = "revision:source";
		fixture.tool.source_tree = "tree:source";
		fixture.tool.package_configuration = "static";
		fixture.tool.occurrence_manifest_digest = "sha256:" + std::string(64U, 'c');
		fixture.tool.installed_executable_digest = "sha256:" + std::string(64U, 'a');
		fixture.worker.installed_binary_digest = "sha256:" + std::string(64U, 'b');
		fixture.manifest.source_revision = fixture.tool.source_revision;
		fixture.manifest.source_tree = fixture.tool.source_tree;
		fixture.manifest.package_configuration = fixture.tool.package_configuration;
		fixture.manifest.occurrence_payload_digest = "sha256:" + std::string(64U, 'd');
		fixture.manifest.inventory_digest = "sha256:" + std::string(64U, 'e');
		fixture.manifest.files = {
			{"materializer-executable",
			 "bin/cxxlens-clang22-materialize",
			 fixture.tool.installed_executable_digest},
			{"worker-executable",
			 "bin/cxxlens-clang-worker-22",
			 fixture.worker.installed_binary_digest},
		};
		fixture.receipt.schema = "rooted-occurrence-v1";
		fixture.receipt.manifest_file_digest = fixture.tool.occurrence_manifest_digest;
		fixture.receipt.occurrence_payload_digest = fixture.manifest.occurrence_payload_digest;
		fixture.receipt.inventory_digest = fixture.manifest.inventory_digest;
		fixture.receipt.prefix_device_inode_observation_digest = "sha256:" + std::string(64U, 'f');
		fixture.receipt.files = {{fixture.manifest.files[0], {}}, {fixture.manifest.files[1], {}}};
		return fixture;
	}

	void public_report_occurrence_binding_rejects_forged_combinations()
	{
		const auto valid = valid_occurrence_binding_fixture();
		auto accepted = validate_materialization_public_report_occurrence_binding(
			valid.tool, valid.worker, valid.manifest, valid.receipt);
		require(accepted.has_value(), "valid request/occurrence binding was rejected");

		const auto reject = [&](const auto& forge, const std::string_view expected_field)
		{
			auto forged = valid;
			forge(forged);
			auto result = validate_materialization_public_report_occurrence_binding(
				forged.tool, forged.worker, forged.manifest, forged.receipt);
			require(!result && result.error().code == "materialization.report-invalid" &&
						result.error().field == expected_field,
					"forged request/occurrence combination was accepted for " +
						std::string{expected_field} +
						(result ? ": success"
								: ": " + result.error().field + "/" + result.error().detail));
		};

		reject(
			[](auto& value)
			{
				value.tool.source_revision = "revision:forged";
			},
			"installation.source_revision");
		reject(
			[](auto& value)
			{
				value.tool.source_tree = "tree:forged";
			},
			"installation.source_tree");
		reject(
			[](auto& value)
			{
				value.tool.package_configuration = "shared";
			},
			"installation.configuration");
		reject(
			[](auto& value)
			{
				value.tool.occurrence_manifest_digest = "sha256:" + std::string(64U, '1');
			},
			"installation.occurrence_manifest_digest");
		reject(
			[](auto& value)
			{
				value.tool.installed_executable_digest = "sha256:" + std::string(64U, '1');
			},
			"installation.materializer");
		reject(
			[](auto& value)
			{
				value.worker.installed_binary_digest = "sha256:" + std::string(64U, '1');
			},
			"installation.worker");
		reject(
			[](auto& value)
			{
				value.manifest.files[0].digest = "sha256:" + std::string(64U, '1');
			},
			"installation.measured.files");
		reject(
			[](auto& value)
			{
				value.receipt.files[1].authority.digest = "sha256:" + std::string(64U, '1');
			},
			"installation.measured.files");
	}

	[[nodiscard]] std::string provider_execution_id_fixture(const std::string_view task_id)
	{
		return "provider-execution:" +
			sdk::content_digest(std::as_bytes(std::span{task_id.data(), task_id.size()}));
	}

	[[nodiscard]] std::string
	sealed_receipt_digest_for_capture(const detailed_task_report_capture& capture)
	{
		std::vector<sdk::provider::detail::provider_sealed_transcript_batch_receipt_projection>
			batches;
		batches.reserve(capture.batches.size());
		for (const auto& batch : capture.batches)
		{
			sdk::provider::detail::provider_sealed_transcript_batch_receipt_projection projection;
			projection.task_id = batch.task_id;
			projection.descriptor_id = batch.descriptor_id;
			projection.descriptor_digest = batch.descriptor_digest;
			projection.dependency_group_id = batch.dependency_group_id;
			projection.atomic_output_group_id = batch.atomic_output_group_id;
			projection.batch_id = batch.batch_id;
			projection.batch_digest = batch.batch_digest;
			projection.ordered_chunk_digests = batch.ordered_chunk_digests;
			for (const auto& row : batch.rows)
				projection.row_canonical_forms.push_back(row.row_canonical_form);
			batches.push_back(std::move(projection));
		}
		std::vector<sdk::provider::coverage_unit> coverage;
		coverage.reserve(capture.coverage.size());
		for (const auto& value : capture.coverage)
			coverage.push_back({value.kind, value.id, value.state, value.reason});
		std::vector<sdk::provider::unresolved_item> unresolved;
		unresolved.reserve(capture.unresolved.size());
		for (const auto& value : capture.unresolved)
			unresolved.push_back({value.code, value.subject, value.detail});
		std::vector<sdk::provider::evidence_item> evidence;
		evidence.reserve(capture.evidence.size());
		for (const auto& value : capture.evidence)
			evidence.push_back({value.kind, value.subject, value.producer, value.summary});
		auto digest = sdk::provider::detail::provider_sealed_transcript_receipt_digest(
			capture.provider_task_id, "provider.success", batches, coverage, unresolved, evidence);
		require(digest.has_value(), "sealed report leaf fixture receipt derivation failed");
		return *digest;
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
			require(journal.record_task_attempt().has_value() &&
						journal.record_worker_launch_attempt().has_value() &&
						journal.record_worker_launch_success().has_value() &&
						journal.record_task_success().has_value(),
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
		require(journal.record_task_attempt().has_value() &&
					journal.record_worker_launch_attempt().has_value() &&
					journal.record_worker_launch_success().has_value() &&
					journal.record_task_success().has_value() &&
					!journal.complete_worker_launches(),
				"journal accepted an incomplete worker census");
		require(journal.record_task_attempt().has_value() &&
					journal.record_worker_launch_attempt().has_value() &&
					journal.record_worker_launch_success().has_value() &&
					journal.record_task_success().has_value() &&
					journal.record_task_attempt().has_value() &&
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
		require(required_unsigned(effects, "task_attempt_count") == 3U &&
					required_unsigned(effects, "task_success_count") == 2U &&
					required_unsigned(effects, "worker_launch_attempt_count") == 3U &&
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

	void coordinator_failure_must_use_worker_phase_code()
	{
		auto journal = bound_journal(1U);
		require(journal.record_task_attempt().has_value() &&
					journal.record_worker_launch_attempt().has_value(),
				"coordinator failure journal did not open the worker launch window");
		auto unmapped = std::move(journal).issue_compact_failure(
			{"materialization.incremental-invalid", "worker", "receipt-validation"});
		require(!unmapped,
				"journal accepted the coordinator's generic incremental failure in worker phase");
		auto mapped = std::move(journal).issue_compact_failure(
			{"materialization.worker-failure",
			 "worker",
			 "source-code=materialization.coverage-incomplete;source-field=provider.coverage;"
			 "source-detail=sealed-transcript-mismatch"});
		require(mapped && mapped->error().code == "materialization.worker-failure" &&
					mapped->error().diagnostic.find("materialization.coverage-incomplete") !=
						std::string::npos,
				"journal did not retain a typed coordinator failure under its allowed worker code");
	}

	void exact_reuse_is_not_a_worker_launch()
	{
		auto journal = bound_journal(1U);
		require(journal.record_task_attempt().has_value() &&
					journal.record_task_success().has_value() &&
					journal.complete_worker_launches().has_value(),
				"journal rejected the zero-frontend-execution reuse window");
		auto authority = std::move(journal).issue_compact_failure(
			{"materialization.transcript-invalid", "transcript", "reuse-receipt"});
		require(authority.has_value() && authority->task_attempt_count() == 1U &&
					authority->task_success_count() == 1U &&
					authority->worker_launch_attempt_count() == 0U &&
					authority->worker_launch_success_count() == 0U,
				"reuse journal conflated task windows with frontend launches");
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

	void postpublication_failure_is_recovery_only_and_never_compact()
	{
		const auto value = engine();
		const auto publication = publication_request(selector(value));
		auto prepared =
			prepare_materialization_store(value, publication, store_plan(value, publication));
		require(prepared.ready_for_publish(),
				"postpublication failure fixture was not publishable");
		auto journal = bound_journal(1U);
		complete_worker_census(journal, 1U);
		require(journal.complete_transcript_validation().has_value() &&
					journal.complete_materialization_validation().has_value() &&
					journal.record_store_preparation(std::move(prepared)).has_value() &&
					journal.complete_store_preparation().has_value(),
				"postpublication failure journal did not reach the boundary");
		auto postpublication = std::move(journal).begin_publication();
		require(postpublication && postpublication->store_observation().publication_attempted &&
					postpublication->store_observation().publish_call_count == 1U,
				"postpublication failure fixture did not attempt exactly one publish");

		auto invalid_phase =
			std::move(*postpublication)
				.issue_no_response_failure(
					static_cast<materialization_postpublication_failure_phase>(255U),
					{"materialization.report-invalid", "report", "invalid-phase"});
		require(!invalid_phase &&
					invalid_phase.error().code == "materialization.execution-journal-invalid",
				"postpublication failure accepted an unknown phase");

		auto invalid = std::move(*postpublication)
						   .issue_no_response_failure(
							   materialization_postpublication_failure_phase::report_validation,
							   {"", "report", "invalid-code"});
		require(!invalid && invalid.error().code == "materialization.execution-journal-invalid",
				"postpublication failure accepted an untyped error");

		auto failure = std::move(*postpublication)
						   .issue_no_response_failure(
							   materialization_postpublication_failure_phase::report_validation,
							   {"materialization.report-invalid", "report", "schema-validation"});
		require(
			failure && failure->valid() &&
				failure->phase() ==
					materialization_postpublication_failure_phase::report_validation &&
				failure->error() ==
					sdk::error{"materialization.report-invalid", "report", "schema-validation"} &&
				failure->store_observation().publication_attempted &&
				failure->store_observation().publish_call_count == 1U &&
				failure->store_observation().publish_returned_record &&
				failure->recovery_authority() ==
					materialization_postpublication_recovery_authority::committed_record_only &&
				failure->process_exit_status() == 2 && !failure->response_authoritative() &&
				!failure->compact_downgrade_allowed(),
			"postpublication failure lost the committed-record-only/no-response contract");

		auto repeated = std::move(*postpublication)
							.issue_no_response_failure(
								materialization_postpublication_failure_phase::stdout_transport,
								{"materialization.report-invalid", "stdout", "repeated"});
		require(!repeated && repeated.error().code == "materialization.execution-journal-invalid" &&
					repeated.error().detail == "consumed-journal",
				"postpublication journal issued a second recovery authority");
	}

	void bounded_detailed_projection_never_promotes_unverified_store()
	{
		detailed_success_report_model model;
		model.generated_at = "2026-08-10T12:34:56Z";
		model.store.backend = "memory";
		model.store.series_id = "series:test";
		model.store.selector_id = "selector:test";
		model.store.published_record = detailed_publication_projection{
			"publication:test", "series:test", "snapshot:test", 1U, 1U, std::nullopt};
		model.store.candidate_identity = detailed_publication_projection{
			"publication:test", "series:test", "snapshot:test", 1U, 0U, std::nullopt};
		model.store.verification = {
			{"current-selector", "present", std::nullopt, std::nullopt},
			{"open-publication", "present", std::nullopt, std::nullopt},
			{"open-snapshot", "present", std::nullopt, std::nullopt},
		};
		model.store.prior_history_retained = true;
		model.store.verified = false;

		detailed_task_report_capture task;
		task.provider_task_id = "task:test";
		task.provider_execution_id = provider_execution_id_fixture(task.provider_task_id);
		task.selected_catalog_compile_unit_id = "compile-unit:selected";
		task.compile_unit_id = "compile-unit:final";
		task.task_input_digest =
			"sha256:1111111111111111111111111111111111111111111111111111111111111111";
		task.input_protocol_major = 1U;
		task.input_protocol_minor = 1U;
		task.logical_input_bytes = 1U;
		task.canonical_chunk_bytes = 1U;
		task.input_chunk_count = 0U;
		task.ordered_chunk_payload_digest_set_digest =
			"sha256:2222222222222222222222222222222222222222222222222222222222222222";
		task.raw_frame_stream_bytes = 1U;
		task.raw_frame_stream_digest =
			"sha256:3333333333333333333333333333333333333333333333333333333333333333";
		task.frame_count = 1U;
		task.frame_transcript_digest =
			"semantic-v2:sha256:4444444444444444444444444444444444444444444444444444444444444444";
		task.sealed_transcript_digest =
			"semantic-v2:sha256:5555555555555555555555555555555555555555555555555555555555555555";
		task.batches.push_back(
			{"task:test",
			 "descriptor:test",
			 "sha256:6666666666666666666666666666666666666666666666666666666666666666",
			 "dependency:test",
			 "atomic:test",
			 "batch:test",
			 "sha256:7777777777777777777777777777777777777777777777777777777777777777",
			 {},
			 {},
			 0U,
			 "sha256:8888888888888888888888888888888888888888888888888888888888888888",
			 {}});
		model.tasks.push_back(std::move(task));

		auto rejected = encode_detailed_success_report(model);
		require(!rejected &&
					rejected.error() ==
						sdk::error{"materialization.report-invalid",
								   "publication",
								   "publication-unverified:committed-verified-required"},
				"bounded detailed projection promoted an unverified Store observation");
	}

	void task_evidence_accumulator_is_bounded()
	{
		detailed_task_report_capture capture;
		capture.provider_task_id = "task:test";
		capture.provider_execution_id = provider_execution_id_fixture(capture.provider_task_id);

		detailed_report_limits byte_limits;
		byte_limits.max_projection_bytes = 128U;
		detailed_task_report_accumulator byte_limited{byte_limits};
		auto rejected = byte_limited.append(capture);
		require(!rejected &&
					rejected.error() ==
						sdk::error{"materialization.report-invalid",
								   "task_results",
								   "limit-exceeded:projection-bytes"},
				"task evidence accumulator ignored its byte limit");

		detailed_report_limits count_limits;
		count_limits.max_tasks = 1U;
		count_limits.max_projection_bytes = 4096U;
		detailed_task_report_accumulator count_limited{count_limits};
		require(count_limited.append(capture).has_value() && count_limited.tasks().size() == 1U,
				"task evidence accumulator rejected its first bounded task");
		auto count_rejected = count_limited.append(capture);
		require(!count_rejected &&
					count_rejected.error() ==
						sdk::error{"materialization.report-invalid",
								   "task_results",
								   "limit-exceeded:count"},
				"task evidence accumulator exceeded its task limit");
	}

	[[nodiscard]] detailed_task_report_capture replayable_capture(std::string task_id)
	{
		detailed_task_report_capture capture;
		capture.provider_task_id = task_id;
		capture.provider_execution_id = provider_execution_id_fixture(task_id);
		capture.project_id = "project:test";
		capture.catalog_id = "catalog:test";
		capture.catalog_digest =
			"sha256:1111111111111111111111111111111111111111111111111111111111111111";
		capture.selected_catalog_compile_unit_id = "compile-unit:selected";
		capture.compile_unit_id = "compile-unit:final";
		capture.variant_id = "variant:test";
		capture.toolchain_context_id = "toolchain:test";
		capture.toolchain_digest =
			"sha256:2222222222222222222222222222222222222222222222222222222222222222";
		capture.source_snapshot_id = "source-snapshot:test";
		capture.source_file_id = "file:test";
		capture.source_logical_path = "project://test.cpp";
		capture.source_content_digest =
			"sha256:3333333333333333333333333333333333333333333333333333333333333333";
		capture.source_size_bytes = 42U;
		capture.source_encoding = "utf8";
		capture.source_line_index_id = "line-index:test";
		capture.source_read_only = true;
		capture.task_input_digest =
			"sha256:4444444444444444444444444444444444444444444444444444444444444444";
		capture.condition_universe_id = "condition-universe:test";
		capture.condition_id = "condition:test";
		capture.interpretation_domain = "cc.clang22-canonical-1";
		capture.input_protocol_major = 1U;
		capture.input_protocol_minor = 1U;
		capture.logical_input_bytes = 42U;
		capture.canonical_chunk_bytes = 42U;
		capture.input_chunk_count = 1U;
		capture.ordered_chunk_digests = {
			"sha256:5555555555555555555555555555555555555555555555555555555555555555"};
		capture.ordered_chunk_payload_digest_set_digest =
			"sha256:6666666666666666666666666666666666666666666666666666666666666666";
		capture.raw_frame_stream = {std::byte{'r'}, std::byte{'a'}, std::byte{'w'}};
		capture.raw_frame_stream_bytes = capture.raw_frame_stream.size();
		capture.raw_frame_stream_digest = sdk::content_digest(capture.raw_frame_stream);
		capture.frame_count = 1U;
		capture.frame_transcript_digest =
			"semantic-v2:sha256:8888888888888888888888888888888888888888888888888888888888888888";
		capture.coverage.push_back({"canonical", "coverage:test", "complete", ""});
		capture.unresolved.push_back({"provider.unavailable", task_id, "observation unavailable"});
		capture.evidence.push_back({"provider", task_id, "clang22", "sealed"});
		detailed_provider_batch_projection batch;
		batch.task_id = task_id;
		batch.descriptor_id = "cc.entity.v1";
		batch.descriptor_digest =
			"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
		batch.dependency_group_id = "canonical";
		batch.atomic_output_group_id = "clang22-atomic";
		batch.batch_id = "cc.entity.v1-batch";
		// Provider batch summaries retain descriptor order, which is not required to be
		// lexical order (cc.entity starts with entity before canonicalization).
		batch.columns = {{"cc.entity.v1.entity", 42U, 1U},
						 {"cc.entity.v1.canonicalization", 42U, 1U}};
		batch.ordered_chunk_digests = {
			"sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
			"sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"};
		batch.row_count = 1U;
		const std::string row_form{"{\"row\":\"" + task_id + "\"}"};
		const auto row_digest =
			sdk::content_digest(std::as_bytes(std::span{row_form.data(), row_form.size()}));
		batch.rows.push_back({0U, row_form, row_digest});
		const auto row_set_digest = sdk::semantic_digest(
			"cxxlens.clang22.materialization-report.row-set.v1", "0:" + row_form + "\n");
		require(row_set_digest.has_value(), "report capture fixture row-set digest failed");
		batch.row_set_digest = *row_set_digest;
		batch.batch_digest = sdk::provider::columnar_batch_digest({batch.task_id,
																   batch.dependency_group_id,
																   batch.atomic_output_group_id,
																   batch.batch_id,
																   batch.descriptor_id,
																   batch.descriptor_digest,
																   batch.row_count,
																   batch.columns,
																   batch.ordered_chunk_digests,
																   {}});
		capture.batches.push_back(std::move(batch));
		capture.sealed_transcript_digest = sealed_receipt_digest_for_capture(capture);
		auto span_id = sdk::source_span_identity("snapshot:test", "file:test", 1U, 2U, "expansion");
		require(span_id.has_value(), "report capture fixture span identity failed");
		capture.observation_rows.push_back(
			{0U,
			 0U,
			 row_digest,
			 false,
			 std::string{"provider-unavailable"},
			 observation_v2_primary_span{
				 *span_id, "snapshot:test", "file:test", 1U, 2U, "expansion", true}});
		sdk::detached_row row;
		row.descriptor_id = "cc.entity.v1";
		row.cells.emplace("boolean", sdk::detached_cell::boolean(true));
		row.cells.emplace("bytes", sdk::detached_cell::bytes({std::byte{0x01}, std::byte{0x02}}));
		row.cells.emplace("optional",
						  sdk::detached_cell::absent({sdk::scalar_kind::utf8_string, {}, true}));
		row.cells.emplace("unknown",
						  sdk::detached_cell::unknown({sdk::scalar_kind::utf8_string, {}, true},
													  "provider-unavailable"));
		capture.base_claim_rows.push_back(std::move(row));
		return capture;
	}

	void report_capture_spool_replays_one_task_at_a_time()
	{
		detailed_report_limits limits;
		limits.max_projection_bytes = 1024U * 1024U;
		auto created = detailed_task_report_replayable_spool::create(limits);
		require(created.has_value(), "report capture spool could not be created");
		auto spool = std::move(*created);
		require(!spool.sealed() && spool.task_count() == 0U && spool.spooled_bytes() == 0U,
				"report capture spool did not start empty and unsealed");

		require(spool.append(replayable_capture("task:one")).has_value(),
				"report capture spool rejected its first task");
		require(spool.append(replayable_capture("task:two")).has_value(),
				"report capture spool rejected its second task");
		const auto bytes_before_seal = spool.spooled_bytes();
		require(spool.task_count() == 2U && bytes_before_seal > 0U,
				"report capture spool did not retain bounded record metadata");

		const auto consume = [&](detailed_task_report_capture&& capture) -> sdk::result<void>
		{
			require(capture.base_claim_rows.size() == 1U && capture.batches.size() == 1U &&
						capture.observation_rows.size() == 1U,
					"report capture replay dropped a nested value-owned field");
			require(capture.base_claim_rows.front().cells.at("boolean").canonical_form() ==
							sdk::detached_cell::boolean(true).canonical_form() &&
						capture.base_claim_rows.front().cells.at("optional").state ==
							sdk::cell_state::absent &&
						capture.base_claim_rows.front().cells.at("unknown").unknown_reason ==
							std::optional<std::string>{"provider-unavailable"},
					"report capture replay changed detached-cell semantics");
			require(capture.observation_rows.front().primary_span &&
						capture.observation_rows.front().primary_span->begin == 1U &&
						capture.observation_rows.front().primary_span->read_only,
					"report capture replay changed observation-span semantics");
			return {};
		};

		auto before_seal = spool.replay(consume);
		require(!before_seal &&
					before_seal.error() ==
						sdk::error{"materialization.report-invalid",
								   "task_spool",
								   "spool-io:replay-lifecycle"},
				"report capture spool replayed before sealing");
		require(spool.seal().has_value() && spool.sealed(),
				"report capture spool could not be sealed");
		require(spool.replay(consume).has_value() && spool.replay(consume).has_value(),
				"report capture spool was not independently replayable twice");
		require(spool.spooled_bytes() == bytes_before_seal,
				"report capture replay changed the sealed spool");
		auto append_after_seal = spool.append(replayable_capture("task:three"));
		require(!append_after_seal &&
					append_after_seal.error() ==
						sdk::error{
							"materialization.report-invalid", "task_spool", "spool-io:lifecycle"},
				"report capture spool accepted a post-seal append");
	}

	[[nodiscard]] sdk::incremental::partition_state prior_artifact_state()
	{
		const auto digest = [](const char value)
		{
			return std::string{"sha256:"} + std::string(64U, value);
		};
		sdk::incremental::input_fingerprint input{digest('a'),
												  digest('b'),
												  digest('c'),
												  digest('d'),
												  digest('e'),
												  digest('f'),
												  digest('1'),
												  digest('2'),
												  digest('3'),
												  digest('4'),
												  digest('5'),
												  digest('6'),
												  digest('7'),
												  digest('8'),
												  "normalizer:v1",
												  digest('9'),
												  digest('a'),
												  "precision:exact"};
		return {"partition:artifact-test", std::move(input), digest('b'), digest('c'), false};
	}

	void prior_artifact_codec_is_canonical_and_bounded()
	{
		auto built_engine = engine();
		auto bundle_selector = selector(built_engine);
		materialization_prior_artifact_bundle bundle;
		bundle.selector = bundle_selector;
		bundle.series_id = bundle_selector.id();
		bundle.publication_id = "publication:artifact-parent";
		bundle.snapshot_id = "snapshot:artifact-parent";
		bundle.sequence = 7U;
		bundle.physical_generation = 11U;
		bundle.parent_publication = "publication:older";
		const auto capture = replayable_capture("task:artifact");
		auto capture_bytes = encode_detailed_task_report_capture(capture);
		require(capture_bytes.has_value(),
				"prior artifact fixture capture was not canonicalizable");
		auto canonical_capture = decode_detailed_task_report_capture(*capture_bytes);
		require(canonical_capture.has_value(), "prior artifact fixture capture did not rehydrate");
		auto state = prior_artifact_state();
		auto state_valid = state.validate();
		require(state_valid.has_value(),
				"prior artifact fixture state is invalid" +
					(state_valid ? std::string{}
								 : " (" + state_valid.error().field + ":" +
							 state_valid.error().detail + ")"));
		bundle.tasks.push_back({{0U,
								 capture.provider_task_id,
								 capture.task_input_digest,
								 capture.selected_catalog_compile_unit_id,
								 capture.compile_unit_id},
								std::move(state),
								"materialization.incremental-sealed-artifact:sha256:"
								"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
								std::move(*canonical_capture)});

		auto encoded = encode_materialization_prior_artifact(bundle);
		require(encoded.has_value() && !encoded->empty(),
				"prior artifact codec rejected the bounded fixture" +
					(encoded ? std::string{}
							 : " (" + encoded.error().field + ":" + encoded.error().detail + ")"));
		auto decoded = decode_materialization_prior_artifact(*encoded);
		require(decoded.has_value() && decoded->tasks.size() == 1U &&
					decoded->tasks.front().identity.canonical_task_ordinal == 0U &&
					decoded->tasks.front().capture.provider_task_id == "task:artifact" &&
					decoded->tasks.front().capture.batches.front().columns.size() == 2U &&
					decoded->tasks.front().capture.batches.front().columns.front().column_id ==
						"cc.entity.v1.entity" &&
					decoded->tasks.front().capture.batches.front().columns.back().column_id ==
						"cc.entity.v1.canonicalization",
				"prior artifact codec did not restore the exact task tuple");
		require(decoded.has_value() && *decoded == bundle,
				"prior artifact structural equality did not preserve the decoded capture");
		auto reencoded = encode_materialization_prior_artifact(*decoded);
		require(reencoded.has_value() && *reencoded == *encoded,
				"prior artifact codec did not preserve canonical bytes");
		auto mismatched_capture = capture;
		mismatched_capture.capture_binding_digest = "semantic-v2:sha256:" + std::string(64U, '0');
		auto mismatched_capture_encoding = encode_detailed_task_report_capture(mismatched_capture);
		require(!mismatched_capture_encoding &&
					mismatched_capture_encoding.error() ==
						sdk::error{"materialization.report-invalid",
								   "task_spool.capture-binding",
								   "transcript-mismatch:mismatch"},
				"task capture encoder accepted a cross-bound capture digest");
		auto semantically_mismatched_capture = capture;
		semantically_mismatched_capture.batches.front().batch_digest =
			"sha256:" + std::string(64U, '0');
		auto semantically_mismatched_encoding =
			encode_detailed_task_report_capture(semantically_mismatched_capture);
		require(!semantically_mismatched_encoding &&
					semantically_mismatched_encoding.error().field == "task_spool.batch" &&
					semantically_mismatched_encoding.error().detail ==
						"invalid-capture:spool-corrupt:batch-digest",
				"task capture encoder accepted a semantically inconsistent batch digest");
		auto duplicate_column_capture = capture;
		duplicate_column_capture.batches.front().columns[1U].column_id =
			duplicate_column_capture.batches.front().columns.front().column_id;
		auto duplicate_column_encoding =
			encode_detailed_task_report_capture(duplicate_column_capture);
		require(!duplicate_column_encoding &&
					duplicate_column_encoding.error().field == "task_spool.batch.columns" &&
					duplicate_column_encoding.error().detail ==
						"invalid-capture:spool-corrupt:nonempty-or-duplicate",
				"task capture encoder accepted duplicate batch columns");
		auto empty_column_capture = capture;
		empty_column_capture.batches.front().columns[1U].column_id.clear();
		auto empty_column_encoding = encode_detailed_task_report_capture(empty_column_capture);
		require(!empty_column_encoding &&
					empty_column_encoding.error().field == "task_spool.batch.columns" &&
					empty_column_encoding.error().detail ==
						"invalid-capture:spool-corrupt:nonempty-or-duplicate",
				"task capture encoder accepted an empty batch column ID");
		auto invalid_runtime_capture = capture;
		invalid_runtime_capture.provider_execution_id.clear();
		auto invalid_runtime_capture_bytes =
			encode_detailed_task_report_capture(invalid_runtime_capture);
		require(invalid_runtime_capture_bytes.has_value(),
				"runtime-receipt decoder fixture could not encode its source spool");
		auto forged_envelope = sdk::canonical_binary_decode(*encoded);
		require(forged_envelope.has_value() && forged_envelope->tuple.size() == 3U,
				"runtime-receipt decoder fixture envelope was not canonical");
		auto forged_body = sdk::canonical_binary_decode(forged_envelope->tuple[1U].byte_string);
		require(forged_body.has_value() && forged_body->tuple.size() == 5U &&
					forged_body->tuple[4U].tuple.size() == 1U,
				"runtime-receipt decoder fixture body was not canonical");
		forged_body->tuple[4U].tuple[0U].tuple[3U].byte_string = *invalid_runtime_capture_bytes;
		auto forged_body_bytes = sdk::canonical_binary(*forged_body);
		require(forged_body_bytes.has_value(),
				"runtime-receipt decoder fixture body could not be re-encoded");
		forged_envelope->tuple[1U].byte_string = *forged_body_bytes;
		forged_envelope->tuple[2U].text = sdk::content_digest(*forged_body_bytes);
		auto forged_bytes = sdk::canonical_binary(*forged_envelope);
		require(forged_bytes.has_value(),
				"runtime-receipt decoder fixture envelope could not be re-encoded");
		auto runtime_rejected = decode_materialization_prior_artifact(*forged_bytes);
		require(!runtime_rejected &&
					runtime_rejected.error() ==
						sdk::error{"materialization.incremental-artifact-invalid",
								   "task.capture",
								   "runtime-receipt"},
				"artifact decoder admitted a missing provider runtime receipt");

		auto corrupted = *encoded;
		corrupted[corrupted.size() / 2U] = static_cast<std::byte>(
			static_cast<unsigned char>(corrupted[corrupted.size() / 2U]) ^ 1U);
		require(!decode_materialization_prior_artifact(corrupted),
				"prior artifact codec accepted a corrupted envelope");
		auto trailing = *encoded;
		trailing.push_back(std::byte{0});
		require(!decode_materialization_prior_artifact(trailing),
				"prior artifact codec accepted trailing bytes");
		materialization_prior_artifact_limits limited;
		limited.max_tasks = 1U;
		require(decode_materialization_prior_artifact(*encoded, limited).has_value(),
				"prior artifact codec confused a task upper bound with an exact count");
		limited.max_total_capture_bytes = 1U;
		require(!encode_materialization_prior_artifact(bundle, limited),
				"prior artifact codec ignored its aggregate capture budget");
	}

	void sqlite_prior_artifact_sidecar_is_selector_bound()
	{
		temporary_working_directory directory;
		auto root = materialization_effect_root::capture_startup();
		require(root.has_value(), "SQLite prior artifact test could not capture the effect root");
		auto built_engine = engine();
		auto bundle_selector = selector(built_engine);
		auto publication = publication_request(bundle_selector);
		publication.backend = "sqlite";
		publication.genesis = false;
		publication.expected_parent_publication = "publication:artifact-grandparent";
		publication.sqlite_path = "store.db";
		const auto capture = replayable_capture("task:memory-artifact");
		auto state = prior_artifact_state();
		materialization_prior_artifact_task task{
			{0U,
			 capture.provider_task_id,
			 capture.task_input_digest,
			 capture.selected_catalog_compile_unit_id,
			 capture.compile_unit_id},
			std::move(state),
			"materialization.incremental-sealed-artifact:sha256:"
			"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
			capture};
		sdk::publication_record record;
		record.publication_id = "publication:memory-artifact";
		record.series_id = publication.series_id;
		record.snapshot_id = "snapshot:memory-artifact";
		record.sequence = 1U;
		record.physical_generation = 1U;
		record.parent_publication = publication.expected_parent_publication;
		record.state = sdk::publication_state::committed;
		materialization_store_observation observation;
		observation.backend = publication.backend;
		observation.selector = publication.selector;
		observation.series_id = publication.series_id;
		observation.expected_parent_publication = publication.expected_parent_publication;
		observation.writer_begin_call_count = 1U;
		observation.publication_attempted = true;
		observation.publish_call_count = 1U;
		observation.candidate_manifest = sdk::snapshot_manifest{};
		observation.candidate_manifest->id = record.snapshot_id;
		observation.candidate_identity =
			materialization_publication_candidate{record.publication_id,
												  record.series_id,
												  record.snapshot_id,
												  record.sequence,
												  record.parent_publication};
		observation.publish_returned_record = record;
		auto capture_spool_result = detailed_task_report_replayable_spool::create();
		require(capture_spool_result.has_value(),
				"SQLite prior artifact capture spool could not be created");
		auto capture_spool = std::move(*capture_spool_result);
		require(capture_spool.append(capture).has_value() && capture_spool.seal().has_value(),
				"SQLite prior artifact capture spool could not be sealed");
		materialization_prior_artifact_task_metadata task_metadata{
			task.identity, task.state, task.sealed_artifact_digest, capture.provider_execution_id};
		auto persisted = persist_materialization_prior_artifact(
			*root, publication, record, observation, capture_spool, {std::move(task_metadata)});
		require(persisted.has_value(), "SQLite prior artifact was not retained after commit");
		auto weak_execution_metadata = materialization_prior_artifact_task_metadata{
			task.identity, task.state, task.sealed_artifact_digest, "execution:weak"};
		auto weak_execution =
			persist_materialization_prior_artifact(*root,
												   publication,
												   record,
												   observation,
												   capture_spool,
												   {std::move(weak_execution_metadata)});
		require(!weak_execution &&
					weak_execution.error() ==
						sdk::error{"materialization.incremental-artifact-invalid",
								   "bundle.tasks",
								   "identity-or-order"},
				"SQLite prior artifact admitted a non-canonical provider execution ID");
		auto locator_digest = sdk::content_digest(
			std::as_bytes(std::span{record.publication_id.data(), record.publication_id.size()}));
		require(locator_digest.starts_with("sha256:"),
				"SQLite prior artifact locator digest invalid");
		const auto sidecar_path =
			std::string{"store.db.cxxlens-incremental-v1-"} + locator_digest.substr(7U);
		auto sidecar = root->open_beneath(sidecar_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		require(sidecar.has_value(), "SQLite prior artifact sidecar was not installed");
		auto identity = materialization_fd_identity(sidecar->get(), true);
		require(identity.has_value() && identity->size_bytes != 0U,
				"SQLite prior artifact sidecar has no stable size");
		std::vector<std::byte> sidecar_bytes(static_cast<std::size_t>(identity->size_bytes));
		std::size_t offset{};
		while (offset < sidecar_bytes.size())
		{
			const auto count = ::read(
				sidecar->get(), sidecar_bytes.data() + offset, sidecar_bytes.size() - offset);
			require(count > 0, "SQLite prior artifact sidecar could not be read");
			offset += static_cast<std::size_t>(count);
		}
		materialization_prior_artifact_bundle expected_bundle;
		expected_bundle.selector = publication.selector;
		expected_bundle.series_id = record.series_id;
		expected_bundle.publication_id = record.publication_id;
		expected_bundle.snapshot_id = record.snapshot_id;
		expected_bundle.sequence = record.sequence;
		expected_bundle.physical_generation = record.physical_generation;
		expected_bundle.parent_publication = record.parent_publication;
		expected_bundle.tasks.push_back(task);
		auto expected_bytes = encode_materialization_prior_artifact(expected_bundle);
		std::size_t first_difference{};
		while (first_difference < sidecar_bytes.size() &&
			   first_difference < (expected_bytes ? expected_bytes->size() : 0U) &&
			   sidecar_bytes[first_difference] == (*expected_bytes)[first_difference])
			++first_difference;
		require(expected_bytes.has_value() && sidecar_bytes == *expected_bytes,
				"streamed SQLite prior artifact differs from canonical vector encoding" +
					(expected_bytes ? " (size=" + std::to_string(sidecar_bytes.size()) + "/" +
							 std::to_string(expected_bytes->size()) + ")"
									: " (canonical encoding failed)") +
					(first_difference < sidecar_bytes.size()
						 ? " first=" + std::to_string(first_difference)
						 : std::string{}));
		auto loaded = decode_materialization_prior_artifact(sidecar_bytes);
		require(loaded.has_value() && loaded->publication_id == record.publication_id &&
					loaded->tasks.size() == 1U,
				"SQLite prior artifact sidecar did not restore the exact publication" +
					(loaded ? std::string{}
							: " (" + loaded.error().field + ":" + loaded.error().detail + ")"));
		auto other_selector = bundle_selector;
		other_selector.channel_id = "other-channel";
		auto other_publication = publication;
		other_publication.selector = other_selector;
		other_publication.series_id = other_selector.id();
		auto other_record = record;
		other_record.series_id = other_publication.series_id;
		materialization_store_observation other_observation = observation;
		other_observation.selector = other_publication.selector;
		other_observation.series_id = other_publication.series_id;
		other_observation.candidate_manifest->id = other_record.snapshot_id;
		other_observation.candidate_identity->series_id = other_record.series_id;
		other_observation.publish_returned_record = other_record;
		materialization_prior_artifact_task_metadata other_task_metadata{
			task.identity, task.state, task.sealed_artifact_digest, capture.provider_execution_id};
		auto selector_conflict =
			persist_materialization_prior_artifact(*root,
												   other_publication,
												   other_record,
												   other_observation,
												   capture_spool,
												   {std::move(other_task_metadata)});
		require(!selector_conflict &&
					selector_conflict.error() ==
						sdk::error{"materialization.incremental-artifact-invalid",
								   "sidecar",
								   "immutable-conflict"},
				"SQLite prior artifact sidecar crossed an unrelated selector boundary");
		auto memory_publication = publication_request(bundle_selector);
		auto memory_loaded =
			load_materialization_prior_artifact(*root, built_engine, memory_publication);
		require(memory_loaded.has_value() && !memory_loaded->has_value(),
				"memory prior artifact API exposed a process-local cache as durable reuse");
	}

	void sqlite_prior_artifact_loads_after_store_close()
	{
		temporary_working_directory directory;
		auto root = materialization_effect_root::capture_startup();
		require(root.has_value(), "SQLite close/reopen test could not capture the effect root");
		auto built_engine = engine();
		auto bundle_selector = selector(built_engine);
		auto parent_request = publication_request(bundle_selector);
		parent_request.backend = "sqlite";
		parent_request.sqlite_path = "store.db";

		auto capture = replayable_capture("task:sqlite-reopen");
		auto state = prior_artifact_state();
		materialization_prior_artifact_task task{
			{0U,
			 capture.provider_task_id,
			 capture.task_input_digest,
			 capture.selected_catalog_compile_unit_id,
			 capture.compile_unit_id},
			std::move(state),
			"materialization.incremental-sealed-artifact:sha256:"
			"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
			capture};
		materialization_prior_artifact_task_metadata task_metadata{
			task.identity, task.state, task.sealed_artifact_digest, capture.provider_execution_id};

		sdk::publication_record current_record;
		{
			auto opener = materialization_rooted_store_opener::create(*root);
			require(opener.has_value(), "SQLite close/reopen test could not create rooted opener");
			auto store = (*opener)->open_sqlite(*parent_request.sqlite_path, built_engine);
			require(store.has_value(), "SQLite close/reopen test could not open the Store");

			auto publish = [&](const validated_publication_request& request,
							   std::string universe) -> sdk::snapshot_handle
			{
				auto plan = store_plan(built_engine, request, std::move(universe));
				auto writer = store->begin(std::move(plan.draft));
				require(writer.has_value(), "SQLite close/reopen test could not begin a writer");
				for (auto& partition_value : plan.partitions)
				{
					auto staged = writer->stage(std::move(partition_value));
					require(staged.has_value(),
							"SQLite close/reopen test could not stage a partition" +
								(staged ? std::string{}
										: " (" + staged.error().code + ":" + staged.error().field +
										 ":" + staged.error().detail + ")"));
				}
				require(writer->validate().has_value(),
						"SQLite close/reopen test could not validate a publication");
				auto published = writer->publish();
				require(published.has_value(),
						"SQLite close/reopen test could not publish a publication");
				return std::move(*published);
			};

			auto parent = publish(parent_request, "universe-materialization-compact-test");
			auto current_request = parent_request;
			current_request.genesis = false;
			current_request.expected_parent_publication = parent.publication().publication_id;
			auto current = publish(current_request, "universe-materialization-compact-test");
			current_record = current.publication();

			materialization_store_observation observation;
			observation.backend = current_request.backend;
			observation.selector = current_request.selector;
			observation.series_id = current_request.series_id;
			observation.expected_parent_publication = current_request.expected_parent_publication;
			observation.writer_begin_call_count = 1U;
			observation.publication_attempted = true;
			observation.publish_call_count = 1U;
			observation.candidate_manifest = current.manifest();
			observation.candidate_identity =
				materialization_publication_candidate{current_record.publication_id,
													  current_record.series_id,
													  current_record.snapshot_id,
													  current_record.sequence,
													  current_record.parent_publication};
			observation.publish_returned_record = current_record;

			auto capture_spool_result = detailed_task_report_replayable_spool::create();
			require(capture_spool_result.has_value(),
					"SQLite close/reopen capture spool could not be created");
			auto capture_spool = std::move(*capture_spool_result);
			require(capture_spool.append(capture).has_value() && capture_spool.seal().has_value(),
					"SQLite close/reopen capture spool could not be sealed");
			require(persist_materialization_prior_artifact(*root,
														   current_request,
														   current_record,
														   observation,
														   capture_spool,
														   {task_metadata})
						.has_value(),
					"SQLite prior artifact was not persisted for close/reopen test");
		}

		auto load_request = parent_request;
		load_request.genesis = false;
		load_request.expected_parent_publication = current_record.publication_id;
		auto loaded = load_materialization_prior_artifact(*root, built_engine, load_request);
		require(loaded.has_value() && loaded->has_value(),
				"SQLite prior artifact did not load after the Store was closed");
		auto& replay_bundle = loaded->value();
		require(replay_bundle.publication.publication_id == current_record.publication_id &&
					replay_bundle.publication.snapshot_id == current_record.snapshot_id &&
					replay_bundle.tasks.size() == 1U && replay_bundle.captures.has_value(),
				"SQLite close/reopen load lost the exact publication/task metadata");
		require(replay_bundle.tasks.front().provider_execution_id == capture.provider_execution_id,
				"SQLite close/reopen load lost the provider execution binding");

		std::optional<detailed_task_report_capture> replayed;
		auto replayed_one = replay_bundle.captures->replay_one(
			0U,
			[&](detailed_task_report_capture&& value) -> sdk::result<void>
			{
				replayed = std::move(value);
				return {};
			});
		require(replayed_one.has_value() && replayed.has_value() &&
					replayed->provider_task_id == capture.provider_task_id &&
					replayed->provider_execution_id == capture.provider_execution_id &&
					replayed->raw_frame_stream_digest == capture.raw_frame_stream_digest,
				"SQLite close/reopen load could not replay one sealed task");
	}

	void prior_artifact_rehydration_reproves_raw_semantics()
	{
		auto capture = replayable_capture("task:raw-reproof");
		sdk::provider::manifest provider_manifest;
		provider_manifest.provider_id = "provider:raw-reproof";
		provider_manifest.provider_version = {1U, 0U, 0U};
		artifact_transcript_sink sink;
		sdk::provider::protocol_writer writer{sink};
		const sdk::provider::protocol_credit credit{64U * 1024U * 1024U, 65536U};
		writer.grant_credit(credit);
		auto hello = sdk::provider::encode_control_text(provider_manifest.canonical_json());
		auto schema =
			sdk::provider::encode_schema_negotiate_metadata({"cxxlens.provider-protocol.v1", 0U});
		auto accepted = sdk::provider::encode_task_accepted_metadata(
			{provider_manifest.provider_id,
			 provider_manifest.provider_version.string(),
			 capture.provider_task_id});
		const std::vector<sdk::provider::coverage_unit> coverage{
			{"task", capture.provider_task_id, "covered", {}}};
		const std::vector<sdk::provider::unresolved_item> unresolved;
		const std::vector<sdk::provider::evidence_item> evidence;
		auto coverage_control = sdk::provider::encode_coverage_metadata(coverage);
		auto unresolved_control = sdk::provider::encode_unresolved_metadata(unresolved);
		auto evidence_control = sdk::provider::encode_evidence_metadata(evidence);
		auto complete = sdk::provider::encode_task_complete_metadata({capture.provider_task_id});
		require(hello && schema && accepted && coverage_control && unresolved_control &&
					evidence_control && complete,
				"raw reproof fixture could not encode its lifecycle controls");
		const auto send_control = [&](const sdk::provider::message_type type,
									  const std::vector<std::byte>& control,
									  const std::uint16_t flags = 0U)
		{
			auto sent = writer.send(
				type, std::span<const std::byte>{control}, std::span<const std::byte>{}, flags);
			require(sent.has_value(), "raw reproof fixture could not write a lifecycle frame");
		};
		send_control(sdk::provider::message_type::hello, *hello);
		send_control(sdk::provider::message_type::schema_negotiate, *schema);
		send_control(sdk::provider::message_type::task_accepted, *accepted);
		send_control(sdk::provider::message_type::coverage_chunk, *coverage_control);
		send_control(sdk::provider::message_type::unresolved_chunk, *unresolved_control);
		send_control(sdk::provider::message_type::progress, *evidence_control);
		send_control(sdk::provider::message_type::task_complete,
					 *complete,
					 static_cast<std::uint16_t>(sdk::provider::frame_flag::end_of_stream));
		auto frames = sdk::provider::decode_frame_stream(sink.transcript);
		require(frames.has_value(), "raw reproof fixture transcript could not be decoded");
		auto frame_receipt =
			sdk::provider::detail::provider_frame_transcript_receipt_digest(*frames);
		require(frame_receipt.has_value(), "raw reproof fixture frame receipt failed");
		capture.raw_frame_stream = sink.transcript;
		capture.raw_frame_stream_bytes = capture.raw_frame_stream.size();
		capture.raw_frame_stream_digest = sdk::content_digest(capture.raw_frame_stream);
		capture.frame_count = frames->size();
		capture.frame_transcript_digest = *frame_receipt;
		capture.capture_binding_digest.clear();
		capture.coverage = {{"task", capture.provider_task_id, "covered", {}}};
		capture.unresolved.clear();
		capture.evidence.clear();
		capture.observation_rows.clear();
		capture.base_claim_rows.clear();
		capture.source_span_claim_rows.clear();

		const auto output_descriptor = descriptor();
		const auto& output_column = output_descriptor.columns.front();
		sdk::detached_row row{output_descriptor.id, {}};
		row.cells.emplace(output_column.id,
						  sdk::detached_cell::typed("company_compact_item_id", "item:raw-reproof"));
		require(sdk::validate_row(output_descriptor, row).has_value(),
				"raw reproof fixture row is invalid");
		detailed_provider_batch_projection batch;
		batch.task_id = capture.provider_task_id;
		batch.descriptor_id = output_descriptor.id;
		batch.descriptor_digest = output_descriptor.descriptor_digest;
		batch.dependency_group_id = "canonical";
		batch.atomic_output_group_id = "raw-reproof";
		batch.batch_id = "batch:raw-reproof";
		batch.columns = {{output_column.id, 1U, 1U}};
		batch.ordered_chunk_digests = {
			"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
		batch.row_count = 1U;
		const auto row_form = row.canonical_form();
		batch.rows.push_back(
			{0U,
			 row_form,
			 sdk::content_digest(std::as_bytes(std::span{row_form.data(), row_form.size()}))});
		auto row_set = sdk::semantic_digest("cxxlens.clang22.materialization-report.row-set.v1",
											"0:" + row_form + "\n");
		require(row_set.has_value(), "raw reproof fixture row-set receipt failed");
		batch.row_set_digest = *row_set;
		batch.batch_digest = sdk::provider::columnar_batch_digest({batch.task_id,
																   batch.dependency_group_id,
																   batch.atomic_output_group_id,
																   batch.batch_id,
																   batch.descriptor_id,
																   batch.descriptor_digest,
																   batch.row_count,
																   batch.columns,
																   batch.ordered_chunk_digests,
																   {}});
		capture.batches = {std::move(batch)};

		auto state = prior_artifact_state();
		materialization_prior_artifact_task artifact{
			{0U,
			 capture.provider_task_id,
			 capture.task_input_digest,
			 capture.selected_catalog_compile_unit_id,
			 capture.compile_unit_id},
			std::move(state),
			"materialization.incremental-sealed-artifact:sha256:"
			"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
			std::move(capture)};
		validated_task_request current;
		current.provider_task_id = artifact.identity.provider_task_id;
		current.provider_execution_id = artifact.capture.provider_execution_id;
		current.task_input_digest = artifact.identity.task_input_digest;
		auto& input = current.worker_input;
		input.project_catalog.catalog_id = artifact.capture.catalog_id;
		input.project_catalog.catalog_digest = artifact.capture.catalog_digest;
		input.selected_catalog_compile_unit = artifact.capture.selected_catalog_compile_unit_id;
		input.compile_unit = artifact.capture.compile_unit_id;
		input.project = artifact.capture.project_id;
		input.variant = artifact.capture.variant_id;
		input.toolchain_context = artifact.capture.toolchain_context_id;
		input.toolchain_digest = artifact.capture.toolchain_digest;
		input.source_snapshot = artifact.capture.source_snapshot_id;
		input.file = artifact.capture.source_file_id;
		input.logical_path = artifact.capture.source_logical_path;
		input.source_content_digest = artifact.capture.source_content_digest;
		input.source_size_bytes = artifact.capture.source_size_bytes;
		input.source_encoding = artifact.capture.source_encoding;
		input.line_index = artifact.capture.source_line_index_id;
		input.source_read_only = artifact.capture.source_read_only;
		input.condition_universe = artifact.capture.condition_universe_id;
		input.condition = artifact.capture.condition_id;
		input.interpretation = artifact.capture.interpretation_domain;
		std::array output_descriptors{output_descriptor};
		auto rehydrated = rehydrate_materialization_prior_artifact(
			artifact,
			0U,
			current,
			provider_manifest,
			std::span<const sdk::relation_descriptor>{output_descriptors},
			credit,
			sdk::provider::protocol_limits{});
		const auto rehydration_failure = rehydrated ? std::string{"success"}
													: rehydrated.error().code + "/" +
				rehydrated.error().field + "/" + rehydrated.error().detail;
		require(!rehydrated &&
					rehydrated.error() ==
						sdk::error{"materialization.incremental-artifact-invalid",
								   "task.capture",
								   "raw-semantic-binding"},
				"prior artifact rehydration did not reject a raw/capture semantic mismatch: " +
					rehydration_failure);
	}

	[[nodiscard]] detailed_success_report_model valid_detailed_success_model()
	{
		detailed_success_report_model model;
		model.generated_at = "2026-08-10T12:34:56Z";
		model.store.backend = "memory";
		model.store.series_id = "series:test";
		model.store.selector_id = "selector:test";
		model.store.published_record = detailed_publication_projection{
			"publication:test", "series:test", "snapshot:test", 1U, 1U, std::nullopt};
		model.store.candidate_identity = detailed_publication_projection{
			"publication:test", "series:test", "snapshot:test", 1U, 0U, std::nullopt};
		model.store.verification = {
			{"current-selector", "present", std::nullopt, std::nullopt},
			{"open-publication", "present", std::nullopt, std::nullopt},
			{"open-snapshot", "present", std::nullopt, std::nullopt},
		};
		model.store.publication_attempted = true;
		model.store.publish_call_count = 1U;
		model.store.prior_history_retained = true;
		model.store.verified = true;
		model.tasks.push_back(replayable_capture("task:capacity"));
		return model;
	}

	void sealed_provider_transcript_report_leaf_binding_is_fail_closed()
	{
		auto model = valid_detailed_success_model();
		auto encoded = encode_detailed_success_report(model);
		require(encoded.has_value() && encoded->find("\"row_canonical_form\"") != std::string::npos,
				"detailed report leaf did not retain the sealed row canonical form");

		auto row_drift = model;
		auto& task = row_drift.tasks.front();
		auto& batch = task.batches.front();
		auto& row = batch.rows.front();
		row.row_canonical_form += "-drift";
		row.row_digest = sdk::content_digest(
			std::as_bytes(std::span{row.row_canonical_form.data(), row.row_canonical_form.size()}));
		auto row_set = sdk::semantic_digest("cxxlens.clang22.materialization-report.row-set.v1",
											"0:" + row.row_canonical_form + "\n");
		require(row_set.has_value(),
				"sealed report leaf negative fixture row-set derivation failed");
		batch.row_set_digest = *row_set;
		for (auto& observation : task.observation_rows)
			observation.observation_row_digest = row.row_digest;
		auto row_rejected = encode_detailed_success_report(row_drift);
		require(!row_rejected &&
					row_rejected.error() ==
						sdk::error{"materialization.report-invalid",
								   "provider_sealed_transcript",
								   "transcript-mismatch:leaf-binding"},
				"report encoder accepted a row mutation with a stale sealed transcript receipt");

		auto receipt_drift = model;
		receipt_drift.tasks.front().sealed_transcript_digest =
			"semantic-v2:sha256:" + std::string(64U, 'f');
		auto receipt_rejected = encode_detailed_success_report(receipt_drift);
		require(!receipt_rejected &&
					receipt_rejected.error() ==
						sdk::error{"materialization.report-invalid",
								   "provider_sealed_transcript",
								   "transcript-mismatch:leaf-binding"},
				"report encoder accepted a runtime sealed receipt mutation");
	}

	void detailed_report_capacity_reservation_is_compositional_and_closed()
	{
		auto model = valid_detailed_success_model();
		auto bound = checked_detailed_report_capacity_upper_bound(model);
		require(bound.has_value(), "valid detailed projection did not produce a capacity bound");
		require(bound->publication_independent_projection > 0U && bound->final_json_framing > 0U &&
					bound->exact_publication_outcome > 0U &&
					bound->exact_sdk_records_and_receipts > 0U &&
					bound->maximum_bounded_diagnostics > 0U &&
					bound->total ==
						bound->publication_independent_projection + bound->final_json_framing +
							bound->exact_publication_outcome +
							bound->exact_sdk_records_and_receipts +
							bound->maximum_bounded_diagnostics,
				"capacity proof dropped a required compositional component");

		auto encoded = encode_detailed_success_report(model);
		require(encoded.has_value() && encoded->size() <= bound->total,
				"capacity upper bound is smaller than the encoded projection: encoded=" +
					std::to_string(encoded ? encoded->size() : 0U) +
					" bound=" + std::to_string(bound->total) +
					(encoded ? std::string{}
							 : ": error=" + encoded.error().field + ":" + encoded.error().detail));

		require(bound->total > 1U, "capacity fixture did not leave room for boundary mutation");
		const auto encode_with_limit = [&](const std::size_t limit)
		{
			auto limited = model;
			limited.limits.max_projection_bytes = limit;
			return encode_detailed_success_report(limited);
		};
		auto limit_minus_one = encode_with_limit(bound->total - 1U);
		require(!limit_minus_one &&
					limit_minus_one.error() ==
						sdk::error{"materialization.report-invalid",
								   "report",
								   "limit-exceeded:capacity-reservation"},
				"capacity limit-minus-one did not fail closed before encoding");
		require(encode_with_limit(bound->total).has_value(),
				"capacity limit was rejected despite the checked bound");
		require(encode_with_limit(bound->total + 1U).has_value(),
				"capacity limit-plus-one was rejected unexpectedly");
	}

	void public_success_report_requires_all_authority_inputs()
	{
		public_materialization_success_report_input input;
		auto rejected = build_public_materialization_success_report(input);
		require(!rejected && rejected.error().code == "materialization.report-invalid" &&
					rejected.error().field == "report" &&
					rejected.error().detail.find("missing=request") != std::string::npos &&
					rejected.error().detail.find(",raw_input_observation") != std::string::npos,
				"public success report accepted absent execution authority");
		public_materialization_prior_artifact_persistence unavailable{
			false,
			"materialization.incremental-artifact-invalid",
			"publication.prior-artifact",
			"sidecar-write-failed"};
		input.prior_artifact_persistence = &unavailable;
		auto still_rejected = build_public_materialization_success_report(input);
		require(
			!still_rejected &&
				still_rejected.error().detail.find(
					"missing=publication.prior_artifact_persistence") == std::string::npos,
			"public success report did not admit an explicit unavailable prior-artifact status");
	}

	void prepublication_capacity_reservation_is_exact_and_single_use()
	{
		detailed_report_limits limits;
		auto capacity_result = check_public_materialization_capacity_reservation(limits);
		require(capacity_result.has_value(),
				"accepted report-limit profile did not mint a capacity proof");
		auto capacity = std::move(*capacity_result);
		const auto limit = capacity.reserved_bytes();
		const auto proof = std::string{capacity.proof_digest()};
		const auto projection = [&](const std::string& binding,
									const std::string& request,
									const std::string& semantic,
									const std::string& occurrence,
									const std::uint64_t task_count,
									const std::size_t reserved_bytes,
									const std::string& capacity_proof,
									const bool issue_capability = false)
		{
			return public_materialization_prepublication_projection_test_peer::make(
				binding,
				request,
				semantic,
				occurrence,
				task_count,
				reserved_bytes,
				capacity_proof,
				issue_capability);
		};
		const auto baseline = [&]
		{
			return projection("binding", "request", "semantic", "occurrence", 1U, limit, proof);
		};
		const auto issued_baseline = [&]
		{
			return projection(
				"binding", "request", "semantic", "occurrence", 1U, limit, proof, true);
		};
		const auto require_equality_mutation = [&](auto&& mutated, const std::string_view field)
		{
			require(!(baseline() == mutated),
					"publication-independent equality ignored mutation of " + std::string{field});
		};

		require_equality_mutation(
			projection("binding-drift", "request", "semantic", "occurrence", 1U, limit, proof),
			"binding_digest");
		require_equality_mutation(
			projection("binding", "request-drift", "semantic", "occurrence", 1U, limit, proof),
			"request_digest");
		require_equality_mutation(
			projection("binding", "request", "semantic-drift", "occurrence", 1U, limit, proof),
			"semantic_request_digest");
		require_equality_mutation(
			projection("binding", "request", "semantic", "occurrence-drift", 1U, limit, proof),
			"occurrence_inventory_digest");
		require_equality_mutation(
			projection("binding", "request", "semantic", "occurrence", 2U, limit, proof),
			"task_count");
		require_equality_mutation(
			projection("binding", "request", "semantic", "occurrence", 1U, limit + 1U, proof),
			"reserved_bytes");
		require_equality_mutation(
			projection("binding", "request", "semantic", "occurrence", 1U, limit, "proof-drift"),
			"capacity_proof_digest");

		auto forged = baseline();
		auto forged_consumed = forged.consume_reserved_capacity(capacity);
		require(!forged_consumed &&
					forged_consumed.error() ==
						sdk::error{"materialization.report-invalid",
								   "report.capacity",
								   "unissued-capability"},
				"matching forged authority fields crossed the private capability boundary");

		auto unconsumed = issued_baseline();
		auto unconsumed_state = unconsumed.validate_reserved_capacity(capacity, limit);
		require(!unconsumed_state &&
					unconsumed_state.error() ==
						sdk::error{"materialization.report-invalid",
								   "report.capacity",
								   "reservation-not-consumed"},
				"the report builder did not reject an unconsumed prepublication projection");

		auto empty_proof =
			projection("binding", "request", "semantic", "occurrence", 1U, limit, {}, true);
		auto empty_proof_consumed = empty_proof.consume_reserved_capacity(capacity);
		require(
			!empty_proof_consumed &&
				empty_proof_consumed.error() ==
					sdk::error{"materialization.report-invalid", "report.capacity", "proof-empty"},
			"capacity consumption accepted an empty projection proof");
		auto empty_proof_state = empty_proof.validate_reserved_capacity(capacity, limit);
		require(
			!empty_proof_state &&
				empty_proof_state.error() ==
					sdk::error{"materialization.report-invalid", "report.capacity", "proof-empty"},
			"capacity validation accepted an empty projection proof");

		auto moved_from_capacity_result = check_public_materialization_capacity_reservation(limits);
		require(moved_from_capacity_result.has_value(),
				"second capacity proof could not be minted for the moved-from regression");
		auto moved_to_capacity = std::move(*moved_from_capacity_result);
		require(moved_to_capacity.reserved_bytes() == limit &&
					!moved_to_capacity.proof_digest().empty(),
				"moving a capacity reservation lost its proof");
		require(moved_from_capacity_result->proof_digest().empty(),
				"moved-from capacity reservation retained its proof digest");
		auto moved_from_consumed =
			issued_baseline().consume_reserved_capacity(*moved_from_capacity_result);
		require(
			!moved_from_consumed &&
				moved_from_consumed.error() ==
					sdk::error{"materialization.report-invalid", "report.capacity", "proof-empty"},
			"capacity consumption accepted a moved-from reservation");
		auto moved_from_state =
			issued_baseline().validate_reserved_capacity(*moved_from_capacity_result, limit);
		require(
			!moved_from_state &&
				moved_from_state.error() ==
					sdk::error{"materialization.report-invalid", "report.capacity", "proof-empty"},
			"capacity validation accepted a moved-from reservation");

		auto proof_mismatch = projection(
			"binding", "request", "semantic", "occurrence", 1U, limit, "proof-drift", true);
		auto proof_mismatch_consumed = proof_mismatch.consume_reserved_capacity(capacity);
		require(!proof_mismatch_consumed &&
					proof_mismatch_consumed.error() ==
						sdk::error{"materialization.report-invalid",
								   "report.capacity",
								   "reservation-mismatch"},
				"capacity consumption accepted a mismatched proof");

		for (const auto attempt : std::array<std::size_t, 4>{0U, limit - 1U, limit, limit + 1U})
		{
			auto candidate = projection(
				"binding", "request", "semantic", "occurrence", 1U, attempt, proof, true);
			auto consumed = candidate.consume_reserved_capacity(capacity);
			const bool expected = attempt == limit;
			require(static_cast<bool>(consumed) == expected,
					"prepublication capacity accepted the wrong boundary: attempt=" +
						std::to_string(attempt));
			require(candidate.reservation_consumed() == expected,
					"prepublication capacity changed lifecycle state on a rejected boundary");
			if (!expected)
			{
				const auto expected_error = attempt == 0U
					? sdk::error{"materialization.report-invalid",
								 "report.capacity",
								 "zero-reservation"}
					: sdk::error{"materialization.report-invalid",
								 "report.capacity",
								 "reservation-mismatch"};
				require(consumed.error() == expected_error,
						"prepublication capacity returned the wrong boundary error");
			}
			if (expected)
			{
				auto repeated = candidate.consume_reserved_capacity(capacity);
				require(!repeated &&
							repeated.error() ==
								sdk::error{"materialization.report-invalid",
										   "report.capacity",
										   "already-consumed"},
						"prepublication capacity reservation was consumable twice");
				require(candidate == issued_baseline(),
						"capacity consumption changed publication-independent authority");
			}
		}

		auto invalid_limits = limits;
		--invalid_limits.max_projection_bytes;
		auto rejected_profile = check_public_materialization_capacity_reservation(invalid_limits);
		require(!rejected_profile &&
					rejected_profile.error() ==
						sdk::error{"materialization.report-invalid",
								   "report.capacity",
								   "authority-profile"},
				"capacity proof accepted a non-authoritative report-limit profile");
	}

	void final_response_spool_is_sealed_before_transport()
	{
		const std::string response{
			R"({"error":null,"process_exit_status":0,"report_version":"2.1.0",)"
			R"("response_kind":"detailed","result":"passed",)"
			R"("schema":"cxxlens.clang22-materialization-report.v2"})"};
		auto zero_limit = stage_public_materialization_final_response(response, 0U);
		require(!zero_limit &&
					zero_limit.error() ==
						sdk::error{
							"materialization.report-invalid", "report", "final-response-boundary"},
				"final response spool accepted a zero-byte response limit");
		auto too_small =
			stage_public_materialization_final_response(response, response.size() - 1U);
		require(!too_small &&
					too_small.error() ==
						sdk::error{
							"materialization.report-invalid", "report", "final-response-boundary"},
				"final response spool accepted a limit-minus-one response");

		auto staged = stage_public_materialization_final_response(response, response.size());
		require(staged && (*staged)->sealed() && (*staged)->size_bytes() == response.size(),
				"final response did not cross the sealed private-spool boundary");
		const auto expected = std::vector<std::byte>{
			std::as_bytes(std::span{response.data(), response.size()}).begin(),
			std::as_bytes(std::span{response.data(), response.size()}).end()};
		std::vector<std::byte> replay(expected.size());
		auto read = (*staged)->read_at(0U, replay);
		require(read && *read == replay.size() && replay == expected,
				"final response spool changed the canonical response bytes");
		auto digest = digest_materialization_spool(**staged);
		require(digest &&
					*digest == cxxlens::sdk::content_digest(std::span<const std::byte>{expected}),
				"final response spool lost its sealed-byte digest binding");
		const std::array extra{std::byte{'x'}};
		auto appended = (*staged)->append(extra);
		require(!appended && (*staged)->sealed(),
				"final response spool accepted mutation after seal");

		auto limit_plus_one =
			stage_public_materialization_final_response(response, response.size() + 1U);
		require(limit_plus_one && (*limit_plus_one)->sealed(),
				"final response spool rejected the exact limit-plus-one boundary");
		for (const auto fragment : std::array<std::size_t, 3>{1U, 3U, 7U})
		{
			std::vector<std::byte> fragmented(expected.size());
			std::size_t offset{};
			while (offset < fragmented.size())
			{
				const auto remaining = fragmented.size() - offset;
				const auto count = fragment < remaining ? fragment : remaining;
				auto target = std::span{fragmented}.subspan(offset, count);
				auto fragmented_read = (*staged)->read_at(offset, target);
				require(fragmented_read && *fragmented_read == count,
						"fragmented final-response replay returned a short read");
				offset += count;
			}
			require(fragmented == expected,
					"fragmented final-response replay changed authoritative bytes");
		}
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
	task_evidence_accumulator_is_bounded();
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
	coordinator_failure_must_use_worker_phase_code();
	exact_reuse_is_not_a_worker_launch();
	static_cast<void>(typed_store_cause_preserves_exact_detail());
	static_cast<void>(failed_head_observation_is_four_state_and_path_bound());
	store_stage_and_publication_boundary_are_closed();
	postpublication_failure_is_recovery_only_and_never_compact();
	bounded_detailed_projection_never_promotes_unverified_store();
	sealed_provider_transcript_report_leaf_binding_is_fail_closed();
	report_capture_spool_replays_one_task_at_a_time();
	prior_artifact_codec_is_canonical_and_bounded();
	sqlite_prior_artifact_sidecar_is_selector_bound();
	sqlite_prior_artifact_loads_after_store_close();
	prior_artifact_rehydration_reproves_raw_semantics();
	detailed_report_capacity_reservation_is_compositional_and_closed();
	public_report_occurrence_binding_rejects_forged_combinations();
	public_success_report_requires_all_authority_inputs();
	prepublication_capacity_reservation_is_exact_and_single_use();
	final_response_spool_is_sealed_before_transport();

	return 0;
}
