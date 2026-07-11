#include <cxxlens/cxxlens.hpp>

auto main() -> int
{
	const auto product_versions = cxxlens::versions();
	auto configuration = cxxlens::configuration::defaults();
	auto fixture =
		cxxlens::testing::workspace_fixture::cpp("int main() { return 0; }").materialize();
	cxxlens::evidence evidence;
	cxxlens::coverage_report coverage;
	cxxlens::schema_registry schemas;
	const auto fixture_schema =
		schemas.find("cxxlens.testing.fixture.v1", cxxlens::semantic_version{1U, 0U, 0U, {}});
	return product_versions.library.major == 0U && product_versions.llvm.major == 22U &&
			configuration && configuration.value().validate() &&
			!configuration.value().resolved_json().empty() && fixture &&
			fixture.value().validate() &&
			cxxlens::testing::assert_schema_conforms("cxxlens.testing.fixture.v1",
													 fixture.value().to_json()) &&
			!evidence.to_json().empty() && coverage.complete() && fixture_schema
		? 0
		: 1;
}
