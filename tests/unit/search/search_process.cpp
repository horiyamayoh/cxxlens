#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <cxxlens/explain.hpp>
#include <cxxlens/search.hpp>

#include "search_fixture.hpp"

namespace
{
	using namespace cxxlens;

	[[nodiscard]] select::call_selector flagship()
	{
		return select::calls_to_method("Base", "step")
			.include_derived_types()
			.include_virtual_overrides()
			.dispatch(select::dispatch_policy::static_and_virtual_candidates);
	}
} // namespace

int main(const int count, const char* const* arguments)
{
	try
	{
		const bool reverse = count > 1 && std::string_view{arguments[1]} == "reverse";
		const auto jobs = count > 2 ? static_cast<std::size_t>(std::stoul(arguments[2])) : 1U;
		const bool markdown = count > 3 && std::string_view{arguments[3]} == "markdown";
		execution_context context;
		context.parallelism = jobs;
		auto opened = cxxlens::test::search_fixture::open(false, true, reverse, context);
		if (!opened)
			throw std::runtime_error{opened.error().message};
		auto fixture = std::move(opened.value());
		auto report = search::calls(fixture.value, flagship());
		if (!report)
			throw std::runtime_error{report.error().message};
		const auto repeated = search::calls(fixture.value, flagship());
		if (!repeated || repeated.value().to_json() != report.value().to_json())
			throw std::runtime_error{"warm search projection changed"};
		auto why = explain::why_not_matched(fixture.value, select::semantic(flagship()));
		if (!why)
			throw std::runtime_error{why.error().message};
		const auto card = explain::for_selector(select::semantic(flagship()));
		if (markdown)
			std::cout << report.value().to_markdown() << "\n---\n"
					  << why.value().to_markdown() << "\n---\n"
					  << card.to_markdown();
		else
			std::cout << report.value().to_json() << '\n'
					  << why.value().to_json() << '\n'
					  << card.to_json() << '\n';
		return EXIT_SUCCESS;
	}
	catch (const std::exception& failure)
	{
		std::cerr << failure.what() << '\n';
		return EXIT_FAILURE;
	}
}
