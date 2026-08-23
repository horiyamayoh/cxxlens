#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <istream>
#include <limits>
#include <map>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "materialization_json.hpp"
#include "provider_worker.hpp"
#include "provider_worker_v4.hpp"
#include "source_closure_fd.hpp"
#include "source_closure_receiver.hpp"

namespace cxxlens::detail::clang22
{
	namespace
	{
		using materialization::json_value;
		using sdk::provider::frame_sink;
		using sdk::provider::message_type;

		constexpr std::size_t maximum_envelope_bytes =
			source_closure_task_v4_maximum_payload_bytes + 64U * 1024U;

		[[nodiscard]] sdk::error
		failure(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::result<const json_value*>
		member(const json_value& object, const std::string_view name, const std::string_view field)
		{
			if (object.as_object() == nullptr)
				return sdk::unexpected(failure(
					"provider.worker-v4-input-invalid", std::string{field}, "object-required"));
			const auto* value = object.member(name);
			if (value == nullptr)
				return sdk::unexpected(failure("provider.worker-v4-input-invalid",
											   std::string{field},
											   "missing:" + std::string{name}));
			return value;
		}

		[[nodiscard]] sdk::result<std::string> string_member(const json_value& object,
															 const std::string_view name,
															 const std::string_view field)
		{
			auto value = member(object, name, field);
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			if (const auto* string = (*value)->as_string())
				return *string;
			return sdk::unexpected(
				failure("provider.worker-v4-input-invalid", std::string{field}, "string-required"));
		}

		[[nodiscard]] sdk::result<std::uint64_t> unsigned_member(const json_value& object,
																 const std::string_view name,
																 const std::string_view field)
		{
			auto value = member(object, name, field);
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			if (const auto* unsigned_value = (*value)->as_unsigned_integer())
				return *unsigned_value;
			if (const auto* signed_value = (*value)->as_signed_integer();
				signed_value != nullptr && *signed_value >= 0)
				return static_cast<std::uint64_t>(*signed_value);
			return sdk::unexpected(failure("provider.worker-v4-input-invalid",
										   std::string{field},
										   "unsigned-integer-required"));
		}

		[[nodiscard]] sdk::result<void>
		exact_members(const json_value& object,
					  const std::span<const std::string_view> expected,
					  const std::string_view field)
		{
			if (object.as_object() == nullptr || !object.has_exact_members(expected))
				return sdk::unexpected(failure(
					"provider.worker-v4-input-invalid", std::string{field}, "field-census"));
			return {};
		}

		[[nodiscard]] sdk::result<std::vector<std::string>> string_array_member(
			const json_value& object, const std::string_view name, const std::string_view field)
		{
			auto value = member(object, name, field);
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			const auto* array = (*value)->as_array();
			if (array == nullptr || array->empty() || array->size() > 4096U)
				return sdk::unexpected(
					failure("provider.worker-v4-input-invalid", std::string{field}, "array-bound"));
			std::vector<std::string> output;
			output.reserve(array->size());
			for (const auto& item : *array)
			{
				const auto* string = item.as_string();
				if (string == nullptr || string->empty() || string->size() > 4096U)
					return sdk::unexpected(failure(
						"provider.worker-v4-input-invalid", std::string{field}, "string-bound"));
				output.push_back(*string);
			}
			return output;
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
					if (raw.size() > maximum_envelope_bytes - static_cast<std::size_t>(count))
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

		struct worker_v4_ingress
		{
			std::vector<std::byte> task_payload;
			std::vector<std::byte> base_task_projection;
			std::string expected_base_task_digest;
			std::string expected_task_v4_input_digest;
			provider_task_v4_input_authority input_authority;
			source_closure_transfer_binding closure_binding;
			std::string expected_transfer_digest;
			std::uint64_t stream_id{};
		};

		[[nodiscard]] sdk::result<worker_v4_ingress> decode_ingress(std::string raw)
		{
			materialization::json_limits limits;
			limits.max_input_bytes = maximum_envelope_bytes;
			limits.max_depth = 16U;
			limits.max_array_elements = 4096U;
			limits.max_object_members = 32U;
			auto document = materialization::parse_json_object(std::move(raw), limits);
			if (!document)
				return sdk::unexpected(failure(
					"provider.worker-v4-input-invalid", "envelope", document.error().detail));
			if (materialization::canonical_json(document->root()) != document->raw_bytes())
				return sdk::unexpected(
					failure("provider.worker-v4-input-invalid", "envelope", "noncanonical"));
			constexpr std::array<std::string_view, 8U> root_fields{"base_task_projection",
																   "closure_binding",
																   "expected_base_task_digest",
																   "expected_task_v4_input_digest",
																   "input_authority",
																   "stream_id",
																   "task_v4_payload",
																   "schema"};
			if (auto valid = exact_members(document->root(), root_fields, "envelope"); !valid)
				return sdk::unexpected(std::move(valid.error()));
			auto schema = string_member(document->root(), "schema", "schema");
			if (!schema || *schema != "cxxlens.clang22.worker-ingress.v4")
				return sdk::unexpected(
					failure("provider.worker-v4-input-invalid", "schema", "unsupported"));
			auto base_digest =
				string_member(document->root(), "expected_base_task_digest", "base-task-digest");
			auto input_digest = string_member(
				document->root(), "expected_task_v4_input_digest", "task-v4-input-digest");
			auto stream_id = unsigned_member(document->root(), "stream_id", "stream-id");
			if (!base_digest || !input_digest || !stream_id || *stream_id == 0U)
				return sdk::unexpected(failure("provider.worker-v4-input-invalid", "authority"));

			auto closure_object = member(document->root(), "closure_binding", "closure-binding");
			auto authority_object = member(document->root(), "input_authority", "input-authority");
			auto payload_object = member(document->root(), "task_v4_payload", "task-v4-payload");
			auto base_projection_object =
				member(document->root(), "base_task_projection", "base-task-projection");
			if (!closure_object || !authority_object || !payload_object ||
				!base_projection_object || (*closure_object)->as_object() == nullptr ||
				(*authority_object)->as_object() == nullptr ||
				(*payload_object)->as_object() == nullptr ||
				(*base_projection_object)->as_object() == nullptr)
				return sdk::unexpected(
					failure("provider.worker-v4-input-invalid", "authority", "object"));
			constexpr std::array<std::string_view, 9U> closure_fields{"closure_digest",
																	  "closure_id",
																	  "expected_transfer_digest",
																	  "first_sequence",
																	  "manifest_digest",
																	  "session_id",
																	  "task_id",
																	  "task_v4_digest",
																	  "stream_id"};
			if (auto valid = exact_members(**closure_object, closure_fields, "closure-binding");
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			constexpr std::array<std::string_view, 4U> authority_fields{
				"effective_arguments",
				"logical_working_directory",
				"normalized_invocation_digest",
				"qualified_read_roots"};
			if (auto valid = exact_members(**authority_object, authority_fields, "input-authority");
				!valid)
				return sdk::unexpected(std::move(valid.error()));

			auto closure_digest =
				string_member(**closure_object, "closure_digest", "closure.digest");
			auto closure_id = string_member(**closure_object, "closure_id", "closure.id");
			auto transfer_digest =
				string_member(**closure_object, "expected_transfer_digest", "closure.transfer");
			auto manifest_digest =
				string_member(**closure_object, "manifest_digest", "closure.manifest");
			auto session_id = string_member(**closure_object, "session_id", "closure.session");
			auto task_id = string_member(**closure_object, "task_id", "closure.task");
			auto task_digest =
				string_member(**closure_object, "task_v4_digest", "closure.task-digest");
			auto first_sequence =
				unsigned_member(**closure_object, "first_sequence", "closure.sequence");
			auto binding_stream = unsigned_member(**closure_object, "stream_id", "closure.stream");
			if (!closure_digest || !closure_id || !transfer_digest || !manifest_digest ||
				!session_id || !task_id || !task_digest || !first_sequence || !binding_stream ||
				*first_sequence != 0U || *binding_stream != *stream_id)
				return sdk::unexpected(
					failure("provider.worker-v4-input-invalid", "closure-binding"));

			auto normalized = string_member(
				**authority_object, "normalized_invocation_digest", "input-authority.invocation");
			auto working_directory = string_member(**authority_object,
												   "logical_working_directory",
												   "input-authority.working-directory");
			auto arguments = string_array_member(
				**authority_object, "effective_arguments", "input-authority.arguments");
			auto roots = string_array_member(
				**authority_object, "qualified_read_roots", "input-authority.read-roots");
			if (!normalized || !working_directory || !arguments || !roots)
				return sdk::unexpected(
					failure("provider.worker-v4-input-invalid", "input-authority"));

			const auto* payload_task_id = (*payload_object)->member("task_id");
			const auto* payload_task_digest = (*payload_object)->member("task_v4_digest");
			if (payload_task_id == nullptr || payload_task_digest == nullptr ||
				payload_task_id->as_string() == nullptr ||
				payload_task_digest->as_string() == nullptr ||
				*payload_task_id->as_string() != *task_id ||
				*payload_task_digest->as_string() != *task_digest)
				return sdk::unexpected(
					failure("source-closure.task-binding-mismatch", "task-v4-payload", "identity"));
			const auto payload_text = materialization::canonical_json(**payload_object);
			const auto payload_bytes =
				std::as_bytes(std::span{payload_text.data(), payload_text.size()});
			if (auto valid =
					validate_source_closure_task_v4_input_digest(payload_bytes, *input_digest);
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			const auto base_projection_text =
				materialization::canonical_json(**base_projection_object);
			const auto base_projection_bytes =
				std::as_bytes(std::span{base_projection_text.data(), base_projection_text.size()});
			if (sdk::content_digest(base_projection_bytes) != *base_digest)
				return sdk::unexpected(
					failure("source-closure.task-v4-binding-mismatch", "base-task-projection"));

			return worker_v4_ingress{
				std::vector<std::byte>{payload_bytes.begin(), payload_bytes.end()},
				std::vector<std::byte>{base_projection_bytes.begin(), base_projection_bytes.end()},
				std::move(*base_digest),
				std::move(*input_digest),
				{std::move(*normalized),
				 std::move(*working_directory),
				 std::move(*arguments),
				 std::move(*roots)},
				{std::move(*session_id),
				 std::move(*task_id),
				 std::move(*task_digest),
				 std::move(*closure_id),
				 std::move(*closure_digest),
				 std::move(*manifest_digest),
				 0U},
				std::move(*transfer_digest),
				*stream_id};
		}

		[[nodiscard]] std::optional<std::string> environment(const char* name)
		{
			const auto* value = std::getenv(name);
			return value == nullptr ? std::nullopt : std::optional<std::string>{value};
		}

		class ingress_authority final : public source_closure_task_v4_authority
		{
		  public:
			explicit ingress_authority(const worker_v4_ingress& input) : input_{&input} {}

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
			const worker_v4_ingress* input_;
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
		auto ingress = decode_ingress(std::move(*raw));
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
		{
			return reject("clang-execution");
		}
		return emit_missing_output(output, receipt->task_id);
	}
} // namespace cxxlens::detail::clang22
