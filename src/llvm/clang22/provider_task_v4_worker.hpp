#pragma once

#include "clang_compiler_vfs.hpp"
#include "provider_task_v4_decode.hpp"

#include <cstdint>
#include <expected>
#include <string>

namespace cxxlens::detail::clang22::source_closure
{
	struct task_v4_worker_receipt
	{
		std::string task_id;
		std::string source_closure_id;
		std::string main_logical_path;
		std::uint64_t decoded_task_bytes{};

		[[nodiscard]] bool operator==(const task_v4_worker_receipt&) const = default;
	};

	/** Decode and execute one sealed task.v4 without ambient project/generated fallback. */
	[[nodiscard]] std::expected<task_v4_worker_receipt, validation_error>
	execute_task_v4_worker(task_v4_replay& replay,
		clang_translation_unit_callback callback,
		task_v4_decode_limits decode_limits = {});
} // namespace cxxlens::detail::clang22::source_closure
