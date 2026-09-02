#include "sdk/gcc_capture_file_port_internal.hpp"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <source_location>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
   "arguments":["g++","-c","../src/main.cpp"]}
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
		const auto database = std::string{R"([{"directory":")"} + (tree.path() / "build").string() +
			R"json(","file":"../src/main.cpp","arguments":["g++","-std=c++23","-c","../src/main.cpp"]}])json";
		write(tree.path() / "build/compile_commands.json", database);
		auto input = request(tree.path().string());
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
		fake_port_drives_canonical_projection_and_exact_bounds();
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
