#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/cxxlens.hpp>

namespace
{
	using namespace cxxlens;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
			throw std::runtime_error{std::string{message}};
	}

	[[nodiscard]] bool contains_code(const std::span<const unresolved> values,
									 const std::string_view code)
	{
		return std::ranges::any_of(values,
								   [code](const unresolved& value)
								   {
									   return value.stable_code == code;
								   });
	}

	[[nodiscard]] symbol_id symbol_named(const fact_store& store, const std::string_view name)
	{
		auto symbols = store.symbols();
		if (!symbols)
			throw std::runtime_error{symbols.error().message};
		const auto found = std::ranges::find(symbols.value(), name, &symbol::qualified_name);
		if (found == symbols.value().end())
			throw std::runtime_error{"expected semantic symbol is missing"};
		return found->id();
	}

	[[nodiscard]] select::call_selector flagship(const select::macro_match_policy macro_policy)
	{
		return select::calls_to_method("Base", "step")
			.include_derived_types()
			.include_virtual_overrides()
			.dispatch(select::dispatch_policy::static_and_virtual_candidates)
			.macro(macro_policy);
	}

	[[nodiscard]] std::string execute(const path& root,
									  const std::string_view backend,
									  const std::size_t jobs,
									  const bool partial,
									  const bool markdown)
	{
		workspace_options options;
		options.project_root = root;
		options.compilation_database = root / "compile_commands.json";
		if (backend == "sqlite")
			options.cache_directory = root / ".cxxlens-cache";
		else
			require(backend == "memory", "backend must be memory or sqlite");
		execution_context context;
		context.parallelism = jobs;
		auto opened = workspace::open(std::move(options), context);
		if (!opened)
			throw std::runtime_error{opened.error().explain()};
		auto workspace = std::move(opened.value());
		require(workspace.compile_units().size() == (partial ? 6U : 4U),
				"compile-unit universe lost a TU or variant");

		auto direct = search::calls(workspace, flagship(select::macro_match_policy::exclude));
		if (!direct)
			throw std::runtime_error{direct.error().explain()};
		require(direct.value().matches().size() == 2U,
				"flagship search did not return exactly two direct call sites");
		const auto store = workspace.facts();
		const auto base_record = symbol_named(store, "Base");
		const auto base_step = symbol_named(store, "Base::step");
		const auto derived_step = symbol_named(store, "Derived::step");
		const auto other_step = symbol_named(store, "Other::step");
		const auto base_call = std::ranges::find_if(direct.value().matches(),
													[&](const call_site& call)
													{
														return call.direct_callee() == base_step;
													});
		require(base_call != direct.value().matches().end() &&
					std::ranges::contains(base_call->possible_callees(), derived_step) &&
					!std::ranges::contains(base_call->possible_callees(), other_step),
				"static and possible virtual targets are not semantically separated");
		require(base_call->receiver_static_type().has_value() &&
					base_call->receiver_static_type()->declaration() == base_record &&
					!base_call->why().items().empty(),
				"receiver type or why-matched evidence is missing");
		require(direct.value().guarantee() == result_guarantee::best_effort &&
					contains_code(direct.value().unresolved_items(),
								  "search.open-world-virtual-target"),
				"open-world virtual dispatch was presented as exact");
		require(direct.value().coverage().complete() != partial,
				"partial/complete coverage classification is incorrect");
		if (partial)
			require(contains_code(direct.value().unresolved_items(),
								  "search.override-coverage-incomplete"),
					"one-TU failure did not preserve an explicit partial result");

		auto with_macro =
			search::calls(workspace, flagship(select::macro_match_policy::include_with_origin));
		if (!with_macro)
			throw std::runtime_error{with_macro.error().explain()};
		require(with_macro.value().matches().size() == 3U,
				"macro-inclusive flagship search did not return the expansion call");
		const auto macro_call = std::ranges::find_if(with_macro.value().matches(),
													 [](const call_site& call)
													 {
														 return call.location().origin !=
															 source_origin::directly_spelled;
													 });
		require(macro_call != with_macro.value().matches().end() &&
					macro_call->location().spelling.has_value() &&
					macro_call->location().expansion.has_value() &&
					!macro_call->location().macro_stack.empty(),
				"macro primary/spelling/expansion provenance is incomplete");

		std::vector<build_variant_id> variants;
		for (const auto& unit : workspace.compile_units())
			if (!std::ranges::contains(variants, unit.variant_id()))
				variants.push_back(unit.variant_id());
		require(variants.size() == 2U, "build variants collapsed");
		const auto direct_json = direct.value().to_json();
		for (const auto& variant : variants)
			require(direct_json.find(variant.value()) != std::string::npos,
					"per-variant provenance is missing from the report");

		auto why = explain::why_not_matched(
			workspace, select::semantic(flagship(select::macro_match_policy::include_with_origin)));
		if (!why)
			throw std::runtime_error{why.error().explain()};
		require(why.value().properties.at("matched") == "3" &&
					why.value().properties.at("rejected") == "1" &&
					why.value().to_json().find("select.type.canonical") != std::string::npos,
				"unrelated same-name why-not accounting is incorrect");

		const auto before_warm = workspace.facts().to_json();
		auto warm = search::calls(workspace, flagship(select::macro_match_policy::exclude));
		require(warm && warm.value().to_json() == direct_json &&
					workspace.facts().to_json() == before_warm,
				"warm query changed facts or canonical search output");
		const auto selector_explanation =
			explain::selector(select::semantic(flagship(select::macro_match_policy::exclude)),
							  explain::detail_level::agent);
		const auto card =
			explain::for_selector(select::semantic(flagship(select::macro_match_policy::exclude)));
		if (markdown)
			return direct.value().to_markdown() + "\n---\n" + why.value().to_markdown() +
				"\n---\n" + card.to_markdown();
		return direct_json + "\n" + with_macro.value().to_json() + "\n" + why.value().to_json() +
			"\n" + selector_explanation.to_json() + "\n" + card.to_json() + "\n";
	}
} // namespace

int main(const int count, const char* const* arguments)
{
	try
	{
		if (count != 6)
			throw std::runtime_error{
				"usage: cxxlens-m2-flagship ROOT BACKEND JOBS normal|partial json|markdown"};
		const auto output = execute(arguments[1],
									arguments[2],
									static_cast<std::size_t>(std::stoul(arguments[3])),
									std::string_view{arguments[4]} == "partial",
									std::string_view{arguments[5]} == "markdown");
		std::cout << output;
		return EXIT_SUCCESS;
	}
	catch (const std::exception& failure)
	{
		std::cerr << failure.what() << '\n';
		return EXIT_FAILURE;
	}
}
