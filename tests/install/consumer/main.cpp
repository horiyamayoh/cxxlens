#include <cxxlens/cxxlens.hpp>

int main()
{
	return cxxlens::sdk::semantic_digest("installed-core", "cxxlens").empty() ? 1 : 0;
}
