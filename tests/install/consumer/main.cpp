#include <cxxlens/cxxlens.hpp>
#include <cxxlens/interop/clang.hpp>

auto main() -> int
{
	const auto product_versions = cxxlens::versions();
	auto configuration = cxxlens::configuration::defaults();
	const auto fixture_builder =
		cxxlens::testing::workspace_fixture::cpp("int main() { return 0; }");
	auto fixture = fixture_builder.materialize();
	auto fixture_workspace = fixture_builder.open();
	cxxlens::evidence evidence;
	cxxlens::coverage_report coverage;
	cxxlens::schema_registry schemas;
	const auto fixture_schema =
		schemas.find("cxxlens.testing.fixture.v1", cxxlens::semantic_version{1U, 0U, 0U, {}});
	const auto scope = cxxlens::analysis_scope::files({"src/main.cpp"}).include_headers();
	const auto facts = cxxlens::fact_profile::semantic_search();
	const auto selector = cxxlens::select::calls_to_method("Base", "start").include_derived_types();
	const auto linked_clang = cxxlens::interop::linked_clang_version();
	return product_versions.library.major == 0U && product_versions.llvm.major == 22U &&
			configuration && configuration.value().validate() &&
			!configuration.value().resolved_json().empty() && fixture &&
			fixture.value().validate() && fixture_workspace &&
			fixture_workspace.value().compile_units().size() == 1U &&
			cxxlens::testing::assert_schema_conforms("cxxlens.testing.fixture.v1",
													 fixture.value().to_json()) &&
			!evidence.to_json().empty() && coverage.complete() && fixture_schema &&
			!scope.to_json().empty() && !facts.to_json().empty() && !selector.to_json().empty() &&
			(linked_clang.llvm_major == 0U || linked_clang.llvm_major == 22U)
		? 0
		: 1;
}
