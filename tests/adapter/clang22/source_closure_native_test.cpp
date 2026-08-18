#include "llvm/clang22/source_closure_native.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	using cxxlens::detail::clang22::make_source_closure_snapshot;
	using cxxlens::detail::clang22::source_closure_encoding;
	using cxxlens::detail::clang22::source_closure_file_input;
	using cxxlens::detail::clang22::source_closure_native_input;
	using cxxlens::detail::clang22::source_closure_role;
	using cxxlens::detail::clang22::with_source_closure_translation_unit;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] source_closure_file_input file(std::string path,
											 source_closure_role role,
											 std::string content)
	{
		return {
			std::move(path),
			role,
			source_closure_encoding::utf8,
			std::make_shared<const std::string>(std::move(content)),
		};
	}

	class temporary_directory final
	{
	  public:
		temporary_directory()
		{
			auto base = std::filesystem::temp_directory_path();
			for (unsigned attempt = 0U; attempt < 100U; ++attempt)
			{
				path_ = base / ("cxxlens-source-closure-native-" +
					std::to_string(static_cast<unsigned long long>(std::rand())) + "-" +
					std::to_string(attempt));
				std::error_code error;
				if (std::filesystem::create_directory(path_, error))
					return;
			}
			throw std::runtime_error{"unable to create temporary directory"};
		}

		~temporary_directory()
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

	class current_directory_guard final
	{
	  public:
		explicit current_directory_guard(const std::filesystem::path& replacement)
			: original_{std::filesystem::current_path()}
		{
			std::filesystem::current_path(replacement);
		}

		~current_directory_guard()
		{
			std::error_code ignored;
			std::filesystem::current_path(original_, ignored);
		}

	  private:
		std::filesystem::path original_;
	};

#if defined(CXXLENS_TEST_CLANGXX22_PATH)
	// The compiler binary, resource directory, and admitted toolchain root are all resolved by
	// CMake from the exact LLVM/Clang 22 package `find_package(LLVM 22.1 CONFIG)` locates and
	// links the real adapter against (see tests/CMakeLists.txt). This is deliberately the same
	// "explicit toolchain-root allowlist" concept `source_closure_invocation.cpp` already
	// establishes via `qualified_read_roots` -- there is exactly one source of truth for what
	// counts as "the admitted toolchain" here, not a second ad hoc one for this test.
	[[nodiscard]] std::vector<std::string> baseline_arguments()
	{
		return {
			CXXLENS_TEST_CLANGXX22_PATH,
			"-std=c++23",
			"-nostdinc",
			"-nostdinc++",
#if defined(CXXLENS_TEST_CLANG22_RESOURCE_DIR)
			"-resource-dir=" CXXLENS_TEST_CLANG22_RESOURCE_DIR,
#endif
			"project://src/main.cpp",
		};
	}
#endif
} // namespace

