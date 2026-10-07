#include "original_resource_inputs.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

#include <clang/AST/Attr.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/Stmt.h>
#include <llvm/ADT/DenseMap.h>

namespace cxxlens::detail::clang22::resource_inputs
{
	namespace
	{
		struct budget
		{
			limits bound;
			std::size_t operations{}, bytes{}, fields{};
			std::optional<sdk::error> failure;
			explicit budget(limits original_limits) : bound(std::move(original_limits)) {}
			bool charge(std::size_t work = 1U, std::size_t retained = 0U)
			{
				if (failure)
					return false;
				if (bound.cancelled && bound.cancelled())
				{
					failure = sdk::error{"native.resource-input-cancelled", "inputs", "cancelled"};
					return false;
				}
				if (work >
						bound.maximum_operations - std::min(operations, bound.maximum_operations) ||
					retained > bound.maximum_retained_bytes -
							std::min(bytes, bound.maximum_retained_bytes))
				{
					failure =
						sdk::error{"native.resource-input-budget", "inputs", "resource-limit"};
					return false;
				}
				operations += work;
				bytes += retained;
				return true;
			}
			bool text(std::string_view v)
			{
				if (v.size() > bound.maximum_text_bytes)
				{
					failure = sdk::error{"native.resource-input-budget", "text", "resource-limit"};
					return false;
				}
				return charge(v.size() + 1U, v.size() * 2U + sizeof(std::string));
			}
			bool field()
			{
				if (fields >= bound.maximum_fields)
				{
					failure =
						sdk::error{"native.resource-input-budget", "fields", "resource-limit"};
					return false;
				}
				++fields;
				return charge(1U, 256U);
			}
		};
		template <class T>
		bool append(std::vector<T>& target, T value, budget& b)
		{
			if (!b.field() || !b.charge(1U, sizeof(T) * 2U))
				return false;
			target.push_back(std::move(value));
			return true;
		}
		const clang::Expr* transparent(const clang::Expr* e, budget& b, bool value_casts = true)
		{
			for (std::size_t depth = 0; e && depth < b.bound.maximum_wrapper_depth; ++depth)
			{
				if (!b.charge())
					return nullptr;
				if (auto* paren = llvm::dyn_cast<clang::ParenExpr>(e))
					e = paren->getSubExpr();
				else if (auto* cleanup = llvm::dyn_cast<clang::ExprWithCleanups>(e))
					e = cleanup->getSubExpr();
				else if (auto* materialization = llvm::dyn_cast<clang::MaterializeTemporaryExpr>(e))
					e = materialization->getSubExpr();
				else if (auto* binding = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(e))
					e = binding->getSubExpr();
				else if (auto* cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e); cast &&
						 value_casts &&
						 (cast->getCastKind() == clang::CK_NoOp ||
						  cast->getCastKind() == clang::CK_LValueToRValue))
					e = cast->getSubExpr();
				else
					return e;
			}
			// Budget exhaustion aborts admission; it cannot become a known empty carrier.
			if (e && !b.failure)
				b.failure =
					sdk::error{"native.resource-input-budget", "wrapper-depth", "resource-limit"};
			return nullptr;
		}
		const clang::Decl* direct_reference(const clang::Expr* e, budget& b)
		{
			e = transparent(e, b);
			if (auto* v = llvm::dyn_cast_or_null<clang::DeclRefExpr>(e))
				return v->getDecl();
			if (auto* v = llvm::dyn_cast_or_null<clang::MemberExpr>(e))
				return v->getMemberDecl();
			return nullptr;
		}
		void storage(address_observation& row, const clang::Decl* destination)
		{
			const auto* v = llvm::dyn_cast_or_null<clang::VarDecl>(destination);
			if (v)
			{
				switch (v->getStorageDuration())
				{
					case clang::SD_Automatic:
						row.storage_duration = "automatic";
						break;
					case clang::SD_Thread:
						row.storage_duration = "thread";
						break;
					case clang::SD_Static:
						row.storage_duration = "static";
						break;
					case clang::SD_FullExpression:
						row.storage_duration = "full_expression";
						break;
					case clang::SD_Dynamic:
						row.storage_duration = "dynamic";
						break;
				}
				row.context =
					v->getDeclContext()->isFileContext() ? "global" : *row.storage_duration;
			}
			else if (llvm::isa_and_nonnull<clang::FieldDecl>(destination))
				row.context = "field";
			if (const auto* n = llvm::dyn_cast_or_null<clang::NamedDecl>(destination))
			{
				// Original declaration linkage, independent from duration/context.
				switch (n->getFormalLinkage())
				{
					case clang::Linkage::Invalid:
						row.linkage = "invalid";
						break;
					case clang::Linkage::None:
						row.linkage = "none";
						break;
					case clang::Linkage::Internal:
						row.linkage = "internal";
						break;
					case clang::Linkage::UniqueExternal:
						row.linkage = "unique_external";
						break;
					case clang::Linkage::VisibleNone:
						row.linkage = "visible_none";
						break;
					case clang::Linkage::Module:
						row.linkage = "module";
						break;
					case clang::Linkage::External:
						row.linkage = "external";
						break;
				}
			}
		}
		address_observation transfer(const clang::Expr* source,
									 const clang::Decl* destination,
									 const clang::Decl* owner,
									 std::string_view kind,
									 budget& b)
		{
			address_observation r;
			r.owner = owner;
			r.source_expression = source;
			r.destination_declaration = destination;
			r.kind = kind;
			r.context = "unknown";
			r.origin_state = "not_classified";
			r.destination_state = destination ? "bound" : "unbound";
			if (source)
			{
				r.source_type = source->getType();
				r.referenced_declaration = direct_reference(source, b);
			}
			if (auto* value = llvm::dyn_cast_or_null<clang::ValueDecl>(destination))
				r.destination_type = value->getType();
			storage(r, destination);
			return r;
		}
		void reference_origin(address_observation& r, budget& b)
		{
			// This operation is called only for a compiler-bound reference initialization,
			// or the direct operand of unary &, never for plain pointer propagation.
			r.origin_expression = transparent(r.source_expression, b);
			r.origin_declaration = direct_reference(r.origin_expression, b);
			r.origin_state = r.origin_expression ? "observed" : "unbound";
		}
		const clang::Decl*
		initializer_destination(const clang::Expr& original, site_context context, budget& b)
		{
			const clang::Expr* root{};
			const clang::Decl* destination{};
			if (const auto* v =
					llvm::dyn_cast_or_null<clang::VarDecl>(context.initializing_declaration))
			{
				root = v->getInit();
				destination = v;
			}
			else if (const auto* f =
						 llvm::dyn_cast_or_null<clang::FieldDecl>(context.initializing_declaration);
					 f && f->hasInClassInitializer())
			{
				root = f->getInClassInitializer();
				destination = f;
			}
			if (root && transparent(root, b, false) == &original)
				return destination;
			if (context.member_initializer && context.member_initializer->isMemberInitializer() &&
				transparent(context.member_initializer->getInit(), b, false) == &original)
				return context.member_initializer->getMember();
			return nullptr;
		}
		void observe_move(const clang::Stmt& statement,
						  site_context context,
						  observations& out,
						  budget& b)
		{
			move_observation r;
			r.owner = context.owner;
			r.destination_state = "unbound";
			if (const auto* c = llvm::dyn_cast<clang::CXXConstructExpr>(&statement))
			{
				r.site = c;
				r.selected = c->getConstructor();
				r.kind = !r.selected						   ? "unresolved_constructor"
					: c->getConstructor()->isMoveConstructor() ? "move_constructor"
					: c->getConstructor()->isCopyConstructor() ? "copy_constructor"
															   : "other_constructor";
				r.elidable = c->isElidable();
				r.destination_declaration = initializer_destination(*c, context, b);
				if (r.kind == "move_constructor" || r.kind == "copy_constructor")
				{
					if (c->getNumArgs())
						r.source_actual = c->getArg(0);
					if (r.selected->getNumParams())
						r.source_formal = r.selected->getParamDecl(0);
				}
			}
			else if (const auto* unresolved =
						 llvm::dyn_cast<clang::CXXUnresolvedConstructExpr>(&statement))
			{
				r.site = unresolved;
				r.kind = "unresolved_constructor";
			}
			else if (const auto* call = llvm::dyn_cast<clang::CallExpr>(&statement))
			{
				const auto* method =
					llvm::dyn_cast_or_null<clang::CXXMethodDecl>(call->getDirectCallee());
				const auto* op = llvm::dyn_cast<clang::CXXOperatorCallExpr>(call);
				if ((!op || op->getOperator() != clang::OO_Equal) &&
					(!method || method->getOverloadedOperator() != clang::OO_Equal))
					return;
				r.site = call;
				r.selected = method;
				r.kind = !method						 ? "unresolved_assignment"
					: method->isMoveAssignmentOperator() ? "move_assignment"
					: method->isCopyAssignmentOperator() ? "copy_assignment"
														 : "other_assignment";
				if (op)
				{
					if (op->getNumArgs())
						r.destination_expression = op->getArg(0);
					if (op->getNumArgs() > 1U)
						r.source_actual = op->getArg(1);
				}
				else if (const auto* member = llvm::dyn_cast<clang::CXXMemberCallExpr>(call))
				{
					r.destination_expression = member->getImplicitObjectArgument();
					if (member->getNumArgs())
						r.source_actual = member->getArg(0);
				}
				const unsigned source_parameter =
					method && method->isExplicitObjectMemberFunction() ? 1U : 0U;
				if (!op && method && method->isExplicitObjectMemberFunction())
				{
					if (call->getNumArgs())
						r.destination_expression = call->getArg(0);
					if (call->getNumArgs() > 1U)
						r.source_actual = call->getArg(1);
				}
				if (method && method->getNumParams() > source_parameter)
					r.source_formal = method->getParamDecl(source_parameter);
				r.destination_declaration = direct_reference(r.destination_expression, b);
			}
			else
				return;
			r.classification_state = r.selected ? "observed" : "unresolved";
			if (r.destination_expression || r.destination_declaration)
				r.destination_state = "bound";
			append(out.moves, r, b);
		}
		sdk::result<observations> finish(observations value, budget& b)
		{
			if (b.failure)
				return sdk::unexpected(*b.failure);
			value.operations = b.operations;
			value.retained_bytes_bound = b.bytes;
			return value;
		}
	} // namespace
	sdk::result<observations> observe_declaration(const clang::Decl& declaration, limits bound)
	{
		budget b{std::move(bound)};
		observations out;
		out.admitted_declaration = &declaration;
		out.attributes_admitted = out.addresses_admitted = true;
		if (!b.charge())
			return finish(std::move(out), b);
		unsigned ordinal = 0U;
		for (const auto* a : declaration.attrs())
		{
			if (!b.charge())
				break;
			attribute_observation r;
			r.declaration = &declaration;
			r.attribute = a;
			r.ordinal = ordinal++;
			r.original_kind = static_cast<unsigned>(a->getKind());
			if (const char* s = a->getSpelling())
				r.spelling = s;
			r.implicit = a->isImplicit();
			r.inherited = a->isInherited();
			if (!b.text(r.spelling))
				break;
			if (const auto* ownership = llvm::dyn_cast<clang::OwnershipAttr>(a))
			{
				r.argument_state = "complete";
				r.ownership_kind = ownership->isTakes() ? "takes"
					: ownership->isHolds()				? "holds"
														: "returns";
				if (ownership->getModule())
				{
					const auto s = ownership->getModule()->getName();
					r.module = std::string_view{s.data(), s.size()};
					if (!b.text(*r.module))
						break;
				}
				const auto* f = llvm::dyn_cast<clang::FunctionDecl>(&declaration);
				const auto* method = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(f);
				unsigned index = 0U;
				for (const auto& arg : ownership->args())
				{
					attribute_argument v;
					v.ordinal = index++;
					v.kind = "parameter_index";
					v.binding_state = "unresolved";
					if (arg.isValid())
					{
						v.source_index = arg.getSourceIndex();
						const bool has_this = method && method->isImplicitObjectMemberFunction();
						v.implicit_this = has_this && *v.source_index == 1U;
						if (!*v.implicit_this)
						{
							v.ast_index = arg.getASTIndex();
							if (f && *v.ast_index < f->getNumParams())
							{
								v.formal = f->getParamDecl(*v.ast_index);
								v.binding_state = "bound";
							}
						}
						else
							v.binding_state = "implicit_this";
					}
					if (!append(r.arguments, v, b))
						break;
				}
			}
			else if (const auto* annotation = llvm::dyn_cast<clang::AnnotateAttr>(a))
			{
				r.argument_state = "complete";
				const auto s = annotation->getAnnotation();
				r.annotation = std::string_view{s.data(), s.size()};
				if (!b.text(*r.annotation))
					break;
				unsigned index = 0U;
				for (const auto* e : annotation->args())
				{
					if (!append(r.arguments,
								attribute_argument{index++,
												   "annotation_expression",
												   "observed",
												   {},
												   {},
												   {},
												   nullptr,
												   e},
								b))
						break;
				}
				for (const auto* e : annotation->delayedArgs())
				{
					if (!append(r.arguments,
								attribute_argument{index++,
												   "annotation_delayed_expression",
												   "unresolved",
												   {},
												   {},
												   {},
												   nullptr,
												   e},
								b))
						break;
				}
			}
			if (!append(out.attributes, std::move(r), b))
				break;
		}
		const clang::Expr* init{};
		clang::QualType type;
		if (const auto* v = llvm::dyn_cast<clang::VarDecl>(&declaration))
		{
			init = v->getInit();
			type = v->getType();
		}
		else if (const auto* f = llvm::dyn_cast<clang::FieldDecl>(&declaration))
		{
			if (f->hasInClassInitializer())
				init = f->getInClassInitializer();
			type = f->getType();
		}
		if (init)
		{
			auto r = transfer(init,
							  &declaration,
							  &declaration,
							  type->isReferenceType() ? "reference_bind" : "initializer",
							  b);
			r.carrier_declaration = &declaration;
			if (type->isReferenceType())
				reference_origin(r, b);
			append(out.addresses, r, b);
		}
		return finish(std::move(out), b);
	}
	sdk::result<observations>
	observe_statement(const clang::Stmt& statement, site_context context, limits bound)
	{
		const auto* owner = context.owner;
		budget b{std::move(bound)};
		observations out;
		out.admitted_syntax = &statement;
		out.moves_admitted = out.addresses_admitted = true;
		if (!b.charge())
			return finish(std::move(out), b);
		observe_move(statement, context, out, b);
		if (const auto* u = llvm::dyn_cast<clang::UnaryOperator>(&statement);
			u && u->getOpcode() == clang::UO_AddrOf)
		{
			auto r = transfer(u->getSubExpr(), nullptr, owner, "address_of", b);
			r.carrier_syntax = u;
			r.destination_expression = u;
			r.destination_type = u->getType();
			r.destination_state = "bound";
			reference_origin(r, b);
			append(out.addresses, r, b);
		}
		else if (const auto* cast = llvm::dyn_cast<clang::ImplicitCastExpr>(&statement); cast &&
				 (cast->getCastKind() == clang::CK_ArrayToPointerDecay ||
				  cast->getCastKind() == clang::CK_FunctionToPointerDecay))
		{
			auto r =
				transfer(cast->getSubExpr(),
						 nullptr,
						 owner,
						 cast->getCastKind() == clang::CK_ArrayToPointerDecay ? "array_decay"
																			  : "function_decay",
						 b);
			r.carrier_syntax = cast;
			r.destination_expression = cast;
			r.destination_type = cast->getType();
			r.destination_state = "bound";
			reference_origin(r, b);
			append(out.addresses, r, b);
		}
		else if (const auto* self = llvm::dyn_cast<clang::CXXThisExpr>(&statement))
		{
			auto r = transfer(self, nullptr, owner, "this_address", b);
			r.carrier_syntax = self;
			r.origin_expression = self;
			r.origin_state = "observed";
			r.destination_expression = self;
			r.destination_type = self->getType();
			r.destination_state = "bound";
			append(out.addresses, r, b);
		}
		else if (const auto* assign = llvm::dyn_cast<clang::BinaryOperator>(&statement);
				 assign && assign->isAssignmentOp())
		{
			auto r = transfer(
				assign->getRHS(), direct_reference(assign->getLHS(), b), owner, "assignment", b);
			r.carrier_syntax = assign;
			r.destination_expression = assign->getLHS();
			r.destination_type = assign->getLHS()->getType();
			r.destination_state = "bound";
			append(out.addresses, r, b);
		}
		else if (const auto* ret = llvm::dyn_cast<clang::ReturnStmt>(&statement))
		{
			auto r = transfer(ret->getRetValue(), nullptr, owner, "return", b);
			r.carrier_syntax = ret;
			r.context = "return";
			r.destination_declaration = llvm::dyn_cast_or_null<clang::FunctionDecl>(owner);
			r.destination_state = r.destination_declaration ? "bound_return_slot" : "unbound";
			if (const auto* f = llvm::dyn_cast_or_null<clang::FunctionDecl>(owner))
			{
				r.destination_type = f->getReturnType();
				if (r.destination_type->isReferenceType())
					reference_origin(r, b);
			}
			append(out.addresses, r, b);
		}
		else if (const auto* lambda = llvm::dyn_cast<clang::LambdaExpr>(&statement))
		{
			// Budget the original capture population before Clang constructs its exact map.
			if (!b.charge(lambda->capture_size(), lambda->capture_size() * 128U))
				return finish(std::move(out), b);
			llvm::DenseMap<const clang::ValueDecl*, clang::FieldDecl*> fields;
			clang::FieldDecl* this_field{};
			lambda->getLambdaClass()->getCaptureFields(fields, this_field);
			auto init = lambda->capture_init_begin();
			unsigned ordinal = 0U;
			for (const auto& capture : lambda->captures())
			{
				const clang::Expr* source = *init++;
				const clang::Decl* destination{};
				if (capture.capturesVariable())
				{
					const auto at = fields.find(capture.getCapturedVar());
					if (at != fields.end())
						destination = at->second;
				}
				else if (capture.capturesThis())
					destination = this_field;
				auto r = transfer(source, destination, owner, "capture", b);
				r.carrier_syntax = lambda;
				r.endpoint_ordinal = ordinal++;
				r.context = "capture";
				r.capture_implicit = capture.isImplicit();
				switch (capture.getCaptureKind())
				{
					case clang::LCK_This:
						r.capture_kind = "this";
						break;
					case clang::LCK_StarThis:
						r.capture_kind = "star_this";
						break;
					case clang::LCK_ByCopy:
						r.capture_kind = "by_copy";
						break;
					case clang::LCK_ByRef:
						r.capture_kind = "by_ref";
						reference_origin(r, b);
						break;
					case clang::LCK_VLAType:
						r.capture_kind = "vla_type";
						break;
				}
				if (!append(out.addresses, r, b))
					break;
			}
		}
		// An exact original constructor initializer context binds its direct child.
		if (context.member_initializer && context.member_initializer->isMemberInitializer() &&
			context.member_initializer->getInit() == &statement)
		{
			auto* field = context.member_initializer->getMember();
			auto r = transfer(
				context.member_initializer->getInit(), field, owner, "member_initializer", b);
			r.carrier_syntax = &statement;
			if (field->getType()->isReferenceType())
				reference_origin(r, b);
			r.endpoint_ordinal = static_cast<unsigned>(out.addresses.size());
			append(out.addresses, r, b);
		}
		// operator= actual operands are independently useful even for copying/other calls.
		for (const auto& move : out.moves)
		{
			if (move.kind == "move_constructor" || move.kind == "copy_constructor" ||
				move.kind == "other_constructor" || move.kind == "unresolved_constructor")
				continue;
			auto r =
				transfer(move.source_actual, move.destination_declaration, owner, "assignment", b);
			r.carrier_syntax = move.site;
			r.endpoint_ordinal = static_cast<unsigned>(out.addresses.size());
			r.destination_expression = move.destination_expression;
			r.destination_state = move.destination_state;
			if (r.destination_expression)
				r.destination_type = r.destination_expression->getType();
			append(out.addresses, r, b);
		}
		return finish(std::move(out), b);
	}

	namespace
	{
		struct writer
		{
			const bindings& bind;
			budget b;
			detached_inputs out;
			const sdk::relation_descriptor* descriptor(std::string_view name)
			{
				if (!b.charge())
					return nullptr;
				const auto* d = bind.descriptor ? bind.descriptor(name) : nullptr;
				if (!d || d->name != name)
				{
					b.failure = sdk::error{"native.resource-input-descriptor",
										   std::string{name},
										   "authoritative-descriptor-unavailable"};
					return nullptr;
				}
				return d;
			}
			sdk::detached_row row(const sdk::relation_descriptor& d)
			{
				sdk::detached_row r{d.id, {}};
				if (!b.text(d.id) || !b.charge(1U, sizeof(sdk::detached_row) * 2U))
					return r;
				for (const auto& c : d.columns)
				{
					if (!b.field() || !b.text(c.id))
						break;
					r.cells.emplace(c.id,
									c.type.optional ? sdk::detached_cell::absent(c.type)
													: sdk::detached_cell::unknown(
														  c.type, "original-fact-unavailable"));
				}
				return r;
			}
			const sdk::column_descriptor* column(const sdk::relation_descriptor& d,
												 std::string_view name)
			{
				if (!b.charge(d.columns.size() + 1U))
					return nullptr;
				for (const auto& c : d.columns)
					if (c.name == name)
						return &c;
				b.failure = sdk::error{"native.resource-input-descriptor",
									   std::string{name},
									   "descriptor-column-missing"};
				return nullptr;
			}
			void put(const sdk::relation_descriptor& d,
					 sdk::detached_row& row,
					 std::string_view name,
					 sdk::detached_cell value)
			{
				if (b.failure)
					return;
				const auto* c = column(d, name);
				if (!c || !b.charge(1U, 256U))
					return;
				value.type = c->type;
				row.cells.insert_or_assign(c->id, std::move(value));
			}
			void unknown(const sdk::relation_descriptor& d,
						 sdk::detached_row& row,
						 std::string_view name,
						 std::string_view reason = "original-binding-unavailable")
			{
				const auto* c = column(d, name);
				if (c)
					put(d, row, name, sdk::detached_cell::unknown(c->type, std::string{reason}));
			}
			void string(const sdk::relation_descriptor& d,
						sdk::detached_row& row,
						std::string_view name,
						std::string_view value,
						bool empty_valid = false)
			{
				if (value.empty() && !empty_valid)
				{
					unknown(d, row, name);
					return;
				}
				if (b.text(value))
					put(d, row, name, sdk::detached_cell::utf8(std::string{value}));
			}
			void number(const sdk::relation_descriptor& d,
						sdk::detached_row& row,
						std::string_view name,
						std::size_t value)
			{
				put(d, row, name, sdk::detached_cell::unsigned_integer(value));
			}
			void boolean(const sdk::relation_descriptor& d,
						 sdk::detached_row& row,
						 std::string_view name,
						 bool value)
			{
				put(d, row, name, sdk::detached_cell::boolean(value));
			}
			void bytes(const sdk::relation_descriptor& d,
					   sdk::detached_row& row,
					   std::string_view name,
					   std::string_view value)
			{
				if (!b.text(value))
					return;
				const auto* start = reinterpret_cast<const std::byte*>(value.data());
				std::vector<std::byte> copied;
				if (!value.empty())
					copied.assign(start, start + value.size());
				put(d, row, name, sdk::detached_cell::bytes(std::move(copied)));
			}
			void list(const sdk::relation_descriptor& d,
					  sdk::detached_row& row,
					  std::string_view name,
					  std::vector<std::string> ids)
			{
				if (!b.charge(ids.size() * 2U + 1U, ids.size() * sizeof(std::string) * 2U))
					return;
				std::ranges::sort(ids);
				if (std::ranges::adjacent_find(ids) != ids.end())
				{
					b.failure = sdk::error{"native.resource-input-binding",
										   std::string{name},
										   "duplicate-original-occurrence"};
					return;
				}
				std::vector<std::byte> encoded;
				for (const auto& id : ids)
				{
					if (!b.text(id) || id.size() > std::numeric_limits<std::uint32_t>::max() ||
						!b.charge(id.size() + 4U, (id.size() + 4U) * 2U))
						return;
					const auto size = static_cast<std::uint32_t>(id.size());
					for (unsigned i = 0; i < 4U; ++i)
						encoded.push_back(static_cast<std::byte>((size >> (8U * i)) & 0xffU));
					for (char c : id)
						encoded.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
				}
				put(d, row, name, sdk::detached_cell::bytes(std::move(encoded)));
			}
			void decl(const sdk::relation_descriptor& d,
					  sdk::detached_row& row,
					  std::string_view name,
					  const clang::Decl* value)
			{
				string(d,
					   row,
					   name,
					   value && bind.declaration ? bind.declaration(*value) : std::string_view{});
			}
			void syntax(const sdk::relation_descriptor& d,
						sdk::detached_row& row,
						std::string_view name,
						const clang::Stmt* value)
			{
				string(
					d, row, name, value && bind.syntax ? bind.syntax(*value) : std::string_view{});
			}
			void type(const sdk::relation_descriptor& d,
					  sdk::detached_row& row,
					  std::string_view name,
					  clang::QualType value)
			{
				string(d,
					   row,
					   name,
					   !value.isNull() && bind.canonical_type ? bind.canonical_type(value)
															  : std::string_view{});
			}
			std::string identity(const sdk::relation_descriptor& d, sdk::detached_row& row)
			{
				if (b.failure)
					return {};
				if (!d.domain_identity.result_column)
				{
					b.failure = sdk::error{
						"native.resource-input-descriptor", d.name, "original-domain-unavailable"};
					return {};
				}
				// Budget canonical encoding's bounded value copies before calling SDK authority.
				for (const auto& [key, cell] : row.cells)
				{
					if (!b.text(key))
						return {};
					if (cell.value)
					{
						if (const auto* v = std::get_if<std::string>(&*cell.value);
							v && !b.text(*v))
							return {};
						if (const auto* v = std::get_if<std::vector<std::byte>>(&*cell.value);
							v && !b.charge(v->size() + 1U, v->size() * 2U))
							return {};
					}
				}
				auto id = sdk::derive_domain_identity(d, row);
				if (!id)
				{
					b.failure = id.error();
					return {};
				}
				const auto result_column = d.column(*d.domain_identity.result_column);
				if (!result_column)
				{
					b.failure = result_column.error();
					return {};
				}
				string(d, row, result_column->name, *id);
				return b.failure ? std::string{} : std::move(*id);
			}
			void retain(const sdk::relation_descriptor& d, sdk::detached_row row)
			{
				if (b.failure)
					return;
				if (!b.charge(row.cells.size() + 1U, 256U))
					return;
				auto validation = sdk::validate_row(d, row);
				if (!validation)
				{
					b.failure = validation.error();
					return;
				}
				append(out.rows, std::move(row), b);
			}
			carrier_summary summary(const observations& input, std::string_view profile)
			{
				carrier_summary r;
				if (input.admitted_declaration && bind.declaration)
					r.declaration = bind.declaration(*input.admitted_declaration);
				if (input.admitted_syntax && bind.syntax)
					r.syntax = bind.syntax(*input.admitted_syntax);
				r.profile = profile;
				b.text(r.declaration);
				b.text(r.syntax);
				if (r.declaration.empty() && r.syntax.empty())
					b.failure = sdk::error{"native.resource-input-binding",
										   "carrier",
										   "original-admission-carrier-unavailable"};
				return r;
			}
			void add_id(carrier_summary& summary, std::string id)
			{
				if (b.text(id))
					append(summary.ids, std::move(id), b);
			}
		};
	} // namespace
	sdk::result<detached_inputs>
	detach(const observations& input, const bindings& bind, limits bound)
	{
		writer w{bind, budget{std::move(bound)}, {}};
		if (!w.b.charge(input.operations, input.retained_bytes_bound))
			return sdk::unexpected(*w.b.failure);
		auto invalid = [](std::string field) -> sdk::result<detached_inputs>
		{
			return sdk::error{"native.resource-input-observation",
							  std::move(field),
							  "inconsistent-original-occurrence"};
		};
		if (input.attributes_admitted && !input.admitted_declaration)
			return invalid("attribute-carrier");
		if (input.moves_admitted && !input.admitted_syntax)
			return invalid("move-carrier");
		for (std::size_t i = 0; i < input.attributes.size(); ++i)
		{
			const auto& a = input.attributes[i];
			if (!w.b.charge(a.arguments.size() + 1U))
				return *w.b.failure;
			if (!input.attributes_admitted || a.declaration != input.admitted_declaration ||
				a.ordinal != i)
				return invalid("attribute-ordinal");
			if (a.argument_state != "complete" && !a.arguments.empty())
				return invalid("argument-state");
			for (std::size_t j = 0; j < a.arguments.size(); ++j)
				if (a.arguments[j].ordinal != j)
					return invalid("argument-ordinal");
		}
		for (const auto& a : input.moves)
		{
			if (!w.b.charge())
				return *w.b.failure;
			if (!input.moves_admitted || !a.site || a.site != input.admitted_syntax)
				return invalid("move-carrier");
		}
		for (std::size_t i = 0; i < input.addresses.size(); ++i)
		{
			if (!w.b.charge())
				return *w.b.failure;
			const auto& a = input.addresses[i];
			if (!input.addresses_admitted || a.endpoint_ordinal != i ||
				(a.carrier_declaration
					 ? a.carrier_declaration != input.admitted_declaration
					 : !a.carrier_syntax || a.carrier_syntax != input.admitted_syntax))
				return invalid("address-ordinal");
		}
		if (bind.compile_unit.empty())
			return sdk::unexpected(sdk::error{
				"native.resource-input-binding", "compile_unit", "original-unit-unavailable"});
		if (input.attributes_admitted)
		{
			auto census = w.summary(input, attribute_profile);
			const auto* d = w.descriptor("cc.declaration_attribute");
			if (!d)
				return sdk::unexpected(*w.b.failure);
			for (const auto& a : input.attributes)
			{
				auto carrier = w.row(*d);
				auto common = [&](sdk::detached_row& row, std::size_t slot, std::string_view kind)
				{
					w.string(*d, row, "compile_unit", bind.compile_unit);
					w.decl(*d, row, "declaration", a.declaration);
					w.number(*d, row, "attribute_ordinal", a.ordinal);
					w.number(*d, row, "argument_slot", slot);
					w.string(*d, row, "profile", attribute_profile);
					w.string(*d, row, "record_kind", kind);
					w.number(*d, row, "attribute_kind", a.original_kind);
					w.boolean(*d, row, "is_implicit", a.implicit);
					w.boolean(*d, row, "is_inherited", a.inherited);
					w.string(*d, row, "argument_state", a.argument_state);
					if (a.argument_state == "complete")
						w.number(*d, row, "argument_count", a.arguments.size());
				};
				common(carrier, 0U, "carrier");
				if (!a.spelling.empty())
					w.string(*d, carrier, "attribute_spelling", a.spelling);
				w.string(*d,
						 carrier,
						 "source",
						 a.attribute && bind.attribute_source ? bind.attribute_source(*a.attribute)
															  : std::string_view{});
				if (!a.ownership_kind.empty())
				{
					w.string(*d, carrier, "ownership_kind", a.ownership_kind);
					if (a.module)
						w.bytes(*d, carrier, "ownership_module", *a.module);
					else
						w.unknown(*d, carrier, "ownership_module");
				}
				if (a.annotation)
					w.bytes(*d, carrier, "annotation_payload", *a.annotation);
				auto carrier_id = w.identity(*d, carrier);
				std::vector<std::string> argument_ids;
				if (a.argument_state != "complete")
					census.argument_inventory_complete = false;
				++census.count;
				w.add_id(census, carrier_id);
				for (const auto& arg : a.arguments)
				{
					auto row = w.row(*d);
					common(row, static_cast<std::size_t>(arg.ordinal) + 1U, "argument");
					w.string(*d, row, "carrier_attribute", carrier_id);
					w.number(*d, row, "argument_ordinal", arg.ordinal);
					w.string(*d, row, "argument_kind", arg.kind);
					w.string(*d, row, "argument_binding_state", arg.binding_state);
					if (arg.source_index)
						w.number(*d, row, "source_parameter_index", *arg.source_index);
					else if (arg.kind == "parameter_index")
						w.unknown(*d, row, "source_parameter_index");
					if (arg.ast_index)
						w.number(*d, row, "ast_parameter_index", *arg.ast_index);
					else if (arg.kind == "parameter_index" &&
							 (!arg.implicit_this || !*arg.implicit_this))
						w.unknown(*d, row, "ast_parameter_index");
					if (arg.implicit_this)
						w.boolean(*d, row, "is_implicit_this", *arg.implicit_this);
					if (arg.kind == "parameter_index" &&
						(!arg.implicit_this || !*arg.implicit_this))
						w.decl(*d, row, "formal_declaration", arg.formal);
					if (arg.kind != "parameter_index")
						w.syntax(*d, row, "argument_expression", arg.expression);
					auto argument_id = w.identity(*d, row);
					if (w.b.text(argument_id))
						append(argument_ids, std::move(argument_id), w.b);
					w.retain(*d, std::move(row));
					++census.argument_count;
				}
				if (a.argument_state == "complete")
					w.list(*d, carrier, "argument_ids", std::move(argument_ids));
				w.retain(*d, std::move(carrier));
			}
			append(w.out.summaries, std::move(census), w.b);
		}
		if (input.moves_admitted)
		{
			auto census = w.summary(input, move_profile);
			const auto* d = w.descriptor("cc.move_event");
			if (!d)
				return sdk::unexpected(*w.b.failure);
			for (const auto& a : input.moves)
			{
				auto row = w.row(*d);
				w.string(*d, row, "compile_unit", bind.compile_unit);
				w.syntax(*d, row, "syntax", a.site);
				w.string(*d, row, "profile", move_profile);
				w.string(*d, row, "kind", a.kind);
				w.string(*d, row, "classification_state", a.classification_state);
				w.string(*d, row, "destination_state", a.destination_state);
				w.decl(*d, row, "owner_declaration", a.owner);
				w.string(*d,
						 row,
						 "source",
						 a.site && bind.source ? bind.source(*a.site) : std::string_view{});
				w.decl(*d, row, "selected_declaration", a.selected);
				w.string(*d,
						 row,
						 "selected_entity",
						 a.selected && bind.entity ? bind.entity(*a.selected) : std::string_view{});
				w.syntax(*d, row, "source_actual", a.source_actual);
				w.decl(*d, row, "source_formal", a.source_formal);
				if (a.destination_expression)
					w.syntax(*d, row, "destination_expression", a.destination_expression);
				w.decl(*d, row, "destination_declaration", a.destination_declaration);
				if (a.elidable)
					w.boolean(*d, row, "is_elidable", *a.elidable);
				w.boolean(*d, row, "is_type_dependent", a.site && a.site->isTypeDependent());
				w.boolean(*d, row, "is_value_dependent", a.site && a.site->isValueDependent());
				if (a.site && bind.potentially_evaluated)
				{
					if (const auto evaluated = bind.potentially_evaluated(*a.site))
						w.boolean(*d, row, "potentially_evaluated", *evaluated);
					else
						w.unknown(*d, row, "potentially_evaluated");
				}
				else
					w.unknown(*d, row, "potentially_evaluated");
				auto id = w.identity(*d, row);
				w.retain(*d, std::move(row));
				++census.count;
				w.add_id(census, std::move(id));
			}
			append(w.out.summaries, std::move(census), w.b);
		}
		if (input.addresses_admitted)
		{
			auto census = w.summary(input, address_profile);
			const auto* d = w.descriptor("cc.address_transfer");
			if (!d)
				return sdk::unexpected(*w.b.failure);
			for (const auto& a : input.addresses)
			{
				const auto decl_id = a.carrier_declaration && bind.declaration
					? bind.declaration(*a.carrier_declaration)
					: std::string_view{};
				const auto syntax_id = a.carrier_syntax && bind.syntax
					? bind.syntax(*a.carrier_syntax)
					: std::string_view{};
				if ((a.carrier_declaration && a.carrier_syntax) ||
					(decl_id.empty() && syntax_id.empty()))
					return sdk::unexpected(sdk::error{"native.resource-input-binding",
													  "address-carrier",
													  "exact-original-carrier-unavailable"});
				auto row = w.row(*d);
				w.string(*d, row, "compile_unit", bind.compile_unit);
				w.string(*d, row, "carrier_kind", a.carrier_declaration ? "declaration" : "syntax");
				if (a.carrier_declaration)
					w.string(*d, row, "carrier_declaration", decl_id);
				else
					w.string(*d, row, "carrier_syntax", syntax_id);
				w.number(*d, row, "endpoint_ordinal", a.endpoint_ordinal);
				w.string(*d, row, "profile", address_profile);
				w.string(*d, row, "kind", a.kind);
				w.string(*d, row, "context", a.context);
				w.string(*d, row, "origin_state", a.origin_state);
				w.string(*d, row, "destination_state", a.destination_state);
				w.decl(*d, row, "owner_declaration", a.owner);
				w.string(*d,
						 row,
						 "source",
						 a.carrier_syntax && bind.source ? bind.source(*a.carrier_syntax)
							 : a.carrier_declaration && bind.declaration_source
							 ? bind.declaration_source(*a.carrier_declaration)
							 : std::string_view{});
				w.syntax(*d, row, "source_expression", a.source_expression);
				if (a.origin_state != "not_classified")
				{
					w.syntax(*d, row, "origin_expression", a.origin_expression);
					w.decl(*d, row, "origin_declaration", a.origin_declaration);
				}
				if (a.destination_expression)
					w.syntax(*d, row, "destination_expression", a.destination_expression);
				w.decl(*d, row, "destination_declaration", a.destination_declaration);
				w.decl(*d, row, "referenced_declaration", a.referenced_declaration);
				w.type(*d, row, "source_type", a.source_type);
				w.type(*d, row, "destination_type", a.destination_type);
				if (a.storage_duration)
					w.string(*d, row, "storage_duration", *a.storage_duration);
				if (a.linkage)
					w.string(*d, row, "linkage", *a.linkage);
				if (a.capture_kind)
					w.string(*d, row, "capture_kind", *a.capture_kind);
				if (a.capture_implicit)
					w.boolean(*d, row, "capture_implicit", *a.capture_implicit);
				auto id = w.identity(*d, row);
				w.retain(*d, std::move(row));
				++census.count;
				w.add_id(census, std::move(id));
			}
			append(w.out.summaries, std::move(census), w.b);
		}
		if (w.b.failure)
			return sdk::unexpected(*w.b.failure);
		w.out.operations = w.b.operations;
		w.out.retained_bytes_bound = w.b.bytes;
		return std::move(w.out);
	}
} // namespace cxxlens::detail::clang22::resource_inputs
