#include "msvc_capture_file_port.hpp"

#include <fstream>
#include <iterator>
#include <limits>
#include <utility>

#ifdef _WIN32
#include <filesystem>
#include <ranges>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace cxxlens::application_analysis_worker
{
	sdk::result<std::string> read_worker_text_file(const std::string_view path)
	{
		std::ifstream input{std::string{path}, std::ios::binary};
		if (!input)
			return sdk::unexpected(
				sdk::error{"application-analysis.worker-io-failed", "input", "open"});
		std::string content{std::istreambuf_iterator<char>{input},
							std::istreambuf_iterator<char>{}};
		if (input.bad())
			return sdk::unexpected(
				sdk::error{"application-analysis.worker-io-failed", "input", "read"});
		return content;
	}

	sdk::result<void> write_worker_binary_file(const std::string_view path,
											   const std::span<const std::byte> content)
	{
		std::ofstream output{std::string{path}, std::ios::binary | std::ios::trunc};
		if (!output)
			return sdk::unexpected(
				sdk::error{"application-analysis.worker-io-failed", "output", "open"});
		output.write(reinterpret_cast<const char*>(content.data()),
					 static_cast<std::streamsize>(content.size()));
		if (!output)
			return sdk::unexpected(
				sdk::error{"application-analysis.worker-io-failed", "output", "write"});
		return {};
	}

#ifdef _WIN32
	namespace
	{
		[[nodiscard]] sdk::error io_error(std::string field, std::string detail)
		{
			return {"application-analysis.worker-io-failed", std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::result<std::wstring> canonical_path(const std::wstring_view input,
															   const bool directory)
		{
			std::error_code failure;
			auto path =
				std::filesystem::canonical(std::filesystem::path{std::wstring{input}}, failure);
			if (failure || (directory && !std::filesystem::is_directory(path, failure)) ||
				(!directory && !std::filesystem::is_regular_file(path, failure)))
				return sdk::unexpected(io_error("path", "canonicalization"));
			return path.native();
		}

		[[nodiscard]] sdk::result<std::vector<std::byte>>
		read_binary(const std::filesystem::path& path, const std::size_t maximum_bytes)
		{
			std::error_code failure;
			const auto size = std::filesystem::file_size(path, failure);
			if (failure || size > maximum_bytes ||
				size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))
				return sdk::unexpected(io_error("input", "size-or-stat"));
			std::ifstream input{path, std::ios::binary};
			if (!input)
				return sdk::unexpected(io_error("input", "open"));
			std::vector<std::byte> content(static_cast<std::size_t>(size));
			input.read(reinterpret_cast<char*>(content.data()), static_cast<std::streamsize>(size));
			if (!input)
				return sdk::unexpected(io_error("input", "read"));
			return content;
		}

		[[nodiscard]] sdk::result<std::string> utf8(const std::wstring_view value)
		{
			if (value.empty())
				return std::string{};
			if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
				return sdk::unexpected(io_error("path", "utf16-length"));
			const auto size = WideCharToMultiByte(CP_UTF8,
												  WC_ERR_INVALID_CHARS,
												  value.data(),
												  static_cast<int>(value.size()),
												  nullptr,
												  0,
												  nullptr,
												  nullptr);
			if (size <= 0)
				return sdk::unexpected(io_error("path", "utf16-invalid"));
			std::string output(static_cast<std::size_t>(size), '\0');
			if (WideCharToMultiByte(CP_UTF8,
									WC_ERR_INVALID_CHARS,
									value.data(),
									static_cast<int>(value.size()),
									output.data(),
									size,
									nullptr,
									nullptr) != size)
				return sdk::unexpected(io_error("path", "utf16-conversion"));
			return output;
		}

		class system_msvc_capture_workspace final : public msvc_capture_workspace
		{
		  public:
			explicit system_msvc_capture_workspace(std::filesystem::path directory)
				: directory_{std::move(directory)},
				  dependency_output_{
					  directory_ /
					  ("source-dependencies." + std::to_string(GetCurrentProcessId()) + ".json")}
			{
			}

			[[nodiscard]] const std::wstring& dependency_output_path() const noexcept override
			{
				return dependency_output_.native();
			}

			void clear_dependency_output() noexcept override
			{
				std::error_code ignored;
				std::filesystem::remove(dependency_output_, ignored);
			}

			[[nodiscard]] sdk::result<std::string>
			read_dependency_output(const std::size_t maximum_bytes) const override
			{
				auto content = read_binary(dependency_output_, maximum_bytes);
				if (!content)
					return sdk::unexpected(std::move(content.error()));
				std::string output;
				output.reserve(content->size());
				for (const auto byte : *content)
					output.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
				return output;
			}

			[[nodiscard]] sdk::result<std::string>
			publish_bundle(const std::span<const std::byte> bundle) override
			{
				const auto digest = sdk::content_digest(bundle);
				const auto name =
					digest.substr(std::string_view{"sha256:"}.size()) + ".cxxlens-capture";
				const auto final = directory_ / name;
				const auto temporary =
					directory_ / (name + ".tmp." + std::to_string(GetCurrentProcessId()));
				{
					std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
					if (!output)
						return sdk::unexpected(io_error("output", "temporary-open"));
					output.write(reinterpret_cast<const char*>(bundle.data()),
								 static_cast<std::streamsize>(bundle.size()));
					if (!output)
					{
						output.close();
						std::error_code ignored;
						std::filesystem::remove(temporary, ignored);
						return sdk::unexpected(io_error("output", "temporary-write"));
					}
				}
				std::error_code failure;
				std::filesystem::rename(temporary, final, failure);
				if (failure)
				{
					if (!std::filesystem::exists(final))
					{
						std::filesystem::remove(temporary, failure);
						return sdk::unexpected(io_error("output", "publish"));
					}
					auto existing = read_binary(final, std::size_t{64U} * 1024U * 1024U);
					std::filesystem::remove(temporary, failure);
					if (!existing || !std::ranges::equal(*existing, bundle))
						return sdk::unexpected(io_error("output", "collision"));
				}
				return utf8(final.native());
			}

		  private:
			std::filesystem::path directory_;
			std::filesystem::path dependency_output_;
		};
	} // namespace

	sdk::result<std::wstring> canonical_worker_directory(const std::wstring_view path)
	{
		return canonical_path(path, true);
	}

	sdk::result<std::wstring> canonical_worker_file(const std::wstring_view path)
	{
		return canonical_path(path, false);
	}

	sdk::result<std::wstring> current_worker_directory()
	{
		std::vector<wchar_t> buffer(32768U);
		const auto size = GetCurrentDirectoryW(static_cast<DWORD>(buffer.size()), buffer.data());
		if (size == 0U || size >= buffer.size())
			return sdk::unexpected(io_error("working_directory", "read"));
		return canonical_path(std::wstring_view{buffer.data(), size}, true);
	}

	sdk::result<std::vector<std::byte>> read_worker_binary_file(const std::wstring_view path,
																const std::size_t maximum_bytes)
	{
		return read_binary(std::filesystem::path{std::wstring{path}}, maximum_bytes);
	}

	sdk::result<std::unique_ptr<msvc_capture_workspace>>
	make_msvc_capture_workspace(const std::wstring_view directory)
	{
		std::error_code failure;
		std::filesystem::create_directories(std::filesystem::path{std::wstring{directory}},
											failure);
		if (failure)
			return sdk::unexpected(io_error("capture_directory", "create"));
		auto canonical = canonical_path(directory, true);
		if (!canonical)
			return sdk::unexpected(std::move(canonical.error()));
		std::unique_ptr<msvc_capture_workspace> workspace =
			std::make_unique<system_msvc_capture_workspace>(
				std::filesystem::path{std::move(*canonical)});
		return workspace;
	}
#endif
} // namespace cxxlens::application_analysis_worker
