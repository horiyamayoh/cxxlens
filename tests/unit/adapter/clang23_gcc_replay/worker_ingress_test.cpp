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

#include "llvm/clang23_gcc_replay/worker_parser.hpp"

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
		value.effective_arguments = {"clang++",
									 "-fsyntax-only",
									 "-nostdinc",
									 "-nostdinc++",
									 "-Iproject://include",
									 "project://main.cpp"};
		detail::decoded_capture_source_member source;
		source.logical_path = "project://main.cpp";
		source.content = bytes("#include \"answer.hpp\"\nint main() { return answer; }\n");
		source.content_digest = content_digest(source.content);
		source.role = "main";
		value.source_members.push_back(std::move(source));
		detail::decoded_capture_source_member header;
		header.logical_path = "project://include/answer.hpp";
		header.content = bytes("inline constexpr int answer = 0;\n");
		header.content_digest = content_digest(header.content);
		header.role = "header";
		value.source_members.push_back(std::move(header));
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

	void parser_uses_only_the_bound_source_closure()
	{
		auto value = input();
		auto parsed = cxxlens::detail::clang23_gcc_replay::parse_replay_input(value);
		require(parsed &&
				parsed->terminal == cxxlens::detail::clang23_gcc_replay::parse_terminal::parsed &&
				parsed->declaration_count > 0U && parsed->error_count == 0U);
		auto repeated = cxxlens::detail::clang23_gcc_replay::parse_replay_input(value);
		require(repeated && repeated->terminal == parsed->terminal &&
				repeated->declaration_count == parsed->declaration_count &&
				repeated->warning_count == parsed->warning_count &&
				repeated->error_count == parsed->error_count);

		using namespace cxxlens::sdk;
		detail::gcc_replay_input_draft invalid = value.value();
		invalid.source_members.front().content = bytes("int main( { return 0; }\n");
		invalid.source_members.front().content_digest =
			content_digest(invalid.source_members.front().content);
		auto syntax_input = detail::validate_gcc_replay_input(std::move(invalid));
		require(syntax_input);
		auto rejected = cxxlens::detail::clang23_gcc_replay::parse_replay_input(*syntax_input);
		require(rejected &&
				rejected->terminal ==
					cxxlens::detail::clang23_gcc_replay::parse_terminal::rejected &&
				rejected->error_count > 0U);
		std::istringstream syntax_stream{string(syntax_input->bytes())};
		std::ostringstream syntax_output;
		auto worker_rejected = cxxlens::detail::clang23_gcc_replay::validate_worker_ingress(
			syntax_stream, syntax_output);
		require(!worker_rejected && worker_rejected.error().field == "translation_unit" &&
				syntax_output.str().empty());

		detail::gcc_replay_input_draft ambient = value.value();
		ambient.source_members.front().content = bytes("#include \"/etc/passwd\"\n");
		ambient.source_members.front().content_digest =
			content_digest(ambient.source_members.front().content);
		auto ambient_input = detail::validate_gcc_replay_input(std::move(ambient));
		require(ambient_input);
		auto unavailable = cxxlens::detail::clang23_gcc_replay::parse_replay_input(*ambient_input);
		require(unavailable &&
				unavailable->terminal ==
					cxxlens::detail::clang23_gcc_replay::parse_terminal::rejected &&
				unavailable->error_count > 0U);
	}
} // namespace

int main()
{
	valid_input_emits_only_bound_digest();
	malformed_and_oversized_input_fail_without_output();
	parser_uses_only_the_bound_source_closure();
}
