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
	using cxxlens::detail::clang22::source_closure_snapshot;
	using cxxlens::detail::clang22::with_source_closure_translation_unit;

	// Only called from main()'s CXXLENS_TEST_CLANGXX22_PATH branch below; when no local Clang
	// toolchain is discovered (as in this repository's thread-sanitizer build, which does not
	// link the full Clang libraries), that branch compiles out and this becomes unused.
	[[maybe_unused]] void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	// Same rationale as require() above: only called from the guarded branch below.
	[[nodiscard, maybe_unused]] source_closure_file_input
	file(std::string path, source_closure_role role, std::string content)
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
				path_ = base /
					("cxxlens-source-closure-native-" +
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
	// links the real adapter against (see tests/CMakeLists.txt), rather than an assumed host
	// path. The admitted root is deliberately only that install prefix -- never `/usr`, never
	// `/etc`, nothing else ambient on the host.
	[[nodiscard]] std::vector<std::string> arguments_with(const std::vector<std::string>& extra)
	{
		std::vector<std::string> arguments{
			CXXLENS_TEST_CLANGXX22_PATH,
			"-std=c++23",
			"-nostdinc",
			"-nostdinc++",
#if defined(CXXLENS_TEST_CLANG22_RESOURCE_DIR)
			"-resource-dir=" CXXLENS_TEST_CLANG22_RESOURCE_DIR,
#endif
		};
		arguments.insert(arguments.end(), extra.begin(), extra.end());
		arguments.push_back("project://src/main.cpp");
		return arguments;
	}

	struct run_outcome
	{
		bool succeeded{};
		bool callback_ran{};
		std::string code;
		std::string detail;
	};

	[[nodiscard]] source_closure_native_input make_input(const source_closure_snapshot& closure,
														 const std::vector<std::string>& extra)
	{
		return {
			closure,
			"project://src/main.cpp",
			"project://src",
			arguments_with(extra),
			std::vector<std::string>{CXXLENS_TEST_CLANG22_ROOT},
		};
	}

	[[nodiscard]] run_outcome run(const source_closure_snapshot& closure,
								  const std::vector<std::string>& extra = {})
	{
		const auto input = make_input(closure, extra);
		run_outcome outcome;
		auto result = with_source_closure_translation_unit(
			input,
			[&outcome](cxxlens::provider::clang22::borrowed_translation_unit&)
				-> cxxlens::sdk::result<void>
			{
				outcome.callback_ran = true;
				return {};
			});
		outcome.succeeded = result.has_value();
		if (!result)
		{
			outcome.code = result.error().code;
			outcome.detail = result.error().detail;
		}
		return outcome;
	}

	[[nodiscard]] run_outcome run_withholding(const source_closure_snapshot& closure,
											  const std::string_view withheld,
											  const std::vector<std::string>& extra = {})
	{
		const auto input = make_input(closure, extra);
		run_outcome outcome;
		auto result =
			cxxlens::detail::clang22::with_source_closure_translation_unit_withholding_member(
				input,
				withheld,
				[&outcome](cxxlens::provider::clang22::borrowed_translation_unit&)
					-> cxxlens::sdk::result<void>
				{
					outcome.callback_ran = true;
					return {};
				});
		outcome.succeeded = result.has_value();
		if (!result)
		{
			outcome.code = result.error().code;
			outcome.detail = result.error().detail;
		}
		return outcome;
	}

	void expect_success(const run_outcome& outcome, const std::string_view what)
	{
		if (!outcome.succeeded)
			std::cerr << what << " unexpectedly failed: " << outcome.code << " / " << outcome.detail
					  << '\n';
		require(outcome.succeeded, what);
		require(outcome.callback_ran, "callback did not run for a successful translation unit");
	}

	void expect_failure(const run_outcome& outcome,
						const std::string_view expected_code,
						const std::string_view what)
	{
		if (outcome.succeeded)
			std::cerr << what << " unexpectedly succeeded\n";
		require(!outcome.succeeded, what);
		if (outcome.code != expected_code)
			std::cerr << what << " returned " << outcome.code << " (expected " << expected_code
					  << ") / " << outcome.detail << '\n';
		require(outcome.code == expected_code, what);
	}

	[[nodiscard]] std::string header(std::string body)
	{
		return "#pragma once\n" + std::move(body);
	}
#endif
} // namespace

