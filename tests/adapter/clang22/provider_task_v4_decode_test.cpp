#include "llvm/clang22/provider_task_v4_decode.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <memory>
#include <span>
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

	[[nodiscard]] source_closure::task_v4 make_task()
	{
		auto main_content =
			bytes("#include \"h.hpp\"\nint main(){return f();}\n");
		auto header_content = bytes("inline int f(){return 3;}\n");
		source_closure::blob main{
			source_closure::sha256_digest(main_content), std::move(main_content)};
		source_closure::blob header{
			source_closure::sha256_digest(header_content),
			std::move(header_content)};
		const auto provenance =
			source_closure::sha256_digest(bytes("decode-test"));
		source_closure::snapshot input;
		input.files = {
			{"project://src/main.cpp", main.content_digest, main.content.size(),
				source_closure::file_role::main_source, provenance},
			{"project://include/h.hpp", header.content_digest,
				header.content.size(), source_closure::file_role::project_header,
				provenance},
		};
		input.blobs = {main, header};
		auto validated = source_closure::validate(std::move(input));
		assert(validated);
		source_closure::task_v4 task;
		task.main_logical_path = "project://src/main.cpp";
		task.logical_working_directory = "project://";
		task.effective_arguments = {
			"clang++", "-Iproject://include", "project://src/main.cpp"};
		task.closure = std::make_shared<const source_closure::validated_snapshot>(
			std::move(*validated));
		auto id = source_closure::derive_task_v4_id(task);
		assert(id);
		task.task_id = *id;
		return task;
	}

	class sink final : public source_closure::task_v4_sink
	{
	  public:
		std::expected<void, source_closure::validation_error>
		append(std::span<const std::byte> value) override
		{
			data.insert(data.end(), value.begin(), value.end());
			return {};
		}
		std::vector<std::byte> data;
	};

	class replay final : public source_closure::task_v4_replay
	{
	  public:
		replay(std::vector<std::byte> value,
			std::size_t maximum_read = ~std::size_t{})
			: data{std::move(value)}, maximum_read{maximum_read}
		{
		}

		std::expected<std::size_t, source_closure::validation_error>
		read_at(std::uint64_t offset,
			std::span<std::byte> destination) override
		{
			if (offset > data.size())
				return std::unexpected(source_closure::validation_error{
					"test.offset", "offset", {}});
			const auto count = std::min({destination.size(),
				data.size() - static_cast<std::size_t>(offset), maximum_read});
			std::copy_n(data.data() + offset, count, destination.data());
			return count;
		}

		std::uint64_t size_bytes() const noexcept override { return data.size(); }
		bool sealed() const noexcept override { return is_sealed; }

		std::vector<std::byte> data;
		std::size_t maximum_read;
		bool is_sealed{true};
	};

	[[nodiscard]] std::vector<std::byte> encoded()
	{
		auto task = make_task();
		sink output;
		auto result = source_closure::encode_task_v4_streaming(task, output);
		assert(result);
		return output.data;
	}
} // namespace

int main()
{
	replay short_reads{encoded(), 3U};
	auto decoded = source_closure::decode_task_v4(short_reads);
	assert(decoded);
	assert(decoded->consumed_bytes == short_reads.data.size());
	assert(decoded->task.task_id == make_task().task_id);
	assert(decoded->task.closure->files.size() == 2U);

	auto trailing_bytes = encoded();
	trailing_bytes.push_back(std::byte{0U});
	replay trailing{std::move(trailing_bytes)};
	auto trailing_result = source_closure::decode_task_v4(trailing);
	assert(!trailing_result);
	assert(trailing_result.error().code ==
		"source-closure.task-v4-trailing-bytes");

	auto wrong_tag = encoded();
	wrong_tag[0U] = std::byte{0x7fU};
	replay tagged{std::move(wrong_tag)};
	auto tagged_result = source_closure::decode_task_v4(tagged);
	assert(!tagged_result);
	assert(tagged_result.error().code == "source-closure.task-v4-tag");

	auto truncated_bytes = encoded();
	truncated_bytes.resize(truncated_bytes.size() - 1U);
	replay truncated{std::move(truncated_bytes)};
	auto truncated_result = source_closure::decode_task_v4(truncated);
	assert(!truncated_result);
	assert(truncated_result.error().code ==
		"source-closure.task-v4-truncated");

	replay unsealed{encoded()};
	unsealed.is_sealed = false;
	auto unsealed_result = source_closure::decode_task_v4(unsealed);
	assert(!unsealed_result);
	assert(unsealed_result.error().code ==
		"source-closure.task-v4-unsealed");
	return 0;
}
