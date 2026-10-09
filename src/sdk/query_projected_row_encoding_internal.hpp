#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <cxxlens/sdk/query.hpp>

#include "json_internal.hpp"

namespace cxxlens::sdk::query::detail
{
	// The row writer and admitted-row byte counter use the same complete wire grammar.
	template <class Sink>
	void emit_row_strings(Sink& sink, const std::span<const std::string> values)
	{
		sink.literal("[");
		bool first = true;
		for (const auto& value : values)
		{
			if (!first)
				sink.literal(",");
			first = false;
			sink.string(value);
		}
		sink.literal("]");
	}

	template <class Sink>
	void emit_row_guarantee(Sink& sink, const claim_guarantee& value)
	{
		sink.literal("{\"approximation\":");
		sink.string(value.approximation);
		sink.literal(",\"assumptions\":");
		sink.string(value.assumptions);
		sink.literal(",\"scope\":");
		sink.string(value.scope);
		sink.literal(",\"verification_modalities\":");
		emit_row_strings(sink, value.verification_modalities);
		sink.literal("}");
	}

	template <class Sink>
	void emit_row_edge(Sink& sink, const query_contributor_edge& value)
	{
		sink.literal("{\"claim_contributor\":");
		sink.string(value.claim_contributor);
		sink.literal(",\"condition_fragments\":");
		emit_row_strings(sink, value.condition.fragments);
		sink.literal(",\"condition_universe\":");
		sink.string(value.condition.universe);
		sink.literal(",\"guarantee\":");
		emit_row_guarantee(sink, value.guarantee);
		sink.literal(",\"interpretation\":");
		sink.string(value.interpretation);
		sink.literal(",\"producer\":{\"id\":");
		sink.string(value.producer.id);
		sink.literal(",\"semantic_contract\":");
		sink.string(value.producer.semantic_contract);
		sink.literal("},\"provenance\":");
		sink.string(value.provenance);
		sink.literal("}");
	}

	template <class Sink>
	void emit_row_scalar(Sink& sink, const scalar_value& value)
	{
		std::visit(
			[&](const auto& item)
			{
				using item_type = std::remove_cvref_t<decltype(item)>;
				if constexpr (std::is_same_v<item_type, bool>)
					sink.literal(item ? "true" : "false");
				else if constexpr (std::is_integral_v<item_type>)
					sink.integer(item);
				else if constexpr (std::is_same_v<item_type, std::string>)
					sink.string(item);
				else
					sink.bytes(item);
			},
			value);
	}

	template <class Sink>
	void emit_row_cell(Sink& sink, const detached_cell& cell)
	{
		switch (cell.state)
		{
			case cell_state::present:
				sink.literal("{\"state\":\"present\",\"type\":");
				sink.string(cell.type.canonical_name());
				sink.literal(",\"value\":");
				emit_row_scalar(sink, *cell.value);
				sink.literal("}");
				return;
			case cell_state::absent:
				sink.literal("{\"state\":\"absent\"}");
				return;
			case cell_state::unknown:
				sink.literal("{\"reason\":");
				sink.string(*cell.unknown_reason);
				sink.literal(",\"state\":\"unknown\"}");
				return;
		}
		sink.literal("{}");
	}

	template <class Sink>
	void emit_projected_row(Sink& sink,
							const annotated_row& row,
							const std::span<const claim_guarantee> guarantees,
							const std::span<const query_contributor_edge> edges)
	{
		sink.literal("{\"claim_contributors\":");
		emit_row_strings(sink, row.claim_contributors);
		sink.literal(",\"condition_fragments\":");
		emit_row_strings(sink, row.presence.fragments);
		sink.literal(",\"condition_universe\":");
		sink.string(row.presence.universe);
		sink.literal(",\"contributor_guarantees\":[");
		bool first = true;
		for (const auto& guarantee : guarantees)
		{
			if (!first)
				sink.literal(",");
			first = false;
			emit_row_guarantee(sink, guarantee);
		}
		sink.literal("],\"interpretation\":");
		sink.string(row.interpretation);
		sink.literal(",\"contributor_edges\":[");
		first = true;
		for (const auto& edge : edges)
		{
			if (!first)
				sink.literal(",");
			first = false;
			emit_row_edge(sink, edge);
		}
		sink.literal("],\"multiplicity\":");
		sink.multiplicity(row.multiplicity);
		sink.literal(",\"producer_contracts\":[");
		first = true;
		for (const auto& producer : row.producer_contracts)
		{
			if (!first)
				sink.literal(",");
			first = false;
			sink.literal("{\"id\":");
			sink.string(producer.id);
			sink.literal(",\"semantic_contract\":");
			sink.string(producer.semantic_contract);
			sink.literal("}");
		}
		sink.literal("],\"provenance\":");
		emit_row_strings(sink, row.provenance);
		sink.literal(",\"values\":{");
		first = true;
		for (const auto& [column, cell] : row.values)
		{
			if (!first)
				sink.literal(",");
			first = false;
			sink.string(column);
			sink.literal(":");
			emit_row_cell(sink, cell);
		}
		sink.literal("}}");
	}

