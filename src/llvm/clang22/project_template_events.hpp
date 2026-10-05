#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <cxxlens/sdk/common.hpp>

namespace clang
{
	class Sema;
} // namespace clang

namespace cxxlens::detail::clang22
{
	struct template_event_limits
	{
		std::size_t maximum_operations{8'000'000U};
		std::size_t maximum_events{100'000U};
		std::size_t maximum_retained_bytes{64U * 1024U * 1024U};
		std::function<bool()> cancelled;
	};
	enum class template_event_source_role
	{
		declaration,
		expression
	};
	struct template_event_source
	{
		std::uint32_t begin{}, end{};
		template_event_source_role role{template_event_source_role::expression};
	};
	struct template_candidate_event
	{
		template_event_source source;
		std::string candidate_usr, route, deduction_result, disposition;
		std::optional<unsigned> overload_failure;
		std::optional<bool> is_system;
		std::size_t ordinal{};
		bool completed{}, binding_complete{};
	};
	struct constant_evaluation_root
	{
		unsigned mode{};
		bool completed{}, constant_context{}, potential_check{}, bytecode{}, fold_failure{};
		std::vector<std::size_t> calls;
	};
	struct constant_lifetime_step
	{
		// Actual compiler designator interpretation, not a spelling/type-name guess.
		std::string kind, member_usr;
		std::uint64_t index{};
		bool virtual_base{};
		friend bool operator==(const constant_lifetime_step&,
							   const constant_lifetime_step&) = default;
	};
	struct constant_evaluated_occurrence
	{
		template_event_source source, lifetime_source;
		std::vector<std::string> callee_usrs;
		std::string owner_usr, lifetime_usr, expression_kind;
		std::vector<constant_lifetime_step> lifetime_path;
		std::optional<bool> is_system;
		unsigned kind{}, depth{};
		std::size_t ordinal{};
		bool binding_complete{};
		std::vector<std::size_t> roots;
	};
	/** Detached compiler observations. No compiler pointer leaves the parser job.
	 */
	struct project_template_event_observations
	{
		std::vector<template_candidate_event> candidates;
		std::vector<constant_evaluation_root> roots;
		std::vector<constant_evaluated_occurrence> calls;
		std::size_t operations{}, retained_bytes{};
		bool hooks_installed{}, frozen{}, candidate_census_complete{}, evaluation_census_complete{};
		std::string candidate_reason, evaluation_reason;
	};
	/** Requires exact statically instrumented compiler TUs; no stock-DSO fallback.
	 * Install from original InitializeSema and freeze before extractor callbacks.
	 */
	class project_template_event_scope
	{
	  public:
		struct implementation;

	  private:
		std::unique_ptr<implementation> state_;

	  public:
		project_template_event_scope(clang::Sema&,
									 project_template_event_observations&,
									 template_event_limits = {});
		~project_template_event_scope();
		project_template_event_scope(const project_template_event_scope&) = delete;
		project_template_event_scope& operator=(const project_template_event_scope&) = delete;
		[[nodiscard]] sdk::result<void> freeze(bool parser_completed, bool fatal_diagnostics);
	};
} // namespace cxxlens::detail::clang22
