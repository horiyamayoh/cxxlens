#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/core/failure.hpp>

namespace cxxlens
{
	namespace
	{
		constexpr std::size_t maximum_cause_depth = 64U;

		constexpr std::array<std::string_view, 78U> built_in_codes{
			"config.file-not-found",
			"config.invalid-value",
			"config.overlay-conflict",
			"config.profile-not-found",
			"config.unknown-key",
			"config.yaml-invalid",
			"core.budget-exhausted",
			"core.cancelled",
			"core.capability-unavailable",
			"core.deadline-exceeded",
			"core.internal-invariant-violation",
			"core.invalid-argument",
			"core.schema-validation-failed",
			"core.unsupported-capability",
			"core.version-mismatch",
			"extractor.invalid-observation",
			"facts.cache-incompatible",
			"facts.identity-collision",
			"facts.reduction-conflict",
			"facts.store-corrupt",
			"facts.transaction-failed",
			"flow.cfg-unavailable",
			"flow.model-invalid",
			"flow.non-convergent",
			"flow.path-budget-exhausted",
			"generate.artifact-quarantined",
			"generate.macro-surface-rejected",
			"generate.odr-risk-rejected",
			"generate.payload-missing",
			"generate.plan-invalid",
			"generate.structural-audit-failed",
			"generate.surface-census-incomplete",
			"generate.target-ambiguous",
			"generate.type-unspellable",
			"generate.unknown-decision",
			"generate.variant-artifact-conflict",
			"interop.borrowed-lifetime-violation",
			"interop.custom-schema-conflict",
			"interop.nondeterministic-extractor",
			"io.retryable",
			"parse.crashed",
			"parse.frontend-failed",
			"parse.invocation-build-failed",
			"parse.timeout",
			"qa.build-failed",
			"qa.coverage-missing",
			"qa.process-not-allowed",
			"qa.report-parse-failed",
			"report.format-unsupported",
			"review.baseline-invalid",
			"review.diff-invalid",
			"review.gate-failed",
			"rules.invalid-metadata",
			"rules.suppression-invalid",
			"search.plan-compilation-failed",
			"search.refinement-failed",
			"search.required-facts-unavailable",
			"select.ambiguous-name",
			"select.invalid-expression",
			"select.type-mismatch",
			"transform.commit-failed",
			"transform.format-failed",
			"transform.macro-edit-rejected",
			"transform.plan-invalid",
			"transform.range-conflict",
			"transform.reparse-failed",
			"transform.source-stale",
			"transform.target-ambiguous",
			"transform.variant-divergence",
			"transform.writer-lock-conflict",
			"workspace.compile-command-ambiguous",
			"workspace.compile-database-invalid",
			"workspace.compile-database-not-found",
			"workspace.driver-not-allowed",
			"workspace.header-inference-rejected",
			"workspace.model-load-failed",
			"workspace.resource-dir-not-found",
			"workspace.snapshot-stale",
		};

		constexpr std::array<std::string_view, 24U> unresolved_codes{
			"workspace.missing-compile-command",
			"workspace.inferred-compile-command",
			"workspace.malformed-compile-command",
			"parse.failed",
			"parse.incomplete-ast",
			"semantic.ambiguous-symbol",
			"semantic.dependent-type",
			"semantic.unresolved-overload",
			"parse.unsupported-language-extension",
			"parse.missing-module-bmi",
			"source.macro-origin-ambiguous",
			"transform.macro-edit-unsafe",
			"source.generated-code-read-only",
			"semantic.function-pointer-target-unknown",
			"semantic.callback-target-unknown",
			"semantic.virtual-dynamic-type-unknown",
			"semantic.open-world-virtual-target",
			"flow.alias-analysis-required",
			"flow.dataflow-budget-exceeded",
			"flow.path-sensitive-budget-exceeded",
			"core.capability-unavailable",
			"workspace.build-variant-disagreement",
			"workspace.stale-source",
			"",
		};

		[[nodiscard]] bool valid_segmented_code(const std::string_view value)
		{
			if (value.empty() || value.front() == '.' || value.back() == '.' ||
				value.find('.') == std::string_view::npos)
				return false;
			bool previous_dot = false;
			for (const char character : value)
			{
				const auto byte = static_cast<unsigned char>(character);
				if (character == '.')
				{
					if (previous_dot)
						return false;
					previous_dot = true;
					continue;
				}
				previous_dot = false;
				if (std::islower(byte) == 0 && std::isdigit(byte) == 0 && character != '-')
					return false;
			}
			return true;
		}

