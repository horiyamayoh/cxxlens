#include "project_template_observer.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

#if defined(CXXLENS_HAS_CLANG22) && CXXLENS_HAS_CLANG22
#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/ExprConcepts.h>
#include <clang/AST/RecordLayout.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Index/USRGeneration.h>
#include <clang/Sema/SemaConcept.h>
#include <clang/Sema/TemplateInstCallback.h>
#include <llvm/ADT/SmallString.h>
#endif

namespace cxxlens::detail::clang22
{
	namespace
	{
		struct stopped
		{
			sdk::error error;
		};
		class budget
		{
		  public:
			budget(project_template_observations& out, const project_template_limits& limits)
				: out_(out), limits_(limits)
			{
			}
			void work(std::size_t amount = 1)
			{
				if (limits_.cancelled && limits_.cancelled())
					throw stopped{{"native.template-cancelled", "template", "cancelled"}};
				if (amount > limits_.maximum_operations -
						std::min(out_.operations, limits_.maximum_operations))
					throw stopped{{"native.template-limit", "template", "operations"}};
				out_.operations += amount;
			}
			void retain(std::size_t bytes)
			{
				work();
				if (bytes > limits_.maximum_retained_bytes -
						std::min(out_.retained_bytes, limits_.maximum_retained_bytes))
					throw stopped{{"native.template-limit", "template", "retained-bytes"}};
				out_.retained_bytes += bytes;
			}
			void member(std::size_t size, std::size_t bytes)
			{
				if (size >= limits_.maximum_members)
					throw stopped{{"native.template-limit", "template", "members"}};
				retain(bytes);
			}
			void depth(std::size_t value)
			{
				work();
				if (value > limits_.maximum_depth)
					throw stopped{{"native.template-limit", "template", "depth"}};
			}
			std::string string(std::string_view bytes)
			{
				retain(bytes.size());
				return std::string(bytes);
			}

		  private:
			project_template_observations& out_;
			const project_template_limits& limits_;
		};
#if defined(CXXLENS_HAS_CLANG22) && CXXLENS_HAS_CLANG22
		template_native_source source(clang::SourceRange value)
		{
			return {value.getBegin().getRawEncoding(), value.getEnd().getRawEncoding()};
		}
		std::string usr(const clang::Decl* value, budget& bounds)
		{
			if (!value)
				return {};
			llvm::SmallString<128> bytes;
			bounds.work();
			if (clang::index::generateUSRForDecl(value->getCanonicalDecl(), bytes))
				return {};
			return bounds.string(bytes.str());
		}
		class arguments
		{
		  public:
			arguments(clang::ASTContext& context, budget& bounds)
				: context_(context), bounds_(bounds)
			{
			}
			std::optional<std::string> encode(llvm::ArrayRef<clang::TemplateArgument> values)
			{
				std::string result;
				append(result, "clang22-canonical-template-argument-tuple/1");
				append(result, std::to_string(values.size()));
				for (const auto& value : values)
					if (!argument(result, context_.getCanonicalTemplateArgument(value), 0))
						return std::nullopt;
				return result;
			}

