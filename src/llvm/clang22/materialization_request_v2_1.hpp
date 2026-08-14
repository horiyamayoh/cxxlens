#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <cxxlens/sdk/provider.hpp>

#include "materialization_request.hpp"
#include "materialization_request_identity.hpp"
#include "materialization_request_stream.hpp"
#include "materialization_task_spool.hpp"

namespace cxxlens::detail::clang22::materialization
{
	class validated_materialization_request_v2_1;

	/** Opaque state which makes source-private request authorities fail closed on move/destruction.
	 */
	class materialization_v2_1_request_lifetime
	{
	  public:
		[[nodiscard]] validated_materialization_request_v2_1* owner() const noexcept
		{
			return owner_;
		}

	  private:
		void bind(validated_materialization_request_v2_1* owner) noexcept
		{
			owner_ = owner;
		}
		void invalidate() noexcept
		{
			owner_ = nullptr;
		}

		validated_materialization_request_v2_1* owner_{};
		friend class validated_materialization_request_v2_1;
	};

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
		std::string catalog_id;
		std::string catalog_digest;
		std::string selected_catalog_compile_unit_id;
		std::string final_relation_compile_unit_id;
		std::string variant_id;
		std::string toolchain_context_id;
		std::string toolchain_digest;
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

		[[nodiscard]] bool
		operator==(const materialization_v2_1_task_metadata_receipt& other) const noexcept
		{
			return task_index == other.task_index && project_id == other.project_id &&
				catalog_id == other.catalog_id && catalog_digest == other.catalog_digest &&
				selected_catalog_compile_unit_id == other.selected_catalog_compile_unit_id &&
				final_relation_compile_unit_id == other.final_relation_compile_unit_id &&
				variant_id == other.variant_id &&
				toolchain_context_id == other.toolchain_context_id &&
				toolchain_digest == other.toolchain_digest &&
				source_snapshot_id == other.source_snapshot_id && file_id == other.file_id &&
				logical_path == other.logical_path &&
				source_content_digest == other.source_content_digest &&
				source_size_bytes == other.source_size_bytes &&
				source_encoding == other.source_encoding && line_index_id == other.line_index_id &&
				source_read_only == other.source_read_only &&
				condition_universe_id == other.condition_universe_id &&
				condition_id == other.condition_id &&
				interpretation_domain == other.interpretation_domain &&
				provider_task_id == other.provider_task_id &&
				provider_execution_id == other.provider_execution_id &&
				task_input_digest == other.task_input_digest &&
				sandbox.minimum == other.sandbox.minimum &&
				sandbox.policy_digest == other.sandbox.policy_digest;
		}
	};

	/**
	 * Source-independent task binding used by planning and claim-adoption metadata paths.
	 *
	 * The decoded input contains authority metadata only: source and canonical task.v3 bytes must
	 * remain empty.  This value deliberately contains no sealed source receipt; that authority is
	 * created only by source-dependent replay.  It never owns a source or task-input spool and is
	 * therefore safe to use for a bounded metadata replay.
	 */
	struct materialization_v2_1_task_metadata_binding
	{
		clang22_task_input input;
		materialization_v2_1_task_metadata_receipt metadata;
	};

	/** Opaque lifetime token held by a task-at-a-time cursor result. */
	struct materialization_v2_1_task_cursor_state;

	/**
	 * One source-dependent task replay issued only after complete v2.1 admission.
	 *
	 * The logical task.v3 bytes and decoded source remain in independent sealed private spools;
	 * neither is converted to a resident request-wide vector.  The token is source-private and is
	 * consumed by the installed materializer execution boundary.
	 */
	struct materialization_v2_1_task_execution
	{
		materialization_v2_1_task_execution(clang22_task_input input,
											materialization_v2_1_task_metadata_receipt metadata,
											clang22_task_source_receipt source_receipt,
											std::unique_ptr<clang22_task_source_spool> source,
											std::unique_ptr<clang22_task_input_spool> task_input);
		materialization_v2_1_task_execution(const materialization_v2_1_task_execution&) = delete;
		materialization_v2_1_task_execution&
		operator=(const materialization_v2_1_task_execution&) = delete;
		materialization_v2_1_task_execution(materialization_v2_1_task_execution&&) noexcept =
			default;
		materialization_v2_1_task_execution&
		operator=(materialization_v2_1_task_execution&&) noexcept = default;
		~materialization_v2_1_task_execution() = default;

		clang22_task_input input;
		materialization_v2_1_task_metadata_receipt metadata;
		clang22_task_source_receipt source_receipt;
		std::unique_ptr<clang22_task_source_spool> source;
		std::unique_ptr<clang22_task_input_spool> task_input;
		/** Set only after the owner has independently consumed both sealed window spools. */
		bool source_window_sealed{};
		bool task_input_window_sealed{};

	  private:
		/**
		 * Source-private lease installed only by materialization_v2_1_task_cursor.  It is
		 * deliberately opaque to downstream code and moves with the complete task binding, so the
		 * cursor cannot advance while any bounded task window is still owned by a consumer.
		 */
		std::shared_ptr<void> cursor_lease;

		void attach_cursor_lease(std::shared_ptr<void> lease) noexcept;
		friend class materialization_v2_1_task_cursor;
	};

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
		/** Replay one source-independent task binding without opening source-dependent spools. */
		[[nodiscard]] sdk::result<materialization_v2_1_task_metadata_binding>
		task_metadata_binding(std::uint64_t task_index);

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
		friend class validated_materialization_request_v2_1;
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
		/** Source-private lifetime state used by bounded authorities; never exposes request
		 * payload. */
		[[nodiscard]] const std::shared_ptr<materialization_v2_1_request_lifetime>&
		lifetime_token() const noexcept;
		/** Replay only the authenticated, source-independent global request authority. */
		[[nodiscard]] sdk::result<json_document> replay_global_authority();
		[[nodiscard]] sdk::result<materialization_v2_1_task_metadata_receipt>
		task_metadata(std::uint64_t task_index);
		/** Replay one source-independent task binding without opening source-dependent spools. */
		[[nodiscard]] sdk::result<materialization_v2_1_task_metadata_binding>
		task_metadata_binding(std::uint64_t task_index);
		/** Replay one fully bound task without materializing its source or task.v3 bytes. */
		[[nodiscard]] sdk::result<materialization_v2_1_task_execution>
		task_execution(std::uint64_t task_index);

	  private:
		validated_materialization_request_v2_1(prevalidated_materialization_request_v2_1 request,
											   streamed_materialization_request_identity identity);

		prevalidated_materialization_request_v2_1 request_;
		streamed_materialization_request_identity identity_;
		std::shared_ptr<materialization_v2_1_request_lifetime> lifetime_;

		friend sdk::result<validated_materialization_request_v2_1>
			admit_materialization_request_v2_1(prevalidated_materialization_request_v2_1);
		friend sdk::result<validated_materialization_request_v2_1>
		admit_materialization_request_v2_1(prevalidated_materialization_request_v2_1,
										   materialization_v2_1_auxiliary_spool_factory&);
	};

	/**
	 * Move-only, source-private replay of exactly one admitted v2.1 task at a time.
	 *
	 * `next()` follows the sealed canonical task index.  The returned execution owns the source and
	 * task-input spools for one bounded validation window; its opaque lease follows moves and must
	 * be destroyed before the next call.  A live binding, a replay error, or an incomplete final
	 * census fails closed.  No request-wide task vector is constructed by this source.
	 */
	class materialization_v2_1_task_cursor final
	{
	  public:
		materialization_v2_1_task_cursor(const materialization_v2_1_task_cursor&) = delete;
		materialization_v2_1_task_cursor&
		operator=(const materialization_v2_1_task_cursor&) = delete;
		materialization_v2_1_task_cursor(materialization_v2_1_task_cursor&&) noexcept;
		materialization_v2_1_task_cursor& operator=(materialization_v2_1_task_cursor&&) noexcept;
		~materialization_v2_1_task_cursor();

		[[nodiscard]] const streamed_materialization_request_identity& identity() const noexcept;
		[[nodiscard]] std::uint64_t task_count() const noexcept;
		[[nodiscard]] std::uint64_t next_task_index() const noexcept;

		/** Return the exact next task, or empty only after the complete task census is consumed. */
		[[nodiscard]] sdk::result<std::optional<materialization_v2_1_task_execution>> next();

		/** Seal the cursor lifecycle only after every task binding has been released. */
		[[nodiscard]] sdk::result<void> finalize() &&;

	  private:
		materialization_v2_1_task_cursor(
			validated_materialization_request_v2_1& request,
			std::shared_ptr<materialization_v2_1_task_cursor_state> state) noexcept;

		validated_materialization_request_v2_1* request_{};
		std::shared_ptr<materialization_v2_1_task_cursor_state> state_;

		friend sdk::result<materialization_v2_1_task_cursor>
		make_materialization_v2_1_task_cursor(validated_materialization_request_v2_1&);
	};

	/** Begin the bounded source-private task replay for one already admitted request. */
	[[nodiscard]] sdk::result<materialization_v2_1_task_cursor>
	make_materialization_v2_1_task_cursor(validated_materialization_request_v2_1& request);

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
