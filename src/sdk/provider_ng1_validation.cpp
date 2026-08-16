#include <array>
#include <cstddef>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#include "provider_ng1_validation_internal.hpp"

namespace cxxlens::sdk::provider::detail
{
	namespace
	{
		constexpr std::string_view heartbeat_schema{"cxxlens.provider-control.heartbeat.v1"};
		constexpr std::string_view progress_schema{"cxxlens.provider-control.progress.v2"};
		constexpr std::string_view resume_schema{"cxxlens.provider-control.resume.v2"};
		constexpr std::string_view spill_schema{"cxxlens.provider-spill-record.v1"};
		constexpr std::string_view spill_payload_domain{"cxxlens.provider-spill-payload.v1"};
		constexpr std::string_view spill_record_domain{"cxxlens.provider-spill-record.v1"};
		constexpr std::string_view spill_prefix_domain{"cxxlens.provider-spill-prefix.v1"};
		constexpr std::string_view spill_fsync_receipt_schema{
			"cxxlens.provider-spill-fsync-receipt.v1"};
		constexpr std::string_view manifest_content_digest_prefix{"sha256:"};
		constexpr std::string_view semantic_digest_prefix{"semantic-v2:sha256:"};
		constexpr std::uint64_t heartbeat_timeout_ns = 5'000'000'000ULL;
		constexpr std::uint64_t heartbeat_startup_grace_ns = 10'000'000'000ULL;
		constexpr std::uint64_t progress_startup_grace_ns = 10'000'000'000ULL;
		constexpr std::uint64_t progress_sample_window_ns = 5'000'000'000ULL;
		constexpr std::uint64_t progress_maximum_sample_gap_ns = 10'000'000'000ULL;
		constexpr std::uint64_t progress_units_per_second = 1U;
		constexpr std::uint64_t nanos_per_second = 1'000'000'000ULL;

