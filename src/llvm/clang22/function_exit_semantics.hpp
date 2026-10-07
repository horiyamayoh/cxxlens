#pragma once
#include <cstddef>
#include <vector>

#include "object_primitive_facts.hpp"
namespace clang
{
	class FunctionDecl;
	class CFG;
} // namespace clang
namespace cxxlens::detail::clang22::object_semantics
{
	struct exit_member
	{
		std::size_t predecessor{}, exit{};
		missing_return facts;
	};
	struct exit_projection
	{
		bool enumeration_complete{};
		std::vector<exit_member> members;
	};
	/** Borrow the compiler's actual function/CFG in the same extraction callback.
	 * Members retain compiler block ordinals only; the emitter must bind them to
	 * its original CFG row IDs and original function/unit/world before transfer. */
	[[nodiscard]] exit_projection observe_function_exits(const clang::FunctionDecl&,
														 const clang::CFG&,
														 std::size_t maximum_blocks = 100'000U,
														 std::size_t maximum_work = 1'000'000U);
} // namespace cxxlens::detail::clang22::object_semantics
