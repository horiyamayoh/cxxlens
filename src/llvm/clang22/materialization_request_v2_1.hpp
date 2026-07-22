#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <cxxlens/sdk/provider.hpp>

#include "materialization_request.hpp"
#include "materialization_request_identity.hpp"
#include "materialization_request_stream.hpp"

namespace cxxlens::detail::clang22::materialization
{
	/** Exact bounded auxiliary index whose private spool is being constructed. */
	enum class materialization_v2_1_auxiliary_spool_purpose : std::uint8_t
	{
		task_unique_index,
		task_collision_metadata,
		execution_unique_index,
		task_input_digest,
	};

	/** Purpose-aware private-spool construction port used by admission and focused fault tests. */
	class materialization_v2_1_auxiliary_spool_factory
	{
	  public:
		virtual ~materialization_v2_1_auxiliary_spool_factory() = default;
		[[nodiscard]] virtual materialization_io_result<
			std::unique_ptr<materialization_replayable_spool>>
		create(materialization_v2_1_auxiliary_spool_purpose purpose) = 0;
		/** Fresh digest state for one index entry or task-input replay; fault-test seam. */
		[[nodiscard]] virtual std::unique_ptr<materialization_digest_accumulator>
		make_digest(materialization_v2_1_auxiliary_spool_purpose purpose);
	};

	/** Exact installed-tool occurrence authority accepted for one v2.1 request. */
	struct materialization_v2_1_tool_authority
	{
		std::string executable;
		std::string interface_version;
		std::string distribution_version;
		std::string source_revision;
		std::string source_tree;
		std::string installed_executable_digest;
		std::string package_configuration;
		std::string occurrence_manifest_digest;

		[[nodiscard]] bool operator==(const materialization_v2_1_tool_authority&) const = default;
	};

	/** Worker and trust authority shared by every task in one accepted request. */
	struct materialization_v2_1_worker_authority
	{
		std::string executable;
		std::string provider_id;
		std::string provider_version;
		std::string installed_binary_digest;
		std::string semantic_contract_digest;
		std::uint64_t protocol_major{};
		std::uint64_t protocol_minor{};
		std::vector<std::string> required_features;
		std::string sandbox_policy_digest;
		std::string trust_policy_digest;
		std::vector<sdk::provider::sandbox_requirement> task_sandbox_requirements;
	};

	/** One bounded task-metadata window replayed from the private receipt spool. */
	struct materialization_v2_1_task_metadata_receipt
	{
		std::uint64_t task_index{};
		std::string project_id;
		std::string selected_catalog_compile_unit_id;
		std::string final_relation_compile_unit_id;
		std::string source_snapshot_id;
		std::string file_id;
		std::string logical_path;
		std::string source_content_digest;
		std::uint64_t source_size_bytes{};
		std::string source_encoding;
		std::string line_index_id;
		bool source_read_only{};
		std::string condition_universe_id;
		std::string condition_id;
		std::string interpretation_domain;
		std::string provider_task_id;
		std::string provider_execution_id;
		std::string task_input_digest;
		sdk::provider::sandbox_requirement sandbox;
	};

	class validated_materialization_request_v2_1;

	/**
	 * Source-independent v2.1 pass-two result without an all-task/source/payload representation.
	 *
	 * This type is deliberately not production execution authority. It proves the selected full
	 * schema and source-independent global/task metadata without retaining decoded source bytes.
	 * A fresh canonical Base64 source replay, its sealed receipt, and exact source-to-task.v3
	 * cross-binding remain mandatory in the later source-dependent stage.
	 */
	class prevalidated_materialization_request_v2_1
	{
	  public:
		prevalidated_materialization_request_v2_1(
			const prevalidated_materialization_request_v2_1&) = delete;
		prevalidated_materialization_request_v2_1&
		operator=(const prevalidated_materialization_request_v2_1&) = delete;
		prevalidated_materialization_request_v2_1(
			prevalidated_materialization_request_v2_1&&) noexcept;
		prevalidated_materialization_request_v2_1&
		operator=(prevalidated_materialization_request_v2_1&&) noexcept;
		~prevalidated_materialization_request_v2_1();

