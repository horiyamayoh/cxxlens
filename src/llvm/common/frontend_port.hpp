#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <vector>

#include <cxxlens/core/failure.hpp>
#include <cxxlens/workspace.hpp>

#include "../../facts/fact_contract.hpp"

namespace cxxlens::detail::frontend
{
	enum class diagnostic_severity : std::uint8_t
	{
		note,
		warning,
		error,
		fatal,
	};

	struct normalized_diagnostic
	{
		std::string id;
		diagnostic_severity severity{diagnostic_severity::note};
		std::string file;
		std::uint32_t line{};
		std::uint32_t column{};
		std::string message;
		auto operator<=>(const normalized_diagnostic&) const = default;
	};

	struct virtual_source_file
	{
		path file;
		std::string content;
	};

	enum class frontend_fault : std::uint8_t
	{
		none,
		timeout,
		cancellation,
		crash,
	};

	struct parse_task
	{
		compile_unit unit;
		std::vector<virtual_source_file> files;
		frontend_fault injected_fault{frontend_fault::none};
	};

	struct parse_coverage
	{
		std::uint32_t requested{1U};
		std::uint32_t parsed{};
		std::uint32_t failed{};
		std::uint32_t cancelled{};
	};

	struct observation_batch
	{
		std::string schema{"cxxlens.frontend-batch.v1"};
		std::string adapter_id;
		std::string adapter_version;
		compile_unit_id unit;
		build_variant_id variant;
		std::uint64_t debug_context_identity{};
		std::vector<facts::observation_record> observations;
		std::vector<normalized_diagnostic> diagnostics;
		parse_coverage coverage;

		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] std::string semantic_representation() const;
	};

	struct adapter_capability
	{
		bool available{};
		std::uint32_t llvm_major{};
		std::uint32_t llvm_minor{};
		std::uint32_t llvm_patch{};
		std::string adapter_version;
		std::vector<std::string> explicit_components;
		std::string limitation;
	};
} // namespace cxxlens::detail::frontend
