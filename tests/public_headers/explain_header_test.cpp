#include <string>

#include <cxxlens/explain.hpp>

int main()
{
	const auto explanation = cxxlens::explain::selector(
		cxxlens::select::semantic(cxxlens::select::calls_to_function("run")));
	return explanation.to_json().find("cxxlens.explanation.v1") == std::string::npos ? 1 : 0;
}
