#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <cxxlens/sdk/application_analysis.hpp>

namespace
{
	using cxxlens::sdk::canonical_binary;
	using cxxlens::sdk::canonical_value;

	[[nodiscard]] std::string digest(const char digit)
	{
		return "sha256:" + std::string(64U, digit);
	}

	[[nodiscard]] canonical_value observed(canonical_value value)
	{
		return canonical_value::from_tuple({canonical_value::from_string("observed"),
											std::move(value),
											canonical_value::from_string({}),
											canonical_value::from_string({})});
	}

	[[nodiscard]] std::vector<std::byte> source_bytes()
	{
		const std::string source{"int main() { return 0; }\n"};
		std::vector<std::byte> output;
		output.reserve(source.size());
		for (const char byte : source)
			output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
		return output;
	}

	[[nodiscard]] canonical_value derived(canonical_value value)
	{
		return canonical_value::from_tuple({canonical_value::from_string("derived"),
											std::move(value),
											canonical_value::from_string({}),
											canonical_value::from_string({})});
	}

	[[nodiscard]] canonical_value unavailable(std::string reason, std::string action)
	{
		return canonical_value::from_tuple({canonical_value::from_string("unavailable"),
											canonical_value::null(),
											canonical_value::from_string(std::move(reason)),
											canonical_value::from_string(std::move(action))});
	}

	[[nodiscard]] canonical_value gap(std::string field, std::string reason, std::string action)
	{
		return canonical_value::from_tuple({canonical_value::from_string(std::move(field)),
											canonical_value::from_string("unavailable"),
											canonical_value::from_string(std::move(reason)),
											canonical_value::from_string(std::move(action))});
	}

	void rebind_source_closure(canonical_value& bundle)
	{
		auto& closure = bundle.tuple[6].tuple[0];
		auto encoded_members = canonical_binary(closure.tuple[6]);
		assert(encoded_members);
		closure.tuple[2] =
			canonical_value::from_string(cxxlens::sdk::content_digest(*encoded_members));
		const std::array fields{
			closure.tuple[2], closure.tuple[3], closure.tuple[4], closure.tuple[5]};
		auto closure_digest =
			cxxlens::sdk::canonical_identity_digest("application-source-closure", fields);
		assert(closure_digest);
		closure.tuple[1] = canonical_value::from_string(std::move(*closure_digest));
		closure.tuple[0] = canonical_value::from_string("source-closure:" + closure.tuple[1].text);
		bundle.tuple[5].tuple[0].tuple[15] = closure.tuple[0];
	}

