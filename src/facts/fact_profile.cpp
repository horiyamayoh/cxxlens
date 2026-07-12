#include <algorithm>
#include <array>
#include <ranges>

#include <cxxlens/facts.hpp>

#include "../core/canonical_json.hpp"

namespace cxxlens
{
	namespace
	{
		using detail::json::json_value;
		constexpr std::array all_kinds{fact_kind::file,
									   fact_kind::compile_command,
									   fact_kind::symbol,
									   fact_kind::type,
									   fact_kind::declaration,
									   fact_kind::definition,
									   fact_kind::reference,
									   fact_kind::call,
									   fact_kind::construction,
									   fact_kind::conversion,
									   fact_kind::inheritance,
									   fact_kind::override_relation,
									   fact_kind::include_relation,
									   fact_kind::macro_definition,
									   fact_kind::macro_expansion,
									   fact_kind::cfg_summary,
									   fact_kind::flow_summary,
									   fact_kind::effect_summary,
									   fact_kind::dynamic_observation,
									   fact_kind::coverage_region,
									   fact_kind::custom};
		constexpr std::array kind_names{"file",
										"compile_command",
										"symbol",
										"type",
										"declaration",
										"definition",
										"reference",
										"call",
										"construction",
										"conversion",
										"inheritance",
										"override_relation",
										"include_relation",
										"macro_definition",
										"macro_expansion",
										"cfg_summary",
										"flow_summary",
										"effect_summary",
										"dynamic_observation",
										"coverage_region",
										"custom"};
		constexpr std::array precision_names{"ast_structural",
											 "local_semantic",
											 "workspace_semantic",
											 "local_flow",
											 "interprocedural_summary",
											 "path_sensitive",
											 "dynamic_observation"};

		[[nodiscard]] fact_profile make_profile(std::initializer_list<fact_kind> kinds,
												precision_level precision)
		{
			fact_profile output;
			for (const auto kind : kinds)
				output = output.include(kind);
			return output.precision(precision);
		}
	} // namespace

	fact_profile fact_profile::minimal()
	{
		return make_profile({fact_kind::file, fact_kind::compile_command},
							precision_level::ast_structural);
	}
	fact_profile fact_profile::semantic_search()
	{
		return make_profile({fact_kind::file,
							 fact_kind::compile_command,
							 fact_kind::symbol,
							 fact_kind::type,
							 fact_kind::declaration,
							 fact_kind::definition,
							 fact_kind::reference,
							 fact_kind::call,
							 fact_kind::inheritance,
							 fact_kind::override_relation,
							 fact_kind::include_relation,
							 fact_kind::macro_expansion},
							precision_level::workspace_semantic);
	}
	fact_profile fact_profile::refactor()
	{
		return semantic_search().include(fact_kind::conversion).include(fact_kind::construction);
	}
	fact_profile fact_profile::generation()
	{
		return refactor().include(fact_kind::macro_definition);
	}
	fact_profile fact_profile::flow()
	{
		return semantic_search()
			.include(fact_kind::cfg_summary)
			.include(fact_kind::flow_summary)
			.include(fact_kind::effect_summary)
			.precision(precision_level::local_flow);
	}
	fact_profile fact_profile::full()
	{
		fact_profile output;
		for (const auto kind : all_kinds)
			if (kind != fact_kind::custom)
				output = output.include(kind);
		return output.precision(precision_level::dynamic_observation);
	}
	fact_profile fact_profile::include(const fact_kind kind) const
	{
		auto output = *this;
		const auto position = std::ranges::lower_bound(output.kinds_, kind);
		if (position == output.kinds_.end() || *position != kind)
			output.kinds_.insert(position, kind);
		return output;
	}
	fact_profile fact_profile::exclude(const fact_kind kind) const
	{
		auto output = *this;
		const auto position = std::ranges::lower_bound(output.kinds_, kind);
		if (position != output.kinds_.end() && *position == kind)
			output.kinds_.erase(position);
		return output;
	}
	fact_profile fact_profile::precision(const precision_level value) const
	{
		auto output = *this;
		output.precision_ = value;
		return output;
	}
	std::string fact_profile::to_json() const
	{
		std::vector<std::string> names;
		names.reserve(kinds_.size());
		for (const auto kind : kinds_)
			names.emplace_back(kind_names.at(static_cast<std::size_t>(kind)));
		std::ranges::sort(names);
		json_value::array kinds;
		kinds.reserve(names.size());
		for (auto& name : names)
			kinds.emplace_back(std::move(name));
		json_value document{detail::json::envelope(
			{"cxxlens.fact-profile.v1"},
			{{"kinds", json_value{std::move(kinds)}},
			 {"precision", precision_names.at(static_cast<std::size_t>(precision_))}})};
		return detail::json::write(document).value();
	}
} // namespace cxxlens
