#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/sdk/provider.hpp>

#include "provider_protocol_v2_adapter.hpp"
#include "provider_validation_internal.hpp"

namespace cxxlens::sdk::provider::detail
{
	/**
	 * Source-private process inheritance authority for one Protocol 2.0 source-closure channel.
	 *
	 * The parent owns the descriptors.  The Linux process adapter borrows them for the child and
	 * closes every other descriptor in the inherited range.  The fstat snapshot is part of the
	 * binding digest so descriptor-number reuse cannot silently attach a foreign channel.
	 */
	struct CXXLENS_PROVIDER_DETAIL_HIDDEN process_inherited_channel_binding
	{
		int read_descriptor{-1};
		int write_descriptor{-1};
		std::string task_id;
		std::string session_id;
		std::string task_v4_digest;
		std::string closure_id;
		std::string closure_digest;
		std::string manifest_digest;
		std::string transfer_digest;
		std::string binding_digest;
		std::uint64_t stream_id{};
		std::uint64_t first_sequence{};
		std::uint64_t read_device{};
		std::uint64_t read_inode{};
		std::uint32_t read_mode{};
		std::uint64_t write_device{};
		std::uint64_t write_inode{};
		std::uint32_t write_mode{};

		[[nodiscard]] result<void> validate() const;
	};

	/** Create a validated borrowed source-closure channel binding for a process invocation. */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN
		result<std::shared_ptr<const process_inherited_channel_binding>>
		make_process_inherited_channel_binding(int read_descriptor,
											   int write_descriptor,
											   std::string task_id,
											   std::string session_id,
											   std::string task_v4_digest,
											   std::string closure_id,
											   std::string closure_digest,
											   std::string manifest_digest,
											   std::string transfer_digest,
											   std::uint64_t stream_id,
											   std::uint64_t first_sequence);

	struct process_source_closure_generation_state;

	/** Move-only custody transferred from a launch view to the process adapter. */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN process_source_closure_descriptor_projection final
	{
	  public:
		process_source_closure_descriptor_projection(
			const process_source_closure_descriptor_projection&) = delete;
		process_source_closure_descriptor_projection&
		operator=(const process_source_closure_descriptor_projection&) = delete;
		process_source_closure_descriptor_projection(
			process_source_closure_descriptor_projection&& other) noexcept;
		process_source_closure_descriptor_projection&
		operator=(process_source_closure_descriptor_projection&&) = delete;
		~process_source_closure_descriptor_projection() noexcept;

	  private:
		process_source_closure_descriptor_projection(
			int read_descriptor,
			int write_descriptor,
			std::uint64_t read_device,
			std::uint64_t read_inode,
			std::uint32_t read_mode,
			std::uint64_t write_device,
			std::uint64_t write_inode,
			std::uint32_t write_mode,
			std::unique_ptr<process_source_closure_generation_state> state,
			std::uint64_t generation) noexcept;

		void close_owned() noexcept;

		int read_descriptor_{-1};
		int write_descriptor_{-1};
		std::uint64_t read_device_{};
		std::uint64_t read_inode_{};
		std::uint32_t read_mode_{};
		std::uint64_t write_device_{};
		std::uint64_t write_inode_{};
		std::uint32_t write_mode_{};
		std::unique_ptr<process_source_closure_generation_state> state_;
		std::uint64_t generation_{};
		bool adapter_consumed_{};

		friend struct process_source_closure_launch_adapter_access;
	};

