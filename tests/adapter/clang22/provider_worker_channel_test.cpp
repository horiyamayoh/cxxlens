#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__) && defined(__GLIBC__)
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cxxlens/sdk/provider.hpp>

#include "llvm/clang22/materialization_json.hpp"
#include "llvm/clang22/provider_task_v4.hpp"
#include "llvm/clang22/source_closure.hpp"
#include "llvm/clang22/source_closure_task_v4.hpp"
#include "llvm/clang22/source_closure_transport.hpp"
#include "protocol_v2/closure.hpp"
#include "sdk/provider_runtime_internal.hpp"

namespace
{
#if defined(__linux__) && defined(__GLIBC__) && defined(CXXLENS_TEST_CLANGXX22_PATH)
	using namespace cxxlens::detail::clang22;
	using cxxlens::sdk::provider::frame;
	using cxxlens::sdk::provider::message_type;
	namespace protocol = cxxlens::protocol_v2;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(EXIT_FAILURE);
		}
	}

	[[nodiscard]] std::shared_ptr<const std::string> content(std::string value)
	{
		return std::make_shared<const std::string>(std::move(value));
	}

	[[nodiscard]] std::vector<std::string> effective_arguments()
	{
#if defined(CXXLENS_TEST_CLANGXX22_PATH)
		return {
			CXXLENS_TEST_CLANGXX22_PATH,
			"-std=c++23",
			"-nostdinc",
			"-nostdinc++",
#if defined(CXXLENS_TEST_CLANG22_RESOURCE_DIR)
			"-resource-dir=" CXXLENS_TEST_CLANG22_RESOURCE_DIR,
#endif
			"project://src/main.cpp",
		};
#else
		return {"/usr/bin/clang++", "-nostdinc", "-nostdinc++", "project://src/main.cpp"};
#endif
	}

	[[nodiscard]] std::vector<std::string> qualified_roots()
	{
#if defined(CXXLENS_TEST_CLANG22_ROOT)
		return {CXXLENS_TEST_CLANG22_ROOT};
#else
		return {"/usr"};
#endif
	}

	[[nodiscard]] std::string executable_digest(const std::string& path)
	{
		std::ifstream input{path, std::ios::binary};
		require(input.good(), "worker executable could not be opened");
		std::string bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
		require(!input.bad(), "worker executable could not be read");
		return cxxlens::sdk::content_digest(std::as_bytes(std::span{bytes}));
	}

	[[nodiscard]] std::string manifest_bytes(const source_closure_snapshot& closure)
	{
		using cxxlens::detail::clang22::materialization::canonical_json;
		using cxxlens::detail::clang22::materialization::json_value;
		std::vector<json_value> members;
		for (const auto& item : closure.members)
		{
			json_value::object_type fields;
			fields.emplace("content_digest", json_value::string(item.content_digest).value());
			fields.emplace("encoding", json_value::string("utf8").value());
			fields.emplace("file_id", json_value::string(item.file_id).value());
			fields.emplace("logical_path", json_value::string(item.logical_path).value());
			fields.emplace("read_only", json_value::boolean(item.read_only));
			fields.emplace("role", json_value::string("main").value());
			fields.emplace("size_bytes", json_value::unsigned_integer(item.size_bytes));
			members.push_back(json_value::object(std::move(fields)).value());
		}
		std::vector<json_value> blobs;
		for (const auto& item : closure.blobs)
		{
			json_value::object_type fields;
			fields.emplace("content_digest", json_value::string(item.content_digest).value());
			fields.emplace("size_bytes", json_value::unsigned_integer(item.size_bytes));
			blobs.push_back(json_value::object(std::move(fields)).value());
		}
		json_value::object_type root;
		root.emplace("blobs", json_value::array(std::move(blobs)));
		root.emplace("closure_digest", json_value::string(closure.closure_digest).value());
		root.emplace("closure_id", json_value::string(closure.snapshot_id).value());
		root.emplace("members", json_value::array(std::move(members)));
		root.emplace("schema",
					 json_value::string(std::string{source_closure_manifest_schema}).value());
		return canonical_json(json_value::object(std::move(root)).value());
	}

	struct fixture
	{
		source_closure_task_v4_input input;
		source_closure_task_v4_identity identity;
		std::string session_id{"provider-session:sha256:" + std::string(64U, '6')};
		std::string expected_transfer;
		std::vector<std::byte> transcript;
		std::string envelope;
	};

	[[nodiscard]] fixture make_fixture()
	{
		auto closure = make_source_closure_snapshot({
			{"project://src/main.cpp",
			 source_closure_role::main,
			 source_closure_encoding::utf8,
			 content("int main() { return 0; }\n")},
		});
		require(closure.has_value(), "worker channel closure fixture failed");
		fixture output;
		output.input.base_task_index = 0U;
		output.input.base_provider_task_id = "task:semantic-v2:sha256:" + std::string(64U, '1');
		const std::string base_projection{"{\"a\":\"b\",\"schema\":\"base\"}"};
		const auto base_bytes =
			std::as_bytes(std::span{base_projection.data(), base_projection.size()});
		output.input.base_task_projection = {base_bytes.begin(), base_bytes.end()};
		output.input.task_input_digest = "sha256:" + std::string(64U, '2');
		output.input.logical_working_directory = "project://src";
		const auto arguments = effective_arguments();
		auto invocation = derive_provider_task_v4_effective_invocation_digest(
			output.input.logical_working_directory, arguments);
		require(invocation.has_value(), "worker channel invocation digest failed");
		output.input.normalized_invocation_digest = std::move(*invocation);
		output.input.toolchain_digest = "semantic-v2:sha256:" + std::string(64U, '4');
		output.input.environment_digest = "sha256:" + std::string(64U, '5');
		output.input.closure = std::move(*closure);
		output.input.main_logical_path = "project://src/main.cpp";
		auto identity = derive_source_closure_task_v4_identity(output.input);
		require(identity.has_value(), "worker channel task-v4 identity failed");
		output.identity = std::move(*identity);

		using cxxlens::sdk::provider::encode_frame;
		const auto manifest = manifest_bytes(output.input.closure);
		auto manifest_digest =
			cxxlens::sdk::semantic_digest(source_closure_manifest_digest_domain, manifest);
		require(manifest_digest && *manifest_digest == output.identity.manifest_digest,
				"worker channel manifest digest diverged");
		const auto closure_id = output.input.closure.snapshot_id;
		source_closure_transfer_binding binding{output.session_id,
												output.identity.task_id,
												output.identity.task_v4_digest,
												closure_id,
												output.input.closure.closure_digest,
												*manifest_digest,
												0U};
		const auto limits = protocol::closure_limits{};
		std::uint64_t sequence{};
		auto append = [&](const message_type type,
						  protocol::closure_control control,
						  std::span<const std::byte> payload = {})
		{
			auto encoded = protocol::encode_closure_control(
				static_cast<protocol::message_type>(static_cast<std::uint16_t>(type)),
				std::move(control),
				limits);
			require(encoded.has_value(), "worker channel control encoding failed");
			frame value;
			value.type = type;
			value.stream_id = 7U;
			value.sequence = sequence++;
			value.control = std::move(*encoded);
			value.payload.assign(payload.begin(), payload.end());
			auto wire = encode_frame(value);
			require(wire.has_value(), "worker channel frame encoding failed");
			output.transcript.insert(output.transcript.end(), wire->begin(), wire->end());
		};
		const auto as_bytes = [](const std::string& value)
		{
			return std::as_bytes(std::span{value.data(), value.size()});
		};
		append(message_type::source_closure_manifest,
			   protocol::source_closure_manifest_descriptor{protocol::manifest_kind::descriptor,
															binding.session_id,
															binding.task_id,
															binding.task_v4_digest,
															binding.closure_id,
															binding.closure_digest,
															binding.manifest_digest,
															manifest.size(),
															manifest.size(),
															1U});
		append(message_type::source_closure_manifest,
			   protocol::source_closure_manifest_chunk{protocol::manifest_kind::chunk,
													   binding.session_id,
													   binding.task_id,
													   binding.manifest_digest,
													   0U,
													   0U,
													   manifest.size()},
			   as_bytes(manifest));
		const auto& blob = output.input.closure.blobs.front();
		append(message_type::source_closure_blob,
			   protocol::source_closure_blob_descriptor{binding.session_id,
														binding.task_id,
														binding.closure_digest,
														0U,
														blob.content_digest,
														blob.size_bytes,
														blob.size_bytes,
														1U});
		append(message_type::source_closure_chunk,
			   protocol::source_closure_chunk{binding.session_id,
											  binding.task_id,
											  0U,
											  blob.content_digest,
											  0U,
											  0U,
											  blob.size_bytes},
			   as_bytes(*blob.content));
		const std::array receipts{
			source_closure_blob_receipt{0U, blob.content_digest, blob.size_bytes}};
		auto receipts_digest = source_closure_blob_receipts_digest(receipts);
		require(receipts_digest.has_value(), "worker channel blob receipt digest failed");
		auto transfer_digest =
			source_closure_transfer_digest(binding, *receipts_digest, 1U, blob.size_bytes);
		require(transfer_digest.has_value(), "worker channel transfer digest failed");
		output.expected_transfer = *transfer_digest;
		append(message_type::source_closure_seal,
			   protocol::source_closure_seal{binding.session_id,
											 binding.task_id,
											 binding.task_v4_digest,
											 binding.manifest_digest,
											 *receipts_digest,
											 1U,
											 blob.size_bytes,
											 binding.closure_digest,
											 *transfer_digest});

		using materialization::canonical_json;
		using materialization::json_value;
		const auto payload_text =
			std::string{reinterpret_cast<const char*>(output.identity.input_payload.data()),
						output.identity.input_payload.size()};
		auto payload_document = materialization::parse_json_object(payload_text);
		require(payload_document.has_value(), "worker channel task payload parse failed");
		json_value::object_type closure_fields;
		closure_fields.emplace("closure_digest",
							   json_value::string(binding.closure_digest).value());
		closure_fields.emplace("closure_id", json_value::string(binding.closure_id).value());
		closure_fields.emplace("expected_transfer_digest",
							   json_value::string(output.expected_transfer).value());
		closure_fields.emplace("first_sequence", json_value::unsigned_integer(0U));
		closure_fields.emplace("manifest_digest",
							   json_value::string(binding.manifest_digest).value());
		closure_fields.emplace("session_id", json_value::string(binding.session_id).value());
		closure_fields.emplace("stream_id", json_value::unsigned_integer(7U));
		closure_fields.emplace("task_id", json_value::string(binding.task_id).value());
		closure_fields.emplace("task_v4_digest",
							   json_value::string(binding.task_v4_digest).value());
		json_value::object_type authority_fields;
		const auto authority_arguments = effective_arguments();
		std::vector<json_value> argument_values;
		for (const auto& argument : authority_arguments)
			argument_values.push_back(json_value::string(argument).value());
		std::vector<json_value> root_values;
		for (const auto& root : qualified_roots())
			root_values.push_back(json_value::string(root).value());
		authority_fields.emplace("effective_arguments",
								 json_value::array(std::move(argument_values)));
		authority_fields.emplace(
			"logical_working_directory",
			json_value::string(output.input.logical_working_directory).value());
		authority_fields.emplace(
			"normalized_invocation_digest",
			json_value::string(output.input.normalized_invocation_digest).value());
		authority_fields.emplace("qualified_read_roots", json_value::array(std::move(root_values)));
		json_value::object_type envelope_fields;
		auto base_document = materialization::parse_json_object(base_projection);
		require(base_document.has_value(), "worker channel base projection parse failed");
		envelope_fields.emplace("base_task_projection", base_document->root());
		envelope_fields.emplace("closure_binding",
								json_value::object(std::move(closure_fields)).value());
		envelope_fields.emplace("expected_base_task_digest",
								json_value::string(output.identity.base_task_digest).value());
		envelope_fields.emplace("expected_task_v4_input_digest",
								json_value::string(output.identity.task_v4_input_digest).value());
		envelope_fields.emplace("input_authority",
								json_value::object(std::move(authority_fields)).value());
		envelope_fields.emplace("stream_id", json_value::unsigned_integer(7U));
		envelope_fields.emplace("schema",
								json_value::string("cxxlens.clang22.worker-ingress.v4").value());
		envelope_fields.emplace("task_v4_payload", payload_document->root());
		output.envelope = canonical_json(json_value::object(std::move(envelope_fields)).value());
		return output;
	}

