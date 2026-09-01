#include "sdk/gcc_toolchain_probe_internal.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace
{
	using cxxlens::sdk::canonical_identity_digest;
	using cxxlens::sdk::canonical_value;
	using cxxlens::sdk::error;
	using cxxlens::sdk::result;
	using cxxlens::sdk::unexpected;
	using cxxlens::sdk::detail::capture_observation_state;
	using cxxlens::sdk::detail::gcc_probe_process_output;
	using cxxlens::sdk::detail::gcc_probe_process_port;
	using cxxlens::sdk::detail::gcc_probe_process_request;
	using cxxlens::sdk::detail::gcc_probe_process_terminal;
	using cxxlens::sdk::detail::gcc_toolchain_probe_request;
	using cxxlens::sdk::detail::probe_gcc_toolchain;

	void require(const bool condition, const std::string& message)
	{
		if (!condition)
			throw std::runtime_error{message};
	}

	[[nodiscard]] gcc_probe_process_output
	successful(std::string path, std::string standard_output = {}, std::string standard_error = {})
	{
		gcc_probe_process_output output;
		output.terminal = gcc_probe_process_terminal::exited;
		output.standard_output = std::move(standard_output);
		output.standard_error = std::move(standard_error);
		output.executable_path = std::move(path);
		output.executable_digest = "sha256:" + std::string(64U, '1');
		output.executable_bytes = 123U;
		return output;
	}

	[[nodiscard]] std::vector<gcc_probe_process_output> valid_outputs(const std::string& prefix,
																	  const std::string& sysroot)
	{
		const auto compiler = prefix + "/bin/g++";
		return {
			successful(compiler, "16.2.0\n"),
			successful(compiler, "x86_64-linux-gnu\n"),
			successful(compiler, sysroot + "\n"),
			successful(compiler, "#define __GNUC__ 16\n#define __SIZEOF_POINTER__ 8\n"),
			successful(compiler,
					   {},
					   "Using built-in specs.\n"
					   "#include \"...\" search starts here:\n"
					   "#include <...> search starts here:\n " +
						   prefix + "/include/c++/16.2.0\n " + sysroot +
						   "/usr/include\n /external/include\nEnd of search list.\n"),
		};
	}

	class fake_process_port final : public gcc_probe_process_port
	{
	  public:
		explicit fake_process_port(std::vector<gcc_probe_process_output> outputs)
			: outputs_{std::move(outputs)}
		{
		}

		result<gcc_probe_process_output> run(const gcc_probe_process_request& request,
											 const std::stop_token& cancellation) override
		{
			requests.push_back(request);
			if (cancellation.stop_requested())
			{
				gcc_probe_process_output output;
				output.terminal = gcc_probe_process_terminal::cancelled;
				return output;
			}
			if (next_ >= outputs_.size())
				return unexpected(error{"test.fake-probe-exhausted", "outputs", {}});
			return outputs_[next_++];
		}

		std::vector<gcc_probe_process_request> requests;

	  private:
		std::vector<gcc_probe_process_output> outputs_;
		std::size_t next_{};
	};

	[[nodiscard]] gcc_toolchain_probe_request request(const std::string& compiler)
	{
		return {compiler,
				"/work",
				{32U,
				 std::size_t{16U} * 1024U,
				 8U,
				 1024U,
				 std::size_t{128U} * 1024U,
				 std::uint64_t{128U} * 1024U * 1024U,
				 4096U},
				123456789U};
	}

	[[nodiscard]] std::string digest(const std::string_view domain,
									 std::vector<canonical_value> fields)
	{
		auto value = canonical_identity_digest(domain, fields);
		require(static_cast<bool>(value), "test digest construction failed");
		return std::move(*value);
	}
} // namespace

