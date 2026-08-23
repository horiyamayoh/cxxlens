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
#include <span>
#include <stop_token>

#include <cxxlens/sdk/provider.hpp>

#include "source_closure_receiver.hpp"

namespace cxxlens::detail::clang22
{
	/** The first descriptor which may be assigned to an inherited closure endpoint. */
	inline constexpr int source_closure_first_inherited_descriptor = 4;

	/** Whether this adapter closes one supplied endpoint during destruction. */
	enum class source_closure_fd_ownership
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
	 * `read` and `write` are caller-supplied descriptors; they may be the same
	 * full-duplex descriptor or two independent pipe endpoints.  The process
	 * adapter owns descriptor inheritance: it creates the endpoints with
	 * close-on-exec, clears FD_CLOEXEC on exactly the descriptors passed to the
	 * child, and does not use descriptor 3.  This adapter requires the resulting
	 * endpoints to be non-blocking and close-on-exec clear, but never changes
	 * either flag.  The default lifetime is borrowed so a process adapter can
	 * remain the sole owner; owned endpoints are closed exactly once here.
	 *
	 * Every read is bounded by the supplied destination span and every write is
	 * drained in bounded non-blocking chunks.  A stop request produces a typed
	 * cancellation error.  Read EOF is returned as zero bytes so the receiver
	 * can distinguish clean channel close from a truncated frame; write HUP,
	 * EPIPE, or connection reset is a typed channel-closed failure.
	 */
	struct source_closure_fd_channel_options
	{
		source_closure_fd_descriptor read;
		source_closure_fd_descriptor write;
		std::stop_token cancellation;
	};

	/**
	 * POSIX implementation of the source-closure frame source and sink ports.
	 *
	 * This class is intentionally source-private and is not connected to the
	 * installed worker or materializer in this slice.  Its factory rejects
	 * standard streams, descriptor 3, close-on-exec endpoints, blocking
	 * endpoints, and invalid descriptors before any I/O occurs.
	 */
	class source_closure_fd_channel final : public source_closure_frame_source,
											public source_closure_frame_sink
	{
	  public:
		[[nodiscard]] static sdk::result<source_closure_fd_channel>
		create(source_closure_fd_channel_options options);

		source_closure_fd_channel(const source_closure_fd_channel&) = delete;
		source_closure_fd_channel& operator=(const source_closure_fd_channel&) = delete;
		source_closure_fd_channel(source_closure_fd_channel&& other) noexcept;
		source_closure_fd_channel& operator=(source_closure_fd_channel&& other) noexcept;
		~source_closure_fd_channel() override;

		[[nodiscard]] sdk::result<std::size_t> read(std::span<std::byte> destination) override;
		[[nodiscard]] sdk::result<void> write(std::span<const std::byte> frame_bytes) override;

	  private:
		source_closure_fd_channel(source_closure_fd_channel_options options) noexcept;
		void close_owned() noexcept;

		int read_descriptor_{-1};
		int write_descriptor_{-1};
		bool owns_read_{false};
		bool owns_write_{false};
		std::stop_token cancellation_;
	};
} // namespace cxxlens::detail::clang22
