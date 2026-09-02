#include "sdk/gcc_capture_file_port_internal.hpp"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <ranges>
#include <source_location>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sdk/gcc_auxiliary_capture_internal.hpp"

namespace
{
	using cxxlens::sdk::detail::capture_file_snapshot;
	using cxxlens::sdk::detail::gcc_capture_file_port;
	using cxxlens::sdk::detail::gcc_probe_process_output;
	using cxxlens::sdk::detail::gcc_probe_process_port;
	using cxxlens::sdk::detail::gcc_probe_process_request;
	using cxxlens::sdk::detail::gcc_probe_process_terminal;

	void require(const bool condition,
				 const std::source_location location = std::source_location::current())
	{
		if (!condition)
		{
			std::cerr << "requirement failed at line " << location.line() << '\n';
			std::abort();
		}
	}

	[[nodiscard]] std::vector<std::byte> bytes(const std::string_view value)
	{
		return {reinterpret_cast<const std::byte*>(value.data()),
				reinterpret_cast<const std::byte*>(value.data() + value.size())};
	}

	void gcc_16_2_response_grammar_is_exact_and_bounded()
	{
		const std::string input{"'a\\$VAR' '\\\"' \"\\$VAR\" \"\\n\" ef\\\ngh ''\0ignored",
								sizeof("'a\\$VAR' '\\\"' \"\\$VAR\" \"\\n\" ef\\\ngh ''\0ignored") -
									1U};
		auto parsed = cxxlens::sdk::detail::parse_gcc_16_2_response_arguments(bytes(input));
		require(parsed &&
				*parsed == std::vector<std::string>{"a\\$VAR", "\\\"", "$VAR", "\\n", "efgh", ""});

		auto limits = cxxlens::sdk::import_limits{};
		limits.maximum_arguments_per_unit = 1U;
		auto count_limited =
			cxxlens::sdk::detail::parse_gcc_16_2_response_arguments(bytes("one two"), limits);
		require(!count_limited && count_limited.error().detail == "count");
		limits = {};
		limits.maximum_string_bytes = 2U;
		auto string_limited =
			cxxlens::sdk::detail::parse_gcc_16_2_response_arguments(bytes("abc"), limits);
		require(!string_limited && string_limited.error().detail == "string-bytes");

		auto whitespace = cxxlens::sdk::detail::parse_gcc_16_2_response_arguments(bytes(" \t\r\n"));
		require(whitespace && whitespace->empty());
	}

	void gcc_16_2_dependency_output_is_exact_and_bounded()
	{
		auto parsed = cxxlens::sdk::detail::parse_gcc_16_2_dependency_output(
			bytes("obj\\ file.o: ../src/main.cpp ../include/a\\ b.hpp \\\n"
				  " ../include/price$$.hpp\nphony: ignored.hpp\n"));
		require(parsed &&
				*parsed ==
					std::vector<std::string>{
						"../src/main.cpp", "../include/a b.hpp", "../include/price$.hpp"});

		auto malformed = cxxlens::sdk::detail::parse_gcc_16_2_dependency_output(bytes("no-rule\n"));
		require(!malformed && malformed.error().detail == "target-rule");
		constexpr char with_nul_text[]{"target: source.cpp\0hidden.hpp"};
		const std::string with_nul{with_nul_text, sizeof(with_nul_text) - 1U};
		auto nul = cxxlens::sdk::detail::parse_gcc_16_2_dependency_output(bytes(with_nul));
		require(!nul && nul.error().detail == "nul-byte");
		auto limits = cxxlens::sdk::import_limits{};
		limits.maximum_source_closure_members = 1U;
		auto count = cxxlens::sdk::detail::parse_gcc_16_2_dependency_output(
			bytes("target: first.hpp second.hpp\n"), limits);
		require(!count && count.error().detail == "count");
	}

	[[nodiscard]] gcc_probe_process_output probe_success(std::string standard_output = {},
														 std::string standard_error = {})
	{
		gcc_probe_process_output output;
		output.terminal = gcc_probe_process_terminal::exited;
		output.standard_output = std::move(standard_output);
		output.standard_error = std::move(standard_error);
		output.executable_path = "/opt/gcc-16.2.0/bin/g++";
		output.executable_digest = "sha256:" + std::string(64U, '1');
		output.executable_bytes = 123U;
		return output;
	}

	[[nodiscard]] std::vector<gcc_probe_process_output> valid_probe_outputs()
	{
		return {
			probe_success("16.2.0\n"),
			probe_success("x86_64-linux-gnu\n"),
			probe_success("/opt/gcc-16.2.0/sysroot\n"),
			probe_success("#define __GNUC__ 16\n"),
			probe_success({},
						  "#include <...> search starts here:\n"
						  " /opt/gcc-16.2.0/include/c++/16.2.0\n"
						  " /opt/gcc-16.2.0/sysroot/usr/include\n"
						  "End of search list.\n"),
		};
	}

	class fake_process_port final : public gcc_probe_process_port
	{
	  public:
		explicit fake_process_port(std::vector<gcc_probe_process_output> outputs)
			: outputs_{std::move(outputs)}
		{
		}

		cxxlens::sdk::result<gcc_probe_process_output>
		run(const gcc_probe_process_request& request, const std::stop_token& cancellation) override
		{
			requests.push_back(request);
			if (cancellation.stop_requested())
			{
				gcc_probe_process_output output;
				output.terminal = gcc_probe_process_terminal::cancelled;
				return output;
			}
			if (next_ >= outputs_.size())
				return cxxlens::sdk::unexpected({"test.capture-probe", "outputs", "exhausted"});
			return outputs_[next_++];
		}

		std::vector<gcc_probe_process_request> requests;

	  private:
		std::vector<gcc_probe_process_output> outputs_;
		std::size_t next_{};
	};

