#pragma once

/** @file provider_task_v4.hpp @brief Metadata-only request-v2.2/task-v4 authority. */

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/provider.hpp>
#include <cxxlens/sdk/store.hpp>

#include "materialization_json.hpp"
#include "source_closure.hpp"

namespace cxxlens::detail::clang22
{
	inline constexpr std::string_view provider_task_v4_schema = "cxxlens.clang22.task.v4";
	inline constexpr std::string_view task_v4_digest_domain = "cxxlens.clang22.task.v4";
	inline constexpr std::string_view task_v4_authority_digest_domain =
		"cxxlens.clang22.task-authority.v4";
	inline constexpr std::string_view task_v4_registry_path =
		"schemas/cxxlens_ng_relation_registry.yaml";
	inline constexpr std::string_view task_v4_engine_generation_contract =
		"cxxlens.clang22-materialization-engine.v2";
	inline constexpr std::string_view task_v4_interpretation_policy_id =
		"cxxlens.clang22-interpretation-policy.v1";
	inline constexpr std::string_view task_v4_interpretation_domain = "cc.clang22-canonical-1";
	inline constexpr std::string_view task_v4_trust_policy_id =
		"cxxlens.clang22-installed-native-worker-trust.v1";

	inline constexpr std::array<std::string_view, 6U> task_v4_base_descriptor_ids{
		"build.project.v1",
		"build.toolchain_context.v1",
		"build.variant.v1",
		"source.file.v1",
		"build.compile_unit.v1",
		"source.span.v1",
	};
	inline constexpr std::array<std::string_view, 6U> task_v4_output_descriptor_ids{
		"cc.call_direct_target.v1",
		"cc.call_site.v1",
		"cc.entity.v1",
		"frontend.clang22.call_observation.v2",
		"frontend.clang22.entity_observation.v2",
		"frontend.clang22.type_observation.v2",
	};
	inline constexpr std::array<std::string_view, 12U> task_v4_engine_descriptor_ids{
		"build.compile_unit.v1",
		"build.project.v1",
		"build.toolchain_context.v1",
		"build.variant.v1",
		"cc.call_direct_target.v1",
		"cc.call_site.v1",
		"cc.entity.v1",
		"frontend.clang22.call_observation.v2",
		"frontend.clang22.entity_observation.v2",
		"frontend.clang22.type_observation.v2",
		"source.file.v1",
		"source.span.v1",
	};
	inline constexpr std::array<std::string_view, 2U> task_v4_dependency_groups{
		"canonical",
		"observation",
	};

	/** Bounds shared by request, manifest, and source-closure transport admission. */
	struct provider_task_v4_limits
	{
		std::size_t maximum_members{4096U};
		std::size_t maximum_unique_blobs{4096U};
		std::size_t maximum_arguments{4096U};
		std::size_t maximum_logical_path_bytes{4096U};
		std::uint64_t maximum_blob_bytes{std::uint64_t{16U} * 1024U * 1024U};
		std::uint64_t maximum_unique_blob_bytes{std::uint64_t{48U} * 1024U * 1024U};
	};

	/**
	 * The validated invocation authority supplied by the request owner to a task-v4 worker.
	 *
	 * The task-v4 payload carries the invocation digest, while the outer v2.2 task authority
	 * supplies the ordered argv and the explicitly admitted toolchain roots.  Keeping these values
	 * in one typed object prevents a worker from accidentally reconstructing them from ambient
	 * process state or from an unbound string span.  Read roots are runtime inputs and are not part
	 * of the effective-invocation semantic digest; they are nevertheless required to be explicit,
	 * absolute, canonical, and duplicate-free before the compiler is entered.
	 */
	struct provider_task_v4_input_authority
	{
		std::string normalized_invocation_digest;
		std::string logical_working_directory;
		std::vector<std::string> effective_arguments;
		std::vector<std::string> qualified_read_roots;

		/** Validate and bind all invocation fields to the decoded task-v4 metadata. */
		[[nodiscard]] sdk::result<void> validate(std::string_view main_logical_path,
												 std::string_view expected_working_directory,
												 std::string_view expected_invocation_digest) const;

		[[nodiscard]] bool operator==(const provider_task_v4_input_authority&) const = default;
	};

	/** Source-free toolchain identity selected by one task. */
	struct provider_task_v4_toolchain_authority
	{
		std::string family;
		std::string exact_version;
		std::string target_triple;
		std::string builtin_headers_digest;
		std::optional<std::string> sysroot;
		std::string abi_digest;
		std::string plugin_spec_digest;

