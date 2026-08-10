#include <algorithm>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/sdk/provider.hpp>

#include "llvm/clang22/materialization_admission_error.hpp"
#include "llvm/clang22/materialization_execution_journal.hpp"
#include "llvm/clang22/materialization_io.hpp"
#include "llvm/clang22/materialization_occurrence.hpp"
#include "llvm/clang22/materialization_pipeline.hpp"
#include "llvm/clang22/materialization_report.hpp"
#include "llvm/clang22/materialization_request_identity.hpp"
#include "llvm/clang22/materialization_request_stream.hpp"
#include "llvm/clang22/materialization_request_v2_1.hpp"
#include "llvm/clang22/materialization_seal.hpp"
#include "llvm/clang22/materialization_streaming_claims.hpp"
#include "llvm/clang22/materialization_task_spool.hpp"
#include "sdk/provider_runtime_internal.hpp"

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::detail::clang22::materialization;
	using cxxlens::detail::clang22::clang22_task_input_replay;
	using cxxlens::sdk::provider::detail::replayable_host_input;

	class stdin_reader final : public materialization_byte_reader
	{
	  public:
		materialization_io_result<std::size_t> read(const std::span<std::byte> destination) override
		{
			std::cin.read(reinterpret_cast<char*>(destination.data()),
						  static_cast<std::streamsize>(destination.size()));
			const auto received = std::cin.gcount();
			if (std::cin.bad() || (std::cin.fail() && !std::cin.eof()) || received < 0)
				return materialization_io_failure{materialization_io_failure_kind::read,
												  materialization_io_operation::input_read};
			return static_cast<std::size_t>(received);
		}
	};

	class task_input_replay final : public replayable_host_input
	{
	  public:
		explicit task_input_replay(const clang22_task_input_replay& input) noexcept : input_{input} {}

		[[nodiscard]] sdk::result<std::uint64_t> size() const override
		{
			if (!input_.sealed())
				return sdk::unexpected(
					sdk::error{"provider.host-transcript-invalid", "task.v3", "unsealed"});
			return input_.size_bytes();
		}

		[[nodiscard]] sdk::result<std::size_t>
		read_at(const std::uint64_t offset, const std::span<std::byte> destination) const override
		{
			if (!input_.sealed())
				return sdk::unexpected(
					sdk::error{"provider.host-transcript-invalid", "task.v3", "unsealed"});
			return const_cast<clang22_task_input_replay&>(input_).read_at(offset, destination);
		}

	  private:
		const clang22_task_input_replay& input_;
	};

	[[nodiscard]] std::string utc_now()
	{
		const auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
		std::tm value{};
#if defined(_WIN32)
		(void)gmtime_s(&value, &time);
#else
		(void)gmtime_r(&time, &value);
#endif
		char output[21]{};
		if (std::strftime(output, sizeof(output), "%Y-%m-%dT%H:%M:%SZ", &value) == 0U)
			return {};
		return output;
	}

	[[nodiscard]] int no_response() noexcept
	{
		return 2;
	}

	[[nodiscard]] int emit_failure(materialization_execution_journal journal,
								 compact_report_error error)
	{
		auto authority = std::move(journal).issue_compact_failure(std::move(error));
		if (!authority)
			return no_response();
		auto report = encode_compact_failure_report(*authority, utc_now());
		if (!report)
			return no_response();
		std::cout << *report;
		return 1;
	}

	[[nodiscard]] std::optional<std::string> role_digest(
		const materialization_occurrence_receipt& receipt, const std::string_view role)
	{
		const auto found = std::ranges::find(receipt.files, role,
			[](const materialization_measured_file& file) -> std::string_view
			{
				return file.authority.role;
			});
		if (found == receipt.files.end())
			return std::nullopt;
		return found->authority.digest;
	}

	[[nodiscard]] sdk::result<sdk::provider::manifest> make_worker_manifest(
		const materialization_v2_1_worker_authority& worker,
		const std::string_view worker_digest)
	{
		if (worker.provider_id != "cxxlens.clang22.reference" ||
			worker.provider_version != "1.0.0" || worker.protocol_major != 1U ||
			worker.protocol_minor != 1U || worker.semantic_contract_digest.empty() ||
			worker_digest.empty())
			return sdk::unexpected(
				sdk::error{"materialization.identity-mismatch", "worker", "runtime-contract"});

		sdk::provider::manifest manifest;
		manifest.provider_id = worker.provider_id;
		manifest.provider_version = {1U, 0U, 0U};
		manifest.package_identity = "cxxlens.clang22.reference.package";
		manifest.publisher = "cxxlens";
		manifest.license = "Apache-2.0";
		manifest.protocol = {1U, 1U, 1U, worker.required_features, {}};
		manifest.platform_tuples = {"linux-glibc"};
		manifest.provider_binary_digest = std::string{worker_digest};
		manifest.provider_semantic_contract_digest = worker.semantic_contract_digest;
		manifest.offered_relations = {
			"cc.call_direct_target@1",
			"cc.call_site@1",
			"cc.entity@1",
			"frontend.clang22.call_observation@2",
			"frontend.clang22.entity_observation@2",
			"frontend.clang22.type_observation@2",
		};
		manifest.interpretation_domains = {"cc.clang22-canonical-1"};
		manifest.invalidation_contract =
			"sha256:5656565656565656565656565656565656565656565656565656565656565656";
		manifest.determinism_contract =
			"sha256:7878787878787878787878787878787878787878787878787878787878787878";
		manifest.resource_class = "provider.clang22";
		manifest.sandbox_minimum = "enforced";
		manifest.requested_qualifications = {
			"canonical-semantic-qualified", "sandbox-qualified", "schema-conformant"};
		if (auto valid = manifest.validate(); !valid)
			return sdk::unexpected(std::move(valid.error()));
		return manifest;
	}

	[[nodiscard]] sdk::result<sdk::provider::sandbox_policy> worker_policy(
		const materialization_v2_1_worker_authority& worker)
	{
		auto policy = sdk::provider::resolve_sandbox_policy(worker.sandbox_policy_digest);
		if (!policy)
			return sdk::unexpected(std::move(policy.error()));
		return std::move(*policy);
	}

	[[nodiscard]] sdk::result<sdk::provider::provider_selection> select_worker(
		const sdk::provider::manifest& manifest,
		const sdk::provider::sandbox_policy& policy,
		std::string executable_path,
		const std::string_view worker_digest)
	{
		auto evidence = sdk::provider::sandbox_evidence_digest(
			policy,
			sdk::provider::execution_budget{},
			sdk::provider::sandbox_assurance::enforced,
			policy.mechanisms,
			worker_digest);
		if (!evidence)
			return sdk::unexpected(std::move(evidence.error()));
		sdk::provider::sandbox_report sandbox{
			"linux-glibc",
			policy.mechanisms,
			sdk::provider::sandbox_assurance::enforced,
			policy.policy_digest(),
			std::move(*evidence),
		};
		sdk::provider::provider_candidate candidate{
			manifest,
			sdk::provider::discovery_source::explicit_path,
			{std::move(executable_path)},
			true,
			true,
			true,
			manifest.requested_qualifications,
			std::move(sandbox),
			{},
		};
		sdk::provider::provider_selection_request request{
			manifest.provider_id,
			manifest.provider_version,
			manifest.provider_binary_digest,
			manifest.provider_semantic_contract_digest,
			{sdk::provider::sandbox_assurance::enforced, policy.policy_digest()},
			true,
			std::nullopt,
		};
		return sdk::provider::select_provider(request, std::span{&candidate, 1U});
	}

	[[nodiscard]] sdk::provider::sandbox_requirement task_sandbox(
		const materialization_v2_1_task_metadata_receipt& metadata)
	{
		return metadata.sandbox;
	}

	[[nodiscard]] sdk::result<sdk::provider::process_task_request> make_process_request(
		const materialization_v2_1_task_execution& execution,
		const sdk::provider::provider_selection& selection,
		std::vector<sdk::relation_descriptor> output_descriptors)
	{
		if (!execution.task_input || !execution.task_input->sealed())
			return sdk::unexpected(
				sdk::error{"materialization.spool-failure", "task-input", "unsealed"});
		sdk::provider::process_task_request request;
		request.selection = selection;
		request.output_descriptors = std::move(output_descriptors);
		request.task_id = execution.metadata.provider_task_id;
		request.task_input_digest = execution.metadata.task_input_digest;
		request.normalized_invocation_digest = execution.input.normalized_invocation_digest;
		request.toolchain_digest = execution.input.toolchain_digest;
		request.environment_digest = execution.input.environment_digest;
		request.sandbox = task_sandbox(execution.metadata);
		request.budget = execution.input.budget;
		request.limits.minimum_minor = 1U;
		request.limits.maximum_minor = 1U;
		request.limits.protocol_major = 1U;
		request.limits.supported_flags =
			static_cast<std::uint16_t>(sdk::provider::frame_flag::end_of_stream);
		request.output_credit = {
			std::max<std::uint64_t>(request.budget.output_bytes, 1U),
			std::max<std::uint64_t>(request.budget.diagnostics + request.budget.rows + 32U, 1U)};
		return request;
	}
} // namespace

