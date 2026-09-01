#include "sdk/build_capture_internal.hpp"

#include <cassert>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace
{
	using namespace cxxlens::sdk;
	using namespace cxxlens::sdk::detail;

	[[nodiscard]] std::string content(const char digit)
	{
		return "sha256:" + std::string(64U, digit);
	}

	[[nodiscard]] std::string semantic(const char digit)
	{
		return "semantic-v2:sha256:" + std::string(64U, digit);
	}

	[[nodiscard]] project_catalog catalog_for(const std::string& unit,
											  const std::string& invocation,
											  const std::string& source,
											  const std::string& environment)
	{
		auto value = project_catalog::make(
			"project://root", environment, {{unit, invocation, source, environment}});
		assert(value);
		return std::move(*value);
	}

	[[nodiscard]] build_capture_draft valid_draft()
	{
		build_capture_draft value;
		value.project_id = "project:one";
		value.selected_catalog_compile_unit_id = "catalog-unit:one";
		value.compile_unit_id = "build-compile-unit:one";
		value.build_variant_id = "build-variant:one";
		value.toolchain_context_id = "toolchain-context:one";
		value.toolchain_digest = semantic('1');
		value.toolchain = {"clang",
						   "22.1.0",
						   "x86_64-pc-linux-gnu",
						   content('2'),
						   std::string{"/opt/toolchain/sysroot"},
						   content('3'),
						   content('4'),
						   captured_value<std::string>::unavailable(
							   "not-carried-by-request-v2_2", "capture-production-compiler-path"),
						   captured_value<std::string>::unavailable(
							   "not-carried-by-request-v2_2", "capture-production-binary-digest")};
		value.variant = {
			"c++", "c++23", "x86_64-pc-linux-gnu", content('5'), content('6'), content('7')};
		value.invocation.original_arguments = captured_value<std::vector<std::string>>::unavailable(
			"not-carried-by-request-v2_2", "recapture-original-argv");
		value.invocation.normalized_semantic_options =
			captured_value<std::vector<normalized_build_option>>::unavailable(
				"not-carried-by-request-v2_2", "recapture-normalized-options");
		value.invocation.effective_replay_arguments =
			captured_value<std::vector<std::string>>::observed(
				{"/opt/toolchain/bin/clang++", "-std=c++23", "project://src/main.cpp"});
		value.invocation.response_files =
			captured_value<std::vector<build_capture_auxiliary_file>>::unavailable(
				"not-carried-by-request-v2_2", "recapture-response-files");
		value.invocation.config_files =
			captured_value<std::vector<build_capture_auxiliary_file>>::unavailable(
				"not-carried-by-request-v2_2", "recapture-config-files");
		value.invocation.environment_effects =
			captured_value<std::vector<build_capture_environment_effect>>::unavailable(
				"environment-values-not-carried", "recapture-allowlisted-environment-effects");
		value.invocation.effective_invocation_digest = semantic('8');
		value.invocation.environment_digest = content('9');
		value.invocation.language = "c++";
		value.invocation.logical_working_directory = "project://src";
		value.invocation.qualified_read_roots = {"/opt/toolchain"};
		value.source = {"source-snapshot:one",
						"file:one",
						"project://src/main.cpp",
						content('a'),
						7U,
						"utf8",
						"line-index:one",
						true};
		value.source_closure = {"source-closure:one", semantic('b'), semantic('c'), 1U, 1U, 7U};
		value.catalog = catalog_for(value.selected_catalog_compile_unit_id,
									value.invocation.effective_invocation_digest,
									value.source.content_digest,
									value.invocation.environment_digest);
		return value;
	}

	void positive_and_location_independent_identity()
	{
		auto first = validate_build_capture(valid_draft());
		assert(first);
		assert(first->gaps().size() == 7U);
		auto relocated = valid_draft();
		relocated.toolchain.sysroot = "/relocated/sysroot";
		relocated.invocation.effective_replay_arguments =
			captured_value<std::vector<std::string>>::observed(
				{"/relocated/bin/clang++", "-std=c++23", "project://src/main.cpp"});
		relocated.invocation.qualified_read_roots = {"/relocated"};
		relocated.invocation.effective_invocation_digest = semantic('d');
		relocated.catalog = catalog_for(relocated.selected_catalog_compile_unit_id,
										relocated.invocation.effective_invocation_digest,
										relocated.source.content_digest,
										relocated.invocation.environment_digest);
		auto second = validate_build_capture(std::move(relocated));
		assert(second);
		assert(first->semantic_identity() == second->semantic_identity());
	}

	void response_file_missing_recursion_and_limits()
	{
		auto missing = valid_draft();
		missing.invocation.response_files =
			captured_value<std::vector<build_capture_auxiliary_file>>::observed({
				{"project://build/options.rsp",
				 captured_value<std::string>::unavailable("response-file-missing",
														  "regenerate-and-recapture"),
				 0U,
				 std::nullopt},
			});
		auto missing_result = validate_build_capture(std::move(missing));
		assert(missing_result);
		assert(!missing_result->gaps().empty());

		auto recursive = valid_draft();
		recursive.invocation
			.response_files = captured_value<std::vector<build_capture_auxiliary_file>>::observed({
			{"project://build/a.rsp", captured_value<std::string>::observed(content('1')), 4U, 1U},
			{"project://build/b.rsp", captured_value<std::string>::observed(content('2')), 4U, 0U},
		});
		auto recursive_result = validate_build_capture(std::move(recursive));
		assert(!recursive_result && recursive_result.error().detail == "recursive-reference");

		auto bounded = valid_draft();
		bounded.invocation
			.response_files = captured_value<std::vector<build_capture_auxiliary_file>>::observed({
			{"project://build/a.rsp", captured_value<std::string>::observed(content('1')), 4U, {}},
			{"project://build/b.rsp", captured_value<std::string>::observed(content('2')), 4U, {}},
		});
		build_capture_limits limits;
		limits.maximum_auxiliary_files = 1U;
		auto bounded_result = validate_build_capture(std::move(bounded), limits);
		assert(!bounded_result &&
			   bounded_result.error().code == "sdk.build-capture-limit-exceeded");
	}

	void unknown_option_and_redacted_environment()
	{
		auto unresolved = valid_draft();
		unresolved.invocation.normalized_semantic_options =
			captured_value<std::vector<normalized_build_option>>::derived({
				{"-fvendor-mode",
				 replay_option_class::unsupported,
				 "unknown-production-option",
				 "add-versioned-option-classification"},
			});
		unresolved.invocation.environment_effects =
			captured_value<std::vector<build_capture_environment_effect>>::observed({
				{"SECRET_MODE",
				 captured_value<std::string>::redacted("secret-semantic-effect",
													   "supply-workspace-local-fingerprint")},
			});
		auto result = validate_build_capture(std::move(unresolved));
		assert(result && result->gaps().size() == 7U);

		auto unknown_without_action = valid_draft();
		unknown_without_action.invocation.normalized_semantic_options =
			captured_value<std::vector<normalized_build_option>>::derived({
				{"-funknown", replay_option_class::unsupported, {}, {}},
			});
		auto invalid_option = validate_build_capture(std::move(unknown_without_action));
		assert(!invalid_option && invalid_option.error().detail == "classification-gap-mismatch");

		auto forged_redaction = valid_draft();
		forged_redaction.invocation.environment_effects =
			captured_value<std::vector<build_capture_environment_effect>>::observed({
				{"SECRET_MODE",
				 {capture_field_state::redacted,
				  std::string{"leaked"},
				  "secret-semantic-effect",
				  "supply-workspace-local-fingerprint"}},
			});
		auto invalid_redaction = validate_build_capture(std::move(forged_redaction));
		assert(!invalid_redaction && invalid_redaction.error().detail == "state-value-mismatch");
	}

	void deterministic_auxiliary_order_and_conflict_rejection()
	{
		auto first = valid_draft();
		first.invocation
			.response_files = captured_value<std::vector<build_capture_auxiliary_file>>::observed({
			{"project://build/b.rsp", captured_value<std::string>::observed(content('2')), 4U, {}},
			{"project://build/a.rsp", captured_value<std::string>::observed(content('1')), 4U, {}},
		});
		auto second = valid_draft();
		second.invocation
			.response_files = captured_value<std::vector<build_capture_auxiliary_file>>::observed({
			{"project://build/a.rsp", captured_value<std::string>::observed(content('1')), 4U, {}},
			{"project://build/b.rsp", captured_value<std::string>::observed(content('2')), 4U, {}},
		});
		auto first_result = validate_build_capture(std::move(first));
		auto second_result = validate_build_capture(std::move(second));
		assert(first_result && second_result &&
			   first_result->semantic_identity() == second_result->semantic_identity());

		auto duplicate = valid_draft();
		auto duplicate_set = validate_build_capture_set({valid_draft(), std::move(duplicate)});
		assert(!duplicate_set && duplicate_set.error().detail == "duplicate-compile-unit");

		auto conflicting = valid_draft();
		conflicting.selected_catalog_compile_unit_id = "catalog-unit:two";
		conflicting.compile_unit_id = "build-compile-unit:two";
		conflicting.source.source_snapshot_id = "source-snapshot:two";
		conflicting.source.file_id = "file:two";
		conflicting.source.logical_path = "project://src/other.cpp";
		conflicting.source.content_digest = content('e');
		conflicting.source_closure.closure_id = "source-closure:two";
		conflicting.source_closure.closure_digest = semantic('e');
		conflicting.source_closure.manifest_digest = semantic('f');
		conflicting.variant.language_standard = "c++20";
		conflicting.catalog = catalog_for(conflicting.selected_catalog_compile_unit_id,
										  conflicting.invocation.effective_invocation_digest,
										  conflicting.source.content_digest,
										  conflicting.invocation.environment_digest);
		auto conflict_set = validate_build_capture_set({valid_draft(), std::move(conflicting)});
		assert(!conflict_set && conflict_set.error().detail == "conflicting-build-variant");

		auto overflow = valid_draft();
		overflow.source.size_bytes = std::numeric_limits<std::uint64_t>::max();
		overflow.source_closure.unique_blob_bytes = std::numeric_limits<std::uint64_t>::max();
		build_capture_limits overflow_limits;
		overflow_limits.maximum_source_closure_bytes = std::numeric_limits<std::uint64_t>::max();
		auto overflow_result = validate_build_capture(std::move(overflow), overflow_limits);
		assert(!overflow_result && overflow_result.error().detail == "overflow");
	}

	void catalog_closure_and_auxiliary_resource_bounds()
	{
		auto catalog_bounded = valid_draft();
		auto expanded =
			project_catalog::make("project://root",
								  catalog_bounded.invocation.environment_digest,
								  {{catalog_bounded.selected_catalog_compile_unit_id,
									catalog_bounded.invocation.effective_invocation_digest,
									catalog_bounded.source.content_digest,
									catalog_bounded.invocation.environment_digest},
								   {"catalog-unit:two",
									semantic('d'),
									content('e'),
									catalog_bounded.invocation.environment_digest}});
		assert(expanded);
		catalog_bounded.catalog = std::move(*expanded);
		build_capture_limits limits;
		limits.maximum_catalog_compile_units = 1U;
		auto catalog_result = validate_build_capture(std::move(catalog_bounded), limits);
		assert(!catalog_result &&
			   catalog_result.error().code == "sdk.build-capture-limit-exceeded");

		auto closure_bounded = valid_draft();
		closure_bounded.source_closure.unique_blob_bytes = std::uint64_t{48U} * 1024U * 1024U + 1U;
		auto closure_result = validate_build_capture(std::move(closure_bounded));
		assert(!closure_result &&
			   closure_result.error().code == "sdk.build-capture-limit-exceeded");

		auto zero_byte = valid_draft();
		zero_byte.source.size_bytes = 0U;
		zero_byte.source_closure.unique_blob_bytes = 0U;
		auto zero_result = validate_build_capture(std::move(zero_byte));
		assert(zero_result);

		auto auxiliary_overflow = valid_draft();
		auxiliary_overflow.invocation.response_files =
			captured_value<std::vector<build_capture_auxiliary_file>>::observed({
				{"project://build/options.rsp",
				 captured_value<std::string>::observed(content('1')),
				 std::numeric_limits<std::uint64_t>::max(),
				 {}},
			});
		auto auxiliary_result = validate_build_capture(std::move(auxiliary_overflow));
		assert(!auxiliary_result && auxiliary_result.error().detail == "overflow");
	}
} // namespace

int main()
{
	positive_and_location_independent_identity();
	response_file_missing_recursion_and_limits();
	unknown_option_and_redacted_environment();
	deterministic_auxiliary_order_and_conflict_rejection();
	catalog_closure_and_auxiliary_resource_bounds();
}