		[[nodiscard]] sdk::result<void> validate() const;
		[[nodiscard]] bool operator==(const provider_task_v4_toolchain_authority&) const = default;
	};

	/** Source-free build variant identity selected by one task. */
	struct provider_task_v4_variant_authority
	{
		std::string language;
		std::string language_standard;
		std::string target_triple;
		std::string predefined_macros_digest;
		std::string include_search_digest;
		std::string semantic_flags_digest;

		[[nodiscard]] sdk::result<void> validate() const;
		[[nodiscard]] bool operator==(const provider_task_v4_variant_authority&) const = default;
	};

	/** One exact installed Relation Registry base-descriptor binding. */
	struct provider_task_v4_base_descriptor_binding
	{
		std::string descriptor_id;
		sdk::semantic_version descriptor_version;
		std::string contract_digest;
		std::string runtime_descriptor_digest;
		std::uint32_t stage_order{};
		std::string output_stage;
		std::string owner;

		[[nodiscard]] sdk::result<void> validate() const;
		[[nodiscard]] bool
		operator==(const provider_task_v4_base_descriptor_binding&) const = default;
	};

	/** One exact installed Relation Registry output-descriptor binding. */
	struct provider_task_v4_output_descriptor_binding
	{
		std::string descriptor_id;
		sdk::semantic_version descriptor_version;
		std::string contract_digest;
		std::string runtime_descriptor_digest;
		std::string dependency_group_id;
		std::string atomic_output_group_id;
		std::string batch_id;
		std::string output_stage;

		[[nodiscard]] sdk::result<void> validate() const;
		[[nodiscard]] bool
		operator==(const provider_task_v4_output_descriptor_binding&) const = default;
	};

	/** Descriptor identity admitted into the immutable relation-engine generation. */
	struct provider_task_v4_admitted_descriptor_binding
	{
		std::string descriptor_id;
		std::string runtime_descriptor_digest;

		[[nodiscard]] sdk::result<void> validate() const;
		[[nodiscard]] bool
		operator==(const provider_task_v4_admitted_descriptor_binding&) const = default;
	};

	/** Typed global project/catalog authority shared by every v2.2 task. */
	struct provider_task_v4_catalog_authority
	{
		std::string project_id;
		sdk::project_catalog catalog;
		std::string catalog_compile_unit_census_digest;

		[[nodiscard]] sdk::result<void> validate() const;
	};

	/** Typed Relation Registry authority; no repository path is inferred at runtime. */
	struct provider_task_v4_registry_authority
	{
		std::string path;
		std::string authority_registry_digest;
		std::vector<provider_task_v4_base_descriptor_binding> base_descriptors;
		std::vector<provider_task_v4_output_descriptor_binding> descriptors;

		[[nodiscard]] sdk::result<void> validate() const;
		[[nodiscard]] bool operator==(const provider_task_v4_registry_authority&) const = default;
	};

	/** Typed immutable relation-engine generation authority. */
	struct provider_task_v4_engine_authority
	{
		std::string generation_contract;
		std::vector<provider_task_v4_admitted_descriptor_binding> admitted_descriptors;
		std::string engine_registry_digest;
		std::string engine_generation_id;

		[[nodiscard]] sdk::result<void> validate() const;
		[[nodiscard]] bool operator==(const provider_task_v4_engine_authority&) const = default;
	};

	/** Typed interpretation policy bound into the task and publication selector. */
	struct provider_task_v4_interpretation_authority
	{
		std::string policy_id;
		std::string selected_domain;
		std::string interpretation_policy_digest;

		[[nodiscard]] sdk::result<void> validate() const;
		[[nodiscard]] bool
		operator==(const provider_task_v4_interpretation_authority&) const = default;
	};

	/** Typed worker trust policy shared by all task launches. */
	struct provider_task_v4_trust_authority
	{
		std::string policy_id;
		std::string execution_profile;
		std::string provider_id;
		sdk::semantic_version provider_version;
		std::string semantic_contract_digest;
		std::uint16_t protocol_major{};
		std::uint16_t protocol_minor{};
		std::vector<std::string> required_features;
		std::string required_qualification;
		std::string worker_sandbox_policy_digest;
		std::vector<sdk::provider::sandbox_requirement> task_sandbox_requirements;
		std::string trust_policy_digest;

		[[nodiscard]] sdk::result<void> validate() const;
	};