	[[nodiscard]] canonical_value valid_bundle()
	{
		auto content = source_bytes();
		const auto source_digest = cxxlens::sdk::content_digest(content);
		const auto source_size = static_cast<std::int64_t>(content.size());
		const std::array file_fields{
			canonical_value::from_string("project"),
			canonical_value::from_string("src/main.cpp"),
			canonical_value::from_string("cxxlens.logical-path.v1"),
		};
		auto source_file_id = cxxlens::sdk::canonical_identity_digest("file", file_fields);
		assert(source_file_id);
		const std::array snapshot_fields{
			canonical_value::from_string(*source_file_id),
			canonical_value::from_string(source_digest),
			canonical_value::from_string("utf8"),
		};
		auto source_snapshot =
			cxxlens::sdk::canonical_identity_digest("source-snapshot", snapshot_fields);
		assert(source_snapshot);
		auto toolchain = canonical_value::from_tuple({
			canonical_value::from_string("gcc"),
			canonical_value::from_string("16.2.0"),
			observed(canonical_value::from_string("/opt/gcc-16.2.0/bin/g++")),
			observed(canonical_value::from_string(digest('1'))),
			canonical_value::from_string("x86_64-linux-gnu"),
			unavailable("no-sysroot", "capture-effective-sysroot"),
			canonical_value::from_string(digest('2')),
			canonical_value::from_string(digest('3')),
			canonical_value::from_string(digest('4')),
			canonical_value::from_string(digest('5')),
		});
		auto environment = canonical_value::from_tuple({canonical_value::from_tuple({
			canonical_value::from_string("gcc.cpath"),
			derived(canonical_value::from_string("project://include")),
		})});
		auto unit = canonical_value::from_tuple({
			canonical_value::from_string("compile-unit:main"),
			observed(canonical_value::from_string(*source_snapshot)),
			canonical_value::from_string(*source_file_id),
			canonical_value::from_string("project://src/main.cpp"),
			canonical_value::from_string(source_digest),
			canonical_value::from_integer(source_size),
			canonical_value::from_string("project://build"),
			canonical_value::from_string("c++"),
			observed(canonical_value::from_tuple({
				canonical_value::from_string("/opt/gcc-16.2.0/bin/g++"),
				canonical_value::from_string("-std=gnu++23"),
				canonical_value::from_string("/workspace/example/src/main.cpp"),
			})),
			observed(canonical_value::from_tuple({})),
			unavailable("config-files-unobserved", "capture-config-files"),
			observed(std::move(environment)),
			observed(canonical_value::from_string("/workspace/example/build")),
			observed(canonical_value::from_string("gnu++23")),
			observed(canonical_value::from_string("gnu")),
			canonical_value::from_string("source-closure:pending"),
		});
		auto closure = canonical_value::from_tuple({
			canonical_value::from_string("source-closure:pending"),
			canonical_value::from_string(digest('7')),
			canonical_value::from_string(digest('8')),
			canonical_value::from_integer(1),
			canonical_value::from_integer(1),
			canonical_value::from_integer(source_size),
			canonical_value::from_tuple({canonical_value::from_tuple({
				canonical_value::from_string(*source_file_id),
				canonical_value::from_string("project://src/main.cpp"),
				observed(canonical_value::from_string(source_digest)),
				observed(canonical_value::from_bytes(std::move(content))),
				canonical_value::from_integer(source_size),
				observed(canonical_value::from_string("main")),
				observed(canonical_value::from_string("utf8")),
				canonical_value::from_boolean(true),
			})}),
		});
		auto gaps = canonical_value::from_tuple({
			gap("compile_units[0].config_files", "config-files-unobserved", "capture-config-files"),
			gap("production_toolchain.sysroot", "no-sysroot", "capture-effective-sysroot"),
		});
		auto bundle = canonical_value::from_tuple({
			canonical_value::from_string("cxxlens.build-capture-bundle.v1"),
			std::move(toolchain),
			canonical_value::from_string("shell-free-wrapper"),
			canonical_value::from_string("x86_64-linux-gnu"),
			canonical_value::from_string("project:gcc-example"),
			canonical_value::from_tuple({std::move(unit)}),
			canonical_value::from_tuple({std::move(closure)}),
			std::move(gaps),
			canonical_value::from_string("project://"),
			observed(canonical_value::from_tuple({canonical_value::from_tuple({
				canonical_value::from_string("/workspace/example"),
				canonical_value::from_string("project://"),
			})})),
		});
		rebind_source_closure(bundle);
		return bundle;
	}

	[[nodiscard]] cxxlens::sdk::relation_descriptor request_descriptor()
	{
		cxxlens::sdk::relation_descriptor value;
		value.id = "test.application_analysis.v1";
		value.name = "test.application_analysis";
		value.version = {1U, 0U, 0U};
		value.semantic_major = 1U;
		value.semantics = "test.application-analysis/1";
		value.owner_namespace = "test";
		value.columns = {{"test.application_analysis.v1.key",
						  "key",
						  {cxxlens::sdk::scalar_kind::utf8_string, {}, false},
						  true,
						  cxxlens::sdk::column_role::claim_key}};
		value.key_columns = {value.columns.front().id};
		value.descriptor_digest =
			*cxxlens::sdk::semantic_digest("cxxlens.relation-descriptor-binding.v2",
										   value.contract_digest + "\n" + value.canonical_form());
		return value;
	}

