#pragma once

#include <span>

#include <cxxlens/facts.hpp>
#include <cxxlens/workspace.hpp>

namespace cxxlens::detail
{
	struct fact_profile_access
	{
		[[nodiscard]] static std::span<const fact_kind> kinds(const fact_profile& value) noexcept
		{
			return value.kinds_;
		}

		[[nodiscard]] static precision_level precision(const fact_profile& value) noexcept
		{
			return value.precision_;
		}
	};

	struct workspace_value_access
	{
		enum class scope_kind : std::uint8_t
		{
			all,
			files,
			compile_units,
			changed_files,
		};

		[[nodiscard]] static scope_kind kind(const analysis_scope& value) noexcept
		{
			return static_cast<scope_kind>(value.kind_);
		}

		[[nodiscard]] static std::span<const path> files(const analysis_scope& value) noexcept
		{
			return value.files_;
		}

		[[nodiscard]] static std::span<const compile_unit_id>
		units(const analysis_scope& value) noexcept
		{
			return value.units_;
		}

		[[nodiscard]] static const std::optional<std::vector<build_variant_id>>&
		variants(const analysis_scope& value) noexcept
		{
			return value.variants_;
		}

		[[nodiscard]] static bool includes_headers(const analysis_scope& value) noexcept
		{
			return value.include_headers_;
		}
	};
} // namespace cxxlens::detail
