#pragma once

#include <optional>
#include <string>
#include <vector>

#include <cxxlens/sdk/provider.hpp>

#include "provider_validation_internal.hpp"

namespace cxxlens::sdk::provider::detail
{
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
		std::optional<sealed_provider_transcript> sealed;
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
} // namespace cxxlens::sdk::provider::detail
