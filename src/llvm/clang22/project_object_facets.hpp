#pragma once
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <clang/AST/Type.h>
#include <cxxlens/sdk/relation.hpp>

#include "direct_lifetime_semantics.hpp"
#include "direct_object_semantics.hpp"
#include "function_exit_semantics.hpp"
#include "owned_object_state.hpp"
#include "value_domain_semantics.hpp"
namespace clang
{
	class Decl;
	class NamedDecl;
	class Stmt;
	class FunctionDecl;
	class CFG;
} // namespace clang
namespace cxxlens::detail::clang22::object_semantics
{
	struct object_facet_limits
	{
		std::size_t maximum_operations{8'000'000U}, maximum_fields{100'000U},
			maximum_retained_bytes{64U * 1024U * 1024U}, maximum_text_bytes{8192U};
		std::function<bool()> cancelled;
	};
	/** A patch to one existing compiler row. The ordinary collector must select
	 * the exact original unit/world and, for detail, its physical source carrier.
	 * No new row, compiler identity, source interval or canonical type is derived. */
	struct object_facet_update
	{
		std::string relation, target, scope_declaration, source;
		std::map<std::string, sdk::detached_cell, std::less<>> fields;
	};
	struct detached_object_facets
	{
		std::vector<object_facet_update> updates;
		std::size_t operations{}, retained_bytes_bound{};
	};
	struct object_facet_bindings
	{
		std::string_view compile_unit, body;
		// Borrowed existing extractor maps remain alive through this detachment.
		std::function<std::string_view(const clang::Decl&)> declaration;
		std::function<std::string_view(const clang::NamedDecl&)> entity;
		std::function<std::string_view(const clang::Stmt&)> syntax;
		std::function<std::string_view(clang::QualType)> canonical_type;
		std::function<std::string_view(const clang::FunctionDecl&)> declaration_source;
		std::function<std::string_view(unsigned)> cfg_node;
		std::function<std::string_view(unsigned, unsigned)> cfg_edge;
	};
	[[nodiscard]] sdk::result<detached_object_facets>
	detach_function_exit_facets(const clang::FunctionDecl&,
								const exit_projection&,
								const object_facet_bindings&,
								object_facet_limits = {});
	[[nodiscard]] sdk::result<detached_object_facets>
	detach_enum_value_facets(const clang::CastExpr&,
							 const enum_value&,
							 const clang::FunctionDecl&,
							 const object_facet_bindings&,
							 object_facet_limits = {});
	[[nodiscard]] sdk::result<detached_object_facets>
	detach_bool_value_facets(const clang::CastExpr&,
							 const bool_representation&,
							 const clang::FunctionDecl&,
							 const object_facet_bindings&,
							 object_facet_limits = {});
	[[nodiscard]] sdk::result<detached_object_facets>
	detach_direct_type_facets(const clang::Expr&,
							  const direct_type_access&,
							  const owned_object_state&,
							  const clang::FunctionDecl&,
							  const object_facet_bindings&,
							  object_facet_limits = {});
	[[nodiscard]] sdk::result<detached_object_facets>
	detach_direct_downcast_facets(const clang::CastExpr&,
								  const direct_downcast&,
								  const owned_object_state&,
								  const clang::FunctionDecl&,
								  const object_facet_bindings&,
								  object_facet_limits = {});
	/** cfg block/element are the actual selected CFGStmt occurrence supplied by
	 * the ordinary emitter, never found by source interval containment. */
	[[nodiscard]] sdk::result<detached_object_facets>
	detach_direct_lifetime_facets(const clang::CXXMemberCallExpr&,
								  const clang::VarDecl&,
								  const direct_lifetime_state&,
								  unsigned block,
								  unsigned element,
								  const clang::FunctionDecl&,
								  const object_facet_bindings&,
								  object_facet_limits = {});
} // namespace cxxlens::detail::clang22::object_semantics
