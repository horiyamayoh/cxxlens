#pragma once

/**
 * @file detached_provider_run_internal.hpp
 * @brief Immutable bounded codec for cxxlens.detached-provider-run.v1.
 */

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/sdk/application_analysis.hpp>

namespace cxxlens::sdk::detail
{
	enum class detached_provider_terminal : std::uint8_t
	{
		complete,
		partial,
		rejected,
		failed,
		cancelled,
	};

	[[nodiscard]] constexpr bool is_valid(const detached_provider_terminal value) noexcept
	{
		return value >= detached_provider_terminal::complete &&
			value <= detached_provider_terminal::cancelled;
	}

	struct detached_provider_identity
	{
		std::string provider_id;
		semantic_version provider_version;
		std::string binary_digest;
		std::string semantic_contract_digest;
		std::string signature_digest;
		std::string revocation_state;
		std::string sandbox_policy_digest;

		[[nodiscard]] bool operator==(const detached_provider_identity&) const = default;
	};

	struct detached_partition_projection
	{
		std::string descriptor_id;
		std::string descriptor_digest;
		std::string dependency_group_id;
		std::string atomic_output_group_id;
		std::string batch_id;
		std::string batch_digest;
		std::uint64_t row_count{};

		[[nodiscard]] bool operator==(const detached_partition_projection&) const = default;
	};

	struct detached_coverage_projection
	{
		std::string kind;
		std::string id;
		std::string state;
		std::string reason;

		[[nodiscard]] bool operator==(const detached_coverage_projection&) const = default;
	};

	struct detached_unresolved_projection
	{
		std::string code;
		std::string subject;
		std::string detail;

		[[nodiscard]] bool operator==(const detached_unresolved_projection&) const = default;
	};

	struct detached_provenance_projection
	{
		std::string kind;
		std::string subject;
		std::string producer;
		std::string summary;

		[[nodiscard]] bool operator==(const detached_provenance_projection&) const = default;
	};

	struct detached_provider_run_draft
	{
		std::string task_id;
		std::string task_input_digest;
		std::string replay_plan_digest;
		detached_provider_identity provider;
		std::vector<std::byte> protocol_transcript;
		detached_provider_terminal terminal{detached_provider_terminal::failed};
		std::vector<detached_partition_projection> partitions;
		std::vector<detached_coverage_projection> coverage;
		std::vector<detached_unresolved_projection> unresolved;
		std::vector<detached_provenance_projection> provenance;
		std::optional<std::string> runtime_receipt_digest;

		[[nodiscard]] bool operator==(const detached_provider_run_draft&) const = default;
	};

	class validated_detached_provider_run
	{
	  public:
		[[nodiscard]] const detached_provider_run_draft& value() const noexcept
		{
			return value_;
		}
		[[nodiscard]] std::span<const std::byte> bytes() const noexcept
		{
			return bytes_;
		}
		[[nodiscard]] std::string_view digest() const noexcept
		{
			return digest_;
		}

	  private:
		validated_detached_provider_run(detached_provider_run_draft value,
										std::vector<std::byte> bytes,
										std::string digest)
			: value_{std::move(value)}, bytes_{std::move(bytes)}, digest_{std::move(digest)}
		{
		}

		detached_provider_run_draft value_;
		std::vector<std::byte> bytes_;
		std::string digest_;

		friend result<validated_detached_provider_run>
			validate_detached_provider_run(detached_provider_run_draft, import_limits);
	};

	/** Validate, canonicalize, and encode one detached candidate without granting adoption. */
	[[nodiscard]] result<validated_detached_provider_run>
	validate_detached_provider_run(detached_provider_run_draft draft, import_limits limits = {});

	/** Strictly decode one complete canonical candidate and re-run the same validator. */
	[[nodiscard]] result<validated_detached_provider_run>
	decode_detached_provider_run(std::span<const std::byte> bytes, import_limits limits = {});
} // namespace cxxlens::sdk::detail
