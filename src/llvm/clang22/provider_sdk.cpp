#include <algorithm>
#include <cctype>
#include <ranges>
#include <string_view>

#include <cxxlens/provider/clang22.hpp>

#include "source_map_adapter.hpp"

namespace cxxlens::provider::clang22
{
	namespace
	{
		[[nodiscard]] bool address_literal(const std::string_view value)
		{
			if (value.starts_with("native-address:") || value.starts_with("native-pointer:"))
				return true;
			if (!value.starts_with("0x") || value.size() < 10U)
				return false;
			return std::ranges::all_of(value.substr(2U),
									   [](const char byte)
									   {
										   return std::isxdigit(static_cast<unsigned char>(byte)) !=
											   0;
									   });
		}
	} // namespace

	result<source_span> normalize_source(borrowed_translation_unit& unit,
										 const clang::SourceRange& range)
	{
		cxxlens::detail::clang22::source_map_adapter normalizer{unit.compiler(), unit.unit(), {}};
		return normalizer.direct_span(range);
	}

	sdk::result<void> detect_native_escape(const sdk::detached_row& row)
	{
		for (const auto& [column, cell] : row.cells)
		{
			if (column.contains("pointer") || column.contains("address"))
				return cxxlens::sdk::unexpected(
					sdk::error{"native.address-escape", column, "identity-marker"});
			if (!cell.value || !std::holds_alternative<std::string>(*cell.value))
				continue;
			if (address_literal(std::get<std::string>(*cell.value)))
				return cxxlens::sdk::unexpected(
					sdk::error{"native.address-escape", column, "value-marker"});
		}
		return {};
	}
} // namespace cxxlens::provider::clang22
