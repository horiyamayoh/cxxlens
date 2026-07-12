#include <algorithm>
#include <array>
#include <ranges>
#include <string>

#include <cxxlens/core.hpp>

#include "../store/sqlite/sqlite_api.hpp"
#include "canonical_json.hpp"
#include "capability_access.hpp"

#ifndef CXXLENS_HAS_CLANG22
#define CXXLENS_HAS_CLANG22 0
#endif

namespace cxxlens
{

	std::string semantic_version::to_string() const
	{
		std::string result =
			std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
		if (!prerelease.empty())
		{
			result += '-';
			result += prerelease;
		}
		return result;
	}

	api_versions versions()
	{
		constexpr std::uint16_t schema_major = 1U;
		const semantic_version initial_schema{schema_major, 0U, 0U, {}};
		return api_versions{
			.library = semantic_version{0U, 1U, 0U, {}},
			.public_schema = initial_schema,
			.semantics = initial_schema,
			.fact_schema = initial_schema,
			.finding_schema = initial_schema,
			.edit_plan_schema = initial_schema,
			.generation_plan_schema = initial_schema,
			.llvm = semantic_version{22U, 1U, 8U, {}},
		};
	}

	bool capability_set::has(const std::string_view id) const
	{
		const auto position = std::ranges::lower_bound(values_, id, {}, &capability::id);
		return position != values_.end() && position->id == id &&
			(position->state == capability_state::available ||
			 position->state == capability_state::experimental);
	}

	capability capability_set::get(const std::string_view id) const
	{
		const auto position = std::ranges::lower_bound(values_, id, {}, &capability::id);
		if (position != values_.end() && position->id == id)
			return *position;
		return {std::string{id},
				capability_state::disabled_at_build,
				"Unknown capability",
				"The capability is not registered by this build"};
	}

	std::vector<capability> capability_set::all() const
	{
		return values_;
	}

	std::string capability_set::to_json() const
	{
		constexpr std::array names{"available",
								   "experimental",
								   "disabled_at_build",
								   "unavailable_for_llvm",
								   "unavailable_for_platform"};
		detail::json::json_value::array rows;
		for (const auto& value : values_)
		{
			detail::json::json_value limitation;
			if (value.limitation)
				limitation = *value.limitation;
			rows.emplace_back(detail::json::json_value::object{
				{"id", value.id},
				{"limitation", std::move(limitation)},
				{"state", names.at(static_cast<std::size_t>(value.state))},
				{"summary", value.summary}});
		}
		auto encoded = detail::json::write(detail::json::json_value{detail::json::envelope(
			{"cxxlens.capabilities.v1"}, {{"capabilities", std::move(rows)}})});
		return encoded ? std::move(encoded.value()) : std::string{};
	}

	capability_set capabilities()
	{
		const auto sqlite = detail::store::sqlite::load_api();
		std::vector<capability> values{
			{"extractor.dynamic-observation",
			 capability_state::disabled_at_build,
			 "Dynamic observation importer",
			 "The dynamic QA milestone is not enabled"},
			{"extractor.flow-summary",
			 capability_state::disabled_at_build,
			 "CFG and flow summary extractor",
			 "The flow-analysis milestone is not enabled"},
			{"facts.in-memory", capability_state::available, "In-memory immutable fact store", {}},
			{"facts.sqlite",
			 sqlite ? capability_state::available : capability_state::unavailable_for_platform,
			 "SQLite immutable fact store",
			 sqlite
				 ? std::optional<std::string>{}
				 : std::optional<std::string>{"A compatible SQLite runtime library was not found"}},
			{"workspace.incremental-provisioning",
			 capability_state::available,
			 "Incremental fact requirement provisioning",
			 {}}};
#if CXXLENS_HAS_CLANG22
		values.push_back({"frontend.clang22",
						  capability_state::available,
						  "LLVM/Clang 22 semantic frontend",
						  {}});
#else
		values.push_back({"frontend.clang22",
						  capability_state::disabled_at_build,
						  "LLVM/Clang 22 semantic frontend",
						  "Clang 22 development libraries were not found at configure time"});
#endif
		return detail::capability_set_access::make(std::move(values));
	}

} // namespace cxxlens
