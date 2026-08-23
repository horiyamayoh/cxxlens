#include "llvm/clang22/source_closure_task_v4_worker.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(CXXLENS_TEST_CLANGXX22_PATH)
#include <clang/AST/ASTContext.h>
#endif

#include "llvm/clang22/materialization_json.hpp"
#include "llvm/clang22/provider_worker.hpp"
#include "llvm/clang22/provider_worker_v4.hpp"
#include "llvm/clang22/source_closure_receiver.hpp"
#include "llvm/clang22/source_closure_task_v4.hpp"
#include "sdk/provider_protocol_v2_adapter.hpp"

namespace
{
	using namespace cxxlens::detail::clang22;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] std::shared_ptr<const std::string> content(std::string value)
	{
		return std::make_shared<const std::string>(std::move(value));
	}

	[[nodiscard]] std::vector<std::string> authority_arguments()
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

	[[nodiscard]] std::vector<std::string> authority_roots()
	{
#if defined(CXXLENS_TEST_CLANGXX22_PATH)
		return {CXXLENS_TEST_CLANG22_ROOT};
#else
		return {"/usr"};
#endif
	}

	[[nodiscard]] source_closure_task_v4_input fixture()
	{
		auto result = make_source_closure_snapshot({
			{"project://src/main.cpp",
			 source_closure_role::main,
			 source_closure_encoding::utf8,
			 content("int main() { return 0; }\n")},
		});
		require(result.has_value(), "worker closure fixture was rejected");
		source_closure_task_v4_input input;
		input.base_task_index = 0U;
		input.base_provider_task_id = "task:semantic-v2:sha256:" + std::string(64U, '1');
		const std::string base_projection{"{\"a\":\"b\",\"schema\":\"base\"}"};
		const auto base_bytes =
			std::as_bytes(std::span{base_projection.data(), base_projection.size()});
		input.base_task_projection = {base_bytes.begin(), base_bytes.end()};
		input.task_input_digest = "sha256:" + std::string(64U, '2');
		input.logical_working_directory = "project://src";
		const auto arguments = authority_arguments();
		auto invocation = derive_provider_task_v4_effective_invocation_digest(
			input.logical_working_directory, arguments);
		require(invocation.has_value(), "worker task-v4 invocation digest could not be derived");
		input.normalized_invocation_digest = std::move(*invocation);
		input.toolchain_digest = "semantic-v2:sha256:" + std::string(64U, '4');
		input.environment_digest = "sha256:" + std::string(64U, '5');
		input.closure = std::move(*result);
		input.main_logical_path = "project://src/main.cpp";
		return input;
	}

	[[nodiscard]] provider_task_v4_input_authority
	make_input_authority(const source_closure_task_v4_input& input)
	{
		auto arguments = authority_arguments();
		auto digest = derive_provider_task_v4_effective_invocation_digest(
			input.logical_working_directory, arguments);
		require(digest.has_value(), "worker task-v4 authority digest could not be derived");
		return {std::move(*digest),
				input.logical_working_directory,
				std::move(arguments),
				authority_roots()};
	}

	class closure_authority final : public provider_worker_v2_2_closure_authority
	{
	  public:
		bool acknowledged_state{};
		std::string session{"provider-session:sha256:" + std::string(64U, '6')};
		std::string task;
		std::string task_digest;
		std::string closure;
		std::string closure_digest_value;
		std::string transfer{"semantic-v2:sha256:" + std::string(64U, '7')};

		cxxlens::sdk::result<void> revalidate() const override
		{
			if (task.empty() || task_digest.empty() || closure.empty() ||
				closure_digest_value.empty())
				return cxxlens::sdk::unexpected(
					{"source-closure.authority-invalid", "identity", "missing"});
			return {};
		}
		[[nodiscard]] bool acknowledged() const noexcept override
		{
			return acknowledged_state;
		}
		[[nodiscard]] std::string_view session_id() const noexcept override
		{
			return session;
		}
		[[nodiscard]] std::string_view task_id() const noexcept override
		{
			return task;
		}
		[[nodiscard]] std::string_view task_v4_digest() const noexcept override
		{
			return task_digest;
		}
		[[nodiscard]] std::string_view closure_id() const noexcept override
		{
			return closure;
		}
		[[nodiscard]] std::string_view closure_digest() const noexcept override
		{
			return closure_digest_value;
		}
		[[nodiscard]] std::string_view transfer_digest() const noexcept override
		{
			return transfer;
		}
	};

