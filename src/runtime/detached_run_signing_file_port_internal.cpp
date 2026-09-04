#include "detached_run_signing_file_port_internal.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <utility>

namespace cxxlens::runtime
{
	namespace
	{
		constexpr std::string_view detached_run_scope{"detached-provider-run"};

		[[nodiscard]] sdk::error unavailable(std::string field, std::string detail)
		{
			return {"application-analysis.detached-run-signing-material-unavailable",
					std::move(field),
					std::move(detail)};
		}

		[[nodiscard]] sdk::error invalid(std::string field, std::string detail)
		{
			return {"application-analysis.detached-run-signing-material-invalid",
					std::move(field),
					std::move(detail)};
		}

		[[nodiscard]] bool digest_like(const std::string_view value) noexcept
		{
			return value.starts_with("sha256:") && value.size() == 71U &&
				std::ranges::all_of(value.substr(7U),
									[](const char byte)
									{
										return (byte >= '0' && byte <= '9') ||
											(byte >= 'a' && byte <= 'f');
									});
		}

		[[nodiscard]] std::filesystem::path native_path(const std::string& value)
		{
#if defined(_WIN32)
			std::u8string utf8;
			utf8.reserve(value.size());
			for (const auto byte : value)
				utf8.push_back(static_cast<char8_t>(static_cast<unsigned char>(byte)));
			return std::filesystem::path{utf8};
#else
			return std::filesystem::path{value};
#endif
		}

		template <std::size_t Size>
		[[nodiscard]] sdk::result<std::array<std::byte, Size>>
		read_exact_key(const std::string& path, const std::string_view field)
		{
			if (path.empty() || path.contains('\0'))
				return sdk::unexpected(invalid(std::string{field}, "path"));
			const auto key_path = native_path(path);
			std::error_code filesystem_error;
			const auto status = std::filesystem::symlink_status(key_path, filesystem_error);
			if (filesystem_error || status.type() == std::filesystem::file_type::not_found)
				return sdk::unexpected(unavailable(std::string{field}, "stat"));
			if (!std::filesystem::is_regular_file(status))
				return sdk::unexpected(invalid(std::string{field}, "not-regular"));
			const auto size = std::filesystem::file_size(key_path, filesystem_error);
			if (filesystem_error)
				return sdk::unexpected(unavailable(std::string{field}, "size"));
			if (size < Size)
				return sdk::unexpected(invalid(std::string{field}, "truncated"));
			if (size > Size)
				return sdk::unexpected(invalid(std::string{field}, "trailing-bytes"));
			std::ifstream input{key_path, std::ios::binary};
			if (!input.is_open())
				return sdk::unexpected(unavailable(std::string{field}, "open"));
			std::array<std::byte, Size + 1U> bytes{};
			input.read(reinterpret_cast<char*>(bytes.data()),
					   static_cast<std::streamsize>(bytes.size()));
			const auto count = input.gcount();
			if (input.bad())
				return sdk::unexpected(unavailable(std::string{field}, "read"));
			if (count < static_cast<std::streamsize>(Size))
				return sdk::unexpected(invalid(std::string{field}, "truncated"));
			if (count > static_cast<std::streamsize>(Size))
				return sdk::unexpected(invalid(std::string{field}, "trailing-bytes"));
			std::array<std::byte, Size> output{};
			std::copy_n(bytes.begin(), Size, output.begin());
			return output;
		}
	} // namespace

	detached_run_signing_file_port::detached_run_signing_file_port(std::string signer_id,
																   std::string private_key_path,
																   std::string public_key_path)
		: signer_id_{std::move(signer_id)}, private_key_path_{std::move(private_key_path)},
		  public_key_path_{std::move(public_key_path)}
	{
	}

	sdk::result<sdk::detail::detached_run_ed25519_signing_material>
	detached_run_signing_file_port::load(const std::string_view scope,
										 const std::string_view signed_subject_digest) const
	{
		if (scope != detached_run_scope || !digest_like(signed_subject_digest))
			return sdk::unexpected(invalid("request", "authority"));
		if (!sdk::validate_strong_id(signer_id_))
			return sdk::unexpected(invalid("signer_id", "strong-id"));
		auto public_key = read_exact_key<sdk::detail::detached_run_ed25519_public_key_bytes>(
			public_key_path_, "public_key_file");
		if (!public_key)
			return sdk::unexpected(std::move(public_key.error()));
		auto private_key = read_exact_key<sdk::detail::detached_run_ed25519_private_key_bytes>(
			private_key_path_, "private_key_file");
		if (!private_key)
			return sdk::unexpected(std::move(private_key.error()));
		return sdk::detail::detached_run_ed25519_signing_material{
			std::string{scope}, signer_id_, std::move(*private_key), std::move(*public_key)};
	}
} // namespace cxxlens::runtime
