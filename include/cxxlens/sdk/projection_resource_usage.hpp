#pragma once
#include <cstddef>
namespace cxxlens::sdk::query
{
	/** Actual charged source-validation operations and conservative charged storage
	 * bound from a successful projection, including its bounded temporary indexes.
	 * These settle an existing caller reservation; they are neither elapsed time
	 * nor quality evidence. Zero on entry and failure. No scalar/closure inference.
	 */
	struct projection_resource_usage
	{
		std::size_t operations{}, retained_bytes_bound{};
	};
} // namespace cxxlens::sdk::query