		[[nodiscard]] const materialization_v2_1_tool_authority& tool() const noexcept;
		[[nodiscard]] const materialization_v2_1_worker_authority& worker() const noexcept;
		[[nodiscard]] const std::string& project_id() const noexcept;
		[[nodiscard]] const sdk::project_catalog& catalog() const noexcept;
		[[nodiscard]] const sdk::relation_engine& engine() const noexcept;
		[[nodiscard]] const std::vector<sdk::relation_descriptor>&
		output_descriptors() const noexcept;
		[[nodiscard]] const validated_publication_request& publication() const noexcept;
		[[nodiscard]] std::uint64_t task_count() const noexcept;
		[[nodiscard]] std::uint64_t declared_source_bytes() const noexcept;
		/**
		 * Replay exactly one bounded metadata receipt; no decoded source authority is returned.
		 * The context's immutable `catalog()` remains the sole catalog owner during replay.
		 */
		[[nodiscard]] sdk::result<materialization_v2_1_task_metadata_receipt>
		task_metadata(std::uint64_t task_index);

	  private:
		prevalidated_materialization_request_v2_1(
			materialization_v2_1_tool_authority tool,
			materialization_v2_1_worker_authority worker,
			std::string project_id,
			sdk::project_catalog catalog,
			sdk::relation_engine engine,
			std::vector<sdk::relation_descriptor> output_descriptors,
			validated_publication_request publication,
			std::uint64_t task_count,
			std::uint64_t declared_source_bytes,
			std::unique_ptr<materialization_replayable_spool> raw_request,
			materialization_request_envelope envelope,
			std::unique_ptr<materialization_request_task_index> task_index);

		materialization_v2_1_tool_authority tool_;
		materialization_v2_1_worker_authority worker_;
		std::string project_id_;
		sdk::project_catalog catalog_;
		sdk::relation_engine engine_;
		std::vector<sdk::relation_descriptor> output_descriptors_;
		validated_publication_request publication_;
		std::uint64_t task_count_{};
		std::uint64_t declared_source_bytes_{};
		std::unique_ptr<materialization_replayable_spool> raw_request_;
		materialization_request_envelope envelope_;
		std::unique_ptr<materialization_request_task_index> task_index_;

		friend sdk::result<prevalidated_materialization_request_v2_1>
			prevalidate_materialization_request_v2_1(
				std::unique_ptr<materialization_replayable_spool>,
				materialization_request_envelope,
				std::unique_ptr<materialization_request_task_index>);
		friend sdk::result<prevalidated_materialization_request_v2_1>
		prevalidate_materialization_request_v2_1(
			std::unique_ptr<materialization_replayable_spool>,
			materialization_request_envelope,
			std::unique_ptr<materialization_request_task_index>,
			materialization_v2_1_auxiliary_spool_factory&);
		friend sdk::result<validated_materialization_request_v2_1>
			admit_materialization_request_v2_1(prevalidated_materialization_request_v2_1);
		friend sdk::result<validated_materialization_request_v2_1>
		admit_materialization_request_v2_1(prevalidated_materialization_request_v2_1,
										   materialization_v2_1_auxiliary_spool_factory&);
	};

	/**
	 * Validate the source-independent subset of one sealed v2.1 request by bounded pass-two replay.
	 *
	 * The legacy v2.0 all-request DOM validator is intentionally not a fallback. This API cannot
	 * authorize provider launch or publication; the later source/task stage must independently
	 * seal and cross-bind the canonical source receipt and exact task.v3 occurrence first.
	 */
	[[nodiscard]] sdk::result<prevalidated_materialization_request_v2_1>
	prevalidate_materialization_request_v2_1(
		std::unique_ptr<materialization_replayable_spool> raw_request,
		materialization_request_envelope envelope,
		std::unique_ptr<materialization_request_task_index> task_index);

