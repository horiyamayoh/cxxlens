#include "application_query_export_internal.hpp"

#include <algorithm>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

#include "bounded_json_internal.hpp"

namespace cxxlens::sdk::detail
{
	namespace
	{
		[[nodiscard]] error failure(std::string field, std::string detail)
		{
			return {
				"application-analysis.query-export-invalid", std::move(field), std::move(detail)};
		}
	} // namespace

	result<std::string> encode_application_queries(const relation_engine& engine,
												   const snapshot_handle& snapshot,
												   const std::span<const std::string> relation_ids,
												   const std::size_t maximum_bytes)
	{
		try
		{
			if (maximum_bytes == 0U || maximum_bytes > maximum_application_query_export_bytes)
				return unexpected(failure("maximum_bytes", "out-of-range"));
			if (relation_ids.empty() || relation_ids.size() > 4096U)
				return unexpected(failure("relation_ids", "count"));
			std::vector<std::string> sorted{relation_ids.begin(), relation_ids.end()};
			std::ranges::sort(sorted);
			if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
				return unexpected(failure("relation_ids", "duplicate"));
			auto runtime = query::reference_engine::bind(snapshot);
			if (!runtime)
				return unexpected(std::move(runtime.error()));
			std::string encoded{"{\"queries\":["};
			const auto append = [&](const std::string_view bytes) -> bool
			{
				if (bytes.size() > maximum_bytes - encoded.size())
					return false;
				encoded.append(bytes);
				return true;
			};
			if (encoded.size() > maximum_bytes)
				return unexpected(failure("output", "byte-limit"));
			bool first = true;
			for (const auto& id : sorted)
			{
				auto descriptor = engine.require_id(id);
				if (!descriptor)
					return unexpected(std::move(descriptor.error()));
				auto builder = query::builder::from(descriptor->descriptor());
				if (!builder)
					return unexpected(std::move(builder.error()));
				const auto logical = std::move(*builder).finish();
				auto executed = runtime->execute(logical);
				if (!executed)
					return unexpected(std::move(executed.error()));
				const auto logical_bytes = logical.canonical_form();
				const auto result_bytes = executed->canonical_form();
				auto relation = json_value::string(id);
				if (!relation)
					return unexpected(std::move(relation.error()));
				if ((!first && !append(",")) || !append("{\"logical_ir\":") ||
					!append(logical_bytes) || !append(",\"relation_id\":") ||
					!append(canonical_json(*relation)) || !append(",\"result\":") ||
					!append(result_bytes) || !append("}"))
					return unexpected(failure("output", "byte-limit"));
				first = false;
			}
			auto snapshot_id = json_value::string(std::string{snapshot.id()});
			if (!snapshot_id)
				return unexpected(std::move(snapshot_id.error()));
			if (!append("],\"schema\":\"cxxlens.application-query-results.v1\",\"snapshot_id\":") ||
				!append(canonical_json(*snapshot_id)) || !append("}\n"))
				return unexpected(failure("output", "byte-limit"));
			return encoded;
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(failure("memory", "allocation"));
		}
		catch (const std::length_error&)
		{
			return unexpected(failure("memory", "allocation-length"));
		}
	}
} // namespace cxxlens::sdk::detail