	[[nodiscard]] bool has_reason(const std::span<const cxxlens::sdk::capture_gap> gaps,
								  const std::string_view reason)
	{
		return std::ranges::any_of(gaps,
								   [&](const auto& gap)
								   {
									   return gap.reason == reason;
								   });
	}

	void positive_decode_and_deterministic_import()
	{
		auto bytes = canonical_binary(valid_bundle());
		assert(bytes);
		auto decoded = cxxlens::sdk::decode_capture_bundle(*bytes);
		auto decoded_again = cxxlens::sdk::decode_capture_bundle(*bytes);
		assert(decoded);
		assert(decoded_again && decoded_again->digest() == decoded->digest());
		assert(decoded->production_compiler() == "gcc-16.2.0");
		assert(decoded->capture_adapter() == "shell-free-wrapper");
		assert(decoded->target_abi() == "x86_64-linux-gnu");
		assert(decoded->project_id() == "project:gcc-example");
		assert(decoded->logical_project_root() == "project://");
		assert(decoded->compile_unit_count() == 1U);
		assert(decoded->gaps().size() == 2U);
		assert(decoded->digest() == cxxlens::sdk::content_digest(*bytes));
		auto imported = cxxlens::sdk::import_capture(*decoded);
		auto imported_again = cxxlens::sdk::import_capture(*decoded_again);
		assert(imported && imported_again);
		assert(imported->id() == imported_again->id());
		assert(imported->capture_bundle_digest() == decoded->digest());
		assert(imported->replay_plans().size() == 1U);
		const auto& plan = imported->replay_plans().front();
		assert(plan.digest() == imported_again->replay_plans().front().digest());
		assert(plan.capture_bundle_digest() == decoded->digest());
		assert(plan.compile_unit_id() == "compile-unit:main");
		assert(plan.analysis_frontend() == "clang-23.1.0-gcc-mode");
		assert(plan.target_abi() == "x86_64-linux-gnu");
		assert(has_reason(plan.unresolved(), "analysis-frontend-differs-from-production-compiler"));
		assert(has_reason(plan.unresolved(), "gcc-extension-fidelity-not-proved-for-clang-replay"));
		assert(imported->unresolved().size() == plan.unresolved().size());
	}

	void replay_fidelity_and_import_bounds_fail_closed()
	{
		auto strict = valid_bundle();
		strict.tuple[5].tuple[0].tuple[8].tuple[1].tuple[1] =
			canonical_value::from_string("-std=c++23");
		strict.tuple[5].tuple[0].tuple[13] = observed(canonical_value::from_string("c++23"));
		strict.tuple[5].tuple[0].tuple[14] = observed(canonical_value::from_string("strict"));
		auto strict_bytes = canonical_binary(strict);
		assert(strict_bytes);
		auto strict_bundle = cxxlens::sdk::decode_capture_bundle(*strict_bytes);
		assert(strict_bundle);
		auto strict_import = cxxlens::sdk::import_capture(*strict_bundle);
		assert(strict_import);
		assert(!has_reason(strict_import->unresolved(),
						   "gcc-extension-fidelity-not-proved-for-clang-replay"));

		auto unknown = valid_bundle();
		unknown.tuple[5].tuple[0].tuple[8].tuple[1].tuple.insert(
			unknown.tuple[5].tuple[0].tuple[8].tuple[1].tuple.begin() + 2,
			canonical_value::from_string("-fvendor-mode"));
		auto unknown_bytes = canonical_binary(unknown);
		assert(unknown_bytes);
		auto unknown_bundle = cxxlens::sdk::decode_capture_bundle(*unknown_bytes);
		assert(unknown_bundle);
		auto unknown_import = cxxlens::sdk::import_capture(*unknown_bundle);
		assert(unknown_import &&
			   has_reason(unknown_import->unresolved(), "gcc-option-not-classified"));

		cxxlens::sdk::import_limits limits;
		limits.maximum_arguments_per_unit = 2U;
		auto bounded = cxxlens::sdk::import_capture(*unknown_bundle, limits);
		assert(!bounded && bounded.error().code == "application-analysis.import-limit-exceeded");

		auto missing_output = valid_bundle();
		missing_output.tuple[5].tuple[0].tuple[8].tuple[1].tuple.push_back(
			canonical_value::from_string("-o"));
		auto missing_output_bytes = canonical_binary(missing_output);
		assert(missing_output_bytes);
		auto missing_output_bundle = cxxlens::sdk::decode_capture_bundle(*missing_output_bytes);
		assert(missing_output_bundle);
		auto missing_output_import = cxxlens::sdk::import_capture(*missing_output_bundle);
		assert(!missing_output_import &&
			   missing_output_import.error().detail == "missing-output-path");

		auto empty_argv = valid_bundle();
		empty_argv.tuple[5].tuple[0].tuple[8] = observed(canonical_value::from_tuple({}));
		auto empty_argv_bytes = canonical_binary(empty_argv);
		assert(empty_argv_bytes);
		auto empty_argv_bundle = cxxlens::sdk::decode_capture_bundle(*empty_argv_bytes);
		assert(empty_argv_bundle);
		auto empty_argv_import = cxxlens::sdk::import_capture(*empty_argv_bundle);
		assert(!empty_argv_import &&
			   empty_argv_import.error().code == "application-analysis.target-unavailable");
	}

