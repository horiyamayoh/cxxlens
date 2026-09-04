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
	/** Exact execution/adoption metadata owned by one admitted replay frontend tuple. */
	struct compiler_replay_frontend_contract
	{
		std::string_view analysis_frontend;
		std::string_view target_abi;
		std::string_view executable;
		std::string_view mode_argument;
		std::string_view dependency_group;
		std::string_view output_normalizer;
		std::string_view observation_technique;
	};

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

	/** Resolve the exact frontend, ABI, executable-mode, execution, and adoption tuple. */
	[[nodiscard]] result<compiler_replay_frontend_contract>
	resolve_compiler_replay_frontend(std::string_view analysis_frontend,
									 std::string_view target_abi,
									 std::span<const std::string> effective_arguments);

	/** Whether a technique is issued by one admitted compiler replay frontend contract. */
	[[nodiscard]] bool
	is_compiler_replay_observation_technique(std::string_view observation_technique) noexcept;

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
