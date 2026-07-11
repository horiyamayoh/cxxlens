#pragma once

/** @file finding.hpp @brief Diagnostic observation and stable semantic finding contract。 */

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/core/evidence.hpp>

namespace cxxlens
{
	/** @brief Diagnostic/finding severity。 */
	enum class severity : std::uint8_t
	{
		note,
		info,
		warning,
		error,
		fatal,
	};

	/** @brief External compiler/tool observation; never implicitly a finding。 */
	struct diagnostic
	{
		/** @brief Producer-owned stable observation ID。 */
		std::string id;
		/** @brief Display prose; never semantic control flow。 */
		std::string message;
		/** @brief Observed severity。 */
		severity level{severity::warning};
		/** @brief Primary normalized location。 */
		std::optional<source_span> primary;
		/** @brief Canonically sorted related locations。 */
		std::vector<source_span> related;
		/** @brief Compiler option such as `-Wconversion`。 */
		std::optional<std::string> compiler_option;

		/** @brief Diagnostic field invariantsを検証する。
		 * @retval value Validならsuccess。 @pre なし。 @post Objectを変更しない。
		 * @note Message substring is never inspected for control flow。
		 * @code{.cpp}
		 * #include <cxxlens/core/finding.hpp>
		 * int main(){cxxlens::diagnostic d;d.id="clang.warning";return d.validate()?0:1;}
		 * @endcode */
		[[nodiscard]] result<void> validate() const;
	};

	/** @brief Identity and material payload used to construct one finding。 */
	struct finding_input
	{
		/** @brief Namespaced rule or recipe ID; identity-relevant。 */
		std::string rule_or_recipe;
		/** @brief Full typed semantic subject ID; identity-relevant。 */
		std::string subject_semantic_id;
		/** @brief Primary semantic source key/range; identity-relevant。 */
		source_span primary;
		/** @brief Build variant signature; identity-relevant。 */
		std::optional<std::string> variant_signature;
		/** @brief Explicit identity-relevant parameters。 */
		std::map<std::string, std::string> identity_parameters;
		/** @brief Display severity; excluded from ID。 */
		severity level{severity::warning};
		/** @brief Result confidence。 */
		confidence certainty{confidence::possible};
		/** @brief Soundness guarantee。 */
		result_guarantee guarantee{result_guarantee::best_effort};
		/** @brief Display prose; excluded from ID。 */
		std::string message;
		/** @brief Structured authoritative evidence。 */
		evidence why;
		/** @brief Preserved partial semantic uncertainty。 */
		std::vector<unresolved> unresolved_items;
		/** @brief Authoritative coverage accounting。 */
		coverage_report coverage;
		/** @brief Achieved precision for guarantee validation。 */
		precision_level achieved_precision{precision_level::ast_structural};
		/** @brief Required precision for guarantee validation。 */
		precision_level required_precision{precision_level::ast_structural};
	};

	/** @brief Independent finding validator registries。 */
	struct finding_validation_context
	{
		/** @brief Evidence snapshot/factory registry。 */
		evidence_validation_context evidence_context;
		/** @brief Sorted unique capability registry for unresolved rows。 */
		std::vector<std::string> capabilities;
	};

	/** @brief Structured, renderer-neutral explanation input。 */
	struct finding_explanation_input
	{
		/** @brief Stable finding ID。 */
		finding_id id;
		/** @brief Rule/recipe ID。 */
		std::string rule_or_recipe;
		/** @brief Structured evidence rows。 */
		std::span<const evidence_item> evidence_items;
		/** @brief Preserved unresolved rows。 */
		std::span<const unresolved> unresolved_items;
		/** @brief Authoritative coverage rows。 */
		std::span<const coverage_unit> coverage_units;
	};

	/** @brief Stable semantic result about user code。 */
	class finding
	{
	  public:
		/** @brief Validated inputからstable findingを作る。
		 * @param[in] input Identity/material fields。 @param[in] context Independent registries。
		 * @retval value Valid finding or structured invariant error。
		 * @pre Identity fields are raw input。 @post Message/severity do not affect ID。
		 * @note Absolute roots and diagnostic prose are rejected/excluded。
		 * @code{.cpp}
		 * #include <cxxlens/core/finding.hpp>
		 * int main(){cxxlens::finding_input i;auto r=cxxlens::finding::make(i,{});return r?1:0;}
		 * @endcode */
		[[nodiscard]] static result<finding> make(finding_input input,
												  const finding_validation_context& context);

		/** @brief Full stable finding IDを返す。
		 * @retval value Typed full ID。 @pre Object is factory-created。 @post 変更しない。
		 * @note Message/severity are excluded from identity。
		 * @code{.cpp}
		 * #include <cxxlens/core/finding.hpp>
		 * int main(){return cxxlens::finding{}.id().empty()?0:1;}
		 * @endcode */
		[[nodiscard]] finding_id id() const;

