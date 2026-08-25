#pragma once

/**
 * @file provider_expected_output_validator_internal.hpp
 * @brief Host-owned expected-output binding around the shared provider transcript seal.
 *
 * The ordinary provider transcript validator authenticates Protocol 2.0 frames and constructs
 * value-owned output rows.  This source-private layer adds the materializer's independent
 * expected-set authority: the task's six descriptor batches, their runtime descriptor digests,
 * dependency/atomic groups, and batch identities.  It never constructs a Store authority and it
 * never derives an expected value from provider output.
 */

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/sdk/provider.hpp>

#include "provider_validation_internal.hpp"

namespace cxxlens::sdk::provider::detail
{
	/** The installed Clang 22 output contract has six ordered descriptors. */
	inline constexpr std::size_t expected_output_descriptor_count = 6U;
	inline constexpr std::array<std::string_view, expected_output_descriptor_count>
		expected_output_descriptor_ids{
			"cc.call_direct_target.v1",
			"cc.call_site.v1",
			"cc.entity.v1",
			"frontend.clang22.call_observation.v2",
			"frontend.clang22.entity_observation.v2",
			"frontend.clang22.type_observation.v2",
		};

	/** One host-authorized output batch binding. */
	struct CXXLENS_PROVIDER_DETAIL_HIDDEN expected_output_batch_binding
	{
		std::string descriptor_id;
		/** The relation descriptor's cxxlens.relation-descriptor-binding.v2 digest. */
		std::string runtime_descriptor_digest;
		std::string dependency_group_id;
		std::string atomic_output_group_id;
		std::string batch_id;

		[[nodiscard]] bool operator==(const expected_output_batch_binding&) const = default;
	};

	/**
	 * Host-owned exact output authority.  The six entries are ordered by the accepted Clang 22
	 * descriptor contract; an empty batch is still represented by its expected batch binding and
	 * must have an authenticated batch_end frame.
	 */
	struct CXXLENS_PROVIDER_DETAIL_HIDDEN expected_output_authority
	{
		std::string task_id;
		std::vector<expected_output_batch_binding> batches;

		[[nodiscard]] result<void> validate() const;
	};

	/**
	 * Immutable typed output seal for the materializer.
	 *
	 * `output()` is the existing value-owned sealed transcript produced by the shared validator.
	 * It contains all retained rows, chunk digests, coverage, unresolved items, and evidence.  The
	 * expected authority is retained alongside it for downstream cross-binding.  No publication,
	 * Store candidate, CAS token, or Store mutation is represented by this type.
	 */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN expected_output_sealed_transcript
	{
	  public:
		[[nodiscard]] const expected_output_authority& authority() const noexcept
		{
			return authority_;
		}
		[[nodiscard]] const sealed_provider_transcript& output() const noexcept
		{
			return output_;
		}

	  private:
		expected_output_sealed_transcript(expected_output_authority authority,
										  sealed_provider_transcript output)
			: authority_{std::move(authority)}, output_{std::move(output)}
		{
		}

		expected_output_authority authority_;
		sealed_provider_transcript output_;

		friend result<expected_output_sealed_transcript>
		validate_expected_output_transcript(expected_output_authority,
											const transcript_validation_request&,
											std::span<const frame>,
											protocol_limits);
	};

	/**
	 * Validate a provider output transcript against independent host authority and seal it for the
	 * materializer.  The shared validator remains responsible for frame sequencing, credit,
	 * handshake, row/column codecs, coverage, unresolved, progress, and terminal semantics.
	 */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<expected_output_sealed_transcript>
	validate_expected_output_transcript(expected_output_authority expected,
										const transcript_validation_request& request,
										std::span<const frame> frames,
										protocol_limits session_limits);
} // namespace cxxlens::sdk::provider::detail
