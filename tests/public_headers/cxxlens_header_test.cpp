#include <cxxlens/cxxlens.hpp>

int main()
{
	return cxxlens::sdk::semantic_digest("public-header", "cxxlens").empty() ? 1 : 0;
}