	/**
	 * One-shot descriptor view issued by the host source-closure launch lease.
	 *
	 * The view owns the private descriptors transferred by the lease.  It has no public
	 * descriptor access; the process adapter consumes it through the narrow friend below.
	 */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN process_source_closure_launch_view final
	{
	  public:
		process_source_closure_launch_view(const process_source_closure_launch_view&) = delete;
		process_source_closure_launch_view&
		operator=(const process_source_closure_launch_view&) = delete;
		process_source_closure_launch_view(process_source_closure_launch_view&& other) noexcept;
		process_source_closure_launch_view&
		operator=(process_source_closure_launch_view&&) = delete;
		~process_source_closure_launch_view() noexcept;

	  private:
		process_source_closure_launch_view(
			int read_descriptor,
			int write_descriptor,
			std::string task_id,
			std::string session_id,
			std::string task_v4_digest,
			std::string closure_id,
			std::string closure_digest,
			std::string manifest_digest,
			std::string transfer_digest,
			std::string binding_digest,
			std::uint64_t stream_id,
			std::uint64_t first_sequence,
			std::uint64_t read_device,
			std::uint64_t read_inode,
			std::uint32_t read_mode,
			std::uint64_t write_device,
			std::uint64_t write_inode,
			std::uint32_t write_mode,
			std::unique_ptr<process_source_closure_generation_state> state) noexcept;

		int read_descriptor_{-1};
		int write_descriptor_{-1};
		std::string task_id_;
		std::string session_id_;
		std::string task_v4_digest_;
		std::string closure_id_;
		std::string closure_digest_;
		std::string manifest_digest_;
		std::string transfer_digest_;
		std::string binding_digest_;
		std::uint64_t stream_id_{};
		std::uint64_t first_sequence_{};
		std::uint64_t read_device_{};
		std::uint64_t read_inode_{};
		std::uint32_t read_mode_{};
		std::uint64_t write_device_{};
		std::uint64_t write_inode_{};
		std::uint32_t write_mode_{};
		std::unique_ptr<process_source_closure_generation_state> state_;
		std::uint64_t generation_{};
		bool adapter_accessed_{};

		friend class process_source_closure_launch;
		friend struct process_source_closure_launch_adapter_access;
	};

	/**
	 * Source-private host launch lease for one authenticated source-closure channel.
	 *
	 * The lease owns private F_DUPFD_CLOEXEC descriptors and is the only issuer of the
	 * measured channel state.  A successful rvalue claim consumes the lease and returns one
	 * move-only descriptor view for the process adapter.
	 */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN process_source_closure_launch final
	{
	  public:
		process_source_closure_launch(const process_source_closure_launch&) = delete;
		process_source_closure_launch& operator=(const process_source_closure_launch&) = delete;
		process_source_closure_launch(process_source_closure_launch&& other) noexcept;
		process_source_closure_launch& operator=(process_source_closure_launch&&) = delete;
		~process_source_closure_launch() noexcept;

		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] result<process_source_closure_launch_view> claim_launch() &&;
		[[nodiscard]] bool validate_live_endpoint_identity() const noexcept;
		void invalidate() const noexcept;
		[[nodiscard]] std::string_view task_id() const noexcept
		{
			return task_id_;
		}
		[[nodiscard]] std::string_view session_id() const noexcept
		{
			return session_id_;
		}
		[[nodiscard]] std::string_view task_v4_digest() const noexcept
		{
			return task_v4_digest_;
		}
		[[nodiscard]] std::string_view closure_id() const noexcept
		{
			return closure_id_;
		}
		[[nodiscard]] std::string_view closure_digest() const noexcept
		{
			return closure_digest_;
		}
		[[nodiscard]] std::string_view manifest_digest() const noexcept
		{
			return manifest_digest_;
		}
		[[nodiscard]] std::string_view transfer_digest() const noexcept
		{
			return transfer_digest_;
		}
		[[nodiscard]] std::string_view binding_digest() const noexcept
		{
			return binding_digest_;
		}
		[[nodiscard]] std::uint64_t stream_id() const noexcept
		{
			return stream_id_;
		}
		[[nodiscard]] std::uint64_t first_sequence() const noexcept
		{
			return first_sequence_;
		}
		[[nodiscard]] std::uint64_t launch_generation() const noexcept
		{
			return generation_;
		}

	  private:
		process_source_closure_launch(
			int read_descriptor,
			int write_descriptor,
			std::string task_id,
			std::string session_id,
			std::string task_v4_digest,
			std::string closure_id,
			std::string closure_digest,
			std::string manifest_digest,
			std::string transfer_digest,
			std::string binding_digest,
			std::uint64_t stream_id,
			std::uint64_t first_sequence,
			std::uint64_t read_device,
			std::uint64_t read_inode,
			std::uint32_t read_mode,
			std::uint64_t write_device,
			std::uint64_t write_inode,
			std::uint32_t write_mode,
			std::unique_ptr<process_source_closure_generation_state> state) noexcept;