		[[nodiscard]] bool valid_scope(const failure_scope scope)
		{
			return scope >= failure_scope::operation && scope <= failure_scope::workspace;
		}

		[[nodiscard]] error validation_failure(std::string field, std::string reason)
		{
			error failure;
			failure.code.value = "core.schema-validation-failed";
			failure.message = "failure contract validation failed";
			failure.attributes.emplace("field", std::move(field));
			failure.attributes.emplace("reason", std::move(reason));
			return failure;
		}

		[[nodiscard]] bool sorted_unique(const std::vector<std::string>& values)
		{
			return std::adjacent_find(values.begin(), values.end(), std::greater_equal{}) ==
				values.end();
		}

		[[nodiscard]] result<void> validate_text_set(const std::vector<std::string>& values,
													 const std::string_view field)
		{
			if (!sorted_unique(values))
				return validation_failure(std::string{field}, "must-be-sorted-unique");
			if (std::ranges::any_of(values, &std::string::empty))
				return validation_failure(std::string{field}, "empty-value");
			return {};
		}

		[[nodiscard]] std::string framed(const std::string_view value)
		{
			return std::to_string(value.size()) + ':' + std::string{value};
		}

		[[nodiscard]] std::string scope_name(const failure_scope scope)
		{
			return std::to_string(static_cast<std::uint8_t>(scope));
		}

		[[nodiscard]] result<void>
		validate_error_node(const error& value,
							const std::vector<std::string>& registered_codes,
							const std::size_t depth)
		{
			if (depth > maximum_cause_depth)
				return validation_failure("causes", "cause-cycle-or-depth-limit");
			if (!valid_segmented_code(value.code.value))
				return validation_failure("code", "invalid-stable-code");
			if (!std::binary_search(
					registered_codes.begin(), registered_codes.end(), value.code.value))
				return validation_failure("code", "unregistered-stable-code");
			if (!valid_scope(value.scope))
				return validation_failure("scope", "invalid-scope");
			for (const auto& location : value.locations)
			{
				if (location.validate())
					return validation_failure("locations", "invalid-source-span");
			}
			if (auto validation = validate_text_set(value.suggested_actions, "suggested_actions");
				!validation)
				return validation;
			for (const auto& [key, attribute] : value.attributes)
			{
				if (!valid_segmented_code("attribute." + key) || attribute.empty())
					return validation_failure("attributes", "invalid-attribute");
			}
			for (const auto& cause : value.causes)
			{
				if (auto validation = validate_error_node(cause, registered_codes, depth + 1U);
					!validation)
					return validation;
			}
			return {};
		}

		[[nodiscard]] std::string error_semantics(const error& value)
		{
			std::vector<std::string> locations;
			locations.reserve(value.locations.size());
			for (const auto& location : value.locations)
				locations.push_back(location.to_canonical_json());
			std::ranges::sort(locations);
			std::vector<std::string> causes;
			causes.reserve(value.causes.size());
			for (const auto& cause : value.causes)
				causes.push_back(error_semantics(cause));
			std::ranges::sort(causes);

			std::string output = "error.v1|code=" + framed(value.code.value) +
				"|scope=" + scope_name(value.scope) + "|retryable=" + (value.retryable ? "1" : "0");
			for (const auto& location : locations)
				output += "|location=" + framed(location);
			for (const auto& cause : causes)
				output += "|cause=" + framed(cause);
			for (const auto& action : value.suggested_actions)
				output += "|action=" + framed(action);
			for (const auto& [key, attribute] : value.attributes)
				output += "|attribute=" + framed(key) + framed(attribute);
			return output;
		}

		void
		append_explanation(const error& value, const std::size_t depth, std::ostringstream& output)
		{
			output << std::string(depth * 2U, ' ') << value.code.value;
			if (!value.message.empty())
				output << ": " << value.message;
			output << '\n';
			for (const auto& cause : value.causes)
				append_explanation(cause, depth + 1U, output);
		}

		[[nodiscard]] bool kind_requires_missing_input(const unresolved_kind kind)
		{
			switch (kind)
			{
				case unresolved_kind::missing_compile_command:
				case unresolved_kind::malformed_compile_command:
				case unresolved_kind::missing_module_bmi:
				case unresolved_kind::capability_unavailable:
				case unresolved_kind::custom:
					return true;
				default:
					return false;
			}
		}
	} // namespace

