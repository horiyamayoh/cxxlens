#pragma once

/** @file source_tokens.hpp @brief Finite, conditioned compiler token streams and their evidence. */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include <cxxlens/sdk/query_transfer.hpp>

namespace cxxlens::sdk::query
{
	/** @brief Borrowed, already validated independent-scan rows. */
	struct source_token_input
	{
		std::span<const annotated_row> files, spans, inventories, tokens;
	};

	/** @brief Checked projection bounds independent of semantic identity. */
	struct source_token_limits
	{
		std::size_t maximum_rows{2000000U};
		std::size_t maximum_condition_expansions{4000000U};
		std::size_t maximum_evidence_bytes{256U * 1024U * 1024U};
		std::size_t maximum_expanded_row_bytes{512U * 1024U * 1024U};
		std::size_t maximum_evidence_references{16000000U};
		std::size_t maximum_streams{100000U};
		std::size_t maximum_stream_tokens{250000U};
		std::size_t maximum_source_queries{4096U};
		std::size_t maximum_source_plan_bytes{64U * 1024U * 1024U};
		[[nodiscard]] result<void> validate() const;
	};

	/** @brief Local stream enumeration, without a global closed-world guarantee. */
	enum class source_token_state : std::uint8_t
	{
		complete,
		partial,
		conflicting
	};

	/** @brief One owned lexical token; unknown classification remains absent. */
	struct source_token
	{
		std::string id, source_span, kind, spelling;
		std::optional<std::uint64_t> ordinal, begin, end;
		std::optional<bool> macro, active, template_context, preprocessor, directive_start;
		std::vector<std::size_t> evidence;
	};

	/** @brief Exactly one source/phase/profile in one interpretation and variant fragment. */
	struct source_token_stream
	{
		std::string id, compile_unit, file, source_snapshot, phase, profile;
		std::string universe, variant, interpretation;
		std::optional<std::uint64_t> source_size, declared_tokens, directive_count;
		source_token_state state{source_token_state::partial};
		std::string active_state, template_state;
		std::vector<std::size_t> evidence;
		std::vector<source_token> tokens;
		std::vector<query_unresolved> gaps;
	};

	/** @brief Owned input evidence and finite streams; original queries retain every side channel.
	 */
	struct source_token_projection
	{
		std::vector<annotated_row> evidence_rows;
		std::vector<source_token_stream> streams;
		std::vector<query_unresolved> unresolved;
		std::optional<application_query_results> source_queries;
	};

	/** @brief Join validated token rows to their source bounds and finite declared inventories. */
	[[nodiscard]] result<source_token_projection>
	project_source_tokens(source_token_input input,
						  source_token_limits limits = {},
						  std::stop_token cancellation = {});

	/** @brief Project public application scans while retaining all original plans and side
	 * channels. */
	[[nodiscard]] result<source_token_projection>
	project_source_tokens(const application_query_results& input,
						  source_token_limits limits = {},
						  std::stop_token cancellation = {});
} // namespace cxxlens::sdk::query
