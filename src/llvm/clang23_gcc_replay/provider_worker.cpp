#include "provider_worker.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <new>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/relations/build_compile_unit.hpp>
#include <cxxlens/relations/build_project.hpp>
#include <cxxlens/relations/build_toolchain_context.hpp>
#include <cxxlens/relations/build_variant.hpp>
#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_declaration.hpp>
#include <cxxlens/relations/cc_entity.hpp>
#include <cxxlens/relations/cc_type.hpp>
#include <cxxlens/relations/source_file.hpp>
#include <cxxlens/relations/source_span.hpp>

#include "observation_normalizer.hpp"
#include "replay_frontend_authority.hpp"
#include "sdk/provider_manifest_codec_internal.hpp"
#include "worker_parser.hpp"

namespace cxxlens::detail::clang23_gcc_replay
{
	namespace
	{
		constexpr std::size_t maximum_protocol_transcript_bytes = std::size_t{32U} * 1024U * 1024U;

		[[nodiscard]] sdk::error failure(std::string field, std::string detail)
		{
			return {
				"application-analysis.replay-provider-failed", std::move(field), std::move(detail)};
		}

		class transcript_sink final : public sdk::provider::frame_sink
		{
		  public:
			[[nodiscard]] sdk::result<void> write(const std::span<const std::byte> bytes) override
			{
				if (bytes.size() > maximum_bytes_ - bytes_.size())
					return sdk::unexpected(failure("protocol", "wire-size-limit"));
				bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
				return {};
			}

			[[nodiscard]] std::vector<std::byte> take() && noexcept
			{
				return std::move(bytes_);
			}

		  private:
			static constexpr std::size_t maximum_bytes_{maximum_protocol_transcript_bytes};
			std::vector<std::byte> bytes_;
		};

		class host_payload_sink final : public sdk::provider::detail::host_input_chunk_sink
		{
		  public:
			explicit host_payload_sink(const std::size_t maximum_bytes)
				: maximum_bytes_{maximum_bytes}
			{
			}

			[[nodiscard]] sdk::result<void> append(const std::span<const std::byte> bytes) override
			{
				if (bytes_.size() > maximum_bytes_ || bytes.size() > maximum_bytes_ - bytes_.size())
					return sdk::unexpected(failure("host_input", "payload-size-limit"));
				bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
				return {};
			}

			[[nodiscard]] const std::vector<std::byte>& bytes() const noexcept
			{
				return bytes_;
			}

		  private:
			std::size_t maximum_bytes_{};
			std::vector<std::byte> bytes_;
		};

		[[nodiscard]] sdk::result<std::vector<std::byte>>
		read_input(std::istream& input, const sdk::import_limits& limits)
		{
			constexpr std::size_t protocol_overhead = std::size_t{1024U} * 1024U;
			if (limits.maximum_bundle_bytes >
				std::numeric_limits<std::size_t>::max() - protocol_overhead)
				return sdk::unexpected(failure("stdin", "size-overflow"));
			const auto maximum = limits.maximum_bundle_bytes + protocol_overhead;
			std::vector<std::byte> encoded;
			encoded.reserve(std::min<std::size_t>(maximum, 8192U));
			std::array<char, 8192U> buffer{};
			while (input)
			{
				input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
				const auto count = input.gcount();
				if (count < 0)
					return sdk::unexpected(failure("stdin", "negative-read-count"));
				const auto bytes = static_cast<std::size_t>(count);
				if (bytes > maximum - encoded.size())
					return sdk::unexpected(failure("stdin", "wire-size-limit"));
				const auto view = std::as_bytes(std::span{buffer.data(), bytes});
				encoded.insert(encoded.end(), view.begin(), view.end());
			}
			if (!input.eof())
				return sdk::unexpected(failure("stdin", "read-failed"));
			if (encoded.empty())
				return sdk::unexpected(failure("stdin", "empty"));
			return encoded;
		}

		[[nodiscard]] sdk::result<sdk::relation_descriptor> descriptor(const std::string_view id)
		{
			const std::array values{
				&build::relations::project::descriptor(),
				&build::relations::compile_unit::descriptor(),
				&build::relations::variant::descriptor(),
				&build::relations::toolchain_context::descriptor(),
				&source::relations::file::descriptor(),
				&source::relations::span::descriptor(),
				&cc::relations::entity::descriptor(),
				&cc::relations::declaration::descriptor(),
				&cc::relations::type::descriptor(),
				&cc::relations::call_site::descriptor(),
				&cc::relations::call_direct_target::descriptor(),
			};
			const auto found = std::ranges::find(values,
												 id,
												 [](const auto* value)
												 {
													 return value->id;
												 });
			if (found == values.end())
				return sdk::unexpected(failure(std::string{id}, "unsupported-relation"));
			return **found;
		}

