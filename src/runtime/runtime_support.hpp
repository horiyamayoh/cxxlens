#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <variant>

namespace cxxlens::detail::runtime
{

	enum class runtime_status : std::uint8_t
	{
		permission_denied,
		missing,
		short_read,
		resource_exhausted,
		timed_out,
		output_limit,
		cancelled,
		invalid_request,
		platform_failure,
	};

	struct runtime_failure
	{
		runtime_status status{runtime_status::platform_failure};
		std::string operation;
		int platform_code{};
	};

	struct unexpected
	{
		runtime_failure failure;
	};

	template <typename T>
	class runtime_result
	{
	  public:
		runtime_result(T value) : storage_{std::move(value)} {}
		runtime_result(unexpected failure) : storage_{std::move(failure.failure)} {}

		[[nodiscard]] bool has_value() const noexcept
		{
			return std::holds_alternative<T>(storage_);
		}
		explicit operator bool() const noexcept
		{
			return has_value();
		}
		[[nodiscard]] T& value()
		{
			return std::get<T>(storage_);
		}
		[[nodiscard]] const T& value() const
		{
			return std::get<T>(storage_);
		}
		[[nodiscard]] runtime_failure& error()
		{
			return std::get<runtime_failure>(storage_);
		}
		[[nodiscard]] const runtime_failure& error() const
		{
			return std::get<runtime_failure>(storage_);
		}
		[[nodiscard]] T& operator*()
		{
			return value();
		}
		[[nodiscard]] const T& operator*() const
		{
			return value();
		}
		[[nodiscard]] T* operator->()
		{
			return &value();
		}
		[[nodiscard]] const T* operator->() const
		{
			return &value();
		}
		template <typename U>
		[[nodiscard]] T value_or(U&& fallback) const
		{
			return has_value() ? value() : static_cast<T>(std::forward<U>(fallback));
		}

	  private:
		std::variant<T, runtime_failure> storage_;
	};

	struct request_context
	{
		std::string operation;
		std::uint64_t call_index{};
		std::stop_token cancellation;
		std::optional<std::chrono::steady_clock::time_point> deadline;
		std::size_t output_limit{std::size_t{1024U} * 1024U};

		[[nodiscard]] bool cancelled() const noexcept
		{
			return cancellation.stop_requested();
		}
	};

} // namespace cxxlens::detail::runtime
