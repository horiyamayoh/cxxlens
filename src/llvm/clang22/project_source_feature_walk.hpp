#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <clang/AST/TypeLoc.h>
#include <clang/Basic/SourceLocation.h>

namespace clang
{
	class ASTContext;
	class Attr;
	class Decl;
	class Stmt;
} // namespace clang

namespace cxxlens::detail::clang22
{
	enum class source_feature_class : std::uint8_t
	{
		declaration,
		statement,
		attribute,
		type_location
	};

	// Borrowed compiler objects expire with the actual translation unit. No ID or
	// physical source ownership is reconstructed by this traversal helper.
	struct original_source_feature_view
	{
		source_feature_class feature_class;
		std::uint32_t compiler_kind;
		std::string_view compiler_kind_name;
		clang::SourceRange source;
		const clang::Decl* declaration_context{};
		const clang::Decl* declaration{};
		const clang::Stmt* statement{};
		const clang::Attr* attribute{};
		clang::TypeLoc type_location{};
		std::uint64_t ordinal{};
	};

	struct source_feature_walk_limits
	{
		std::size_t maximum_operations{8'000'000U};
		std::size_t maximum_members{1'000'000U};
		std::size_t maximum_depth{256U};
		std::size_t maximum_pending_statements{16'384U};
		bool (*cancelled)(void*) noexcept {};
		void* cancellation_context{};
		// Optional enclosing work budget; independent traversal limits still apply.
		bool (*try_charge_operation)(void*) noexcept {};
		void* operation_context{};
	};

	struct source_feature_walk_result
	{
		std::size_t operations{}, members{}, deferred_constructs{};
		bool completed{};
		std::string_view frontier;
	};

	// The observer owns row/source/identity retention and its independent bounds.
	using source_feature_observer = bool (*)(void*, const original_source_feature_view&) noexcept;

	[[nodiscard]] source_feature_walk_result
	walk_original_source_features(clang::ASTContext&,
								  source_feature_observer,
								  void* observer_context,
								  source_feature_walk_limits = {});
} // namespace cxxlens::detail::clang22
