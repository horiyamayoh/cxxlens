#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <cxxlens/core/failure.hpp>

namespace
{
	auto check(const bool condition, const std::string& message) -> bool
	{
		if (!condition)
			std::cerr << message << '\n';
		return condition;
	}

	auto make_error(std::string code, std::string message) -> cxxlens::error
	{
		cxxlens::error failure;
		failure.code.value = std::move(code);
		failure.message = std::move(message);
		return failure;
	}

	auto make_unresolved(const cxxlens::unresolved_kind kind,
						 std::string code,
						 std::string summary = "uncertain") -> cxxlens::unresolved
	{
		cxxlens::unresolved item;
		item.kind = kind;
		item.stable_code = std::move(code);
		item.summary = std::move(summary);
		return item;
	}

	auto test_error_contract() -> bool
	{
		const auto& registry = cxxlens::common_error_codes();
		bool passed = check(std::ranges::is_sorted(registry), "common registry is not sorted");
		passed &= check(std::adjacent_find(registry.begin(), registry.end()) == registry.end(),
						"common registry contains duplicates");
		for (const std::string code : {"core.invalid-argument",
									   "config.invalid-value",
									   "io.retryable",
									   "core.internal-invariant-violation"})
		{
			passed &= check(make_error(code, "display only").validate(registry).has_value(),
							"registered error rejected: " + code);
		}

		auto first = make_error("core.cancelled", "wording A");
		first.attributes = {{"operation", "search.calls"}};
		first.suggested_actions = {"retry.with-new-snapshot"};
		auto second = first;
		second.message = "completely different wording";
		passed &= check(first.semantic_representation() == second.semantic_representation(),
						"message changed error semantics");

		auto parent_a = make_error("core.internal-invariant-violation", "parent");
		parent_a.causes = {make_error("core.cancelled", "a"),
						   make_error("core.deadline-exceeded", "b")};
		auto parent_b = parent_a;
		std::ranges::reverse(parent_b.causes);
		passed &= check(parent_a.semantic_representation() == parent_b.semantic_representation(),
						"cause insertion order changed canonical semantics");

		passed &= check(!make_error("", "empty").validate(registry), "empty code accepted");
		passed &= check(!make_error("Core.Bad", "case").validate(registry),
						"invalid code syntax accepted");
		passed &= check(!make_error("plugin.unknown", "unknown").validate(registry),
						"unregistered code accepted");
		auto invalid_scope = make_error("core.cancelled", "scope");
		invalid_scope.scope = static_cast<cxxlens::failure_scope>(255U);
		passed &= check(!invalid_scope.validate(registry), "invalid scope accepted");

		auto too_deep = make_error("core.internal-invariant-violation", "root");
		for (std::size_t depth = 0U; depth < 65U; ++depth)
		{
			auto wrapper = make_error("core.internal-invariant-violation", "nested");
			wrapper.causes.push_back(std::move(too_deep));
			too_deep = std::move(wrapper);
		}
		passed &= check(!too_deep.validate(registry), "cause depth/cycle guard did not reject");
		return passed;
	}

