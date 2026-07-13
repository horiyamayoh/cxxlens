#pragma once

/**
 * @file select_search_explain.hpp
 * @brief Issue #45 Contract Candidate declarations for the remaining selector/search/explain APIs.
 *
 * This is a non-installed fragment. Issue #53 owns stable-header integration and Issue #54 owns
 * the frozen transition. No declaration in this file has a production implementation in #45.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/explain.hpp>
#include <cxxlens/search.hpp>

namespace cxxlens
{
	namespace transform
	{
		class codemod;
		class edit_plan;
	} // namespace transform
	namespace generate
	{
		class generation_plan;
	} // namespace generate
	namespace rules
	{
		class rule;
	} // namespace rules
} // namespace cxxlens

namespace cxxlens::select
{
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

	/** @brief Type-erase one expression selector without changing normalized identity. */
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

	/** @brief Desugar a target predicate without name-only identity. */
	[[nodiscard]] reference_selector references_to(symbol_selector target);
	/** @brief Type-erase one reference selector without changing normalized identity. */
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

namespace cxxlens::search
{
	/** @brief Search semantic symbols in one provisioned immutable snapshot. */
	[[nodiscard]] result<search_report<symbol>> symbols(
		const workspace& workspace,
		select::symbol_selector selector = select::any_symbol(),
		search_options options = {});
	/** @brief Search detached semantic reference rows; missing facts are never an empty match set. */
	[[nodiscard]] result<search_report<reference>> references(
		const workspace& workspace,
		select::reference_selector selector,
		search_options options = {});
	/** @brief Search include relations while preserving spelling, resolution, macro, and variant state. */
	[[nodiscard]] result<search_report<include_relation>> includes(
		const workspace& workspace,
		select::include_selector selector = {},
		search_options options = {});
	/** @brief Search macro expansions with definition/argument/expansion origins kept distinct. */
	[[nodiscard]] result<search_report<macro_expansion>> macros(
		const workspace& workspace,
		select::macro_selector selector,
		search_options options = {});
	/** @brief Search versioned conversion facts without pretty-type-string equality. */
	[[nodiscard]] result<search_report<fact>> conversions(
		const workspace& workspace,
		select::conversion_selector selector,
		search_options options = {});
} // namespace cxxlens::search

namespace cxxlens::explain
{
	/** @brief Project an already validated edit plan; never re-run planning. */
	[[nodiscard]] explanation edit_plan(
		const transform::edit_plan& value, detail_level level = detail_level::normal);
	/** @brief Project an already validated generation plan; never re-run generation decisions. */
	[[nodiscard]] explanation generation_plan(
		const generate::generation_plan& value, detail_level level = detail_level::normal);

	/** @brief Build deterministic agent guidance from authoritative rule metadata. */
	[[nodiscard]] agent_task_card for_rule(const rules::rule& value);
	/** @brief Build deterministic agent guidance from an immutable codemod contract. */
	[[nodiscard]] agent_task_card for_codemod(const transform::codemod& value);
	/** @brief Build deterministic agent guidance from an authoritative generation plan. */
	[[nodiscard]] agent_task_card for_generation(const generate::generation_plan& value);
	/** @brief Return canonical frozen-catalog data without runtime state or sensitive paths. */
	[[nodiscard]] std::string api_catalog_json();
} // namespace cxxlens::explain