	[[nodiscard]] cxxlens::sdk::detail::gcc_compile_commands_capture_request
	request(std::string root = "/physical/project")
	{
		cxxlens::sdk::detail::gcc_compile_commands_capture_request output;
		output.project_id = "project:gcc-file-capture";
		output.project_root = std::move(root);
		output.compile_commands_path = output.project_root + "/build/compile_commands.json";
		output.compiler_path = "/opt/gcc-16.2.0/bin/g++";
		output.process_limits = {32U,
								 std::size_t{16U} * 1024U,
								 8U,
								 1024U,
								 std::size_t{128U} * 1024U,
								 std::uint64_t{128U} * 1024U * 1024U,
								 4096U};
		output.absolute_wall_deadline_ns = 123456789U;
		return output;
	}

	class fake_file_port final : public gcc_capture_file_port
	{
	  public:
		cxxlens::sdk::result<std::string>
		canonical_directory(const std::string_view path,
							const std::size_t maximum_path_bytes) override
		{
			directory_limits.push_back(maximum_path_bytes);
			const auto found = directories.find(std::string{path});
			if (found == directories.end())
				return cxxlens::sdk::unexpected({"test.capture-io", "directory", "missing"});
			return found->second;
		}

		cxxlens::sdk::result<capture_file_snapshot>
		read_regular_file(const std::string_view path,
						  const cxxlens::sdk::detail::capture_file_read_limits limits) override
		{
			file_limits.emplace_back(limits.maximum_file_bytes, limits.maximum_path_bytes);
			required_roots.emplace_back(limits.required_canonical_root);
			const auto found = files.find(std::string{path});
			if (found == files.end())
				return cxxlens::sdk::unexpected(
					{std::string{cxxlens::sdk::detail::capture_file_unavailable_code},
					 "file",
					 "missing"});
			if (enforce_limits && found->second.content.size() > limits.maximum_file_bytes)
				return cxxlens::sdk::unexpected(
					{"application-analysis.import-limit-exceeded", "capture.file", "byte-count"});
			return found->second;
		}

		std::map<std::string, std::string> directories;
		std::map<std::string, capture_file_snapshot> files;
		std::vector<std::size_t> directory_limits;
		std::vector<std::pair<std::uint64_t, std::size_t>> file_limits;
		std::vector<std::string> required_roots;
		bool enforce_limits{true};
	};

	[[nodiscard]] const cxxlens::sdk::detail::build_capture_environment_effect*
	find_effect(const std::vector<cxxlens::sdk::detail::build_capture_environment_effect>& effects,
				const std::string_view name)
	{
		const auto found = std::ranges::find(
			effects, name, &cxxlens::sdk::detail::build_capture_environment_effect::name);
		return found == effects.end() ? nullptr : &*found;
	}

	void gcc_environment_is_allowlisted_logical_and_deterministic()
	{
		fake_file_port files;
		for (const auto& path : {"/physical/project/build",
								 "/physical/project/include",
								 "/physical/project/shared",
								 "/physical/project/lib",
								 "/physical/project/tool:prefix",
								 "/opt/gcc/bin"})
			files.directories.emplace(path, path);
		std::vector<std::string> environment{
			"SECRET_TOKEN=do-not-persist",
			"SOURCE_DATE_EPOCH=00042",
			"LC_CTYPE=ignored-by-lc-all",
			"LC_ALL=C.UTF-8",
			"C_INCLUDE_PATH=/physical/project/c-only",
			"CPLUS_INCLUDE_PATH=../include:",
			"CPATH=/physical/project/shared",
			"COMPILER_PATH=/opt/gcc/bin",
			"GCC_EXEC_PREFIX=/physical/project/tool:prefix",
			"LIBRARY_PATH=/physical/project/lib",
		};
		const auto original_environment = environment;
		const cxxlens::sdk::detail::gcc_environment_capture_request request{
			environment,
			"/physical/project/build",
			"/physical/project",
			"c++",
			32U,
			4096U,
			4096U,
		};
		auto captured = cxxlens::sdk::detail::capture_gcc_16_2_environment_effects(files, request);
		require(captured && environment == original_environment && captured->size() == 7U &&
				std::ranges::is_sorted(
					*captured, {}, &cxxlens::sdk::detail::build_capture_environment_effect::name));

		const auto* compiler = find_effect(*captured, "gcc.compiler-path");
		const auto* cpath = find_effect(*captured, "gcc.cpath");
		const auto* cplus = find_effect(*captured, "gcc.cplus-include-path");
		const auto* prefix = find_effect(*captured, "gcc.exec-prefix");
		const auto* library = find_effect(*captured, "gcc.library-path");
		const auto* locale = find_effect(*captured, "gcc.locale-ctype");
		const auto* epoch = find_effect(*captured, "gcc.source-date-epoch");
		require(compiler &&
				compiler->semantic_value.state ==
					cxxlens::sdk::detail::capture_field_state::redacted &&
				!compiler->semantic_value.value &&
				compiler->semantic_value.reason == "machine-local-environment-path");
		require(prefix && prefix->semantic_value.value &&
				prefix->semantic_value.value->find("project://tool:prefix") != std::string::npos);
		for (const auto* effect : {cpath, cplus, library})
			require(effect && effect->semantic_value.value &&
					effect->semantic_value.value->starts_with("path-list-v1|") &&
					effect->semantic_value.value->find("project://") != std::string::npos &&
					effect->semantic_value.value->find("/physical/") == std::string::npos);
		require(cplus->semantic_value.value->find("project://include") != std::string::npos &&
				cplus->semantic_value.value->find("project://") != std::string::npos);
		require(locale && locale->semantic_value.value == "C.UTF-8" && epoch &&
				epoch->semantic_value.value == "42" &&
				find_effect(*captured, "SECRET_TOKEN") == nullptr &&
				find_effect(*captured, "gcc.c-include-path") == nullptr);

		std::ranges::reverse(environment);
		const cxxlens::sdk::detail::gcc_environment_capture_request reordered_request{
			environment, "/physical/project/build", "/physical/project", "c++", 32U, 4096U, 4096U};
		auto reordered =
			cxxlens::sdk::detail::capture_gcc_16_2_environment_effects(files, reordered_request);
		require(reordered && *reordered == *captured);
	}

