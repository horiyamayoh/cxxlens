#include <array>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include "sdk/bounded_store_v6_internal.hpp"
#include "sdk/bounded_store_v6_memory_internal.hpp"
#include "sdk/bounded_store_v6_sqlite_internal.hpp"

namespace detail = cxxlens::sdk::detail;
using cxxlens::sdk::canonical_binary;
using cxxlens::sdk::canonical_value;
using cxxlens::sdk::content_digest;
using cxxlens::sdk::error;
using cxxlens::sdk::result;

namespace
{
	void require(const bool value, const std::string_view message)
	{
		if (!value)
		{
			std::cerr << message << '\n';
			std::exit(EXIT_FAILURE);
		}
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
		auto encoded = canonical_binary(
			canonical_value::from_tuple({canonical_value::from_string(std::string{value})}));
		require(encoded.has_value(), "canonical tuple encoding failed");
		return std::move(*encoded);
	}

	[[nodiscard]] std::array<std::byte, 32U> raw_digest(const std::string_view digest)
	{
		require(digest.starts_with("sha256:") && digest.size() == 71U,
				"non-canonical digest in test");
		std::array<std::byte, 32U> output{};
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

	[[nodiscard]] std::vector<detail::bounded_store_v6_memory_source_frame> frames()
	{
		const auto key = tuple("partition-0");
		const auto payload = tuple("partition-payload");
		std::vector<detail::bounded_store_v6_memory_source_frame> output;
		for (const auto kind : {detail::bounded_store_v6_record_kind::partition_begin,
								detail::bounded_store_v6_record_kind::partition_end})
		{
			auto frame = detail::encode_bounded_store_v6_memory_frame(kind, key, payload);
			require(frame.has_value(), "frame encoding failed");
			output.push_back({kind, std::move(*frame)});
		}
		return output;
	}

	[[nodiscard]] std::vector<std::byte>
	concatenate(const std::vector<detail::bounded_store_v6_memory_source_frame>& source)
	{
		std::vector<std::byte> output;
		for (const auto& frame : source)
			output.insert(output.end(), frame.bytes.begin(), frame.bytes.end());
		return output;
	}

	class report_writer final : public detail::bounded_store_report_tail_writer
	{
	  public:
		result<detail::bounded_store_v6_report_tail_reservation>
		reserve_maximum_tail(const std::uint64_t tail, const std::uint64_t maximum) override
		{
			if (tail != detail::bounded_store_v6_exact_report_tail_bytes ||
				maximum != detail::bounded_store_v6_max_report_bytes)
				return error{"test.report", "tail", "contract"};
			reserved_ = true;
			return detail::bounded_store_v6_report_tail_reservation{"writer:sqlite",
																	"spool:sqlite",
																	"reservation:sqlite",
																	1024U,
																	tail,
																	maximum,
																	maximum};
		}
		result<void> append_terminal(detail::bounded_store_v6_publication_terminal value) override
		{
			terminal_ = value;
			return reserved_ ? result<void>{} : error{"test.report", "terminal", "unreserved"};
		}
		result<void> validate_full_schema() override
		{
			return terminal_ ? result<void>{} : error{"test.report", "schema", "terminal"};
		}
		result<void> validate_complete_section_census(const std::uint32_t count) override
		{
			return terminal_ && count == detail::bounded_store_v6_report_section_count
				? result<void>{}
				: error{"test.report", "section-census", "mismatch"};
		}
		result<void> validate_bottom_up_bindings() override
		{
			return terminal_ ? result<void>{} : error{"test.report", "bindings", "terminal"};
		}
		result<std::uint64_t> sealed_report_bytes() const override
		{
			return terminal_ && reserved_
				? result<std::uint64_t>{1024U + detail::bounded_store_v6_exact_report_tail_bytes}
				: error{"test.report", "bytes", "not-sealed"};
		}
		result<void> release() override
		{
			++release_calls;
			return {};
		}
		std::uint64_t release_calls{};

	  private:
		bool reserved_{};
		std::optional<detail::bounded_store_v6_publication_terminal> terminal_;
	};

	void sqlite_v6_phase_round_trip()
	{
		const auto source = frames();
		auto bytes = concatenate(source);
		const auto binary = raw_digest(content_digest(bytes));
		const auto selected = selector();
		auto head = detail::make_bounded_store_v6_genesis_head(selected);
		require(head.has_value(), "genesis head failed");
		detail::bounded_store_v6_session_metadata metadata{detail::bounded_store_v6_backend::sqlite,
														   "engine-generation:test",
														   selected,
														   "sqlite-v6-test-session",
														   "/tmp/cxxlens-store-v6-sqlite-test.db",
														   *head};
		std::remove(metadata.exact_sqlite_path->c_str());
		auto session = detail::bounded_store_v6_phase_core::begin_staging_session(metadata);
		require(session.has_value(), "sqlite session failed");
		detail::bounded_store_v6_external_census census{1U,
														1U,
														2U,
														0U,
														0U,
														0U,
														0U,
														0U,
														static_cast<std::uint64_t>(bytes.size()),
														binary,
														"authority:sqlite-test"};
		auto input = detail::bounded_store_v6_phase_core::seal_input(std::move(*session), census);
		require(input.has_value(), "sqlite input seal failed");
		auto backend = detail::make_bounded_store_v6_sqlite_backend_port(metadata);
		require(backend.has_value(), "sqlite backend factory failed");
		auto prepared = detail::bounded_store_v6_phase_core::prepare_publication(
			std::move(*input), std::move(*backend));
		require(prepared.has_value(), "sqlite preparation failed");
		detail::bounded_store_v6_task_receipt receipt{"task:sqlite",
													  0U,
													  1U,
													  2U,
													  0U,
													  0U,
													  0U,
													  0U,
													  0U,
													  static_cast<std::uint64_t>(bytes.size()),
													  binary,
													  "receipt:sqlite"};
		auto task_source = detail::make_bounded_store_v6_memory_task_frame_source(source);
		require(task_source.has_value(), "sqlite task source failed");
		auto task = detail::bounded_store_v6_phase_core::seal_task_source(std::move(receipt),
																		  std::move(*task_source));
		require(task.has_value(), "sqlite task seal failed");
		require(static_cast<bool>(detail::bounded_store_v6_phase_core::stage_from_source(
					*prepared, std::move(*task))),
				"sqlite staging failed");
		require(static_cast<bool>(
					detail::bounded_store_v6_phase_core::seal_prepared_publication(*prepared)),
				"sqlite staging seal failed");
		auto expected_source =
			detail::make_bounded_store_v6_memory_expected_semantic_source(source);
		require(expected_source.has_value(), "sqlite expected source failed");
		auto expected = detail::bounded_store_v6_phase_core::seal_expected_projection(
			std::move(*expected_source), *prepared);
		require(expected.has_value(), "sqlite expected seal failed");
		auto actual = detail::bounded_store_v6_phase_core::take_physical_actual_cursor(*prepared);
		require(actual.has_value(), "sqlite physical cursor failed");
		auto match = detail::bounded_store_v6_phase_core::compare_bounded_store_projections(
			*prepared, std::move(*expected), std::move(*actual));
		require(match.has_value(), "sqlite physical comparison failed");
		auto report = detail::bounded_store_v6_phase_core::reserve_report_tail(
			*prepared, *match, std::make_unique<report_writer>());
		require(report.has_value(), "sqlite report reservation failed");
		auto validated = detail::bounded_store_v6_phase_core::bind_publication(
			std::move(*prepared), std::move(*match), std::move(*report));
		require(validated.has_value(), "sqlite publication binding failed");
		auto terminal = detail::bounded_store_v6_phase_core::publish_once(std::move(*validated));
		require(terminal.has_value() &&
					terminal->observation().terminal ==
						detail::bounded_store_v6_publication_terminal::committed_verified,
				"sqlite publication/reopen verification failed");
		auto release = detail::bounded_store_v6_phase_core::finalize_and_validate_report(*terminal);
		require(release.has_value(), "sqlite report finalization failed");
		auto cleanup = detail::bounded_store_v6_phase_core::drain(*terminal, std::move(*release));
		require(cleanup.has_value() && cleanup->drained, "sqlite cleanup failed");
		std::remove(metadata.exact_sqlite_path->c_str());
	}
} // namespace

int main()
{
	sqlite_v6_phase_round_trip();
	return 0;
}
