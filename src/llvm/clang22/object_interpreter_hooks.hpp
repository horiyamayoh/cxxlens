#pragma once

#include <cstdint>

#include <clang/AST/APValue.h>
#include <clang/AST/Type.h>
#include <llvm/ADT/ArrayRef.h>

namespace clang
{
	class ASTContext;
	class Expr;
	class FieldDecl;
	class FunctionDecl;
} // namespace clang
namespace cxxlens_object_semantics_hook
{
	/** Functional access routes exported by the actual linked original evaluator.
	 * These describe compiler observations, not complete runtime path coverage. */
	[[nodiscard]] std::uint32_t interpreter_routes() noexcept;
	/** Original interpreter state at a failed or successful access, before the
	 * compiler state is discarded. This is not a diagnostic/rule classification.
	 * The view is borrowed for this callback only. Implementations immediately
	 * detach source/USR/type/generation/path axes and never retain AST pointers
	 * across the parser job. Absence of events cannot close runtime paths. */
	enum class interpreter_kind
	{
		inactive_union_member,
		absent_subobject,
		ended_call_frame,
		deleted_allocation,
		incompatible_static_downcast,
		before_dynamic_construction
	};
	struct interpreter_access_view
	{
		interpreter_kind kind;
		const clang::Expr* expression{};
		const clang::FunctionDecl* owner{};
		clang::APValue::LValueBase storage;
		clang::QualType complete_type, subobject_type, target_type;
		llvm::ArrayRef<clang::APValue::LValuePathEntry> path;
		unsigned access_kind{};
		const clang::FieldDecl* active_field{};
		const clang::FieldDecl* selected_field{};
		bool actual_polymorphic_use{};
	};
	void
	interpreter_access(clang::ASTContext&, const void* evaluation, const interpreter_access_view&);
} // namespace cxxlens_object_semantics_hook
