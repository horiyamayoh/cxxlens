#include "llvm/clang23_gcc_replay/worker_ingress.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
	template <class value_type>
	void require(const value_type& condition)
	{
		if (!static_cast<bool>(condition))
			std::abort();
	}

	[[nodiscard]] std::vector<std::byte> bytes(const std::string& value)
	{
		std::vector<std::byte> output;
		for (const auto byte : value)
			output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
		return output;
	}

	[[nodiscard]] cxxlens::sdk::detail::validated_gcc_replay_input input()
	{
		using namespace cxxlens::sdk;
		detail::gcc_replay_input_draft value;
		value.imported_project_id = "imported-project:sha256:" + std::string(64U, '1');
		value.capture_bundle_digest = "sha256:" + std::string(64U, '2');
		value.replay_plan_digest = "sha256:" + std::string(64U, '3');
		value.compile_unit_id = "compile-unit:main";
		value.analysis_frontend = "clang-23.1.0-gcc-mode";
		value.target_abi = "x86_64-linux-gnu";
		value.effective_arguments = {"clang++", "-fsyntax-only", "project://main.cpp"};
		detail::decoded_capture_source_member source;
		source.logical_path = "project://main.cpp";
		source.content = bytes("int main() { return 0; }\n");
		source.content_digest = content_digest(source.content);
		source.role = "main";
		value.source_members.push_back(std::move(source));
		value.source_closure_digest = "application-source-closure:sha256:" + std::string(64U, '4');
		value.requested_relation_descriptor_ids = {"source.file.v1"};
		value.interpretation = "cc.clang23-gcc-replay-1";
		auto validated = detail::validate_gcc_replay_input(std::move(value));
		require(validated);
		return std::move(*validated);
	}

	[[nodiscard]] std::string string(std::span<const std::byte> value)
	{
		std::string output;
		output.reserve(value.size());
		std::ranges::transform(value,
							   std::back_inserter(output),
							   [](const std::byte byte)
							   {
								   return static_cast<char>(std::to_integer<unsigned char>(byte));
							   });
		return output;
	}

	void valid_input_emits_only_bound_digest()
	{
		auto value = input();
		std::istringstream source{string(value.bytes())};
		std::ostringstream output;
		auto result = cxxlens::detail::clang23_gcc_replay::validate_worker_ingress(source, output);
		require(result);
		require(output.str() == std::string{value.input_digest()} + "\n");
	}

	void malformed_and_oversized_input_fail_without_output()
	{
		auto value = input();
		auto malformed = string(value.bytes());
		malformed.pop_back();
		std::istringstream truncated{malformed};
		std::ostringstream truncated_output;
		auto rejected = cxxlens::detail::clang23_gcc_replay::validate_worker_ingress(
			truncated, truncated_output);
		require(!rejected && truncated_output.str().empty());

		cxxlens::sdk::import_limits limits;
		limits.maximum_bundle_bytes = value.bytes().size() - 1U;
		std::istringstream large{string(value.bytes())};
		std::ostringstream large_output;
		auto bounded = cxxlens::detail::clang23_gcc_replay::validate_worker_ingress(
			large, large_output, limits);
		require(!bounded && bounded.error().code == "application-analysis.import-limit-exceeded" &&
				bounded.error().detail == "input-bytes");
		require(large_output.str().empty());
	}
} // namespace

int main()
{
	valid_input_emits_only_bound_digest();
	malformed_and_oversized_input_fail_without_output();
}
