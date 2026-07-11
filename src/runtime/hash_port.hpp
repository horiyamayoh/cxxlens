#pragma once

#include <compare>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "runtime_support.hpp"

namespace cxxlens::detail::runtime
{
	class filesystem_port;

	struct hash_request
	{
		std::string algorithm;
		std::uint32_t version{};
		std::string domain;
		std::span<const std::byte> payload;
	};

	struct digest
	{
		std::string algorithm;
		std::uint32_t version{};
		std::string domain;
		std::string hexadecimal;

		auto operator<=>(const digest&) const = default;
	};

	class hash_port
	{
	  public:
		virtual ~hash_port() = default;
		[[nodiscard]] virtual runtime_result<digest>
		calculate(const hash_request& request, const request_context& context) const = 0;
	};

	class fnv1a_hash_adapter final : public hash_port
	{
	  public:
		[[nodiscard]] runtime_result<digest>
		calculate(const hash_request& request, const request_context& context) const override;
	};

	[[nodiscard]] hash_request make_hash_request(std::string domain, std::string_view payload);
	[[nodiscard]] runtime_result<digest> digest_file(const filesystem_port& files,
													 const hash_port& hashes,
													 const std::filesystem::path& path,
													 std::string domain,
													 const request_context& context);

} // namespace cxxlens::detail::runtime