	void gcc_environment_bounds_and_malformed_values_fail_closed()
	{
		fake_file_port files;
		files.directories.emplace("/physical/project/build", "/physical/project/build");
		files.directories.emplace("/physical/project/include", "/physical/project/include");
		std::vector<std::string> environment{"CPATH=../include"};
		cxxlens::sdk::detail::gcc_environment_capture_request request{
			environment, "/physical/project/build", "/physical/project", "c++", 8U, 1024U, 4096U};
		auto valid = cxxlens::sdk::detail::capture_gcc_16_2_environment_effects(files, request);
		require(valid && valid->size() == 1U);

		auto limits = cxxlens::sdk::import_limits{};
		limits.maximum_string_bytes = 8U;
		auto string_limited =
			cxxlens::sdk::detail::capture_gcc_16_2_environment_effects(files, request, limits);
		require(!string_limited && string_limited.error().detail == "string-bytes");
		request.maximum_environment_count = 0U;
		auto zero = cxxlens::sdk::detail::capture_gcc_16_2_environment_effects(files, request);
		require(!zero && zero.error().detail == "zero");
		request.maximum_environment_count = 8U;
		request.maximum_environment_bytes = 2U;
		auto byte_limited =
			cxxlens::sdk::detail::capture_gcc_16_2_environment_effects(files, request);
		require(!byte_limited && byte_limited.error().detail == "bytes");

		request.maximum_environment_bytes = 1024U;
		environment = {"CPATH=../include", "CPATH=../include"};
		request.environment = environment;
		auto duplicate = cxxlens::sdk::detail::capture_gcc_16_2_environment_effects(files, request);
		require(!duplicate && duplicate.error().detail == "duplicate-name");
		environment = {"MALFORMED"};
		request.environment = environment;
		auto malformed = cxxlens::sdk::detail::capture_gcc_16_2_environment_effects(files, request);
		require(!malformed && malformed.error().detail == "name-value");
		environment = {"SOURCE_DATE_EPOCH=not-a-number"};
		request.environment = environment;
		auto invalid_epoch =
			cxxlens::sdk::detail::capture_gcc_16_2_environment_effects(files, request);
		require(invalid_epoch && invalid_epoch->size() == 1U &&
				invalid_epoch->front().semantic_value.state ==
					cxxlens::sdk::detail::capture_field_state::unavailable);
		request.language = "fortran";
		auto unsupported =
			cxxlens::sdk::detail::capture_gcc_16_2_environment_effects(files, request);
		require(!unsupported && unsupported.error().detail == "canonical-context");
	}

	void gcc_invocation_planner_selects_one_source_and_dependency_output()
	{
		const std::vector<std::string> with_dependency{"/opt/gcc-16.2.0/bin/g++",
													   "-I",
													   "../include",
													   "-x",
													   "c++",
													   "-MMD",
													   "-MF",
													   "main.d",
													   "-c",
													   "../src/main.c"};
		auto existing = cxxlens::sdk::detail::plan_gcc_16_2_invocation(
			{with_dependency, "/opt/gcc-16.2.0/bin/g++", "/project/.capture/private.d"});
		require(existing && existing->source_path == "../src/main.c" &&
				existing->language == "c++" && existing->capture_arguments == with_dependency &&
				existing->dependency_output_path == "main.d" &&
				!existing->dependency_arguments_injected);

		const std::vector<std::string> inject{
			"/opt/gcc-16.2.0/bin/gcc", "-D", "VALUE=1", "-c", "src/main.c"};
		auto injected = cxxlens::sdk::detail::plan_gcc_16_2_invocation(
			{inject, "/opt/gcc-16.2.0/bin/gcc", "/project/.capture/private.d"});
		require(injected && injected->source_path == "src/main.c" && injected->language == "c" &&
				injected->dependency_arguments_injected &&
				injected->dependency_output_path == "/project/.capture/private.d" &&
				injected->capture_arguments ==
					std::vector<std::string>{"/opt/gcc-16.2.0/bin/gcc",
											 "-D",
											 "VALUE=1",
											 "-c",
											 "src/main.c",
											 "-MMD",
											 "-MF",
											 "/project/.capture/private.d"});

		const std::vector<std::string> attached{
			"/opt/gcc-16.2.0/bin/g++", "-MMD", "-MFmain.d", "-c", "src/main.cpp"};
		auto attached_plan = cxxlens::sdk::detail::plan_gcc_16_2_invocation(
			{attached, "/opt/gcc-16.2.0/bin/g++", {}});
		require(attached_plan && attached_plan->dependency_output_path == "main.d" &&
				attached_plan->language == "c++");
		const std::vector<std::string> path_without_mode{
			"/opt/gcc-16.2.0/bin/g++", "-MF", "main.d", "-c", "src/main.cpp"};
		auto completed = cxxlens::sdk::detail::plan_gcc_16_2_invocation(
			{path_without_mode, "/opt/gcc-16.2.0/bin/g++", {}});
		require(completed && completed->dependency_arguments_injected &&
				completed->capture_arguments.back() == "-MMD");

		const std::vector<std::string> explicit_custom_source{"/opt/gcc-16.2.0/bin/g++",
															  "-x",
															  "c++",
															  "-include",
															  "forced.cpp",
															  "-c",
															  "src/main.generated"};
		auto custom = cxxlens::sdk::detail::plan_gcc_16_2_invocation(
			{explicit_custom_source, "/opt/gcc-16.2.0/bin/g++", "/project/.capture/private.d"});
		require(custom && custom->source_path == "src/main.generated" && custom->language == "c++");
	}

