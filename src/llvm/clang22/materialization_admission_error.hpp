#pragma once

#include <string>
#include <string_view>
#include <utility>

#include <cxxlens/sdk/common.hpp>

#include "materialization_io.hpp"

namespace cxxlens::detail::clang22::materialization
{
	/** Source-private process outcome; never a stable report error or compact-response code. */
	inline constexpr std::string_view materialization_admission_no_response_code = "no-response";

	[[nodiscard]] inline sdk::error materialization_admission_no_response()
	{
		return {std::string{materialization_admission_no_response_code}, {}, {}};
	}

	[[nodiscard]] inline bool
	is_materialization_admission_no_response(const sdk::error& error) noexcept
	{
		return error.code == materialization_admission_no_response_code;
	}

	[[nodiscard]] inline sdk::error materialization_admission_spool_failure(std::string phase,
																			std::string detail)
	{
		return {"materialization.spool-failure", std::move(phase), std::move(detail)};
	}

	[[nodiscard]] inline sdk::error materialization_admission_io_failure(
		const materialization_io_failure& failure, std::string phase, std::string detail)
	{
		if (!is_materialization_actual_io_or_hash_failure(failure))
			return materialization_admission_no_response();
		return materialization_admission_spool_failure(std::move(phase), std::move(detail));
	}

	[[nodiscard]] inline sdk::error normalize_materialization_admission_spool_failure(
		sdk::error error, const std::string_view phase, const std::string_view context)
	{
		if (is_materialization_admission_no_response(error))
			return error;
		if (error.code != "materialization.spool-failure")
			return materialization_admission_no_response();
		std::string detail{context};
		if (!error.field.empty())
			detail += ":" + error.field;
		if (!error.detail.empty())
			detail += ":" + error.detail;
		return materialization_admission_spool_failure(std::string{phase}, std::move(detail));
	}
} // namespace cxxlens::detail::clang22::materialization
