#pragma once

/** @file select.hpp @brief LLVM-free immutable M2 semantic selector value DSL. */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/core/failure.hpp>
#include <cxxlens/facts.hpp>
#include <cxxlens/source.hpp>

namespace cxxlens::select
{
	namespace detail
	{
		struct selector_node;
		struct selector_access;
	} // namespace detail

	/** @brief Stable name comparison policy. */
	enum class name_match : std::uint8_t
	{
		exact,
		qualified_exact,
		unqualified_exact,
		glob
	};
	/** @brief Stable macro-origin selection policy. */
	enum class macro_match_policy : std::uint8_t
	{
		exclude,
		include_with_origin,
		only_macro_arguments,
		only_macro_bodies,
		only_expansions
	};
	/** @brief Stable implicit-node selection policy. */
	enum class implicit_node_policy : std::uint8_t
	{
		spelled_only,
		include_language_implicit,
		implicit_only
	};
	/** @brief Stable template selection policy. */
	enum class template_selection_policy : std::uint8_t
	{
		patterns,
		observed_instantiations,
		patterns_and_observed_instantiations
	};
	/** @brief Stable multi-variant selection policy. */
	enum class variant_match_policy : std::uint8_t
	{
		any_variant,
		all_variants,
		report_per_variant,
		reject_disagreement
	};
	/** @brief Stable call-target dispatch policy. */
	enum class dispatch_policy : std::uint8_t
	{
		direct_only,
		static_target,
		static_and_virtual_candidates,
		include_indirect_candidates
	};

	/** @brief Concrete conservative execution requirements for a normalized selector. */
	struct selector_requirements
	{
		/** @brief Concrete sorted fact profile. */
		fact_profile facts;
		/** @brief Minimum semantic precision; never an implicit downgrade. */
		precision_level minimum_precision{precision_level::ast_structural};
		/** @brief Sorted unique capability IDs. */
		std::vector<std::string> capabilities;
	};

