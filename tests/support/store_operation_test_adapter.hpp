#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "sdk/store_operation_port_internal.hpp"

namespace cxxlens::test
{
	enum class store_operation_ambiguity_side : std::uint8_t
	{
		before_delegate,
		after_delegate,
	};

	/**
	 * Tests-only decorator for the Store effect boundary.
	 *
	 * Each planned result is consumed by exactly one matching operation. Unplanned operations are
	 * forwarded to the real or recording delegate, so production code needs no mutable fault state.
	 */
	class store_operation_test_adapter final : public sdk::store_operation_port
	{
	  public:
		explicit store_operation_test_adapter(sdk::store_operation_port& delegate) noexcept;

		void inject_next_write_resource_exhaustion(int native_code) noexcept;
		void inject_next_sync_ambiguity(int native_code,
										store_operation_ambiguity_side side) noexcept;
		void inject_next_descriptor_close_ambiguity(int native_code,
													store_operation_ambiguity_side side) noexcept;
		void inject_next_sqlite_close_ambiguity(int native_code,
												store_operation_ambiguity_side side) noexcept;
		void inject_next_commit_ambiguity(int native_code,
										  store_operation_ambiguity_side side) noexcept;

		[[nodiscard]] std::size_t write_call_count() const noexcept;
		[[nodiscard]] std::size_t sync_call_count() const noexcept;
		[[nodiscard]] std::size_t descriptor_close_call_count() const noexcept;
		[[nodiscard]] std::size_t sqlite_close_call_count() const noexcept;
		[[nodiscard]] std::size_t commit_call_count() const noexcept;

		[[nodiscard]] sdk::store_write_outcome
		write_exact(int descriptor, std::span<const std::byte> bytes) noexcept override;
		[[nodiscard]] sdk::store_sync_outcome
		synchronize(int descriptor, sdk::store_sync_target target) noexcept override;
		[[nodiscard]] sdk::store_close_outcome close_descriptor(int descriptor) noexcept override;
		[[nodiscard]] sdk::store_close_outcome
		close_sqlite(sdk::store_sqlite_operation_binding binding) noexcept override;
		[[nodiscard]] sdk::store_commit_outcome
		commit_sqlite(sdk::store_sqlite_operation_binding binding) noexcept override;

	  private:
		struct ambiguity_plan
		{
			int native_code{};
			store_operation_ambiguity_side side{store_operation_ambiguity_side::before_delegate};
		};

		sdk::store_operation_port& delegate_;
		std::optional<int> next_write_resource_exhaustion_;
		std::optional<ambiguity_plan> next_sync_ambiguity_;
		std::optional<ambiguity_plan> next_descriptor_close_ambiguity_;
		std::optional<ambiguity_plan> next_sqlite_close_ambiguity_;
		std::optional<ambiguity_plan> next_commit_ambiguity_;
		std::size_t write_call_count_{};
		std::size_t sync_call_count_{};
		std::size_t descriptor_close_call_count_{};
		std::size_t sqlite_close_call_count_{};
		std::size_t commit_call_count_{};
	};
} // namespace cxxlens::test
