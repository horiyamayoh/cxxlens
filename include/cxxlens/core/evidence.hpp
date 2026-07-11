#pragma once

/** @file evidence.hpp @brief Structured evidence、coverage accounting、guarantee contract。 */

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/core/failure.hpp>
#include <cxxlens/core/identity.hpp>

namespace cxxlens
{
	/** @brief Confidence category for a semantic result。 */
	enum class confidence : std::uint8_t
	{
		speculative,
		possible,
		probable,
		high,
		certain,
	};

	/** @brief Soundness/completeness guarantee category。 */
	enum class result_guarantee : std::uint8_t
	{
		exact_within_coverage,
		sound_over_approximation,
		sound_under_approximation,
		best_effort,
		heuristic,
	};

	/** @brief Typed evidence category。 */
	// Integrated design section 10.2 fixes the public underlying type.
	enum class evidence_kind : std::uint16_t // NOLINT(performance-enum-size)
	{
		source,
		ast_binding,
		canonical_symbol,
		canonical_type,
		inheritance_relation,
		override_relation,
		call_resolution,
		control_flow,
		dataflow_path,
		api_model,
		dynamic_observation,
		build_context,
		approximation,
		exclusion,
		custom,
	};

	/** @brief One structured evidence row。Summary is a prose projection。 */
	struct evidence_item
	{
		/** @brief Typed evidence category。 */
		evidence_kind kind{};
		/** @brief Human-facing, non-semantic prose。 */
		std::string summary;
		/** @brief Normalized source when the evidence is source-bound。 */
		std::optional<source_span> source;
		/** @brief Stable fact IDs supporting this row。 */
		std::vector<fact_id> supporting_facts;
		/** @brief Typed extension and version attributes。 */
		std::map<std::string, std::string> attributes;
	};

	/** @brief Independent evidence validation inputs。 */
	struct evidence_validation_context
	{
		/** @brief Sorted unique facts present in the acquired snapshot。 */
		std::vector<fact_id> snapshot_facts;
		/** @brief Sorted unique namespaced custom evidence factories。 */
		std::vector<std::string> custom_factories;
	};

	/** @brief Canonically ordered structured evidence rows。 */
	class evidence
	{
	  public:
		/** @brief Add and canonicalize one authoritative row。
		 * @param[in] item Evidence row。 @retval value This builder。
		 * @pre Item may be unvalidated。 @post Rows are in canonical semantic order。
		 * @note Semantic duplicates retain the lexically smallest prose summary。
		 * @code{.cpp}
		 * #include <cxxlens/core/evidence.hpp>
		 * int main(){cxxlens::evidence e;cxxlens::evidence_item
		 * i;i.kind=cxxlens::evidence_kind::source; e.add(i);return e.items().size()==1?0:1;}
		 * @endcode */
		evidence& add(evidence_item item);

		/** @brief Union another evidence set idempotently。
		 * @param[in] other Source rows。 @retval value This builder。
		 * @pre Both sets may be unvalidated。 @post Rows are canonical and semantic duplicates
		 * collapse。
		 * @note Insertion and worker completion order do not affect semantic rows。
		 * @code{.cpp}
		 * #include <cxxlens/core/evidence.hpp>
		 * int main(){cxxlens::evidence a,b;a.merge(b);return a.items().empty()?0:1;}
		 * @endcode */
		evidence& merge(const evidence& other);

		/** @brief Access authoritative canonical rows。
		 * @retval value Immutable row span。 @pre なし。 @post Objectを変更しない。
		 * @note Summary counters are never authoritative。
		 * @code{.cpp}
		 * #include <cxxlens/core/evidence.hpp>
		 * int main(){return cxxlens::evidence{}.items().empty()?0:1;}
		 * @endcode */
		[[nodiscard]] std::span<const evidence_item> items() const noexcept;

		/** @brief Row invariants and snapshot membershipを検証する。
		 * @param[in] context Independent snapshot/factory registry。 @retval value
		 * Validならsuccess。
		 * @pre Context registries are expected sorted unique。 @post Objectを変更しない。
		 * @note Raw prose alone is never valid evidence。
		 * @code{.cpp}
		 * #include <cxxlens/core/evidence.hpp>
		 * int main(){return cxxlens::evidence{}.validate({})?0:1;}
		 * @endcode */
		[[nodiscard]] result<void> validate(const evidence_validation_context& context) const;

		/** @brief Prose-independent semantic rowsを返す。
		 * @retval value Canonical framed representation。 @pre `validate()` succeeds。
		 * @post Objectを変更しない。 @note Summary wording is excluded。
		 * @code{.cpp}
		 * #include <cxxlens/core/evidence.hpp>
		 * int main(){return cxxlens::evidence{}.semantic_representation().empty()?1:0;}
		 * @endcode */
		[[nodiscard]] std::string semantic_representation() const;