		  private:
			clang::ASTContext& context_;
			budget& bounds_;
			void append(std::string& out, std::string_view value)
			{
				const auto count = std::to_string(value.size());
				bounds_.retain(count.size() + 1 + value.size());
				out += count;
				out += ':';
				out.append(value);
			}
			bool type(std::string& out, clang::QualType value)
			{
				if (value.isNull())
					return false;
				llvm::SmallString<128> bytes;
				bounds_.work();
				if (clang::index::generateUSRForType(
						context_.getCanonicalType(value), context_, bytes))
					return false;
				append(out, bytes.str());
				return true;
			}
			bool integer(std::string& out, const llvm::APInt& value, bool signed_)
			{
				llvm::SmallString<128> bytes;
				value.toString(bytes, 10, signed_);
				append(out, std::to_string(value.getBitWidth()));
				append(out, signed_ ? "signed" : "unsigned");
				append(out, bytes.str());
				return true;
			}
			bool apvalue(std::string& out,
						 const clang::APValue& value,
						 clang::QualType value_type,
						 std::size_t depth)
			{
				bounds_.depth(depth);
				append(out, std::to_string(value.getKind()));
				if (value.isInt())
					return integer(out, value.getInt(), value.getInt().isSigned());
				if (value.isFloat())
					return integer(out, value.getFloat().bitcastToAPInt(), false);
				if (value.isComplexInt())
					return integer(out, value.getComplexIntReal(), true) &&
						integer(out, value.getComplexIntImag(), true);
				if (value.isComplexFloat())
					return integer(out, value.getComplexFloatReal().bitcastToAPInt(), false) &&
						integer(out, value.getComplexFloatImag().bitcastToAPInt(), false);
				if (value.isVector())
				{
					const auto* vector =
						llvm::dyn_cast_or_null<clang::VectorType>(value_type.getTypePtrOrNull());
					if (!vector)
						return false;
					append(out, std::to_string(value.getVectorLength()));
					for (unsigned i = 0; i < value.getVectorLength(); ++i)
						if (!apvalue(
								out, value.getVectorElt(i), vector->getElementType(), depth + 1))
							return false;
					return true;
				}
				if (value.isArray())
				{
					const auto* array = context_.getAsArrayType(value_type);
					if (!array)
						return false;
					append(out, std::to_string(value.getArraySize()));
					append(out, std::to_string(value.getArrayInitializedElts()));
					for (unsigned i = 0; i < value.getArrayInitializedElts(); ++i)
						if (!apvalue(out,
									 value.getArrayInitializedElt(i),
									 array->getElementType(),
									 depth + 1))
							return false;
					append(out, value.hasArrayFiller() ? "filler" : "no-filler");
					return !value.hasArrayFiller() ||
						apvalue(out, value.getArrayFiller(), array->getElementType(), depth + 1);
				}
				if (value.isStruct())
				{
					const auto* record = value_type->getAsCXXRecordDecl();
					if (!record)
						return false;
					append(out, std::to_string(value.getStructNumBases()));
					unsigned i{};
					for (const auto& base : record->bases())
					{
						if (i >= value.getStructNumBases() ||
							!apvalue(out, value.getStructBase(i++), base.getType(), depth + 1))
							return false;
					}
					if (i != value.getStructNumBases())
						return false;
					append(out, std::to_string(value.getStructNumFields()));
					i = 0;
					for (const auto* field : record->fields())
					{
						append(out, usr(field, bounds_));
						if (i >= value.getStructNumFields() ||
							!apvalue(out, value.getStructField(i++), field->getType(), depth + 1))
							return false;
					}
					return i == value.getStructNumFields();
				}
				if (value.isUnion())
				{
					const auto* field = value.getUnionField();
					if (!field)
					{
						append(out, "inactive");
						return true;
					}
					const auto identity = usr(field, bounds_);
					if (identity.empty())
						return false;
					append(out, identity);
					return apvalue(out, value.getUnionValue(), field->getType(), depth + 1);
				}
				if (value.isMemberPointer())
				{
					const auto* member = value.getMemberPointerDecl();
					if (!member)
					{
						append(out, "null");
						return true;
					}
					const auto identity = usr(member, bounds_);
					if (identity.empty())
						return false;
					append(out, identity);
					append(out, value.isMemberPointerToDerivedMember() ? "derived" : "base");
					append(out, std::to_string(value.getMemberPointerPath().size()));
					for (const auto* record : value.getMemberPointerPath())
					{
						const auto record_identity = usr(record, bounds_);
						if (record_identity.empty())
							return false;
						append(out, record_identity);
					}
					return true;
				}
				if (value.isLValue())
				{
					if (value.isNullPointer())
					{
						append(out, "null");
						return true;
					}
					const auto base = value.getLValueBase();
					const auto* declaration = base.dyn_cast<const clang::ValueDecl*>();
					if (!declaration || value.getLValueCallIndex() != 0 ||
						value.getLValueVersion() != 0)
						return false;
					const auto identity = usr(declaration, bounds_);
					if (identity.empty())
						return false;
					append(out, identity);
					append(out, std::to_string(value.getLValueOffset().getQuantity()));
					append(out, value.isLValueOnePastTheEnd() ? "one-past" : "within");
					if (!value.hasLValuePath())
					{
						append(out, "no-path");
						return true;
					}
					append(out, std::to_string(value.getLValuePath().size()));
					auto current = declaration->getType();
					for (const auto& entry : value.getLValuePath())
					{
						bounds_.work();
						if (const auto* array = context_.getAsArrayType(current))
						{
							append(out, "array");
							append(out, std::to_string(entry.getAsArrayIndex()));
							current = array->getElementType();
							continue;
						}
						if (!current->isRecordType())
							return false;
						const auto selected = entry.getAsBaseOrMember();
						const auto* selected_decl = selected.getPointer();
						const auto selected_usr = usr(selected_decl, bounds_);
						if (selected_usr.empty())
							return false;
						append(out, selected_usr);
						append(out, selected.getInt() ? "virtual" : "nonvirtual");
						if (const auto* field = llvm::dyn_cast<clang::FieldDecl>(selected_decl))
							current = field->getType();
						else if (const auto* record =
									 llvm::dyn_cast<clang::CXXRecordDecl>(selected_decl))
							current = context_.getCanonicalTagType(record);
						else
							return false;
					}
					return true;
				}
				return false;
			}
			bool argument(std::string& out, const clang::TemplateArgument& value, std::size_t depth)
			{
				bounds_.depth(depth);
				append(out, std::to_string(value.getKind()));
				switch (value.getKind())
				{
					case clang::TemplateArgument::Null:
						return false;
					case clang::TemplateArgument::Type:
						return type(out, value.getAsType());
					case clang::TemplateArgument::Declaration:
					{
						const auto identity = usr(value.getAsDecl(), bounds_);
						if (identity.empty())
							return false;
						append(out, identity);
						return type(out, value.getParamTypeForDecl());
					}
					case clang::TemplateArgument::NullPtr:
						return type(out, value.getNullPtrType());
					case clang::TemplateArgument::Integral:
						return type(out, value.getIntegralType()) &&
							integer(out, value.getAsIntegral(), value.getAsIntegral().isSigned());
					case clang::TemplateArgument::Template:
					case clang::TemplateArgument::TemplateExpansion:
					{
						const auto identity = usr(
							value.getAsTemplateOrTemplatePattern().getAsTemplateDecl(), bounds_);
						if (identity.empty())
							return false;
						append(out, identity);
						if (value.getKind() == clang::TemplateArgument::TemplateExpansion)
						{
							const auto count = value.getNumTemplateExpansions();
							append(out, count ? std::to_string(*count) : "dependent");
							if (!count)
								return false;
						}
						return true;
					}
					case clang::TemplateArgument::Pack:
						append(out, std::to_string(value.pack_size()));
						for (const auto& item : value.pack_elements())
							if (!argument(out, item, depth + 1))
								return false;
						return true;
					case clang::TemplateArgument::StructuralValue:
						return type(out, value.getStructuralValueType()) &&
							apvalue(out,
									value.getAsStructuralValue(),
									value.getStructuralValueType(),
									depth + 1);
					case clang::TemplateArgument::Expression:
						return false;
				}
				return false;
			}
		};

		class trace final : public clang::TemplateInstantiationCallback
		{
		  public:
			trace(project_template_observations& out, project_template_limits limits)
				: out_(out), limits_(std::move(limits))
			{
			}
			void initialize(const clang::Sema&) override
			{
				if (!out_.trace_frozen)
					out_.trace_initialised = true;
			}
			void finalize(const clang::Sema&) override
			{
				if (!out_.trace_frozen && !out_.active_frames.empty())
					out_.trace_partial = true;
			}
			void atTemplateBegin(const clang::Sema& sema,
								 const clang::Sema::CodeSynthesisContext& context) override
			{
				if (out_.trace_frozen)
					return;
				try
				{
					budget bounds(out_, limits_);
					bounds.member(out_.frames.size(), sizeof(template_native_frame));
					template_native_frame item;
					item.ordinal = out_.frames.size();
					item.kind = bounds.string("synthesis:" + std::to_string(context.Kind));
					item.is_instantiation = context.isInstantiationRecord();
					item.depth = sema.CodeSynthesisContexts.size() - sema.NonInstantiationEntries;
					bounds.depth(item.depth);
					if (!out_.active_frames.empty())
						item.parent = out_.active_frames.back();
					item.entity_usr = usr(context.Entity, bounds);
					item.template_usr = usr(context.Template, bounds);
					item.source = source(context.InstantiationRange.isValid()
											 ? context.InstantiationRange
											 : clang::SourceRange(context.PointOfInstantiation));
					// These union variants carry call arguments or a special-member enum.
					if (context.Kind != clang::Sema::CodeSynthesisContext::DeclaringSpecialMember &&
						context.Kind !=
							clang::Sema::CodeSynthesisContext::BuildingBuiltinDumpStructCall)
					{
						const auto encoded =
							arguments(sema.Context, bounds).encode(context.template_arguments());
						item.argument_state = encoded ? "complete" : "partial";
						if (encoded)
							item.canonical_arguments = *encoded;
					}
					bounds.retain(sizeof(std::size_t));
					out_.active_frames.push_back(item.ordinal);
					out_.frames.push_back(std::move(item));
				}
				catch (const stopped&)
				{
					out_.trace_partial = true;
				}
				catch (const std::bad_alloc&)
				{
					out_.trace_partial = true;
				}
			}
			void atTemplateEnd(const clang::Sema&,
							   const clang::Sema::CodeSynthesisContext& context) override
			{
				if (out_.trace_frozen)
					return;
				try
				{
					budget bounds(out_, limits_);
					bounds.work();
					if (out_.active_frames.empty())
					{
						out_.trace_partial = true;
						return;
					}
					auto& value = out_.frames[out_.active_frames.back()];
					if (value.kind != "synthesis:" + std::to_string(context.Kind) ||
						value.source.begin !=
							source(context.InstantiationRange.isValid()
									   ? context.InstantiationRange
									   : clang::SourceRange(context.PointOfInstantiation))
								.begin)
					{
						out_.trace_partial = true;
						return;
					}
					value.ended = true;
					out_.active_frames.pop_back();
				}
				catch (const stopped&)
				{
					out_.trace_partial = true;
				}
			}