	/** @brief Immutable selector over files. */
	class file_selector
	{
	  public:
		/** @brief Construct the unconstrained file selector. @pre
		 * None. @post Safe defaults are explicit on serialization. @note Requires file facts.
		 * @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		file_selector() = default;
		/** @brief Add an exact normalized path predicate. @param[in] value Path value. @retval
		 * value New selector. @pre Path is non-empty. @post Original is unchanged. @note Paths
		 * serialize generically. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] file_selector path_exact(path value) const;
		/** @brief Add a path glob predicate. @param[in] value Glob value. @retval value New
		 * selector. @pre Glob is non-empty. @post Original is unchanged. @note Glob is a filter,
		 * not semantic identity. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] file_selector path_glob(std::string value) const;
		/** @brief Add generated-file state. @param[in] value Desired state. @retval value New
		 * selector. @pre None. @post Original is unchanged. @note Default is true. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] file_selector generated(bool value = true) const;
		/** @brief Add system-file state. @param[in] value Desired state. @retval value New
		 * selector. @pre None. @post Original is unchanged. @note Default is true. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] file_selector system(bool value = true) const;
		/** @brief Intersect with a normalized disjunction. @param[in] values Alternatives. @retval
		 * value New selector. @pre Values have file domain. @post Empty alternatives produce false.
		 * @note Operand order is irrelevant. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] file_selector any_of(std::vector<file_selector> values) const;
		/** @brief Negate this selector. @retval value New selector. @pre None. @post Double
		 * negation normalizes away. @note Three-valued matching preserves ambiguity. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] file_selector negate() const;
		/** @brief Serialize canonical normalized form. @retval value Selector v1 JSON. @pre
		 * Selector is valid. @post No state changes. @note JSON bytes are structural identity.
		 * @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] std::string to_json() const;

	  private:
		explicit file_selector(std::shared_ptr<const detail::selector_node> value);
		std::shared_ptr<const detail::selector_node> value_;
		friend struct detail::selector_access;
	};

	/** @brief Immutable selector over semantic symbols. */
	class symbol_selector
	{
	  public:
		/** @brief Construct the unconstrained symbol selector. @pre
		 * None. @post Safe defaults are explicit on serialization. @note Requires symbol facts.
		 * @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		symbol_selector() = default;
		/** @brief Add one symbol kind. @param[in] value Kind. @retval value New selector. @pre Enum
		 * is valid. @post Original unchanged. @note Unknown stays explicit. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] symbol_selector kind(symbol_kind value) const;
		/** @brief Add a kind set. @param[in] values Kinds. @retval value New selector. @pre Enums
		 * are valid. @post Set is sorted unique. @note Empty set is false. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] symbol_selector kinds(std::vector<symbol_kind> values) const;
		/** @brief Add canonical-resolver name input. @param[in] value Name. @param[in] policy Match
		 * policy. @retval value New selector. @pre Name is non-empty. @post Original unchanged.
		 * @note Exact modes do not define symbol identity. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] symbol_selector name(std::string value,
										   name_match policy = name_match::qualified_exact) const;
		/** @brief Restrict declaration file. @param[in] value File selector. @retval value New
		 * selector. @pre Domains are valid. @post Requirements include file. @note Nested selector
		 * stays typed. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] symbol_selector declared_in(file_selector value) const;
		/** @brief Add definition state. @param[in] value Desired state. @retval value New selector.
		 * @pre None. @post Original unchanged. @note Default is true. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] symbol_selector defined(bool value = true) const;
		/** @brief Restrict semantic owner. @param[in] value Owner selector. @retval value New
		 * selector. @pre Symbol domain. @post Requirements are unioned. @note Names are not
		 * equality. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] symbol_selector member_of(symbol_selector value) const;
		/** @brief Restrict by inheritance. @param[in] value Base selector. @retval value New
		 * selector. @pre Symbol domain. @post Requires inheritance/workspace precision. @note
		 * Open-world uncertainty is retained by execution. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] symbol_selector derived_from(symbol_selector value) const;
		/** @brief Restrict by override relation. @param[in] value Base method selector. @retval
		 * value New selector. @pre Symbol domain. @post Requires override/workspace precision.
		 * @note Same name is insufficient. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] symbol_selector overrides(symbol_selector value) const;
		/** @brief Add public-surface state. @param[in] value Desired state. @retval value New
		 * selector. @pre None. @post Original unchanged. @note Accessibility is semantic.
		 * @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] symbol_selector public_surface(bool value = true) const;
		/** @brief Set macro policy. @param[in] value Policy. @retval value New selector. @pre Enum
		 * is valid. @post Prior policy is replaced. @note Non-exclude modes require macro facts.
		 * @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] symbol_selector macro(macro_match_policy value) const;
		/** @brief Set variant policy. @param[in] value Policy. @retval value New selector. @pre
		 * Enum is valid. @post Prior policy is replaced. @note Default rejects disagreement.
		 * @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] symbol_selector variants(variant_match_policy value) const;
		/** @brief Intersect a disjunction. @param[in] values Alternatives. @retval value New
		 * selector. @pre Symbol domain. @post Order and duplicates normalize away. @note Empty
		 * disjunction is false. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] symbol_selector any_of(std::vector<symbol_selector> values) const;
		/** @brief Intersect a conjunction. @param[in] values Operands. @retval value New selector.
		 * @pre Symbol domain. @post Order and duplicates normalize away. @note Empty conjunction is
		 * true. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] symbol_selector all_of(std::vector<symbol_selector> values) const;
		/** @brief Negate this selector. @retval value New selector. @pre None. @post Double
		 * negation normalizes away. @note Ambiguous remains ambiguous. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] symbol_selector negate() const;
		/** @brief Calculate conservative concrete requirements. @retval value Canonical
		 * requirements. @pre Valid selector. @post No state changes. @note All execution branches
		 * are unioned. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] selector_requirements requirements() const;
		/** @brief Render deterministic human explanation. @retval value Explanation. @pre Valid
		 * selector. @post No state changes. @note Prose is not control flow. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] std::string explain() const;
		/** @brief Serialize canonical normalized form. @retval value Selector v1 JSON. @pre Valid
		 * selector. @post No state changes. @note JSON bytes are structural identity. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] std::string to_json() const;

	  private:
		explicit symbol_selector(std::shared_ptr<const detail::selector_node> value);
		std::shared_ptr<const detail::selector_node> value_;
		friend struct detail::selector_access;
	};

	/** @brief Immutable selector over canonical type facts. */
	class type_selector
	{
	  public:
		/** @brief Construct the unconstrained type selector. @pre
		 * None. @post Canonical type requirement is explicit. @note Requires local semantic type
		 * facts. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		type_selector() = default;
		/** @brief Add canonical resolver input. @param[in] value Canonical spelling. @retval value
		 * New selector. @pre Non-empty. @post Original unchanged. @note String alone is not type
		 * identity. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] type_selector canonical(std::string value) const;
		/** @brief Add source spelling filter. @param[in] value Spelling. @retval value New
		 * selector. @pre Non-empty. @post Original unchanged. @note Presentation spelling is
		 * non-authoritative. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] type_selector spelling(std::string value) const;
		/** @brief Restrict declaring symbol. @param[in] value Symbol selector. @retval value New
		 * selector. @pre Typed domain. @post Requirements union. @note Resolver establishes
		 * identity. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] type_selector declared_as(symbol_selector value) const;
		/** @brief Require pointer pointee. @param[in] value Pointee selector. @retval value New
		 * selector. @pre Type domain. @post Original unchanged. @note Structural TypeIR is used.
		 * @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] type_selector pointer_to(type_selector value) const;
		/** @brief Require reference referent. @param[in] value Referent selector. @retval value New
		 * selector. @pre Type domain. @post Original unchanged. @note Lvalue/rvalue are retained by
		 * TypeIR. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] type_selector reference_to(type_selector value) const;
		/** @brief Add const qualification. @param[in] value Desired state. @retval value New
		 * selector. @pre None. @post Original unchanged. @note Default true. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] type_selector const_qualified(bool value = true) const;
		/** @brief Restrict record inheritance. @param[in] value Base record selector. @retval value
		 * New selector. @pre Typed domains. @post Requires inheritance/workspace precision. @note
		 * Names alone are insufficient. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] type_selector derived_from(symbol_selector value) const;
		/** @brief Set derived-type inclusion. @param[in] value Desired state. @retval value New
		 * selector. @pre None. @post Prior state replaced. @note True requires inheritance.
		 * @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] type_selector including_derived(bool value = true) const;
		/** @brief Restrict semantic conversion. @param[in] value Destination selector. @retval
		 * value New selector. @pre Type domain. @post Requires conversion facts. @note
		 * Pretty-string reparsing is forbidden. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] type_selector convertible_to(type_selector value) const;
		/** @brief Restrict template specialization. @param[in] value Template qualified name.
		 * @retval value New selector. @pre Non-empty. @post Original unchanged. @note Resolver
		 * establishes template identity. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] type_selector specialization_of(std::string value) const;
		/** @brief Ignore top-level cvref. @retval value New selector. @pre None. @post Original
		 * unchanged. @note Structural core type remains required. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] type_selector any_cvref() const;
		/** @brief Calculate conservative concrete requirements. @retval value Canonical
		 * requirements. @pre Valid selector. @post No state changes. @note Branch requirements are
		 * unioned. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] selector_requirements requirements() const;
		/** @brief Render deterministic explanation. @retval value Explanation. @pre Valid selector.
		 * @post No state changes. @note Prose is non-authoritative. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] std::string explain() const;
		/** @brief Serialize canonical normalized form. @retval value Selector v1 JSON. @pre Valid
		 * selector. @post No state changes. @note JSON bytes are structural identity. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] std::string to_json() const;

	  private:
		explicit type_selector(std::shared_ptr<const detail::selector_node> value);
		std::shared_ptr<const detail::selector_node> value_;
		friend struct detail::selector_access;
	};

	/** @brief Immutable selector over call facts. */
	class call_selector
	{
	  public:
		/** @brief Construct the unconstrained call selector. @pre
		 * None. @post Safe policies are explicit on serialization. @note Requires call facts.
		 * @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		call_selector() = default;
		/** @brief Add one call kind. @param[in] value Kind. @retval value New selector. @pre Enum
		 * valid. @post Original unchanged. @note Unknown stays explicit. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] call_selector kind(call_kind value) const;
		/** @brief Add call kind set. @param[in] values Kinds. @retval value New selector. @pre
		 * Enums valid. @post Sorted unique. @note Empty set is false. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] call_selector kinds(std::vector<call_kind> values) const;
		/** @brief Restrict resolved callee. @param[in] value Symbol selector. @retval value New
		 * selector. @pre Symbol domain. @post Requirements union. @note Resolver avoids name
		 * identity. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] call_selector callee(symbol_selector value) const;
		/** @brief Restrict qualified callee input. @param[in] value Qualified name. @retval value
		 * New selector. @pre Non-empty. @post Original unchanged. @note Canonical resolver is
		 * required. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] call_selector callee_name(std::string value) const;
		/** @brief Restrict free-function name input. @param[in] value Name. @retval value New
		 * selector. @pre Non-empty. @post Original unchanged. @note Overloads remain semantic.
		 * @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] call_selector function_name(std::string value) const;
		/** @brief Restrict method name input. @param[in] value Name. @retval value New selector.
		 * @pre Non-empty. @post Original unchanged. @note Receiver type disambiguates. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] call_selector method_name(std::string value) const;
		/** @brief Restrict receiver type. @param[in] value Type selector. @retval value New
		 * selector. @pre Type domain. @post Requirements union. @note TypeIR identity is used.
		 * @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] call_selector receiver_type(type_selector value) const;
		/** @brief Set derived receiver inclusion. @param[in] value Desired state. @retval value New
		 * selector. @pre None. @post Prior setting replaced. @note True requires
		 * inheritance/workspace precision. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] call_selector include_derived_types(bool value = true) const;
		/** @brief Set virtual override inclusion. @param[in] value Desired state. @retval value New
		 * selector. @pre None. @post Prior setting replaced. @note True requires override/workspace
		 * precision. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] call_selector include_virtual_overrides(bool value = true) const;
		/** @brief Set dispatch policy. @param[in] value Policy. @retval value New selector. @pre
		 * Enum valid. @post Prior policy replaced. @note Default is direct-only. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] call_selector dispatch(dispatch_policy value) const;
		/** @brief Restrict argument type. @param[in] index Zero-based index. @param[in] value Type
		 * selector. @retval value New selector. @pre Type domain. @post Requirements union. @note
		 * Missing arguments do not match. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] call_selector argument_type(std::size_t index, type_selector value) const;
		/** @brief Restrict containing symbol. @param[in] value Symbol selector. @retval value New
		 * selector. @pre Symbol domain. @post Requirements union. @note Lexical names are not
		 * identity. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] call_selector inside(symbol_selector value) const;
		/** @brief Restrict containing file. @param[in] value File selector. @retval value New
		 * selector. @pre File domain. @post Requires file facts. @note Paths are normalized by
		 * execution context. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] call_selector in_file(file_selector value) const;
		/** @brief Set implicit-node policy. @param[in] value Policy. @retval value New selector.
		 * @pre Enum valid. @post Prior policy replaced. @note Default is spelled-only. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] call_selector implicit(implicit_node_policy value) const;
		/** @brief Set macro-origin policy. @param[in] value Policy. @retval value New selector.
		 * @pre Enum valid. @post Prior policy replaced. @note Non-exclude requires macro facts.
		 * @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] call_selector macro(macro_match_policy value) const;
		/** @brief Set template policy. @param[in] value Policy. @retval value New selector. @pre
		 * Enum valid. @post Prior policy replaced. @note Default selects patterns. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] call_selector templates(template_selection_policy value) const;
		/** @brief Set variant policy. @param[in] value Policy. @retval value New selector. @pre
		 * Enum valid. @post Prior policy replaced. @note Default rejects disagreement. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] call_selector variants(variant_match_policy value) const;
		/** @brief Raise minimum precision. @param[in] value Precision. @retval value New selector.
		 * @pre Enum valid. @post No downgrade below predicate needs. @note Requirement is explicit.
		 * @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] call_selector precision(precision_level value) const;
		/** @brief Calculate conservative concrete requirements. @retval value Canonical
		 * requirements. @pre Valid selector. @post No state changes. @note All nested branches are
		 * unioned. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] selector_requirements requirements() const;
		/** @brief Render deterministic explanation. @retval value Explanation. @pre Valid selector.
		 * @post No state changes. @note Prose is non-authoritative. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] std::string explain() const;
		/** @brief Serialize canonical normalized form. @retval value Selector v1 JSON. @pre Valid
		 * selector. @post No state changes. @note JSON bytes are structural identity. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] std::string to_json() const;

	  private:
		explicit call_selector(std::shared_ptr<const detail::selector_node> value);
		std::shared_ptr<const detail::selector_node> value_;
		friend struct detail::selector_access;
	};

	/** @brief Type-erased immutable M2 selector. */
	class semantic_selector
	{
	  public:
		/** @brief Parse and type-check selector v1 JSON. @param[in] input JSON bytes. @retval value
		 * Typed-erased selector or structured error. @pre Input is bounded UTF-8 JSON. @post
		 * Canonical serialization is normalized. @note Unknown schemas/policies fail closed.
		 * @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] static result<semantic_selector> from_json(std::string_view input);
		/** @brief Calculate conservative concrete requirements. @retval value Canonical
		 * requirements. @pre Valid selector. @post No state changes. @note Type erasure preserves
		 * exact requirements. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] selector_requirements requirements() const;
		/** @brief Render deterministic explanation. @retval value Explanation. @pre Valid selector.
		 * @post No state changes. @note Prose is non-authoritative. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] std::string explain() const;
		/** @brief Serialize canonical normalized form. @retval value Selector v1 JSON. @pre Valid
		 * selector. @post No state changes. @note Typed and erased bytes are identical. @code{.cpp}
		 * #include <cxxlens/select.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] std::string to_json() const;

	  private:
		explicit semantic_selector(std::shared_ptr<const detail::selector_node> value);
		std::shared_ptr<const detail::selector_node> value_;
		friend struct detail::selector_access;
	};

	/** @brief Construct an unconstrained symbol selector. @retval value Selector with explicit safe
	 * policies. @pre None. @post Canonical value returned. @note Requires symbol facts. @code{.cpp}
	 * #include <cxxlens/select.hpp>
	 * int main(){return 0;}
	 * @endcode */
	[[nodiscard]] symbol_selector any_symbol();
	/** @brief Construct a function selector. @param[in] qualified_name Optional qualified name.
	 * @retval value Function selector. @pre Name may be empty. @post Canonical value returned.
	 * @note Resolver establishes identity. @code{.cpp}
	 * #include <cxxlens/select.hpp>
	 * int main(){return 0;}
	 * @endcode */
	[[nodiscard]] symbol_selector function(std::string qualified_name = {});
	/** @brief Construct a method selector. @param[in] qualified_name Optional qualified name.
	 * @retval value Method selector. @pre Name may be empty. @post Canonical value returned. @note
	 * Resolver establishes identity. @code{.cpp}
	 * #include <cxxlens/select.hpp>
	 * int main(){return 0;}
	 * @endcode */
	[[nodiscard]] symbol_selector method(std::string qualified_name = {});
	/** @brief Construct a record selector. @param[in] qualified_name Optional qualified name.
	 * @retval value Record selector. @pre Name may be empty. @post Canonical value returned. @note
	 * Class/struct share semantic record family. @code{.cpp}
	 * #include <cxxlens/select.hpp>
	 * int main(){return 0;}
	 * @endcode */
	[[nodiscard]] symbol_selector record(std::string qualified_name = {});
	/** @brief Construct a variable selector. @param[in] qualified_name Optional qualified name.
	 * @retval value Variable selector. @pre Name may be empty. @post Canonical value returned.
	 * @note Resolver establishes identity. @code{.cpp}
	 * #include <cxxlens/select.hpp>
	 * int main(){return 0;}
	 * @endcode */
	[[nodiscard]] symbol_selector variable(std::string qualified_name = {});
	/** @brief Construct a macro symbol selector. @param[in] name Optional macro name. @retval value
	 * Macro selector. @pre Name may be empty. @post Canonical value returned. @note Macro origin
	 * policy stays explicit. @code{.cpp}
	 * #include <cxxlens/select.hpp>
	 * int main(){return 0;}
	 * @endcode */
	[[nodiscard]] symbol_selector macro(std::string name = {});
	/** @brief Construct a type selector. @param[in] canonical Optional canonical resolver input.
	 * @retval value Type selector. @pre Input may be empty. @post Canonical value returned. @note
	 * Pretty string alone is not identity. @code{.cpp}
	 * #include <cxxlens/select.hpp>
	 * int main(){return 0;}
	 * @endcode */
	[[nodiscard]] type_selector type(std::string canonical = {});
	/** @brief Construct an unconstrained call selector. @retval value Selector with explicit safe
	 * policies. @pre None. @post Canonical value returned. @note Requires call facts. @code{.cpp}
	 * #include <cxxlens/select.hpp>
	 * int main(){return 0;}
	 * @endcode */
	[[nodiscard]] call_selector any_call();
	/** @brief Construct calls to a semantic callee selector. @param[in] callee Callee selector.
	 * @retval value Call selector. @pre Symbol domain. @post Requirements union. @note Names alone
	 * are insufficient. @code{.cpp}
	 * #include <cxxlens/select.hpp>
	 * int main(){return 0;}
	 * @endcode */
	[[nodiscard]] call_selector calls_to(symbol_selector callee);
	/** @brief Construct calls to a qualified function. @param[in] qualified_name Qualified name.
	 * @retval value Call selector. @pre Non-empty. @post Canonical value returned. @note Resolver
	 * handles overloads. @code{.cpp}
	 * #include <cxxlens/select.hpp>
	 * int main(){return 0;}
	 * @endcode */
	[[nodiscard]] call_selector calls_to_function(std::string qualified_name);
	/** @brief Construct calls to a method. @param[in] receiver_type Receiver canonical input.
	 * @param[in] method_name Method name. @retval value Call selector. @pre Inputs non-empty. @post
	 * Canonical value returned. @note Flagship policies remain explicit. @code{.cpp}
	 * #include <cxxlens/select.hpp>
	 * int main()
	 * {
	 *     auto selector = cxxlens::select::calls_to_method("Base", "start")
	 *         .include_derived_types()
	 *         .include_virtual_overrides()
	 *         .dispatch(cxxlens::select::dispatch_policy::static_and_virtual_candidates);
	 *     return selector.to_json().empty();
	 * }
	 * @endcode */
	[[nodiscard]] call_selector calls_to_method(std::string receiver_type, std::string method_name);

