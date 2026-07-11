#include <string>

#include <cxxlens/core.hpp>

namespace cxxlens
{

	std::string semantic_version::to_string() const
	{
		std::string result =
			std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
		if (!prerelease.empty())
		{
			result += '-';
			result += prerelease;
		}
		return result;
	}

	api_versions versions()
	{
		constexpr std::uint16_t schema_major = 1U;
		const semantic_version initial_schema{schema_major, 0U, 0U, {}};
		return api_versions{
			.library = semantic_version{0U, 1U, 0U, {}},
			.public_schema = initial_schema,
			.semantics = initial_schema,
			.fact_schema = initial_schema,
			.finding_schema = initial_schema,
			.edit_plan_schema = initial_schema,
			.generation_plan_schema = initial_schema,
			.llvm = semantic_version{22U, 1U, 8U, {}},
		};
	}

} // namespace cxxlens
