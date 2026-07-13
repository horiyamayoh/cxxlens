#include "testing_plan_assertions.hpp"

int main()
{
	const auto edits = cxxlens::testing::edit_plan_assertion{}
					   .valid()
					   .changes_file("src/example.cpp")
					   .reparses();
	const auto generated = cxxlens::testing::generation_plan_assertion{}
						   .valid()
						   .census_complete()
						   .no_unknown_decisions();
	(void)edits;
	(void)generated;
	return 0;
}