		  private:
			project_template_observations& out_;
			project_template_limits limits_;
		};

		class collector : public clang::RecursiveASTVisitor<collector>
		{
			using visitor = clang::RecursiveASTVisitor<collector>;

		  public:
			collector(clang::Sema& sema,
					  project_template_observations& out,
					  project_template_limits limits)
				: sema_(sema), out_(out), limits_(std::move(limits)), bounds_(out_, limits_)
			{
			}
			bool shouldVisitTemplateInstantiations() const
			{
				return true;
			}
			bool TraverseDecl(clang::Decl* value)
			{
				if (!value)
					return true;
				bounds_.depth(++depth_);
				const auto* previous = function_;
				const auto* previous_template = template_;
				if (const auto* owner = llvm::dyn_cast<clang::TemplateDecl>(value))
					template_ = owner;
				else if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(value))
				{
					if (function->getDescribedFunctionTemplate())
						template_ = function->getDescribedFunctionTemplate();
				}
				else if (const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(value))
				{
					if (record->getDescribedClassTemplate())
						template_ = record->getDescribedClassTemplate();
				}
				if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(value))
					function_ = function;
				const bool result = visitor::TraverseDecl(value);
				function_ = previous;
				template_ = previous_template;
				--depth_;
				return result;
			}
			bool TraverseStmt(clang::Stmt* value)
			{
				if (!value)
					return true;
				bounds_.depth(++depth_);
				const bool result = visitor::TraverseStmt(value);
				--depth_;
				return result;
			}
			bool VisitTemplateDecl(clang::TemplateDecl* value)
			{
				const auto* canonical = value->getCanonicalDecl();
				if (primary_.contains(canonical))
					return true;
				const auto index = subject("primary", value->getSourceRange(), value);
				bounds_.retain(sizeof(std::pair<const clang::Decl*, std::size_t>));
				primary_.emplace(canonical, index);
				auto* parameters = value->getTemplateParameters();
				if (parameters && parameters->getRequiresClause())
					subject("requires_clause",
							parameters->getRequiresClause()->getSourceRange(),
							value);
				llvm::SmallVector<clang::AssociatedConstraint, 4> constraints;
				value->getAssociatedConstraints(constraints);
				normalize(value, constraints);
				return true;
			}
			bool VisitConceptDecl(clang::ConceptDecl* value)
			{
				const clang::AssociatedConstraint constraint(value->getConstraintExpr());
				normalize(value, llvm::ArrayRef<clang::AssociatedConstraint>(constraint));
				return true;
			}
			bool VisitConceptSpecializationExpr(clang::ConceptSpecializationExpr* value)
			{
				bounds_.work();
				const auto index = subject("argument_use", value->getSourceRange(), nullptr);
				set_arguments(index, value->getTemplateArguments());
				connect_primary(index, value->getNamedConcept());
				return true;
			}
			bool VisitFunctionDecl(clang::FunctionDecl* value)
			{
				if (value->getTrailingRequiresClause())
					subject("requires_clause",
							value->getTrailingRequiresClause().ConstraintExpr->getSourceRange(),
							value);
				if (value->getTemplateSpecializationKind() != clang::TSK_Undeclared)
				{
					const auto index =
						value->getTemplateSpecializationKind() == clang::TSK_ImplicitInstantiation
						? implicit(value, value->getPointOfInstantiation())
						: subject("argument_use", value->getSourceRange(), value);
					const auto* info = value->getTemplateSpecializationInfo();
					if (info && info->TemplateArguments)
					{
						set_arguments(index, info->TemplateArguments->asArray());
						connect_primary(index, info->getTemplate());
					}
					else if (const auto* owner =
								 llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(
									 value->getDeclContext()))
					{
						set_arguments(index, owner->getTemplateArgs().asArray());
						connect_primary(index, owner->getSpecializedTemplate());
					}
					else
						out_.subjects[index].argument_state = "partial";
				}
				if (!value->getDescribedFunctionTemplate())
				{
					llvm::SmallVector<clang::AssociatedConstraint, 4> constraints;
					value->getAssociatedConstraints(constraints);
					normalize(value, constraints);
				}
				return true;
			}
			bool VisitClassTemplateSpecializationDecl(clang::ClassTemplateSpecializationDecl* value)
			{
				if (value->getSpecializationKind() != clang::TSK_Undeclared)
				{
					const auto index =
						value->getSpecializationKind() == clang::TSK_ImplicitInstantiation
						? implicit(value, value->getPointOfInstantiation())
						: subject("argument_use", value->getSourceRange(), value);
					set_arguments(index, value->getTemplateArgs().asArray());
					connect_primary(index, value->getSpecializedTemplate());
				}
				return true;
			}
			bool VisitVarTemplateSpecializationDecl(clang::VarTemplateSpecializationDecl* value)
			{
				if (value->getSpecializationKind() != clang::TSK_Undeclared)
				{
					const auto index =
						value->getSpecializationKind() == clang::TSK_ImplicitInstantiation
						? implicit(value, value->getPointOfInstantiation())
						: subject("argument_use", value->getSourceRange(), value);
					set_arguments(index, value->getTemplateArgs().asArray());
					connect_primary(index, value->getSpecializedTemplate());
				}
				return true;
			}
			bool VisitVarDecl(clang::VarDecl* value)
			{
				if (llvm::isa<clang::VarTemplateSpecializationDecl>(value) ||
					value->getTemplateSpecializationKind() != clang::TSK_ImplicitInstantiation)
					return true;
				if (const auto* info = value->getMemberSpecializationInfo())
				{
					const auto index = implicit(value, info->getPointOfInstantiation());
					if (const auto* owner = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(
							value->getDeclContext()))
					{
						set_arguments(index, owner->getTemplateArgs().asArray());
						connect_primary(index, owner->getSpecializedTemplate());
					}
					else
						out_.subjects[index].argument_state = "partial";
				}
				return true;
			}
			bool VisitCXXRecordDecl(clang::CXXRecordDecl* value)
			{
				if (llvm::isa<clang::ClassTemplateSpecializationDecl>(value) ||
					value->getTemplateSpecializationKind() != clang::TSK_ImplicitInstantiation)
					return true;
				if (const auto* info = value->getMemberSpecializationInfo())
				{
					const auto index = implicit(value, info->getPointOfInstantiation());
					if (const auto* owner = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(
							value->getDeclContext()))
					{
						set_arguments(index, owner->getTemplateArgs().asArray());
						connect_primary(index, owner->getSpecializedTemplate());
					}
					else
						out_.subjects[index].argument_state = "partial";
				}
				return true;
			}
			bool VisitTypeLoc(clang::TypeLoc value)
			{
				bounds_.work();
				if (value.isNull())
					return true;
				if (const auto written = value.getAs<clang::TemplateSpecializationTypeLoc>();
					!written.isNull())
				{
					const auto* specialization = written.getTypePtr();
					const auto* primary = specialization->getTemplateName().getAsTemplateDecl();
					const auto index = subject("argument_use", value.getSourceRange(), nullptr);
					set_arguments(index, specialization->template_arguments());
					if (primary)
						connect_primary(index, primary);
				}
				if (!value.getType()->isDependentType())
					return true;
				// Every written dependent type construction is an original
				// occurrence, including dependent qualified template names and
				// compound pointer/reference/array/function type expressions.
				const auto discriminator =
					"type-loc:" + std::to_string(static_cast<unsigned>(value.getTypeLocClass()));
				const auto index =
					subject("dependent_type_use", value.getSourceRange(), nullptr, discriminator);
				out_.subjects[index].dependent = true;
				return true;
			}
			bool VisitCallExpr(clang::CallExpr* value)
			{
				if (!value->getDirectCallee() &&
					(value->isTypeDependent() || value->isValueDependent()))
				{
					const auto index =
						subject("dependent_unresolved_call", value->getSourceRange(), nullptr);
					out_.subjects[index].dependent = true;
					out_.subjects[index].target_state = "partial";
				}
				return true;
			}
			bool VisitCXXUnresolvedConstructExpr(clang::CXXUnresolvedConstructExpr* value)
			{
				const auto index =
					subject("dependent_unresolved_call", value->getSourceRange(), nullptr);
				out_.subjects[index].dependent = true;
				out_.subjects[index].target_state = "partial";
				return true;
			}
			bool VisitRequiresExpr(clang::RequiresExpr* value)
			{
				subject("requires_expression", value->getSourceRange(), nullptr);
				return true;
			}
			bool VisitLambdaExpr(clang::LambdaExpr* value)
			{
				const auto index =
					subject("lambda", value->getSourceRange(), value->getLambdaClass());
				auto& initial = out_.subjects[index];
				initial.owner_usr = usr(function_, bounds_);
				initial.capture_state = "complete";
				const auto* record = value->getLambdaClass();
				llvm::DenseMap<const clang::ValueDecl*, clang::FieldDecl*> fields;
				clang::FieldDecl* this_field{};
				record->getCaptureFields(fields, this_field);
				const clang::ASTRecordLayout* layout{};
				if (record->isCompleteDefinition() && !record->isDependentContext() &&
					!record->isInvalidDecl())
				{
					layout = &sema_.Context.getASTRecordLayout(record);
					initial.closure_size_bits =
						static_cast<std::uint64_t>(layout->getSize().getQuantity()) *
						sema_.Context.getCharWidth();
				}
				std::size_t ordinal{};
				for (const auto& capture : value->captures())
				{
					bounds_.member(out_.captures.size(), sizeof(template_native_capture));
					template_native_capture item;
					item.lambda = index;
					item.index = ordinal++;
					item.source = source(clang::SourceRange(capture.getLocation()));
					const clang::FieldDecl* field{};
					if (capture.capturesThis())
					{
						field = this_field;
						item.kind =
							capture.getCaptureKind() == clang::LCK_StarThis ? "star_this" : "this";
					}
					else if (capture.capturesVariable())
					{
						const auto* variable = capture.getCapturedVar();
						item.captured_usr = usr(variable, bounds_);
						const auto found = fields.find(variable);
						if (found != fields.end())
							field = found->second;
						item.kind = capture.getCaptureKind() == clang::LCK_ByRef ? "by_reference"
																				 : "by_copy";
						if (value->isInitCapture(&capture))
							item.kind = "init_" + item.kind;
					}
					else
						item.kind = "vla_type";
					if (capture.isPackExpansion())
						item.kind += "_pack";
					if (layout && field && !field->getType()->isDependentType() &&
						!field->getType()->isIncompleteType() &&
						!field->getType()->isSizelessType())
					{
						item.field_index = field->getFieldIndex();
						item.offset_bits = layout->getFieldOffset(field->getFieldIndex());
						item.size_bits = sema_.Context.getTypeSize(field->getType());
					}
					else
					{
						item.complete = false;
						item.reason = "capture_storage_unavailable";
					}
					bounds_.retain(item.kind.size() + item.reason.size() + sizeof(std::size_t));
					out_.subjects[index].captures.push_back(out_.captures.size());
					out_.captures.push_back(std::move(item));
				}
				return true;
			}
			void finish()
			{
				for (const auto& [index, owner] : pending_owner_)
				{
					const auto found = primary_.find(owner->getCanonicalDecl());
					if (found != primary_.end())
						out_.subjects[index].owner = found->second;
					else
					{
						out_.subjects[index].complete = false;
						out_.subjects[index].reason = "template_owner_unavailable";
					}
				}
				for (const auto& [index, primary] : pending_primary_)
				{
					const auto found = primary_.find(primary->getCanonicalDecl());
					if (found != primary_.end())
						out_.subjects[index].primary = found->second;
					else
					{
						out_.subjects[index].complete = false;
						out_.subjects[index].reason = "primary_template_unavailable";
					}
				}
			}

