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

	[[nodiscard]] std::vector<std::string> effective_arguments()
	{
		return {
			"/usr/bin/clang++-22",
			"-std=c++23",
			"-nostdinc",
			"-nostdinc++",
			"-resource-dir=/usr/lib/llvm-22/lib/clang/22",
			"--gcc-toolchain=/usr",
			"-Iproject://include",
			"project://src/main.cpp",
		};
	}
} // namespace

int main()
{
	temporary_directory ambient;
	std::filesystem::create_directories(ambient.path() / "include/nested");
	{
		std::ofstream shadow{ambient.path() / "include/nested/value.hpp"};
		shadow << "#error ambient shadow must never be visible\n";
	}
	current_directory_guard cwd{ambient.path()};

	auto closure = make_source_closure_snapshot({
		file("project://src/main.cpp",
			 source_closure_role::main,
			 "#include \"answer.hpp\"\nint use_answer() { return answer(); }\n"),
		file("project://include/answer.hpp",
			 source_closure_role::header,
			 "#pragma once\n#include \"nested/value.hpp\"\n"
			 "inline int answer() { return nested_value; }\n"),
		file("project://include/nested/value.hpp",
			 source_closure_role::generated,
			 "#pragma once\ninline constexpr int nested_value = 42;\n"),
	});
	require(closure.has_value(), "valid native source closure was rejected");

	bool callback_ran{};
	source_closure_native_input input{
		*closure,
		"project://src/main.cpp",
		"project://src",
		effective_arguments(),
		{"/usr", "/lib", "/lib64"},
	};
	auto result = with_source_closure_translation_unit(input,
		[&callback_ran](cxxlens::provider::clang22::borrowed_translation_unit&)
			-> cxxlens::sdk::result<void>
		{
			callback_ran = true;
			return {};
		});
	if (!result && result.error().code == "native.unsupported-clang-major")
		return 77;
	if (!result)
		std::cerr << "native source-closure parse failed: " << result.error().code << " / "
				  << result.error().field << " / " << result.error().detail << '\n';
	require(result.has_value(), "native source-closure parse failed");
	require(callback_ran, "native source-closure callback was not executed");

	auto missing = make_source_closure_snapshot({
		file("project://src/main.cpp",
			 source_closure_role::main,
			 "#include \"answer.hpp\"\nint use_answer() { return answer(); }\n"),
		file("project://include/answer.hpp",
			 source_closure_role::header,
			 "#pragma once\n#include \"nested/value.hpp\"\n"
			 "inline int answer() { return nested_value; }\n"),
	});
	require(missing.has_value(), "missing-member fixture closure was rejected too early");
	input.closure = *missing;
	callback_ran = false;
	result = with_source_closure_translation_unit(input,
		[&callback_ran](cxxlens::provider::clang22::borrowed_translation_unit&)
			-> cxxlens::sdk::result<void>
		{
			callback_ran = true;
			return {};
		});
	require(!result, "missing closure member unexpectedly parsed");
	require(result.error().code == "source-closure.member-missing",
			"missing closure member returned the wrong typed failure");
	require(!callback_ran, "callback ran despite a missing required closure member");
	return 0;
}
