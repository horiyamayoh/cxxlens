#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <cxxlens/core.hpp>
#include <cxxlens/select.hpp>
#include <cxxlens/workspace.hpp>

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

namespace cxxlens::explain
{
	/** @brief Requested human or agent explanation detail. */
	enum class detail_level : std::uint8_t
	{
		/** Summary plus bounded representative rows. */
		summary,
		/** Default human-readable explanation detail. */
		normal,
		/** Expanded human-readable evidence and unresolved detail. */
		verbose,
		/** Coding-agent-oriented actions and contract properties. */
		agent
	};

	/** @brief Authoritative structured explanation with pure projections. */
	struct explanation
	{
		/** Stable presentation title. */
		std::string title;
		/** Human summary derived from the structured rows. */
		std::string summary;
		/** Canonically ordered evaluation or guidance steps. */
		std::vector<std::string> steps;
		/** Structured evidence rows supporting the explanation. */
		std::vector<evidence_item> evidence;
		/** Structured unresolved prerequisites and partial states. */
		std::vector<unresolved> unresolved_items;
		/** Bounded remediation or follow-up actions. */
		std::vector<std::string> suggested_actions;
		/** Stable machine-readable scalar properties. */
		std::map<std::string, std::string> properties;

		/** @brief Render this explanation as deterministic Markdown. @retval value Human
		 * projection.
		 * @pre Rows may be empty but must remain value-owned. @post Explanation is unchanged. @note
		 * Properties and unresolved prerequisites are not omitted.
		 * @code{.cpp}
		 * #include <cxxlens/explain.hpp>
		 * int main(){cxxlens::explain::explanation e;return e.to_markdown().empty()?1:0;}
		 * @endcode */
		[[nodiscard]] std::string to_markdown() const;
		/** @brief Serialize this explanation as canonical JSON. @retval value Explanation v1 JSON.
		 * @pre Structured rows contain valid public values. @post Explanation is unchanged. @note
		 * Summaries are derived from the same rows used by Markdown.
		 * @code{.cpp}
		 * #include <cxxlens/explain.hpp>
		 * int main(){cxxlens::explain::explanation e;return e.to_json().empty()?1:0;}
		 * @endcode */
		[[nodiscard]] std::string to_json() const;
	};

	/** @brief Explain one normalized selector without executing it. @param[in] value Selector
	 * value. @param[in] level Detail level. @retval value Structured explanation. @pre Selector is
	 * valid. @post Selector is unchanged. @note Required facts and precision remain explicit.
	 * @code{.cpp}
	 * #include <cxxlens/explain.hpp>
	 * int main(){auto
	 * e=cxxlens::explain::selector(cxxlens::select::semantic(cxxlens::select::any_call()));return
	 * e.title.empty();}
	 * @endcode */
	[[nodiscard]] explanation selector(const select::semantic_selector& value,
									   detail_level level = detail_level::normal);
	/** @brief Explain one finding's evidence and unresolved state. @param[in] value Finding value.
	 * @param[in] level Detail level. @retval value Structured explanation. @pre Finding is valid.
	 * @post Finding is unchanged. @note Finding prose is not used as control flow.
	 * @code{.cpp}
	 * #include <cxxlens/explain.hpp>
	 * int main(){return 0;}
	 * @endcode */
	[[nodiscard]] explanation finding(const cxxlens::finding& value,
									  detail_level level = detail_level::normal);
	/** @brief Explain requested-universe coverage. @param[in] value Coverage report. @param[in]
	 * level Detail level. @retval value Structured explanation. @pre Coverage validates. @post
	 * Coverage is unchanged. @note Empty coverage and unresolved coverage remain distinguishable.
	 * @code{.cpp}
	 * #include <cxxlens/explain.hpp>
	 * int main(){cxxlens::coverage_report c;return cxxlens::explain::coverage(c).title.empty();}
	 * @endcode */
	[[nodiscard]] explanation coverage(const coverage_report& value,
									   detail_level level = detail_level::normal);
	/** @brief Execute a selector and explain rejected and unresolved candidates. @param[in]
	 * workspace Workspace handle. @param[in] selector_value Selector to evaluate. @param[in] scope
	 * Requested scope.
	 * @param[in] level Detail level. @param[in] execution Cancellation/deadline controls. @retval
	 * value Structured predicate accounting and bounded examples. @retval unexpected(error-code)
	 * Invalid selector domain, provisioning, or execution failure. @pre M2 supports call-domain
	 * selectors. @post Workspace facts may be warmed; user source is unchanged. @note Predicate
	 * rejection is separate from missing coverage and open-world uncertainty.
	 * @code{.cpp}
	 * #include <cxxlens/explain.hpp>
	 * int main(){return 0;}
	 * @endcode */
	[[nodiscard]] result<explanation> why_not_matched(const workspace& workspace,
													  select::semantic_selector selector_value,
													  analysis_scope scope = analysis_scope::all(),
													  detail_level level = detail_level::normal,
													  execution_context execution = {});

