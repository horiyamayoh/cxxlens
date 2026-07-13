#include <algorithm>
#include <array>
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

	std::filesystem::path make_same_basename_root(const std::filesystem::path& base,
												  const std::string& name,
												  const bool reverse)
	{
		const auto root = base / name;
		write(root / "src/a/common.cpp", "int common_a(){return 1;}\n");
		write(root / "src/b/common.cpp", "int common_b(){return 2;}\n");
		const auto directory = json_path(root / "build");
		const std::string first = "{\"directory\":\"" + directory +
			"\",\"file\":\"../src/a/common.cpp\",\"arguments\":[\"clang++\",\"-std=c++23\","
			"\"-c\",\"../src/a/common.cpp\"]}";
		const std::string second = "{\"directory\":\"" + directory +
			"\",\"file\":\"../src/b/common.cpp\",\"arguments\":[\"clang++\",\"-std=c++23\","
			"\"-c\",\"../src/b/common.cpp\"]}";
		write(root / "build/compile_commands.json",
			  "[" + (reverse ? second + "," + first : first + "," + second) + "]");
		return root;
	}

	void require_failure(const cxxlens::workspace_options& options,
						 const std::string_view code,
						 const std::string_view reason)
	{
		const auto opened = cxxlens::workspace::open(options);
		require(!opened && opened.error().code.value == code &&
					opened.error().attributes.contains("offset") &&
					opened.error().attributes.at("reason") == reason,
				"compile database failure reason/offset was not stable");
	}

	void check_json_conformance(const std::filesystem::path& base)
	{
		const auto root = base / "json-conformance";
		const std::string unicode_name = "unicode-\xCE\xA9-\xF0\x9F\x98\x80.cpp";
		const std::string unicode_define = "-DNAME=\xCE\xA9-\xF0\x9F\x98\x80";
		write(root / unicode_name, "int unicode_value;\n");
		const auto directory = json_path(root);
		write(root / "unicode.json",
			  "[{\"directory\":\"" + directory +
				  "\",\"file\":\"unicode-\\u03A9-\\uD83D\\uDE00.cpp\","
				  "\"arguments\":[\"clang++\",\"-DNAME=\\u03A9-\\uD83D\\uDE00\","
				  "\"unicode-\\u03A9-\\uD83D\\uDE00.cpp\"],"
				  "\"vendor_number\":-12.5e+2,\"vendor_bool\":true,\"vendor_null\":null,"
				  "\"vendor_nested\":{\"values\":[false,null,0,1.25E-3]}}]");
		const auto unicode = cxxlens::workspace::open(
			cxxlens::workspace_options::from_compilation_database(root / "unicode.json"));
		require(
			unicode && unicode.value().compile_units().size() == 1U &&
				unicode.value().compile_units().front().command().file.filename() == unicode_name &&
				unicode.value().compile_units().front().command().arguments.at(1) == unicode_define,
			"escaped BMP/surrogate Unicode or unknown scalar fields were not decoded");

		const auto large_root = base / "large-json";
		write(large_root / "source.cpp", "int large_database;\n");
		std::string large_database = "[{\"directory\":\"" + json_path(large_root) +
			"\",\"file\":\"source.cpp\",\"arguments\":[\"clang++\",\"source.cpp\"],"
			"\"vendor_padding\":\"";
		large_database.append(std::size_t{17U} * 1024U * 1024U, 'x');
		large_database += "\"}]";
		write(large_root / "compile_commands.json", large_database);
		auto large_options = cxxlens::workspace_options::from_compilation_database(
			large_root / "compile_commands.json");
		const auto large = cxxlens::workspace::open(large_options);
		require(large && large.value().compile_units().size() == 1U &&
					large_database.size() > std::size_t{16U} * 1024U * 1024U,
				"valid compilation database larger than the former 16 MiB cap was rejected");

		auto byte_limited_options = large_options;
		byte_limited_options.compilation_database_byte_budget = std::size_t{16U} * 1024U * 1024U;
		const auto byte_limited = cxxlens::workspace::open(byte_limited_options);
		require(!byte_limited && byte_limited.error().code.value == "core.budget-exhausted" &&
					byte_limited.error().attributes.at("reason") ==
						"compilation-database-byte-budget" &&
					byte_limited.error().attributes.contains("limit") &&
					byte_limited.error().attributes.contains("observed"),
				"pre-read compilation database byte budget was not structured");
		auto invalid_budget_options = large_options;
		invalid_budget_options.compilation_database_byte_budget = 0U;
		const auto invalid_budget = cxxlens::workspace::open(invalid_budget_options);
		require(!invalid_budget && invalid_budget.error().code.value == "core.invalid-argument" &&
					invalid_budget.error().attributes.at("reason") ==
						"compilation-database-byte-budget-zero",
				"zero compilation database budget was accepted as unlimited");
		auto string_limited_options = large_options;
		string_limited_options.compilation_database_string_byte_budget = 1024U;
		const auto string_limited = cxxlens::workspace::open(string_limited_options);
		require(!string_limited && string_limited.error().code.value == "core.budget-exhausted" &&
					string_limited.error().attributes.at("reason") ==
						"compilation-database-string-byte-budget" &&
					string_limited.error().attributes.contains("offset"),
				"decoded JSON string budget was not enforced");

		auto entry_limited_options =
			cxxlens::workspace_options::from_compilation_database(root / "unicode.json");
		entry_limited_options.compilation_database_entry_budget = 3U;
		const auto entry_limited = cxxlens::workspace::open(entry_limited_options);
		require(!entry_limited && entry_limited.error().code.value == "core.budget-exhausted" &&
					entry_limited.error().attributes.at("reason") ==
						"compilation-database-entry-budget",
				"JSON container entry budget was not enforced");

		std::string depth_bomb(66U, '[');
		depth_bomb += "null";
		depth_bomb.append(66U, ']');
		write(root / "depth.json", depth_bomb);
		auto depth_options =
			cxxlens::workspace_options::from_compilation_database(root / "depth.json");
		depth_options.compilation_database_nesting_budget = 8U;
		const auto depth_limited = cxxlens::workspace::open(depth_options);
		require(!depth_limited && depth_limited.error().code.value == "core.budget-exhausted" &&
					depth_limited.error().attributes.at("reason") ==
						"compilation-database-nesting-budget",
				"JSON nesting budget did not reject a depth bomb");

		const auto invalid = base / "invalid-json";
		write(invalid / "malformed-unicode.json", R"(["\u12G4"])");
		require_failure(cxxlens::workspace_options::from_compilation_database(
							invalid / "malformed-unicode.json"),
						"workspace.compile-database-invalid",
						"malformed-unicode-escape");
		write(invalid / "high-surrogate.json", R"(["\uD800x"])");
		require_failure(
			cxxlens::workspace_options::from_compilation_database(invalid / "high-surrogate.json"),
			"workspace.compile-database-invalid",
			"unpaired-high-surrogate");
		write(invalid / "low-surrogate.json", R"(["\uDC00"])");
		require_failure(
			cxxlens::workspace_options::from_compilation_database(invalid / "low-surrogate.json"),
			"workspace.compile-database-invalid",
			"unpaired-low-surrogate");
		write(invalid / "number.json", R"([{"vendor":01}])");
		require_failure(
			cxxlens::workspace_options::from_compilation_database(invalid / "number.json"),
			"workspace.compile-database-invalid",
			"invalid-number");
		std::string invalid_utf8{"[\""};
		invalid_utf8.push_back(static_cast<char>(0xC0U));
		invalid_utf8.push_back(static_cast<char>(0xAFU));
		invalid_utf8 += "\"]";
		write(invalid / "utf8.json", invalid_utf8);
		require_failure(
			cxxlens::workspace_options::from_compilation_database(invalid / "utf8.json"),
			"workspace.compile-database-invalid",
			"invalid-utf8");
	}
} // namespace

