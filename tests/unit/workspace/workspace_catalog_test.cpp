#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <cxxlens/workspace.hpp>

namespace
{
	void write(const std::filesystem::path& path, const std::string& content)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream output{path, std::ios::binary};
		output << content;
	}

	std::string json_path(const std::filesystem::path& path)
	{
		auto value = path.generic_string();
		std::string output;
		for (const char character : value)
		{
			if (character == '\\' || character == '"')
				output.push_back('\\');
			output.push_back(character);
		}
		return output;
	}

	void require(bool condition, const char* message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	std::filesystem::path
	make_root(const std::filesystem::path& base, const std::string& name, bool reverse)
	{
		const auto root = base / name;
		write(root / "src/main.cpp", "int main(){return 0;}\n");
		write(root / "src/other.cpp", "int other(){return 1;}\n");
		write(root / ".cxxlens.yaml", "schema: cxxlens.config.v1\n");
		write(root / "build/flags.rsp",
			  "-DVALUE=1 -I ../include --target=x86_64-test -resource-dir=/clang/resource "
			  "-mabi=sysv");
		const auto directory = json_path(root / "build");
		const std::string first = "{\"directory\":\"" + directory +
			"\",\"file\":\"../src/"
			"main.cpp\",\"arguments\":[\"clang++\",\"@flags.rsp\",\"-std=c++20\",\"../src/"
			"main.cpp\"],"
			"\"command\":\"clang++ -fplugin=evil.so ../src/main.cpp\"}";
		const std::string second = "{\"directory\":\"" + directory +
			"\",\"file\":\"../src/main.cpp\",\"command\":\"clang++ @flags.rsp -std=c++23 "
			"../src/main.cpp ; touch should-not-exist\"}";
		write(root / "build/compile_commands.json",
			  "[" + (reverse ? second + "," + first : first + "," + second) + "]");
		return root;
	}
} // namespace

