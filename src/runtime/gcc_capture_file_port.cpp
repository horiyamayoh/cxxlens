#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "sdk/gcc_capture_file_port_internal.hpp"
#include "sealed_executable_internal.hpp"

#if defined(__linux__)
#include <cstdio>

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

		[[nodiscard]] error unavailable_error(std::string field, std::string detail)
		{
			return {
				std::string{capture_file_unavailable_code}, std::move(field), std::move(detail)};
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
			auto path = canonical_open_descriptor_path({file.get(), maximum_path_bytes});
			if (path)
				return path;
			if (path.error().code == "runtime.descriptor-path-limit")
				return unexpected(limit_error("capture.path", "path-bytes"));
			return unexpected(io_error("capture.path", "canonical-path"));
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

		class system_capture_workspace final : public gcc_capture_workspace
		{
		  public:
			system_capture_workspace(std::string directory,
									 std::string workspace,
									 const std::size_t maximum_path_bytes)
				: directory_{std::move(directory)}, workspace_{std::move(workspace)},
				  dependency_{workspace_ + "/dependencies.d"}, staging_{workspace_ + "/bundle.tmp"},
				  maximum_path_bytes_{maximum_path_bytes}
			{
			}
			~system_capture_workspace() override
			{
				(void)::unlink(dependency_.c_str());
				(void)::unlink(staging_.c_str());
				for (const auto& path : staged_specifications_)
					(void)::unlink(path.c_str());
				(void)::rmdir(workspace_.c_str());
			}
			[[nodiscard]] std::string_view dependency_output_path() const noexcept override
			{
				return dependency_;
			}
			[[nodiscard]] result<std::string>
			stage_specification(const std::span<const std::byte> content,
								const std::size_t index) override
			{
				try
				{
					auto path = workspace_ + "/spec-" + std::to_string(index) + ".spec";
					if (path.size() > maximum_path_bytes_)
						return unexpected(limit_error("capture.specification", "path-bytes"));
					staged_specifications_.push_back(path);
					descriptor output{
						::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)};
					if (output.get() < 0)
						return unexpected(io_error("capture.specification", "create"));
					std::size_t offset{};
					while (offset < content.size())
					{
						const auto count =
							::write(output.get(), content.data() + offset, content.size() - offset);
						if (count < 0 && errno == EINTR)
							continue;
						if (count <= 0)
							return unexpected(io_error("capture.specification", "write"));
						offset += static_cast<std::size_t>(count);
					}
					if (::fsync(output.get()) != 0)
						return unexpected(io_error("capture.specification", "fsync"));
					return path;
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(io_error("capture.specification", "allocation"));
				}
				catch (const std::length_error&)
				{
					return unexpected(io_error("capture.specification", "allocation-length"));
				}
			}
			[[nodiscard]] result<std::string>
			publish_bundle(const std::span<const std::byte> content) override
			{
				try
				{
					const auto digest = content_digest(content);
					if (!digest.starts_with("sha256:") || digest.size() != 71U)
						return unexpected(io_error("capture.bundle", "digest"));
					const auto destination =
						directory_ + "/capture-" + digest.substr(7U) + ".cxxlens";
					if (destination.size() > maximum_path_bytes_)
						return unexpected(limit_error("capture.bundle", "path-bytes"));
					descriptor output{
						::open(staging_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)};
					if (output.get() < 0)
						return unexpected(io_error("capture.bundle", "create"));
					std::size_t offset{};
					while (offset < content.size())
					{
						const auto count =
							::write(output.get(), content.data() + offset, content.size() - offset);
						if (count < 0 && errno == EINTR)
							continue;
						if (count <= 0)
							return unexpected(io_error("capture.bundle", "write"));
						offset += static_cast<std::size_t>(count);
					}
					if (::fsync(output.get()) != 0)
						return unexpected(io_error("capture.bundle", "fsync"));
					if (::rename(staging_.c_str(), destination.c_str()) != 0)
						return unexpected(io_error("capture.bundle", "rename"));
					return destination;
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(io_error("capture.bundle", "allocation"));
				}
				catch (const std::length_error&)
				{
					return unexpected(io_error("capture.bundle", "allocation-length"));
				}
			}

		  private:
			std::string directory_;
			std::string workspace_;
			std::string dependency_;
			std::string staging_;
			std::vector<std::string> staged_specifications_;
			std::size_t maximum_path_bytes_{};
		};
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
					{
						if (errno == ENOENT || errno == ENOTDIR)
							return unexpected(unavailable_error("capture.file", "missing"));
						if (errno == EACCES || errno == EPERM)
							return unexpected(
								unavailable_error("capture.file", "permission-denied"));
						return unexpected(io_error("capture.file", "open"));
					}
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

			result<std::string>
			digest_regular_tree(const std::string_view path,
								const capture_tree_digest_limits limits) override
			{
#if defined(__linux__)
				if (path.empty() || path.contains('\0') || limits.maximum_files == 0U ||
					limits.maximum_total_bytes == 0U || limits.maximum_path_bytes == 0U)
					return unexpected(io_error("capture.tree", "invalid-request"));
				try
				{
					auto root = canonical_directory(path, limits.maximum_path_bytes);
					if (!root)
						return unexpected(std::move(root.error()));
					auto enumerate = [&]() -> result<std::vector<std::string>>
					{
						std::vector<std::string> paths;
						std::error_code failure;
						std::filesystem::recursive_directory_iterator iterator{
							*root, std::filesystem::directory_options::none, failure};
						const std::filesystem::recursive_directory_iterator end;
						if (failure)
							return unexpected(io_error("capture.tree", "enumerate"));
						for (; iterator != end; iterator.increment(failure))
						{
							if (failure)
								return unexpected(io_error("capture.tree", "enumerate"));
							const auto status = iterator->symlink_status(failure);
							if (failure)
								return unexpected(io_error("capture.tree", "metadata"));
							if (std::filesystem::is_symlink(status))
							{
								if (std::filesystem::is_directory(iterator->status(failure)))
									iterator.disable_recursion_pending();
								return unexpected(io_error("capture.tree", "symlink"));
							}
							if (!std::filesystem::is_regular_file(status))
								continue;
							auto entry = iterator->path().string();
							if (entry.size() > limits.maximum_path_bytes)
								return unexpected(limit_error("capture.tree", "path-bytes"));
							paths.push_back(std::move(entry));
							if (paths.size() > limits.maximum_files)
								return unexpected(limit_error("capture.tree", "file-count"));
						}
						std::ranges::sort(paths);
						return paths;
					};
					auto paths = enumerate();
					if (!paths)
						return unexpected(std::move(paths.error()));
					std::uint64_t remaining = limits.maximum_total_bytes;
					std::vector<canonical_value> entries;
					entries.reserve(paths->size());
					for (const auto& entry : *paths)
					{
						auto file =
							read_regular_file(entry, {remaining, limits.maximum_path_bytes, *root});
						if (!file)
							return unexpected(std::move(file.error()));
						if (file->content.size() > remaining)
							return unexpected(limit_error("capture.tree", "byte-count"));
						remaining -= static_cast<std::uint64_t>(file->content.size());
						std::error_code relative_failure;
						const auto relative = std::filesystem::relative(
							file->canonical_path, *root, relative_failure);
						if (relative_failure || relative.empty() ||
							relative.string().starts_with(".."))
							return unexpected(io_error("capture.tree", "relative-path"));
						entries.push_back(canonical_value::from_tuple({
							canonical_value::from_string(relative.generic_string()),
							canonical_value::from_string(content_digest(file->content)),
							canonical_value::from_integer(
								static_cast<std::int64_t>(file->content.size())),
						}));
					}
					auto rechecked = enumerate();
					if (!rechecked || *rechecked != *paths)
						return unexpected(io_error("capture.tree", "changed-during-read"));
					auto encoded =
						canonical_binary(canonical_value::from_tuple(std::move(entries)));
					if (!encoded)
						return unexpected(std::move(encoded.error()));
					return content_digest(*encoded);
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(io_error("capture.tree", "allocation"));
				}
				catch (const std::length_error&)
				{
					return unexpected(io_error("capture.tree", "allocation-length"));
				}
#else
				(void)path;
				(void)limits;
				return unexpected(unavailable_error("capture.tree", "unsupported-platform"));
#endif
			}

			result<std::unique_ptr<gcc_capture_workspace>>
			create_workspace(const std::string_view capture_directory,
							 const std::size_t maximum_path_bytes) override
			{
#if defined(__linux__)
				auto canonical = canonical_directory(capture_directory, maximum_path_bytes);
				if (!canonical)
					return unexpected(std::move(canonical.error()));
				try
				{
					if (maximum_path_bytes < 25U || canonical->size() > maximum_path_bytes - 25U)
						return unexpected(limit_error("capture.workspace", "path-bytes"));
					std::string pattern = *canonical + "/.cxxlens-capture-XXXXXX";
					if (::mkdtemp(pattern.data()) == nullptr)
						return unexpected(io_error("capture.workspace", "create"));
					return std::unique_ptr<gcc_capture_workspace>{
						std::make_unique<system_capture_workspace>(
							std::move(*canonical), std::move(pattern), maximum_path_bytes)};
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(io_error("capture.workspace", "allocation"));
				}
				catch (const std::length_error&)
				{
					return unexpected(io_error("capture.workspace", "allocation-length"));
				}
#else
				(void)capture_directory;
				(void)maximum_path_bytes;
				return unexpected(unavailable_error("capture.workspace", "unsupported-platform"));
#endif
			}
		};
	} // namespace

	std::unique_ptr<gcc_capture_file_port> make_system_gcc_capture_file_port()
	{
		return std::make_unique<system_gcc_capture_file_port>();
	}
} // namespace cxxlens::sdk::detail