	/** @brief Type-erase a file selector. @param[in] value Typed selector. @retval value Erased
	 * selector. @pre Valid selector. @post Semantics preserved. @note JSON is identical.
	 * @code{.cpp}
	 * #include <cxxlens/select.hpp>
	 * int main(){return 0;}
	 * @endcode */
	[[nodiscard]] semantic_selector semantic(file_selector value);
	/** @brief Type-erase a symbol selector. @param[in] value Typed selector. @retval value Erased
	 * selector. @pre Valid selector. @post Semantics preserved. @note Requirements identical.
	 * @code{.cpp}
	 * #include <cxxlens/select.hpp>
	 * int main(){return 0;}
	 * @endcode */
	[[nodiscard]] semantic_selector semantic(symbol_selector value);
	/** @brief Type-erase a type selector. @param[in] value Typed selector. @retval value Erased
	 * selector. @pre Valid selector. @post Semantics preserved. @note Requirements identical.
	 * @code{.cpp}
	 * #include <cxxlens/select.hpp>
	 * int main(){return 0;}
	 * @endcode */
	[[nodiscard]] semantic_selector semantic(type_selector value);
	/** @brief Type-erase a call selector. @param[in] value Typed selector. @retval value Erased
	 * selector. @pre Valid selector. @post Semantics preserved. @note Requirements identical.
	 * @code{.cpp}
	 * #include <cxxlens/select.hpp>
	 * int main(){return 0;}
	 * @endcode */
	[[nodiscard]] semantic_selector semantic(call_selector value);

