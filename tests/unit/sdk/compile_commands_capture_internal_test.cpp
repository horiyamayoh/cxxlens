#include "sdk/compile_commands_capture_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <ranges>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

#include "sdk/gcc_capture_bundle_internal.hpp"

namespace
{
	using cxxlens::sdk::detail::decode_compile_commands;
	using cxxlens::sdk::detail::encode_gcc_compile_commands_bundle;

	void require(const bool condition,
				 const std::source_location location = std::source_location::current())
	{
		if (!condition)
		{
			std::cerr << "requirement failed at line " << location.line() << '\n';
			std::abort();
		}
	}

	void require_error(const std::string_view input,
					   const std::string_view field,
					   const std::string_view detail,
					   cxxlens::sdk::import_limits limits = {})
	{
		auto decoded = decode_compile_commands(input, limits);
		require(!decoded);
		require(decoded.error().field.contains(field));
		if (!decoded.error().detail.contains(detail))
		{
			std::cerr << "expected detail " << detail << ", got " << decoded.error().detail << '\n';
			std::abort();
		}
	}

	void arguments_form_preserves_variants_and_order()
	{
		constexpr std::string_view input = R"json([
  {"directory":"/work/build","file":"../src/main.cpp","output":"main.o",
   "arguments":["/opt/gcc/bin/g++","-DVALUE=two words","-c","../src/main.cpp"]},
  {"directory":"/work/build","file":"../src/main.cpp",
   "arguments":["/opt/gcc/bin/g++","-DVALUE=second","-c","../src/main.cpp"]}
])json";
		auto decoded = decode_compile_commands(input);
		require(decoded && decoded->entries().size() == 2U);
		auto repeated = decode_compile_commands(input);
		require(repeated && repeated->entries() == decoded->entries());
		require(decoded->entries()[0].directory == "/work/build");
		require(decoded->entries()[0].file == "../src/main.cpp");
		require(decoded->entries()[0].output == "main.o");
		require(decoded->entries()[0].arguments[1] == "-DVALUE=two words");
		require(!decoded->entries()[0].decoded_from_command);
		require(decoded->entries()[1].arguments[1] == "-DVALUE=second");
	}

	void command_form_is_decoded_without_shell_expansion()
	{
		constexpr std::string_view input = R"json([
  {"directory":"/work/build","file":"src/main file.cpp",
   "command":"g++ \"-DNAME=a b\" 'src/main file.cpp' '' -o out.o"}
])json";
		auto decoded = decode_compile_commands(input);
		require(decoded && decoded->entries().size() == 1U);
		const auto& entry = decoded->entries().front();
		require(entry.decoded_from_command);
		require(entry.arguments.size() == 6U);
		require(entry.arguments[0] == "g++");
		require(entry.arguments[1] == "-DNAME=a b");
		require(entry.arguments[2] == "src/main file.cpp");
		require(entry.arguments[3].empty());
		require(entry.arguments[4] == "-o" && entry.arguments[5] == "out.o");
	}

	void malformed_and_shell_dependent_inputs_fail_closed()
	{
		require_error("{}", "compile_commands", "array-required");
		require_error("[]", "compile_commands", "empty");
		require_error(R"([{"directory":"relative","file":"a.cpp","arguments":["g++"]}])",
					  "directory",
					  "absolute-path-required");
		require_error(R"([{"directory":"/w","file":"a.cpp"}])",
					  "compile_commands[0]",
					  "exactly-one-command-form-required");
		require_error(
			R"([{"directory":"/w","file":"a.cpp","arguments":["g++"],"command":"g++ a.cpp"}])",
			"compile_commands[0]",
			"exactly-one-command-form-required");
		require_error(R"([{"directory":"/w","file":"a.cpp","command":"g++ $(touch pwn) a.cpp"}])",
					  "command",
					  "shell-expansion");
		require_error(R"([{"directory":"/w","file":"a.cpp","command":"g++ a.cpp; echo pwn"}])",
					  "command",
					  "shell-semantics");
		require_error(R"([{"directory":"/w","file":"a.cpp","command":"g++ 'a.cpp"}])",
					  "command",
					  "unterminated-quote");
		require_error(R"([{"directory":"/w","file":"a.cpp","arguments":["g++"],"extra":true}])",
					  "extra",
					  "unknown-member");
		require_error(R"([{"directory":"/w","file":"a.cpp","arguments":["g++","bad\u0000token"]}])",
					  "arguments[1]",
					  "nul-byte");
	}

	void explicit_bounds_apply_before_adoption()
	{
		auto limits = cxxlens::sdk::import_limits{};
		limits.maximum_compile_units = 1U;
		require_error(
			R"([{"directory":"/w","file":"a.cpp","arguments":["g++"]},{"directory":"/w","file":"b.cpp","arguments":["g++"]}])",
			"compile_commands",
			"compile-unit-count",
			limits);

		limits = {};
		limits.maximum_arguments_per_unit = 2U;
		require_error(R"([{"directory":"/w","file":"a.cpp","arguments":["g++","-c","a.cpp"]}])",
					  "arguments",
					  "argument-count",
					  limits);

		limits = {};
		limits.maximum_string_bytes = 3U;
		require_error(R"([{"directory":"/work","file":"a","arguments":["g++"]}])",
					  "compile_commands",
					  "string-byte-limit",
					  limits);
	}

	[[nodiscard]] cxxlens::sdk::detail::captured_text_observation observed(std::string value)
	{
		return {
			cxxlens::sdk::detail::capture_observation_state::observed, std::move(value), {}, {}};
	}

	[[nodiscard]] std::vector<std::byte> bytes(const std::string_view value)
	{
		return {reinterpret_cast<const std::byte*>(value.data()),
				reinterpret_cast<const std::byte*>(value.data() + value.size())};
	}

	[[nodiscard]] cxxlens::sdk::detail::gcc_compile_commands_bundle_input bundle_input()
	{
		const std::string source{"int main() { return 0; }\n"};
		cxxlens::sdk::detail::gcc_compile_commands_bundle_input input;
		input.project_id = "project:gcc-compile-commands";
		input.physical_project_root = "/work";
		input.toolchain.exact_version = "16.2.0";
		input.toolchain.canonical_binary_path = observed("/opt/gcc-16.2.0/bin/g++");
		input.toolchain.binary_digest = observed("sha256:" + std::string(64U, '1'));
		input.toolchain.target_triple = "x86_64-linux-gnu";
		input.toolchain.sysroot = observed("/opt/gcc-16.2.0/sysroot");
		input.toolchain.abi_digest = observed("sha256:" + std::string(64U, '2'));
		input.toolchain.builtin_headers_digest = observed("sha256:" + std::string(64U, '3'));
		input.toolchain.builtin_macros_digest = observed("sha256:" + std::string(64U, '4'));
		input.toolchain.include_search_digest = observed("sha256:" + std::string(64U, '5'));
		input.sources.push_back(
			{std::vector<std::byte>{
				 reinterpret_cast<const std::byte*>(source.data()),
				 reinterpret_cast<const std::byte*>(source.data() + source.size())},
			 "utf8",
			 {},
			 {}});
		return input;
	}

	[[nodiscard]] bool has_gap(const cxxlens::sdk::capture_bundle& bundle,
							   const std::string_view field,
							   const std::string_view reason)
	{
		return std::ranges::any_of(bundle.gaps(),
								   [&](const auto& gap)
								   {
									   return gap.field == field && gap.reason == reason;
								   });
	}

	void canonical_bundle_projection_is_validated_and_deterministic()
	{
		constexpr std::string_view database = R"json([
  {"directory":"/work/build","file":"../src/main.cpp",
   "arguments":["/opt/gcc-16.2.0/bin/g++","-std=gnu++23","-I/host/a",
                "--sysroot=/machine/a","-c","../src/main.cpp"]}
])json";
		auto capture = decode_compile_commands(database);
		require(static_cast<bool>(capture));
		auto input = bundle_input();
		input.toolchain.sysroot = observed("/machine/a");
		auto encoded = encode_gcc_compile_commands_bundle(*capture, input);
		auto repeated = encode_gcc_compile_commands_bundle(*capture, input);
		require(encoded && repeated && *encoded == *repeated);
		auto relocated_capture = decode_compile_commands(R"json([
  {"directory":"/mirror/build","file":"../src/main.cpp",
   "arguments":["/opt/gcc-16.2.0/bin/g++","-std=gnu++23","-I/host/b",
                "--sysroot=/machine/b","-c","../src/main.cpp"]}
])json");
		require(static_cast<bool>(relocated_capture));
		auto relocated_input = input;
		relocated_input.physical_project_root = "/mirror";
		relocated_input.toolchain.sysroot = observed("/machine/b");
		auto relocated = encode_gcc_compile_commands_bundle(*relocated_capture, relocated_input);
		require(static_cast<bool>(relocated));
		auto encoded_value = cxxlens::sdk::canonical_binary_decode(*encoded);
		auto relocated_value = cxxlens::sdk::canonical_binary_decode(*relocated);
		require(encoded_value && relocated_value);
		require(encoded_value->tuple[5].tuple[0].tuple[0].text ==
				relocated_value->tuple[5].tuple[0].tuple[0].text);

		auto decoded = cxxlens::sdk::decode_capture_bundle(*encoded);
		require(static_cast<bool>(decoded));
		require(decoded->production_compiler() == "gcc-16.2.0");
		require(decoded->capture_adapter() == "compile-commands");
		require(decoded->project_id() == "project:gcc-compile-commands");
		require(decoded->compile_unit_count() == 1U);
		require(has_gap(
			*decoded, "source_closures[0].membership_coverage", "dependency-output-unobserved"));
		require(has_gap(
			*decoded, "compile_units[0].environment_effects", "environment-effects-unobserved"));
		auto imported = cxxlens::sdk::import_capture(*decoded);
		require(imported && imported->replay_plans().size() == 1U);
		require(std::ranges::any_of(imported->unresolved(),
									[](const auto& gap)
									{
										return gap.reason == "dependency-output-unobserved";
									}));
	}

	void projection_rejects_mismatched_or_untrusted_observations()
	{
		constexpr std::string_view database = R"json([
  {"directory":"/work/build","file":"../src/main.cpp",
   "arguments":["g++","-std=c++23","-c","../src/main.cpp"]}
])json";
		auto capture = decode_compile_commands(database);
		require(static_cast<bool>(capture));

		auto missing_source = bundle_input();
		missing_source.sources.clear();
		auto missing = encode_gcc_compile_commands_bundle(*capture, missing_source);
		require(!missing && missing.error().detail == "compile-unit-count-mismatch");

		auto unpinned = bundle_input();
		unpinned.toolchain.exact_version = "16.2.1";
		auto wrong_version = encode_gcc_compile_commands_bundle(*capture, unpinned);
		require(!wrong_version && wrong_version.error().detail == "not-pinned");

		auto outside_capture = decode_compile_commands(
			R"([{"directory":"/outside","file":"main.cpp","arguments":["g++","main.cpp"]}])");
		require(static_cast<bool>(outside_capture));
		auto outside = encode_gcc_compile_commands_bundle(*outside_capture, bundle_input());
		require(!outside && outside.error().detail == "path-outside-project-root");

		auto bounded = bundle_input();
		auto limits = cxxlens::sdk::import_limits{};
		limits.maximum_source_closure_bytes = 4U;
		auto too_large = encode_gcc_compile_commands_bundle(*capture, bounded, limits);
		require(!too_large && too_large.error().detail == "byte-count");
	}

	void duplicate_source_variants_share_one_canonical_closure()
	{
		constexpr std::string_view database = R"json([
  {"directory":"/work/build","file":"../src/main.cpp",
   "arguments":["g++","-DVARIANT=one","-std=c++23","../src/main.cpp"]},
  {"directory":"/work/build","file":"../src/main.cpp",
   "arguments":["g++","-DVARIANT=two","-std=c++23","../src/main.cpp"]}
])json";
		auto capture = decode_compile_commands(database);
		require(static_cast<bool>(capture));
		auto input = bundle_input();
		input.sources.push_back(input.sources.front());
		for (const char variant : {'1', '2'})
		{
			cxxlens::sdk::detail::gcc_invocation_observation observation;
			observation.response_files = cxxlens::sdk::detail::captured_value<
				std::vector<cxxlens::sdk::detail::build_capture_auxiliary_file>>::observed({
				{"project://build/options-" + std::string(1U, variant) + ".rsp",
				 cxxlens::sdk::detail::captured_value<std::string>::observed(
					 "sha256:" + std::string(64U, variant)),
				 1U,
				 std::nullopt},
			});
			observation.source_closure_members = cxxlens::sdk::detail::captured_value<
				std::vector<cxxlens::sdk::detail::gcc_source_closure_member_observation>>::
				observed({{"/work/include/shared.hpp", bytes("#pragma once\n"), "utf8", "header"}});
			input.invocations.push_back(std::move(observation));
		}
		auto encoded = encode_gcc_compile_commands_bundle(*capture, input);
		require(static_cast<bool>(encoded));
		auto value = cxxlens::sdk::canonical_binary_decode(*encoded);
		require(static_cast<bool>(value));
		require(value->tuple[5].tuple.size() == 2U);
		require(value->tuple[6].tuple.size() == 1U);
		require(value->tuple[6].tuple.front().tuple[7].tuple[1].text == "complete");
		require(value->tuple[5].tuple[0].tuple[0].text != value->tuple[5].tuple[1].tuple[0].text);
		require(value->tuple[5].tuple[0].tuple[15].text == value->tuple[5].tuple[1].tuple[15].text);
		for (const auto& unit : value->tuple[5].tuple)
		{
			const auto& arguments = unit.tuple[8].tuple[1].tuple;
			const auto first_variant =
				std::ranges::any_of(arguments,
									[](const auto& argument)
									{
										return argument.text == "-DVARIANT=one";
									});
			const auto expected_response =
				first_variant ? "project://build/options-1.rsp" : "project://build/options-2.rsp";
			require(unit.tuple[9].tuple[1].tuple.front().tuple.front().text == expected_response);
		}
	}

	void observed_source_closure_is_complete_canonical_and_bounded()
	{
		constexpr std::string_view database = R"json([
  {"directory":"/work/build","file":"../src/main.cpp",
   "arguments":["g++","@options.rsp","-std=c++23","../src/main.cpp"]}
])json";
		auto capture = decode_compile_commands(database);
		require(static_cast<bool>(capture));
		auto input = bundle_input();
		cxxlens::sdk::detail::gcc_invocation_observation invocation;
		invocation.source_closure_members = cxxlens::sdk::detail::captured_value<
			std::vector<cxxlens::sdk::detail::gcc_source_closure_member_observation>>::observed({
			{"/work/build/options.rsp", bytes("-DVALUE=1\n"), "utf8", "generated"},
			{"/work/include/shared.hpp", bytes("-DVALUE=1\n"), "utf8", "header"},
		});
		input.invocations.push_back(invocation);

		auto encoded = encode_gcc_compile_commands_bundle(*capture, input);
		require(static_cast<bool>(encoded));
		auto decoded = cxxlens::sdk::decode_capture_bundle(*encoded);
		require(static_cast<bool>(decoded));
		require(!has_gap(
			*decoded, "source_closures[0].membership_coverage", "dependency-output-unobserved"));
		auto value = cxxlens::sdk::canonical_binary_decode(*encoded);
		require(static_cast<bool>(value));
		const auto& closure = value->tuple[6].tuple.front().tuple;
		require(closure[3].integer == 3);
		require(closure[4].integer == 2);
		require(closure[5].integer ==
				static_cast<std::int64_t>(input.sources.front().content.size() +
										  bytes("-DVALUE=1\n").size()));
		require(closure[7].tuple[0].text == "observed" && closure[7].tuple[1].text == "complete");
		const auto& members = closure[6].tuple;
		require(std::ranges::is_sorted(members,
									   [](const auto& left, const auto& right)
									   {
										   return left.tuple.front().text <
											   right.tuple.front().text;
									   }));

		auto permuted = input;
		std::ranges::reverse(*permuted.invocations.front().source_closure_members.value);
		auto permuted_encoded = encode_gcc_compile_commands_bundle(*capture, permuted);
		require(permuted_encoded && *permuted_encoded == *encoded);

		auto main_only = bundle_input();
		cxxlens::sdk::detail::gcc_invocation_observation main_only_invocation;
		main_only_invocation.source_closure_members = cxxlens::sdk::detail::captured_value<
			std::vector<cxxlens::sdk::detail::gcc_source_closure_member_observation>>::observed({});
		main_only.invocations.push_back(std::move(main_only_invocation));
		auto main_only_encoded = encode_gcc_compile_commands_bundle(*capture, main_only);
		require(static_cast<bool>(main_only_encoded));
		auto main_only_value = cxxlens::sdk::canonical_binary_decode(*main_only_encoded);
		require(main_only_value && main_only_value->tuple[6].tuple.front().tuple[3].integer == 1 &&
				main_only_value->tuple[6].tuple.front().tuple[7].tuple[1].text == "complete");

		auto duplicate = input;
		duplicate.invocations.front().source_closure_members.value->back().canonical_path =
			"/work/build/options.rsp";
		auto duplicate_result = encode_gcc_compile_commands_bundle(*capture, duplicate);
		require(!duplicate_result && duplicate_result.error().detail == "duplicate-logical-path");

		auto outside = input;
		outside.invocations.front().source_closure_members.value->front().canonical_path =
			"/outside/options.rsp";
		auto outside_result = encode_gcc_compile_commands_bundle(*capture, outside);
		require(!outside_result && outside_result.error().detail == "path-outside-project-root");

		auto invalid_role = input;
		invalid_role.invocations.front().source_closure_members.value->front().role = "main";
		auto role_result = encode_gcc_compile_commands_bundle(*capture, invalid_role);
		require(!role_result && role_result.error().detail == "enum");

		auto limits = cxxlens::sdk::import_limits{};
		limits.maximum_source_closure_members = 2U;
		auto member_bounded = encode_gcc_compile_commands_bundle(*capture, input, limits);
		require(!member_bounded && member_bounded.error().detail == "count");
		limits = {};
		limits.maximum_source_closure_bytes = input.sources.front().content.size();
		auto byte_bounded = encode_gcc_compile_commands_bundle(*capture, input, limits);
		require(!byte_bounded && byte_bounded.error().detail == "byte-count");
	}

	void adapter_observations_replace_only_their_actionable_gaps()
	{
		constexpr std::string_view database = R"json([
  {"directory":"/work/build","file":"../src/main.cpp",
   "arguments":["g++","@options.rsp","-std=c++23","../src/main.cpp"]}
])json";
		auto capture = decode_compile_commands(database);
		require(static_cast<bool>(capture));
		auto input = bundle_input();
		cxxlens::sdk::detail::gcc_invocation_observation invocation;
		invocation.response_files = cxxlens::sdk::detail::captured_value<
			std::vector<cxxlens::sdk::detail::build_capture_auxiliary_file>>::observed({
			{"project://build/options.rsp",
			 cxxlens::sdk::detail::captured_value<std::string>::observed("sha256:" +
																		 std::string(64U, '6')),
			 17U,
			 std::nullopt},
		});
		invocation.config_files = cxxlens::sdk::detail::captured_value<
			std::vector<cxxlens::sdk::detail::build_capture_auxiliary_file>>::observed({});
		invocation.environment_effects = cxxlens::sdk::detail::captured_value<
			std::vector<cxxlens::sdk::detail::build_capture_environment_effect>>::observed({
			{"cpath",
			 cxxlens::sdk::detail::captured_value<std::string>::derived("sha256:" +
																		std::string(64U, '7'))},
			{"secret-mode",
			 cxxlens::sdk::detail::captured_value<std::string>::redacted(
				 "secret-semantic-effect", "supply-workspace-local-fingerprint")},
		});
		input.invocations.push_back(std::move(invocation));

		auto encoded = encode_gcc_compile_commands_bundle(*capture, input);
		require(static_cast<bool>(encoded));
		auto decoded = cxxlens::sdk::decode_capture_bundle(*encoded);
		require(static_cast<bool>(decoded));
		require(!has_gap(*decoded, "compile_units[0].response_files", "response-files-unobserved"));
		require(!has_gap(*decoded, "compile_units[0].config_files", "config-files-unobserved"));
		require(!has_gap(
			*decoded, "compile_units[0].environment_effects", "environment-effects-unobserved"));
		require(has_gap(*decoded,
						"compile_units[0].environment_effects[1].semantic_value",
						"secret-semantic-effect"));

		auto malformed = input;
		malformed.invocations.front().response_files.value->front().parent_index = 0U;
		auto rejected = encode_gcc_compile_commands_bundle(*capture, malformed);
		require(!rejected && rejected.error().detail == "recursive-reference");

		auto mismatch = input;
		mismatch.invocations.push_back({});
		auto mismatched = encode_gcc_compile_commands_bundle(*capture, mismatch);
		require(!mismatched && mismatched.error().detail == "compile-unit-count-mismatch");

		auto bounded = input;
		bounded.invocations.front().response_files.value->push_back(
			{"project://build/more.rsp",
			 cxxlens::sdk::detail::captured_value<std::string>::observed("sha256:" +
																		 std::string(64U, '8')),
			 1U,
			 std::nullopt});
		auto limits = cxxlens::sdk::import_limits{};
		limits.maximum_auxiliary_files_per_unit = 1U;
		auto over_limit = encode_gcc_compile_commands_bundle(*capture, bounded, limits);
		require(!over_limit && over_limit.error().detail == "auxiliary-file-count");

		auto forged_absent = input;
		forged_absent.invocations.front().response_files.state =
			cxxlens::sdk::detail::capture_field_state::unavailable;
		forged_absent.invocations.front().response_files.reason = "not-observed";
		forged_absent.invocations.front().response_files.completion_action = "recapture";
		auto forged_absent_result = encode_gcc_compile_commands_bundle(*capture, forged_absent);
		require(!forged_absent_result &&
				forged_absent_result.error().detail == "captured-value-shape");

		auto forged_nested = input;
		forged_nested.invocations.front()
			.response_files.value->front()
			.content_digest.completion_action = "must-be-empty-when-observed";
		auto forged_nested_result = encode_gcc_compile_commands_bundle(*capture, forged_nested);
		require(!forged_nested_result &&
				forged_nested_result.error().detail == "captured-value-shape");

		auto forged_toolchain = input;
		forged_toolchain.toolchain.binary_digest.state =
			cxxlens::sdk::detail::capture_observation_state::redacted;
		forged_toolchain.toolchain.binary_digest.reason = "secret";
		forged_toolchain.toolchain.binary_digest.completion_action = "supply-digest";
		auto forged_toolchain_result =
			encode_gcc_compile_commands_bundle(*capture, forged_toolchain);
		require(!forged_toolchain_result &&
				forged_toolchain_result.error().detail == "captured-value-shape");

		auto forged_closure = input;
		forged_closure.invocations.front().source_closure_members =
			cxxlens::sdk::detail::captured_value<
				std::vector<cxxlens::sdk::detail::gcc_source_closure_member_observation>>::
				observed({{"/work/include/a.hpp", bytes("#pragma once\n"), "utf8", "header"}});
		forged_closure.invocations.front().source_closure_members.state =
			cxxlens::sdk::detail::capture_field_state::unavailable;
		forged_closure.invocations.front().source_closure_members.reason = "not-observed";
		forged_closure.invocations.front().source_closure_members.completion_action = "recapture";
		auto forged_closure_result = encode_gcc_compile_commands_bundle(*capture, forged_closure);
		require(!forged_closure_result &&
				forged_closure_result.error().detail == "captured-value-shape");
	}
} // namespace

int main()
{
	arguments_form_preserves_variants_and_order();
	command_form_is_decoded_without_shell_expansion();
	malformed_and_shell_dependent_inputs_fail_closed();
	explicit_bounds_apply_before_adoption();
	canonical_bundle_projection_is_validated_and_deterministic();
	projection_rejects_mismatched_or_untrusted_observations();
	duplicate_source_variants_share_one_canonical_closure();
	observed_source_closure_is_complete_canonical_and_bounded();
	adapter_observations_replace_only_their_actionable_gaps();
}
