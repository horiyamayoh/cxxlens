#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "fault_plan.hpp"

namespace cxxlens::detail::runtime
{

	struct file_status
	{
		bool exists{};
		bool regular{};
		std::uintmax_t size{};
	};

	class filesystem_port
	{
	  public:
		virtual ~filesystem_port() = default;
		[[nodiscard]] virtual runtime_result<std::string>
		read(const std::filesystem::path& path, const request_context& context) const = 0;
		[[nodiscard]] virtual runtime_result<std::vector<std::filesystem::path>>
		list(const std::filesystem::path& path, const request_context& context) const = 0;
		[[nodiscard]] virtual runtime_result<file_status>
		stat(const std::filesystem::path& path, const request_context& context) const = 0;
		[[nodiscard]] virtual runtime_result<std::filesystem::path>
		canonicalize(const std::filesystem::path& path, const request_context& context) const = 0;
		[[nodiscard]] virtual runtime_result<bool> remove(const std::filesystem::path& path,
														  const request_context& context) = 0;
	};

	class standard_filesystem_adapter final : public filesystem_port
	{
	  public:
		[[nodiscard]] runtime_result<std::string>
		read(const std::filesystem::path& path, const request_context& context) const override;
		[[nodiscard]] runtime_result<std::vector<std::filesystem::path>>
		list(const std::filesystem::path& path, const request_context& context) const override;
		[[nodiscard]] runtime_result<file_status>
		stat(const std::filesystem::path& path, const request_context& context) const override;
		[[nodiscard]] runtime_result<std::filesystem::path>
		canonicalize(const std::filesystem::path& path,
					 const request_context& context) const override;
		[[nodiscard]] runtime_result<bool> remove(const std::filesystem::path& path,
												  const request_context& context) override;
	};

	class memory_filesystem_adapter final : public filesystem_port
	{
	  public:
		explicit memory_filesystem_adapter(fault_plan faults = {});
		memory_filesystem_adapter& add(const std::filesystem::path& path, std::string content);
		memory_filesystem_adapter& reverse_enumeration(bool enabled) noexcept;

		[[nodiscard]] runtime_result<std::string>
		read(const std::filesystem::path& path, const request_context& context) const override;
		[[nodiscard]] runtime_result<std::vector<std::filesystem::path>>
		list(const std::filesystem::path& path, const request_context& context) const override;
		[[nodiscard]] runtime_result<file_status>
		stat(const std::filesystem::path& path, const request_context& context) const override;
		[[nodiscard]] runtime_result<std::filesystem::path>
		canonicalize(const std::filesystem::path& path,
					 const request_context& context) const override;
		[[nodiscard]] runtime_result<bool> remove(const std::filesystem::path& path,
												  const request_context& context) override;

	  private:
		std::map<std::filesystem::path, std::string> files_;
		fault_plan faults_;
		bool reverse_{};
	};

	[[nodiscard]] std::vector<std::filesystem::path>
	canonical_path_order(std::vector<std::filesystem::path> paths);
	[[nodiscard]] bool is_within_root(const std::filesystem::path& root,
									  const std::filesystem::path& candidate);

} // namespace cxxlens::detail::runtime
