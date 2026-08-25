#include "materialization_v4_incremental_ingress.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		[[nodiscard]] sdk::error invalid(std::string field, std::string detail = {})
		{
			return {"materialization.v4-incremental-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::error mismatch(std::string field, std::string detail = {})
		{
			return {"materialization.v4-incremental-mismatch", std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::error missing_authority(std::string field)
		{
			return {"materialization.v4-store-authority-missing", std::move(field), {}};
		}

		[[nodiscard]] sdk::error incomplete(std::string field)
		{
			return {"materialization.v4-store-incomplete", std::move(field), {}};
		}

		[[nodiscard]] sdk::canonical_value text(const std::string_view value)
		{
			return sdk::canonical_value::from_string(std::string{value});
		}

		[[nodiscard]] sdk::result<sdk::canonical_value> count(const std::uint64_t value,
															  const std::string_view field)
		{
			if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
				return sdk::unexpected(invalid(std::string{field}, "signed-bound"));
			return sdk::canonical_value::from_integer(static_cast<std::int64_t>(value));
		}

		[[nodiscard]] sdk::result<void>
		validate_claim_receipt_counters(const materialization_v4_claim_receipt& value)
		{
			const std::array<std::pair<std::string_view, std::uint64_t>, 5U> counters{{
				{"task-index", value.task_index},
				{"claim-count", value.claim_count},
				{"unresolved-count", value.unresolved_count},
				{"conflict-count", value.conflict_count},
				{"differential-disagreement-count", value.differential_disagreement_count},
			}};
			for (const auto& [field, number] : counters)
				if (auto valid = count(number, std::string{"task-receipt."} + std::string{field});
					!valid)
					return sdk::unexpected(std::move(valid.error()));
			return {};
		}

		[[nodiscard]] sdk::canonical_value
		claim_receipt_projection(const materialization_v4_claim_receipt& value)
		{
			return sdk::canonical_value::from_tuple({
				text(value.schema),
				text(value.binding_digest),
				text(value.materialization_request_id),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.task_index)),
				text(value.task_id),
				text(value.task_v4_digest),
				text(value.provider_execution_id),
				text(value.source_closure_id),
				text(value.source_closure_digest),
				text(value.manifest_digest),
				text(value.task_input_digest),
				text(value.claim_batch_content_digest),
				text(value.partition_id),
				text(value.partition_content_digest),
				text(value.coverage_digest),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.claim_count)),
				sdk::canonical_value::from_integer(
					static_cast<std::int64_t>(value.unresolved_count)),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.conflict_count)),
				sdk::canonical_value::from_integer(
					static_cast<std::int64_t>(value.differential_disagreement_count)),
				sdk::canonical_value::from_boolean(value.complete),
				text(value.receipt_digest),
			});
		}

		[[nodiscard]] sdk::result<std::string>
		receipt_digest(const materialization_v4_incremental_receipt& value)
		{
			std::vector<sdk::canonical_value> fields;
			fields.reserve(5U + value.task_receipts.size());
			fields.push_back(text(value.schema));
			fields.push_back(text(value.materialization_request_id));
			if (auto tasks = count(value.task_count, "receipt.task-count"); !tasks)
				return sdk::unexpected(std::move(tasks.error()));
			else
				fields.push_back(std::move(*tasks));
			std::vector<sdk::canonical_value> task_values;
			task_values.reserve(value.task_receipts.size());
			for (const auto& task : value.task_receipts)
			{
				if (auto valid = validate_claim_receipt_counters(task); !valid)
					return sdk::unexpected(std::move(valid.error()));
				task_values.push_back(claim_receipt_projection(task));
			}
			fields.push_back(sdk::canonical_value::from_tuple(std::move(task_values)));
			if (auto claims = count(value.claim_count, "receipt.claim-count"); !claims)
				return sdk::unexpected(std::move(claims.error()));
			else
				fields.push_back(std::move(*claims));
			if (auto unresolved = count(value.unresolved_count, "receipt.unresolved-count");
				!unresolved)
				return sdk::unexpected(std::move(unresolved.error()));
			else
				fields.push_back(std::move(*unresolved));
			if (auto conflicts = count(value.conflict_count, "receipt.conflict-count"); !conflicts)
				return sdk::unexpected(std::move(conflicts.error()));
			else
				fields.push_back(std::move(*conflicts));
			if (auto differential = count(value.differential_disagreement_count,
										  "receipt.differential-disagreement-count");
				!differential)
				return sdk::unexpected(std::move(differential.error()));
			else
				fields.push_back(std::move(*differential));
			fields.push_back(sdk::canonical_value::from_boolean(value.complete));
			return sdk::canonical_identity_digest(materialization_v4_incremental_receipt_schema,
												  fields);
		}

		[[nodiscard]] sdk::result<void>
		validate_receipt_header(const materialization_v4_incremental_receipt& value)
		{
			if (value.schema != materialization_v4_incremental_receipt_schema)
				return sdk::unexpected(invalid("receipt.schema", "unsupported"));
			if (auto valid = sdk::validate_strong_id(value.materialization_request_id); !valid)
				return sdk::unexpected(invalid("receipt.request-id", "strong-id"));
			if (value.task_count == 0U)
				return sdk::unexpected(invalid("receipt.task-count", "empty"));
			if (value.task_count > materialization_v4_incremental_max_tasks)
				return sdk::unexpected(invalid("receipt.task-count", "bound"));
			if (value.task_count != value.task_receipts.size())
				return sdk::unexpected(mismatch("receipt.task-count", "task-receipts"));
			if (auto valid = sdk::validate_strong_id(value.receipt_digest); !valid)
				return sdk::unexpected(invalid("receipt.digest", "strong-id"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		add_checked(std::uint64_t& total, const std::uint64_t value, const std::string_view field)
		{
			if (value > std::numeric_limits<std::uint64_t>::max() - total)
				return sdk::unexpected(invalid(std::string{field}, "overflow"));
			total += value;
			return {};
		}

		[[nodiscard]] sdk::result<void>
		validate_authority(const materialization_v4_store_publication_authority& value)
		{
			const std::array<std::pair<std::string_view, std::string_view>, 3U> ids{{
				{"analysis-recipe", value.analysis_recipe_digest},
				{"output-plan", value.output_plan_digest},
				{"publication-target", value.publication_target},
			}};
			for (const auto& [field, id] : ids)
				if (auto valid = sdk::validate_strong_id(id); !valid)
					return sdk::unexpected(
						invalid(std::string{"authority."} + std::string{field}, "strong-id"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		validate_inputs(const sdk::relation_engine& engine,
						const materialization_v4_incremental_receipt& receipt,
						const std::span<const materialization_v4_claim_sealed* const> sealed_tasks,
						const std::span<const sdk::claim> existing)
		{
			if (auto valid = validate_receipt_header(receipt); !valid)
				return valid;
			if (sealed_tasks.empty() ||
				sealed_tasks.size() > materialization_v4_incremental_max_tasks)
				return sdk::unexpected(invalid("sealed-tasks", "bound-or-empty"));
			if (sealed_tasks.size() != receipt.task_receipts.size())
				return sdk::unexpected(mismatch("sealed-tasks", "task-receipts"));

			std::uint64_t claims{};
			std::uint64_t unresolved{};
			std::uint64_t conflicts{};
			std::uint64_t differential{};
			bool complete = true;
			for (std::size_t index = 0; index < sealed_tasks.size(); ++index)
			{
				const auto* sealed = sealed_tasks[index];
				if (sealed == nullptr)
					return sdk::unexpected(invalid("sealed-tasks", "null"));
				if (auto valid =
						validate_materialization_v4_claim_receipt(engine, *sealed, existing);
					!valid)
					return sdk::unexpected(std::move(valid.error()));
				const auto& task = sealed->receipt;
				if (task.task_index != index)
					return sdk::unexpected(mismatch("task-index", "canonical-order"));
				if (task.materialization_request_id != receipt.materialization_request_id)
					return sdk::unexpected(mismatch("request-id", "task"));
				if (task != receipt.task_receipts[index])
					return sdk::unexpected(mismatch("task-receipt", "sealed"));
				for (std::size_t previous = 0; previous < index; ++previous)
					if (task.task_id == receipt.task_receipts[previous].task_id)
						return sdk::unexpected(invalid("task-id", "duplicate"));
				if (auto valid = add_checked(claims, task.claim_count, "claim-count"); !valid)
					return valid;
				if (auto valid = add_checked(unresolved, task.unresolved_count, "unresolved-count");
					!valid)
					return valid;
				if (auto valid = add_checked(conflicts, task.conflict_count, "conflict-count");
					!valid)
					return valid;
				if (auto valid = add_checked(differential,
											 task.differential_disagreement_count,
											 "differential-disagreement-count");
					!valid)
					return valid;
				complete = complete && task.complete;
			}
			if (receipt.claim_count != claims)
				return sdk::unexpected(mismatch("claim-count", "tasks"));
			if (receipt.unresolved_count != unresolved)
				return sdk::unexpected(mismatch("unresolved-count", "tasks"));
			if (receipt.conflict_count != conflicts)
				return sdk::unexpected(mismatch("conflict-count", "tasks"));
			if (receipt.differential_disagreement_count != differential)
				return sdk::unexpected(mismatch("differential-disagreement-count", "tasks"));
			if (receipt.complete != complete)
				return sdk::unexpected(mismatch("complete", "tasks"));
			auto expected_digest = receipt_digest(receipt);
			if (!expected_digest)
				return sdk::unexpected(std::move(expected_digest.error()));
			if (*expected_digest != receipt.receipt_digest)
				return sdk::unexpected(mismatch("receipt-digest", "recomputed"));
			return {};
		}
	} // namespace

	sdk::result<materialization_v4_incremental_receipt> make_materialization_v4_incremental_receipt(
		const sdk::relation_engine& engine,
		const std::span<const materialization_v4_claim_sealed* const> sealed_tasks,
		const std::span<const sdk::claim> existing)
	{
		if (sealed_tasks.empty() || sealed_tasks.size() > materialization_v4_incremental_max_tasks)
			return sdk::unexpected(invalid("sealed-tasks", "bound-or-empty"));
		const auto* first = sealed_tasks.front();
		if (first == nullptr)
			return sdk::unexpected(invalid("sealed-tasks", "null"));

		materialization_v4_incremental_receipt value;
		value.materialization_request_id = first->receipt.materialization_request_id;
		value.task_count = sealed_tasks.size();
		value.task_receipts.reserve(sealed_tasks.size());
		for (const auto* sealed : sealed_tasks)
		{
			if (sealed == nullptr)
				return sdk::unexpected(invalid("sealed-tasks", "null"));
			if (auto valid = validate_materialization_v4_claim_receipt(engine, *sealed, existing);
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			value.task_receipts.push_back(sealed->receipt);
		}
		for (const auto& task : value.task_receipts)
		{
			if (task.materialization_request_id != value.materialization_request_id)
				return sdk::unexpected(mismatch("request-id", "task"));
			if (task.task_index != static_cast<std::uint64_t>(&task - value.task_receipts.data()))
				return sdk::unexpected(mismatch("task-index", "canonical-order"));
			if (auto valid = add_checked(value.claim_count, task.claim_count, "claim-count");
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			if (auto valid =
					add_checked(value.unresolved_count, task.unresolved_count, "unresolved-count");
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			if (auto valid =
					add_checked(value.conflict_count, task.conflict_count, "conflict-count");
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			if (auto valid = add_checked(value.differential_disagreement_count,
										 task.differential_disagreement_count,
										 "differential-disagreement-count");
				!valid)
				return sdk::unexpected(std::move(valid.error()));
		}
		// A receipt set is complete only if every task is complete.
		value.complete = true;
		for (const auto& task : value.task_receipts)
			value.complete = value.complete && task.complete;
		auto digest = receipt_digest(value);
		if (!digest)
			return sdk::unexpected(std::move(digest.error()));
		value.receipt_digest = std::move(*digest);
		if (auto valid = validate_inputs(engine, value, sealed_tasks, existing); !valid)
			return sdk::unexpected(std::move(valid.error()));
		return value;
	}

	sdk::result<void> validate_materialization_v4_incremental_receipt(
		const sdk::relation_engine& engine,
		const materialization_v4_incremental_receipt& receipt,
		const std::span<const materialization_v4_claim_sealed* const> sealed_tasks,
		const std::span<const sdk::claim> existing)
	{
		return validate_inputs(engine, receipt, sealed_tasks, existing);
	}

	sdk::result<materialization_v4_store_ingress> admit_materialization_v4_store_ingress(
		const sdk::relation_engine& engine,
		const materialization_v4_incremental_receipt& receipt,
		const std::span<const materialization_v4_claim_sealed* const> sealed_tasks,
		std::optional<materialization_v4_store_publication_authority> authority)
	{
		if (!authority)
			return sdk::unexpected(missing_authority("provider-output"));
		if (auto valid = validate_inputs(engine, receipt, sealed_tasks, {}); !valid)
			return sdk::unexpected(std::move(valid.error()));
		if (!receipt.complete)
			return sdk::unexpected(incomplete("receipt"));
		if (auto valid = validate_authority(*authority); !valid)
			return sdk::unexpected(std::move(valid.error()));
		return materialization_v4_store_ingress{receipt, std::move(*authority)};
	}
} // namespace cxxlens::detail::clang22::materialization
