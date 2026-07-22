#pragma once

#include <string>

#include <cxxlens/sdk/common.hpp>

#include "materialization_execution_journal.hpp"

namespace cxxlens::detail::clang22::materialization
{
	/**
	 * Encode the exact compact-failure branch of materialization report v2.1.
	 *
	 * The only semantic input is a non-forgeable token issued by the consumed execution journal.
	 * No caller-constructed phase, effect ledger, or publication-dependent value is accepted.
	 */
	[[nodiscard]] sdk::result<std::string>
	encode_compact_failure_report(const compact_failure_authority& authority,
								  std::string generated_at);
} // namespace cxxlens::detail::clang22::materialization
