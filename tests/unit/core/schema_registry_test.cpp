#include <iostream>
#include <string>
#include <vector>

#include <cxxlens/core/schema.hpp>

namespace
{
	auto check(const bool condition, const std::string& message) -> bool
	{
		if (!condition)
			std::cerr << message << '\n';
		return condition;
	}
} // namespace

auto main() -> int
{
	using namespace cxxlens;
	schema_registry registry;
	bool passed = check(registry.all().size() == 8U, "M0 registry is incomplete");
	for (const auto& entry : registry.all())
	{
		passed &= check(registry.find(entry.id, entry.version).has_value(),
						"registered schema did not resolve");
	}
	passed &= check(!registry.find("cxxlens.unknown.v1", {1U, 0U, 0U, {}}),
					"unknown required schema accepted");
	passed &= check(!registry.find("cxxlens.finding.v1", {2U, 0U, 0U, {}}),
					"unknown required schema version accepted");

	const auto current = registry.find("cxxlens.finding.v1", {1U, 0U, 0U, {}}).value();
	auto proposed = current;
	proposed.version.minor = 1U;
	const std::vector compatible{
		schema_change{schema_change_kind::optional_field_added, "help_uri"}};
	passed &= check(registry.check_compatibility(current, proposed, compatible).status ==
						schema_compatibility::compatible,
					"compatible optional addition rejected");
	for (const auto kind : {schema_change_kind::optional_field_removed,
							schema_change_kind::required_field_added,
							schema_change_kind::required_field_removed,
							schema_change_kind::field_type_changed,
							schema_change_kind::field_meaning_changed})
	{
		const std::vector change{schema_change{kind, "message"}};
		passed &= check(registry.check_compatibility(current, proposed, change).status ==
							schema_compatibility::incompatible_rebuild_required,
						"incompatible schema change accepted");
	}
	auto major = current;
	major.version.major = 2U;
	passed &= check(registry.check_compatibility(current, major, {}).status ==
						schema_compatibility::incompatible_rebuild_required,
					"major schema change accepted without rebuild");
	return passed ? 0 : 1;
}
