#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <cxxlens/sdk/provider.hpp>

namespace cxxlens::detail::clang22
{
	/** Exact installed Clang 22 task.v3 input, independent from native parser state. */
	struct clang22_task_input
	{
		/** Exact global project-catalog authority carried by task.v3. */
		sdk::project_catalog project_catalog;
		/** Catalog-local input identity selected for this translation unit. */
		std::string selected_catalog_compile_unit;
		/** Independently derived build.compile_unit relation identity. */
		std::string compile_unit;
		std::string project;
		std::string variant;
		std::string toolchain_context;
		std::string toolchain_digest;
		struct toolchain_fields
		{
			std::string family;
			std::string exact_version;
			std::string target_triple;
			std::string builtin_headers_digest;
			std::optional<std::string> sysroot;
			std::string abi_digest;
			std::string plugin_spec_digest;
		} toolchain;
		struct variant_fields
		{
			std::string language;
			std::string language_standard;
			std::string target_triple;
			std::string predefined_macros_digest;
			std::string include_search_digest;
			std::string semantic_flags_digest;
		} variant_authority;
		std::string normalized_invocation_digest;
		std::string environment_digest;
		std::string language;
		std::string working_directory;
		std::string condition_universe;
		std::string condition;
		std::string interpretation;
		std::string source_snapshot;
		std::string file;
		std::string logical_path;
		std::string source_content_digest;
		/** Exact request spelling bound by task.v3 (decoded bytes are kept separately below). */
		std::string source_content_base64;
		std::uint64_t source_size_bytes{};
		std::string source_encoding;
		std::string line_index;
		bool source_read_only{};
		std::string source;
		std::vector<std::string> arguments;
		std::vector<std::string> requested_descriptors;
		std::vector<std::string> dependency_groups;
		sdk::provider::execution_budget budget;
		struct sandbox_fields
		{
			std::string minimum;
			std::string policy_digest;
		} sandbox;

		[[nodiscard]] sdk::result<void> validate() const;
	};

	[[nodiscard]] sdk::result<std::vector<std::byte>>
	encode_task_input(const clang22_task_input& input);
	[[nodiscard]] sdk::result<clang22_task_input>
	decode_task_input(std::span<const std::byte> input);
	/** @brief Derive the exact generic condition-ref from the ordered request pair. */
	[[nodiscard]] sdk::result<std::string>
	provider_condition_ref_id(const clang22_task_input& input);
	/** @brief Reconstruct the shared portable task from task.v3 and validated worker authority. */
	[[nodiscard]] sdk::result<sdk::provider::task>
	reconstruct_provider_task(const clang22_task_input& input,
							  std::vector<sdk::relation_descriptor> exact_outputs,
							  std::string provider_semantic_contract_digest);
} // namespace cxxlens::detail::clang22
