#include <cerrno>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "sdk/gcc_capture_file_port_internal.hpp"

#if defined(__linux__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cxxlens::sdk::detail
{
	namespace
	{
		[[nodiscard]] error io_error(std::string field, std::string detail)
		{
			return {"application-analysis.capture-io-failed", std::move(field), std::move(detail)};
		}

		[[nodiscard]] error limit_error(std::string field, std::string detail)
		{
			return {
				"application-analysis.import-limit-exceeded", std::move(field), std::move(detail)};
		}

#if defined(__linux__)
		class descriptor
		{
		  public:
			explicit descriptor(const int value) noexcept : value_{value} {}
			~descriptor()
			{
				if (value_ >= 0)
					(void)::close(value_);
			}
			descriptor(const descriptor&) = delete;
			descriptor& operator=(const descriptor&) = delete;
			[[nodiscard]] int get() const noexcept
			{
				return value_;
			}

		  private:
			int value_;
		};

		[[nodiscard]] result<std::string>
		canonical_descriptor_path(const descriptor& file, const std::size_t maximum_path_bytes)
		{
			if (maximum_path_bytes == 0U ||
				maximum_path_bytes >= std::numeric_limits<std::size_t>::max() - 1U)
				return unexpected(limit_error("capture.path", "path-bytes"));
			const auto link = "/proc/self/fd/" + std::to_string(file.get());
			std::vector<char> buffer(maximum_path_bytes + 1U);
			const auto count = ::readlink(link.c_str(), buffer.data(), maximum_path_bytes + 1U);
			if (count < 0)
				return unexpected(io_error("capture.path", "canonical-path"));
			if (std::cmp_greater(count, maximum_path_bytes))
				return unexpected(limit_error("capture.path", "path-bytes"));
			std::string output{buffer.data(), static_cast<std::size_t>(count)};
			if (output.empty() || !output.starts_with('/') || output.ends_with(" (deleted)"))
				return unexpected(io_error("capture.path", "canonical-path"));
			return output;
		}

		[[nodiscard]] bool same_file_state(const struct stat& left,
										   const struct stat& right) noexcept
		{
			return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
				left.st_mode == right.st_mode && left.st_size == right.st_size &&
				left.st_mtim.tv_sec == right.st_mtim.tv_sec &&
				left.st_mtim.tv_nsec == right.st_mtim.tv_nsec &&
				left.st_ctim.tv_sec == right.st_ctim.tv_sec &&
				left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
		}

		[[nodiscard]] bool at_or_below(const std::string_view path,
									   const std::string_view root) noexcept
		{
			return root.empty() || path == root ||
				(path.size() > root.size() && path.starts_with(root) &&
				 (root == "/" || path[root.size()] == '/'));
		}
#endif

		class system_gcc_capture_file_port final : public gcc_capture_file_port
		{
		  public:
			result<std::string> canonical_directory(const std::string_view path,
													const std::size_t maximum_path_bytes) override
			{
#if defined(__linux__)
				try
				{
					if (path.empty() || path.contains('\0'))
						return unexpected(io_error("capture.directory", "invalid-path"));
					const std::string owned{path};
					descriptor file{::open(owned.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY)};
					if (file.get() < 0)
						return unexpected(io_error("capture.directory", "open"));
					return canonical_descriptor_path(file, maximum_path_bytes);
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(io_error("capture.directory", "allocation"));
				}
				catch (const std::length_error&)
				{
					return unexpected(io_error("capture.directory", "allocation"));
				}
#else
				(void)path;
				(void)maximum_path_bytes;
				return unexpected(io_error("capture.directory", "unsupported-platform"));
#endif
			}

			result<capture_file_snapshot>
			read_regular_file(const std::string_view path,
							  const capture_file_read_limits limits) override
			{
#if defined(__linux__)
				if (path.empty() || path.contains('\0'))
					return unexpected(io_error("capture.file", "invalid-path"));
				try
				{
					const std::string owned{path};
					descriptor file{::open(owned.c_str(), O_RDONLY | O_CLOEXEC)};
					if (file.get() < 0)
						return unexpected(io_error("capture.file", "open"));
					struct stat before{};
					if (::fstat(file.get(), &before) != 0)
						return unexpected(io_error("capture.file", "metadata"));
					if (!S_ISREG(before.st_mode))
						return unexpected(io_error("capture.file", "not-regular-file"));
					if (before.st_size < 0 ||
						std::cmp_greater(before.st_size, limits.maximum_file_bytes) ||
						static_cast<std::uint64_t>(before.st_size) >
							std::numeric_limits<std::size_t>::max())
						return unexpected(limit_error("capture.file", "byte-count"));
					auto canonical = canonical_descriptor_path(file, limits.maximum_path_bytes);
					if (!canonical)
						return unexpected(std::move(canonical.error()));
					if (!at_or_below(*canonical, limits.required_canonical_root))
						return unexpected(io_error("capture.file", "path-outside-project-root"));
					std::vector<std::byte> content(static_cast<std::size_t>(before.st_size));
					std::size_t offset{};
					while (offset < content.size())
					{
						const auto count =
							::read(file.get(), content.data() + offset, content.size() - offset);
						if (count < 0 && errno == EINTR)
							continue;
						if (count <= 0)
							return unexpected(io_error("capture.file", "short-read"));
						offset += static_cast<std::size_t>(count);
					}
					std::byte extra{};
					for (;;)
					{
						const auto count = ::read(file.get(), &extra, 1U);
						if (count < 0 && errno == EINTR)
							continue;
						if (count < 0)
							return unexpected(io_error("capture.file", "read"));
						if (count != 0)
							return unexpected(io_error("capture.file", "changed-during-read"));
						break;
					}
					struct stat after{};
					if (::fstat(file.get(), &after) != 0)
						return unexpected(io_error("capture.file", "metadata"));
					if (!same_file_state(before, after))
						return unexpected(io_error("capture.file", "changed-during-read"));
					return capture_file_snapshot{std::move(*canonical), std::move(content)};
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(io_error("capture.file", "allocation"));
				}
				catch (const std::length_error&)
				{
					return unexpected(io_error("capture.file", "allocation"));
				}
#else
				(void)path;
				(void)limits;
				return unexpected(io_error("capture.file", "unsupported-platform"));
#endif
			}
		};
	} // namespace

	std::unique_ptr<gcc_capture_file_port> make_system_gcc_capture_file_port()
	{
		return std::make_unique<system_gcc_capture_file_port>();
	}
} // namespace cxxlens::sdk::detail