		[[nodiscard]] error
		ng1_error(std::string_view suffix, std::string field, std::string detail = {})
		{
			std::string code{"provider."};
			code.append(suffix);
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool valid_semantic_digest(const std::string_view value)
		{
			if (!value.starts_with(semantic_digest_prefix) ||
				value.size() != semantic_digest_prefix.size() + 64U)
				return false;
			for (const auto byte : value.substr(semantic_digest_prefix.size()))
				if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
					return false;
			return true;
		}

		[[nodiscard]] bool valid_manifest_content_digest(const std::string_view value)
		{
			if (!value.starts_with(manifest_content_digest_prefix) ||
				value.size() != manifest_content_digest_prefix.size() + 64U)
				return false;
			for (const auto byte : value.substr(manifest_content_digest_prefix.size()))
				if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
					return false;
			return true;
		}

		[[nodiscard]] result<void> valid_id(const std::string_view value,
											const std::string_view field,
											const std::string_view failure_code)
		{
			if (auto valid = cxxlens::sdk::validate_strong_id(value); !valid)
				return unexpected(ng1_error(failure_code, std::string{field}, "strong-id"));
			return {};
		}

		[[nodiscard]] result<void> valid_digest(const std::string_view value,
												const std::string_view field,
												const std::string_view failure_code)
		{
			if (!valid_semantic_digest(value))
				return unexpected(ng1_error(failure_code, std::string{field}, "semantic-v2"));
			return {};
		}

		[[nodiscard]] result<void>
		valid_manifest_content_digest(const std::string_view value,
									  const std::string_view field,
									  const std::string_view failure_code)
		{
			if (!valid_manifest_content_digest(value))
				return unexpected(ng1_error(failure_code, std::string{field}, "sha256"));
			return {};
		}

		[[nodiscard]] result<std::uint64_t> checked_elapsed(const std::uint64_t current,
															const std::uint64_t previous,
															const std::string_view code,
															const std::string_view field)
		{
			if (current < previous)
				return unexpected(ng1_error(code, std::string{field}, "backwards-or-underflow"));
			return current - previous;
		}

		[[nodiscard]] result<void> accept_contiguous(std::optional<std::uint64_t>& last,
													 const std::uint64_t observed,
													 const std::string_view code,
													 const std::string_view field)
		{
			const auto expected = last ? (*last == std::numeric_limits<std::uint64_t>::max()
											  ? std::optional<std::uint64_t>{}
											  : std::optional<std::uint64_t>{*last + 1U})
									   : std::optional<std::uint64_t>{0U};
			if (!expected || observed != *expected)
				return unexpected(
					ng1_error(code, std::string{field}, "non-contiguous-or-overflow"));
			last = observed;
			return {};
		}

		[[nodiscard]] std::string resume_kind_text(const ng1_resume_kind kind)
		{
			switch (kind)
			{
				case ng1_resume_kind::request:
					return "request";
				case ng1_resume_kind::accepted:
					return "accepted";
				case ng1_resume_kind::rejected:
					return "rejected";
			}
			return {};
		}

		[[nodiscard]] canonical_value canonical_u64(const std::uint64_t value)
		{
			std::vector<std::byte> bytes(sizeof(value));
			for (std::size_t index{}; index < bytes.size(); ++index)
				bytes[bytes.size() - index - 1U] = static_cast<std::byte>(value >> (index * 8U));
			return canonical_value::from_bytes(std::move(bytes));
		}

		[[nodiscard]] canonical_value canonical_text(const std::string_view value)
		{
			return canonical_value::from_string(std::string{value});
		}

		[[nodiscard]] result<std::string>
		semantic_tuple_digest(const std::string_view domain,
							  const std::vector<canonical_value>& fields)
		{
			auto encoded = canonical_binary(canonical_value::from_tuple(fields));
			if (!encoded)
				return unexpected(std::move(encoded.error()));
			const std::string bytes{reinterpret_cast<const char*>(encoded->data()),
									encoded->size()};
			return semantic_digest(domain, bytes);
		}

		[[nodiscard]] result<std::uint64_t> checked_spill_size_add(const std::uint64_t left,
																   const std::uint64_t right,
																   const std::string_view field)
		{
			if (right > std::numeric_limits<std::uint64_t>::max() - left)
				return unexpected(ng1_error("spill-corrupt", std::string{field}, "size-overflow"));
			return left + right;
		}

		[[nodiscard]] constexpr std::uint64_t cbor_head_bytes(const std::uint64_t value) noexcept
		{
			if (value < 24U)
				return 1U;
			if (value <= std::numeric_limits<std::uint8_t>::max())
				return 2U;
			if (value <= std::numeric_limits<std::uint16_t>::max())
				return 3U;
			if (value <= std::numeric_limits<std::uint32_t>::max())
				return 5U;
			return 9U;
		}

		[[nodiscard]] result<std::uint64_t> cbor_text_bytes(const std::string_view value,
															const std::string_view field)
		{
			const auto length = static_cast<std::uint64_t>(value.size());
			return checked_spill_size_add(cbor_head_bytes(length), length, field);
		}

		[[nodiscard]] result<std::uint64_t> cbor_bytes_bytes(const std::span<const std::byte> value,
															 const std::string_view field)
		{
			const auto length = static_cast<std::uint64_t>(value.size());
			return checked_spill_size_add(cbor_head_bytes(length), length, field);
		}

		[[nodiscard]] result<std::uint64_t> spill_record_wire_bytes(const ng1_spill_record& record)
		{
			// The source-private codec uses one deterministic-CBOR map with the exact eleven
			// authority fields and an eight-byte big-endian length prefix.
			std::uint64_t total = cbor_head_bytes(11U) + sizeof(std::uint64_t);
			auto add_text_pair = [&total](const std::string_view key,
										  const std::string_view value) -> result<void>
			{
				auto key_bytes = cbor_text_bytes(key, "record-key");
				if (!key_bytes)
					return unexpected(std::move(key_bytes.error()));
				auto value_bytes = cbor_text_bytes(value, key);
				if (!value_bytes)
					return unexpected(std::move(value_bytes.error()));
				auto next = checked_spill_size_add(total, *key_bytes, key);
				if (!next)
					return unexpected(std::move(next.error()));
				next = checked_spill_size_add(*next, *value_bytes, key);
				if (!next)
					return unexpected(std::move(next.error()));
				total = *next;
				return {};
			};
			auto add_uint_pair = [&total](const std::string_view key,
										  const std::uint64_t value) -> result<void>
			{
				auto key_bytes = cbor_text_bytes(key, "record-key");
				if (!key_bytes)
					return unexpected(std::move(key_bytes.error()));
				auto next = checked_spill_size_add(total, *key_bytes, key);
				if (!next)
					return unexpected(std::move(next.error()));
				next = checked_spill_size_add(*next, cbor_head_bytes(value), key);
				if (!next)
					return unexpected(std::move(next.error()));
				total = *next;
				return {};
			};
			auto add_bytes_pair = [&total](const std::string_view key,
										   const std::span<const std::byte> value) -> result<void>
			{
				auto key_bytes = cbor_text_bytes(key, "record-key");
				if (!key_bytes)
					return unexpected(std::move(key_bytes.error()));
				auto value_bytes = cbor_bytes_bytes(value, key);
				if (!value_bytes)
					return unexpected(std::move(value_bytes.error()));
				auto next = checked_spill_size_add(total, *key_bytes, key);
				if (!next)
					return unexpected(std::move(next.error()));
				next = checked_spill_size_add(*next, *value_bytes, key);
				if (!next)
					return unexpected(std::move(next.error()));
				total = *next;
				return {};
			};

			for (const auto& valid :
				 {add_text_pair("schema", record.schema),
				  add_uint_pair("record_ordinal", record.record_ordinal),
				  add_text_pair("task_id", record.task_id),
				  add_text_pair("dependency_group_id", record.dependency_group_id),
				  add_text_pair("atomic_output_group_id", record.atomic_output_group_id),
				  add_text_pair("batch_id", record.batch_id),
				  add_uint_pair("stream_id", record.stream_id),
				  add_uint_pair("sequence", record.sequence),
				  add_bytes_pair("payload_bytes", record.payload_bytes),
				  add_text_pair("payload_digest", record.payload_digest),
				  add_text_pair("record_digest", record.record_digest)})
				if (!valid)
					return unexpected(valid.error());
			return total;
		}

		struct ng1_u128
		{
			std::uint64_t high{};
			std::uint64_t low{};
		};

		/** Exact 64x64 -> 128 multiplication without compiler-specific integer types. */
		[[nodiscard]] ng1_u128 multiply_u64(const std::uint64_t left,
											const std::uint64_t right) noexcept
		{
			constexpr std::uint64_t mask = 0xffff'ffffULL;
			const std::uint64_t left_low = left & mask;
			const std::uint64_t left_high = left >> 32U;
			const std::uint64_t right_low = right & mask;
			const std::uint64_t right_high = right >> 32U;

			const std::uint64_t low_product = left_low * right_low;
			const std::uint64_t carry = low_product >> 32U;
			const std::uint64_t high_low_product = left_high * right_low + carry;
			const std::uint64_t middle_digit = high_low_product & mask;
			const std::uint64_t high_carry = high_low_product >> 32U;
			const std::uint64_t low_high_product = left_low * right_high + middle_digit;
			const std::uint64_t low = (low_product & mask) | ((low_high_product & mask) << 32U);
			const std::uint64_t high =
				left_high * right_high + high_carry + (low_high_product >> 32U);
			return {high, low};
		}

		[[nodiscard]] bool less_equal(const ng1_u128 left, const ng1_u128 right) noexcept
		{
			return left.high < right.high || (left.high == right.high && left.low <= right.low);
		}

		[[nodiscard]] bool progress_rate_satisfied(const std::uint64_t delta_units,
												   const std::uint64_t delta_time_ns) noexcept
		{
			const auto work = multiply_u64(delta_units, nanos_per_second);
			const auto required = multiply_u64(delta_time_ns, progress_units_per_second);
			return less_equal(required, work);
		}

		[[nodiscard]] result<ng1_recovery_state>
		invalid_transition([[maybe_unused]] const ng1_recovery_state state,
						   [[maybe_unused]] const ng1_recovery_event event)
		{
			return unexpected(
				ng1_error("recovery-failed", "transition", "transition-not-in-matrix"));
		}
	} // namespace