int main()
{
#if !defined(CXXLENS_TEST_CLANGXX22_PATH)
	std::cerr << "no local clang++ toolchain discovered for the source-closure native test\n";
	return 77;
#else
	// The only qualified (admitted) toolchain root for every scenario below is the exact
	// LLVM/Clang 22 install the adapter itself is linked against -- deliberately never `/usr`,
	// never `/etc`, nothing else ambient on the host. Every scenario therefore also doubles as
	// continuous coverage for issue #261 finding 1 (see the dedicated ambient-probing scenario
	// below for the sharpest demonstration): if the VFS still hard-failed on Clang's own
	// speculative toolchain probing outside the closure and this narrow root, every scenario in
	// this file would fail, not just the one that names it.
	const std::vector<std::string> qualified_toolchain_root{CXXLENS_TEST_CLANG22_ROOT};

	std::cerr << "[scenario 1] ordinary success\n";
	// Scenario 1: ordinary success. A real multi-file project closure (main file including a
	// header that itself includes a generated header) parses end to end and the callback
	// observes a fully resolved translation unit.
	{
		auto closure = make_source_closure_snapshot({
			file("project://src/main.cpp",
				 source_closure_role::main,
				 "#include \"answer.hpp\"\nint use_answer() { return answer(); }\n"),
			file("project://src/answer.hpp",
				 source_closure_role::header,
				 "#pragma once\n#include \"nested/value.hpp\"\n"
				 "inline int answer() { return nested_value; }\n"),
			file("project://src/nested/value.hpp",
				 source_closure_role::generated,
				 "#pragma once\ninline constexpr int nested_value = 42;\n"),
		});
		require(closure.has_value(), "valid native source closure was rejected");

		bool callback_ran{};
		source_closure_native_input input{
			*closure,
			"project://src/main.cpp",
			"project://src",
			baseline_arguments(),
			qualified_toolchain_root,
		};
		auto result = with_source_closure_translation_unit(input,
			[&callback_ran](cxxlens::provider::clang22::borrowed_translation_unit&)
				-> cxxlens::sdk::result<void>
			{
				callback_ran = true;
				return {};
			});
		if (!result)
			std::cerr << "native source-closure parse failed: " << result.error().code << " / "
					  << result.error().field << " / " << result.error().detail << '\n';
		require(result.has_value(), "native source-closure parse failed");
		require(callback_ran, "native source-closure callback was not executed");
	}

	std::cerr << "[scenario 2] fatal missing #include\n";
	// Scenario 2 (baseline negative case): an ordinary, unconditional `#include` of an absent
	// header is a fatal preprocessor error to Clang itself. This was the only negative case the
	// previously shipped suite had, and it asserted the callback never ran on the theory that a
	// fatal preprocessor error always prevents `HandleTranslationUnit` from being reached. That
	// turns out not to hold in general (Clang 22 was observed to still reach the AST consumer
	// here, presumably continuing with whatever partially-parsed AST it had). That is exactly
	// why gating `missing_failure()` on `!outcome`/Clang's own pass-fail signal is unsound in
	// general -- see finding 2 -- and why this suite does not assert anything about
	// `callback_ran` for this scenario. What must hold, unconditionally, is that the closure-
	// completeness check itself still reports the typed failure regardless of whatever Clang's
	// own outcome or callback-execution behavior happened to be.
	{
		auto missing = make_source_closure_snapshot({
			file("project://src/main.cpp",
				 source_closure_role::main,
				 "#include \"answer.hpp\"\nint use_answer() { return answer(); }\n"),
			file("project://src/answer.hpp",
				 source_closure_role::header,
				 "#pragma once\n#include \"nested/value.hpp\"\n"
				 "inline int answer() { return nested_value; }\n"),
		});
		require(missing.has_value(), "missing-member fixture closure was rejected too early");

		source_closure_native_input input{
			*missing,
			"project://src/main.cpp",
			"project://src",
			baseline_arguments(),
			qualified_toolchain_root,
		};
		auto result = with_source_closure_translation_unit(input,
			[](cxxlens::provider::clang22::borrowed_translation_unit&) -> cxxlens::sdk::result<void>
			{
				return {};
			});
		require(!result, "missing closure member unexpectedly parsed");
		require(result.error().code == "source-closure.member-missing",
				"missing closure member returned the wrong typed failure");
	}

	std::cerr << "[scenario 3] __has_include optional-probe asymmetry\n";
	// Scenario 3 (issue #261 finding 2 -- asymmetric fail-closed enforcement): the closure omits
	// a header that main.cpp only probes through `__has_include`, a construct Clang tolerates
	// without ever diagnosing an error. Clang's own outcome is a genuine independent variable
	// here: it succeeds on its own terms (`callback_ran` proves the callback executed, i.e. the
	// translation unit reached `HandleTranslationUnit`). The closure is nonetheless incomplete
	// relative to what the translation unit actually looked for, and `missing_failure()` must be
	// checked unconditionally -- not only inside a `!outcome` branch -- for this to still fail
	// the task. Before the fix, this scenario would have parsed "successfully".
	{
		auto optional_probe = make_source_closure_snapshot({
			file("project://src/main.cpp",
				 source_closure_role::main,
				 "#include \"answer.hpp\"\n"
				 "#if __has_include(\"optional_generated.hpp\")\n"
				 "#include \"optional_generated.hpp\"\n"
				 "#endif\n"
				 "int use_answer() { return answer(); }\n"),
			file("project://src/answer.hpp",
				 source_closure_role::header,
				 "#pragma once\ninline int answer() { return 42; }\n"),
		});
		require(optional_probe.has_value(),
				"optional-probe fixture closure was rejected too early");

		bool callback_ran{};
		source_closure_native_input input{
			*optional_probe,
			"project://src/main.cpp",
			"project://src",
			baseline_arguments(),
			qualified_toolchain_root,
		};
		auto result = with_source_closure_translation_unit(input,
			[&callback_ran](cxxlens::provider::clang22::borrowed_translation_unit&)
				-> cxxlens::sdk::result<void>
			{
				callback_ran = true;
				return {};
			});
		require(callback_ran,
				"Clang's own run must independently reach the callback for this to be a real "
				"asymmetry test, not a repeat of scenario 2");
		require(!result,
				"a closure-incomplete __has_include probe unexpectedly parsed as successful");
		require(result.error().code == "source-closure.member-missing",
				"optional-probe closure incompleteness returned the wrong typed failure");
	}

	std::cerr << "[scenario 4] ambient toolchain probing must not abort the task\n";
	// Scenario 4 (issue #261 finding 1 -- fail-closed VFS vs. Clang's own speculative toolchain
	// probing): no `--gcc-toolchain` is pinned, so Clang's driver is completely free to run its
	// own distro/toolchain-dependent GCC-installation-candidate search and OS-release detection
	// (confirmed by `strace`-ing a bare `clang++` invocation to land on paths such as
	// `/usr/lib/gcc/<triple>/<ver>`, `/etc/os-release`, `/etc/lsb-release`, `/opt/rh`, and
	// `/etc/env.d/gcc` -- see the disposition entry for the exact trace). None of those paths
	// are under the closure or under the one admitted toolchain root, so every one of them must
	// resolve as a plain "not found" here, exactly like an ordinary absent path, rather than
	// aborting the whole task.
	{
		auto closure = make_source_closure_snapshot({
			file("project://src/main.cpp",
				 source_closure_role::main,
				 "#include \"answer.hpp\"\nint use_answer() { return answer(); }\n"),
			file("project://src/answer.hpp",
				 source_closure_role::header,
				 "#pragma once\ninline int answer() { return 42; }\n"),
		});
		require(closure.has_value(), "ambient-probing fixture closure was rejected too early");

		bool callback_ran{};
		source_closure_native_input input{
			*closure,
			"project://src/main.cpp",
			"project://src",
			baseline_arguments(),
			qualified_toolchain_root,
		};
		auto result = with_source_closure_translation_unit(input,
			[&callback_ran](cxxlens::provider::clang22::borrowed_translation_unit&)
				-> cxxlens::sdk::result<void>
			{
				callback_ran = true;
				return {};
			});
		if (!result)
			std::cerr << "ambient-probing scenario unexpectedly failed: " << result.error().code
					  << " / " << result.error().field << " / " << result.error().detail << '\n';
		require(result.has_value(),
				"Clang's own speculative toolchain probing outside the closure and the admitted "
				"toolchain root incorrectly aborted the task");
		require(callback_ran, "ambient-probing scenario callback was not executed");
	}

	std::cerr << "[scenario 5] ambient shadow file must never be visible\n";
	// Ambient-shadow regression (kept from the original suite): even with a permissive current
	// working directory containing a shadow copy of a closure member, the closure's own
	// authenticated bytes are what get compiled -- never the ambient file.
	{
		temporary_directory ambient;
		std::filesystem::create_directories(ambient.path() / "src");
		{
			std::ofstream shadow{ambient.path() / "src/answer.hpp"};
			shadow << "#error ambient shadow must never be visible\n";
		}
		current_directory_guard cwd{ambient.path()};

		auto closure = make_source_closure_snapshot({
			file("project://src/main.cpp",
				 source_closure_role::main,
				 "#include \"answer.hpp\"\nint use_answer() { return answer(); }\n"),
			file("project://src/answer.hpp",
				 source_closure_role::header,
				 "#pragma once\ninline int answer() { return 42; }\n"),
		});
		require(closure.has_value(), "ambient-shadow fixture closure was rejected too early");

		bool callback_ran{};
		source_closure_native_input input{
			*closure,
			"project://src/main.cpp",
			"project://src",
			baseline_arguments(),
			qualified_toolchain_root,
		};
		auto result = with_source_closure_translation_unit(input,
			[&callback_ran](cxxlens::provider::clang22::borrowed_translation_unit&)
				-> cxxlens::sdk::result<void>
			{
				callback_ran = true;
				return {};
			});
		if (!result)
			std::cerr << "ambient-shadow scenario unexpectedly failed: " << result.error().code
					  << " / " << result.error().field << " / " << result.error().detail << '\n';
		require(result.has_value(), "ambient shadow scenario unexpectedly failed");
		require(callback_ran, "ambient shadow scenario callback was not executed");
	}

	return 0;
#endif
}
