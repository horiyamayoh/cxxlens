#pragma once

/**
 * @file provider_task_v4_authority.hpp
 * @brief Source-private, move-only authority for one request-v2.2/task-v4 occurrence.
 *
 * This header is deliberately not an installed SDK surface.  The identity projection is a
 * read-only value used by the source-private issuer and by later handoff stages; it is not an
 * authority by itself.  An authority can only be obtained from the issuer declared in
 * `provider_task_v4_authority_internal.hpp` and can only be consumed once.
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>

namespace cxxlens::detail::clang22
{
	inline constexpr std::string_view provider_task_v4_authority_schema =
		"cxxlens.clang22.task-authority.v4";
	inline constexpr std::string_view provider_task_v4_authority_request_schema =
		"cxxlens.clang22-materialization-request.v2_2";
	inline constexpr std::string_view provider_task_v4_authority_request_version = "2.2.0";
	inline constexpr std::string_view provider_task_v4_authority_task_schema =
		"cxxlens.clang22.task.v4";
	inline constexpr std::string_view provider_task_v4_authority_process_mode =
		"task-v4-source-closure-v2";
	inline constexpr std::string_view provider_task_v4_authority_domain =
		"cxxlens.clang22.task-authority.v4";

	/**
	 * Hard product bounds.  A caller may choose smaller limits, but may never raise or disable one
	 * of these limits.  Counts and byte totals are validated before an authority is issued.
	 */
	struct provider_task_v4_authority_limits
	{
		std::uint64_t maximum_tasks{4096U};
		std::uint64_t maximum_arguments{4096U};
		std::uint64_t maximum_argument_bytes{2048U};
		std::uint64_t maximum_roots{256U};
		std::uint64_t maximum_root_path_bytes{4096U};
		std::uint64_t maximum_manifest_bytes{std::uint64_t{40U} * 1024U * 1024U};
		std::uint64_t maximum_members{4096U};
		std::uint64_t maximum_blobs{4096U};
		std::uint64_t maximum_blob_bytes{std::uint64_t{16U} * 1024U * 1024U};
		std::uint64_t maximum_unique_blob_bytes{std::uint64_t{48U} * 1024U * 1024U};
		std::uint64_t maximum_source_bytes{std::uint64_t{16U} * 1024U * 1024U};
		std::uint64_t maximum_aggregate_source_bytes{std::uint64_t{512U} * 1024U * 1024U};
		std::uint64_t maximum_output_groups{4096U};
		std::uint64_t maximum_output_bytes{std::uint64_t{1024U} * 1024U * 1024U};
		std::uint64_t maximum_resident_bytes{1'310'720U};

		[[nodiscard]] sdk::result<void> validate() const;
	};

	/**
	 * Complete identity supplied to the source-private issuer.
	 *
	 * This is intentionally a projection, not an authority: copying or modifying it cannot mint a
	 * task capability.  Every field participates in the issued authority digest, including the
	 * process endpoint observations and exact toolchain occurrence fields.
	 */
	struct provider_task_v4_authority_identity
	{
		std::string authority_schema;

		// Request v2.2 identity and negotiated Protocol 2.0 capability.
		std::string request_schema;
		std::string request_version;
		std::string request_id;
		std::string request_digest;
		std::string materialization_request_id;
		std::string semantic_request_digest;
		std::uint16_t protocol_major{};
		std::uint16_t protocol_minor{};
		std::vector<std::string> required_features;

		// Exact task occurrence and task-v4 closure extension binding.
		std::uint64_t task_count{};
		std::uint64_t task_index{};
		std::string provider_task_id;
		std::string provider_execution_id;
		std::string task_schema;
		std::string task_id;
		std::string task_v4_digest;
		std::string base_task_digest;
		std::string main_logical_path;
		std::string logical_working_directory;
		std::string task_input_digest;
		std::string normalized_invocation_digest;
		std::string environment_digest;

		// Session and source-closure transfer identity.
		std::string session_id;
		std::string closure_id;
		std::string closure_digest;
		std::string manifest_digest;
		std::string transfer_digest;

		// Exact installed process-channel binding.  `first_sequence` is protocol sequence zero.
		std::string process_mode;
		std::uint64_t stream_id{};
		std::uint64_t first_sequence{};
		std::string process_binding_digest;
		std::int64_t read_descriptor{};
		std::int64_t write_descriptor{};
		std::uint64_t read_device{};
		std::uint64_t read_inode{};
		std::uint32_t read_mode{};
		std::uint64_t write_device{};
		std::uint64_t write_inode{};
		std::uint32_t write_mode{};

		// Exact toolchain occurrence identity.  `toolchain_sysroot == nullopt` means no sysroot.
		std::string toolchain_digest;
		std::string toolchain_family;
		std::string toolchain_exact_version;
		std::string toolchain_target_triple;
		std::string toolchain_executable;
		std::string toolchain_executable_digest;
		std::string builtin_headers_digest;
		std::optional<std::string> toolchain_sysroot;
		std::string abi_digest;
		std::string plugin_spec_digest;

		// Checked resource profile bound to this exact occurrence.
		std::uint64_t argument_count{};
		std::uint64_t longest_argument_bytes{};
		std::uint64_t root_count{};
		std::uint64_t longest_root_path_bytes{};
		std::uint64_t manifest_bytes{};
		std::uint64_t member_count{};
		std::uint64_t blob_count{};
		std::uint64_t blob_bytes{};
		std::uint64_t unique_blob_bytes{};
		std::uint64_t source_bytes{};
		std::uint64_t aggregate_source_bytes{};
		std::uint64_t output_group_count{};
		std::uint64_t output_bytes{};
		std::uint64_t resident_bytes{};

		[[nodiscard]] bool operator==(const provider_task_v4_authority_identity&) const = default;
	};

	/** Validate every identity relation and bound before the source-private issuer is called. */
	[[nodiscard]] sdk::result<void> validate_provider_task_v4_authority_identity(
		const provider_task_v4_authority_identity& identity,
		provider_task_v4_authority_limits limits = {});

	/** Derive the deterministic semantic digest for a validated identity projection. */
	[[nodiscard]] sdk::result<std::string>
	derive_provider_task_v4_authority_digest(const provider_task_v4_authority_identity& identity);

	/**
	 * Issuer-sealed capability for one exact request/task/session/closure/process/toolchain tuple.
	 *
	 * No default or public value constructor exists.  Copying is forbidden, and moving transfers
	 * the sole live state.  `consume() &&` invalidates the token even when its result is moved out;
	 * a second call is a replay and fails closed.
	 */
	class provider_task_v4_authority final
	{
	  public:
		~provider_task_v4_authority();
		provider_task_v4_authority(const provider_task_v4_authority&) = delete;
		provider_task_v4_authority& operator=(const provider_task_v4_authority&) = delete;
		provider_task_v4_authority(provider_task_v4_authority&&) noexcept;
		provider_task_v4_authority& operator=(provider_task_v4_authority&&) noexcept;

		[[nodiscard]] bool valid() const noexcept;
		[[nodiscard]] std::string_view authority_digest() const noexcept;
		[[nodiscard]] sdk::result<provider_task_v4_authority_identity> snapshot() const;
		[[nodiscard]] sdk::result<provider_task_v4_authority_identity> consume() &&;
		[[nodiscard]] bool
		matches(const provider_task_v4_authority_identity& candidate) const noexcept;

	  private:
		struct state;
		std::unique_ptr<state> state_;

		explicit provider_task_v4_authority(std::unique_ptr<state> state) noexcept;

		friend sdk::result<provider_task_v4_authority>
		issue_provider_task_v4_authority(provider_task_v4_authority_identity&& identity,
										 provider_task_v4_authority_limits limits);
	};
} // namespace cxxlens::detail::clang22
