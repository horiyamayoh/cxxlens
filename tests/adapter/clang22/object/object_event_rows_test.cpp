#include <cstdlib>
#include <iostream>

#include "project_object_event_rows.hpp"
namespace n = cxxlens::detail::clang22::object_semantics;
void require(bool v, const char* message)
{
	if (!v)
	{
		std::cerr << message << '\n';
		std::exit(1);
	}
}
std::string_view text(const n::project_object_event_rows& rows, std::string_view key)
{
	const auto& cell =
		rows.inventory_facets.at(std::string{"cc.declaration_inventory.v1."} + std::string{key});
	return cell.value ? std::get<std::string>(*cell.value) : std::string_view{};
}
int main()
{
	unsigned cases{};
	n::object_event_bindings bindings;
	bindings.compile_unit = "original-unit";
	{
		n::project_object_observations input;
		require(!n::detach_project_object_events(input, bindings), "unfrozen parser admitted");
		++cases;
	}
	{
		n::project_object_observations input;
		input.frozen = input.hooks_installed = true;
		auto out = n::detach_project_object_events(input, bindings);
		require(bool(out), "known empty stream rejected");
		require(out->rows.empty() && text(*out, "sequence_pair_state") == "complete" &&
					text(*out, "object_state_state") == "complete",
				"finite empty stream changed independent state");
		require(std::get<std::uint64_t>(
					*out->inventory_facets.at("cc.declaration_inventory.v1.sequence_pair_count")
						 .value) == 0U,
				"finite empty count missing");
		++cases;
	}
	{
		n::project_object_observations input;
		input.frozen = true;
		auto out = n::detach_project_object_events(input, bindings);
		require(bool(out), "stock stream rejection");
		require(
			text(*out, "sequence_pair_state") == "unavailable" &&
				!out->inventory_facets.contains("cc.declaration_inventory.v1.sequence_pair_count"),
			"stock no hooks fabricated zero");
		++cases;
	}
	{
		n::project_object_observations input;
		input.frozen = input.hooks_installed = true;
		input.sequence_partial = true;
		auto out = n::detach_project_object_events(input, bindings);
		require(bool(out) && text(*out, "sequence_pair_state") == "partial" &&
					text(*out, "object_state_state") == "complete",
				"one event frontier poisoned independent stream");
		++cases;
	}
	{
		n::project_object_observations input;
		input.frozen = input.hooks_installed = true;
		auto out = n::detach_project_object_events(input, bindings);
		require(bool(out), "empty stream default cap");
		n::object_facet_limits limits;
		limits.maximum_operations = out->operations;
		limits.maximum_retained_bytes = out->retained_bytes_bound;
		require(bool(n::detach_project_object_events(input, bindings, limits)),
				"measured empty output not reproducible");
		--limits.maximum_retained_bytes;
		require(!n::detach_project_object_events(input, bindings, limits),
				"below measured bytes accepted");
		++cases;
	}
	{
		n::project_object_observations input;
		input.frozen = true;
		n::object_facet_limits limits;
		limits.maximum_fields = 1U;
		require(!n::detach_project_object_events(input, bindings, limits),
				"inventory fields exceeded cap");
		++cases;
	}
	{
		n::project_object_observations input;
		input.frozen = true;
		n::object_facet_limits limits;
		unsigned checkpoints{};
		limits.cancelled = [&]
		{
			return ++checkpoints == 4U;
		};
		require(!n::detach_project_object_events(input, bindings, limits),
				"nested cancellation bypassed");
		++cases;
	}
	std::cout << cases << " pure original object event-row cases PASS\n";
}
