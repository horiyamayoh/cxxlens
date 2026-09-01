#include "sdk/compile_commands_capture_internal.hpp"

#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string>
#include <string_view>

namespace
{
	using cxxlens::sdk::detail::decode_compile_commands;

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
} // namespace

int main()
{
	arguments_form_preserves_variants_and_order();
	command_form_is_decoded_without_shell_expansion();
	malformed_and_shell_dependent_inputs_fail_closed();
	explicit_bounds_apply_before_adoption();
}
