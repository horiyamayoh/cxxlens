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

#include "llvm/clang23_gcc_replay/source_authority_binder.hpp"
#include "llvm/clang23_gcc_replay/worker_observation_codec.hpp"
#include "llvm/clang23_gcc_replay/worker_parser.hpp"
#include "sdk/source_identity_internal.hpp"

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
		source.content = bytes("#include \"answer.hpp\"\n"
							   "int answer_value(int value) { return value; }\n"
							   "double answer_value(double value) { return value; }\n"
							   "int main() { return answer_value(answer); }\n");
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
		value.source_members.push_back(std::move(source));
		detail::decoded_capture_source_member header;
		header.logical_path = "project://include/answer.hpp";
		header.content = bytes("inline constexpr int answer = 0;\n"
							   "inline int header_answer() { return answer; }\n");
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

	[[nodiscard]] std::vector<std::byte>
	execute_bytes(const cxxlens::sdk::detail::validated_gcc_replay_input& value)
	{
		std::istringstream source{string(value.bytes())};
		std::ostringstream output;
		auto result = cxxlens::detail::clang23_gcc_replay::execute_worker_ingress(source, output);
		require(result);
		return bytes(output.str());
	}

	[[nodiscard]] cxxlens::detail::clang23_gcc_replay::worker_observation_output
	execute(const cxxlens::sdk::detail::validated_gcc_replay_input& value)
	{
		auto decoded =
			cxxlens::detail::clang23_gcc_replay::decode_worker_observations(execute_bytes(value));
		require(decoded);
		return std::move(*decoded);
	}

	void valid_input_emits_bound_detached_observations()
	{
		auto value = input();
		auto output = execute(value);
		require(output.replay_input_digest == value.input_digest() && output.error_count == 0U &&
				output.declaration_count > 0U && output.observations.entities.size() == 4U &&
				output.observations.declarations.size() == 4U &&
				output.observations.types.size() == 4U &&
				output.observations.direct_calls.size() == 1U);
		auto repeated = execute(value);
		require(repeated == output);
		require(execute_bytes(value) == execute_bytes(value));
	}

	void worker_output_codec_is_bounded_and_strict()
	{
		using namespace cxxlens::detail::clang23_gcc_replay;
		auto value = input();
		auto encoded = execute_bytes(value);
		auto truncated = encoded;
		truncated.pop_back();
		require(!decode_worker_observations(truncated));
		auto trailing = encoded;
		trailing.push_back(std::byte{});
		require(!decode_worker_observations(trailing));
		auto wrong_root = encoded;
		wrong_root.front() = std::byte{0x80};
		require(!decode_worker_observations(wrong_root));

		worker_observation_codec_limits limits;
		limits.maximum_bytes = encoded.size() - 1U;
		auto bounded = decode_worker_observations(encoded, limits);
		require(!bounded && bounded.error().field == "binary");
		limits = {};
		limits.maximum_observations = 1U;
		auto bounded_items = decode_worker_observations(encoded, limits);
		require(!bounded_items && bounded_items.error().field == "binary");

		auto parsed = parse_replay_input(value);
		require(parsed);
		auto bounded_output = encode_worker_observations(value.input_digest(), *parsed, limits);
		require(!bounded_output && bounded_output.error().field == "observations");
		parsed->terminal = parse_terminal::rejected;
		auto rejected = encode_worker_observations(value.input_digest(), *parsed);
		require(!rejected && rejected.error().field == "parse_terminal");
	}

	void malformed_and_oversized_input_fail_without_output()
	{
		auto value = input();
		auto malformed = string(value.bytes());
		malformed.pop_back();
		std::istringstream truncated{malformed};
		std::ostringstream truncated_output;
		auto rejected = cxxlens::detail::clang23_gcc_replay::execute_worker_ingress(
			truncated, truncated_output);
		require(!rejected && truncated_output.str().empty());

		cxxlens::sdk::import_limits limits;
		limits.maximum_bundle_bytes = value.bytes().size() - 1U;
		std::istringstream large{string(value.bytes())};
		std::ostringstream large_output;
		auto bounded = cxxlens::detail::clang23_gcc_replay::execute_worker_ingress(
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
				parsed->declaration_count > 0U && parsed->error_count == 0U &&
				parsed->observations.entities.size() == 4U &&
				parsed->observations.declarations.size() == 4U &&
				parsed->observations.types.size() == 4U &&
				parsed->observations.direct_calls.size() == 1U &&
				parsed->observations.limitations.empty() &&
				parsed->observations.traversal_entries > 0U);
		const auto answer_entity =
			std::ranges::find_if(parsed->observations.entities,
								 [](const auto& entity)
								 {
									 return entity.qualified_name == "answer_value" &&
										 entity.canonical_type.contains("int (int)");
								 });
		const auto overload =
			std::ranges::find_if(parsed->observations.entities,
								 [](const auto& entity)
								 {
									 return entity.qualified_name == "answer_value" &&
										 entity.canonical_type.contains("double (double)");
								 });
		const auto main_entity = std::ranges::find(
			parsed->observations.entities,
			std::string_view{"main"},
			&cxxlens::detail::clang23_gcc_replay::observed_entity::qualified_name);
		const auto header_entity = std::ranges::find(
			parsed->observations.entities,
			std::string_view{"header_answer"},
			&cxxlens::detail::clang23_gcc_replay::observed_entity::qualified_name);
		require(answer_entity != parsed->observations.entities.end() &&
				overload != parsed->observations.entities.end() &&
				answer_entity->provider_local_key != overload->provider_local_key &&
				main_entity != parsed->observations.entities.end() &&
				header_entity != parsed->observations.entities.end() &&
				header_entity->source.logical_path == "project://include/answer.hpp");
		const auto& call = parsed->observations.direct_calls.front();
		require(call.caller_provider_local_key &&
				*call.caller_provider_local_key == main_entity->provider_local_key &&
				call.target_provider_local_key == answer_entity->provider_local_key &&
				call.kind == "direct_function" &&
				call.source.logical_path == "project://main.cpp" &&
				call.source.begin < call.source.end);
		auto repeated = cxxlens::detail::clang23_gcc_replay::parse_replay_input(value);
		require(repeated && repeated->terminal == parsed->terminal &&
				repeated->declaration_count == parsed->declaration_count &&
				repeated->warning_count == parsed->warning_count &&
				repeated->error_count == parsed->error_count &&
				repeated->observations == parsed->observations);

		cxxlens::detail::clang23_gcc_replay::observer_limits observation_limits;
		observation_limits.maximum_observations = 1U;
		auto bounded =
			cxxlens::detail::clang23_gcc_replay::parse_replay_input(value, observation_limits);
		require(!bounded &&
				bounded.error().code == "application-analysis.replay-observation-resource-limit" &&
				bounded.error().detail == "observations");
		observation_limits = {};
		observation_limits.maximum_traversal_entries = 1U;
		auto traversal =
			cxxlens::detail::clang23_gcc_replay::parse_replay_input(value, observation_limits);
		require(!traversal && traversal.error().detail == "traversal-entries");
		observation_limits = {};
		observation_limits.maximum_logical_bytes = 1U;
		auto logical_bytes =
			cxxlens::detail::clang23_gcc_replay::parse_replay_input(value, observation_limits);
		require(!logical_bytes && logical_bytes.error().detail == "logical-bytes");
		observation_limits = {};
		observation_limits.maximum_traversal_depth = 0U;
		auto invalid_limits =
			cxxlens::detail::clang23_gcc_replay::parse_replay_input(value, observation_limits);
		require(!invalid_limits && invalid_limits.error().field == "maximum_traversal_depth" &&
				invalid_limits.error().detail == "outside-product-bound");

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
		auto worker_rejected = cxxlens::detail::clang23_gcc_replay::execute_worker_ingress(
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

	void observations_bind_only_to_capture_source_authority()
	{
		using namespace cxxlens::detail::clang23_gcc_replay;
		auto value = input();
		auto detached = execute(value);
		auto bound = bind_observation_sources(value, detached.observations);
		require(bound && bound->replay_input_digest == value.input_digest() &&
				!bound->spans.empty());
		for (const auto& span : bound->spans)
		{
			require(span.observed.logical_path.starts_with("project://") &&
					span.observed.begin <= span.observed.end && span.role == "spelling" &&
					span.read_only && span.span_id.starts_with("source-span:") &&
					span.source_snapshot_id.starts_with("source-snapshot:") &&
					span.file_id.starts_with("file:"));
		}
		auto repeated = bind_observation_sources(value, detached.observations);
		require(repeated && *repeated == *bound);

		auto unknown = detached.observations;
		unknown.entities.front().source.logical_path = "project://missing.cpp";
		auto missing = bind_observation_sources(value, unknown);
		require(!missing && missing.error().detail == "not-in-source-closure");

		auto outside = detached.observations;
		outside.declarations.front().source.end = 1000000U;
		auto out_of_bounds = bind_observation_sources(value, outside);
		require(!out_of_bounds && out_of_bounds.error().detail == "range-out-of-bounds");

		using namespace cxxlens::sdk;
		detail::gcc_replay_input_draft unavailable = value.value();
		unavailable.source_members.front().source_snapshot_id.reset();
		auto unbound_input = detail::validate_gcc_replay_input(std::move(unavailable));
		require(unbound_input);
		auto unbound = bind_observation_sources(*unbound_input, detached.observations);
		require(!unbound && unbound.error().detail == "capture-identity-unavailable");

		detail::gcc_replay_input_draft forged = value.value();
		forged.source_members.front().file_id = "file:sha256:" + std::string(64U, 'a');
		auto forged_input = detail::validate_gcc_replay_input(std::move(forged));
		require(forged_input);
		auto mismatch = bind_observation_sources(*forged_input, detached.observations);
		require(!mismatch && mismatch.error().detail == "file-identity-mismatch");

		detail::gcc_replay_input_draft stale = value.value();
		stale.source_members.front().source_snapshot_id =
			"source-snapshot:sha256:" + std::string(64U, 'b');
		auto stale_input = detail::validate_gcc_replay_input(std::move(stale));
		require(stale_input);
		auto stale_snapshot = bind_observation_sources(*stale_input, detached.observations);
		require(!stale_snapshot && stale_snapshot.error().detail == "snapshot-identity-mismatch");
	}
} // namespace

int main()
{
	valid_input_emits_bound_detached_observations();
	worker_output_codec_is_bounded_and_strict();
	malformed_and_oversized_input_fail_without_output();
	parser_uses_only_the_bound_source_closure();
	observations_bind_only_to_capture_source_authority();
}
