#pragma once

/**
 * @file msvc_response_arguments.hpp
 * @brief Source-private MSVC response-file tokenization authority.
 */

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace cxxlens::application_analysis_worker
{
	enum class msvc_response_parse_failure
	{
		embedded_nul,
		unterminated_quote,
		argument_count,
	};

	/** Parse one MSVC response file with bounded argv construction. */
	[[nodiscard]] std::expected<std::vector<std::string>, msvc_response_parse_failure>
	parse_msvc_response_arguments(std::span<const std::byte> content,
								  std::size_t maximum_arguments);
} // namespace cxxlens::application_analysis_worker
