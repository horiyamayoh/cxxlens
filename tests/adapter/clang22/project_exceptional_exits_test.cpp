#include "llvm/clang22/project_exceptional_exits.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string_view>
#include <tuple>

namespace native = cxxlens::detail::clang22;
namespace provider = cxxlens::provider::clang22;
namespace sdk = cxxlens::sdk;
namespace
{
	void require(bool value, std::string_view message)
	{
		if (!value)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}
	const native::exceptional_physical_scope&
	scope(const native::project_exceptional_exit_observations& result, std::string_view name)
	{
		const auto found =
			std::ranges::find_if(result.scopes,
								 [&](const auto& value)
								 {
									 return value.owner_usr.find(name) != std::string::npos;
								 });
		require(found != result.scopes.end(), name);
		return *found;
	}
	std::size_t count(const native::exceptional_physical_scope& scope, std::string_view role)
	{
		std::size_t total{};
		for (const auto& variant : scope.variants)
			for (const auto& occurrence : variant.occurrences)
				total += occurrence.role == role && occurrence.eligibility == "eligible" ? 1U : 0U;
		return total;
	}
} // namespace
int main()
{
	provider::translation_unit_input input{"snapshot:eh-fixture",
										   "file:eh-fixture",
										   "eh.cpp",
										   R"CPP(
 void risky(); void safe() noexcept;
 inline void not_odr_used() { risky(); }
 struct Written { Written() { risky(); } ~Written() { safe(); } };
 struct Guard { Guard(); ~Guard() noexcept(false); };
 struct Copy { Copy(const Copy&); ~Copy() noexcept; };
 void empty() {}
 void escaping_call() { risky(); }
 void safe_call() { safe(); }
 void guarded_call() { Guard guard; risky(); }
 void closed_catch() { try { risky(); } catch (...) {} }
 void typed_catch() { try { risky(); } catch (int) {} }
 void explicit_throw() { throw 1; }
 void caught_throw() { try { throw 1; } catch (...) {} }
 void throwing_copy(Copy& value) { throw value; }
 void builtin_stop() { __builtin_trap(); }
 int builtin_absolute(int value) { return __builtin_abs(value); }
 void pseudo_cleanup(int* value) { using I = int; value->~I(); }
 void nonthrowing_owner() noexcept { risky(); }
 extern "C" void user_termination() noexcept __attribute__((noreturn));
 void caught_then_user_termination() noexcept {
   try { risky(); } catch (...) { user_termination(); }
 }
 void nonthrowing_guarded() noexcept { Guard guard; risky(); }
 Guard* allocation() { return new Guard; }
 void deallocation(Guard* pointer) { delete pointer; }
 void default_target(int value = (risky(), 0));
 void activated_default() { default_target(); }
 void enclosing() { auto body = [] { throw 2; }; body(); }
 template<class T> void dependent() { T::call(); }
 )CPP",
										   {"clang++",
											"-std=c++23",
											"-nostdinc",
											"-nostdinc++",
											"-fexceptions",
											"-fcxx-exceptions",
											"-O0",
											"eh.cpp"}};
	auto parsed = provider::with_translation_unit(
		input,
		[&](provider::borrowed_translation_unit& unit) -> sdk::result<void>
		{
			const auto result = native::observe_project_exceptional_exits(unit);
			if (!result)
			{
				std::cerr << result.error().code << ':' << result.error().field << ':'
						  << result.error().detail << '\n';
				return sdk::unexpected(result.error());
			}
			require(result->frozen && result->hooks_installed,
					"actual recorder profile is installed and frozen");
			for (const auto& physical : result->scopes)
				for (const auto& variant : physical.variants)
					if (variant.topology_complete)
					{
						require(variant.topology_observed && !variant.blocks.empty(),
								"complete topology has an independently observed block census");
						std::set<std::uint64_t> blocks;
						std::size_t entries{};
						for (const auto& block : variant.blocks)
						{
							require(blocks.insert(block.ordinal).second &&
										block.instruction_count != 0U && block.terminator_opcode,
									"actual block identity and terminator geometry are retained");
							entries += block.is_entry ? 1U : 0U;
						}
						require(entries == 1U, "the original LLVM entry is unique");
						std::set<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>> edges;
						for (const auto& edge : variant.successors)
						{
							const auto from =
								std::ranges::find(variant.blocks,
												  edge.from,
												  &native::exceptional_original_block::ordinal);
							require(from != variant.blocks.end() && blocks.contains(edge.to) &&
										edge.terminator_instruction_ordinal + 1U ==
											from->instruction_count &&
										edges
											.emplace(edge.from,
													 edge.terminator_instruction_ordinal,
													 edge.ordinal)
											.second,
									"successor occurrences retain exact block/terminator/index "
									"geometry");
							if (edge.invoke)
								require((edge.ordinal == 0U && edge.kind == "normal") ||
											(edge.ordinal == 1U && edge.kind == "unwind"),
										"Invoke successor indices are the actual normal/unwind "
										"operands");
						}
					}
			for (const auto& value : result->scopes)
			{
				std::cout << value.owner_usr << " complete=" << value.complete
						  << " admitted=" << value.admitted << " variants=" << value.variants.size()
						  << " reason=" << value.reason << '\n';
				for (const auto& variant : value.variants)
					for (const auto& occurrence : variant.occurrences)
						std::cout << "  " << variant.kind << ':' << variant.index << ' '
								  << occurrence.role << ' ' << occurrence.eligibility << ' '
								  << occurrence.expression_kind << ' ' << occurrence.target_usr
								  << " intrinsic=" << occurrence.intrinsic_id.value_or(0U)
								  << " nounwind=" << occurrence.does_not_throw.value_or(false)
								  << " noreturn=" << occurrence.does_not_return.value_or(false)
								  << " invoke=" << occurrence.is_invoke.value_or(false)
								  << " boundary=" << occurrence.eh_selected_scope_kind
								  << " disposition=" << occurrence.eh_disposition << '\n';
			}
			const auto& empty = scope(*result, "@F@empty#");
			require(
				empty.complete && empty.variants.size() == 1U &&
					empty.variants.front().occurrences.empty(),
				"independently admitted empty body is a known empty original lowering population");
			require(scope(*result, "@F@not_odr_used#").complete,
					"written unused inline definition must be lowered independently");
			require(count(scope(*result, "@F@escaping_call#"), "escaping_call") == 1U,
					"direct escaping call retained");
			require(count(scope(*result, "@F@safe_call#"), "escaping_call") == 0U,
					"actual nonthrowing call excluded");
			require(count(scope(*result, "@F@guarded_call#"), "unhandled_resume") > 0U,
					"cleanup unwinds retain original unhandled continuation");
			require(count(scope(*result, "@F@typed_catch#"), "unhandled_resume") > 0U,
					"unmatched typed catch retains original unhandled continuation");
			require(count(scope(*result, "@F@closed_catch#"), "unhandled_resume") == 0U,
					"catch all excludes unhandled continuation");
			require(count(scope(*result, "@F@explicit_throw#"), "written_throw") == 1U,
					"written throw independently admitted");
			require(count(scope(*result, "@F@caught_throw#"), "written_throw") == 1U,
					"caught written throw remains an original throw occurrence");
			require(count(scope(*result, "@F@explicit_throw#"), "escaping_call") == 0U,
					"original throw runtime helper does not duplicate the written throw");
			const auto& copy = scope(*result, "@F@throwing_copy#");
			require(copy.complete && count(copy, "written_throw") == 1U,
					"throw operand copy lowering population closes independently");
			bool original_copy{};
			for (const auto& variant : copy.variants)
				for (const auto& occurrence : variant.occurrences)
					if (!occurrence.target_usr.empty() &&
						occurrence.expression_kind == "CXXConstructExpr")
					{
						require(occurrence.emitter_methods && (*occurrence.emitter_methods & 1U),
								"selected throw operand copy retains its actual generic emitter "
								"method");
						original_copy = true;
						require(occurrence.role != "lowering_helper",
								"actual copy constructor is not the compiler throw helper");
					}
			require(
				original_copy && count(copy, "unhandled_resume") > 0U,
				"throwing copy retains original selected target and its independent unwind exit");
			require(count(scope(*result, "@F@nonthrowing_owner#"), "termination") > 0U,
					"actual noexcept termination carrier retained");
			auto disposition = [&](std::string_view function, std::string_view expected)
			{
				for (const auto& variant : scope(*result, function).variants)
					for (const auto& occurrence : variant.occurrences)
						if (occurrence.is_invoke.value_or(false) &&
							occurrence.eh_boundary_observed &&
							occurrence.eh_disposition == expected)
							return true;
				return false;
			};
			require(disposition("@F@nonthrowing_owner#", "direct_function_spec_termination"),
					"the exact Invoke selects its live original function-spec terminate scope");
			require(disposition("@F@closed_catch#", "catch_dispatch") &&
						!disposition("@F@closed_catch#", "direct_function_spec_termination"),
					"catch selection does not become a function-spec escape");
			require(
				disposition("@F@caught_then_user_termination#", "catch_dispatch") &&
					!disposition("@F@caught_then_user_termination#",
								 "direct_function_spec_termination"),
				"a caught exception followed by unrelated user termination is not a direct escape");
			require(disposition("@F@guarded_call#", "cleanup_dispatch") &&
						!disposition("@F@guarded_call#", "direct_function_spec_termination"),
					"cleanup interception remains distinct from direct function-spec termination");
			// The actual LLVM22 EHScopeStack suppresses EH cleanups under a
			// terminate scope; a normal cleanup registration is not an unwind route.
			require(disposition("@F@nonthrowing_guarded#", "direct_function_spec_termination") &&
						!disposition("@F@nonthrowing_guarded#", "cleanup_dispatch"),
					"suppressed EH cleanup does not invent an interception route");
			require(scope(*result, "@F@builtin_stop#").complete,
					"builtin bypasses close only with actual emitter membership");
			require(!scope(*result, "@F@activated_default#").complete,
					"unrepresented actual default activation remains partial");
			require(!scope(*result, "@FT@>1#Tdependent#").complete,
					"dependent body does not become a known zero");
			const auto& enclosing = scope(*result, "@F@enclosing#");
			require(
				enclosing.complete,
				"original nonthrowing memory intrinsics preserve exceptional membership closure");
			require(count(enclosing, "written_throw") == 0U,
					"nested lambda throw is outside enclosing physical body");
			bool lambda_throw{};
			for (const auto& value : result->scopes)
				lambda_throw |= value.owner_usr.find("@F@operator()") != std::string::npos &&
					count(value, "written_throw") == 1U;
			require(lambda_throw, "actual written lambda has its own admitted lowering scope");
			native::exceptional_exit_limits limits;
			limits.maximum_operations = 1U;
			const auto bounded = native::observe_project_exceptional_exits(unit, limits);
			require(!bounded && bounded.error().code == "native.exceptional-exit-budget",
					"original admission is bounded before lowering");
			limits = {};
			limits.cancelled = []
			{
				return true;
			};
			const auto cancelled = native::observe_project_exceptional_exits(unit, limits);
			require(!cancelled && cancelled.error().code == "native.exceptional-exit-cancelled",
					"active cancellation is observed before lowering");
			std::cout << "original exceptional scope/call/throw/unwind/termination/bounds PASS "
					  << result->scopes.size() << " scopes\n";
			return {};
		});
	if (!parsed)
	{
		std::cerr << parsed.error().code << ':' << parsed.error().field << ':'
				  << parsed.error().detail << '\n';
		return 1;
	}
}
