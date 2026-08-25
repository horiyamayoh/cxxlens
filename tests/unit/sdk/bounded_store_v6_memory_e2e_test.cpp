#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "sdk/bounded_store_v6_internal.hpp"
#include "sdk/bounded_store_v6_memory_internal.hpp"
#include "sdk/store_identity_internal.hpp"
#include "store_operation_test_adapter.hpp"

namespace detail = cxxlens::sdk::detail;
using cxxlens::sdk::canonical_binary;
using cxxlens::sdk::canonical_value;
using cxxlens::sdk::content_digest;
using cxxlens::sdk::error;
using cxxlens::sdk::result;
using cxxlens::sdk::unexpected;

namespace
{
	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(EXIT_FAILURE);
		}
	}

	[[nodiscard]] std::array<std::byte, 32U> raw_digest(const std::string_view digest)
	{
		std::array<std::byte, 32U> output{};
		require(digest.starts_with("sha256:") && digest.size() == 71U,
				"digest helper received a non-canonical digest");
		const auto nibble = [](const char value) -> unsigned
		{
			if (value >= '0' && value <= '9')
				return static_cast<unsigned>(value - '0');
			return static_cast<unsigned>(value - 'a' + 10);
		};
		for (std::size_t index{}; index < output.size(); ++index)
			output[index] = static_cast<std::byte>((nibble(digest[7U + index * 2U]) << 4U) |
												   nibble(digest[8U + index * 2U]));
		return output;
	}

	[[nodiscard]] cxxlens::sdk::snapshot_series_selector selector()
	{
		const std::string digest{"sha256:" + std::string(64U, '0')};
		return {"catalog:test",
				"channel:test",
				"engine:test",
				"conditions:test",
				digest,
				digest,
				digest};
	}

	[[nodiscard]] std::vector<std::byte> tuple(const std::string_view value)
	{
		std::vector<canonical_value> values;
		values.push_back(canonical_value::from_string(std::string{value}));
		auto encoded = canonical_binary(canonical_value::from_tuple(std::move(values)));
		require(encoded.has_value(), "canonical tuple could not be encoded");
		return std::move(*encoded);
	}

	[[nodiscard]] std::vector<detail::bounded_store_v6_memory_source_frame>
	partition_frames(const std::string_view partition_name,
					 const std::string_view claim_name,
					 const std::string_view claim_value)
	{
		const auto partition_key = tuple(partition_name);
		const auto partition_payload = tuple("partition-payload");
		const auto claim_key = tuple(claim_name);
		const auto claim_payload = tuple(claim_value);
		std::vector<detail::bounded_store_v6_memory_source_frame> output;
		for (const auto& [kind, key, payload] :
			 std::initializer_list<std::tuple<detail::bounded_store_v6_record_kind,
											  std::vector<std::byte>,
											  std::vector<std::byte>>>{
				 {detail::bounded_store_v6_record_kind::partition_begin,
				  partition_key,
				  partition_payload},
				 {detail::bounded_store_v6_record_kind::claim_occurrence, claim_key, claim_payload},
				 {detail::bounded_store_v6_record_kind::claim_annotation, claim_key, claim_payload},
				 {detail::bounded_store_v6_record_kind::partition_end,
				  partition_key,
				  partition_payload}})
		{
			auto encoded = detail::encode_bounded_store_v6_memory_frame(kind, key, payload);
			require(encoded.has_value(), "memory event frame could not be encoded");
			output.push_back({kind, std::move(*encoded)});
		}
		return output;
	}

	[[nodiscard]] std::vector<detail::bounded_store_v6_memory_source_frame>
	frames(const bool altered_claim = false)
	{
		return partition_frames(
			"partition-0", "claim-0", altered_claim ? "changed-claim" : "claim-payload");
	}

	[[nodiscard]] std::vector<detail::bounded_store_v6_memory_source_frame> two_partition_frames()
	{
		auto output = partition_frames("partition-0", "claim-0", "claim-payload");
		auto second = partition_frames("partition-1", "claim-1", "second-claim-payload");
		output.insert(output.end(), second.begin(), second.end());
		return output;
	}

	struct frame_census
	{
		std::uint64_t partition_count{};
		std::uint64_t event_count{};
		std::uint64_t claim_count{};
		std::uint64_t row_count{};
		std::uint64_t annotation_count{};
		std::uint64_t coverage_count{};
		std::uint64_t unresolved_count{};
		std::uint64_t framed_bytes{};
	};

	[[nodiscard]] frame_census
	count_frames(const std::vector<detail::bounded_store_v6_memory_source_frame>& source)
	{
		frame_census counts;
		for (const auto& frame : source)
		{
			counts.framed_bytes += static_cast<std::uint64_t>(frame.bytes.size());
			++counts.event_count;
			switch (frame.kind)
			{
				case detail::bounded_store_v6_record_kind::partition_begin:
					++counts.partition_count;
					break;
				case detail::bounded_store_v6_record_kind::claim_occurrence:
					++counts.claim_count;
					break;
				case detail::bounded_store_v6_record_kind::detached_row:
					++counts.row_count;
					break;
				case detail::bounded_store_v6_record_kind::claim_annotation:
					++counts.annotation_count;
					break;
				case detail::bounded_store_v6_record_kind::coverage:
					++counts.coverage_count;
					break;
				case detail::bounded_store_v6_record_kind::unresolved:
					++counts.unresolved_count;
					break;
				case detail::bounded_store_v6_record_kind::partition_end:
					break;
			}
		}
		return counts;
	}

	[[nodiscard]] std::vector<std::byte>
	concatenate_frames(const std::vector<detail::bounded_store_v6_memory_source_frame>& source,
					   const std::uint64_t repetition_count = 1U)
	{
		const auto counts = count_frames(source);
		std::vector<std::byte> bytes;
		bytes.reserve(static_cast<std::size_t>(counts.framed_bytes * repetition_count));
		for (std::uint64_t repetition{}; repetition < repetition_count; ++repetition)
			for (const auto& frame : source)
				bytes.insert(bytes.end(), frame.bytes.begin(), frame.bytes.end());
		return bytes;
	}

	[[nodiscard]] detail::bounded_store_v6_external_census
	census(const std::vector<detail::bounded_store_v6_memory_source_frame>& source,
		   const std::uint64_t task_count = 1U,
		   const std::string_view authority_binding = "authority:test-input-v6")
	{
		const auto counts = count_frames(source);
		auto bytes = concatenate_frames(source, task_count);
		const auto digest = content_digest(bytes);
		return {task_count,
				counts.partition_count * task_count,
				counts.event_count * task_count,
				counts.claim_count * task_count,
				counts.row_count * task_count,
				counts.annotation_count * task_count,
				counts.coverage_count * task_count,
				counts.unresolved_count * task_count,
				static_cast<std::uint64_t>(bytes.size()),
				raw_digest(digest),
				std::string{authority_binding}};
	}

	[[nodiscard]] detail::bounded_store_v6_external_census
	repeated_census(const std::vector<detail::bounded_store_v6_memory_source_frame>& source,
					const std::uint64_t task_count)
	{
		return census(source, task_count, "authority:test-input-v6-maximum");
	}

	[[nodiscard]] detail::bounded_store_v6_task_receipt
	task_receipt(const std::vector<detail::bounded_store_v6_memory_source_frame>& source,
				 const std::uint64_t ordinal = 0U,
				 const std::string_view task_id = {})
	{
		const auto counts = count_frames(source);
		const auto bytes = concatenate_frames(source);
		const auto digest = content_digest(bytes);
		const std::string id =
			task_id.empty() ? "task:" + std::to_string(ordinal) : std::string{task_id};
		return {id,
				ordinal,
				counts.partition_count,
				counts.event_count,
				counts.claim_count,
				counts.row_count,
				counts.annotation_count,
				counts.coverage_count,
				counts.unresolved_count,
				counts.framed_bytes,
				raw_digest(digest),
				"task-authority:test-input-v6/" + id};
	}

	[[nodiscard]] std::vector<detail::bounded_store_v6_memory_source_frame>
	repeated_frames(const std::vector<detail::bounded_store_v6_memory_source_frame>& source,
					const std::uint64_t task_count)
	{
		std::vector<detail::bounded_store_v6_memory_source_frame> output;
		output.reserve(static_cast<std::size_t>(task_count * source.size()));
		for (std::uint64_t task{}; task < task_count; ++task)
			output.insert(output.end(), source.begin(), source.end());
		return output;
	}

	struct report_release_state
	{
		std::uint64_t release_calls{};
	};

	class report_writer final : public detail::bounded_store_report_tail_writer
	{
	  public:
		explicit report_writer(std::shared_ptr<report_release_state> release_state = {},
							   const bool invalid_reservation = false,
							   const bool fail_release = false,
							   const bool throw_release = false)
			: invalid_reservation_{invalid_reservation}, fail_release_{fail_release},
			  throw_release_{throw_release}, release_state_{std::move(release_state)}
		{
		}

		result<detail::bounded_store_v6_report_tail_reservation>
		reserve_maximum_tail(const std::uint64_t tail, const std::uint64_t maximum) override
		{
			if (tail != detail::bounded_store_v6_exact_report_tail_bytes ||
				maximum != detail::bounded_store_v6_max_report_bytes)
				return error{"test.report", "tail", "contract"};
			reserved_ = true;
			return detail::bounded_store_v6_report_tail_reservation{"writer:test",
																	"spool:test",
																	"reservation:test",
																	1024U,
																	invalid_reservation_ ? tail - 1U
																						 : tail,
																	maximum,
																	maximum};
		}
		result<void>
		append_terminal(const detail::bounded_store_v6_publication_terminal value) override
		{
			terminal_ = value;
			return reserved_ ? result<void>{} : error{"test.report", "terminal", "unreserved"};
		}
		result<void> validate_full_schema() override
		{
			return terminal_ ? result<void>{} : error{"test.report", "schema", "terminal"};
		}
		result<void>
		validate_complete_section_census(const std::uint32_t exact_section_count) override
		{
			return terminal_ && exact_section_count == detail::bounded_store_v6_report_section_count
				? result<void>{}
				: error{"test.report", "section-census", "mismatch"};
		}
		result<void> validate_bottom_up_bindings() override
		{
			return terminal_ ? result<void>{} : error{"test.report", "bindings", "terminal"};
		}
		result<std::uint64_t> sealed_report_bytes() const override
		{
			return reserved_ && terminal_
				? 1024U + detail::bounded_store_v6_exact_report_tail_bytes
				: result<std::uint64_t>{error{"test.report", "bytes", "not-sealed"}};
		}
		result<void> release() override
		{
			if (release_state_)
				++release_state_->release_calls;
			if (released_)
				return {};
			released_ = true;
			if (throw_release_)
				throw std::runtime_error{"report-release-exception"};
			if (fail_release_)
				return error{"test.report", "release", "fault"};
			return {};
		}

	  private:
		bool reserved_{};
		bool released_{};
		bool invalid_reservation_{};
		bool fail_release_{};
		bool throw_release_{};
		std::optional<detail::bounded_store_v6_publication_terminal> terminal_;
		std::shared_ptr<report_release_state> release_state_;
	};

	struct failing_report_state
	{
		std::uint64_t append_calls{};
		std::uint64_t section_census_calls{};
		std::uint64_t release_calls{};
	};

	class failing_report_writer final : public detail::bounded_store_report_tail_writer
	{
	  public:
		explicit failing_report_writer(std::shared_ptr<failing_report_state> state)
			: state_{std::move(state)}
		{
		}

		result<detail::bounded_store_v6_report_tail_reservation>
		reserve_maximum_tail(const std::uint64_t tail, const std::uint64_t maximum) override
		{
			if (tail != detail::bounded_store_v6_exact_report_tail_bytes ||
				maximum != detail::bounded_store_v6_max_report_bytes)
				return error{"test.report", "tail", "contract"};
			return detail::bounded_store_v6_report_tail_reservation{"writer:failing",
																	"spool:failing",
																	"reservation:failing",
																	1024U,
																	tail,
																	maximum,
																	maximum};
		}
		result<void> append_terminal(detail::bounded_store_v6_publication_terminal) override
		{
			++state_->append_calls;
			return {};
		}
		result<void> validate_full_schema() override
		{
			return {};
		}
		result<void> validate_complete_section_census(std::uint32_t) override
		{
			++state_->section_census_calls;
			return error{"test.report", "section-census", "fault"};
		}
		result<void> validate_bottom_up_bindings() override
		{
			return {};
		}
		result<std::uint64_t> sealed_report_bytes() const override
		{
			return error{"test.report", "bytes", "unreachable"};
		}
		result<void> release() override
		{
			++state_->release_calls;
			return {};
		}

	  private:
		std::shared_ptr<failing_report_state> state_;
	};

	class nonstandard_throwing_source final : public detail::bounded_store_v6_task_frame_source
	{
	  public:
		result<std::optional<detail::bounded_store_v6_record_extent>> next_record() override
		{
			throw 17;
		}
		result<std::size_t> read_record_bytes(std::span<std::byte>) override
		{
			return error{"test.source", "read", "unreachable"};
		}
		result<bool> canonical_order_validated() const override
		{
			return false;
		}
	};

	/** Test-only kind decorator used solely to exercise SQLite metadata binding before publication.
	 */
	class sqlite_kind_backend final : public detail::bounded_store_v6_backend_port
	{
	  public:
		explicit sqlite_kind_backend(std::unique_ptr<detail::bounded_store_v6_backend_port> inner)
			: inner_{std::move(inner)}
		{
		}
		sqlite_kind_backend(std::unique_ptr<detail::bounded_store_v6_backend_port> inner,
							error exact_publish_failure)
			: inner_{std::move(inner)}, exact_publish_failure_{std::move(exact_publish_failure)}
		{
		}
		sqlite_kind_backend(std::unique_ptr<detail::bounded_store_v6_backend_port> inner,
							const bool throw_abort)
			: inner_{std::move(inner)}, throw_abort_{throw_abort}
		{
		}
		detail::bounded_store_v6_backend backend() const noexcept override
		{
			return detail::bounded_store_v6_backend::sqlite;
		}
		result<void> bind_physical_anchor(
			std::shared_ptr<const detail::bounded_store_v6_physical_anchor> anchor) override
		{
			return inner_->bind_physical_anchor(std::move(anchor));
		}
		std::shared_ptr<const detail::bounded_store_v6_physical_anchor>
		physical_anchor() const noexcept override
		{
			return inner_->physical_anchor();
		}
		std::string_view physical_anchor_binding() const noexcept override
		{
			return inner_->physical_anchor_binding();
		}
		result<void> begin_record(const detail::bounded_store_v6_record_extent& extent) override
		{
			return inner_->begin_record(extent);
		}
		result<void> append_record_bytes(std::span<const std::byte> bytes) override
		{
			return inner_->append_record_bytes(bytes);
		}
		result<void> finish_record() override
		{
			return inner_->finish_record();
		}
		result<void> seal_staging() override
		{
			return inner_->seal_staging();
		}
		result<detail::bounded_store_v6_measured_projection> measured_projection() const override
		{
			return inner_->measured_projection();
		}
		result<std::unique_ptr<detail::bounded_store_v6_actual_cursor_source>>
		open_actual_cursor() override
		{
			return inner_->open_actual_cursor();
		}
		result<detail::bounded_store_v6_effect_result> publish_once() override
		{
			if (exact_publish_failure_)
			{
				if (scripted_publish_called_)
					return error{"store.invariant-breach", "test-backend", "publish-replay"};
				scripted_publish_called_ = true;
				// The decorator can only surface an exact failed SDK observation.  It cannot mint
				// a candidate, successful publication, returned handle, or effect receipt.
				return detail::bounded_store_v6_effect_result{
					*exact_publish_failure_, std::nullopt, true, false, false};
			}
			return inner_->publish_once();
		}
		result<detail::bounded_store_v6_reopen_observation> reopen() override
		{
			return inner_->reopen();
		}
		result<void> abort_staging() override
		{
			auto aborted = inner_->abort_staging();
			if (throw_abort_)
				throw std::runtime_error{"backend-abort-exception"};
			return aborted;
		}

	  private:
		std::unique_ptr<detail::bounded_store_v6_backend_port> inner_;
		std::optional<error> exact_publish_failure_;
		bool scripted_publish_called_{};
		bool throw_abort_{};
	};

	struct prepared_case
	{
		std::shared_ptr<cxxlens::test::store_operation_test_adapter> operations{
			std::make_shared<cxxlens::test::store_operation_test_adapter>(
				cxxlens::sdk::make_default_store_operation_port())};
		detail::bounded_store_v6_memory_store store{selector(), operations};
		detail::bounded_store_v6_session_metadata metadata;
		std::vector<detail::bounded_store_v6_memory_source_frame> source_frames{frames()};

		prepared_case()
		{
			metadata.backend = detail::bounded_store_v6_backend::memory;
			metadata.relation_engine_generation = "engine-generation:test";
			metadata.selector = selector();
			metadata.staging_session_id = "staging:test";
			auto head = detail::make_bounded_store_v6_genesis_head(metadata.selector);
			require(head.has_value(), "genesis head could not be made");
			metadata.expected_head = *head;
		}
	};

	[[nodiscard]] result<detail::bounded_store_prepared_publication> stage_with_external_census(
		prepared_case& value,
		std::unique_ptr<detail::bounded_store_v6_backend_port> backend,
		detail::bounded_store_v6_external_census external_census,
		const std::vector<detail::bounded_store_v6_memory_source_frame>& task_frames,
		detail::bounded_store_v6_task_receipt receipt)
	{
		auto session = detail::bounded_store_v6_phase_core::begin_staging_session(value.metadata);
		if (!session)
			return unexpected(std::move(session.error()));
		auto input = detail::bounded_store_v6_phase_core::seal_input(std::move(*session),
																	 std::move(external_census));
		if (!input)
			return unexpected(std::move(input.error()));
		auto prepared = detail::bounded_store_v6_phase_core::prepare_publication(
			std::move(*input), std::move(backend));
		if (!prepared)
			return unexpected(std::move(prepared.error()));
		auto source = detail::make_bounded_store_v6_memory_task_frame_source(task_frames);
		if (!source)
			return unexpected(std::move(source.error()));
		auto task = detail::bounded_store_v6_phase_core::seal_task_source(std::move(receipt),
																		  std::move(*source));
		if (!task)
			return unexpected(std::move(task.error()));
		if (auto staged =
				detail::bounded_store_v6_phase_core::stage_from_source(*prepared, std::move(*task));
			!staged)
			return unexpected(std::move(staged.error()));
		if (auto sealed = detail::bounded_store_v6_phase_core::seal_prepared_publication(*prepared);
			!sealed)
			return unexpected(std::move(sealed.error()));
		return std::move(*prepared);
	}

	[[nodiscard]] result<detail::bounded_store_prepared_publication>
	stage(prepared_case& value, std::unique_ptr<detail::bounded_store_v6_backend_port> backend)
	{
		return stage_with_external_census(value,
										  std::move(backend),
										  census(value.source_frames),
										  value.source_frames,
										  task_receipt(value.source_frames));
	}

	[[nodiscard]] result<detail::bounded_store_validated_publication>
	prepare(prepared_case& value,
			std::unique_ptr<detail::bounded_store_v6_backend_port> backend,
			std::vector<detail::bounded_store_v6_memory_source_frame> expected_frames,
			std::unique_ptr<detail::bounded_store_report_tail_writer> writer =
				std::make_unique<report_writer>())
	{
		auto prepared = stage(value, std::move(backend));
		if (!prepared)
			return unexpected(std::move(prepared.error()));
		auto expected_source = detail::make_bounded_store_v6_memory_expected_semantic_source(
			std::move(expected_frames));
		if (!expected_source)
			return unexpected(std::move(expected_source.error()));
		auto expected = detail::bounded_store_v6_phase_core::seal_expected_projection(
			std::move(*expected_source), *prepared);
		if (!expected)
			return unexpected(std::move(expected.error()));
		auto actual = detail::bounded_store_v6_phase_core::take_physical_actual_cursor(*prepared);
		if (!actual)
			return unexpected(std::move(actual.error()));
		auto match = detail::bounded_store_v6_phase_core::compare_bounded_store_projections(
			*prepared, std::move(*expected), std::move(*actual));
		if (!match)
			return unexpected(std::move(match.error()));
		auto report = detail::bounded_store_v6_phase_core::reserve_report_tail(
			*prepared, *match, std::move(writer));
		if (!report)
			return unexpected(std::move(report.error()));
		auto validated = detail::bounded_store_v6_phase_core::bind_publication(
			std::move(*prepared), std::move(*match), std::move(*report));
		if (!validated)
			return unexpected(std::move(validated.error()));
		return std::move(*validated);
	}

	void finish_report_and_cleanup(detail::bounded_store_terminal_custody& terminal)
	{
		auto release = detail::bounded_store_v6_phase_core::finalize_and_validate_report(terminal);
		require(release.has_value(), "full report validation failed");
		auto cleanup = detail::bounded_store_v6_phase_core::drain(terminal, std::move(*release));
		require(cleanup && cleanup->attempted && cleanup->drained &&
					cleanup->report_release_attempted && cleanup->report_released &&
					cleanup->backend_cleanup_attempted && cleanup->backend_cleanup_drained &&
					!cleanup->report_failure && !cleanup->backend_failure && !cleanup->failure &&
					terminal.cleanup_observation() == *cleanup && terminal.cleanup_drained(),
				"terminal/report cleanup was not a single drained operation");
	}

	void positive_publish_reopen_and_drain()
	{
		prepared_case value;
		auto backend = value.store.make_backend_port(value.metadata);
		require(backend.has_value(), "Memory backend port could not open");
		auto validated = prepare(value, std::move(*backend), value.source_frames);
		require(validated.has_value(), "positive Memory publication could not prepare");
		auto terminal = detail::bounded_store_v6_phase_core::publish_once(std::move(*validated));
		require(terminal.has_value(), "Memory publication failed");
		require(terminal->observation().terminal ==
					detail::bounded_store_v6_publication_terminal::committed_verified,
				"Memory publication was not reopened and verified");
		require(terminal->observation().publish_call_count == 1U &&
					terminal->observation().publication_attempted,
				"publication was not one-shot");
		require(terminal->observation().reopen.factory_attempted &&
					terminal->observation().reopen.publication.status ==
						detail::bounded_store_v6_lookup_observation::state::present,
				"fresh Memory reopen observation is incomplete");
		require(!terminal->observation().reopen.fresh_backend_binding.empty() &&
					terminal->observation().reopen.fresh_backend_binding !=
						terminal->observation().physical_anchor_binding,
				"reopen reused the staging physical-anchor binding instead of a fresh traversal");
		require(terminal->observation().reopen.canonical_export_digest.has_value(),
				"fresh Memory export digest is missing");
		require(
			terminal->observation().returned_snapshot &&
				terminal->observation().physical_binary_sha256 !=
					raw_digest(
						terminal->observation().returned_snapshot->semantic_projection_digest) &&
				terminal->observation().physical_binary_sha256 !=
					raw_digest(
						terminal->observation().returned_snapshot->canonical_export_digest) &&
				terminal->observation().returned_snapshot->semantic_projection_digest !=
					terminal->observation().returned_snapshot->canonical_export_digest,
			"physical, semantic, and export identities were not independently derived");
		require(terminal->observation().reopen.current.publication ==
					terminal->observation().returned_publication,
				"reopen returned a different publication");
		finish_report_and_cleanup(*terminal);
		require(value.store.retained_publication_count() == 1U,
				"Memory publication was not retained as product state");
		require(value.store.live_staging_payload_count() == 0U,
				"successful cleanup retained a private staging payload");
	}

	void memory_store_is_fresh_genesis_only()
	{
		prepared_case value;
		auto first_backend = value.store.make_backend_port(value.metadata);
		require(first_backend.has_value(), "fresh Memory backend could not open");
		auto first = prepare(value, std::move(*first_backend), value.source_frames);
		require(first.has_value(), "fresh Memory publication could not prepare");
		auto first_terminal = detail::bounded_store_v6_phase_core::publish_once(std::move(*first));
		require(first_terminal &&
					first_terminal->observation().terminal ==
						detail::bounded_store_v6_publication_terminal::committed_verified,
				"fresh Memory publication did not commit");
		require(first_terminal->observation().returned_publication &&
					first_terminal->observation().returned_snapshot,
				"fresh Memory publication omitted identity metadata");
		const auto first_publication = *first_terminal->observation().returned_publication;
		const auto first_snapshot = *first_terminal->observation().returned_snapshot;
		finish_report_and_cleanup(*first_terminal);

		auto first_head = value.store.current_head();
		require(first_head &&
					first_head->value ==
						detail::bounded_store_v6_expected_head::kind::publication &&
					first_head->publication && first_head->snapshot &&
					*first_head->publication == first_publication &&
					*first_head->snapshot == first_snapshot,
				"current_head did not retain the exact first publication and snapshot");

		value.metadata.expected_head = *first_head;
		value.metadata.staging_session_id = "staging:test:second";
		auto second_backend = value.store.make_backend_port(value.metadata);
		require(!second_backend && second_backend.error().code == "store.invariant-breach" &&
					second_backend.error().field == "backend" &&
					second_backend.error().detail == "memory-genesis-required",
				"Memory append request was admitted after the fresh-genesis publication");
		require(value.store.retained_publication_count() == 1U &&
					value.store.retained_complete_payload_count() == 1U,
				"genesis-only Memory store retained duplicate history or payload F");
	}

	void one_task_can_contain_two_partitions()
	{
		prepared_case value;
		value.source_frames = two_partition_frames();
		const auto receipt = task_receipt(value.source_frames);
		require(receipt.ordinal == 0U && receipt.partition_count == 2U &&
					receipt.event_count == 8U && receipt.claim_count == 2U,
				"two-partition task receipt census was not derived from frames");
		const auto external = census(value.source_frames);
		require(external.task_count == 1U && external.partition_count == 2U &&
					external.event_count == 8U && external.claim_count == 2U,
				"two-partition external census was not derived from frames");
		auto backend = value.store.make_backend_port(value.metadata);
		require(backend.has_value(), "two-partition backend could not open");
		auto validated = prepare(value, std::move(*backend), value.source_frames);
		require(validated.has_value(), "one two-partition task could not prepare");
		auto terminal = detail::bounded_store_v6_phase_core::publish_once(std::move(*validated));
		require(terminal &&
					terminal->observation().terminal ==
						detail::bounded_store_v6_publication_terminal::committed_verified,
				"one two-partition task did not publish");
		require(terminal->observation().returned_snapshot &&
					terminal->observation().returned_snapshot->partition_count == 2U &&
					terminal->observation().returned_snapshot->claim_count == 2U,
				"two-partition snapshot census was not retained");
		finish_report_and_cleanup(*terminal);
		require(value.store.retained_publication_count() == 1U &&
					value.store.retained_complete_payload_count() == 1U,
				"two-partition publication was not atomically retained");
	}

	void annotation_census_is_independent_from_claim_occurrences()
	{
		prepared_case value;
		value.source_frames = frames();
		auto extra = detail::encode_bounded_store_v6_memory_frame(
			detail::bounded_store_v6_record_kind::claim_annotation,
			tuple("claim-1"),
			tuple("second-origin-association"));
		require(extra.has_value(), "independent annotation frame could not encode");
		value.source_frames.insert(
			value.source_frames.end() - 1,
			{detail::bounded_store_v6_record_kind::claim_annotation, std::move(*extra)});
		const auto counts = count_frames(value.source_frames);
		require(counts.claim_count == 1U && counts.annotation_count == 2U &&
					counts.event_count == 5U,
				"annotation fixture did not separate claim and origin-association census");
		auto external = census(value.source_frames);
		require(external.claim_count == 1U && external.annotation_count == 2U,
				"external census collapsed annotation count into claim count");
		auto backend = value.store.make_backend_port(value.metadata);
		require(backend.has_value(), "independent annotation backend could not open");
		auto validated = prepare(value, std::move(*backend), value.source_frames);
		require(validated.has_value(), "independent annotation projection could not prepare");
		auto terminal = detail::bounded_store_v6_phase_core::publish_once(std::move(*validated));
		require(terminal &&
					terminal->observation().terminal ==
						detail::bounded_store_v6_publication_terminal::committed_verified,
				"independent annotation projection did not publish and reopen");
		finish_report_and_cleanup(*terminal);
	}

	void abort_cleanup_fault_is_attempted_once()
	{
		prepared_case value;
		auto backend = value.store.make_backend_port(value.metadata);
		require(backend.has_value(), "cleanup-fault backend could not open");
		auto release_state = std::make_shared<report_release_state>();
		std::size_t observations_before_cleanup{};
		{
			auto validated = prepare(value,
									 std::move(*backend),
									 value.source_frames,
									 std::make_unique<report_writer>(release_state));
			require(validated.has_value(), "cleanup-fault publication could not prepare");
			auto terminal =
				detail::bounded_store_v6_phase_core::publish_once(std::move(*validated));
			require(terminal &&
						terminal->observation().terminal ==
							detail::bounded_store_v6_publication_terminal::committed_verified,
					"cleanup-fault publication did not commit");
			const auto committed_publication = terminal->observation().returned_publication;
			require(committed_publication && value.store.retained_publication_count() == 1U,
					"cleanup-fault publication was not visible before drain");
			auto release =
				detail::bounded_store_v6_phase_core::finalize_and_validate_report(*terminal);
			require(release.has_value(), "cleanup-fault report could not finalize");
			value.operations->inject_next_backend_fault(
				cxxlens::sdk::store_backend_operation::abort_staging,
				cxxlens::sdk::store_backend_observation_fault::backend_failure,
				cxxlens::test::store_operation_ambiguity_side::before_delegate);
			observations_before_cleanup = value.operations->backend_observation_call_count();
			auto cleanup =
				detail::bounded_store_v6_phase_core::drain(*terminal, std::move(*release));
			require(cleanup && cleanup->attempted && !cleanup->drained && cleanup->failure &&
						cleanup->report_release_attempted && cleanup->report_released &&
						cleanup->backend_cleanup_attempted && !cleanup->backend_cleanup_drained &&
						!cleanup->report_failure && cleanup->backend_failure &&
						cleanup->failure->code == "store.invariant-breach" &&
						cleanup->failure->field == "operation-port" &&
						cleanup->failure->detail == "memory-failure",
					"abort cleanup fault was not returned as a typed failure");
			require(terminal->cleanup_observation() == *cleanup,
					"failed backend cleanup was not retained in terminal custody");
			require(value.operations->backend_observation_call_count() ==
						observations_before_cleanup + 1U,
					"abort cleanup fault made an unexpected number of backend attempts");
			require(release_state->release_calls == 1U && !terminal->cleanup_drained() &&
						value.store.retained_publication_count() == 1U &&
						value.store.current_head()->publication == committed_publication,
					"failed cleanup changed publication visibility or retried report release");
			require(value.store.live_staging_payload_count() == 0U,
					"failed cleanup retained the private staging payload");
		}
		// Terminal custody has been destroyed after the failed drain.  The backend's cleanup_called
		// and writer_release_attempted latches must prevent a second abort or release attempt.
		require(release_state->release_calls == 1U &&
					value.operations->backend_observation_call_count() ==
						observations_before_cleanup + 1U,
				"terminal destruction retried failed cleanup or report release");
	}

	void report_release_failure_still_drains_backend_once()
	{
		const auto exercise = [](const bool throw_release)
		{
			prepared_case value;
			auto backend = value.store.make_backend_port(value.metadata);
			require(backend.has_value(), "report-release-fault backend could not open");
			auto release_state = std::make_shared<report_release_state>();
			std::size_t observations_before_cleanup{};
			{
				auto validated = prepare(value,
										 std::move(*backend),
										 value.source_frames,
										 std::make_unique<report_writer>(
											 release_state, false, !throw_release, throw_release));
				require(validated.has_value(),
						"report-release-fault publication could not prepare");
				auto terminal =
					detail::bounded_store_v6_phase_core::publish_once(std::move(*validated));
				require(terminal &&
							terminal->observation().terminal ==
								detail::bounded_store_v6_publication_terminal::committed_verified,
						"report-release-fault publication did not commit");
				auto release =
					detail::bounded_store_v6_phase_core::finalize_and_validate_report(*terminal);
				require(release.has_value(), "report-release-fault report could not finalize");
				observations_before_cleanup = value.operations->backend_observation_call_count();
				auto cleanup =
					detail::bounded_store_v6_phase_core::drain(*terminal, std::move(*release));
				require(
					cleanup && cleanup->attempted && !cleanup->drained &&
						cleanup->report_release_attempted && !cleanup->report_released &&
						cleanup->backend_cleanup_attempted && cleanup->backend_cleanup_drained &&
						cleanup->report_failure && !cleanup->backend_failure && cleanup->failure &&
						terminal->cleanup_observation() == *cleanup && !terminal->cleanup_drained(),
					"report release fault did not retain the complete cleanup ledger");
				if (throw_release)
					require(cleanup->report_failure->code == "store.invariant-breach" &&
								cleanup->report_failure->field == "report-cleanup" &&
								cleanup->report_failure->detail == "report-release-exception",
							"report release exception lost its typed cleanup classification");
				else
					require(*cleanup->report_failure == error{"test.report", "release", "fault"},
							"report release result failure was not preserved exactly");
				require(value.operations->backend_observation_call_count() ==
								observations_before_cleanup + 1U &&
							release_state->release_calls == 1U &&
							value.store.live_staging_payload_count() == 0U &&
							value.store.retained_publication_count() == 1U,
						"report failure stopped backend cleanup or changed publication visibility");
			}
			require(release_state->release_calls == 1U &&
						value.operations->backend_observation_call_count() ==
							observations_before_cleanup + 1U,
					"terminal destruction retried a failed report release or backend cleanup");
		};
		exercise(false);
		exercise(true);
	}

	void cleanup_dual_failure_and_abort_exception_are_retained_once()
	{
		{
			prepared_case value;
			auto backend = value.store.make_backend_port(value.metadata);
			require(backend.has_value(), "dual-cleanup-fault backend could not open");
			auto release_state = std::make_shared<report_release_state>();
			auto validated =
				prepare(value,
						std::move(*backend),
						value.source_frames,
						std::make_unique<report_writer>(release_state, false, true, false));
			require(validated.has_value(), "dual-cleanup-fault publication could not prepare");
			auto terminal =
				detail::bounded_store_v6_phase_core::publish_once(std::move(*validated));
			require(terminal.has_value(), "dual-cleanup-fault publication lost terminal custody");
			auto release =
				detail::bounded_store_v6_phase_core::finalize_and_validate_report(*terminal);
			require(release.has_value(), "dual-cleanup-fault report could not finalize");
			value.operations->inject_next_backend_fault(
				cxxlens::sdk::store_backend_operation::abort_staging,
				cxxlens::sdk::store_backend_observation_fault::backend_failure,
				cxxlens::test::store_operation_ambiguity_side::before_delegate);
			const auto observations_before = value.operations->backend_observation_call_count();
			auto cleanup =
				detail::bounded_store_v6_phase_core::drain(*terminal, std::move(*release));
			require(cleanup && cleanup->attempted && !cleanup->drained &&
						cleanup->report_release_attempted && !cleanup->report_released &&
						cleanup->backend_cleanup_attempted && !cleanup->backend_cleanup_drained &&
						cleanup->report_failure == error{"test.report", "release", "fault"} &&
						cleanup->backend_failure ==
							error{"store.invariant-breach", "operation-port", "memory-failure"} &&
						cleanup->failure == cleanup->report_failure &&
						terminal->cleanup_observation() == *cleanup && !terminal->cleanup_drained(),
					"simultaneous report/backend failures were not retained in one-shot custody");
			require(release_state->release_calls == 1U &&
						value.operations->backend_observation_call_count() ==
							observations_before + 1U &&
						value.store.live_staging_payload_count() == 0U &&
						value.store.retained_publication_count() == 1U,
					"dual cleanup failure retried, retained staging, or hid the publication");
		}

		{
			prepared_case value;
			auto inner = value.store.make_backend_port(value.metadata);
			require(inner.has_value(), "abort-throw backend could not open");
			value.metadata.backend = detail::bounded_store_v6_backend::sqlite;
			value.metadata.exact_sqlite_path = "/tmp/cxxlens-v6-abort-throw.sqlite";
			auto validated = prepare(value,
									 std::make_unique<sqlite_kind_backend>(std::move(*inner), true),
									 value.source_frames);
			require(validated.has_value(), "abort-throw publication could not prepare");
			auto terminal =
				detail::bounded_store_v6_phase_core::publish_once(std::move(*validated));
			require(terminal.has_value(), "abort-throw publication lost terminal custody");
			auto release =
				detail::bounded_store_v6_phase_core::finalize_and_validate_report(*terminal);
			require(release.has_value(), "abort-throw report could not finalize");
			auto cleanup =
				detail::bounded_store_v6_phase_core::drain(*terminal, std::move(*release));
			require(cleanup && cleanup->attempted && !cleanup->drained &&
						cleanup->report_released && !cleanup->backend_cleanup_drained &&
						cleanup->backend_failure ==
							error{"store.invariant-breach",
								  "backend-cleanup",
								  "backend-abort-exception"} &&
						cleanup->failure == cleanup->backend_failure &&
						terminal->cleanup_observation() == *cleanup &&
						!terminal->cleanup_drained() &&
						value.store.live_staging_payload_count() == 0U,
					"backend abort exception escaped or lost its terminal cleanup ledger");
		}
	}

	void foreign_report_release_is_rejected_before_cleanup_effects()
	{
		prepared_case left;
		prepared_case right;
		auto left_backend = left.store.make_backend_port(left.metadata);
		auto right_backend = right.store.make_backend_port(right.metadata);
		require(left_backend && right_backend, "foreign-release backends could not open");
		auto left_release_state = std::make_shared<report_release_state>();
		auto right_release_state = std::make_shared<report_release_state>();
		auto left_validated = prepare(left,
									  std::move(*left_backend),
									  left.source_frames,
									  std::make_unique<report_writer>(left_release_state));
		auto right_validated = prepare(right,
									   std::move(*right_backend),
									   right.source_frames,
									   std::make_unique<report_writer>(right_release_state));
		require(left_validated && right_validated,
				"foreign-release publications could not prepare");
		auto left_terminal =
			detail::bounded_store_v6_phase_core::publish_once(std::move(*left_validated));
		auto right_terminal =
			detail::bounded_store_v6_phase_core::publish_once(std::move(*right_validated));
		require(left_terminal && right_terminal,
				"foreign-release publications lost terminal custody");
		auto left_release =
			detail::bounded_store_v6_phase_core::finalize_and_validate_report(*left_terminal);
		auto right_release =
			detail::bounded_store_v6_phase_core::finalize_and_validate_report(*right_terminal);
		require(left_release && right_release, "foreign-release reports could not finalize");
		const auto left_before = left.operations->backend_observation_call_count();
		const auto right_before = right.operations->backend_observation_call_count();
		auto rejected =
			detail::bounded_store_v6_phase_core::drain(*left_terminal, std::move(*right_release));
		require(rejected && !rejected->attempted && !rejected->drained && rejected->failure &&
					rejected->failure->code == "store.invariant-breach" &&
					rejected->failure->field == "cleanup" &&
					rejected->failure->detail == "release-mismatch" &&
					!left_terminal->cleanup_observation().attempted &&
					left_release_state->release_calls == 0U &&
					right_release_state->release_calls == 0U &&
					left.operations->backend_observation_call_count() == left_before &&
					right.operations->backend_observation_call_count() == right_before,
				"foreign report release performed a cleanup effect or consumed terminal custody");
		auto left_cleanup =
			detail::bounded_store_v6_phase_core::drain(*left_terminal, std::move(*left_release));
		require(left_cleanup && left_cleanup->drained && left_release_state->release_calls == 1U,
				"valid release did not remain usable after foreign-token rejection");
	}

	void reopen_observation_verifier_rejects_identity_spoofs()
	{
		detail::bounded_store_v6_publication_observation parent_publication;
		parent_publication.publication_id = "publication:parent";
		parent_publication.series_id = "series:test";
		parent_publication.snapshot_id = "snapshot:parent";
		parent_publication.sequence = 4U;
		parent_publication.physical_generation = 7U;
		parent_publication.state = cxxlens::sdk::publication_state::committed;
		detail::bounded_store_v6_snapshot_observation parent_snapshot;
		parent_snapshot.snapshot_id = parent_publication.snapshot_id;
		parent_snapshot.partition_count = 3U;
		parent_snapshot.semantic_projection_digest = "sha256:" + std::string(64U, '1');
		parent_snapshot.canonical_export_digest = "sha256:" + std::string(64U, '2');

		detail::bounded_store_v6_publication_observation committed;
		committed.publication_id = "publication:candidate";
		committed.series_id = parent_publication.series_id;
		committed.snapshot_id = "snapshot:candidate";
		committed.sequence = 5U;
		committed.physical_generation = 8U;
		committed.parent_publication = parent_publication.publication_id;
		committed.state = cxxlens::sdk::publication_state::committed;
		detail::bounded_store_v6_snapshot_observation candidate;
		candidate.snapshot_id = committed.snapshot_id;
		candidate.partition_count = 4U;
		candidate.row_count = 2U;
		candidate.claim_count = 3U;
		candidate.coverage_count = 1U;
		candidate.unresolved_count = 0U;
		candidate.semantic_projection_digest = "sha256:" + std::string(64U, '3');
		candidate.canonical_export_digest = "sha256:" + std::string(64U, '4');

		detail::bounded_store_v6_expected_head expected;
		expected.value = detail::bounded_store_v6_expected_head::kind::publication;
		expected.selector = selector();
		expected.publication = parent_publication;
		expected.snapshot = parent_snapshot;

		const auto make_observation = [&]()
		{
			detail::bounded_store_v6_reopen_observation observed;
			observed.factory_attempted = true;
			observed.fresh_backend_binding = "memory-fresh:test";
			observed.current.status = detail::bounded_store_v6_lookup_observation::state::present;
			observed.current.publication = committed;
			observed.current.snapshot = candidate;
			observed.expected_parent.status =
				detail::bounded_store_v6_lookup_observation::state::present;
			observed.expected_parent.publication = parent_publication;
			observed.expected_parent.snapshot = parent_snapshot;
			observed.publication.status =
				detail::bounded_store_v6_lookup_observation::state::present;
			observed.publication.publication = committed;
			observed.publication.snapshot = candidate;
			observed.snapshot.status = detail::bounded_store_v6_lookup_observation::state::present;
			observed.snapshot.publication = committed;
			observed.snapshot.snapshot = candidate;
			observed.canonical_export_digest = candidate.canonical_export_digest;
			return observed;
		};
		const auto require_mismatch =
			[&](const detail::bounded_store_v6_expected_head& head,
				const detail::bounded_store_v6_publication_observation& publication,
				const detail::bounded_store_v6_snapshot_observation& snapshot,
				const detail::bounded_store_v6_reopen_observation& observed,
				const std::string_view message)
		{
			auto result = detail::validate_bounded_store_v6_reopen_observation(
				head, publication, snapshot, observed);
			require(!result && result.error().code == "store.invariant-breach" &&
						result.error().field == "reopen" &&
						result.error().detail == "verification-mismatch",
					message);
		};

		auto valid = detail::validate_bounded_store_v6_reopen_observation(
			expected, committed, candidate, make_observation());
		require(valid.has_value(), "exact reopen observation was rejected");

		{
			auto spoof = make_observation();
			++spoof.current.snapshot->partition_count;
			require_mismatch(expected,
							 committed,
							 candidate,
							 spoof,
							 "candidate snapshot count spoof was accepted");
		}
		{
			auto spoof = make_observation();
			spoof.snapshot.snapshot->semantic_projection_digest = "sha256:" + std::string(64U, '5');
			require_mismatch(expected,
							 committed,
							 candidate,
							 spoof,
							 "candidate snapshot digest spoof was accepted");
		}
		{
			auto spoof_candidate = candidate;
			spoof_candidate.canonical_export_digest = "sha256:" + std::string(64U, '6');
			require_mismatch(expected,
							 committed,
							 spoof_candidate,
							 make_observation(),
							 "candidate input digest spoof was accepted");
		}
		{
			auto spoof = make_observation();
			++spoof.expected_parent.snapshot->partition_count;
			require_mismatch(expected,
							 committed,
							 candidate,
							 spoof,
							 "expected-parent snapshot spoof was accepted");
		}
		{
			auto spoof = make_observation();
			spoof.current.publication->physical_generation = committed.physical_generation - 1U;
			require_mismatch(expected,
							 committed,
							 candidate,
							 spoof,
							 "publication physical-generation regression was accepted");
		}
		{
			auto spoof = make_observation();
			spoof.publication.publication->physical_generation = 0U;
			require_mismatch(expected,
							 committed,
							 candidate,
							 spoof,
							 "zero publication physical generation was accepted");
		}
		{
			auto spoof = make_observation();
			spoof.canonical_export_digest = "sha256:" + std::string(64U, '7');
			require_mismatch(expected,
							 committed,
							 candidate,
							 spoof,
							 "canonical export digest mismatch was accepted");
		}
		for (const auto present_failure : {true, false})
		{
			auto spoof = make_observation();
			if (present_failure)
				spoof.current.failure = error{"store.sqlite-failure", "database", "opaque"};
			else
				spoof.expected_parent.status =
					detail::bounded_store_v6_lookup_observation::state::not_found;
			auto rejected = detail::validate_bounded_store_v6_reopen_observation(
				expected, committed, candidate, spoof);
			require(!rejected && rejected.error().code == "store.invariant-breach" &&
						rejected.error().field == "reopen" &&
						rejected.error().detail == "non-total-lookup",
					"non-total fresh lookup observation was accepted");
		}
	}

	void nonstandard_source_exception_aborts_without_effect()
	{
		prepared_case value;
		{
			auto session =
				detail::bounded_store_v6_phase_core::begin_staging_session(value.metadata);
			require(session.has_value(), "throwing-source session could not open");
			auto input = detail::bounded_store_v6_phase_core::seal_input(
				std::move(*session), census(value.source_frames));
			require(input.has_value(), "throwing-source census could not seal");
			auto backend = value.store.make_backend_port(value.metadata);
			require(backend.has_value(), "throwing-source backend could not open");
			auto prepared = detail::bounded_store_v6_phase_core::prepare_publication(
				std::move(*input), std::move(*backend));
			require(prepared.has_value(), "throwing-source publication could not prepare");
			auto task = detail::bounded_store_v6_phase_core::seal_task_source(
				task_receipt(value.source_frames), std::make_unique<nonstandard_throwing_source>());
			require(task.has_value(), "throwing-source receipt could not seal");
			auto staged =
				detail::bounded_store_v6_phase_core::stage_from_source(*prepared, std::move(*task));
			require(!staged && staged.error().code == "store.invariant-breach" &&
						staged.error().field == "task" &&
						staged.error().detail == "non-standard-exception",
					"non-standard source exception escaped or changed classification");
			auto sealed = detail::bounded_store_v6_phase_core::seal_prepared_publication(*prepared);
			require(!sealed && sealed.error().code == "store.invariant-breach" &&
						sealed.error().field == "prepare",
					"aborted source candidate remained sealable");
			require(value.store.retained_publication_count() == 0U,
					"throwing source exposed a partial publication");
		}
		require(value.store.live_staging_payload_count() == 0U,
				"throwing source retained its private staging payload");
	}

	void projection_mismatch_is_zero_effect()
	{
		prepared_case value;
		auto backend = value.store.make_backend_port(value.metadata);
		require(backend.has_value(), "Memory backend port could not open for mismatch");
		auto prepared = stage(value, std::move(*backend));
		require(prepared.has_value(), "mismatch candidate could not stage");
		auto expected_source =
			detail::make_bounded_store_v6_memory_expected_semantic_source(frames(true));
		require(expected_source.has_value(), "tampered expected source could not open");
		auto expected = detail::bounded_store_v6_phase_core::seal_expected_projection(
			std::move(*expected_source), *prepared);
		require(expected.has_value(), "tampered expected projection could not seal");
		auto actual = detail::bounded_store_v6_phase_core::take_physical_actual_cursor(*prepared);
		require(actual.has_value(), "actual cursor could not open for mismatch");
		auto mismatch = detail::bounded_store_v6_phase_core::compare_bounded_store_projections(
			*prepared, std::move(*expected), std::move(*actual));
		require(!mismatch && mismatch.error().code == "store.corrupt" &&
					mismatch.error().field == "projection",
				"tampered expected projection was accepted");
		auto head = value.store.current_head();
		require(head && head->value == detail::bounded_store_v6_expected_head::kind::genesis &&
					value.store.retained_publication_count() == 0U,
				"projection mismatch became a visible publication");
	}

	void explicit_not_attempted_is_zero_effect_and_cleanup_safe()
	{
		prepared_case value;
		auto backend = value.store.make_backend_port(value.metadata);
		require(backend.has_value(), "not-attempted backend could not open");
		auto prepared = stage(value, std::move(*backend));
		require(prepared.has_value(), "not-attempted candidate could not stage");
		auto expected_source =
			detail::make_bounded_store_v6_memory_expected_semantic_source(value.source_frames);
		require(expected_source.has_value(), "not-attempted expected cursor could not open");
		auto expected = detail::bounded_store_v6_phase_core::seal_expected_projection(
			std::move(*expected_source), *prepared);
		auto actual = detail::bounded_store_v6_phase_core::take_physical_actual_cursor(*prepared);
		require(expected && actual, "not-attempted compare cursors could not open");
		auto match = detail::bounded_store_v6_phase_core::compare_bounded_store_projections(
			*prepared, std::move(*expected), std::move(*actual));
		require(match.has_value(), "not-attempted projections did not match");
		auto report = detail::bounded_store_v6_phase_core::reserve_report_tail(
			*prepared, *match, std::make_unique<report_writer>());
		require(report.has_value(), "not-attempted report tail could not reserve");
		auto terminal = detail::bounded_store_v6_phase_core::capture_not_attempted(
			std::move(*prepared), std::move(*report));
		require(terminal &&
					terminal->observation().terminal ==
						detail::bounded_store_v6_publication_terminal::not_attempted &&
					terminal->observation().publish_call_count == 0U &&
					!terminal->observation().publication_attempted &&
					!terminal->observation().sdk_error &&
					!terminal->observation().backend_call_error &&
					!terminal->observation().returned_publication &&
					!terminal->observation().returned_snapshot &&
					!terminal->observation().returned_export_digest &&
					!terminal->observation().reopen.factory_attempted &&
					value.store.retained_publication_count() == 0U,
				"explicit not-attempted terminal fabricated or performed a publication");
		finish_report_and_cleanup(*terminal);
		require(value.store.live_staging_payload_count() == 0U &&
					value.store.retained_publication_count() == 0U,
				"not-attempted cleanup retained staging or exposed partial visibility");
	}

	void memory_publish_errors_are_invariant_or_unverified()
	{
		prepared_case value;
		auto first_backend = value.store.make_backend_port(value.metadata);
		require(first_backend.has_value(), "first Memory backend port could not open");
		auto first = prepare(value, std::move(*first_backend), value.source_frames);
		require(first.has_value(), "first Memory publication could not prepare");
		// Admit a second genesis candidate before either effect.  Only the first atomic publish may
		// consume genesis; the loser is an invariant breach, never a normal stale outcome.
		value.metadata.staging_session_id = "staging:test:concurrent-loser";
		auto second_backend = value.store.make_backend_port(value.metadata);
		require(second_backend.has_value(), "concurrent Memory backend port could not open");
		auto second = prepare(value, std::move(*second_backend), value.source_frames);
		require(second.has_value(), "concurrent Memory publication could not prepare");
		auto first_terminal = detail::bounded_store_v6_phase_core::publish_once(std::move(*first));
		require(first_terminal &&
					first_terminal->observation().terminal ==
						detail::bounded_store_v6_publication_terminal::committed_verified,
				"first Memory publication did not commit");
		const auto first_publication = first_terminal->observation().returned_publication;
		finish_report_and_cleanup(*first_terminal);

		auto stale = detail::bounded_store_v6_phase_core::publish_once(std::move(*second));
		require(stale &&
					stale->observation().terminal ==
						detail::bounded_store_v6_publication_terminal::committed_unverified &&
					stale->observation().backend_call_error &&
					stale->observation().backend_call_error->code == "store.invariant-breach" &&
					stale->observation().backend_call_error->field == "head" &&
					stale->observation().backend_call_error->detail == "compare-and-swap" &&
					!stale->observation().sdk_error,
				"Memory outer publish error lost conservative terminal custody");
		finish_report_and_cleanup(*stale);
		require(value.store.retained_publication_count() == 1U,
				"head mismatch exposed a partial Memory publication");
		auto current = value.store.current_head();
		require(current && current->publication == first_publication,
				"head mismatch changed the visible Memory head");
		require(value.store.live_staging_payload_count() == 0U,
				"Memory invariant breach retained staging custody");

		prepared_case reopen_case;
		auto reopen_backend = reopen_case.store.make_backend_port(reopen_case.metadata);
		require(reopen_backend.has_value(), "reopen-failure backend could not open");
		auto unknown = prepare(reopen_case, std::move(*reopen_backend), reopen_case.source_frames);
		require(unknown.has_value(), "reopen-failure candidate could not prepare");
		reopen_case.operations->inject_next_backend_fault(
			cxxlens::sdk::store_backend_operation::reopen_factory,
			cxxlens::sdk::store_backend_observation_fault::backend_failure,
			cxxlens::test::store_operation_ambiguity_side::before_delegate);
		auto unknown_terminal =
			detail::bounded_store_v6_phase_core::publish_once(std::move(*unknown));
		require(unknown_terminal &&
					unknown_terminal->observation().terminal ==
						detail::bounded_store_v6_publication_terminal::committed_unverified &&
					unknown_terminal->observation().publish_call_count == 1U &&
					unknown_terminal->observation().publication_attempted &&
					!unknown_terminal->observation().sdk_error &&
					unknown_terminal->observation().reopen.factory_attempted &&
					unknown_terminal->observation().reopen.factory_error &&
					unknown_terminal->observation().reopen.factory_error->code ==
						"store.invariant-breach" &&
					unknown_terminal->observation().reopen.factory_error->field ==
						"operation-port" &&
					unknown_terminal->observation().returned_publication,
				"reopen failure did not retain committed_unverified");
		require(reopen_case.store.retained_publication_count() == 1U,
				"reopen failure lost or duplicated the committed backing");
		finish_report_and_cleanup(*unknown_terminal);

		prepared_case after_effect_case;
		auto after_effect_backend =
			after_effect_case.store.make_backend_port(after_effect_case.metadata);
		require(after_effect_backend.has_value(), "after-effect backend could not open");
		auto after_effect = prepare(
			after_effect_case, std::move(*after_effect_backend), after_effect_case.source_frames);
		require(after_effect.has_value(), "after-effect candidate could not prepare");
		after_effect_case.operations->inject_next_backend_fault(
			cxxlens::sdk::store_backend_operation::publish_once,
			cxxlens::sdk::store_backend_observation_fault::backend_failure,
			cxxlens::test::store_operation_ambiguity_side::after_delegate);
		auto after_effect_terminal =
			detail::bounded_store_v6_phase_core::publish_once(std::move(*after_effect));
		require(
			after_effect_terminal &&
				after_effect_terminal->observation().terminal ==
					detail::bounded_store_v6_publication_terminal::committed_unverified &&
				after_effect_terminal->observation().publish_call_count == 1U &&
				after_effect_terminal->observation().publication_attempted &&
				after_effect_terminal->observation().backend_call_error &&
				after_effect_terminal->observation().backend_call_error->code ==
					"store.invariant-breach" &&
				after_effect_terminal->observation().backend_call_error->field ==
					"operation-port" &&
				!after_effect_terminal->observation().sdk_error &&
				after_effect_terminal->observation().verification_failure ==
					detail::bounded_store_v6_verification_failure::local_verification_unavailable &&
				after_effect_case.store.retained_publication_count() == 1U,
			"effect-after-delegate failure lost conservative terminal custody or visibility");
		auto committed_head = after_effect_case.store.current_head();
		require(committed_head &&
					committed_head->value ==
						detail::bounded_store_v6_expected_head::kind::publication,
				"effect-after-delegate failure did not preserve the committed current head");
		finish_report_and_cleanup(*after_effect_terminal);
		require(
			after_effect_case.store.retained_publication_count() == 1U &&
				after_effect_case.store.live_staging_payload_count() == 0U,
			"effect-after-delegate cleanup retried publish or removed the committed publication");
	}

	void every_fresh_reopen_phase_failure_is_total_and_unverified()
	{
		using operation = cxxlens::sdk::store_backend_operation;
		using side = cxxlens::test::store_operation_ambiguity_side;
		struct reopen_fault
		{
			operation value;
			side injection_side;
		};
		const std::array faults{
			reopen_fault{operation::reopen_factory, side::before_delegate},
			reopen_fault{operation::reopen_factory, side::after_delegate},
			reopen_fault{operation::open_physical_cursor, side::before_delegate},
			reopen_fault{operation::finish_physical_cursor, side::after_delegate},
			reopen_fault{operation::lookup_current, side::before_delegate},
			reopen_fault{operation::lookup_expected_parent, side::before_delegate},
			reopen_fault{operation::lookup_publication, side::before_delegate},
			reopen_fault{operation::lookup_snapshot, side::before_delegate},
			reopen_fault{operation::canonical_export, side::before_delegate},
		};
		for (std::size_t index{}; index < faults.size(); ++index)
		{
			prepared_case value;
			value.metadata.staging_session_id = "staging:reopen-fault:" + std::to_string(index);
			auto backend = value.store.make_backend_port(value.metadata);
			require(backend.has_value(), "fresh-reopen-fault backend could not open");
			auto validated = prepare(value, std::move(*backend), value.source_frames);
			require(validated.has_value(), "fresh-reopen-fault candidate could not prepare");
			value.operations->inject_next_backend_fault(
				faults[index].value,
				cxxlens::sdk::store_backend_observation_fault::backend_failure,
				faults[index].injection_side);
			auto terminal =
				detail::bounded_store_v6_phase_core::publish_once(std::move(*validated));
			require(
				terminal &&
					terminal->observation().terminal ==
						detail::bounded_store_v6_publication_terminal::committed_unverified &&
					terminal->observation().publish_call_count == 1U &&
					terminal->observation().publication_attempted &&
					terminal->observation().returned_publication &&
					!terminal->observation().sdk_error &&
					!terminal->observation().backend_call_error &&
					terminal->observation().verification_failure ==
						detail::bounded_store_v6_verification_failure::reopen_observation &&
					terminal->observation().reopen.factory_attempted &&
					value.store.retained_publication_count() == 1U,
				"fresh reopen phase failure was not retained as a total unverified observation");
			const auto& reopened = terminal->observation().reopen;
			const auto observed_failure = reopened.factory_error.has_value() ||
				reopened.current.status ==
					detail::bounded_store_v6_lookup_observation::state::failed ||
				reopened.expected_parent.status ==
					detail::bounded_store_v6_lookup_observation::state::failed ||
				reopened.publication.status ==
					detail::bounded_store_v6_lookup_observation::state::failed ||
				reopened.snapshot.status ==
					detail::bounded_store_v6_lookup_observation::state::failed ||
				reopened.canonical_export_error.has_value();
			require(observed_failure,
					"fresh reopen fault disappeared from the bounded total observation");
			finish_report_and_cleanup(*terminal);
			require(value.store.retained_publication_count() == 1U &&
						value.store.live_staging_payload_count() == 0U,
					"fresh reopen failure cleanup changed visibility or retained staging");
		}
	}

	void source_faults_and_one_shot_cursor()
	{
		const auto expect_source_error =
			[](std::vector<detail::bounded_store_v6_memory_source_frame> value,
			   const std::string_view detail_value)
		{
			auto source = detail::make_bounded_store_v6_memory_task_frame_source(std::move(value));
			require(!source && source.error().code == "store.corrupt" &&
						source.error().field == "source" && source.error().detail == detail_value,
					"Memory source fault was accepted");
		};
		expect_source_error({}, "missing-partition-end");
		auto dropped = frames();
		dropped.pop_back();
		expect_source_error(std::move(dropped), "missing-partition-end");
		auto tampered = frames();
		tampered[1U].bytes[20U] ^= std::byte{0x01U};
		expect_source_error(std::move(tampered), "checksum");
		auto reordered = frames();
		std::swap(reordered[1U], reordered[2U]);
		expect_source_error(std::move(reordered), "reordered-or-duplicate");
		auto duplicated = frames();
		duplicated.insert(duplicated.begin() + 2, duplicated[1U]);
		expect_source_error(std::move(duplicated), "reordered-or-duplicate");
		auto truncated = frames();
		truncated.front().bytes.resize(48U);
		expect_source_error(std::move(truncated), "frame-shape");
		auto replay_source_result =
			detail::make_bounded_store_v6_memory_task_frame_source(frames());
		require(replay_source_result.has_value(), "valid source could not open for replay test");
		auto replay_source = std::move(*replay_source_result);
		for (;;)
		{
			auto next = replay_source->next_record();
			require(next.has_value(), "valid source failed during replay test");
			if (!*next)
				break;
			std::vector<std::byte> bytes(static_cast<std::size_t>((*next)->framed_bytes));
			std::size_t offset{};
			while (offset < bytes.size())
			{
				auto read =
					replay_source->read_record_bytes(std::span<std::byte>{bytes}.subspan(offset));
				require(read && *read > 0U, "valid source returned an empty read");
				offset += *read;
			}
		}
		auto source_replay = replay_source->next_record();
		require(!source_replay && source_replay.error().code == "store.invariant-breach" &&
					source_replay.error().field == "source" &&
					source_replay.error().detail == "replay",
				"expected cursor was reusable after EOF");

		prepared_case gap_case;
		const auto complete = frames();
		gap_case.source_frames = {complete.front(), complete.back()};
		auto gap_backend = gap_case.store.make_backend_port(gap_case.metadata);
		require(gap_backend.has_value(), "gap candidate backend could not open");
		// The authority census still describes the complete partition.  The supplied task cursor
		// omits the claim and annotation, so a closed but incomplete task cannot be published.
		auto gap = stage_with_external_census(gap_case,
											  std::move(*gap_backend),
											  census(complete),
											  gap_case.source_frames,
											  task_receipt(gap_case.source_frames));
		require(!gap && gap.error().code == "store.corrupt" && gap.error().field == "projection" &&
					gap.error().detail == "sealed-input-census" &&
					gap_case.store.retained_publication_count() == 0U,
				"missing event gap was not rejected before publication");

		prepared_case value;
		auto backend = value.store.make_backend_port(value.metadata);
		require(backend.has_value(), "cursor replay backend could not open");
		auto prepared = stage(value, std::move(*backend));
		require(prepared.has_value(), "cursor replay candidate could not stage");
		auto actual = detail::bounded_store_v6_phase_core::take_physical_actual_cursor(*prepared);
		require(actual.has_value(), "physical cursor could not be taken");
		auto replay = detail::bounded_store_v6_phase_core::take_physical_actual_cursor(*prepared);
		require(!replay && replay.error().code == "store.invariant-breach" &&
					replay.error().field == "actual",
				"physical cursor was reusable after its one-shot transfer");
	}

	void task_receipt_spoofs_are_rejected_without_publication()
	{
		// Receipt validation itself rejects a closed-census forgery before a backend is touched.
		{
			const auto source_frames = frames();
			auto source = detail::make_bounded_store_v6_memory_task_frame_source(source_frames);
			require(source.has_value(), "receipt-validation source could not open");
			auto receipt = task_receipt(source_frames);
			receipt.event_count = 2U;
			receipt.annotation_count = 0U;
			auto sealed = detail::bounded_store_v6_phase_core::seal_task_source(std::move(receipt),
																				std::move(*source));
			require(!sealed && sealed.error().code == "store.invariant-breach" &&
						sealed.error().field == "task" && sealed.error().detail == "event-census",
					"task census spoof passed receipt sealing");
		}

		// A nonzero but altered digest is structurally valid and must be rejected only after the
		// complete source has been compared.  The failed candidate remains unpublished.
		{
			prepared_case value;
			auto backend = value.store.make_backend_port(value.metadata);
			require(backend.has_value(), "digest-spoof backend could not open");
			auto source =
				detail::make_bounded_store_v6_memory_task_frame_source(value.source_frames);
			require(source.has_value(), "digest-spoof source could not open");
			auto receipt = task_receipt(value.source_frames);
			receipt.binary_sha256[0U] ^= std::byte{0x01U};
			auto staged = stage_with_external_census(value,
													 std::move(*backend),
													 census(value.source_frames),
													 value.source_frames,
													 std::move(receipt));
			require(!staged && staged.error().code == "store.corrupt" &&
						staged.error().field == "task" &&
						staged.error().detail == "receipt-mismatch",
					"task digest spoof was accepted");
			require(value.store.retained_publication_count() == 0U,
					"digest-spoof candidate became visible");
			auto head = value.store.current_head();
			require(head && head->value == detail::bounded_store_v6_expected_head::kind::genesis,
					"digest-spoof candidate changed the current head");
		}

		// Keep the event total closed while changing the kind census.  This exercises the semantic
		// receipt comparison independently from the binary digest check.
		{
			prepared_case value;
			auto backend = value.store.make_backend_port(value.metadata);
			require(backend.has_value(), "census-spoof backend could not open");
			auto receipt = task_receipt(value.source_frames);
			receipt.claim_count = 0U;
			receipt.annotation_count = 0U;
			receipt.row_count = 2U;
			auto staged = stage_with_external_census(value,
													 std::move(*backend),
													 census(value.source_frames),
													 value.source_frames,
													 std::move(receipt));
			require(!staged && staged.error().code == "store.corrupt" &&
						staged.error().field == "task" &&
						staged.error().detail == "receipt-mismatch",
					"task kind-census spoof was accepted");
			require(value.store.retained_publication_count() == 0U,
					"census-spoof candidate became visible");
		}

		// An out-of-range ordinal is rejected before the first backend record is admitted.
		{
			prepared_case value;
			auto backend = value.store.make_backend_port(value.metadata);
			require(backend.has_value(), "ordinal-spoof backend could not open");
			auto staged = stage_with_external_census(value,
													 std::move(*backend),
													 census(value.source_frames),
													 value.source_frames,
													 task_receipt(value.source_frames, 1U));
			require(!staged && staged.error().code == "store.invariant-breach" &&
						staged.error().field == "task" &&
						staged.error().detail == "ordinal-or-identity",
					"task ordinal spoof was accepted");
			require(value.store.retained_publication_count() == 0U,
					"ordinal-spoof candidate became visible");
		}

		// A duplicate task identity is rejected even when the second ordinal is otherwise valid.
		{
			prepared_case value;
			const auto partition = frames();
			auto session =
				detail::bounded_store_v6_phase_core::begin_staging_session(value.metadata);
			require(session.has_value(), "identity-spoof session could not open");
			auto input = detail::bounded_store_v6_phase_core::seal_input(
				std::move(*session), repeated_census(partition, 2U));
			require(input.has_value(), "identity-spoof census could not seal");
			auto backend = value.store.make_backend_port(value.metadata);
			require(backend.has_value(), "identity-spoof backend could not open");
			auto prepared = detail::bounded_store_v6_phase_core::prepare_publication(
				std::move(*input), std::move(*backend));
			require(prepared.has_value(), "identity-spoof publication could not prepare");
			auto first_source = detail::make_bounded_store_v6_memory_task_frame_source(partition);
			require(first_source.has_value(), "identity-spoof first source could not open");
			auto first_task = detail::bounded_store_v6_phase_core::seal_task_source(
				task_receipt(partition, 0U, "same-task"), std::move(*first_source));
			require(first_task.has_value(), "identity-spoof first receipt could not seal");
			auto first = detail::bounded_store_v6_phase_core::stage_from_source(
				*prepared, std::move(*first_task));
			require(first.has_value(), "identity-spoof first task could not stage");
			auto second_source = detail::make_bounded_store_v6_memory_task_frame_source(partition);
			require(second_source.has_value(), "identity-spoof second source could not open");
			auto second_task = detail::bounded_store_v6_phase_core::seal_task_source(
				task_receipt(partition, 1U, "same-task"), std::move(*second_source));
			require(second_task.has_value(), "identity-spoof second receipt could not seal");
			auto second = detail::bounded_store_v6_phase_core::stage_from_source(
				*prepared, std::move(*second_task));
			require(!second && second.error().code == "store.invariant-breach" &&
						second.error().field == "task" &&
						second.error().detail == "ordinal-or-identity",
					"duplicate task identity was accepted");
			require(value.store.retained_publication_count() == 0U,
					"identity-spoof candidate became visible");
		}
	}

	void maximum_task_candidate_is_streamed_and_published()
	{
		prepared_case value;
		const auto partition = frames();
		auto session = detail::bounded_store_v6_phase_core::begin_staging_session(value.metadata);
		require(session.has_value(), "maximum-task staging session could not open");
		auto input = detail::bounded_store_v6_phase_core::seal_input(
			std::move(*session), repeated_census(partition, detail::bounded_store_v6_max_tasks));
		require(input.has_value(), "4096-task input census could not seal");
		auto backend = value.store.make_backend_port(value.metadata);
		require(backend.has_value(), "4096-task Memory backend could not open");
		auto prepared = detail::bounded_store_v6_phase_core::prepare_publication(
			std::move(*input), std::move(*backend));
		require(prepared.has_value(), "4096-task publication could not prepare");
		for (std::uint64_t task{}; task < detail::bounded_store_v6_max_tasks; ++task)
		{
			auto source = detail::make_bounded_store_v6_memory_task_frame_source(partition);
			require(source.has_value(), "4096-task partition cursor could not open");
			auto receipt = detail::bounded_store_v6_phase_core::seal_task_source(
				task_receipt(partition, task), std::move(*source));
			require(receipt.has_value(), "4096-task receipt could not seal");
			auto staged = detail::bounded_store_v6_phase_core::stage_from_source(
				*prepared, std::move(*receipt));
			require(staged.has_value(), "4096-task partition could not stream");
		}
		auto sealed = detail::bounded_store_v6_phase_core::seal_prepared_publication(*prepared);
		require(sealed.has_value(), "4096-task physical staging could not seal");
		auto expected_source = detail::make_bounded_store_v6_memory_expected_semantic_source(
			repeated_frames(partition, detail::bounded_store_v6_max_tasks));
		require(expected_source.has_value(), "4096-task expected cursor could not open");
		auto expected = detail::bounded_store_v6_phase_core::seal_expected_projection(
			std::move(*expected_source), *prepared);
		require(expected.has_value(), "4096-task expected projection could not seal");
		auto actual = detail::bounded_store_v6_phase_core::take_physical_actual_cursor(*prepared);
		require(actual.has_value(), "4096-task physical cursor could not open");
		auto match = detail::bounded_store_v6_phase_core::compare_bounded_store_projections(
			*prepared, std::move(*expected), std::move(*actual));
		require(match.has_value() &&
					match->observation().record_count ==
						detail::bounded_store_v6_max_tasks * partition.size(),
				"4096-task physical projection did not match full expected bytes");
		auto report = detail::bounded_store_v6_phase_core::reserve_report_tail(
			*prepared, *match, std::make_unique<report_writer>());
		require(report.has_value(), "4096-task report tail could not reserve");
		auto validated = detail::bounded_store_v6_phase_core::bind_publication(
			std::move(*prepared), std::move(*match), std::move(*report));
		require(validated.has_value(), "4096-task publication token could not bind");
		auto terminal = detail::bounded_store_v6_phase_core::publish_once(std::move(*validated));
		require(terminal &&
					terminal->observation().terminal ==
						detail::bounded_store_v6_publication_terminal::committed_verified,
				"4096-task Memory publication did not commit and reopen");
		finish_report_and_cleanup(*terminal);
		require(value.store.retained_publication_count() == 1U,
				"4096-task publication was not atomically visible");
	}

	void report_finalization_failure_is_one_shot_and_cleanup_safe()
	{
		prepared_case value;
		auto backend = value.store.make_backend_port(value.metadata);
		require(backend.has_value(), "report-failure Memory backend could not open");
		auto writer_state = std::make_shared<failing_report_state>();
		{
			auto validated = prepare(value,
									 std::move(*backend),
									 value.source_frames,
									 std::make_unique<failing_report_writer>(writer_state));
			require(validated.has_value(), "report-failure candidate could not prepare");
			auto terminal =
				detail::bounded_store_v6_phase_core::publish_once(std::move(*validated));
			require(terminal.has_value(), "report-failure publication lost terminal custody");
			auto first =
				detail::bounded_store_v6_phase_core::finalize_and_validate_report(*terminal);
			require(!first && first.error().code == "test.report" &&
						first.error().field == "section-census",
					"full report census fault was not preserved");
			auto replay =
				detail::bounded_store_v6_phase_core::finalize_and_validate_report(*terminal);
			require(!replay && replay.error().code == "store.invariant-breach" &&
						replay.error().field == "report" && replay.error().detail == "not-live",
					"partially written report tail was retried");
			require(writer_state->append_calls == 1U && writer_state->section_census_calls == 1U,
					"report mutation was not one-shot");
		}
		require(writer_state->release_calls == 1U,
				"failed report finalization did not release exact writer custody once");
		require(value.store.retained_publication_count() == 1U,
				"report validation failure changed committed product visibility");
	}

	void invalid_report_reservation_releases_once_without_effect()
	{
		prepared_case value;
		auto release_state = std::make_shared<report_release_state>();
		{
			auto backend = value.store.make_backend_port(value.metadata);
			require(backend.has_value(), "reservation-failure backend could not open");
			auto prepared = stage(value, std::move(*backend));
			require(prepared.has_value(), "reservation-failure candidate could not stage");
			auto expected_source =
				detail::make_bounded_store_v6_memory_expected_semantic_source(value.source_frames);
			require(expected_source.has_value(),
					"reservation-failure expected source could not open");
			auto expected = detail::bounded_store_v6_phase_core::seal_expected_projection(
				std::move(*expected_source), *prepared);
			auto actual =
				detail::bounded_store_v6_phase_core::take_physical_actual_cursor(*prepared);
			require(expected && actual, "reservation-failure cursors could not open");
			auto match = detail::bounded_store_v6_phase_core::compare_bounded_store_projections(
				*prepared, std::move(*expected), std::move(*actual));
			require(match.has_value(), "reservation-failure projection did not match");
			auto reservation = detail::bounded_store_v6_phase_core::reserve_report_tail(
				*prepared, *match, std::make_unique<report_writer>(release_state, true));
			require(!reservation && reservation.error().code == "store.resource-limit" &&
						reservation.error().field == "report-tail" &&
						release_state->release_calls == 1U,
					"invalid physical reservation was retained or not released exactly once");
			require(value.store.retained_publication_count() == 0U,
					"invalid report reservation exposed a publication");
		}
		require(release_state->release_calls == 1U &&
					value.store.live_staging_payload_count() == 0U,
				"reservation failure destruction retried release or retained backend custody");
	}

	void sqlite_writer_publish_tuple_matrix_is_closed()
	{
		using class_value = detail::bounded_store_v6_error_class;
		constexpr auto sqlite = detail::bounded_store_v6_backend::sqlite;
		constexpr auto memory = detail::bounded_store_v6_backend::memory;
		const std::string series{"series:exact"};
		const std::string snapshot{"snapshot:exact"};
		const std::string publication{"publication:exact"};
		const auto classify =
			[&](const detail::bounded_store_v6_backend backend, const error& value)
		{
			return detail::classify_bounded_store_v6_error(
				backend, series, snapshot, publication, value);
		};
		require(classify(sqlite, {"store.publication-conflict", series, {}}) ==
					class_value::stale_parent,
				"exact SQLite CAS loss was not classified stale");
		for (const auto field : {"publication_sequence", "physical_generation"})
			require(classify(sqlite, {"store.counter-overflow", field, {}}) ==
						class_value::corrupt_store,
					"exact SQLite counter overflow was not classified store failure");
		require(classify(sqlite, {"store.hash-collision", snapshot, {}}) ==
						class_value::corrupt_store &&
					classify(sqlite, {"store.snapshot-ambiguous", snapshot, {}}) ==
						class_value::corrupt_store &&
					classify(sqlite, {"store.sqlite-failure", "database", "opaque-occurrence"}) ==
						class_value::sqlite_failure,
				"exact SQLite hash/ambiguity/I-O tuple was not accepted");
		for (const auto detail_value : {"backend",
										"column-count",
										"publication-row",
										"series-head-count",
										"series-head",
										"series-head-sequence"})
			require(classify(sqlite, {"store.corrupt", "sqlite", detail_value}) ==
						class_value::corrupt_store,
					"exact SQLite authority corruption tuple was not accepted");
		for (const auto detail_value :
			 {"authority-record", "duplicate-publication-id", "parent", "parent-sequence"})
			require(classify(sqlite, {"store.corrupt", publication, detail_value}) ==
						class_value::corrupt_store,
					"exact publication corruption tuple was not accepted");
		for (const auto detail_value : {"duplicate-sequence", "series-roots", "series-head-cas"})
			require(classify(sqlite, {"store.corrupt", series, detail_value}) ==
						class_value::corrupt_store,
					"exact series corruption tuple was not accepted");

		const std::array near_misses{
			error{"store.publication-conflict", series, "nonempty"},
			error{"store.publication-conflict", "series:near", {}},
			error{"store.publish-stale-parent", series, {}},
			error{"store.counter-overflow", "publication-sequence", {}},
			error{"store.counter-overflow", "physical_generation", "nonempty"},
			error{"store.hash-collision", "snapshot:near", {}},
			error{"store.snapshot-ambiguous", snapshot, "nonempty"},
			error{"store.sqlite-failure", "database", {}},
			error{"store.sqlite-failure", "sqlite", "opaque-occurrence"},
			error{"store.backend-unavailable", "database", "opaque-occurrence"},
			error{"store.reopen-required", "database", "opaque-occurrence"},
			error{"store.corrupt", "sqlite", "series-head-extra"},
			error{"store.corrupt", publication, "parent-extra"},
			error{"store.corrupt", series, "series-head"},
		};
		for (const auto& near_miss : near_misses)
			require(classify(sqlite, near_miss) == class_value::invariant_breach,
					"near-miss SQLite tuple entered the authoritative outcome map");
		require(classify(memory, {"store.publication-conflict", series, {}}) ==
						class_value::invariant_breach &&
					classify(memory, {"store.counter-overflow", "publication_sequence", {}}) ==
						class_value::invariant_breach &&
					classify(memory, {"store.sqlite-failure", "database", "opaque"}) ==
						class_value::invariant_breach,
				"Memory SDK error was mapped to a normal publication outcome");
	}

	void sqlite_exact_failed_effects_reach_all_typed_terminals()
	{
		struct terminal_case
		{
			error failure;
			detail::bounded_store_v6_publication_terminal terminal;
		};
		const auto series_id = selector().id();
		const std::array cases{
			terminal_case{{"store.publication-conflict", series_id, {}},
						  detail::bounded_store_v6_publication_terminal::rejected_stale},
			terminal_case{
				{"store.sqlite-failure", "database", "opaque-occurrence"},
				detail::bounded_store_v6_publication_terminal::publication_outcome_unknown},
			terminal_case{{"store.corrupt", "sqlite", "backend"},
						  detail::bounded_store_v6_publication_terminal::rejected_store_failure},
		};
		for (std::size_t index{}; index < cases.size(); ++index)
		{
			prepared_case value;
			auto inner = value.store.make_backend_port(value.metadata);
			require(inner.has_value(), "typed-terminal physical backend could not open");
			value.metadata.backend = detail::bounded_store_v6_backend::sqlite;
			value.metadata.exact_sqlite_path =
				"/tmp/cxxlens-v6-typed-terminal-" + std::to_string(index) + ".sqlite";
			auto backend =
				std::make_unique<sqlite_kind_backend>(std::move(*inner), cases[index].failure);
			auto validated = prepare(value, std::move(backend), value.source_frames);
			require(validated.has_value(), "typed-terminal publication could not prepare");
			auto terminal =
				detail::bounded_store_v6_phase_core::publish_once(std::move(*validated));
			require(terminal && terminal->observation().terminal == cases[index].terminal &&
						terminal->observation().publish_call_count == 1U &&
						terminal->observation().publication_attempted &&
						terminal->observation().sdk_error == cases[index].failure &&
						!terminal->observation().backend_call_error &&
						!terminal->observation().verification_failure &&
						!terminal->observation().returned_publication &&
						value.store.retained_publication_count() == 0U,
					"exact SQLite SDK failure did not retain its closed terminal outcome");
			finish_report_and_cleanup(*terminal);
			require(value.store.live_staging_payload_count() == 0U &&
						value.store.retained_publication_count() == 0U,
					"failed SQLite terminal retained staging or fabricated visibility");
		}

		prepared_case unlisted_value;
		auto unlisted_inner = unlisted_value.store.make_backend_port(unlisted_value.metadata);
		require(unlisted_inner.has_value(), "unlisted-tuple physical backend could not open");
		unlisted_value.metadata.backend = detail::bounded_store_v6_backend::sqlite;
		unlisted_value.metadata.exact_sqlite_path = "/tmp/cxxlens-v6-unlisted.sqlite";
		const error unlisted{"store.publish-stale-parent", series_id, {}};
		auto unlisted_validated =
			prepare(unlisted_value,
					std::make_unique<sqlite_kind_backend>(std::move(*unlisted_inner), unlisted),
					unlisted_value.source_frames);
		require(unlisted_validated.has_value(), "unlisted-tuple publication could not prepare");
		auto unlisted_terminal =
			detail::bounded_store_v6_phase_core::publish_once(std::move(*unlisted_validated));
		require(
			unlisted_terminal &&
				unlisted_terminal->observation().terminal ==
					detail::bounded_store_v6_publication_terminal::committed_unverified &&
				unlisted_terminal->observation().backend_call_error == unlisted &&
				!unlisted_terminal->observation().sdk_error &&
				unlisted_terminal->observation().verification_failure ==
					detail::bounded_store_v6_verification_failure::local_verification_unavailable &&
				unlisted_value.store.retained_publication_count() == 0U,
			"unlisted effect-call tuple escaped without terminal custody");
		finish_report_and_cleanup(*unlisted_terminal);
	}

	void deterministic_ids_and_bounds()
	{
		prepared_case left;
		prepared_case right;
		auto invalid_backend = left.metadata;
		invalid_backend.backend = static_cast<detail::bounded_store_v6_backend>(0xffU);
		auto invalid_backend_session =
			detail::bounded_store_v6_phase_core::begin_staging_session(std::move(invalid_backend));
		require(!invalid_backend_session &&
					invalid_backend_session.error() ==
						error{"store.invariant-breach", "backend", "invalid-kind"},
				"unknown backend kind was not rejected before phase construction");
		auto invalid_head = left.metadata;
		invalid_head.expected_head.value =
			static_cast<detail::bounded_store_v6_expected_head::kind>(0xffU);
		auto invalid_head_session =
			detail::bounded_store_v6_phase_core::begin_staging_session(std::move(invalid_head));
		require(!invalid_head_session &&
					invalid_head_session.error() ==
						error{"store.invariant-breach", "expected-head", "invalid-kind"},
				"unknown expected-head kind was accepted as a publication authority");
		require(detail::validate_bounded_store_v6_product_constants().has_value(),
				"complete product bound matrix was rejected");
		auto left_backend = left.store.make_backend_port(left.metadata);
		auto right_backend = right.store.make_backend_port(right.metadata);
		require(left_backend && right_backend, "determinism backend could not open");
		auto left_prepared = stage(left, std::move(*left_backend));
		auto right_prepared = stage(right, std::move(*right_backend));
		require(left_prepared && right_prepared, "determinism candidate could not stage");
		require(left_prepared->observation().candidate_id ==
						right_prepared->observation().candidate_id &&
					left_prepared->observation().candidate_snapshot_id ==
						right_prepared->observation().candidate_snapshot_id,
				"identical Memory projections produced nondeterministic candidate identities");
		prepared_case rebound;
		auto rebound_backend = rebound.store.make_backend_port(rebound.metadata);
		require(rebound_backend.has_value(), "receipt-binding backend could not open");
		auto receipt = task_receipt(rebound.source_frames);
		receipt.immutable_binding = "task-authority:independent-runtime-receipt";
		auto rebound_prepared = stage_with_external_census(rebound,
														   std::move(*rebound_backend),
														   census(rebound.source_frames),
														   rebound.source_frames,
														   std::move(receipt));
		require(rebound_prepared &&
					rebound_prepared->observation().candidate_snapshot_id ==
						left_prepared->observation().candidate_snapshot_id &&
					rebound_prepared->observation().candidate_id !=
						left_prepared->observation().candidate_id,
				"candidate identity discarded the immutable task receipt binding");

		prepared_case engine_rebound;
		engine_rebound.metadata.relation_engine_generation = "engine-generation:other";
		auto engine_backend = engine_rebound.store.make_backend_port(engine_rebound.metadata);
		require(engine_backend.has_value(), "engine-binding backend could not open");
		auto engine_prepared = stage(engine_rebound, std::move(*engine_backend));
		require(engine_prepared &&
					engine_prepared->observation().candidate_snapshot_id ==
						left_prepared->observation().candidate_snapshot_id &&
					engine_prepared->observation().candidate_id !=
						left_prepared->observation().candidate_id,
				"candidate identity discarded the relation-engine generation");

		prepared_case session_rebound;
		session_rebound.metadata.staging_session_id = "staging:test:other";
		auto session_backend = session_rebound.store.make_backend_port(session_rebound.metadata);
		require(session_backend.has_value(), "session-binding backend could not open");
		auto session_prepared = stage(session_rebound, std::move(*session_backend));
		require(session_prepared &&
					session_prepared->observation().candidate_id !=
						left_prepared->observation().candidate_id,
				"candidate identity discarded the staging session nonce");

		prepared_case head_rebound;
		auto inner_head_backend = head_rebound.store.make_backend_port(head_rebound.metadata);
		require(inner_head_backend.has_value(), "head-binding physical backend could not open");
		detail::bounded_store_v6_publication_observation parent;
		parent.series_id = head_rebound.metadata.selector.id();
		parent.snapshot_id = left_prepared->observation().candidate_snapshot_id;
		parent.sequence = 1U;
		parent.physical_generation = 1U;
		parent.state = cxxlens::sdk::publication_state::committed;
		auto parent_id = detail::publication_record_identity(
			parent.series_id, parent.snapshot_id, parent.sequence, parent.parent_publication);
		require(parent_id.has_value(), "expected-head publication identity could not be made");
		parent.publication_id = *parent_id;
		detail::bounded_store_v6_snapshot_observation parent_snapshot{
			parent.snapshot_id,
			1U,
			1U,
			1U,
			1U,
			1U,
			"sha256:" + std::string(64U, '1'),
			"sha256:" + std::string(64U, '2')};
		auto publication_head = detail::make_bounded_store_v6_publication_head(
			head_rebound.metadata.selector, std::move(parent), std::move(parent_snapshot));
		require(publication_head.has_value(), "expected publication head could not be sealed");
		head_rebound.metadata.backend = detail::bounded_store_v6_backend::sqlite;
		head_rebound.metadata.exact_sqlite_path = "/tmp/cxxlens-v6-head-binding.sqlite";
		head_rebound.metadata.expected_head = *publication_head;
		auto head_backend = std::make_unique<sqlite_kind_backend>(std::move(*inner_head_backend));
		auto head_prepared = stage(head_rebound, std::move(head_backend));
		require(head_prepared &&
					head_prepared->observation().candidate_id !=
						left_prepared->observation().candidate_id,
				"candidate identity discarded the complete expected head");
		auto overflow = detail::checked_bounded_store_v6_record_frame_bytes(
			std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<std::uint64_t>::max());
		require(!overflow && overflow.error().code == "store.resource-limit" &&
					overflow.error().field == "record-bytes",
				"frame overflow was not rejected before allocation");
		for (const auto accepted : {1U, 4095U, 4096U})
			require(detail::validate_bounded_store_v6_task_count(accepted).has_value(),
					"supported task count was rejected");
		for (const auto rejected : {0U, 4097U})
		{
			auto tasks = detail::validate_bounded_store_v6_task_count(rejected);
			require(!tasks && tasks.error().code == "store.resource-limit" &&
						tasks.error().field == "tasks",
					"unsupported task count was not rejected before allocation");
		}
		const auto aggregate_minus_one = detail::checked_bounded_store_v6_aggregate_charge(
			0U, detail::bounded_store_v6_max_aggregate_bytes - 1U);
		const auto aggregate_exact = detail::checked_bounded_store_v6_aggregate_charge(
			detail::bounded_store_v6_max_aggregate_bytes - 1U, 1U);
		const auto aggregate_plus_one = detail::checked_bounded_store_v6_aggregate_charge(
			detail::bounded_store_v6_max_aggregate_bytes, 1U);
		const auto aggregate_overflow = detail::checked_bounded_store_v6_aggregate_charge(
			std::numeric_limits<std::uint64_t>::max(), 1U);
		require(aggregate_minus_one &&
					*aggregate_minus_one == detail::bounded_store_v6_max_aggregate_bytes - 1U &&
					aggregate_exact &&
					*aggregate_exact == detail::bounded_store_v6_max_aggregate_bytes,
				"supported aggregate boundary was rejected");
		require(!aggregate_plus_one && !aggregate_overflow &&
					aggregate_plus_one.error().code == "store.resource-limit" &&
					aggregate_plus_one.error().field == "aggregate-bytes" &&
					aggregate_overflow.error().field == "aggregate-bytes",
				"aggregate overflow was not rejected before allocation or backend I/O");
		require(detail::bounded_store_v6_max_tasks == 4096U &&
					detail::bounded_store_v6_max_aggregate_bytes == 512U * 1024U * 1024U,
				"Memory product bounds drifted");
	}
} // namespace

