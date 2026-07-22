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

	  private:
		provider_runtime_receipt(std::uint64_t raw_stdout_byte_count,
								 std::string raw_stdout_sha256,
								 std::uint64_t decoded_frame_count,
								 std::string frame_transcript_digest,
								 std::string sealed_transcript_digest);

		std::uint64_t raw_stdout_byte_count_{};
		std::string raw_stdout_sha256_;
		std::uint64_t decoded_frame_count_{};
		std::string frame_transcript_digest_;
		std::string sealed_transcript_digest_;

		friend result<provider_runtime_receipt>
		make_provider_runtime_receipt(std::uint64_t,
									  std::string,
									  std::span<const frame>,
									  std::string_view,
									  std::string_view,
									  const sealed_provider_transcript&);
	};

	/** Construct the five-field receipt in the same pass that owns the immutable output seal. */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<provider_runtime_receipt>
	make_provider_runtime_receipt(std::uint64_t raw_stdout_byte_count,
								  std::string raw_stdout_sha256,
								  std::span<const frame> frames,
								  std::string_view task_id,
								  std::string_view terminal,
								  const sealed_provider_transcript& sealed);

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
				!frames.empty() && frames.back().type == message_type::task_complete;
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
