#include <array>
#include <cstddef>
#include <cstdlib>
#include <istream>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_entity.hpp>

#include "observation_v2.hpp"
#include "provider_worker.hpp"
#include "provider_worker_ingress.hpp"
#include "provider_worker_protocol_v2_input.hpp"
#include "provider_worker_v4.hpp"
#include "provider_worker_v4_ast_observer.hpp"
#include "provider_worker_v4_output_normalizer.hpp"
#include "source_closure_fd.hpp"
#include "source_closure_receiver.hpp"

namespace cxxlens::detail::clang22
{
	namespace
	{
		[[nodiscard]] sdk::error
		failure(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] std::optional<std::string> environment(const char* name)
		{
			const auto* value = std::getenv(name);
			return value == nullptr ? std::nullopt : std::optional<std::string>{value};
		}

		[[nodiscard]] sdk::result<sdk::provider::host_transcript_expectation> host_expectation()
		{
			auto manifest = environment("CXXLENS_PROVIDER_MANIFEST");
			auto task_id = environment("CXXLENS_PROVIDER_TASK_ID");
			auto task_input = environment("CXXLENS_PROVIDER_TASK_INPUT_DIGEST");
			auto invocation = environment("CXXLENS_PROVIDER_NORMALIZED_INVOCATION_DIGEST");
			auto toolchain = environment("CXXLENS_PROVIDER_TOOLCHAIN_DIGEST");
			auto environment_digest = environment("CXXLENS_PROVIDER_ENVIRONMENT_DIGEST");
			if (!manifest || !task_id || !task_input || !invocation || !toolchain ||
				!environment_digest || manifest->empty() || task_id->empty() ||
				task_input->empty() || invocation->empty() || toolchain->empty() ||
				environment_digest->empty())
				return sdk::unexpected(
					failure("provider.worker-protocol-v2-input-invalid", "environment", "binding"));
			return sdk::provider::host_transcript_expectation{
				*manifest,
				{*task_id, *task_input, *invocation, *toolchain, *environment_digest},
				sdk::provider::protocol_limits{}};
		}

		class ingress_authority final : public source_closure_task_v4_authority
		{
		  public:
			explicit ingress_authority(const provider_worker_v4_ingress& input) : input_{&input} {}

			[[nodiscard]] std::string_view task_id() const noexcept override
			{
				return input_->closure_binding.task_id;
			}
			[[nodiscard]] std::string_view task_v4_digest() const noexcept override
			{
				return input_->closure_binding.task_v4_digest;
			}
			[[nodiscard]] sdk::result<void> revalidate() const override
			{
				if (input_->closure_binding.task_id !=
					"task:" + input_->closure_binding.task_v4_digest)
					return sdk::unexpected(
						failure("source-closure.task-binding-mismatch", "task-id"));
				return validate_source_closure_task_v4_input_digest(
					input_->task_payload, input_->expected_task_v4_input_digest);
			}

		  private:
			const provider_worker_v4_ingress* input_;
		};

		class output_frame_sink final : public sdk::provider::frame_sink
		{
		  public:
			explicit output_frame_sink(std::ostream& output) noexcept : output_{&output} {}

			[[nodiscard]] sdk::result<void> write(const std::span<const std::byte> bytes) override
			{
				output_->write(reinterpret_cast<const char*>(bytes.data()),
							   static_cast<std::streamsize>(bytes.size()));
				if (!*output_)
					return sdk::unexpected(
						failure("provider.worker-output-write-failed", "stdout"));
				return {};
			}

		  private:
			std::ostream* output_;
		};

		[[nodiscard]] const sdk::relation_descriptor& descriptor_for_batch(const std::size_t index)
		{
			static const std::array<const sdk::relation_descriptor*, 6U> descriptors{
				&cc::relations::call_direct_target::descriptor(),
				&cc::relations::call_site::descriptor(),
				&cc::relations::entity::descriptor(),
				&materialization::call_observation_v2_descriptor(),
				&materialization::entity_observation_v2_descriptor(),
				&materialization::type_observation_v2_descriptor()};
			return *descriptors.at(index);
		}

	} // namespace