int main()
{
	try
	{
		const std::string prefix{"/toolchain-a"};
		const std::string sysroot{"/machine-a/sysroot"};
		fake_process_port processes{valid_outputs(prefix, sysroot)};
		auto observed = probe_gcc_toolchain(processes, request(prefix + "/bin/g++"));
		require(observed && observed->exact_version == "16.2.0" &&
					observed->target_triple == "x86_64-linux-gnu",
				"pinned GCC observation failed");
		require(observed->canonical_binary_path.value == prefix + "/bin/g++" &&
					observed->binary_digest.value == "sha256:" + std::string(64U, '1') &&
					observed->sysroot.value == sysroot,
				"measured compiler identity or sysroot was not retained");
		const std::string macros{"#define __GNUC__ 16\n#define __SIZEOF_POINTER__ 8\n"};
		const auto expected_macros =
			digest("gcc-builtin-macros-v1", {canonical_value::from_string(macros)});
		const std::string normalized_search =
			"$compiler-prefix/include/c++/16.2.0\n$sysroot/usr/include\n"
			"$host-root/external/include\n";
		const auto expected_search =
			digest("gcc-include-search-v1", {canonical_value::from_string(normalized_search)});
		const auto expected_abi = digest("gcc-abi-observation-v1",
										 {canonical_value::from_string("16.2.0"),
										  canonical_value::from_string("x86_64-linux-gnu"),
										  canonical_value::from_string(expected_macros)});
		require(observed->builtin_macros_digest.value == expected_macros &&
					observed->include_search_digest.value == expected_search &&
					observed->abi_digest.value == expected_abi,
				"normalized semantic observations were not deterministically digested");
		require(observed->builtin_headers_digest.state == capture_observation_state::unavailable &&
					observed->builtin_headers_digest.reason ==
						"builtin-header-content-unobserved" &&
					observed->builtin_headers_digest.completion_action ==
						"capture-builtin-header-source-closure",
				"uncaptured builtin header content was inferred as complete");
		require(processes.requests.size() == 5U, "probe command census changed");
		for (const auto& process_request : processes.requests)
			require(process_request.argv.front() == prefix + "/bin/g++" &&
						process_request.working_directory == "/work" &&
						process_request.environment == std::vector<std::string>{"LC_ALL=C"} &&
						process_request.absolute_wall_deadline_ns == 123456789U,
					"probe did not preserve explicit binary/environment/deadline authority");
		require(processes.requests[0].argv ==
						std::vector<std::string>{
							prefix + "/bin/g++", "-dumpfullversion", "-dumpversion"} &&
					processes.requests[3].argv ==
						std::vector<std::string>{
							prefix + "/bin/g++", "-dM", "-E", "-x", "c++", "-std=gnu++23", "-"},
				"probe argv was not shell-free and exact");

		fake_process_port relocated{valid_outputs("/relocated/gcc", "/other/sysroot")};
		auto relocated_observation =
			probe_gcc_toolchain(relocated, request("/relocated/gcc/bin/g++"));
		require(relocated_observation &&
					relocated_observation->builtin_macros_digest.value ==
						observed->builtin_macros_digest.value &&
					relocated_observation->include_search_digest.value ==
						observed->include_search_digest.value &&
					relocated_observation->abi_digest.value == observed->abi_digest.value,
				"machine-local compiler/sysroot relocation changed semantic observation identity");

		auto empty_sysroot_outputs = valid_outputs(prefix, {});
		empty_sysroot_outputs[2].standard_output = "\n";
		empty_sysroot_outputs[4].standard_error =
			"#include <...> search starts here:\n /usr/include\nEnd of search list.\n";
		fake_process_port empty_sysroot{std::move(empty_sysroot_outputs)};
		auto empty = probe_gcc_toolchain(empty_sysroot, request(prefix + "/bin/g++"));
		require(empty && empty->sysroot.state == capture_observation_state::unavailable &&
					empty->sysroot.reason == "compiler-reported-empty-sysroot",
				"empty GCC sysroot was promoted to a complete observation");

		auto changed_outputs = valid_outputs(prefix, sysroot);
		changed_outputs[1].executable_digest = "sha256:" + std::string(64U, '2');
		fake_process_port changed{std::move(changed_outputs)};
		auto changed_result = probe_gcc_toolchain(changed, request(prefix + "/bin/g++"));
		require(!changed_result && changed_result.error().field == "compiler" &&
					changed_result.error().detail == "changed-during-probe",
				"compiler replacement across probes was accepted");

		auto wrong_version_outputs = valid_outputs(prefix, sysroot);
		wrong_version_outputs[0].standard_output = "16.1.0\n";
		fake_process_port wrong_version{std::move(wrong_version_outputs)};
		auto wrong = probe_gcc_toolchain(wrong_version, request(prefix + "/bin/g++"));
		require(!wrong && wrong.error().code == "application-analysis.gcc-toolchain-unavailable" &&
					wrong.error().field == "exact-version",
				"unpinned GCC version was accepted");

		auto malformed_outputs = valid_outputs(prefix, sysroot);
		malformed_outputs[4].standard_error = "no include search markers\n";
		fake_process_port malformed{std::move(malformed_outputs)};
		auto malformed_result = probe_gcc_toolchain(malformed, request(prefix + "/bin/g++"));
		require(!malformed_result && malformed_result.error().field == "include-search" &&
					malformed_result.error().detail == "missing-start-marker",
				"malformed include search was accepted");

		gcc_probe_process_output timeout_output;
		timeout_output.terminal = gcc_probe_process_terminal::timed_out;
		fake_process_port timed_out{{std::move(timeout_output)}};
		auto timeout = probe_gcc_toolchain(timed_out, request(prefix + "/bin/g++"));
		require(!timeout &&
					timeout.error().code == "application-analysis.gcc-toolchain-unavailable" &&
					timeout.error().detail == "timed-out",
				"probe timeout was not preserved as structured unavailable");

		std::stop_source stopped;
		stopped.request_stop();
		fake_process_port cancelled{valid_outputs(prefix, sysroot)};
		auto cancelled_result =
			probe_gcc_toolchain(cancelled, request(prefix + "/bin/g++"), stopped.get_token());
		require(!cancelled_result && cancelled_result.error().detail == "cancelled",
				"probe cancellation was not preserved");

		std::cout << "GCC toolchain probe tests passed\n";
		return EXIT_SUCCESS;
	}
	catch (const std::exception& exception)
	{
		std::cerr << exception.what() << '\n';
		return EXIT_FAILURE;
	}
}