		  private:
			clang::Sema& sema_;
			project_template_observations& out_;
			project_template_limits limits_;
			budget bounds_;
			std::size_t depth_{};
			const clang::FunctionDecl* function_{};
			const clang::TemplateDecl* template_{};
			std::map<const clang::Decl*, std::size_t> primary_;
			std::map<const clang::Decl*, std::size_t> implicit_;
			std::vector<std::pair<std::size_t, const clang::TemplateDecl*>> pending_primary_,
				pending_owner_;
			std::map<std::tuple<std::string, std::uint32_t, std::uint32_t, std::string>,
					 std::size_t>
				occurrences_;
			std::size_t subject(std::string_view kind,
								clang::SourceRange location,
								const clang::Decl* declaration,
								std::string_view discriminator = {})
			{
				const auto begin = location.getBegin().getRawEncoding(),
						   end = location.getEnd().getRawEncoding();
				const auto identity = usr(declaration, bounds_);
				bounds_.retain(discriminator.size());
				const auto occurrence_identity =
					discriminator.empty() ? identity : std::string{discriminator};
				bounds_.retain(
					sizeof(std::tuple<std::string, std::uint32_t, std::uint32_t, std::string>) +
					identity.size() + kind.size());
				auto key = std::tuple{std::string(kind), begin, end, occurrence_identity};
				if (kind != "constraint_root")
				{
					const auto found = occurrences_.find(key);
					if (found != occurrences_.end())
						return found->second;
				}
				bounds_.member(out_.subjects.size(), sizeof(template_native_subject));
				template_native_subject value;
				value.kind = bounds_.string(kind);
				value.occurrence_kind = bounds_.string(discriminator);
				value.source = source(location);
				if (kind == "primary" || kind == "constraint_root")
					value.source.role = template_native_source_role::declaration;
				value.semantic_usr = identity;
				value.definition_source = value.source;
				if (location.getBegin().isValid())
					value.is_system = sema_.getSourceManager().isInSystemHeader(
						sema_.getSourceManager().getExpansionLoc(location.getBegin()));
				const auto index = out_.subjects.size();
				out_.subjects.push_back(std::move(value));
				if (template_ && kind != "primary")
				{
					bounds_.retain(sizeof(std::pair<std::size_t, const clang::TemplateDecl*>));
					pending_owner_.emplace_back(index, template_);
				}
				occurrences_.insert_or_assign(std::move(key), index);
				return index;
			}
			std::size_t implicit(const clang::NamedDecl* declaration, clang::SourceLocation poi)
			{
				const auto* canonical = declaration->getCanonicalDecl();
				const auto found = implicit_.find(canonical);
				if (found != implicit_.end())
					return found->second;
				const auto index =
					subject("implicit_instance", clang::SourceRange(poi), declaration);
				auto& value = out_.subjects[index];
				value.definition_source = source(declaration->getSourceRange());
				value.definition_source.role = template_native_source_role::declaration;
				value.materialization_kind = "implicit_instantiation";
				value.dependent = false;
				bounds_.retain(sizeof(std::pair<const clang::Decl*, std::size_t>));
				implicit_.emplace(canonical, index);
				return index;
			}
			void set_arguments(std::size_t index, llvm::ArrayRef<clang::TemplateArgument> args)
			{
				const auto encoded = arguments(sema_.Context, bounds_).encode(args);
				auto& value = out_.subjects[index];
				value.argument_state = encoded ? "complete" : "partial";
				if (encoded)
					value.canonical_arguments = *encoded;
				else
					value.reason = "canonical_template_argument_unavailable";
			}
			void connect_primary(std::size_t index, const clang::TemplateDecl* primary)
			{
				bounds_.retain(sizeof(std::pair<std::size_t, const clang::TemplateDecl*>));
				pending_primary_.emplace_back(index, primary);
			}
			void normalize(const clang::NamedDecl* declaration,
						   llvm::ArrayRef<clang::AssociatedConstraint> constraints)
			{
				if (constraints.empty())
					return;
				const auto index =
					subject("constraint_root", declaration->getSourceRange(), declaration);
				const auto* root =
					sema_.getNormalizedAssociatedConstraints(declaration, constraints);
				if (!root)
				{
					out_.subjects[index].normalization_state = "partial";
					out_.subjects[index].reason = "compiler_constraint_normalization_failed";
					out_.constraints_partial = true;
					return;
				}
				out_.subjects[index].normalization_state = "complete";
				out_.subjects[index].constraint_root = constraint(*root, index, "0", 1);
			}
			std::size_t constraint(const clang::NormalizedConstraint& value,
								   std::size_t owner,
								   std::string path,
								   std::uint64_t depth)
			{
				bounds_.depth(depth);
				bounds_.member(out_.constraints.size(), sizeof(template_native_constraint));
				template_native_constraint node;
				node.root_subject = owner;
				node.path = bounds_.string(path);
				node.depth = depth;
				node.mapping_state = "complete";
				node.source = source(value.getSourceRange());
				const auto index = out_.constraints.size();
				out_.constraints.push_back(std::move(node));
				auto child = [&](const clang::NormalizedConstraint& item, unsigned ordinal)
				{
					const auto member =
						constraint(item, owner, path + "." + std::to_string(ordinal), depth + 1);
					bounds_.retain(sizeof(std::size_t));
					out_.constraints[index].children.push_back(member);
				};
				switch (value.getKind())
				{
					case clang::NormalizedConstraint::ConstraintKind::Compound:
					{
						const auto& compound = static_cast<const clang::CompoundConstraint&>(value);
						out_.constraints[index].kind = compound.getCompoundKind() ==
								clang::NormalizedConstraint::CCK_Conjunction
							? "conjunction"
							: "disjunction";
						child(compound.getLHS(), 0);
						child(compound.getRHS(), 1);
						break;
					}
					case clang::NormalizedConstraint::ConstraintKind::ConceptId:
						out_.constraints[index].kind = "concept";
						child(static_cast<const clang::ConceptIdConstraint&>(value)
								  .getNormalizedConstraint(),
							  0);
						break;
					case clang::NormalizedConstraint::ConstraintKind::FoldExpanded:
					{
						const auto& fold = static_cast<const clang::FoldExpandedConstraint&>(value);
						out_.constraints[index].kind = fold.getFoldOperator() ==
								clang::NormalizedConstraint::FoldOperatorKind::And
							? "fold_and"
							: "fold_or";
						child(fold.getNormalizedPattern(), 0);
						break;
					}
					case clang::NormalizedConstraint::ConstraintKind::Atomic:
						out_.constraints[index].kind = "atomic";
						break;
				}
				if (value.getKind() != clang::NormalizedConstraint::ConstraintKind::Compound)
				{
					const auto& mapped =
						static_cast<const clang::NormalizedConstraintWithParamMapping&>(value);
					if (mapped.hasParameterMapping())
					{
						llvm::SmallVector<clang::TemplateArgument, 8> args;
						for (const auto& argument : mapped.getParameterMapping())
						{
							bounds_.retain(sizeof(clang::TemplateArgument));
							args.push_back(argument.getArgument());
						}
						const auto encoded = arguments(sema_.Context, bounds_).encode(args);
						if (encoded)
						{
							auto& bytes = out_.constraints[index].parameter_mapping;
							bytes = *encoded;
							auto append = [&](std::string_view part)
							{
								const auto size = std::to_string(part.size());
								bounds_.retain(size.size() + 1 + part.size());
								bytes += size;
								bytes += ':';
								bytes.append(part);
							};
							const auto& occurrences = mapped.mappingOccurenceList();
							append(std::to_string(occurrences.size()));
							for (unsigned position = 0; position < occurrences.size(); ++position)
							{
								bounds_.work();
								append(occurrences[position] ? "1" : "0");
							}
							const auto pack = mapped.getPackSubstitutionIndex();
							append(pack ? std::to_string(*pack) : "none");
						}
						else
						{
							out_.constraints[index].mapping_state = "partial";
							out_.constraints[index].reason =
								"canonical_parameter_mapping_unavailable";
						}
					}
				}
				return index;
			}
		};