int main(int argc, char** argv)
{
	const auto base = std::filesystem::temp_directory_path() / "cxxlens-workspace-catalog-test";
	std::filesystem::remove_all(base);
	check_json_conformance(base);
	const auto root_a = make_root(base, "checkout-a", false);
	const auto root_b = make_root(base, "relocated-checkout-b", true);
	const auto collision_a = make_same_basename_root(base, "collision-checkout-a", false);
	const auto collision_b = make_same_basename_root(base, "collision-relocated-b", true);
	const auto collision_opened_a = cxxlens::workspace::open(
		cxxlens::workspace_options::from_compilation_database(collision_a / "build"));
	const auto collision_opened_b = cxxlens::workspace::open(
		cxxlens::workspace_options::from_compilation_database(collision_b / "build"));
	require(collision_opened_a && collision_opened_b, "default out-of-tree workspace open failed");
	const auto collision_units_a = collision_opened_a.value().compile_units();
	const auto collision_units_b = collision_opened_b.value().compile_units();
	require(collision_opened_a.value().root() == collision_a && collision_units_a.size() == 2U,
			"default project root was not inferred from build and sources");
	require(collision_units_a[0].main_file() != collision_units_a[1].main_file() &&
				collision_units_a[0].id() != collision_units_a[1].id(),
			"default root collapsed same-basename sources");
	require(collision_units_a[0].variant_id() == collision_units_a[1].variant_id(),
			"source hierarchy leaked into variant identity");
	for (std::size_t index = 0U; index < collision_units_a.size(); ++index)
		require(collision_units_a[index].main_file() == collision_units_b[index].main_file() &&
					collision_units_a[index].id() == collision_units_b[index].id(),
				"default root identity changed after relocation");
	const auto collision_evidence = collision_opened_a.value().explain_build_context();
	require(collision_evidence.find("\"project_root_origin\":\"inferred-common-ancestor\"") !=
					std::string::npos &&
				collision_evidence.find("\"policy\":\"project-relative-v2\"") !=
					std::string::npos &&
				collision_evidence.find(collision_a.generic_string()) != std::string::npos,
			"inferred root evidence missing");

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
	const std::array scope_family{
		cxxlens::analysis_scope::all(),
		cxxlens::analysis_scope::files({"src/main.cpp"}),
		cxxlens::analysis_scope::compile_units({units_a.front().id()}),
		cxxlens::analysis_scope::changed_files({"src/main.cpp"}),
		cxxlens::analysis_scope::all().include_headers(false),
		cxxlens::analysis_scope::all().variants({units_a.front().variant_id()}),
	};
	for (const auto& scope : scope_family)
		require(!scope.to_json().empty(), "analysis scope family member produced no contract");
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

	const auto external_project = base / "external-policy/project";
	const auto external_source = base / "external-policy/generated/common.cpp";
	write(external_source, "int generated(){return 0;}\n");
	write(external_project / "build/compile_commands.json",
		  "[{\"directory\":\"" + json_path(external_project / "build") + "\",\"file\":\"" +
			  json_path(external_source) + "\",\"arguments\":[\"clang++\",\"-c\",\"" +
			  json_path(external_source) + "\"]}]");
	auto external_options =
		cxxlens::workspace_options::from_compilation_database(external_project / "build");
	external_options.project_root = external_project;
	external_options.generated = cxxlens::generated_code_policy::include;
	const auto external = cxxlens::workspace::open(external_options);
	require(!external && external.error().code.value == "workspace.compile-database-invalid" &&
				external.error().attributes.at("reason") == "source-outside-project-root",
			"external generated source was accepted");

	std::filesystem::remove_all(base);
	return 0;
}