int main()
{
#if !defined(CXXLENS_TEST_CLANGXX22_PATH)
	std::cerr << "no local clang++ toolchain discovered for the source-closure native test\n";
	return 77;
#else
	// ---------------------------------------------------------------------------------------
	// Group A -- ordinary project layouts must materialize.
	//
	// These are the layouts DF-0261 exists to enable. Each one makes Clang probe paths under the
	// synthetic project root that are *not* closure members (the includer-directory-first rule
	// for quoted includes, every `-I`/`-iquote` entry ahead of the one that hits, `-I`
	// directories holding no members, and `__has_include`). A miss on any such non-member path
	// must be an ordinary ENOENT, exactly like a miss outside the project root, because the
	// closure never claimed it. Before the member-aware audit these all failed with a spurious
	// `source-closure.member-missing` even though the closure was complete.
	// ---------------------------------------------------------------------------------------

	std::cerr << "[A1] include/ + src/ split with -I\n";
	{
		auto closure = make_source_closure_snapshot({
			file("project://src/main.cpp",
				 source_closure_role::main,
				 "#include \"answer.hpp\"\nint use_answer() { return answer(); }\n"),
			file("project://include/answer.hpp",
				 source_closure_role::header,
				 header("inline int answer() { return 42; }\n")),
		});
		require(closure.has_value(), "include/src split fixture was rejected");
		// Clang tries `/__cxxlens_project__/src/answer.hpp` first (includer directory) and only
		// then the `-I` entry that actually holds the member.
		expect_success(run(*closure, {"-Iproject://include"}),
					   "conventional include/ + src/ project with -I");
	}

	std::cerr << "[A2] include/ + src/ split with -iquote\n";
	{
		auto closure = make_source_closure_snapshot({
			file("project://src/main.cpp",
				 source_closure_role::main,
				 "#include \"answer.hpp\"\nint use_answer() { return answer(); }\n"),
			file("project://include/answer.hpp",
				 source_closure_role::header,
				 header("inline int answer() { return 42; }\n")),
		});
		require(closure.has_value(), "iquote fixture was rejected");
		expect_success(run(*closure, {"-iquote", "project://include"}),
					   "conventional include/ + src/ project with separate-token -iquote");
	}

	std::cerr << "[A3] angle include across two -I entries\n";
	{
		auto closure = make_source_closure_snapshot({
			file("project://src/main.cpp",
				 source_closure_role::main,
				 "#include <answer.hpp>\nint use_answer() { return answer(); }\n"),
			file("project://second/answer.hpp",
				 source_closure_role::header,
				 header("inline int answer() { return 42; }\n")),
		});
		require(closure.has_value(), "multi -I fixture was rejected");
		// `project://first` holds no members at all; it must be an ordinary search-order miss.
		expect_success(run(*closure, {"-Iproject://first", "-Iproject://second"}),
					   "angle include resolved from the second of two -I entries");
	}

	std::cerr << "[A4] -I naming a directory with no members\n";
	{
		auto closure = make_source_closure_snapshot({
			file("project://src/main.cpp",
				 source_closure_role::main,
				 "#include \"answer.hpp\"\nint use_answer() { return answer(); }\n"),
			file("project://src/answer.hpp",
				 source_closure_role::header,
				 header("inline int answer() { return 42; }\n")),
		});
		require(closure.has_value(), "empty -I fixture was rejected");
		expect_success(run(*closure, {"-Iproject://vendor/include"}),
					   "-I naming a member-less project directory");
	}

	std::cerr << "[A5] __has_include probing a non-member\n";
	{
		auto closure = make_source_closure_snapshot({
			file("project://src/main.cpp",
				 source_closure_role::main,
				 "#include \"answer.hpp\"\n"
				 "#if __has_include(\"local_overrides.hpp\")\n"
				 "#error the closure never claimed local_overrides.hpp\n"
				 "#endif\n"
				 "int use_answer() { return answer(); }\n"),
			file("project://src/answer.hpp",
				 source_closure_role::header,
				 header("inline int answer() { return 42; }\n")),
		});
		require(closure.has_value(), "__has_include fixture was rejected");
		// The `#error` also pins that the probe evaluates false rather than resolving to
		// something ambient.
		expect_success(run(*closure), "__has_include probe of a path the closure never claimed");
	}

	std::cerr << "[A6] driver toolchain probing outside the project root\n";
	{
		// No `--gcc-toolchain` is pinned, so the driver's own distro-dependent GCC-installation
		// candidate search and OS-release detection run completely unconstrained. `strace` on a
		// bare clang++-22 shows these landing on `/usr/lib/gcc/<triple>/<ver>`, `/etc/os-release`,
		// `/etc/lsb-release`, `/opt/rh` and `/etc/env.d/gcc` -- none of which are the closure or
		// the one admitted toolchain root.
		auto closure = make_source_closure_snapshot({
			file("project://src/main.cpp",
				 source_closure_role::main,
				 "#include \"answer.hpp\"\nint use_answer() { return answer(); }\n"),
			file("project://src/answer.hpp",
				 source_closure_role::header,
				 header("inline int answer() { return 42; }\n")),
		});
		require(closure.has_value(), "toolchain-probing fixture was rejected");
		expect_success(run(*closure), "unconstrained driver toolchain probing");
	}

	// ---------------------------------------------------------------------------------------
	// Group B -- an incomplete closure is Clang's own error, not a member-missing verdict.
	// ---------------------------------------------------------------------------------------

	std::cerr << "[B1] incomplete closure reports Clang's own diagnostic\n";
	{
		auto closure = make_source_closure_snapshot({
			file("project://src/main.cpp",
				 source_closure_role::main,
				 "#include \"answer.hpp\"\nint use_answer() { return answer(); }\n"),
			file("project://src/answer.hpp",
				 source_closure_role::header,
				 header("#include \"nested/value.hpp\"\n"
						"inline int answer() { return nested_value; }\n")),
		});
		require(closure.has_value(), "incomplete-closure fixture was rejected");
		// `nested/value.hpp` is not a member and was never claimed, so the audit must stay
		// silent; the task fails through Clang's ordinary file-not-found path instead. Asserting
		// the *specific* code here is what stops a future change from quietly reclassifying
		// unclaimed probes as member-missing again.
		expect_failure(run(*closure),
					   "native.parse-failed",
					   "unconditionally including a path no member claims");
	}

	// ---------------------------------------------------------------------------------------
	// Group C -- a member the manifest actually claims but that cannot be served is a hard,
	// unconditional failure. A validated closure plus a successful mount cannot reach this state
	// on its own, so the testing-only withholding seam manufactures it (see the header).
	// ---------------------------------------------------------------------------------------

	std::cerr << "[C1] claimed-but-unservable member, Clang also fails\n";
	{
		auto closure = make_source_closure_snapshot({
			file("project://src/main.cpp",
				 source_closure_role::main,
				 "#include \"answer.hpp\"\nint use_answer() { return answer(); }\n"),
			file("project://src/answer.hpp",
				 source_closure_role::header,
				 header("inline int answer() { return 42; }\n")),
		});
		require(closure.has_value(), "withholding fixture was rejected");
		auto outcome = run_withholding(*closure, "project://src/answer.hpp");
		expect_failure(outcome,
					   "source-closure.member-missing",
					   "an unservable claimed member on an ordinary include");
		require(outcome.detail == "project://src/answer.hpp",
				"member-missing failure did not name the claimed member");
	}

	std::cerr << "[C2] claimed-but-unservable member, Clang itself succeeds\n";
	{
		// This is the finding-2 regression: Clang tolerates the absence entirely (an
		// `__has_include` guard, no diagnostic, translation unit completes), so its own outcome
		// is a genuine independent variable. `callback_ran` below proves Clang reached
		// `HandleTranslationUnit` and succeeded on its own terms. The task must still fail,
		// which only holds because the audit is consulted unconditionally rather than inside a
		// `!outcome` branch.
		auto closure = make_source_closure_snapshot({
			file("project://src/main.cpp",
				 source_closure_role::main,
				 "#include \"answer.hpp\"\n"
				 "#if __has_include(\"generated.hpp\")\n"
				 "#include \"generated.hpp\"\n"
				 "#endif\n"
				 "int use_answer() { return answer(); }\n"),
			file("project://src/answer.hpp",
				 source_closure_role::header,
				 header("inline int answer() { return 42; }\n")),
			file("project://src/generated.hpp",
				 source_closure_role::generated,
				 header("inline constexpr int generated_value = 7;\n")),
		});
		require(closure.has_value(), "optional-probe withholding fixture was rejected");
		auto outcome = run_withholding(*closure, "project://src/generated.hpp");
		require(outcome.callback_ran,
				"Clang's own run must independently reach the callback for this to be a real "
				"asymmetry test");
		expect_failure(outcome,
					   "source-closure.member-missing",
					   "an unservable claimed member that Clang itself tolerates");
		require(outcome.detail == "project://src/generated.hpp",
				"member-missing failure did not name the claimed member");
	}

	// ---------------------------------------------------------------------------------------
	// Group D -- the ambient-read barrier. Nothing outside the closure and the admitted
	// toolchain root may ever be read, including files that genuinely exist on disk.
	// ---------------------------------------------------------------------------------------

	std::cerr << "[D1] absolute include of a real on-disk file outside admitted roots\n";
	{
		temporary_directory ambient;
		const auto ambient_header = ambient.path() / "ambient.hpp";
		{
			std::ofstream shadow{ambient_header};
			shadow << "#pragma once\ninline int answer() { return 1; }\n";
		}
		require(std::filesystem::exists(ambient_header),
				"ambient fixture header was not created on the real filesystem");

		// The file really exists and the path is spelled absolutely, so nothing but the routing
		// policy prevents it from being read.
		auto closure = make_source_closure_snapshot({
			file("project://src/main.cpp",
				 source_closure_role::main,
				 "#include \"" + ambient_header.string() +
					 "\"\n"
					 "int use_answer() { return answer(); }\n"),
		});
		require(closure.has_value(), "absolute-ambient fixture was rejected");
		expect_failure(run(*closure),
					   "native.parse-failed",
					   "absolute include of a real file outside the admitted roots");
	}

	std::cerr << "[D2] relative traversal out of the project root\n";
	{
		auto closure = make_source_closure_snapshot({
			file("project://src/main.cpp",
				 source_closure_role::main,
				 "#include \"../../../../etc/passwd\"\nint use_answer() { return 0; }\n"),
		});
		require(closure.has_value(), "traversal fixture was rejected");
		expect_failure(run(*closure),
					   "native.parse-failed",
					   "relative traversal escaping the synthetic project root");
	}

	std::cerr << "[D3] synthetic-root prefix boundary\n";
	{
		// `/__cxxlens_project__evil` shares a string prefix with the synthetic root but is not
		// beneath it; it must route to the denied region, not the closure region.
		auto closure = make_source_closure_snapshot({
			file("project://src/main.cpp",
				 source_closure_role::main,
				 "#include \"/__cxxlens_project__evil/answer.hpp\"\n"
				 "int use_answer() { return 0; }\n"),
		});
		require(closure.has_value(), "prefix-boundary fixture was rejected");
		expect_failure(run(*closure),
					   "native.parse-failed",
					   "path sharing a prefix with the synthetic root but outside it");
	}

	std::cerr << "[D4] ambient shadow reachable through the process working directory\n";
	{
		// The mounted filesystem overrides getCurrentWorkingDirectory() to the synthetic project
		// directory, so a CWD-relative lookup can never reach the real tree. Pin that: place a
		// shadow at the process CWD under both the bare spelling and the synthetic layout, and
		// confirm the closure's own authenticated bytes are what compile.
		temporary_directory ambient;
		std::filesystem::create_directories(ambient.path() / "src");
		for (const auto& shadow_path :
			 {ambient.path() / "answer.hpp", ambient.path() / "src/answer.hpp"})
		{
			std::ofstream shadow{shadow_path};
			shadow << "#pragma once\ninline int answer() { return 1; }\n";
		}
		current_directory_guard cwd{ambient.path()};

		auto closure = make_source_closure_snapshot({
			file("project://src/main.cpp",
				 source_closure_role::main,
				 "#include \"answer.hpp\"\n"
				 "static_assert(answer() == 42, \"ambient shadow bytes were compiled\");\n"
				 "int use_answer() { return answer(); }\n"),
			file("project://src/answer.hpp",
				 source_closure_role::header,
				 header("inline constexpr int answer() { return 42; }\n")),
		});
		require(closure.has_value(), "ambient-shadow fixture was rejected");
		// The static_assert is the real check: it fails to compile if the ambient `return 1;`
		// definition were ever the one that got included.
		expect_success(run(*closure), "ambient shadow at the process working directory");
	}

	return 0;
#endif
}
