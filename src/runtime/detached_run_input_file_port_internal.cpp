#include "detached_run_input_file_port_internal.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>
#include <utility>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cxxlens::runtime
{
	namespace
	{
		[[nodiscard]] sdk::error unavailable(std::string field, std::string detail)
		{
			return {"application-analysis.detached-run-input-unavailable",
					std::move(field),
					std::move(detail)};
		}

		[[nodiscard]] sdk::error invalid(std::string field, std::string detail)
		{
			return {"application-analysis.detached-run-input-invalid",
					std::move(field),
					std::move(detail)};
		}

		[[nodiscard]] sdk::error limit(std::string field, std::string detail)
		{
			return {"application-analysis.detached-run-input-limit",
					std::move(field),
					std::move(detail)};
		}

		[[nodiscard]] std::string indexed_field(const std::size_t index)
		{
			return "detached_run_files[" + std::to_string(index) + ']';
		}

		[[nodiscard]] std::filesystem::path native_path(const std::string& value)
		{
#if defined(_WIN32)
			std::u8string utf8;
			utf8.reserve(value.size());
			for (const auto byte : value)
				utf8.push_back(static_cast<char8_t>(static_cast<unsigned char>(byte)));
			return std::filesystem::path{utf8};
#else
			return std::filesystem::path{value};
#endif
		}

#if defined(__linux__)
		class descriptor final
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

		[[nodiscard]] bool same_file_state(const struct stat& left,
										   const struct stat& right) noexcept
		{
			return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
				left.st_size == right.st_size && left.st_mtim.tv_sec == right.st_mtim.tv_sec &&
				left.st_mtim.tv_nsec == right.st_mtim.tv_nsec;
		}
#endif
	} // namespace

	sdk::result<std::vector<std::vector<std::byte>>>
	detached_run_input_file_port::read(const std::span<const std::string> paths,
									   const sdk::import_limits limits) const
	{
		try
		{
			auto valid_limits = limits.validate();
			if (!valid_limits)
				return sdk::unexpected(std::move(valid_limits.error()));
			if (paths.empty())
				return sdk::unexpected(invalid("detached_run_files", "empty"));
			if (paths.size() > limits.maximum_compile_units)
				return sdk::unexpected(limit("detached_run_files", "count"));

			std::set<std::filesystem::path> unique_paths;
			std::vector<std::vector<std::byte>> output;
			output.reserve(paths.size());
			std::size_t total_bytes{};
			for (std::size_t index{}; index < paths.size(); ++index)
			{
				const auto field = indexed_field(index);
				if (paths[index].empty() || paths[index].contains('\0'))
					return sdk::unexpected(invalid(field, "path"));
				if (paths[index].size() > limits.maximum_string_bytes)
					return sdk::unexpected(limit(field, "path-bytes"));
				const auto path = native_path(paths[index]);
				if (!path.is_absolute())
					return sdk::unexpected(invalid(field, "absolute-path-required"));
				if (path.lexically_normal() != path)
					return sdk::unexpected(invalid(field, "normalized-path-required"));
				if (!unique_paths.insert(path).second)
					return sdk::unexpected(invalid(field, "duplicate-path"));

#if defined(__linux__)
				descriptor input{::open(paths[index].c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
				if (input.get() < 0)
				{
					if (errno == ELOOP)
						return sdk::unexpected(invalid(field, "not-regular"));
					return sdk::unexpected(unavailable(field, "open"));
				}
				struct stat before{};
				if (::fstat(input.get(), &before) != 0)
					return sdk::unexpected(unavailable(field, "metadata"));
				if (!S_ISREG(before.st_mode))
					return sdk::unexpected(invalid(field, "not-regular"));
				if (before.st_size == 0)
					return sdk::unexpected(invalid(field, "empty"));
				if (before.st_size < 0 ||
					static_cast<std::uintmax_t>(before.st_size) > limits.maximum_bundle_bytes ||
					static_cast<std::uintmax_t>(before.st_size) >
						std::numeric_limits<std::size_t>::max())
					return sdk::unexpected(limit(field, "bytes"));
				const auto size = static_cast<std::size_t>(before.st_size);
				if (size > limits.maximum_total_metadata_bytes - total_bytes)
					return sdk::unexpected(limit("detached_run_files", "total-bytes"));

				std::vector<std::byte> bytes(size);
				std::size_t offset{};
				while (offset < bytes.size())
				{
					const auto remaining = bytes.size() - offset;
					const auto chunk = std::min(
						remaining, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
					const auto count = ::read(input.get(), bytes.data() + offset, chunk);
					if (count < 0 && errno == EINTR)
						continue;
					if (count < 0)
						return sdk::unexpected(unavailable(field, "read"));
					if (count == 0)
						return sdk::unexpected(invalid(field, "truncated"));
					offset += static_cast<std::size_t>(count);
				}
				std::byte trailing{};
				for (;;)
				{
					const auto count = ::read(input.get(), &trailing, 1U);
					if (count < 0 && errno == EINTR)
						continue;
					if (count < 0)
						return sdk::unexpected(unavailable(field, "read"));
					if (count != 0)
						return sdk::unexpected(invalid(field, "trailing-bytes"));
					break;
				}
				struct stat after{};
				if (::fstat(input.get(), &after) != 0)
					return sdk::unexpected(unavailable(field, "metadata"));
				if (!same_file_state(before, after))
					return sdk::unexpected(invalid(field, "changed-during-read"));
				total_bytes += size;
				output.push_back(std::move(bytes));
#else
				(void)path;
				return sdk::unexpected(unavailable(field, "unsupported-platform"));
#endif
			}
			return output;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(unavailable("detached_run_files", "allocation"));
		}
		catch (const std::length_error&)
		{
			return sdk::unexpected(limit("detached_run_files", "allocation-length"));
		}
	}
} // namespace cxxlens::runtime
