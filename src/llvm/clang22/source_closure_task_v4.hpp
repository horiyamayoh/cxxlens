#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "source_closure.hpp"

namespace cxxlens::detail::clang22
{
	/**
	 * The task-v4 authority which is available before transport framing.
	 *
	 * `base_task_projection` is the complete canonical v2.2 base-task projection with source
	 * bytes removed.  It is deliberately supplied by the request validator rather than rebuilt
	 * here: this seam can therefore bind the v4 extension to the exact inherited authority without
	 * silently inventing a second projection.
	 */
	struct source_closure_task_v4_input
	{
		std::uint64_t base_task_index{};
		std::string base_provider_task_id;
		std::vector<std::byte> base_task_projection;
		std::string task_input_digest;
		std::string normalized_invocation_digest;
		std::string toolchain_digest;
		std::string environment_digest;
		source_closure_snapshot closure;
		std::string main_logical_path;
		std::string logical_working_directory;
	};

	/** Exact independently-derived v4 identity and payload bytes. */
	struct source_closure_task_v4_identity
	{
		std::string task_id;
		std::string task_v4_digest;
		std::string base_task_digest;
		std::string manifest_digest;
		std::string task_v4_input_digest;
		std::vector<std::byte> semantic_projection;
		std::vector<std::byte> input_payload;

		[[nodiscard]] bool operator==(const source_closure_task_v4_identity&) const = default;
	};

	struct source_closure_task_v4_decoded
	{
		source_closure_task_v4_input input;
		source_closure_task_v4_identity identity;
	};

	inline constexpr std::uint64_t source_closure_task_v4_maximum_payload_bytes =
		std::uint64_t{88U} * 1024U * 1024U;

	/** Build the canonical source-closure manifest digest without retaining blob bytes in it. */
	[[nodiscard]] sdk::result<std::string>
	source_closure_manifest_digest(const source_closure_snapshot& closure);

	/**
	 * Derive task.v4 semantic identity and the recursion-free input payload.
	 *
	 * The semantic projection excludes `task_id`, `task_v4_digest`, and the input payload digest.
	 * The input payload includes the already-derived task ID/digest and excludes only its own
	 * content digest.  Consequently no digest is computed over bytes that contain that digest.
	 */
	[[nodiscard]] sdk::result<source_closure_task_v4_identity>
	derive_source_closure_task_v4_identity(const source_closure_task_v4_input& input);

	/**
	 * Decode one complete canonical task-v4 input payload and rebind it to the independently
	 * assembled closure, base-task digest, and outer input-descriptor digest.  This function does
	 * not read a path, open a process, or consult an ambient cache.
	 */
	[[nodiscard]] sdk::result<source_closure_task_v4_decoded>
	decode_source_closure_task_v4_input(std::span<const std::byte> payload,
										const source_closure_snapshot& closure,
										std::string_view expected_base_task_digest,
										std::string_view expected_task_v4_input_digest);

	/** Check the content digest used by open_task/input_descriptor before any bytes are consumed.
	 */
	[[nodiscard]] sdk::result<void>
	validate_source_closure_task_v4_input_digest(std::span<const std::byte> payload,
												 std::string_view expected_input_digest);
} // namespace cxxlens::detail::clang22