	result<void> ng1_session_binding::validate() const
	{
		if (provider_version.major == 0U)
			return unexpected(
				ng1_error("heartbeat-clock-invalid", "provider_version", "major-zero"));
		for (const auto [value, field] :
			 std::array{std::pair{std::string_view{provider_id}, std::string_view{"provider_id"}},
						std::pair{std::string_view{protocol_session_id},
								  std::string_view{"protocol_session_id"}},
						std::pair{std::string_view{task_id}, std::string_view{"task_id"}}})
			if (auto valid = valid_id(value, field, "heartbeat-clock-invalid"); !valid)
				return valid;
		return {};
	}

	result<ng1_heartbeat_state> ng1_heartbeat_state::create(ng1_session_binding binding,
															const std::uint64_t started_at_ns)
	{
		if (auto valid = binding.validate(); !valid)
			return unexpected(std::move(valid.error()));
		ng1_heartbeat_state output;
		output.binding_ = std::move(binding);
		output.started_at_ns_ = started_at_ns;
		return output;
	}

	result<void> ng1_heartbeat_state::accept(const ng1_heartbeat_sample& sample,
											 const std::uint64_t highest_observed_sequence,
											 const std::string_view host_observed_staged_digest)
	{
		if (terminal_)
			return unexpected(
				ng1_error("heartbeat-clock-invalid", "terminal", "heartbeat-after-terminal"));
		if (sample.schema != heartbeat_schema)
			return unexpected(ng1_error("heartbeat-clock-invalid", "schema", "unexpected"));
		if (sample.binding != binding_)
			return unexpected(ng1_error("heartbeat-clock-invalid", "binding", "mismatch"));
		if (!valid_semantic_digest(host_observed_staged_digest))
			return unexpected(ng1_error(
				"heartbeat-clock-invalid", "host_observed_staged_digest", "invalid-digest"));
		if (!valid_semantic_digest(sample.staged_digest))
			return unexpected(
				ng1_error("heartbeat-clock-invalid", "staged_digest", "invalid-digest"));
		if (sample.staged_digest != host_observed_staged_digest)
			return unexpected(
				ng1_error("heartbeat-clock-invalid", "staged_digest", "host-state-mismatch"));
		if (sample.kind != ng1_heartbeat_kind::probe && sample.kind != ng1_heartbeat_kind::ack)
			return unexpected(ng1_error("heartbeat-clock-invalid", "kind", "unknown"));
		if (sample.host_receipt_time_ns < started_at_ns_)
			return unexpected(ng1_error(
				"heartbeat-clock-invalid", "host_receipt_time_ns", "before-session-start"));
		if (sample.provider_monotonic_time_ns > sample.host_receipt_time_ns)
			return unexpected(ng1_error("heartbeat-clock-invalid", "monotonic_time_ns", "future"));
		if (last_provider_time_ns_ && sample.provider_monotonic_time_ns < *last_provider_time_ns_)
			return unexpected(
				ng1_error("heartbeat-clock-invalid", "monotonic_time_ns", "backwards"));
		if (last_host_receipt_time_ns_ && sample.host_receipt_time_ns < *last_host_receipt_time_ns_)
			return unexpected(
				ng1_error("heartbeat-clock-invalid", "host_receipt_time_ns", "backwards"));
		if (sample.kind == ng1_heartbeat_kind::probe)
		{
			auto next_sequence = last_probe_sequence_;
			if (auto valid = accept_contiguous(next_sequence,
											   sample.heartbeat_sequence,
											   "heartbeat-sequence",
											   "heartbeat_sequence");
				!valid)
				return valid;
			if (sample.highest_contiguous_acked_sequence > highest_observed_sequence)
				return unexpected(ng1_error("heartbeat-clock-invalid",
											"highest_contiguous_acked_sequence",
											"ahead-of-observed"));
			last_probe_sequence_ = next_sequence;
			last_probe_host_receipt_ns_ = sample.host_receipt_time_ns;
		}
		else
		{
			if (!last_valid_ack_received_ns_)
			{
				const auto since_start = checked_elapsed(sample.host_receipt_time_ns,
														 started_at_ns_,
														 "heartbeat-clock-invalid",
														 "host_receipt_time_ns");
				if (!since_start)
					return unexpected(std::move(since_start.error()));
				if (*since_start >= heartbeat_startup_grace_ns)
					return unexpected(ng1_error(
						"heartbeat-timeout", "host_receipt_time_ns", "startup-deadline-reached"));
			}
			if (last_probe_host_receipt_ns_)
			{
				const auto since_probe = checked_elapsed(sample.host_receipt_time_ns,
														 *last_probe_host_receipt_ns_,
														 "heartbeat-clock-invalid",
														 "host_receipt_time_ns");
				if (!since_probe)
					return unexpected(std::move(since_probe.error()));
				if (*since_probe >= heartbeat_timeout_ns)
					return unexpected(ng1_error(
						"heartbeat-timeout", "host_receipt_time_ns", "ack-deadline-reached"));
			}
			auto next_sequence = last_ack_sequence_;
			if (auto valid = accept_contiguous(next_sequence,
											   sample.heartbeat_sequence,
											   "heartbeat-sequence",
											   "heartbeat_sequence");
				!valid)
				return valid;
			if (sample.highest_contiguous_acked_sequence > highest_observed_sequence)
				return unexpected(ng1_error("heartbeat-clock-invalid",
											"highest_contiguous_acked_sequence",
											"ahead-of-observed"));
			last_ack_sequence_ = next_sequence;
			last_valid_ack_received_ns_ = sample.host_receipt_time_ns;
		}
		last_provider_time_ns_ = sample.provider_monotonic_time_ns;
		last_host_receipt_time_ns_ = sample.host_receipt_time_ns;
		return {};
	}