	void duplicate_source_variants_bind_one_closure()
	{
		auto bundle = valid_bundle();
		auto second = bundle.tuple[5].tuple[0];
		second.tuple[0] = canonical_value::from_string("compile-unit:variant-two");
		bundle.tuple[5].tuple.push_back(std::move(second));
		bundle.tuple[7].tuple.insert(bundle.tuple[7].tuple.begin() + 1,
									 gap("compile_units[1].config_files",
										 "config-files-unobserved",
										 "capture-config-files"));
		auto bytes = canonical_binary(bundle);
		assert(bytes);
		auto decoded = cxxlens::sdk::decode_capture_bundle(*bytes);
		assert(decoded && decoded->compile_unit_count() == 2U);
		auto imported = cxxlens::sdk::import_capture(*decoded);
		assert(imported && imported->replay_plans().size() == 2U);
		assert(std::ranges::none_of(imported->replay_plans()[0].unresolved(),
									[](const auto& value)
									{
										return value.field.starts_with("compile_units[1].");
									}));
		assert(std::ranges::none_of(imported->replay_plans()[1].unresolved(),
									[](const auto& value)
									{
										return value.field.starts_with("compile_units[0].");
									}));

		auto missing_closure = std::move(bundle);
		missing_closure.tuple[5].tuple[1].tuple[15] =
			canonical_value::from_string("source-closure:missing");
		auto missing_bytes = canonical_binary(missing_closure);
		assert(missing_bytes);
		auto missing = cxxlens::sdk::decode_capture_bundle(*missing_bytes);
		assert(!missing && missing.error().detail == "reference-mismatch");
	}