int main(const int argc, char**)
{
	if (argc != 1)
		return 2;

	// Capture the effect root before request parsing. It is retained for the later rooted Store
	// boundary even while the provider-only execution path is being established.
	auto effect_root = materialization_effect_root::capture_startup();
	if (!effect_root)
		return no_response();

	stdin_reader input;
	auto raw_request = make_materialization_private_spool();
	if (!raw_request)
		return no_response();
	auto observed = capture_bounded_input(input, **raw_request);
	if (!observed)
		return no_response();
	auto journal = materialization_execution_journal::begin(*observed);
	if (!journal)
		return no_response();
	if (!observed->complete)
		return emit_failure(std::move(*journal),
			{"materialization.request-invalid", "input-limit", "maximum-bytes"});
	if (auto passed = journal->pass_input_limit(); !passed)
		return no_response();

	auto task_index = make_materialization_request_task_index((*raw_request)->size_bytes());
	if (!task_index)
		return emit_failure(std::move(*journal),
			{"materialization.spool-failure", "task-index", "create"});
	auto envelope = scan_materialization_request_envelope(**raw_request, {}, task_index->get());
	if (!envelope)
		return emit_failure(std::move(*journal),
			{"materialization.request-invalid", "request-envelope", "strict-json"});
	if (auto passed = journal->pass_json_decode(); !passed)
		return no_response();
	if (auto passed = journal->pass_request_envelope(); !passed)
		return no_response();
	if (auto passed = journal->pass_request_version(); !passed)
		return no_response();

	auto request = validate_materialization_request_v2_1(
		std::move(*raw_request), std::move(*envelope), std::move(*task_index));
	if (!request)
		return emit_failure(std::move(*journal),
			{"materialization.request-invalid", "request-schema", "selected-contract"});
	if (auto passed = journal->pass_request_schema(); !passed)
		return no_response();

	const auto& identity = request->identity();
	if (auto bound = journal->authenticate_request(
			{identity.materialization_request_id, identity.request_digest, identity.semantic_request_digest},
			request->request().task_count());
		!bound)
		return no_response();

	const auto& tool = request->request().tool();
	const auto& worker = request->request().worker();
	materialization_occurrence_expectation occurrence_expectation{
		tool.source_revision,
		tool.source_tree,
		tool.package_configuration,
		tool.occurrence_manifest_digest,
		tool.installed_executable_digest,
		worker.installed_binary_digest,
	};
	auto occurrence = measure_materialization_occurrence(occurrence_expectation);
	if (!occurrence)
		return emit_failure(std::move(*journal),
			{"materialization.identity-mismatch", "installation", "occurrence"});
	auto worker_digest = role_digest(occurrence->receipt(), "worker-executable");
	if (!worker_digest)
		return emit_failure(std::move(*journal),
			{"materialization.identity-mismatch", "worker-executable", "digest"});
	auto worker_fd = occurrence->open_role("worker-executable");
	if (!worker_fd)
		return emit_failure(std::move(*journal),
			{"materialization.identity-mismatch", "worker-executable", "sealed-fd"});
	auto manifest = make_worker_manifest(worker, *worker_digest);
	if (!manifest)
		return emit_failure(std::move(*journal),
			{"materialization.identity-mismatch", "worker-manifest", "runtime-contract"});
	auto policy = worker_policy(worker);
	if (!policy)
		return emit_failure(std::move(*journal),
			{"materialization.identity-mismatch", "worker-sandbox", "policy"});
	const auto executable_path =
		std::string{"/proc/self/fd/"} + std::to_string(worker_fd->get());
	auto selection = select_worker(*manifest, *policy, executable_path, *worker_digest);
	if (!selection)
		return emit_failure(std::move(*journal),
			{"materialization.identity-mismatch", "worker-selection", "exact-provider"});
	auto processes = sdk::provider::detail::make_system_replayable_provider_process_port();
	if (!processes)
		return emit_failure(std::move(*journal),
			{"materialization.identity-mismatch", "process-port", "unavailable"});

	// The v2.1 request remains the source authority.  This private context retains only bounded
	// task metadata and sealed source receipts; claims construction receives one live result from
	// the loader below and never gets a request-wide resident task.v3/source representation.
	auto claim_context = make_materialization_v2_1_claim_context(*request, occurrence->receipt());
	if (!claim_context)
		return emit_failure(std::move(*journal),
			{"materialization.identity-mismatch", "claims-bridge", "runtime-contract"});
	if (auto completed = journal->complete_installation_binding(); !completed)
		return no_response();
	auto& claim_request = claim_context->request;
	const auto request_subject = request->identity().materialization_request_id;
	const detailed_report_limits report_limits{};
	if (claim_request.tasks.size() > report_limits.max_tasks)
		return emit_failure(std::move(*journal),
			{"materialization.report-invalid", request_subject, "task-evidence-limit"});
	detailed_task_report_accumulator task_reports{report_limits};
	std::optional<sealed_materialization_result> live_result;
	std::size_t next_task_index{};
	bool launch_in_flight{};

	const auto execute_one = [&](const std::size_t index,
								const bool retain_result) -> sdk::result<void>
	{
		if (index != next_task_index || index >= claim_request.tasks.size())
			return sdk::unexpected(
				sdk::error{"materialization.execution-journal-invalid", "task", "noncanonical-order"});
		next_task_index = index + 1U;
		live_result.reset();
		if (auto attempted = journal->record_worker_launch_attempt(); !attempted)
			return sdk::unexpected(std::move(attempted.error()));
		launch_in_flight = true;
		auto execution = request->task_execution(index);
		if (!execution)
			return sdk::unexpected(
				sdk::error{"materialization.worker-failure", request_subject, "task-replay"});
		auto process_request = make_process_request(
			*execution, *selection, request->request().output_descriptors());
		if (!process_request)
			return sdk::unexpected(
				sdk::error{"materialization.worker-failure", request_subject, "process-request"});
		task_input_replay replay{*execution->task_input};
		auto outcome = sdk::provider::detail::execute_provider_process_replayable(
			*processes, *process_request, replay);
		if (!outcome || !outcome->succeeded() || !outcome->sealed ||
			!outcome->runtime_receipt)
			return sdk::unexpected(sdk::error{
				"materialization.worker-failure", execution->metadata.provider_task_id, "execution"});
		if (auto launched = journal->record_worker_launch_success(); !launched)
			return sdk::unexpected(std::move(launched.error()));
		launch_in_flight = false;

		streamed_validated_materialization_task_request seal_request{
			std::move(execution->input),
			std::move(execution->source_receipt),
			std::move(execution->metadata.provider_task_id),
			std::move(execution->metadata.provider_execution_id),
			std::move(execution->metadata.task_input_digest),
			std::move(execution->metadata.sandbox),
			std::move(execution->task_input),
		};
		auto sealed = validate_and_seal_materialization(
			std::move(seal_request), std::move(*outcome->sealed));
		if (!sealed)
			return sdk::unexpected(std::move(sealed.error()));
		// Capture the bounded, source-private task evidence before claims adoption.  The
		// provider seal has been transferred into `sealed`; the capture routine binds the
		// remaining runtime receipts to that immutable authority and retains no raw frames.
		auto task_report = capture_detailed_task_report(*outcome, *sealed, report_limits);
		if (!task_report)
			return sdk::unexpected(std::move(task_report.error()));
		if (auto appended = task_reports.append(std::move(*task_report)); !appended)
			return sdk::unexpected(std::move(appended.error()));
		if (retain_result)
			live_result.emplace(std::move(*sealed));
		return {};
	};

	const materialization_task_result_loader load =
		[&](const std::size_t index)
		-> sdk::result<std::reference_wrapper<const sealed_materialization_result>>
	{
		auto executed = execute_one(index, true);
		if (!executed)
			return sdk::unexpected(std::move(executed.error()));
		if (!live_result)
			return sdk::unexpected(
				sdk::error{"materialization.claim-invalid", request_subject, "missing-live-result"});
		return std::cref(*live_result);
	};

	auto claims = construct_materialization_claims_from_loader(
		claim_request, load, claim_context->producer_authority, claim_context->guarantee_authority);
	if (!claims)
	{
		// Claim construction can fail before the final task has been launched. Drain only the
		// remaining authenticated tasks, one at a time, so the journal can issue a phase-authentic
		// materialization failure without retaining an all-task result vector.
		while (!launch_in_flight && next_task_index < claim_request.tasks.size())
		{
			auto drained = execute_one(next_task_index, false);
			if (!drained)
				break;
		}
		if (launch_in_flight)
			return emit_failure(std::move(*journal),
				{"materialization.worker-failure", request_subject, "execution"});
		if (next_task_index != claim_request.tasks.size())
			return no_response();
		if (auto completed = journal->complete_worker_launches(); !completed)
			return no_response();
		if (auto completed = journal->complete_transcript_validation(); !completed)
			return no_response();
		const auto code = claims.error().code == "materialization.span-invalid"
			? "materialization.span-invalid"
			: claims.error().code == "materialization.coverage-incomplete"
				? "materialization.coverage-incomplete"
				: "materialization.claim-invalid";
		return emit_failure(std::move(*journal),
			{code, request_subject, claims.error().field.empty() ? "claims" : "claims"});
	}
	if (launch_in_flight || next_task_index != claim_request.tasks.size())
		return no_response();
	if (auto completed = journal->complete_worker_launches(); !completed)
		return no_response();
	if (auto completed = journal->complete_transcript_validation(); !completed)
		return no_response();

	auto transaction = make_materialization_store_transaction(claim_request, *claims);
	if (!transaction)
		return emit_failure(std::move(*journal),
			{"materialization.claim-invalid", request_subject, "store-transaction"});
	if (auto completed = journal->complete_materialization_validation(); !completed)
		return no_response();
	auto preparation = prepare_materialization_store(
		claim_request.engine, claim_request.publication, std::move(*transaction));
	const bool ready_for_publish = preparation.ready_for_publish();
	if (auto recorded = journal->record_store_preparation(std::move(preparation)); !recorded)
		return no_response();
	if (!ready_for_publish)
		return emit_failure(std::move(*journal),
			{"materialization.store-failure", request_subject, "prepublication"});
	if (!journal->complete_store_preparation())
		return no_response();

	// A detailed success report must bind the post-publication record, reopen receipts, physical
	// generation, and the independent execution/claim census.  Until that encoder is present, do
	// not cross the irreversible Store publication boundary or emit a success response.
	return emit_failure(std::move(*journal),
		{"materialization.report-invalid", request_subject, "success-report-not-installed"});
}
