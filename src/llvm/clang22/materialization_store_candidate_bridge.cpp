#include "materialization_store_candidate_bridge.hpp"

#include <memory>
#include <new>
#include <utility>

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		[[nodiscard]] sdk::error bridge_error(std::string field, std::string detail)
		{
			return {"materialization.store-bridge-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::error bridge_io_error(std::string field,
												 const materialization_io_failure& failure)
		{
			return {"materialization.store-bridge-io",
					std::move(field),
					std::to_string(static_cast<unsigned>(failure.operation))};
		}

		[[nodiscard]] sdk::result<std::unique_ptr<bounded_store_record_spool>> make_record_spool()
		{
			auto storage = make_materialization_private_spool();
			if (!storage)
				return sdk::unexpected(bridge_io_error("record-spool", storage.error()));
			return make_bounded_store_record_spool(std::move(*storage));
		}

		class publication_adapter final : public bounded_store_publication_port
		{
		  public:
			explicit publication_adapter(std::function<bounded_store_publication_terminal(
											 std::string_view, std::string_view)> publish)
				: publish_{std::move(publish)}
			{
			}

			[[nodiscard]] bounded_store_publication_terminal
			publish_once(const std::string_view candidate_id,
						 const std::string_view expected_head) override
			{
				if (called_ || !publish_)
					return bounded_store_publication_terminal::publication_outcome_unknown;
				called_ = true;
				return publish_(candidate_id, expected_head);
			}

		  private:
			std::function<bounded_store_publication_terminal(std::string_view, std::string_view)>
				publish_;
			bool called_{};
		};

		[[nodiscard]] sdk::result<std::unique_ptr<materialization_private_spool>>
		make_report_storage()
		{
			auto storage = make_materialization_private_spool();
			if (!storage)
				return sdk::unexpected(bridge_io_error("report-spool", storage.error()));
			std::unique_ptr<materialization_private_spool> base{std::move(*storage)};
			return base;
		}
	} // namespace

	sdk::result<materialization_store_candidate_bridge_result>
	run_materialization_store_candidate_bridge(
		materialization_store_candidate_bridge_request request)
	{
		if (request.staging_session_id.empty() || !request.replay_tasks ||
			!request.build_expected_projection || !request.build_actual_projection ||
			!request.write_publication_independent_report || !request.write_exact_outcome_report ||
			request.external_census.task_count == 0U ||
			request.external_census.input_digest.empty())
			return sdk::unexpected(bridge_error("request", "incomplete"));
		if (request.limits.max_tasks == 0U || request.limits.max_aggregate_bytes == 0U ||
			request.limits.max_record_bytes == 0U || request.limits.max_spool_bytes == 0U ||
			request.limits.report_tail_bytes == 0U || request.limits.max_report_bytes == 0U)
			return sdk::unexpected(bridge_error("limits", "zero"));

		auto input = make_record_spool();
		auto expected = make_record_spool();
		auto actual = make_record_spool();
		if (!input || !expected || !actual)
			return sdk::unexpected(!input		   ? std::move(input.error())
									   : !expected ? std::move(expected.error())
												   : std::move(actual.error()));

		auto candidate = begin_bounded_store_candidate(std::move(request.staging_session_id),
													   std::move(request.expected_head),
													   request.limits,
													   std::move(*input),
													   std::move(*expected),
													   std::move(*actual),
													   std::move(request.cleanup));
		if (!candidate)
			return sdk::unexpected(std::move(candidate.error()));

		const auto consume_task = [&](const std::span<const std::byte> task) -> sdk::result<void>
		{
			return candidate->append_task(task);
		};
		if (auto replayed = request.replay_tasks(consume_task); !replayed)
		{
			candidate->abort();
			return sdk::unexpected(std::move(replayed.error()));
		}
		if (auto sealed = candidate->seal_input(request.external_census); !sealed)
		{
			candidate->abort();
			return sdk::unexpected(std::move(sealed.error()));
		}
		if (auto expected_built =
				candidate->build_expected_projection(std::move(request.build_expected_projection));
			!expected_built)
		{
			candidate->abort();
			return sdk::unexpected(std::move(expected_built.error()));
		}
		if (auto actual_built =
				candidate->build_actual_projection(std::move(request.build_actual_projection));
			!actual_built)
		{
			candidate->abort();
			return sdk::unexpected(std::move(actual_built.error()));
		}
		if (auto compared = candidate->compare_projections(); !compared)
		{
			candidate->abort();
			return sdk::unexpected(std::move(compared.error()));
		}

		std::unique_ptr<materialization_private_spool> report_storage =
			std::move(request.report_storage);
		if (!report_storage)
		{
			auto created = make_report_storage();
			if (!created)
			{
				candidate->abort();
				return sdk::unexpected(std::move(created.error()));
			}
			report_storage = std::move(*created);
		}
		auto report = make_bounded_store_report_writer(std::move(report_storage), request.limits);
		if (!report)
		{
			candidate->abort();
			return sdk::unexpected(std::move(report.error()));
		}
		if (auto reserved = candidate->reserve_report_tail(*report); !reserved)
		{
			candidate->abort();
			return sdk::unexpected(std::move(reserved.error()));
		}
		if (auto written = request.write_publication_independent_report(*report); !written)
		{
			candidate->abort();
			return sdk::unexpected(std::move(written.error()));
		}

		const bool has_publication = static_cast<bool>(request.publish_once);
		publication_adapter adapter{std::move(request.publish_once)};
		sdk::result<void> terminal = has_publication ? candidate->publish_once(adapter)
													 : candidate->finish_without_publication();
		std::optional<sdk::error> publication_error;
		if (!terminal)
			publication_error = terminal.error();
		if (!terminal && !candidate->publication_terminal())
		{
			candidate->abort();
			return sdk::unexpected(std::move(*publication_error));
		}
		const auto terminal_value = candidate->publication_terminal();
		if (!terminal_value)
			return sdk::unexpected(bridge_error("publication", "terminal-missing"));
		if (auto written = request.write_exact_outcome_report(*report, *terminal_value); !written)
			return sdk::unexpected(std::move(written.error()));
		if (auto finalized = candidate->finalize_report(*report); !finalized)
			return sdk::unexpected(std::move(finalized.error()));

		materialization_store_candidate_bridge_result output{candidate->phase(),
															 candidate->publication_terminal(),
															 true,
															 candidate->cleanup_failed()};
		if (publication_error)
			return sdk::unexpected(std::move(*publication_error));
		return output;
	}
} // namespace cxxlens::detail::clang22::materialization
