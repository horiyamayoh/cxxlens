#include "frontend_port.hpp"

#include <algorithm>
#include <string_view>

namespace cxxlens::detail::frontend
{
	namespace
	{
		[[nodiscard]] error invalid_batch(std::string reason)
		{
			error failure;
			failure.code.value = "extractor.invalid-observation";
			failure.message = "Frontend batch invariant failed";
			failure.scope = failure_scope::compile_unit;
			failure.attributes.emplace("reason", std::move(reason));
			return failure;
		}

		[[nodiscard]] std::string severity_name(const diagnostic_severity value)
		{
			switch (value)
			{
				case diagnostic_severity::note:
					return "note";
				case diagnostic_severity::warning:
					return "warning";
				case diagnostic_severity::error:
					return "error";
				case diagnostic_severity::fatal:
					return "fatal";
			}
			return "fatal";
		}
	} // namespace

	result<void> observation_batch::validate() const
	{
		if (schema != "cxxlens.frontend-batch.v1")
			return invalid_batch("schema");
		if (adapter_id != "clang22.frontend" || adapter_version.empty())
			return invalid_batch("adapter");
		if (!unit.valid() || !variant.valid())
			return invalid_batch("identity");
		if (coverage.requested != coverage.parsed + coverage.failed + coverage.cancelled)
			return invalid_batch("coverage-accounting");
		for (const auto& observation : observations)
		{
			if (observation.adapter_id != adapter_id ||
				observation.adapter_version != adapter_version || observation.llvm_major != 22U ||
				observation.compile_unit != unit || observation.variant != variant)
				return invalid_batch("observation-envelope");
			if (auto checked = facts::validate(observation); !checked)
				return invalid_batch("observation-schema");
		}
		if (!std::ranges::is_sorted(diagnostics))
			return invalid_batch("diagnostic-order");
		if (std::ranges::adjacent_find(diagnostics) != diagnostics.end())
			return invalid_batch("duplicate-diagnostic");
		for (const auto& diagnostic : diagnostics)
		{
			if (diagnostic.id.empty() || diagnostic.message.find('\033') != std::string::npos)
				return invalid_batch("diagnostic-content");
		}
		return {};
	}

	std::string observation_batch::semantic_representation() const
	{
		std::string output = schema + "|adapter=" + adapter_id + "@" + adapter_version +
			"|unit=" + std::string{unit.value()} + "|variant=" + std::string{variant.value()} +
			"|coverage=" + std::to_string(coverage.requested) + ":" +
			std::to_string(coverage.parsed) + ":" + std::to_string(coverage.failed) + ":" +
			std::to_string(coverage.cancelled);
		for (const auto& diagnostic : diagnostics)
		{
			output += "|diagnostic=" + diagnostic.id + ":" + severity_name(diagnostic.severity) +
				":" + diagnostic.file + ":" + std::to_string(diagnostic.line) + ":" +
				std::to_string(diagnostic.column) + ":" + diagnostic.message;
		}
		return output;
	}
} // namespace cxxlens::detail::frontend
