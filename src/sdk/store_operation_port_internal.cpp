#include "store_operation_port_internal.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <limits>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace cxxlens::sdk
{
	namespace
	{
		constexpr std::size_t maximum_interrupted_write_retries{64U};

		[[nodiscard]] constexpr bool resource_exhausted(const int code) noexcept
		{
			if (code == ENOSPC)
				return true;
#if defined(EDQUOT)
			if (code == EDQUOT)
				return true;
#endif
			return false;
		}

		[[nodiscard]] constexpr bool valid_sync_target(const store_sync_target target) noexcept
		{
			switch (target)
			{
				case store_sync_target::file_data:
				case store_sync_target::file:
				case store_sync_target::parent_directory:
					return true;
			}
			return false;
		}

		[[nodiscard]] constexpr bool
		valid_sqlite_binding(const store_sqlite_operation_binding& binding) noexcept
		{
			return binding.context != nullptr && binding.invoke != nullptr &&
				binding.success_code != binding.resource_exhausted_code;
		}
	} // namespace

	store_write_outcome
	default_store_operation_port::write_exact(const int descriptor,
											  const std::span<const std::byte> bytes) noexcept
	{
		if (descriptor < 0)
			return {store_write_state::failed, 0U, EBADF, false, false};
		if (bytes.empty())
			return {store_write_state::complete, 0U, 0, false, false};

#if defined(__unix__) || defined(__APPLE__)
		std::size_t transferred{};
		std::size_t interrupted_retries{};
		while (transferred < bytes.size())
		{
			const auto remaining = bytes.size() - transferred;
			const auto bounded_count = std::min(
				remaining, static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()));
			errno = 0;
			const auto count = ::write(descriptor, bytes.data() + transferred, bounded_count);
			if (count > 0)
			{
				transferred += static_cast<std::size_t>(count);
				interrupted_retries = 0U;
				continue;
			}
			if (count == 0)
			{
				return {
					store_write_state::outcome_unknown, transferred, 0, true, transferred != 0U};
			}

			const auto code = errno;
			if (code == EINTR && interrupted_retries < maximum_interrupted_write_retries)
			{
				++interrupted_retries;
				continue;
			}
			if (resource_exhausted(code))
			{
				return {store_write_state::resource_exhausted,
						transferred,
						code,
						true,
						transferred != 0U};
			}
			const auto state = code == EINTR || code == EIO ? store_write_state::outcome_unknown
															: store_write_state::failed;
			return {state, transferred, code, true, transferred != 0U};
		}
		return {store_write_state::complete, transferred, 0, true, true};
#else
		(void)bytes;
		return {store_write_state::failed, 0U, ENOTSUP, false, false};
#endif
	}

	store_sync_outcome
	default_store_operation_port::synchronize(const int descriptor,
											  const store_sync_target target) noexcept
	{
		if (!valid_sync_target(target))
			return {target, store_sync_state::failed, EINVAL, false};
		if (descriptor < 0)
			return {target, store_sync_state::failed, EBADF, false};

#if defined(__unix__) || defined(__APPLE__)
		errno = 0;
		int status{};
		if (target == store_sync_target::file_data)
		{
#if defined(__APPLE__)
			status = ::fsync(descriptor);
#else
			status = ::fdatasync(descriptor);
#endif
		}
		else
		{
			status = ::fsync(descriptor);
		}
		if (status == 0)
			return {target, store_sync_state::durable, 0, true};
		const auto code = errno;
		if (resource_exhausted(code))
			return {target, store_sync_state::resource_exhausted, code, true};
		if (code == EINTR || code == EIO)
			return {target, store_sync_state::outcome_unknown, code, true};
		return {target, store_sync_state::failed, code, true};
#else
		return {target, store_sync_state::failed, ENOTSUP, false};
#endif
	}

	store_close_outcome
	default_store_operation_port::close_descriptor(const int descriptor) noexcept
	{
		if (descriptor < 0)
			return {store_close_state::not_attempted, EBADF, false};
#if defined(__unix__) || defined(__APPLE__)
		errno = 0;
		if (::close(descriptor) == 0)
			return {store_close_state::confirmed_closed, 0, true};
		// POSIX does not provide a portable retry-safe interpretation of a failed close. Never
		// retry.
		return {store_close_state::outcome_unknown, errno, true};
#else
		return {store_close_state::not_attempted, ENOTSUP, false};
#endif
	}

	store_close_outcome default_store_operation_port::close_sqlite(
		const store_sqlite_operation_binding binding) noexcept
	{
		if (!valid_sqlite_binding(binding))
			return {store_close_state::not_attempted, EINVAL, false};
		try
		{
			const auto code = binding.invoke(binding.context);
			return code == binding.success_code
				? store_close_outcome{store_close_state::confirmed_closed, 0, true}
				: store_close_outcome{store_close_state::outcome_unknown, code, true};
		}
		catch (...)
		{
			return {store_close_state::outcome_unknown, 0, true};
		}
	}

	store_commit_outcome default_store_operation_port::commit_sqlite(
		const store_sqlite_operation_binding binding) noexcept
	{
		if (!valid_sqlite_binding(binding))
			return {store_commit_state::not_attempted, EINVAL, false, false};
		try
		{
			const auto code = binding.invoke(binding.context);
			if (code == binding.success_code)
				return {store_commit_state::committed, 0, true, true};
			if (code == binding.resource_exhausted_code)
				return {store_commit_state::resource_exhausted, code, true, true};
			return {store_commit_state::outcome_unknown, code, true, true};
		}
		catch (...)
		{
			return {store_commit_state::outcome_unknown, 0, true, true};
		}
	}
} // namespace cxxlens::sdk