	int run_provider_worker_v4_source_closure(std::istream& input,
											  std::ostream& output,
											  const int read_descriptor,
											  const int write_descriptor)
	{
		const auto reject = [](const std::string_view)
		{
			return EXIT_FAILURE;
		};
		auto expected = host_expectation();
		if (!expected)
			return reject("host-expectation");
		auto protocol_input =
			decode_provider_worker_protocol_v2_input_until_close(input, *expected);
		if (!protocol_input)
			return reject("host-transcript");
		if (protocol_input->task.task_id != *environment("CXXLENS_PROVIDER_TASK_ID"))
			return reject("host-task-binding");
		std::string raw{reinterpret_cast<const char*>(protocol_input->payload.data()),
						protocol_input->payload.size()};
		auto ingress = decode_provider_worker_v4_ingress(std::move(raw));
		if (!ingress)
			return reject("decode-envelope");
		if (read_descriptor == write_descriptor)
			return reject("duplicate-fd");

		const auto session = environment("CXXLENS_PROVIDER_SOURCE_CLOSURE_SESSION_ID");
		const auto task = environment("CXXLENS_PROVIDER_SOURCE_CLOSURE_TASK_ID");
		const auto task_digest = environment("CXXLENS_PROVIDER_SOURCE_CLOSURE_TASK_V4_DIGEST");
		const auto closure = environment("CXXLENS_PROVIDER_SOURCE_CLOSURE_ID");
		const auto closure_digest = environment("CXXLENS_PROVIDER_SOURCE_CLOSURE_DIGEST");
		const auto transfer = environment("CXXLENS_PROVIDER_SOURCE_CLOSURE_TRANSFER_DIGEST");
		if (!session || !task || !task_digest || !closure || !closure_digest || !transfer ||
			*session != ingress->closure_binding.session_id ||
			*task != ingress->closure_binding.task_id ||
			*task_digest != ingress->closure_binding.task_v4_digest ||
			*closure != ingress->closure_binding.closure_id ||
			*closure_digest != ingress->closure_binding.closure_digest ||
			*transfer != ingress->expected_transfer_digest)
			return reject("environment-binding");
		if (protocol_input->task.task_id != ingress->closure_binding.task_id ||
			protocol_input->task.task_input_digest != protocol_input->protocol_content_digest)
			return reject("task-input-binding");

		auto channel = source_closure_fd_channel::create(
			{{read_descriptor, source_closure_fd_ownership::borrowed},
			 {write_descriptor, source_closure_fd_ownership::borrowed},
			 {}});
		if (!channel)
			return reject("fd-channel");
		ingress_authority authority{*ingress};
		source_closure_receiver_options options{
			ingress->closure_binding, &authority, ingress->stream_id, 16'384U, {}};
		auto received = receive_source_closure_frames(*channel, *channel, std::move(options));
		if (!received || received->credentials.transfer_digest != *transfer)
			return reject("receive-closure");

		auto decoded = decode_source_closure_task_v4_input(ingress->task_payload,
														   received->snapshot,
														   ingress->expected_base_task_digest,
														   ingress->expected_task_v4_input_digest);
		if (!decoded)
			return reject("decode-task-v4");
		decoded->input.base_task_projection = ingress->base_task_projection;
		if (decoded->identity.task_id != ingress->closure_binding.task_id ||
			decoded->identity.task_v4_digest != ingress->closure_binding.task_v4_digest)
			return reject("task-identity");

		const auto observer_metadata = *decoded;
		const auto output_authority = ingress->output;
		std::optional<provider_worker_v4_normalized_output> normalized;
		bool callback_ran = false;
		auto receipt = execute_provider_worker_v4(
			{std::move(*decoded),
			 std::move(received->snapshot),
			 std::move(ingress->input_authority),
			 [&observer_metadata, &output_authority, &normalized, &callback_ran](
				 provider::clang22::borrowed_translation_unit& unit) mutable -> sdk::result<void>
			 {
				 callback_ran = true;
				 provider_worker_v4_ast_observer_limits observer_limits;
				 observer_limits.maximum_observations =
					 static_cast<std::size_t>(output_authority.maximum_rows);
				 observer_limits.maximum_rows =
					 static_cast<std::size_t>(output_authority.maximum_rows);
				 auto observed = observe_provider_worker_v4_ast(
					 unit, observer_metadata, output_authority.compile_unit_id, observer_limits);
				 if (!observed)
					 return sdk::unexpected(std::move(observed.error()));
				 provider_worker_v4_output_normalizer_options options;
				 options.toolchain_context_id = output_authority.toolchain_context_id;
				 options.limits.maximum_observations =
					 static_cast<std::size_t>(output_authority.maximum_rows);
				 options.limits.maximum_rows =
					 static_cast<std::size_t>(output_authority.maximum_rows);
				 options.limits.maximum_output_bytes =
					 static_cast<std::size_t>(output_authority.maximum_output_bytes);
				 auto output_value =
					 normalize_provider_worker_v4_output(std::move(*observed), std::move(options));
				 if (!output_value)
					 return sdk::unexpected(std::move(output_value.error()));
				 normalized.emplace(std::move(*output_value));
				 return {};
			 }});
		if (!receipt || !callback_ran || !receipt->translation_unit_executed || !normalized)
			return reject("clang-execution");

		output_frame_sink sink{output};
		sdk::provider::protocol_writer writer{sink, expected->limits};
		writer.grant_credit(protocol_input->output_credit);
		writer.set_output_budget(output_authority.maximum_output_bytes);
		auto manifest = environment("CXXLENS_PROVIDER_MANIFEST");
		if (!manifest)
			return reject("manifest");
		auto hello = sdk::provider::encode_control_text(*manifest);
		auto schema = sdk::provider::encode_schema_negotiate_metadata(
			{"cxxlens.provider-protocol.v2", sdk::provider::protocol_v2_minor});
		if (!hello || !schema || !writer.send(sdk::provider::message_type::hello, *hello) ||
			!writer.send(sdk::provider::message_type::schema_negotiate, *schema))
			return reject("output-handshake");
		auto accepted =
			sdk::provider::encode_task_accepted_metadata({output_authority.provider_id,
														  output_authority.provider_version,
														  observer_metadata.identity.task_id});
		if (!accepted || !writer.send(sdk::provider::message_type::task_accepted, *accepted))
			return reject("task-accepted");

		const std::array<sdk::relation_descriptor, 6U> descriptors{descriptor_for_batch(0U),
																   descriptor_for_batch(1U),
																   descriptor_for_batch(2U),
																   descriptor_for_batch(3U),
																   descriptor_for_batch(4U),
																   descriptor_for_batch(5U)};
		sdk::provider::execution_budget budget;
		budget.output_bytes = output_authority.maximum_output_bytes;
		budget.rows = output_authority.maximum_rows;
		sdk::provider::context callback_context{writer,
												{std::stop_token{}, budget},
												observer_metadata.identity.task_id,
												descriptors,
												output_authority.dependency_groups};
		for (std::size_t index{}; index < normalized->batches.size(); ++index)
		{
			auto relation = callback_context.relation(descriptors[index]);
			const auto& batch = normalized->batches[index];
			if (auto begun = relation.begin(
					batch.dependency_group_id, batch.atomic_output_group_id, batch.batch_id);
				!begun)
				return reject("batch-begin");
			for (const auto& row : batch.rows)
				if (auto pushed = relation.push(row); !pushed)
					return reject("batch-row");
			if (auto ended = relation.end(); !ended)
				return reject("batch-end");
		}
		callback_context.coverage().request("task", observer_metadata.identity.task_id);
		callback_context.coverage().request("source-closure",
											observer_metadata.input.closure.snapshot_id);
		if (auto classified =
				callback_context.coverage().classify({"task",
													  observer_metadata.identity.task_id,
													  "covered",
													  "translation-unit-executed"});
			!classified)
			return reject("coverage");
		if (auto classified = callback_context.coverage().classify(
				{"source-closure",
				 observer_metadata.input.closure.snapshot_id,
				 normalized->exact_equivalence ? "covered" : "unresolved",
				 normalized->exact_equivalence ? "exact" : "provider-limitations"});
			!classified)
			return reject("coverage");
		for (const auto& item : normalized->unresolved)
			callback_context.unresolved().add(item);
		for (const auto& limitation : normalized->limitations)
			callback_context.evidence().add({"cxxlens.provider-limitation",
											 observer_metadata.identity.task_id,
											 output_authority.provider_id,
											 limitation});
		if (auto valid = callback_context.validate(); !valid)
			return reject("context");
		auto coverage = std::move(callback_context.coverage()).finish();
		auto unresolved = std::move(callback_context.unresolved()).finish();
		auto evidence = std::move(callback_context.evidence()).finish();
		if (!coverage || !unresolved || !evidence)
			return reject("metadata");
		auto coverage_control = sdk::provider::encode_coverage_metadata(*coverage);
		auto unresolved_control = sdk::provider::encode_unresolved_metadata(*unresolved);
		auto evidence_control = sdk::provider::encode_evidence_metadata(*evidence);
		if (!coverage_control || !unresolved_control || !evidence_control ||
			!writer.send(sdk::provider::message_type::coverage_chunk, *coverage_control) ||
			!writer.send(sdk::provider::message_type::unresolved_chunk, *unresolved_control) ||
			!writer.send(sdk::provider::message_type::progress, *evidence_control))
			return reject("metadata-output");
		auto complete =
			sdk::provider::encode_task_complete_metadata({observer_metadata.identity.task_id});
		if (!complete || !writer.send(sdk::provider::message_type::task_complete, *complete))
			return reject("task-complete");
		return EXIT_SUCCESS;
	}
} // namespace cxxlens::detail::clang22
