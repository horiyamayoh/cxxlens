#pragma once

#include <cstdint>
#include <memory>

#include "materialization_io.hpp"
#include "provider_task_v3.hpp"

namespace cxxlens::detail::clang22::materialization
{
	inline constexpr std::uint64_t maximum_clang22_task_source_bytes = 16U * 1024U * 1024U;
	inline constexpr std::uint64_t maximum_clang22_task_input_bytes = 64U * 1024U * 1024U;

	/** Private append/seal/replay port for one exact canonical task.v3 occurrence. */
	class clang22_task_input_spool : public clang22_task_input_sink,
									 public clang22_task_input_replay
	{
	  public:
		~clang22_task_input_spool() override = default;
		[[nodiscard]] virtual sdk::result<void> seal() = 0;
	};

	/** Create one anonymous source spool with the exact 16 MiB/source receipt contract. */
	[[nodiscard]] sdk::result<std::unique_ptr<clang22_task_source_spool>>
	make_materialization_task_source_spool();

	/**
	 * Dependency-injected source-spool construction used by private fault tests.
	 * Both dependencies transfer ownership and must be non-null.
	 */
	[[nodiscard]] sdk::result<std::unique_ptr<clang22_task_source_spool>>
	make_materialization_task_source_spool(
		std::unique_ptr<materialization_replayable_spool> storage,
		std::unique_ptr<materialization_digest_accumulator> content_digest);

	/** Create one anonymous canonical task.v3 spool with the exact 64 MiB contract. */
	[[nodiscard]] sdk::result<std::unique_ptr<clang22_task_input_spool>>
	make_materialization_task_input_spool();

	/** Dependency-injected task-input construction used by private fault tests. */
	[[nodiscard]] sdk::result<std::unique_ptr<clang22_task_input_spool>>
	make_materialization_task_input_spool(
		std::unique_ptr<materialization_replayable_spool> storage);
} // namespace cxxlens::detail::clang22::materialization