	result<void> ng1_heartbeat_state::check_liveness(const std::uint64_t now_ns) const
	{
		if (terminal_)
			return {};
		const auto elapsed =
			checked_elapsed(now_ns, started_at_ns_, "heartbeat-clock-invalid", "now_ns");
		if (!elapsed)
			return unexpected(std::move(elapsed.error()));
		if (!last_valid_ack_received_ns_)
		{
			if (*elapsed >= heartbeat_startup_grace_ns)
				return unexpected(
					ng1_error("heartbeat-timeout", "now_ns", "startup-grace-expired"));
			return {};
		}
		if (last_probe_host_receipt_ns_)
		{
			const auto since_probe = checked_elapsed(
				now_ns, *last_probe_host_receipt_ns_, "heartbeat-clock-invalid", "now_ns");
			if (!since_probe)
				return unexpected(std::move(since_probe.error()));
			if (*since_probe >= heartbeat_timeout_ns)
				return unexpected(
					ng1_error("heartbeat-timeout", "now_ns", "probe-deadline-reached"));
		}
		const auto since_ack = checked_elapsed(
			now_ns, *last_valid_ack_received_ns_, "heartbeat-clock-invalid", "now_ns");
		if (!since_ack)
			return unexpected(std::move(since_ack.error()));
		if (*since_ack >= heartbeat_timeout_ns)
			return unexpected(ng1_error("heartbeat-timeout", "now_ns", "ack-deadline-reached"));
		return {};
	}

	result<void> ng1_heartbeat_state::mark_terminal() noexcept
	{
		terminal_ = true;
		return {};
	}

	void ng1_heartbeat_state::rebase_start(const std::uint64_t started_at_ns) noexcept
	{
		started_at_ns_ = started_at_ns;
		last_probe_sequence_.reset();
		last_ack_sequence_.reset();
		last_provider_time_ns_.reset();
		last_probe_host_receipt_ns_.reset();
		last_host_receipt_time_ns_.reset();
		last_valid_ack_received_ns_.reset();
		terminal_ = false;
	}

	result<ng1_progress_state> ng1_progress_state::create(std::string task_id,
														  std::string dependency_group_id,
														  const std::uint64_t started_at_ns)
	{
		if (auto valid = valid_id(task_id, "task_id", "progress-rate"); !valid)
			return unexpected(std::move(valid.error()));
		if (auto valid = valid_id(dependency_group_id, "dependency_group_id", "progress-rate");
			!valid)
			return unexpected(std::move(valid.error()));
		ng1_progress_state output;
		output.task_id_ = std::move(task_id);
		output.dependency_group_id_ = std::move(dependency_group_id);
		output.started_at_ns_ = started_at_ns;
		return output;
	}

