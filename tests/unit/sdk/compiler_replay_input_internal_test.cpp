#include "sdk/compiler_replay_input_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "sdk/application_build_capture_adapter_internal.hpp"
#include "sdk/source_identity_internal.hpp"

namespace
{
	template <class value_type>
	void require(const value_type& condition)
	{
		if (!static_cast<bool>(condition))
			std::abort();
	}

	[[nodiscard]] std::string digest(const char digit)
	{
		return "sha256:" + std::string(64U, digit);
	}

	[[nodiscard]] std::vector<std::byte> bytes(const std::string& value)
	{
		std::vector<std::byte> output;
		for (const auto byte : value)
			output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
		return output;
	}

	[[nodiscard]] cxxlens::sdk::detail::compiler_replay_input_draft draft()
	{
		using namespace cxxlens::sdk;
		detail::compiler_replay_input_draft value;
		value.imported_project_id = "imported-project:" + digest('1');
		value.capture_bundle_digest = digest('2');
		value.replay_plan_digest = digest('3');
		value.compile_unit_id = "compile-unit:main";
		value.analysis_frontend = "clang-23.1.0-gcc-mode";
		value.target_abi = "x86_64-linux-gnu";
		value.effective_arguments = {"clang++", "-fsyntax-only", "project://src/main.cpp"};
		detail::decoded_capture_source_member source;
		source.logical_path = "project://src/main.cpp";
		source.content = bytes("int main() { return 0; }\n");
		source.content_digest = content_digest(source.content);
		source.role = "main";
		source.encoding = "utf8";
		auto source_file = detail::derive_source_file_id("src/main.cpp");
		require(source_file);
		source.file_id = *source_file;
		auto source_snapshot = detail::derive_source_snapshot_id(
			source.file_id, source.content_digest, *source.encoding);
		require(source_snapshot);
		source.source_snapshot_id = *source_snapshot;
		value.source_members.push_back(std::move(source));
		value.source_closure_digest = "application-source-closure:" + digest('4');
		value.requested_relation_descriptor_ids = {"source.file.v1", "build.project.v1"};
		value.interpretation = "cc.clang23-gcc-replay-1";
		value.unresolved = {
			{"z.field", "unavailable", "z-reason", "capture-z"},
			{"a.field", "unavailable", "a-reason", "capture-a"},
			{"a.field", "unavailable", "a-reason", "capture-a"},
		};
		return value;
	}

	void deterministic_round_trip_canonicalizes_set_fields()
	{
		using namespace cxxlens::sdk;
		auto first = detail::validate_compiler_replay_input(draft());
		auto second_draft = draft();
		std::ranges::reverse(second_draft.requested_relation_descriptor_ids);
		std::ranges::reverse(second_draft.unresolved);
		auto second = detail::validate_compiler_replay_input(std::move(second_draft));
		require(first && second);
		require(std::ranges::equal(first->bytes(), second->bytes()));
		require(first->input_digest() == second->input_digest());
		require(first->value().requested_relation_descriptor_ids.front() == "build.project.v1");
		require(first->value().unresolved.size() == 2U);
		auto decoded = detail::decode_compiler_replay_input(first->bytes());
		require(decoded && decoded->input_digest() == first->input_digest());
		require(decoded->value().source_members.front().content ==
				first->value().source_members.front().content);
	}

	void malformed_authority_and_source_content_fail_closed()
	{
		using namespace cxxlens::sdk;
		auto wrong_frontend = draft();
		wrong_frontend.analysis_frontend = "clang-24-gcc-mode";
		auto frontend = detail::validate_compiler_replay_input(std::move(wrong_frontend));
		require(!frontend && frontend.error().field == "frontend");

		auto forged_source = draft();
		forged_source.source_members.front().content.push_back(std::byte{'x'});
		auto source = detail::validate_compiler_replay_input(std::move(forged_source));
		require(!source && source.error().detail == "digest-mismatch");

		auto escaping = draft();
		escaping.source_members.front().logical_path = "project://../secret.hpp";
		auto path = detail::validate_compiler_replay_input(std::move(escaping));
		require(!path && path.error().detail == "canonical-unique");

		auto wrong_main = draft();
		wrong_main.effective_arguments.back() = "project://src/other.cpp";
		auto binding = detail::validate_compiler_replay_input(std::move(wrong_main));
		require(!binding && binding.error().field == "effective_argv" &&
				binding.error().detail == "main-source-binding");
		auto duplicate_main = draft();
		duplicate_main.effective_arguments.push_back("project://src/main.cpp");
		auto duplicate_binding = detail::validate_compiler_replay_input(std::move(duplicate_main));
		require(!duplicate_binding && duplicate_binding.error().detail == "main-source-binding");
		auto source_before_option = draft();
		source_before_option.effective_arguments.push_back("-DVALUE=1");
		auto ordered = detail::validate_compiler_replay_input(std::move(source_before_option));
		require(ordered);

		auto duplicate_relation = draft();
		duplicate_relation.requested_relation_descriptor_ids = {"source.file.v1", "source.file.v1"};
		auto duplicate = detail::validate_compiler_replay_input(std::move(duplicate_relation));
		require(!duplicate && duplicate.error().detail == "duplicate");
	}

