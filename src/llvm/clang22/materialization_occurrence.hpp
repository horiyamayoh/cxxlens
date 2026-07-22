#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "materialization_rooted_vfs.hpp"

namespace cxxlens::detail::clang22::materialization
{
	inline constexpr std::string_view materialization_occurrence_manifest_path =
		"share/cxxlens/materialization/clang22/occurrence-v1.json";

	struct materialization_occurrence_file
	{
		std::string role;
		std::string path;
		std::string digest;

		[[nodiscard]] bool operator==(const materialization_occurrence_file&) const = default;
	};

	struct materialization_occurrence_manifest
	{
		std::string source_revision;
		std::string source_tree;
		std::string package_configuration;
		std::vector<materialization_occurrence_file> files;
		std::string occurrence_payload_digest;
		std::string inventory_digest;

		[[nodiscard]] bool operator==(const materialization_occurrence_manifest&) const = default;
	};

	struct materialization_occurrence_expectation
	{
		std::string source_revision;
		std::string source_tree;
		std::string package_configuration;
		std::string occurrence_manifest_digest;
		std::string materializer_executable_digest;
		std::string worker_executable_digest;
	};

	struct materialization_measured_file
	{
		materialization_occurrence_file authority;
		materialization_file_identity identity;

		[[nodiscard]] bool operator==(const materialization_measured_file&) const = default;
	};

	struct materialization_occurrence_receipt
	{
		std::string schema{"rooted-occurrence-v1"};
		std::string manifest_file_digest;
		std::string occurrence_payload_digest;
		std::string inventory_digest;
		std::string prefix_device_inode_observation_digest;
		std::vector<materialization_measured_file> files;
	};

	/** Parse and close-check exact static/shared manifest bytes without filesystem trust claims. */
	[[nodiscard]] sdk::result<materialization_occurrence_manifest>
	parse_materialization_occurrence_manifest(std::span<const std::byte> bytes,
											  std::string_view expected_configuration);

	/**
	 * Authenticated installed occurrence. The retained prefix and measured role FDs are the only
	 * later object roots; argv[0], PATH, mutable role paths, and the textual /proc target are never
	 * returned as authority. Measurement retains one kernel-sealed immutable snapshot per role;
	 * open_role() returns a read-only handle to that snapshot rather than a descriptor for the
	 * still-mutable installed inode.
	 */
	class measured_materialization_occurrence
	{
	  public:
		measured_materialization_occurrence(const measured_materialization_occurrence&) = delete;
		measured_materialization_occurrence&
		operator=(const measured_materialization_occurrence&) = delete;
		measured_materialization_occurrence(measured_materialization_occurrence&&) noexcept =
			default;
		measured_materialization_occurrence&
		operator=(measured_materialization_occurrence&&) noexcept = default;

		[[nodiscard]] const materialization_occurrence_manifest& manifest() const noexcept;
		[[nodiscard]] const materialization_occurrence_receipt& receipt() const noexcept;
		/** Return an independent read-only handle to the role's measured immutable snapshot. */
		[[nodiscard]] sdk::result<materialization_owned_fd> open_role(std::string_view role) const;

	  private:
		measured_materialization_occurrence(materialization_owned_fd prefix,
											materialization_occurrence_manifest manifest,
											materialization_occurrence_receipt receipt,
											std::vector<materialization_owned_fd> role_descriptors);

		materialization_owned_fd prefix_;
		materialization_occurrence_manifest manifest_;
		materialization_occurrence_receipt receipt_;
		std::vector<materialization_owned_fd> role_descriptors_;

		friend sdk::result<measured_materialization_occurrence>
		measure_materialization_occurrence(const materialization_occurrence_expectation&);
	};

	/** Measure /proc/self/exe, derive its exact installed prefix, and close the full manifest. */
	[[nodiscard]] sdk::result<measured_materialization_occurrence>
	measure_materialization_occurrence(const materialization_occurrence_expectation& expected);
} // namespace cxxlens::detail::clang22::materialization
