#include "sqlite_store_fault_injection_internal.hpp"

namespace cxxlens::sdk
{
	sqlite_store_fault_directive
	dispatch_sqlite_store_fault(const sqlite_store_fault_event& event) noexcept
	{
		return {event, sqlite_store_fault_action::none, false};
	}
} // namespace cxxlens::sdk