	void frontend_abi_and_executable_mode_are_one_authority_tuple()
	{
		using namespace cxxlens::sdk;
		auto msvc = draft();
		msvc.analysis_frontend = "clang-cl-23.1.0";
		msvc.target_abi = "x86_64-pc-windows-msvc";
		msvc.effective_arguments = {"clang-cl", "/Zs", "project://src/main.cpp"};
		msvc.interpretation = "cc.clangcl23-msvc-replay-1";
		auto validated = detail::validate_compiler_replay_input(msvc);
		require(validated);
		auto msvc_contract = detail::resolve_compiler_replay_frontend(
			msvc.analysis_frontend, msvc.target_abi, msvc.effective_arguments);
		require(msvc_contract && msvc_contract->dependency_group == "clangcl23-msvc-replay" &&
				msvc_contract->output_normalizer == "clangcl23-msvc-replay-output-normalizer.v1" &&
				msvc_contract->observation_technique == "clang_cl_msvc_replay");
		require(
			detail::is_compiler_replay_observation_technique(msvc_contract->observation_technique));
		require(!detail::is_compiler_replay_observation_technique("compiler-replay-unknown"));
		auto decoded = detail::decode_compiler_replay_input(validated->bytes());
		require(decoded && decoded->value().analysis_frontend == msvc.analysis_frontend);

		auto mismatched_abi = msvc;
		mismatched_abi.target_abi = "x86_64-linux-gnu";
		auto abi = detail::validate_compiler_replay_input(std::move(mismatched_abi));
		require(!abi && abi.error().detail == "unsupported-tuple");
		auto mismatched_mode = std::move(msvc);
		mismatched_mode.effective_arguments.front() = "clang++";
		auto mode = detail::validate_compiler_replay_input(std::move(mismatched_mode));
		require(!mode && mode.error().detail == "unsupported-tuple");

		auto gcc = draft();
		auto gcc_contract = detail::resolve_compiler_replay_frontend(
			gcc.analysis_frontend, gcc.target_abi, gcc.effective_arguments);
		require(gcc_contract && gcc_contract->dependency_group == "clang23-gcc-replay" &&
				gcc_contract->output_normalizer == "clang23-gcc-replay-output-normalizer.v1" &&
				gcc_contract->observation_technique == "clang_gcc_mode_replay");
	}

	void decoder_rejects_noncanonical_order_truncation_and_depth()
	{
		using namespace cxxlens::sdk;
		auto value = detail::validate_compiler_replay_input(draft());
		require(value);
		auto root = canonical_binary_decode(value->bytes());
		require(root);
		std::ranges::reverse(root->tuple[10].tuple);
		auto reordered = canonical_binary(*root);
		require(reordered);
		auto order = detail::decode_compiler_replay_input(*reordered);
		require(!order && order.error().detail == "noncanonical-set-order");

		auto truncated = std::vector<std::byte>{value->bytes().begin(), value->bytes().end() - 1};
		auto short_input = detail::decode_compiler_replay_input(truncated);
		require(!short_input);

		auto limits = import_limits{};
		limits.maximum_nesting_depth = 1U;
		auto nested = detail::decode_compiler_replay_input(value->bytes(), limits);
		require(!nested && nested.error().detail == "nesting-depth");

		limits = {};
		limits.maximum_bundle_bytes = value->bytes().size() - 1U;
		auto bounded = detail::decode_compiler_replay_input(value->bytes(), limits);
		require(!bounded && bounded.error().code == "application-analysis.import-limit-exceeded");
	}

	[[nodiscard]] std::string plan_digest(const cxxlens::sdk::replay_plan::implementation& plan)
	{
		using namespace cxxlens::sdk;
		std::vector<canonical_value> arguments;
		for (const auto& argument : plan.effective_arguments)
			arguments.push_back(canonical_value::from_string(argument));
		std::vector<canonical_value> unresolved;
		for (const auto& gap : plan.unresolved)
			unresolved.push_back(canonical_value::from_tuple({
				canonical_value::from_string(gap.field),
				canonical_value::from_string(gap.state),
				canonical_value::from_string(gap.reason),
				canonical_value::from_string(gap.completion_action),
			}));
		auto encoded = canonical_binary(canonical_value::from_tuple({
			canonical_value::from_string("cxxlens.compiler-replay-plan.v1"),
			canonical_value::from_string(plan.capture_bundle_digest),
			canonical_value::from_string(plan.compile_unit_id),
			canonical_value::from_string(plan.analysis_frontend),
			canonical_value::from_string(plan.target_abi),
			canonical_value::from_tuple(std::move(arguments)),
			canonical_value::from_tuple({}),
			canonical_value::from_string(plan.source_closure_digest),
			canonical_value::from_tuple(std::move(unresolved)),
		}));
		require(encoded);
		return content_digest(*encoded);
	}

