#pragma once

#include <cstddef>

#include "object_primitive_facts.hpp"

namespace clang
{
	class ASTContext;
	class CastExpr;
} // namespace clang

namespace cxxlens::detail::clang22::object_semantics
{
	/** Borrow the exact compiler cast and source operand in its extraction
	 * callback. The publisher must retain the original syntax/type/source FKs;
	 * this helper never derives storage identity or wider rule closure. */
	[[nodiscard]] enum_value observe_enum_value(const clang::CastExpr&,
												const clang::ASTContext&,
												std::size_t maximum_integer_bits = 16'384U);
	[[nodiscard]] bool_representation
	observe_bool_representation(const clang::CastExpr&,
								const clang::ASTContext&,
								std::size_t maximum_integer_bits = 16'384U);
} // namespace cxxlens::detail::clang22::object_semantics
