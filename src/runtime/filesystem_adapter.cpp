#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#include "filesystem_port.hpp"

namespace cxxlens::detail::runtime
{
	namespace
	{
		[[nodiscard]] runtime_failure map_error(const std::string& operation,
												const std::error_code& error)
		{
			auto status = runtime_status::platform_failure;
			if (error == std::errc::permission_denied)
			{
				status = runtime_status::permission_denied;
			}
			else if (error == std::errc::no_such_file_or_directory)
			{
				status = runtime_status::missing;
			}
			return {status, operation, error.value()};
		}

		[[nodiscard]] std::optional<runtime_failure> preflight(const request_context& context)
		{
			if (context.cancelled())
			{
				return runtime_failure{runtime_status::cancelled, context.operation, 0};
			}
			if (context.deadline && std::chrono::steady_clock::now() >= *context.deadline)
			{
				return runtime_failure{runtime_status::timed_out, context.operation, 0};
			}
			return std::nullopt;
		}

		[[nodiscard]] std::filesystem::path normalized(const std::filesystem::path& path)
		{
			return path.lexically_normal();
		}
	} // namespace

	runtime_result<std::string>
	standard_filesystem_adapter::read(const std::filesystem::path& path,
									  const request_context& context) const
	{
		if (auto failure = preflight(context))
		{
			return unexpected{std::move(*failure)};
		}
		std::error_code error;
		const auto expected_size = std::filesystem::file_size(path, error);
		if (error)
		{
			return unexpected{map_error(context.operation, error)};
		}
		std::ifstream input{path, std::ios::binary};
		if (!input)
		{
			return unexpected{
				runtime_failure{runtime_status::platform_failure, context.operation, errno}};
		}
		std::string content{std::istreambuf_iterator<char>{input},
							std::istreambuf_iterator<char>{}};
		if (input.bad() || content.size() != expected_size)
		{
			return unexpected{
				runtime_failure{runtime_status::short_read, context.operation, errno}};
		}
		return content;
	}

	runtime_result<std::vector<std::filesystem::path>>
	standard_filesystem_adapter::list(const std::filesystem::path& path,
									  const request_context& context) const
	{
		if (auto failure = preflight(context))
		{
			return unexpected{std::move(*failure)};
		}
		std::error_code error;
		std::vector<std::filesystem::path> result;
		for (std::filesystem::directory_iterator iterator{path, error}, end;
			 iterator != end && !error;
			 ++iterator)
		{
			result.push_back(iterator->path());
		}
		if (error)
		{
			return unexpected{map_error(context.operation, error)};
		}
		return result;
	}

	runtime_result<file_status>
	standard_filesystem_adapter::stat(const std::filesystem::path& path,
									  const request_context& context) const
	{
		if (auto failure = preflight(context))
		{
			return unexpected{std::move(*failure)};
		}
		std::error_code error;
		const auto status = std::filesystem::status(path, error);
		if (error)
		{
			return unexpected{map_error(context.operation, error)};
		}
		std::uintmax_t size{};
		if (std::filesystem::is_regular_file(status))
		{
			size = std::filesystem::file_size(path, error);
			if (error)
			{
				return unexpected{map_error(context.operation, error)};
			}
		}
		return file_status{
			std::filesystem::exists(status), std::filesystem::is_regular_file(status), size};
	}

	runtime_result<std::filesystem::path>
	standard_filesystem_adapter::canonicalize(const std::filesystem::path& path,
											  const request_context& context) const
	{
		if (auto failure = preflight(context))
		{
			return unexpected{std::move(*failure)};
		}
		std::error_code error;
		auto result = std::filesystem::weakly_canonical(path, error);
		if (error)
		{
			return unexpected{map_error(context.operation, error)};
		}
		return result;
	}

	runtime_result<bool> standard_filesystem_adapter::remove(const std::filesystem::path& path,
															 const request_context& context)
	{
		if (auto failure = preflight(context))
			return unexpected{std::move(*failure)};
		std::error_code error;
		const bool removed = std::filesystem::remove(path, error);
		if (error)
			return unexpected{map_error(context.operation, error)};
		return removed;
	}

