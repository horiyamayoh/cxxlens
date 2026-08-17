#include "llvm/clang22/source_closure_invocation.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	using cxxlens::detail::clang22::prepare_source_closure_invocation;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	void expect_code(const cxxlens::sdk::result<cxxlens::detail::clang22::source_closure_invocation>&
					 result,
				 const std::string_view code)
	{
		require(!result, "invalid invocation unexpectedly succeeded");
		require(result.error().code == code, "invalid invocation returned the wrong typed code");
	}
} // namespace

int main()
{
	const std::vector<std::string> roots{"/opt/clang-22", "/qualified/sysroot"};
	const std::vector<std::string> arguments{
		"/opt/clang-22/bin/clang++",
		"-std=c++23",
		"-Iproject://include",
		"-isystem",
		"project://third_party/include",
		"-include",
		"project://generated/config.hpp",
		"-imacrosproject://generated/macros.hpp",
		"-resource-dir=/opt/clang-22/lib/clang/22",
		"--sysroot=/qualified/sysroot",
		"-isystem",
		"/qualified/sysroot/usr/include",
		"project://src/main.cpp",
	};
	auto prepared = prepare_source_closure_invocation(
		arguments, "project://src/main.cpp", "project://src", roots);
	require(prepared.has_value(), "valid source-closure invocation was rejected");
	require(prepared->tool_name == "/opt/clang-22/bin/clang++",
			"compiler argv0 was not bound to a qualified toolchain root");
	require(prepared->compiler_filename == "/__cxxlens_project__/src/main.cpp",
			"main source was not mapped to the synthetic root");
	require(prepared->working_directory == "/__cxxlens_project__/src",
			"working directory was not mapped to the synthetic root");
	const std::vector<std::string> expected{
		"-std=c++23",
		"-I/__cxxlens_project__/include",
		"-isystem",
		"/__cxxlens_project__/third_party/include",
		"-include",
		"/__cxxlens_project__/generated/config.hpp",
		"-imacros/__cxxlens_project__/generated/macros.hpp",
		"-resource-dir=/opt/clang-22/lib/clang/22",
		"--sysroot=/qualified/sysroot",
		"-isystem",
		"/qualified/sysroot/usr/include",
	};
	require(prepared->compiler_arguments == expected,
			"known path-bearing compiler arguments were not rewritten canonically");

	auto reordered_roots = prepare_source_closure_invocation(
		arguments,
		"project://src/main.cpp",
		"project://src",
		std::vector<std::string>{"/qualified/sysroot", "/opt/clang-22"});
	require(reordered_roots.has_value() &&
			reordered_roots->qualified_read_roots == prepared->qualified_read_roots,
			"qualified roots were not canonicalized independently of input order");

	auto relative_include = arguments;
	relative_include[2] = "-Iinclude";
	expect_code(prepare_source_closure_invocation(
		relative_include, "project://src/main.cpp", "project://src", roots),
		"source-closure.ambient-fallback-denied");

	auto unqualified_absolute = arguments;
	unqualified_absolute[11] = "/usr/include";
	expect_code(prepare_source_closure_invocation(
		unqualified_absolute, "project://src/main.cpp", "project://src", roots),
		"source-closure.toolchain-input-unqualified");

	auto response_file = arguments;
	response_file.insert(response_file.end() - 1, "@ambient.rsp");
	expect_code(prepare_source_closure_invocation(
		response_file, "project://src/main.cpp", "project://src", roots),
		"source-closure.ambient-fallback-denied");

	auto overlay = arguments;
	overlay.insert(overlay.end() - 1, "-ivfsoverlay");
	overlay.insert(overlay.end() - 1, "/qualified/sysroot/overlay.yaml");
	expect_code(prepare_source_closure_invocation(
		overlay, "project://src/main.cpp", "project://src", roots),
		"source-closure.toolchain-input-unqualified");

	auto module = arguments;
	module.insert(module.end() - 1, "-fmodule-file=/qualified/sysroot/module.pcm");
	expect_code(prepare_source_closure_invocation(
		module, "project://src/main.cpp", "project://src", roots),
		"source-closure.toolchain-input-unqualified");

	auto hidden_project_path = arguments;
	hidden_project_path.insert(hidden_project_path.end() - 1,
		"-DPROJECT_PATH=project://secret.hpp");
	expect_code(prepare_source_closure_invocation(
		hidden_project_path, "project://src/main.cpp", "project://src", roots),
		"source-closure.path-invalid");

	auto wrong_main = arguments;
	wrong_main.back() = "project://src/other.cpp";
	expect_code(prepare_source_closure_invocation(
		wrong_main, "project://src/main.cpp", "project://src", roots),
		"source-closure.main-invalid");

	expect_code(prepare_source_closure_invocation(
		arguments,
		"project://src/main.cpp",
		"project://src",
		std::vector<std::string>{"/opt/clang-22", "/opt/clang-22"}),
		"source-closure.toolchain-input-unqualified");
	return 0;
}