	void gcc_invocation_planner_rejects_ambiguous_or_noncompile_input()
	{
		const auto plan = [](std::vector<std::string> arguments,
							 const std::string_view injected = "/project/.capture/private.d")
		{
			return cxxlens::sdk::detail::plan_gcc_16_2_invocation(
				{arguments, "/opt/gcc/bin/g++", injected});
		};
		auto multiple = plan({"/opt/gcc/bin/g++", "-c", "src/a.cpp", "src/b.cpp"});
		require(!multiple && multiple.error().detail == "multiple");
		auto no_compile = plan({"/opt/gcc/bin/g++", "src/a.cpp"});
		require(!no_compile && no_compile.error().detail == "compile-only-required");
		auto preprocessing = plan({"/opt/gcc/bin/g++", "-E", "-c", "src/a.cpp"});
		require(!preprocessing && preprocessing.error().detail == "object-compile-required");
		auto unknown_language =
			plan({"/opt/gcc/bin/g++", "-x", "objective-c++", "-c", "src/a.cpp"});
		require(!unknown_language && unknown_language.error().detail == "unsupported");
		auto duplicate_dependency =
			plan({"/opt/gcc/bin/g++", "-MF", "a.d", "-MFb.d", "-c", "src/a.cpp"});
		require(!duplicate_dependency && duplicate_dependency.error().detail == "duplicate");
		auto preprocessor_dependency =
			plan({"/opt/gcc/bin/g++", "-Wp,-MMD,a.d", "-c", "src/a.cpp"});
		require(!preprocessor_dependency &&
				preprocessor_dependency.error().detail == "preprocessor-option-unsupported");
		auto not_expanded = plan({"/opt/gcc/bin/g++", "@compile.rsp", "-c", "src/a.cpp"});
		require(!not_expanded && not_expanded.error().detail == "response-not-expanded");
		auto missing_injection = plan({"/opt/gcc/bin/g++", "-c", "src/a.cpp"}, {});
		require(!missing_injection &&
				missing_injection.error().detail == "injection-path-required");

		auto limits = cxxlens::sdk::import_limits{};
		limits.maximum_arguments_per_unit = 5U;
		const std::vector<std::string> bounded_arguments{"/opt/gcc/bin/g++", "-c", "src/a.cpp"};
		auto bounded = cxxlens::sdk::detail::plan_gcc_16_2_invocation(
			{bounded_arguments, "/opt/gcc/bin/g++", "/project/.capture/private.d"}, limits);
		require(!bounded && bounded.error().detail == "count");
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

	[[nodiscard]] cxxlens::sdk::result<std::vector<std::byte>> capture_with_valid_probe(
		gcc_capture_file_port& files,
		const cxxlens::sdk::detail::gcc_compile_commands_capture_request& input,
		const cxxlens::sdk::import_limits limits = {})
	{
		fake_process_port processes{valid_probe_outputs()};
		return cxxlens::sdk::detail::capture_gcc_compile_commands(files, processes, input, limits);
	}

	void fake_port_drives_canonical_projection_and_exact_bounds()
	{
		fake_file_port files;
		files.directories.emplace("/requested/project", "/physical/project");
		files.directories.emplace("/alias/build", "/physical/project/build");
		constexpr std::string_view database = R"json([
  {"directory":"/alias/build","file":"../src/main.cpp",
   "arguments":["/opt/gcc-16.2.0/bin/g++","-std=gnu++23","-c","../src/main.cpp"]}
])json";
		files.files.emplace("/requested/project/build/compile_commands.json",
							capture_file_snapshot{"/physical/project/build/compile_commands.json",
												  bytes(database)});
		files.files.emplace("/physical/project/build/../src/main.cpp",
							capture_file_snapshot{"/physical/project/src/main.cpp",
												  bytes("int main() { return 0; }\n")});
		auto input = request("/requested/project");
		fake_process_port processes{valid_probe_outputs()};
		auto captured = cxxlens::sdk::detail::capture_gcc_compile_commands(files, processes, input);
		require(static_cast<bool>(captured));
		auto decoded = cxxlens::sdk::decode_capture_bundle(*captured);
		require(decoded && decoded->logical_project_root() == "project://");
		require(decoded->compile_unit_count() == 1U);
		require(files.file_limits.size() == 2U);
		require(files.file_limits[0].first == cxxlens::sdk::import_limits{}.maximum_bundle_bytes);
		require(files.file_limits[1].first ==
				cxxlens::sdk::import_limits{}.maximum_source_closure_bytes);
		require(files.required_roots.size() == 2U && files.required_roots[0].empty() &&
				files.required_roots[1] == "/physical/project");
		require(files.directory_limits.size() == 2U);
		require(processes.requests.size() == 5U);
		for (const auto& probe : processes.requests)
			require(probe.argv.front() == input.compiler_path &&
					probe.working_directory == "/physical/project" &&
					probe.absolute_wall_deadline_ns == input.absolute_wall_deadline_ns);

