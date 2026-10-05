#include "project_abi_observer.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>
#include <string_view>

#if defined(CXXLENS_HAS_CLANG22) && CXXLENS_HAS_CLANG22
#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/GlobalDecl.h>
#include <clang/AST/RecordLayout.h>
#include <clang/AST/VTableBuilder.h>
#include <clang/Basic/CodeGenOptions.h>
#include <clang/Basic/TargetInfo.h>
#include <clang/Basic/Version.h>
#include <clang/CodeGen/CodeGenABITypes.h>
#include <clang/CodeGen/ModuleBuilder.h>
#include <clang/Lex/HeaderSearch.h>
#include <clang/Lex/Preprocessor.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>

#endif

namespace cxxlens::detail::clang22
{
#if defined(CXXLENS_HAS_CLANG22) && CXXLENS_HAS_CLANG22
	namespace
	{
		struct abi_failure
		{
			sdk::error value;
		};
		struct unsupported
		{
			std::string reason;
		};
		[[noreturn]] void fail(std::string field, std::string reason)
		{
			throw abi_failure{
				{"application-analysis.abi-budget", std::move(field), std::move(reason)}};
		}
		std::uint64_t positive(clang::CharUnits value)
		{
			const auto v = value.getQuantity();
			if (v < 0)
				throw unsupported{"compiler-negative-storage-offset"};
			return static_cast<std::uint64_t>(v);
		}
	} // namespace
	class project_abi_observer::implementation
	{
	  public:
		implementation(clang::ASTContext& ast,
					   clang::Preprocessor& pp,
					   const clang::CodeGenOptions& opts,
					   project_abi_limits limits)
			: ast_{ast}, pp_{pp}, options_{opts}, limits_{limits}
		{
			if (!limits.maximum_operations || !limits.maximum_depth ||
				!limits.maximum_signature_bytes || !limits.maximum_extents)
				fail("limits", "zero-limit");
			const auto observation_signature_limit = limits_.maximum_signature_bytes;
			limits_.maximum_signature_bytes = 64U * 1024U;
			std::string payload;
			text(payload, "clang22-storage-and-call-interface/1");
			text(payload, CLANG_VERSION_STRING);
			text(payload, ast.getTargetInfo().getTriple().str());
			text(payload, ast.getTargetInfo().getDataLayoutString());
			text(payload, ast.getTargetInfo().getABI().str());
			number(payload, static_cast<std::uint64_t>(ast.getTargetInfo().getCXXABI().getKind()));
			number(payload, static_cast<std::uint64_t>(ast.getLangOpts().getClangABICompat()));
			number(payload, ast.getLangOpts().RelativeCXXABIVTables);
			number(payload, ast.getLangOpts().OmitVTableRTTI);
			auto digest = sdk::semantic_digest("cc.clang22.abi-context.v1", payload);
			if (!digest)
				throw abi_failure{digest.error()};
			context_ = std::move(*digest);
			limits_.maximum_signature_bytes = observation_signature_limit;
		}
		sdk::result<project_abi_observation> record(const clang::RecordDecl& declaration)
		{
			try
			{
				reset();
				project_abi_observation out;
				out.abi_context = context_;
				const auto* def = declaration.getDefinition();
				if (!declaration.isThisDeclarationADefinition() || !def ||
					!def->isCompleteDefinition() || def->isInvalidDecl())
				{
					out.reason = "record-definition-unavailable";
					return out;
				}
				if (def->isDependentContext())
				{
					out.reason = "observe-record-specialization";
					return out;
				}
				const auto& actual = ast_.getASTRecordLayout(def);
				out.byte_size = positive(actual.getSize());
				out.byte_alignment = positive(actual.getAlignment());
				try
				{
					occupied(*def, 0, out.occupied_ranges, 0, true, *out.byte_size);
					if (out.occupied_ranges.empty())
					{
						if (const auto* cxx = llvm::dyn_cast<clang::CXXRecordDecl>(def);
							cxx && cxx->isEmpty() && *out.byte_size)
							add_extent(out.occupied_ranges, 0, 1, *out.byte_size);
					}
					normalize(out.occupied_ranges, *out.byte_size);
					out.layout_state = "complete";
				}
				catch (const unsupported& e)
				{
					out.reason = e.reason;
					out.layout_state = "partial";
					normalize(out.occupied_ranges, *out.byte_size);
				}
				try
				{
					text(out.abi_signature, "record");
					record_signature(*def, out.abi_signature, 0, true);
					out.abi_state = "complete";
				}
				catch (const unsupported& e)
				{
					out.abi_signature.clear();
					if (out.reason.empty())
						out.reason = e.reason;
					out.abi_state = "unknown";
				}
				return out;
			}
			catch (const abi_failure& e)
			{
				return sdk::unexpected(e.value);
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(sdk::error{
					"application-analysis.abi-allocation-failed", "record", "allocation"});
			}
		}
		sdk::result<project_abi_observation> function(const clang::FunctionDecl& function)
		{
			try
			{
				reset();
				project_abi_observation out;
				out.abi_context = context_;
				out.reason = "function-has-no-object-layout";
				if (function.isInvalidDecl() || function.isDependentContext() ||
					function.getType()->isDependentType())
				{
					out.reason = "observe-function-specialization";
					return out;
				}
				try
				{
					text(out.abi_signature, "function");
					function_signature(function, out.abi_signature, 0);
					out.abi_state = "complete";
				}
				catch (const unsupported& e)
				{
					out.abi_signature.clear();
					out.reason = e.reason;
				}
				return out;
			}
			catch (const abi_failure& e)
			{
				return sdk::unexpected(e.value);
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(sdk::error{
					"application-analysis.abi-allocation-failed", "function", "allocation"});
			}
		}

