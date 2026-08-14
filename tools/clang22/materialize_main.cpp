#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <ctime>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <cxxlens/sdk/provider.hpp>

#include "llvm/clang22/materialization_admission_error.hpp"
#include "llvm/clang22/materialization_execution_journal.hpp"
#include "llvm/clang22/materialization_incremental_coordinator.hpp"
#include "llvm/clang22/materialization_incremental_ingress.hpp"
#include "llvm/clang22/materialization_io.hpp"
#include "llvm/clang22/materialization_occurrence.hpp"
#include "llvm/clang22/materialization_partition_event_stream.hpp"
#include "llvm/clang22/materialization_pipeline.hpp"
#include "llvm/clang22/materialization_prior_artifact.hpp"
#include "llvm/clang22/materialization_public_report.hpp"
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
	enum class production_boundary_state : std::uint8_t
	{
		open,
		cancelled,
		publication_started,
	};

	static_assert(std::atomic<std::uint8_t>::is_always_lock_free,
				  "the production signal boundary requires a lock-free atomic");
	std::atomic<std::uint8_t> production_boundary{
		static_cast<std::uint8_t>(production_boundary_state::open)};

	void request_production_cancel(const int) noexcept
	{
		auto expected = static_cast<std::uint8_t>(production_boundary_state::open);
		(void)production_boundary.compare_exchange_strong(
			expected,
			static_cast<std::uint8_t>(production_boundary_state::cancelled),
			std::memory_order_relaxed,
			std::memory_order_relaxed);
	}

	[[nodiscard]] bool production_cancellation_requested() noexcept
	{
		return production_boundary.load(std::memory_order_relaxed) ==
			static_cast<std::uint8_t>(production_boundary_state::cancelled);
	}

	[[nodiscard]] bool begin_production_publication_gate() noexcept
	{
		auto expected = static_cast<std::uint8_t>(production_boundary_state::open);
		return production_boundary.compare_exchange_strong(
			expected,
			static_cast<std::uint8_t>(production_boundary_state::publication_started),
			std::memory_order_relaxed,
			std::memory_order_relaxed);
	}

	class production_signal_scope final
	{
	  public:
		production_signal_scope() noexcept
			: previous_interrupt_{std::signal(SIGINT, request_production_cancel)},
			  previous_terminate_{std::signal(SIGTERM, request_production_cancel)}
		{
		}
		production_signal_scope(const production_signal_scope&) = delete;
		production_signal_scope& operator=(const production_signal_scope&) = delete;
		~production_signal_scope() noexcept
		{
			if (previous_interrupt_ != SIG_ERR)
				(void)std::signal(SIGINT, previous_interrupt_);
			if (previous_terminate_ != SIG_ERR)
				(void)std::signal(SIGTERM, previous_terminate_);
		}

	  private:
		using signal_handler = void (*)(int);
		signal_handler previous_interrupt_{};
		signal_handler previous_terminate_{};
	};

	using namespace cxxlens;
	using namespace cxxlens::detail::clang22::materialization;
	using cxxlens::detail::clang22::clang22_task_input_replay;
	using cxxlens::sdk::provider::detail::replayable_host_input;

	[[nodiscard]] bool
	task_cursor_metadata_matches(const materialization_v2_1_task_execution& execution,
								 const std::size_t request_task_index,
								 const validated_task_request& task,
								 const sdk::project_catalog& catalog)
	{
		const auto& metadata = execution.metadata;
		if (!task.source_receipt || execution.source_receipt != *task.source_receipt)
			return false;
		return metadata.task_index == request_task_index &&
			metadata.project_id == task.worker_input.project &&
			metadata.catalog_id == catalog.catalog_id &&
			metadata.catalog_digest == catalog.catalog_digest &&
			metadata.selected_catalog_compile_unit_id ==
			task.worker_input.selected_catalog_compile_unit &&
			metadata.final_relation_compile_unit_id == task.worker_input.compile_unit &&
			metadata.variant_id == task.worker_input.variant &&
			metadata.toolchain_context_id == task.worker_input.toolchain_context &&
			metadata.toolchain_digest == task.worker_input.toolchain_digest &&
			metadata.source_snapshot_id == task.worker_input.source_snapshot &&
			metadata.file_id == task.worker_input.file &&
			metadata.logical_path == task.worker_input.logical_path &&
			metadata.source_content_digest == task.worker_input.source_content_digest &&
			metadata.source_size_bytes == task.worker_input.source_size_bytes &&
			metadata.source_encoding == task.worker_input.source_encoding &&
			metadata.line_index_id == task.worker_input.line_index &&
			metadata.source_read_only == task.worker_input.source_read_only &&
			metadata.condition_universe_id == task.worker_input.condition_universe &&
			metadata.condition_id == task.worker_input.condition &&
			metadata.interpretation_domain == task.worker_input.interpretation &&
			metadata.provider_task_id == task.provider_task_id &&
			metadata.provider_execution_id == task.provider_execution_id &&
			metadata.task_input_digest == task.task_input_digest &&
			metadata.sandbox.minimum == task.sandbox.minimum &&
			metadata.sandbox.policy_digest == task.sandbox.policy_digest;
	}

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
		explicit task_input_replay(const clang22_task_input_replay& input) noexcept : input_{input}
		{
		}

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

	[[nodiscard]] bool write_authoritative_response(const std::string_view response) noexcept
	{
		try
		{
			std::cout.write(response.data(), static_cast<std::streamsize>(response.size()));
			std::cout.flush();
			return static_cast<bool>(std::cout);
		}
		catch (...)
		{
			return false;
		}
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
		return write_authoritative_response(*report) ? 1 : no_response();
	}

	[[nodiscard]] compact_report_error remap_typed_failure(const std::string_view allowed_code,
														   const std::string_view subject,
														   const sdk::error& source)
	{
		return {std::string{allowed_code},
				std::string{subject},
				"source-code=" + source.code + ";source-field=" + source.field +
					";source-detail=" + source.detail};
	}

	[[nodiscard]] int emit_typed_failure(materialization_execution_journal journal,
										 const std::string_view allowed_code,
										 const std::string_view subject,
										 const sdk::error& source)
	{
		return emit_failure(std::move(journal), remap_typed_failure(allowed_code, subject, source));
	}

	[[nodiscard]] std::string materialization_validation_failure_code(const sdk::error& source)
	{
		if (source.code == "materialization.span-invalid" ||
			source.code == "materialization.claim-invalid" ||
			source.code == "materialization.coverage-incomplete")
			return source.code;
		return "materialization.claim-invalid";
	}

	[[nodiscard]] std::optional<std::string>
	role_digest(const materialization_occurrence_receipt& receipt, const std::string_view role)
	{
		const auto found =
			std::ranges::find(receipt.files,
							  role,
							  [](const materialization_measured_file& file) -> std::string_view
							  {
								  return file.authority.role;
							  });
		if (found == receipt.files.end())
			return std::nullopt;
		return found->authority.digest;
	}

	[[nodiscard]] sdk::result<sdk::provider::manifest>
	make_worker_manifest(const materialization_v2_1_worker_authority& worker,
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

	[[nodiscard]] sdk::result<sdk::provider::sandbox_policy>
	worker_policy(const materialization_v2_1_worker_authority& worker)
	{
		auto policy = sdk::provider::resolve_sandbox_policy(worker.sandbox_policy_digest);
		if (!policy)
			return sdk::unexpected(std::move(policy.error()));
		return std::move(*policy);
	}

	[[nodiscard]] sdk::result<sdk::provider::provider_selection>
	select_worker(const sdk::provider::manifest& manifest,
				  const sdk::provider::sandbox_policy& policy,
				  std::string executable_path,
				  const std::string_view worker_digest)
	{
		auto evidence =
			sdk::provider::sandbox_evidence_digest(policy,
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

	[[nodiscard]] sdk::provider::sandbox_requirement
	task_sandbox(const materialization_v2_1_task_metadata_receipt& metadata)
	{
		return metadata.sandbox;
	}

	[[nodiscard]] sdk::result<sdk::provider::process_task_request>
	make_process_request(const materialization_v2_1_task_execution& execution,
						 const sdk::provider::provider_selection& selection,
						 std::vector<sdk::relation_descriptor> output_descriptors,
						 const std::stop_token cancellation)
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
		const auto checked_add = [](const std::uint64_t lhs,
									const std::uint64_t rhs) -> std::optional<std::uint64_t>
		{
			if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs)
				return std::nullopt;
			return lhs + rhs;
		};
		auto output_credit = checked_add(request.budget.diagnostics, request.budget.rows);
		if (output_credit)
			output_credit = checked_add(*output_credit, 32U);
		if (!output_credit)
			return sdk::unexpected(
				sdk::error{"materialization.report-invalid", "output-credit", "budget-overflow"});
		request.output_credit = {std::max<std::uint64_t>(request.budget.output_bytes, 1U),
								 std::max<std::uint64_t>(*output_credit, 1U)};
		request.cancellation = cancellation;
		return request;
	}

	[[nodiscard]] sdk::result<std::string>
	identity_digest(const std::string_view domain, const std::span<const std::string_view> values)
	{
		std::vector<sdk::canonical_value> fields;
		fields.reserve(values.size());
		for (const auto value : values)
			fields.push_back(sdk::canonical_value::from_string(std::string{value}));
		return sdk::canonical_identity_digest(domain, fields);
	}

	[[nodiscard]] sdk::result<std::string>
	identity_digest(const std::string_view domain, std::initializer_list<std::string_view> values)
	{
		return identity_digest(domain,
							   std::span<const std::string_view>{values.begin(), values.size()});
	}

	[[nodiscard]] sdk::result<std::string>
	production_partition_id(const materialization_v2_1_task_metadata_binding& task)
	{
		return identity_digest("clang22-materialization-partition",
							   {task.metadata.provider_task_id,
								task.metadata.task_input_digest,
								task.input.selected_catalog_compile_unit,
								task.input.compile_unit});
	}

	[[nodiscard]] sdk::result<sdk::incremental::partition_state>
	production_partition_state(const prevalidated_materialization_request_v2_1& request,
							   const materialization_v2_1_task_metadata_binding& task,
							   const std::string_view worker_digest,
							   const std::string_view worker_semantics_digest)
	{
		const auto& input = task.input;
		if (!input.source.empty() || !input.source_content_base64.empty())
			return sdk::unexpected(sdk::error{
				"materialization.incremental-invalid", "task", "metadata-source-residency"});
		std::vector<std::string_view> dependency_values;
		dependency_values.reserve(input.dependency_groups.size() + 1U);
		dependency_values.push_back(input.logical_path);
		for (const auto& dependency : input.dependency_groups)
			dependency_values.push_back(dependency);
		auto dependency_digest =
			identity_digest("clang22-incremental-dependencies", dependency_values);
		if (!dependency_digest)
			return sdk::unexpected(std::move(dependency_digest.error()));
		auto variant_digest = identity_digest("clang22-incremental-variant",
											  {input.variant,
											   input.variant_authority.language,
											   input.variant_authority.language_standard,
											   input.variant_authority.target_triple,
											   input.variant_authority.predefined_macros_digest,
											   input.variant_authority.include_search_digest,
											   input.variant_authority.semantic_flags_digest});
		if (!variant_digest)
			return sdk::unexpected(std::move(variant_digest.error()));
		auto provider_set_digest = identity_digest(
			"clang22-incremental-provider-set",
			{worker_semantics_digest, worker_digest, task.metadata.sandbox.policy_digest});
		if (!provider_set_digest)
			return sdk::unexpected(std::move(provider_set_digest.error()));
		auto interpretation_policy_digest =
			identity_digest("clang22-incremental-interpretation-policy",
							{input.interpretation, input.condition_universe, input.condition});
		if (!interpretation_policy_digest)
			return sdk::unexpected(std::move(interpretation_policy_digest.error()));
		auto condition_universe_digest =
			identity_digest("clang22-incremental-condition-universe", {input.condition_universe});
		if (!condition_universe_digest)
			return sdk::unexpected(std::move(condition_universe_digest.error()));
		auto refresh_policy_digest = identity_digest("clang22-incremental-refresh-policy",
													 {"clang22-incremental-refresh-v1"});
		if (!refresh_policy_digest)
			return sdk::unexpected(std::move(refresh_policy_digest.error()));
		std::vector<std::string_view> relation_values;
		relation_values.reserve(request.output_descriptors().size() * 2U);
		for (const auto& descriptor : request.output_descriptors())
		{
			relation_values.push_back(descriptor.id);
			relation_values.push_back(descriptor.contract_digest);
		}
		auto relation_digest =
			identity_digest("clang22-incremental-relation-descriptors", relation_values);
		if (!relation_digest)
			return sdk::unexpected(std::move(relation_digest.error()));
		auto assumption_digest =
			identity_digest("clang22-incremental-assumptions", {"clang22-production-exact-v1"});
		if (!assumption_digest)
			return sdk::unexpected(std::move(assumption_digest.error()));
		auto partition_id = production_partition_id(task);
		if (!partition_id)
			return sdk::unexpected(std::move(partition_id.error()));
		auto coverage_digest = identity_digest("clang22-incremental-coverage",
											   {*partition_id, input.source_content_digest});
		if (!coverage_digest)
			return sdk::unexpected(std::move(coverage_digest.error()));
		auto closure_digest =
			identity_digest("clang22-incremental-closure", {*partition_id, input.condition});
		if (!closure_digest)
			return sdk::unexpected(std::move(closure_digest.error()));
		sdk::incremental::input_fingerprint fingerprint{
			input.source_content_digest,
			*dependency_digest,
			input.normalized_invocation_digest,
			input.toolchain_digest,
			*condition_universe_digest,
			*variant_digest,
			*provider_set_digest,
			std::string{request.engine().registry_digest()},
			*interpretation_policy_digest,
			*refresh_policy_digest,
			input.environment_digest,
			std::string{worker_digest},
			std::string{worker_semantics_digest},
			*relation_digest,
			"clang22-incremental-normalizer-v1",
			request.catalog().catalog_digest,
			*assumption_digest,
			"exact"};
		if (auto valid = fingerprint.validate(); !valid)
			return sdk::unexpected(std::move(valid.error()));
		return sdk::incremental::partition_state{std::move(*partition_id),
												 std::move(fingerprint),
												 std::move(*coverage_digest),
												 std::move(*closure_digest),
												 false};
	}

	[[nodiscard]] sdk::result<std::vector<std::string>> partition_ids_from_event_projections(
		const std::span<const materialization_incremental_event_projection> events)
	{
		if (events.empty())
			return sdk::unexpected(sdk::error{
				"materialization.incremental-invalid", "partition-spool", "empty-event-set"});
		std::vector<std::string> output;
		for (const auto& event : events)
		{
			if (!sdk::validate_strong_id(event.partition_id))
				return sdk::unexpected(sdk::error{
					"materialization.incremental-invalid", "partition-spool", "partition-id"});
			if (output.empty() || output.back() != event.partition_id)
				output.push_back(event.partition_id);
		}
		if (!std::ranges::is_sorted(output) || std::ranges::adjacent_find(output) != output.end())
			return sdk::unexpected(sdk::error{
				"materialization.incremental-invalid", "partition-spool", "partition-order"});
		return output;
	}

	struct production_incremental_plan_bundle
	{
		sdk::incremental::materialization_plan plan;
		std::vector<materialization_incremental_task_binding> bindings;
	};

	[[nodiscard]] sdk::result<production_incremental_plan_bundle> make_production_incremental_plan(
		validated_materialization_request_v2_1& request,
		const std::string_view worker_digest,
		const std::string_view worker_semantics_digest,
		const std::optional<materialization_prior_artifact_replay_bundle>& prior)
	{
		const auto task_count = request.request().task_count();
		if (task_count == 0U || task_count > std::numeric_limits<std::size_t>::max())
			return sdk::unexpected(
				sdk::error{"materialization.incremental-invalid", "tasks", "metadata-census"});
		const auto task_count_size = static_cast<std::size_t>(task_count);
		std::vector<sdk::incremental::partition_candidate> candidates;
		std::vector<sdk::incremental::partition_state> states;
		std::vector<materialization_incremental_task_identity> identities;
		std::vector<std::string> provider_execution_ids;
		candidates.reserve(task_count_size);
		states.reserve(task_count_size);
		identities.reserve(task_count_size);
		provider_execution_ids.reserve(task_count_size);
		for (std::size_t index{}; index < task_count_size; ++index)
		{
			auto metadata = request.task_metadata_binding(static_cast<std::uint64_t>(index));
			if (!metadata)
				return sdk::unexpected(std::move(metadata.error()));
			auto state = production_partition_state(
				request.request(), *metadata, worker_digest, worker_semantics_digest);
			if (!state)
				return sdk::unexpected(std::move(state.error()));
			states.push_back(std::move(*state));
			identities.push_back({index,
								  metadata->metadata.provider_task_id,
								  metadata->metadata.task_input_digest,
								  metadata->input.selected_catalog_compile_unit,
								  metadata->input.compile_unit});
			provider_execution_ids.push_back(metadata->metadata.provider_execution_id);
		}
		for (std::size_t index{}; index < states.size(); ++index)
		{
			std::optional<sdk::incremental::partition_state> prior_state;
			if (prior)
			{
				auto archived =
					std::ranges::find_if(prior->tasks,
										 [index](const auto& task)
										 {
											 return task.identity.canonical_task_ordinal == index;
										 });
				if (archived != prior->tasks.end() &&
					archived->identity.provider_task_id == identities[index].provider_task_id &&
					archived->identity.task_input_digest == identities[index].task_input_digest &&
					archived->provider_execution_id == provider_execution_ids[index] &&
					archived->identity.selected_catalog_compile_unit_id ==
						identities[index].selected_catalog_compile_unit_id &&
					archived->identity.final_relation_compile_unit_id ==
						identities[index].final_relation_compile_unit_id &&
					archived->state == states[index])
					prior_state = archived->state;
			}
			candidates.push_back({states[index], std::move(prior_state)});
		}
		auto plan = sdk::incremental::make_materialization_plan(candidates);
		if (!plan)
			return sdk::unexpected(std::move(plan.error()));
		std::vector<materialization_incremental_task_binding> bindings;
		bindings.reserve(task_count_size);
		for (std::size_t index{}; index < task_count_size; ++index)
		{
			std::vector<materialization_incremental_partition_binding> partitions;
			std::optional<materialization_incremental_prior_artifact> prior_artifact;
			if (prior)
			{
				auto archived = std::ranges::find_if(
					prior->tasks,
					[index](const auto& task_value)
					{
						return task_value.identity.canonical_task_ordinal == index;
					});
				if (archived != prior->tasks.end() &&
					archived->identity.provider_task_id == identities[index].provider_task_id &&
					archived->identity.task_input_digest == identities[index].task_input_digest &&
					archived->provider_execution_id == provider_execution_ids[index] &&
					archived->identity.selected_catalog_compile_unit_id ==
						identities[index].selected_catalog_compile_unit_id &&
					archived->identity.final_relation_compile_unit_id ==
						identities[index].final_relation_compile_unit_id &&
					archived->state == states[index])
					prior_artifact = materialization_incremental_prior_artifact{
						archived->state, archived->sealed_artifact_digest};
			}
			partitions.emplace_back(states[index].partition_id,
									std::optional<sdk::incremental::partition_state>{states[index]},
									std::move(prior_artifact));
			bindings.emplace_back(std::move(identities[index]), std::move(partitions));
		}
		return production_incremental_plan_bundle{std::move(*plan), std::move(bindings)};
	}

	[[nodiscard]] sdk::result<std::vector<std::unique_ptr<materialization_replayable_spool>>>
	encode_production_partition_spools(
		const validated_materialization_request& request,
		const materialization_producer_authority& producer_authority,
		const materialization_guarantee_authority& guarantee_authority,
		const std::string_view request_id,
		const std::size_t task_index,
		const sealed_materialization_result& result,
		const materialization_incremental_pre_encoder_seal& seal)
	{
		auto all_events =
			materialization_incremental_result_event_projections(request,
																 task_index,
																 result,
																 std::span<const std::string>{},
																 producer_authority,
																 guarantee_authority);
		if (!all_events)
			return sdk::unexpected(std::move(all_events.error()));
		auto actual_partition_ids = partition_ids_from_event_projections(*all_events);
		if (!actual_partition_ids || *actual_partition_ids != seal.partition_ids)
			return sdk::unexpected(actual_partition_ids
									   ? sdk::error{"materialization.incremental-invalid",
													"partition-spool",
													"receipt-partition-set"}
									   : std::move(actual_partition_ids.error()));

		std::vector<std::unique_ptr<materialization_replayable_spool>> output;
		output.reserve(seal.partition_ids.size());
		for (std::size_t partition_index{}; partition_index < seal.partition_ids.size();
			 ++partition_index)
		{
			const auto& partition_id = seal.partition_ids[partition_index];
			auto partition_begin =
				std::ranges::find_if(*all_events,
									 [&](const auto& event)
									 {
										 return event.partition_id == partition_id;
									 });
			if (partition_begin == all_events->end())
				return sdk::unexpected(sdk::error{
					"materialization.incremental-invalid", "partition-spool", "partition-missing"});
			auto partition_end = partition_begin;
			while (partition_end != all_events->end() &&
				   partition_end->partition_id == partition_id)
				++partition_end;
			const std::span<const materialization_incremental_event_projection> events{
				&*partition_begin, static_cast<std::size_t>(partition_end - partition_begin)};
			std::uint64_t body_bytes{};
			for (const auto& event : events)
			{
				auto frame_size =
					materialization_partition_event_frame_size(event.key, event.payload);
				if (!frame_size ||
					*frame_size > std::numeric_limits<std::uint64_t>::max() - body_bytes)
					return sdk::unexpected(sdk::error{
						"materialization.incremental-invalid", "partition-spool", "size"});
				body_bytes += *frame_size;
			}
			if (seal.partition_ids.empty() ||
				task_index > std::numeric_limits<std::uint64_t>::max() ||
				partition_index > std::numeric_limits<std::uint64_t>::max() ||
				seal.partition_ids.size() > std::numeric_limits<std::uint64_t>::max())
				return sdk::unexpected(sdk::error{
					"materialization.incremental-invalid", "partition-spool", "ordinal"});
			const auto task_ordinal = static_cast<std::uint64_t>(task_index);
			const auto partition_count = static_cast<std::uint64_t>(seal.partition_ids.size());
			if (task_ordinal > std::numeric_limits<std::uint64_t>::max() / partition_count)
				return sdk::unexpected(
					sdk::error{"materialization.incremental-invalid", "partition-spool", "index"});
			const auto spool_base = task_ordinal * partition_count;
			const auto partition_ordinal = static_cast<std::uint64_t>(partition_index);
			if (partition_ordinal > std::numeric_limits<std::uint64_t>::max() - spool_base)
				return sdk::unexpected(
					sdk::error{"materialization.incremental-invalid", "partition-spool", "index"});
			const auto spool_index = spool_base + partition_ordinal;
			const auto ordinal = materialization_event_ordinal{task_ordinal, partition_ordinal};
			auto stream = materialization_partition_event_stream::begin(
				std::string{request_id}, spool_index, ordinal, events.size(), body_bytes);
			if (!stream)
				return sdk::unexpected(std::move(stream.error()));
			for (const auto& event : events)
			{
				auto appended = stream->append(event.kind, event.key, event.payload);
				if (!appended)
					return sdk::unexpected(std::move(appended.error()));
			}
			auto finalized = std::move(*stream).finalize();
			if (!finalized)
				return sdk::unexpected(std::move(finalized.error()));
			auto spool = std::move(*stream).release_spool();
			if (!spool)
				return sdk::unexpected(sdk::error{
					"materialization.incremental-invalid", "partition-spool", "release"});
			output.push_back(std::move(spool));
		}
		return output;
	}

	[[nodiscard]] sdk::result<std::vector<std::unique_ptr<materialization_replayable_spool>>>
	encode_production_partition_spools(const materialization_v2_1_claim_authority& claim_authority,
									   const materialization_v2_1_task_execution& task,
									   const std::string_view request_id,
									   const std::size_t task_index,
									   const sealed_materialization_result& result,
									   const materialization_incremental_pre_encoder_seal& seal)
	{
		auto all_events = materialization_incremental_result_event_projections(
			claim_authority, task_index, task, result, std::span<const std::string>{});
		if (!all_events)
			return sdk::unexpected(std::move(all_events.error()));
		auto actual_partition_ids = partition_ids_from_event_projections(*all_events);
		if (!actual_partition_ids || *actual_partition_ids != seal.partition_ids)
			return sdk::unexpected(actual_partition_ids
									   ? sdk::error{"materialization.incremental-invalid",
													"partition-spool",
													"receipt-partition-set"}
									   : std::move(actual_partition_ids.error()));

		std::vector<std::unique_ptr<materialization_replayable_spool>> output;
		output.reserve(seal.partition_ids.size());
		for (std::size_t partition_index{}; partition_index < seal.partition_ids.size();
			 ++partition_index)
		{
			const auto& partition_id = seal.partition_ids[partition_index];
			auto partition_begin =
				std::ranges::find_if(*all_events,
									 [&](const auto& event)
									 {
										 return event.partition_id == partition_id;
									 });
			if (partition_begin == all_events->end())
				return sdk::unexpected(sdk::error{
					"materialization.incremental-invalid", "partition-spool", "partition-missing"});
			auto partition_end = partition_begin;
			while (partition_end != all_events->end() &&
				   partition_end->partition_id == partition_id)
				++partition_end;
			const std::span<const materialization_incremental_event_projection> events{
				&*partition_begin, static_cast<std::size_t>(partition_end - partition_begin)};
			std::uint64_t body_bytes{};
			for (const auto& event : events)
			{
				auto frame_size =
					materialization_partition_event_frame_size(event.key, event.payload);
				if (!frame_size ||
					*frame_size > std::numeric_limits<std::uint64_t>::max() - body_bytes)
					return sdk::unexpected(sdk::error{
						"materialization.incremental-invalid", "partition-spool", "size"});
				body_bytes += *frame_size;
			}
			if (seal.partition_ids.empty() ||
				task_index > std::numeric_limits<std::uint64_t>::max() ||
				partition_index > std::numeric_limits<std::uint64_t>::max() ||
				seal.partition_ids.size() > std::numeric_limits<std::uint64_t>::max())
				return sdk::unexpected(sdk::error{
					"materialization.incremental-invalid", "partition-spool", "ordinal"});
			const auto task_ordinal = static_cast<std::uint64_t>(task_index);
			const auto partition_count = static_cast<std::uint64_t>(seal.partition_ids.size());
			if (task_ordinal > std::numeric_limits<std::uint64_t>::max() / partition_count)
				return sdk::unexpected(
					sdk::error{"materialization.incremental-invalid", "partition-spool", "index"});
			const auto spool_base = task_ordinal * partition_count;
			const auto partition_ordinal = static_cast<std::uint64_t>(partition_index);
			if (partition_ordinal > std::numeric_limits<std::uint64_t>::max() - spool_base)
				return sdk::unexpected(
					sdk::error{"materialization.incremental-invalid", "partition-spool", "index"});
			const auto spool_index = spool_base + partition_ordinal;
			const auto ordinal = materialization_event_ordinal{task_ordinal, partition_ordinal};
			auto stream = materialization_partition_event_stream::begin(
				std::string{request_id}, spool_index, ordinal, events.size(), body_bytes);
			if (!stream)
				return sdk::unexpected(std::move(stream.error()));
			for (const auto& event : events)
			{
				auto appended = stream->append(event.kind, event.key, event.payload);
				if (!appended)
					return sdk::unexpected(std::move(appended.error()));
			}
			auto finalized = std::move(*stream).finalize();
			if (!finalized)
				return sdk::unexpected(std::move(finalized.error()));
			auto spool = std::move(*stream).release_spool();
			if (!spool)
				return sdk::unexpected(sdk::error{
					"materialization.incremental-invalid", "partition-spool", "release"});
			output.push_back(std::move(spool));
		}
		return output;
	}

	class production_incremental_executor final : public materialization_incremental_task_executor
	{
	  public:
		production_incremental_executor(
			validated_materialization_request_v2_1& source_request,
			materialization_v2_1_task_cursor task_cursor,
			const validated_materialization_request& legacy_request,
			const sdk::provider::provider_selection& selection,
			const std::unique_ptr<sdk::provider::detail::replayable_provider_process_port>&
				processes,
			materialization_execution_journal& journal,
			detailed_task_report_replayable_spool& task_reports,
			const std::vector<sdk::relation_descriptor>& output_descriptors,
			const detailed_report_limits& report_limits,
			const materialization_prior_artifact_replay_bundle* prior_artifact,
			std::string request_id,
			const materialization_producer_authority& producer_authority,
			const materialization_guarantee_authority& guarantee_authority)
			: source_request_{source_request}, task_cursor_{std::move(task_cursor)},
			  legacy_request_{legacy_request}, selection_{selection}, processes_{processes},
			  journal_{journal}, task_reports_{task_reports},
			  output_descriptors_{output_descriptors}, report_limits_{report_limits},
			  prior_artifact_{prior_artifact}, request_id_{std::move(request_id)},
			  producer_authority_{producer_authority}, guarantee_authority_{guarantee_authority},
			  cancellation_watcher_{
				  [this](const std::stop_token stop)
				  {
					  while (!stop.stop_requested() && !production_cancellation_requested())
						  std::this_thread::sleep_for(std::chrono::milliseconds{10});
					  if (!stop.stop_requested())
						  cancellation_source_.request_stop();
				  }}
		{
		}

		sdk::result<materialization_incremental_task_execution>
		execute(const std::size_t request_task_index,
				const validated_task_request& task,
				const materialization_incremental_task_binding& binding) override
		{
			if (binding.partitions.size() != 1U || !binding.partitions.front().current_state)
				return sdk::unexpected(sdk::error{
					"materialization.incremental-invalid", "binding", "one-partition-required"});
			if (auto completed = close_pending_task(); !completed)
				return sdk::unexpected(std::move(completed.error()));
			if (auto attempted = journal_.record_task_attempt(); !attempted)
				return sdk::unexpected(std::move(attempted.error()));
			if (auto attempted = journal_.record_worker_launch_attempt(); !attempted)
				return sdk::unexpected(std::move(attempted.error()));
			auto next = task_cursor_.next();
			if (!next)
				return sdk::unexpected(std::move(next.error()));
			if (!*next ||
				!task_cursor_metadata_matches(
					**next, request_task_index, task, source_request_.request().catalog()))
				return sdk::unexpected(sdk::error{
					"materialization.incremental-invalid", "task-cursor", "order-or-end"});
			auto execution = std::move(**next);
			if (!execution.source || !execution.source->sealed() || !execution.task_input ||
				!execution.task_input->sealed())
				return sdk::unexpected(sdk::error{
					"materialization.incremental-invalid", "task-cursor", "unsealed-spool"});
			if (auto valid = execution.input.validate_with_catalog(
					source_request_.request().catalog(), execution.source_receipt);
				!valid)
				return sdk::unexpected(sdk::error{
					"materialization.incremental-invalid", "task-cursor", "input-binding"});
			auto process_request =
				make_process_request(execution,
									 selection_,
									 std::vector<sdk::relation_descriptor>{
										 output_descriptors_.begin(), output_descriptors_.end()},
									 cancellation_source_.get_token());
			if (!process_request)
				return sdk::unexpected(std::move(process_request.error()));
			task_input_replay replay{*execution.task_input};
			auto outcome = sdk::provider::detail::execute_provider_process_replayable(
				*processes_, *process_request, replay);
			if (!outcome || !outcome->succeeded() || !outcome->sealed || !outcome->runtime_receipt)
				return sdk::unexpected(sdk::error{
					"materialization.worker-failure", task.provider_task_id, "execution"});
			const auto task_metadata = execution.metadata;
			streamed_validated_materialization_task_request seal_request{
				std::move(execution.input),
				&source_request_.request().catalog(),
				std::move(execution.source_receipt),
				std::move(execution.metadata.provider_task_id),
				std::move(execution.metadata.provider_execution_id),
				std::move(execution.metadata.task_input_digest),
				std::move(execution.metadata.sandbox),
				std::move(execution.task_input),
			};
			auto sealed = validate_and_seal_materialization(std::move(seal_request),
															std::move(*outcome->sealed));
			if (!sealed)
				return sdk::unexpected(std::move(sealed.error()));
			outcome->sealed.reset();
			auto task_report =
				capture_detailed_task_report(*outcome, *sealed, task_metadata, report_limits_);
			if (!task_report)
				return sdk::unexpected(std::move(task_report.error()));
			if (auto appended = task_reports_.append(std::move(*task_report)); !appended)
				return sdk::unexpected(std::move(appended.error()));

			auto artifact_digest = seal_materialization_incremental_artifact_digest(*sealed);
			if (!artifact_digest)
				return sdk::unexpected(std::move(artifact_digest.error()));
			artifact_tasks_.push_back(materialization_prior_artifact_task_metadata{
				binding.task_identity,
				*binding.partitions.front().current_state,
				*artifact_digest,
				task_metadata.provider_execution_id});
			auto events =
				materialization_incremental_result_event_projections(legacy_request_,
																	 request_task_index,
																	 *sealed,
																	 std::span<const std::string>{},
																	 producer_authority_,
																	 guarantee_authority_);
			if (!events)
				return sdk::unexpected(std::move(events.error()));
			auto partition_ids = partition_ids_from_event_projections(*events);
			if (!partition_ids)
				return sdk::unexpected(std::move(partition_ids.error()));
			auto partition_set_digest =
				seal_materialization_incremental_task_partition_set_digest(*partition_ids);
			if (!partition_set_digest)
				return sdk::unexpected(std::move(partition_set_digest.error()));
			const auto& runtime = *outcome->runtime_receipt;
			auto task_receipt = make_materialization_incremental_task_receipt(
				legacy_request_,
				request_task_index,
				runtime.raw_stdout_byte_count(),
				std::string{runtime.raw_stdout_sha256()},
				runtime.decoded_frame_count(),
				std::string{runtime.frame_transcript_digest()},
				std::string{runtime.sealed_transcript_digest()},
				std::span<const materialization_incremental_event_projection>{*events});
			if (!task_receipt)
				return sdk::unexpected(std::move(task_receipt.error()));
			materialization_incremental_pre_encoder_seal pre_encoder{
				std::move(*task_receipt), *artifact_digest, *partition_set_digest, *partition_ids};
			materialization_incremental_provider_execution_receipt receipt{
				1U,
				std::string{sealed->provider_task_id()},
				std::string{sealed->provider_execution_id()},
				*artifact_digest,
				*partition_ids,
				*partition_set_digest,
				std::optional<materialization_incremental_pre_encoder_seal>{
					std::move(pre_encoder)}};
			const auto request_id = request_id_;
			const auto* request = &legacy_request_;
			const auto* producer_authority = &producer_authority_;
			const auto* guarantee_authority = &guarantee_authority_;
			auto delayed_encoder =
				[request, producer_authority, guarantee_authority, request_id, request_task_index](
					const sealed_materialization_result& result,
					const materialization_incremental_pre_encoder_seal& seal)
				-> sdk::result<std::vector<std::unique_ptr<materialization_replayable_spool>>>
			{
				return encode_production_partition_spools(*request,
														  *producer_authority,
														  *guarantee_authority,
														  request_id,
														  request_task_index,
														  result,
														  seal);
			};
			// Keep the launch in-flight across the coordinator's independent receipt, transcript,
			// and delayed-encoder checks.  A failure in any of those checks must still be
			// representable by the worker-launch phase of the execution journal.
			task_success_pending_ = true;
			task_worker_launch_pending_ = true;
			return materialization_incremental_task_execution{
				std::move(*sealed), std::move(receipt), std::move(delayed_encoder)};
		}

		sdk::result<materialization_incremental_task_reuse>
		load_reusable(const std::size_t request_task_index,
					  const validated_task_request& task,
					  const materialization_incremental_task_binding& binding) override
		{
			if (!prior_artifact_ || !prior_artifact_->captures || binding.partitions.size() != 1U ||
				!binding.partitions.front().current_state ||
				!binding.partitions.front().prior_artifact)
				return sdk::unexpected(sdk::error{
					"materialization.incremental-invalid", "executor", "reuse-authority-missing"});
			if (auto completed = close_pending_task(); !completed)
				return sdk::unexpected(std::move(completed.error()));
			if (auto attempted = journal_.record_task_attempt(); !attempted)
				return sdk::unexpected(std::move(attempted.error()));
			auto archived = std::ranges::find_if(
				prior_artifact_->tasks,
				[&](const auto& task_value)
				{
					return task_value.identity.canonical_task_ordinal == request_task_index;
				});
			if (archived == prior_artifact_->tasks.end() ||
				archived->identity != binding.task_identity ||
				archived->state != *binding.partitions.front().current_state ||
				archived->sealed_artifact_digest !=
					binding.partitions.front().prior_artifact->sealed_artifact_digest)
				return sdk::unexpected(sdk::error{
					"materialization.incremental-invalid", "executor", "reuse-identity"});
			std::optional<detailed_task_report_capture> archived_capture;
			auto replayed = prior_artifact_->captures->replay_one(
				static_cast<std::size_t>(std::distance(prior_artifact_->tasks.begin(), archived)),
				[&](detailed_task_report_capture&& capture) -> sdk::result<void>
				{
					archived_capture = std::move(capture);
					return {};
				});
			if (!replayed || !archived_capture)
				return sdk::unexpected(
					sdk::error{"materialization.incremental-invalid", "executor", "prior-capture"});
			materialization_prior_artifact_task archived_task{archived->identity,
															  archived->state,
															  archived->sealed_artifact_digest,
															  std::move(*archived_capture)};
			auto next = task_cursor_.next();
			if (!next)
				return sdk::unexpected(std::move(next.error()));
			if (!*next ||
				!task_cursor_metadata_matches(
					**next, request_task_index, task, source_request_.request().catalog()))
				return sdk::unexpected(sdk::error{
					"materialization.incremental-invalid", "task-cursor", "order-or-end"});
			auto execution = std::move(**next);
			if (!execution.source || !execution.source->sealed() || !execution.task_input ||
				!execution.task_input->sealed())
				return sdk::unexpected(sdk::error{
					"materialization.incremental-invalid", "task-cursor", "unsealed-spool"});
			if (auto valid = execution.input.validate_with_catalog(
					source_request_.request().catalog(), execution.source_receipt);
				!valid)
				return sdk::unexpected(sdk::error{
					"materialization.incremental-invalid", "task-cursor", "input-binding"});
			auto process_request =
				make_process_request(execution,
									 selection_,
									 std::vector<sdk::relation_descriptor>{
										 output_descriptors_.begin(), output_descriptors_.end()},
									 cancellation_source_.get_token());
			if (!process_request)
				return sdk::unexpected(std::move(process_request.error()));
			auto sealed = rehydrate_materialization_prior_artifact(
				archived_task,
				request_task_index,
				task,
				selection_.selected_candidate().description,
				output_descriptors_,
				process_request->output_credit,
				process_request->limits,
				report_limits_);
			if (!sealed)
				return sdk::unexpected(std::move(sealed.error()));
			auto artifact_digest = seal_materialization_incremental_artifact_digest(*sealed);
			if (!artifact_digest || *artifact_digest != archived->sealed_artifact_digest)
				return sdk::unexpected(sdk::error{
					"materialization.incremental-invalid", "executor", "reuse-artifact-digest"});
			if (auto appended = task_reports_.append(archived_task.capture); !appended)
				return sdk::unexpected(std::move(appended.error()));
			auto events =
				materialization_incremental_result_event_projections(legacy_request_,
																	 request_task_index,
																	 *sealed,
																	 std::span<const std::string>{},
																	 producer_authority_,
																	 guarantee_authority_);
			if (!events)
				return sdk::unexpected(std::move(events.error()));
			auto partition_ids = partition_ids_from_event_projections(*events);
			if (!partition_ids)
				return sdk::unexpected(std::move(partition_ids.error()));
			auto partition_set_digest =
				seal_materialization_incremental_task_partition_set_digest(*partition_ids);
			if (!partition_set_digest)
				return sdk::unexpected(std::move(partition_set_digest.error()));
			auto task_receipt = make_materialization_incremental_task_receipt(
				legacy_request_,
				request_task_index,
				archived_task.capture.raw_frame_stream_bytes,
				archived_task.capture.raw_frame_stream_digest,
				archived_task.capture.frame_count,
				archived_task.capture.frame_transcript_digest,
				archived_task.capture.sealed_transcript_digest,
				std::span<const materialization_incremental_event_projection>{*events});
			if (!task_receipt)
				return sdk::unexpected(std::move(task_receipt.error()));
			materialization_incremental_pre_encoder_seal pre_encoder{
				std::move(*task_receipt), *artifact_digest, *partition_set_digest, *partition_ids};
			materialization_incremental_provider_execution_receipt receipt{
				0U,
				std::string{sealed->provider_task_id()},
				std::string{sealed->provider_execution_id()},
				*artifact_digest,
				*partition_ids,
				*partition_set_digest,
				std::optional<materialization_incremental_pre_encoder_seal>{
					std::move(pre_encoder)}};
			const auto request_id = request_id_;
			const auto* request = &legacy_request_;
			const auto* producer_authority = &producer_authority_;
			const auto* guarantee_authority = &guarantee_authority_;
			auto delayed_encoder =
				[request, producer_authority, guarantee_authority, request_id, request_task_index](
					const sealed_materialization_result& result,
					const materialization_incremental_pre_encoder_seal& seal)
				-> sdk::result<std::vector<std::unique_ptr<materialization_replayable_spool>>>
			{
				return encode_production_partition_spools(*request,
														  *producer_authority,
														  *guarantee_authority,
														  request_id,
														  request_task_index,
														  result,
														  seal);
			};
			artifact_tasks_.push_back(materialization_prior_artifact_task_metadata{
				binding.task_identity,
				*binding.partitions.front().current_state,
				archived->sealed_artifact_digest,
				archived->provider_execution_id});
			task_success_pending_ = true;
			task_worker_launch_pending_ = false;
			return materialization_incremental_task_reuse{
				std::move(*sealed), std::move(receipt), std::move(delayed_encoder)};
		}

		[[nodiscard]] bool cancellation_requested() const noexcept override
		{
			if (production_cancellation_requested())
				cancellation_source_.request_stop();
			return cancellation_source_.stop_requested();
		}

		[[nodiscard]] bool dynamic_typed_partition_ids() const noexcept override
		{
			return true;
		}

		sdk::result<void> finalize_task_cursor() &&
		{
			auto finalized = std::move(task_cursor_).finalize();
			if (!finalized)
				return finalized;
			return close_pending_task();
		}

		[[nodiscard]] std::vector<materialization_prior_artifact_task_metadata>
		release_prior_artifact_tasks() &&
		{
			return std::move(artifact_tasks_);
		}

	  private:
		sdk::result<void> close_pending_task()
		{
			if (!task_success_pending_)
				return {};
			if (task_worker_launch_pending_)
			{
				auto completed = journal_.record_worker_launch_success();
				if (!completed)
					return completed;
				task_worker_launch_pending_ = false;
			}
			auto completed = journal_.record_task_success();
			if (!completed)
				return completed;
			task_success_pending_ = false;
			return {};
		}

		validated_materialization_request_v2_1& source_request_;
		materialization_v2_1_task_cursor task_cursor_;
		const validated_materialization_request& legacy_request_;
		const sdk::provider::provider_selection& selection_;
		const std::unique_ptr<sdk::provider::detail::replayable_provider_process_port>& processes_;
		materialization_execution_journal& journal_;
		detailed_task_report_replayable_spool& task_reports_;
		bool task_success_pending_{};
		bool task_worker_launch_pending_{};
		const std::vector<sdk::relation_descriptor>& output_descriptors_;
		detailed_report_limits report_limits_;
		const materialization_prior_artifact_replay_bundle* prior_artifact_{};
		std::vector<materialization_prior_artifact_task_metadata> artifact_tasks_;
		std::string request_id_;
		const materialization_producer_authority& producer_authority_;
		const materialization_guarantee_authority& guarantee_authority_;
		mutable std::stop_source cancellation_source_;
		std::jthread cancellation_watcher_;
	};

	/**
	 * Production executor for the v2.1 cursor bridge.  The cursor, rather than this executor,
	 * owns task advancement.  Each method consumes only the currently leased task window and the
	 * delayed encoder is invoked before that lease is released by the coordinator.
	 */
	class production_v2_1_executor final
	{
	  public:
		production_v2_1_executor(
			validated_materialization_request_v2_1& source_request,
			const materialization_v2_1_claim_authority& claim_authority,
			const materialization_incremental_selected_request_binding_set& binding_set,
			const sdk::provider::provider_selection& selection,
			const std::unique_ptr<sdk::provider::detail::replayable_provider_process_port>&
				processes,
			materialization_execution_journal& journal,
			detailed_task_report_replayable_spool& task_reports,
			const std::vector<sdk::relation_descriptor>& output_descriptors,
			const detailed_report_limits& report_limits,
			const materialization_prior_artifact_replay_bundle* prior_artifact,
			std::string request_id)
			: source_request_{source_request}, claim_authority_{claim_authority},
			  binding_set_{binding_set}, selection_{selection}, processes_{processes},
			  journal_{journal}, task_reports_{task_reports},
			  output_descriptors_{output_descriptors}, report_limits_{report_limits},
			  prior_artifact_{prior_artifact}, request_id_{std::move(request_id)},
			  cancellation_watcher_{
				  [this](const std::stop_token stop)
				  {
					  while (!stop.stop_requested() && !production_cancellation_requested())
						  std::this_thread::sleep_for(std::chrono::milliseconds{10});
					  if (!stop.stop_requested())
						  cancellation_source_.request_stop();
				  }}
		{
		}

		sdk::result<materialization_incremental_task_execution>
		execute(const std::size_t request_task_index,
				materialization_v2_1_task_execution& execution,
				const materialization_incremental_task_binding& binding)
		{
			if (binding.partitions.size() != 1U || !binding.partitions.front().current_state ||
				!execution.source || !execution.source->sealed() || !execution.task_input ||
				!execution.task_input->sealed())
				return sdk::unexpected(sdk::error{
					"materialization.incremental-invalid", "binding-or-window", "invalid"});
			if (auto completed = close_pending_task(); !completed)
				return sdk::unexpected(std::move(completed.error()));
			if (auto attempted = journal_.record_task_attempt(); !attempted)
				return sdk::unexpected(std::move(attempted.error()));
			if (auto attempted = journal_.record_worker_launch_attempt(); !attempted)
				return sdk::unexpected(std::move(attempted.error()));
			if (execution.metadata.task_index != request_task_index ||
				execution.source->receipt() != execution.source_receipt)
				return sdk::unexpected(
					sdk::error{"materialization.incremental-invalid", "task-window", "identity"});
			if (auto valid = execution.input.validate_with_catalog(
					source_request_.request().catalog(), execution.source_receipt);
				!valid)
				return sdk::unexpected(sdk::error{
					"materialization.incremental-invalid", "task-window", "input-binding"});
			auto process_request =
				make_process_request(execution,
									 selection_,
									 std::vector<sdk::relation_descriptor>{
										 output_descriptors_.begin(), output_descriptors_.end()},
									 cancellation_source_.get_token());
			if (!process_request)
				return sdk::unexpected(std::move(process_request.error()));
			task_input_replay replay{*execution.task_input};
			auto outcome = sdk::provider::detail::execute_provider_process_replayable(
				*processes_, *process_request, replay);
			if (!outcome || !outcome->succeeded() || !outcome->sealed || !outcome->runtime_receipt)
				return sdk::unexpected(sdk::error{"materialization.worker-failure",
												  execution.metadata.provider_task_id,
												  "execution"});
			if (auto consumed = consume_materialization_v2_1_task_window(execution); !consumed)
				return sdk::unexpected(std::move(consumed.error()));
			const auto task_metadata = execution.metadata;
			const auto task_input = execution.input;
			const auto source_receipt = execution.source_receipt;
			streamed_validated_materialization_task_request seal_request{
				task_input,
				&source_request_.request().catalog(),
				source_receipt,
				task_metadata.provider_task_id,
				task_metadata.provider_execution_id,
				task_metadata.task_input_digest,
				task_metadata.sandbox,
				std::move(execution.task_input),
			};
			auto sealed = validate_and_seal_materialization(std::move(seal_request),
															std::move(*outcome->sealed));
			if (!sealed)
				return sdk::unexpected(std::move(sealed.error()));
			outcome->sealed.reset();
			auto task_report =
				capture_detailed_task_report(*outcome, *sealed, task_metadata, report_limits_);
			if (!task_report)
				return sdk::unexpected(std::move(task_report.error()));
			if (auto appended = task_reports_.append(std::move(*task_report)); !appended)
				return sdk::unexpected(std::move(appended.error()));
			return make_execution(request_task_index,
								  execution,
								  binding,
								  std::move(*sealed),
								  outcome->runtime_receipt->raw_stdout_byte_count(),
								  std::string{outcome->runtime_receipt->raw_stdout_sha256()},
								  outcome->runtime_receipt->decoded_frame_count(),
								  std::string{outcome->runtime_receipt->frame_transcript_digest()},
								  std::string{outcome->runtime_receipt->sealed_transcript_digest()},
								  true);
		}

		sdk::result<materialization_incremental_task_reuse>
		load_reusable(const std::size_t request_task_index,
					  materialization_v2_1_task_execution& execution,
					  const materialization_incremental_task_binding& binding)
		{
			if (!prior_artifact_ || !prior_artifact_->captures || binding.partitions.size() != 1U ||
				!binding.partitions.front().current_state ||
				!binding.partitions.front().prior_artifact || !execution.source ||
				!execution.source->sealed() || !execution.task_input ||
				!execution.task_input->sealed())
				return sdk::unexpected(sdk::error{
					"materialization.incremental-invalid", "executor", "reuse-authority-missing"});
			if (auto completed = close_pending_task(); !completed)
				return sdk::unexpected(std::move(completed.error()));
			if (auto attempted = journal_.record_task_attempt(); !attempted)
				return sdk::unexpected(std::move(attempted.error()));
			if (execution.metadata.task_index != request_task_index ||
				execution.source->receipt() != execution.source_receipt)
				return sdk::unexpected(
					sdk::error{"materialization.incremental-invalid", "task-window", "identity"});
			validated_task_request task{execution.input,
										execution.metadata.provider_task_id,
										execution.metadata.provider_execution_id,
										execution.metadata.task_input_digest,
										execution.metadata.sandbox,
										{},
										execution.source_receipt};
			auto archived = std::ranges::find_if(
				prior_artifact_->tasks,
				[&](const auto& task_value)
				{
					return task_value.identity.canonical_task_ordinal == request_task_index;
				});
			if (archived == prior_artifact_->tasks.end() ||
				archived->identity != binding.task_identity ||
				archived->state != *binding.partitions.front().current_state ||
				archived->sealed_artifact_digest !=
					binding.partitions.front().prior_artifact->sealed_artifact_digest)
				return sdk::unexpected(sdk::error{
					"materialization.incremental-invalid", "executor", "reuse-identity"});
			std::optional<detailed_task_report_capture> archived_capture;
			auto replayed = prior_artifact_->captures->replay_one(
				static_cast<std::size_t>(std::distance(prior_artifact_->tasks.begin(), archived)),
				[&](detailed_task_report_capture&& capture) -> sdk::result<void>
				{
					archived_capture = std::move(capture);
					return {};
				});
			if (!replayed || !archived_capture)
				return sdk::unexpected(
					sdk::error{"materialization.incremental-invalid", "executor", "prior-capture"});
			materialization_prior_artifact_task archived_task{archived->identity,
															  archived->state,
															  archived->sealed_artifact_digest,
															  std::move(*archived_capture)};
			auto process_request =
				make_process_request(execution,
									 selection_,
									 std::vector<sdk::relation_descriptor>{
										 output_descriptors_.begin(), output_descriptors_.end()},
									 cancellation_source_.get_token());
			if (!process_request)
				return sdk::unexpected(std::move(process_request.error()));
			auto sealed = rehydrate_materialization_prior_artifact(
				archived_task,
				request_task_index,
				task,
				selection_.selected_candidate().description,
				output_descriptors_,
				process_request->output_credit,
				process_request->limits,
				report_limits_);
			if (!sealed)
				return sdk::unexpected(std::move(sealed.error()));
			auto artifact_digest = seal_materialization_incremental_artifact_digest(*sealed);
			if (!artifact_digest || *artifact_digest != archived->sealed_artifact_digest)
				return sdk::unexpected(sdk::error{
					"materialization.incremental-invalid", "executor", "reuse-artifact-digest"});
			if (auto appended = task_reports_.append(archived_task.capture); !appended)
				return sdk::unexpected(std::move(appended.error()));
			return make_reuse(request_task_index,
							  execution,
							  binding,
							  std::move(*sealed),
							  archived_task.capture.raw_frame_stream_bytes,
							  archived_task.capture.raw_frame_stream_digest,
							  archived_task.capture.frame_count,
							  archived_task.capture.frame_transcript_digest,
							  archived_task.capture.sealed_transcript_digest);
		}

		[[nodiscard]] bool cancellation_requested() const noexcept
		{
			if (production_cancellation_requested())
				cancellation_source_.request_stop();
			return cancellation_source_.stop_requested();
		}

		sdk::result<void> finalize_pending() &&
		{
			return close_pending_task();
		}

		[[nodiscard]] std::vector<materialization_prior_artifact_task_metadata>
		release_prior_artifact_tasks() &&
		{
			return std::move(artifact_tasks_);
		}

	  private:
		sdk::result<materialization_incremental_task_execution>
		make_execution(const std::size_t task_index,
					   materialization_v2_1_task_execution& task,
					   const materialization_incremental_task_binding& binding,
					   sealed_materialization_result result,
					   const std::uint64_t stdout_bytes,
					   std::string stdout_digest,
					   const std::uint64_t frame_count,
					   std::string frame_digest,
					   std::string sealed_digest,
					   const bool worker_launch)
		{
			auto artifact_digest = seal_materialization_incremental_artifact_digest(result);
			if (!artifact_digest)
				return sdk::unexpected(std::move(artifact_digest.error()));
			artifact_tasks_.push_back(materialization_prior_artifact_task_metadata{
				binding.task_identity,
				*binding.partitions.front().current_state,
				*artifact_digest,
				task.metadata.provider_execution_id});
			auto events = materialization_incremental_receipt_event_projections(
				claim_authority_, task_index, task, result, std::span<const std::string>{});
			if (!events)
				return sdk::unexpected(std::move(events.error()));
			auto partition_ids = partition_ids_from_event_projections(*events);
			if (!partition_ids)
				return sdk::unexpected(std::move(partition_ids.error()));
			auto partition_set_digest =
				seal_materialization_incremental_task_partition_set_digest(*partition_ids);
			if (!partition_set_digest)
				return sdk::unexpected(std::move(partition_set_digest.error()));
			auto task_receipt = make_materialization_incremental_task_receipt(
				claim_authority_,
				binding_set_,
				task_index,
				task,
				stdout_bytes,
				std::move(stdout_digest),
				frame_count,
				std::move(frame_digest),
				std::move(sealed_digest),
				std::span<const materialization_incremental_event_projection>{*events});
			if (!task_receipt)
				return sdk::unexpected(std::move(task_receipt.error()));
			materialization_incremental_pre_encoder_seal pre_encoder{
				std::move(*task_receipt), *artifact_digest, *partition_set_digest, *partition_ids};
			materialization_incremental_provider_execution_receipt receipt{
				worker_launch ? 1U : 0U,
				std::string{result.provider_task_id()},
				std::string{result.provider_execution_id()},
				*artifact_digest,
				*partition_ids,
				*partition_set_digest,
				std::optional<materialization_incremental_pre_encoder_seal>{
					std::move(pre_encoder)}};
			const auto* authority = &claim_authority_;
			const auto* task_ptr = &task;
			const auto request_id = request_id_;
			auto delayed_encoder = [authority, task_ptr, request_id, task_index](
									   const sealed_materialization_result& delayed_result,
									   const materialization_incremental_pre_encoder_seal& seal)
				-> sdk::result<std::vector<std::unique_ptr<materialization_replayable_spool>>>
			{
				return encode_production_partition_spools(
					*authority, *task_ptr, request_id, task_index, delayed_result, seal);
			};
			task_success_pending_ = true;
			task_worker_launch_pending_ = worker_launch;
			return materialization_incremental_task_execution{
				std::move(result), std::move(receipt), std::move(delayed_encoder)};
		}

		sdk::result<materialization_incremental_task_reuse>
		make_reuse(const std::size_t task_index,
				   materialization_v2_1_task_execution& task,
				   const materialization_incremental_task_binding& binding,
				   sealed_materialization_result result,
				   const std::uint64_t stdout_bytes,
				   std::string stdout_digest,
				   const std::uint64_t frame_count,
				   std::string frame_digest,
				   std::string sealed_digest)
		{
			auto artifact_digest = seal_materialization_incremental_artifact_digest(result);
			if (!artifact_digest)
				return sdk::unexpected(std::move(artifact_digest.error()));
			if (binding.partitions.empty() || !binding.partitions.front().current_state)
				return sdk::unexpected(
					sdk::error{"materialization.incremental-invalid", "binding", "state"});
			artifact_tasks_.push_back(materialization_prior_artifact_task_metadata{
				binding.task_identity,
				*binding.partitions.front().current_state,
				*artifact_digest,
				task.metadata.provider_execution_id});
			auto events = materialization_incremental_receipt_event_projections(
				claim_authority_, task_index, task, result, std::span<const std::string>{});
			if (!events)
				return sdk::unexpected(std::move(events.error()));
			auto partition_ids = partition_ids_from_event_projections(*events);
			if (!partition_ids)
				return sdk::unexpected(std::move(partition_ids.error()));
			auto partition_set_digest =
				seal_materialization_incremental_task_partition_set_digest(*partition_ids);
			if (!partition_set_digest)
				return sdk::unexpected(std::move(partition_set_digest.error()));
			auto task_receipt = make_materialization_incremental_task_receipt(
				claim_authority_,
				binding_set_,
				task_index,
				task,
				stdout_bytes,
				std::move(stdout_digest),
				frame_count,
				std::move(frame_digest),
				std::move(sealed_digest),
				std::span<const materialization_incremental_event_projection>{*events});
			if (!task_receipt)
				return sdk::unexpected(std::move(task_receipt.error()));
			materialization_incremental_pre_encoder_seal pre_encoder{
				std::move(*task_receipt), *artifact_digest, *partition_set_digest, *partition_ids};
			materialization_incremental_provider_execution_receipt receipt{
				0U,
				std::string{result.provider_task_id()},
				std::string{result.provider_execution_id()},
				*artifact_digest,
				*partition_ids,
				*partition_set_digest,
				std::optional<materialization_incremental_pre_encoder_seal>{
					std::move(pre_encoder)}};
			const auto* authority = &claim_authority_;
			const auto* task_ptr = &task;
			const auto request_id = request_id_;
			auto delayed_encoder = [authority, task_ptr, request_id, task_index](
									   const sealed_materialization_result& delayed_result,
									   const materialization_incremental_pre_encoder_seal& seal)
				-> sdk::result<std::vector<std::unique_ptr<materialization_replayable_spool>>>
			{
				return encode_production_partition_spools(
					*authority, *task_ptr, request_id, task_index, delayed_result, seal);
			};
			task_success_pending_ = true;
			task_worker_launch_pending_ = false;
			return materialization_incremental_task_reuse{
				std::move(result), std::move(receipt), std::move(delayed_encoder)};
		}

		sdk::result<void> close_pending_task()
		{
			if (!task_success_pending_)
				return {};
			if (task_worker_launch_pending_)
			{
				auto completed = journal_.record_worker_launch_success();
				if (!completed)
					return completed;
				task_worker_launch_pending_ = false;
			}
			auto completed = journal_.record_task_success();
			if (!completed)
				return completed;
			task_success_pending_ = false;
			return {};
		}

		validated_materialization_request_v2_1& source_request_;
		const materialization_v2_1_claim_authority& claim_authority_;
		const materialization_incremental_selected_request_binding_set& binding_set_;
		const sdk::provider::provider_selection& selection_;
		const std::unique_ptr<sdk::provider::detail::replayable_provider_process_port>& processes_;
		materialization_execution_journal& journal_;
		detailed_task_report_replayable_spool& task_reports_;
		bool task_success_pending_{};
		bool task_worker_launch_pending_{};
		const std::vector<sdk::relation_descriptor>& output_descriptors_;
		detailed_report_limits report_limits_;
		const materialization_prior_artifact_replay_bundle* prior_artifact_{};
		std::vector<materialization_prior_artifact_task_metadata> artifact_tasks_;
		std::string request_id_;
		mutable std::stop_source cancellation_source_;
		std::jthread cancellation_watcher_;
	};
} // namespace

