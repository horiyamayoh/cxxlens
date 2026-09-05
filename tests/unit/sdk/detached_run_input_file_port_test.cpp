#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/detached_run_input_file_port_internal.hpp"

namespace
{
	using namespace cxxlens;

	void require(const bool condition, const std::string_view detail)
	{
		if (!condition)
		{
			std::cerr << detail << '\n';
			std::abort();
		}
	}

	class temporary_directory
	{
	  public:
		temporary_directory()
		{
			path_ = std::filesystem::temp_directory_path() /
				("cxxlens detached input " + std::to_string(std::rand()));
			std::error_code error;
			std::filesystem::create_directories(path_, error);
			require(!error, "temporary directory creation failed");
		}

		~temporary_directory()
		{
			std::error_code ignored;
			std::filesystem::remove_all(path_, ignored);
		}

		[[nodiscard]] const std::filesystem::path& path() const noexcept
		{
			return path_;
		}

	  private:
		std::filesystem::path path_;
	};

	void write(const std::filesystem::path& path, const std::span<const std::byte> bytes)
	{
		std::ofstream output{path, std::ios::binary | std::ios::trunc};
		output.write(reinterpret_cast<const char*>(bytes.data()),
					 static_cast<std::streamsize>(bytes.size()));
		require(static_cast<bool>(output), "test envelope write failed");
	}

	void require_error(const sdk::result<std::vector<std::vector<std::byte>>>& value,
					   const std::string_view code,
					   const std::string_view field,
					   const std::string_view detail)
	{
		require(!value && value.error().code == code && value.error().field == field &&
					value.error().detail == detail,
				"unexpected detached input error");
	}
} // namespace

int main()
{
	using namespace cxxlens;
	const temporary_directory directory;
	const runtime::detached_run_input_file_port files;
	const auto first_path = directory.path() / "first.run";
	const auto second_path = directory.path() / "second.run";
	const std::array first{std::byte{1U}, std::byte{2U}, std::byte{3U}};
	const std::array second{std::byte{7U}, std::byte{8U}};
	write(first_path, first);
	write(second_path, second);

	const std::vector paths{second_path.string(), first_path.string()};
	auto loaded = files.read(paths);
	require(loaded && loaded->size() == 2U &&
				(*loaded)[0] == std::vector(second.begin(), second.end()) &&
				(*loaded)[1] == std::vector(first.begin(), first.end()),
			"bounded detached inputs did not preserve caller transport order");
	auto repeat = files.read(paths);
	require(repeat && *repeat == *loaded, "detached input read was not deterministic");

	require_error(files.read(std::span<const std::string>{}),
				  "application-analysis.detached-run-input-invalid",
				  "detached_run_files",
				  "empty");
	auto count_limits = sdk::import_limits{};
	count_limits.maximum_compile_units = 1U;
	require_error(files.read(paths, count_limits),
				  "application-analysis.detached-run-input-limit",
				  "detached_run_files",
				  "count");

	const std::vector duplicates{first_path.string(), first_path.string()};
	require_error(files.read(duplicates),
				  "application-analysis.detached-run-input-invalid",
				  "detached_run_files[1]",
				  "duplicate-path");
	const std::vector relative{std::string{"first.run"}};
	require_error(files.read(relative),
				  "application-analysis.detached-run-input-invalid",
				  "detached_run_files[0]",
				  "absolute-path-required");
	const std::vector non_normal{(directory.path() / "." / "first.run").string()};
	require_error(files.read(non_normal),
				  "application-analysis.detached-run-input-invalid",
				  "detached_run_files[0]",
				  "normalized-path-required");
	const std::vector nul_path{first_path.string() + std::string{"\0ignored", 8U}};
	require_error(files.read(nul_path),
				  "application-analysis.detached-run-input-invalid",
				  "detached_run_files[0]",
				  "path");
	auto path_limits = sdk::import_limits{};
	path_limits.maximum_string_bytes = first_path.string().size() - 1U;
	require_error(files.read(std::span{paths}.last(1U), path_limits),
				  "application-analysis.detached-run-input-limit",
				  "detached_run_files[0]",
				  "path-bytes");

	auto file_limits = sdk::import_limits{};
	file_limits.maximum_bundle_bytes = first.size() - 1U;
	require_error(files.read(std::span{duplicates}.first(1U), file_limits),
				  "application-analysis.detached-run-input-limit",
				  "detached_run_files[0]",
				  "bytes");
	auto total_limits = sdk::import_limits{};
	total_limits.maximum_total_metadata_bytes = first.size() + second.size() - 1U;
	require_error(files.read(paths, total_limits),
				  "application-analysis.detached-run-input-limit",
				  "detached_run_files",
				  "total-bytes");

	const auto empty_path = directory.path() / "empty.run";
	write(empty_path, std::span<const std::byte>{});
	const std::vector empty{empty_path.string()};
	require_error(files.read(empty),
				  "application-analysis.detached-run-input-invalid",
				  "detached_run_files[0]",
				  "empty");
	const std::vector missing{(directory.path() / "missing.run").string()};
	auto missing_result = files.read(missing);
	require_error(missing_result,
				  "application-analysis.detached-run-input-unavailable",
				  "detached_run_files[0]",
				  "open");
	require(!missing_result.error().field.contains(directory.path().string()),
			"source-private detached input path leaked into error field");

	const std::vector non_regular{directory.path().string()};
	require_error(files.read(non_regular),
				  "application-analysis.detached-run-input-invalid",
				  "detached_run_files[0]",
				  "not-regular");
	const auto symlink_path = directory.path() / "linked.run";
	std::error_code symlink_error;
	std::filesystem::create_symlink(first_path, symlink_path, symlink_error);
	require(!symlink_error, "test symlink creation failed");
	const std::vector symlink{symlink_path.string()};
	require_error(files.read(symlink),
				  "application-analysis.detached-run-input-invalid",
				  "detached_run_files[0]",
				  "not-regular");
	return EXIT_SUCCESS;
}
