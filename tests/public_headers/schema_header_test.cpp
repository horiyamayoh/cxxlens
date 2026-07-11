#include <cxxlens/core/schema.hpp>

auto main() -> int
{
	cxxlens::schema_registry registry;
	return registry.find("cxxlens.finding.v1", {1U, 0U, 0U, {}}) ? 0 : 1;
}
