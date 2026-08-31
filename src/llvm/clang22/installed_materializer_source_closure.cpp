#include "installed_materializer_source_closure.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "source_closure_fd.hpp"

namespace cxxlens::detail::clang22
{
	namespace
	{
		using materialization::json_value;

		[[nodiscard]] sdk::error
		failure(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::error invalid(std::string field, std::string detail = {})
		{
			return failure(
				"materialization.request-v2_2-invalid", std::move(field), std::move(detail));
		}

		[[nodiscard]] sdk::result<const json_value*>
		member(const json_value& object, const std::string_view name, const std::string_view field)
		{
			if (object.as_object() == nullptr)
				return sdk::unexpected(invalid(std::string{field}, "object-required"));
			const auto* value = object.member(name);
			if (value == nullptr)
				return sdk::unexpected(invalid(std::string{field}, "missing:" + std::string{name}));
			return value;
		}

		[[nodiscard]] sdk::result<std::string> string_member(const json_value& object,
															 const std::string_view name,
															 const std::string_view field)
		{
			auto value = member(object, name, field);
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			if (const auto* text = (*value)->as_string(); text != nullptr)
				return *text;
			return sdk::unexpected(invalid(std::string{field}, "string-required"));
		}

		[[nodiscard]] sdk::result<std::uint64_t> unsigned_member(const json_value& object,
																 const std::string_view name,
																 const std::string_view field)
		{
			auto value = member(object, name, field);
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			if (const auto* unsigned_value = (*value)->as_unsigned_integer();
				unsigned_value != nullptr)
				return *unsigned_value;
			if (const auto* signed_value = (*value)->as_signed_integer();
				signed_value != nullptr && *signed_value >= 0)
				return static_cast<std::uint64_t>(*signed_value);
			return sdk::unexpected(invalid(std::string{field}, "unsigned-integer-required"));
		}

		[[nodiscard]] sdk::result<bool> boolean_member(const json_value& object,
													   const std::string_view name,
													   const std::string_view field)
		{
			auto value = member(object, name, field);
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			if (const auto* boolean = (*value)->as_boolean(); boolean != nullptr)
				return *boolean;
			return sdk::unexpected(invalid(std::string{field}, "boolean-required"));
		}

		[[nodiscard]] sdk::result<std::vector<std::string>> string_array_member(
			const json_value& object, const std::string_view name, const std::string_view field)
		{
			auto value = member(object, name, field);
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			const auto* array = (*value)->as_array();
			if (array == nullptr || array->empty() || array->size() > 4096U)
				return sdk::unexpected(invalid(std::string{field}, "array-bound"));
			std::vector<std::string> output;
			output.reserve(array->size());
			for (const auto& item : *array)
			{
				const auto* text = item.as_string();
				if (text == nullptr || text->empty() || text->size() > 4096U ||
					text->contains('\0'))
					return sdk::unexpected(invalid(std::string{field}, "string-bound"));
				output.push_back(*text);
			}
			return output;
		}

		[[nodiscard]] sdk::result<std::string> base_task_digest(const json_value& task)
		{
			constexpr std::array<std::string_view, 8U> fields{"environment_digest",
															  "normalized_invocation_digest",
															  "provider_execution_id",
															  "provider_task_id",
															  "source",
															  "task_input_digest",
															  "toolchain_digest",
															  "working_directory"};
			json_value::object_type projection;
			for (const auto name : fields)
			{
				const auto* value = task.member(name);
				if (value == nullptr)
					return sdk::unexpected(invalid("tasks", "missing:" + std::string{name}));
				projection.emplace(std::string{name}, *value);
			}
			auto encoded = json_value::object(std::move(projection));
			if (!encoded)
				return sdk::unexpected(std::move(encoded.error()));
			const auto canonical = materialization::canonical_json(*encoded);
			return sdk::content_digest(
				std::as_bytes(std::span{canonical.data(), canonical.size()}));
		}

		[[nodiscard]] sdk::result<provider_task_v4_source> parse_source(const json_value& task)
		{
			auto value = member(task, "source", "tasks.source");
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			auto snapshot = string_member(**value, "source_snapshot_id", "source.snapshot");
			auto file = string_member(**value, "file_id", "source.file");
			auto path = string_member(**value, "logical_path", "source.path");
			auto digest = string_member(**value, "content_digest", "source.content");
			auto size = unsigned_member(**value, "size_bytes", "source.size");
			auto encoding = string_member(**value, "encoding", "source.encoding");
			auto line_index = string_member(**value, "line_index_id", "source.line-index");
			auto read_only = boolean_member(**value, "read_only", "source.read-only");
			if (!snapshot || !file || !path || !digest || !size || !encoding || !line_index ||
				!read_only)
				return sdk::unexpected(invalid("tasks.source", "field"));
			return provider_task_v4_source{std::move(*snapshot),
										   std::move(*file),
										   std::move(*path),
										   std::move(*digest),
										   *size,
										   std::move(*encoding),
										   std::move(*line_index),
										   *read_only};
		}

		[[nodiscard]] sdk::result<provider_task_v4_base_task>
		parse_base_task(const json_value& task)
		{
			auto provider_id = string_member(task, "provider_task_id", "tasks.provider_task_id");
			auto execution_id =
				string_member(task, "provider_execution_id", "tasks.provider_execution_id");
			auto input_digest = string_member(task, "task_input_digest", "tasks.task-input");
			auto invocation =
				string_member(task, "normalized_invocation_digest", "tasks.invocation");
			auto toolchain = string_member(task, "toolchain_digest", "tasks.toolchain");
			auto environment = string_member(task, "environment_digest", "tasks.environment");
			auto workdir = string_member(task, "working_directory", "tasks.working-directory");
			auto source = parse_source(task);
			auto digest = base_task_digest(task);
			if (!provider_id || !execution_id || !input_digest || !invocation || !toolchain ||
				!environment || !workdir || !source || !digest)
				return sdk::unexpected(invalid("tasks", "field"));
			return provider_task_v4_base_task{std::move(*provider_id),
											  std::move(*execution_id),
											  std::move(*digest),
											  std::move(*input_digest),
											  std::move(*invocation),
											  std::move(*toolchain),
											  std::move(*environment),
											  std::move(*workdir),
											  std::move(*source)};
		}

		[[nodiscard]] sdk::result<source_closure_summary>
		parse_closure_summary(const json_value& value)
		{
			auto id = string_member(value, "source_closure_id", "source-closure.id");
			auto digest = string_member(value, "source_closure_digest", "source-closure.digest");
			auto manifest = string_member(value, "manifest_digest", "source-closure.manifest");
			auto members = unsigned_member(value, "member_count", "source-closure.members");
			auto blobs = unsigned_member(value, "blob_count", "source-closure.blobs");
			auto bytes = unsigned_member(value, "unique_blob_bytes", "source-closure.bytes");
			if (!id || !digest || !manifest || !members || !blobs || !bytes)
				return sdk::unexpected(invalid("source_closures", "field"));
			return source_closure_summary{
				std::move(*id), std::move(*digest), std::move(*manifest), *members, *blobs, *bytes};
		}

		[[nodiscard]] sdk::result<provider_task_v4_open_task>
		parse_open_task(const json_value& value)
		{
			auto input = string_member(value, "task_input_digest", "task.open-task.input");
			auto invocation =
				string_member(value, "normalized_invocation_digest", "task.open-task.invocation");
			auto toolchain = string_member(value, "toolchain_digest", "task.open-task.toolchain");
			auto environment =
				string_member(value, "environment_digest", "task.open-task.environment");
			if (!input || !invocation || !toolchain || !environment)
				return sdk::unexpected(invalid("task_extensions.open_task", "field"));
			return provider_task_v4_open_task{std::move(*input),
											  std::move(*invocation),
											  std::move(*toolchain),
											  std::move(*environment)};
		}

		[[nodiscard]] sdk::result<provider_task_v4> parse_task_extension(const json_value& value)
		{
			auto schema = string_member(value, "schema", "task.schema");
			auto id = string_member(value, "task_id", "task.id");
			auto digest = string_member(value, "task_v4_digest", "task.digest");
			auto index = unsigned_member(value, "base_task_index", "task.index");
			auto provider_id = string_member(value, "base_provider_task_id", "task.provider-id");
			auto base_digest = string_member(value, "base_task_digest", "task.base-digest");
			auto open_value = member(value, "open_task", "task.open-task");
			auto closure_value = member(value, "source_closure", "task.source-closure");
			auto main_path = string_member(value, "main_logical_path", "task.main-path");
			auto workdir = string_member(value, "logical_working_directory", "task.workdir");
			if (!schema || !id || !digest || !index || !provider_id || !base_digest ||
				!open_value || !closure_value || !main_path || !workdir)
				return sdk::unexpected(invalid("task_extensions", "field"));
			auto open = parse_open_task(**open_value);
			auto closure_id = string_member(**closure_value, "id", "task.source-closure.id");
			auto closure_digest =
				string_member(**closure_value, "digest", "task.source-closure.digest");
			auto manifest =
				string_member(**closure_value, "manifest_digest", "task.source-closure.manifest");
			if (!open || !closure_id || !closure_digest || !manifest)
				return sdk::unexpected(invalid("task_extensions", "nested"));
			return provider_task_v4{std::move(*schema),
									std::move(*id),
									std::move(*digest),
									*index,
									std::move(*provider_id),
									std::move(*base_digest),
									std::move(*open),
									source_closure_summary{std::move(*closure_id),
														   std::move(*closure_digest),
														   std::move(*manifest),
														   0U,
														   0U,
														   0U},
									std::move(*main_path),
									std::move(*workdir)};
		}

		[[nodiscard]] sdk::result<materialization::materialization_request_v2_2>
		parse_request(const json_value& root)
		{
			auto schema = string_member(root, "schema", "request.schema");
			auto version = string_member(root, "request_version", "request.version");
			auto request_id = string_member(root, "request_id", "request.id");
			auto request_digest = string_member(root, "request_digest", "request.digest");
			auto materialization_id =
				string_member(root, "materialization_request_id", "request.materialization-id");
			auto semantic_digest =
				string_member(root, "semantic_request_digest", "request.semantic-digest");
			auto features = string_array_member(root, "required_features", "required_features");
			if (!schema || !version || !request_id || !request_digest || !materialization_id ||
				!semantic_digest || !features)
				return sdk::unexpected(invalid("request", "field"));

			auto worker_value = member(root, "worker", "request.worker");
			if (!worker_value)
				return sdk::unexpected(std::move(worker_value.error()));
			auto major = unsigned_member(**worker_value, "protocol_major", "worker.protocol-major");
			auto minor = unsigned_member(**worker_value, "protocol_minor", "worker.protocol-minor");
			if (!major || !minor || *major > std::numeric_limits<std::uint16_t>::max() ||
				*minor > std::numeric_limits<std::uint16_t>::max())
				return sdk::unexpected(invalid("worker.protocol", "range"));

			auto tasks_value = member(root, "tasks", "request.tasks");
			auto closures_value = member(root, "source_closures", "request.source-closures");
			auto extensions_value = member(root, "task_extensions", "request.task-extensions");
			if (!tasks_value || !closures_value || !extensions_value ||
				(*tasks_value)->as_array() == nullptr || (*closures_value)->as_array() == nullptr ||
				(*extensions_value)->as_array() == nullptr ||
				(*tasks_value)->as_array()->size() != (*extensions_value)->as_array()->size())
				return sdk::unexpected(invalid("request", "task-census"));

			std::vector<provider_task_v4_base_task> base_tasks;
			base_tasks.reserve((*tasks_value)->as_array()->size());
			for (const auto& task : *(*tasks_value)->as_array())
			{
				auto parsed = parse_base_task(task);
				if (!parsed)
					return sdk::unexpected(std::move(parsed.error()));
				base_tasks.push_back(std::move(*parsed));
			}
			std::vector<source_closure_summary> source_closures;
			source_closures.reserve((*closures_value)->as_array()->size());
			for (const auto& closure : *(*closures_value)->as_array())
			{
				auto parsed = parse_closure_summary(closure);
				if (!parsed)
					return sdk::unexpected(std::move(parsed.error()));
				source_closures.push_back(std::move(*parsed));
			}
			std::vector<provider_task_v4> task_extensions;
			task_extensions.reserve((*extensions_value)->as_array()->size());
			for (const auto& extension : *(*extensions_value)->as_array())
			{
				auto parsed = parse_task_extension(extension);
				if (!parsed)
					return sdk::unexpected(std::move(parsed.error()));
				task_extensions.push_back(std::move(*parsed));
			}
			for (auto& task : task_extensions)
			{
				const auto found = std::ranges::find_if(
					source_closures,
					[&](const auto& closure)
					{
						return closure.source_closure_id == task.source_closure.source_closure_id;
					});
				if (found == source_closures.end() ||
					found->source_closure_digest != task.source_closure.source_closure_digest ||
					found->manifest_digest != task.source_closure.manifest_digest)
					return sdk::unexpected(
						invalid("task_extensions.source_closure", "summary-binding"));
				task.source_closure = *found;
			}

			constexpr std::array<std::string_view, 12U> inherited_names{
				"engine",
				"group_topology",
				"interpretation_policy",
				"materialization_request_id",
				"publication",
				"project",
				"registry",
				"semantic_request_digest",
				"tasks",
				"tool",
				"trust_policy",
				"worker"};
			json_value::object_type inherited;
			for (const auto name : inherited_names)
			{
				const auto* value = root.member(name);
				if (value == nullptr)
					return sdk::unexpected(
						invalid("inherited_authority", "missing:" + std::string{name}));
				inherited.emplace(std::string{name}, *value);
			}
			auto inherited_value = json_value::object(std::move(inherited));
			if (!inherited_value)
				return sdk::unexpected(std::move(inherited_value.error()));

			materialization::materialization_request_v2_2 request;
			request.schema = std::move(*schema);
			request.request_version = std::move(*version);
			request.protocol_major = static_cast<std::uint16_t>(*major);
			request.protocol_minor = static_cast<std::uint16_t>(*minor);
			request.request_id = std::move(*request_id);
			request.request_digest = std::move(*request_digest);
			request.required_features = std::move(*features);
			request.materialization_request_id = std::move(*materialization_id);
			request.semantic_request_digest = std::move(*semantic_digest);
			request.inherited_authority = std::move(*inherited_value);
			request.base_tasks = std::move(base_tasks);
			request.source_closures = std::move(source_closures);
			request.task_extensions = std::move(task_extensions);
			return request;
		}

		struct channel_environment
		{
			int read_descriptor{-1};
			int write_descriptor{-1};
			std::string session_id;
			std::string task_id;
			std::string task_v4_digest;
			std::string closure_id;
			std::string closure_digest;
			std::string manifest_digest;
			std::string transfer_digest;
			std::uint64_t stream_id{};
			std::uint64_t first_sequence{};
		};

		[[nodiscard]] std::optional<std::string> environment(const char* name)
		{
			const auto* value = std::getenv(name);
			return value == nullptr ? std::nullopt : std::optional<std::string>{value};
		}

		[[nodiscard]] sdk::result<int> descriptor_environment(const char* name)
		{
			auto value = environment(name);
			if (!value || value->empty())
				return sdk::unexpected(failure("source-closure.channel-invalid", name, "missing"));
			int descriptor{};
			const auto [end, error] =
				std::from_chars(value->data(), value->data() + value->size(), descriptor);
			if (error != std::errc{} || end != value->data() + value->size() || descriptor < 4)
				return sdk::unexpected(
					failure("source-closure.channel-invalid", name, "descriptor"));
			return descriptor;
		}

		[[nodiscard]] sdk::result<channel_environment> channel_from_environment()
		{
			const auto mode = environment("CXXLENS_PROVIDER_INGRESS_MODE");
			if (!mode)
				return sdk::unexpected(failure(
					"source-closure.channel-required", "environment", "ingress-mode-missing"));
			if (*mode != "task-v4-source-closure-v2")
				return sdk::unexpected(
					failure("source-closure.channel-invalid", "environment", "ingress-mode"));
			auto read = descriptor_environment("CXXLENS_PROVIDER_SOURCE_CLOSURE_READ_FD");
			auto write = descriptor_environment("CXXLENS_PROVIDER_SOURCE_CLOSURE_WRITE_FD");
			const auto session = environment("CXXLENS_PROVIDER_SOURCE_CLOSURE_SESSION_ID");
			const auto task = environment("CXXLENS_PROVIDER_SOURCE_CLOSURE_TASK_ID");
			const auto task_digest = environment("CXXLENS_PROVIDER_SOURCE_CLOSURE_TASK_V4_DIGEST");
			const auto closure = environment("CXXLENS_PROVIDER_SOURCE_CLOSURE_ID");
			const auto closure_digest = environment("CXXLENS_PROVIDER_SOURCE_CLOSURE_DIGEST");
			const auto manifest_digest =
				environment("CXXLENS_PROVIDER_SOURCE_CLOSURE_MANIFEST_DIGEST");
			const auto transfer = environment("CXXLENS_PROVIDER_SOURCE_CLOSURE_TRANSFER_DIGEST");
			const auto stream = environment("CXXLENS_PROVIDER_SOURCE_CLOSURE_STREAM_ID");
			const auto first = environment("CXXLENS_PROVIDER_SOURCE_CLOSURE_FIRST_SEQUENCE");
			if (!read || !write || !session || !task || !task_digest || !closure ||
				!closure_digest || !manifest_digest || !transfer || !stream || !first ||
				session->empty() || task->empty() || task_digest->empty() || closure->empty() ||
				closure_digest->empty() || manifest_digest->empty() || transfer->empty() ||
				stream->empty() || first->empty())
				return sdk::unexpected(
					failure("source-closure.channel-invalid", "environment", "binding-missing"));
			std::uint64_t stream_id{};
			std::uint64_t first_sequence{};
			const auto stream_result =
				std::from_chars(stream->data(), stream->data() + stream->size(), stream_id);
			const auto first_result =
				std::from_chars(first->data(), first->data() + first->size(), first_sequence);
			if (stream_result.ec != std::errc{} ||
				stream_result.ptr != stream->data() + stream->size() ||
				first_result.ec != std::errc{} ||
				first_result.ptr != first->data() + first->size() || stream_id == 0U)
				return sdk::unexpected(
					failure("source-closure.channel-invalid", "environment", "sequence"));
			return channel_environment{*read,
									   *write,
									   *session,
									   *task,
									   *task_digest,
									   *closure,
									   *closure_digest,
									   *manifest_digest,
									   *transfer,
									   stream_id,
									   first_sequence};
		}

		class request_authority final : public source_closure_task_v4_authority
		{
		  public:
			request_authority(
				const materialization::validated_materialization_request_v2_2& request,
				const std::size_t task_index)
				: request_{&request}, task_index_{task_index}
			{
			}

			[[nodiscard]] std::string_view task_id() const noexcept override
			{
				return request_->request.task_extensions[task_index_].task_id;
			}

			[[nodiscard]] std::string_view task_v4_digest() const noexcept override
			{
				return request_->request.task_extensions[task_index_].task_v4_digest;
			}

			[[nodiscard]] sdk::result<void> revalidate() const override
			{
				auto checked = materialization::validate_materialization_request_v2_2(
					request_->request, request_->negotiated_features);
				if (!checked)
					return sdk::unexpected(std::move(checked.error()));
				if (task_index_ >= checked->request.task_extensions.size())
					return sdk::unexpected(failure(
						"source-closure.task-binding-mismatch", "task-index", "out-of-range"));
				return {};
			}

		  private:
			const materialization::validated_materialization_request_v2_2* request_;
			std::size_t task_index_;
		};
	} // namespace

