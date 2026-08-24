#include "llvm/clang22/installed_materializer_source_closure.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "llvm/clang22/materialization_json.hpp"
#include "llvm/clang22/source_closure.hpp"
#include "llvm/clang22/source_closure_transport.hpp"
#include "protocol_v2/closure.hpp"

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::detail::clang22;
	using namespace cxxlens::detail::clang22::materialization;
	namespace protocol = ::cxxlens::protocol_v2;

	[[nodiscard]] std::string semantic(const char digit)
	{
		return "semantic-v2:sha256:" + std::string(64U, digit);
	}

	[[nodiscard]] std::string content(const char digit)
	{
		return "sha256:" + std::string(64U, digit);
	}

	[[nodiscard]] json_value text(const std::string_view value)
	{
		auto encoded = json_value::string(std::string{value});
		assert(encoded);
		return *encoded;
	}

	[[nodiscard]] json_value object(json_value::object_type fields)
	{
		auto encoded = json_value::object(std::move(fields));
		assert(encoded);
		return *encoded;
	}

	[[nodiscard]] json_value source_json(const source_closure_manifest& manifest)
	{
		const auto& member = manifest.members.front();
		return object({
			{"content_digest", text(member.content_digest)},
			{"encoding", text(member.encoding)},
			{"file_id", text(member.file_id)},
			{"line_index_id", text("line-index:sha256:" + std::string(64U, '4'))},
			{"logical_path", text(member.logical_path)},
			{"read_only", json_value::boolean(true)},
			{"size_bytes", json_value::unsigned_integer(member.size_bytes)},
			{"source_snapshot_id", text("source-snapshot:installed-receiver")},
		});
	}

	[[nodiscard]] source_closure_manifest make_manifest()
	{
		const auto source = std::make_shared<const std::string>("int main() { return 0; }\n");
		auto snapshot = make_source_closure_snapshot({{"project://src/main.cpp",
													   source_closure_role::main,
													   source_closure_encoding::utf8,
													   source}});
		assert(snapshot);
		source_closure_manifest manifest;
		for (const auto& member : snapshot->members)
			manifest.members.push_back({member.file_id,
										member.logical_path,
										"main",
										"utf8",
										member.size_bytes,
										member.content_digest,
										true});
		for (const auto& blob : snapshot->blobs)
			manifest.blobs.push_back({blob.content_digest, blob.size_bytes});
		// The wire closure identity is the same source snapshot identity that the receiver
		// authenticates.  The manifest digest is the independent JSON projection digest.
		manifest.closure_digest = snapshot->closure_digest;
		manifest.closure_id = snapshot->snapshot_id;
		manifest.manifest_digest = *derive_source_closure_manifest_digest(manifest);
		return manifest;
	}

	[[nodiscard]] std::string manifest_json(const source_closure_manifest& manifest)
	{
		std::vector<json_value> members;
		for (const auto& value : manifest.members)
			members.push_back(object({
				{"content_digest", text(value.content_digest)},
				{"encoding", text(value.encoding)},
				{"file_id", text(value.file_id)},
				{"logical_path", text(value.logical_path)},
				{"read_only", json_value::boolean(value.read_only)},
				{"role", text(value.role)},
				{"size_bytes", json_value::unsigned_integer(value.size_bytes)},
			}));
		std::vector<json_value> blobs;
		for (const auto& value : manifest.blobs)
			blobs.push_back(object({
				{"content_digest", text(value.content_digest)},
				{"size_bytes", json_value::unsigned_integer(value.size_bytes)},
			}));
		return canonical_json(object({
			{"blobs", json_value::array(std::move(blobs))},
			{"closure_digest", text(manifest.closure_digest)},
			{"closure_id", text(manifest.closure_id)},
			{"members", json_value::array(std::move(members))},
			{"schema", text(source_closure_manifest_schema)},
		}));
	}

	[[nodiscard]] provider_task_v4_base_task make_base(const source_closure_manifest& manifest,
													   const json_value& base_json)
	{
		const auto& source = manifest.members.front();
		const auto canonical = canonical_json(base_json);
		provider_task_v4_base_task base;
		base.provider_task_id = "task:semantic-v2:sha256:" + std::string(64U, 'a');
		base.provider_execution_id = "provider-execution:installed-receiver";
		base.canonical_base_task_digest =
			sdk::content_digest(std::as_bytes(std::span{canonical.data(), canonical.size()}));
		base.task_input_digest = content('b');
		base.normalized_invocation_digest = semantic('c');
		base.toolchain_digest = semantic('d');
		base.environment_digest = content('e');
		base.working_directory = "project://src";
		base.source = {"source-snapshot:installed-receiver",
					   source.file_id,
					   source.logical_path,
					   source.content_digest,
					   source.size_bytes,
					   source.encoding,
					   "line-index:sha256:" + std::string(64U, '4'),
					   true};
		return base;
	}

	[[nodiscard]] provider_task_v4 make_task(const source_closure_manifest& manifest,
											 const provider_task_v4_base_task& base)
	{
		provider_task_v4 task;
		task.base_task_index = 0U;
		task.base_provider_task_id = base.provider_task_id;
		task.base_task_digest = base.canonical_base_task_digest;
		task.open_task = {base.task_input_digest,
						  base.normalized_invocation_digest,
						  base.toolchain_digest,
						  base.environment_digest};
		task.source_closure = {manifest.closure_id,
							   manifest.closure_digest,
							   manifest.manifest_digest,
							   static_cast<std::uint64_t>(manifest.members.size()),
							   static_cast<std::uint64_t>(manifest.blobs.size()),
							   manifest.blobs.front().size_bytes};
		task.main_logical_path = base.source.logical_path;
		task.logical_working_directory = base.working_directory;
		task.task_v4_digest = *derive_provider_task_v4_digest(task);
		task.task_id = "task:" + task.task_v4_digest;
		assert(validate_provider_task_v4_identity(task));
		return task;
	}

	[[nodiscard]] json_value base_task_json(const source_closure_manifest& manifest)
	{
		return object({
			{"environment_digest", text(content('e'))},
			{"normalized_invocation_digest", text(semantic('c'))},
			{"provider_execution_id", text("provider-execution:installed-receiver")},
			{"provider_task_id", text("task:semantic-v2:sha256:" + std::string(64U, 'a'))},
			{"source", source_json(manifest)},
			{"task_input_digest", text(content('b'))},
			{"toolchain_digest", text(semantic('d'))},
			{"working_directory", text("project://src")},
		});
	}

	[[nodiscard]] json_value summary_json(const source_closure_manifest& manifest)
	{
		return object({
			{"blob_count", json_value::unsigned_integer(manifest.blobs.size())},
			{"manifest_digest", text(manifest.manifest_digest)},
			{"member_count", json_value::unsigned_integer(manifest.members.size())},
			{"source_closure_digest", text(manifest.closure_digest)},
			{"source_closure_id", text(manifest.closure_id)},
			{"unique_blob_bytes", json_value::unsigned_integer(manifest.blobs.front().size_bytes)},
		});
	}

	[[nodiscard]] json_value extension_json(const provider_task_v4& task)
	{
		return object({
			{"base_provider_task_id", text(task.base_provider_task_id)},
			{"base_task_digest", text(task.base_task_digest)},
			{"base_task_index", json_value::unsigned_integer(task.base_task_index)},
			{"logical_working_directory", text(task.logical_working_directory)},
			{"main_logical_path", text(task.main_logical_path)},
			{"open_task",
			 object({
				 {"environment_digest", text(task.open_task.environment_digest)},
				 {"normalized_invocation_digest",
				  text(task.open_task.normalized_invocation_digest)},
				 {"task_input_digest", text(task.open_task.task_input_digest)},
				 {"toolchain_digest", text(task.open_task.toolchain_digest)},
			 })},
			{"schema", text(task.schema)},
			{"source_closure",
			 object({
				 {"digest", text(task.source_closure.source_closure_digest)},
				 {"id", text(task.source_closure.source_closure_id)},
				 {"manifest_digest", text(task.source_closure.manifest_digest)},
			 })},
			{"task_id", text(task.task_id)},
			{"task_v4_digest", text(task.task_v4_digest)},
		});
	}

	struct fixture
	{
		materialization_request_v2_2 request;
		json_value root;
		source_closure_manifest manifest;
		std::string session;
		std::string transfer_digest;
		std::vector<std::byte> transcript;
	};

	[[nodiscard]] fixture make_fixture()
	{
		const auto manifest = make_manifest();
		const auto base_shape = base_task_json(manifest);
		const auto base = make_base(manifest, base_shape);
		const auto task = make_task(manifest, base);
		materialization_request_v2_2 request;
		request.materialization_request_id = "materialization-authority:installed-receiver";
		request.semantic_request_digest = semantic('f');
		request.required_features = materialization_request_v2_2_required_features();
		request.base_tasks = {base};
		request.source_closures = {{manifest.closure_id,
									manifest.closure_digest,
									manifest.manifest_digest,
									1U,
									1U,
									manifest.blobs.front().size_bytes}};
		request.task_extensions = {task};
		std::map<std::string, json_value, utf8_byte_less> inherited;
		for (const auto name : {"engine",
								"group_topology",
								"interpretation_policy",
								"publication",
								"project",
								"registry",
								"tool",
								"trust_policy"})
			inherited.emplace(name, json_value::null());
		inherited.emplace("materialization_request_id", text(request.materialization_request_id));
		inherited.emplace("semantic_request_digest", text(request.semantic_request_digest));
		inherited.emplace("tasks", json_value::array({base_shape}));
		inherited.emplace("worker",
						  object({
							  {"protocol_major", json_value::unsigned_integer(2U)},
							  {"protocol_minor", json_value::unsigned_integer(0U)},
						  }));
		request.inherited_authority = object(std::move(inherited));
		request.request_digest = *derive_materialization_request_v2_2_digest(request);
		request.request_id = "materialization-request:" + request.request_digest;

		const auto base_value = base_shape;
		const auto extension_value = extension_json(task);
		const auto worker_value = object({
			{"protocol_major", json_value::unsigned_integer(2U)},
			{"protocol_minor", json_value::unsigned_integer(0U)},
		});
		std::map<std::string, json_value, utf8_byte_less> root_fields;
		for (const auto name : {"engine",
								"group_topology",
								"interpretation_policy",
								"publication",
								"project",
								"registry",
								"tool",
								"trust_policy"})
			root_fields.emplace(name, json_value::null());
		root_fields.emplace("materialization_request_id", text(request.materialization_request_id));
		root_fields.emplace("semantic_request_digest", text(request.semantic_request_digest));
		root_fields.emplace("worker", worker_value);
		root_fields.emplace("schema", text(std::string{materialization_request_v2_2_schema}));
		root_fields.emplace("request_version",
							text(std::string{materialization_request_v2_2_version}));
		root_fields.emplace("request_digest", text(request.request_digest));
		root_fields.emplace("request_id", text(request.request_id));
		root_fields.emplace(
			"required_features",
			json_value::array({text("task-input-chunks-v2"), text("task-source-closure-v2")}));
		root_fields.emplace("tasks", json_value::array({base_value}));
		root_fields.emplace("source_closures", json_value::array({summary_json(manifest)}));
		root_fields.emplace("task_extensions", json_value::array({extension_value}));

		fixture output{std::move(request),
					   object(std::move(root_fields)),
					   manifest,
					   "provider-session:sha256:" + std::string(64U, '1'),
					   {},
					   {}};
		const auto manifest_bytes = manifest_json(output.manifest);
		const auto source = std::make_shared<const std::string>("int main() { return 0; }\n");
		const auto binding =
			source_closure_transfer_binding{output.session,
											output.request.task_extensions.front().task_id,
											output.request.task_extensions.front().task_v4_digest,
											output.manifest.closure_id,
											output.manifest.closure_digest,
											output.manifest.manifest_digest,
											0U};
		std::uint64_t sequence{};
		auto append = [&](const sdk::provider::message_type type,
						  protocol::closure_control control,
						  const std::span<const std::byte> payload = {})
		{
			auto encoded = protocol::encode_closure_control(
				static_cast<protocol::message_type>(static_cast<std::uint16_t>(type)), control);
			assert(encoded);
			sdk::provider::frame frame;
			frame.type = type;
			frame.stream_id = 1U;
			frame.sequence = sequence++;
			frame.control = std::move(*encoded);
			frame.payload.assign(payload.begin(), payload.end());
			auto wire = sdk::provider::encode_frame(frame);
			assert(wire);
			output.transcript.insert(output.transcript.end(), wire->begin(), wire->end());
		};
		append(sdk::provider::message_type::source_closure_manifest,
			   protocol::source_closure_manifest_descriptor{protocol::manifest_kind::descriptor,
															output.session,
															binding.task_id,
															binding.task_v4_digest,
															binding.closure_id,
															binding.closure_digest,
															binding.manifest_digest,
															manifest_bytes.size(),
															manifest_bytes.size(),
															1U});
		append(sdk::provider::message_type::source_closure_manifest,
			   protocol::source_closure_manifest_chunk{protocol::manifest_kind::chunk,
													   output.session,
													   binding.task_id,
													   binding.manifest_digest,
													   0U,
													   0U,
													   manifest_bytes.size()},
			   std::as_bytes(std::span{manifest_bytes.data(), manifest_bytes.size()}));
		append(
			sdk::provider::message_type::source_closure_blob,
			protocol::source_closure_blob_descriptor{output.session,
													 binding.task_id,
													 binding.closure_digest,
													 0U,
													 output.manifest.blobs.front().content_digest,
													 output.manifest.blobs.front().size_bytes,
													 output.manifest.blobs.front().size_bytes,
													 1U});
		append(sdk::provider::message_type::source_closure_chunk,
			   protocol::source_closure_chunk{output.session,
											  binding.task_id,
											  0U,
											  output.manifest.blobs.front().content_digest,
											  0U,
											  0U,
											  output.manifest.blobs.front().size_bytes},
			   std::as_bytes(std::span{source->data(), source->size()}));
		const std::array receipts{
			source_closure_blob_receipt{0U,
										output.manifest.blobs.front().content_digest,
										output.manifest.blobs.front().size_bytes}};
		const auto receipts_digest = *source_closure_blob_receipts_digest(receipts);
		output.transfer_digest = *source_closure_transfer_digest(
			binding, receipts_digest, 1U, output.manifest.blobs.front().size_bytes);
		append(sdk::provider::message_type::source_closure_seal,
			   protocol::source_closure_seal{output.session,
											 binding.task_id,
											 binding.task_v4_digest,
											 binding.manifest_digest,
											 receipts_digest,
											 1U,
											 output.manifest.blobs.front().size_bytes,
											 binding.closure_digest,
											 output.transfer_digest});
		return output;
	}

	struct socket_channel_endpoints
	{
		int child_read{-1};
		int host_write{-1};
		int child_write{-1};
		int host_read{-1};

		socket_channel_endpoints()
		{
			int input[2]{};
			int ack[2]{};
			if (::socketpair(AF_UNIX, SOCK_STREAM, 0, input) != 0)
				std::abort();
			if (::socketpair(AF_UNIX, SOCK_STREAM, 0, ack) != 0)
			{
				(void)::close(input[0]);
				(void)::close(input[1]);
				std::abort();
			}
			child_read = input[0];
			host_write = input[1];
			child_write = ack[0];
			host_read = ack[1];
			prepare_child_endpoint(child_read);
			prepare_child_endpoint(child_write);
		}

		socket_channel_endpoints(const socket_channel_endpoints&) = delete;
		socket_channel_endpoints& operator=(const socket_channel_endpoints&) = delete;

		~socket_channel_endpoints()
		{
			for (const auto descriptor : {child_read, host_write, child_write, host_read})
			{
				if (descriptor >= 0)
					(void)::close(descriptor);
			}
		}

	  private:
		static void prepare_child_endpoint(int& descriptor)
		{
			if (descriptor < 4)
			{
				const auto inherited = ::fcntl(descriptor, F_DUPFD, 4);
				if (inherited < 4)
					std::abort();
				const auto closed = ::close(descriptor);
				if (closed != 0)
				{
					(void)::close(inherited);
					std::abort();
				}
				descriptor = inherited;
			}
			const auto status_flags = ::fcntl(descriptor, F_GETFL);
			if (status_flags < 0 || ::fcntl(descriptor, F_SETFL, status_flags | O_NONBLOCK) != 0)
				std::abort();
			const auto descriptor_flags = ::fcntl(descriptor, F_GETFD);
			if (descriptor_flags < 0 ||
				::fcntl(descriptor, F_SETFD, descriptor_flags & ~FD_CLOEXEC) != 0)
				std::abort();
		}
	};

	void set_channel_environment(const fixture& value,
								 const int read_descriptor,
								 const int write_descriptor)
	{
		setenv("CXXLENS_PROVIDER_INGRESS_MODE", "task-v4-source-closure-v2", 1);
		setenv(
			"CXXLENS_PROVIDER_SOURCE_CLOSURE_READ_FD", std::to_string(read_descriptor).c_str(), 1);
		setenv("CXXLENS_PROVIDER_SOURCE_CLOSURE_WRITE_FD",
			   std::to_string(write_descriptor).c_str(),
			   1);
		setenv("CXXLENS_PROVIDER_SOURCE_CLOSURE_SESSION_ID", value.session.c_str(), 1);
		setenv("CXXLENS_PROVIDER_SOURCE_CLOSURE_TASK_ID",
			   value.request.task_extensions.front().task_id.c_str(),
			   1);
		setenv("CXXLENS_PROVIDER_SOURCE_CLOSURE_TASK_V4_DIGEST",
			   value.request.task_extensions.front().task_v4_digest.c_str(),
			   1);
		setenv("CXXLENS_PROVIDER_SOURCE_CLOSURE_ID", value.manifest.closure_id.c_str(), 1);
		setenv("CXXLENS_PROVIDER_SOURCE_CLOSURE_DIGEST", value.manifest.closure_digest.c_str(), 1);
		setenv("CXXLENS_PROVIDER_SOURCE_CLOSURE_TRANSFER_DIGEST", value.transfer_digest.c_str(), 1);
	}

	void clear_channel_environment()
	{
		for (const auto name : {"CXXLENS_PROVIDER_INGRESS_MODE",
								"CXXLENS_PROVIDER_SOURCE_CLOSURE_READ_FD",
								"CXXLENS_PROVIDER_SOURCE_CLOSURE_WRITE_FD",
								"CXXLENS_PROVIDER_SOURCE_CLOSURE_SESSION_ID",
								"CXXLENS_PROVIDER_SOURCE_CLOSURE_TASK_ID",
								"CXXLENS_PROVIDER_SOURCE_CLOSURE_TASK_V4_DIGEST",
								"CXXLENS_PROVIDER_SOURCE_CLOSURE_ID",
								"CXXLENS_PROVIDER_SOURCE_CLOSURE_DIGEST",
								"CXXLENS_PROVIDER_SOURCE_CLOSURE_TRANSFER_DIGEST"})
			unsetenv(name);
	}

	void write_all(const int descriptor, const std::span<const std::byte> bytes)
	{
		std::size_t offset{};
		while (offset < bytes.size())
		{
			const auto count = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
			assert(count > 0);
			offset += static_cast<std::size_t>(count);
		}
	}

	void positive_fd_receiver()
	{
		auto value = make_fixture();
		socket_channel_endpoints channel;
		set_channel_environment(value, channel.child_read, channel.child_write);
		write_all(channel.host_write, value.transcript);
		auto received = receive_installed_materializer_source_closure(value.root);
		if (!received)
		{
			std::cerr << received.error().code << ':' << received.error().field << ':'
					  << received.error().detail << '\n';
			std::abort();
		}
		if (received->request.request.request_digest != value.request.request_digest ||
			received->receiver.snapshot.snapshot_id != value.manifest.closure_id)
			std::abort();
		std::array<std::byte, 4096U> ack_bytes{};
		const auto ack_size = ::read(channel.host_read, ack_bytes.data(), ack_bytes.size());
		if (ack_size <= 0)
			std::abort();
		clear_channel_environment();
	}

	void disconnected_is_explicit()
	{
		clear_channel_environment();
		auto result = receive_installed_materializer_source_closure(json_value::null());
		if (result || result.error().code != "source-closure.channel-required")
			std::abort();
	}

	void duplicate_channel_custody_is_rejected()
	{
		auto value = make_fixture();
		socket_channel_endpoints channel;
		set_channel_environment(value, channel.child_read, channel.child_read);
		auto result = receive_installed_materializer_source_closure(value.root);
		if (result || result.error().code != "source-closure.channel-invalid" ||
			result.error().field != "descriptor" || result.error().detail != "duplicate")
			std::abort();
		clear_channel_environment();
	}

	void foreign_task_binding_is_rejected()
	{
		auto value = make_fixture();
		socket_channel_endpoints channel;
		set_channel_environment(value, channel.child_read, channel.child_write);
		const auto foreign_task = "task:semantic-v2:sha256:" + std::string(64U, 'f');
		setenv("CXXLENS_PROVIDER_SOURCE_CLOSURE_TASK_ID", foreign_task.c_str(), 1);
		auto result = receive_installed_materializer_source_closure(value.root);
		if (result || result.error().code != "source-closure.task-binding-mismatch")
			std::abort();
		clear_channel_environment();
	}
} // namespace

int main()
{
	disconnected_is_explicit();
	duplicate_channel_custody_is_rejected();
	foreign_task_binding_is_rejected();
	positive_fd_receiver();
	return 0;
}
