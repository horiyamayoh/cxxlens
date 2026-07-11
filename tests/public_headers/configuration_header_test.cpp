#include <cxxlens/configuration.hpp>

int main()
{
	auto configuration = cxxlens::configuration::defaults();
	return configuration && configuration.value().validate() ? 0 : 1;
}
