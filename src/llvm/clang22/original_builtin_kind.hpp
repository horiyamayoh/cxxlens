#pragma once

#include <optional>
#include <string_view>

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Type.h>

namespace cxxlens::detail::clang22
{
	struct original_builtin_kind_observation
	{
		std::string_view kind;
		std::string_view state;
	};

	// Names are exact compiler enum discriminators. They are not pretty type
	// spellings, object-size predicates or a consumer structural grammar.
	inline std::string_view original_builtin_kind_name(clang::BuiltinType::Kind kind)
	{
		switch (kind)
		{
#define IMAGE_TYPE(ImgType, Id, SingletonId, Access, Suffix) \
	case clang::BuiltinType::Id: \
		return #Id;
#include <clang/Basic/OpenCLImageTypes.def>
#define EXT_OPAQUE_TYPE(ExtType, Id, Ext) \
	case clang::BuiltinType::Id: \
		return #Id;
#include <clang/Basic/OpenCLExtensionTypes.def>
#define SVE_TYPE(Name, Id, SingletonId) \
	case clang::BuiltinType::Id: \
		return #Id;
#include <clang/Basic/AArch64ACLETypes.def>
#define PPC_VECTOR_TYPE(Name, Id, Size) \
	case clang::BuiltinType::Id: \
		return #Id;
#include <clang/Basic/PPCTypes.def>
#define RVV_TYPE(Name, Id, SingletonId) \
	case clang::BuiltinType::Id: \
		return #Id;
#include <clang/Basic/RISCVVTypes.def>
#define WASM_TYPE(Name, Id, SingletonId) \
	case clang::BuiltinType::Id: \
		return #Id;
#include <clang/Basic/WebAssemblyReferenceTypes.def>
#define AMDGPU_TYPE(Name, Id, SingletonId, Width, Align) \
	case clang::BuiltinType::Id: \
		return #Id;
#include <clang/Basic/AMDGPUTypes.def>
#define HLSL_INTANGIBLE_TYPE(Name, Id, SingletonId) \
	case clang::BuiltinType::Id: \
		return #Id;
#include <clang/Basic/HLSLIntangibleTypes.def>
#define BUILTIN_TYPE(Id, SingletonId) \
	case clang::BuiltinType::Id: \
		return #Id;
#include <clang/AST/BuiltinTypes.def>
		}
		return "Unknown";
	}

	inline original_builtin_kind_observation observe_original_builtin_kind(clang::QualType input)
	{
		if (input.isNull())
			return {{}, "unavailable"};
		const auto canonical = input.getCanonicalType();
		const auto* builtin = llvm::dyn_cast<clang::BuiltinType>(canonical.getTypePtr());
		if (!builtin)
			return {};
		const auto kind = original_builtin_kind_name(builtin->getKind());
		if (kind == "Unknown")
			return {kind, "unsupported"};
		if (builtin->isDependentType())
			return {kind, "unknown"};
		if (builtin->isPlaceholderType())
			return {kind, "partial"};
		return {kind, "complete"};
	}

	struct original_integer_representation
	{
		std::optional<unsigned> bit_width;
		std::optional<bool> signed_value;
		std::string_view state;
		clang::QualType underlying_type;
	};

	inline original_integer_representation
	observe_original_integer_representation(const clang::ASTContext& context, clang::QualType input)
	{
		if (input.isNull())
			return {{}, {}, "unavailable", {}};
		const auto canonical = input.getCanonicalType();
		if (canonical->isDependentType())
			return {{}, {}, "unknown", {}};
		if (const auto* builtin = llvm::dyn_cast<clang::BuiltinType>(canonical.getTypePtr());
			builtin && builtin->isPlaceholderType())
			return {{}, {}, "partial", {}};
		clang::QualType representation = canonical;
		clang::QualType underlying;
		if (const auto* enumeration = canonical->getAs<clang::EnumType>())
		{
			underlying = enumeration->getDecl()->getIntegerType();
			if (underlying.isNull() || underlying->isDependentType())
				return {{}, {}, "unknown", {}};
			representation = underlying.getCanonicalType();
		}
		if (!representation->isIntegerType())
			return {{}, {}, "not_applicable", {}};
		// Original C99 integer value width excludes padding; sizeof/type layout
		// cannot substitute it for _BitInt. Bool's actual value domain 0..1 is
		// separately identified by original builtin_kind=Bool. Enum underlying
		// representation never claims to enumerate the enum's value domain.
		const unsigned width = context.getIntWidth(representation);
		if (width == 0U)
			return {{}, {}, "unsupported", underlying};
		return {width, representation->isSignedIntegerType(), "complete", underlying};
	}
} // namespace cxxlens::detail::clang22