	auto test_unresolved_contract() -> bool
	{
		bool passed = true;
		const std::vector<std::string> capabilities{"semantic.ast"};
		for (auto item :
			 {make_unresolved(cxxlens::unresolved_kind::ambiguous_symbol,
							  "semantic.ambiguous-symbol"),
			  make_unresolved(cxxlens::unresolved_kind::dependent_type, "semantic.dependent-type"),
			  make_unresolved(cxxlens::unresolved_kind::open_world_virtual_target,
							  "semantic.open-world-virtual-target")})
		{
			passed &= check(item.validate(capabilities).has_value(),
							"standard unresolved fixture rejected");
		}
		auto missing = make_unresolved(cxxlens::unresolved_kind::missing_compile_command,
									   "workspace.missing-compile-command");
		missing.missing_inputs = {"compile-command"};
		passed &=
			check(missing.validate(capabilities).has_value(), "missing-command fixture rejected");

		auto capability = make_unresolved(cxxlens::unresolved_kind::capability_unavailable,
										  "core.capability-unavailable");
		capability.missing_inputs = {"semantic.ast"};
		capability.required_capability = "semantic.ast";
		passed &= check(capability.validate(capabilities).has_value(),
						"known capability fixture rejected");
		capability.required_capability = "semantic.unknown";
		passed &= check(!capability.validate(capabilities), "unknown capability accepted");

		auto dependent_a = make_unresolved(
			cxxlens::unresolved_kind::dependent_type, "semantic.dependent-type", "wording A");
		auto dependent_b = dependent_a;
		dependent_b.summary = "wording B";
		passed &=
			check(dependent_a.semantic_representation() == dependent_b.semantic_representation(),
				  "summary changed unresolved semantics");

		auto custom = make_unresolved(cxxlens::unresolved_kind::custom, "plugin.model-missing");
		passed &= check(!custom.validate(capabilities), "custom unresolved lost missing inputs");
		custom.missing_inputs = {"model-recipe"};
		passed &= check(custom.validate(capabilities).has_value(),
						"namespaced custom unresolved rejected");
		auto mismatched =
			make_unresolved(cxxlens::unresolved_kind::dependent_type, "semantic.ambiguous-symbol");
		passed &= check(!mismatched.validate(capabilities), "kind/code mismatch accepted");
		auto unknown_precision = dependent_a;
		unknown_precision.required_precision = static_cast<cxxlens::precision_level>(255U);
		passed &= check(!unknown_precision.validate(capabilities), "unknown precision accepted");
		return passed;
	}

	struct read_report
	{
		std::vector<int> values;
		std::vector<cxxlens::unresolved> unresolved_items;
	};

	auto inject_failure(const std::string& code) -> cxxlens::result<int>
	{
		return make_error(code, "injected failure");
	}

	auto test_result_and_classification() -> bool
	{
		bool passed = true;
		cxxlens::result<read_report> empty{read_report{}};
		read_report partial;
		partial.unresolved_items.push_back(
			make_unresolved(cxxlens::unresolved_kind::dependent_type, "semantic.dependent-type"));
		cxxlens::result<read_report> unresolved_success{std::move(partial)};
		cxxlens::result<read_report> failed{make_error("core.invalid-argument", "bad input")};
		passed &= check(empty && empty.value().unresolved_items.empty(),
						"covered empty result was not representable");
		passed &= check(unresolved_success && !unresolved_success.value().unresolved_items.empty(),
						"unresolved collapsed to empty success");
		passed &= check(!failed && failed.error().code.value == "core.invalid-argument",
						"operation failure classification lost");

		auto original = make_error("core.cancelled", "stop");
		original.causes.push_back(make_error("io.retryable", "read interrupted"));
		cxxlens::result<void> void_failure{original};
		auto value_failure = cxxlens::propagate_failure<int>(void_failure);
		auto void_again = cxxlens::propagate_failure<void>(value_failure);
		passed &= check(value_failure.error().semantic_representation() ==
							original.semantic_representation(),
						"void-to-value propagation changed error chain");
		passed &= check(void_again.error().semantic_representation() ==
							original.semantic_representation(),
						"value-to-void propagation changed error chain");

		for (const std::string code :
			 {"core.cancelled", "core.deadline-exceeded", "core.budget-exhausted", "io.retryable"})
		{
			passed &= check(cxxlens::stable_code_registry{}.contains(code),
							"distinct control-flow code unavailable: " + code);
			const auto injected = inject_failure(code);
			passed &= check(!injected && injected.error().code.value == code,
							"failure injection changed control-flow code: " + code);
		}
		return passed;
	}

	auto test_registry() -> bool
	{
		cxxlens::stable_code_registry registry;
		bool passed = check(registry.register_code("plugin.example").has_value(),
							"package code registration failed");
		passed &= check(registry.contains("plugin.example"), "registered code not found");
		passed &= check(!registry.register_code("plugin.example"), "duplicate code accepted");
		passed &= check(!registry.register_code("Plugin.invalid"), "invalid package code accepted");
		passed &= check(std::ranges::is_sorted(registry.all()), "registration broke order");
		return passed;
	}
} // namespace

auto main() -> int
{
	const bool passed = test_error_contract() && test_unresolved_contract() &&
		test_result_and_classification() && test_registry();
	return passed ? 0 : 1;
}