	/** Typed output-group authority; ordering and partiality are contract fields. */
	struct provider_task_v4_group_topology_authority
	{
		std::vector<std::string> dependency_groups;
		std::string atomic_output_group;
		std::string partial_policy;

		[[nodiscard]] sdk::result<void> validate() const;
		[[nodiscard]] bool
		operator==(const provider_task_v4_group_topology_authority&) const = default;
	};

	/** Typed publication target authority; no Store operation is performed by this model. */
	struct provider_task_v4_publication_authority
	{
		std::string backend;
		sdk::snapshot_series_selector selector;
		std::string series_id;
		bool genesis{};
		std::optional<std::string> expected_parent_publication;
		std::optional<std::string> sqlite_path;
		std::string partial_policy;
		std::uint64_t transaction_count{};
		bool reopen_before_success{};
		std::string recipe_id;
		std::string recipe_digest;
		std::string output_plan_digest;
		std::string publication_target;

		[[nodiscard]] sdk::result<void> validate() const;
		[[nodiscard]] bool
		operator==(const provider_task_v4_publication_authority&) const = default;
	};

	/** Typed installed materializer occurrence authority. */
	struct provider_task_v4_tool_authority
	{
		std::string executable;
		std::string interface_version;
		std::string distribution_version;
		std::string source_revision;
		std::string source_tree;
		std::string installed_executable_digest;
		std::string package_configuration;
		std::string occurrence_manifest_digest;

		[[nodiscard]] sdk::result<void> validate() const;
		[[nodiscard]] bool operator==(const provider_task_v4_tool_authority&) const = default;
	};

	/** Typed worker occurrence and Protocol 2.0 identity authority. */
	struct provider_task_v4_worker_authority
	{
		std::string executable;
		std::string provider_id;
		sdk::semantic_version provider_version;
		std::string installed_binary_digest;
		std::string semantic_contract_digest;
		std::uint16_t protocol_major{};
		std::uint16_t protocol_minor{};
		std::vector<std::string> required_features;
		std::string sandbox_policy_digest;

		[[nodiscard]] sdk::result<void> validate() const;
		[[nodiscard]] bool operator==(const provider_task_v4_worker_authority&) const = default;
	};

	/** One source-closure member's metadata; source bytes are deliberately absent. */
	struct source_closure_manifest_member
	{
		std::string file_id;
		std::string logical_path;
		std::string role;
		std::string encoding;
		std::uint64_t size_bytes{};
		std::string content_digest;
		bool read_only{};

		[[nodiscard]] bool operator==(const source_closure_manifest_member&) const = default;
	};

	/** One digest-addressed blob descriptor; the blob payload is transported separately. */
	struct source_closure_manifest_blob
	{
		std::string content_digest;
		std::uint64_t size_bytes{};

		[[nodiscard]] bool operator==(const source_closure_manifest_blob&) const = default;
	};

	/** Complete manifest metadata, without member or blob bytes. */
	struct source_closure_manifest
	{
		std::string schema{source_closure_manifest_schema};
		std::string closure_id;
		std::string closure_digest;
		std::string manifest_digest;
		std::vector<source_closure_manifest_member> members;
		std::vector<source_closure_manifest_blob> blobs;

		[[nodiscard]] sdk::result<void> validate(provider_task_v4_limits limits = {}) const;
	};

	/** The compact closure summary carried in request v2.2 before transfer. */
	struct source_closure_summary
	{
		std::string source_closure_id;
		std::string source_closure_digest;
		std::string manifest_digest;
		std::uint64_t member_count{};
		std::uint64_t blob_count{};
		std::uint64_t unique_blob_bytes{};

		[[nodiscard]] sdk::result<void> validate(provider_task_v4_limits limits = {}) const;
		[[nodiscard]] bool operator==(const source_closure_summary&) const = default;
	};

	/** Source metadata retained by a v2.2 base task; no source bytes are representable. */
	struct provider_task_v4_source
	{
		std::string source_snapshot_id;
		std::string file_id;
		std::string logical_path;
		std::string content_digest;
		std::uint64_t size_bytes{};
		std::string encoding;
		std::string line_index_id;
		bool read_only{};

		[[nodiscard]] sdk::result<void> validate(provider_task_v4_limits limits = {}) const;
		[[nodiscard]] bool operator==(const provider_task_v4_source&) const = default;
	};