	result<void> ng1_progress_state::observe(const ng1_progress_sample& sample,
											 const bool terminal_sample)
	{
		if (terminal_observed_)
			return unexpected(ng1_error("progress-rate", "terminal", "after-terminal"));
		if (sample.schema != progress_schema)
			return unexpected(ng1_error("progress-rate", "schema", "unexpected"));
		if (sample.task_id != task_id_ || sample.dependency_group_id != dependency_group_id_)
			return unexpected(ng1_error("progress-rate", "binding", "mismatch"));
		if (sample.total_units == 0U || sample.completed_units > sample.total_units)
			return unexpected(ng1_error("progress-rate", "completed_units", "range"));
		if (total_units_ && sample.total_units != *total_units_)
			return unexpected(ng1_error("progress-rate", "total_units", "changed"));
		if (sample.host_receipt_time_ns < started_at_ns_)
			return unexpected(ng1_error("progress-rate", "host_receipt_time_ns", "before-start"));
		if (sample.provider_monotonic_time_ns > sample.host_receipt_time_ns)
			return unexpected(ng1_error("heartbeat-clock-invalid", "monotonic_time_ns", "future"));
		if (last_host_receipt_time_ns_ && sample.host_receipt_time_ns < *last_host_receipt_time_ns_)
			return unexpected(ng1_error("progress-rate", "host_receipt_time_ns", "backwards"));
		if (last_provider_time_ns_ && sample.provider_monotonic_time_ns < *last_provider_time_ns_)
			return unexpected(ng1_error("progress-rate", "monotonic_time_ns", "backwards"));

		auto next_sequence = last_sequence_;
		if (auto valid = accept_contiguous(
				next_sequence, sample.progress_sequence, "progress-rate", "progress_sequence");
			!valid)
			return valid;
		if (terminal_sample && sample.completed_units != sample.total_units)
			return unexpected(ng1_error("progress-rate", "completed_units", "terminal-not-total"));
		if (!last_host_receipt_time_ns_)
		{
			const auto since_start = checked_elapsed(sample.host_receipt_time_ns,
													 started_at_ns_,
													 "progress-rate",
													 "host_receipt_time_ns");
			if (!since_start)
				return unexpected(std::move(since_start.error()));
			if (*since_start >= progress_startup_grace_ns)
				return unexpected(
					ng1_error("progress-rate", "host_receipt_time_ns", "startup-deadline-reached"));
		}

		auto next_rate_checkpoint_receipt_ns = rate_checkpoint_receipt_ns_;
		auto next_rate_checkpoint_completed_units = rate_checkpoint_completed_units_;
		if (last_host_receipt_time_ns_)
		{
			if (!rate_checkpoint_receipt_ns_ || !rate_checkpoint_completed_units_)
				return unexpected(ng1_error("progress-rate", "rate_checkpoint", "missing"));
			const auto elapsed = checked_elapsed(sample.host_receipt_time_ns,
												 *last_host_receipt_time_ns_,
												 "progress-rate",
												 "host_receipt_time_ns");
			if (!elapsed)
				return unexpected(std::move(elapsed.error()));
			if (*elapsed == 0U)
				return unexpected(ng1_error("progress-rate", "sample_gap", "zero-elapsed"));
			if (*elapsed > progress_maximum_sample_gap_ns)
				return unexpected(ng1_error("progress-rate", "sample_gap", "maximum-exceeded"));
			if (sample.completed_units < *last_completed_units_)
				return unexpected(ng1_error("progress-rate", "completed_units", "backwards"));
			const auto delta_units = sample.completed_units - *last_completed_units_;
			const auto since_start = checked_elapsed(sample.host_receipt_time_ns,
													 started_at_ns_,
													 "progress-rate",
													 "host_receipt_time_ns");
			if (!since_start)
				return unexpected(std::move(since_start.error()));
			const bool after_grace = *since_start > progress_startup_grace_ns;
			const auto checkpoint_elapsed = checked_elapsed(sample.host_receipt_time_ns,
															*rate_checkpoint_receipt_ns_,
															"progress-rate",
															"host_receipt_time_ns");
			if (!checkpoint_elapsed)
				return unexpected(std::move(checkpoint_elapsed.error()));
			const auto checkpoint_delta_units =
				sample.completed_units - *rate_checkpoint_completed_units_;
			if (after_grace && delta_units == 0U)
				return unexpected(
					ng1_error("progress-rate", "completed_units", "zero-after-grace"));
			const bool admitted =
				terminal_sample || *checkpoint_elapsed >= progress_sample_window_ns;
			if (admitted && (after_grace || terminal_sample) &&
				!progress_rate_satisfied(checkpoint_delta_units, *checkpoint_elapsed))
				return unexpected(ng1_error("progress-rate", "rate", "minimum-not-met"));
			if (admitted)
			{
				next_rate_checkpoint_receipt_ns = sample.host_receipt_time_ns;
				next_rate_checkpoint_completed_units = sample.completed_units;
			}
		}
		else
		{
			next_rate_checkpoint_receipt_ns = sample.host_receipt_time_ns;
			next_rate_checkpoint_completed_units = sample.completed_units;
		}

		last_sequence_ = next_sequence;
		last_provider_time_ns_ = sample.provider_monotonic_time_ns;
		last_host_receipt_time_ns_ = sample.host_receipt_time_ns;
		last_completed_units_ = sample.completed_units;
		rate_checkpoint_receipt_ns_ = next_rate_checkpoint_receipt_ns;
		rate_checkpoint_completed_units_ = next_rate_checkpoint_completed_units;
		total_units_ = sample.total_units;
		terminal_observed_ = terminal_sample;
		return {};
	}

	result<void> ng1_progress_state::finish() const
	{
		if (!terminal_observed_ || !total_units_ || !last_completed_units_ ||
			*last_completed_units_ != *total_units_)
			return unexpected(ng1_error("progress-rate", "terminal", "total-not-reached"));
		return {};
	}

	void ng1_progress_state::rebase_start(const std::uint64_t started_at_ns) noexcept
	{
		started_at_ns_ = started_at_ns;
		last_sequence_.reset();
		last_provider_time_ns_.reset();
		last_host_receipt_time_ns_.reset();
		last_completed_units_.reset();
		rate_checkpoint_receipt_ns_.reset();
		rate_checkpoint_completed_units_.reset();
		total_units_.reset();
		terminal_observed_ = false;
	}

	result<void> ng1_resume_binding::validate() const
	{
		if (provider_version.major == 0U)
			return unexpected(ng1_error("resume-token-stale", "provider_version", "major-zero"));
		for (const auto [value, field] :
			 std::array{std::pair{std::string_view{provider_id}, std::string_view{"provider_id"}},
						std::pair{std::string_view{protocol_session_id},
								  std::string_view{"protocol_session_id"}},
						std::pair{std::string_view{task_id}, std::string_view{"task_id"}},
						std::pair{std::string_view{dependency_group_id},
								  std::string_view{"dependency_group_id"}},
						std::pair{std::string_view{atomic_output_group_id},
								  std::string_view{"atomic_output_group_id"}},
						std::pair{std::string_view{batch_id}, std::string_view{"batch_id"}}})
			if (auto valid = valid_id(value, field, "resume-token-stale"); !valid)
				return valid;
		for (const auto [value, field] :
			 std::array{std::pair{std::string_view{provider_binary_digest},
								  std::string_view{"provider_binary_digest"}},
						std::pair{std::string_view{provider_semantic_contract_digest},
								  std::string_view{"provider_semantic_contract_digest"}}})
			if (auto valid = valid_manifest_content_digest(value, field, "resume-token-stale");
				!valid)
				return valid;
		for (const auto [value, field] :
			 std::array{std::pair{std::string_view{task_input_digest},
								  std::string_view{"task_input_digest"}},
						std::pair{std::string_view{normalized_invocation_digest},
								  std::string_view{"normalized_invocation_digest"}},
						std::pair{std::string_view{toolchain_digest},
								  std::string_view{"toolchain_digest"}},
						std::pair{std::string_view{environment_digest},
								  std::string_view{"environment_digest"}},
						std::pair{std::string_view{sandbox_policy_digest},
								  std::string_view{"sandbox_policy_digest"}}})
			if (auto valid = valid_digest(value, field, "resume-token-stale"); !valid)
				return valid;
		return {};
	}

