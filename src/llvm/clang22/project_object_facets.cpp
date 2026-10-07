#include "project_object_facets.hpp"

#include <clang/AST/DeclCXX.h>
#include <clang/AST/ExprCXX.h>
#include <cxxlens/relations/cc_body.hpp>
#include <cxxlens/relations/cc_cfg_edge.hpp>
#include <cxxlens/relations/cc_entity_detail.hpp>
#include <cxxlens/relations/cc_syntax_node.hpp>
#include <cxxlens/relations/cc_type.hpp>
namespace cxxlens::detail::clang22::object_semantics
{
	namespace
	{
		using sdk::detached_cell;
		struct writer
		{
			object_facet_limits limits;
			detached_object_facets out;
			std::optional<sdk::error> failure;
			std::size_t fields{};
			bool charge(std::size_t work = 1U, std::size_t bytes = 0U)
			{
				if (failure)
					return false;
				if (limits.cancelled && limits.cancelled())
				{
					failure = sdk::error{"native.object-cancelled", "facets", "cancelled"};
					return false;
				}
				if (out.operations > limits.maximum_operations ||
					work > limits.maximum_operations - out.operations ||
					out.retained_bytes_bound > limits.maximum_retained_bytes ||
					bytes > limits.maximum_retained_bytes - out.retained_bytes_bound)
				{
					failure = sdk::error{"native.object-budget", "facets", "resource-limit"};
					return false;
				}
				out.operations += work;
				out.retained_bytes_bound += bytes;
				return true;
			}
			bool text(std::string_view value)
			{
				if (value.size() > limits.maximum_text_bytes)
				{
					failure = sdk::error{"native.object-budget", "text", "resource-limit"};
					return false;
				}
				return charge(value.size() + 1U, value.size() + sizeof(std::string) * 2U);
			}
			object_facet_update* update(std::string_view relation,
										std::string_view target,
										std::string_view declaration,
										std::string_view source = {})
			{
				// Missing bindings remain unavailable to the SDK. Do not invent a row key.
				if (target.empty())
					return nullptr;
				if (!text(relation) || !text(target) || !text(declaration) || !text(source) ||
					!charge(1U, sizeof(object_facet_update) * 2U + 128U))
					return nullptr;
				out.updates.push_back({std::string{relation},
									   std::string{target},
									   std::string{declaration},
									   std::string{source},
									   {}});
				return &out.updates.back();
			}
			const sdk::relation_descriptor& descriptor(const object_facet_update& row)
			{
				namespace r = cxxlens::cc::relations;
				if (row.relation == "cc.entity_detail.v1")
					return r::entity_detail::descriptor();
				if (row.relation == "cc.body.v1")
					return r::body::descriptor();
				if (row.relation == "cc.cfg_edge.v1")
					return r::cfg_edge::descriptor();
				if (row.relation == "cc.type.v1")
					return r::type::descriptor();
				return r::syntax_node::descriptor();
			}
			template <class Value>
			void put(object_facet_update* row, std::string_view name, const Value& value)
			{
				if (!row || failure)
					return;
				if (fields == limits.maximum_fields || !text(name) || !charge(1U, 256U))
				{
					if (!failure)
						failure = sdk::error{"native.object-budget", "fields", "resource-limit"};
					return;
				}
				++fields;
				const auto& columns = descriptor(*row).columns;
				const auto at = std::ranges::find(columns, name, &sdk::column_descriptor::name);
				if (at == columns.end())
				{
					failure =
						sdk::error{"native.object-input", "field", "descriptor-column-missing"};
					return;
				}
				detached_cell cell;
				if constexpr (std::same_as<Value, std::string_view>)
				{
					if (value.empty())
						cell = detached_cell::unknown(at->type, "original-binding-unavailable");
					else
					{
						if (!text(value))
							return;
						cell = detached_cell::utf8(std::string{value});
					}
				}
				else if constexpr (std::same_as<Value, bool>)
					cell = detached_cell::boolean(value);
				else
					cell = detached_cell::unsigned_integer(value);
				cell.type = at->type;
				row->fields.emplace(std::string{name}, std::move(cell));
			}
			void optional(object_facet_update* row,
						  std::string_view name,
						  const std::optional<bool>& value)
			{
				if (value)
					put(row, name, *value); // Absence stays unobserved; never default false.
			}
			sdk::result<detached_object_facets> finish()
			{
				if (failure)
					return *failure;
				return std::move(out);
			}
		};
		std::string_view declaration(const object_facet_bindings& b, const clang::Decl& d)
		{
			return b.declaration ? b.declaration(d) : std::string_view{};
		}
		std::string_view syntax(const object_facet_bindings& b, const clang::Stmt& d)
		{
			return b.syntax ? b.syntax(d) : std::string_view{};
		}
		std::string_view type(const object_facet_bindings& b, clang::QualType q)
		{
			return b.canonical_type && !q.isNull() ? b.canonical_type(q) : std::string_view{};
		}
		std::string_view entity(const object_facet_bindings& b, const clang::NamedDecl& d)
		{
			return b.entity ? b.entity(d) : std::string_view{};
		}
		std::string_view state_name(state s)
		{
			return s == state::complete	  ? "complete"
				: s == state::conflicting ? "conflicting"
										  : "partial";
		}
		object_facet_update* value_base(writer& w,
										const clang::Expr& e,
										const clang::FunctionDecl& owner,
										const object_facet_bindings& b,
										std::string_view kind,
										std::string_view profile,
										state state_value)
		{
			auto* row = w.update("cc.syntax_node.v1", syntax(b, e), declaration(b, owner));
			w.put(row, "object_scope_declaration", declaration(b, owner));
			w.put(row, "object_fact_kind", kind);
			w.put(row, "object_fact_profile", profile);
			w.put(row, "object_fact_state", state_name(state_value));
			w.put(row, "canonical_type", type(b, e.getType()));
			return row;
		}
		void original_operand(writer& w,
							  object_facet_update* row,
							  const clang::CastExpr& e,
							  const object_facet_bindings& b)
		{
			const auto* input = e.getSubExpr();
			if (!input)
				return;
			w.put(row, "operand", syntax(b, *input));
			w.put(row, "operand_type", type(b, input->getType()));
		}
		void range(writer& w,
				   object_facet_update* row,
				   std::string_view lower,
				   std::string_view upper,
				   const std::optional<integer_range>& value)
		{
			if (value)
			{
				w.put(row, lower, std::string_view{value->lower});
				w.put(row, upper, std::string_view{value->upper});
			}
		}
		void object(writer& w,
					object_facet_update* row,
					const clang::VarDecl* d,
					const clang::Expr* address,
					const object_facet_bindings& b,
					const owned_object_state& current)
		{
			if (!d)
				return;
			w.put(row, "object_declaration", declaration(b, *d));
			w.put(row, "object_entity", entity(b, *d));
			w.put(row, "declared_object_type", type(b, d->getType()));
			if (address)
				w.put(row, "object_address", syntax(b, *address));
			w.put(row,
				  "current_object_profile",
				  std::string_view{"clang22-owned-object-initialization-before-address/1"});
			w.put(row,
				  "current_object_state",
				  std::string_view{current.live_unreplaced() ? "live_unreplaced" : "unknown"});
		}
	} // namespace
	sdk::result<detached_object_facets> detach_function_exit_facets(const clang::FunctionDecl& f,
																	const exit_projection& facts,
																	const object_facet_bindings& b,
																	object_facet_limits limits)
	{
		writer w{std::move(limits), {}, std::nullopt, 0U};
		const auto physical = declaration(b, f);
		const auto source = b.declaration_source ? b.declaration_source(f) : std::string_view{};
		auto* detail = w.update("cc.entity_detail.v1", entity(b, f), physical, source);
		w.put(
			detail, "function_exit_profile", std::string_view{"clang22-original-function-exits/1"});
		w.put(detail,
			  "function_exit_state",
			  std::string_view{facts.enumeration_complete ? "complete" : "partial"});
		// Function roles exist even with a finite empty exit-edge population.
		const auto role = llvm::isa<clang::CXXConstructorDecl>(f) ? "constructor"
			: llvm::isa<clang::CXXDestructorDecl>(f)			  ? "destructor"
			: f.getReturnType()->isDependentType()				  ? "unknown"
			: f.getReturnType()->isVoidType()					  ? "void"
			: f.getReturnType()->isReferenceType()				  ? "reference"
																  : "value";
		w.put(detail, "return_kind", std::string_view{role});
		w.put(detail, "is_main", f.isMain());
		const auto* coroutine = llvm::dyn_cast<clang::CoroutineBodyStmt>(f.getBody());
		w.put(detail, "is_coroutine", coroutine != nullptr);
		if (coroutine)
			w.put(
				detail, "coroutine_has_return_void", coroutine->getFallthroughHandler() != nullptr);
		auto* body = w.update("cc.body.v1", b.body, physical);
		w.put(body, "function_exit_declaration", physical);
		for (const auto& member : facts.members)
		{
			if (!w.charge())
				break;
			const auto edge = b.cfg_edge ? b.cfg_edge(static_cast<unsigned>(member.predecessor),
													  static_cast<unsigned>(member.exit))
										 : std::string_view{};
			auto* row = w.update("cc.cfg_edge.v1", edge, physical);
			w.put(row, "function_exit_declaration", physical);
			constexpr std::array<std::string_view, 6U> names{"fallthrough",
															 "return_value",
															 "exceptional",
															 "no_return",
															 "unreachable",
															 "unknown"};
			w.put(row, "function_exit_kind", names[static_cast<std::size_t>(member.facts.exits)]);
			w.put(row,
				  "function_exit_profile",
				  std::string_view{"clang22-original-function-exits/1"});
			w.put(row, "function_exit_state", state_name(member.facts.exit_binding));
		}
		return w.finish();
	}
	sdk::result<detached_object_facets> detach_enum_value_facets(const clang::CastExpr& e,
																 const enum_value& facts,
																 const clang::FunctionDecl& owner,
																 const object_facet_bindings& b,
																 object_facet_limits limits)
	{
		writer w{std::move(limits), {}, std::nullopt, 0U};
		auto* row = value_base(
			w, e, owner, b, "enum_cast", "clang22-original-enum-cast/1", facts.enumeration_binding);
		original_operand(w, row, e, b);
		range(w, row, "original_value_lower", "original_value_upper", facts.original_value);
		auto* domain = w.update("cc.type.v1", type(b, e.getType()), declaration(b, owner));
		w.optional(domain, "enum_fixed_underlying", facts.fixed_underlying);
		w.put(
			domain, "enum_value_profile", std::string_view{"clang22-original-enum-value-domain/1"});
		w.put(domain, "enum_value_state", state_name(facts.enumeration_binding));
		range(w, domain, "enum_value_lower", "enum_value_upper", facts.compiler_value_domain);
		return w.finish();
	}
	sdk::result<detached_object_facets> detach_bool_value_facets(const clang::CastExpr& e,
																 const bool_representation& facts,
																 const clang::FunctionDecl& owner,
																 const object_facet_bindings& b,
																 object_facet_limits limits)
	{
		writer w{std::move(limits), {}, std::nullopt, 0U};
		constexpr std::array<std::string_view, 4U> names{"bool_representation_load",
														 "bool_bit_cast",
														 "bool_integral_conversion",
														 "bool_unknown"};
		auto* row = value_base(w,
							   e,
							   owner,
							   b,
							   names[static_cast<std::size_t>(facts.operation)],
							   "clang22-original-bool-representation/1",
							   facts.representation_binding);
		original_operand(w, row, e, b);
		w.optional(row, "canonical_bool", facts.canonical_bool);
		w.put(row, "representation_state", state_name(facts.representation_binding));
		range(
			w, row, "original_value_lower", "original_value_upper", facts.original_representation);
		return w.finish();
	}
	sdk::result<detached_object_facets> detach_direct_type_facets(const clang::Expr& e,
																  const direct_type_access& facts,
																  const owned_object_state& current,
																  const clang::FunctionDecl& owner,
																  const object_facet_bindings& b,
																  object_facet_limits limits)
	{
		writer w{std::move(limits), {}, std::nullopt, 0U};
		auto* row =
			value_base(w,
					   e,
					   owner,
					   b,
					   "direct_type_access",
					   "clang22-original-direct-type-access/1",
					   facts.permitted_for_declared_type ? state::complete : state::unavailable);
		const auto* read = llvm::dyn_cast<clang::CastExpr>(&e);
		if (read)
			original_operand(w, row, *read, b);
		object(w, row, facts.object, facts.actual_address, b, current);
		w.optional(row, "type_access_permission", facts.permitted_for_declared_type);
		return w.finish();
	}
	sdk::result<detached_object_facets>
	detach_direct_downcast_facets(const clang::CastExpr& e,
								  const direct_downcast& facts,
								  const owned_object_state& current,
								  const clang::FunctionDecl& owner,
								  const object_facet_bindings& b,
								  object_facet_limits limits)
	{
		writer w{std::move(limits), {}, std::nullopt, 0U};
		auto* row =
			value_base(w,
					   e,
					   owner,
					   b,
					   "direct_static_downcast",
					   "clang22-original-direct-static-downcast/1",
					   facts.compatible_with_declared_type ? state::complete : state::unavailable);
		original_operand(w, row, e, b);
		object(w, row, facts.object, facts.actual_address, b, current);
		w.put(row, "object_target_type", type(b, facts.target_type));
		w.optional(
			row, "downcast_compatible_with_declared_type", facts.compatible_with_declared_type);
		return w.finish();
	}
	sdk::result<detached_object_facets>
	detach_direct_lifetime_facets(const clang::CXXMemberCallExpr& e,
								  const clang::VarDecl& d,
								  const direct_lifetime_state& facts,
								  unsigned block,
								  unsigned element,
								  const clang::FunctionDecl& owner,
								  const object_facet_bindings& b,
								  object_facet_limits limits)
	{
		writer w{std::move(limits), {}, std::nullopt, 0U};
		auto* row = value_base(w,
							   e,
							   owner,
							   b,
							   "direct_member_lifetime",
							   "clang22-original-direct-record-member-lifetime/1",
							   facts.complete() ? state::complete : state::unavailable);
		const auto* receiver = e.getImplicitObjectArgument();
		receiver = receiver ? receiver->IgnoreParenImpCasts() : nullptr;
		w.put(row, "object_declaration", declaration(b, d));
		w.put(row, "object_entity", entity(b, d));
		w.put(row, "declared_object_type", type(b, d.getType()));
		w.put(row, "object_body", b.body);
		w.put(row, "object_cfg_node", b.cfg_node ? b.cfg_node(block) : std::string_view{});
		w.put(row, "object_cfg_element_index", static_cast<std::uint64_t>(element));
		if (facts.phase)
			w.put(row, "object_phase", std::string_view{*facts.phase});
		w.put(row, "actual_polymorphic_use", facts.actual_polymorphic_use);
		if (receiver)
		{
			w.put(row, "object_receiver", syntax(b, *receiver));
			auto* ref = w.update("cc.syntax_node.v1", syntax(b, *receiver), declaration(b, owner));
			w.put(ref, "object_declaration", declaration(b, d));
			w.put(ref, "object_entity", entity(b, d));
			w.put(ref, "canonical_type", type(b, d.getType()));
		}
		return w.finish();
	}
} // namespace cxxlens::detail::clang22::object_semantics
