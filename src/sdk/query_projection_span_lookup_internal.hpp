#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <cxxlens/sdk/query.hpp>

namespace cxxlens::sdk::query::detail
{
	struct projection_lookup_empty_metadata
	{
	};
	template <class Budget,
			  class Payload = const annotated_row*,
			  class Metadata = projection_lookup_empty_metadata>
	struct projection_span_lookup
	{
		using key = std::array<std::string_view, 4U>;
		using rows = std::vector<Payload>;
		struct entry
		{
			key identity;
			rows originals;
			[[no_unique_address]] Metadata metadata;
		};
		struct number_less
		{
			Budget* meter;
			bool operator()(std::uint64_t left, std::uint64_t right) const
			{
				meter->work();
				return left < right;
			}
		};
		Budget& meter;
		std::map<std::uint64_t, std::vector<entry>, number_less> buckets;
		explicit projection_span_lookup(Budget& budget)
			: meter(budget), buckets(number_less{&budget})
		{
		}
		std::uint64_t fingerprint(std::string_view id)
		{
			meter.work();
			std::uint64_t value = 14695981039346656037ULL;
			// Buckets only narrow the candidates. Complete four-axis byte equality
			// below admits every row, including arbitrary IDs with equal suffixes.
			meter.work(2U);
			value ^= static_cast<std::uint64_t>(id.size());
			value *= 1099511628211ULL;
			meter.work();
			const auto start = id.size() > 8U ? id.size() - 8U : 0U;
			for (std::size_t i = start; i < id.size(); ++i)
			{
				meter.work(2U);
				value ^= static_cast<unsigned char>(id[i]);
				value *= 1099511628211ULL;
			}
			return value;
		}
		bool equal(const key& left, const key& right)
		{
			for (std::size_t part{}; part < left.size(); ++part)
			{
				meter.work();
				if (left[part].size() != right[part].size())
					return false;
				for (std::size_t i{}; i < left[part].size(); ++i)
				{
					meter.work(2U);
					if (static_cast<unsigned char>(left[part][i]) !=
						static_cast<unsigned char>(right[part][i]))
						return false;
				}
			}
			return true;
		}
		template <class T>
		void append(std::vector<T>& values, T value)
		{
			if (values.size() == values.capacity())
			{
				const auto old = values.capacity();
				if (old > std::numeric_limits<std::size_t>::max() / 2U)
					throw std::length_error("projection-span-capacity");
				const auto next = old ? old * 2U : 1U;
				if (next > std::numeric_limits<std::size_t>::max() / (sizeof(T) * 2U))
					throw std::length_error("projection-span-capacity");
				meter.retain(next * sizeof(T) * 2U);
				meter.temporary_peak = std::max(meter.temporary_peak, meter.retained);
				meter.work(values.size() * (sizeof(T) / sizeof(void*) + 1U) + 1U);
				values.reserve(next);
				meter.retained -= old * sizeof(T) * 2U;
			}
			meter.work(sizeof(T) / sizeof(void*) + 1U);
			values.push_back(std::move(value));
		}
		void add(const key& identity, Payload row, Metadata metadata = {})
		{
			if constexpr (!std::is_empty_v<Metadata>)
				meter.work(2U * (sizeof(Metadata) / sizeof(void*)) + 1U);
			const auto hash = fingerprint(identity[0]);
			auto bucket = buckets.find(hash);
			if (bucket == buckets.end())
			{
				meter.retain(sizeof(typename decltype(buckets)::value_type) + 256U);
				bucket = buckets.try_emplace(hash).first;
			}
			for (auto& candidate : bucket->second)
				if (equal(candidate.identity, identity))
				{
					append(candidate.originals, row);
					return;
				}
			meter.retain(2U * sizeof(entry));
			meter.temporary_peak = std::max(meter.temporary_peak, meter.retained);
			struct frame
			{
				Budget& budget;
				~frame()
				{
					budget.retained -= 2U * sizeof(entry);
				}
			} temporary{meter};
			append(bucket->second, entry{identity, {}, std::move(metadata)});
			append(bucket->second.back().originals, row);
		}
		const rows& find(const key& identity)
		{
			static const rows empty;
			const auto bucket = buckets.find(fingerprint(identity[0]));
			if (bucket == buckets.end())
				return empty;
			for (const auto& candidate : bucket->second)
				if (equal(candidate.identity, identity))
					return candidate.originals;
			return empty;
		}
		template <class Visit>
		void visit(Visit visitor)
		{
			for (auto& [hash, alternatives] : buckets)
			{
				(void)hash;
				meter.work();
				for (auto& candidate : alternatives)
				{
					meter.work();
					visitor(candidate.originals);
				}
			}
		}
	};
} // namespace cxxlens::sdk::query::detail