	/** Derive the exact effective-invocation semantic digest from ordered argv and workdir. */
	[[nodiscard]] sdk::result<std::string> derive_provider_task_v4_effective_invocation_digest(
		std::string_view logical_working_directory,
		std::span<const std::string> effective_arguments);

	/** The exact open-task fields cross-bound into task v4. */
	struct provider_task_v4_open_task
	{
		std::string task_input_digest;
		std::string normalized_invocation_digest;
		std::string toolchain_digest;
		std::string environment_digest;

		[[nodiscard]] sdk::result<void> validate() const;
		[[nodiscard]] bool operator==(const provider_task_v4_open_task&) const = default;
	};

	/** Metadata binding from one v2.2 base task, sufficient for task-v4 cross-checks. */
	struct provider_task_v4_base_task
	{
		std::string provider_task_id;
		std::string provider_execution_id;
		/** Digest of the complete source-free v2.2 base-task projection. */
		std::string canonical_base_task_digest;
		std::string task_input_digest;
		std::string normalized_invocation_digest;
		std::string toolchain_digest;
		std::string environment_digest;
		std::string working_directory;
		provider_task_v4_source source;

		[[nodiscard]] sdk::result<void> validate(provider_task_v4_limits limits = {}) const;
		[[nodiscard]] bool operator==(const provider_task_v4_base_task&) const = default;
	};

	/**
	 * Complete source-free v2.2 task authority used to assemble task-v4 input.
	 *
	 * The compact task-v4 extension remains a closure binding.  This value carries the independent
	 * catalog/build/invocation/output authority which the worker must receive from the request
	 * owner.  It deliberately contains no source bytes, JSON DOM, process handle, or Store result.
	 */
	struct provider_task_v4_task_authority
	{
		std::string provider_task_id;
		std::string provider_execution_id;
		std::string task_input_digest;
		std::string project_id;
		std::string catalog_id;
		std::string catalog_digest;
		std::string selected_catalog_compile_unit_id;
		std::string compile_unit_id;
		std::string build_variant_id;
		std::string toolchain_context_id;
		std::string toolchain_digest;
		provider_task_v4_toolchain_authority toolchain;
		provider_task_v4_variant_authority variant;
		std::string normalized_invocation_digest;
		std::string environment_digest;
		std::string language;
		std::string working_directory;
		std::string condition_universe_id;
		std::string condition_id;
		std::string interpretation_domain;
		provider_task_v4_source source;
		provider_task_v4_input_authority input_authority;
		std::vector<std::string> requested_descriptor_ids;
		std::vector<std::string> dependency_groups;
		sdk::provider::execution_budget budget;
		sdk::provider::sandbox_requirement sandbox;

		[[nodiscard]] sdk::result<void>
		validate(const provider_task_v4_catalog_authority& catalog_authority,
				 const provider_task_v4_group_topology_authority& group_topology) const;
	};

	/**
	 * Complete typed Protocol 2.2 request authority.
	 *
	 * This is the source-free handoff value for the request-v2.2 bridge. Validation is
	 * independent of JSON transport validation: a JSON document may be decoded for framing, but it
	 * cannot manufacture this authority.  The canonical projection is deterministic and contains
	 * only typed metadata and digests.
	 */
	struct provider_task_v4_request_authority
	{
		provider_task_v4_tool_authority tool;
		provider_task_v4_worker_authority worker;
		provider_task_v4_catalog_authority project;
		provider_task_v4_registry_authority registry;
		provider_task_v4_engine_authority engine;
		provider_task_v4_interpretation_authority interpretation_policy;
		provider_task_v4_trust_authority trust_policy;
		provider_task_v4_group_topology_authority group_topology;
		std::vector<provider_task_v4_task_authority> tasks;
		provider_task_v4_publication_authority publication;

		[[nodiscard]] sdk::result<void> validate() const;
		[[nodiscard]] sdk::result<std::vector<std::byte>> canonical_projection() const;
		[[nodiscard]] sdk::result<std::string> authority_digest() const;
	};

	/** One task-v4 closure extension. It owns no source or manifest bytes. */
	struct provider_task_v4
	{
		std::string schema{provider_task_v4_schema};
		std::string task_id;
		std::string task_v4_digest;
		std::uint64_t base_task_index{};
		std::string base_provider_task_id;
		/** Digest binding for the complete source-free base-task projection. */
		std::string base_task_digest;
		provider_task_v4_open_task open_task;
		source_closure_summary source_closure;
		std::string main_logical_path;
		std::string logical_working_directory;