		int read_descriptor_{-1};
		int write_descriptor_{-1};
		std::string task_id_;
		std::string session_id_;
		std::string task_v4_digest_;
		std::string closure_id_;
		std::string closure_digest_;
		std::string manifest_digest_;
		std::string transfer_digest_;
		std::string binding_digest_;
		std::uint64_t stream_id_{};
		std::uint64_t first_sequence_{};
		std::uint64_t read_device_{};
		std::uint64_t read_inode_{};
		std::uint32_t read_mode_{};
		std::uint64_t write_device_{};
		std::uint64_t write_inode_{};
		std::uint32_t write_mode_{};
		std::unique_ptr<process_source_closure_generation_state> state_;
		std::uint64_t generation_{};

		friend result<process_source_closure_launch>
		make_process_source_closure_launch(int,
										   int,
										   std::string,
										   std::string,
										   std::string,
										   std::string,
										   std::string,
										   std::string,
										   std::string,
										   std::uint64_t,
										   std::uint64_t);
	};

	/** Narrow source-private adapter access; raw descriptors exist only during the callback. */
	struct CXXLENS_PROVIDER_DETAIL_HIDDEN process_source_closure_launch_adapter_access
	{
		using descriptor_operation = result<void> (*)(void*, int, int);

		[[nodiscard]] static result<process_source_closure_descriptor_projection>
		consume(process_source_closure_launch_view&& value);
		[[nodiscard]] static result<void>
		consume(process_source_closure_descriptor_projection&& value,
				void* context,
				descriptor_operation operation);
	};

	/** Issue one validated, move-only host launch lease. */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<process_source_closure_launch>
	make_process_source_closure_launch(int read_descriptor,
									   int write_descriptor,
									   std::string task_id,
									   std::string session_id,
									   std::string task_v4_digest,
									   std::string closure_id,
									   std::string closure_digest,
									   std::string manifest_digest,
									   std::string transfer_digest,
									   std::uint64_t stream_id,
									   std::uint64_t first_sequence);

	/**
	 * Independent Protocol 2.0 source-closure channel owned by the provider runtime.
	 *
	 * This is deliberately not part of `validate_provider_transcript`: provider output frames and
	 * host-to-provider closure frames have different channels, sequence guards, and terminal
	 * semantics.  A worker may retain this typed state and call `accept` for each decoded closure
	 * frame; only `acknowledged()` exposes an execution-ready closure authority.
	 */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN provider_runtime_closure_channel
	{
	  public:
		[[nodiscard]] static result<provider_runtime_closure_channel>
		create(provider_protocol_v2_session session);

		[[nodiscard]] result<void> accept(const frame& value);
		[[nodiscard]] provider_protocol_v2_phase phase() const noexcept
		{
			return state_.phase();
		}
		[[nodiscard]] bool acknowledged() const noexcept
		{
			return phase() == provider_protocol_v2_phase::acknowledged;
		}
		[[nodiscard]] bool rejected() const noexcept
		{
			return phase() == provider_protocol_v2_phase::rejected;
		}
		[[nodiscard]] const provider_protocol_v2_session& session() const noexcept
		{
			return session_;
		}

	  private:
		provider_runtime_closure_channel(provider_protocol_v2_closure_state state,
										 provider_protocol_v2_session session)
			: state_{std::move(state)}, session_{std::move(session)}
		{
		}

		provider_protocol_v2_closure_state state_;
		provider_protocol_v2_session session_;
	};

	/**
	 * Source-private identity carried by a shared runtime validation pass.  Empty optional
	 * projections are permitted for the generic NG0 runtime, but the NG1 replay bridge requires
	 * every field to be present and compares it with the host resume binding.
	 */
	struct CXXLENS_PROVIDER_DETAIL_HIDDEN provider_runtime_provenance
	{
		std::string provider_id;
		semantic_version provider_version;
		std::string provider_binary_digest;
		std::string provider_semantic_contract_digest;
		std::string protocol_session_id;
		std::string task_id;
		std::string task_input_digest;
		std::string normalized_invocation_digest;
		std::string toolchain_digest;
		std::string environment_digest;
		std::string sandbox_policy_digest;
		std::string dependency_group_id;
		std::string atomic_output_group_id;
		std::string batch_id;
		std::uint64_t stream_id{};
	};

	/**
	 * Exact source-private batch projection used by the sealed-transcript receipt.
	 *
	 * This deliberately contains the canonical row forms rather than a report-specific row-set
	 * digest.  A materialization report can therefore reprove that its retained leaf is the same
	 * projection that the shared runtime sealed, without reimplementing the receipt codec or using
	 * a diagnostic process digest as authority.
	 */
	struct CXXLENS_PROVIDER_DETAIL_HIDDEN provider_sealed_transcript_batch_receipt_projection
	{
		std::string task_id;
		std::string descriptor_id;
		std::string descriptor_digest;
		std::string dependency_group_id;
		std::string atomic_output_group_id;
		std::string batch_id;
		std::string batch_digest;
		std::vector<std::string> ordered_chunk_digests;
		std::vector<std::string> row_canonical_forms;
	};

	/** Closed runtime-owned evidence derived from raw bytes, decoded frames, and one immutable
	 * seal. */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN provider_runtime_receipt
	{
	  public:
		[[nodiscard]] std::uint64_t raw_stdout_byte_count() const noexcept;
		[[nodiscard]] std::string_view raw_stdout_sha256() const noexcept;
		[[nodiscard]] std::uint64_t decoded_frame_count() const noexcept;
		/** Exact sequence of the first decoded frame retained by this receipt. */
		[[nodiscard]] std::uint64_t first_frame_sequence() const noexcept;
		[[nodiscard]] std::string_view frame_transcript_digest() const noexcept;
		[[nodiscard]] std::string_view sealed_transcript_digest() const noexcept;
		[[nodiscard]] const provider_runtime_provenance& provenance() const noexcept;
		/** Revalidate the opaque raw/frame/sealed receipt before a source-private handoff. */
		[[nodiscard]] result<void> validate() const;

	  private:
		provider_runtime_receipt(std::uint64_t raw_stdout_byte_count,
								 std::string raw_stdout_sha256,
								 std::uint64_t decoded_frame_count,
								 std::uint64_t first_frame_sequence,
								 std::string frame_transcript_digest,
								 std::string sealed_transcript_digest,
								 provider_runtime_provenance provenance);

		std::uint64_t raw_stdout_byte_count_{};
		std::string raw_stdout_sha256_;
		std::uint64_t decoded_frame_count_{};
		std::uint64_t first_frame_sequence_{};
		std::string frame_transcript_digest_;
		std::string sealed_transcript_digest_;
		provider_runtime_provenance provenance_;

		friend result<provider_runtime_receipt>
		make_provider_runtime_receipt(std::uint64_t,
									  std::string,
									  std::span<const frame>,
									  std::string_view,
									  std::string_view,
									  const sealed_provider_transcript&);
		friend result<provider_runtime_receipt>
		make_provider_runtime_receipt(std::uint64_t,
									  std::string,
									  std::span<const frame>,
									  provider_runtime_provenance,
									  std::string_view,
									  const sealed_provider_transcript&);
	};

	/** Construct the runtime receipt in the same pass that owns the immutable output seal. */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<provider_runtime_receipt>
	make_provider_runtime_receipt(std::uint64_t raw_stdout_byte_count,
								  std::string raw_stdout_sha256,
								  std::span<const frame> frames,
								  std::string_view task_id,
								  std::string_view terminal,
								  const sealed_provider_transcript& sealed);

	/** Construct a runtime receipt with the complete source-private NG1 identity projection. */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<provider_runtime_receipt>
	make_provider_runtime_receipt(std::uint64_t raw_stdout_byte_count,
								  std::string raw_stdout_sha256,
								  std::span<const frame> frames,
								  provider_runtime_provenance provenance,
								  std::string_view terminal,
								  const sealed_provider_transcript& sealed);

	/** Recompute the sealed-transcript receipt projection for a source-private consumer. */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<std::string>
	provider_sealed_transcript_receipt_digest(std::string_view task_id,
											  std::string_view terminal,
											  const sealed_provider_transcript& sealed);
	/** Recompute the same receipt from a lossless source-private report leaf projection. */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<std::string>
	provider_sealed_transcript_receipt_digest(
		std::string_view task_id,
		std::string_view terminal,
		std::span<const provider_sealed_transcript_batch_receipt_projection> batches,
		std::span<const coverage_unit> coverage,
		std::span<const unresolved_item> unresolved,
		std::span<const evidence_item> evidence);
	/** Recompute the exact frame transcript receipt from a bounded decoded frame stream. */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<std::string>
	provider_frame_transcript_receipt_digest(std::span<const frame> frames);

	/**
	 * Source-private process extension that lets the runtime write a replayed transcript directly
	 * into a sealed process-input occurrence. Implementations must not retain the writer callback.
	 */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN replayable_provider_process_port
	{
	  public:
		using input_writer = std::function<result<void>(frame_sink&)>;
		virtual ~replayable_provider_process_port() = default;
		[[nodiscard]] virtual result<process_output>
		run_replayable(const process_invocation& invocation,
					   const input_writer& write_input,
					   std::stop_token cancellation) const = 0;
	};

	/** Source-private system process port with replayable stdin construction. */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN std::unique_ptr<replayable_provider_process_port>
	make_system_replayable_provider_process_port();

	/**
	 * Tool-private result of one process launch and the shared transcript validation pass.
	 *
	 * Frames remain diagnostic evidence.  Only `sealed` is row-adoption authority, and it is absent
	 * for transport, terminal, transcript, or row/domain-validation failure.
	 */
	struct CXXLENS_PROVIDER_DETAIL_HIDDEN provider_process_validation_outcome
	{
		std::string terminal;
		manifest provider;
		std::string task_input_digest;
		std::string normalized_invocation_digest;
		std::string toolchain_digest;
		std::string environment_digest;
		std::string measured_executable_digest;
		sandbox_report sandbox;
		/** Exact successful stdout bytes retained for a source-private durable replay receipt. */
		std::vector<std::byte> raw_frame_stream;
		std::vector<frame> frames;
		std::vector<unresolved_item> diagnostics;
		int exit_code{};
		int termination_signal{};
		bool validated_transcript_success{};
		std::optional<sealed_host_input> input_seal;
		std::optional<sealed_provider_transcript> sealed;
		std::optional<expected_provider_identity> provider_identity;
		std::optional<provider_runtime_receipt> runtime_receipt;
		std::optional<error> sealing_error;

		[[nodiscard]] bool succeeded() const noexcept
		{
			return validated_transcript_success && terminal == "provider.success" && sealed &&
				runtime_receipt && !frames.empty() &&
				frames.back().type == message_type::task_complete;
		}
	};

	/**
	 * Validate the source-private handoff from an accepted process task to materialization.
	 *
	 * The validator binds the selected provider, task/input digests, sealed input, and runtime
	 * receipt before a caller may adopt the sealed provider transcript.  It does not derive or
	 * accept a provider-execution identity; that identity remains owned by materialization
	 * admission.
	 */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<void>
	validate_provider_process_runtime_binding(const provider_process_validation_outcome& outcome,
											  const process_task_request& request);

	/** Launch once and share the exact typed validation pass with the public process runtime. */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<provider_process_validation_outcome>
	execute_provider_process(const provider_process_port& processes,
							 const process_task_request& request);

	/** Execute from a replayable logical input without materializing the host transcript vector. */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<provider_process_validation_outcome>
	execute_provider_process_replayable(const replayable_provider_process_port& processes,
										const process_task_request& request,
										const replayable_host_input& input);
} // namespace cxxlens::sdk::provider::detail
