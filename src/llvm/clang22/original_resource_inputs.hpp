#pragma once
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <clang/AST/Type.h>
#include <cxxlens/sdk/relation.hpp>

namespace clang
{
	class Attr;
	class CXXCtorInitializer;
	class Decl;
	class Expr;
	class FunctionDecl;
	class NamedDecl;
	class ParmVarDecl;
	class Stmt;
} // namespace clang
namespace cxxlens::detail::clang22::resource_inputs
{
	inline constexpr std::string_view move_profile = "clang22-original-selected-special-member/1";
	inline constexpr std::string_view attribute_profile =
		"clang22-original-declaration-attributes/1";
	inline constexpr std::string_view address_profile = "clang22-original-address-transfer/1";
	struct limits
	{
		std::size_t maximum_operations{8'000'000U}, maximum_fields{100'000U},
			maximum_retained_bytes{64U * 1024U * 1024U}, maximum_text_bytes{8192U},
			maximum_wrapper_depth{64U};
		std::function<bool()> cancelled;
	};
	// All compiler pointers and byte views are borrowed within the native job only.
	// Detach before AST destruction. No compiler pointer enters a detached row.
	struct attribute_argument
	{
		unsigned ordinal{};
		std::string_view kind, binding_state;
		std::optional<unsigned> source_index, ast_index;
		std::optional<bool> implicit_this;
		const clang::ParmVarDecl* formal{};
		const clang::Expr* expression{};
	};
	struct attribute_observation
	{
		const clang::Decl* declaration{};
		const clang::Attr* attribute{};
		unsigned ordinal{}, original_kind{};
		std::string_view spelling, ownership_kind, argument_state{"unsupported"};
		std::optional<std::string_view> module, annotation;
		bool implicit{}, inherited{};
		std::vector<attribute_argument> arguments;
	};
	struct move_observation
	{
		const clang::Expr* site{};
		const clang::Decl* owner{};
		const clang::FunctionDecl* selected{};
		const clang::Expr* source_actual{};
		const clang::ParmVarDecl* source_formal{};
		const clang::Expr* destination_expression{};
		const clang::Decl* destination_declaration{};
		std::string_view kind, classification_state, destination_state;
		std::optional<bool> elidable;
	};
	struct address_observation
	{
		const clang::Decl* carrier_declaration{};
		const clang::Stmt* carrier_syntax{};
		unsigned endpoint_ordinal{};
		const clang::Decl* owner{};
		const clang::Expr* source_expression{};
		const clang::Expr* origin_expression{};
		const clang::Decl* origin_declaration{};
		const clang::Expr* destination_expression{};
		const clang::Decl* destination_declaration{};
		const clang::Decl* referenced_declaration{};
		clang::QualType source_type, destination_type;
		std::string_view kind, context, origin_state, destination_state;
		std::optional<std::string_view> storage_duration, linkage, capture_kind;
		std::optional<bool> capture_implicit;
	};
	struct observations
	{
		const clang::Decl* admitted_declaration{};
		const clang::Stmt* admitted_syntax{};
		std::vector<attribute_observation> attributes;
		std::vector<move_observation> moves;
		std::vector<address_observation> addresses;
		std::size_t operations{}, retained_bytes_bound{};
		bool attributes_admitted{}, moves_admitted{}, addresses_admitted{};
	};
	// Observe exactly one original admitted carrier; the collector owns traversal,
	// scope populations and admission. These functions never traverse a body or CFG.
	[[nodiscard]] sdk::result<observations> observe_declaration(const clang::Decl&, limits = {});
	struct site_context
	{
		const clang::Decl* owner{};
		// Exact active original initializer contexts supplied by the collector's
		// existing native traversal. Helper validates child pointers before binding.
		const clang::Decl* initializing_declaration{};
		const clang::CXXCtorInitializer* member_initializer{};
	};
	[[nodiscard]] sdk::result<observations>
	observe_statement(const clang::Stmt&, site_context = {}, limits = {});
	struct bindings
	{
		std::string_view compile_unit;
		std::function<std::string_view(const clang::Decl&)> declaration;
		std::function<std::string_view(const clang::NamedDecl&)> entity;
		std::function<std::string_view(const clang::Stmt&)> syntax;
		std::function<std::string_view(const clang::Stmt&)> source;
		std::function<std::string_view(const clang::Decl&)> declaration_source;
		std::function<std::string_view(const clang::Attr&)> attribute_source;
		std::function<std::string_view(clang::QualType)> canonical_type;
		std::function<std::optional<bool>(const clang::Expr&)> potentially_evaluated;
		// Parent supplies authoritative generated descriptors, never a second registry.
		std::function<const sdk::relation_descriptor*(std::string_view)> descriptor;
	};
	struct carrier_summary
	{
		std::string declaration, syntax;
		std::string_view profile;
		std::size_t count{}, argument_count{};
		bool argument_inventory_complete{true};
		std::vector<std::string> ids;
	};
	struct detached_inputs
	{
		std::vector<sdk::detached_row> rows;
		std::vector<carrier_summary> summaries;
		std::size_t operations{}, retained_bytes_bound{};
	};
	[[nodiscard]] sdk::result<detached_inputs>
	detach(const observations&, const bindings&, limits = {});
} // namespace cxxlens::detail::clang22::resource_inputs
