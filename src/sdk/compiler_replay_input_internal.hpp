#pragma once

/**
 * @file compiler_replay_input_internal.hpp
 * @brief Bounded canonical host-to-worker input for a validated compiler replay plan.
 */

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "application_analysis_internal.hpp"

namespace cxxlens::sdk::detail
{
	struct compiler_replay_input_draft
	{
		std::string imported_project_id;
		std::string capture_bundle_digest;
		std::string replay_plan_digest;
		std::string compile_unit_id;
		std::string analysis_frontend;
		std::string target_abi;
		std::vector<std::string> effective_arguments;
		std::string source_closure_digest;
		std::vector<decoded_capture_source_member> source_members;
		std::vector<std::string> requested_relation_descriptor_ids;
		std::string interpretation;
		std::vector<capture_gap> unresolved;
	};

	class validated_compiler_replay_input
	{
	  public:
		[[nodiscard]] const compiler_replay_input_draft& value() const noexcept
		{
			return value_;
		}
		[[nodiscard]] std::span<const std::byte> bytes() const noexcept
		{
			return bytes_;
		}
		[[nodiscard]] std::string_view input_digest() const noexcept
		{
			return input_digest_;
		}

	  private:
		validated_compiler_replay_input(compiler_replay_input_draft value,
										std::vector<std::byte> bytes,
										std::string input_digest)
			: value_{std::move(value)}, bytes_{std::move(bytes)},
			  input_digest_{std::move(input_digest)}
		{
		}

		compiler_replay_input_draft value_;
		std::vector<std::byte> bytes_;
		std::string input_digest_;

		friend result<validated_compiler_replay_input>
			validate_compiler_replay_input(compiler_replay_input_draft, import_limits);
	};

	/** Validate the exact frontend, ABI, and executable-mode tuple admitted by Phase 3. */
	[[nodiscard]] result<void>
	validate_compiler_replay_frontend(std::string_view analysis_frontend,
									  std::string_view target_abi,
									  std::span<const std::string> effective_arguments);

	/** Validate, canonicalize set-like fields, and bind the complete worker input digest. */
	[[nodiscard]] result<validated_compiler_replay_input>
	validate_compiler_replay_input(compiler_replay_input_draft draft, import_limits limits = {});

	/** Strictly decode one complete canonical worker input and re-run the same validator. */
	[[nodiscard]] result<validated_compiler_replay_input>
	decode_compiler_replay_input(std::span<const std::byte> bytes, import_limits limits = {});

	/** Build one input only from an imported capture and its validated replay plan. */
	[[nodiscard]] result<validated_compiler_replay_input>
	make_compiler_replay_input(const imported_project::implementation& project,
							   const replay_plan::implementation& plan,
							   std::span<const std::string> requested_relation_descriptor_ids,
							   std::string_view interpretation,
							   import_limits limits = {},
							   std::string_view materialized_compile_unit_id = {});
} // namespace cxxlens::sdk::detail
