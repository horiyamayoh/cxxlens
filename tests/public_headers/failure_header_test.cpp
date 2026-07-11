#include <string>

#include <cxxlens/core/failure.hpp>

auto main() -> int
{
	cxxlens::error failure;
	failure.code.value = "core.cancelled";
	failure.message = "cancelled";
	cxxlens::result<std::string> value{failure};
	return !value && value.error().code.value == "core.cancelled" ? 0 : 1;
}
