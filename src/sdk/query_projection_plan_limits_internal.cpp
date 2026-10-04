#include "query_projection_plan_limits_internal.hpp"

#include <utility>

namespace cxxlens::sdk::query::detail
{
	namespace
	{
		struct plan_failure
		{
			error value;
		};
		[[noreturn]] void fail(std::string_view prefix,
							   std::string_view suffix,
							   std::string field,
							   std::string detail)
		{
			throw plan_failure{
				{std::string{prefix} + std::string{suffix}, std::move(field), std::move(detail)}};
		}
		void cancelled(std::stop_token token, std::string_view prefix)
		{
			if (token.stop_requested())
				fail(prefix, "-cancelled", "projection", "stop-requested");
		}
		void bound_source_plans(const application_query_results& input,
								std::size_t maximum_queries,
								std::size_t maximum_bytes,
								std::string_view prefix,
								std::stop_token cancellation)
		{
			if (input.scans.size() > maximum_queries)
				fail(prefix, "-budget", "source-queries", "limit-exceeded");
			std::size_t bytes{};
			const auto add = [&](std::size_t amount)
			{
				if (bytes > maximum_bytes || amount > maximum_bytes - bytes)
					fail(prefix, "-budget", "source-plan-bytes", "limit-exceeded");
				bytes += amount;
			};
			const auto text_bytes = [&](const std::string& value)
			{
				add(value.size());
				add(1U);
			};
			const auto strings = [&](const std::vector<std::string>& values)
			{
				for (const auto& value : values)
				{
					cancelled(cancellation, prefix);
					add(sizeof(std::string));
					text_bytes(value);
				}
			};
			add(sizeof(application_query_results));
			text_bytes(input.snapshot_id);
			for (const auto& scan : input.scans)
			{
				cancelled(cancellation, prefix);
				add(sizeof(application_relation_scan));
				text_bytes(scan.relation_id);
				text_bytes(scan.logical_ir.root);
				for (const auto& descriptor : scan.logical_ir.relation_requirements)
				{
					cancelled(cancellation, prefix);
					add(sizeof(relation_descriptor));
					for (const auto* value : {&descriptor.id,
											  &descriptor.name,
											  &descriptor.semantics,
											  &descriptor.owner_namespace,
											  &descriptor.contract_canonical,
											  &descriptor.contract_digest,
											  &descriptor.descriptor_digest})
						text_bytes(*value);
					for (const auto& column : descriptor.columns)
					{
						cancelled(cancellation, prefix);
						add(sizeof(column_descriptor));
						text_bytes(column.id);
						text_bytes(column.name);
						text_bytes(column.type.parameter);
					}
					strings(descriptor.key_columns);
					strings(descriptor.conflict_columns);
					strings(descriptor.domain_identity.projection);
					text_bytes(descriptor.domain_identity.contract);
					if (descriptor.domain_identity.result_column)
						text_bytes(*descriptor.domain_identity.result_column);
					for (const auto& reference : descriptor.references)
					{
						add(sizeof(relation_reference_descriptor));
						text_bytes(reference.target_relation);
						strings(reference.source_columns);
						strings(reference.target_columns);
					}
				}
				for (const auto& node : scan.logical_ir.nodes)
				{
					cancelled(cancellation, prefix);
					add(sizeof(ir_node));
					text_bytes(node.id);
					text_bytes(node.operator_id);
					text_bytes(node.arguments);
					strings(node.inputs);
				}
				for (const auto& column : scan.logical_ir.output_schema)
				{
					cancelled(cancellation, prefix);
					add(sizeof(column_ref));
					text_bytes(column.descriptor_id);
					text_bytes(column.column_id);
					text_bytes(column.type.parameter);
					text_bytes(column.source_alias);
				}
			}
		}
	} // namespace

	result<void> check_source_plan_limits(const application_query_results& input,
										  std::size_t maximum_queries,
										  std::size_t maximum_bytes,
										  std::stop_token cancellation,
										  std::string_view prefix)
	{
		try
		{
			bound_source_plans(input, maximum_queries, maximum_bytes, prefix, cancellation);
			return {};
		}
		catch (const plan_failure& failure)
		{
			return failure.value;
		}
	}
} // namespace cxxlens::sdk::query::detail
