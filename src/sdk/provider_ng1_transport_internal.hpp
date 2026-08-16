#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <cxxlens/sdk/provider.hpp>

#include "provider_ng1_validation_internal.hpp"
#include "provider_visibility_internal.hpp"

namespace cxxlens::sdk::provider::detail
{
	/** The reserved NG1 heartbeat wire id, kept source-private until NG1 activation. */
	inline constexpr std::uint16_t ng1_heartbeat_message_id = 23U;
	inline constexpr message_type ng1_heartbeat_message_type =
		static_cast<message_type>(ng1_heartbeat_message_id);

	[[nodiscard]] constexpr bool is_ng1_heartbeat_message(const message_type value) noexcept
	{
		return static_cast<std::uint16_t>(value) == ng1_heartbeat_message_id;
	}

	/** Source-private typed heartbeat control value for the NG1 protocol. */
	struct CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_heartbeat_control
	{
		std::string schema{"cxxlens.provider-control.heartbeat.v1"};
		ng1_heartbeat_kind kind{ng1_heartbeat_kind::probe};
		std::string provider_id;
		semantic_version provider_version;
		std::string protocol_session_id;
		std::string task_id;
		std::uint64_t stream_id{};
		std::uint64_t heartbeat_sequence{};
		std::uint64_t monotonic_time_ns{};
		std::uint64_t highest_contiguous_acked_sequence{};
		std::string staged_digest;

		[[nodiscard]] bool operator==(const ng1_heartbeat_control&) const = default;

		/** Add the host receipt without allowing provider time to become authority. */
		[[nodiscard]] result<ng1_heartbeat_sample>
		to_validation_sample(std::uint64_t host_receipt_time_ns) const;
	};

	/** Source-private typed progress control value for the NG1 protocol. */
	struct CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_progress_control
	{
		std::string schema{"cxxlens.provider-control.progress.v2"};
		std::string task_id;
		std::string dependency_group_id;
		std::uint64_t progress_sequence{};
		std::uint64_t monotonic_time_ns{};
		std::uint64_t completed_units{};
		std::uint64_t total_units{};

		[[nodiscard]] bool operator==(const ng1_progress_control&) const = default;

		/** Add the host receipt used by the validator's rate authority. */
		[[nodiscard]] result<ng1_progress_sample>
		to_validation_sample(std::uint64_t host_receipt_time_ns) const;
	};

	/** Source-private typed resume control value for the NG1 protocol. */
	struct CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_resume_control
	{
		std::string schema{"cxxlens.provider-control.resume.v2"};
		ng1_resume_kind kind{ng1_resume_kind::request};
		ng1_resume_binding binding;
		std::uint64_t highest_contiguous_acked_sequence{};
		std::string staged_digest;
		std::uint64_t token_generation{};
		std::string token_digest;

		[[nodiscard]] bool operator==(const ng1_resume_control&) const = default;

		/** Bridge only a digest-checked value into the validator's token type. */
		[[nodiscard]] result<ng1_resume_token> to_validation_token() const;
	};

	/** Encode/decode the exact deterministic-CBOR heartbeat map. */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<std::vector<std::byte>>
	encode_ng1_heartbeat_control(const ng1_heartbeat_control& value);
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<ng1_heartbeat_control>
	decode_ng1_heartbeat_control(std::span<const std::byte> control);

	/** Encode/decode the exact deterministic-CBOR progress map. */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<std::vector<std::byte>>
	encode_ng1_progress_control(const ng1_progress_control& value);
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<ng1_progress_control>
	decode_ng1_progress_control(std::span<const std::byte> control);

	/** Encode/decode the exact deterministic-CBOR resume map. */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<std::vector<std::byte>>
	encode_ng1_resume_control(const ng1_resume_control& value);
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<ng1_resume_control>
	decode_ng1_resume_control(std::span<const std::byte> control);
} // namespace cxxlens::sdk::provider::detail
