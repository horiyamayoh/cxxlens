#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <cxxlens/facts.hpp>
#include <cxxlens/select.hpp>

namespace cxxlens::detail::graph
{
	enum class dispatch_form : std::uint8_t
	{
		direct,
		nonvirtual_member,
		virtual_open,
		qualified_virtual,
		final_virtual,
		indirect,
		dependent
	};

	enum class resolution_status : std::uint8_t
	{
		exact,
		partial,
		unresolved
	};

	struct variant_call_observation
	{
		build_variant_id variant;
		std::optional<symbol_id> static_target;
		std::vector<symbol_id> observed_candidates;
		dispatch_form form{dispatch_form::dependent};
		bool operator==(const variant_call_observation&) const = default;
	};

	struct candidate_request
	{
		std::string call_key;
		std::optional<symbol_id> static_target;
		std::vector<symbol_id> observed_candidates;
		dispatch_form form{dispatch_form::dependent};
		std::vector<variant_call_observation> variants;
		bool inheritance_coverage_complete{true};
		bool override_coverage_complete{true};
		bool closed_world{};
		bool receiver_is_final{};
		select::variant_match_policy variant_policy{
			select::variant_match_policy::report_per_variant};
		std::size_t candidate_budget{100000U};
		std::stop_token cancellation;
	};

	struct per_variant_candidates
	{
		build_variant_id variant;
		std::vector<symbol_id> candidates;
		bool operator==(const per_variant_candidates&) const = default;
	};

	struct candidate_resolution
	{
		std::string schema{"cxxlens.virtual-candidate-resolution.v1"};
		std::string call_key;
		std::optional<symbol_id> static_target;
		std::vector<symbol_id> possible_targets;
		std::vector<per_variant_candidates> per_variant;
		resolution_status status{resolution_status::unresolved};
		confidence certainty{confidence::possible};
		result_guarantee guarantee{result_guarantee::best_effort};
		evidence why;
		std::vector<unresolved> unresolved_items;

		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] std::string to_json() const;
	};

	class virtual_candidate_resolver
	{
	  public:
		explicit virtual_candidate_resolver(std::vector<override_edge> overrides);
		[[nodiscard]] candidate_resolution resolve(candidate_request request) const;

	  private:
		std::vector<override_edge> overrides_;
	};
} // namespace cxxlens::detail::graph
