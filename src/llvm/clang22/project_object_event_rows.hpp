#pragma once
#include <span>

#include "project_object_recorder.hpp"
namespace cxxlens::detail::clang22::object_semantics
{
	struct object_event_bindings
	{
		std::string_view compile_unit;
		// Borrow existing exact compiler-ID/source/USR maps. No second normalization.
		std::function<std::string_view(const clang::Decl&)> declaration;
		std::function<std::string_view(const clang::NamedDecl&)> entity;
		std::function<std::span<const std::byte>(const clang::NamedDecl&)> usr;
		std::function<std::string_view(const clang::Stmt&)> syntax;
		std::function<std::string_view(const clang::Stmt&)> source;
		std::function<std::optional<bool>(const clang::Expr&)> is_system;
		std::function<std::string_view(const clang::FunctionDecl&)> body;
		std::function<std::string_view(clang::QualType)> canonical_type;
		// The existing template event row-detacher owns these original root IDs.
		std::function<std::string_view(std::size_t)> evaluation_root;
	};
	struct project_object_event_rows
	{
		std::vector<sdk::detached_row> rows;
		// Qualified original declaration-inventory columns; merge before validation.
		std::map<std::string, sdk::detached_cell, std::less<>> inventory_facets;
		std::size_t operations{}, retained_bytes_bound{};
	};
	[[nodiscard]] sdk::result<project_object_event_rows> detach_project_object_events(
		const project_object_observations&, const object_event_bindings&, object_facet_limits = {});
} // namespace cxxlens::detail::clang22::object_semantics
