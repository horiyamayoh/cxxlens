#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <cxxlens/cxxlens.hpp>
#include <cxxlens/explain.hpp>
#include <cxxlens/interop/clang.hpp>
#include <cxxlens/search.hpp>

auto main() -> int
{
	const auto product_versions = cxxlens::versions();
	const auto capability_registry = cxxlens::capabilities();
	auto configuration = cxxlens::configuration::defaults();
	const auto missing_configuration = cxxlens::configuration::load("missing-cxxlens.yaml");
	const auto nearest_configuration = cxxlens::configuration::load_nearest("missing/source.cpp");
	const auto missing_profile = configuration.value().with_profile("missing");
	const auto overlaid_configuration = configuration.value().overlay(configuration.value());
	const auto fixture_builder =
		cxxlens::testing::workspace_fixture::cpp("int main() { return 0; }");
	auto fixture = fixture_builder.materialize();
	auto fixture_workspace = fixture_builder.open();
	cxxlens::evidence evidence;
	cxxlens::evidence_item evidence_item;
	evidence_item.kind = cxxlens::evidence_kind::approximation;
	evidence_item.attributes = {{"approximation.kind", "installed-consumer"}};
	evidence.add(std::move(evidence_item));
	cxxlens::coverage_report coverage;
	cxxlens::schema_registry schemas;
	const auto fixture_schema =
		schemas.find("cxxlens.testing.fixture.v1", cxxlens::semantic_version{1U, 0U, 0U, {}});
	const auto search_schema =
		schemas.find("cxxlens.search-report.v1", cxxlens::semantic_version{1U, 0U, 0U, {}});
	const auto scope = cxxlens::analysis_scope::files({"src/main.cpp"}).include_headers();
	const auto all_scope = cxxlens::analysis_scope::all();
	const auto changed_scope = cxxlens::analysis_scope::changed_files({"src/main.cpp"});
	const auto unit_scope = cxxlens::analysis_scope::compile_units({});
	const auto facts = cxxlens::fact_profile::semantic_search();
	const auto full_facts = cxxlens::fact_profile::full()
								.include(cxxlens::fact_kind::symbol)
								.exclude(cxxlens::fact_kind::macro_expansion)
								.precision(cxxlens::precision_level::workspace_semantic);
	cxxlens::testing::result_observation observation;
	const auto assertion =
		cxxlens::testing::result_assertion{}.has_exactly(0U).has_no_errors().is_complete().check(
			observation);
	const auto property = cxxlens::testing::check_property(
		cxxlens::testing::property_options{7U, 1U, std::nullopt},
		[](const std::uint64_t seed, const std::size_t)
		{
			return seed;
		},
		[](const std::uint64_t value)
		{
			return value == 7U;
		},
		[](const std::uint64_t value)
		{
			return std::to_string(value);
		});
	const auto selector = cxxlens::select::calls_to_method("Base", "start").include_derived_types();
	cxxlens::search_options search_options;
	search_options.result_limit = 16U;
	const auto explanation = cxxlens::explain::selector(cxxlens::select::semantic(selector));
	const auto linked_clang = cxxlens::interop::linked_clang_version();
	return product_versions.library.major == 0U && product_versions.llvm.major == 22U &&
			capability_registry.has("workspace.incremental-provisioning") && configuration &&
			configuration.value().validate() && !configuration.value().resolved_json().empty() &&
			!missing_configuration && !nearest_configuration && !missing_profile &&
			overlaid_configuration && fixture && fixture.value().validate() && fixture_workspace &&
			fixture_workspace.value().compile_units().size() == 1U &&
			cxxlens::testing::assert_schema_conforms("cxxlens.testing.fixture.v1",
													 fixture.value().to_json()) &&
			!evidence.items().empty() && !evidence.to_json().empty() &&
			!evidence.to_markdown().empty() && coverage.complete() && coverage.units().empty() &&
			!coverage.to_json().empty() && !coverage.to_markdown().empty() && fixture_schema &&
			search_schema && !scope.to_json().empty() && !all_scope.to_json().empty() &&
			!changed_scope.to_json().empty() && !unit_scope.to_json().empty() &&
			!facts.to_json().empty() && !full_facts.to_json().empty() && assertion &&
			property.passed() && !selector.to_json().empty() &&
			search_options.result_limit == 16U && !explanation.to_json().empty() &&
			(linked_clang.llvm_major == 0U || linked_clang.llvm_major == 22U)
		? 0
		: 1;
}
