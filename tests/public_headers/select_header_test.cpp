#include <string>
#include <type_traits>

#include <cxxlens/select.hpp>

int main()
{
	static_assert(std::is_copy_constructible_v<cxxlens::select::semantic_selector>);
	const auto selector = cxxlens::select::calls_to_method("Base", "start")
							  .include_derived_types()
							  .include_virtual_overrides();
	return selector.to_json().find("cxxlens.selector.v1") == std::string::npos ? 1 : 0;
}