		auto alias_input = input;
		alias_input.compiler_path = "/alias/g++";
		fake_process_port alias_processes{valid_probe_outputs()};
		auto alias_capture =
			cxxlens::sdk::detail::capture_gcc_compile_commands(files, alias_processes, alias_input);
		require(static_cast<bool>(alias_capture) && !alias_processes.requests.empty() &&
				alias_processes.requests.front().argv.front() == alias_input.compiler_path);
	}

	void response_and_spec_files_are_captured_recursively_without_claiming_closure()
	{
		fake_file_port files;
		files.directories.emplace("/physical/project", "/physical/project");
		files.directories.emplace("/physical/project/build", "/physical/project/build");
		constexpr std::string_view database = R"json([
  {"directory":"/physical/project/build","file":"../src/main.cpp",
	   "arguments":["/opt/gcc-16.2.0/bin/g++","@outer.rsp","-MMD","-MF","deps.d","-c","../src/main.cpp"]}
])json";
		files.files.emplace("/physical/project/build/compile_commands.json",
							capture_file_snapshot{"/physical/project/build/compile_commands.json",
												  bytes(database)});
		files.files.emplace("/physical/project/build/../src/main.cpp",
							capture_file_snapshot{"/physical/project/src/main.cpp",
												  bytes("int main() { return 0; }\n")});
		files.files.emplace("/physical/project/src/main.cpp",
							capture_file_snapshot{"/physical/project/src/main.cpp",
												  bytes("int main() { return 0; }\n")});
		files.files.emplace(
			"/physical/project/build/outer.rsp",
			capture_file_snapshot{"/physical/project/build/outer.rsp",
								  bytes("@nested.rsp --specs=custom.spec '-DNAME=a b'\n")});
		files.files.emplace(
			"/physical/project/build/nested.rsp",
			capture_file_snapshot{"/physical/project/build/nested.rsp", bytes("-I../include\n")});
		files.files.emplace(
			"/physical/project/build/custom.spec",
			capture_file_snapshot{"/physical/project/build/custom.spec", bytes("*link:\n")});
		files.files.emplace(
			"/physical/project/build/deps.d",
			capture_file_snapshot{
				"/physical/project/build/deps.d",
				bytes("main.o: ../src/main.cpp ../include/a.hpp /outside/external.hpp\n")});
		files.files.emplace(
			"/physical/project/include/a.hpp",
			capture_file_snapshot{"/physical/project/include/a.hpp", bytes("#pragma once\n")});
		const std::vector<std::string> original_arguments{"/opt/gcc-16.2.0/bin/g++",
														  "@outer.rsp",
														  "-MMD",
														  "-MF",
														  "deps.d",
														  "-c",
														  "../src/main.cpp"};
		auto prepared = cxxlens::sdk::detail::prepare_gcc_16_2_response_files(
			files,
			original_arguments,
			"/physical/project/build",
			"/physical/project",
			cxxlens::sdk::import_limits{}.maximum_source_closure_bytes);
		require(prepared && prepared->response_files.size() == 2U &&
				prepared->closure_members.size() == 2U && prepared->captured_bytes != 0U &&
				prepared->expanded_arguments ==
					std::vector<std::string>{"/opt/gcc-16.2.0/bin/g++",
											 "-I../include",
											 "--specs=custom.spec",
											 "-DNAME=a b",
											 "-MMD",
											 "-MF",
											 "deps.d",
											 "-c",
											 "../src/main.cpp"});

		auto captured = capture_with_valid_probe(files, request());
		auto repeated = capture_with_valid_probe(files, request());
		require(captured && repeated && *captured == *repeated);
		auto decoded = cxxlens::sdk::decode_capture_bundle(*captured);
		require(static_cast<bool>(decoded));
		require(!has_gap(*decoded, "compile_units[0].response_files", "response-files-unobserved"));
		require(!has_gap(*decoded, "compile_units[0].config_files", "config-files-unobserved"));
		require(has_gap(*decoded,
						"source_closures[0].membership_coverage",
						"dependency-output-not-bound-to-invocation"));
		auto imported = cxxlens::sdk::import_capture(*decoded);
		require(imported && imported->replay_plans().size() == 1U);
		require(std::ranges::none_of(imported->replay_plans().front().unresolved(),
									 [](const auto& gap)
									 {
										 return gap.reason == "response-file-expansion-unavailable";
									 }));

		auto value = cxxlens::sdk::canonical_binary_decode(*captured);
		require(static_cast<bool>(value));
		const auto& unit = value->tuple[5].tuple.front().tuple;
		require(unit[9].tuple[0].text == "observed" && unit[9].tuple[1].tuple.size() == 2U);
		require(unit[9].tuple[1].tuple[0].tuple[3].type ==
				cxxlens::sdk::canonical_value::kind::null_value);
		require(unit[9].tuple[1].tuple[1].tuple[3].integer == 0);
		require(unit[10].tuple[0].text == "observed" && unit[10].tuple[1].tuple.size() == 1U);
		const auto& closure = value->tuple[6].tuple.front().tuple;
		require(closure[3].integer == 5 &&
				closure[7].tuple[1].type == cxxlens::sdk::canonical_value::kind::null_value);
		require(std::ranges::any_of(closure[6].tuple,
									[](const auto& member)
									{
										return member.tuple[1].text == "project://include/a.hpp" &&
											member.tuple[5].tuple[1].text == "header" &&
											member.tuple[3].tuple[1].byte_string ==
											bytes("#pragma once\n");
									}));
		auto forged = *value;
		forged.tuple[5].tuple.front().tuple[9].tuple[1].tuple.front().tuple[1].tuple[1] =
			cxxlens::sdk::canonical_value::from_string("sha256:" + std::string(64U, '0'));
		auto forged_bytes = cxxlens::sdk::canonical_binary(forged);
		require(static_cast<bool>(forged_bytes));
		auto forged_result = cxxlens::sdk::decode_capture_bundle(*forged_bytes);
		require(!forged_result &&
				forged_result.error().detail == "source-closure-binding-mismatch");

		auto include_files = files;
		include_files.files["/physical/project/build/custom.spec"].content =
			bytes("%include <nested.spec>\n");
		auto include_capture = capture_with_valid_probe(include_files, request());
		require(static_cast<bool>(include_capture));
		auto include_decoded = cxxlens::sdk::decode_capture_bundle(*include_capture);
		require(include_decoded &&
				has_gap(*include_decoded,
						"compile_units[0].config_files",
						"spec-include-search-unobserved"));

		auto missing_dependency = files;
		missing_dependency.files.erase("/physical/project/build/deps.d");
		auto missing_dependency_capture = capture_with_valid_probe(missing_dependency, request());
		require(static_cast<bool>(missing_dependency_capture));
		auto missing_dependency_decoded =
			cxxlens::sdk::decode_capture_bundle(*missing_dependency_capture);
		require(missing_dependency_decoded &&
				has_gap(*missing_dependency_decoded,
						"source_closures[0].membership_coverage",
						"dependency-output-unreadable"));

		auto invalid_dependency = files;
		invalid_dependency.files["/physical/project/build/deps.d"].content =
			bytes("truncated dependency output\n");
		auto invalid_dependency_capture = capture_with_valid_probe(invalid_dependency, request());
		require(static_cast<bool>(invalid_dependency_capture));
		auto invalid_dependency_decoded =
			cxxlens::sdk::decode_capture_bundle(*invalid_dependency_capture);
		require(invalid_dependency_decoded &&
				has_gap(*invalid_dependency_decoded,
						"source_closures[0].membership_coverage",
						"dependency-output-invalid"));
	}

	void shell_free_invocation_owns_complete_dependency_membership()
	{
		fake_file_port files;
		files.directories.emplace("/physical/project", "/physical/project");
		files.directories.emplace("/physical/project/build", "/physical/project/build");
		files.files.emplace(
			"/physical/project/build/../src/main.cpp",
			capture_file_snapshot{"/physical/project/src/main.cpp",
								  bytes("#include \"a.hpp\"\nint main() { return value; }\n")});
		files.files.emplace(
			"/physical/project/src/main.cpp",
			capture_file_snapshot{"/physical/project/src/main.cpp",
								  bytes("#include \"a.hpp\"\nint main() { return value; }\n")});
		files.files.emplace(
			"/physical/project/build/deps.d",
			capture_file_snapshot{"/physical/project/build/deps.d",
								  bytes("main.o: ../src/main.cpp ../include/a.hpp\n")});
		files.files.emplace("/physical/project/include/a.hpp",
							capture_file_snapshot{"/physical/project/include/a.hpp",
												  bytes("inline constexpr int value = 0;\n")});

		cxxlens::sdk::detail::gcc_invocation_capture_request input;
		input.project_id = "project:gcc-wrapper";
		input.project_root = "/physical/project";
		input.working_directory = "/physical/project/build";
		input.source_path = "../src/main.cpp";
		input.compiler_path = "/opt/gcc-16.2.0/bin/g++";
		input.original_arguments = {input.compiler_path, "-I../include", "-c", input.source_path};
		input.capture_arguments = {
			input.compiler_path, "-I../include", "-MMD", "-MF", "deps.d", "-c", input.source_path};
		input.environment_effects = {{
			"gcc.cpath",
			cxxlens::sdk::detail::captured_value<std::string>::observed("sha256:" +
																		std::string(64U, '2')),
		}};
		input.process_limits = request().process_limits;
		input.absolute_wall_deadline_ns = request().absolute_wall_deadline_ns;
		fake_process_port processes{valid_probe_outputs()};
		auto captured = cxxlens::sdk::detail::capture_gcc_invocation(files, processes, input);
		fake_process_port repeated_processes{valid_probe_outputs()};
		auto repeated =
			cxxlens::sdk::detail::capture_gcc_invocation(files, repeated_processes, input);
		require(captured && repeated && *captured == *repeated);
		auto decoded = cxxlens::sdk::decode_capture_bundle(*captured);
		require(decoded && decoded->capture_adapter() == "shell-free-wrapper" &&
				decoded->compile_unit_count() == 1U);
		require(std::ranges::none_of(decoded->gaps(),
									 [](const auto& gap)
									 {
										 return gap.field ==
											 "source_closures[0].membership_coverage";
									 }));
		auto value = cxxlens::sdk::canonical_binary_decode(*captured);
		require(value && value->tuple[5].tuple.front().tuple[11].tuple[0].text == "observed" &&
				value->tuple[5].tuple.front().tuple[11].tuple[1].tuple.size() == 1U &&
				value->tuple[6].tuple.front().tuple[3].integer == 2);

		files.files.emplace("/physical/project/build/compile.rsp",
							capture_file_snapshot{"/physical/project/build/compile.rsp",
												  bytes("-I../include -c ../src/main.cpp\n")});
		auto response_input = input;
		response_input.original_arguments = {input.compiler_path, "@compile.rsp"};
		auto prepared = cxxlens::sdk::detail::prepare_gcc_16_2_response_files(
			files,
			response_input.original_arguments,
			input.working_directory,
			input.project_root,
			cxxlens::sdk::import_limits{}.maximum_source_closure_bytes);
		require(prepared && prepared->response_files.size() == 1U);
		response_input.capture_arguments = prepared->expanded_arguments;
		response_input.capture_arguments.insert(response_input.capture_arguments.end(),
												{"-MMD", "-MF", "deps.d"});
		prepared->expanded_arguments = response_input.capture_arguments;
		auto mismatched_prepared = *prepared;
		mismatched_prepared.expanded_arguments.emplace_back("-DFORGED=1");
		auto rejected_binding = cxxlens::sdk::detail::capture_gcc_auxiliary_files(
			files,
			response_input.capture_arguments,
			input.working_directory,
			input.project_root,
			"/physical/project/src/main.cpp",
			cxxlens::sdk::import_limits{}.maximum_source_closure_bytes,
			{},
			true,
			std::move(mismatched_prepared));
		require(!rejected_binding &&
				rejected_binding.error().detail == "execution-binding-mismatch");
		fake_process_port response_processes{valid_probe_outputs()};
		auto response_capture = cxxlens::sdk::detail::capture_gcc_invocation(
			files, response_processes, response_input, {}, {}, std::move(*prepared));
		require(static_cast<bool>(response_capture));
		auto response_value = cxxlens::sdk::canonical_binary_decode(*response_capture);
		require(response_value &&
				response_value->tuple[5].tuple.front().tuple[9].tuple[0].text == "observed" &&
				response_value->tuple[5].tuple.front().tuple[9].tuple[1].tuple.size() == 1U &&
				response_value->tuple[6].tuple.front().tuple[3].integer == 3);

		auto missing_files = files;
		missing_files.files.erase("/physical/project/build/deps.d");
		fake_process_port missing_processes{valid_probe_outputs()};
		auto missing =
			cxxlens::sdk::detail::capture_gcc_invocation(missing_files, missing_processes, input);
		require(static_cast<bool>(missing));
		auto missing_decoded = cxxlens::sdk::decode_capture_bundle(*missing);
		require(missing_decoded &&
				has_gap(*missing_decoded,
						"source_closures[0].membership_coverage",
						"dependency-output-unreadable"));

		auto missing_member_files = files;
		missing_member_files.files.erase("/physical/project/include/a.hpp");
		fake_process_port missing_member_processes{valid_probe_outputs()};
		auto missing_member = cxxlens::sdk::detail::capture_gcc_invocation(
			missing_member_files, missing_member_processes, input);
		require(static_cast<bool>(missing_member));
		auto missing_member_decoded = cxxlens::sdk::decode_capture_bundle(*missing_member);
		require(missing_member_decoded &&
				has_gap(*missing_member_decoded,
						"source_closures[0].membership_coverage",
						"dependency-member-unreadable"));

		auto mismatched = input;
		mismatched.capture_arguments.front() = "/different/g++";
		fake_process_port unused_processes{valid_probe_outputs()};
		auto mismatch =
			cxxlens::sdk::detail::capture_gcc_invocation(files, unused_processes, mismatched);
		require(!mismatch && mismatch.error().detail == "compiler-identity-mismatch" &&
				unused_processes.requests.empty());

		auto identity_mismatch = input;
		identity_mismatch.expected_compiler_digest = "sha256:" + std::string(64U, '0');
		fake_process_port identity_processes{valid_probe_outputs()};
		auto rejected_identity = cxxlens::sdk::detail::capture_gcc_invocation(
			files, identity_processes, identity_mismatch);
		require(!rejected_identity &&
				rejected_identity.error().detail == "executed-identity-mismatch");

		auto limits = cxxlens::sdk::import_limits{};
		limits.maximum_arguments_per_unit = 2U;
		fake_process_port limited_processes{valid_probe_outputs()};
		auto limited =
			cxxlens::sdk::detail::capture_gcc_invocation(files, limited_processes, input, limits);
		require(!limited && limited.error().detail == "argument-count");
	}

	void response_file_missing_recursion_and_bounds_fail_closed()
	{
		fake_file_port files;
		files.directories.emplace("/physical/project", "/physical/project");
		files.directories.emplace("/physical/project/build", "/physical/project/build");
		constexpr std::string_view database = R"json([
  {"directory":"/physical/project/build","file":"../src/main.cpp",
   "arguments":["/opt/gcc-16.2.0/bin/g++","@outer.rsp","-c","../src/main.cpp"]}
])json";
		files.files.emplace("/physical/project/build/compile_commands.json",
							capture_file_snapshot{"/physical/project/build/compile_commands.json",
												  bytes(database)});
		files.files.emplace("/physical/project/build/../src/main.cpp",
							capture_file_snapshot{"/physical/project/src/main.cpp",
												  bytes("int main() { return 0; }\n")});

		auto missing = capture_with_valid_probe(files, request());
		require(static_cast<bool>(missing));
		auto missing_decoded = cxxlens::sdk::decode_capture_bundle(*missing);
		require(missing_decoded &&
				has_gap(*missing_decoded,
						"compile_units[0].response_files[0].content_digest",
						"response-file-unreadable"));
		const std::vector<std::string> strict_arguments{
			"/opt/gcc-16.2.0/bin/g++", "@outer.rsp", "-c", "../src/main.cpp"};
		auto strict_missing = cxxlens::sdk::detail::prepare_gcc_16_2_response_files(
			files,
			strict_arguments,
			"/physical/project/build",
			"/physical/project",
			cxxlens::sdk::import_limits{}.maximum_source_closure_bytes);
		require(!strict_missing && strict_missing.error().detail == "unreadable-before-execution");

		auto recursive = files;
		recursive.files.emplace(
			"/physical/project/build/outer.rsp",
			capture_file_snapshot{"/physical/project/build/outer.rsp", bytes("@outer.rsp\n")});
		auto recursive_result = capture_with_valid_probe(recursive, request());
		require(!recursive_result && recursive_result.error().detail == "recursive-reference");

		auto nested = recursive;
		nested.files["/physical/project/build/outer.rsp"].content = bytes("@nested.rsp\n");
		nested.files.emplace(
			"/physical/project/build/nested.rsp",
			capture_file_snapshot{"/physical/project/build/nested.rsp", bytes("-DVALUE=1\n")});
		auto deep = nested;
		deep.files["/physical/project/build/outer.rsp"].content = bytes("@depth-0.rsp\n");
		for (std::size_t index{}; index < 32U; ++index)
		{
			const auto name = "/physical/project/build/depth-" + std::to_string(index) + ".rsp";
			const auto next = index + 1U < 32U ? "@depth-" + std::to_string(index + 1U) + ".rsp\n"
											   : "-DVALUE=1\n";
			deep.files.emplace(name, capture_file_snapshot{name, bytes(next)});
		}
		auto depth_limited = capture_with_valid_probe(deep, request());
		require(!depth_limited && depth_limited.error().detail == "depth");

		auto limits = cxxlens::sdk::import_limits{};
		limits.maximum_source_closure_bytes =
			files.files.at("/physical/project/build/../src/main.cpp").content.size();
		auto byte_limited = capture_with_valid_probe(nested, request(), limits);
		require(!byte_limited && byte_limited.error().detail == "byte-count");
	}

	void fake_port_failures_and_canonical_escape_fail_closed()
	{
		fake_file_port unqualified;
		unqualified.directories.emplace("/physical/project", "/physical/project");
		auto wrong_version_outputs = valid_probe_outputs();
		wrong_version_outputs[0].standard_output = "16.1.0\n";
		fake_process_port wrong_version{std::move(wrong_version_outputs)};
		auto unqualified_result = cxxlens::sdk::detail::capture_gcc_compile_commands(
			unqualified, wrong_version, request());
		require(!unqualified_result &&
				unqualified_result.error().code ==
					"application-analysis.gcc-toolchain-unavailable" &&
				unqualified.file_limits.empty());

		fake_file_port missing;
		missing.directories.emplace("/physical/project", "/physical/project");
		auto missing_result = capture_with_valid_probe(missing, request());
		require(!missing_result && missing_result.error().detail == "missing");

		fake_file_port escaped;
		escaped.directories.emplace("/physical/project", "/physical/project");
		escaped.directories.emplace("/physical/project/build", "/physical/project/build");
		constexpr std::string_view database = R"json([
  {"directory":"/physical/project/build","file":"../src/main.cpp",
	   "arguments":["/opt/gcc-16.2.0/bin/g++","-c","../src/main.cpp"]}
])json";
		escaped.files.emplace("/physical/project/build/compile_commands.json",
							  capture_file_snapshot{"/physical/project/build/compile_commands.json",
													bytes(database)});
		escaped.files.emplace("/physical/project/build/../src/main.cpp",
							  capture_file_snapshot{"/outside/main.cpp", bytes("int x;\n")});
		auto escaped_result = capture_with_valid_probe(escaped, request());
		require(!escaped_result && escaped_result.error().detail == "path-outside-project-root");

		auto outside_working = escaped;
		outside_working.directories["/physical/project/build"] = "/outside";
		outside_working.file_limits.clear();
		auto outside_working_result = capture_with_valid_probe(outside_working, request());
		require(!outside_working_result &&
				outside_working_result.error().detail == "path-outside-project-root" &&
				outside_working.file_limits.size() == 1U);

		auto limits = cxxlens::sdk::import_limits{};
		limits.maximum_bundle_bytes = 4U;
		auto limited = capture_with_valid_probe(escaped, request(), limits);
		require(!limited && limited.error().detail == "byte-count");

		escaped.enforce_limits = false;
		auto independently_limited = capture_with_valid_probe(escaped, request(), limits);
		require(!independently_limited && independently_limited.error().detail == "byte-count");
		limits = {};
		limits.maximum_source_closure_bytes = 4U;
		auto independently_source_limited = capture_with_valid_probe(escaped, request(), limits);
		require(!independently_source_limited &&
				independently_source_limited.error().detail == "byte-count");

		auto oversized_path = escaped;
		oversized_path.files.at("/physical/project/build/compile_commands.json").canonical_path =
			"/" + std::string(cxxlens::sdk::import_limits{}.maximum_string_bytes, 'x');
		auto independently_path_limited = capture_with_valid_probe(oversized_path, request());
		require(!independently_path_limited &&
				independently_path_limited.error().detail == "path-bytes");

		auto unbound = escaped;
		unbound.file_limits.clear();
		unbound.required_roots.clear();
		constexpr std::string_view unbound_database = R"json([
  {"directory":"/physical/project/build","file":"../src/main.cpp",
   "arguments":["g++","-c","../src/main.cpp"]}
])json";
		unbound.files.at("/physical/project/build/compile_commands.json").content =
			bytes(unbound_database);
		auto unbound_result = capture_with_valid_probe(unbound, request());
		require(!unbound_result &&
				unbound_result.error().field == "compile_commands[0].arguments[0]" &&
				unbound_result.error().detail == "absolute-compiler-path-required" &&
				unbound.file_limits.size() == 1U);

		auto mismatched = unbound;
		mismatched.file_limits.clear();
		constexpr std::string_view mismatched_database = R"json([
  {"directory":"/physical/project/build","file":"../src/main.cpp",
   "arguments":["/different/g++","-c","../src/main.cpp"]}
])json";
		mismatched.files.at("/physical/project/build/compile_commands.json").content =
			bytes(mismatched_database);
		auto mismatched_result = capture_with_valid_probe(mismatched, request());
		require(!mismatched_result &&
				mismatched_result.error().field == "compile_commands[0].arguments[0]" &&
				mismatched_result.error().detail == "compiler-identity-mismatch" &&
				mismatched.file_limits.size() == 1U);

		auto relative_request = request();
		relative_request.compiler_path = "g++";
		fake_file_port untouched;
		fake_process_port unused_processes{valid_probe_outputs()};
		auto relative_result = cxxlens::sdk::detail::capture_gcc_compile_commands(
			untouched, unused_processes, relative_request);
		require(!relative_result && relative_result.error().field == "compiler_path" &&
				untouched.directory_limits.empty() && unused_processes.requests.empty());
	}

	class temporary_tree
	{
	  public:
		temporary_tree()
		{
			std::string pattern{"/tmp/cxxlens-gcc-capture-files-XXXXXX"};
			pattern.push_back('\0');
			const auto created = ::mkdtemp(pattern.data());
			require(created != nullptr);
			path_ = created;
		}
		~temporary_tree()
		{
			std::error_code ignored;
			std::filesystem::remove_all(path_, ignored);
		}
		[[nodiscard]] const std::filesystem::path& path() const noexcept
		{
			return path_;
		}

	  private:
		std::filesystem::path path_;
	};

	void write(const std::filesystem::path& path, const std::string_view content)
	{
		std::ofstream output{path, std::ios::binary};
		require(static_cast<bool>(output));
		output.write(content.data(), static_cast<std::streamsize>(content.size()));
		require(static_cast<bool>(output));
	}

	void system_port_reads_one_stable_snapshot_and_rejects_escape()
	{
#if defined(__linux__)
		temporary_tree tree;
		std::filesystem::create_directories(tree.path() / "build");
		std::filesystem::create_directories(tree.path() / "src");
		write(tree.path() / "src/main.cpp", "int main() { return 0; }\n");
		auto input = request(tree.path().string());
		const auto database = std::string{R"([{"directory":")"} + (tree.path() / "build").string() +
			R"json(","file":"../src/main.cpp","arguments":[")json" + input.compiler_path +
			R"json(","-std=c++23","-c","../src/main.cpp"]}])json";
		write(tree.path() / "build/compile_commands.json", database);
		auto port = cxxlens::sdk::detail::make_system_gcc_capture_file_port();
		auto captured = capture_with_valid_probe(*port, input);
		require(static_cast<bool>(captured));

		temporary_tree outside;
		write(outside.path() / "escape.cpp", "int escape;\n");
		std::filesystem::remove(tree.path() / "src/main.cpp");
		std::filesystem::create_symlink(outside.path() / "escape.cpp",
										tree.path() / "src/main.cpp");
		auto escaped = capture_with_valid_probe(*port, input);
		require(!escaped && escaped.error().detail == "path-outside-project-root");

		auto non_regular_input = input;
		non_regular_input.compile_commands_path = (tree.path() / "build").string();
		auto non_regular = capture_with_valid_probe(*port, non_regular_input);
		require(!non_regular && non_regular.error().detail == "not-regular-file");
#endif
	}
} // namespace

int main() noexcept
{
	try
	{
		gcc_16_2_response_grammar_is_exact_and_bounded();
		gcc_16_2_dependency_output_is_exact_and_bounded();
		gcc_environment_is_allowlisted_logical_and_deterministic();
		gcc_environment_bounds_and_malformed_values_fail_closed();
		gcc_invocation_planner_selects_one_source_and_dependency_output();
		gcc_invocation_planner_rejects_ambiguous_or_noncompile_input();
		fake_port_drives_canonical_projection_and_exact_bounds();
		response_and_spec_files_are_captured_recursively_without_claiming_closure();
		shell_free_invocation_owns_complete_dependency_membership();
		response_file_missing_recursion_and_bounds_fail_closed();
		fake_port_failures_and_canonical_escape_fail_closed();
		system_port_reads_one_stable_snapshot_and_rejects_escape();
		return EXIT_SUCCESS;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
	}
	catch (...)
	{
		std::cerr << "unexpected test failure\n";
	}
	return EXIT_FAILURE;
}