		/** @brief Authoritative rowsからcanonical JSONを射影する。
		 * @retval value Versioned JSON。 @pre Rows are valid。 @post Objectを変更しない。
		 * @note Summary/count is derived only from rows。
		 * @code{.cpp}
		 * #include <cxxlens/core/evidence.hpp>
		 * int main(){return
		 * cxxlens::evidence{}.to_json().find("evidence.v1")!=std::string::npos?0:1;}
		 * @endcode */
		[[nodiscard]] std::string to_json() const;

		/** @brief Authoritative rowsからMarkdownを射影する。
		 * @retval value Human-readable rows。 @pre Rows are valid。 @post Objectを変更しない。
		 * @note Prose is not a control-flow input。
		 * @code{.cpp}
		 * #include <cxxlens/core/evidence.hpp>
		 * int main(){return cxxlens::evidence{}.to_markdown().empty()?0:1;}
		 * @endcode */
		[[nodiscard]] std::string to_markdown() const;

	  private:
		std::vector<evidence_item> items_;
	};

	/** @brief Terminal coverage state。 */
	enum class coverage_state : std::uint8_t
	{
		covered,
		excluded,
		failed,
		unresolved,
		not_applicable,
	};

	/** @brief One requested semantic unit key。 */
	struct coverage_request
	{
		/** @brief Namespaced unit kind。 */
		std::string kind;
		/** @brief Stable unit identifier。 */
		std::string id;
	};

	/** @brief Exactly one terminal classification for a requested unit。 */
	struct coverage_unit
	{
		/** @brief Namespaced unit kind。 */
		std::string kind;
		/** @brief Stable unit identifier。 */
		std::string id;
		/** @brief Terminal accounting state。 */
		coverage_state state{coverage_state::unresolved};
		/** @brief Stable reason code for non-covered states。 */
		std::optional<std::string> reason;
	};

	/** @brief Requested-universe accounting derived only from authoritative rows。 */
	class coverage_report
	{
	  public:
		/** @brief Add one requested unit。
		 * @param[in] request Requested key。 @retval value This builder。
		 * @pre Key may be unvalidated。 @post Requests are canonical; duplicates remain
		 * detectable。
		 * @note Request universe is independent from terminal rows。
		 * @code{.cpp}
		 * #include <cxxlens/core/evidence.hpp>
		 * int main(){cxxlens::coverage_report r;r.request({"file","f"});return
		 * r.requested().size()==1?0:1;}
		 * @endcode */
		coverage_report& request(coverage_request request);

		/** @brief Add one terminal classification row。
		 * @param[in] unit Terminal row。 @retval value This builder。
		 * @pre Row may be unvalidated。 @post Rows are canonical; duplicates remain detectable。
		 * @note A validator rejects unrequested or multi-state rows。
		 * @code{.cpp}
		 * #include <cxxlens/core/evidence.hpp>
		 * int main(){cxxlens::coverage_report
		 * r;r.classify({"file","f",cxxlens::coverage_state::covered}); return
		 * r.units().size()==1?0:1;}
		 * @endcode */
		coverage_report& classify(coverage_unit unit);

		/** @brief Union worker-local requested units and rows。
		 * @param[in] other Worker-local report。 @retval value This builder。
		 * @pre Reports may be unvalidated。 @post Canonical order is restored。
		 * @note Duplicate ownership is retained for validator rejection。
		 * @code{.cpp}
		 * #include <cxxlens/core/evidence.hpp>
		 * int main(){cxxlens::coverage_report a,b;a.merge(b);return a.units().empty()?0:1;}
		 * @endcode */
		coverage_report& merge(const coverage_report& other);

		/** @brief Full accounting equationを検証する。
		 * @retval value Every request has exactly one valid rowならsuccess。
		 * @pre なし。 @post Objectを変更しない。
		 * @note Duplicate/missing/multi-state/unrequested units are hard errors。
		 * @code{.cpp}
		 * #include <cxxlens/core/evidence.hpp>
		 * int main(){return cxxlens::coverage_report{}.validate()?0:1;}
		 * @endcode */
		[[nodiscard]] result<void> validate() const;

		/** @brief Failed/unresolved rowがない完全会計か返す。
		 * @retval value Valid and no failed/unresolved rowならtrue。 @pre なし。
		 * @post Objectを変更しない。 @note Empty requested universe is complete。
		 * @code{.cpp}
		 * #include <cxxlens/core/evidence.hpp>
		 * int main(){return cxxlens::coverage_report{}.complete()?0:1;}
		 * @endcode */
		[[nodiscard]] bool complete() const;

