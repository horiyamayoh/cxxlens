#pragma once

#include "project_source_feature_walk.hpp"

namespace clang
{
	class FunctionDecl;
} // namespace clang

namespace cxxlens::detail::clang22
{
	enum class source_feature_type_role : std::uint8_t
	{
		unsupported,
		declared,
		expression,
		located
	};
	enum class source_feature_reference_kind : std::uint8_t
	{
		unsupported,
		declaration_reference,
		member_reference,
		selected_constructor,
		direct_callee,
		indirect_call
	};
	struct original_source_feature_references
	{
		clang::QualType type;
		source_feature_type_role type_role{source_feature_type_role::unsupported};
		const clang::Decl* referenced_declaration{};
		const clang::FunctionDecl* selected_callable{};
		source_feature_reference_kind reference_kind{source_feature_reference_kind::unsupported};
	};

	// These are actual original compiler objects/method outcomes. Entity, callable
	// and type bindings remain independent, and all borrowed objects expire with
	// the native job. Unsupported is never interpreted as absence of references.
	[[nodiscard]] original_source_feature_references
	observe_original_source_feature_references(const original_source_feature_view&) noexcept;
} // namespace cxxlens::detail::clang22
