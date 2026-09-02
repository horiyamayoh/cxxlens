#pragma once

/**
 * @file gcc_replay_input_internal.hpp
 * @brief Bounded canonical host-to-worker input for Clang 23 GCC-mode replay.
 */

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "application_analysis_internal.hpp"

namespace cxxlens::sdk::detail
{
	struct gcc_replay_input_draft
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

	class validated_gcc_replay_input
	{
	  public:
		[[nodiscard]] const gcc_replay_input_draft& value() const noexcept
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
		validated_gcc_replay_input(gcc_replay_input_draft value,
								   std::vector<std::byte> bytes,
								   std::string input_digest)
			: value_{std::move(value)}, bytes_{std::move(bytes)},
			  input_digest_{std::move(input_digest)}
		{
		}

		gcc_replay_input_draft value_;
		std::vector<std::byte> bytes_;
		std::string input_digest_;

		friend result<validated_gcc_replay_input> validate_gcc_replay_input(gcc_replay_input_draft,
																			import_limits);
	};

	/** Validate, canonicalize set-like fields, and bind the complete worker input digest. */
	[[nodiscard]] result<validated_gcc_replay_input>
	validate_gcc_replay_input(gcc_replay_input_draft draft, import_limits limits = {});

	/** Strictly decode one complete canonical worker input and re-run the same validator. */
	[[nodiscard]] result<validated_gcc_replay_input>
	decode_gcc_replay_input(std::span<const std::byte> bytes, import_limits limits = {});

	/** Build one input only from an imported capture and its validated replay plan. */
	[[nodiscard]] result<validated_gcc_replay_input>
	make_gcc_replay_input(const imported_project::implementation& project,
						  const replay_plan::implementation& plan,
						  std::span<const std::string> requested_relation_descriptor_ids,
						  std::string_view interpretation,
						  import_limits limits = {});
} // namespace cxxlens::sdk::detail