		[[nodiscard]] sdk::result<void> validate(provider_task_v4_limits limits = {}) const;
		[[nodiscard]] bool operator==(const provider_task_v4&) const = default;
	};

	/** Derive the semantic closure identity from a complete metadata-only manifest. */
	[[nodiscard]] sdk::result<std::string>
	derive_source_closure_digest(const source_closure_manifest& manifest);
	/** Derive the semantic digest of a complete canonical metadata-only manifest. */
	[[nodiscard]] sdk::result<std::string>
	derive_source_closure_manifest_digest(const source_closure_manifest& manifest);
	/** Derive the semantic task-v4 digest, excluding task_id and task_v4_digest. */
	[[nodiscard]] sdk::result<std::string>
	derive_provider_task_v4_digest(const provider_task_v4& task);
	/** Return the canonical JSON task-v4 projection used by the identity function. */
	[[nodiscard]] sdk::result<materialization::json_value>
	provider_task_v4_identity_projection(const provider_task_v4& task);

	/** Validate a task's declared identity after recomputing the task-v4 projection. */
	[[nodiscard]] sdk::result<void>
	validate_provider_task_v4_identity(const provider_task_v4& task);

	/** Validate a source summary against the separately transferred metadata manifest. */
	[[nodiscard]] sdk::result<void>
	bind_source_closure_summary(const source_closure_summary& summary,
								const source_closure_manifest& manifest,
								provider_task_v4_limits limits = {});

	/** Validate the main member metadata binding required by task v4. */
	[[nodiscard]] sdk::result<void>
	bind_provider_task_v4_main_member(const provider_task_v4_base_task& base,
									  const provider_task_v4& task,
									  const source_closure_manifest& manifest,
									  provider_task_v4_limits limits = {});

	/**
	 * Validate the main member against the authenticated transferred source closure as well as
	 * its metadata manifest.  This overload is the pre-acceptance boundary for line-index binding;
	 * the manifest-only overload cannot authenticate source bytes and must not be used for it.
	 */
	[[nodiscard]] sdk::result<void>
	bind_provider_task_v4_main_member(const provider_task_v4_base_task& base,
									  const provider_task_v4& task,
									  const source_closure_manifest& manifest,
									  const source_closure_snapshot& snapshot,
									  provider_task_v4_limits limits = {});
} // namespace cxxlens::detail::clang22

namespace cxxlens::detail::clang22::materialization
{
	using ::cxxlens::detail::clang22::provider_task_v4;
	using ::cxxlens::detail::clang22::provider_task_v4_admitted_descriptor_binding;
	using ::cxxlens::detail::clang22::provider_task_v4_base_descriptor_binding;
	using ::cxxlens::detail::clang22::provider_task_v4_base_task;
	using ::cxxlens::detail::clang22::provider_task_v4_catalog_authority;
	using ::cxxlens::detail::clang22::provider_task_v4_engine_authority;
	using ::cxxlens::detail::clang22::provider_task_v4_group_topology_authority;
	using ::cxxlens::detail::clang22::provider_task_v4_input_authority;
	using ::cxxlens::detail::clang22::provider_task_v4_interpretation_authority;
	using ::cxxlens::detail::clang22::provider_task_v4_limits;
	using ::cxxlens::detail::clang22::provider_task_v4_open_task;
	using ::cxxlens::detail::clang22::provider_task_v4_output_descriptor_binding;
	using ::cxxlens::detail::clang22::provider_task_v4_publication_authority;
	using ::cxxlens::detail::clang22::provider_task_v4_registry_authority;
	using ::cxxlens::detail::clang22::provider_task_v4_request_authority;
	using ::cxxlens::detail::clang22::provider_task_v4_source;
	using ::cxxlens::detail::clang22::provider_task_v4_task_authority;
	using ::cxxlens::detail::clang22::provider_task_v4_tool_authority;
	using ::cxxlens::detail::clang22::provider_task_v4_toolchain_authority;
	using ::cxxlens::detail::clang22::provider_task_v4_trust_authority;
	using ::cxxlens::detail::clang22::provider_task_v4_variant_authority;
	using ::cxxlens::detail::clang22::provider_task_v4_worker_authority;
	using ::cxxlens::detail::clang22::source_closure_manifest;
	using ::cxxlens::detail::clang22::source_closure_manifest_blob;
	using ::cxxlens::detail::clang22::source_closure_manifest_member;
	using ::cxxlens::detail::clang22::source_closure_summary;
} // namespace cxxlens::detail::clang22::materialization
