#include "llvm/clang22/provider_task_v4.hpp"

#include <cassert>
#include <cstddef>
#include <memory>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace source_closure = cxxlens::detail::clang22::source_closure;

namespace
{
	[[nodiscard]] std::vector<std::byte> bytes(const std::string& value)
	{
		return {reinterpret_cast<const std::byte*>(value.data()),
			reinterpret_cast<const std::byte*>(value.data() + value.size())};
	}

	[[nodiscard]] source_closure::blob blob(std::string value)
	{
		auto content = bytes(value);
		return {source_closure::sha256_digest(content), std::move(content)};
	}

	[[nodiscard]] std::shared_ptr<const source_closure::validated_snapshot> closure()
	{
		auto main = blob("#include \"large.hpp\"\nint main(){return value();}\n");
		auto large = blob("inline int value(){return 42;}\n" + std::string(150000, ' '));
		const auto provenance =
			source_closure::sha256_digest(bytes("task-v4-test"));
		source_closure::snapshot input;
		input.files = {
			{"project://src/main.cpp", main.content_digest, main.content.size(),
				source_closure::file_role::main_source, provenance},
			{"project://include/large.hpp", large.content_digest, large.content.size(),
				source_closure::file_role::project_header, provenance},
		};
		input.blobs = {main, large};
		auto result = source_closure::validate(std::move(input));
		assert(result);
		return std::make_shared<const source_closure::validated_snapshot>(
			std::move(*result));
	}

	class vector_sink final : public source_closure::task_v4_sink
	{
	  public:
		std::expected<void, source_closure::validation_error>
		append(std::span<const std::byte> bytes) override
		{
			calls.push_back(bytes.size());
			data.insert(data.end(), bytes.begin(), bytes.end());
			return {};
		}

		std::vector<std::byte> data;
		std::vector<std::size_t> calls;
	};

	class failing_sink final : public source_closure::task_v4_sink
	{
	  public:
		std::expected<void, source_closure::validation_error>
		append(std::span<const std::byte>) override
		{
			return std::unexpected(source_closure::validation_error{
				"test.sink-failure", "sink", {}});
		}
	};

	[[nodiscard]] source_closure::task_v4 task(
		std::shared_ptr<const source_closure::validated_snapshot> value)
	{
		source_closure::task_v4 result;
		result.main_logical_path = "project://src/main.cpp";
		result.logical_working_directory = "project://";
		result.effective_arguments = {
			"clang++", "-Iproject://include", "project://src/main.cpp"};
		result.closure = std::move(value);
		auto id = source_closure::derive_task_v4_id(result);
		assert(id);
		result.task_id = *id;
		return result;
	}
} // namespace

int main()
{
	auto shared = closure();
	auto input = task(shared);

	vector_sink first;
	source_closure::task_v4_options small;
	small.maximum_chunk_bytes = 4096U;
	auto encoded = source_closure::encode_task_v4_streaming(input, first, small);
	assert(encoded);
	assert(encoded->stream_bytes == first.data.size());
	assert(encoded->content_chunks > 2U);
	assert(encoded->stream_digest == source_closure::sha256_digest(first.data));

	vector_sink second;
	source_closure::task_v4_options large;
	large.maximum_chunk_bytes = 65536U;
	auto encoded_again =
		source_closure::encode_task_v4_streaming(input, second, large);
	assert(encoded_again);
	assert(first.data == second.data);
	assert(encoded->stream_digest == encoded_again->stream_digest);

	auto sibling = task(shared);
	sibling.effective_arguments.insert(
		sibling.effective_arguments.begin() + 1, "-DSECOND=1");
	auto sibling_id = source_closure::derive_task_v4_id(sibling);
	assert(sibling_id);
	sibling.task_id = *sibling_id;
	assert(sibling.closure.get() == input.closure.get());
	assert(sibling.task_id != input.task_id);

	failing_sink failure;
	auto failed = source_closure::encode_task_v4_streaming(input, failure);
	assert(!failed);
	assert(failed.error().code == "test.sink-failure");

	std::stop_source stop;
	stop.request_stop();
	vector_sink cancelled_sink;
	source_closure::task_v4_options cancelled;
	cancelled.cancellation = stop.get_token();
	auto cancelled_result =
		source_closure::encode_task_v4_streaming(input, cancelled_sink, cancelled);
	assert(!cancelled_result);
	assert(cancelled_result.error().code == "source-closure.task-v4-cancelled");
	assert(cancelled_sink.data.empty());

	auto bad = input;
	bad.task_id =
		"task:semantic-v2:sha256:0000000000000000000000000000000000000000000000000000000000000000";
	vector_sink bad_sink;
	auto mismatch = source_closure::encode_task_v4_streaming(bad, bad_sink);
	assert(!mismatch);
	assert(mismatch.error().code == "source-closure.task-v4-id-mismatch");
	assert(bad_sink.data.empty());
	return 0;
}
