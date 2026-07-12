#pragma once

#include <atomic>
#include <thread>

#include <cxxlens/core/failure.hpp>

namespace cxxlens::detail::frontend
{
	class borrowed_lifetime_token
	{
	  public:
		borrowed_lifetime_token() noexcept : owner_{std::this_thread::get_id()} {}
		borrowed_lifetime_token(const borrowed_lifetime_token&) = delete;
		borrowed_lifetime_token& operator=(const borrowed_lifetime_token&) = delete;

		[[nodiscard]] result<void> check() const;
		[[nodiscard]] bool active_on_owner() const noexcept;
		void retire() noexcept;

	  private:
		std::thread::id owner_;
		std::atomic_bool active_{true};
	};
} // namespace cxxlens::detail::frontend