	void resource_and_shape_fail_closed()
	{
		auto bytes = canonical_binary(valid_bundle());
		assert(bytes);
		cxxlens::sdk::import_limits limits;
		limits.maximum_bundle_bytes = bytes->size() - 1U;
		auto oversized = cxxlens::sdk::decode_capture_bundle(*bytes, limits);
		assert(!oversized &&
			   oversized.error().code == "application-analysis.import-limit-exceeded");

		auto mismatched = valid_bundle();
		mismatched.tuple[2] = canonical_value::from_string("msbuild-cltool-proxy");
		auto mismatch_bytes = canonical_binary(mismatched);
		assert(mismatch_bytes);
		auto mismatch = cxxlens::sdk::decode_capture_bundle(*mismatch_bytes);
		assert(!mismatch && mismatch.error().detail == "toolchain-mismatch");

		auto missing_gap = valid_bundle();
		missing_gap.tuple[7].tuple.pop_back();
		auto missing_gap_bytes = canonical_binary(missing_gap);
		assert(missing_gap_bytes);
		auto gap_result = cxxlens::sdk::decode_capture_bundle(*missing_gap_bytes);
		assert(!gap_result && gap_result.error().detail == "census-mismatch");

		auto partial_source = valid_bundle();
		partial_source.tuple[6].tuple[0].tuple[4] = canonical_value::from_integer(0);
		partial_source.tuple[6].tuple[0].tuple[5] = canonical_value::from_integer(0);
		partial_source.tuple[6].tuple[0].tuple[6].tuple[0].tuple[3] =
			unavailable("source-bytes-unavailable", "recapture-source-closure");
		partial_source.tuple[7].tuple.push_back(gap("source_closures[0].members[0].content",
													"source-bytes-unavailable",
													"recapture-source-closure"));
		rebind_source_closure(partial_source);
		auto partial_source_bytes = canonical_binary(partial_source);
		assert(partial_source_bytes);
		auto partial_source_result = cxxlens::sdk::decode_capture_bundle(*partial_source_bytes);
		assert(partial_source_result && partial_source_result->gaps().size() == 3U);

		auto forged_blob_census = valid_bundle();
		forged_blob_census.tuple[6].tuple[0].tuple[4] = canonical_value::from_integer(0);
		auto forged_blob_census_bytes = canonical_binary(forged_blob_census);
		assert(forged_blob_census_bytes);
		auto forged_blob_census_result =
			cxxlens::sdk::decode_capture_bundle(*forged_blob_census_bytes);
		assert(!forged_blob_census_result &&
			   forged_blob_census_result.error().detail == "blob-census-mismatch");

		auto tampered_source = valid_bundle();
		tampered_source.tuple[6].tuple[0].tuple[6].tuple[0].tuple[3] =
			observed(canonical_value::from_bytes({std::byte{0x78}}));
		auto tampered_source_bytes = canonical_binary(tampered_source);
		assert(tampered_source_bytes);
		auto tampered_source_result = cxxlens::sdk::decode_capture_bundle(*tampered_source_bytes);
		assert(!tampered_source_result &&
			   tampered_source_result.error().detail == "digest-or-size-mismatch");

		auto missing_source = valid_bundle();
		missing_source.tuple[6].tuple[0].tuple[6].tuple[0].tuple[0] =
			canonical_value::from_string("source-file:other");
		rebind_source_closure(missing_source);
		auto missing_source_bytes = canonical_binary(missing_source);
		assert(missing_source_bytes);
		auto missing_source_result = cxxlens::sdk::decode_capture_bundle(*missing_source_bytes);
		assert(!missing_source_result &&
			   missing_source_result.error().detail == "binding-mismatch");

		auto forged_snapshot = valid_bundle();
		forged_snapshot.tuple[5].tuple[0].tuple[1] =
			observed(canonical_value::from_string("source-snapshot:forged"));
		auto forged_snapshot_bytes = canonical_binary(forged_snapshot);
		assert(forged_snapshot_bytes);
		auto forged_snapshot_result = cxxlens::sdk::decode_capture_bundle(*forged_snapshot_bytes);
		assert(!forged_snapshot_result &&
			   forged_snapshot_result.error().detail == "source-snapshot-mismatch");

		auto non_main_source = valid_bundle();
		non_main_source.tuple[6].tuple[0].tuple[6].tuple[0].tuple[5] =
			observed(canonical_value::from_string("header"));
		rebind_source_closure(non_main_source);
		auto non_main_bytes = canonical_binary(non_main_source);
		assert(non_main_bytes);
		auto non_main_result = cxxlens::sdk::decode_capture_bundle(*non_main_bytes);
		assert(!non_main_result &&
			   non_main_result.error().detail == "compile-unit-source-mismatch");

		auto forged_closure_id = valid_bundle();
		forged_closure_id.tuple[6].tuple[0].tuple[0] =
			canonical_value::from_string("source-closure:forged");
		forged_closure_id.tuple[5].tuple[0].tuple[15] =
			canonical_value::from_string("source-closure:forged");
		auto forged_closure_bytes = canonical_binary(forged_closure_id);
		assert(forged_closure_bytes);
		auto forged_closure_result = cxxlens::sdk::decode_capture_bundle(*forged_closure_bytes);
		assert(!forged_closure_result &&
			   forged_closure_result.error().detail == "binding-mismatch");

		auto raw_environment_name = valid_bundle();
		raw_environment_name.tuple[5].tuple[0].tuple[11].tuple[1].tuple[0].tuple[0] =
			canonical_value::from_string("CPATH");
		auto raw_environment_bytes = canonical_binary(raw_environment_name);
		assert(raw_environment_bytes);
		auto raw_environment_result = cxxlens::sdk::decode_capture_bundle(*raw_environment_bytes);
		assert(!raw_environment_result && raw_environment_result.error().field.ends_with(".name"));

		auto recursive = valid_bundle();
		recursive.tuple[5].tuple[0].tuple[9] = observed(canonical_value::from_tuple({
			canonical_value::from_tuple({
				canonical_value::from_string("project://build/options.rsp"),
				observed(canonical_value::from_string(digest('9'))),
				canonical_value::from_integer(12),
				canonical_value::from_integer(0),
			}),
		}));
		auto recursive_bytes = canonical_binary(recursive);
		assert(recursive_bytes);
		auto recursive_result = cxxlens::sdk::decode_capture_bundle(*recursive_bytes);
		assert(!recursive_result && recursive_result.error().detail == "recursive-reference");

		auto unpinned = valid_bundle();
		unpinned.tuple[1].tuple[1] = canonical_value::from_string("16.3.0");
		auto unpinned_bytes = canonical_binary(unpinned);
		assert(unpinned_bytes);
		auto unpinned_result = cxxlens::sdk::decode_capture_bundle(*unpinned_bytes);
		assert(!unpinned_result && unpinned_result.error().detail == "not-pinned");

		auto relative_compiler = valid_bundle();
		relative_compiler.tuple[1].tuple[2] =
			observed(canonical_value::from_string("toolchain/bin/g++"));
		auto relative_compiler_bytes = canonical_binary(relative_compiler);
		assert(relative_compiler_bytes);
		auto relative_compiler_result =
			cxxlens::sdk::decode_capture_bundle(*relative_compiler_bytes);
		assert(!relative_compiler_result &&
			   relative_compiler_result.error().detail == "not-canonical-absolute");

		auto unmapped_working_directory = valid_bundle();
		unmapped_working_directory.tuple[5].tuple[0].tuple[12] =
			observed(canonical_value::from_string("/another/build"));
		auto unmapped_bytes = canonical_binary(unmapped_working_directory);
		assert(unmapped_bytes);
		auto unmapped_result = cxxlens::sdk::decode_capture_bundle(*unmapped_bytes);
		assert(!unmapped_result && unmapped_result.error().detail == "unmapped-physical-path");

		auto overlapping_mapping = valid_bundle();
		overlapping_mapping.tuple[9] = observed(canonical_value::from_tuple({
			canonical_value::from_tuple({canonical_value::from_string("/workspace"),
										 canonical_value::from_string("project://")}),
			canonical_value::from_tuple({canonical_value::from_string("/workspace/example"),
										 canonical_value::from_string("project://src")}),
		}));
		auto overlapping_bytes = canonical_binary(overlapping_mapping);
		assert(overlapping_bytes);
		auto overlapping_result = cxxlens::sdk::decode_capture_bundle(*overlapping_bytes);
		assert(!overlapping_result && overlapping_result.error().detail == "overlapping-authority");

		auto excessive_mappings = valid_bundle();
		cxxlens::sdk::import_limits path_limits;
		path_limits.maximum_path_mappings = 1U;
		excessive_mappings.tuple[9] = observed(canonical_value::from_tuple({
			canonical_value::from_tuple({canonical_value::from_string("/external"),
										 canonical_value::from_string("project://external")}),
			canonical_value::from_tuple({canonical_value::from_string("/workspace/example"),
										 canonical_value::from_string("project://")}),
		}));
		auto excessive_mapping_bytes = canonical_binary(excessive_mappings);
		assert(excessive_mapping_bytes);
		auto excessive_mapping_result =
			cxxlens::sdk::decode_capture_bundle(*excessive_mapping_bytes, path_limits);
		assert(!excessive_mapping_result &&
			   excessive_mapping_result.error().code ==
				   "application-analysis.import-limit-exceeded");

		auto escaping_logical_path = valid_bundle();
		escaping_logical_path.tuple[5].tuple[0].tuple[3] =
			canonical_value::from_string("external://src/main.cpp");
		auto escaping_bytes = canonical_binary(escaping_logical_path);
		assert(escaping_bytes);
		auto escaping_result = cxxlens::sdk::decode_capture_bundle(*escaping_bytes);
		assert(!escaping_result &&
			   escaping_result.error().detail == "duplicate-or-noncanonical-order");

		canonical_value nested = canonical_value::from_string("leaf");
		for (std::size_t depth{}; depth < 12U; ++depth)
			nested = canonical_value::from_tuple({std::move(nested)});
		auto nested_bytes = canonical_binary(nested);
		assert(nested_bytes);
		limits = {};
		limits.maximum_nesting_depth = 8U;
		auto nested_result = cxxlens::sdk::decode_capture_bundle(*nested_bytes, limits);
		assert(!nested_result && nested_result.error().detail == "nesting-depth");
	}

