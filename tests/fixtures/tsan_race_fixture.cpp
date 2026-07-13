#include <cstdint>
#include <iostream>
#include <thread>

#include <barrier>

namespace
{
	std::uint64_t shared_counter{};
} // namespace

int main()
{
	std::barrier start{3};
	const auto increment = [&start]
	{
		start.arrive_and_wait();
		for (std::uint64_t index{}; index < 100'000U; ++index)
			++shared_counter;
	};

	std::jthread first{increment};
	std::jthread second{increment};
	start.arrive_and_wait();
	first.join();
	second.join();
	std::cout << shared_counter << '\n';
}
