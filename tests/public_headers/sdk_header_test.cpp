#include <type_traits>

#include <cxxlens/relations/cc_declaration.hpp>
#include <cxxlens/relations/cc_type_component.hpp>
#include <cxxlens/relations/core_claim_conflict.hpp>
#include <cxxlens/relations/core_differential_disagreement.hpp>
#include <cxxlens/relations/core_provider_execution.hpp>
#include <cxxlens/relations/core_unresolved.hpp>
#include <cxxlens/relations/source_origin.hpp>
#include <cxxlens/sdk.hpp>

int main()
{
	static_assert(!std::is_constructible_v<cxxlens::sdk::scalar_value, void*>);
	return 0;
}
