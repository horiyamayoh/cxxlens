#include "sdk/gcc_capture_file_port_internal.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
	using cxxlens::sdk::detail::capture_file_snapshot;
	using cxxlens::sdk::detail::gcc_capture_file_port;

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

	[[nodiscard]] cxxlens::sdk::detail::captured_text_observation observed(std::string value)
	{
		return {
			cxxlens::sdk::detail::capture_observation_state::observed, std::move(value), {}, {}};
	}

	[[nodiscard]] cxxlens::sdk::detail::gcc_compile_commands_capture_request
	request(std::string root = "/physical/project")
	{
		cxxlens::sdk::detail::gcc_compile_commands_capture_request output;
		output.project_id = "project:gcc-file-capture";
		output.project_root = std::move(root);
		output.compile_commands_path = output.project_root + "/build/compile_commands.json";
		output.toolchain.exact_version = "16.2.0";
		output.toolchain.canonical_binary_path = observed("/opt/gcc-16.2.0/bin/g++");
		output.toolchain.binary_digest = observed("sha256:" + std::string(64U, '1'));
		output.toolchain.target_triple = "x86_64-linux-gnu";
		output.toolchain.sysroot = observed("/opt/gcc-16.2.0/sysroot");
		output.toolchain.abi_digest = observed("sha256:" + std::string(64U, '2'));
		output.toolchain.builtin_headers_digest = observed("sha256:" + std::string(64U, '3'));
		output.toolchain.builtin_macros_digest = observed("sha256:" + std::string(64U, '4'));
		output.toolchain.include_search_digest = observed("sha256:" + std::string(64U, '5'));
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
				return cxxlens::sdk::unexpected({"test.capture-io", "file", "missing"});
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
		auto captured = cxxlens::sdk::detail::capture_gcc_compile_commands(files, input);
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
	}

	void fake_port_failures_and_canonical_escape_fail_closed()
	{
		fake_file_port missing;
		missing.directories.emplace("/physical/project", "/physical/project");
		auto missing_result =
			cxxlens::sdk::detail::capture_gcc_compile_commands(missing, request());
		require(!missing_result && missing_result.error().detail == "missing");

		fake_file_port escaped;
		escaped.directories.emplace("/physical/project", "/physical/project");
		escaped.directories.emplace("/physical/project/build", "/physical/project/build");
		constexpr std::string_view database = R"json([
  {"directory":"/physical/project/build","file":"../src/main.cpp",
   "arguments":["g++","-c","../src/main.cpp"]}
])json";
		escaped.files.emplace("/physical/project/build/compile_commands.json",
							  capture_file_snapshot{"/physical/project/build/compile_commands.json",
													bytes(database)});
		escaped.files.emplace("/physical/project/build/../src/main.cpp",
							  capture_file_snapshot{"/outside/main.cpp", bytes("int x;\n")});
		auto escaped_result =
			cxxlens::sdk::detail::capture_gcc_compile_commands(escaped, request());
		require(!escaped_result && escaped_result.error().detail == "path-outside-project-root");

		auto outside_working = escaped;
		outside_working.directories["/physical/project/build"] = "/outside";
		outside_working.file_limits.clear();
		auto outside_working_result =
			cxxlens::sdk::detail::capture_gcc_compile_commands(outside_working, request());
		require(!outside_working_result &&
				outside_working_result.error().detail == "path-outside-project-root" &&
				outside_working.file_limits.size() == 1U);

		auto limits = cxxlens::sdk::import_limits{};
		limits.maximum_bundle_bytes = 4U;
		auto limited =
			cxxlens::sdk::detail::capture_gcc_compile_commands(escaped, request(), limits);
		require(!limited && limited.error().detail == "byte-count");

		escaped.enforce_limits = false;
		auto independently_limited =
			cxxlens::sdk::detail::capture_gcc_compile_commands(escaped, request(), limits);
		require(!independently_limited && independently_limited.error().detail == "byte-count");
		limits = {};
		limits.maximum_source_closure_bytes = 4U;
		auto independently_source_limited =
			cxxlens::sdk::detail::capture_gcc_compile_commands(escaped, request(), limits);
		require(!independently_source_limited &&
				independently_source_limited.error().detail == "byte-count");

		auto oversized_path = escaped;
		oversized_path.files.at("/physical/project/build/compile_commands.json").canonical_path =
			"/" + std::string(cxxlens::sdk::import_limits{}.maximum_string_bytes, 'x');
		auto independently_path_limited =
			cxxlens::sdk::detail::capture_gcc_compile_commands(oversized_path, request());
		require(!independently_path_limited &&
				independently_path_limited.error().detail == "path-bytes");
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
		const auto database = std::string{"[{\"directory\":\""} + (tree.path() / "build").string() +
			R"json(","file":"../src/main.cpp","arguments":["g++","-std=c++23","-c","../src/main.cpp"]}])json";
		write(tree.path() / "build/compile_commands.json", database);
		auto input = request(tree.path().string());
		auto port = cxxlens::sdk::detail::make_system_gcc_capture_file_port();
		auto captured = cxxlens::sdk::detail::capture_gcc_compile_commands(*port, input);
		require(static_cast<bool>(captured));

		temporary_tree outside;
		write(outside.path() / "escape.cpp", "int escape;\n");
		std::filesystem::remove(tree.path() / "src/main.cpp");
		std::filesystem::create_symlink(outside.path() / "escape.cpp",
										tree.path() / "src/main.cpp");
		auto escaped = cxxlens::sdk::detail::capture_gcc_compile_commands(*port, input);
		require(!escaped && escaped.error().detail == "path-outside-project-root");

		auto non_regular_input = input;
		non_regular_input.compile_commands_path = (tree.path() / "build").string();
		auto non_regular =
			cxxlens::sdk::detail::capture_gcc_compile_commands(*port, non_regular_input);
		require(!non_regular && non_regular.error().detail == "not-regular-file");
#endif
	}
} // namespace

int main()
{
	fake_port_drives_canonical_projection_and_exact_bounds();
	fake_port_failures_and_canonical_escape_fail_closed();
	system_port_reads_one_stable_snapshot_and_rejects_escape();
}
