#include "materialization_store_projection.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
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
									   const std::uint64_t limit,
									   std::uint64_t& output) noexcept
		{
			if (left > limit || right > limit - left)
				return false;
			output = left + right;
			return true;
		}

		[[nodiscard]] bool
		valid_kind(const materialization_store_projection_record_kind kind) noexcept
		{
			const auto value = static_cast<std::uint8_t>(kind);
			return value >= 1U && value <= 14U;
		}

		[[nodiscard]] bool exact_content_digest(const std::string_view value) noexcept
		{
			if (value.size() != 71U || !value.starts_with("sha256:"))
				return false;
			return std::ranges::all_of(value.substr(7U),
									   [](const char byte)
									   {
										   return (byte >= '0' && byte <= '9') ||
											   (byte >= 'a' && byte <= 'f');
									   });
		}

		void put_u64(std::vector<std::byte>& output, const std::uint64_t value)
		{
			for (int shift = 56; shift >= 0; shift -= 8)
				output.push_back(
					static_cast<std::byte>((value >> static_cast<unsigned>(shift)) & 0xffU));
		}

		[[nodiscard]] std::uint64_t get_u64(const std::span<const std::byte> input) noexcept
		{
			std::uint64_t output{};
			for (const auto byte : input)
				output = (output << 8U) | std::to_integer<std::uint8_t>(byte);
			return output;
		}

		[[nodiscard]] std::string order_key(const materialization_store_projection_record& record)
		{
			std::string output;
			output.reserve(1U + record.key.size());
			output.push_back(static_cast<char>(record.kind));
			output.append(record.key);
			return output;
		}

		[[nodiscard]] sdk::result<void> read_exact(materialization_replayable_spool& source,
												   const std::uint64_t offset,
												   const std::span<std::byte> destination)
		{
			std::size_t copied{};
			while (copied < destination.size())
			{
				auto read = source.read_at(offset + copied, destination.subspan(copied));
				if (!read)
					return sdk::unexpected(failure("store.spool-failure", "projection", "read"));
				if (*read == 0U || *read > destination.size() - copied)
					return sdk::unexpected(failure("store.corrupt", "projection", "truncated"));
				copied += *read;
			}
			return {};
		}

		class projection_stream_state;

		class projection_cursor final : public materialization_store_projection_cursor
		{
		  public:
			explicit projection_cursor(const projection_stream_state& stream) noexcept
				: stream_{&stream}
			{
			}

			[[nodiscard]] sdk::result<std::optional<materialization_store_projection_record>>
			next() override;

		  private:
			const projection_stream_state* stream_{};
			std::uint64_t offset_{};
		};

		class projection_stream_state
		{
		  public:
			projection_stream_state(std::unique_ptr<materialization_replayable_spool> storage,
									materialization_store_projection_limits limits) noexcept
				: storage_{std::move(storage)}, limits_{limits}
			{
			}

			std::unique_ptr<materialization_replayable_spool> storage_;
			materialization_store_projection_limits limits_;
			std::uint64_t record_count_{};
			std::uint64_t byte_count_{};
			bool sealed_{};
			bool poisoned_{};
			std::string content_digest_;
			std::string semantic_digest_;

			friend class projection_cursor;
			friend class materialization_store_projection_stream;
		};

		sdk::result<std::optional<materialization_store_projection_record>>
		projection_cursor::next()
		{
			if (stream_ == nullptr || stream_->poisoned_ || !stream_->storage_)
				return sdk::unexpected(
					failure("store.projection-state", "cursor", "storage-missing"));
			if (!stream_->sealed_ || !stream_->storage_->sealed())
				return sdk::unexpected(failure("store.projection-state", "cursor", "before-seal"));
			if (offset_ == stream_->byte_count_)
				return std::optional<materialization_store_projection_record>{};
			if (offset_ > stream_->byte_count_ ||
				stream_->byte_count_ - offset_ < record_header_bytes + record_digest_bytes)
				return sdk::unexpected(failure("store.corrupt", "projection", "truncated-header"));

			std::array<std::byte, record_header_bytes> header{};
			if (auto read = read_exact(*stream_->storage_, offset_, header); !read)
				return sdk::unexpected(std::move(read.error()));
			const auto key_size = get_u64(std::span{header}.subspan(1U, sizeof(std::uint64_t)));
			const auto payload_size = get_u64(
				std::span{header}.subspan(1U + sizeof(std::uint64_t), sizeof(std::uint64_t)));
			std::uint64_t body_size{};
			std::uint64_t frame_size{};
			if (!checked_add(
					record_header_bytes, key_size, stream_->limits_.max_record_bytes, body_size) ||
				!checked_add(
					body_size, payload_size, stream_->limits_.max_record_bytes, body_size) ||
				!checked_add(body_size,
							 record_digest_bytes,
							 stream_->limits_.max_record_bytes,
							 frame_size) ||
				frame_size > stream_->byte_count_ - offset_ ||
				frame_size > std::numeric_limits<std::size_t>::max())
				return sdk::unexpected(failure("store.corrupt", "projection", "record-length"));

			std::vector<std::byte> frame;
			try
			{
				frame.resize(static_cast<std::size_t>(frame_size));
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(
					failure("store.resource-limit", "projection", "record-allocation"));
			}
			if (auto read = read_exact(*stream_->storage_, offset_, frame); !read)
				return sdk::unexpected(std::move(read.error()));
			auto decoded = decode_materialization_store_projection_record(frame, stream_->limits_);
			if (!decoded)
				return sdk::unexpected(std::move(decoded.error()));
			offset_ += frame_size;
			return std::optional<materialization_store_projection_record>{std::move(*decoded)};
		}
	} // namespace

	sdk::result<std::vector<std::byte>> encode_materialization_store_projection_record(
		const materialization_store_projection_record& record,
		const materialization_store_projection_limits& limits)
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
		try
		{
			body.reserve(static_cast<std::size_t>(body_size));
			body.push_back(static_cast<std::byte>(record.kind));
			put_u64(body, record.key.size());
			put_u64(body, record.payload.size());
			const auto key_bytes =
				std::as_bytes(std::span<const char>{record.key.data(), record.key.size()});
			body.insert(body.end(), key_bytes.begin(), key_bytes.end());
			body.insert(body.end(), record.payload.begin(), record.payload.end());
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(failure("store.resource-limit", "record-bytes", "allocation"));
		}
		const auto digest = sdk::content_digest(body);
		if (!exact_content_digest(digest))
			return sdk::unexpected(failure("store.hash-failure", "record", "content-digest"));

		std::vector<std::byte> frame;
		try
		{
			frame.reserve(static_cast<std::size_t>(frame_size));
			frame.insert(frame.end(), body.begin(), body.end());
			const auto digest_bytes =
				std::as_bytes(std::span<const char>{digest.data(), digest.size()});
			frame.insert(frame.end(), digest_bytes.begin(), digest_bytes.end());
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(failure("store.resource-limit", "record-bytes", "allocation"));
		}
		return frame;
	}

	sdk::result<materialization_store_projection_record>
	decode_materialization_store_projection_record(
		const std::span<const std::byte> bytes,
		const materialization_store_projection_limits& limits)
	{
		if (bytes.size() < record_header_bytes + record_digest_bytes ||
			bytes.size() > limits.max_record_bytes)
			return sdk::unexpected(failure("store.corrupt", "record", "truncated-or-length"));
		const auto kind = static_cast<materialization_store_projection_record_kind>(
			std::to_integer<std::uint8_t>(bytes.front()));
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
		if (!exact_content_digest(observed_digest))
			return sdk::unexpected(failure("store.corrupt", "record", "digest-format"));
		const auto expected_digest = sdk::content_digest(bytes.first(digest_offset));
		if (observed_digest != expected_digest)
			return sdk::unexpected(failure("store.corrupt", "record", "checksum"));

		materialization_store_projection_record output;
		try
		{
			output.kind = kind;
			output.key.assign(reinterpret_cast<const char*>(bytes.data() + key_offset),
							  static_cast<std::size_t>(key_size));
			output.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset),
								  bytes.begin() + static_cast<std::ptrdiff_t>(digest_offset));
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(failure("store.resource-limit", "record", "decode-allocation"));
		}
		return output;
	}

	struct materialization_store_projection_stream::state : projection_stream_state
	{
		state(std::unique_ptr<materialization_replayable_spool> storage,
			  const materialization_store_projection_limits limits) noexcept
			: projection_stream_state{std::move(storage), limits}
		{
		}
	};

	materialization_store_projection_stream::materialization_store_projection_stream(
		std::unique_ptr<state> state_value)
		: state_{std::move(state_value)}
	{
	}

	materialization_store_projection_stream::materialization_store_projection_stream(
		materialization_store_projection_stream&&) noexcept = default;
	materialization_store_projection_stream& materialization_store_projection_stream::operator=(
		materialization_store_projection_stream&&) noexcept = default;
	materialization_store_projection_stream::~materialization_store_projection_stream() = default;

	sdk::result<materialization_store_projection_stream>
	materialization_store_projection_stream::create(
		const materialization_store_projection_limits limits)
	{
		if (limits.max_record_bytes < record_header_bytes + record_digest_bytes ||
			limits.max_spool_bytes == 0U || limits.max_records == 0U)
			return sdk::unexpected(failure("store.resource-limit", "limits", "invalid"));
		auto storage = make_materialization_private_spool();
		if (!storage)
			return sdk::unexpected(failure("store.spool-failure", "projection", "create"));
		try
		{
			return materialization_store_projection_stream{
				std::make_unique<state>(std::move(*storage), limits)};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(failure("store.resource-limit", "projection", "allocation"));
		}
	}

	sdk::result<void> materialization_store_projection_stream::append(
		const materialization_store_projection_record& record)
	{
		if (!state_ || state_->poisoned_ || !state_->storage_ || state_->sealed_)
			return sdk::unexpected(
				failure("store.projection-state", "append", "sealed-or-invalid"));
		if (state_->record_count_ >= state_->limits_.max_records)
			return sdk::unexpected(failure("store.resource-limit", "records", "maximum"));
		auto encoded = encode_materialization_store_projection_record(record, state_->limits_);
		if (!encoded)
			return sdk::unexpected(std::move(encoded.error()));
		std::uint64_t next_bytes{};
		if (!checked_add(
				state_->byte_count_, encoded->size(), state_->limits_.max_spool_bytes, next_bytes))
			return sdk::unexpected(failure("store.resource-limit", "spool-bytes", "maximum"));
		const auto appended = state_->storage_->append(*encoded);
		if (!appended)
		{
			state_->poisoned_ = true;
			return sdk::unexpected(failure("store.spool-failure", "projection", "append"));
		}
		state_->byte_count_ = next_bytes;
		++state_->record_count_;
		return {};
	}

	sdk::result<void> materialization_store_projection_stream::seal()
	{
		if (!state_ || state_->poisoned_ || !state_->storage_)
			return sdk::unexpected(failure("store.projection-state", "seal", "storage-missing"));
		if (state_->sealed_)
			return {};
		if (auto sealed = state_->storage_->seal(); !sealed)
		{
			state_->poisoned_ = true;
			return sdk::unexpected(failure("store.spool-failure", "projection", "seal"));
		}
		auto content = digest_materialization_spool(*state_->storage_);
		if (!content)
		{
			state_->poisoned_ = true;
			return sdk::unexpected(failure("store.hash-failure", "projection", "content-digest"));
		}
		auto semantic = sdk::semantic_digest("cxxlens.clang22-store-projection.v1", *content);
		if (!semantic)
		{
			state_->poisoned_ = true;
			return sdk::unexpected(std::move(semantic.error()));
		}
		state_->content_digest_ = std::move(*content);
		state_->semantic_digest_ = std::move(*semantic);
		state_->sealed_ = true;
		return {};
	}

	sdk::result<std::unique_ptr<materialization_store_projection_cursor>>
	materialization_store_projection_stream::open_cursor() const
	{
		if (!state_ || state_->poisoned_ || !state_->storage_ || !state_->sealed_ ||
			!state_->storage_->sealed())
			return sdk::unexpected(failure("store.projection-state", "cursor", "before-seal"));
		try
		{
			return std::unique_ptr<materialization_store_projection_cursor>{
				new projection_cursor{*state_}};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(failure("store.resource-limit", "cursor", "allocation"));
		}
	}

	sdk::result<void> materialization_store_projection_stream::validate_canonical_order() const
	{
		auto cursor = open_cursor();
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
			auto current = order_key(**next);
			if (prior && current <= *prior)
				return sdk::unexpected(
					failure("store.corrupt", "projection-order", "noncanonical"));
			prior = std::move(current);
		}
	}

	std::uint64_t materialization_store_projection_stream::record_count() const noexcept
	{
		return state_ ? state_->record_count_ : 0U;
	}
	std::uint64_t materialization_store_projection_stream::byte_count() const noexcept
	{
		return state_ ? state_->byte_count_ : 0U;
	}
	bool materialization_store_projection_stream::sealed() const noexcept
	{
		return state_ && state_->sealed_ && !state_->poisoned_ && state_->storage_ &&
			state_->storage_->sealed();
	}
	const materialization_store_projection_limits&
	materialization_store_projection_stream::limits() const noexcept
	{
		static const materialization_store_projection_limits invalid{};
		return state_ ? state_->limits_ : invalid;
	}

	sdk::result<std::string> materialization_store_projection_stream::content_digest() const
	{
		if (!sealed())
			return sdk::unexpected(failure("store.projection-state", "digest", "before-seal"));
		return state_->content_digest_;
	}

	sdk::result<std::string> materialization_store_projection_stream::semantic_digest() const
	{
		if (!sealed())
			return sdk::unexpected(failure("store.projection-state", "digest", "before-seal"));
		return state_->semantic_digest_;
	}

	sdk::result<materialization_store_projection_comparison>
	compare_materialization_store_projections(
		const materialization_store_projection_stream& expected,
		const materialization_store_projection_stream& actual)
	{
		if (!expected.sealed() || !actual.sealed())
			return sdk::unexpected(failure("store.projection-state", "compare", "before-seal"));
		if (auto order = expected.validate_canonical_order(); !order)
			return sdk::unexpected(std::move(order.error()));
		if (auto order = actual.validate_canonical_order(); !order)
			return sdk::unexpected(std::move(order.error()));
		auto expected_cursor = expected.open_cursor();
		auto actual_cursor = actual.open_cursor();
		if (!expected_cursor || !actual_cursor)
			return sdk::unexpected(failure("store.projection-state", "compare", "cursor"));

		materialization_store_projection_comparison output;
		for (std::uint64_t index{};; ++index)
		{
			auto expected_record = (*expected_cursor)->next();
			auto actual_record = (*actual_cursor)->next();
			if (!expected_record || !actual_record)
				return sdk::unexpected(failure("store.corrupt", "projection", "cursor"));
			if (!*expected_record && !*actual_record)
				return output;
			if (!*expected_record)
			{
				output.kind = materialization_store_projection_mismatch_kind::actual_extra;
				output.record_index = index;
				output.actual = std::move(**actual_record);
				return output;
			}
			if (!*actual_record)
			{
				output.kind = materialization_store_projection_mismatch_kind::expected_missing;
				output.record_index = index;
				output.expected = std::move(**expected_record);
				return output;
			}
			if (**expected_record != **actual_record)
			{
				const auto expected_order = order_key(**expected_record);
				const auto actual_order = order_key(**actual_record);
				if (expected_order < actual_order)
				{
					output.kind = materialization_store_projection_mismatch_kind::expected_missing;
					output.record_index = index;
					output.expected = std::move(**expected_record);
					return output;
				}
				if (expected_order > actual_order)
				{
					output.kind = materialization_store_projection_mismatch_kind::actual_extra;
					output.record_index = index;
					output.actual = std::move(**actual_record);
					return output;
				}
				output.kind = materialization_store_projection_mismatch_kind::full_byte_mismatch;
				output.record_index = index;
				output.expected = std::move(**expected_record);
				output.actual = std::move(**actual_record);
				return output;
			}
		}
	}
} // namespace cxxlens::detail::clang22::materialization
