#include "llvm/clang22/source_closure_receiver.hpp"

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

#include "llvm/clang22/materialization_json.hpp"
#include "sdk/provider_protocol_v2_adapter.hpp"

namespace
{
	using namespace cxxlens::detail::clang22;
	using cxxlens::sdk::provider::frame;
	using cxxlens::sdk::provider::message_type;
	namespace provider_detail = ::cxxlens::sdk::provider::detail;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	template <typename T>
	void require_result(const cxxlens::sdk::result<T>& result, const std::string_view message)
	{
		require(result.has_value(), message);
	}

	[[nodiscard]] std::string typed(const std::string_view prefix, const char digit)
	{
		return std::string{prefix} + std::string(64U, digit);
	}

	[[nodiscard]] std::span<const std::byte> bytes(const std::string& value)
	{
		return std::as_bytes(std::span{value.data(), value.size()});
	}

	class memory_source final : public source_closure_frame_source
	{
	  public:
		explicit memory_source(std::vector<std::byte> input) : input_{std::move(input)} {}

		cxxlens::sdk::result<std::size_t> read(const std::span<std::byte> destination) override
		{
			if (offset_ == input_.size())
				return std::size_t{};
			const auto count = std::min<std::size_t>(destination.size(), 17U);
			const auto available = std::min(count, input_.size() - offset_);
			std::ranges::copy(std::span{input_}.subspan(offset_, available), destination.begin());
			offset_ += available;
			return available;
		}

	  private:
		std::vector<std::byte> input_;
		std::size_t offset_{};
	};

	class memory_sink final : public source_closure_frame_sink
	{
	  public:
		cxxlens::sdk::result<void> write(const std::span<const std::byte> frame_bytes) override
		{
			output_.insert(output_.end(), frame_bytes.begin(), frame_bytes.end());
			return {};
		}

		std::vector<std::byte> output_;
	};

	class authority final : public source_closure_task_v4_authority
	{
	  public:
		authority(std::string task, std::string digest)
			: task_{std::move(task)}, digest_{std::move(digest)}
		{
		}

		std::string task_;
		std::string digest_;
		std::size_t revalidate_calls{};

		std::string_view task_id() const noexcept override
		{
			return task_;
		}
		std::string_view task_v4_digest() const noexcept override
		{
			return digest_;
		}
		cxxlens::sdk::result<void> revalidate() const override
		{
			++const_cast<authority*>(this)->revalidate_calls;
			return {};
		}
	};

	struct fixture
	{
		source_closure_snapshot closure;
		source_closure_transfer_binding binding;
		std::string manifest_bytes;
		std::vector<std::byte> transcript;
	};

	[[nodiscard]] std::string manifest(const source_closure_snapshot& closure)
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

