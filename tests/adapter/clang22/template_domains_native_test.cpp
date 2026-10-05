#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>

#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Sema/SemaConsumer.h>
#include <clang/Tooling/Tooling.h>

#include "llvm/clang22/project_template_observer.hpp"
using namespace cxxlens::detail::clang22;
std::set<std::pair<std::uint32_t, std::uint32_t>> expected_compounds;
struct original_type_census : clang::RecursiveASTVisitor<original_type_census>
{
	bool VisitTypeLoc(clang::TypeLoc value)
	{
		if (!value.isNull() && value.getType()->isDependentType() &&
			(value.getTypeLocClass() == clang::TypeLoc::Pointer ||
			 value.getTypeLocClass() == clang::TypeLoc::TemplateSpecialization))
		{
			const auto range = value.getSourceRange();
			expected_compounds.emplace(range.getBegin().getRawEncoding(),
									   range.getEnd().getRawEncoding());
		}
		return true;
	}
};
struct consumer : clang::SemaConsumer
{
	project_template_observations& out;
	clang::Sema* sema{};
	consumer(project_template_observations& value) : out(value) {}
	void InitializeSema(clang::Sema& value) override
	{
		sema = &value;
		install_project_template_observer(value, out);
	}
	void ForgetSema() override
	{
		sema = nullptr;
	}
	void HandleTranslationUnit(clang::ASTContext& context) override
	{
		original_type_census independent;
		independent.TraverseDecl(context.getTranslationUnitDecl());
		auto result = observe_project_templates(*sema, out);
		if (!result)
		{
			std::cerr << result.error().code << ':' << result.error().detail << '\n';
			std::abort();
		}
	}
};
struct action : clang::ASTFrontendAction
{
	project_template_observations& out;
	action(project_template_observations& value) : out(value) {}
	std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance&,
														  llvm::StringRef) override
	{
		return std::make_unique<consumer>(out);
	}
};
void require(bool value, const char* reason)
{
	if (!value)
	{
		std::cerr << reason << "\n";
		std::exit(1);
	}
}
int main()
{
	project_template_observations out;
	const bool okay =
		clang::tooling::runToolOnCodeWithArgs(std::make_unique<action>(out),
											  R"CPP(
 template<class T> concept Size = sizeof(T)>1;
 static_assert(Size<int>);
 template<class T> void compound(T* pointer, typename T::template Inner<int>* nested);
 template<class T> requires Size<T> struct Box { T field; };
 template<class T> constexpr int id(T value) { return value; }
 template<class T> int dependent(T value) requires requires { value.run(); } { typename T::item x; return value.run(); }
 Box<int> b; int use() { int i=7; Box<int> local; auto lambda=[i,&local] { return i+local.field; }; return lambda()+id(3); }
 )CPP",
											  {"-std=c++23", "-nostdinc", "-nostdinc++"},
											  "/project/main.cpp");
	if (!okay)
		return 2;

	require(std::ranges::any_of(out.subjects,
								[&](const auto& original)
								{
									return original.kind == "argument_use" &&
										original.argument_state == "complete" && original.primary &&
										out.subjects[*original.primary].semantic_usr ==
										"c:@CT@Size";
								}),
			"concrete concept use retains actual canonical arguments and primary");
	require(std::ranges::any_of(out.subjects,
								[&](const auto& original)
								{
									return original.kind == "dependent_type_use" &&
										original.source.end > original.source.begin &&
										original.source.end - original.source.begin > 1U;
								}),
			"compound and qualified dependent type constructions are admitted");
	require(expected_compounds.size() >= 3U, "independent compiler compound census exists");
	for (const auto& expected : expected_compounds)
		require(std::ranges::any_of(out.subjects,
									[&](const auto& original)
									{
										return original.kind == "dependent_type_use" &&
											!original.occurrence_kind.empty() &&
											original.source.begin == expected.first &&
											original.source.end == expected.second;
									}),
				"every actual dependent pointer/qualified-template occurrence retained");
	require(out.trace_initialised && out.trace_frozen && !out.trace_partial,
			"original preparse trace closed");
	require(std::ranges::any_of(out.subjects,
								[](auto& s)
								{
									return s.kind == "implicit_instance" &&
										s.argument_state == "complete";
								}),
			"actual implicit specialization tuple");
	require(std::ranges::any_of(out.subjects,
								[](auto& s)
								{
									return s.kind == "dependent_type_use";
								}),
			"written dependent type");
	require(std::ranges::any_of(out.subjects,
								[](auto& s)
								{
									return s.kind == "dependent_unresolved_call";
								}),
			"actual unresolved dependent call");
	require(std::ranges::any_of(out.subjects,
								[](auto& s)
								{
									return s.kind == "lambda" && s.closure_size_bits.has_value();
								}),
			"actual closure layout");
	require(!out.constraints.empty() && !out.frames.empty(),
			"actual normalized tree and original frames");
	for (auto& c : out.captures)
		require(c.complete && c.size_bits.has_value(), "actual capture field mapping");
	project_template_bindings bindings;
	bindings.compile_unit = "unit:original-template-fixture";
	const auto rows = detach_project_templates(out, bindings);
	if (!rows)
	{
		std::cerr << rows.error().code << ':' << rows.error().field << ':' << rows.error().detail
				  << '\n';
		return 3;
	}
	require(rows->size() ==
				out.subjects.size() + out.constraints.size() + out.captures.size() +
					out.frames.size() + 1,
			"all original members and independent atomic inventory detach");
	for (const auto& row : *rows)
	{
		if (row.descriptor_id == "cc.template_inventory.v1" ||
			row.descriptor_id == "cc.template_subject.v1")
		{
			const auto& profile = row.cells.at(row.descriptor_id + ".profile");
			require(profile.value.has_value() &&
						std::get<std::string>(*profile.value) ==
							"clang22-original-template-domains/2",
					"corrected actual source admission has its own semantic profile");
		}
		if (row.descriptor_id == "cc.template_subject.v1" &&
			std::get<std::string>(*row.cells.at(row.descriptor_id + ".kind").value) ==
				"dependent_type_use")
		{
			const auto occurrence = row.cells.find(row.descriptor_id + ".occurrence_kind");
			require(occurrence != row.cells.end() && occurrence->second.value.has_value() &&
						std::get<std::string>(*occurrence->second.value).starts_with("type-loc:"),
					"actual compiler type constructor discriminator remains observable");
		}
	}
	project_template_limits bounded;
	bounded.maximum_members = 1;
	const auto oversized = detach_project_templates(out, bindings, bounded);
	require(!oversized && oversized.error().code == "native.template-limit",
			"native membership bound before allocation");
	unsigned polls{};
	bounded = {};
	bounded.cancelled = [&]
	{
		return ++polls == 5;
	};
	const auto cancelled = detach_project_templates(out, bindings, bounded);
	require(!cancelled && cancelled.error().code == "native.template-cancelled",
			"native active callback cancellation");
	std::cout << "original template native fixture PASS\n";
}