int main(int argc, char** argv)
{
	const auto base = std::filesystem::temp_directory_path() / "cxxlens-workspace-catalog-test";
	std::filesystem::remove_all(base);
	const auto root_a = make_root(base, "checkout-a", false);
	const auto root_b = make_root(base, "relocated-checkout-b", true);

	auto options_a = cxxlens::workspace_options::from_compilation_database(root_a / "build");
	options_a.project_root = root_a;
	options_a.configuration_file = root_a / ".cxxlens.yaml";
	auto options_b = cxxlens::workspace_options::from_compilation_database(root_b / "build");
	options_b.project_root = root_b;
	options_b.configuration_file = root_b / ".cxxlens.yaml";
	const auto opened_a = cxxlens::workspace::open(options_a);
	const auto opened_b = cxxlens::workspace::open(options_b);
	require(opened_a.has_value() && opened_b.has_value(), "workspace open failed");
	const auto units_a = opened_a.value().compile_units();
	const auto units_b = opened_b.value().compile_units();
	require(units_a.size() == 2U && units_b.size() == 2U, "variants were not preserved");
	for (std::size_t index = 0U; index < units_a.size(); ++index)
	{
		require(units_a[index].id() == units_b[index].id(),
				"compile-unit ID depends on root/order");
		require(units_a[index].variant_id() == units_b[index].variant_id(),
				"variant ID depends on root/order");
		require(units_a[index].command_digest() == units_b[index].command_digest(),
				"command digest differs");
		require(units_a[index].command().arguments.size() >= 8U, "response file was not expanded");
		require(units_a[index].target().triple == "x86_64-test", "target was not projected");
		require(units_a[index].target().resource_directory == "/clang/resource",
				"resource directory missing");
	}
	require(opened_a.value().command_for("src/main.cpp").size() == 2U, "ambiguity was hidden");
	require(opened_a.value().command_for("src/missing.cpp").empty(),
			"zero result was not preserved");
	require(!std::filesystem::exists(root_a / "build/should-not-exist"),
			"shell metacharacters executed");
	require(units_a.front().target().language_standard != units_a.back().target().language_standard,
			"distinct standards collapsed");

	auto inferred_options = options_a;
	inferred_options.commands = cxxlens::compile_command_policy::allow_header_inference;
	const auto inferred = cxxlens::workspace::open(inferred_options);
	require(inferred && inferred.value().command_for("src/main.hpp").size() == 2U,
			"header inference candidates missing");
	require(inferred.value().explain_build_context("src/main.hpp").find("stem-match") !=
				std::string::npos,
			"header inference evidence missing");

	write(root_a / "src/main.cpp", "int main(){return 2;}\n");
	write(root_a / ".cxxlens.yaml", "schema: cxxlens.config.v1\noutput: {deterministic: true}\n");
	{
		std::ofstream database{root_a / "build/compile_commands.json", std::ios::app};
		database << '\n';
	}
	require(opened_a.value().explain_build_context().find("\"compatible\":false") !=
				std::string::npos,
			"stale source was not detected");

	const auto scope_a = cxxlens::analysis_scope::files({"b.cpp", "a.cpp", "a.cpp"})
							 .include_headers()
							 .variants({units_a[1].variant_id(), units_a[0].variant_id()});
	const auto scope_b = cxxlens::analysis_scope::files({"a.cpp", "b.cpp"})
							 .include_headers()
							 .variants({units_a[0].variant_id(), units_a[1].variant_id()});
	require(scope_a.to_json() == scope_b.to_json(), "analysis scope order is unstable");
	if (argc == 2 && std::string_view{argv[1]} == "--emit")
	{
		std::cout << scope_a.to_json() << '\n' << opened_a.value().explain_build_context() << '\n';
	}

	const auto negative = base / "negative";
	write(negative / "bad.json", "not-json");
	require(!cxxlens::workspace::open(
				cxxlens::workspace_options::from_compilation_database(negative / "bad.json")),
			"malformed database accepted");
	require(!cxxlens::workspace::open(
				cxxlens::workspace_options::from_compilation_database(negative / "missing.json")),
			"missing database accepted");
	write(negative / "source.cpp", "int x;\n");
	const auto directory = json_path(negative);
	write(negative / "arguments.json",
		  "[{\"directory\":\"" + directory +
			  "\",\"file\":\"source.cpp\",\"arguments\":[\"clang++\",\"-DNAME=a "
			  "b\",\"-std=c++23\",\"source.cpp\"]}]");
	write(negative / "command.json",
		  "[{\"directory\":\"" + directory +
			  "\",\"file\":\"source.cpp\",\"command\":\"clang++ '-DNAME=a b' -std=c++23 "
			  "source.cpp\"}]");
	const auto arguments_form = cxxlens::workspace::open(
		cxxlens::workspace_options::from_compilation_database(negative / "arguments.json"));
	const auto command_form = cxxlens::workspace::open(
		cxxlens::workspace_options::from_compilation_database(negative / "command.json"));
	require(arguments_form && command_form &&
				arguments_form.value().compile_units().front().command_digest() ==
					command_form.value().compile_units().front().command_digest(),
			"arguments and equivalent command normalized differently");
	write(negative / "other.cpp", "int other;\n");
	write(negative / "shared-variant.json",
		  "[{\"directory\":\"" + directory +
			  "\",\"file\":\"source.cpp\",\"arguments\":[\"clang++\",\"-std=c++23\","
			  "\"-DMODE=1\",\"-c\",\"source.cpp\",\"-o\",\"source.o\"]},"
			  "{\"directory\":\"" +
			  directory +
			  "\",\"file\":\"other.cpp\",\"arguments\":[\"clang++\",\"-std=c++23\","
			  "\"-DMODE=1\",\"-c\",\"other.cpp\",\"-o\",\"other.o\"]}]");
	const auto shared_variant = cxxlens::workspace::open(
		cxxlens::workspace_options::from_compilation_database(negative / "shared-variant.json"));
	require(shared_variant && shared_variant.value().compile_units().size() == 2U &&
				shared_variant.value().compile_units()[0].variant_id() ==
					shared_variant.value().compile_units()[1].variant_id() &&
				shared_variant.value().compile_units()[0].id() !=
					shared_variant.value().compile_units()[1].id(),
			"source and output paths leaked into the build-variant identity");
	write(negative / "plugin.json",
		  "[{\"directory\":\"" + directory +
			  "\",\"file\":\"source.cpp\",\"arguments\":[\"clang++\",\"-Xclang\",\"-load\",\"evil."
			  "so\",\"source.cpp\"]}]");
	const auto plugin = cxxlens::workspace::open(
		cxxlens::workspace_options::from_compilation_database(negative / "plugin.json"));
	require(!plugin && plugin.error().code.value == "workspace.driver-not-allowed",
			"plugin flag accepted");

	write(negative / "duplicate.json",
		  "[{\"directory\":\"" + directory +
			  "\",\"file\":\"source.cpp\",\"arguments\":[\"clang++\",\"source.cpp\"]},"
			  "{\"directory\":\"" +
			  directory +
			  "\",\"file\":\"source.cpp\",\"arguments\":[\"clang++\",\"source.cpp\"]}]");
	const auto duplicate = cxxlens::workspace::open(
		cxxlens::workspace_options::from_compilation_database(negative / "duplicate.json"));
	require(!duplicate && duplicate.error().attributes.at("reason") == "duplicate-command",
			"duplicate command was not diagnosed");

	std::filesystem::remove_all(base);
	return 0;
}
