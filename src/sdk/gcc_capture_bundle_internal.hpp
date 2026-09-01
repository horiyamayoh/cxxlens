#pragma once

/**
 * @file gcc_capture_bundle_internal.hpp
 * @brief Canonical GCC capture projection from explicit build observations.
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "compile_commands_capture_internal.hpp"

namespace cxxlens::sdk::detail
{
	enum class capture_observation_state : std::uint8_t
	{
		observed,
		derived,
		redacted,
		unavailable,
	};

	struct captured_text_observation
	{
		capture_observation_state state{capture_observation_state::unavailable};
		std::optional<std::string> value;
		std::string reason;
		std::string completion_action;
	};

	struct gcc_toolchain_observation
	{
		std::string exact_version;
		captured_text_observation canonical_binary_path;
		captured_text_observation binary_digest;
		std::string target_triple;
		captured_text_observation sysroot;
		captured_text_observation abi_digest;
		captured_text_observation builtin_headers_digest;
		captured_text_observation builtin_macros_digest;
		captured_text_observation include_search_digest;
	};

	struct gcc_source_observation
	{
		std::vector<std::byte> content;
		std::string encoding{"binary_or_unknown"};
		// Empty only for pure projector callers. Filesystem-backed capture supplies both values
		// from the same opened objects that yielded the bytes.
		std::string canonical_source_path;
		std::string canonical_working_directory;
	};

	struct gcc_compile_commands_bundle_input
	{
		std::string project_id;
		std::string physical_project_root;
		gcc_toolchain_observation toolchain;
		std::vector<gcc_source_observation> sources;
	};

	/**
	 * Project one decoded compilation database and explicit source/toolchain observations into the
	 * canonical capture bundle. The resulting bytes are accepted only after decode_capture_bundle()
	 * independently revalidates every identity, census, gap, and ordering invariant.
	 */
	[[nodiscard]] result<std::vector<std::byte>>
	encode_gcc_compile_commands_bundle(const compile_commands_capture& capture,
									   const gcc_compile_commands_bundle_input& input,
									   import_limits limits = {});
} // namespace cxxlens::sdk::detail
