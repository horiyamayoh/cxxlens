#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include <cxxlens/facts.hpp>
#include <cxxlens/select.hpp>
#include <cxxlens/workspace.hpp>

namespace cxxlens::detail
{
	template <class T>
	struct search_report_data;
	struct search_report_access;
} // namespace cxxlens::detail

namespace cxxlens
{
	/** @brief Public controls for one immutable semantic search operation. */
	struct search_options
	{
		/** Requested workspace scope. */
		analysis_scope scope{analysis_scope::all()};
		/** Minimum requested precision; selector requirements may raise it. */
		precision_level precision{precision_level::workspace_semantic};
		/** Multi-variant result policy. */
		select::variant_match_policy variants{select::variant_match_policy::report_per_variant};
		/** Whether human projections should expand unresolved detail. */
		bool include_unresolved{true};
		/** Whether incomplete coverage fails instead of returning a partial report. */
		bool strict_coverage{};
		/** Optional maximum number of returned matches. */
		std::optional<std::uint64_t> result_limit;
		/** Cancellation, deadline, and scheduler controls. */
		execution_context execution;
	};

	/** @brief Immutable authoritative semantic search result.
	 * @tparam T Detached public result row type. */
	template <class T>
	class search_report
	{
	  public:
		/** @brief Construct an unbound empty report handle. @pre None. @post Accessors expose an
		 * empty best-effort result. @note Search functions return bound validated reports.
		 * @code{.cpp}
		 * #include <cxxlens/search.hpp>
		 * int main(){cxxlens::search_report<cxxlens::call_site> r;return r.matches().empty()?0:1;}
		 * @endcode */
		search_report() = default;
		/** @brief Return canonical authoritative match rows. @retval value Borrowed immutable rows.
		 * @pre Report remains alive. @post Report is unchanged. @note Ordering is semantic ID,
		 * variant, then source key.
		 * @code{.cpp}
		 * #include <cxxlens/search.hpp>
		 * int main(){cxxlens::search_report<cxxlens::call_site> r;return r.matches().empty()?0:1;}
		 * @endcode */
		[[nodiscard]] std::span<const T> matches() const noexcept;
		/** @brief Return exact requested-universe coverage. @retval value Borrowed coverage report.
		 * @pre Report remains alive. @post Report is unchanged. @note Empty matches do not imply
		 * complete coverage.
		 * @code{.cpp}
		 * #include <cxxlens/search.hpp>
		 * int main(){cxxlens::search_report<cxxlens::call_site> r;return
		 * r.coverage().validate()?0:1;}
		 * @endcode */
		[[nodiscard]] const coverage_report& coverage() const noexcept;
		/** @brief Return canonical unresolved prerequisites and partial states. @retval value
		 * Borrowed immutable rows. @pre Report remains alive. @post Report is unchanged. @note
		 * Open-world and missing coverage remain distinct from empty results.
		 * @code{.cpp}
		 * #include <cxxlens/search.hpp>
		 * int main(){cxxlens::search_report<cxxlens::call_site> r;return
		 * r.unresolved_items().empty()?0:1;}
		 * @endcode */
		[[nodiscard]] std::span<const unresolved> unresolved_items() const noexcept;
		/** @brief Return achieved result guarantee. @retval value Typed guarantee. @pre None. @post
		 * Report is unchanged. @note Confidence and guarantee are independent.
		 * @code{.cpp}
		 * #include <cxxlens/search.hpp>
		 * int main(){cxxlens::search_report<cxxlens::call_site> r;(void)r.guarantee();return 0;}
		 * @endcode */
		[[nodiscard]] result_guarantee guarantee() const noexcept;
		/** @brief Return achieved semantic precision. @retval value Typed precision. @pre None.
		 * @post Report is unchanged. @note Selector requirements may raise the requested value.
		 * @code{.cpp}
		 * #include <cxxlens/search.hpp>
		 * int main(){cxxlens::search_report<cxxlens::call_site> r;(void)r.precision();return 0;}
		 * @endcode */
		[[nodiscard]] precision_level precision() const noexcept;
		/** @brief Render the normalized query and execution-accounting explanation. @retval value
		 * Deterministic prose. @pre None. @post Report is unchanged. @note Prose is not a
		 * control-flow input.
		 * @code{.cpp}
		 * #include <cxxlens/search.hpp>
		 * int main(){cxxlens::search_report<cxxlens::call_site>
		 * r;(void)r.query_explanation();return 0;}
		 * @endcode */
		[[nodiscard]] std::string query_explanation() const;
		/** @brief Serialize authoritative rows and derived summaries. @retval value Canonical
		 * search report v1 JSON. @pre Bound reports validate. @post Report is unchanged. @note
		 * Evidence, coverage, variants, guarantee, and unresolved rows are preserved.
		 * @code{.cpp}
		 * #include <cxxlens/search.hpp>
		 * int main(){cxxlens::search_report<cxxlens::call_site> r;return r.to_json().empty()?1:0;}
		 * @endcode */
		[[nodiscard]] std::string to_json() const;
		/** @brief Render a human projection of the same authoritative rows. @retval value
		 * Deterministic Markdown. @pre Bound reports validate. @post Report is unchanged. @note
		 * Partial coverage and unresolved state are never hidden.
		 * @code{.cpp}
		 * #include <cxxlens/search.hpp>
		 * int main(){cxxlens::search_report<cxxlens::call_site> r;return
		 * r.to_markdown().empty()?1:0;}
		 * @endcode */
		[[nodiscard]] std::string to_markdown() const;