	result<void> error::validate(const std::vector<std::string>& registered_codes) const
	{
		if (!sorted_unique(registered_codes))
			return validation_failure("registered_codes", "must-be-sorted-unique");
		return validate_error_node(*this, registered_codes, 0U);
	}

	std::string error::semantic_representation() const
	{
		return error_semantics(*this);
	}

	std::string error::explain() const
	{
		std::ostringstream output;
		append_explanation(*this, 0U, output);
		return output.str();
	}

	result<void> unresolved::validate(const std::vector<std::string>& registered_capabilities) const
	{
		const auto kind_index = static_cast<std::size_t>(kind);
		if (kind_index >= unresolved_codes.size())
			return validation_failure("kind", "unknown-unresolved-kind");
		if (!valid_scope(scope))
			return validation_failure("scope", "invalid-scope");
		if (!valid_segmented_code(stable_code))
			return validation_failure("stable_code", "invalid-stable-code");
		if (kind != unresolved_kind::custom && stable_code != unresolved_codes.at(kind_index))
			return validation_failure("stable_code", "kind-code-mismatch");
		if (kind == unresolved_kind::custom &&
			std::ranges::find(unresolved_codes, stable_code) != unresolved_codes.end())
			return validation_failure("stable_code", "custom-code-must-be-namespaced");
		if (auto validation = validate_text_set(missing_inputs, "missing_inputs"); !validation)
			return validation;
		if (kind_requires_missing_input(kind) && missing_inputs.empty())
			return validation_failure("missing_inputs", "required-for-kind");
		if (auto validation = validate_text_set(suggested_actions, "suggested_actions");
			!validation)
			return validation;
		if (required_precision &&
			static_cast<std::uint8_t>(*required_precision) >
				static_cast<std::uint8_t>(precision_level::dynamic_observation))
			return validation_failure("required_precision", "unknown-precision");
		if (!sorted_unique(registered_capabilities))
			return validation_failure("registered_capabilities", "must-be-sorted-unique");
		if (kind == unresolved_kind::capability_unavailable && !required_capability)
			return validation_failure("required_capability", "required-for-kind");
		if (required_capability &&
			!std::binary_search(registered_capabilities.begin(),
								registered_capabilities.end(),
								*required_capability))
			return validation_failure("required_capability", "unknown-capability");
		for (const auto& related_span : related)
		{
			if (related_span.validate())
				return validation_failure("related", "invalid-source-span");
		}
		for (const auto& [key, attribute] : attributes)
		{
			if (!valid_segmented_code("attribute." + key) || attribute.empty())
				return validation_failure("attributes", "invalid-attribute");
		}
		return {};
	}

	std::string unresolved::semantic_representation() const
	{
		std::vector<std::string> spans;
		spans.reserve(related.size());
		for (const auto& span : related)
			spans.push_back(span.to_canonical_json());
		std::ranges::sort(spans);
		std::string output =
			"unresolved.v1|kind=" + std::to_string(static_cast<std::uint16_t>(kind)) +
			"|code=" + framed(stable_code) + "|scope=" + scope_name(scope);
		for (const auto& span : spans)
			output += "|related=" + framed(span);
		for (const auto& input : missing_inputs)
			output += "|missing=" + framed(input);
		for (const auto& action : suggested_actions)
			output += "|action=" + framed(action);
		if (required_precision)
			output +=
				"|precision=" + std::to_string(static_cast<std::uint8_t>(*required_precision));
		if (required_capability)
			output += "|capability=" + framed(*required_capability);
		for (const auto& [key, attribute] : attributes)
			output += "|attribute=" + framed(key) + framed(attribute);
		return output;
	}

	stable_code_registry::stable_code_registry() : codes_{common_error_codes()} {}

	result<void> stable_code_registry::register_code(std::string code)
	{
		if (!valid_segmented_code(code))
			return validation_failure("code", "invalid-stable-code");
		const auto position = std::ranges::lower_bound(codes_, code);
		if (position != codes_.end() && *position == code)
			return validation_failure("code", "duplicate-stable-code");
		codes_.insert(position, std::move(code));
		return {};
	}

	bool stable_code_registry::contains(const std::string_view code) const noexcept
	{
		return std::binary_search(codes_.begin(), codes_.end(), code);
	}

	const std::vector<std::string>& stable_code_registry::all() const noexcept
	{
		return codes_;
	}

	const std::vector<std::string>& common_error_codes()
	{
		static const std::vector<std::string> codes{built_in_codes.begin(), built_in_codes.end()};
		return codes;
	}
} // namespace cxxlens
