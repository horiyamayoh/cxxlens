#include "msvc_source_dependencies.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <ranges>
#include <stdexcept>
#include <utility>

#include "sdk/bounded_json_internal.hpp"

namespace cxxlens::application_analysis_worker
{
	namespace
	{
		[[nodiscard]] sdk::error invalid(std::string field, std::string detail)
		{
			return {"application-analysis.msvc-source-dependencies-invalid",
					std::move(field),
					std::move(detail)};
		}

		[[nodiscard]] sdk::error limit(std::string field, std::string detail)
		{
			return {"application-analysis.msvc-source-dependencies-limit-exceeded",
					std::move(field),
					std::move(detail)};
		}
	} // namespace

	sdk::result<msvc_source_dependencies>
	decode_msvc_source_dependencies(const std::string_view document,
									const std::size_t maximum_sources,
									const std::size_t maximum_string_bytes)
	{
		try
		{
			if (maximum_sources == 0U || maximum_string_bytes == 0U)
				return sdk::unexpected(limit("limits", "zero"));
			sdk::detail::json_limits limits;
			constexpr std::size_t maximum_document_bytes = std::size_t{16U} * 1024U * 1024U;
			const auto string_budget =
				maximum_sources > maximum_document_bytes / maximum_string_bytes
				? maximum_document_bytes
				: maximum_sources * maximum_string_bytes;
			limits.max_input_bytes = maximum_document_bytes;
			limits.max_depth = 8U;
			limits.max_array_elements = maximum_sources;
			limits.max_object_members = 32U;
			limits.max_string_bytes = maximum_string_bytes;
			limits.max_total_string_bytes = string_budget;
			limits.max_total_values =
				maximum_sources > std::numeric_limits<std::size_t>::max() - 64U
				? std::numeric_limits<std::size_t>::max()
				: maximum_sources + 64U;
			sdk::detail::json_parse_contract contract;
			contract.error_code = "application-analysis.msvc-source-dependencies-invalid";
			contract.error_field = "document";
			auto parsed = sdk::detail::parse_json_value(document, limits, contract);
			if (!parsed)
				return sdk::unexpected(std::move(parsed.error()));

			const auto* version = parsed->member("Version");
			const auto* data = parsed->member("Data");
			const auto* version_text = version == nullptr ? nullptr : version->as_string();
			if (version_text == nullptr || (*version_text != "1.1" && *version_text != "1.2"))
				return sdk::unexpected(invalid("Version", "unsupported"));
			if (data == nullptr || data->as_object() == nullptr)
				return sdk::unexpected(invalid("Data", "object-required"));
			const auto* source = data->member("Source");
			const auto* includes = data->member("Includes");
			const auto* source_text = source == nullptr ? nullptr : source->as_string();
			const auto* include_values = includes == nullptr ? nullptr : includes->as_array();
			if (source_text == nullptr || source_text->empty())
				return sdk::unexpected(invalid("Data.Source", "nonempty-string-required"));
			if (include_values == nullptr)
				return sdk::unexpected(invalid("Data.Includes", "array-required"));
			if (include_values->size() > maximum_sources - 1U)
				return sdk::unexpected(limit("Data.Includes", "count"));

			msvc_source_dependencies output;
			output.source = *source_text;
			output.includes.reserve(include_values->size());
			for (std::size_t index{}; index < include_values->size(); ++index)
			{
				const auto* include = (*include_values)[index].as_string();
				if (include == nullptr || include->empty())
					return sdk::unexpected(
						invalid("Data.Includes[" + std::to_string(index) + "]", "string-required"));
				output.includes.push_back(*include);
			}
			std::ranges::sort(output.includes);
			if (std::ranges::adjacent_find(output.includes) != output.includes.end())
				return sdk::unexpected(invalid("Data.Includes", "duplicate"));
			return output;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(limit("document", "allocation"));
		}
		catch (const std::length_error&)
		{
			return sdk::unexpected(limit("document", "allocation-length"));
		}
	}
} // namespace cxxlens::application_analysis_worker
