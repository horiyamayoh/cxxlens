#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <cxxlens/sdk/provider.hpp>

#include "provider_validation_internal.hpp"

namespace cxxlens::sdk::provider::detail
{
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

	/** Closed runtime-owned evidence derived from raw bytes, decoded frames, and one immutable
	 * seal. */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN provider_runtime_receipt
	{
	  public:
		[[nodiscard]] std::uint64_t raw_stdout_byte_count() const noexcept;
		[[nodiscard]] std::string_view raw_stdout_sha256() const noexcept;
		[[nodiscard]] std::uint64_t decoded_frame_count() const noexcept;
		[[nodiscard]] std::string_view frame_transcript_digest() const noexcept;
		[[nodiscard]] std::string_view sealed_transcript_digest() const noexcept;
		[[nodiscard]] const provider_runtime_provenance& provenance() const noexcept;

	  private:
		provider_runtime_receipt(std::uint64_t raw_stdout_byte_count,
								 std::string raw_stdout_sha256,
								 std::uint64_t decoded_frame_count,
								 std::string frame_transcript_digest,
								 std::string sealed_transcript_digest,
								 provider_runtime_provenance provenance);

		std::uint64_t raw_stdout_byte_count_{};
		std::string raw_stdout_sha256_;
		std::uint64_t decoded_frame_count_{};
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
