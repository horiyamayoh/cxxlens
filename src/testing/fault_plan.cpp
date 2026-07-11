#include <algorithm>
#include <tuple>
#include <utility>

#include <cxxlens/testing.hpp>

namespace cxxlens::testing
{
	namespace
	{
		[[nodiscard]] auto key(const fault_injection& injection)
		{
			return std::tuple{injection.target, injection.operation, injection.call_index};
		}

		[[nodiscard]] error invalid_fault(std::string reason)
		{
			error failure;
			failure.code.value = "testing.fixture-invalid";
			failure.message = "testing fault plan is invalid";
			failure.attributes.emplace("reason", std::move(reason));
			return failure;
		}
	} // namespace

	result<fault_plan> fault_plan::make(std::vector<fault_injection> injections)
	{
		std::ranges::sort(injections,
						  [](const auto& left, const auto& right)
						  {
							  return key(left) < key(right);
						  });
		for (std::size_t index = 0U; index < injections.size(); ++index)
		{
			const auto& injection = injections.at(index);
			if (injection.operation.empty())
				return invalid_fault("operation-empty");
			if (!stable_code_registry{}.contains(injection.failure.code.value))
				return invalid_fault("error-code-unregistered");
			if (index != 0U && key(injections.at(index - 1U)) == key(injection))
				return invalid_fault("duplicate-fault-key");
		}
		fault_plan plan;
		plan.injections_ = std::move(injections);
		return plan;
	}

	result<void> fault_plan::probe(const fault_target target,
								   const std::string_view operation,
								   const std::uint64_t call_index) const
	{
		const auto requested = std::tuple{target, std::string{operation}, call_index};
		const auto found = std::ranges::lower_bound(injections_,
													requested,
													{},
													[](const fault_injection& injection)
													{
														return key(injection);
													});
		if (found != injections_.end() && key(*found) == requested)
			return found->failure;
		return {};
	}
} // namespace cxxlens::testing
