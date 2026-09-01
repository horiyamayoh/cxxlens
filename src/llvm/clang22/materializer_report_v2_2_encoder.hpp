#pragma once

/**
 * @file materializer_report_v2_2_encoder.hpp
 * @brief Concrete installed-worker projection for the v2.2 materialization response.
 *
 * The encoder consumes only the immutable authorities issued by request admission, occurrence
 * measurement, the sealed provider transcript, and the reopened Store publication.  It never
 * reconstructs a success response from a process exit code or from diagnostic frame text.
 */

#include <string>

#include "materialization_json.hpp"
#include "materialization_occurrence.hpp"
#include "materializer_worker_bridge.hpp"
#include "sdk/materialization_io_internal.hpp"

namespace cxxlens::detail::clang22
{
	[[nodiscard]] sdk::result<std::string> encode_materializer_v2_2_success_report(
		const sdk::detail::raw_input_observation& raw_input,
		const materialization::json_value& request_root,
		const materializer_store_execution& execution,
		const materialization::measured_materialization_occurrence& occurrence);
} // namespace cxxlens::detail::clang22