int main()
{
	positive_publish_reopen_and_drain();
	memory_store_is_fresh_genesis_only();
	one_task_can_contain_two_partitions();
	annotation_census_is_independent_from_claim_occurrences();
	abort_cleanup_fault_is_attempted_once();
	report_release_failure_still_drains_backend_once();
	cleanup_dual_failure_and_abort_exception_are_retained_once();
	foreign_report_release_is_rejected_before_cleanup_effects();
	reopen_observation_verifier_rejects_identity_spoofs();
	nonstandard_source_exception_aborts_without_effect();
	projection_mismatch_is_zero_effect();
	explicit_not_attempted_is_zero_effect_and_cleanup_safe();
	memory_publish_errors_are_invariant_or_unverified();
	every_fresh_reopen_phase_failure_is_total_and_unverified();
	source_faults_and_one_shot_cursor();
	task_receipt_spoofs_are_rejected_without_publication();
	maximum_task_candidate_is_streamed_and_published();
	report_finalization_failure_is_one_shot_and_cleanup_safe();
	invalid_report_reservation_releases_once_without_effect();
	sqlite_writer_publish_tuple_matrix_is_closed();
	sqlite_exact_failed_effects_reach_all_typed_terminals();
	deterministic_ids_and_bounds();
	return EXIT_SUCCESS;
}
