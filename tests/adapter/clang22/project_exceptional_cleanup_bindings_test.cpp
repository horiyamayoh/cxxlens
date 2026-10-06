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
				require(variable && variable->hasLocalStorage(),
						"cleanup observer invented an automatic declaration");
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
}
