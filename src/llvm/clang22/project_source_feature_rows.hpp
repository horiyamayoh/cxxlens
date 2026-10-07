#pragma once

#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/relation.hpp>

#include "project_source_feature_walk.hpp"

namespace clang
{
	class NamedDecl;
} // namespace clang

namespace cxxlens::detail::clang22
{
	struct source_feature_source_binding
	{
		std::string_view file, snapshot, source;
		std::optional<bool> is_system;
	};
	struct source_feature_entered_file
	{
		std::string_view file, snapshot;
	};
	struct source_feature_bindings
	{
		std::string_view compile_unit;
		// These callbacks borrow the existing collector's original identity maps.
		// They never infer a missing binding from a source range or display name.
		std::function<source_feature_source_binding(const original_source_feature_view&)> source;
		std::function<std::string_view(const clang::Decl&)> declaration;
		std::function<std::string_view(const clang::NamedDecl&)> entity;
		std::function<std::string_view(clang::QualType)> canonical_type;
		std::function<std::string_view(const clang::Stmt&)> syntax;
	};
	struct source_feature_row_limits
	{
		source_feature_walk_limits traversal;
		std::size_t maximum_retained_bytes{64U * 1024U * 1024U};
		std::size_t maximum_binding_bytes{4096U};
	};
	struct project_source_feature_rows
	{
		std::vector<sdk::detached_row> rows;
		source_feature_walk_result traversal;
		std::size_t operations{}, retained_bytes_bound{};
		bool source_complete{};
	};
	// Entry membership is observed by the original PP callback, independently of
	// AST traversal and source binding. Unopened closure files are not invented.
	[[nodiscard]] sdk::result<project_source_feature_rows>
	detach_original_source_features(clang::ASTContext&,
									const source_feature_bindings&,
									std::span<const source_feature_entered_file>,
									bool entered_files_complete,
									source_feature_row_limits = {});
} // namespace cxxlens::detail::clang22