	sdk::result<installed_materializer_source_closure_result>
	receive_installed_materializer_source_closure(const materialization::json_value& request_root)
	{
		auto channel = channel_from_environment();
		if (!channel)
			return sdk::unexpected(std::move(channel.error()));

		auto request = parse_request(request_root);
		if (!request)
			return sdk::unexpected(std::move(request.error()));
		auto advertised_features = request->required_features;
		auto validated = materialization::validate_materialization_request_v2_2(
			std::move(*request), advertised_features);
		if (!validated)
			return sdk::unexpected(std::move(validated.error()));

		const auto found = std::ranges::find_if(
			validated->request.task_extensions,
			[&](const auto& task)
			{
				return task.task_id == channel->task_id &&
					task.task_v4_digest == channel->task_v4_digest &&
					task.source_closure.source_closure_id == channel->closure_id &&
					task.source_closure.source_closure_digest == channel->closure_digest;
			});
		if (found == validated->request.task_extensions.end())
			return sdk::unexpected(failure(
				"source-closure.task-binding-mismatch", "environment", "request-task-closure"));
		if (found->source_closure.manifest_digest != channel->manifest_digest)
			return sdk::unexpected(
				failure("source-closure.digest-mismatch", "manifest_digest", "environment"));
		const auto task_index =
			static_cast<std::size_t>(found - validated->request.task_extensions.begin());
		source_closure_transfer_binding binding{channel->session_id,
												channel->task_id,
												channel->task_v4_digest,
												channel->closure_id,
												channel->closure_digest,
												found->source_closure.manifest_digest,
												channel->first_sequence};
		auto result_binding = binding;
		request_authority authority{*validated, task_index};
		auto fd_channel = source_closure_fd_channel::create(
			{{channel->read_descriptor, source_closure_fd_ownership::borrowed},
			 {channel->write_descriptor, source_closure_fd_ownership::borrowed},
			 {}});
		if (!fd_channel)
			return sdk::unexpected(std::move(fd_channel.error()));
		auto received = receive_source_closure_frames(
			*fd_channel,
			*fd_channel,
			{std::move(binding), &authority, 1U, 16'384U, {}, std::stop_token{}});
		if (!received)
			return sdk::unexpected(std::move(received.error()));
		if (received->credentials.transfer_digest != channel->transfer_digest)
			return sdk::unexpected(
				failure("source-closure.digest-mismatch", "transfer_digest", "environment"));
		return installed_materializer_source_closure_result{
			std::move(*validated), std::move(result_binding), std::move(*received)};
	}
} // namespace cxxlens::detail::clang22
