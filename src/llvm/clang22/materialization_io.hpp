#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <variant>

namespace cxxlens::detail::clang22::materialization
{
	inline constexpr std::uint64_t maximum_raw_request_bytes = 1U << 30U;
	inline constexpr std::size_t default_stream_chunk_bytes = 64U * 1024U;
	inline constexpr std::size_t maximum_stream_chunk_bytes = 1024U * 1024U;

	/** Closed failure classes retained entirely inside the private materializer boundary. */
	enum class materialization_io_failure_kind : std::uint8_t
	{
		invalid_configuration,
		allocation,
		read,
		write,
		spool,
		hash,
	};

	/** Exact operation at which a private streaming failure occurred. */
	enum class materialization_io_operation : std::uint8_t
	{
		configuration,
		buffer_allocation,
		input_read,
		spool_write,
		spool_seal,
		digest_update,
		digest_finalize,
	};

	struct materialization_io_failure
	{
		materialization_io_failure_kind kind{materialization_io_failure_kind::read};
		materialization_io_operation operation{materialization_io_operation::input_read};

		[[nodiscard]] bool operator==(const materialization_io_failure&) const = default;
	};

	template <class Value>
	class materialization_io_result
	{
	  public:
		materialization_io_result(Value value) : storage_{std::move(value)} {}
		materialization_io_result(materialization_io_failure failure) : storage_{failure} {}

		[[nodiscard]] bool has_value() const noexcept
		{
			return std::holds_alternative<Value>(storage_);
		}

		[[nodiscard]] explicit operator bool() const noexcept
		{
			return has_value();
		}

		[[nodiscard]] Value& value()
		{
			return std::get<Value>(storage_);
		}

		[[nodiscard]] const Value& value() const
		{
			return std::get<Value>(storage_);
		}

		[[nodiscard]] Value& operator*()
		{
			return value();
		}

		[[nodiscard]] const Value& operator*() const
		{
			return value();
		}

		[[nodiscard]] Value* operator->()
		{
			return &value();
		}

		[[nodiscard]] const Value* operator->() const
		{
			return &value();
		}

		[[nodiscard]] materialization_io_failure& error()
		{
			return std::get<materialization_io_failure>(storage_);
		}

		[[nodiscard]] const materialization_io_failure& error() const
		{
			return std::get<materialization_io_failure>(storage_);
		}

	  private:
		std::variant<Value, materialization_io_failure> storage_;
	};

	template <>
	class materialization_io_result<void>
	{
	  public:
		materialization_io_result() = default;
		materialization_io_result(materialization_io_failure failure) : storage_{failure} {}

		[[nodiscard]] bool has_value() const noexcept
		{
			return std::holds_alternative<std::monostate>(storage_);
		}

		[[nodiscard]] explicit operator bool() const noexcept
		{
			return has_value();
		}

		[[nodiscard]] materialization_io_failure& error()
		{
			return std::get<materialization_io_failure>(storage_);
		}

		[[nodiscard]] const materialization_io_failure& error() const
		{
			return std::get<materialization_io_failure>(storage_);
		}

	  private:
		std::variant<std::monostate, materialization_io_failure> storage_;
	};

	/**
	 * Private byte-source port. A successful zero count is permanent EOF; positive short reads are
	 * normal. Implementations must never return a count larger than `destination.size()`.
	 */
	class materialization_byte_reader
	{
	  public:
		virtual ~materialization_byte_reader() = default;
		[[nodiscard]] virtual materialization_io_result<std::size_t>
		read(std::span<std::byte> destination) = 0;
	};

	/**
	 * Private spool port. `append` is all-bytes-or-error and `seal` makes the observed prefix
	 * immutable for a later streaming parser. Filesystem permissions and cleanup stay behind this
	 * port; this interface never exposes a path.
	 */
	class materialization_private_spool
	{
	  public:
		virtual ~materialization_private_spool() = default;
		[[nodiscard]] virtual materialization_io_result<void>
		append(std::span<const std::byte> bytes) = 0;
		[[nodiscard]] virtual materialization_io_result<void> seal() = 0;
	};

	/** Private incremental digest port; hash implementation and failure injection stay outside I/O.
	 */
	class materialization_digest_accumulator
	{
	  public:
		virtual ~materialization_digest_accumulator() = default;
		[[nodiscard]] virtual materialization_io_result<void>
		update(std::span<const std::byte> bytes) = 0;
		[[nodiscard]] virtual materialization_io_result<std::string> finish() = 0;
	};

	/** Exact bounded raw-input observation; no suffix size or digest is representable. */
	struct raw_input_observation
	{
		std::uint64_t byte_limit{};
		std::uint64_t observed_size_bytes{};
		std::string observed_prefix_digest;
		bool complete{};

		[[nodiscard]] bool operator==(const raw_input_observation&) const = default;
	};

	struct bounded_input_options
	{
		std::uint64_t byte_limit{maximum_raw_request_bytes};
		std::size_t chunk_bytes{default_stream_chunk_bytes};
	};

	/**
	 * Stream at most `byte_limit + 1` bytes into a private spool while hashing incrementally.
	 *
	 * At EOF on or before the limit, the returned observation is complete. Reaching limit+1 stops
	 * immediately without reading or making any claim about the unread suffix and returns
	 * `complete == false`.
	 */
	[[nodiscard]] materialization_io_result<raw_input_observation>
	capture_bounded_input(materialization_byte_reader& input,
						  materialization_private_spool& spool,
						  materialization_digest_accumulator& digest,
						  bounded_input_options options = {});

	/** Production SHA-256 adapter overload; the state machine still consumes only the digest port.
	 */
	[[nodiscard]] materialization_io_result<raw_input_observation>
	capture_bounded_input(materialization_byte_reader& input,
						  materialization_private_spool& spool,
						  bounded_input_options options = {});
} // namespace cxxlens::detail::clang22::materialization