	/** @brief Immutable selector over detached expression facts and bounded refinement. */
	class expression_selector
	{
	  public:
		[[nodiscard]] expression_selector type_is(type_selector value) const;
		[[nodiscard]] expression_selector refers_to(symbol_selector value) const;
		[[nodiscard]] expression_selector is_null_pointer() const;
		[[nodiscard]] expression_selector is_literal() const;
		[[nodiscard]] expression_selector integer_value(std::int64_t value) const;
		[[nodiscard]] expression_selector string_value(std::string value) const;
		[[nodiscard]] expression_selector implicit(implicit_node_policy value) const;
		[[nodiscard]] expression_selector macro(macro_match_policy value) const;
		[[nodiscard]] selector_requirements requirements() const;
		[[nodiscard]] std::string to_json() const;

	  private:
		std::shared_ptr<const detail::selector_node> value_;
		friend struct detail::selector_access;
	};

	[[nodiscard]] semantic_selector semantic(expression_selector value);

	/** @brief Immutable selector over detached semantic reference rows. */
	class reference_selector
	{
	  public:
		[[nodiscard]] reference_selector target(symbol_selector value) const;
		[[nodiscard]] reference_selector kind(reference_kind value) const;
		[[nodiscard]] reference_selector kinds(std::vector<reference_kind> values) const;
		[[nodiscard]] reference_selector inside(symbol_selector value) const;
		[[nodiscard]] reference_selector in_file(file_selector value) const;
		[[nodiscard]] reference_selector macro(macro_match_policy value) const;
		[[nodiscard]] reference_selector variants(variant_match_policy value) const;
		[[nodiscard]] selector_requirements requirements() const;
		[[nodiscard]] std::string to_json() const;

