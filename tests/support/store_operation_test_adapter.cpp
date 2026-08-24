#include "store_operation_test_adapter.hpp"

#include <stdexcept>
#include <utility>

namespace cxxlens::test
{
	store_operation_test_adapter::store_operation_test_adapter(
		std::shared_ptr<sdk::store_operation_port> delegate)
		: owned_delegate_(std::move(delegate)), delegate_(owned_delegate_.get())
	{
		if (delegate_ == nullptr)
			throw std::invalid_argument{"Store operation delegate is required"};
	}

	store_operation_test_adapter::store_operation_test_adapter(
		sdk::store_operation_port& delegate) noexcept
		: delegate_(&delegate)
	{
	}

	void store_operation_test_adapter::inject_next_write_resource_exhaustion(
		const int native_code) noexcept
	{
		next_write_resource_exhaustion_ = native_code;
	}

	void store_operation_test_adapter::inject_next_sync_ambiguity(
		const int native_code, const store_operation_ambiguity_side side) noexcept
	{
		next_sync_ambiguity_ = ambiguity_plan{native_code, side};
	}

	void store_operation_test_adapter::inject_next_descriptor_close_ambiguity(
		const int native_code, const store_operation_ambiguity_side side) noexcept
	{
		next_descriptor_close_ambiguity_ = ambiguity_plan{native_code, side};
	}

	void store_operation_test_adapter::inject_next_sqlite_close_ambiguity(
		const int native_code, const store_operation_ambiguity_side side) noexcept
	{
		next_sqlite_close_ambiguity_ = ambiguity_plan{native_code, side};
	}

	void store_operation_test_adapter::inject_next_commit_ambiguity(
		const int native_code, const store_operation_ambiguity_side side) noexcept
	{
		next_commit_ambiguity_ = ambiguity_plan{native_code, side};
	}

	void store_operation_test_adapter::inject_next_backend_fault(
		const sdk::store_backend_operation operation,
		const sdk::store_backend_observation_fault fault,
		const store_operation_ambiguity_side side,
		const int native_code) noexcept
	{
		next_backend_fault_ = backend_fault_plan{operation, fault, native_code, side};
	}

	std::size_t store_operation_test_adapter::write_call_count() const noexcept
	{
		return write_call_count_;
	}

	std::size_t store_operation_test_adapter::sync_call_count() const noexcept
	{
		return sync_call_count_;
	}

	std::size_t store_operation_test_adapter::descriptor_close_call_count() const noexcept
	{
		return descriptor_close_call_count_;
	}

	std::size_t store_operation_test_adapter::sqlite_close_call_count() const noexcept
	{
		return sqlite_close_call_count_;
	}

	std::size_t store_operation_test_adapter::commit_call_count() const noexcept
	{
		return commit_call_count_;
	}

	std::size_t store_operation_test_adapter::backend_observation_call_count() const noexcept
	{
		return backend_observation_call_count_;
	}

	sdk::store_write_outcome
	store_operation_test_adapter::write_exact(const int descriptor,
											  const std::span<const std::byte> bytes) noexcept
	{
		++write_call_count_;
		if (next_write_resource_exhaustion_)
		{
			const auto code = *std::exchange(next_write_resource_exhaustion_, std::nullopt);
			return {sdk::store_write_state::resource_exhausted, 0U, code, true, false};
		}
		return delegate_->write_exact(descriptor, bytes);
	}

	sdk::store_sync_outcome
	store_operation_test_adapter::synchronize(const int descriptor,
											  const sdk::store_sync_target target) noexcept
	{
		++sync_call_count_;
		if (next_sync_ambiguity_)
		{
			const auto plan = *std::exchange(next_sync_ambiguity_, std::nullopt);
			if (plan.side == store_operation_ambiguity_side::after_delegate)
				(void)delegate_->synchronize(descriptor, target);
			return {target, sdk::store_sync_state::outcome_unknown, plan.native_code, true};
		}
		return delegate_->synchronize(descriptor, target);
	}

	sdk::store_close_outcome
	store_operation_test_adapter::close_descriptor(const int descriptor) noexcept
	{
		++descriptor_close_call_count_;
		if (next_descriptor_close_ambiguity_)
		{
			const auto plan = *std::exchange(next_descriptor_close_ambiguity_, std::nullopt);
			if (plan.side == store_operation_ambiguity_side::after_delegate)
				(void)delegate_->close_descriptor(descriptor);
			return {sdk::store_close_state::outcome_unknown, plan.native_code, true};
		}
		return delegate_->close_descriptor(descriptor);
	}

	sdk::store_close_outcome store_operation_test_adapter::close_sqlite(
		const sdk::store_sqlite_operation_binding binding) noexcept
	{
		++sqlite_close_call_count_;
		if (next_sqlite_close_ambiguity_)
		{
			const auto plan = *std::exchange(next_sqlite_close_ambiguity_, std::nullopt);
			if (plan.side == store_operation_ambiguity_side::after_delegate)
				(void)delegate_->close_sqlite(binding);
			return {sdk::store_close_state::outcome_unknown, plan.native_code, true};
		}
		return delegate_->close_sqlite(binding);
	}

	sdk::store_commit_outcome store_operation_test_adapter::commit_sqlite(
		const sdk::store_sqlite_operation_binding binding) noexcept
	{
		++commit_call_count_;
		if (next_commit_ambiguity_)
		{
			const auto plan = *std::exchange(next_commit_ambiguity_, std::nullopt);
			if (plan.side == store_operation_ambiguity_side::after_delegate)
				(void)delegate_->commit_sqlite(binding);
			return {sdk::store_commit_state::outcome_unknown, plan.native_code, true, true};
		}
		return delegate_->commit_sqlite(binding);
	}

	sdk::store_backend_operation_observation
	store_operation_test_adapter::observe_backend_operation(
		const sdk::store_backend_operation_event& event) noexcept
	{
		++backend_observation_call_count_;
		if (next_backend_fault_ &&
			next_backend_fault_->side == store_operation_ambiguity_side::before_delegate &&
			next_backend_fault_->operation == event.operation &&
			event.point == sdk::store_backend_observation_point::before_operation)
		{
			const auto plan = *std::exchange(next_backend_fault_, std::nullopt);
			return {plan.fault,
					plan.native_code,
					event.operation_attempted,
					event.effect_may_have_occurred};
		}
		const auto forwarded = delegate_->observe_backend_operation(event);
		if (next_backend_fault_ &&
			next_backend_fault_->side == store_operation_ambiguity_side::after_delegate &&
			next_backend_fault_->operation == event.operation &&
			event.point == sdk::store_backend_observation_point::after_operation)
		{
			const auto plan = *std::exchange(next_backend_fault_, std::nullopt);
			return {plan.fault,
					plan.native_code,
					event.operation_attempted,
					event.effect_may_have_occurred};
		}
		return forwarded;
	}
} // namespace cxxlens::test
