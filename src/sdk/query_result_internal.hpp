#pragma once

#include <functional>
#include <limits>
#include <optional>

#include <cxxlens/sdk/query.hpp>

namespace cxxlens::sdk::query
{
	struct query_result::data
	{
		std::vector<annotated_row> row_values;
		// Immutable decoder-derived wire sizes, excluding locale-sensitive multiplicity.
		// Native execution and other owners without this fact keep the ordinary row walk.
		std::vector<std::size_t> row_wire_base_sizes;
		execution_status status{execution_status::failed_before_result};
		bool ordered{};
		bool input_complete{};
		bool closed_world{};
		std::vector<snapshot_query_coverage> coverage;
		std::vector<std::string> closures;
		std::vector<query_unresolved> unresolved;
		std::vector<claim_conflict> conflict_values;
		std::vector<differential_disagreement> disagreement_values;
		std::vector<claim_producer> producers;
		query_summary_guarantee guarantee{"unknown",
										  "query-empty",
										  {"query-empty", {}},
										  {"query-empty"},
										  {"assumptions:unknown"},
										  {},
										  0U,
										  {},
										  {},
										  {}};
		query_explanation logical;
		query_explanation physical;
		std::string ir_digest;
		std::string snapshot;
		std::string publication;
		// Set only after all stored rows have passed generic input validation.
		bool rows_validated{};
	};

	struct query_transfer_access
	{
		struct evidence_owner_view
		{
			std::span<const annotated_row> rows;
			std::shared_ptr<const query_result::data> owner;
		};
		[[nodiscard]] static evidence_owner_view
		borrow_evidence_owner(const query_result& result) noexcept
		{
			return result.data_ ? evidence_owner_view{result.data_->row_values, result.data_}
								: evidence_owner_view{};
		}
		template <class Evidence, class Step>
		[[nodiscard]] static std::optional<Evidence>
		share_evidence_row(const evidence_owner_view& view,
						   const annotated_row* wanted,
						   std::string_view relation,
						   Step step)
		{
			step(1U);
			if (!view.owner || !wanted || view.rows.data() != view.owner->row_values.data() ||
				view.rows.size() != view.owner->row_values.size())
				return {};
			std::size_t first{}, last = view.rows.size();
			const std::less<const annotated_row*> less;
			while (first < last)
			{
				step(1U);
				const auto middle = first + (last - first) / 2U;
				if (less(&view.rows[middle], wanted))
					first = middle + 1U;
				else
					last = middle;
			}
			step(1U);
			if (first == view.rows.size() || &view.rows[first] != wanted)
				return {};
			step(relation.size() + 1U);
			return Evidence{std::string{relation},
							std::shared_ptr<const annotated_row>{view.owner, wanted},
							typename Evidence::shared_original_tag{}};
		}
		struct row_size_view
		{
			std::span<const annotated_row> rows;
			std::span<const std::size_t> base_sizes;

			template <class Step>
			[[nodiscard]] std::optional<std::size_t> find(const annotated_row* wanted,
														  Step step) const
			{
				step();
				if (rows.size() != base_sizes.size())
					return {};
				std::size_t first{}, last = rows.size();
				const std::less<const annotated_row*> less;
				while (first < last)
				{
					step();
					const auto middle = first + (last - first) / 2U;
					if (less(&rows[middle], wanted))
						first = middle + 1U;
					else
						last = middle;
				}
				step();
				if (first == rows.size() || &rows[first] != wanted ||
					base_sizes[first] == std::numeric_limits<std::size_t>::max())
					return {};
				return base_sizes[first];
			}
		};
		[[nodiscard]] static row_size_view borrow_row_sizes(const query_result& result) noexcept
		{
			if (!result.data_ || !result.data_->rows_validated ||
				result.data_->row_wire_base_sizes.size() != result.data_->row_values.size())
				return {};
			return {result.data_->row_values, result.data_->row_wire_base_sizes};
		}
		template <class Overflow>
		[[nodiscard]] static std::size_t row_size_metadata_bytes(const query_result& result,
																 Overflow overflow)
		{
			if (!result.data_)
				return 0U;
			constexpr auto geometry = sizeof(std::vector<std::size_t>);
			const auto capacity = result.data_->row_wire_base_sizes.capacity();
			if (capacity >
				(std::numeric_limits<std::size_t>::max() - geometry) / sizeof(std::size_t))
				overflow();
			return geometry + capacity * sizeof(std::size_t);
		}
		[[nodiscard]] static std::span<const annotated_row>
		borrow_rows(const query_result& result) noexcept
		{
			return result.data_ ? std::span<const annotated_row>{result.data_->row_values}
								: std::span<const annotated_row>{};
		}
		[[nodiscard]] static bool rows_validated(const query_result& result) noexcept
		{
			return result.data_ && result.data_->rows_validated;
		}
		[[nodiscard]] static query_result make(std::shared_ptr<const query_result::data> data)
		{
			return query_result{std::move(data)};
		}
	};
} // namespace cxxlens::sdk::query
