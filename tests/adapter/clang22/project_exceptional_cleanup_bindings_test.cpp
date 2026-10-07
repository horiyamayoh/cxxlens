#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Type.h>

#include "llvm/clang22/project_exception_specification.hpp"
#include "llvm/clang22/project_exceptional_exits.hpp"

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
} // namespace
int main()
{
	provider::translation_unit_input input{"snapshot:cleanup-fixture",
										   "file:cleanup-fixture",
										   "cleanup.cpp",
										   R"CPP(
void risky(); void safe() noexcept;
void true_spec() noexcept(sizeof(int)>0);
void false_spec() noexcept(false);
template<class T> void dependent_spec() noexcept(T::value);
using Alias = void() noexcept; Alias alias_spec;
struct Guard { Guard(); ~Guard() noexcept(false); };
struct Safe { Safe(); ~Safe() noexcept; };
Guard make();
void local() { Guard first; risky(); }
void two() { Guard first; Guard second; make(); risky(); }
void normal_only() { Safe value; }
void no_throw_owner() noexcept { risky(); }
)CPP",
										   {"clang++",
											"-std=c++23",
											"-nostdinc",
											"-nostdinc++",
											"-fexceptions",
											"-fcxx-exceptions",
											"-O0",
											"cleanup.cpp"}};
	auto parsed = provider::with_translation_unit(
		input,
		[&](provider::borrowed_translation_unit& unit) -> sdk::result<void>
		{
			std::size_t specs{}, dependent_specs{};
			for (const auto* declaration : unit.ast().getTranslationUnitDecl()->decls())
			{
				const auto* function = llvm::dyn_cast<clang::FunctionDecl>(declaration);
				if (const auto* templated =
						llvm::dyn_cast<clang::FunctionTemplateDecl>(declaration))
					function = templated->getTemplatedDecl();
				if (!function)
					continue;
				const auto* type = function->getType()->getAs<clang::FunctionProtoType>();
				require(type != nullptr, "actual fixture prototype absent");
				const auto before = type->getExceptionSpecType();
				const auto observed = native::observe_function_exception_specification(*function);
				require(type->getExceptionSpecType() == before,
						"observer resolved or changed original exception specification");
				const auto name = function->getNameAsString();
				if (name == "dependent_spec")
				{
					require(observed.kind == "dependent_noexcept" && observed.state == "partial" &&
								!observed.nonthrowing,
							"dependent specification was evaluated or guessed");
					++dependent_specs;
				}
				if (name == "risky" || name == "false_spec")
				{
					require(observed.state == "complete" && observed.nonthrowing == false,
							"potentially throwing specification changed");
					++specs;
				}
				if (name == "safe" || name == "true_spec" || name == "alias_spec")
				{
					require(observed.state == "complete" && observed.nonthrowing == true,
							"actual nonthrowing specification or alias lost");
					++specs;
				}
			}
			require(specs == 5U && dependent_specs == 1U,
					"independent actual specification fixture coverage");
			std::map<const clang::Decl*, std::string> original_declarations;
			native::exceptional_compiler_bindings bindings;
			bindings.declaration = [&](const clang::Decl* declaration) -> std::string_view
			{
				const auto* variable = llvm::dyn_cast<clang::VarDecl>(declaration);
				const auto* function = llvm::dyn_cast<clang::FunctionDecl>(declaration);
				require((variable && variable->hasLocalStorage()) || function,
						"binding must retain an actual cleanup variable or function boundary");
				auto [entry, inserted] = original_declarations.try_emplace(declaration);
				if (inserted)
					entry->second =
						"actual-declaration-" + std::to_string(original_declarations.size());
				return entry->second;
			};
			const auto observed = native::observe_project_exceptional_exits(unit, {}, bindings);
			if (!observed)
				return sdk::unexpected(observed.error());
			std::size_t normal{}, exceptional{}, unbound_temporaries{}, runtime_helpers{};
			std::set<std::string> objects;
			for (const auto& scope : observed->scopes)
				for (const auto& variant : scope.variants)
					for (const auto& occurrence : variant.occurrences)
					{
						if (occurrence.cleanup_profile.empty())
							continue;
						require(occurrence.cleanup_profile ==
										"clang22-destroy-object-cleanup-emission/1" &&
									occurrence.cleanup_emission_ordinal,
								"original cleanup emission metadata missing");
						if (occurrence.cleanup_declaration.empty())
						{
							require(!occurrence.cleanup_registration_ordinal,
									"temporary inherited an unrelated automatic registration");
							++unbound_temporaries;
							continue;
						}
						require(occurrence.cleanup_registration_ordinal.has_value(),
								"actual automatic cleanup lost registration");
						if (!occurrence.emitter_methods || (*occurrence.emitter_methods & 1U) == 0U)
						{
							require(occurrence.cleanup_target_usr.empty() &&
										!occurrence.cleanup_target_dtor_type,
									"runtime helper inherited the destructor-emitter target");
							++runtime_helpers;
							continue;
						}
						require(!occurrence.cleanup_target_usr.empty() &&
									occurrence.cleanup_target_dtor_type &&
									occurrence.cleanup_target_profile ==
										"clang22-destructor-emission-target/1",
								"actual automatic cleanup lost registration or selected target");
						objects.insert(occurrence.cleanup_declaration);
						normal += occurrence.cleanup_route == "normal" ? 1U : 0U;
						exceptional += occurrence.cleanup_route == "exceptional" ? 1U : 0U;
					}
			require(normal >= 4U && exceptional >= 3U && objects.size() == 4U,
					"independent automatic objects and normal/EH routes not retained");
			require(unbound_temporaries != 0U,
					"actual temporary cleanup failed to retain its independent unbound origin");
			std::cout << "actual exception specifications=" << specs << " normal=" << normal
					  << " exceptional=" << exceptional << " objects=" << objects.size()
					  << " unbound temporaries=" << unbound_temporaries
					  << " runtime helpers=" << runtime_helpers << '\n';
			return {};
		});
	if (!parsed)
	{
		std::cerr << parsed.error().code << ':' << parsed.error().field << ':'
				  << parsed.error().detail << '\n';
		return 1;
	}

	provider::translation_unit_input coroutine_input{"snapshot:coroutine-wrapper",
													 "file:coroutine-wrapper",
													 "coroutine.cpp",
													 R"CPP(
namespace std {
template<class Promise = void> struct coroutine_handle {
 static coroutine_handle from_address(void*) noexcept { return {}; }
 operator coroutine_handle<void>() const noexcept { return {}; }
};
template<class Return, class... Args> struct coroutine_traits {
 using promise_type = typename Return::promise_type;
};
}
struct Awaiter {
 bool await_ready() noexcept { return false; }
 void await_suspend(std::coroutine_handle<>) noexcept {}
 void await_resume() noexcept {}
};
struct Task { struct promise_type {
 Task get_return_object() { return {}; }
 Awaiter initial_suspend() { return {}; }
 Awaiter final_suspend() noexcept { return {}; }
 void return_void() noexcept {}
 void unhandled_exception() noexcept {}
}; };
Task coroutine_probe() { co_await Awaiter{}; co_return; }
)CPP",
													 {"clang++",
													  "-std=c++23",
													  "-nostdinc",
													  "-nostdinc++",
													  "-fexceptions",
													  "-fcxx-exceptions",
													  "-O0",
													  "coroutine.cpp"}};
	auto coroutine_parsed = provider::with_translation_unit(
		coroutine_input,
		[&](provider::borrowed_translation_unit& unit) -> sdk::result<void>
		{
			const clang::FunctionDecl* coroutine{};
			for (const auto* declaration : unit.ast().getTranslationUnitDecl()->decls())
				if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(declaration);
					function && function->getNameAsString() == "coroutine_probe")
					coroutine = function;
			require(coroutine != nullptr, "actual coroutine declaration absent");
			native::exceptional_compiler_bindings bindings;
			bindings.scope = [&](const clang::FunctionDecl* declaration)
			{
				return declaration == coroutine
					? native::exceptional_scope_binding{"detail:coroutine",
														"function:coroutine",
														"source:coroutine",
														"body:coroutine"}
					: native::exceptional_scope_binding{};
			};
			const auto observed = native::observe_project_exceptional_exits(unit, {}, bindings);
			if (!observed)
				return sdk::unexpected(observed.error());
			std::size_t bound_scopes{};
			for (const auto& scope : observed->scopes)
				if (scope.detail == "detail:coroutine")
				{
					++bound_scopes;
					require(!scope.variants.empty() && !scope.activation_complete &&
								!scope.complete,
							"coroutine helpers borrowed owner or closed unsupported activation");
				}
			require(bound_scopes == 1U, "unowned helper acquired the written coroutine scope");
			return {};
		});
	if (!coroutine_parsed)
	{
		std::cerr << coroutine_parsed.error().code << ':' << coroutine_parsed.error().field << ':'
				  << coroutine_parsed.error().detail << '\n';
		return 1;
	}
}