	result<std::string> ng1_resume_token_digest(const ng1_resume_token& token)
	{
		if (token.schema != resume_schema)
			return unexpected(ng1_error("resume-replay-invalid", "schema", "unexpected"));
		if (auto valid = token.binding.validate(); !valid)
			return unexpected(std::move(valid.error()));
		if (token.kind != ng1_resume_kind::request && token.kind != ng1_resume_kind::accepted &&
			token.kind != ng1_resume_kind::rejected)
			return unexpected(ng1_error("resume-replay-invalid", "kind", "unknown"));
		if (auto valid =
				valid_digest(token.staged_digest, "staged_digest", "resume-replay-invalid");
			!valid)
			return unexpected(std::move(valid.error()));
		if (!token.token_generation)
			return unexpected(ng1_error("resume-replay-invalid", "token_generation", "zero"));

		std::vector<canonical_value> fields;
		fields.reserve(21U);
		fields.push_back(canonical_text(token.schema));
		fields.push_back(canonical_text(resume_kind_text(token.kind)));
		fields.push_back(canonical_text(token.binding.provider_id));
		fields.push_back(canonical_text(token.binding.provider_version.string()));
		fields.push_back(canonical_text(token.binding.provider_binary_digest));
		fields.push_back(canonical_text(token.binding.provider_semantic_contract_digest));
		fields.push_back(canonical_text(token.binding.protocol_session_id));
		fields.push_back(canonical_text(token.binding.task_id));
		fields.push_back(canonical_text(token.binding.task_input_digest));
		fields.push_back(canonical_text(token.binding.normalized_invocation_digest));
		fields.push_back(canonical_text(token.binding.toolchain_digest));
		fields.push_back(canonical_text(token.binding.environment_digest));
		fields.push_back(canonical_text(token.binding.sandbox_policy_digest));
		fields.push_back(canonical_text(token.binding.dependency_group_id));
		fields.push_back(canonical_text(token.binding.atomic_output_group_id));
		fields.push_back(canonical_text(token.binding.batch_id));
		fields.push_back(canonical_u64(token.binding.stream_id));
		fields.push_back(canonical_u64(token.highest_contiguous_acked_sequence));
		fields.push_back(canonical_text(token.staged_digest));
		fields.push_back(canonical_u64(token.token_generation));
		return semantic_tuple_digest("cxxlens.provider-resume-token.v1", fields);
	}

	result<void> ng1_spill_fsync_receipt::validate() const
	{
		if (schema != spill_fsync_receipt_schema)
			return unexpected(ng1_error("resume-token-stale", "schema", "unexpected"));
		for (const auto [value, field] :
			 std::array{std::pair{std::string_view{provider_id}, std::string_view{"provider_id"}},
						std::pair{std::string_view{protocol_session_id},
								  std::string_view{"protocol_session_id"}},
						std::pair{std::string_view{task_id}, std::string_view{"task_id"}}})
			if (auto valid = valid_id(value, field, "resume-token-stale"); !valid)
				return unexpected(ng1_error("resume-token-stale", std::string{field}, "identity"));
		if (auto valid = valid_digest(staged_digest, "staged_digest", "resume-token-stale"); !valid)
			return unexpected(std::move(valid.error()));
		if (auto valid = valid_digest(spill_digest, "spill_digest", "resume-token-stale"); !valid)
			return unexpected(std::move(valid.error()));
		if (fsync_sequence == 0U)
			return unexpected(ng1_error("resume-token-stale", "fsync_sequence", "zero"));
		return {};
	}

	result<void> ng1_spill_binding::validate() const
	{
		for (const auto [value, field] :
			 std::array{std::pair{std::string_view{provider_id}, std::string_view{"provider_id"}},
						std::pair{std::string_view{protocol_session_id},
								  std::string_view{"protocol_session_id"}},
						std::pair{std::string_view{task_id}, std::string_view{"task_id"}},
						std::pair{std::string_view{dependency_group_id},
								  std::string_view{"dependency_group_id"}},
						std::pair{std::string_view{atomic_output_group_id},
								  std::string_view{"atomic_output_group_id"}},
						std::pair{std::string_view{batch_id}, std::string_view{"batch_id"}}})
			if (auto valid = valid_id(value, field, "spill-corrupt"); !valid)
				return valid;
		return {};
	}

	result<std::string> ng1_spill_payload_digest(const std::span<const std::byte> payload)
	{
		std::string bytes;
		if (!payload.empty())
			bytes.assign(reinterpret_cast<const char*>(payload.data()), payload.size());
		return semantic_digest(spill_payload_domain, bytes);
	}

	result<std::string> ng1_spill_record_digest(const ng1_spill_record& record)
	{
		if (record.schema != spill_schema)
			return unexpected(ng1_error("spill-corrupt", "schema", "unexpected"));
		for (const auto [value, field] :
			 std::array{std::pair{std::string_view{record.task_id}, std::string_view{"task_id"}},
						std::pair{std::string_view{record.dependency_group_id},
								  std::string_view{"dependency_group_id"}},
						std::pair{std::string_view{record.atomic_output_group_id},
								  std::string_view{"atomic_output_group_id"}},
						std::pair{std::string_view{record.batch_id}, std::string_view{"batch_id"}}})
			if (auto valid = valid_id(value, field, "spill-corrupt"); !valid)
				return unexpected(std::move(valid.error()));

		auto expected_payload_digest = ng1_spill_payload_digest(record.payload_bytes);
		if (!expected_payload_digest)
			return unexpected(std::move(expected_payload_digest.error()));
		if (record.payload_digest != *expected_payload_digest)
			return unexpected(ng1_error("spill-corrupt", "payload_digest", "mismatch"));

		std::vector<canonical_value> fields;
		fields.reserve(11U);
		fields.push_back(canonical_text(record.schema));
		fields.push_back(canonical_u64(record.record_ordinal));
		fields.push_back(canonical_text(record.task_id));
		fields.push_back(canonical_text(record.dependency_group_id));
		fields.push_back(canonical_text(record.atomic_output_group_id));
		fields.push_back(canonical_text(record.batch_id));
		fields.push_back(canonical_u64(record.stream_id));
		fields.push_back(canonical_u64(record.sequence));
		fields.push_back(canonical_value::from_bytes(
			std::vector<std::byte>{record.payload_bytes.begin(), record.payload_bytes.end()}));
		fields.push_back(canonical_text(record.payload_digest));
		return semantic_tuple_digest(spill_record_domain, fields);
	}

