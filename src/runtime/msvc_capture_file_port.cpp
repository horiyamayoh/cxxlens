#include "msvc_capture_file_port.hpp"

#include <fstream>
#include <iterator>
#include <utility>

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
} // namespace cxxlens::application_analysis_worker