#if defined(__linux__) && defined(__GLIBC__)
	struct descriptors
	{
		int read_child{-1};
		int read_parent{-1};
		int write_child{-1};
		int write_parent{-1};
		~descriptors()
		{
			for (const auto value : {read_child, read_parent, write_child, write_parent})
				if (value >= 0)
					(void)::close(value);
		}
		descriptors() = default;
		descriptors(descriptors&& other) noexcept
			: read_child{std::exchange(other.read_child, -1)},
			  read_parent{std::exchange(other.read_parent, -1)},
			  write_child{std::exchange(other.write_child, -1)},
			  write_parent{std::exchange(other.write_parent, -1)}
		{
		}
		descriptors& operator=(descriptors&& other) noexcept
		{
			if (this != &other)
			{
				for (const auto value : {read_child, read_parent, write_child, write_parent})
					if (value >= 0)
						(void)::close(value);
				read_child = std::exchange(other.read_child, -1);
				read_parent = std::exchange(other.read_parent, -1);
				write_child = std::exchange(other.write_child, -1);
				write_parent = std::exchange(other.write_parent, -1);
			}
			return *this;
		}
		descriptors(const descriptors&) = delete;
		descriptors& operator=(const descriptors&) = delete;
	};

	[[nodiscard]] descriptors make_descriptors()
	{
		descriptors output;
		int read_pair[2]{-1, -1};
		int write_pair[2]{-1, -1};
		require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, read_pair) ==
					0,
				"worker channel read socketpair failed");
		require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, write_pair) ==
					0,
				"worker channel write socketpair failed");
		output.read_child = read_pair[0];
		output.read_parent = read_pair[1];
		output.write_child = write_pair[0];
		output.write_parent = write_pair[1];
		for (const auto descriptor : {output.read_child, output.write_child})
		{
			const auto promoted = ::fcntl(descriptor, F_DUPFD, 4);
			require(promoted >= 4, "worker channel descriptor promotion failed");
			const auto flags = ::fcntl(promoted, F_GETFD);
			require(flags >= 0 && ::fcntl(promoted, F_SETFD, flags & ~FD_CLOEXEC) == 0,
					"worker channel close-on-exec clear failed");
			const auto status = ::fcntl(promoted, F_GETFL);
			require(status >= 0 && ::fcntl(promoted, F_SETFL, status | O_NONBLOCK) == 0,
					"worker channel nonblocking setup failed");
			if (descriptor == output.read_child)
				output.read_child = promoted;
			else
				output.write_child = promoted;
			(void)::close(descriptor);
		}
		return output;
	}

	void write_all(const int descriptor, const std::span<const std::byte> bytes)
	{
		std::size_t offset{};
		while (offset < bytes.size())
		{
			const auto count = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
			if (count > 0)
			{
				offset += static_cast<std::size_t>(count);
				continue;
			}
			require(count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK),
					"worker channel write failed");
			pollfd descriptor_poll{descriptor, POLLOUT, 0};
			require(::poll(&descriptor_poll, 1U, 3000) > 0, "worker channel write timed out");
		}
	}

	[[nodiscard]] std::vector<std::byte> read_available(const int descriptor)
	{
		std::vector<std::byte> output;
		std::array<std::byte, 4096U> buffer{};
		for (;;)
		{
			const auto count = ::read(descriptor, buffer.data(), buffer.size());
			if (count > 0)
			{
				output.insert(output.end(), buffer.begin(), buffer.begin() + count);
				continue;
			}
			if (count == 0 || (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)))
				return output;
			require(errno == EINTR, "worker channel ACK read failed");
		}
	}
