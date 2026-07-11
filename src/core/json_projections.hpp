#pragma once

#include "canonical_json.hpp"

namespace cxxlens
{
	struct source_span;
	struct error;
	struct unresolved;
	class evidence;
	class coverage_report;
	struct diagnostic;
} // namespace cxxlens

namespace cxxlens::detail::json
{
	[[nodiscard]] json_value source_span_value(const source_span& span);
	[[nodiscard]] json_value error_value(const error& failure);
	[[nodiscard]] json_value unresolved_value(const unresolved& item);
	[[nodiscard]] json_value evidence_value(const evidence& why);
	[[nodiscard]] json_value coverage_value(const coverage_report& coverage);
	[[nodiscard]] json_value diagnostic_value(const diagnostic& observation);
} // namespace cxxlens::detail::json
