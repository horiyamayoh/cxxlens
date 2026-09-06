#pragma once

/** @file msvc_capture_file_port.hpp @brief Minimal worker filesystem capability. */

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>

namespace cxxlens::application_analysis_worker
{
	[[nodiscard]] sdk::result<std::string> read_worker_text_file(std::string_view path);
	[[nodiscard]] sdk::result<void> write_worker_binary_file(std::string_view path,
															 std::span<const std::byte> content);

#ifdef _WIN32
	class msvc_capture_workspace
	{
	  public:
		virtual ~msvc_capture_workspace() = default;
		[[nodiscard]] virtual const std::wstring& dependency_output_path() const noexcept = 0;
		virtual void clear_dependency_output() noexcept = 0;
		[[nodiscard]] virtual sdk::result<std::string>
		read_dependency_output(std::size_t maximum_bytes) const = 0;
		[[nodiscard]] virtual sdk::result<std::string>
		publish_bundle(std::span<const std::byte> bundle) = 0;
	};

	[[nodiscard]] sdk::result<std::wstring> canonical_worker_directory(std::wstring_view path);
	[[nodiscard]] sdk::result<std::wstring> canonical_worker_file(std::wstring_view path);
	[[nodiscard]] sdk::result<std::wstring> current_worker_directory();
	[[nodiscard]] sdk::result<std::vector<std::byte>>
	read_worker_binary_file(std::wstring_view path, std::size_t maximum_bytes);
	[[nodiscard]] sdk::result<std::unique_ptr<msvc_capture_workspace>>
	make_msvc_capture_workspace(std::wstring_view directory);
#endif
} // namespace cxxlens::application_analysis_worker