		[[nodiscard]] const std::vector<sdk::detached_row>*
		rows_for(const normalized_observation_candidates& value, const std::string_view id)
		{
			if (id == source::relations::span::descriptor().id)
				return &value.source_spans;
			if (id == cc::relations::entity::descriptor().id)
				return &value.entities;
			if (id == cc::relations::declaration::descriptor().id)
				return &value.declarations;
			if (id == cc::relations::type::descriptor().id)
				return &value.types;
			if (id == cc::relations::call_site::descriptor().id)
				return &value.call_sites;
			if (id == cc::relations::call_direct_target::descriptor().id)
				return &value.direct_targets;
			return nullptr;
		}

		[[nodiscard]] sdk::result<std::string> batch_identity(const std::string_view domain,
															  const std::string_view task,
															  const std::string_view descriptor_id)
		{
			return sdk::semantic_digest(domain,
										std::string{task} + "\n" + std::string{descriptor_id});
		}

		struct prepared_worker_input
		{
			sdk::provider::manifest provider;
			sdk::detail::validated_compiler_replay_input replay;
			sdk::detail::compiler_replay_frontend_contract frontend;
			sdk::provider::detail::sealed_host_input host_input;
			std::vector<sdk::relation_descriptor> descriptors;
			sdk::provider::execution_budget budget;
		};