	[[nodiscard]] std::pair<cxxlens::sdk::imported_project::implementation,
							cxxlens::sdk::replay_plan::implementation>
	exact_imported_project()
	{
		using namespace cxxlens::sdk;
		auto input = draft();
		auto capture = std::make_shared<capture_bundle::implementation>();
		capture->digest = input.capture_bundle_digest;
		capture->project_id = "project:gcc-replay";
		capture->projection.toolchain_family = "gcc";
		capture->projection.toolchain_version = "16.2.0";
		capture->projection.production_compiler_path = "/opt/gcc-16.2.0/bin/g++";
		capture->projection.production_compiler_binary_digest = digest('5');
		capture->projection.target_triple = "x86_64-linux-gnu";
		capture->projection.abi_digest = digest('6');
		capture->projection.builtin_headers_digest = digest('7');
		capture->projection.builtin_macros_digest = digest('8');
		capture->projection.include_search_digest = digest('9');
		capture->projection.target_abi = input.target_abi;
		capture->projection.logical_project_root = "project://";
		capture->projection.path_mappings = {{"/workspace/example", "project://"}};

		detail::decoded_capture_source_closure closure;
		closure.id = "source-closure:main";
		closure.digest = input.source_closure_digest;
		closure.manifest_digest = digest('a');
		closure.member_count = 1U;
		closure.blob_count = 1U;
		closure.unique_blob_bytes = input.source_members.front().content.size();
		closure.members = input.source_members;
		capture->projection.source_closures.push_back(std::move(closure));

		detail::decoded_capture_unit unit;
		unit.compile_unit_id = input.compile_unit_id;
		unit.source_snapshot_id = input.source_members.front().source_snapshot_id;
		unit.source_file_id = input.source_members.front().file_id;
		unit.source_logical_path = input.source_members.front().logical_path;
		unit.source_content_digest = input.source_members.front().content_digest;
		unit.source_size_bytes = input.source_members.front().content.size();
		unit.logical_working_directory = "project://build";
		unit.language = "c++";
		unit.original_arguments = {
			"/opt/gcc-16.2.0/bin/g++", "-std=gnu++23", "project://src/main.cpp"};
		unit.environment_effects = {
			{"gcc.cpath", "derived", std::string{"project://include"}, {}, {}},
		};
		unit.language_standard = "gnu++23";
		unit.extension_mode = "gnu";
		unit.source_closure_id = "source-closure:main";
		unit.source_closure_digest = input.source_closure_digest;
		capture->projection.compile_units.push_back(std::move(unit));

		replay_plan::implementation plan;
		plan.capture_bundle_digest = capture->digest;
		plan.compile_unit_id = input.compile_unit_id;
		plan.analysis_frontend = input.analysis_frontend;
		plan.target_abi = input.target_abi;
		plan.effective_arguments = input.effective_arguments;
		plan.source_closure_digest = input.source_closure_digest;
		plan.digest = plan_digest(plan);

		imported_project::implementation project;
		project.id = input.imported_project_id;
		project.capture_bundle_digest = capture->digest;
		project.capture = capture;
		auto catalog = project_catalog::make("project://",
											 digest('b'),
											 {{plan.compile_unit_id,
											   digest('c'),
											   input.source_members.front().content_digest,
											   digest('b')}});
		require(catalog);
		project.catalog = std::move(*catalog);
		return {std::move(project), std::move(plan)};
	}

	void imported_capture_adapts_to_generic_capture_authority()
	{
		using namespace cxxlens::sdk;
		auto [project, plan] = exact_imported_project();
		auto first = detail::make_application_build_capture(project, plan);
		require(first);
		require(first->value().catalog.catalog_digest == project.catalog.catalog_digest);
		require(first->value().selected_catalog_compile_unit_id == plan.compile_unit_id);
		require(first->value().toolchain.family == "gcc");
		require(first->value().toolchain.exact_version == "16.2.0");
		require(first->value().invocation.effective_replay_arguments.value ==
				plan.effective_arguments);
		require(first->value().source_closure.closure_digest == plan.source_closure_digest);

		auto relocated = project;
		auto relocated_capture = std::make_shared<capture_bundle::implementation>(*project.capture);
		relocated_capture->projection.production_compiler_path = "/relocated/gcc/bin/g++";
		relocated_capture->projection.path_mappings = {{"/relocated/project", "project://"}};
		relocated.capture = std::move(relocated_capture);
		auto second = detail::make_application_build_capture(relocated, plan);
		require(second && second->semantic_identity() == first->semantic_identity());

		auto incomplete = project;
		auto incomplete_capture =
			std::make_shared<capture_bundle::implementation>(*project.capture);
		incomplete_capture->projection.abi_digest.reset();
		incomplete.capture = std::move(incomplete_capture);
		auto rejected = detail::make_application_build_capture(incomplete, plan);
		require(!rejected &&
				rejected.error().code == "application-analysis.materialization-unavailable" &&
				rejected.error().field == "production_toolchain.abi_digest");

		auto mismatched_plan = plan;
		mismatched_plan.target_abi = "aarch64-linux-gnu";
		auto mismatched = detail::make_application_build_capture(project, mismatched_plan);
		require(!mismatched && mismatched.error().field == "replay_plan" &&
				mismatched.error().detail == "capture-binding-mismatch");
	}