#if defined(CXXLENS_TEST_CLANGXX22_PATH)
	class frame_source final : public source_closure_frame_source
	{
	  public:
		explicit frame_source(std::vector<std::byte> input) : input_{std::move(input)} {}

		cxxlens::sdk::result<std::size_t> read(const std::span<std::byte> destination) override
		{
			if (offset_ == input_.size())
				return std::size_t{};
			const auto count = std::min<std::size_t>(destination.size(), 31U);
			const auto available = std::min(count, input_.size() - offset_);
			std::copy_n(input_.begin() + static_cast<std::ptrdiff_t>(offset_),
						available,
						destination.begin());
			offset_ += available;
			return available;
		}

	  private:
		std::vector<std::byte> input_;
		std::size_t offset_{};
	};

	class frame_sink final : public source_closure_frame_sink
	{
	  public:
		cxxlens::sdk::result<void> write(const std::span<const std::byte> bytes) override
		{
			output_.insert(output_.end(), bytes.begin(), bytes.end());
			return {};
		}

		std::vector<std::byte> output_;
	};

	class receiver_authority final : public source_closure_task_v4_authority
	{
	  public:
		receiver_authority(std::string task_id,
						   std::string task_digest,
						   std::string closure_id,
						   std::string closure_digest)
			: task_id_{std::move(task_id)}, task_digest_{std::move(task_digest)},
			  closure_id_{std::move(closure_id)}, closure_digest_{std::move(closure_digest)}
		{
		}

		std::string_view task_id() const noexcept override
		{
			return task_id_;
		}
		std::string_view task_v4_digest() const noexcept override
		{
			return task_digest_;
		}
		cxxlens::sdk::result<void> revalidate() const override
		{
			if (task_id_.empty() || task_digest_.empty() || closure_id_.empty() ||
				closure_digest_.empty())
				return cxxlens::sdk::unexpected(
					{"source-closure.authority-invalid", "identity", "missing"});
			return {};
		}

		std::string task_id_;
		std::string task_digest_;
		std::string closure_id_;
		std::string closure_digest_;
	};

	[[nodiscard]] std::span<const std::byte> bytes(const std::string& value)
	{
		return std::as_bytes(std::span{value.data(), value.size()});
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
		root.emplace("schema", json_value::string("cxxlens.source-closure-manifest.v1").value());
		return canonical_json(json_value::object(std::move(root)).value());
	}

	[[nodiscard]] std::vector<std::byte>
	closure_transcript(const source_closure_snapshot& closure,
					   const source_closure_task_v4_identity& identity,
					   const std::string& session_id)
	{
		using cxxlens::sdk::provider::frame;
		using cxxlens::sdk::provider::message_type;
		namespace provider_detail = cxxlens::sdk::provider::detail;
		const auto manifest = manifest_bytes(closure);
		const auto manifest_digest =
			cxxlens::sdk::semantic_digest("cxxlens.source-closure-manifest.v1", manifest);
		require(manifest_digest && *manifest_digest == identity.manifest_digest,
				"receiver/worker manifest digest diverged");

		const auto task_id = identity.task_id;
		const auto closure_id = closure.snapshot_id;
		const auto limits = provider_detail::provider_protocol_v2_closure_limits{};
		std::vector<std::byte> output;
		std::uint64_t sequence{};
		const auto append = [&](const message_type type,
								provider_detail::provider_protocol_v2_control control,
								std::span<const std::byte> payload = {})
		{
			auto encoded =
				provider_detail::encode_provider_protocol_v2_closure_control(type, control, limits);
			require(encoded.has_value(), "worker E2E closure control encoding failed");
			frame value;
			value.type = type;
			value.stream_id = 7U;
			value.sequence = sequence++;
			value.control = std::move(*encoded);
			value.payload.assign(payload.begin(), payload.end());
			auto wire = cxxlens::sdk::provider::encode_frame(value);
			require(wire.has_value(), "worker E2E closure frame encoding failed");
			output.insert(output.end(), wire->begin(), wire->end());
		};

		append(message_type::source_closure_manifest,
			   provider_detail::provider_protocol_v2_manifest_descriptor{
				   cxxlens::protocol_v2::manifest_kind::descriptor,
				   session_id,
				   task_id,
				   identity.task_v4_digest,
				   closure_id,
				   closure.closure_digest,
				   *manifest_digest,
				   manifest.size(),
				   manifest.size(),
				   1U});
		append(message_type::source_closure_manifest,
			   provider_detail::provider_protocol_v2_manifest_chunk{
				   cxxlens::protocol_v2::manifest_kind::chunk,
				   session_id,
				   task_id,
				   *manifest_digest,
				   0U,
				   0U,
				   manifest.size()},
			   bytes(manifest));
		require(closure.blobs.size() == 1U, "worker E2E fixture blob census changed");
		const auto& blob = closure.blobs.front();
		append(message_type::source_closure_blob,
			   provider_detail::provider_protocol_v2_blob{session_id,
														  task_id,
														  closure.closure_digest,
														  0U,
														  blob.content_digest,
														  blob.size_bytes,
														  blob.size_bytes,
														  1U});
		append(message_type::source_closure_chunk,
			   provider_detail::provider_protocol_v2_chunk{
				   session_id, task_id, 0U, blob.content_digest, 0U, 0U, blob.size_bytes},
			   bytes(*blob.content));
		const std::array receipts{
			source_closure_blob_receipt{0U, blob.content_digest, blob.size_bytes}};
		auto receipts_digest = source_closure_blob_receipts_digest(receipts);
		require(receipts_digest.has_value(), "worker E2E receipt digest failed");
		auto transfer_digest = source_closure_transfer_digest({session_id,
															   task_id,
															   identity.task_v4_digest,
															   closure_id,
															   closure.closure_digest,
															   *manifest_digest,
															   0U},
															  *receipts_digest,
															  1U,
															  blob.size_bytes);
		require(transfer_digest.has_value(), "worker E2E transfer digest failed");
		append(message_type::source_closure_seal,
			   provider_detail::provider_protocol_v2_seal{session_id,
														  task_id,
														  identity.task_v4_digest,
														  *manifest_digest,
														  *receipts_digest,
														  1U,
														  blob.size_bytes,
														  closure.closure_digest,
														  *transfer_digest});
		return output;
	}

	void receiver_to_worker_positive(const source_closure_task_v4_input& input,
									 const source_closure_task_v4_identity& identity)
	{
		const std::string session_id = "provider-session:sha256:" + std::string(64U, '8');
		const auto transcript = closure_transcript(input.closure, identity, session_id);
		frame_source source{transcript};
		frame_sink sink;
		receiver_authority authority{identity.task_id,
									 identity.task_v4_digest,
									 input.closure.snapshot_id,
									 input.closure.closure_digest};
		const source_closure_transfer_binding binding{session_id,
													  identity.task_id,
													  identity.task_v4_digest,
													  input.closure.snapshot_id,
													  input.closure.closure_digest,
													  identity.manifest_digest,
													  0U};
		auto received = receive_source_closure_frames(source, sink, {binding, &authority, 7U});
		if (!received)
			std::cerr << "receiver positive path failed: " << received.error().code << " / "
					  << received.error().field << " / " << received.error().detail << '\n';
		require(received.has_value(), "receiver rejected canonical closure before worker");
		require(!sink.output_.empty(), "receiver did not emit the authenticated ACK");
		const auto ack = cxxlens::sdk::provider::decode_frame(sink.output_);
		require(ack.has_value() &&
					ack->type == cxxlens::sdk::provider::message_type::source_closure_ack &&
					ack->sequence == 5U,
				"receiver emitted an invalid source-closure ACK");

		const auto input_authority = make_input_authority(input);
		bool callback_ran = false;
		bool ast_available = false;
		auto receipt = execute_provider_worker_v4(
			{source_closure_task_v4_decoded{input, identity},
			 std::move(received->snapshot),
			 input_authority,
			 [&callback_ran,
			  &ast_available](cxxlens::provider::clang22::borrowed_translation_unit& unit)
				 -> cxxlens::sdk::result<void>
			 {
				 callback_ran = true;
				 ast_available = unit.ast().getTranslationUnitDecl() != nullptr;
				 (void)unit.source_manager();
				 return {};
			 }});
		if (!receipt)
			std::cerr << "receiver-to-worker exact path failed: " << receipt.error().code << " / "
					  << receipt.error().field << " / " << receipt.error().detail << '\n';
		require(receipt.has_value(), "receiver snapshot did not reach exact Clang worker");
		require(callback_ran && ast_available && receipt->translation_unit_executed,
				"exact Clang callback did not produce a valid execution receipt");
		require(receipt->task_id == identity.task_id &&
					receipt->task_v4_digest == identity.task_v4_digest &&
					receipt->source_closure_id == input.closure.snapshot_id,
				"worker receipt lost authenticated task or closure identity");
		require(receipt->missing_output.size() == 3U &&
					receipt->missing_output[0].field == "provider-output.analysis-recipe" &&
					receipt->missing_output[1].field == "provider-output.output-plan" &&
					receipt->missing_output[2].field == "provider-output.publication-target",
				"worker receipt did not retain the explicit Store/publication boundary");
	}