#endif
#endif
} // namespace

int main(const int argc, const char* const* argv)
{
#if defined(__linux__) && defined(__GLIBC__) && defined(CXXLENS_TEST_CLANGXX22_PATH)
	require(argc == 2, "worker executable path is missing");
	auto fixture = make_fixture();
	auto run = [&](std::string envelope, const bool expect_success)
	{
		auto descriptors = make_descriptors();
		const auto task = fixture.identity.task_id;
		auto binding = cxxlens::sdk::provider::detail::make_process_inherited_channel_binding(
			descriptors.read_child,
			descriptors.write_child,
			task,
			fixture.session_id,
			fixture.input.closure.closure_digest,
			fixture.expected_transfer);
		require(binding.has_value(), "worker channel process binding failed");
		auto policies = cxxlens::sdk::provider::builtin_sandbox_policies();
		require(!policies.empty(), "worker channel sandbox registry empty");
		cxxlens::sdk::provider::process_invocation invocation;
		invocation.argv = {argv[1]};
		invocation.standard_input.assign(
			std::as_bytes(std::span{envelope.data(), envelope.size()}).begin(),
			std::as_bytes(std::span{envelope.data(), envelope.size()}).end());
		invocation.budget.wall_ms = 15'000U;
		invocation.budget.cpu_ms = 15'000U;
		invocation.budget.address_space_bytes = 1024U * 1024U * 1024U;
		invocation.budget.transport_bytes = 128U * 1024U * 1024U;
		invocation.budget.output_bytes = 4U * 1024U * 1024U;
		invocation.budget.open_files = 128U;
		invocation.budget.subprocesses = 1U;
		invocation.sandbox = {cxxlens::sdk::provider::sandbox_assurance::enforced,
							  policies.front().policy_digest()};
		invocation.expected_binary_digest = executable_digest(argv[1]);
		invocation.environment = {{"CXXLENS_PROVIDER_MANIFEST", "cxxlens.clang22.reference"}};
		invocation.inherited_channel = std::move(*binding);
		if (expect_success)
			write_all(descriptors.read_parent, fixture.transcript);
		auto port = cxxlens::sdk::provider::make_system_provider_process_port();
		require(port != nullptr, "worker channel process port unavailable");
		auto result = port->run(invocation, {});
		if (!result || result->status != cxxlens::sdk::provider::process_status::exited)
		{
			std::cerr << "worker channel status="
					  << (result ? static_cast<int>(result->status) : -1)
					  << " code=" << (result ? result->failure_code : result.error().code)
					  << " stderr=" << (result ? result->standard_error : result.error().detail)
					  << '\n';
			require(false, "worker channel process did not exit");
		}
		if (expect_success && result->exit_code != 0)
			std::cerr << "worker channel child exit=" << result->exit_code
					  << " failure=" << result->failure_code << " stderr=" << result->standard_error
					  << " stdout=" << result->standard_output.size() << '\n';
		if (expect_success)
		{
			require(result->exit_code == 0, "worker channel v4 ingress failed");
			auto frames = cxxlens::sdk::provider::decode_frame_stream(result->standard_output);
			require(frames && frames->size() == 3U &&
						frames->back().type == message_type::task_failed,
					"worker channel output did not fail closed after translation");
			auto failed =
				cxxlens::sdk::provider::decode_task_failed_metadata(frames->back().control);
			require(failed && failed->error_code == "provider.output-authority-missing" &&
						failed->task_id == fixture.identity.task_id,
					"worker channel publication boundary was not typed");
			auto ack = read_available(descriptors.write_parent);
			require(!ack.empty(), "worker channel did not emit closure ACK");
			auto ack_frame = cxxlens::sdk::provider::decode_frame(ack);
			require(ack_frame && ack_frame->type == message_type::source_closure_ack,
					"worker channel emitted an invalid closure ACK");
		}
		else
			require(result->exit_code != 0 && result->standard_output.empty(),
					"tampered worker channel envelope was accepted");
	};
	run(fixture.envelope, true);
	auto tampered = fixture.envelope;
	const auto marker = tampered.find(fixture.identity.task_v4_input_digest);
	require(marker != std::string::npos, "worker channel test digest marker missing");
	tampered.replace(
		marker, fixture.identity.task_v4_input_digest.size(), "sha256:" + std::string(64U, 'f'));
	run(std::move(tampered), false);
	return EXIT_SUCCESS;
#else
	(void)argc;
	(void)argv;
	return 77;
#endif
}
