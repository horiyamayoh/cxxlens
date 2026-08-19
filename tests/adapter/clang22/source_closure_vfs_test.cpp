#include "llvm/clang22/source_closure_vfs.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace
{
	using cxxlens::detail::clang22::make_source_closure_snapshot;
	using cxxlens::detail::clang22::source_closure_encoding;
	using cxxlens::detail::clang22::source_closure_file_input;
	using cxxlens::detail::clang22::source_closure_role;
	using cxxlens::detail::clang22::source_closure_vfs;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] source_closure_file_input
	file(std::string logical_path, source_closure_role role, std::string bytes)
	{
		return {
			std::move(logical_path),
			role,
			source_closure_encoding::utf8,
			std::make_shared<const std::string>(std::move(bytes)),
		};
	}

	[[nodiscard]] source_closure_vfs mounted_vfs()
	{
		auto closure = make_source_closure_snapshot({
			file("project://src/main.cpp",
				 source_closure_role::main,
				 "#include \"include/answer.hpp\"\nint main() { return answer(); }\n"),
			file("project://src/include/answer.hpp",
				 source_closure_role::header,
				 "#pragma once\ninline int answer() { return 42; }\n"),
			file("project://src/include/nested/inner.hpp",
				 source_closure_role::header,
				 "#include \"../../shared.hpp\"\n"),
			file("project://src/shared.hpp", source_closure_role::header, "closure-shared\n"),
			file("project://generated/config.hpp",
				 source_closure_role::generated,
				 "#define CXXLENS_CONFIG 1\n"),
		});
		require(closure.has_value(), "VFS fixture closure was rejected");
		auto mounted = source_closure_vfs::mount(std::move(*closure));
		require(mounted.has_value(), "valid source closure did not mount");
		return std::move(*mounted);
	}

	void expect_code(
		const cxxlens::sdk::result<cxxlens::detail::clang22::source_closure_vfs_file>& result,
		const std::string_view code)
	{
		require(!result, "VFS lookup unexpectedly succeeded");
		require(result.error().code == code, "VFS lookup failed with the wrong typed code");
	}
} // namespace

int main()
{
	auto vfs = mounted_vfs();
	auto direct = vfs.open("project://src/include/answer.hpp");
	require(direct.has_value() &&
				*direct->content == "#pragma once\ninline int answer() { return 42; }\n",
			"canonical project lookup returned the wrong immutable bytes");
	require(direct->synthetic_path == "/__cxxlens_project__/src/include/answer.hpp",
			"project lookup did not bind the synthetic compiler root");

	auto synthetic = vfs.open("/__cxxlens_project__/generated/config.hpp");
	require(synthetic.has_value() && synthetic->role == source_closure_role::generated,
			"synthetic-root lookup did not preserve generated role");

	auto quoted = vfs.open_relative("include/answer.hpp", "project://src/main.cpp");
	require(quoted.has_value() && quoted->logical_path == "project://src/include/answer.hpp",
			"quoted include did not resolve relative to the authenticated source");
	auto nested = vfs.open_relative("../../shared.hpp", "project://src/include/nested/inner.hpp");
	require(nested.has_value() && *nested->content == "closure-shared\n",
			"bounded parent traversal did not resolve inside the closure root");

	expect_code(vfs.open_relative("../../../etc/passwd", "project://src/main.cpp"),
				"source-closure.ambient-fallback-denied");
	expect_code(vfs.open_relative("include/missing.hpp", "project://src/main.cpp"),
				"source-closure.member-missing");
	expect_code(vfs.open("/tmp/ambient.hpp"), "source-closure.toolchain-input-unqualified");
	expect_code(vfs.open("/__cxxlens_project__/src/../ambient.hpp"), "source-closure.path-invalid");

	auto root_entries = vfs.list_directory(source_closure_vfs::synthetic_root());
	require(root_entries.has_value() && root_entries->size() == 2U,
			"synthetic root directory census was not canonical");
	auto include_entries = vfs.list_directory("project://src/include");
	require(include_entries.has_value() && include_entries->size() == 2U,
			"nested closure directory census was not canonical");
	auto denied_write = vfs.deny_write("/__cxxlens_project__/src/main.cpp");
	require(!denied_write && denied_write.error().code == "source-closure.ambient-fallback-denied",
			"write-like operation was not rejected");

	const auto original_directory = std::filesystem::current_path();
	const auto shadow_root = std::filesystem::temp_directory_path() /
		("cxxlens-source-closure-shadow-" + vfs.closure().closure_digest.substr(20U, 16U));
	std::error_code ignored;
	std::filesystem::remove_all(shadow_root, ignored);
	std::filesystem::create_directories(shadow_root / "src/include");
	{
		std::ofstream shadow(shadow_root / "src/include/answer.hpp", std::ios::binary);
		shadow << "ambient-shadow\n";
	}
	std::filesystem::current_path(shadow_root);
	auto shadow_resistant = vfs.open_relative("include/answer.hpp", "project://src/main.cpp");
	std::filesystem::current_path(original_directory);
	std::filesystem::remove_all(shadow_root, ignored);
	require(shadow_resistant.has_value() &&
				shadow_resistant->content->find("answer() { return 42; }") != std::string::npos,
			"ambient checkout bytes changed closure-backed lookup semantics");

	auto invalid_closure = vfs.closure();
	invalid_closure.snapshot_id = "source-closure:semantic-v2:sha256:invalid";
	require(!source_closure_vfs::mount(std::move(invalid_closure)),
			"invalid closure identity mounted successfully");
	return 0;
}
