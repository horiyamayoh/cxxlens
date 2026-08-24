#include "materialization_store_memory_backend.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <new>
#include <ranges>
#include <utility>

#include "materialization_io.hpp"

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		constexpr std::size_t wire_header_bytes =
			static_cast<std::size_t>(bounded_memory_backend_wire_overhead - 71U);
		constexpr std::size_t wire_digest_bytes = 71U;

		[[nodiscard]] sdk::error
		failure(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::error io_failure(const std::string_view field,
											const materialization_io_failure& value)
		{
			return failure("store.memory-io",
						   std::string{field},
						   std::to_string(static_cast<unsigned>(value.operation)) + ":" +
							   std::to_string(static_cast<unsigned>(value.kind)));
		}

		[[nodiscard]] bool valid_kind(const bounded_memory_record_kind value) noexcept
		{
			return value >= bounded_memory_record_kind::task_result &&
				value <= bounded_memory_record_kind::metadata;
		}

		[[nodiscard]] bool valid_digest(const std::string_view value) noexcept
		{
			return value.size() == wire_digest_bytes && value.starts_with("sha256:") &&
				std::ranges::all_of(value.substr(7U),
									[](const char byte)
									{
										return (byte >= '0' && byte <= '9') ||
											(byte >= 'a' && byte <= 'f');
									});
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

		[[nodiscard]] sdk::result<std::uint64_t>
		checked_frame_bytes(const std::uint64_t key_bytes,
							const std::uint64_t payload_bytes,
							const bounded_memory_backend_limits& limits)
		{
			std::uint64_t body{};
			std::uint64_t frame{};
			if (!checked_add(wire_header_bytes, key_bytes, limits.max_record_bytes, body) ||
				!checked_add(body, payload_bytes, limits.max_record_bytes, body) ||
				!checked_add(body, wire_digest_bytes, limits.max_record_bytes, frame) ||
				frame > std::numeric_limits<std::size_t>::max())
				return sdk::unexpected(
					failure("store.resource-limit", "record-bytes", "checked-overflow"));
			return frame;
		}

		void put_u64(std::span<std::byte> output, const std::uint64_t value) noexcept
		{
			for (std::size_t index{}; index < sizeof(std::uint64_t); ++index)
				output[index] = static_cast<std::byte>(
					(value >> ((sizeof(std::uint64_t) - index - 1U) * 8U)) & 0xffU);
		}

		[[nodiscard]] std::uint64_t get_u64(const std::span<const std::byte> input) noexcept
		{
			std::uint64_t value{};
			for (const auto byte : input)
				value = (value << 8U) | std::to_integer<std::uint8_t>(byte);
			return value;
		}

		[[nodiscard]] sdk::result<std::string> sha256_parts(const std::span<const std::byte> first,
															const std::span<const std::byte> second,
															const std::span<const std::byte> third)
		{
			auto digest = make_materialization_sha256_accumulator();
			if (!digest)
				return sdk::unexpected(failure("store.hash-failure", "accumulator", "missing"));
			for (const auto bytes : {first, second, third})
				if (!bytes.empty())
					if (auto updated = digest->update(bytes); !updated)
						return sdk::unexpected(io_failure("digest-update", updated.error()));
			auto finished = digest->finish();
			if (!finished)
				return sdk::unexpected(io_failure("digest-finish", finished.error()));
			if (!valid_digest(*finished))
				return sdk::unexpected(failure("store.hash-failure", "digest", "invalid"));
			return std::move(*finished);
		}

		[[nodiscard]] sdk::result<std::string>
		payload_digest(const std::span<const std::byte> payload)
		{
			return sha256_parts(payload, {}, {});
		}

		[[nodiscard]] sdk::result<void> validate_record(const bounded_memory_record& record,
														const bounded_memory_backend_limits& limits)
		{
			if (!valid_kind(record.kind))
				return sdk::unexpected(failure("store.memory-record-invalid", "kind", "unknown"));
			if (record.key.empty() || record.key.find('\0') != std::string::npos)
				return sdk::unexpected(
					failure("store.memory-record-invalid", "key", "empty-or-nul"));
			if (auto valid = sdk::validate_utf8_text(record.key); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (record.key.size() > std::numeric_limits<std::uint64_t>::max() ||
				record.payload.size() > std::numeric_limits<std::uint64_t>::max())
				return sdk::unexpected(failure("store.resource-limit", "record-bytes", "size"));
			if (auto frame = checked_frame_bytes(record.key.size(), record.payload.size(), limits);
				!frame)
				return sdk::unexpected(std::move(frame.error()));
			else if (*frame > limits.max_window_bytes)
				return sdk::unexpected(failure("store.resource-limit", "record-window", "bound"));
			return {};
		}

		[[nodiscard]] sdk::result<std::string>
		publication_id(const bounded_memory_publication& value)
		{
			return derive_bounded_memory_publication_id(
				value.candidate_id, value.parent_publication, value.payload_digest, value.sequence);
		}

		[[nodiscard]] sdk::result<void>
		validate_publication(const bounded_memory_publication& value)
		{
			if (value.publication_id.empty() || value.candidate_id.empty() ||
				value.parent_publication.empty() || !valid_digest(value.payload_digest) ||
				value.sequence == 0U || value.task_count == 0U || value.record_count == 0U)
				return sdk::unexpected(
					failure("store.memory-publication-invalid", "identity", "shape"));
			auto expected = publication_id(value);
			if (!expected)
				return sdk::unexpected(std::move(expected.error()));
			if (*expected != value.publication_id)
				return sdk::unexpected(
					failure("store.memory-publication-invalid", "publication-id", "recomputed"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		validate_candidate_and_head(const std::string_view candidate_id,
									const std::string_view expected_head)
		{
			if (auto valid = sdk::validate_strong_id(candidate_id); !valid)
				return sdk::unexpected(
					failure("store.memory-session-invalid", "candidate-id", "strong-id"));
			if (expected_head != "genesis")
				if (auto valid = sdk::validate_strong_id(expected_head); !valid)
					return sdk::unexpected(
						failure("store.memory-session-invalid", "expected-head", "strong-id"));
			return {};
		}

		struct physical_payload
		{
			std::shared_ptr<const std::vector<std::byte>> bytes;
			bounded_memory_backend_limits limits;
		};

		struct publication_state
		{
			bounded_memory_publication publication;
			std::shared_ptr<const std::vector<std::byte>> payload;
			bounded_memory_backend_limits limits;
		};

		[[nodiscard]] sdk::result<bounded_memory_record> decode_record(
			const physical_payload& source, std::uint64_t& offset, std::uint64_t& window_bytes)
		{
			if (!source.bytes)
				return sdk::unexpected(
					failure("store.memory-cursor-invalid", "payload", "missing"));
			const auto& bytes = *source.bytes;
			if (offset == bytes.size())
				return sdk::unexpected(failure("store.memory-cursor-eof", "cursor", "eof"));
			if (offset > bytes.size() ||
				bytes.size() - offset < wire_header_bytes + wire_digest_bytes)
				return sdk::unexpected(
					failure("store.memory-corrupt", "record", "truncated-header"));
			const auto header_offset = static_cast<std::size_t>(offset);
			const auto header =
				std::span<const std::byte>{bytes}.subspan(header_offset, wire_header_bytes);
			const auto key_bytes = get_u64(header.subspan(1U, sizeof(std::uint64_t)));
			const auto payload_bytes =
				get_u64(header.subspan(1U + sizeof(std::uint64_t), sizeof(std::uint64_t)));
			auto frame = checked_frame_bytes(key_bytes, payload_bytes, source.limits);
			if (!frame)
				return sdk::unexpected(std::move(frame.error()));
			if (*frame > bytes.size() - offset)
				return sdk::unexpected(
					failure("store.memory-corrupt", "record", "truncated-frame"));
			if (*frame > source.limits.max_window_bytes)
				return sdk::unexpected(failure("store.resource-limit", "cursor-window", "bound"));
			const auto key_offset = header_offset + wire_header_bytes;
			const auto payload_offset = key_offset + static_cast<std::size_t>(key_bytes);
			const auto digest_offset = payload_offset + static_cast<std::size_t>(payload_bytes);
			try
			{
				std::string key{reinterpret_cast<const char*>(bytes.data() + key_offset),
								static_cast<std::size_t>(key_bytes)};
				std::vector<std::byte> payload{
					bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset),
					bytes.begin() + static_cast<std::ptrdiff_t>(digest_offset)};
				if (key.empty() || key.find('\0') != std::string::npos)
					return sdk::unexpected(
						failure("store.memory-corrupt", "record-key", "empty-or-nul"));
				if (auto valid = sdk::validate_utf8_text(key); !valid)
					return sdk::unexpected(std::move(valid.error()));
				const auto digest_bytes = std::span<const std::byte>{bytes}.subspan(
					header_offset,
					wire_header_bytes + static_cast<std::size_t>(key_bytes) +
						static_cast<std::size_t>(payload_bytes));
				auto actual_digest = payload_digest(digest_bytes);
				if (!actual_digest)
					return sdk::unexpected(std::move(actual_digest.error()));
				const std::string expected_digest{
					reinterpret_cast<const char*>(bytes.data() + digest_offset), wire_digest_bytes};
				if (*actual_digest != expected_digest)
					return sdk::unexpected(
						failure("store.memory-corrupt", "record-digest", "mismatch"));
				const auto kind_value = std::to_integer<std::uint8_t>(header.front());
				const auto kind = static_cast<bounded_memory_record_kind>(kind_value);
				if (!valid_kind(kind))
					return sdk::unexpected(
						failure("store.memory-corrupt", "record-kind", "unknown"));
				offset += *frame;
				window_bytes = *frame;
				return bounded_memory_record{kind, std::move(key), std::move(payload)};
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(
					failure("store.resource-limit", "cursor-window", "allocation"));
			}
		}

		[[nodiscard]] sdk::result<std::shared_ptr<const std::vector<std::byte>>>
		freeze_payload(std::vector<std::byte> payload)
		{
			try
			{
				return std::make_shared<const std::vector<std::byte>>(std::move(payload));
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(
					failure("store.resource-limit", "final-payload", "allocation"));
			}
		}

		class ordinary_memory_cas_port final : public bounded_memory_cas_port
		{
		  public:
			[[nodiscard]] sdk::result<bounded_memory_publication_terminal>
			compare_exchange_once(const std::string_view expected_head,
								  const std::string_view observed_head,
								  effect commit) override
			{
				if (expected_head != observed_head)
					return bounded_memory_publication_terminal::rejected_stale;
				if (!commit)
					return bounded_memory_publication_terminal::rejected_store_failure;
				auto applied = commit();
				if (!applied)
					return sdk::unexpected(std::move(applied.error()));
				return bounded_memory_publication_terminal::committed_verified;
			}
		};
	} // namespace

	struct bounded_memory_backend::state
	{
		state(options value, std::shared_ptr<bounded_memory_cas_port> cas_port)
			: options_{std::move(value)},
			  cas_port_{cas_port ? std::move(cas_port)
								 : std::make_shared<ordinary_memory_cas_port>()}
		{
		}

		options options_;
		std::shared_ptr<bounded_memory_cas_port> cas_port_;
		mutable std::mutex mutex_;
		std::string head_{"genesis"};
		std::uint64_t next_sequence_{1U};
		std::map<std::string, publication_state, std::less<>> publications_;
	};

	struct bounded_memory_record_cursor::state
	{
		physical_payload payload;
		std::uint64_t offset{};
		std::uint64_t window_bytes{};
		std::uint64_t maximum_window_bytes{};
	};

	struct bounded_memory_backend_snapshot::state
	{
		publication_state publication;
	};

	struct bounded_memory_backend_session::state
	{
		std::shared_ptr<bounded_memory_backend::state> backend;
		bounded_memory_backend_limits limits;
		std::string candidate_id;
		std::string expected_head;
		std::vector<std::byte> payload;
		std::shared_ptr<const std::vector<std::byte>> frozen_payload;
		std::string payload_digest;
		std::uint64_t task_count{};
		std::uint64_t record_count{};
		bool failed{};
		bool sealed{};
		bool parity_verified{};
		bool publication_attempted{};
		std::optional<bounded_memory_publication_terminal> terminal;
		std::optional<bounded_memory_publication> publication;
	};

	sdk::result<void> bounded_memory_backend_limits::validate() const
	{
		if (max_tasks == 0U || max_tasks > bounded_memory_backend_max_tasks ||
			max_payload_bytes == 0U ||
			max_payload_bytes > bounded_memory_backend_max_payload_bytes ||
			max_record_bytes < bounded_memory_backend_wire_overhead ||
			max_record_bytes > bounded_memory_backend_max_record_bytes ||
			max_window_bytes < bounded_memory_backend_wire_overhead ||
			max_window_bytes > max_record_bytes)
			return sdk::unexpected(failure("store.resource-limit", "limits", "invalid"));
		return {};
	}

	bounded_memory_record_cursor::bounded_memory_record_cursor(std::shared_ptr<const state> value)
		: state_{std::make_unique<bounded_memory_record_cursor::state>(*value)}
	{
	}

	bounded_memory_record_cursor::~bounded_memory_record_cursor() = default;
	bounded_memory_record_cursor::bounded_memory_record_cursor(
		bounded_memory_record_cursor&&) noexcept = default;
	bounded_memory_record_cursor&
	bounded_memory_record_cursor::operator=(bounded_memory_record_cursor&&) noexcept = default;

	sdk::result<std::optional<bounded_memory_record>> bounded_memory_record_cursor::next()
	{
		if (!state_ || !state_->payload.bytes)
			return sdk::unexpected(failure("store.memory-cursor-invalid", "state", "missing"));
		if (state_->offset == state_->payload.bytes->size())
			return std::optional<bounded_memory_record>{};
		if (state_->offset > state_->payload.bytes->size())
			return sdk::unexpected(failure("store.memory-corrupt", "cursor", "offset"));
		auto value = decode_record(state_->payload, state_->offset, state_->window_bytes);
		if (!value)
			return sdk::unexpected(std::move(value.error()));
		if (state_->window_bytes > state_->maximum_window_bytes)
			return sdk::unexpected(failure("store.resource-limit", "cursor-window", "bound"));
		return std::optional<bounded_memory_record>{std::move(*value)};
	}

	std::uint64_t bounded_memory_record_cursor::decoded_window_bytes() const noexcept
	{
		return state_ ? state_->window_bytes : 0U;
	}

	std::uint64_t bounded_memory_record_cursor::maximum_window_bytes() const noexcept
	{
		return state_ ? state_->maximum_window_bytes : 0U;
	}

	bounded_memory_backend_snapshot::bounded_memory_backend_snapshot(
		std::shared_ptr<const state> value)
		: state_{std::make_unique<bounded_memory_backend_snapshot::state>(*value)}
	{
	}

	bounded_memory_backend_snapshot::bounded_memory_backend_snapshot(
		bounded_memory_backend_snapshot&&) noexcept = default;
	bounded_memory_backend_snapshot& bounded_memory_backend_snapshot::operator=(
		bounded_memory_backend_snapshot&&) noexcept = default;
	bounded_memory_backend_snapshot::~bounded_memory_backend_snapshot() = default;

	const bounded_memory_publication& bounded_memory_backend_snapshot::publication() const noexcept
	{
		static const bounded_memory_publication empty{};
		return state_ ? state_->publication.publication : empty;
	}

	sdk::result<std::unique_ptr<bounded_memory_record_cursor>>
	bounded_memory_backend_snapshot::open_cursor() const
	{
		if (!state_ || !state_->publication.payload)
			return sdk::unexpected(failure("store.memory-cursor-invalid", "snapshot", "missing"));
		try
		{
			auto cursor_state = std::make_shared<bounded_memory_record_cursor::state>();
			cursor_state->payload = {state_->publication.payload, state_->publication.limits};
			cursor_state->maximum_window_bytes = state_->publication.limits.max_window_bytes;
			return std::unique_ptr<bounded_memory_record_cursor>{
				new bounded_memory_record_cursor{std::move(cursor_state)}};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(failure("store.resource-limit", "cursor", "allocation"));
		}
	}

	sdk::result<void> bounded_memory_backend_snapshot::verify_identity() const
	{
		if (!state_ || !state_->publication.payload)
			return sdk::unexpected(
				failure("store.memory-publication-invalid", "payload", "missing"));
		const auto& publication = state_->publication.publication;
		if (auto valid = validate_publication(publication); !valid)
			return valid;
		auto digest = payload_digest(*state_->publication.payload);
		if (!digest)
			return sdk::unexpected(std::move(digest.error()));
		if (*digest != publication.payload_digest ||
			publication.payload_bytes != state_->publication.payload->size())
			return sdk::unexpected(
				failure("store.memory-corrupt", "publication", "payload-identity"));
		auto cursor = open_cursor();
		if (!cursor)
			return sdk::unexpected(std::move(cursor.error()));
		std::uint64_t record_count{};
		std::uint64_t task_count{};
		for (;;)
		{
			auto next = (*cursor)->next();
			if (!next)
				return sdk::unexpected(std::move(next.error()));
			if (!*next)
				break;
			++record_count;
			if ((**next).kind == bounded_memory_record_kind::task_result)
				++task_count;
		}
		if (record_count != publication.record_count || task_count != publication.task_count)
			return sdk::unexpected(failure("store.memory-corrupt", "publication", "record-census"));
		return {};
	}

	std::span<const std::byte> bounded_memory_backend_snapshot::final_payload() const noexcept
	{
		if (!state_ || !state_->publication.payload)
			return {};
		return *state_->publication.payload;
	}

	bounded_memory_backend_session::bounded_memory_backend_session(std::unique_ptr<state> state)
		: state_{std::move(state)}
	{
	}

	bounded_memory_backend_session::bounded_memory_backend_session(
		bounded_memory_backend_session&&) noexcept = default;
	bounded_memory_backend_session&
	bounded_memory_backend_session::operator=(bounded_memory_backend_session&&) noexcept = default;
	bounded_memory_backend_session::~bounded_memory_backend_session() = default;

	std::string_view bounded_memory_backend_session::candidate_id() const noexcept
	{
		return state_ ? state_->candidate_id : std::string_view{};
	}

	std::string_view bounded_memory_backend_session::expected_head() const noexcept
	{
		return state_ ? state_->expected_head : std::string_view{};
	}

	std::uint64_t bounded_memory_backend_session::task_count() const noexcept
	{
		return state_ ? state_->task_count : 0U;
	}

	std::uint64_t bounded_memory_backend_session::record_count() const noexcept
	{
		return state_ ? state_->record_count : 0U;
	}

	std::uint64_t bounded_memory_backend_session::payload_bytes() const noexcept
	{
		if (!state_)
			return 0U;
		return state_->sealed && state_->frozen_payload ? state_->frozen_payload->size()
														: state_->payload.size();
	}

	bool bounded_memory_backend_session::sealed() const noexcept
	{
		return state_ && state_->sealed;
	}

	bool bounded_memory_backend_session::parity_verified() const noexcept
	{
		return state_ && state_->parity_verified;
	}

	std::optional<bounded_memory_publication_terminal>
	bounded_memory_backend_session::publication_terminal() const noexcept
	{
		return state_ ? state_->terminal : std::nullopt;
	}

	const std::optional<bounded_memory_publication>&
	bounded_memory_backend_session::publication() const noexcept
	{
		static const std::optional<bounded_memory_publication> empty;
		return state_ ? state_->publication : empty;
	}

	sdk::result<void>
	bounded_memory_backend_session::append_task(const std::span<const std::byte> task_payload)
	{
		if (!state_ || state_->failed || state_->sealed || state_->publication_attempted)
			return sdk::unexpected(
				failure("store.memory-session-state", "append-task", "terminal"));
		if (state_->task_count >= state_->limits.max_tasks)
			return sdk::unexpected(failure("store.resource-limit", "task-count", "bound"));
		try
		{
			const auto key = std::to_string(state_->task_count);
			if (task_payload.size() > std::numeric_limits<std::uint64_t>::max())
				return sdk::unexpected(failure("store.resource-limit", "record-bytes", "size"));
			const auto payload_size = static_cast<std::uint64_t>(task_payload.size());
			auto frame = checked_frame_bytes(key.size(), payload_size, state_->limits);
			if (!frame)
				return sdk::unexpected(std::move(frame.error()));
			if (*frame > state_->limits.max_window_bytes)
				return sdk::unexpected(failure("store.resource-limit", "record-window", "bound"));
			std::uint64_t next_bytes{};
			if (!checked_add(
					state_->payload.size(), *frame, state_->limits.max_payload_bytes, next_bytes))
				return sdk::unexpected(failure("store.resource-limit", "payload-bytes", "bound"));
			bounded_memory_record record;
			record.kind = bounded_memory_record_kind::task_result;
			record.key = key;
			record.payload.assign(task_payload.begin(), task_payload.end());
			return append_record(std::move(record));
		}
		catch (const std::bad_alloc&)
		{
			state_->failed = true;
			return sdk::unexpected(failure("store.resource-limit", "final-payload", "allocation"));
		}
	}

	sdk::result<void> bounded_memory_backend_session::append_record(bounded_memory_record record)
	{
		if (!state_ || state_->failed || state_->sealed || state_->publication_attempted)
			return sdk::unexpected(
				failure("store.memory-session-state", "append-record", "terminal"));
		if (record.kind == bounded_memory_record_kind::task_result &&
			state_->task_count >= state_->limits.max_tasks)
			return sdk::unexpected(failure("store.resource-limit", "task-count", "bound"));
		if (auto valid = validate_record(record, state_->limits); !valid)
			return valid;
		const auto frame =
			checked_frame_bytes(record.key.size(), record.payload.size(), state_->limits);
		if (!frame)
			return sdk::unexpected(std::move(frame.error()));
		std::uint64_t next_bytes{};
		if (!checked_add(
				state_->payload.size(), *frame, state_->limits.max_payload_bytes, next_bytes))
			return sdk::unexpected(failure("store.resource-limit", "payload-bytes", "bound"));
		std::array<std::byte, wire_header_bytes> header{};
		header[0U] = static_cast<std::byte>(record.kind);
		put_u64(std::span{header}.subspan(1U, sizeof(std::uint64_t)), record.key.size());
		put_u64(std::span{header}.subspan(1U + sizeof(std::uint64_t), sizeof(std::uint64_t)),
				record.payload.size());
		const auto key_bytes =
			std::as_bytes(std::span<const char>{record.key.data(), record.key.size()});
		const auto digest = sha256_parts(header, key_bytes, record.payload);
		if (!digest)
			return sdk::unexpected(std::move(digest.error()));
		try
		{
			state_->payload.reserve(static_cast<std::size_t>(next_bytes));
			state_->payload.insert(state_->payload.end(), header.begin(), header.end());
			state_->payload.insert(state_->payload.end(), key_bytes.begin(), key_bytes.end());
			state_->payload.insert(
				state_->payload.end(), record.payload.begin(), record.payload.end());
			state_->payload.insert(
				state_->payload.end(),
				std::as_bytes(std::span<const char>{digest->data(), digest->size()}).begin(),
				std::as_bytes(std::span<const char>{digest->data(), digest->size()}).end());
		}
		catch (const std::bad_alloc&)
		{
			state_->failed = true;
			return sdk::unexpected(failure("store.resource-limit", "final-payload", "allocation"));
		}
		if (state_->payload.size() != next_bytes)
		{
			state_->failed = true;
			return sdk::unexpected(failure("store.memory-corrupt", "payload", "append-census"));
		}
		++state_->record_count;
		if (record.kind == bounded_memory_record_kind::task_result)
			++state_->task_count;
		return {};
	}

	sdk::result<void> bounded_memory_backend_session::seal()
	{
		if (!state_ || state_->failed || state_->publication_attempted)
			return sdk::unexpected(failure("store.memory-session-state", "seal", "terminal"));
		if (state_->sealed)
			return {};
		if (state_->task_count == 0U || state_->record_count == 0U)
			return sdk::unexpected(failure("store.memory-session-invalid", "payload", "empty"));
		auto digest = payload_digest(state_->payload);
		if (!digest)
			return sdk::unexpected(std::move(digest.error()));
		auto frozen = freeze_payload(std::move(state_->payload));
		if (!frozen)
		{
			state_->failed = true;
			return sdk::unexpected(std::move(frozen.error()));
		}
		state_->frozen_payload = std::move(*frozen);
		state_->payload.clear();
		state_->payload.shrink_to_fit();
		state_->payload_digest = std::move(*digest);
		state_->sealed = true;
		return {};
	}

	sdk::result<std::unique_ptr<bounded_memory_record_cursor>>
	bounded_memory_backend_session::open_cursor() const
	{
		if (!state_ || !state_->sealed || !state_->frozen_payload)
			return sdk::unexpected(failure("store.memory-session-state", "cursor", "before-seal"));
		try
		{
			auto cursor_state = std::make_shared<bounded_memory_record_cursor::state>();
			cursor_state->payload = {state_->frozen_payload, state_->limits};
			cursor_state->maximum_window_bytes = state_->limits.max_window_bytes;
			return std::unique_ptr<bounded_memory_record_cursor>{
				new bounded_memory_record_cursor{std::move(cursor_state)}};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(failure("store.resource-limit", "cursor", "allocation"));
		}
	}

	sdk::result<bounded_memory_parity_receipt>
	bounded_memory_backend_session::compare_expected(expected_record_reader expected)
	{
		if (!state_ || !state_->sealed || state_->publication_attempted)
			return sdk::unexpected(failure("store.memory-session-state", "parity", "phase"));
		if (!expected)
			return sdk::unexpected(failure("store.memory-parity-invalid", "expected", "missing"));
		if (state_->parity_verified)
			return sdk::unexpected(failure("store.memory-session-state", "parity", "duplicate"));
		auto cursor = open_cursor();
		if (!cursor)
			return sdk::unexpected(std::move(cursor.error()));
		std::uint64_t count{};
		std::uint64_t maximum_window{};
		for (;;)
		{
			auto actual = (*cursor)->next();
			if (!actual)
				return sdk::unexpected(std::move(actual.error()));
			auto expected_value = expected();
			if (!expected_value)
				return sdk::unexpected(std::move(expected_value.error()));
			if (!*actual || !*expected_value)
			{
				if (*actual || *expected_value)
					return sdk::unexpected(
						failure("store.memory-parity-mismatch", "record-count", "different"));
				break;
			}
			if (auto valid = validate_record(**expected_value, state_->limits); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (**actual != **expected_value)
				return sdk::unexpected(
					failure("store.memory-parity-mismatch", "record", "payload-or-key"));
			++count;
			maximum_window = std::max(maximum_window, (*cursor)->decoded_window_bytes());
		}
		if (count != state_->record_count)
			return sdk::unexpected(
				failure("store.memory-parity-mismatch", "record-count", "census"));
		state_->parity_verified = true;
		return bounded_memory_parity_receipt{
			count, state_->frozen_payload->size(), maximum_window, state_->payload_digest};
	}

	sdk::result<bounded_memory_publication_terminal> bounded_memory_backend_session::publish_once()
	{
		if (!state_ || !state_->sealed || !state_->parity_verified)
			return sdk::unexpected(failure("store.memory-session-state", "publish", "not-ready"));
		if (state_->publication_attempted)
			return sdk::unexpected(failure("store.memory-session-state", "publish", "retry"));
		state_->publication_attempted = true;
		state_->terminal = bounded_memory_publication_terminal::not_attempted;
		std::scoped_lock lock{state_->backend->mutex_};
		if (state_->backend->next_sequence_ == 0U)
		{
			state_->terminal = bounded_memory_publication_terminal::rejected_store_failure;
			return *state_->terminal;
		}
		bounded_memory_publication value;
		bounded_memory_publication session_publication;
		bool effect_applied = false;
		try
		{
			value.candidate_id = state_->candidate_id;
			value.parent_publication = state_->expected_head;
			value.payload_digest = state_->payload_digest;
			value.sequence = state_->backend->next_sequence_;
			value.task_count = state_->task_count;
			value.record_count = state_->record_count;
			value.payload_bytes = state_->frozen_payload->size();
			auto identity = publication_id(value);
			if (!identity)
			{
				state_->terminal = bounded_memory_publication_terminal::rejected_store_failure;
				return *state_->terminal;
			}
			value.publication_id = std::move(*identity);
			session_publication = value;
			publication_state stored{value, state_->frozen_payload, state_->limits};
			std::string new_head = value.publication_id;
			bounded_memory_cas_port::effect commit =
				[state = state_->backend,
				 new_head = std::move(new_head),
				 stored = std::move(stored),
				 &effect_applied]() mutable -> sdk::result<void>
			{
				try
				{
					auto [unused, inserted] =
						state->publications_.emplace(new_head, std::move(stored));
					(void)unused;
					if (!inserted)
						return sdk::unexpected(failure(
							"store.memory-publication-invalid", "publication-id", "duplicate"));
					state->head_.swap(new_head);
					++state->next_sequence_;
					effect_applied = true;
					return {};
				}
				catch (const std::bad_alloc&)
				{
					return sdk::unexpected(
						failure("store.resource-limit", "publication", "allocation"));
				}
			};
			auto outcome = state_->backend->cas_port_->compare_exchange_once(
				state_->expected_head, state_->backend->head_, std::move(commit));
			if (!outcome)
			{
				if (effect_applied)
				{
					state_->publication.emplace(std::move(session_publication));
					state_->terminal =
						bounded_memory_publication_terminal::publication_outcome_unknown;
					return *state_->terminal;
				}
				state_->terminal = bounded_memory_publication_terminal::rejected_store_failure;
				return *state_->terminal;
			}
			if (!is_valid(*outcome) ||
				*outcome == bounded_memory_publication_terminal::not_attempted)
			{
				if (effect_applied)
				{
					state_->publication.emplace(std::move(session_publication));
					state_->terminal =
						bounded_memory_publication_terminal::publication_outcome_unknown;
					return *state_->terminal;
				}
				state_->terminal = bounded_memory_publication_terminal::rejected_store_failure;
				return *state_->terminal;
			}
			if (!effect_applied &&
				(*outcome == bounded_memory_publication_terminal::committed_verified ||
				 *outcome == bounded_memory_publication_terminal::publication_outcome_unknown))
			{
				state_->terminal = bounded_memory_publication_terminal::rejected_store_failure;
				return *state_->terminal;
			}
			if (effect_applied &&
				(*outcome == bounded_memory_publication_terminal::rejected_stale ||
				 *outcome == bounded_memory_publication_terminal::rejected_store_failure))
			{
				state_->publication.emplace(std::move(session_publication));
				state_->terminal = bounded_memory_publication_terminal::publication_outcome_unknown;
				return *state_->terminal;
			}
			if (effect_applied)
				state_->publication.emplace(std::move(session_publication));
			state_->terminal = *outcome;
			return *state_->terminal;
		}
		catch (const std::bad_alloc&)
		{
			if (effect_applied)
			{
				state_->publication.emplace(std::move(session_publication));
				state_->terminal = bounded_memory_publication_terminal::publication_outcome_unknown;
				return *state_->terminal;
			}
			state_->terminal = bounded_memory_publication_terminal::rejected_store_failure;
			return *state_->terminal;
		}
	}

	bounded_memory_backend::bounded_memory_backend() : bounded_memory_backend(options{}) {}

	bounded_memory_backend::bounded_memory_backend(options value)
		: bounded_memory_backend(std::move(value), nullptr)
	{
	}

	bounded_memory_backend::bounded_memory_backend(
		options value, std::shared_ptr<bounded_memory_cas_port> cas_port)
		: state_{std::make_shared<state>(std::move(value), std::move(cas_port))}
	{
	}

	bounded_memory_backend::bounded_memory_backend(bounded_memory_backend&&) noexcept = default;
	bounded_memory_backend&
	bounded_memory_backend::operator=(bounded_memory_backend&&) noexcept = default;
	bounded_memory_backend::~bounded_memory_backend() = default;

	sdk::result<bounded_memory_backend_session>
	bounded_memory_backend::begin(std::string candidate_id, std::string expected_head)
	{
		if (!state_)
			return sdk::unexpected(failure("store.memory-backend-state", "backend", "missing"));
		if (auto valid = state_->options_.limits.validate(); !valid)
			return sdk::unexpected(std::move(valid.error()));
		if (auto valid = validate_candidate_and_head(candidate_id, expected_head); !valid)
			return sdk::unexpected(std::move(valid.error()));
		try
		{
			auto session = std::make_unique<bounded_memory_backend_session::state>();
			session->backend = state_;
			session->limits = state_->options_.limits;
			session->candidate_id = std::move(candidate_id);
			session->expected_head = std::move(expected_head);
			return bounded_memory_backend_session{std::move(session)};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(failure("store.resource-limit", "session", "allocation"));
		}
	}

	sdk::result<bounded_memory_backend_snapshot>
	bounded_memory_backend::reopen(const std::string_view publication_id_value) const
	{
		if (!state_ || publication_id_value.empty())
			return sdk::unexpected(
				failure("store.memory-reopen-invalid", "publication-id", "missing"));
		std::scoped_lock lock{state_->mutex_};
		const auto found = state_->publications_.find(publication_id_value);
		if (found == state_->publications_.end())
			return sdk::unexpected(
				failure("store.publication-not-found", "publication-id", "missing"));
		try
		{
			auto snapshot_state =
				std::make_shared<bounded_memory_backend_snapshot::state>(found->second);
			bounded_memory_backend_snapshot snapshot{std::move(snapshot_state)};
			if (auto valid = snapshot.verify_identity(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			return snapshot;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(failure("store.resource-limit", "reopen", "allocation"));
		}
	}

	sdk::result<std::string> bounded_memory_backend::current_head() const
	{
		if (!state_)
			return sdk::unexpected(failure("store.memory-backend-state", "backend", "missing"));
		std::scoped_lock lock{state_->mutex_};
		return state_->head_;
	}

	std::uint64_t bounded_memory_backend::committed_publication_count() const noexcept
	{
		if (!state_)
			return 0U;
		std::scoped_lock lock{state_->mutex_};
		return state_->publications_.size();
	}

	sdk::result<std::string>
	derive_bounded_memory_publication_id(const std::string_view candidate_id,
										 const std::string_view parent_publication,
										 const std::string_view payload_digest_value,
										 const std::uint64_t sequence)
	{
		if (auto valid = sdk::validate_strong_id(candidate_id); !valid)
			return sdk::unexpected(std::move(valid.error()));
		if (parent_publication != "genesis")
			if (auto valid = sdk::validate_strong_id(parent_publication); !valid)
				return sdk::unexpected(std::move(valid.error()));
		if (!valid_digest(payload_digest_value) || sequence == 0U)
			return sdk::unexpected(
				failure("store.memory-publication-invalid", "identity", "input"));
		const std::vector<sdk::canonical_value> fields{
			sdk::canonical_value::from_string(std::string{candidate_id}),
			sdk::canonical_value::from_string(std::string{parent_publication}),
			sdk::canonical_value::from_string(std::string{payload_digest_value}),
			sdk::canonical_value::from_integer(static_cast<std::int64_t>(sequence)),
		};
		return sdk::canonical_identity_digest("cxxlens.clang22.bounded-memory-publication.v1",
											  fields);
	}
} // namespace cxxlens::detail::clang22::materialization
