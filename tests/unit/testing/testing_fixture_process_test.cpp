#include <iostream>
#include <string_view>

#include <cxxlens/testing.hpp>

int main(const int argc, const char* const* argv)
{
	if (argc != 3)
		return 2;
	const bool reverse = std::string_view{argv[2]} == "reverse";
	auto fixture =
		cxxlens::testing::workspace_fixture::cpp("#include \"a.hpp\"\nint main(){return f();}")
			.argument("-Wall");
	const cxxlens::testing::fixture_variant first{"a", {"-DA"}, {{"MODE", "a"}}};
	const cxxlens::testing::fixture_variant second{"z", {"-DZ"}, {{"MODE", "z"}}};
	if (reverse)
		fixture = fixture.add_file("z.cpp", "int z;")
					  .add_header("a.hpp", "int f();")
					  .add_variant(second)
					  .add_variant(first);
	else
		fixture = fixture.add_header("a.hpp", "int f();")
					  .add_file("z.cpp", "int z;")
					  .add_variant(first)
					  .add_variant(second);
	auto bundle = fixture.materialize(argv[1]);
	if (!bundle)
		return 1;
	std::cout << bundle.value().to_json() << '\n';
	return 0;
}
