#pragma once

#include <chrono>

namespace cxxlens::detail::runtime
{

	class time_port
	{
	  public:
		virtual ~time_port() = default;
		[[nodiscard]] virtual std::chrono::system_clock::time_point wall_now() const noexcept = 0;
		[[nodiscard]] virtual std::chrono::steady_clock::time_point steady_now() const noexcept = 0;
	};

	class real_time_adapter final : public time_port
	{
	  public:
		[[nodiscard]] std::chrono::system_clock::time_point wall_now() const noexcept override;
		[[nodiscard]] std::chrono::steady_clock::time_point steady_now() const noexcept override;
	};

	class fixed_time_adapter final : public time_port
	{
	  public:
		fixed_time_adapter(std::chrono::system_clock::time_point wall,
						   std::chrono::steady_clock::time_point steady) noexcept;
		[[nodiscard]] std::chrono::system_clock::time_point wall_now() const noexcept override;
		[[nodiscard]] std::chrono::steady_clock::time_point steady_now() const noexcept override;

	  private:
		std::chrono::system_clock::time_point wall_;
		std::chrono::steady_clock::time_point steady_;
	};

} // namespace cxxlens::detail::runtime
