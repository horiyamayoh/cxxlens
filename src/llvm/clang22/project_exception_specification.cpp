#include "project_exception_specification.hpp"

#include <clang/AST/Decl.h>
#include <clang/AST/Type.h>
#include <clang/Basic/ExceptionSpecificationType.h>

namespace cxxlens::detail::clang22
{
	function_exception_specification
	observe_function_exception_specification(const clang::FunctionDecl& function)
	{
		const auto* type = function.getType()->getAs<clang::FunctionProtoType>();
		if (!type)
			return {};
		function_exception_specification result;
		result.state = "complete";
		// The compiler's stored exception-specification enum is safe even when
		// canThrow()/isNothrow() would instantiate or evaluate a lazy specification.
		switch (type->getExceptionSpecType())
		{
			case clang::EST_None:
				result.kind = "none";
				result.nonthrowing = false;
				break;
			case clang::EST_DynamicNone:
				result.kind = "dynamic_none";
				result.nonthrowing = true;
				break;
			case clang::EST_Dynamic:
				result.kind = "dynamic";
				result.nonthrowing = false;
				break;
			case clang::EST_MSAny:
				result.kind = "ms_any";
				result.nonthrowing = false;
				break;
			case clang::EST_NoThrow:
				result.kind = "no_throw";
				result.nonthrowing = true;
				break;
			case clang::EST_BasicNoexcept:
				result.kind = "basic_noexcept";
				result.nonthrowing = true;
				break;
			case clang::EST_NoexceptFalse:
				result.kind = "noexcept_false";
				result.nonthrowing = false;
				break;
			case clang::EST_NoexceptTrue:
				result.kind = "noexcept_true";
				result.nonthrowing = true;
				break;
			case clang::EST_DependentNoexcept:
				result.kind = "dependent_noexcept";
				result.state = "partial";
				break;
			case clang::EST_Unevaluated:
				result.kind = "unevaluated";
				result.state = "partial";
				break;
			case clang::EST_Uninstantiated:
				result.kind = "uninstantiated";
				result.state = "partial";
				break;
			case clang::EST_Unparsed:
				result.kind = "unparsed";
				result.state = "partial";
				break;
			default:
				result.kind = "unknown";
				result.state = "partial";
				break;
		}
		return result;
	}
} // namespace cxxlens::detail::clang22
