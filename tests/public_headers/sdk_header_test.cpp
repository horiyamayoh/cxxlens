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
	static_assert(!std::is_default_constructible_v<cxxlens::sdk::capture_bundle>);
	static_assert(!std::is_default_constructible_v<cxxlens::sdk::replay_plan>);
	static_assert(!std::is_default_constructible_v<cxxlens::sdk::imported_project>);
	static_assert(!std::is_default_constructible_v<cxxlens::sdk::materialization_request>);
	static_assert(!std::is_default_constructible_v<cxxlens::sdk::materialization_result>);
	static_assert(cxxlens::sdk::is_valid(cxxlens::sdk::replay_fidelity::unsupported));
	static_assert(
		cxxlens::sdk::is_valid(cxxlens::sdk::materialization_terminal::published_partial));
	return 0;
}