	  private:
		explicit search_report(std::shared_ptr<const detail::search_report_data<T>> data);
		std::shared_ptr<const detail::search_report_data<T>> data_;
		friend struct detail::search_report_access;
	};

	extern template class search_report<call_site>;
	extern template class search_report<inheritance_edge>;
	extern template class search_report<override_edge>;
} // namespace cxxlens

namespace cxxlens::search
{
	/** @brief Execute a typed call selector. @param[in] workspace Immutable workspace handle.
	 * @param[in] selector Call selector. @param[in] options Search controls. @retval value
	 * Validated report. @retval unexpected(error-code) Invalid input, provisioning, cancellation,
	 * or strict coverage failure. @pre Workspace and selector are valid. @post Workspace facts may
	 * be warmed; user source is unchanged. @note Uses the shared staged query engine and virtual
	 * resolver.
	 * @code{.cpp}
	 * #include <cxxlens/search.hpp>
	 * int main(){return 0;}
	 * @endcode */
	[[nodiscard]] result<search_report<call_site>>
	calls(const workspace& workspace,
		  select::call_selector selector = select::any_call(),
		  search_options options = {});
	/** @brief Find calls to one qualified free-function input. @param[in] workspace Workspace
	 * handle.
	 * @param[in] qualified_name Resolver input, not identity. @param[in] options Search controls.
	 * @retval value Validated call report. @retval unexpected(error-code) Name/input,
	 * provisioning, or execution failure. @pre Qualified name is non-empty. @post User source is
	 * unchanged. @note Overloads remain semantic candidates.
	 * @code{.cpp}
	 * #include <cxxlens/search.hpp>
	 * int main(){return 0;}
	 * @endcode */
	[[nodiscard]] result<search_report<call_site>> calls_to_function(const workspace& workspace,
																	 std::string qualified_name,
																	 search_options options = {});
	/** @brief Find method calls for a receiver type and method input. @param[in] workspace
	 * Workspace handle. @param[in] receiver_type Resolver input type name. @param[in] method_name
	 * Method input.
	 * @param[in] options Search controls. @retval value Polymorphism-aware call report. @retval
	 * unexpected(error-code) Input, provisioning, or execution failure. @pre Names are non-empty.
	 * @post User source is unchanged. @note Static target and possible virtual candidates remain
	 * separate.
	 * @code{.cpp}
	 * #include <cxxlens/search.hpp>
	 * int main(){return 0;}
	 * @endcode */
	[[nodiscard]] result<search_report<call_site>> calls_to_method(const workspace& workspace,
																   std::string receiver_type,
																   std::string method_name,
																   search_options options = {});
	/** @brief Query direct or transitive derived inheritance edges. @param[in] workspace Workspace
	 * handle. @param[in] record Root record selector. @param[in] transitive Include descendant
	 * closure. @param[in] options Search controls. @retval value Validated inheritance report.
	 * @retval unexpected(error-code) Input, provisioning, or strict coverage failure. @pre Symbol
	 * selector is valid. @post User source is unchanged. @note Edge evidence remains authoritative.
	 * @code{.cpp}
	 * #include <cxxlens/search.hpp>
	 * int main(){return 0;}
	 * @endcode */
	[[nodiscard]] result<search_report<inheritance_edge>>
	inheritance(const workspace& workspace,
				select::symbol_selector record,
				bool transitive = true,
				search_options options = {});
	/** @brief Query direct override edges around selected methods. @param[in] workspace Workspace
	 * handle. @param[in] method Method selector. @param[in] reverse Select overriding methods when
	 * true, overridden methods otherwise. @param[in] options Search controls. @retval value
	 * Validated override report. @retval unexpected(error-code) Input, provisioning, or strict
	 * coverage failure. @pre Symbol selector is valid. @post User source is unchanged. @note No
	 * name-only override inference is performed.
	 * @code{.cpp}
	 * #include <cxxlens/search.hpp>
	 * int main(){return 0;}
	 * @endcode */
	[[nodiscard]] result<search_report<override_edge>> overrides(const workspace& workspace,
																 select::symbol_selector method,
																 bool reverse = false,
																 search_options options = {});

	/** @brief Search semantic symbols in one provisioned immutable snapshot. */
	[[nodiscard]] result<search_report<symbol>>
	symbols(const workspace& workspace,
			select::symbol_selector selector = select::any_symbol(),
			search_options options = {});
	/** @brief Search detached semantic reference rows without collapsing missing facts to empty. */
	[[nodiscard]] result<search_report<reference>> references(const workspace& workspace,
															  select::reference_selector selector,
															  search_options options = {});
	/** @brief Search include relations while preserving resolution and variant state. */
	[[nodiscard]] result<search_report<include_relation>>
	includes(const workspace& workspace,
			 select::include_selector selector = {},
			 search_options options = {});
	/** @brief Search macro expansions with definition and expansion origins kept distinct. */
	[[nodiscard]] result<search_report<macro_expansion>> macros(const workspace& workspace,
																select::macro_selector selector,
																search_options options = {});
	/** @brief Search versioned conversion facts without pretty-type-string identity. */
	[[nodiscard]] result<search_report<fact>> conversions(const workspace& workspace,
														  select::conversion_selector selector,
														  search_options options = {});
} // namespace cxxlens::search