	/** @brief Stable coding-agent guidance derived from a public selector contract. */
	struct agent_task_card
	{
		/** Concrete task objective derived from the selector. */
		std::string goal;
		/** Public headers required to implement the task. */
		std::vector<std::string> required_headers;
		/** Public API entry points and result accessors to use. */
		std::vector<std::string> api_calls;
		/** Workspace, fact, and semantic preconditions. */
		std::vector<std::string> preconditions;
		/** Authoritative result dimensions the task must preserve. */
		std::vector<std::string> expected_outputs;
		/** Stable failure codes and semantic failure modes. */
		std::vector<std::string> failure_modes;
		/** Deterministic validation steps for the completed task. */
		std::vector<std::string> verification_steps;

		/** @brief Render agent guidance as deterministic Markdown. @retval value Human/agent card.
		 * @pre Rows may be empty. @post Card is unchanged. @note Shell strings are never generated.
		 * @code{.cpp}
		 * #include <cxxlens/explain.hpp>
		 * int main(){cxxlens::explain::agent_task_card c;return c.to_markdown().empty()?1:0;}
		 * @endcode */
		[[nodiscard]] std::string to_markdown() const;
		/** @brief Serialize agent guidance as canonical JSON. @retval value Agent task card v1
		 * JSON.
		 * @pre Rows may be empty. @post Card is unchanged. @note Verification steps remain
		 * explicit.
		 * @code{.cpp}
		 * #include <cxxlens/explain.hpp>
		 * int main(){cxxlens::explain::agent_task_card c;return c.to_json().empty()?1:0;}
		 * @endcode */
		[[nodiscard]] std::string to_json() const;
	};

	/** @brief Build an agent task card for a selector. @param[in] value Selector contract.
	 * @retval value Headers, API calls, facts, failures, and verification steps. @pre Selector is
	 * valid. @post Selector is unchanged. @note Forbidden name-only and hidden fallback shortcuts
	 * are stated explicitly.
	 * @code{.cpp}
	 * #include <cxxlens/explain.hpp>
	 * int main(){auto
	 * c=cxxlens::explain::for_selector(cxxlens::select::semantic(cxxlens::select::any_call()));return
	 * c.goal.empty();}
	 * @endcode */
	[[nodiscard]] agent_task_card for_selector(const select::semantic_selector& value);

	/** @brief Project an already validated edit plan without replanning. */
	[[nodiscard]] explanation edit_plan(const transform::edit_plan& value,
										detail_level level = detail_level::normal);
	/** @brief Project an already validated generation plan without rerunning decisions. */
	[[nodiscard]] explanation generation_plan(const generate::generation_plan& value,
											  detail_level level = detail_level::normal);
	/** @brief Build deterministic agent guidance from authoritative rule metadata. */
	[[nodiscard]] agent_task_card for_rule(const rules::rule& value);
	/** @brief Build deterministic agent guidance from an immutable codemod contract. */
	[[nodiscard]] agent_task_card for_codemod(const transform::codemod& value);
	/** @brief Build deterministic agent guidance from an authoritative generation plan. */
	[[nodiscard]] agent_task_card for_generation(const generate::generation_plan& value);
	/** @brief Return canonical frozen-catalog data without runtime state or sensitive paths. */
	[[nodiscard]] std::string api_catalog_json();
} // namespace cxxlens::explain