		[[nodiscard]] sdk::result<prepared_worker_input>
		prepare_worker_input(const std::span<const std::byte> encoded,
							 const provider_worker_authority& authority,
							 const sdk::import_limits& limits)
		{
			constexpr std::size_t protocol_overhead = std::size_t{1024U} * 1024U;
			if (auto valid = limits.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (limits.maximum_bundle_bytes >
					std::numeric_limits<std::size_t>::max() - protocol_overhead ||
				encoded.size() > limits.maximum_bundle_bytes + protocol_overhead)
				return sdk::unexpected(failure("stdin", "wire-size-limit"));
			auto provider = sdk::detail::decode_provider_manifest(authority.host.provider_manifest);
			if (!provider)
				return sdk::unexpected(std::move(provider.error()));
			if (provider->provider_id != authority.provider_id ||
				provider->provider_version != authority.provider_version ||
				provider->provider_binary_digest != authority.provider_binary_digest ||
				provider->provider_semantic_contract_digest !=
					authority.provider_semantic_contract_digest)
				return sdk::unexpected(failure("provider", "worker-identity-mismatch"));
			const bool gcc_authority = authority.replay_frontend == gcc_replay_frontend_id &&
				authority.provider_id == provider_id &&
				authority.provider_version == provider_version;
			const bool msvc_authority = authority.replay_frontend == msvc_replay_frontend_id &&
				authority.provider_id == msvc_provider_id &&
				authority.provider_version == msvc_provider_version;
			if (!gcc_authority && !msvc_authority)
				return sdk::unexpected(failure("provider", "worker-authority-invalid"));
			auto frames = sdk::provider::decode_frame_stream(encoded, authority.host.limits);
			if (!frames)
				return sdk::unexpected(std::move(frames.error()));
			host_payload_sink host_payload{limits.maximum_bundle_bytes};
			auto host_input = sdk::provider::detail::validate_host_transcript_incremental(
				*frames, {authority.host, true}, host_payload);
			if (!host_input)
				return sdk::unexpected(std::move(host_input.error()));
			auto replay = sdk::detail::decode_compiler_replay_input(host_payload.bytes(), limits);
			if (!replay)
				return sdk::unexpected(std::move(replay.error()));
			auto frontend =
				sdk::detail::resolve_compiler_replay_frontend(replay->value().analysis_frontend,
															  replay->value().target_abi,
															  replay->value().effective_arguments);
			if (!frontend || frontend->analysis_frontend != authority.replay_frontend)
				return sdk::unexpected(failure("replay_input", "wrong-worker-frontend"));
			if (replay->input_digest() != host_input->task().task_input_digest)
				return sdk::unexpected(failure("replay_input", "host-digest-mismatch"));

			std::vector<sdk::relation_descriptor> descriptors;
			descriptors.reserve(replay->value().requested_relation_descriptor_ids.size());
			for (const auto& id : replay->value().requested_relation_descriptor_ids)
			{
				auto value = descriptor(id);
				if (!value)
					return sdk::unexpected(std::move(value.error()));
				descriptors.push_back(std::move(*value));
			}
			sdk::provider::execution_budget budget;
			budget.output_bytes = std::min(budget.output_bytes, host_input->credit().bytes);
			return prepared_worker_input{std::move(*provider),
										 std::move(*replay),
										 *frontend,
										 std::move(*host_input),
										 std::move(descriptors),
										 budget};
		}

		[[nodiscard]] sdk::result<provider_worker_result>
		validate_worker_output(prepared_worker_input prepared,
							   const provider_worker_authority& authority,
							   std::vector<std::byte> transcript)
		{
			if (transcript.empty() || transcript.size() > maximum_protocol_transcript_bytes)
				return sdk::unexpected(failure("protocol", "wire-size-limit"));
			auto required_features = prepared.provider.protocol.required_features;
			std::ranges::sort(required_features);
			auto offered_relations = prepared.provider.offered_relations;
			std::ranges::sort(offered_relations);
			auto provider_signature_digest = prepared.provider.signature;
			sdk::provider::detail::expected_provider_identity expected_identity{
				authority.provider_id,
				authority.provider_version,
				authority.provider_binary_digest,
				authority.provider_semantic_contract_digest,
				authority.host.limits.protocol_major,
				authority.host.limits.maximum_minor,
				std::move(required_features),
				authority.sandbox_policy_digest,
				std::move(offered_relations)};
			auto validated =
				sdk::provider::detail::validate_detached_provider_transcript_from_sealed_input(
					{std::move(prepared.provider),
					 std::move(expected_identity),
					 std::move(prepared.descriptors),
					 authority.host.limits,
					 prepared.budget},
					std::move(prepared.host_input),
					transcript);
			if (!validated)
				return sdk::unexpected(std::move(validated.error()));
			return provider_worker_result{std::move(transcript),
										  std::string{prepared.replay.value().replay_plan_digest},
										  std::move(provider_signature_digest),
										  std::move(*validated)};
		}

		[[nodiscard]] sdk::result<sdk::detail::validated_detached_provider_run>
		seal_worker_output(provider_worker_result result,
						   detached_provider_worker_authority authority,
						   const sdk::detail::detached_run_signer& signer,
						   const sdk::import_limits& limits)
		{
			if (!result.provider_signature_digest ||
				*result.provider_signature_digest != authority.provider_signature_digest)
				return sdk::unexpected(failure("provider_signature", "launcher-binding-mismatch"));
			if (authority.provider_revocation_state != "not-revoked")
				return sdk::unexpected(failure("provider_revocation", "not-trusted"));
			const auto& task = result.validated_transcript.input_seal.task();
			return sdk::detail::build_detached_provider_run_from_validated_transcript(
				{task.task_id,
				 task.task_input_digest,
				 task.normalized_invocation_digest,
				 task.toolchain_digest,
				 task.environment_digest,
				 result.replay_plan_digest,
				 {authority.worker.provider_id,
				  authority.worker.provider_version,
				  authority.worker.provider_binary_digest,
				  authority.worker.provider_semantic_contract_digest,
				  authority.provider_signature_digest,
				  authority.provider_revocation_state,
				  authority.worker.sandbox_policy_digest}},
				result.protocol_transcript,
				result.validated_transcript,
				signer,
				limits);
		}
	} // namespace

