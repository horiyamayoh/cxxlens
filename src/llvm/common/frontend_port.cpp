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

		[[nodiscard]] std::string observation_key(const facts::observation_record& value)
		{
			const auto semantic = value.payload.find("semantic_key");
			return std::to_string(static_cast<std::uint16_t>(value.kind)) + ":" +
				(semantic == value.payload.end() ? std::string{} : semantic->second);
		}

		void append_framed(std::string& output, const std::string_view value)
		{
			output += std::to_string(value.size()) + ":";
			output.append(value);
		}

		[[nodiscard]] std::string_view coverage_name(const coverage_state value) noexcept
		{
			switch (value)
			{
				case coverage_state::covered:
					return "covered";
				case coverage_state::excluded:
					return "excluded";
				case coverage_state::failed:
					return "failed";
				case coverage_state::unresolved:
					return "unresolved";
				case coverage_state::not_applicable:
					return "not_applicable";
			}
			return "unresolved";
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
		if (!std::ranges::is_sorted(observations, {}, observation_key) ||
			std::ranges::adjacent_find(observations, {}, observation_key) != observations.end())
			return invalid_batch("observation-order");
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
		for (const auto& observation : observations)
		{
			output +=
				"|observation=" + std::to_string(static_cast<std::uint16_t>(observation.kind)) +
				":";
			append_framed(output,
						  observation.source ? observation.source->to_canonical_json() : "");
			for (const auto& [key, value] : observation.payload)
			{
				append_framed(output, key);
				append_framed(output, value);
			}
			for (const auto& contribution : observation.coverage_contributions)
			{
				append_framed(output, contribution.kind);
				append_framed(output, contribution.id);
				append_framed(output, coverage_name(contribution.state));
				append_framed(output, contribution.reason.value_or(""));
			}
		}
		return output;
	}
} // namespace cxxlens::detail::frontend