int main(const int argc, char**)
{
	if (argc != 1)
		return 2;
	production_signal_scope cancellation_signals;

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
		return emit_failure(
			std::move(*journal),
			{"materialization.request-invalid", "request-schema", "selected-contract"});
	if (auto passed = journal->pass_request_schema(); !passed)
		return no_response();

	const auto& identity = request->identity();
	if (auto bound = journal->authenticate_request({identity.materialization_request_id,
													identity.request_digest,
													identity.semantic_request_digest},
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
		return emit_typed_failure(std::move(*journal),
								  "materialization.identity-mismatch",
								  identity.materialization_request_id,
								  occurrence.error());
	auto worker_digest = role_digest(occurrence->receipt(), "worker-executable");
	if (!worker_digest)
		return emit_typed_failure(
			std::move(*journal),
			"materialization.identity-mismatch",
			identity.materialization_request_id,
			sdk::error{"materialization.identity-mismatch", "worker-executable", "role-missing"});
	auto worker_fd = occurrence->open_role("worker-executable");
	if (!worker_fd)
		return emit_typed_failure(std::move(*journal),
								  "materialization.identity-mismatch",
								  identity.materialization_request_id,
								  worker_fd.error());
	auto manifest = make_worker_manifest(worker, *worker_digest);
	if (!manifest)
		return emit_typed_failure(std::move(*journal),
								  "materialization.identity-mismatch",
								  identity.materialization_request_id,
								  manifest.error());
	auto policy = worker_policy(worker);
	if (!policy)
		return emit_typed_failure(std::move(*journal),
								  "materialization.identity-mismatch",
								  identity.materialization_request_id,
								  policy.error());
	const auto executable_path = std::string{"/proc/self/fd/"} + std::to_string(worker_fd->get());
	auto selection = select_worker(*manifest, *policy, executable_path, *worker_digest);
	if (!selection)
		return emit_typed_failure(std::move(*journal),
								  "materialization.identity-mismatch",
								  identity.materialization_request_id,
								  selection.error());
	auto processes = sdk::provider::detail::make_system_replayable_provider_process_port();
	if (!processes)
		return emit_typed_failure(
			std::move(*journal),
			"materialization.identity-mismatch",
			identity.materialization_request_id,
			sdk::error{"materialization.identity-mismatch", "process-port", "unavailable"});

	// The v2.1 request remains the source authority. The production bridge retains only task
	// metadata, sealed source receipts, and replayable bounded claim/report spools (never decoded
	// source/task.v3 bytes). The resident claim oracle remains qualification-only.
	auto claim_context = make_materialization_v2_1_claim_context(*request, occurrence->receipt());
	if (!claim_context)
		return emit_typed_failure(std::move(*journal),
								  "materialization.identity-mismatch",
								  identity.materialization_request_id,
								  claim_context.error());
	auto loaded_prior_artifact = load_materialization_prior_artifact(
		*effect_root, request->request().engine(), request->request().publication());
	if (!loaded_prior_artifact)
		return emit_typed_failure(std::move(*journal),
								  "materialization.identity-mismatch",
								  identity.materialization_request_id,
								  sdk::error{"materialization.identity-mismatch",
											 "prior-artifact",
											 "invalid-or-unavailable"});
	auto prior_artifact = std::move(*loaded_prior_artifact);
	if (auto completed = journal->complete_installation_binding(); !completed)
		return no_response();
	const auto request_subject = request->identity().materialization_request_id;
	auto request_globals = request->replay_global_authority();
	if (!request_globals)
		return no_response();
	const auto& admitted_request = request->request();
	const detailed_report_limits report_limits{};
	if (admitted_request.task_count() > report_limits.max_tasks)
		return no_response();
	auto task_report_spool_result = detailed_task_report_replayable_spool::create(report_limits);
	if (!task_report_spool_result)
		return no_response();
	auto task_reports = std::move(*task_report_spool_result);
	auto incremental_request_id =
		materialization_incremental_request_id(claim_context->claim_authority);
	if (!incremental_request_id || *incremental_request_id != request_subject)
		return no_response();
	auto plan_bundle = make_production_incremental_plan(
		*request, *worker_digest, worker.semantic_contract_digest, prior_artifact);
	if (!plan_bundle)
		return no_response();
	auto bounded_source =
		materialization_bounded_claim_source::begin(claim_context->claim_authority);
	if (!bounded_source)
		return emit_typed_failure(std::move(*journal),
								  "materialization.claim-invalid",
								  request_subject,
								  bounded_source.error());
	auto ingress_begin = materialization_incremental_ingress::begin_dynamic(
		*request, claim_context->claim_authority, claim_context->selected_request_binding_set);
	if (!ingress_begin)
		return emit_typed_failure(std::move(*journal),
								  "materialization.incremental-invalid",
								  request_subject,
								  ingress_begin.error());
	std::optional<materialization_incremental_ingress> ingress{std::move(*ingress_begin)};
	production_v2_1_executor executor{*request,
									  claim_context->claim_authority,
									  claim_context->selected_request_binding_set,
									  *selection,
									  processes,
									  *journal,
									  task_reports,
									  admitted_request.output_descriptors(),
									  report_limits,
									  prior_artifact ? &*prior_artifact : nullptr,
									  *incremental_request_id};
	std::set<std::string, std::less<>> provider_execution_ids;
	const auto consume_output = [&](auto output,
									const std::size_t task_index,
									const sdk::incremental::action action,
									materialization_v2_1_task_execution& task) -> sdk::result<void>
	{
		const auto expected_provider_calls =
			action == sdk::incremental::action::recompute ? 1U : 0U;
		if (output.receipt.provider_call_count != expected_provider_calls ||
			output.receipt.provider_task_id != output.result.provider_task_id() ||
			output.receipt.provider_execution_id != output.result.provider_execution_id() ||
			output.receipt.provider_task_id != task.metadata.provider_task_id ||
			output.receipt.provider_execution_id != task.metadata.provider_execution_id ||
			!output.receipt.pre_encoder_seal)
			return sdk::unexpected(sdk::error{
				"materialization.incremental-invalid", "executor", "sealed-result-mismatch"});
		if (!provider_execution_ids.insert(output.receipt.provider_execution_id).second)
			return sdk::unexpected(sdk::error{
				"materialization.incremental-invalid", "executor", "duplicate-execution-id"});
		auto artifact_digest = seal_materialization_incremental_artifact_digest(output.result);
		if (!artifact_digest || *artifact_digest != output.receipt.sealed_artifact_digest)
			return sdk::unexpected(sdk::error{
				"materialization.incremental-invalid", "executor", "artifact-receipt-mismatch"});
		auto pre_encoder = std::move(*output.receipt.pre_encoder_seal);
		if (pre_encoder.result_artifact_digest != *artifact_digest ||
			pre_encoder.partition_ids != output.receipt.covered_partition_ids ||
			pre_encoder.task_partition_set_digest != output.receipt.task_partition_set_digest)
			return sdk::unexpected(sdk::error{
				"materialization.incremental-invalid", "executor", "pre-encoder-mismatch"});
		if (auto valid = validate_materialization_incremental_task_receipt(
				claim_context->claim_authority,
				claim_context->selected_request_binding_set,
				task_index,
				task,
				pre_encoder.task_receipt);
			!valid)
			return sdk::unexpected(std::move(valid.error()));
		auto encoded_spools = output.encode_partition_spools(output.result, pre_encoder);
		if (!encoded_spools)
			return sdk::unexpected(std::move(encoded_spools.error()));
		auto task_claims = construct_materialization_bounded_task_claims(
			claim_context->claim_authority, task_index, task, output.result);
		if (!task_claims)
			return sdk::unexpected(std::move(task_claims.error()));
		materialization_incremental_task_ingress ingress_task{std::move(output.result),
															  std::move(pre_encoder.task_receipt),
															  std::move(*encoded_spools)};
		if (auto consumed = std::move(*ingress).consume_task(task, std::move(ingress_task));
			!consumed)
			return sdk::unexpected(std::move(consumed.error()));
		if (auto consumed = bounded_source->consume_task(std::move(*task_claims)); !consumed)
			return sdk::unexpected(std::move(consumed.error()));
		return {};
	};
	auto cursor_run = run_materialization_incremental_v2_1_task_cursor(
		*request,
		plan_bundle->plan,
		std::span<const materialization_incremental_task_binding>{plan_bundle->bindings},
		[&](const std::size_t task_index,
			const sdk::incremental::action action,
			materialization_v2_1_task_execution& task,
			const materialization_incremental_task_binding& binding) -> sdk::result<void>
		{
			if (binding.partitions.size() != 1U || !binding.partitions.front().current_state)
				return sdk::unexpected(sdk::error{
					"materialization.incremental-invalid", "binding", "one-partition-required"});
			if (action == sdk::incremental::action::reuse)
			{
				auto reused = executor.load_reusable(task_index, task, binding);
				if (!reused)
					return sdk::unexpected(std::move(reused.error()));
				return consume_output(std::move(*reused), task_index, action, task);
			}
			if (executor.cancellation_requested())
				return sdk::unexpected(
					sdk::error{"materialization.worker-failure", "executor", "cancelled"});
			auto executed = executor.execute(task_index, task, binding);
			if (!executed)
				return sdk::unexpected(std::move(executed.error()));
			return consume_output(std::move(*executed), task_index, action, task);
		});
	if (!cursor_run)
		return emit_typed_failure(std::move(*journal),
								  "materialization.worker-failure",
								  request_subject,
								  cursor_run.error());
	if (auto finalized = std::move(executor).finalize_pending(); !finalized)
		return emit_typed_failure(std::move(*journal),
								  "materialization.worker-failure",
								  request_subject,
								  finalized.error());
	auto artifact_tasks = std::move(executor).release_prior_artifact_tasks();
	if (auto completed = journal->complete_worker_launches(); !completed)
		return no_response();
	auto ingress_result = std::move(*ingress).finalize_with_claim_stream();
	if (!ingress_result)
		return emit_typed_failure(std::move(*journal),
								  "materialization.incremental-invalid",
								  request_subject,
								  ingress_result.error());
	auto claim_stream =
		materialization_claim_stream_source::begin(*incremental_request_id,
												   admitted_request.task_count(),
												   ingress_result->journal,
												   std::move(ingress_result->claim_stream_tasks));
	if (!claim_stream)
		return emit_typed_failure(std::move(*journal),
								  "materialization.incremental-invalid",
								  request_subject,
								  claim_stream.error());
	if (auto completed = journal->complete_transcript_validation(); !completed)
		return no_response();
	if (auto sealed = task_reports.seal(); !sealed)
		return emit_typed_failure(
			std::move(*journal), "materialization.spool-failure", request_subject, sealed.error());
	auto sealed_bounded_source = std::move(*bounded_source).finalize();
	if (!sealed_bounded_source)
		return emit_typed_failure(std::move(*journal),
								  "materialization.claim-invalid",
								  request_subject,
								  sealed_bounded_source.error());

	auto streaming_transaction = make_materialization_streaming_store_transaction(
		claim_context->claim_authority, *sealed_bounded_source);
	if (!streaming_transaction)
	{
		const auto& source = streaming_transaction.error();
		return emit_typed_failure(std::move(*journal),
								  materialization_validation_failure_code(source),
								  request_subject,
								  source);
	}
	streaming_transaction->external_authority = {
		&*claim_stream,
		&ingress_result->journal,
	};
	if (auto completed = journal->complete_materialization_validation(); !completed)
		return no_response();
	std::unique_ptr<materialization_rooted_store_opener> rooted_opener;
	if (admitted_request.publication().backend == "sqlite")
	{
		auto created_rooted_opener = materialization_rooted_store_opener::create(*effect_root);
		if (!created_rooted_opener)
			return no_response();
		rooted_opener = std::move(*created_rooted_opener);
	}
	auto& partition_source = *sealed_bounded_source;
	materialization_store_preparation preparation = rooted_opener
		? prepare_materialization_store_streaming(admitted_request.engine(),
												  admitted_request.publication(),
												  std::move(*streaming_transaction),
												  partition_source,
												  *rooted_opener)
		: prepare_materialization_store_streaming(admitted_request.engine(),
												  admitted_request.publication(),
												  std::move(*streaming_transaction),
												  partition_source);
	const bool ready_for_publish = preparation.ready_for_publish();
	std::optional<sdk::error> store_failure;
	if (!ready_for_publish && preparation.observation().first_issue)
	{
		if (const auto* typed = std::get_if<materialization_store_sdk_failure>(
				&*preparation.observation().first_issue))
			store_failure = typed->error;
	}
	if (auto recorded = journal->record_store_preparation(std::move(preparation)); !recorded)
		return no_response();
	if (!ready_for_publish)
	{
		if (!store_failure)
			return no_response();
		return emit_typed_failure(
			std::move(*journal), "materialization.store-failure", request_subject, *store_failure);
	}
	if (!journal->complete_store_preparation())
		return no_response();
	auto prepublication = prepare_public_materialization_prepublication_projection(
		*request,
		*observed,
		occurrence->manifest(),
		occurrence->receipt(),
		report_limits.max_projection_bytes);
	if (!prepublication)
		return emit_typed_failure(std::move(*journal),
								  "materialization.report-invalid",
								  request_subject,
								  prepublication.error());
	public_materialization_prior_artifact_persistence prior_artifact_persistence;
	try
	{
		// Allocate the memory-backend unavailable representation before the irreversible Store
		// boundary. SQLite persistence failures after publication are exit-two/no-response events;
		// they must never be downgraded into a success report.
		prior_artifact_persistence.error_code.reserve(64U);
		prior_artifact_persistence.error_field.reserve(64U);
		prior_artifact_persistence.error_detail.reserve(64U);
		prior_artifact_persistence.error_code = "materialization.incremental-artifact-invalid";
		prior_artifact_persistence.error_field = "publication.prior-artifact";
		prior_artifact_persistence.error_detail = "persistence-failed";
	}
	catch (const std::bad_alloc&)
	{
		return emit_failure(std::move(*journal),
							{"materialization.report-invalid",
							 request_subject,
							 "prior-artifact-fallback-allocation"});
	}

	// Only the bounded publication-independent projection is constructed before the irreversible
	// Store boundary. Linearize cancellation against that boundary: a signal that wins before this
	// CAS prevents publish(), while a signal arriving after it is deliberately post-publication.
	if (!begin_production_publication_gate())
		return emit_failure(
			std::move(*journal),
			{"materialization.report-invalid", request_subject, "cancelled-before-publication"});
	// The detailed report consumes the post-publication observation and must match this
	// prepublication authority before emitting success.
	auto postpublication = std::move(*journal).begin_publication();
	if (!postpublication)
		return no_response();
	public_materialization_success_report_input public_input;
	public_input.request = &*request;
	public_input.request_globals = &*request_globals;
	public_input.task_report_spool = &task_reports;
	public_input.raw_input = &*observed;
	public_input.occurrence_manifest = &occurrence->manifest();
	public_input.occurrence_receipt = &occurrence->receipt();
	public_input.bounded_claims = &*sealed_bounded_source;
	public_input.store = &postpublication->store_observation();
	public_input.prepublication = &*prepublication;
	if (rooted_opener && rooted_opener->receipt())
		public_input.rooted_vfs_receipt = &*rooted_opener->receipt();
	public_input.generated_at = utc_now();
	public_input.maximum_report_bytes = report_limits.max_projection_bytes;
	try
	{
		if (admitted_request.publication().backend == "memory")
		{
			// The memory backend is intentionally process-local.  This installed tool consumes one
			// request and exits, so reporting a committed memory artifact would falsely claim a
			// subsequent invocation can recover it.
			prior_artifact_persistence.error_code = "materialization.incremental-artifact-invalid";
			prior_artifact_persistence.error_field = "memory";
			prior_artifact_persistence.error_detail = "process-lifetime-only";
		}
		else if (admitted_request.publication().backend == "sqlite")
		{
			const auto& store_observation = postpublication->store_observation();
			if (!store_observation.publish_returned_record)
				return no_response();
			try
			{
				auto persisted = persist_materialization_prior_artifact(
					*effect_root,
					admitted_request.publication(),
					*store_observation.publish_returned_record,
					store_observation,
					task_reports,
					std::move(artifact_tasks));
				if (!persisted)
					return no_response();
			}
			catch (...)
			{
				return no_response();
			}
			prior_artifact_persistence.committed = true;
		}
		else
			return no_response();
		if (prior_artifact_persistence.committed)
		{
			prior_artifact_persistence.error_code.clear();
			prior_artifact_persistence.error_field.clear();
			prior_artifact_persistence.error_detail.clear();
		}
		public_input.prior_artifact_persistence = &prior_artifact_persistence;
		auto public_model = build_public_materialization_success_report(public_input);
		if (!public_model)
			return no_response();
		auto report = encode_public_materialization_success_report(std::move(*public_model));
		if (!report)
			return no_response();
		return write_authoritative_response(*report) ? 0 : no_response();
	}
	catch (const std::bad_alloc&)
	{
		return no_response();
	}
	catch (...)
	{
		return no_response();
	}
}
