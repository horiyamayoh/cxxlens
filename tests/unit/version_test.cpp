#include <algorithm>
#include <compare>
#include <iostream>
#include <ranges>
#include <string>

#include <cxxlens/core.hpp>

namespace
{

	auto check(bool condition, const std::string& message) -> bool
	{
		if (!condition)
		{
			std::cerr << message << '\n';
		}
		return condition;
	}

} // namespace

auto main() -> int
{
	bool passed = true;
	passed &= check(cxxlens::semantic_version{1U, 2U, 3U, {}}.to_string() == "1.2.3",
					"stable version formatting failed");
	passed &= check(cxxlens::semantic_version{1U, 2U, 3U, "rc.1"}.to_string() == "1.2.3-rc.1",
					"prerelease version formatting failed");
	passed &=
		check(cxxlens::semantic_version{1U, 2U, 3U, {}} < cxxlens::semantic_version{1U, 3U, 0U, {}},
			  "version comparison failed");

	const auto product_versions = cxxlens::versions();
	passed &= check(product_versions.library == cxxlens::semantic_version{0U, 1U, 0U, {}},
					"library version mismatch");
	passed &= check(product_versions.public_schema.major == 1U, "public schema version missing");
	passed &= check(product_versions.semantics.major == 1U, "semantics version missing");
	passed &= check(product_versions.fact_schema.major == 1U, "fact schema version missing");
	passed &= check(product_versions.finding_schema.major == 1U, "finding schema version missing");
	passed &=
		check(product_versions.edit_plan_schema.major == 1U, "edit plan schema version missing");
	passed &= check(product_versions.generation_plan_schema.major == 1U,
					"generation plan schema version missing");
	passed &= check(product_versions.llvm == cxxlens::semantic_version{22U, 1U, 8U, {}},
					"LLVM baseline mismatch");
	const auto capability_registry = cxxlens::capabilities();
	passed &= check(capability_registry.has("workspace.incremental-provisioning"),
					"incremental provisioning capability is missing");
	passed &= check(!capability_registry.has("unknown.capability") &&
						capability_registry.get("unknown.capability").state ==
							cxxlens::capability_state::disabled_at_build &&
						capability_registry.get("unknown.capability").limitation.has_value(),
					"unknown capability became an empty or available success");
	passed &= check(std::ranges::is_sorted(capability_registry.all(), {}, &cxxlens::capability::id),
					"capabilities are not canonically ordered");
	passed &=
		check(capability_registry.to_json().find("cxxlens.capabilities.v1") != std::string::npos,
			  "capability schema projection is missing");

	return passed ? 0 : 1;
}
