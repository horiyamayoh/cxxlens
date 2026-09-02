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

#include "build_capture_internal.hpp"
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

	/** One captured non-main member of a compile unit's complete source closure. */
	struct gcc_source_closure_member_observation
	{
		std::string canonical_path;
		std::vector<std::byte> content;
		std::string encoding{"binary_or_unknown"};
		std::string role{"header"};
	};

	/** Invocation details supplied by the capture adapter rather than reconstructed later. */
	struct gcc_invocation_observation
	{
		captured_value<std::vector<build_capture_auxiliary_file>> response_files =
			captured_value<std::vector<build_capture_auxiliary_file>>::unavailable(
				"response-files-unobserved", "recapture-with-shell-free-wrapper");
		captured_value<std::vector<build_capture_auxiliary_file>> config_files =
			captured_value<std::vector<build_capture_auxiliary_file>>::unavailable(
				"config-files-unobserved", "recapture-with-shell-free-wrapper");
		captured_value<std::vector<build_capture_environment_effect>> environment_effects =
			captured_value<std::vector<build_capture_environment_effect>>::unavailable(
				"environment-effects-unobserved", "recapture-with-shell-free-wrapper");
		/** Observed non-main members; they may be retained even when membership is incomplete. */
		std::vector<gcc_source_closure_member_observation> source_closure_members;
		/** Exact membership coverage is independent from the retained member observations. */
		captured_value<std::string> source_closure_membership =
			captured_value<std::string>::unavailable(
				"dependency-output-unobserved",
				"recapture-with-shell-free-wrapper-or-run-dependency-probe");
	};

	struct gcc_compile_commands_bundle_input
	{
		std::string project_id;
		std::string physical_project_root;
		gcc_toolchain_observation toolchain;
		std::vector<gcc_source_observation> sources;
		/** Empty selects the compilation-database unavailable observations for every unit. */
		std::vector<gcc_invocation_observation> invocations;
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
