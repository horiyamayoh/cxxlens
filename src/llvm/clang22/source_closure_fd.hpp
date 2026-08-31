#pragma once

/**
 * @file source_closure_fd.hpp
 * @brief Explicit bounded POSIX descriptor port for source-closure frames.
 *
 * This port deliberately does not assign a descriptor number or alter the
 * process adapter.  A caller supplies the already-inherited endpoints and
 * remains responsible for their inheritance setup.  The verified executable
 * descriptor (3) is reserved by the process adapter; closure endpoints must
 * therefore be explicit descriptors at or above 4.
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>

#include <cxxlens/sdk/provider.hpp>

#include "source_closure_receiver.hpp"

namespace cxxlens::detail::clang22
{
	/** The first descriptor which may be assigned to an inherited closure endpoint. */
	inline constexpr int source_closure_first_inherited_descriptor = 4;

	/** Whether factory invocation borrows or consumes one supplied endpoint. */
	enum class source_closure_fd_ownership : std::uint8_t
	{
		borrowed,
		owned,
	};

	/** One explicit inherited descriptor and its lifetime contract. */
	struct source_closure_fd_descriptor
	{
		int value{-1};
		source_closure_fd_ownership ownership{source_closure_fd_ownership::borrowed};
	};

	/**
	 * Explicit physical port for one source-closure frame channel.
	 *
	 * `read` and `write` are caller-supplied descriptors for two independent
	 * connected sockets already authenticated by the process binding.  Reusing one full-duplex
	 * descriptor or admitting a pipe would collapse input and ACK custody into one endpoint, so
	 * both forms are rejected before I/O.  The process adapter owns descriptor inheritance: it
	 * creates the endpoints with close-on-exec, clears FD_CLOEXEC on exactly the descriptors passed
	 * to the child, and does not use descriptor 3.  This adapter requires the resulting endpoints
	 * to be non-blocking and close-on-exec clear, but never changes either flag.  The default
	 * lifetime is borrowed so a process adapter can remain the sole owner.  The factory atomically
	 * pins each admitted endpoint into a private close-on-exec duplicate, so closing or reusing a
	 * borrowed descriptor number cannot redirect later frame I/O.  Passing `owned` transfers the
	 * supplied descriptor itself to the factory; it is closed on both success and failure.  The
	 * private duplicates are always closed exactly once by this channel.
	 *
	 * Every read is bounded by the supplied destination span and every write is
	 * drained in bounded non-blocking chunks using MSG_NOSIGNAL (or the platform socket option)
	 * without changing process-wide or thread-local signal state.  A stop request produces a typed
	 * cancellation error.  Read EOF is returned as zero bytes so the receiver
	 * can distinguish clean channel close from a truncated frame; write HUP,
	 * EPIPE, or connection reset is a typed channel-closed failure.
	 */
	struct source_closure_fd_channel_options
	{
		source_closure_fd_descriptor read;
		source_closure_fd_descriptor write;
		std::stop_token cancellation;
		/** Optional caller-owned clock; null selects the channel-owned system clock. */
		const source_closure_monotonic_clock* clock{};
		/** Absolute bound for one no-progress interval; completed byte progress starts the next. */
		std::uint64_t progress_timeout_ns{source_closure_default_progress_timeout_ns};
	};

	/**
	 * POSIX implementation of the source-closure frame source and sink ports.
	 *
	 * This class is source-private.  Its factory rejects
	 * standard streams, descriptor 3, close-on-exec endpoints, blocking
	 * endpoints, and invalid descriptors before any I/O occurs.
	 */
	class source_closure_fd_channel final : public source_closure_frame_source,
											public source_closure_frame_sink
	{
	  public:
		[[nodiscard]] static sdk::result<source_closure_fd_channel>
		create(const source_closure_fd_channel_options& options);

		source_closure_fd_channel(const source_closure_fd_channel&) = delete;
		source_closure_fd_channel& operator=(const source_closure_fd_channel&) = delete;
		source_closure_fd_channel(source_closure_fd_channel&& other) noexcept;
		source_closure_fd_channel& operator=(source_closure_fd_channel&& other) noexcept;
		~source_closure_fd_channel() override;

		[[nodiscard]] sdk::result<std::size_t> read(std::span<std::byte> destination) override;
		[[nodiscard]] sdk::result<void> write(std::span<const std::byte> frame_bytes) override;

	  private:
		source_closure_fd_channel(const source_closure_fd_channel_options& options,
								  int pinned_read,
								  int pinned_write,
								  std::uint64_t read_device,
								  std::uint64_t read_inode,
								  std::uint32_t read_mode,
								  std::uint64_t write_device,
								  std::uint64_t write_inode,
								  std::uint32_t write_mode) noexcept;
		void close_pinned() noexcept;
		[[nodiscard]] const source_closure_monotonic_clock& clock() const noexcept;

		int read_descriptor_{-1};
		int write_descriptor_{-1};
		std::uint64_t read_device_{};
		std::uint64_t read_inode_{};
		std::uint32_t read_mode_{};
		std::uint64_t write_device_{};
		std::uint64_t write_inode_{};
		std::uint32_t write_mode_{};
		bool poisoned_{false};
		std::stop_token cancellation_;
		source_closure_system_monotonic_clock system_clock_;
		const source_closure_monotonic_clock* injected_clock_{};
		std::uint64_t progress_timeout_ns_{source_closure_default_progress_timeout_ns};
	};
} // namespace cxxlens::detail::clang22
