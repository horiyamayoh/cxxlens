#include <cstdlib>
#include <iostream>
#include <map>

#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Analysis/CFG.h>
#include <clang/Tooling/Tooling.h>

#include "project_object_facets.hpp"
namespace n = cxxlens::detail::clang22::object_semantics;
void require(bool value, const char* message)
{
	if (!value)
	{
		std::cerr << message << '\n';
		std::exit(1);
	}
}
struct collector : clang::RecursiveASTVisitor<collector>
{
	std::map<const clang::Decl*, std::string> declarations;
	std::map<const clang::Stmt*, std::string> expressions;
	std::map<const void*, std::string> types;
	std::vector<const clang::CastExpr*> casts;
	std::vector<const clang::CXXMemberCallExpr*> member_calls;
	std::vector<const clang::FunctionDecl*> functions;
	bool VisitDecl(clang::Decl* d)
	{
		declarations.emplace(d, "original-declaration-" + std::to_string(declarations.size()));
		return true;
	}
	bool VisitExpr(clang::Expr* e)
	{
		expressions.emplace(e, "original-expression-" + std::to_string(expressions.size()));
		types.emplace(e->getType().getCanonicalType().getAsOpaquePtr(),
					  "original-type-" + std::to_string(types.size()));
		return true;
	}
	bool VisitVarDecl(clang::VarDecl* d)
	{
		types.emplace(d->getType().getCanonicalType().getAsOpaquePtr(),
					  "original-type-" + std::to_string(types.size()));
		return true;
	}
	bool VisitCastExpr(clang::CastExpr* e)
	{
		casts.push_back(e);
		return true;
	}
	bool VisitCXXMemberCallExpr(clang::CXXMemberCallExpr* e)
	{
		member_calls.push_back(e);
		return true;
	}
	bool VisitFunctionDecl(clang::FunctionDecl* f)
	{
		if (f->doesThisDeclarationHaveABody())
			functions.push_back(f);
		return true;
	}
};
std::string_view text(const n::object_facet_update& row, std::string_view key)
{
	const auto at = row.fields.find(key);
	if (at == row.fields.end() || !at->second.value)
		return {};
	const auto* value = std::get_if<std::string>(&*at->second.value);
	return value ? *value : std::string_view{};
}
int main()
{
	auto ast = clang::tooling::buildASTFromCodeWithArgs(
		R"cpp(
 enum Range {first=1,last=3};
 Range enum_bad(){return static_cast<Range>(4);}
 bool bool_bad(){return __builtin_bit_cast(bool, static_cast<unsigned char>(2));}
 bool bool_conversion(){return static_cast<bool>(2);}
 int missing(){}
 struct Object { virtual void use(); ~Object(){} };
 void ended(){Object object;object.~Object();object.use();}
 void live(){Object object;object.use();}
 void opaque();
 void unknown(){Object object;opaque();object.use();}
 float typed(){int object=1;return *reinterpret_cast<float*>(&object);}
 )cpp",
		{"-std=c++23", "-nostdinc", "-nostdinc++", "-Wno-return-type"},
		"/project/primitives.cpp");
	require(static_cast<bool>(ast), "actual AST absent");
	collector c;
	c.TraverseDecl(ast->getASTContext().getTranslationUnitDecl());
	n::object_facet_bindings bindings;
	bindings.compile_unit = "original-unit";
	bindings.body = "original-body";
	bindings.declaration = [&](const clang::Decl& d) -> std::string_view
	{
		auto it = c.declarations.find(&d);
		return it == c.declarations.end() ? std::string_view{} : std::string_view{it->second};
	};
	bindings.entity = bindings.declaration;
	bindings.syntax = [&](const clang::Stmt& e) -> std::string_view
	{
		auto it = c.expressions.find(&e);
		return it == c.expressions.end() ? std::string_view{} : std::string_view{it->second};
	};
	bindings.canonical_type = [&](clang::QualType type) -> std::string_view
	{
		auto it = c.types.find(type.getCanonicalType().getAsOpaquePtr());
		return it == c.types.end() ? std::string_view{} : std::string_view{it->second};
	};
	bindings.declaration_source = [](const clang::FunctionDecl&) -> std::string_view
	{
		return "original-declaration-source";
	};
	bindings.cfg_node = [](unsigned) -> std::string_view
	{
		return "original-cfg-node";
	};
	bindings.cfg_edge = [](unsigned, unsigned) -> std::string_view
	{
		return "original-cfg-edge";
	};
	unsigned cases{};
	for (const auto* f : c.functions)
	{
		clang::CFG::BuildOptions options;
		options.AddImplicitDtors = true;
		options.AddLifetime = true;
		auto cfg = clang::CFG::buildCFG(f, f->getBody(), &ast->getASTContext(), options);
		require(static_cast<bool>(cfg), "original CFG missing");
		if (f->getNameAsString() == "missing")
		{
			auto native = n::observe_function_exits(*f, *cfg);
			auto out = n::detach_function_exit_facets(*f, native, bindings);
			require(out && out->updates.size() == 3U, "physical function facets absent");
			require(out->updates[0].scope_declaration == bindings.declaration(*f) &&
						text(out->updates[0], "return_kind") == "value" &&
						text(out->updates[2], "function_exit_kind") == "fallthrough",
					"original exact definition exit role differs");
			++cases;
			auto limits = n::object_facet_limits{};
			limits.maximum_operations = out->operations;
			limits.maximum_retained_bytes = out->retained_bytes_bound;
			require(static_cast<bool>(n::detach_function_exit_facets(*f, native, bindings, limits)),
					"measured native facets not reproducible");
			++cases;
			limits.maximum_retained_bytes = 1U;
			require(!n::detach_function_exit_facets(*f, native, bindings, limits),
					"native facet cap ignored");
			++cases;
		}
		for (const auto* cast : c.casts)
		{
			// Select the actual compiler ownership by traversal of this exact body.
			struct member : clang::RecursiveASTVisitor<member>
			{
				const clang::Stmt* target;
				bool found{};
				bool VisitStmt(clang::Stmt* s)
				{
					found |= s == target;
					return true;
				}
			};
			member owns;
			owns.target = cast;
			owns.TraverseStmt(f->getBody());
			if (!owns.found)
				continue;
			if (cast->getType()->isEnumeralType())
			{
				auto native = n::observe_enum_value(*cast, ast->getASTContext());
				auto out = n::detach_enum_value_facets(*cast, native, *f, bindings);
				require(out && out->updates.size() == 2U &&
							text(out->updates[0], "original_value_lower") == "4" &&
							text(out->updates[1], "enum_value_upper") == "3",
						"original enum facts lost");
				require(text(out->updates[0], "canonical_type") ==
							bindings.canonical_type(cast->getType()),
						"display spelling used as type identity");
				++cases;
			}
			if (cast->getType()->isBooleanType() &&
				(cast->getCastKind() == clang::CK_IntegralToBoolean ||
				 llvm::isa<clang::BuiltinBitCastExpr>(cast)))
			{
				auto native = n::observe_bool_representation(*cast, ast->getASTContext());
				auto out = n::detach_bool_value_facets(*cast, native, *f, bindings);
				require(out && out->updates.size() == 1U, "original Bool facets absent");
				if (llvm::isa<clang::BuiltinBitCastExpr>(cast))
					require(text(out->updates[0], "original_value_lower") == "2",
							"bit pattern lost");
				else
					require(text(out->updates[0], "object_fact_kind") ==
									"bool_integral_conversion" &&
								!out->updates[0].fields.contains("original_value_lower"),
							"conversion became invalid representation");
				++cases;
			}
		}
		for (const auto* call : c.member_calls)
		{
			const auto* receiver = call->getImplicitObjectArgument();
			const auto* ref = receiver
				? llvm::dyn_cast<clang::DeclRefExpr>(receiver->IgnoreParenImpCasts())
				: nullptr;
			const auto* object = ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
			if (!object || object->getDeclContext() != f ||
				llvm::isa<clang::CXXDestructorDecl>(call->getMethodDecl()))
				continue;
			auto native = n::observe_direct_lifetime_state(*f, *object, *call, *cfg);
			auto out =
				n::detach_direct_lifetime_facets(*call, *object, native, 0U, 0U, *f, bindings);
			require(out && out->updates.size() == 2U, "native lifetime/receiver facets missing");
			if (f->getNameAsString() == "ended")
				require(text(out->updates[0], "object_phase") == "ended" &&
							text(out->updates[0], "object_fact_state") == "complete",
						"actual destructor order not retained");
			if (f->getNameAsString() == "live")
				require(text(out->updates[0], "object_phase") == "live",
						"live original point lost");
			if (f->getNameAsString() == "unknown")
				require(text(out->updates[0], "object_fact_state") == "partial",
						"opaque action became lifetime proof");
			++cases;
		}
	}
	require(cases >= 9U, "actual fixture coverage missing");
	std::cout << cases << " actual original primitive facet cases PASS\n";
}
