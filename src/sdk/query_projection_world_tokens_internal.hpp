#pragma once

#include "query_projection_span_lookup_internal.hpp"

namespace cxxlens::sdk::query::detail
{
	struct projection_world_token
	{
		std::size_t slot{};
	};
	struct projection_world_metadata
	{
		mutable projection_world_token world;
	};
	struct projection_world_identity
	{
		std::array<std::string_view, 4U> values;
		mutable projection_world_token world;
		projection_world_identity(std::string_view id,
								  std::string_view universe,
								  std::string_view variant,
								  std::string_view interpretation)
			: values{id, universe, variant, interpretation}
		{
		}
		std::string_view operator[](std::size_t part) const
		{
			return values[part];
		}
		auto begin() const
		{
			return values.begin();
		}
		auto end() const
		{
			return values.end();
		}
	};
	template <class Budget>
	struct projection_world_tokens
	{
		Budget& meter;
		projection_span_lookup<Budget, std::size_t> tokens;
		std::size_t next_slot{1U};
		bool enabled{};
		explicit projection_world_tokens(Budget& budget) : meter(budget), tokens(budget)
		{
			meter.work(sizeof(projection_world_tokens) / sizeof(void*) + 1U);
		}

		template <class Identity>
		projection_world_token ensure(const Identity& stable, projection_world_token& token)
		{
			meter.work();
			if (!token.slot)
			{
				// Keys borrow stable validated input throughout this projector's lifetime.
				// A sampled Variant bucket always retains full three-axis admission.
				meter.work(9U);
				const typename decltype(tokens)::key key{stable[2], stable[1], stable[3], {}};
				if (const auto& found = tokens.find(key); !found.empty())
				{
					meter.work(2U);
					token = {found.front()};
				}
				else
				{
					meter.work(3U);
					if (next_slot == std::numeric_limits<std::size_t>::max())
						throw std::length_error("projection-world-capacity");
					const auto slot = next_slot++;
					tokens.add(key, slot);
					meter.work(2U);
					token = {slot};
				}
			}
			meter.work();
			return token;
		}
		bool same(projection_world_token left, projection_world_token right)
		{
			meter.work(4U);
			return left.slot != 0U && right.slot != 0U && left.slot == right.slot;
		}
		bool exact_id(std::string_view left, std::string_view right)
		{
			meter.work();
			if (left.size() != right.size())
				return false;
			for (std::size_t at{}; at < left.size(); ++at)
			{
				meter.work(2U);
				if (static_cast<unsigned char>(left[at]) != static_cast<unsigned char>(right[at]))
					return false;
			}
			return true;
		}
		template <class Payload>
		const std::vector<Payload>&
		find(projection_span_lookup<Budget, Payload, projection_world_metadata>& lookup,
			 const std::array<std::string_view, 4U>& key,
			 projection_world_token world)
		{
			static const std::vector<Payload> empty;
			meter.work();
			const auto bucket = lookup.buckets.find(lookup.fingerprint(key[0]));
			if (bucket == lookup.buckets.end())
				return empty;
			for (const auto& candidate : bucket->second)
			{
				if (!exact_id(candidate.identity[0], key[0]))
					continue;
				if (same(ensure(candidate.identity, candidate.metadata.world), world))
					return candidate.originals;
			}
			return empty;
		}
	};
} // namespace cxxlens::sdk::query::detail
