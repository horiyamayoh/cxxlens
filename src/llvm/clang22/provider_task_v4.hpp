#pragma once

/** @file provider_task_v4.hpp @brief Metadata-only request-v2.2/task-v4 authority. */

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "materialization_json.hpp"

namespace cxxlens::detail::clang22
{
	inline constexpr std::string_view provider_task_v4_schema = "cxxlens.clang22.task.v4";
	inline constexpr std::string_view source_closure_manifest_schema =
		"cxxlens.source-closure-manifest.v1";
	// The task-v4 manifest and the runtime source-closure snapshot share one semantic identity.
	// Keeping the domain here identical to source_closure.cpp prevents a sealed worker snapshot
	// from becoming a foreign manifest merely because it crossed the task-v4 boundary.
	inline constexpr std::string_view source_closure_digest_domain =
		"cxxlens.clang22.source-closure.v1";
	inline constexpr std::string_view source_closure_manifest_digest_domain =
		"cxxlens.source-closure-manifest.v1";
	inline constexpr std::string_view task_v4_digest_domain = "cxxlens.clang22.task.v4";

	/** Bounds shared by request, manifest, and source-closure transport admission. */
	struct provider_task_v4_limits
	{
		std::size_t maximum_members{4096U};
		std::size_t maximum_unique_blobs{4096U};
		std::size_t maximum_arguments{4096U};
		std::size_t maximum_logical_path_bytes{4096U};
		std::uint64_t maximum_blob_bytes{16U * 1024U * 1024U};
		std::uint64_t maximum_unique_blob_bytes{48U * 1024U * 1024U};
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
} // namespace cxxlens::detail::clang22

namespace cxxlens::detail::clang22::materialization
{
	using ::cxxlens::detail::clang22::provider_task_v4;
	using ::cxxlens::detail::clang22::provider_task_v4_base_task;
	using ::cxxlens::detail::clang22::provider_task_v4_input_authority;
	using ::cxxlens::detail::clang22::provider_task_v4_limits;
	using ::cxxlens::detail::clang22::provider_task_v4_open_task;
	using ::cxxlens::detail::clang22::provider_task_v4_source;
	using ::cxxlens::detail::clang22::source_closure_manifest;
	using ::cxxlens::detail::clang22::source_closure_manifest_blob;
	using ::cxxlens::detail::clang22::source_closure_manifest_member;
	using ::cxxlens::detail::clang22::source_closure_summary;
} // namespace cxxlens::detail::clang22::materialization
