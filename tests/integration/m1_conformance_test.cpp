#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/cxxlens.hpp>
#include <cxxlens/interop/clang.hpp>

namespace
{
	using namespace cxxlens;

	[[noreturn]] void fail(const std::string& message)
	{
		throw std::runtime_error{message};
	}

	void require(const bool condition, const std::string& message)
	{
		if (!condition)
			fail(message);
	}

	[[nodiscard]] testing::workspace_fixture fixture(const bool reverse)
	{
		constexpr std::string_view header = R"cpp(#pragma once
#define WRAP(value) (value)
struct Base { virtual int run(); };
struct Derived final : Base { int run() override; };
int left();
int right();
int helper(int value);
)cpp";
		constexpr std::string_view main_source = R"cpp(#include "api.hpp"
int Base::run() { return 0; }
int Derived::run() { return MODE; }
int selected() {
#if MODE == 1
  return left();
#else
  return right();
#endif
}
int invoke(Base& base, Derived& derived) {
  return WRAP(base.run()) + derived.run() + helper(selected());
}
)cpp";
		constexpr std::string_view support_source = R"cpp(#include "api.hpp"
int left() { return 1; }
int right() { return 2; }
int helper(int value) { return value; }
)cpp";

		auto value = testing::workspace_fixture::cpp(std::string{main_source});
		if (reverse)
			value = value.add_file("support.cpp", std::string{support_source})
						.add_header("api.hpp", std::string{header})
						.add_variant({"mode-two", {"-DMODE=2"}, {}})
						.add_variant({"mode-one", {"-DMODE=1"}, {}});
		else
			value = value.add_header("api.hpp", std::string{header})
						.add_file("support.cpp", std::string{support_source})
						.add_variant({"mode-one", {"-DMODE=1"}, {}})
						.add_variant({"mode-two", {"-DMODE=2"}, {}});
		return value;
	}

	[[nodiscard]] std::string execute(const std::size_t jobs, const bool reverse)
	{
		execution_context context;
		context.parallelism = jobs;
		auto opened = fixture(reverse).open(context);
		require(opened.has_value(), "production-path memory fixture did not open");
		auto workspace = std::move(opened.value());
		require(workspace.compile_units().size() == 4U,
				"multi-TU x multi-variant compile-unit universe was not preserved");
		require(workspace.capabilities().has("frontend.clang22"),
				"M1 conformance requires the exact Clang 22 frontend capability");
		const auto linked = interop::linked_clang_version();
		require(linked.llvm_major == 22U, "M1 acceptance is not using the linked Clang 22 adapter");
		bool callback_observed{};
		auto borrowed = interop::with_translation_unit(
			workspace,
			workspace.compile_units().front().id(),
			[&callback_observed](interop::borrowed_clang_tu& value) -> result<void>
			{
				callback_observed = !value.unit().id().empty();
				return {};
			},
			context);
		require(borrowed && callback_observed,
				"M1 public borrowed translation-unit corridor was not executed");

		const auto profile = fact_profile::semantic_search().include(fact_kind::macro_definition);
		auto ensured = workspace.ensure(profile, analysis_scope::all(), context);
		require(ensured.has_value(), "real frontend to reducer/store provisioning failed");
		const auto store = workspace.facts();
		const auto coverage = store.coverage(profile);
		require(coverage.validate() && coverage.complete() &&
					coverage.requested().size() == workspace.compile_units().size() * 13U,
				"M1 fact coverage universe is incomplete or unbalanced");

		auto all = store.find(fact_query::all());
		auto symbols = store.symbols();
		auto calls = store.calls();
		auto inheritance = store.inheritance();
		auto overrides = store.overrides();
		auto includes = store.includes();
		auto macros = store.macros();
		require(all && symbols && calls && inheritance && overrides && includes && macros,
				"one or more installed M1 fact queries failed");
		require(!symbols.value().empty() && !calls.value().empty() &&
					!inheritance.value().empty() && !overrides.value().empty() &&
					!includes.value().empty() && !macros.value().empty(),
				"real extractor silently omitted an M1 semantic fact family");

		const auto derived = std::ranges::find_if(symbols.value(),
												  [](const symbol& value)
												  {
													  return value.name() == "Derived";
												  });
		auto declarations = store.find(fact_query::all().kind(fact_kind::declaration));
		require(derived != symbols.value().end() && declarations && !declarations.value().empty(),
				"detached symbol/declaration facts are missing");
		bool reference_query_succeeded{};
		for (const auto& value : symbols.value())
		{
			auto references = store.references(value.id());
			if (references && !references.value().empty())
			{
				reference_query_succeeded = true;
				break;
			}
		}
		require(reference_query_succeeded,
				"detached reference query did not retain any semantic target");

		const auto macro = std::ranges::find_if(macros.value(),
												[](const macro_expansion& value)
												{
													return value.name == "WRAP";
												});
		require(macro != macros.value().end() && macro->expansion.spelling.has_value() &&
					macro->expansion.expansion.has_value() &&
					!macro->expansion.macro_stack.empty() &&
					macro->expansion.origin != source_origin::directly_spelled,
				"macro spelling/expansion/origin/provenance did not survive reduction and query");

		std::vector<build_variant_id> variants;
		for (const auto& unit : workspace.command_for("main.cpp"))
			if (!std::ranges::contains(variants, unit.variant_id()))
				variants.push_back(unit.variant_id());
		std::ranges::sort(variants,
						  {},
						  [](const build_variant_id& value)
						  {
							  return value.value();
						  });
		require(variants.size() == 2U, "variant identity collapsed distinct commands");
		std::vector<std::string> variant_calls;
		for (const auto& variant : variants)
		{
			auto selected = store.find(fact_query::all().kind(fact_kind::call).variant(variant));
			require(selected && !selected.value().empty(),
					"variant-specific call facts are missing");
			variant_calls.push_back(
				store.to_json(fact_query::all().kind(fact_kind::call).variant(variant)));
		}
		require(variant_calls.front() != variant_calls.back(),
				"divergent conditional commands were merged into one variant result");

		const auto before = store.to_json();
		require(workspace.ensure(profile, analysis_scope::all(), context).has_value() &&
					workspace.facts().to_json() == before,
				"warm production-path ensure changed the immutable fact snapshot");
		auto doctor = workspace.doctor();
		require(doctor && doctor.value().healthy() && doctor.value().diagnostics().empty(),
				"healthy M1 production workspace doctor is not stable and explicit");

		return workspace.facts().to_json() + "\n" + workspace.facts().coverage(profile).to_json() +
			"\n" + doctor.value().to_json() + "\n";
	}
} // namespace

int main(const int argument_count, const char* const* arguments)
{
	try
	{
		std::size_t jobs = 1U;
		bool reverse{};
		bool emit{};
		if (argument_count >= 2)
			emit = std::string_view{arguments[1]} == "--emit";
		if (argument_count >= 3)
			jobs = static_cast<std::size_t>(std::stoul(arguments[2]));
		if (argument_count >= 4)
			reverse = std::string_view{arguments[3]} == "reverse";
		const auto output = execute(jobs, reverse);
		if (emit)
			std::cout << output;
		return EXIT_SUCCESS;
	}
	catch (const std::exception& failure)
	{
		std::cerr << failure.what() << '\n';
		return EXIT_FAILURE;
	}
}