	result<ng1_spill_prefix_state> ng1_spill_prefix_state::create(ng1_spill_binding binding)
	{
		if (auto valid = binding.validate(); !valid)
			return unexpected(std::move(valid.error()));
		ng1_spill_prefix_state output;
		output.binding_ = std::move(binding);
		return output;
	}

	result<void> ng1_spill_prefix_state::append(const ng1_spill_record& record)
	{
		if (record_digests_.size() >= ng1_spill_maximum_records)
			return unexpected(ng1_error("spill-corrupt", "record_ordinal", "record-quota"));
		if (record.record_ordinal != next_record_ordinal_)
			return unexpected(ng1_error("spill-corrupt", "record_ordinal", "non-contiguous"));
		if (record.sequence != next_sequence_)
			return unexpected(ng1_error("spill-corrupt", "sequence", "non-contiguous"));
		if (record.task_id != binding_.task_id ||
			record.dependency_group_id != binding_.dependency_group_id ||
			record.atomic_output_group_id != binding_.atomic_output_group_id ||
			record.batch_id != binding_.batch_id || record.stream_id != binding_.stream_id)
			return unexpected(ng1_error("spill-corrupt", "binding", "mismatch"));

		auto expected_digest = ng1_spill_record_digest(record);
		if (!expected_digest)
			return unexpected(std::move(expected_digest.error()));
		if (record.record_digest != *expected_digest)
			return unexpected(ng1_error("spill-corrupt", "record_digest", "mismatch"));

		auto wire_bytes = spill_record_wire_bytes(record);
		if (!wire_bytes)
			return unexpected(std::move(wire_bytes.error()));
		if (*wire_bytes > ng1_spill_maximum_record_bytes)
			return unexpected(ng1_error("spill-corrupt", "record_bytes", "record-quota"));
		if (*wire_bytes > ng1_spill_maximum_total_bytes - total_bytes_)
			return unexpected(ng1_error("spill-corrupt", "total_bytes", "total-quota"));

		record_digests_.push_back(record.record_digest);
		total_bytes_ += *wire_bytes;
		++next_record_ordinal_;
		++next_sequence_;
		return {};
	}

	result<void> ng1_spill_prefix_state::validate_ack_frontier(
		const std::uint64_t highest_contiguous_acked_sequence) const
	{
		if (record_digests_.empty() || highest_contiguous_acked_sequence >= next_sequence_)
			return unexpected(
				ng1_error("spill-corrupt", "highest_contiguous_acked_sequence", "not-in-prefix"));
		return {};
	}

	result<std::string> ng1_spill_prefix_state::spill_digest() const
	{
		std::vector<canonical_value> fields;
		fields.reserve(record_digests_.size());
		for (const auto& digest : record_digests_)
			fields.push_back(canonical_text(digest));
		return semantic_tuple_digest(spill_prefix_domain, fields);
	}

	result<ng1_spill_fsync_receipt> ng1_spill_prefix_state::observe_host_fsync(
		const std::uint64_t highest_contiguous_acked_sequence,
		std::string staged_digest,
		const std::uint64_t fsync_sequence) const
	{
		if (!valid_semantic_digest(staged_digest))
			return unexpected(ng1_error("spill-corrupt", "staged_digest", "semantic-v2"));
		if (auto valid = validate_ack_frontier(highest_contiguous_acked_sequence); !valid)
			return unexpected(std::move(valid.error()));
		if (fsync_sequence == 0U)
			return unexpected(ng1_error("spill-corrupt", "fsync_sequence", "zero"));
		auto prefix_digest = spill_digest();
		if (!prefix_digest)
			return unexpected(std::move(prefix_digest.error()));
		ng1_spill_fsync_receipt receipt{std::string{spill_fsync_receipt_schema},
										binding_.provider_id,
										binding_.protocol_session_id,
										binding_.task_id,
										binding_.stream_id,
										highest_contiguous_acked_sequence,
										std::move(staged_digest),
										std::move(*prefix_digest),
										total_bytes_,
										total_records(),
										fsync_sequence};
		if (auto valid = receipt.validate(); !valid)
			return unexpected(ng1_error("spill-corrupt", "receipt", "invalid"));
		return receipt;
	}

	result<ng1_resume_state> ng1_resume_state::create(ng1_resume_binding binding)
	{
		if (auto valid = binding.validate(); !valid)
			return unexpected(std::move(valid.error()));
		ng1_resume_state output;
		output.binding_ = std::move(binding);
		return output;
	}

