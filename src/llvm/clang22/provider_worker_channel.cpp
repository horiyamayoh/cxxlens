#include <array>
#include <cstddef>
#include <cstdlib>
#include <istream>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "provider_worker.hpp"
#include "provider_worker_ingress.hpp"
#include "provider_worker_v4.hpp"
#include "source_closure_fd.hpp"
#include "source_closure_receiver.hpp"

namespace cxxlens::detail::clang22
{
	namespace
	{
		using sdk::provider::frame_sink;
		using sdk::provider::message_type;

		[[nodiscard]] sdk::error
		failure(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::result<std::string> read_envelope(std::istream& input)
		{
			std::string raw;
			std::array<char, 64U * 1024U> buffer{};
			for (;;)
			{
				input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
				const auto count = input.gcount();
				if (count > 0)
				{
					if (raw.size() >
						provider_worker_v4_maximum_envelope_bytes - static_cast<std::size_t>(count))
						return sdk::unexpected(
							failure("provider.worker-v4-input-invalid", "envelope", "byte-limit"));
					raw.append(buffer.data(), static_cast<std::size_t>(count));
				}
				if (input.bad() || (input.fail() && !input.eof()))
					return sdk::unexpected(
						failure("provider.worker-v4-input-invalid", "envelope", "read"));
				if (input.eof())
					break;
			}
			if (raw.empty())
				return sdk::unexpected(
					failure("provider.worker-v4-input-invalid", "envelope", "empty"));
			return raw;
		}

		[[nodiscard]] std::optional<std::string> environment(const char* name)
		{
			const auto* value = std::getenv(name);
			return value == nullptr ? std::nullopt : std::optional<std::string>{value};
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

		class stdout_sink final : public frame_sink
		{
		  public:
			explicit stdout_sink(std::ostream& output) : output_{&output} {}
			sdk::result<void> write(const std::span<const std::byte> bytes) override
			{
				output_->write(reinterpret_cast<const char*>(bytes.data()),
							   static_cast<std::streamsize>(bytes.size()));
				output_->flush();
				return output_->good()
					? sdk::result<void>{}
					: sdk::unexpected(failure("provider.worker-write", "stdout", "write-failed"));
			}

		  private:
			std::ostream* output_;
		};

		[[nodiscard]] int emit_missing_output(std::ostream& output, const std::string_view task_id)
		{
			auto manifest = environment("CXXLENS_PROVIDER_MANIFEST");
			if (!manifest || manifest->empty())
				return EXIT_FAILURE;
			stdout_sink sink{output};
			sdk::provider::protocol_limits limits;
			sdk::provider::protocol_writer writer{sink, limits};
			writer.grant_credit({4U * 1024U * 1024U, 8U});
			auto hello = sdk::provider::encode_control_text(*manifest);
			auto schema = sdk::provider::encode_schema_negotiate_metadata(
				{"cxxlens.provider-protocol.v2", sdk::provider::protocol_v2_minor});
			auto failed = sdk::provider::encode_task_failed_metadata(
				{"provider.output-authority-missing", std::string{task_id}, "publication"});
			if (!hello || !schema || !failed || !writer.send(message_type::hello, *hello) ||
				!writer.send(message_type::schema_negotiate, *schema) ||
				!writer.send(message_type::task_failed, *failed))
				return EXIT_FAILURE;
			return EXIT_SUCCESS;
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
		auto raw = read_envelope(input);
		if (!raw)
			return reject("read-envelope");
		auto ingress = decode_provider_worker_v4_ingress(std::move(*raw));
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
		if (!received || received->transfer_digest != *transfer)
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

		bool callback_ran = false;
		auto receipt = execute_provider_worker_v4(
			{std::move(*decoded),
			 std::move(received->snapshot),
			 std::move(ingress->input_authority),
			 [&callback_ran](provider::clang22::borrowed_translation_unit&) -> sdk::result<void>
			 {
				 callback_ran = true;
				 return {};
			 }});
		if (!receipt || !callback_ran || !receipt->translation_unit_executed)
			return reject("clang-execution");
		return emit_missing_output(output, receipt->task_id);
	}
} // namespace cxxlens::detail::clang22