		/** @brief Rule/recipe IDを返す。
		 * @retval value Stable rule/recipe view。 @pre なし。 @post 変更しない。
		 * @note Canonical orderingのfirst key。
		 * @code{.cpp}
		 * #include <cxxlens/core/finding.hpp>
		 * int main(){return cxxlens::finding{}.rule_or_recipe().empty()?0:1;}
		 * @endcode */
		[[nodiscard]] std::string_view rule_or_recipe() const noexcept;

		/** @brief Severityを返す。
		 * @retval value Display/gate severity。 @pre なし。 @post 変更しない。
		 * @note Severity is not identity-relevant。
		 * @code{.cpp}
		 * #include <cxxlens/core/finding.hpp>
		 * int main(){return cxxlens::finding{}.level()==cxxlens::severity::warning?0:1;}
		 * @endcode */
		[[nodiscard]] severity level() const noexcept;

		/** @brief Confidenceを返す。
		 * @retval value Typed confidence。 @pre なし。 @post 変更しない。
		 * @note Filtering uses enum order, not prose。
		 * @code{.cpp}
		 * #include <cxxlens/core/finding.hpp>
		 * int main(){return cxxlens::finding{}.certainty()==cxxlens::confidence::possible?0:1;}
		 * @endcode */
		[[nodiscard]] confidence certainty() const noexcept;

		/** @brief Guaranteeを返す。
		 * @retval value Typed result guarantee。 @pre なし。 @post 変更しない。
		 * @note Validator checks evidence/coverage compatibility。
		 * @code{.cpp}
		 * #include <cxxlens/core/finding.hpp>
		 * int main(){return
		 * cxxlens::finding{}.guarantee()==cxxlens::result_guarantee::best_effort?0:1;}
		 * @endcode */
		[[nodiscard]] result_guarantee guarantee() const noexcept;

		/** @brief Primary normalized sourceを返す。
		 * @retval value Source reference。 @pre Factory-created object。 @post 変更しない。
		 * @note Source semantic key/range is identity-relevant。
		 * @code{.cpp}
		 * #include <cxxlens/core/finding.hpp>
		 * int main(){cxxlens::finding f;return f.primary_location().validate()?0:1;}
		 * @endcode */
		[[nodiscard]] const source_span& primary_location() const noexcept;

		/** @brief Display messageを返す。
		 * @retval value Prose view。 @pre なし。 @post 変更しない。
		 * @note Message is excluded from ID/control flow。
		 * @code{.cpp}
		 * #include <cxxlens/core/finding.hpp>
		 * int main(){return cxxlens::finding{}.message().empty()?0:1;}
		 * @endcode */
		[[nodiscard]] std::string_view message() const noexcept;

		/** @brief Structured evidenceを返す。
		 * @retval value Authoritative evidence reference。 @pre なし。 @post 変更しない。
		 * @note Filters/deduplication preserve it exactly。
		 * @code{.cpp}
		 * #include <cxxlens/core/finding.hpp>
		 * int main(){return cxxlens::finding{}.why().items().empty()?0:1;}
		 * @endcode */
		[[nodiscard]] const evidence& why() const noexcept;

		/** @brief Preserved unresolved rowsを返す。
		 * @retval value Immutable unresolved span。 @pre なし。 @post 変更しない。
		 * @note Empty and unresolved remain distinct through coverage/result。
		 * @code{.cpp}
		 * #include <cxxlens/core/finding.hpp>
		 * int main(){return cxxlens::finding{}.unresolved_items().empty()?0:1;}
		 * @endcode */
		[[nodiscard]] std::span<const unresolved> unresolved_items() const noexcept;

		/** @brief Authoritative coverageを返す。
		 * @retval value Coverage reference。 @pre なし。 @post 変更しない。
		 * @note Filters preserve accounting rows。
		 * @code{.cpp}
		 * #include <cxxlens/core/finding.hpp>
		 * int main(){return cxxlens::finding{}.coverage().complete()?0:1;}
		 * @endcode */
		[[nodiscard]] const coverage_report& coverage() const noexcept;

		/** @brief Renderer-neutral explanation inputを返す。
		 * @retval value Views over ID/evidence/unresolved/coverage。 @pre なし。 @post 変更しない。
		 * @note Rendering itself is owned downstream。
		 * @code{.cpp}
		 * #include <cxxlens/core/finding.hpp>
		 * int main(){return cxxlens::finding{}.explanation_input().evidence_items.empty()?0:1;}
		 * @endcode */
		[[nodiscard]] finding_explanation_input explanation_input() const noexcept;