	struct projected_row_string_sink
	{
		std::string value;
		void literal(const std::string_view text)
		{
			value.append(text);
		}
		void string(const std::string_view text)
		{
			value += cxxlens::sdk::detail::canonical_json_string(text);
		}
		template <class Integer>
		void integer(const Integer number)
		{
			value += std::to_string(number);
		}
		void multiplicity(const std::uint64_t number)
		{
			// Preserve the original stream spelling of row multiplicity.
			std::ostringstream output;
			output << number;
			value += output.str();
		}
		void bytes(const std::span<const std::byte> data)
		{
			constexpr std::string_view digits{"0123456789abcdef"};
			value.push_back('"');
			for (const auto byte : data)
			{
				const auto number = std::to_integer<unsigned char>(byte);
				value.push_back(digits[number >> 4U]);
				value.push_back(digits[number & 0x0fU]);
			}
			value.push_back('"');
		}
	};

	template <class Step, class Overflow>
	class projected_row_size_sink
	{
	  public:
		projected_row_size_sink(Step step, Overflow overflow)
			: step_{std::move(step)}, overflow_{std::move(overflow)}
		{
		}
		[[nodiscard]] std::size_t size() const noexcept
		{
			return size_;
		}
		void literal(const std::string_view text)
		{
			step_();
			add(text.size());
		}
		void string(const std::string_view text)
		{
			step_();
			std::size_t encoded = 2U;
			const auto invalid = cxxlens::sdk::detail::invalid_utf8_offset(
				text,
				[&]
				{
					step_();
				},
				[&](const unsigned char byte)
				{
					const auto width = byte == '"' || byte == '\\' || byte == '\b' ||
							byte == '\f' || byte == '\n' || byte == '\r' || byte == '\t'
						? 2U
						: (byte < 0x20U ? 6U : 1U);
					if (width > std::numeric_limits<std::size_t>::max() - encoded)
						overflow_();
					encoded += width;
				});
			// The public encoder emits an empty quoted string for invalid UTF-8.
			add(invalid ? 2U : encoded);
		}
		template <class Integer>
		void integer(const Integer number)
		{
			step_();
			using unsigned_type = std::make_unsigned_t<Integer>;
			unsigned_type remaining{};
			std::size_t digits{};
			if constexpr (std::is_signed_v<Integer>)
			{
				if (number < 0)
				{
					remaining = static_cast<unsigned_type>(-(number + 1));
					++remaining;
					++digits;
				}
				else
					remaining = static_cast<unsigned_type>(number);
			}
			else
				remaining = number;
			do
			{
				step_();
				++digits;
				remaining /= 10U;
			} while (remaining != 0U);
			add(digits);
		}
		void multiplicity(const std::uint64_t number)
		{
			step_();
			std::ostringstream output;
			output << number;
			const auto text = output.str();
			for (std::size_t i{}; i < text.size(); ++i)
				step_();
			add(text.size());
		}
		void bytes(const std::span<const std::byte> data)
		{
			step_();
			if (data.size() > (std::numeric_limits<std::size_t>::max() - 2U) / 2U)
				overflow_();
			add(data.size() * 2U + 2U);
		}

	  private:
		void add(const std::size_t amount)
		{
			if (amount > std::numeric_limits<std::size_t>::max() - size_)
				overflow_();
			size_ += amount;
		}
		Step step_;
		Overflow overflow_;
		std::size_t size_{};
	};

	// Only rows already admitted as canonical may borrow their guarantee/edge order.
	template <class Step, class Overflow>
	[[nodiscard]] std::size_t
	admitted_projected_row_size(const annotated_row& row, Step step, Overflow overflow)
	{
		projected_row_size_sink sink{std::move(step), std::move(overflow)};
		emit_projected_row(sink, row, row.contributor_guarantees, row.contributor_edges);
		return sink.size();
	}
} // namespace cxxlens::sdk::query::detail
