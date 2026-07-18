#include <array>
#include <iostream>
#include <string>

#include <cxxlens/sdk.hpp>

int main()
{
	using namespace cxxlens::sdk::incremental;
	const auto digest = [](const char value)
	{
		return "sha256:" + std::string(64U, value);
	};
	input_fingerprint input{digest('1'),
							digest('2'),
							digest('3'),
							digest('4'),
							digest('5'),
							digest('6'),
							digest('7'),
							digest('8'),
							digest('9'),
							digest('a'),
							digest('6'),
							digest('7'),
							digest('8'),
							digest('9'),
							"normalizer-v1",
							digest('a'),
							digest('b'),
							"exact"};
	partition_state prior{"partition:example", input, digest('c'), digest('d'), false};
	const std::array candidates{partition_candidate{prior, prior}};
	auto plan = make_materialization_plan(candidates);
	if (!plan || !plan->validate())
		return 1;
	std::cout << (plan->warm_zero ? "warm-zero" : "recompute") << '\n';
	closure_request request{
		{"entity:base"},
		"inheritance-subtype-set",
		{{"entity:base", "entity:derived", {"configured"}, "domain:cpp", {"evidence:example"}}},
		2U,
		1U};
	auto closure = bounded_transitive_closure(request);
	if (!closure || !closure->validate() || !closure->closure_certified)
		return 1;
	std::cout << closure->closure_kind << ':' << closure->rows.size() << '\n';
	return 0;
}
