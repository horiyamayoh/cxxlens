#pragma once

/** @file testing.hpp @brief In-memory provider conformance and fault-injection harness. */

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/provider.hpp>

namespace cxxlens::sdk::testing
{
	/** @brief Stable provider harness fault. */
	enum class provider_fault : std::uint8_t
	{
		none,
		no_credit,
		cancel_before_run,
		corrupt_checksum,
		truncate_last_frame,
		wrong_direction,
		drop_terminal,
		wrong_terminal_task,
		credit_exceeded,
	};
	/** @brief Test exact membership in the closed provider fault enum. */
	[[nodiscard]] constexpr bool is_valid(const provider_fault value) noexcept
	{
		return value >= provider_fault::none && value <= provider_fault::credit_exceeded;
	}

	/** @brief Canonical conformance observation. */
	struct conformance_report
	{
		bool accepted{};
		std::string reason_code;
		std::vector<provider::frame> frames;
		std::string semantic_transcript;
	};

	/** @brief Apply the production typed logical-stream validator to decoded frames. */
	[[nodiscard]] result<conformance_report>
	validate_logical_transcript(const provider::task& task,
								std::string_view provider_id,
								semantic_version provider_version,
								std::span<const provider::frame> frames,
								provider::protocol_credit credit);

	/** @brief Apply the same validator to a full provider-to-host process transcript. */
	[[nodiscard]] result<conformance_report>
	validate_process_transcript(const provider::task& task,
								const provider::manifest& manifest,
								std::span<const provider::frame> frames,
								provider::protocol_credit credit,
								provider::protocol_limits limits = {});

	/** @brief Deterministic in-memory client/server harness using production frame codecs. */
	class provider_harness
	{
	  public:
		[[nodiscard]] result<conformance_report>
		run(provider::portable_provider& implementation,
			const provider::task& task,
			provider_fault fault = provider_fault::none) const;
	};

	/** @brief Compare one conformance transcript to an exact golden value. */
	[[nodiscard]] result<void> require_golden(const conformance_report& report,
											  std::string_view expected);
} // namespace cxxlens::sdk::testing