#endif
	} // namespace

	void install_project_template_observer(clang::Sema& sema,
										   project_template_observations& out,
										   project_template_limits limits)
	{
#if defined(CXXLENS_HAS_CLANG22) && CXXLENS_HAS_CLANG22
		if (out.trace_installed)
		{
			out.trace_partial = true;
			return;
		}
		out.trace_installed = true;
		// ParseAST initializes its existing callback chain before Parser::Initialize
		// invokes SemaConsumer::InitializeSema. Initialize this newly installed
		// observer here, still before the first parsed source declaration.
		auto callback = std::make_unique<trace>(out, std::move(limits));
		callback->initialize(sema);
		sema.TemplateInstCallbacks.push_back(std::move(callback));
#else
		(void)sema;
		(void)limits;
		out.trace_partial = true;
#endif
	}
	sdk::result<void> observe_project_templates(clang::Sema& sema,
												project_template_observations& out,
												project_template_limits limits)
	{
#if defined(CXXLENS_HAS_CLANG22) && CXXLENS_HAS_CLANG22
		out.trace_frozen = true;
		if (sema.getDiagnostics().hasErrorOccurred())
			out.subjects_partial = out.constraints_partial = out.captures_partial =
				out.trace_partial = true;
		if (!out.trace_installed || !out.trace_initialised || !out.active_frames.empty())
			out.trace_partial = true;
		try
		{
			collector value(sema, out, std::move(limits));
			if (!value.TraverseDecl(sema.Context.getTranslationUnitDecl()))
				return sdk::error{"native.template-unavailable", "AST", "traversal"};
			value.finish();
			return {};
		}
		catch (const stopped& reason)
		{
			out.subjects_partial = out.constraints_partial = out.captures_partial = true;
			return reason.error;
		}
		catch (const std::bad_alloc&)
		{
			out.subjects_partial = out.constraints_partial = out.captures_partial = true;
			return sdk::error{"native.template-limit", "template", "allocation"};
		}
#else
		(void)sema;
		(void)out;
		(void)limits;
		return sdk::error{"native.template-unsupported", "compiler", "clang22-unavailable"};
#endif
	}

	sdk::result<std::vector<sdk::detached_row>>
	detach_project_templates(const project_template_observations& observations,
							 const project_template_bindings& bindings,
							 project_template_limits limits)
	{
		try
		{
			project_template_observations counters;
			counters.operations = observations.operations;
			counters.retained_bytes = observations.retained_bytes;
			budget bounds(counters, limits);
			if (bindings.compile_unit.empty())
				return sdk::error{"native.template-binding-invalid", "compile_unit", "missing"};
			const auto descriptor = [&](std::string_view name) -> const sdk::relation_descriptor&
			{
				for (const auto& value : sdk::standard_relation_descriptors())
				{
					bounds.work();
					if (value.id == name)
						return value;
				}
				throw stopped{
					{"native.template-unsupported", std::string(name), "descriptor-unregistered"}};
			};
			const auto& subjects = descriptor("cc.template_subject.v1");
			const auto& constraints = descriptor("cc.constraint_node.v1");
			const auto& captures = descriptor("cc.lambda_capture.v1");
			const auto& frames = descriptor("cc.template_instantiation_frame.v1");
			const auto& inventory = descriptor("cc.template_inventory.v1");
			using fields = std::map<std::string, sdk::detached_cell, std::less<>>;
			const auto make =
				[&](const sdk::relation_descriptor& relation, fields values, bool validate = true)
			{
				bounds.retain(sizeof(sdk::detached_row) +
							  values.size() * sizeof(sdk::detached_cell));
				sdk::detached_row row{relation.id, {}};
				for (const auto& column : relation.columns)
				{
					bounds.work();
					const auto found = values.find(column.name);
					if (found != values.end())
					{
						auto value = std::move(found->second);
						value.type = column.type;
						row.cells.emplace(column.id, std::move(value));
					}
					else if (column.type.optional)
						row.cells.emplace(column.id, sdk::detached_cell::absent(column.type));
				}
				const auto key = relation.column(*relation.domain_identity.result_column);
				if (!key)
					throw stopped{key.error()};
				const auto identity = sdk::derive_domain_identity(relation, row);
				if (!identity)
					throw stopped{identity.error()};
				bounds.retain(identity->size());
				row.cells.insert_or_assign(
					key->id, sdk::detached_cell::typed(key->type.parameter, *identity));
				if (validate)
				{
					const auto valid = sdk::validate_row(relation, row);
					if (!valid)
						throw stopped{valid.error()};
				}
				return row;
			};
			const auto key = [&](const sdk::relation_descriptor& relation, fields values)
			{
				return make(relation, std::move(values), false);
			};
			const auto identity =
				[](const sdk::detached_row& row, const sdk::relation_descriptor& relation)
			{
				return std::get<std::string>(
					*row.cells.at(*relation.domain_identity.result_column).value);
			};
			const auto text = [&](std::string_view value)
			{
				return sdk::detached_cell::utf8(bounds.string(value));
			};
			const auto bytes = [&](std::string_view value)
			{
				bounds.retain(value.size());
				std::vector<std::byte> out;
				out.reserve(value.size());
				for (char byte : value)
					out.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
				return sdk::detached_cell::bytes(std::move(out));
			};
			const auto list = [&](const std::vector<std::string>& values)
			{
				std::vector<std::byte> out;
				for (const auto& value : values)
				{
					bounds.work();
					if (value.size() > std::numeric_limits<std::uint32_t>::max())
						throw stopped{{"native.template-limit", "IDs", "length"}};
					bounds.retain(4 + value.size());
					const auto size = static_cast<std::uint32_t>(value.size());
					for (unsigned shift = 0; shift < 32; shift += 8)
						out.push_back(static_cast<std::byte>((size >> shift) & 255U));
					for (char byte : value)
						out.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
				}
				return sdk::detached_cell{{sdk::scalar_kind::set, {}, false},
										  sdk::cell_state::present,
										  sdk::scalar_value{std::move(out)},
										  std::nullopt};
			};
			const auto bind_source =
				[&](fields& values, std::string_view column, template_native_source location)
			{
				if (bindings.source && location.begin)
				{
					bounds.work();
					const auto value = bindings.source(location);
					if (!value.empty())
						values.emplace(column, text(value));
				}
			};
			const auto bind_entity =
				[&](fields& values, std::string_view column, std::string_view original)
			{
				if (bindings.entity && !original.empty())
				{
					bounds.work();
					const auto value = bindings.entity(original);
					if (!value.empty())
						values.emplace(column, text(value));
				}
			};
			const auto n = observations.subjects.size(), c = observations.constraints.size(),
					   l = observations.captures.size(), f = observations.frames.size();
			for (const auto size : {n, c, l, f})
				if (size > limits.maximum_members)
					throw stopped{{"native.template-limit", "members", "count"}};
			bounds.retain((n + c + l + f) * sizeof(std::string));
			std::vector<std::string> subject_ids(n), constraint_ids(c), capture_ids(l),
				frame_ids(f);
			// Derive all original keys first; payload references cannot alter stable IDs.
			for (std::size_t i = 0; i < n; ++i)
				subject_ids[i] =
					identity(key(subjects,
								 {{"compile_unit", text(bindings.compile_unit)},
								  {"kind", text(observations.subjects[i].kind)},
								  {"ordinal", sdk::detached_cell::unsigned_integer(i)}}),
							 subjects);
			for (std::size_t i = 0; i < c; ++i)
			{
				const auto& value = observations.constraints[i];
				if (value.root_subject >= n)
					throw stopped{{"native.template-binding-invalid", "root_subject", "index"}};
				constraint_ids[i] =
					identity(key(constraints,
								 {{"compile_unit", text(bindings.compile_unit)},
								  {"root_subject", text(subject_ids[value.root_subject])},
								  {"path", text(value.path)}}),
							 constraints);
			}
			for (std::size_t i = 0; i < l; ++i)
			{
				const auto& value = observations.captures[i];
				if (value.lambda >= n)
					throw stopped{{"native.template-binding-invalid", "lambda", "index"}};
				capture_ids[i] =
					identity(key(captures,
								 {{"compile_unit", text(bindings.compile_unit)},
								  {"lambda", text(subject_ids[value.lambda])},
								  {"index", sdk::detached_cell::unsigned_integer(value.index)}}),
							 captures);
			}
			for (std::size_t i = 0; i < f; ++i)
				frame_ids[i] = identity(key(frames,
											{{"compile_unit", text(bindings.compile_unit)},
											 {"ordinal", sdk::detached_cell::unsigned_integer(i)}}),
										frames);
			std::vector<sdk::detached_row> result;
			bounds.retain((n + c + l + f + 1) * sizeof(sdk::detached_row));
			result.reserve(n + c + l + f + 1);
			for (std::size_t i = 0; i < n; ++i)
			{
				const auto& value = observations.subjects[i];
				fields row{{"compile_unit", text(bindings.compile_unit)},
						   {"kind", text(value.kind)},
						   {"ordinal", sdk::detached_cell::unsigned_integer(i)},
						   {"profile", text("clang22-original-template-domains/2")},
						   {"observation_state", text(value.complete ? "complete" : "partial")}};
				bind_source(row, "source", value.source);
				bind_source(row, "definition_source", value.definition_source);
				if (!value.occurrence_kind.empty())
					row.emplace("occurrence_kind", text(value.occurrence_kind));
				bind_entity(row, "entity", value.semantic_usr);
				bind_entity(row, "owner_entity", value.owner_usr);
				if (!value.semantic_usr.empty())
					row.emplace("semantic_usr", bytes(value.semantic_usr));
				const auto reference = [&](std::string_view column,
										   std::optional<std::size_t> index,
										   const std::vector<std::string>& ids)
				{
					if (index)
					{
						if (*index >= ids.size())
							throw stopped{
								{"native.template-binding-invalid", std::string(column), "index"}};
						row.emplace(column, text(ids[*index]));
					}
				};
				reference("primary", value.primary, subject_ids);
				reference("owner", value.owner, subject_ids);
				reference("constraint_root", value.constraint_root, constraint_ids);
				if (value.is_system)
					row.emplace("is_system", sdk::detached_cell::boolean(*value.is_system));
				if (value.dependent)
					row.emplace("dependent", sdk::detached_cell::boolean(*value.dependent));
				for (const auto& [column, payload] :
					 std::initializer_list<std::pair<std::string_view, std::string_view>>{
						 {"materialization_kind", value.materialization_kind},
						 {"target_state", value.target_state},
						 {"argument_state", value.argument_state},
						 {"normalization_state", value.normalization_state},
						 {"capture_state", value.capture_state},
						 {"reason", value.reason}})
					if (!payload.empty())
						row.emplace(column, text(payload));
				if (!value.argument_state.empty())
				{
					row.emplace("argument_profile",
								text("clang22-canonical-template-argument-tuple/1"));
					if (value.argument_state == "complete")
						row.emplace("canonical_arguments", bytes(value.canonical_arguments));
				}
				if (value.kind == "lambda")
				{
					std::vector<std::string> ids;
					bounds.retain(value.captures.size() * sizeof(std::string));
					for (const auto member : value.captures)
					{
						if (member >= l)
							throw stopped{
								{"native.template-binding-invalid", "capture_ids", "index"}};
						bounds.retain(capture_ids[member].size());
						ids.push_back(capture_ids[member]);
					}
					std::ranges::sort(ids);
					row.emplace("capture_count", sdk::detached_cell::unsigned_integer(ids.size()));
					row.emplace("capture_ids", list(ids));
					if (value.closure_size_bits)
						row.emplace("closure_size_bits",
									sdk::detached_cell::unsigned_integer(*value.closure_size_bits));
				}
				result.push_back(make(subjects, std::move(row)));
			}
			for (const auto& value : observations.constraints)
			{
				fields row{
					{"compile_unit", text(bindings.compile_unit)},
					{"root_subject", text(subject_ids[value.root_subject])},
					{"path", text(value.path)},
					{"kind", text(value.kind)},
					{"depth", sdk::detached_cell::unsigned_integer(value.depth)},
					{"profile", text("clang22-sema-associated-constraint-normalization/1")},
					{"child_count", sdk::detached_cell::unsigned_integer(value.children.size())},
					{"mapping_state", text(value.mapping_state)},
					{"observation_state", text(value.complete ? "complete" : "partial")}};
				bind_source(row, "source", value.source);
				if (!value.parameter_mapping.empty())
					row.emplace("parameter_mapping", bytes(value.parameter_mapping));
				if (!value.reason.empty())
					row.emplace("reason", text(value.reason));
				if (value.children.size() > 2)
					throw stopped{{"native.template-binding-invalid", "children", "arity"}};
				for (std::size_t j = 0; j < value.children.size(); ++j)
				{
					if (value.children[j] >= c)
						throw stopped{{"native.template-binding-invalid", "children", "index"}};
					row.emplace(j == 0 ? "left_child" : "right_child",
								text(constraint_ids[value.children[j]]));
				}
				result.push_back(make(constraints, std::move(row)));
			}
			for (const auto& value : observations.captures)
			{
				fields row{{"compile_unit", text(bindings.compile_unit)},
						   {"lambda", text(subject_ids[value.lambda])},
						   {"index", sdk::detached_cell::unsigned_integer(value.index)},
						   {"kind", text(value.kind)},
						   {"profile", text("clang22-lambda-capture-storage/1")},
						   {"observation_state", text(value.complete ? "complete" : "partial")}};
				bind_source(row, "source", value.source);
				bind_entity(row, "captured_entity", value.captured_usr);
				if (!value.captured_usr.empty())
					row.emplace("captured_usr", bytes(value.captured_usr));
				for (const auto& [column, number] : std::initializer_list<
						 std::pair<std::string_view, std::optional<std::uint64_t>>>{
						 {"field_index", value.field_index},
						 {"offset_bits", value.offset_bits},
						 {"size_bits", value.size_bits}})
					if (number)
						row.emplace(column, sdk::detached_cell::unsigned_integer(*number));
				if (!value.reason.empty())
					row.emplace("reason", text(value.reason));
				result.push_back(make(captures, std::move(row)));
			}
			for (const auto& value : observations.frames)
			{
				fields row{
					{"compile_unit", text(bindings.compile_unit)},
					{"ordinal", sdk::detached_cell::unsigned_integer(value.ordinal)},
					{"kind", text(value.kind)},
					{"is_instantiation", sdk::detached_cell::boolean(value.is_instantiation)},
					{"depth", sdk::detached_cell::unsigned_integer(value.depth)},
					{"profile", text("clang22-original-template-instantiation-stack/1")},
					{"completion_state", text(value.ended ? "ended" : "partial")},
					{"observation_state", text(value.complete ? "complete" : "partial")}};
				bind_source(row, "source", value.source);
				if (value.parent)
				{
					if (*value.parent >= f)
						throw stopped{{"native.template-binding-invalid", "parent", "index"}};
					row.emplace("parent", text(frame_ids[*value.parent]));
				}
				if (!value.entity_usr.empty())
					row.emplace("entity_usr", bytes(value.entity_usr));
				if (!value.template_usr.empty())
					row.emplace("template_usr", bytes(value.template_usr));
				if (!value.argument_state.empty())
				{
					row.emplace("argument_profile",
								text("clang22-canonical-template-argument-tuple/1"));
					row.emplace("argument_state", text(value.argument_state));
					if (value.argument_state == "complete")
						row.emplace("canonical_arguments", bytes(value.canonical_arguments));
				}
				if (!value.reason.empty())
					row.emplace("reason", text(value.reason));
				result.push_back(make(frames, std::move(row)));
			}
			fields population{{"compile_unit", text(bindings.compile_unit)},
							  {"profile", text("clang22-original-template-domains/2")}};
			for (const auto& [prefix, ids, partial] : std::initializer_list<
					 std::tuple<std::string_view, const std::vector<std::string>*, bool>>{
					 {"subject", &subject_ids, observations.subjects_partial},
					 {"constraint", &constraint_ids, observations.constraints_partial},
					 {"capture", &capture_ids, observations.captures_partial},
					 {"frame", &frame_ids, observations.trace_partial}})
			{
				bounds.retain(ids->size() * sizeof(std::string));
				for (const auto& id : *ids)
					bounds.retain(id.size());
				auto sorted = *ids;
				std::ranges::sort(sorted);
				population.emplace(std::string(prefix) + "_count",
								   sdk::detached_cell::unsigned_integer(sorted.size()));
				population.emplace(std::string(prefix) + "_ids", list(sorted));
				population.emplace(std::string(prefix) + "_state",
								   text(partial ? "partial" : "complete"));
			}
			result.push_back(make(inventory, std::move(population)));
			return result;
		}
		catch (const stopped& reason)
		{
			return reason.error;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::error{"native.template-limit", "detachment", "allocation"};
		}
		catch (const std::length_error&)
		{
			return sdk::error{"native.template-limit", "detachment", "length"};
		}
	}
} // namespace cxxlens::detail::clang22
