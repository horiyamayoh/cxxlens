#include <array>
#include <cstddef>
#include <iomanip>
#include <sstream>

#include "filesystem_port.hpp"
#include "hash_port.hpp"

namespace cxxlens::detail::runtime
{
	namespace
	{
		constexpr std::uint64_t offset_basis = 14695981039346656037ULL;
		constexpr std::uint64_t prime = 1099511628211ULL;

		void append(std::uint64_t& state, const std::span<const std::byte> bytes) noexcept
		{
			for (const auto byte : bytes)
			{
				state ^= std::to_integer<std::uint8_t>(byte);
				state *= prime;
			}
		}

		void append(std::uint64_t& state, const std::string_view text) noexcept
		{
			append(state, std::as_bytes(std::span{text.data(), text.size()}));
		}

		void separator(std::uint64_t& state) noexcept
		{
			constexpr std::array marker{std::byte{0x00}, std::byte{0xFF}};
			append(state, marker);
		}
	} // namespace

	runtime_result<digest> fnv1a_hash_adapter::calculate(const hash_request& request,
														 const request_context& context) const
	{
		if (context.cancelled())
		{
			return unexpected{runtime_failure{runtime_status::cancelled, context.operation, 0}};
		}
		if (request.algorithm != "fnv1a64" || request.version != 1U || request.domain.empty())
		{
			return unexpected{
				runtime_failure{runtime_status::invalid_request, context.operation, 0}};
		}
		auto state = offset_basis;
		append(state, request.algorithm);
		separator(state);
		append(state, std::to_string(request.version));
		separator(state);
		append(state, request.domain);
		separator(state);
		append(state, request.payload);
		std::ostringstream output;
		output << std::hex << std::setfill('0') << std::setw(16) << state;
		return digest{request.algorithm, request.version, request.domain, output.str()};
	}

	hash_request make_hash_request(std::string domain, const std::string_view payload)
	{
		return hash_request{"fnv1a64",
							1U,
							std::move(domain),
							std::as_bytes(std::span{payload.data(), payload.size()})};
	}

	runtime_result<digest> digest_file(const filesystem_port& files,
									   const hash_port& hashes,
									   const std::filesystem::path& path,
									   std::string domain,
									   const request_context& context)
	{
		auto content = files.read(path, context);
		if (!content)
		{
			return unexpected{content.error()};
		}
		return hashes.calculate(make_hash_request(std::move(domain), *content), context);
	}

} // namespace cxxlens::detail::runtime