	sdk::result<provider_worker_result> run_provider_worker(std::istream& input,
															provider_worker_authority authority,
															const sdk::import_limits limits)
	{
		try
		{
			if (auto valid = limits.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			auto encoded = read_input(input, limits);
			if (!encoded)
				return sdk::unexpected(std::move(encoded.error()));
			auto prepared = prepare_worker_input(*encoded, authority, limits);
			if (!prepared)
				return sdk::unexpected(std::move(prepared.error()));
			auto parsed = parse_replay_input(prepared->replay);
			if (!parsed)
				return sdk::unexpected(std::move(parsed.error()));
			if (parsed->terminal != parse_terminal::parsed)
				return sdk::unexpected(failure("translation_unit", "syntax-rejected"));
			normalized_observation_candidates normalized;
			const bool source_authority_complete =
				std::ranges::all_of(prepared->replay.value().source_members,
									[](const auto& member)
									{
										return !member.file_id.empty() &&
											member.source_snapshot_id.has_value() &&
											member.encoding.has_value();
									});
			if (source_authority_complete)
			{
				auto candidate = normalize_observation_candidates(
					prepared->replay,
					worker_observation_output{std::string{prepared->replay.input_digest()},
											  parsed->declaration_count,
											  parsed->warning_count,
											  parsed->error_count,
											  std::move(parsed->observations)});
				if (!candidate)
					return sdk::unexpected(std::move(candidate.error()));
				normalized = std::move(*candidate);
			}
			else
			{
				normalized.replay_input_digest = prepared->replay.input_digest();
				normalized.unresolved = prepared->replay.value().unresolved;
				normalized.unresolved.push_back(
					{"source.identity",
					 "unavailable",
					 "validated-capture-source-identity-incomplete",
					 "recapture-source-content-with-encoding-and-snapshot-identity"});
			}

			transcript_sink sink;
			sdk::provider::protocol_writer writer{sink, authority.host.limits};
			writer.grant_credit(prepared->host_input.credit());
			auto hello = sdk::provider::encode_control_text(authority.host.provider_manifest);
			auto negotiated = sdk::provider::encode_schema_negotiate_metadata(
				{"cxxlens.provider-protocol.v2", sdk::provider::protocol_v2_minor});
			auto accepted =
				sdk::provider::encode_task_accepted_metadata({authority.provider_id,
															  authority.provider_version.string(),
															  prepared->host_input.task().task_id});
			if (!hello || !negotiated || !accepted)
				return sdk::unexpected(failure("protocol", "control-encoding"));
			if (auto sent = writer.send(sdk::provider::message_type::hello, *hello); !sent)
				return sdk::unexpected(std::move(sent.error()));
			if (auto sent = writer.send(sdk::provider::message_type::schema_negotiate, *negotiated);
				!sent)
				return sdk::unexpected(std::move(sent.error()));
			if (auto sent = writer.send(sdk::provider::message_type::task_accepted, *accepted);
				!sent)
				return sdk::unexpected(std::move(sent.error()));

			sdk::provider::context context{
				writer,
				{std::stop_token{}, prepared->budget},
				prepared->host_input.task().task_id,
				prepared->descriptors,
				std::array<std::string, 1U>{std::string{prepared->frontend.dependency_group}}};
			context.coverage().request("task", prepared->host_input.task().task_id);
			if (auto classified = context.coverage().classify({"task",
															   prepared->host_input.task().task_id,
															   "covered",
															   "translation-unit-executed"});
				!classified)
				return sdk::unexpected(std::move(classified.error()));
			for (const auto& value : prepared->descriptors)
			{
				const auto* rows = rows_for(normalized, value.id);
				if (rows != nullptr && source_authority_complete)
				{
					auto atomic = batch_identity("clang23.gcc-replay.atomic-output.v1",
												 prepared->host_input.task().task_id,
												 value.id);
					auto batch = batch_identity("clang23.gcc-replay.batch.v1",
												prepared->host_input.task().task_id,
												value.id);
					if (!atomic || !batch)
						return sdk::unexpected(failure(value.id, "batch-identity"));
					auto sink_value = context.relation(value);
					if (auto begun = sink_value.begin(
							std::string{prepared->frontend.dependency_group}, *atomic, *batch);
						!begun)
						return sdk::unexpected(std::move(begun.error()));
					for (const auto& row : *rows)
						if (auto pushed = sink_value.push(row); !pushed)
							return sdk::unexpected(std::move(pushed.error()));
					if (auto ended = sink_value.end(); !ended)
						return sdk::unexpected(std::move(ended.error()));
				}

				context.coverage().request("relation", value.id);
				const bool host_owned = rows == nullptr;
				const bool partial = !normalized.unresolved.empty();
				if (auto classified = context.coverage().classify(
						{"relation",
						 value.id,
						 host_owned || partial ? "unresolved" : "covered",
						 host_owned	   ? "host-materialization-authority"
							 : partial ? "capture-or-replay-gap"
									   : "frontend-observed"});
					!classified)
					return sdk::unexpected(std::move(classified.error()));
			}
			for (const auto& gap : normalized.unresolved)
				context.unresolved().add(
					{"application-analysis.capture-gap",
					 gap.field,
					 gap.state + ":" + gap.reason + ":" + gap.completion_action});
			context.evidence().add({"application-analysis.replay",
									prepared->replay.value().compile_unit_id,
									authority.provider_id,
									std::string{prepared->replay.value().replay_plan_digest}});
			if (auto valid = context.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			auto coverage = std::move(context.coverage()).finish();
			auto unresolved = std::move(context.unresolved()).finish();
			auto evidence = std::move(context.evidence()).finish();
			if (!coverage || !unresolved || !evidence)
				return sdk::unexpected(failure("protocol", "metadata-validation"));
			auto coverage_control = sdk::provider::encode_coverage_metadata(*coverage);
			auto unresolved_control = sdk::provider::encode_unresolved_metadata(*unresolved);
			auto evidence_control = sdk::provider::encode_evidence_metadata(*evidence);
			if (!coverage_control || !unresolved_control || !evidence_control)
				return sdk::unexpected(failure("protocol", "metadata-encoding"));
			for (const auto& [type, control] : {
					 std::pair{sdk::provider::message_type::coverage_chunk, &*coverage_control},
					 std::pair{sdk::provider::message_type::unresolved_chunk, &*unresolved_control},
					 std::pair{sdk::provider::message_type::progress, &*evidence_control},
				 })
				if (auto sent = writer.send(type, *control); !sent)
					return sdk::unexpected(std::move(sent.error()));
			auto complete =
				sdk::provider::encode_task_complete_metadata({prepared->host_input.task().task_id});
			if (!complete)
				return sdk::unexpected(std::move(complete.error()));
			if (auto sent = writer.send(sdk::provider::message_type::task_complete, *complete);
				!sent)
				return sdk::unexpected(std::move(sent.error()));
			auto transcript = std::move(sink).take();
			return validate_worker_output(std::move(*prepared), authority, std::move(transcript));
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(failure("memory", "allocation"));
		}
		catch (const std::length_error&)
		{
			return sdk::unexpected(failure("memory", "length"));
		}
	}

	sdk::result<provider_worker_result>
	validate_provider_worker_transcript(const std::span<const std::byte> host_transcript,
										const std::span<const std::byte> protocol_transcript,
										provider_worker_authority authority,
										const sdk::import_limits limits)
	{
		try
		{
			if (protocol_transcript.empty() ||
				protocol_transcript.size() > maximum_protocol_transcript_bytes)
				return sdk::unexpected(failure("protocol", "wire-size-limit"));
			auto prepared = prepare_worker_input(host_transcript, authority, limits);
			if (!prepared)
				return sdk::unexpected(std::move(prepared.error()));
			return validate_worker_output(
				std::move(*prepared),
				authority,
				std::vector<std::byte>{protocol_transcript.begin(), protocol_transcript.end()});
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(failure("memory", "allocation"));
		}
		catch (const std::length_error&)
		{
			return sdk::unexpected(failure("memory", "length"));
		}
	}

	sdk::result<void> execute_provider_worker(std::istream& input,
											  std::ostream& output,
											  provider_worker_authority authority,
											  const sdk::import_limits limits)
	{
		auto result = run_provider_worker(input, std::move(authority), limits);
		if (!result)
			return sdk::unexpected(std::move(result.error()));
		output.write(reinterpret_cast<const char*>(result->protocol_transcript.data()),
					 static_cast<std::streamsize>(result->protocol_transcript.size()));
		if (!output)
			return sdk::unexpected(failure("stdout", "write-failed"));
		return {};
	}

	sdk::result<sdk::detail::validated_detached_provider_run>
	run_detached_provider_worker(std::istream& input,
								 detached_provider_worker_authority authority,
								 const sdk::detail::detached_run_signer& signer,
								 const sdk::import_limits limits)
	{
		auto result = run_provider_worker(input, authority.worker, limits);
		if (!result)
			return sdk::unexpected(std::move(result.error()));
		return seal_worker_output(std::move(*result), std::move(authority), signer, limits);
	}

	sdk::result<sdk::detail::validated_detached_provider_run>
	seal_detached_provider_worker_transcript(const std::span<const std::byte> host_transcript,
											 const std::span<const std::byte> protocol_transcript,
											 detached_provider_worker_authority authority,
											 const sdk::detail::detached_run_signer& signer,
											 const sdk::import_limits limits)
	{
		auto result = validate_provider_worker_transcript(
			host_transcript, protocol_transcript, authority.worker, limits);
		if (!result)
			return sdk::unexpected(std::move(result.error()));
		return seal_worker_output(std::move(*result), std::move(authority), signer, limits);
	}

	sdk::result<void>
	execute_detached_provider_worker(std::istream& input,
									 std::ostream& output,
									 detached_provider_worker_authority authority,
									 const sdk::detail::detached_run_signer& signer,
									 const sdk::import_limits limits)
	{
		auto result = run_detached_provider_worker(input, std::move(authority), signer, limits);
		if (!result)
			return sdk::unexpected(std::move(result.error()));
		output.write(reinterpret_cast<const char*>(result->bytes().data()),
					 static_cast<std::streamsize>(result->bytes().size()));
		if (!output)
			return sdk::unexpected(failure("stdout", "write-failed"));
		return {};
	}
} // namespace cxxlens::detail::clang23_gcc_replay
