#include "sqlite_store_fault_injection_internal.hpp"

#include <limits>

namespace cxxlens::sdk
{
	namespace
	{
		thread_local sqlite_store_fault_scope* active_scope{};
	} // namespace

	sqlite_store_fault_scope::sqlite_store_fault_scope(sqlite_store_fault_plan plan) noexcept
		: plan_{plan}
	{
		if (!valid_sqlite_store_fault_plan(plan_))
		{
			observation_.invalid_plan = true;
			return;
		}
		if (active_scope != nullptr)
		{
			observation_.nested_scope_suppressed = true;
			return;
		}
		active_scope = this;
		active_ = true;
	}

	sqlite_store_fault_scope::~sqlite_store_fault_scope() noexcept
	{
		if (active_ && active_scope == this)
			active_scope = nullptr;
	}

	bool sqlite_store_fault_scope::active() const noexcept
	{
		return active_;
	}

	const sqlite_store_fault_plan& sqlite_store_fault_scope::plan() const noexcept
	{
		return plan_;
	}

	sqlite_store_fault_observation sqlite_store_fault_scope::observation() const noexcept
	{
		return observation_;
	}

	void sqlite_store_fault_scope::increment(std::uint64_t& value) noexcept
	{
		if (value == std::numeric_limits<std::uint64_t>::max())
		{
			observation_.count_overflow = true;
			return;
		}
		++value;
	}

	sqlite_store_fault_directive
	sqlite_store_fault_scope::dispatch(const sqlite_store_fault_event& event) noexcept
	{
		increment(observation_.observed_event_count);
		observation_.last_observed_event = event;
		observation_.has_last_observed_event = true;
		if (!valid_sqlite_store_fault_event(event))
		{
			observation_.invalid_event_observed = true;
			return {event, sqlite_store_fault_action::none, false};
		}
		if (event != plan_.event)
			return {event, sqlite_store_fault_action::none, false};

		increment(observation_.matching_event_count);
		observation_.matched_event = event;
		observation_.has_matched_event = true;
		if (plan_.action == sqlite_store_fault_action::observe_only)
			return {event, sqlite_store_fault_action::none, false};
		if (directive_issued_)
			return {event, sqlite_store_fault_action::none, false};

		directive_issued_ = true;
		increment(observation_.issued_directive_count);
		return {event, plan_.action, true};
	}

	sqlite_store_fault_directive
	dispatch_sqlite_store_fault(const sqlite_store_fault_event& event) noexcept
	{
		if (active_scope == nullptr)
			return {event, sqlite_store_fault_action::none, false};
		return active_scope->dispatch(event);
	}
} // namespace cxxlens::sdk
