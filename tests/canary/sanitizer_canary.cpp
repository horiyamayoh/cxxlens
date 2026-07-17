#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>
#include <thread>

namespace
{
#if defined(_MSC_VER)
#define CXXLENS_CANARY_NOINLINE __declspec(noinline)
#else
#define CXXLENS_CANARY_NOINLINE [[gnu::noinline]]
#endif

	CXXLENS_CANARY_NOINLINE auto address_violation() -> int
	{
		auto allocation = std::make_unique<int[]>(1);
		allocation[0] = 7;
		volatile int* escaped = allocation.get();
		allocation.reset();
		return escaped[0];
	}

	CXXLENS_CANARY_NOINLINE auto undefined_violation() -> std::int32_t
	{
		volatile std::int32_t maximum = std::numeric_limits<std::int32_t>::max();
		return maximum + 1;
	}

	CXXLENS_CANARY_NOINLINE auto thread_violation() -> int
	{
		int shared = 0;
		auto increment = [&shared]
		{
			for (int index = 0; index < 100'000; ++index)
			{
				++shared;
			}
		};
		std::thread first(increment);
		std::thread second(increment);
		first.join();
		second.join();
		return shared;
	}

#undef CXXLENS_CANARY_NOINLINE

} // namespace

auto main(int argc, char** argv) -> int
{
	if (argc != 2)
	{
		return 64;
	}
	const std::string_view mode(argv[1]);
	if (mode == "address")
	{
		return address_violation();
	}
	if (mode == "undefined")
	{
		return undefined_violation();
	}
	if (mode == "thread")
	{
		return thread_violation() == 0 ? 1 : 0;
	}
	return 64;
}