	/** Dependency-injected form preserving the production phase taxonomy under spool faults. */
	[[nodiscard]] sdk::result<prevalidated_materialization_request_v2_1>
	prevalidate_materialization_request_v2_1(
		std::unique_ptr<materialization_replayable_spool> raw_request,
		materialization_request_envelope envelope,
		std::unique_ptr<materialization_request_task_index> task_index,
		materialization_v2_1_auxiliary_spool_factory& auxiliary_spools);

	/**
	 * Effect-free production admission token for one exact v2.1 request occurrence.
	 *
	 * Construction proves the selected full schema for every task before derived binding, then
	 * validates every independently sealed source receipt, canonical task.v3 occurrence, portable
	 * task/execution identity, duplicate execution census, and the three root request identities.
	 * No provider launch or Store effect is authorized by the prevalidated type alone.
	 */
	class validated_materialization_request_v2_1
	{
	  public:
		validated_materialization_request_v2_1(const validated_materialization_request_v2_1&) =
			delete;
		validated_materialization_request_v2_1&
		operator=(const validated_materialization_request_v2_1&) = delete;
		validated_materialization_request_v2_1(validated_materialization_request_v2_1&&) noexcept;
		validated_materialization_request_v2_1&
		operator=(validated_materialization_request_v2_1&&) noexcept;
		~validated_materialization_request_v2_1();

		[[nodiscard]] const prevalidated_materialization_request_v2_1& request() const noexcept;
		[[nodiscard]] const streamed_materialization_request_identity& identity() const noexcept;
		[[nodiscard]] sdk::result<materialization_v2_1_task_metadata_receipt>
		task_metadata(std::uint64_t task_index);

	  private:
		validated_materialization_request_v2_1(prevalidated_materialization_request_v2_1 request,
											   streamed_materialization_request_identity identity);

		prevalidated_materialization_request_v2_1 request_;
		streamed_materialization_request_identity identity_;

		friend sdk::result<validated_materialization_request_v2_1>
			admit_materialization_request_v2_1(prevalidated_materialization_request_v2_1);
		friend sdk::result<validated_materialization_request_v2_1>
		admit_materialization_request_v2_1(prevalidated_materialization_request_v2_1,
										   materialization_v2_1_auxiliary_spool_factory&);
	};

	/** Complete source-dependent admission after the effect-free metadata prevalidation phase. */
	[[nodiscard]] sdk::result<validated_materialization_request_v2_1>
	admit_materialization_request_v2_1(prevalidated_materialization_request_v2_1 request);

	/** Dependency-injected admission form for execution-index infrastructure verification. */
	[[nodiscard]] sdk::result<validated_materialization_request_v2_1>
	admit_materialization_request_v2_1(
		prevalidated_materialization_request_v2_1 request,
		materialization_v2_1_auxiliary_spool_factory& auxiliary_spools);

	/** Run the exact v2.1 prevalidation and source-dependent admission path without fallback. */
	[[nodiscard]] sdk::result<validated_materialization_request_v2_1>
	validate_materialization_request_v2_1(
		std::unique_ptr<materialization_replayable_spool> raw_request,
		materialization_request_envelope envelope,
		std::unique_ptr<materialization_request_task_index> task_index);

	/** Complete dependency-injected form used by private admission fault tests. */
	[[nodiscard]] sdk::result<validated_materialization_request_v2_1>
	validate_materialization_request_v2_1(
		std::unique_ptr<materialization_replayable_spool> raw_request,
		materialization_request_envelope envelope,
		std::unique_ptr<materialization_request_task_index> task_index,
		materialization_v2_1_auxiliary_spool_factory& auxiliary_spools);
} // namespace cxxlens::detail::clang22::materialization