	void materialization_request_factory_validates_authority()
	{
		cxxlens::sdk::relation_registry registry;
		auto descriptor = request_descriptor();
		assert(registry.add(descriptor));
		auto engine = registry.build("application-analysis-test");
		assert(engine);
		const auto policies = cxxlens::sdk::provider::builtin_sandbox_policies();
		assert(!policies.empty());
		cxxlens::sdk::provider::provider_selection_request provider;
		provider.provider_id = "provider:gcc-replay";
		provider.provider_version = {1U, 0U, 0U};
		provider.provider_binary_digest = digest('a');
		provider.provider_semantic_contract_digest = digest('b');
		provider.sandbox = {cxxlens::sdk::provider::sandbox_assurance::enforced,
							policies.front().policy_digest()};
		cxxlens::sdk::snapshot_draft publication{{"catalog:test",
												  "experimental",
												  std::string{engine->generation()},
												  "condition:test",
												  std::string{engine->registry_digest()},
												  digest('c'),
												  digest('d')},
												 {1U, 0U, 0U},
												 digest('e'),
												 std::nullopt};
		auto request = cxxlens::sdk::materialization_request::make(
			*engine, publication, {descriptor.id}, "cc.clang23-gcc-replay-1", provider);
		assert(request);
		assert(request->relation_descriptor_ids().size() == 1U);
		assert(request->interpretation() == "cc.clang23-gcc-replay-1");

		auto duplicate = cxxlens::sdk::materialization_request::make(*engine,
																	 publication,
																	 {descriptor.id, descriptor.id},
																	 "cc.clang23-gcc-replay-1",
																	 provider);
		assert(!duplicate && duplicate.error().detail == "duplicate");
		auto zero_budget = cxxlens::sdk::provider::execution_budget{};
		zero_budget.output_bytes = 0U;
		auto invalid_budget = cxxlens::sdk::materialization_request::make(*engine,
																		  std::move(publication),
																		  {descriptor.id},
																		  "cc.clang23-gcc-replay-1",
																		  std::move(provider),
																		  zero_budget);
		assert(!invalid_budget && invalid_budget.error().field == "budget");
	}
} // namespace

int main()
{
	positive_decode_and_deterministic_import();
	replay_fidelity_and_import_bounds_fail_closed();
	duplicate_source_variants_bind_one_closure();
	resource_and_shape_fail_closed();
	materialization_request_factory_validates_authority();
}
