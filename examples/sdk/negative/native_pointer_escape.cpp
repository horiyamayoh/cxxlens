#include <cxxlens/provider/clang22.hpp>

int main()
{
	int value = 0;
	(void)cxxlens::provider::clang22::detach_scalar(&value); // expected compile failure
}
