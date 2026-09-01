#pragma once

/**
 * @file application_analysis_internal.hpp
 * @brief Source-private immutable projection shared by capture decode and replay planning.
 */

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <cxxlens/sdk/application_analysis.hpp>

namespace cxxlens::sdk::detail
{
	struct decoded_capture_path_mapping
	{
		std::string physical_prefix;
		std::string logical_prefix;
	};

	struct decoded_capture_unit
	{
		std::string compile_unit_id;
		std::optional<std::string> source_snapshot_id;
		std::string source_file_id;
		std::string source_logical_path;
		std::string source_content_digest;
		std::uint64_t source_size_bytes{};
		std::string logical_working_directory;
		std::string language;
		std::optional<std::vector<std::string>> original_arguments;
		std::optional<std::string> language_standard;
		std::optional<std::string> extension_mode;
		std::string source_closure_id;
		std::string source_closure_digest;
	};

	struct decoded_capture_projection
	{
		std::string toolchain_family;
		std::string toolchain_version;
		std::string target_triple;
		std::string target_abi;
		std::string project_id;
		std::string logical_project_root;
		std::vector<decoded_capture_path_mapping> path_mappings;
		std::vector<decoded_capture_unit> compile_units;
	};

	struct replay_option_mapping
	{
		std::string production_token;
		std::vector<std::string> replay_tokens;
		replay_fidelity fidelity{replay_fidelity::unsupported};
		std::string affected_scope;
		std::string reason;
		std::string completion_action;
	};
} // namespace cxxlens::sdk::detail

namespace cxxlens::sdk
{
	struct capture_bundle::implementation
	{
		canonical_value root;
		std::string digest;
		std::string production_compiler;
		std::string capture_adapter;
		std::string target_abi;
		std::string project_id;
		std::string logical_project_root;
		std::size_t compile_unit_count{};
		std::vector<capture_gap> gaps;
		detail::decoded_capture_projection projection;
	};

	struct replay_plan::implementation
	{
		std::string digest;
		std::string capture_bundle_digest;
		std::string compile_unit_id;
		std::string analysis_frontend;
		std::string target_abi;
		std::vector<std::string> effective_arguments;
		std::vector<detail::replay_option_mapping> option_mappings;
		std::string source_closure_digest;
		std::vector<capture_gap> unresolved;
	};

	struct imported_project::implementation
	{
		std::string id;
		std::string capture_bundle_digest;
		std::vector<replay_plan> replay_plans;
		std::vector<capture_gap> unresolved;
	};
} // namespace cxxlens::sdk