	  private:
		std::shared_ptr<const detail::selector_node> value_;
		friend struct detail::selector_access;
	};

	[[nodiscard]] reference_selector references_to(symbol_selector target);
	[[nodiscard]] semantic_selector semantic(reference_selector value);

	/** @brief Immutable selector over detached conversion facts. */
	class conversion_selector
	{
	  public:
		[[nodiscard]] conversion_selector kind(std::string value) const;
		[[nodiscard]] conversion_selector from(type_selector value) const;
		[[nodiscard]] conversion_selector to(type_selector value) const;
		[[nodiscard]] conversion_selector implicit(bool value = true) const;
		[[nodiscard]] conversion_selector in_file(file_selector value) const;
		[[nodiscard]] selector_requirements requirements() const;

	  private:
		std::shared_ptr<const detail::selector_node> value_;
		friend struct detail::selector_access;
	};

	/** @brief Immutable selector over include relations and provider evidence. */
	class include_selector
	{
	  public:
		[[nodiscard]] include_selector header(std::string spelling) const;
		[[nodiscard]] include_selector resolved_to(path value) const;
		[[nodiscard]] include_selector used(bool value = true) const;
		[[nodiscard]] include_selector system(bool value = true) const;
		[[nodiscard]] include_selector providing(symbol_selector value) const;
		[[nodiscard]] include_selector included_by(file_selector value) const;
		[[nodiscard]] selector_requirements requirements() const;

	  private:
		std::shared_ptr<const detail::selector_node> value_;
		friend struct detail::selector_access;
	};

	/** @brief Immutable selector over macro definitions/expansions with exact origin. */
	class macro_selector
	{
	  public:
		[[nodiscard]] macro_selector name(std::string value) const;
		[[nodiscard]] macro_selector function_like(bool value = true) const;
		[[nodiscard]] macro_selector used_in(file_selector value) const;
		[[nodiscard]] macro_selector argument_contains(expression_selector value) const;
		[[nodiscard]] selector_requirements requirements() const;

	  private:
		std::shared_ptr<const detail::selector_node> value_;
		friend struct detail::selector_access;
	};

	[[nodiscard]] semantic_selector semantic(conversion_selector value);
	[[nodiscard]] semantic_selector semantic(include_selector value);
	[[nodiscard]] semantic_selector semantic(macro_selector value);
} // namespace cxxlens::select
