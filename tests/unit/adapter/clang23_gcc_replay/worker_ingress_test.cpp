#include "llvm/clang23_gcc_replay/worker_ingress.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_declaration.hpp>
#include <cxxlens/relations/cc_entity.hpp>
#include <cxxlens/relations/cc_type.hpp>
#include <cxxlens/relations/source_file.hpp>
#include <cxxlens/relations/source_span.hpp>

#include "llvm/clang23_gcc_replay/observation_normalizer.hpp"
#include "llvm/clang23_gcc_replay/provider_worker.hpp"
#include "llvm/clang23_gcc_replay/source_authority_binder.hpp"
#include "llvm/clang23_gcc_replay/worker_observation_codec.hpp"
#include "llvm/clang23_gcc_replay/worker_parser.hpp"
#include "sdk/provider_validation_internal.hpp"
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

	[[nodiscard]] cxxlens::sdk::detail::validated_compiler_replay_input input()
	{
		using namespace cxxlens::sdk;
		detail::compiler_replay_input_draft value;
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
							   "[[nodiscard]] int answer_value(int value) { return value; }\n"
							   "double answer_value(double value) { return value; }\n"
							   "int* pointer_value(int* value) { return value; }\n"
							   "#define CALL_ANSWER(v) answer_value(v)\n"
							   "int main() { return CALL_ANSWER(answer); }\n");
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
		value.requested_relation_descriptor_ids = {"cc.call_direct_target.v1",
												   "cc.call_site.v1",
												   "cc.declaration.v1",
												   "cc.entity.v1",
												   "cc.type.v1",
												   "source.file.v1",
												   "source.span.v1"};
		value.interpretation = "cc.clang23-gcc-replay-1";
		auto validated = detail::validate_compiler_replay_input(std::move(value));
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
	execute_bytes(const cxxlens::sdk::detail::validated_compiler_replay_input& value)
	{
		std::istringstream source{string(value.bytes())};
		std::ostringstream output;
		auto result = cxxlens::detail::clang23_gcc_replay::execute_worker_ingress(source, output);
		require(result);
		return bytes(output.str());
	}

	[[nodiscard]] cxxlens::detail::clang23_gcc_replay::worker_observation_output
	execute(const cxxlens::sdk::detail::validated_compiler_replay_input& value)
	{
		auto decoded =
			cxxlens::detail::clang23_gcc_replay::decode_worker_observations(execute_bytes(value));
		require(decoded);
		return std::move(*decoded);
	}

	[[nodiscard]] cxxlens::sdk::provider::manifest
	provider_manifest(const std::span<const std::string> relations)
	{
		using namespace cxxlens::detail::clang23_gcc_replay;
		using namespace cxxlens::sdk::provider;
		manifest value;
		value.provider_id = provider_id;
		value.provider_version = provider_version;
		value.package_identity = "cxxlens.clang23-gcc-replay.package";
		value.publisher = "cxxlens.project";
		value.license = "Apache-2.0 WITH LLVM-exception";
		value.protocol = {protocol_v2_major,
						  protocol_v2_minor,
						  protocol_v2_minor,
						  {"credit-backpressure", "task-input-chunks-v2"},
						  {}};
		value.platform_tuples = {"linux-x86_64-clang23"};
		value.provider_binary_digest = "sha256:" + std::string(64U, 'a');
		value.provider_semantic_contract_digest = "semantic-v2:sha256:" + std::string(64U, 'b');
		value.offered_relations.assign(relations.begin(), relations.end());
		value.interpretation_domains = {"cc.clang23-gcc-replay-1"};
		value.invalidation_contract = "sha256:" + std::string(64U, 'c');
		value.determinism_contract = "sha256:" + std::string(64U, 'd');
		value.resource_class = "provider.application-analysis";
		value.requested_qualifications = {"experimental"};
		return value;
	}

	[[nodiscard]] std::vector<cxxlens::sdk::relation_descriptor>
	frontend_descriptors(const std::span<const std::string> relations)
	{
		using namespace cxxlens;
		const std::array known{
			&source::relations::file::descriptor(),
			&source::relations::span::descriptor(),
			&cc::relations::entity::descriptor(),
			&cc::relations::declaration::descriptor(),
			&cc::relations::type::descriptor(),
			&cc::relations::call_site::descriptor(),
			&cc::relations::call_direct_target::descriptor(),
		};
		std::vector<sdk::relation_descriptor> output;
		for (const auto& id : relations)
		{
			const auto found = std::ranges::find(known,
												 id,
												 [](const auto* value)
												 {
													 return value->id;
												 });
			require(found != known.end());
			output.push_back(**found);
		}
		return output;
	}

	struct provider_execution
	{
		std::vector<std::byte> host;
		std::vector<std::byte> output;
		cxxlens::sdk::provider::manifest manifest;
		cxxlens::sdk::provider::host_transcript_expectation expectation;
		cxxlens::sdk::provider::protocol_credit credit;
	};

	[[nodiscard]] provider_execution
	execute_provider(const cxxlens::sdk::detail::validated_compiler_replay_input& value)
	{
		using namespace cxxlens::detail::clang23_gcc_replay;
		using namespace cxxlens::sdk;
		using namespace cxxlens::sdk::provider;
		provider_execution execution;
		execution.manifest = provider_manifest(value.value().requested_relation_descriptor_ids);
		require(execution.manifest.validate());
		execution.expectation = {execution.manifest.canonical_json(),
								 {"task:clang23-gcc-replay",
								  std::string{value.input_digest()},
								  "semantic-v2:sha256:" + std::string(64U, '1'),
								  "semantic-v2:sha256:" + std::string(64U, '2'),
								  "sha256:" + std::string(64U, '3')},
								 {}};
		execution.credit = {std::uint64_t{64U} * 1024U * 1024U, 65536U};
		auto host = encode_host_transcript(
			{execution.expectation,
			 execution.credit,
			 std::vector<std::byte>{value.bytes().begin(), value.bytes().end()}});
		require(host);
		execution.host = std::move(*host);
		std::istringstream input_stream{string(execution.host)};
		std::ostringstream output_stream;
		auto result = execute_provider_worker(
			input_stream,
			output_stream,
			{execution.expectation, execution.manifest.provider_semantic_contract_digest});
		require(result);
		execution.output = bytes(output_stream.str());
		return execution;
	}

	void provider_worker_emits_one_validated_protocol_authority()
	{
		using namespace cxxlens::detail::clang23_gcc_replay;
		using namespace cxxlens::sdk;
		using namespace cxxlens::sdk::provider;
		auto value = input();
		auto execution = execute_provider(value);
		auto frames = decode_frame_stream(execution.output);
		require(frames && !frames->empty() && frames->back().type == message_type::task_complete);
		auto descriptors = frontend_descriptors(value.value().requested_relation_descriptor_ids);
		execution_budget budget;
		const cxxlens::sdk::provider::detail::transcript_validation_request request{
			execution.expectation.task.task_id,
			std::string{provider_id},
			provider_version,
			&execution.manifest,
			descriptors,
			execution.credit,
			&budget,
			true,
		};
		auto validated =
			cxxlens::sdk::provider::detail::validate_provider_transcript(request, *frames, {});
		require(validated &&
				validated->kind ==
					cxxlens::sdk::provider::detail::transcript_terminal_kind::complete &&
				validated->sealed() && !validated->sealing_error());
		const auto& sealed = *validated->sealed();
		require(sealed.batches().size() == 6U && sealed.coverage().size() == 8U &&
				std::ranges::any_of(sealed.coverage(),
									[](const auto& coverage)
									{
										return coverage.id == "source.file.v1" &&
											coverage.state == "unresolved" &&
											coverage.reason == "host-materialization-authority";
									}) &&
				std::ranges::any_of(sealed.unresolved(),
									[](const auto& unresolved)
									{
										return unresolved.code ==
											"application-analysis.capture-gap" &&
											unresolved.subject == "cc.type.v1";
									}) &&
				std::ranges::any_of(sealed.coverage(),
									[](const auto& coverage)
									{
										return coverage.id == "cc.entity.v1" &&
											coverage.state == "unresolved" &&
											coverage.reason == "capture-or-replay-gap";
									}));
		auto repeated = execute_provider(value);
		require(repeated.output == execution.output);
		auto incomplete_draft = value.value();
		incomplete_draft.source_members.front().source_snapshot_id.reset();
		auto incomplete =
			cxxlens::sdk::detail::validate_compiler_replay_input(std::move(incomplete_draft));
		require(incomplete);
		auto partial_execution = execute_provider(*incomplete);
		auto partial_frames = decode_frame_stream(partial_execution.output);
		require(partial_frames);
		auto partial_descriptors =
			frontend_descriptors(incomplete->value().requested_relation_descriptor_ids);
		const cxxlens::sdk::provider::detail::transcript_validation_request partial_request{
			partial_execution.expectation.task.task_id,
			std::string{provider_id},
			provider_version,
			&partial_execution.manifest,
			partial_descriptors,
			partial_execution.credit,
			&budget,
			true,
		};
		auto partial = cxxlens::sdk::provider::detail::validate_provider_transcript(
			partial_request, *partial_frames, {});
		require(partial && partial->sealed() && partial->sealed()->batches().empty() &&
				std::ranges::any_of(partial->sealed()->unresolved(),
									[](const auto& unresolved)
									{
										return unresolved.subject == "source.identity";
									}));

		auto truncated = execution.host;
		truncated.pop_back();
		std::istringstream truncated_stream{string(truncated)};
		std::ostringstream rejected_output;
		auto rejected = execute_provider_worker(
			truncated_stream,
			rejected_output,
			{execution.expectation, execution.manifest.provider_semantic_contract_digest});
		require(!rejected && rejected_output.str().empty());

		std::istringstream valid_host_stream{string(execution.host)};
		std::ostringstream invalid_authority_output;
		auto invalid_authority =
			execute_provider_worker(valid_host_stream,
									invalid_authority_output,
									{execution.expectation, "sha256:" + std::string(64U, 'b')});
		require(!invalid_authority && invalid_authority_output.str().empty());
	}

	void valid_input_emits_bound_detached_observations()
	{
		auto value = input();
		auto output = execute(value);
		require(output.replay_input_digest == value.input_digest() && output.error_count == 0U &&
				output.declaration_count > 0U && output.observations.entities.size() == 5U &&
				output.observations.declarations.size() == 5U &&
				output.observations.types.size() == 5U &&
				output.observations.direct_calls.size() == 1U);
		require(std::ranges::count_if(output.observations.types,
									  [](const auto& type)
									  {
										  return type.structure.has_value() &&
											  !type.unavailable_reason.has_value() &&
											  !type.structure->components.empty();
									  }) == 4 &&
				std::ranges::count_if(output.observations.types,
									  [](const auto& type)
									  {
										  return !type.structure.has_value() &&
											  type.unavailable_reason == "non-builtin-component";
									  }) == 1);
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
				parsed->observations.entities.size() == 5U &&
				parsed->observations.declarations.size() == 5U &&
				parsed->observations.types.size() == 5U &&
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
		const auto attributed = std::ranges::find(
			parsed->observations.declarations,
			answer_entity->provider_local_key,
			&cxxlens::detail::clang23_gcc_replay::observed_declaration::entity_provider_local_key);
		require(attributed != parsed->observations.declarations.end() &&
				std::ranges::find(attributed->attributes, std::string_view{"nodiscard"}) !=
					attributed->attributes.end() &&
				!attributed->implicit && !attributed->friend_declaration && !attributed->exported);
		const auto& call = parsed->observations.direct_calls.front();
		require(call.caller_provider_local_key &&
				*call.caller_provider_local_key == main_entity->provider_local_key &&
				call.target_provider_local_key == answer_entity->provider_local_key &&
				call.kind == "direct_function" &&
				call.source.logical_path == "project://main.cpp" &&
				call.source.begin < call.source.end && call.source.role == "expansion" &&
				!call.origins.empty());
		for (const auto& origin : call.origins)
			require(origin.kind.starts_with("macro-spelling") &&
					origin.logical_path == "project://main.cpp" && origin.begin < origin.end &&
					origin.read_only);
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
		detail::compiler_replay_input_draft invalid = value.value();
		invalid.source_members.front().content = bytes("int main( { return 0; }\n");
		invalid.source_members.front().content_digest =
			content_digest(invalid.source_members.front().content);
		auto syntax_input = detail::validate_compiler_replay_input(std::move(invalid));
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

		detail::compiler_replay_input_draft ambient = value.value();
		ambient.source_members.front().content = bytes("#include \"/etc/passwd\"\n");
		ambient.source_members.front().content_digest =
			content_digest(ambient.source_members.front().content);
		auto ambient_input = detail::validate_compiler_replay_input(std::move(ambient));
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
		bool saw_expansion{};
		for (const auto& span : bound->spans)
		{
			require(span.observed.logical_path.starts_with("project://") &&
					span.observed.begin <= span.observed.end &&
					(span.role == "spelling" || span.role == "expansion") && span.read_only &&
					span.span_id.starts_with("source-span:") &&
					span.source_snapshot_id.starts_with("source-snapshot:") &&
					span.file_id.starts_with("file:"));
			saw_expansion = saw_expansion || span.role == "expansion";
		}
		require(saw_expansion);
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

		auto outside_origin = detached.observations;
		outside_origin.direct_calls.front().origins.front().logical_path = "project://missing.cpp";
		auto missing_origin = bind_observation_sources(value, outside_origin);
		require(!missing_origin && missing_origin.error().detail == "origin-not-in-source-closure");
		auto invalid_origin = detached.observations;
		invalid_origin.direct_calls.front().origins.front().kind = "invented";
		auto rejected_origin = bind_observation_sources(value, invalid_origin);
		require(!rejected_origin && rejected_origin.error().detail == "origin-kind-invalid");

		using namespace cxxlens::sdk;
		detail::compiler_replay_input_draft unavailable = value.value();
		unavailable.source_members.front().source_snapshot_id.reset();
		auto unbound_input = detail::validate_compiler_replay_input(std::move(unavailable));
		require(unbound_input);
		auto unbound = bind_observation_sources(*unbound_input, detached.observations);
		require(!unbound && unbound.error().detail == "capture-identity-unavailable");

		detail::compiler_replay_input_draft forged = value.value();
		forged.source_members.front().file_id = "file:sha256:" + std::string(64U, 'a');
		auto forged_input = detail::validate_compiler_replay_input(std::move(forged));
		require(forged_input);
		auto mismatch = bind_observation_sources(*forged_input, detached.observations);
		require(!mismatch && mismatch.error().detail == "file-identity-mismatch");

		detail::compiler_replay_input_draft stale = value.value();
		stale.source_members.front().source_snapshot_id =
			"source-snapshot:sha256:" + std::string(64U, 'b');
		auto stale_input = detail::validate_compiler_replay_input(std::move(stale));
		require(stale_input);
		auto stale_snapshot = bind_observation_sources(*stale_input, detached.observations);
		require(!stale_snapshot && stale_snapshot.error().detail == "snapshot-identity-mismatch");
	}

	void same_expansion_macro_calls_retain_distinct_ordered_origins()
	{
		using namespace cxxlens;
		auto base = input();
		auto draft = base.value();
		auto main = std::ranges::find(draft.source_members,
									  std::string_view{"main"},
									  &sdk::detail::decoded_capture_source_member::role);
		require(main != draft.source_members.end());
		main->content = bytes("int answer_value(int value) { return value; }\n"
							  "#define TWICE(v) (answer_value(v) + answer_value(v))\n"
							  "int main() { return TWICE(1); }\n");
		main->content_digest = sdk::content_digest(main->content);
		auto snapshot = sdk::detail::derive_source_snapshot_id(
			main->file_id, main->content_digest, *main->encoding);
		require(snapshot);
		main->source_snapshot_id = std::move(*snapshot);
		auto value = sdk::detail::validate_compiler_replay_input(std::move(draft));
		require(value);
		auto detached = execute(*value);
		require(detached.observations.direct_calls.size() == 2U);
		const auto& first = detached.observations.direct_calls[0];
		const auto& second = detached.observations.direct_calls[1];
		require(first.source == second.source && first.source.role == "expansion" &&
				!first.origins.empty() && !second.origins.empty() &&
				first.origins != second.origins);
		auto bound =
			detail::clang23_gcc_replay::bind_observation_sources(*value, detached.observations);
		require(bound &&
				std::ranges::count(bound->spans,
								   std::string_view{"expansion"},
								   &detail::clang23_gcc_replay::bound_source_span::role) == 1);
		require(execute_bytes(*value) == execute_bytes(*value));
		auto normalized =
			detail::clang23_gcc_replay::normalize_observation_candidates(*value, detached);
		require(normalized && normalized->call_sites.size() == 2U &&
				normalized->direct_targets.size() == 2U);
		std::vector<std::uint64_t> ordinals;
		for (const auto& row : normalized->call_sites)
		{
			const auto& ordinal_value = row.cells.at("cc.call_site.v1.ordinal").value;
			require(ordinal_value && std::holds_alternative<std::uint64_t>(*ordinal_value));
			ordinals.push_back(std::get<std::uint64_t>(*ordinal_value));
		}
		std::ranges::sort(ordinals);
		require(ordinals == std::vector<std::uint64_t>{0U, 1U});
		auto reversed = detached;
		std::ranges::reverse(reversed.observations.direct_calls);
		auto reversed_normalized =
			detail::clang23_gcc_replay::normalize_observation_candidates(*value, reversed);
		require(reversed_normalized && *reversed_normalized == *normalized);
	}

	void detached_observations_normalize_to_existing_relation_contracts()
	{
		using namespace cxxlens::detail::clang23_gcc_replay;
		auto value = input();
		auto detached = execute(value);
		auto normalized = normalize_observation_candidates(value, detached);
		require(normalized && normalized->replay_input_digest == value.input_digest() &&
				!normalized->source_spans.empty() && normalized->entities.size() == 5U);
		for (const auto& row : normalized->source_spans)
			require(row.descriptor_id == "source.span.v1");
		for (const auto& row : normalized->entities)
			require(row.descriptor_id == "cc.entity.v1" &&
					row.cells.at("cc.entity.v1.entity").value.has_value() &&
					row.cells.at("cc.entity.v1.provider_local_key").value.has_value());
		require(normalized->declarations.size() == 5U);
		for (const auto& row : normalized->declarations)
			require(row.descriptor_id == "cc.declaration.v1" &&
					row.cells.at("cc.declaration.v1.declaration").value.has_value() &&
					row.cells.at("cc.declaration.v1.entity").value.has_value() &&
					row.cells.at("cc.declaration.v1.source").value.has_value() &&
					row.cells.at("cc.declaration.v1.attributes").value.has_value() &&
					row.cells.at("cc.declaration.v1.is_implicit").value.has_value() &&
					row.cells.at("cc.declaration.v1.is_friend").value.has_value() &&
					row.cells.at("cc.declaration.v1.is_exported").value.has_value());
		require(normalized->call_sites.size() == 1U && normalized->direct_targets.size() == 1U);
		require(normalized->call_sites.front().descriptor_id == "cc.call_site.v1" &&
				normalized->direct_targets.front().descriptor_id == "cc.call_direct_target.v1");
		require(normalized->types.size() == 3U);
		for (const auto& row : normalized->types)
			require(row.descriptor_id == "cc.type.v1" &&
					row.cells.at("cc.type.v1.type").value.has_value() &&
					row.cells.at("cc.type.v1.component_signature_digest").value.has_value() &&
					row.cells.at("cc.type.v1.qualifiers").value.has_value() &&
					row.cells.at("cc.type.v1.spelling").value.has_value());
		require(std::ranges::any_of(normalized->unresolved,
									[](const auto& gap)
									{
										return gap.field == "cc.type.v1" &&
											gap.state == "unavailable" &&
											gap.reason.contains("non-builtin-component");
									}));
		auto repeated = normalize_observation_candidates(value, detached);
		require(repeated && *repeated == *normalized);

		auto mismatched = detached;
		mismatched.replay_input_digest = "sha256:" + std::string(64U, 'a');
		require(!normalize_observation_candidates(value, mismatched));
		auto failed = detached;
		failed.error_count = 1U;
		require(!normalize_observation_candidates(value, failed));
		auto noncanonical_attributes = detached;
		noncanonical_attributes.observations.declarations.front().attributes = {"z", "a"};
		require(!normalize_observation_candidates(value, noncanonical_attributes));
		auto noncanonical_type = detached;
		noncanonical_type.observations.types.front().structure->qualifiers = {"volatile", "const"};
		require(!normalize_observation_candidates(value, noncanonical_type));
		auto ambiguous_type_state = detached;
		ambiguous_type_state.observations.types.front().unavailable_reason = "forged";
		require(!normalize_observation_candidates(value, ambiguous_type_state));

		auto conflicting = detached;
		conflicting.observations.entities[1].provider_local_key =
			conflicting.observations.entities.front().provider_local_key;
		conflicting.observations.entities[1].canonical_type = "conflicting-type";
		require(!normalize_observation_candidates(value, conflicting));

		auto unrequested_draft = value.value();
		unrequested_draft.requested_relation_descriptor_ids = {"source.file.v1"};
		auto unrequested_input =
			cxxlens::sdk::detail::validate_compiler_replay_input(std::move(unrequested_draft));
		require(unrequested_input);
		auto unrequested_worker = detached;
		unrequested_worker.replay_input_digest = std::string{unrequested_input->input_digest()};
		auto unrequested = normalize_observation_candidates(*unrequested_input, unrequested_worker);
		require(unrequested && unrequested->source_spans.empty() && unrequested->entities.empty() &&
				unrequested->declarations.empty() && unrequested->types.empty() &&
				unrequested->call_sites.empty() && unrequested->direct_targets.empty());

		auto entity_only_draft = value.value();
		entity_only_draft.requested_relation_descriptor_ids = {"cc.entity.v1"};
		auto entity_only_input =
			cxxlens::sdk::detail::validate_compiler_replay_input(std::move(entity_only_draft));
		require(entity_only_input);
		auto entity_only_worker = detached;
		entity_only_worker.replay_input_digest = std::string{entity_only_input->input_digest()};
		auto entity_only = normalize_observation_candidates(*entity_only_input, entity_only_worker);
		require(entity_only && entity_only->source_spans.empty() &&
				entity_only->entities.size() == 5U);
		for (const auto& row : entity_only->entities)
			require(!row.cells.contains("cc.entity.v1.anchor"));

		auto declaration_only_draft = value.value();
		declaration_only_draft.requested_relation_descriptor_ids = {"cc.declaration.v1"};
		auto declaration_only_input =
			cxxlens::sdk::detail::validate_compiler_replay_input(std::move(declaration_only_draft));
		require(declaration_only_input);
		auto declaration_only_worker = detached;
		declaration_only_worker.replay_input_digest =
			std::string{declaration_only_input->input_digest()};
		auto declaration_only =
			normalize_observation_candidates(*declaration_only_input, declaration_only_worker);
		require(declaration_only && declaration_only->entities.empty() &&
				declaration_only->source_spans.empty() &&
				declaration_only->declarations.size() == 5U);

		auto type_only_draft = value.value();
		type_only_draft.requested_relation_descriptor_ids = {"cc.type.v1"};
		auto type_only_input =
			cxxlens::sdk::detail::validate_compiler_replay_input(std::move(type_only_draft));
		require(type_only_input);
		auto type_only_worker = detached;
		type_only_worker.replay_input_digest = std::string{type_only_input->input_digest()};
		auto partial_type = normalize_observation_candidates(*type_only_input, type_only_worker);
		require(partial_type && partial_type->types.size() == 3U &&
				std::ranges::any_of(partial_type->unresolved,
									[](const auto& gap)
									{
										return gap.field == "cc.type.v1" &&
											gap.state == "unavailable" &&
											gap.completion_action ==
											"use-a-qualified-native-gap-provider-for-this-type-"
											"structure";
									}));

		auto calls_only_draft = value.value();
		calls_only_draft.requested_relation_descriptor_ids = {"cc.call_direct_target.v1",
															  "cc.call_site.v1"};
		auto calls_only_input =
			cxxlens::sdk::detail::validate_compiler_replay_input(std::move(calls_only_draft));
		require(calls_only_input);
		auto calls_only_worker = detached;
		calls_only_worker.replay_input_digest = std::string{calls_only_input->input_digest()};
		auto missing_target = calls_only_worker;
		const auto target =
			missing_target.observations.direct_calls.front().target_provider_local_key;
		std::erase_if(missing_target.observations.entities,
					  [&](const auto& entity)
					  {
						  return entity.provider_local_key == target;
					  });
		auto partial = normalize_observation_candidates(*calls_only_input, missing_target);
		require(partial && partial->call_sites.size() == 1U && partial->direct_targets.empty() &&
				std::ranges::any_of(partial->unresolved,
									[](const auto& gap)
									{
										return gap.field == "cc.call_direct_target.v1.target" &&
											gap.state == "unavailable" &&
											gap.completion_action ==
											"recapture-with-a-worker-that-detaches-the-direct-"
											"callee-entity";
									}));
		auto missing_caller = calls_only_worker;
		missing_caller.observations.direct_calls.front().caller_provider_local_key =
			"missing-caller";
		require(!normalize_observation_candidates(*calls_only_input, missing_caller));
	}
} // namespace

int main()
{
	provider_worker_emits_one_validated_protocol_authority();
	valid_input_emits_bound_detached_observations();
	worker_output_codec_is_bounded_and_strict();
	malformed_and_oversized_input_fail_without_output();
	parser_uses_only_the_bound_source_closure();
	observations_bind_only_to_capture_source_authority();
	same_expansion_macro_calls_retain_distinct_ordered_origins();
	detached_observations_normalize_to_existing_relation_contracts();
}