	result<void> ng1_resume_state::accept(const ng1_resume_token& token,
										  const ng1_spill_fsync_receipt& receipt,
										  const bool open_dependency_group,
										  const bool terminal,
										  const std::uint64_t highest_observed_sequence)
	{
		const auto expected_digest = ng1_resume_token_digest(token);
		if (!expected_digest || token.token_digest != *expected_digest)
			return unexpected(
				ng1_error("resume-replay-invalid", "token_digest", "projection-mismatch"));
		if (auto valid = receipt.validate(); !valid)
			return unexpected(std::move(valid.error()));
		if (token.kind != ng1_resume_kind::accepted || terminal || open_dependency_group)
			return unexpected(
				ng1_error("resume-token-stale", "authority", "not-durable-or-terminal"));
		if (token.binding != binding_)
			return unexpected(ng1_error("resume-token-stale", "binding", "mismatch"));
		if (receipt.provider_id != binding_.provider_id ||
			receipt.protocol_session_id != binding_.protocol_session_id ||
			receipt.task_id != binding_.task_id || receipt.stream_id != binding_.stream_id ||
			receipt.highest_contiguous_acked_sequence != token.highest_contiguous_acked_sequence ||
			receipt.staged_digest != token.staged_digest)
			return unexpected(
				ng1_error("resume-replay-invalid", "fsync_receipt", "binding-mismatch"));
		if (last_fsync_sequence_ && receipt.fsync_sequence <= *last_fsync_sequence_)
			return unexpected(
				ng1_error("resume-replay-invalid", "fsync_sequence", "not-increasing"));
		if (last_receipt_)
		{
			if (token.highest_contiguous_acked_sequence < last_acked_sequence_)
				return unexpected(ng1_error(
					"resume-replay-invalid", "highest_contiguous_acked_sequence", "backwards"));
			if (token.highest_contiguous_acked_sequence == last_acked_sequence_ &&
				token.staged_digest != last_receipt_->staged_digest)
				return unexpected(
					ng1_error("resume-replay-invalid", "staged_digest", "changed-at-sequence"));
			if (receipt.total_bytes < last_receipt_->total_bytes ||
				receipt.total_records < last_receipt_->total_records)
				return unexpected(ng1_error(
					"resume-replay-invalid", "spill_receipt", "append-only-counter-backwards"));
			if (token.highest_contiguous_acked_sequence == last_acked_sequence_ &&
				receipt.spill_digest != last_receipt_->spill_digest)
				return unexpected(
					ng1_error("resume-replay-invalid", "spill_digest", "changed-at-sequence"));
		}
		if (token.highest_contiguous_acked_sequence > highest_observed_sequence ||
			token.highest_contiguous_acked_sequence == std::numeric_limits<std::uint64_t>::max())
			return unexpected(ng1_error(
				"resume-replay-invalid", "highest_contiguous_acked_sequence", "replay-boundary"));
		if (last_generation_ && token.token_generation <= *last_generation_)
			return unexpected(
				ng1_error("resume-token-stale", "token_generation", "not-increasing"));
		last_generation_ = token.token_generation;
		last_acked_sequence_ = token.highest_contiguous_acked_sequence;
		last_fsync_sequence_ = receipt.fsync_sequence;
		last_receipt_ = receipt;
		accepted_ = true;
		return {};
	}

	result<std::uint64_t> ng1_resume_state::replay_start_sequence() const
	{
		if (!accepted_ || last_acked_sequence_ == std::numeric_limits<std::uint64_t>::max())
			return unexpected(ng1_error("resume-replay-invalid",
										"highest_contiguous_acked_sequence",
										"no-replay-authority"));
		return last_acked_sequence_ + 1U;
	}

	result<ng1_recovery_state> ng1_recovery_transition(const ng1_recovery_state state,
													   const ng1_recovery_event event)
	{
		switch (state)
		{
			case ng1_recovery_state::running:
				switch (event)
				{
					case ng1_recovery_event::heartbeat_timeout:
						return ng1_recovery_state::heartbeat_timeout;
					case ng1_recovery_event::progress_rate_failure:
						return ng1_recovery_state::progress_rate_failure;
					case ng1_recovery_event::cancel_requested:
						return ng1_recovery_state::cancel_requested;
					case ng1_recovery_event::worker_exit:
						return ng1_recovery_state::worker_killed;
					case ng1_recovery_event::output_sealed:
						return ng1_recovery_state::completed;
					case ng1_recovery_event::invalid_heartbeat_clock:
						return ng1_recovery_state::failed;
					default:
						return invalid_transition(state, event);
				}
			case ng1_recovery_state::heartbeat_timeout:
			case ng1_recovery_state::progress_rate_failure:
				return event == ng1_recovery_event::worker_kill_confirmed
					? result<ng1_recovery_state>{ng1_recovery_state::worker_killed}
					: invalid_transition(state, event);
			case ng1_recovery_state::cancel_requested:
				if (event == ng1_recovery_event::cancel_acknowledged)
					return ng1_recovery_state::failed;
				if (event == ng1_recovery_event::cancel_timeout)
					return ng1_recovery_state::worker_killed;
				return invalid_transition(state, event);
			case ng1_recovery_state::worker_killed:
				if (event == ng1_recovery_event::durable_token_valid)
					return ng1_recovery_state::resume_replay;
				if (event == ng1_recovery_event::durable_token_invalid)
					return ng1_recovery_state::failed;
				return invalid_transition(state, event);
			case ng1_recovery_state::resume_replay:
				if (event == ng1_recovery_event::replay_valid)
					return ng1_recovery_state::resumed;
				if (event == ng1_recovery_event::replay_invalid)
					return ng1_recovery_state::failed;
				return invalid_transition(state, event);
			case ng1_recovery_state::resumed:
				if (event == ng1_recovery_event::output_sealed)
					return ng1_recovery_state::completed;
				if (event == ng1_recovery_event::output_invalid)
					return ng1_recovery_state::failed;
				return invalid_transition(state, event);
			case ng1_recovery_state::completed:
			case ng1_recovery_state::failed:
				return invalid_transition(state, event);
		}
		return invalid_transition(state, event);
	}
} // namespace cxxlens::sdk::provider::detail