		/** @brief Canonical requested universeを返す。
		 * @retval value Immutable request span。 @pre なし。 @post Objectを変更しない。
		 * @note Summary counts are not stored。
		 * @code{.cpp}
		 * #include <cxxlens/core/evidence.hpp>
		 * int main(){return cxxlens::coverage_report{}.requested().empty()?0:1;}
		 * @endcode */
		[[nodiscard]] std::span<const coverage_request> requested() const noexcept;

		/** @brief Canonical authoritative terminal rowsを返す。
		 * @retval value Immutable row span。 @pre なし。 @post Objectを変更しない。
		 * @note Serializers consume only these rows and requested universe。
		 * @code{.cpp}
		 * #include <cxxlens/core/evidence.hpp>
		 * int main(){return cxxlens::coverage_report{}.units().empty()?0:1;}
		 * @endcode */
		[[nodiscard]] std::span<const coverage_unit> units() const noexcept;

		/** @brief State rowsを数える。
		 * @param[in] state Terminal state。 @retval value Authoritative row count。
		 * @pre なし。 @post Objectを変更しない。 @note No mutable counter exists。
		 * @code{.cpp}
		 * #include <cxxlens/core/evidence.hpp>
		 * int main(){return
		 * cxxlens::coverage_report{}.count(cxxlens::coverage_state::covered)==0?0:1;}
		 * @endcode */
		[[nodiscard]] std::size_t count(coverage_state state) const noexcept;

		/** @brief Compile-unit covered IDsをcanonical orderで返す。
		 * @retval value Valid typed IDs。 @pre Report is valid。 @post Objectを変更しない。
		 * @note Only kind `compile-unit` is projected。
		 * @code{.cpp}
		 * #include <cxxlens/core/evidence.hpp>
		 * int main(){return cxxlens::coverage_report{}.covered_compile_units().empty()?0:1;}
		 * @endcode */
		[[nodiscard]] std::vector<compile_unit_id> covered_compile_units() const;

		/** @brief Compile-unit failed IDsをcanonical orderで返す。
		 * @retval value Valid typed IDs。 @pre Report is valid。 @post Objectを変更しない。
		 * @note Only kind `compile-unit` is projected。
		 * @code{.cpp}
		 * #include <cxxlens/core/evidence.hpp>
		 * int main(){return cxxlens::coverage_report{}.failed_compile_units().empty()?0:1;}
		 * @endcode */
		[[nodiscard]] std::vector<compile_unit_id> failed_compile_units() const;

		/** @brief Requested universeとauthoritative rowsからJSONを射影する。
		 * @retval value Versioned canonical JSON。 @pre Report is valid。 @post
		 * Objectを変更しない。
		 * @note Counts are derived from rows in this call。
		 * @code{.cpp}
		 * #include <cxxlens/core/evidence.hpp>
		 * int main(){return
		 * cxxlens::coverage_report{}.to_json().find("coverage.v1")!=std::string::npos?0:1;}
		 * @endcode */
		[[nodiscard]] std::string to_json() const;

		/** @brief Authoritative rowsからMarkdownを射影する。
		 * @retval value Human-readable accounting table。 @pre Report is valid。
		 * @post Objectを変更しない。 @note Counts are derived, never stored。
		 * @code{.cpp}
		 * #include <cxxlens/core/evidence.hpp>
		 * int main(){return cxxlens::coverage_report{}.to_markdown().empty()?1:0;}
		 * @endcode */
		[[nodiscard]] std::string to_markdown() const;

	  private:
		std::vector<coverage_request> requested_;
		std::vector<coverage_unit> units_;
	};

	/** @brief Guarantee, precision, coverage and evidence compatibilityを検証する。
	 * @param[in] guarantee Claimed guarantee。 @param[in] achieved Achieved precision。
	 * @param[in] required Required precision。 @param[in] coverage Authoritative accounting。
	 * @param[in] why Structured evidence。 @retval value Compatibleならsuccess。
	 * @pre Coverage/evidence are independently valid。 @post Inputsを変更しない。
	 * @note Exactness cannot coexist with missing coverage, precision or evidence。
	 * @code{.cpp}
	 * #include <cxxlens/core/evidence.hpp>
	 * int main(){cxxlens::coverage_report c;cxxlens::evidence e;return
	 * cxxlens::validate_result_contract(cxxlens::result_guarantee::best_effort,
	 * cxxlens::precision_level::ast_structural,cxxlens::precision_level::ast_structural,c,e)?0:1;}
	 * @endcode */
	[[nodiscard]] result<void> validate_result_contract(result_guarantee guarantee,
														precision_level achieved,
														precision_level required,
														const coverage_report& coverage,
														const evidence& why);
} // namespace cxxlens