	void msvc_capture_adapts_through_the_same_generic_authority()
	{
		using namespace cxxlens::sdk;
		auto [project, plan] = exact_imported_project();
		auto capture = std::make_shared<capture_bundle::implementation>(*project.capture);
		capture->projection.toolchain_family = "msvc";
		capture->projection.toolchain_version = "19.51.36256";
		capture->projection.target_triple = "x86_64-pc-windows-msvc";
		capture->projection.target_abi = "x86_64-pc-windows-msvc";
		project.capture = std::move(capture);
		plan.analysis_frontend = "clang-cl-23.1.0";
		plan.target_abi = "x86_64-pc-windows-msvc";
		plan.effective_arguments = {"clang-cl", "/Zs", "project://src/main.cpp"};
		plan.digest = plan_digest(plan);
		auto adapted = detail::make_application_build_capture(project, plan);
		require(adapted);
		const std::array relations{std::string{"source.file.v1"}};
		auto provider_input = detail::make_compiler_replay_input(
			project, plan, relations, "cc.clangcl23-msvc-replay-1");
		require(provider_input && provider_input->value().analysis_frontend == "clang-cl-23.1.0");
	}

	void imported_capture_is_the_only_source_closure_authority()
	{
		using namespace cxxlens::sdk;
		auto capture = std::make_shared<capture_bundle::implementation>();
		capture->digest = digest('2');
		detail::decoded_capture_source_closure closure;
		closure.id = "source-closure:main";
		auto input = draft();
		closure.members = input.source_members;
		capture->projection.source_closures.push_back(std::move(closure));
		detail::decoded_capture_unit unit;
		unit.compile_unit_id = "compile-unit:main";
		unit.source_closure_id = "source-closure:main";
		unit.source_closure_digest = input.source_closure_digest;
		capture->projection.compile_units.push_back(std::move(unit));

		imported_project::implementation project;
		project.id = input.imported_project_id;
		project.capture_bundle_digest = capture->digest;
		project.capture = capture;
		replay_plan::implementation plan;
		plan.capture_bundle_digest = capture->digest;
		plan.compile_unit_id = "compile-unit:main";
		plan.analysis_frontend = "clang-23.1.0-gcc-mode";
		plan.target_abi = "x86_64-linux-gnu";
		plan.effective_arguments = input.effective_arguments;
		plan.source_closure_digest = input.source_closure_digest;
		plan.unresolved = input.unresolved;
		std::ranges::sort(
			plan.unresolved,
			[](const auto& left, const auto& right)
			{
				return std::tie(left.field, left.state, left.reason, left.completion_action) <
					std::tie(right.field, right.state, right.reason, right.completion_action);
			});
		plan.unresolved.erase(std::ranges::unique(plan.unresolved).begin(), plan.unresolved.end());
		plan.digest = plan_digest(plan);
		const std::array relations{std::string{"source.file.v1"}};
		auto made =
			detail::make_compiler_replay_input(project, plan, relations, "cc.clang23-gcc-replay-1");
		require(made && made->value().source_members.size() == 1U);
		require(made->value().source_members.front().logical_path ==
				input.source_members.front().logical_path);
		require(made->value().source_members.front().content ==
				input.source_members.front().content);

		project.capture_bundle_digest = digest('9');
		auto forged =
			detail::make_compiler_replay_input(project, plan, relations, "cc.clang23-gcc-replay-1");
		require(!forged && forged.error().detail == "binding-mismatch");
	}
} // namespace

int main()
{
	deterministic_round_trip_canonicalizes_set_fields();
	malformed_authority_and_source_content_fail_closed();
	frontend_abi_and_executable_mode_are_one_authority_tuple();
	decoder_rejects_noncanonical_order_truncation_and_depth();
	imported_capture_adapts_to_generic_capture_authority();
	msvc_capture_adapts_through_the_same_generic_authority();
	imported_capture_is_the_only_source_closure_authority();
}
