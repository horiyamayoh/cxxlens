#pragma once

/**
 * @file sealed_executable_internal.hpp
 * @brief Source-private authority for measuring and sealing one executable image.
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <cxxlens/sdk/common.hpp>

#if defined(__GNUC__) || defined(__clang__)
#define CXXLENS_RUNTIME_DETAIL_HIDDEN __attribute__((visibility("hidden")))
#else
#define CXXLENS_RUNTIME_DETAIL_HIDDEN
#endif

namespace cxxlens::sdk::detail
{
	struct CXXLENS_RUNTIME_DETAIL_HIDDEN sealed_executable_request
	{
		std::string_view executable_path;
		std::string_view working_directory;
		std::optional<std::uint64_t> absolute_wall_deadline_ns;
		std::optional<std::uint64_t> maximum_image_bytes;
		std::optional<std::size_t> maximum_canonical_path_bytes;
	};

	/** Exact bytes that were measured and can only be executed from the sealed descriptor. */
	class CXXLENS_RUNTIME_DETAIL_HIDDEN sealed_executable final
	{
	  public:
		sealed_executable(int image,
						  std::string digest,
						  std::string canonical_source_path,
						  std::uint64_t byte_count) noexcept;
		~sealed_executable();
		sealed_executable(const sealed_executable&) = delete;
		sealed_executable& operator=(const sealed_executable&) = delete;
		sealed_executable(sealed_executable&& other) noexcept;
		sealed_executable& operator=(sealed_executable&& other) noexcept;

		[[nodiscard]] int native_handle() const noexcept;
		[[nodiscard]] const std::string& digest() const noexcept;
		[[nodiscard]] const std::string& canonical_source_path() const noexcept;
		[[nodiscard]] std::uint64_t byte_count() const noexcept;

	  private:
		int image_{-1};
		std::string digest_;
		std::string canonical_source_path_;
		std::uint64_t byte_count_{};
	};

	struct CXXLENS_RUNTIME_DETAIL_HIDDEN canonical_descriptor_path_request
	{
		int descriptor{-1};
		std::size_t maximum_path_bytes{};
	};

	/** Resolve the canonical path of an already-open descriptor without reopening its pathname. */
	[[nodiscard]] CXXLENS_RUNTIME_DETAIL_HIDDEN result<std::string>
	canonical_open_descriptor_path(canonical_descriptor_path_request request);

	/**
	 * Open one explicit executable, copy and hash it once, then irreversibly seal the copied image.
	 * A requested canonical path is derived from the same opened descriptor. Unsupported hosts fail
	 * closed; no path re-open or ambient executable lookup is performed.
	 */
	[[nodiscard]] CXXLENS_RUNTIME_DETAIL_HIDDEN result<sealed_executable>
	open_sealed_executable(const sealed_executable_request& request);
} // namespace cxxlens::sdk::detail

#undef CXXLENS_RUNTIME_DETAIL_HIDDEN
