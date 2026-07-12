#include "borrowed_lifetime.hpp"

#include <string>

namespace cxxlens::detail::frontend
{
	result<void> borrowed_lifetime_token::check() const
	{
		if (active_on_owner())
		{
			return {};
		}
		error failure;
		failure.code.value = "interop.borrowed-lifetime-violation";
		failure.message = "Borrowed Clang state is outside its callback lifetime or owning thread";
		failure.scope = failure_scope::operation;
		failure.attributes.emplace(
			"reason", active_.load(std::memory_order_acquire) ? "wrong-thread" : "retired");
		return failure;
	}

	bool borrowed_lifetime_token::active_on_owner() const noexcept
	{
		return active_.load(std::memory_order_acquire) && owner_ == std::this_thread::get_id();
	}

	void borrowed_lifetime_token::retire() noexcept
	{
		active_.store(false, std::memory_order_release);
	}
} // namespace cxxlens::detail::frontend
