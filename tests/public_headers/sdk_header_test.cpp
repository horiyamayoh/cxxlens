#include <type_traits>

#include <cxxlens/sdk.hpp>

int main()
{
	static_assert(!std::is_constructible_v<cxxlens::sdk::scalar_value, void*>);
	return 0;
}
