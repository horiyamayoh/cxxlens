#pragma once

#include <cstddef>

namespace cxxlens::detail::clang22::materialization
{
	/** Bounds shared by the source-private prior artifact codecs. */
	struct materialization_prior_artifact_limits
	{
		static constexpr std::size_t default_max_bytes = 1024U * 1024U * 1024U;

		std::size_t max_bytes{default_max_bytes};
		std::size_t max_tasks{4096U};
		std::size_t max_capture_bytes{default_max_bytes};
		std::size_t max_total_capture_bytes{default_max_bytes};
		std::size_t max_batches_per_task{6U};
		std::size_t max_chunks_per_batch{65536U};
		std::size_t max_side_channel_records{65536U};
		std::size_t max_string_bytes{16U * 1024U * 1024U};
	};
} // namespace cxxlens::detail::clang22::materialization
