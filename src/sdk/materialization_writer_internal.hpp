#pragma once

/**
 * @file materialization_writer_internal.hpp
 * @brief Compiler-neutral, single-path Store publication for validated materialization results.
 */

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "materialization_task_internal.hpp"

namespace cxxlens::sdk::detail
{
	/** One immutable Store partition admitted from a host capture or provider result. */
	struct materialization_writer_partition
	{
		partition_draft draft;
		partition_manifest manifest;
		snapshot_partition_binding binding;
	};

	/** The sole semantic source accepted by the materialization Store writer. */
	class validated_materialization_publication_source
	{
	  public:
		validated_materialization_publication_source(
			validated_materialization_publication_source&&) noexcept = default;
		validated_materialization_publication_source&
		operator=(validated_materialization_publication_source&&) noexcept = default;
		validated_materialization_publication_source(
			const validated_materialization_publication_source&) = default;
		validated_materialization_publication_source&
		operator=(const validated_materialization_publication_source&) = default;

		[[nodiscard]] const materialization_publication_requirement& authority() const noexcept
		{
			return authority_;
		}
		[[nodiscard]] std::string_view materialization_request_id() const noexcept
		{
			return materialization_request_id_;
		}
		[[nodiscard]] std::string_view task_id() const noexcept
		{
			return task_id_;
		}
		[[nodiscard]] std::string_view task_input_digest() const noexcept
		{
			return task_input_digest_;
		}
		[[nodiscard]] std::string_view result_digest() const noexcept
		{
			return result_digest_;
		}
		[[nodiscard]] std::string_view source_receipt_digest() const noexcept
		{
			return source_receipt_digest_;
		}
		[[nodiscard]] materialization_terminal terminal() const noexcept
		{
			return terminal_;
		}
		[[nodiscard]] std::span<const materialization_writer_partition> partitions() const noexcept
		{
			return partitions_;
		}
		[[nodiscard]] std::span<const closure_candidate> closures() const noexcept
		{
			return closures_;
		}

		[[nodiscard]] result<void> validate(const relation_engine& engine) const;

	  private:
		validated_materialization_publication_source(
			materialization_publication_requirement authority,
			std::string materialization_request_id,
			std::string task_id,
			std::string task_input_digest,
			std::string result_digest,
			std::string source_receipt_digest,
			materialization_terminal terminal,
			std::vector<materialization_writer_partition> partitions,
			std::vector<closure_candidate> closures)
			: authority_{std::move(authority)},
			  materialization_request_id_{std::move(materialization_request_id)},
			  task_id_{std::move(task_id)}, task_input_digest_{std::move(task_input_digest)},
			  result_digest_{std::move(result_digest)},
			  source_receipt_digest_{std::move(source_receipt_digest)}, terminal_{terminal},
			  partitions_{std::move(partitions)}, closures_{std::move(closures)}
		{
		}

		materialization_publication_requirement authority_;
		std::string materialization_request_id_;
		std::string task_id_;
		std::string task_input_digest_;
		std::string result_digest_;
		std::string source_receipt_digest_;
		materialization_terminal terminal_;
		std::vector<materialization_writer_partition> partitions_;
		std::vector<closure_candidate> closures_;

		friend result<validated_materialization_publication_source>
		make_materialization_publication_source(const relation_engine&,
												const validated_materialization_task&,
												const validated_materialization_result&,
												std::span<const partition_draft>,
												std::string);
		friend result<struct materialization_store_publication>
		publish_materialization_source(const relation_engine&,
									   snapshot_store&,
									   validated_materialization_publication_source,
									   const std::optional<std::string>&);
		friend result<validated_materialization_publication_source>
		combine_materialization_publication_sources(
			const relation_engine&, std::vector<validated_materialization_publication_source>);
	};

	[[nodiscard]] result<validated_materialization_publication_source>
	make_materialization_publication_source(const relation_engine& engine,
											const validated_materialization_task& task,
											const validated_materialization_result& result,
											std::span<const partition_draft> host_partitions,
											std::string source_receipt_digest);

	/**
	 * Combine independently validated unit sources into one prepublication transaction source.
	 * Every source must carry the same public request and Store authority. No Store effect occurs.
	 */
	[[nodiscard]] result<validated_materialization_publication_source>
	combine_materialization_publication_sources(
		const relation_engine& engine,
		std::vector<validated_materialization_publication_source> sources);

	struct materialization_store_publication
	{
		snapshot_handle snapshot;
		materialization_publication_requirement authority;
		std::string materialization_request_id;
		std::string task_id;
		std::string task_input_digest;
		std::string result_digest;
		std::string source_receipt_digest;
		materialization_terminal terminal{materialization_terminal::failed};
		bool publication_verified{};
	};

	[[nodiscard]] result<materialization_store_publication>
	publish_materialization_source(const relation_engine& engine,
								   snapshot_store& store,
								   validated_materialization_publication_source source,
								   const std::optional<std::string>& v6_sqlite_path = std::nullopt);
} // namespace cxxlens::sdk::detail
