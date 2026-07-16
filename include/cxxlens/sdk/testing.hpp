#pragma once

/** @file testing.hpp @brief In-memory provider conformance and fault-injection harness. */

#include <cstdint>
#include <string>
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
	};

	/** @brief Canonical conformance observation. */
	struct conformance_report
	{
		bool accepted{};
		std::string reason_code;
		std::vector<provider::frame> frames;
		std::string semantic_transcript;
	};

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