#endif
} // namespace

int main()
{
	auto input = fixture();
	auto identity = derive_source_closure_task_v4_identity(input);
	require(identity.has_value(), "worker task-v4 fixture could not derive identity");
	const auto input_authority = make_input_authority(input);

	// A complete, bound payload reaches the candidate's explicit callback gate.  No compiler is
	// launched on this host because the callback is absent; the callback gate remains fail-closed
	// until the production dispatcher supplies a validated compiler callback.
	auto result = execute_source_closure_task_v4_candidate({
		identity->input_payload,
		input.closure,
		identity->base_task_digest,
		identity->task_v4_input_digest,
		input_authority,
		cxxlens::provider::clang22::translation_unit_callback{},
	});
	require(!result && result.error().code == "source-closure.worker-input-invalid",
			"bound task-v4 payload did not fail closed at the inactive callback gate");

	auto wrong_input_digest = identity->task_v4_input_digest;
	wrong_input_digest.back() = wrong_input_digest.back() == '0' ? '1' : '0';
	result = execute_source_closure_task_v4_candidate({
		identity->input_payload,
		input.closure,
		identity->base_task_digest,
		wrong_input_digest,
		input_authority,
		cxxlens::provider::clang22::translation_unit_callback{},
	});
	require(!result && result.error().code == "source-closure.task-v4-input-digest-mismatch",
			"worker candidate accepted a payload with a foreign outer input digest");

#if defined(CXXLENS_TEST_CLANGXX22_PATH)
	// A complete task-v4 payload must reach the real, exact Clang 22 callback through the
	// closure-exclusive VFS. The callback deliberately only observes the borrowed lifetime;
	// all compiler-owned state must be detached by the worker before this receipt is returned.
	const auto exact_authority = make_input_authority(input);
	bool callback_ran = false;
	bool ast_available = false;
	result = execute_source_closure_task_v4_candidate({
		identity->input_payload,
		input.closure,
		identity->base_task_digest,
		identity->task_v4_input_digest,
		exact_authority,
		[&callback_ran, &ast_available](cxxlens::provider::clang22::borrowed_translation_unit& unit)
			-> cxxlens::sdk::result<void>
		{
			callback_ran = true;
			ast_available = unit.ast().getTranslationUnitDecl() != nullptr;
			(void)unit.source_manager();
			return {};
		},
	});
	if (!result)
		std::cerr << "exact Clang 22 task-v4 execution failed: " << result.error().code << " / "
				  << result.error().detail << '\n';
	require(result.has_value(), "exact Clang 22 task-v4 candidate did not execute successfully");
	require(callback_ran && ast_available,
			"exact Clang 22 task-v4 callback did not expose a valid AST");
	require(result->task_id == identity->task_id,
			"task-v4 worker receipt returned a foreign task identity");
	require(result->task_v4_digest == identity->task_v4_digest,
			"task-v4 worker receipt returned a foreign task digest");
	require(result->task_v4_input_digest == identity->task_v4_input_digest,
			"task-v4 worker receipt returned a foreign input digest");
	require(result->closure_id == input.closure.snapshot_id,
			"task-v4 worker receipt returned a foreign closure identity");

	// The production dispatcher must not announce task_accepted before the authenticated
	// message-28 closure ACK.  The rejected attempt must not enter the compiler callback.
	closure_authority authority;
	authority.task = identity->task_id;
	authority.task_digest = identity->task_v4_digest;
	authority.closure = input.closure.snapshot_id;
	authority.closure_digest_value = input.closure.closure_digest;
	std::vector<std::string> events;
	const auto make_dispatch_input = [&]()
	{
		return provider_worker_v2_2_dispatch_input{
			{identity->input_payload,
			 input.closure,
			 identity->base_task_digest,
			 identity->task_v4_input_digest,
			 exact_authority,
			 [&events](cxxlens::provider::clang22::borrowed_translation_unit& unit)
				 -> cxxlens::sdk::result<void>
			 {
				 events.emplace_back("compiled");
				 (void)unit.ast();
				 (void)unit.source_manager();
				 return {};
			 }},
			&authority,
			[&events](const source_closure_task_v4_identity&) -> cxxlens::sdk::result<void>
			{
				events.emplace_back("accepted");
				return {};
			}};
	};
	result = run_provider_worker_v2_2(make_dispatch_input());
	require(!result && result.error().code == "source-closure.task-accepted-before-ack",
			"v2.2 worker announced task acceptance before closure ACK");
	require(events.empty(), "pre-ACK worker path reached an acceptance or compiler callback");

	authority.acknowledged_state = true;
	result = run_provider_worker_v2_2(make_dispatch_input());
	require(result.has_value(), "ACKed v2.2 worker dispatcher did not execute Clang");
	require(events == std::vector<std::string>{"accepted", "compiled"},
			"v2.2 worker event order was not ACK -> task_accepted -> Clang callback");

	// Exercise the authenticated frame channel and exact worker as one in-process positive slice.
	// The receipt deliberately remains non-publishable: task-v4 does not carry an analysis recipe,
	// output plan, or Store publication target.
	receiver_to_worker_positive(input, *identity);
#endif

	return 0;
}
