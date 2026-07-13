#include "select_search_explain.hpp"

#include <type_traits>

int main()
{
	const auto expression = cxxlens::select::expression_selector{}
								.type_is(cxxlens::select::type("int"))
								.implicit(cxxlens::select::implicit_node_policy::spelled_only);
	const auto reference = cxxlens::select::references_to(cxxlens::select::function("ns::run"));
	const auto erased_expression = cxxlens::select::semantic(expression);
	const auto erased_reference = cxxlens::select::semantic(reference);
	static_assert(std::is_move_constructible_v<cxxlens::select::semantic_selector>);
	return erased_expression.to_json().empty() || erased_reference.to_json().empty() ? 1 : 0;
}
