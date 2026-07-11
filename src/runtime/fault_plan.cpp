#include "fault_plan.hpp"

#include <utility>

namespace cxxlens::detail::runtime
{

	fault_plan& fault_plan::fail(std::string operation,
								 const std::uint64_t call_index,
								 const runtime_status status)
	{
		auto stored_operation = operation;
		failures_.insert_or_assign(key{std::move(operation), call_index},
								   runtime_failure{status, std::move(stored_operation), 0});
		return *this;
	}

	const runtime_failure* fault_plan::match(const request_context& context) const noexcept
	{
		const auto found = failures_.find(key{context.operation, context.call_index});
		return found == failures_.end() ? nullptr : &found->second;
	}

} // namespace cxxlens::detail::runtime
