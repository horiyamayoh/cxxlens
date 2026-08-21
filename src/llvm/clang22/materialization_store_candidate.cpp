#include "materialization_store_candidate.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <ranges>
#include <string_view>
#include <utility>

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		constexpr std::size_t record_header_bytes = 1U + sizeof(std::uint64_t) * 2U;
		constexpr std::size_t record_digest_bytes = 71U;

		[[nodiscard]] sdk::error
		failure(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool checked_add(const std::uint64_t left,
									   const std::uint64_t right,
									   const std::uint64_t maximum,
									   std::uint64_t& output) noexcept
		{
			if (left > maximum || right > maximum - left)
				return false;
			output = left + right;
			return true;
		}

		[[nodiscard]] bool valid_kind(const bounded_store_record_kind value) noexcept
		{
			return static_cast<std::uint8_t>(value) >= 1U &&
				static_cast<std::uint8_t>(value) <= 14U;
		}

		void put_u64(std::vector<std::byte>& output, const std::uint64_t value)
		{
			for (int shift = 56; shift >= 0; shift -= 8)
				output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
		}

		[[nodiscard]] std::uint64_t get_u64(const std::span<const std::byte> input) noexcept
		{
			std::uint64_t output{};
			for (const auto byte : input)
				output = (output << 8U) | std::to_integer<std::uint8_t>(byte);
			return output;
		}

		[[nodiscard]] bool exact_digest(const std::string_view value) noexcept
		{
			return value.size() == 71U && value.starts_with("sha256:") &&
				std::ranges::all_of(value.substr(7U),
									[](const char byte)
									{
										return (byte >= '0' && byte <= '9') ||
											(byte >= 'a' && byte <= 'f');
									});
		}

		[[nodiscard]] std::string record_order_key(const bounded_store_record& record)
		{
			std::string output;
			output.reserve(1U + record.key.size());
			output.push_back(static_cast<char>(record.kind));
			output.append(record.key);
			return output;
		}

		class record_spool_impl;

		class record_cursor_impl final : public bounded_store_record_cursor
		{
		  public:
			explicit record_cursor_impl(const record_spool_impl& spool) noexcept : spool_{&spool} {}
			[[nodiscard]] sdk::result<std::optional<bounded_store_record>> next() override;

		  private:
			const record_spool_impl* spool_{};
			std::uint64_t offset_{};
		};

		class record_spool_impl final : public bounded_store_record_spool
		{
		  public:
			record_spool_impl(std::unique_ptr<materialization_replayable_spool> storage,
							  bounded_store_limits limits) noexcept
				: storage_{std::move(storage)}, limits_{limits}
			{
			}

			[[nodiscard]] sdk::result<void> append(const bounded_store_record& record) override
			{
				if (!storage_ || sealed_)
					return sdk::unexpected(
						failure("store.candidate-state", "spool", "append-after-seal"));
				auto encoded = encode_bounded_store_record(record, limits_);
				if (!encoded)
					return sdk::unexpected(std::move(encoded.error()));
				std::uint64_t next{};
				if (!checked_add(bytes_, encoded->size(), limits_.max_spool_bytes, next))
					return sdk::unexpected(
						failure("store.resource-limit", "spool-bytes", "checked-overflow"));
				if (auto appended = storage_->append(*encoded); !appended)
					return sdk::unexpected(
						failure("store.spool-failure", "append", "private-spool"));
				bytes_ = next;
				++record_count_;
				return {};
			}

			[[nodiscard]] sdk::result<void> seal() override
			{
				if (!storage_)
					return sdk::unexpected(
						failure("store.candidate-state", "spool", "storage-missing"));
				if (sealed_)
					return {};
				if (auto sealed = storage_->seal(); !sealed)
					return sdk::unexpected(failure("store.spool-failure", "seal", "private-spool"));
				sealed_ = true;
				return {};
			}

			[[nodiscard]] sdk::result<std::unique_ptr<bounded_store_record_cursor>>
			open_cursor() const override
			{
				if (!storage_ || !sealed_ || !storage_->sealed())
					return sdk::unexpected(
						failure("store.candidate-state", "spool", "cursor-before-seal"));
				return std::unique_ptr<bounded_store_record_cursor>{new record_cursor_impl{*this}};
			}

			[[nodiscard]] std::uint64_t record_count() const noexcept override
			{
				return record_count_;
			}
			[[nodiscard]] std::uint64_t byte_count() const noexcept override
			{
				return bytes_;
			}
			[[nodiscard]] bool sealed() const noexcept override
			{
				return sealed_ && storage_ && storage_->sealed();
			}

			[[nodiscard]] materialization_replayable_spool& storage() const noexcept
			{
				return *storage_;
			}
			[[nodiscard]] const bounded_store_limits& limits() const noexcept
			{
				return limits_;
			}

		  private:
			std::unique_ptr<materialization_replayable_spool> storage_;
			bounded_store_limits limits_;
			std::uint64_t bytes_{};
			std::uint64_t record_count_{};
			bool sealed_{};

			friend class record_cursor_impl;
		};

		[[nodiscard]] sdk::result<void> read_exact(materialization_replayable_spool& source,
												   const std::uint64_t offset,
												   const std::span<std::byte> output)
		{
			std::size_t copied{};
			while (copied < output.size())
			{
				auto read = source.read_at(offset + copied, output.subspan(copied));
				if (!read)
					return sdk::unexpected(failure("store.spool-failure", "read", "private-spool"));
				if (*read == 0U || *read > output.size() - copied)
					return sdk::unexpected(
						failure("store.corrupt", "projection", "truncated-record"));
				copied += *read;
			}
			return {};
		}

		sdk::result<std::optional<bounded_store_record>> record_cursor_impl::next()
		{
			if (spool_ == nullptr)
				return sdk::unexpected(
					failure("store.candidate-state", "cursor", "storage-missing"));
			if (offset_ == spool_->byte_count())
				return std::optional<bounded_store_record>{};
			if (offset_ > spool_->byte_count() ||
				spool_->byte_count() - offset_ < record_header_bytes + record_digest_bytes)
				return sdk::unexpected(failure("store.corrupt", "projection", "truncated-header"));

			std::array<std::byte, record_header_bytes> header{};
			if (auto read = read_exact(spool_->storage(), offset_, header); !read)
				return sdk::unexpected(std::move(read.error()));
			const auto key_size = get_u64(std::span{header}.subspan(1U, sizeof(std::uint64_t)));
			const auto payload_size = get_u64(
				std::span{header}.subspan(1U + sizeof(std::uint64_t), sizeof(std::uint64_t)));
			std::uint64_t body_size{};
			std::uint64_t frame_size{};
			if (!checked_add(
					record_header_bytes, key_size, spool_->limits().max_record_bytes, body_size) ||
				!checked_add(
					body_size, payload_size, spool_->limits().max_record_bytes, body_size) ||
				!checked_add(body_size,
							 record_digest_bytes,
							 spool_->limits().max_record_bytes,
							 frame_size) ||
				frame_size > spool_->byte_count() - offset_ ||
				frame_size > std::numeric_limits<std::size_t>::max())
				return sdk::unexpected(failure("store.corrupt", "projection", "record-length"));
			std::vector<std::byte> frame(static_cast<std::size_t>(frame_size));
			if (auto read = read_exact(spool_->storage(), offset_, frame); !read)
				return sdk::unexpected(std::move(read.error()));
			auto decoded = decode_bounded_store_record(frame, spool_->limits());
			if (!decoded)
				return sdk::unexpected(std::move(decoded.error()));
			offset_ += frame_size;
			return std::optional<bounded_store_record>{std::move(*decoded)};
		}

		[[nodiscard]] sdk::result<void>
		validate_actual_order(const bounded_store_record_spool& spool)
		{
			auto cursor = spool.open_cursor();
			if (!cursor)
				return sdk::unexpected(std::move(cursor.error()));
			std::optional<std::string> prior;
			for (;;)
			{
				auto next = (*cursor)->next();
				if (!next)
					return sdk::unexpected(std::move(next.error()));
				if (!*next)
					return {};
				auto order = record_order_key(**next);
				if (prior && order <= *prior)
					return sdk::unexpected(
						failure("store.corrupt", "actual-projection", "physical-key-order"));
				prior = std::move(order);
			}
		}
	} // namespace

	sdk::result<std::vector<std::byte>>
	encode_bounded_store_record(const bounded_store_record& record,
								const bounded_store_limits& limits)
	{
		if (!valid_kind(record.kind))
			return sdk::unexpected(failure("store.corrupt", "record-kind", "unknown"));
		std::uint64_t body_size{};
		std::uint64_t frame_size{};
		if (record.key.size() > std::numeric_limits<std::uint64_t>::max() ||
			record.payload.size() > std::numeric_limits<std::uint64_t>::max() ||
			!checked_add(
				record_header_bytes, record.key.size(), limits.max_record_bytes, body_size) ||
			!checked_add(body_size, record.payload.size(), limits.max_record_bytes, body_size) ||
			!checked_add(body_size, record_digest_bytes, limits.max_record_bytes, frame_size) ||
			frame_size > std::numeric_limits<std::size_t>::max())
			return sdk::unexpected(
				failure("store.resource-limit", "record-bytes", "checked-overflow"));

		std::vector<std::byte> body;
		body.reserve(static_cast<std::size_t>(body_size));
		body.push_back(static_cast<std::byte>(record.kind));
		put_u64(body, record.key.size());
		put_u64(body, record.payload.size());
		body.insert(body.end(),
					std::as_bytes(std::span{record.key.data(), record.key.size()}).begin(),
					std::as_bytes(std::span{record.key.data(), record.key.size()}).end());
		body.insert(body.end(), record.payload.begin(), record.payload.end());
		const auto digest = sdk::content_digest(body);
		if (!exact_digest(digest))
			return sdk::unexpected(failure("store.hash-failure", "record", "digest"));
		body.insert(body.end(),
					std::as_bytes(std::span{digest.data(), digest.size()}).begin(),
					std::as_bytes(std::span{digest.data(), digest.size()}).end());
		return body;
	}

	sdk::result<bounded_store_record>
	decode_bounded_store_record(const std::span<const std::byte> bytes,
								const bounded_store_limits& limits)
	{
		if (bytes.size() < record_header_bytes + record_digest_bytes ||
			bytes.size() > limits.max_record_bytes)
			return sdk::unexpected(failure("store.corrupt", "record", "length"));
		const auto kind =
			static_cast<bounded_store_record_kind>(std::to_integer<std::uint8_t>(bytes.front()));
		if (!valid_kind(kind))
			return sdk::unexpected(failure("store.corrupt", "record-kind", "unknown"));
		const auto key_size = get_u64(bytes.subspan(1U, sizeof(std::uint64_t)));
		const auto payload_size =
			get_u64(bytes.subspan(1U + sizeof(std::uint64_t), sizeof(std::uint64_t)));
		std::uint64_t body_size{};
		std::uint64_t expected_size{};
		if (!checked_add(record_header_bytes, key_size, limits.max_record_bytes, body_size) ||
			!checked_add(body_size, payload_size, limits.max_record_bytes, body_size) ||
			!checked_add(body_size, record_digest_bytes, limits.max_record_bytes, expected_size) ||
			expected_size != bytes.size() || key_size > std::numeric_limits<std::size_t>::max() ||
			payload_size > std::numeric_limits<std::size_t>::max())
			return sdk::unexpected(failure("store.corrupt", "record", "length"));
		const auto key_offset = record_header_bytes;
		const auto payload_offset = key_offset + static_cast<std::size_t>(key_size);
		const auto digest_offset = payload_offset + static_cast<std::size_t>(payload_size);
		const auto digest_bytes = bytes.subspan(digest_offset, record_digest_bytes);
		const std::string observed_digest{reinterpret_cast<const char*>(digest_bytes.data()),
										  digest_bytes.size()};
		if (!exact_digest(observed_digest))
			return sdk::unexpected(failure("store.corrupt", "record", "digest-format"));
		const auto expected_digest = sdk::content_digest(bytes.first(digest_offset));
		if (observed_digest != expected_digest)
			return sdk::unexpected(failure("store.corrupt", "record", "checksum"));
		bounded_store_record output;
		output.kind = kind;
		output.key.assign(reinterpret_cast<const char*>(bytes.data() + key_offset),
						  static_cast<std::size_t>(key_size));
		output.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset),
							  bytes.begin() + static_cast<std::ptrdiff_t>(digest_offset));
		return output;
	}

	sdk::result<std::unique_ptr<bounded_store_record_spool>>
	make_bounded_store_record_spool(std::unique_ptr<materialization_replayable_spool> storage,
									bounded_store_limits limits)
	{
		if (!storage || limits.max_record_bytes < record_header_bytes + record_digest_bytes ||
			limits.max_spool_bytes == 0U || limits.max_aggregate_bytes == 0U)
			return sdk::unexpected(failure("store.resource-limit", "limits", "invalid"));
		return std::unique_ptr<bounded_store_record_spool>{
			new record_spool_impl{std::move(storage), limits}};
	}

	struct bounded_store_report_writer::state
	{
		std::unique_ptr<materialization_private_spool> storage;
		bounded_store_limits limits;
		std::uint64_t bytes{};
		std::optional<bounded_store_publication_terminal> terminal;
		bool reserved{};
		bool finalized{};
	};

	bounded_store_report_writer::bounded_store_report_writer(std::unique_ptr<state> state_value)
		: state_{std::move(state_value)}
	{
	}
	bounded_store_report_writer::bounded_store_report_writer(
		bounded_store_report_writer&&) noexcept = default;
	bounded_store_report_writer&
	bounded_store_report_writer::operator=(bounded_store_report_writer&&) noexcept = default;
	bounded_store_report_writer::~bounded_store_report_writer() = default;

	sdk::result<void> bounded_store_report_writer::reserve()
	{
		if (!state_ || !state_->storage || state_->reserved || state_->finalized)
			return sdk::unexpected(failure("store.candidate-state", "report", "reserve"));
		state_->reserved = true;
		return {};
	}

	sdk::result<void> bounded_store_report_writer::append(const std::span<const std::byte> bytes)
	{
		if (!state_ || !state_->reserved || state_->finalized)
			return sdk::unexpected(failure("store.candidate-state", "report", "append"));
		std::uint64_t next{};
		if (!checked_add(state_->bytes, bytes.size(), state_->limits.report_tail_bytes, next))
			return sdk::unexpected(
				failure("store.resource-limit", "report-tail", "checked-overflow"));
		if (!checked_add(next, 0U, state_->limits.max_report_bytes, next))
			return sdk::unexpected(
				failure("store.resource-limit", "report-bytes", "checked-overflow"));
		if (auto appended = state_->storage->append(bytes); !appended)
			return sdk::unexpected(failure("store.spool-failure", "report", "private-spool"));
		state_->bytes = next;
		return {};
	}

	sdk::result<void>
	bounded_store_report_writer::finalize(const bounded_store_publication_terminal terminal)
	{
		if (!state_ || !state_->reserved || state_->finalized || !is_valid(terminal))
			return sdk::unexpected(failure("store.candidate-state", "report", "finalize"));
		if (auto sealed = state_->storage->seal(); !sealed)
			return sdk::unexpected(failure("store.spool-failure", "report", "seal"));
		state_->terminal = terminal;
		state_->finalized = true;
		return {};
	}

	bool bounded_store_report_writer::reserved() const noexcept
	{
		return state_ && state_->reserved;
	}
	bool bounded_store_report_writer::finalized() const noexcept
	{
		return state_ && state_->finalized;
	}
	std::uint64_t bounded_store_report_writer::bytes_written() const noexcept
	{
		return state_ ? state_->bytes : 0U;
	}
	std::optional<bounded_store_publication_terminal>
	bounded_store_report_writer::terminal() const noexcept
	{
		return state_ ? state_->terminal : std::nullopt;
	}

	sdk::result<bounded_store_report_writer>
	make_bounded_store_report_writer(std::unique_ptr<materialization_private_spool> storage,
									 bounded_store_limits limits)
	{
		if (!storage || limits.report_tail_bytes == 0U ||
			limits.report_tail_bytes > limits.max_report_bytes)
			return sdk::unexpected(failure("store.resource-limit", "report", "invalid"));
		return bounded_store_report_writer{
			std::make_unique<bounded_store_report_writer::state>(bounded_store_report_writer::state{
				std::move(storage), limits, 0U, std::nullopt, false, false})};
	}

	struct bounded_store_candidate::state
	{
		bounded_store_candidate_phase phase{bounded_store_candidate_phase::staging_session_open};
		std::string staging_session_id;
		std::string expected_head;
		std::string candidate_id;
		bounded_store_limits limits;
		std::unique_ptr<bounded_store_record_spool> input;
		std::unique_ptr<bounded_store_record_spool> expected;
		std::unique_ptr<bounded_store_record_spool> actual;
		std::unique_ptr<materialization_digest_accumulator> input_digest;
		std::optional<bounded_store_publication_terminal> terminal;
		std::uint64_t task_count{};
		std::uint64_t input_bytes{};
		bool report_reserved{};
	};

	bounded_store_candidate::bounded_store_candidate(std::unique_ptr<state> state_value)
		: state_{std::move(state_value)}
	{
	}
	bounded_store_candidate::bounded_store_candidate(bounded_store_candidate&&) noexcept = default;
	bounded_store_candidate&
	bounded_store_candidate::operator=(bounded_store_candidate&&) noexcept = default;
	bounded_store_candidate::~bounded_store_candidate() = default;

	bounded_store_candidate_phase bounded_store_candidate::phase() const noexcept
	{
		return state_ ? state_->phase : bounded_store_candidate_phase::aborted;
	}
	std::string_view bounded_store_candidate::staging_session_id() const noexcept
	{
		return state_ ? state_->staging_session_id : std::string_view{};
	}
	std::string_view bounded_store_candidate::candidate_id() const noexcept
	{
		return state_ ? state_->candidate_id : std::string_view{};
	}
	std::optional<bounded_store_publication_terminal>
	bounded_store_candidate::publication_terminal() const noexcept
	{
		return state_ ? state_->terminal : std::nullopt;
	}

	sdk::result<void>
	bounded_store_candidate::append_task(const std::span<const std::byte> sealed_task)
	{
		if (!state_ ||
			(state_->phase != bounded_store_candidate_phase::staging_session_open &&
			 state_->phase != bounded_store_candidate_phase::appending))
			return sdk::unexpected(failure("store.candidate-state", "append-task", "phase"));
		if (sealed_task.empty())
			return sdk::unexpected(failure("store.input-invalid", "task", "empty"));
		if (state_->task_count >= state_->limits.max_tasks)
			return sdk::unexpected(failure("store.resource-limit", "tasks", "maximum"));
		std::uint64_t next_bytes{};
		if (!checked_add(state_->input_bytes,
						 sealed_task.size(),
						 state_->limits.max_aggregate_bytes,
						 next_bytes))
			return sdk::unexpected(
				failure("store.resource-limit", "input-bytes", "checked-overflow"));
		bounded_store_record record;
		record.kind = bounded_store_record_kind::task_result;
		record.key = std::to_string(state_->task_count);
		record.payload.assign(sealed_task.begin(), sealed_task.end());
		auto encoded = encode_bounded_store_record(record, state_->limits);
		if (!encoded)
			return sdk::unexpected(std::move(encoded.error()));
		if (auto appended = state_->input->append(record); !appended)
			return sdk::unexpected(std::move(appended.error()));
		if (auto updated = state_->input_digest->update(*encoded); !updated)
			return sdk::unexpected(failure("store.hash-failure", "input", "digest"));
		state_->input_bytes = next_bytes;
		++state_->task_count;
		state_->phase = bounded_store_candidate_phase::appending;
		return {};
	}

	sdk::result<void>
	bounded_store_candidate::seal_input(const bounded_store_external_census& census)
	{
		if (!state_ || state_->phase != bounded_store_candidate_phase::appending)
			return sdk::unexpected(failure("store.candidate-state", "seal-input", "phase"));
		if (state_->task_count == 0U || census.task_count != state_->task_count ||
			census.input_bytes != state_->input_bytes)
			return sdk::unexpected(
				failure("store.input-census-mismatch", "input", "count-or-bytes"));
		if (!state_->input->seal())
			return sdk::unexpected(failure("store.spool-failure", "input", "seal"));
		auto digest = state_->input_digest->finish();
		if (!digest)
			return sdk::unexpected(failure("store.hash-failure", "input", "digest"));
		if (*digest != census.input_digest)
			return sdk::unexpected(failure("store.input-census-mismatch", "input", "digest"));
		state_->phase = bounded_store_candidate_phase::input_sealed;
		const auto identity = sdk::semantic_digest("cxxlens.store.incremental-candidate.v1",
												   *digest + "\n" + state_->expected_head);
		if (!identity)
			return sdk::unexpected(failure("store.hash-failure", "candidate", "identity"));
		state_->candidate_id = std::move(*identity);
		state_->phase = bounded_store_candidate_phase::candidate_identity_sealed;
		return {};
	}

	sdk::result<void> bounded_store_candidate::build_expected_projection(projection_builder builder)
	{
		if (!state_ || state_->phase != bounded_store_candidate_phase::candidate_identity_sealed ||
			!builder)
			return sdk::unexpected(
				failure("store.candidate-state", "expected-projection", "phase"));
		state_->phase = bounded_store_candidate_phase::independently_validating;
		if (auto built = builder(*state_->expected); !built)
			return sdk::unexpected(std::move(built.error()));
		if (state_->expected->record_count() == 0U)
			return sdk::unexpected(failure("store.corrupt", "expected-projection", "empty"));
		if (auto sealed = state_->expected->seal(); !sealed)
			return sdk::unexpected(std::move(sealed.error()));
		state_->phase = bounded_store_candidate_phase::expected_projection_sealed;
		return {};
	}

	sdk::result<void> bounded_store_candidate::build_actual_projection(projection_builder builder)
	{
		if (!state_ || state_->phase != bounded_store_candidate_phase::expected_projection_sealed ||
			!builder)
			return sdk::unexpected(failure("store.candidate-state", "actual-projection", "phase"));
		if (auto built = builder(*state_->actual); !built)
			return sdk::unexpected(std::move(built.error()));
		if (state_->actual->record_count() == 0U)
			return sdk::unexpected(failure("store.corrupt", "actual-projection", "empty"));
		if (auto sealed = state_->actual->seal(); !sealed)
			return sdk::unexpected(std::move(sealed.error()));
		if (auto ordered = validate_actual_order(*state_->actual); !ordered)
			return sdk::unexpected(std::move(ordered.error()));
		state_->phase = bounded_store_candidate_phase::actual_projection_sealed;
		return {};
	}

	sdk::result<void> bounded_store_candidate::compare_projections()
	{
		if (!state_ || state_->phase != bounded_store_candidate_phase::actual_projection_sealed)
			return sdk::unexpected(failure("store.candidate-state", "compare", "phase"));
		auto expected = state_->expected->open_cursor();
		auto actual = state_->actual->open_cursor();
		if (!expected || !actual)
			return sdk::unexpected(failure("store.corrupt", "projection", "cursor"));
		for (;;)
		{
			auto expected_record = (*expected)->next();
			auto actual_record = (*actual)->next();
			if (!expected_record || !actual_record)
				return sdk::unexpected(failure("store.corrupt", "projection", "cursor"));
			if (!*expected_record && !*actual_record)
				break;
			if (!*expected_record || !*actual_record || **expected_record != **actual_record)
				return sdk::unexpected(
					failure("store.corrupt", "projection", "full-byte-mismatch"));
			auto expected_bytes = encode_bounded_store_record(**expected_record, state_->limits);
			auto actual_bytes = encode_bounded_store_record(**actual_record, state_->limits);
			if (!expected_bytes || !actual_bytes || *expected_bytes != *actual_bytes)
				return sdk::unexpected(
					failure("store.corrupt", "projection", "full-byte-mismatch"));
		}
		state_->phase = bounded_store_candidate_phase::validation_sealed;
		return {};
	}

	sdk::result<void>
	bounded_store_candidate::reserve_report_tail(bounded_store_report_writer& report)
	{
		if (!state_ || state_->phase != bounded_store_candidate_phase::validation_sealed ||
			state_->report_reserved)
			return sdk::unexpected(failure("store.candidate-state", "report-tail", "phase"));
		if (auto reserved = report.reserve(); !reserved)
			return sdk::unexpected(std::move(reserved.error()));
		state_->report_reserved = true;
		state_->phase = bounded_store_candidate_phase::report_tail_reserved;
		return {};
	}

	sdk::result<void> bounded_store_candidate::finish_without_publication()
	{
		if (!state_ || state_->phase != bounded_store_candidate_phase::report_tail_reserved)
			return sdk::unexpected(failure("store.candidate-state", "publish", "phase"));
		state_->terminal = bounded_store_publication_terminal::not_attempted;
		state_->phase = bounded_store_candidate_phase::publication_terminal;
		return {};
	}

	sdk::result<void> bounded_store_candidate::publish_once(bounded_store_publication_port& backend)
	{
		if (!state_ || state_->phase != bounded_store_candidate_phase::report_tail_reserved ||
			state_->terminal)
			return sdk::unexpected(failure("store.candidate-state", "publish", "replay-or-phase"));
		state_->phase = bounded_store_candidate_phase::publication_attempted_once;
		bounded_store_publication_terminal terminal{
			bounded_store_publication_terminal::publication_outcome_unknown};
		try
		{
			terminal = backend.publish_once(state_->candidate_id, state_->expected_head);
		}
		catch (...)
		{
			state_->terminal = bounded_store_publication_terminal::publication_outcome_unknown;
			state_->phase = bounded_store_candidate_phase::publication_terminal;
			return sdk::unexpected(
				failure("store.publication-outcome-unknown", "publish", "backend-exception"));
		}
		const bool valid_terminal =
			is_valid(terminal) && terminal != bounded_store_publication_terminal::not_attempted;
		state_->terminal = valid_terminal
			? terminal
			: bounded_store_publication_terminal::publication_outcome_unknown;
		state_->phase = bounded_store_candidate_phase::publication_terminal;
		if (!valid_terminal)
			return sdk::unexpected(failure(
				"store.publication-outcome-invalid", "publish", "not-attempted-or-invalid"));
		return {};
	}

	sdk::result<void> bounded_store_candidate::finalize_report(bounded_store_report_writer& report)
	{
		if (!state_ || state_->phase != bounded_store_candidate_phase::publication_terminal ||
			!state_->terminal)
			return sdk::unexpected(failure("store.candidate-state", "report", "terminal"));
		if (auto finalized = report.finalize(*state_->terminal); !finalized)
		{
			state_->phase = bounded_store_candidate_phase::report_transport_failed;
			return sdk::unexpected(std::move(finalized.error()));
		}
		state_->phase = bounded_store_candidate_phase::report_finalized;
		return {};
	}

	void bounded_store_candidate::abort() noexcept
	{
		if (!state_ || state_->phase == bounded_store_candidate_phase::publication_terminal ||
			state_->phase == bounded_store_candidate_phase::report_finalized ||
			state_->phase == bounded_store_candidate_phase::report_transport_failed)
			return;
		state_->phase = bounded_store_candidate_phase::aborted;
	}

	sdk::result<bounded_store_candidate>
	begin_bounded_store_candidate(std::string staging_session_id,
								  std::string expected_head,
								  const bounded_store_limits limits,
								  std::unique_ptr<bounded_store_record_spool> input,
								  std::unique_ptr<bounded_store_record_spool> expected,
								  std::unique_ptr<bounded_store_record_spool> actual)
	{
		if (staging_session_id.empty() || !input || !expected || !actual ||
			limits.max_tasks == 0U || limits.max_aggregate_bytes == 0U ||
			limits.max_record_bytes == 0U || limits.max_spool_bytes == 0U)
			return sdk::unexpected(failure("store.candidate-invalid", "begin", "configuration"));
		auto digest = make_materialization_sha256_accumulator();
		if (!digest)
			return sdk::unexpected(failure("store.hash-failure", "input", "allocation"));
		auto state = std::make_unique<bounded_store_candidate::state>();
		state->staging_session_id = std::move(staging_session_id);
		state->expected_head = std::move(expected_head);
		state->limits = limits;
		state->input = std::move(input);
		state->expected = std::move(expected);
		state->actual = std::move(actual);
		state->input_digest = std::move(digest);
		return bounded_store_candidate{std::move(state)};
	}
} // namespace cxxlens::detail::clang22::materialization
