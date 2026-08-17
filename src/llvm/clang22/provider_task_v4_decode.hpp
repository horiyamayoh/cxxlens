#pragma once

#include "provider_task_v4.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace cxxlens::detail::clang22::source_closure
{
	class task_v4_replay
	{
	  public:
		virtual ~task_v4_replay() = default;
		[[nodiscard]] virtual std::expected<std::size_t, validation_error>
		read_at(std::uint64_t offset, std::span<std::byte> destination) = 0;
		[[nodiscard]] virtual std::uint64_t size_bytes() const noexcept = 0;
		[[nodiscard]] virtual bool sealed() const noexcept = 0;
	};

	struct task_v4_decode_limits
	{
		std::uint64_t maximum_stream_bytes{128U * 1024U * 1024U};
		std::size_t maximum_string_bytes{4096U};
		std::size_t maximum_arguments{4096U};
		std::size_t maximum_files{4096U};
		std::size_t maximum_blobs{4096U};
	};

	struct decoded_task_v4
	{
		task_v4 task;
		std::uint64_t consumed_bytes{};
	};

	[[nodiscard]] std::expected<decoded_task_v4, validation_error>
	decode_task_v4(task_v4_replay& replay, task_v4_decode_limits limits = {});
} // namespace cxxlens::detail::clang22::source_closure
