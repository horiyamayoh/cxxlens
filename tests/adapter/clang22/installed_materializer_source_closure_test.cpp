#include "llvm/clang22/installed_materializer_source_closure.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/relations/build_compile_unit.hpp>
#include <cxxlens/relations/build_project.hpp>
#include <cxxlens/relations/build_toolchain_context.hpp>
#include <cxxlens/relations/build_variant.hpp>
#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_entity.hpp>
#include <cxxlens/relations/source_file.hpp>
#include <cxxlens/relations/source_span.hpp>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "llvm/clang22/materialization_json.hpp"
#include "llvm/clang22/materializer_worker_bridge.hpp"
#include "llvm/clang22/observation_v2.hpp"
#include "llvm/clang22/source_closure.hpp"
#include "llvm/clang22/source_closure_transport.hpp"
#include "materialization_request_v2_2_fixture.hpp"
#include "protocol_v2/closure.hpp"

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::detail::clang22;
	using namespace cxxlens::detail::clang22::materialization;
	namespace protocol = ::cxxlens::protocol_v2;

	class conformance_provider_trust_issuer final : public provider_trust_issuer_port
	{
	  public:
		[[nodiscard]] sdk::result<provider_trust_issuance>
		issue(const sdk::provider::manifest& manifest,
			  const std::string_view measured_binary_digest,
			  const provider_task_v4_trust_authority& policy) override
		{
			const auto normalized_provider_id = [&]
			{
				auto value = policy.provider_id;
				std::ranges::replace(value, ':', '.');
				return value;
			}();
			if (manifest.provider_id != normalized_provider_id ||
				manifest.provider_version != policy.provider_version ||
				manifest.provider_binary_digest != measured_binary_digest ||
				manifest.provider_semantic_contract_digest !=
					policy.semantic_contract_digest.substr(std::string_view{"semantic-v2:"}.size()))
				return sdk::unexpected(sdk::error{
					"security.certificate-subject-mismatch", "provider", "conformance-subject"});
			auto subject = provider_trust_subject_digest(manifest.provider_id,
														 manifest.provider_version,
														 measured_binary_digest,
														 policy.required_qualification);
			if (!subject)
				return sdk::unexpected(std::move(subject.error()));
			return provider_trust_issuance{true,
										   true,
										   {policy.required_qualification},
										   std::move(*subject),
										   "cxxlens.conformance-issuer.v1",
										   "certificate:conformance-clang22",
										   "1"};
		}
	};

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

	[[nodiscard]] sdk::relation_engine runtime_engine()
	{
		sdk::relation_registry registry;
		const std::array<const sdk::relation_descriptor*, 12U> descriptors{
			&build::relations::project::descriptor(),
			&build::relations::toolchain_context::descriptor(),
			&build::relations::variant::descriptor(),
			&build::relations::compile_unit::descriptor(),
			&source::relations::file::descriptor(),
			&source::relations::span::descriptor(),
			&cc::relations::call_direct_target::descriptor(),
			&cc::relations::call_site::descriptor(),
			&cc::relations::entity::descriptor(),
			&materialization::call_observation_v2_descriptor(),
			&materialization::entity_observation_v2_descriptor(),
			&materialization::type_observation_v2_descriptor(),
		};
		for (const auto* descriptor : descriptors)
		{
			auto added = registry.add(*descriptor);
			assert(added);
		}
		auto engine = registry.build("installed-materializer-source-closure-test");
		assert(engine);
		return std::move(*engine);
	}

	void bind_runtime_engine_authority(provider_task_v4_request_authority& authority)
	{
		auto engine = runtime_engine();
		const auto descriptors = engine.descriptors();
		authority.engine.admitted_descriptors.clear();
		for (const auto id : task_v4_engine_descriptor_ids)
		{
			const auto found = std::ranges::find_if(descriptors,
													[id](const auto& descriptor)
													{
														return descriptor.id == id;
													});
			assert(found != descriptors.end());
			authority.engine.admitted_descriptors.push_back({found->id, found->descriptor_digest});
		}
		authority.engine.engine_registry_digest = std::string{engine.registry_digest()};
		authority.publication.selector.relation_registry_digest =
			authority.engine.engine_registry_digest;
		authority.publication.series_id = authority.publication.selector.id();
	}

	[[nodiscard]] fixture make_fixture()
	{
		const auto manifest = make_manifest();
		auto root = cxxlens_test_materialization_request_v2_2_complete_document();
		assert(root.as_object() != nullptr);
		auto authority_seed = decode_provider_task_v4_request_authority(root);
		assert(authority_seed && authority_seed->tasks.size() == 1U);
		const auto& member = manifest.members.front();
		const auto& seed_unit = authority_seed->project.catalog.compile_units.front();
		auto catalog =
			sdk::project_catalog::make(authority_seed->project.catalog.logical_root,
									   authority_seed->project.catalog.environment_digest,
									   {{seed_unit.compile_unit_id,
										 seed_unit.effective_invocation_digest,
										 member.content_digest,
										 seed_unit.environment_digest}});
		assert(catalog);
		{
			auto root_fields = *root.as_object();
			auto project_fields = *root_fields.at("project").as_object();
			project_fields.insert_or_assign("catalog_id", text(catalog->catalog_id));
			project_fields.insert_or_assign("catalog_digest", text(catalog->catalog_digest));
			auto units = *project_fields.at("catalog_compile_units").as_array();
			assert(units.size() == 1U);
			auto unit_fields = *units.front().as_object();
			unit_fields.insert_or_assign("source_digest", text(member.content_digest));
			units.front() = object(std::move(unit_fields));
			project_fields.insert_or_assign("catalog_compile_units",
											json_value::array(std::move(units)));
			root_fields.insert_or_assign("project", object(std::move(project_fields)));

			auto tasks = *root_fields.at("tasks").as_array();
			auto task_fields = *tasks.front().as_object();
			task_fields.insert_or_assign("catalog_id", text(catalog->catalog_id));
			task_fields.insert_or_assign("catalog_digest", text(catalog->catalog_digest));
			auto source_fields = *task_fields.at("source").as_object();
			source_fields.insert_or_assign("source_snapshot_id", text(manifest.closure_id));
			source_fields.insert_or_assign("file_id", text(member.file_id));
			source_fields.insert_or_assign("logical_path", text(member.logical_path));
			source_fields.insert_or_assign("content_digest", text(member.content_digest));
			source_fields.insert_or_assign("size_bytes",
										   json_value::unsigned_integer(member.size_bytes));
			source_fields.insert_or_assign("encoding", text(member.encoding));
			source_fields.insert_or_assign("line_index_id",
										   text("line-index:sha256:" + std::string(64U, '4')));
			task_fields.insert_or_assign("source", object(std::move(source_fields)));
			tasks.front() = object(std::move(task_fields));
			root_fields.insert_or_assign("tasks", json_value::array(std::move(tasks)));

			auto publication_fields = *root_fields.at("publication").as_object();
			auto selector_fields = *publication_fields.at("selector").as_object();
			selector_fields.insert_or_assign("catalog_id", text(catalog->catalog_id));
			publication_fields.insert_or_assign("selector", object(std::move(selector_fields)));
			auto selector = authority_seed->publication.selector;
			selector.catalog_id = catalog->catalog_id;
			publication_fields.insert_or_assign("series_id", text(selector.id()));
			root_fields.insert_or_assign("publication", object(std::move(publication_fields)));
			root = object(std::move(root_fields));
		}
		auto authority = decode_provider_task_v4_request_authority(root);
		assert(authority && authority->tasks.size() == 1U);
		bind_runtime_engine_authority(*authority);
		const auto* authority_tasks = root.member("tasks");
		assert(authority_tasks != nullptr && authority_tasks->as_array() != nullptr &&
			   authority_tasks->as_array()->size() == 1U);
		constexpr std::array<std::string_view, 8U> base_fields{"environment_digest",
															   "normalized_invocation_digest",
															   "provider_execution_id",
															   "provider_task_id",
															   "source",
															   "task_input_digest",
															   "toolchain_digest",
															   "working_directory"};
		json_value::object_type base_projection_fields;
		for (const auto name : base_fields)
		{
			const auto* value = authority_tasks->as_array()->front().member(name);
			assert(value != nullptr);
			base_projection_fields.emplace(std::string{name}, *value);
		}
		const auto base_projection_value = json_value::object(std::move(base_projection_fields));
		assert(base_projection_value);
		const auto base_projection = canonical_json(*base_projection_value);
		const auto& authority_task = authority->tasks.front();
		provider_task_v4_base_task base;
		base.provider_task_id = authority_task.provider_task_id;
		base.provider_execution_id = authority_task.provider_execution_id;
		base.canonical_base_task_digest = sdk::content_digest(
			std::as_bytes(std::span{base_projection.data(), base_projection.size()}));
		base.task_input_digest = authority_task.task_input_digest;
		base.normalized_invocation_digest = authority_task.normalized_invocation_digest;
		base.toolchain_digest = authority_task.toolchain_digest;
		base.environment_digest = authority_task.environment_digest;
		base.working_directory = authority_task.working_directory;
		base.source = authority_task.source;
		assert(base.canonical_base_task_digest.starts_with("sha256:"));
		const auto task = make_task(manifest, base);
		materialization_request_v2_2 request;
		const auto* materialization_id = root.member("materialization_request_id");
		const auto* semantic_digest = root.member("semantic_request_digest");
		assert(materialization_id != nullptr && materialization_id->as_string() != nullptr &&
			   semantic_digest != nullptr && semantic_digest->as_string() != nullptr);
		request.materialization_request_id = *materialization_id->as_string();
		request.semantic_request_digest = *semantic_digest->as_string();
		request.required_features = materialization_request_v2_2_required_features();
		request.inherited_authority = root;
		request.base_tasks = {base};
		request.source_closures = {{manifest.closure_id,
									manifest.closure_digest,
									manifest.manifest_digest,
									1U,
									1U,
									manifest.blobs.front().size_bytes}};
		request.task_extensions = {task};
		request.request_digest = *derive_materialization_request_v2_2_digest(request);
		request.request_id = "materialization-request:" + request.request_digest;

		const auto extension_value = extension_json(task);
		auto root_fields = *root.as_object();
		{
			auto engine_fields = *root_fields.at("engine").as_object();
			engine_fields.insert_or_assign("engine_registry_digest",
										   text(authority->engine.engine_registry_digest));
			auto admitted = *engine_fields.at("admitted_descriptors").as_array();
			assert(admitted.size() == authority->engine.admitted_descriptors.size());
			for (std::size_t index{}; index < admitted.size(); ++index)
			{
				auto fields = *admitted[index].as_object();
				fields.insert_or_assign(
					"runtime_descriptor_digest",
					text(authority->engine.admitted_descriptors[index].runtime_descriptor_digest));
				admitted[index] = object(std::move(fields));
			}
			engine_fields.insert_or_assign("admitted_descriptors",
										   json_value::array(std::move(admitted)));
			root_fields.insert_or_assign("engine", object(std::move(engine_fields)));
			auto publication_fields = *root_fields.at("publication").as_object();
			auto selector_fields = *publication_fields.at("selector").as_object();
			selector_fields.insert_or_assign(
				"relation_registry_digest",
				text(authority->publication.selector.relation_registry_digest));
			publication_fields.insert_or_assign("selector", object(std::move(selector_fields)));
			publication_fields.insert_or_assign("series_id",
												text(authority->publication.series_id));
			root_fields.insert_or_assign("publication", object(std::move(publication_fields)));
		}
		root_fields.insert_or_assign("request_digest", text(request.request_digest));
		root_fields.insert_or_assign("request_id", text(request.request_id));
		root_fields.insert_or_assign(
			"required_features",
			json_value::array({text("task-input-chunks-v2"), text("task-source-closure-v2")}));
		root_fields.insert_or_assign("source_closures",
									 json_value::array({summary_json(manifest)}));
		root_fields.insert_or_assign("task_extensions", json_value::array({extension_value}));

		auto normalized_root = object(std::move(root_fields));
		request.inherited_authority = normalized_root;
		auto normalized_digest = derive_materialization_request_v2_2_digest(request);
		assert(normalized_digest);
		request.request_digest = *normalized_digest;
		request.request_id = "materialization-request:" + request.request_digest;
		auto final_root_fields = *normalized_root.as_object();
		final_root_fields.insert_or_assign("request_digest", text(request.request_digest));
		final_root_fields.insert_or_assign("request_id", text(request.request_id));
		normalized_root = object(std::move(final_root_fields));
		request.inherited_authority = normalized_root;
		fixture output{std::move(request),
					   std::move(normalized_root),
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
		setenv("CXXLENS_PROVIDER_SOURCE_CLOSURE_MANIFEST_DIGEST",
			   value.manifest.manifest_digest.c_str(),
			   1);
		setenv("CXXLENS_PROVIDER_SOURCE_CLOSURE_TRANSFER_DIGEST", value.transfer_digest.c_str(), 1);
		setenv("CXXLENS_PROVIDER_SOURCE_CLOSURE_STREAM_ID", "1", 1);
		setenv("CXXLENS_PROVIDER_SOURCE_CLOSURE_FIRST_SEQUENCE", "0", 1);
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
								"CXXLENS_PROVIDER_SOURCE_CLOSURE_MANIFEST_DIGEST",
								"CXXLENS_PROVIDER_SOURCE_CLOSURE_TRANSFER_DIGEST",
								"CXXLENS_PROVIDER_SOURCE_CLOSURE_STREAM_ID",
								"CXXLENS_PROVIDER_SOURCE_CLOSURE_FIRST_SEQUENCE"})
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

	[[nodiscard]] std::string executable_digest(const std::string_view path)
	{
		std::ifstream input{std::string{path}, std::ios::binary};
		assert(input.good());
		const std::string bytes{std::istreambuf_iterator<char>{input},
								std::istreambuf_iterator<char>{}};
		assert(!input.bad());
		return sdk::content_digest(std::as_bytes(std::span{bytes.data(), bytes.size()}));
	}

	void positive_fd_receiver(const std::string_view worker_path)
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
		if (!worker_path.empty())
		{
			conformance_provider_trust_issuer issuer;
			received->request.authority.worker.executable = std::string{worker_path};
			received->request.authority.worker.installed_binary_digest =
				executable_digest(worker_path);
			const auto& member = received->receiver.snapshot.members.front();
			received->request.authority.tasks.front().source = {
				received->receiver.snapshot.snapshot_id,
				member.file_id,
				member.logical_path,
				member.content_digest,
				member.size_bytes,
				"utf8",
				"line-index:sha256:" + std::string(64U, '4'),
				true};
			received->request.request.base_tasks.front().source =
				received->request.authority.tasks.front().source;
			auto execution = run_materializer_worker(std::move(*received), issuer);
			if (!execution || !execution->outcome.succeeded())
			{
				if (!execution)
					std::cerr << execution.error().code << ':' << execution.error().field << ':'
							  << execution.error().detail << '\n';
				else
					std::cerr << execution->outcome.terminal << ':' << execution->outcome.exit_code
							  << '\n';
				std::abort();
			}
			auto published = publish_materializer_worker(std::move(*execution));
			if (!published)
			{
				std::cerr << published.error().code << ':' << published.error().field << ':'
						  << published.error().detail << '\n';
				std::abort();
			}
			if (published->publication.snapshot.id().empty())
				std::abort();
			const auto expected_partition_count =
				task_v4_base_descriptor_ids.size() + task_v4_output_descriptor_ids.size();
			if (published->publication.snapshot.manifest().partitions.size() !=
				expected_partition_count)
				std::abort();
			if (published->receipt.batch_receipts.size() != task_v4_output_descriptor_ids.size() ||
				published->publication.output_batch_count != task_v4_output_descriptor_ids.size() ||
				published->publication.output_receipt_digest != published->receipt.receipt_digest ||
				published->receipt.claim_count == 0U || !published->receipt.complete)
				std::abort();
		}
		std::array<std::byte, 4096U> ack_bytes{};
		const auto ack_size = ::read(channel.host_read, ack_bytes.data(), ack_bytes.size());
		if (ack_size <= 0)
			std::abort();
		clear_channel_environment();
	}

	void production_issuer_fails_closed(const std::string_view worker_path)
	{
		auto value = make_fixture();
		socket_channel_endpoints channel;
		set_channel_environment(value, channel.child_read, channel.child_write);
		write_all(channel.host_write, value.transcript);
		auto received = receive_installed_materializer_source_closure(value.root);
		if (!received)
			std::abort();
		received->request.authority.worker.executable = std::string{worker_path};
		received->request.authority.worker.installed_binary_digest = executable_digest(worker_path);
		auto execution = run_materializer_worker(std::move(*received));
		if (execution || execution.error().code != "security.certification-missing")
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

int main(const int argc, char** argv)
{
	disconnected_is_explicit();
	duplicate_channel_custody_is_rejected();
	foreign_task_binding_is_rejected();
	if (argc == 2)
		production_issuer_fails_closed(argv[1]);
	positive_fd_receiver(argc == 2 ? argv[1] : std::string_view{});
	return 0;
}
