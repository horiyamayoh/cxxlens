#include "worker_ingress.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <ios>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "worker_parser.hpp"

namespace cxxlens::detail::clang23_gcc_replay
{
	namespace
	{
		[[nodiscard]] sdk::error failure(std::string field, std::string detail)
		{
			return {"application-analysis.replay-worker-ingress-failed",
					std::move(field),
					std::move(detail)};
		}

		[[nodiscard]] sdk::error limit(std::string field, std::string detail)
		{
			return {
				"application-analysis.import-limit-exceeded", std::move(field), std::move(detail)};
		}
	} // namespace

	sdk::result<void> validate_worker_ingress(std::istream& input,
											  std::ostream& output,
											  const sdk::import_limits limits)
	{
		try
		{
			if (auto valid = limits.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));

			std::vector<std::byte> encoded;
			encoded.reserve(std::min<std::size_t>(limits.maximum_bundle_bytes, 8192U));
			std::array<char, 8192U> buffer{};
			while (input)
			{
				input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
				const auto count = input.gcount();
				if (count < 0)
					return sdk::unexpected(failure("stdin", "negative-read-count"));
				const auto bytes = static_cast<std::size_t>(count);
				if (bytes > limits.maximum_bundle_bytes - encoded.size())
					return sdk::unexpected(limit("stdin", "input-bytes"));
				for (std::size_t index{}; index < bytes; ++index)
					encoded.push_back(
						static_cast<std::byte>(static_cast<unsigned char>(buffer[index])));
			}
			if (!input.eof())
				return sdk::unexpected(failure("stdin", "read-failed"));
			if (encoded.empty())
				return sdk::unexpected(failure("stdin", "empty"));

			auto decoded = sdk::detail::decode_gcc_replay_input(encoded, limits);
			if (!decoded)
				return sdk::unexpected(std::move(decoded.error()));
			auto parsed = parse_replay_input(*decoded);
			if (!parsed)
				return sdk::unexpected(std::move(parsed.error()));
			if (parsed->terminal != parse_terminal::parsed)
				return sdk::unexpected(failure("translation_unit", "syntax-rejected"));
			output << decoded->input_digest() << '\n';
			if (!output)
				return sdk::unexpected(failure("stdout", "write-failed"));
			return {};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(limit("memory", "allocation"));
		}
		catch (const std::length_error&)
		{
			return sdk::unexpected(limit("memory", "length"));
		}
		catch (const std::ios_base::failure&)
		{
			return sdk::unexpected(failure("stream", "io-exception"));
		}
	}
} // namespace cxxlens::detail::clang23_gcc_replay
