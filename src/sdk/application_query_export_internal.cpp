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
			json_value::array_type queries;
			queries.reserve(sorted.size());
			std::size_t retained_bytes{};
			json_limits limits;
			limits.max_input_bytes = maximum_bytes;
			limits.max_array_elements = 1000000U;
			limits.max_total_string_bytes = maximum_bytes;
			limits.max_total_values = maximum_application_query_export_bytes;
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
				if (logical_bytes.size() > maximum_bytes - retained_bytes)
					return unexpected(failure("output", "byte-limit"));
				retained_bytes += logical_bytes.size();
				if (result_bytes.size() > maximum_bytes - retained_bytes)
					return unexpected(failure("output", "byte-limit"));
				retained_bytes += result_bytes.size();
				auto ir = parse_json_value(logical_bytes, limits);
				auto result = parse_json_value(result_bytes, limits);
				auto relation = json_value::string(id);
				if (!ir || !result || !relation)
					return unexpected(!ir ? std::move(ir.error())
										  : !result ? std::move(result.error())
													: std::move(relation.error()));
				auto entry = json_value::object({{"logical_ir", std::move(*ir)},
												 {"relation_id", std::move(*relation)},
												 {"result", std::move(*result)}});
				if (!entry)
					return unexpected(std::move(entry.error()));
				queries.push_back(std::move(*entry));
			}
			auto schema = json_value::string("cxxlens.application-query-results.v1");
			auto snapshot_id = json_value::string(std::string{snapshot.id()});
			if (!schema || !snapshot_id)
				return unexpected(!schema ? std::move(schema.error())
										  : std::move(snapshot_id.error()));
			auto root = json_value::object({{"queries", json_value::array(std::move(queries))},
											{"schema", std::move(*schema)},
											{"snapshot_id", std::move(*snapshot_id)}});
			if (!root)
				return unexpected(std::move(root.error()));
			auto encoded = canonical_json_line(*root);
			if (encoded.size() > maximum_bytes)
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