	  private:
		void reset()
		{
			operations_ = 0;
		}
		void work(std::size_t depth = 0)
		{
			if (depth > limits_.maximum_depth)
				fail("depth", "limit-exceeded");
			if (++operations_ > limits_.maximum_operations)
				fail("operations", "limit-exceeded");
		}
		void reserve(std::string& to, std::size_t bytes)
		{
			if (to.size() > limits_.maximum_signature_bytes ||
				bytes > limits_.maximum_signature_bytes - to.size())
				fail("signature", "byte-limit");
		}
		void number(std::string& out, std::uint64_t value)
		{
			reserve(out, 8U);
			for (unsigned s{}; s < 64U; s += 8U)
				out.push_back(static_cast<char>((value >> s) & 255U));
		}
		void text(std::string& out, std::string_view value)
		{
			reserve(out, 8U + value.size());
			number(out, value.size());
			out.append(value);
		}
		void signed_number(std::string& out, std::int64_t value)
		{
			number(out, static_cast<std::uint64_t>(value));
		}
		clang::CodeGenerator& generator()
		{
			if (!generator_)
			{
				generator_ =
					clang::CreateLLVMCodeGen(pp_.getDiagnostics(),
											 "cxxlens-abi-observer",
											 pp_.getFileManager().getVirtualFileSystemPtr(),
											 pp_.getHeaderSearchInfo().getHeaderSearchOpts(),
											 pp_.getPreprocessorOpts(),
											 options_,
											 llvm_);
				if (!generator_)
					throw unsupported{"compiler-code-generation-context-unavailable"};
				generator_->Initialize(ast_);
			}
			return *generator_;
		}
		void llvm_type(llvm::Type* type, std::string& out, std::size_t depth)
		{
			work(depth);
			if (!type)
			{
				number(out, 0);
				return;
			}
			number(out, static_cast<std::uint64_t>(type->getTypeID()) + 1U);
			if (auto* integer = llvm::dyn_cast<llvm::IntegerType>(type))
				number(out, integer->getBitWidth());
			else if (auto* pointer = llvm::dyn_cast<llvm::PointerType>(type))
				number(out, pointer->getAddressSpace());
			else if (auto* array = llvm::dyn_cast<llvm::ArrayType>(type))
			{
				number(out, array->getNumElements());
				llvm_type(array->getElementType(), out, depth + 1U);
			}
			else if (auto* vector = llvm::dyn_cast<llvm::VectorType>(type))
			{
				number(out, vector->getElementCount().getKnownMinValue());
				number(out, vector->getElementCount().isScalable());
				llvm_type(vector->getElementType(), out, depth + 1U);
			}
			else if (auto* record = llvm::dyn_cast<llvm::StructType>(type))
			{
				if (record->isOpaque())
					throw unsupported{"compiler-opaque-storage-type"};
				number(out, record->isPacked());
				number(out, record->getNumElements());
				for (auto* field : record->elements())
					llvm_type(field, out, depth + 1U);
			}
			else if (auto* function = llvm::dyn_cast<llvm::FunctionType>(type))
			{
				number(out, function->isVarArg());
				llvm_type(function->getReturnType(), out, depth + 1U);
				number(out, function->getNumParams());
				for (auto* p : function->params())
					llvm_type(p, out, depth + 1U);
			}
			else if (!type->isVoidTy() && !type->isFloatingPointTy() && !type->isTokenTy() &&
					 !type->isLabelTy() && !type->isMetadataTy())
				throw unsupported{"unsupported-compiler-storage-type"};
		}
		bool abi_attribute(llvm::Attribute::AttrKind kind)
		{
			switch (kind)
			{
				case llvm::Attribute::ByVal:
				case llvm::Attribute::ByRef:
				case llvm::Attribute::StructRet:
				case llvm::Attribute::InAlloca:
				case llvm::Attribute::Preallocated:
				case llvm::Attribute::InReg:
				case llvm::Attribute::Nest:
				case llvm::Attribute::SExt:
				case llvm::Attribute::ZExt:
				case llvm::Attribute::SwiftSelf:
				case llvm::Attribute::SwiftError:
				case llvm::Attribute::SwiftAsync:
				case llvm::Attribute::Returned:
				case llvm::Attribute::Alignment:
					return true;
				default:
					return false;
			}
		}
		void attributes(llvm::AttributeSet input, std::string& out, std::size_t depth)
		{
			std::vector<llvm::Attribute> attrs;
			for (const auto attr : input)
			{
				work(depth);
				if (!attr.isStringAttribute() && abi_attribute(attr.getKindAsEnum()))
					attrs.push_back(attr);
			}
			std::ranges::sort(attrs,
							  {},
							  [](const auto& a)
							  {
								  return a.getKindAsEnum();
							  });
			number(out, attrs.size());
			for (const auto& attr : attrs)
			{
				number(out, attr.getKindAsEnum());
				if (attr.isTypeAttribute())
				{
					number(out, 1U);
					llvm_type(attr.getValueAsType(), out, depth + 1U);
				}
				else if (attr.isIntAttribute())
				{
					number(out, 2U);
					number(out, attr.getValueAsInt());
				}
				else
					number(out, 0U);
			}
		}
		void global_signature(clang::GlobalDecl decl, std::string& out, std::size_t depth)
		{
			work(depth);
			auto* address = generator().GetAddrOfGlobal(decl, false);
			auto* function =
				address ? llvm::dyn_cast<llvm::Function>(address->stripPointerCasts()) : nullptr;
			if (!function)
				throw unsupported{"compiler-function-lowering-unavailable"};
			number(out, function->getCallingConv());
			llvm_type(function->getFunctionType(), out, depth + 1U);
			attributes(function->getAttributes().getRetAttrs(), out, depth + 1U);
			number(out, function->arg_size());
			for (unsigned index{}; index < function->arg_size(); ++index)
				attributes(function->getAttributes().getParamAttrs(index), out, depth + 1U);
		}
		void arg_info(const clang::CodeGen::ABIArgInfo& info, std::string& out, std::size_t depth)
		{
			work(depth);
			number(out, info.getKind());
			if (info.canHaveCoerceToType())
				llvm_type(info.getCoerceToType(), out, depth + 1U);
			llvm_type(info.getPaddingType(), out, depth + 1U);
			number(out, info.getPaddingInReg());
			if (info.isDirect() || info.isExtend() || info.isTargetSpecific())
			{
				number(out, info.getDirectOffset());
				number(out, info.getDirectAlign());
				number(out, info.getInReg());
				if (info.isDirect() || info.isTargetSpecific())
					number(out, info.getCanBeFlattened());
				if (info.isExtend())
				{
					number(out, info.isSignExt());
					number(out, info.isZeroExt());
				}
			}
			if (info.isIndirect() || info.isIndirectAliased())
			{
				number(out, positive(info.getIndirectAlign()));
				number(out, info.getIndirectAddrSpace());
				number(out, info.getIndirectRealign());
				if (info.isIndirect())
				{
					number(out, info.getIndirectByVal());
					number(out, info.getInReg());
					number(out, info.isSRetAfterThis());
				}
			}
			if (info.isCoerceAndExpand())
				llvm_type(info.getUnpaddedCoerceAndExpandType(), out, depth + 1U);
			if (info.isInAlloca())
			{
				number(out, info.getInAllocaFieldIndex());
				number(out, info.getInAllocaIndirect());
				number(out, info.getInAllocaSRet());
			}
		}
		void function_signature(const clang::FunctionDecl& fn, std::string& out, std::size_t depth)
		{
			work(depth);
			const auto* proto = fn.getType()->getAs<clang::FunctionProtoType>();
			if (!proto)
				throw unsupported{"function-prototype-required"};
			const auto eligible = [&](clang::QualType type)
			{
				return type->isVoidType() ||
					(!type->isDependentType() && !type->isIncompleteType() &&
					 !type->isUndeducedType());
			};
			if (!eligible(proto->getReturnType()))
				throw unsupported{"complete-function-result-type-required"};
			for (const auto t : proto->param_types())
				if (!eligible(t))
					throw unsupported{"complete-function-parameter-type-required"};
			if (const auto* ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(&fn))
			{
				number(out, 2U);
				global_signature(clang::GlobalDecl(ctor, clang::Ctor_Complete), out, depth + 1U);
				global_signature(clang::GlobalDecl(ctor, clang::Ctor_Base), out, depth + 1U);
				return;
			}
			if (const auto* dtor = llvm::dyn_cast<clang::CXXDestructorDecl>(&fn))
			{
				number(out, dtor->isVirtual() ? 3U : 2U);
				global_signature(clang::GlobalDecl(dtor, clang::Dtor_Complete), out, depth + 1U);
				global_signature(clang::GlobalDecl(dtor, clang::Dtor_Base), out, depth + 1U);
				if (dtor->isVirtual())
					global_signature(
						clang::GlobalDecl(dtor, clang::Dtor_Deleting), out, depth + 1U);
				return;
			}
			number(out, 1U);
			global_signature(clang::GlobalDecl(&fn), out, depth + 1U);
			const auto& info = llvm::isa<clang::CXXMethodDecl>(&fn)
				? clang::CodeGen::arrangeCXXMethodType(
					  generator().CGM(),
					  llvm::cast<clang::CXXMethodDecl>(&fn)->getParent(),
					  proto,
					  llvm::cast<clang::CXXMethodDecl>(&fn))
				: clang::CodeGen::arrangeFreeFunctionType(
					  generator().CGM(),
					  ast_.getCanonicalType(fn.getType()).castAs<clang::FunctionProtoType>());
			number(out, info.getEffectiveCallingConvention());
			number(out, info.isVariadic());
			number(out, info.getNumRequiredArgs());
			number(out, info.isInstanceMethod());
			number(out, info.getMaxVectorWidth());
			arg_info(info.getReturnInfo(), out, depth + 1U);
			number(out, info.arg_size());
			for (const auto& arg : info.arguments())
				arg_info(arg.info, out, depth + 1U);
			if (info.usesInAlloca())
			{
				number(out, 1U);
				llvm_type(info.getArgStruct(), out, depth + 1U);
				number(out, positive(info.getArgStructAlignment()));
			}
			else
				number(out, 0U);
		}
		void add_extent(std::vector<std::pair<std::uint64_t, std::uint64_t>>& out,
						std::uint64_t begin,
						std::uint64_t size,
						std::uint64_t bound)
		{
			work();
			if (!size)
				return;
			if (begin > bound || size > bound - begin)
				throw unsupported{"compiler-storage-extent-outside-object"};
			if (out.size() >= limits_.maximum_extents)
				fail("occupied-ranges", "count-limit");
			out.emplace_back(begin, begin + size);
		}
		void normalize(std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges,
					   std::uint64_t bound)
		{
			std::ranges::sort(ranges);
			std::size_t used{};
			for (const auto& r : ranges)
			{
				work();
				if (r.first >= r.second || r.second > bound)
					throw unsupported{"compiler-storage-extent-outside-object"};
				if (used && r.first <= ranges[used - 1].second)
					ranges[used - 1].second = std::max(ranges[used - 1].second, r.second);
				else
					ranges[used++] = r;
			}
			ranges.resize(used);
		}
		void occupied(const clang::RecordDecl& record,
					  std::uint64_t offset,
					  std::vector<std::pair<std::uint64_t, std::uint64_t>>& out,
					  std::size_t depth,
					  bool complete,
					  std::uint64_t rootbound)
		{
			work(depth);
			const auto& layout = ast_.getASTRecordLayout(&record);
			const auto* cxx = llvm::dyn_cast<clang::CXXRecordDecl>(&record);
			if (cxx)
			{
				if (cxx->isEmpty() && offset < rootbound)
					add_extent(out, offset, 1, rootbound);
				const auto pointer = ast_.getTargetInfo().getPointerWidth(clang::LangAS::Default) /
					ast_.getCharWidth();
				if (layout.hasOwnVFPtr())
					add_extent(out, offset, pointer, rootbound);
				if (layout.hasOwnVBPtr())
					add_extent(out, offset + positive(layout.getVBPtrOffset()), pointer, rootbound);
				for (const auto& base : cxx->bases())
					if (!base.isVirtual())
					{
						work(depth);
						const auto* b = base.getType()->getAsCXXRecordDecl();
						if (!b || !b->hasDefinition())
							throw unsupported{"complete-base-layout-required"};
						occupied(*b->getDefinition(),
								 offset + positive(layout.getBaseClassOffset(b)),
								 out,
								 depth + 1U,
								 false,
								 rootbound);
					}
				if (complete)
					for (const auto& base : cxx->vbases())
					{
						work(depth);
						const auto* b = base.getType()->getAsCXXRecordDecl();
						if (!b || !b->hasDefinition())
							throw unsupported{"complete-virtual-base-layout-required"};
						occupied(*b->getDefinition(),
								 offset + positive(layout.getVBaseClassOffset(b)),
								 out,
								 depth + 1U,
								 false,
								 rootbound);
					}
			}
			unsigned ordinal{};
			for (const auto* field : record.fields())
			{
				work(depth);
				const auto bit = layout.getFieldOffset(ordinal++);
				if (field->isZeroSize(ast_))
				{
					if (const auto* empty = field->getType()->getAsCXXRecordDecl();
						empty && empty->isEmpty())
					{
						if (bit % ast_.getCharWidth())
							throw unsupported{"compiler-nonbyte-empty-member-offset"};
						const auto begin = offset + bit / ast_.getCharWidth();
						if (begin < rootbound)
							add_extent(out, begin, 1, rootbound);
					}
					continue;
				}
				if (field->isBitField())
				{
					if (field->isUnnamedBitField())
						continue;
					const auto width = field->getBitWidthValue();
					if (!width)
						continue;
					const auto charwidth = ast_.getCharWidth();
					if (width > std::numeric_limits<std::uint64_t>::max() - bit)
						throw unsupported{"compiler-bitfield-width-overflow"};
					const auto end = bit + width;
					if (end > std::numeric_limits<std::uint64_t>::max() - (charwidth - 1U))
						throw unsupported{"compiler-bitfield-width-overflow"};
					add_extent(out,
							   offset + bit / charwidth,
							   (end + charwidth - 1U) / charwidth - bit / charwidth,
							   rootbound);
				}
				else
				{
					if (bit % ast_.getCharWidth())
						throw unsupported{"compiler-nonbyte-member-offset"};
					const auto t = field->getType();
					if (t->isIncompleteArrayType())
						continue;
					if (t->isDependentType() || t->isIncompleteType() || t->isUndeducedType())
						throw unsupported{"complete-member-storage-type-required"};
					const auto size = t->isReferenceType()
						? ast_.getTargetInfo().getPointerWidth(t.getAddressSpace()) /
							ast_.getCharWidth()
						: positive(ast_.getTypeSizeInChars(t));
					add_extent(out, offset + bit / ast_.getCharWidth(), size, rootbound);
				}
			}
		}
		void record_signature(const clang::RecordDecl& record,
							  std::string& out,
							  std::size_t depth,
							  bool complete)
		{
			work(depth);
			const auto& layout = ast_.getASTRecordLayout(&record);
			number(out, record.isUnion());
			number(out, positive(layout.getSize()));
			number(out, positive(layout.getAlignment()));
			number(out, positive(layout.getDataSize()));
			const auto type = ast_.getCanonicalTagType(&record);
			llvm_type(
				clang::CodeGen::convertTypeForMemory(generator().CGM(), type), out, depth + 1U);
			unsigned ordinal{};
			number(out,
				   static_cast<std::uint64_t>(
					   std::distance(record.field_begin(), record.field_end())));
			for (const auto* field : record.fields())
			{
				work(depth);
				number(out, layout.getFieldOffset(ordinal++));
				number(out, field->isBitField());
				number(out, field->isZeroSize(ast_));
				if (field->isBitField())
					number(out, field->getBitWidthValue());
				llvm_type(clang::CodeGen::convertTypeForMemory(generator().CGM(), field->getType()),
						  out,
						  depth + 1U);
			}
			const auto* cxx = llvm::dyn_cast<clang::CXXRecordDecl>(&record);
			number(out, cxx != nullptr);
			if (!cxx)
				return;
			number(out, positive(layout.getNonVirtualSize()));
			number(out, positive(layout.getNonVirtualAlignment()));
			number(out, layout.hasOwnVFPtr());
			number(out, layout.hasOwnVBPtr());
			if (layout.hasVBPtr())
				number(out, positive(layout.getVBPtrOffset()));
			number(out, cxx->getNumBases());
			for (const auto& base : cxx->bases())
			{
				work(depth);
				number(out, base.isVirtual());
				const auto* b = base.getType()->getAsCXXRecordDecl();
				if (!b || !b->hasDefinition())
					throw unsupported{"complete-base-layout-required"};
				if (!base.isVirtual())
					number(out, positive(layout.getBaseClassOffset(b)));
				record_signature(*b->getDefinition(), out, depth + 1U, false);
			}
			number(out, complete ? cxx->getNumVBases() : 0U);
			if (complete)
				for (const auto& base : cxx->vbases())
				{
					const auto* b = base.getType()->getAsCXXRecordDecl();
					if (!b || !b->hasDefinition())
						throw unsupported{"complete-virtual-base-layout-required"};
					number(out, positive(layout.getVBaseClassOffset(b)));
					record_signature(*b->getDefinition(), out, depth + 1U, false);
				}
			number(out, cxx->isDynamicClass());
			if (cxx->isDynamicClass())
			{
				auto* vt = llvm::dyn_cast<clang::ItaniumVTableContext>(ast_.getVTableContext());
				if (!vt)
					throw unsupported{"microsoft-vtable-interface-not-supported"};
				const auto& vtable = vt->getVTableLayout(cxx);
				number(out, vtable.getNumVTables());
				for (std::size_t i{}; i < vtable.getNumVTables(); ++i)
				{
					number(out, vtable.getVTableOffset(i));
					number(out, vtable.getVTableSize(i));
				}
				number(out, vtable.vtable_components().size());
				for (const auto& component : vtable.vtable_components())
				{
					work(depth);
					number(out, component.getKind());
					switch (component.getKind())
					{
						case clang::VTableComponent::CK_VCallOffset:
							signed_number(out, component.getVCallOffset().getQuantity());
							break;
						case clang::VTableComponent::CK_VBaseOffset:
							signed_number(out, component.getVBaseOffset().getQuantity());
							break;
						case clang::VTableComponent::CK_OffsetToTop:
							signed_number(out, component.getOffsetToTop().getQuantity());
							break;
						case clang::VTableComponent::CK_RTTI:
							break;
						case clang::VTableComponent::CK_UnusedFunctionPointer:
							function_signature(*component.getUnusedFunctionDecl(), out, depth + 1U);
							break;
						default:
							global_signature(component.getGlobalDecl(false), out, depth + 1U);
							break;
					}
				}
				number(out, vtable.vtable_thunks().size());
				for (const auto& [slot, thunk] : vtable.vtable_thunks())
				{
					work(depth);
					number(out, slot);
					signed_number(out, thunk.This.NonVirtual);
					signed_number(out, thunk.This.Virtual.Itanium.VCallOffsetOffset);
					signed_number(out, thunk.Return.NonVirtual);
					signed_number(out, thunk.Return.Virtual.Itanium.VBaseOffsetOffset);
				}
			}
		}
		clang::ASTContext& ast_;
		clang::Preprocessor& pp_;
		const clang::CodeGenOptions& options_;
		project_abi_limits limits_;
		std::size_t operations_{};
		std::string context_;
		llvm::LLVMContext llvm_;
		std::unique_ptr<clang::CodeGenerator> generator_;
	};
	project_abi_observer::project_abi_observer(clang::ASTContext& a,
											   clang::Preprocessor& p,
											   const clang::CodeGenOptions& o,
											   project_abi_limits l)
		: impl_{std::make_unique<implementation>(a, p, o, l)}
	{
	}
	project_abi_observer::~project_abi_observer() = default;
	sdk::result<project_abi_observation> project_abi_observer::record(const clang::RecordDecl& r)
	{
		return impl_->record(r);
	}
	sdk::result<project_abi_observation>
	project_abi_observer::function(const clang::FunctionDecl& f)
	{
		return impl_->function(f);
	}
#else
	class project_abi_observer::implementation
	{
	};
	project_abi_observer::project_abi_observer(clang::ASTContext&,
											   clang::Preprocessor&,
											   const clang::CodeGenOptions&,
											   project_abi_limits)
	{
	}
	project_abi_observer::~project_abi_observer() = default;
	sdk::result<project_abi_observation> project_abi_observer::record(const clang::RecordDecl&)
	{
		return sdk::unexpected(
			sdk::error{"native.unsupported-clang-major", "abi", "clang-major-22"});
	}
	sdk::result<project_abi_observation> project_abi_observer::function(const clang::FunctionDecl&)
	{
		return sdk::unexpected(
			sdk::error{"native.unsupported-clang-major", "abi", "clang-major-22"});
	}
#endif

} // namespace cxxlens::detail::clang22