		/** @brief Full finding invariantを再検証する。
		 * @param[in] context Independent registries。 @retval value Validならsuccess。
		 * @pre なし。 @post Objectを変更しない。 @note ID is recomputed and compared。
		 * @code{.cpp}
		 * #include <cxxlens/core/finding.hpp>
		 * int main(){return cxxlens::finding{}.validate({})?1:0;}
		 * @endcode */
		[[nodiscard]] result<void> validate(const finding_validation_context& context) const;

		/** @brief Canonical full material payload semanticsを返す。
		 * @retval value Canonical conflict-comparison payload。 @pre Valid finding。 @post
		 * 変更しない。
		 * @note Message/severity are material conflict fields but ID-excluded。
		 * @code{.cpp}
		 * #include <cxxlens/core/finding.hpp>
		 * int main(){return cxxlens::finding{}.semantic_representation().empty()?1:0;}
		 * @endcode */
		[[nodiscard]] std::string semantic_representation() const;

	  private:
		finding_id id_;
		std::string rule_or_recipe_;
		std::string subject_semantic_id_;
		source_span primary_;
		std::optional<std::string> variant_signature_;
		std::map<std::string, std::string> identity_parameters_;
		severity level_{severity::warning};
		confidence certainty_{confidence::possible};
		result_guarantee guarantee_{result_guarantee::best_effort};
		std::string message_;
		evidence why_;
		std::vector<unresolved> unresolved_items_;
		coverage_report coverage_;
		precision_level achieved_precision_{precision_level::ast_structural};
		precision_level required_precision_{precision_level::ast_structural};

		friend class finding_set;
	};

	/** @brief Canonically ordered, conflict-checked finding collection。 */
	class finding_set
	{
	  public:
		/** @brief Insert one finding or reject conflicting duplicate。
		 * @param[in] value Finding row。 @retval value Inserted/equivalent duplicateならsuccess。
		 * @pre Value is validated by producer。 @post Canonical total order is preserved。
		 * @note Same ID with different material payload is a hard invariant error。
		 * @code{.cpp}
		 * #include <cxxlens/core/finding.hpp>
		 * int main(){cxxlens::finding_set s;return s.add(cxxlens::finding{})?1:0;}
		 * @endcode */
		[[nodiscard]] result<void> add(finding value);

		/** @brief Number of unique findingsを返す。
		 * @retval value Row count。 @pre なし。 @post 変更しない。 @note Derived from rows。
		 * @code{.cpp}
		 * #include <cxxlens/core/finding.hpp>
		 * int main(){return cxxlens::finding_set{}.size()==0?0:1;}
		 * @endcode */
		[[nodiscard]] std::size_t size() const noexcept;

		/** @brief Empty collectionか返す。
		 * @retval value No rowsならtrue。 @pre なし。 @post 変更しない。
		 * @note Operation failure/partiality is represented by result/coverage, not this bool。
		 * @code{.cpp}
		 * #include <cxxlens/core/finding.hpp>
		 * int main(){return cxxlens::finding_set{}.empty()?0:1;}
		 * @endcode */
		[[nodiscard]] bool empty() const noexcept;

		/** @brief Canonical total-order rowsを返す。
		 * @retval value Immutable finding span。 @pre なし。 @post 変更しない。
		 * @note Order is rule/source/subject/variant/full ID。
		 * @code{.cpp}
		 * #include <cxxlens/core/finding.hpp>
		 * int main(){return cxxlens::finding_set{}.all().empty()?0:1;}
		 * @endcode */
		[[nodiscard]] std::span<const finding> all() const noexcept;

		/** @brief Pure minimum-confidence filter。
		 * @param[in] minimum Inclusive threshold。 @retval value New canonical set。
		 * @pre Threshold enum is valid。 @post Source set remains unchanged。
		 * @note Full finding payload is copied without reconstruction。
		 * @code{.cpp}
		 * #include <cxxlens/core/finding.hpp>
		 * int main(){cxxlens::finding_set s;return
		 * s.minimum_confidence(cxxlens::confidence::high).empty()?0:1;}
		 * @endcode */
		[[nodiscard]] finding_set minimum_confidence(confidence minimum) const;

		/** @brief Pure minimum-severity filter。
		 * @param[in] minimum Inclusive threshold。 @retval value New canonical set。
		 * @pre Threshold enum is valid。 @post Source set remains unchanged。
		 * @note Evidence/unresolved/coverage are preserved exactly。
		 * @code{.cpp}
		 * #include <cxxlens/core/finding.hpp>
		 * int main(){cxxlens::finding_set s;return
		 * s.minimum_severity(cxxlens::severity::error).empty()?0:1;}
		 * @endcode */
		[[nodiscard]] finding_set minimum_severity(severity minimum) const;

	  private:
		std::vector<finding> rows_;
	};
} // namespace cxxlens
