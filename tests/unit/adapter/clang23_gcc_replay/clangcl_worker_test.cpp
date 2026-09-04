#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "llvm/clang23_gcc_replay/replay_frontend_authority.hpp"
#include "llvm/clang23_gcc_replay/worker_ingress.hpp"
#include "llvm/clang23_gcc_replay/worker_observation_codec.hpp"
#include "sdk/source_identity_internal.hpp"

namespace
{
	template <class value_type>
	void require(const value_type& condition)
	{
		if (!static_cast<bool>(condition))
			std::abort();
	}

	[[nodiscard]] std::vector<std::byte> bytes(const std::string_view value)
	{
		std::vector<std::byte> output;
		output.reserve(value.size());
		for (const auto byte : value)
			output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
		return output;
	}

	[[nodiscard]] std::string text(const std::span<const std::byte> value)
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

	[[nodiscard]] cxxlens::sdk::detail::validated_compiler_replay_input
	input(const std::string_view frontend, const std::string_view abi)
	{
		using namespace cxxlens::sdk;
		detail::compiler_replay_input_draft draft;
		draft.imported_project_id = "imported-project:sha256:" + std::string(64U, '1');
		draft.capture_bundle_digest = "sha256:" + std::string(64U, '2');
		draft.replay_plan_digest = "sha256:" + std::string(64U, '3');
		draft.compile_unit_id = "compile-unit:windows-main";
		draft.analysis_frontend = frontend;
		draft.target_abi = abi;
		draft.effective_arguments = frontend == "clang-cl-23.1.0"
			? std::vector<std::string>{"clang-cl",
									   "/Zs",
									   "/Iproject://include",
									   "project://main.cpp"}
			: std::vector<std::string>{"clang++", "-fsyntax-only", "project://main.cpp"};
		detail::decoded_capture_source_member source;
		source.logical_path = "project://main.cpp";
		source.content = bytes("#include \"answer.hpp\"\nint answer() { return value; }\n");
		source.content_digest = content_digest(source.content);
		source.role = "main";
		source.encoding = "utf8";
		auto source_file = detail::derive_source_file_id("main.cpp");
		require(source_file);
		source.file_id = *source_file;
		auto source_snapshot = detail::derive_source_snapshot_id(
			source.file_id, source.content_digest, *source.encoding);
		require(source_snapshot);
		source.source_snapshot_id = *source_snapshot;
		draft.source_members.push_back(std::move(source));
		detail::decoded_capture_source_member header;
		header.logical_path = "project://include/answer.hpp";
		header.content = bytes("inline constexpr int value = 23;\n");
		header.content_digest = content_digest(header.content);
		header.role = "header";
		header.encoding = "utf8";
		auto header_file = detail::derive_source_file_id("include/answer.hpp");
		require(header_file);
		header.file_id = *header_file;
		auto header_snapshot = detail::derive_source_snapshot_id(
			header.file_id, header.content_digest, *header.encoding);
		require(header_snapshot);
		header.source_snapshot_id = *header_snapshot;
		draft.source_members.push_back(std::move(header));
		draft.source_closure_digest = "application-source-closure:sha256:" + std::string(64U, '4');
		draft.requested_relation_descriptor_ids = {"cc.declaration.v1", "cc.entity.v1"};
		draft.interpretation = frontend == "clang-cl-23.1.0" ? "cc.clangcl23-msvc-replay-1"
															 : "cc.clang23-gcc-replay-1";
		auto validated = detail::validate_compiler_replay_input(std::move(draft));
		require(validated);
		return std::move(*validated);
	}
} // namespace

int main()
{
	using namespace cxxlens::detail::clang23_gcc_replay;
	auto msvc = input("clang-cl-23.1.0", "x86_64-pc-windows-msvc");
	std::istringstream source{text(msvc.bytes())};
	std::ostringstream output;
	auto accepted = execute_worker_ingress(source, output, msvc_replay_frontend_id);
	require(accepted);
	auto decoded = decode_worker_observations(bytes(output.str()));
	require(decoded && decoded->replay_input_digest == msvc.input_digest() &&
			decoded->error_count == 0U && !decoded->observations.declarations.empty());

	auto gcc = input("clang-23.1.0-gcc-mode", "x86_64-linux-gnu");
	std::istringstream foreign{text(gcc.bytes())};
	std::ostringstream rejected_output;
	auto rejected = execute_worker_ingress(foreign, rejected_output, msvc_replay_frontend_id);
	require(!rejected && rejected.error().field == "replay_input" &&
			rejected.error().detail == "wrong-worker-frontend" && rejected_output.str().empty());

	auto truncated = text(msvc.bytes());
	truncated.pop_back();
	std::istringstream malformed{truncated};
	std::ostringstream malformed_output;
	auto invalid = execute_worker_ingress(malformed, malformed_output, msvc_replay_frontend_id);
	require(!invalid && malformed_output.str().empty());
}
