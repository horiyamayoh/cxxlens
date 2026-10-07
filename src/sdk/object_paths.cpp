#include "object_path_internal.hpp"
namespace cxxlens::sdk::query
{
	result<std::vector<compiler_object_path_step>> decode_compiler_object_path(
		std::span<const std::byte> original, finite_population_limits limits, std::stop_token stop)
	{
		if (auto valid = limits.validate(); !valid)
			return valid.error();
		object_detail::budget b{limits, stop};
		return object_detail::capture<std::vector<compiler_object_path_step>>(
			[&]
			{
				return object_detail::decode_path(original, b);
			});
	}
	result<std::vector<std::byte>>
	encode_compiler_object_path(std::span<const compiler_object_path_step> original,
								finite_population_limits limits,
								std::stop_token stop)
	{
		if (auto valid = limits.validate(); !valid)
			return valid.error();
		object_detail::budget b{limits, stop};
		return object_detail::capture<std::vector<std::byte>>(
			[&]
			{
				return object_detail::encode_path(original, b);
			});
	}
} // namespace cxxlens::sdk::query
