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

	[[nodiscard]] canonical_value valid_bundle()
	{
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
			canonical_value::from_string("CPATH"),
			derived(canonical_value::from_string("project://include")),
		})});
		auto unit = canonical_value::from_tuple({
			canonical_value::from_string("compile-unit:main"),
			canonical_value::from_string("source-snapshot:one"),
			canonical_value::from_string("source-file:main"),
			canonical_value::from_string("project://src/main.cpp"),
			canonical_value::from_string(digest('6')),
			canonical_value::from_integer(42),
			canonical_value::from_string("project://build"),
			canonical_value::from_string("c++"),
			observed(canonical_value::from_tuple({
				canonical_value::from_string("/opt/gcc-16.2.0/bin/g++"),
				canonical_value::from_string("-std=c++23"),
				canonical_value::from_string("project://src/main.cpp"),
			})),
			observed(canonical_value::from_tuple({})),
			unavailable("config-files-unobserved", "capture-config-files"),
			observed(std::move(environment)),
		});
		auto closure = canonical_value::from_tuple({
			canonical_value::from_string("source-closure:one"),
			canonical_value::from_string(digest('7')),
			canonical_value::from_string(digest('8')),
			canonical_value::from_integer(1),
			canonical_value::from_integer(1),
			canonical_value::from_integer(42),
		});
		auto gaps = canonical_value::from_tuple({
			gap("compile_units[0].config_files", "config-files-unobserved", "capture-config-files"),
			gap("production_toolchain.sysroot", "no-sysroot", "capture-effective-sysroot"),
		});
		return canonical_value::from_tuple({
			canonical_value::from_string("cxxlens.build-capture-bundle.v1"),
			std::move(toolchain),
			canonical_value::from_string("shell-free-wrapper"),
			canonical_value::from_string("x86_64-linux-gnu"),
			canonical_value::from_string("project:gcc-example"),
			canonical_value::from_tuple({std::move(unit)}),
			std::move(closure),
			std::move(gaps),
		});
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

	void positive_decode_and_unavailable_import()
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
		assert(decoded->compile_unit_count() == 1U);
		assert(decoded->gaps().size() == 2U);
		assert(decoded->digest() == cxxlens::sdk::content_digest(*bytes));
		auto imported = cxxlens::sdk::import_capture(*decoded);
		assert(!imported);
		assert(imported.error().code == "application-analysis.target-unavailable");
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
	positive_decode_and_unavailable_import();
	resource_and_shape_fail_closed();
	materialization_request_factory_validates_authority();
}