	runtime_result<bool>
	standard_filesystem_adapter::create_directories(const std::filesystem::path& path,
													const request_context& context)
	{
		if (auto failure = preflight(context))
			return unexpected{std::move(*failure)};
		std::error_code error;
		const bool created = std::filesystem::create_directories(path, error);
		if (error)
			return unexpected{map_error(context.operation, error)};
		return created || std::filesystem::is_directory(path);
	}

	memory_filesystem_adapter::memory_filesystem_adapter(fault_plan faults)
		: faults_{std::move(faults)}
	{
	}

	memory_filesystem_adapter& memory_filesystem_adapter::add(const std::filesystem::path& path,
															  std::string content)
	{
		files_.insert_or_assign(normalized(path), std::move(content));
		return *this;
	}

	memory_filesystem_adapter&
	memory_filesystem_adapter::reverse_enumeration(const bool enabled) noexcept
	{
		reverse_ = enabled;
		return *this;
	}

	runtime_result<std::string>
	memory_filesystem_adapter::read(const std::filesystem::path& path,
									const request_context& context) const
	{
		if (auto failure = preflight(context))
		{
			return unexpected{std::move(*failure)};
		}
		if (const auto* failure = faults_.match(context))
		{
			return unexpected{*failure};
		}
		const auto found = files_.find(normalized(path));
		if (found == files_.end())
		{
			return unexpected{runtime_failure{runtime_status::missing, context.operation, 0}};
		}
		return found->second;
	}

	runtime_result<std::vector<std::filesystem::path>>
	memory_filesystem_adapter::list(const std::filesystem::path& path,
									const request_context& context) const
	{
		if (auto failure = preflight(context))
		{
			return unexpected{std::move(*failure)};
		}
		if (const auto* failure = faults_.match(context))
		{
			return unexpected{*failure};
		}
		std::vector<std::filesystem::path> result;
		const auto root = normalized(path);
		for (const auto& [candidate, unused] : files_)
		{
			(void)unused;
			if (is_within_root(root, candidate))
			{
				result.push_back(candidate);
			}
		}
		if (reverse_)
		{
			std::ranges::reverse(result);
		}
		return result;
	}

	runtime_result<file_status>
	memory_filesystem_adapter::stat(const std::filesystem::path& path,
									const request_context& context) const
	{
		if (auto failure = preflight(context))
		{
			return unexpected{std::move(*failure)};
		}
		if (const auto* failure = faults_.match(context))
		{
			return unexpected{*failure};
		}
		const auto found = files_.find(normalized(path));
		return found == files_.end() ? file_status{}
									 : file_status{true, true, found->second.size()};
	}

	runtime_result<std::filesystem::path>
	memory_filesystem_adapter::canonicalize(const std::filesystem::path& path,
											const request_context& context) const
	{
		if (auto failure = preflight(context))
		{
			return unexpected{std::move(*failure)};
		}
		if (const auto* failure = faults_.match(context))
		{
			return unexpected{*failure};
		}
		return normalized(path);
	}

	runtime_result<bool> memory_filesystem_adapter::remove(const std::filesystem::path& path,
														   const request_context& context)
	{
		if (auto failure = preflight(context))
			return unexpected{std::move(*failure)};
		if (const auto* failure = faults_.match(context))
			return unexpected{*failure};
		return files_.erase(normalized(path)) != 0U;
	}

	runtime_result<bool>
	memory_filesystem_adapter::create_directories(const std::filesystem::path&,
												  const request_context& context)
	{
		if (auto failure = preflight(context))
			return unexpected{std::move(*failure)};
		if (const auto* failure = faults_.match(context))
			return unexpected{*failure};
		return true;
	}

	std::vector<std::filesystem::path>
	canonical_path_order(std::vector<std::filesystem::path> paths)
	{
		std::ranges::sort(paths,
						  {},
						  [](const auto& path)
						  {
							  return path.generic_string();
						  });
		return paths;
	}

	bool is_within_root(const std::filesystem::path& root, const std::filesystem::path& candidate)
	{
		const auto normalized_root = normalized(root);
		const auto normalized_candidate = normalized(candidate);
		auto root_part = normalized_root.begin();
		auto candidate_part = normalized_candidate.begin();
		for (; root_part != normalized_root.end(); ++root_part, ++candidate_part)
		{
			if (candidate_part == normalized_candidate.end() || *root_part != *candidate_part)
			{
				return false;
			}
		}
		return true;
	}

} // namespace cxxlens::detail::runtime
