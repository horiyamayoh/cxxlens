#pragma once
#include <limits>
#include <stdexcept>

#include <cxxlens/sdk/object_semantics.hpp>

namespace cxxlens::sdk::query::object_detail
{
	struct failure
	{
		error value;
	};
	[[noreturn]] inline void fail(std::string_view field,
								  std::string_view reason,
								  std::string_view code = "sdk.object-input-invalid")
	{
		throw failure{{std::string{code}, std::string{field}, std::string{reason}}};
	}
	struct budget
	{
		finite_population_limits limits;
		std::stop_token stop;
		std::size_t operations{}, retained{}, rows{}, conditions{}, members{}, evidence{},
			references{};
		void charge(std::size_t& used, std::size_t n, std::size_t cap, std::string_view field)
		{
			if (used > cap || n > cap - used)
				fail(field, "limit-exceeded", "sdk.object-budget");
			used += n;
		}
		void work(std::size_t n = 1U)
		{
			if (stop.stop_requested() || (limits.cancelled && limits.cancelled()))
				fail("projection", "stop-requested", "sdk.object-cancelled");
			charge(operations, n, limits.maximum_operations, "operations");
		}
		void retain(std::size_t n)
		{
			charge(retained, n, limits.maximum_retained_bytes, "retained-bytes");
		}
		void member()
		{
			work();
			charge(members, 1U, limits.maximum_members, "members");
		}
	};
	inline constexpr std::string_view path_magic{"cxxlens-object-path/1\0", 22U};
	inline void validate_step(const compiler_object_path_step& step)
	{
		using k = compiler_object_path_step::kind;
		if (step.category > k::base)
			fail("storage_path", "unknown-step");
		if (step.category <= k::vector)
		{
			if (!step.member_usr.empty() || step.virtual_base)
				fail("storage_path", "index-step-payload");
		}
		else if (step.member_usr.empty() || step.index ||
				 (step.category == k::field && step.virtual_base))
			fail("storage_path", "member-step-payload");
	}
	inline std::vector<compiler_object_path_step> decode_path(std::span<const std::byte> original,
															  budget& b)
	{
		b.work();
		if (original.size() < path_magic.size() + 4U)
			fail("storage_path", "truncated-header");
		for (std::size_t i{}; i < path_magic.size(); ++i)
		{
			b.work();
			if (original[i] != static_cast<std::byte>(path_magic[i]))
				fail("storage_path", "profile-magic");
		}
		std::size_t at = path_magic.size();
		const auto read = [&](unsigned bytes)
		{
			b.work(bytes);
			if (bytes > original.size() - at)
				fail("storage_path", "truncated-step");
			std::uint64_t result{};
			for (unsigned i{}; i < bytes; ++i)
				result |= static_cast<std::uint64_t>(std::to_integer<unsigned>(original[at++]))
					<< (8U * i);
			return result;
		};
		const auto count = read(4U);
		if (count > b.limits.maximum_members || count > (original.size() - at) / 14U)
			fail("storage_path", "step-count", "sdk.object-budget");
		b.retain(static_cast<std::size_t>(count) * sizeof(compiler_object_path_step));
		std::vector<compiler_object_path_step> result;
		result.reserve(static_cast<std::size_t>(count));
		for (std::uint64_t i{}; i < count; ++i)
		{
			b.member();
			compiler_object_path_step step;
			step.category = static_cast<compiler_object_path_step::kind>(read(1U));
			step.index = read(8U);
			const auto virtual_flag = read(1U);
			if (virtual_flag > 1U)
				fail("storage_path", "virtual-flag");
			step.virtual_base = virtual_flag != 0U;
			const auto size = read(4U);
			if (size > original.size() - at)
				fail("storage_path", "truncated-usr");
			b.retain(static_cast<std::size_t>(size));
			b.work(static_cast<std::size_t>(size));
			step.member_usr.assign(original.begin() + static_cast<std::ptrdiff_t>(at),
								   original.begin() + static_cast<std::ptrdiff_t>(at + size));
			at += static_cast<std::size_t>(size);
			validate_step(step);
			result.push_back(std::move(step));
		}
		if (at != original.size())
			fail("storage_path", "trailing-bytes");
		return result;
	}
	inline std::vector<std::byte> encode_path(std::span<const compiler_object_path_step> steps,
											  budget& b)
	{
		b.work();
		if (steps.size() > std::numeric_limits<std::uint32_t>::max() ||
			steps.size() > b.limits.maximum_members)
			fail("storage_path", "step-count", "sdk.object-budget");
		std::size_t size = path_magic.size() + 4U;
		for (const auto& step : steps)
		{
			b.member();
			validate_step(step);
			if (step.member_usr.size() > std::numeric_limits<std::uint32_t>::max() ||
				step.member_usr.size() > b.limits.maximum_retained_bytes ||
				size > b.limits.maximum_retained_bytes - step.member_usr.size() ||
				b.limits.maximum_retained_bytes < 14U ||
				size + step.member_usr.size() > b.limits.maximum_retained_bytes - 14U)
				fail("storage_path", "encoded-bytes", "sdk.object-budget");
			size += 14U + step.member_usr.size();
		}
		b.retain(size);
		std::vector<std::byte> output;
		output.reserve(size);
		const auto append = [&](std::uint64_t value, unsigned bytes)
		{
			b.work(bytes);
			for (unsigned i{}; i < bytes; ++i)
				output.push_back(static_cast<std::byte>((value >> (8U * i)) & 255U));
		};
		for (char c : path_magic)
		{
			b.work();
			output.push_back(static_cast<std::byte>(c));
		}
		append(steps.size(), 4U);
		for (const auto& step : steps)
		{
			append(static_cast<std::uint8_t>(step.category), 1U);
			append(step.index, 8U);
			append(step.virtual_base ? 1U : 0U, 1U);
			append(step.member_usr.size(), 4U);
			b.work(step.member_usr.size());
			output.insert(output.end(), step.member_usr.begin(), step.member_usr.end());
		}
		return output;
	}
	template <class T, class Work>
	result<T> capture(Work&& work)
	{
		try
		{
			return work();
		}
		catch (const failure& e)
		{
			return e.value;
		}
		catch (const std::bad_alloc&)
		{
			return error{"sdk.object-resource-exhausted", "projection", "allocation"};
		}
		catch (const std::length_error&)
		{
			return error{"sdk.object-resource-exhausted", "projection", "length"};
		}
	}
} // namespace cxxlens::sdk::query::object_detail
