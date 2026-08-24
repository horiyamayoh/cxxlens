#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "materialization_execution_journal.hpp"

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		[[nodiscard]] sdk::error v4_execution_error(std::string field, std::string detail = {})
		{
			return {"materialization.v4-execution-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::result<sdk::canonical_value> v4_count(const std::uint64_t value,
																 const std::string_view field)
		{
			if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
				return sdk::unexpected(v4_execution_error(std::string{field}, "signed-bound"));
			return sdk::canonical_value::from_integer(static_cast<std::int64_t>(value));
		}

		[[nodiscard]] sdk::canonical_value v4_text(const std::string_view value)
		{
			return sdk::canonical_value::from_string(std::string{value});
		}

		[[nodiscard]] sdk::canonical_value
		v4_task_receipt_projection(const materialization_v4_claim_receipt& value)
		{
			return sdk::canonical_value::from_tuple({
				v4_text(value.schema),
				v4_text(value.binding_digest),
				v4_text(value.materialization_request_id),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.task_index)),
				v4_text(value.task_id),
				v4_text(value.task_v4_digest),
				v4_text(value.provider_execution_id),
				v4_text(value.source_closure_id),
				v4_text(value.source_closure_digest),
				v4_text(value.manifest_digest),
				v4_text(value.task_input_digest),
				v4_text(value.claim_batch_content_digest),
				v4_text(value.partition_id),
				v4_text(value.partition_content_digest),
				v4_text(value.coverage_digest),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.claim_count)),
				sdk::canonical_value::from_integer(
					static_cast<std::int64_t>(value.unresolved_count)),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.conflict_count)),
				sdk::canonical_value::from_integer(
					static_cast<std::int64_t>(value.differential_disagreement_count)),
				sdk::canonical_value::from_boolean(value.complete),
				v4_text(value.receipt_digest),
			});
		}

		[[nodiscard]] sdk::result<void>
		v4_task_receipt_shape(const materialization_v4_claim_receipt& value)
		{
			if (value.schema != materialization_v4_claim_receipt_schema ||
				value.task_index > 4095U || !sdk::validate_strong_id(value.binding_digest) ||
				!sdk::validate_strong_id(value.materialization_request_id) ||
				!sdk::validate_strong_id(value.task_id) ||
				!sdk::validate_strong_id(value.task_v4_digest) ||
				!sdk::validate_strong_id(value.provider_execution_id) ||
				!sdk::validate_strong_id(value.source_closure_id) ||
				!sdk::validate_strong_id(value.source_closure_digest) ||
				!sdk::validate_strong_id(value.manifest_digest) ||
				!sdk::validate_strong_id(value.task_input_digest) ||
				!sdk::validate_strong_id(value.claim_batch_content_digest) ||
				!sdk::validate_strong_id(value.partition_id) ||
				!sdk::validate_strong_id(value.partition_content_digest) ||
				!sdk::validate_strong_id(value.coverage_digest) ||
				!sdk::validate_strong_id(value.receipt_digest))
				return sdk::unexpected(v4_execution_error("task-receipt", "identity"));
			const std::array<std::uint64_t, 4U> counts{value.claim_count,
													   value.unresolved_count,
													   value.conflict_count,
													   value.differential_disagreement_count};
			for (const auto count : counts)
				if (count > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
					return sdk::unexpected(v4_execution_error("task-receipt", "signed-bound"));
			return {};
		}

		[[nodiscard]] sdk::result<std::string>
		v4_incremental_digest(const materialization_v4_incremental_receipt& value)
		{
			std::vector<sdk::canonical_value> fields;
			fields.reserve(10U);
			fields.push_back(v4_text(value.schema));
			fields.push_back(v4_text(value.materialization_request_id));
			auto task_count = v4_count(value.task_count, "task-count");
			if (!task_count)
				return sdk::unexpected(std::move(task_count.error()));
			fields.push_back(std::move(*task_count));
			std::vector<sdk::canonical_value> tasks;
			tasks.reserve(value.task_receipts.size());
			for (const auto& task : value.task_receipts)
			{
				if (auto valid = v4_task_receipt_shape(task); !valid)
					return sdk::unexpected(std::move(valid.error()));
				tasks.push_back(v4_task_receipt_projection(task));
			}
			fields.push_back(sdk::canonical_value::from_tuple(std::move(tasks)));
			const std::array<std::pair<std::uint64_t, std::string_view>, 4U> counts{{
				{value.claim_count, "claim-count"},
				{value.unresolved_count, "unresolved-count"},
				{value.conflict_count, "conflict-count"},
				{value.differential_disagreement_count, "differential-count"},
			}};
			for (const auto [number, field] : counts)
			{
				auto encoded = v4_count(number, field);
				if (!encoded)
					return sdk::unexpected(std::move(encoded.error()));
				fields.push_back(std::move(*encoded));
			}
			fields.push_back(sdk::canonical_value::from_boolean(value.complete));
			return sdk::canonical_identity_digest(materialization_v4_incremental_receipt_schema,
												  fields);
		}

		[[nodiscard]] sdk::result<void>
		v4_incremental_shape(const materialization_v4_incremental_receipt& value,
							 const bool require_complete)
		{
			if (value.schema != materialization_v4_incremental_receipt_schema ||
				value.task_count == 0U ||
				value.task_count > materialization_v4_incremental_max_tasks ||
				value.task_count != value.task_receipts.size() ||
				!sdk::validate_strong_id(value.materialization_request_id) ||
				!sdk::validate_strong_id(value.receipt_digest) ||
				(require_complete && !value.complete))
				return sdk::unexpected(v4_execution_error("incremental-receipt", "shape"));
			std::uint64_t claims{};
			std::uint64_t unresolved{};
			std::uint64_t conflicts{};
			std::uint64_t differential{};
			bool complete = true;
			for (std::size_t index{}; index < value.task_receipts.size(); ++index)
			{
				const auto& task = value.task_receipts[index];
				if (auto valid = v4_task_receipt_shape(task); !valid)
					return valid;
				if (task.materialization_request_id != value.materialization_request_id ||
					task.task_index != index)
					return sdk::unexpected(v4_execution_error("incremental-receipt", "order"));
				for (std::size_t previous{}; previous < index; ++previous)
					if (task.task_id == value.task_receipts[previous].task_id)
						return sdk::unexpected(
							v4_execution_error("incremental-receipt", "duplicate"));
				const std::array<std::pair<std::uint64_t, std::uint64_t*>, 4U> additions{{
					{task.claim_count, &claims},
					{task.unresolved_count, &unresolved},
					{task.conflict_count, &conflicts},
					{task.differential_disagreement_count, &differential},
				}};
				for (const auto [number, total] : additions)
				{
					if (number > std::numeric_limits<std::uint64_t>::max() - *total)
						return sdk::unexpected(
							v4_execution_error("incremental-receipt", "overflow"));
					*total += number;
				}
				complete = complete && task.complete;
			}
			if (value.claim_count != claims || value.unresolved_count != unresolved ||
				value.conflict_count != conflicts ||
				value.differential_disagreement_count != differential || value.complete != complete)
				return sdk::unexpected(v4_execution_error("incremental-receipt", "census"));
			auto digest = v4_incremental_digest(value);
			if (!digest || *digest != value.receipt_digest)
				return sdk::unexpected(v4_execution_error("incremental-receipt", "digest"));
			return {};
		}

		[[nodiscard]] sdk::result<std::string>
		v4_execution_digest(const materialization_v4_execution_receipt& value)
		{
			std::vector<sdk::canonical_value> tasks;
			tasks.reserve(value.tasks.size());
			for (const auto& task : value.tasks)
			{
				auto calls = v4_count(task.provider_call_count, "provider-call-count");
				if (!calls)
					return sdk::unexpected(std::move(calls.error()));
				tasks.push_back(sdk::canonical_value::from_tuple({
					v4_text(task.receipt.receipt_digest),
					sdk::canonical_value::from_boolean(task.reused),
					std::move(*calls),
				}));
			}
			auto task_count = v4_count(value.task_count, "task-count");
			auto provider_calls = v4_count(value.provider_call_count, "provider-call-count");
			auto reused = v4_count(value.reused_task_count, "reused-task-count");
			if (!task_count || !provider_calls || !reused)
				return sdk::unexpected(v4_execution_error("execution-receipt", "signed-bound"));
			return sdk::canonical_identity_digest(
				materialization_v4_execution_receipt::schema,
				std::array<sdk::canonical_value, 7U>{
					v4_text(value.materialization_request_id),
					std::move(*task_count),
					sdk::canonical_value::from_tuple(std::move(tasks)),
					std::move(*provider_calls),
					std::move(*reused),
					v4_text(value.incremental_receipt_digest),
					v4_text(materialization_v4_execution_receipt::schema),
				});
		}
	} // namespace

	struct materialization_v4_execution_journal::state
	{
		std::string materialization_request_id;
		std::uint64_t task_count{};
		std::vector<materialization_v4_task_execution> tasks;
		std::uint64_t provider_call_count{};
		std::uint64_t reused_task_count{};
	};

	materialization_v4_execution_journal::materialization_v4_execution_journal(
		std::unique_ptr<state> state) noexcept
		: state_{std::move(state)}
	{
	}

	materialization_v4_execution_journal::materialization_v4_execution_journal(
		materialization_v4_execution_journal&&) noexcept = default;
	materialization_v4_execution_journal& materialization_v4_execution_journal::operator=(
		materialization_v4_execution_journal&&) noexcept = default;
	materialization_v4_execution_journal::~materialization_v4_execution_journal() = default;

	sdk::result<materialization_v4_execution_journal>
	materialization_v4_execution_journal::begin(std::string materialization_request_id,
												const std::uint64_t task_count)
	{
		if (!sdk::validate_strong_id(materialization_request_id) || task_count == 0U ||
			task_count > materialization_v4_incremental_max_tasks)
			return sdk::unexpected(v4_execution_error("begin", "request-or-bound"));
		auto value = std::make_unique<state>();
		value->materialization_request_id = std::move(materialization_request_id);
		value->task_count = task_count;
		value->tasks.reserve(static_cast<std::size_t>(task_count));
		return materialization_v4_execution_journal{std::move(value)};
	}

	sdk::result<void>
	materialization_v4_execution_journal::record(materialization_v4_claim_receipt receipt,
												 const bool reused,
												 const std::uint64_t provider_call_count)
	{
		if (!state_)
			return sdk::unexpected(v4_execution_error("record", "consumed-journal"));
		if (auto valid = v4_task_receipt_shape(receipt); !valid)
			return valid;
		if (receipt.materialization_request_id != state_->materialization_request_id ||
			receipt.task_index != state_->tasks.size() ||
			state_->tasks.size() >= state_->task_count)
			return sdk::unexpected(v4_execution_error("record", "request-or-order"));
		if ((reused && provider_call_count != 0U) || (!reused && provider_call_count == 0U))
			return sdk::unexpected(v4_execution_error("record", "provider-call-census"));
		if (provider_call_count >
			std::numeric_limits<std::uint64_t>::max() - state_->provider_call_count)
			return sdk::unexpected(v4_execution_error("record", "provider-call-overflow"));
		state_->provider_call_count += provider_call_count;
		if (reused)
			++state_->reused_task_count;
		state_->tasks.push_back({std::move(receipt), reused, provider_call_count});
		return {};
	}

	sdk::result<materialization_v4_execution_receipt>
	materialization_v4_execution_journal::finish(materialization_v4_incremental_receipt expected) &&
	{
		if (!state_)
			return sdk::unexpected(v4_execution_error("finish", "consumed-journal"));
		if (expected.materialization_request_id != state_->materialization_request_id ||
			expected.task_count != state_->task_count || state_->tasks.size() != state_->task_count)
			return sdk::unexpected(v4_execution_error("finish", "request-or-census"));
		if (auto valid = v4_incremental_shape(expected, false); !valid)
			return sdk::unexpected(std::move(valid.error()));
		for (std::size_t index{}; index < state_->tasks.size(); ++index)
			if (state_->tasks[index].receipt != expected.task_receipts[index])
				return sdk::unexpected(v4_execution_error("finish", "receipt-mismatch"));
		materialization_v4_execution_receipt output{
			state_->materialization_request_id,
			state_->task_count,
			std::move(state_->tasks),
			state_->provider_call_count,
			state_->reused_task_count,
			expected.receipt_digest,
			{},
		};
		auto digest = v4_execution_digest(output);
		if (!digest)
			return sdk::unexpected(std::move(digest.error()));
		output.execution_digest = std::move(*digest);
		state_.reset();
		return output;
	}

	sdk::result<void> materialization_v4_execution_journal::validate_exact_reuse(
		const materialization_v4_incremental_receipt& prior,
		const materialization_v4_incremental_receipt& current)
	{
		if (auto valid = v4_incremental_shape(prior, true); !valid)
			return valid;
		if (auto valid = v4_incremental_shape(current, true); !valid)
			return valid;
		if (prior != current)
			return sdk::unexpected(v4_execution_error("reuse", "identity-mismatch"));
		return {};
	}
} // namespace cxxlens::detail::clang22::materialization
