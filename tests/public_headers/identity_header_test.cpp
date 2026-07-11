#include <string>

#include <cxxlens/core/identity.hpp>

auto main() -> int
{
	const cxxlens::fact_id id{"fact_" + std::string(64U, 'a')};
	return id.valid() && id.full_digest().size() == 64U ? 0 : 1;
}