	[[nodiscard]] fixture make_fixture()
	{
		const auto source = std::make_shared<const std::string>("int main() { return 0; }\n");
		auto closure = make_source_closure_snapshot({{"project://src/main.cpp",
													  source_closure_role::main,
													  source_closure_encoding::utf8,
													  source}});
		require_result(closure, "receiver fixture closure failed");
		fixture output{std::move(*closure), {}, {}, {}};
		output.manifest_bytes = manifest(output.closure);
		auto manifest_digest = cxxlens::sdk::semantic_digest("cxxlens.source-closure-manifest.v1",
															 output.manifest_bytes);
		require_result(manifest_digest, "receiver fixture manifest digest failed");
		output.binding.session_id = typed("provider-session:sha256:", '1');
		output.binding.task_v4_digest = typed("semantic-v2:sha256:", '2');
		output.binding.task_id = "task:" + output.binding.task_v4_digest;
		output.binding.closure_id = output.closure.snapshot_id;
		output.binding.closure_digest = output.closure.closure_digest;
		output.binding.manifest_digest = *manifest_digest;

		const auto limits = provider_detail::provider_protocol_v2_closure_limits{};
		std::uint64_t sequence{};
		auto append = [&](const message_type type,
						  provider_detail::provider_protocol_v2_control control,
						  std::span<const std::byte> payload = {})
		{
			auto encoded =
				provider_detail::encode_provider_protocol_v2_closure_control(type, control, limits);
			require_result(encoded, "receiver fixture control encoding failed");
			frame value;
			value.type = type;
			value.stream_id = 7U;
			value.sequence = sequence++;
			value.control = std::move(*encoded);
			value.payload.assign(payload.begin(), payload.end());
			auto wire = cxxlens::sdk::provider::encode_frame(value);
			require_result(wire, "receiver fixture frame encoding failed");
			output.transcript.insert(output.transcript.end(), wire->begin(), wire->end());
		};

		append(message_type::source_closure_manifest,
			   provider_detail::provider_protocol_v2_manifest_descriptor{
				   cxxlens::protocol_v2::manifest_kind::descriptor,
				   output.binding.session_id,
				   output.binding.task_id,
				   output.binding.task_v4_digest,
				   output.binding.closure_id,
				   output.binding.closure_digest,
				   output.binding.manifest_digest,
				   output.manifest_bytes.size(),
				   output.manifest_bytes.size(),
				   1U});
		append(message_type::source_closure_manifest,
			   provider_detail::provider_protocol_v2_manifest_chunk{
				   cxxlens::protocol_v2::manifest_kind::chunk,
				   output.binding.session_id,
				   output.binding.task_id,
				   output.binding.manifest_digest,
				   0U,
				   0U,
				   output.manifest_bytes.size()},
			   bytes(output.manifest_bytes));
		const auto& blob = output.closure.blobs.front();
		append(message_type::source_closure_blob,
			   provider_detail::provider_protocol_v2_blob{output.binding.session_id,
														  output.binding.task_id,
														  output.binding.closure_digest,
														  0U,
														  blob.content_digest,
														  blob.size_bytes,
														  blob.size_bytes,
														  1U});
		append(message_type::source_closure_chunk,
			   provider_detail::provider_protocol_v2_chunk{output.binding.session_id,
														   output.binding.task_id,
														   0U,
														   blob.content_digest,
														   0U,
														   0U,
														   blob.size_bytes},
			   bytes(*blob.content));
		const std::array receipts{
			source_closure_blob_receipt{0U, blob.content_digest, blob.size_bytes}};
		auto receipts_digest = source_closure_blob_receipts_digest(receipts);
		require_result(receipts_digest, "receiver fixture receipt digest failed");
		auto transfer_digest =
			source_closure_transfer_digest(output.binding, *receipts_digest, 1U, blob.size_bytes);
		require_result(transfer_digest, "receiver fixture transfer digest failed");
		append(message_type::source_closure_seal,
			   provider_detail::provider_protocol_v2_seal{output.binding.session_id,
														  output.binding.task_id,
														  output.binding.task_v4_digest,
														  output.binding.manifest_digest,
														  *receipts_digest,
														  1U,
														  blob.size_bytes,
														  output.binding.closure_digest,
														  *transfer_digest});
		return output;
	}

	void positive_transfer()
	{
		auto input = make_fixture();
		memory_source source{input.transcript};
		memory_sink sink;
		authority task{input.binding.task_id, input.binding.task_v4_digest};
		auto result =
			receive_source_closure_frames(source, sink, {input.binding, &task, 7U, 16'384U});
		require_result(result, "receiver rejected valid source closure");
		require(result->snapshot.snapshot_id == input.closure.snapshot_id &&
					result->snapshot.closure_digest == input.closure.closure_digest &&
					result->snapshot.members.size() == input.closure.members.size() &&
					result->snapshot.blobs.size() == input.closure.blobs.size() &&
					result->snapshot.blobs.front().content != nullptr &&
					*result->snapshot.blobs.front().content == *input.closure.blobs.front().content,
				"receiver changed closure snapshot");
		require(task.revalidate_calls == 1U, "receiver skipped inherited authority revalidation");
		require(!sink.output_.empty(), "receiver did not emit source-closure ACK");
		auto ack = cxxlens::sdk::provider::decode_frame(sink.output_);
		require_result(ack, "receiver ACK was not a valid provider frame");
		require(ack->type == message_type::source_closure_ack && ack->sequence == 5U,
				"receiver ACK had the wrong sequence");
		auto decoded =
			provider_detail::decode_provider_protocol_v2_closure_control(ack->type, ack->control);
		require_result(decoded, "receiver ACK control was not canonical");
		const auto* value = std::get_if<provider_detail::provider_protocol_v2_ack>(&*decoded);
		require(value != nullptr && value->spool_receipt == result->credentials.spool_receipt &&
					value->cleanup_owner == result->credentials.cleanup_owner &&
					value->transfer_digest == result->transfer_digest,
				"receiver ACK was not bound to the sealed spool");
	}

	void truncated_transfer()
	{
		auto input = make_fixture();
		input.transcript.resize(input.transcript.size() - 104U - 0U);
		memory_source source{std::move(input.transcript)};
		memory_sink sink;
		authority task{input.binding.task_id, input.binding.task_v4_digest};
		auto result =
			receive_source_closure_frames(source, sink, {input.binding, &task, 7U, 16'384U});
		require(!result && result.error().code == "source-closure.truncated-stream",
				"truncated source closure did not fail closed");
		require(sink.output_.empty(), "truncated source closure emitted an ACK");
	}
} // namespace

int main()
{
	positive_transfer();
	truncated_transfer();
	return 0;
}
