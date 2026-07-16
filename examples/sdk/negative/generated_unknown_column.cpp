#include <cxxlens/sdk/query.hpp>

struct not_a_generated_column
{
};

int main()
{
	(void)cxxlens::sdk::query::col<not_a_generated_column>(); // expected compile failure
}
