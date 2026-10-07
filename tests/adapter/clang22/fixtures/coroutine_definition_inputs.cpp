// Original named and lambda coroutine definitions; no system headers.
namespace std
{
	template <class Promise = void>
	struct coroutine_handle
	{
		static coroutine_handle from_address(void*) noexcept
		{
			return {};
		}
		operator coroutine_handle<void>() const noexcept
		{
			return {};
		}
	};
	template <class Return, class... Args>
	struct coroutine_traits
	{
		using promise_type = typename Return::promise_type;
	};
} // namespace std
struct Awaiter
{
	bool await_ready() noexcept
	{
		return false;
	}
	void await_suspend(std::coroutine_handle<>) noexcept {}
	void await_resume() noexcept {}
};
struct Task
{
	struct promise_type
	{
		Task get_return_object()
		{
			return {};
		}
		Awaiter initial_suspend()
		{
			return {};
		}
		Awaiter final_suspend() noexcept
		{
			return {};
		}
		void return_void() noexcept {}
		void unhandled_exception() noexcept {}
	};
};
Task named_coroutine()
{
	co_await Awaiter{};
	co_return;
}
void lambda_coroutine()
{
	auto nested = []() -> Task
	{
		co_await Awaiter{};
		co_return;
	};
	(void)nested;
}
