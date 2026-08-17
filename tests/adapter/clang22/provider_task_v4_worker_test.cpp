#include "llvm/clang22/provider_task_v4_worker.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/Basic/SourceManager.h>

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

	[[nodiscard]] source_closure::blob blob(std::string value)
	{
		auto content = bytes(value);
		return {source_closure::sha256_digest(content), std::move(content)};
	}

	[[nodiscard]] source_closure::task_v4 make_task(const bool missing)
	{
		auto main = blob(std::string{"#include \""} +
			(missing ? "ambient.hpp" : "nested.hpp") +
			"\"\nint main(){return nested();}\n");
		auto nested = blob(
			"#include \"config.hpp\"\ninline int nested(){return VALUE;}\n");
		auto generated = blob("#define VALUE 9\n");
		const auto provenance =
			source_closure::sha256_digest(bytes("worker-test"));
		source_closure::snapshot input;
		input.files = {
			{"project://src/main.cpp", main.content_digest, main.content.size(),
				source_closure::file_role::main_source, provenance},
			{"project://include/nested.hpp", nested.content_digest,
				nested.content.size(), source_closure::file_role::project_header,
				provenance},
			{"generated://build/config.hpp", generated.content_digest,
				generated.content.size(), source_closure::file_role::generated_header,
				provenance},
		};
		input.blobs = {main, nested, generated};
		auto closure = source_closure::validate(std::move(input));
		assert(closure);
		source_closure::task_v4 task;
		task.main_logical_path = "project://src/main.cpp";
		task.logical_working_directory = "project://";
		task.effective_arguments = {"clang++", "-std=c++23",
			"-Iproject://include", "-Igenerated://build",
			"project://src/main.cpp"};
		task.closure = std::make_shared<const source_closure::validated_snapshot>(
			std::move(*closure));
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
		explicit replay(std::vector<std::byte> value) : data{std::move(value)} {}

		std::expected<std::size_t, source_closure::validation_error>
		read_at(std::uint64_t offset,
			std::span<std::byte> destination) override
		{
			const auto count = std::min<std::size_t>(7U,
				std::min(destination.size(),
					data.size() - static_cast<std::size_t>(offset)));
			std::copy_n(data.data() + offset, count, destination.data());
			return count;
		}

		std::uint64_t size_bytes() const noexcept override { return data.size(); }
		bool sealed() const noexcept override { return true; }
		std::vector<std::byte> data;
	};

	[[nodiscard]] replay encoded(const bool missing)
	{
		auto task = make_task(missing);
		sink output;
		auto encoded = source_closure::encode_task_v4_streaming(task, output);
		assert(encoded);
		return replay{std::move(output.data)};
	}
} // namespace

int main()
{
	bool called{};
	auto valid_replay = encoded(false);
	auto valid = source_closure::execute_task_v4_worker(valid_replay,
		[&](clang::ASTContext& context, clang::SourceManager&)
			-> std::expected<void, source_closure::clang_run_error>
		{
			called = context.getTranslationUnitDecl() != nullptr;
			return {};
		});
	assert(valid);
	assert(called);
	assert(valid->decoded_task_bytes == valid_replay.data.size());

	called = false;
	auto missing_replay = encoded(true);
	auto missing = source_closure::execute_task_v4_worker(missing_replay,
		[&](clang::ASTContext&, clang::SourceManager&)
			-> std::expected<void, source_closure::clang_run_error>
		{
			called = true;
			return {};
		});
	assert(!missing);
	assert(missing.error().code == "source-closure.clang-parse-failed");
	assert(!called);

	auto malformed_bytes = encoded(false).data;
	malformed_bytes[0U] = std::byte{0xffU};
	replay malformed{std::move(malformed_bytes)};
	called = false;
	auto rejected = source_closure::execute_task_v4_worker(malformed,
		[&](clang::ASTContext&, clang::SourceManager&)
			-> std::expected<void, source_closure::clang_run_error>
		{
			called = true;
			return {};
		});
	assert(!rejected);
	assert(rejected.error().code == "source-closure.task-v4-tag");
	assert(!called);
	return 0;
}
