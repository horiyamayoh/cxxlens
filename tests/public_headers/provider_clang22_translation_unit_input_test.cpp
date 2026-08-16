#include <string>
#include <utility>
#include <vector>

#include <cxxlens/provider/clang22.hpp>

namespace
{
	[[nodiscard]] cxxlens::provider::clang22::translation_unit_input input(std::string logical_path)
	{
		return {
			"source-snapshot:translation-unit-input",
			"file:translation-unit-input",
			std::move(logical_path),
			"int main() { return 0; }",
			{"clang++", "-std=c++23"},
		};
	}

	[[nodiscard]] bool rejects_logical_path(std::string logical_path)
	{
		const auto result = input(std::move(logical_path)).validate();
		return !result && result.error().code == "native.input-invalid" &&
			result.error().field == "logical_path";
	}
} // namespace

int main()
{
	if (!input("project://src/foo..bar.hpp").validate())
		return 1;
	if (!rejects_logical_path("project://src/../main.cpp"))
		return 2;
	if (!rejects_logical_path("project://src/..\\main.cpp"))
		return 3;
	if (!rejects_logical_path("/absolute/main.cpp"))
		return 4;
	if (!rejects_logical_path("\\absolute\\main.cpp"))
		return 5;
	std::string embedded_nul = "project://src/bad";
	embedded_nul.push_back('\0');
	embedded_nul += "path.cpp";
	if (!rejects_logical_path(std::move(embedded_nul)))
		return 6;
	return 0;
}
