#include <string>
#include <type_traits>

#include <cxxlens/workspace.hpp>

int main()
{
	static_assert(std::is_copy_constructible_v<cxxlens::analysis_scope>);
	const auto scope = cxxlens::analysis_scope::files({"src/a.cpp"}).include_headers();
	return scope.to_json().find("cxxlens.analysis-scope.v1") == std::string::npos ? 1 : 0;
}
