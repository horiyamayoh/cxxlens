#pragma once

#include "source_closure.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace cxxlens::detail::clang22::source_closure
{
	struct task_v4
	{
		std::string task_id;
		std::string main_logical_path;
		std::string logical_working_directory;
		std::vector<std::string> effective_arguments;
		std::shared_ptr<const validated_snapshot> closure;
	};

	class task_v4_sink
	{
	  public:
		virtual ~task_v4_sink() = default;
		[[nodiscard]] virtual std::expected<void, validation_error>
		append(std::span<const std::byte> bytes) = 0;
	};

	struct task_v4_options
	{
		std::size_t maximum_chunk_bytes{64U * 1024U};
		std::stop_token cancellation;
	};

	struct task_v4_receipt
	{
		std::string task_id;
		std::string stream_digest;
		std::string source_closure_digest;
		std::uint64_t stream_bytes{};
		std::uint64_t content_chunks{};

		[[nodiscard]] bool operator==(const task_v4_receipt&) const = default;
	};

	/** Derive the task identity from metadata and the immutable closure identity. */
	[[nodiscard]] std::expected<std::string, validation_error>
	derive_task_v4_id(const task_v4& task);

	/**
	 * Emit canonical task.v4 bytes without materializing the complete payload.
	 * Blob payloads are written in bounded chunks, but chunk size is not part of the
	 * canonical byte stream and therefore cannot alter stream_digest or task_id.
	 */
	[[nodiscard]] std::expected<task_v4_receipt, validation_error>
	encode_task_v4_streaming(
		const task_v4& task, task_v4_sink& sink, task_v4_options options = {});
} // namespace cxxlens::detail::clang22::source_closure
