#include <string>

#include <cxxlens/provider/clang22.hpp>

int main()
{
	auto detached = cxxlens::provider::clang22::detach_scalar(std::string{"installed"});
	return detached && detached->validate() ? 0 : 1;
}
