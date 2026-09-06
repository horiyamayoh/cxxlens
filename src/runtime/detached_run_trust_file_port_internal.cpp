#include "detached_run_trust_file_port_internal.hpp"

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

		[[nodiscard]] std::optional<
			std::array<std::byte, sdk::detail::detached_run_ed25519_public_key_bytes>>
		read_public_key(const std::string& path)
		{
			constexpr auto size = sdk::detail::detached_run_ed25519_public_key_bytes;
			if (path.empty() || path.contains('\0'))
				return std::nullopt;
			const auto key_path = native_path(path);
			std::error_code filesystem_error;
			const auto status = std::filesystem::symlink_status(key_path, filesystem_error);
			if (filesystem_error || !std::filesystem::is_regular_file(status))
				return std::nullopt;
			const auto file_size = std::filesystem::file_size(key_path, filesystem_error);
			if (filesystem_error || file_size != size)
				return std::nullopt;
			std::ifstream input{key_path, std::ios::binary};
			if (!input.is_open())
				return std::nullopt;
			std::array<std::byte, size + 1U> bytes{};
			input.read(reinterpret_cast<char*>(bytes.data()),
					   static_cast<std::streamsize>(bytes.size()));
			if (input.bad() || input.gcount() != static_cast<std::streamsize>(size))
				return std::nullopt;
			std::array<std::byte, size> output{};
			std::copy_n(bytes.begin(), size, output.begin());
			return output;
		}

		[[nodiscard]] std::optional<std::string>
		trust_store_digest(const std::string_view scope,
						   const std::string_view signer_id,
						   const std::string_view key_fingerprint,
						   const sdk::detail::detached_run_public_key_state state)
		{
			const auto state_name = state == sdk::detail::detached_run_public_key_state::trusted
				? "trusted"
				: state == sdk::detail::detached_run_public_key_state::revoked ? "revoked"
																			   : "";
			if (std::string_view{state_name}.empty())
				return std::nullopt;
			auto projection = sdk::canonical_binary(sdk::canonical_value::from_tuple(
				{sdk::canonical_value::from_string(std::string{scope}),
				 sdk::canonical_value::from_string(std::string{signer_id}),
				 sdk::canonical_value::from_string(std::string{key_fingerprint}),
				 sdk::canonical_value::from_string(state_name)}));
			if (!projection)
				return std::nullopt;
			return sdk::content_digest(*projection);
		}
	} // namespace

	detached_run_trust_file_port::detached_run_trust_file_port(
		std::string signer_id,
		std::string public_key_path,
		const sdk::detail::detached_run_public_key_state state)
		: signer_id_{std::move(signer_id)}, public_key_path_{std::move(public_key_path)},
		  state_{state}
	{
	}

	std::optional<sdk::detail::trusted_detached_run_ed25519_public_key>
	detached_run_trust_file_port::lookup(const std::string_view scope,
										 const std::string_view signer_id,
										 const std::string_view key_fingerprint) const
	{
		if (scope != detached_run_scope || signer_id != signer_id_ ||
			!digest_like(key_fingerprint) || !sdk::validate_strong_id(signer_id_))
			return std::nullopt;
		auto public_key = read_public_key(public_key_path_);
		if (!public_key || sdk::content_digest(*public_key) != key_fingerprint)
			return std::nullopt;
		auto trust_digest = trust_store_digest(scope, signer_id, key_fingerprint, state_);
		if (!trust_digest)
			return std::nullopt;
		return sdk::detail::trusted_detached_run_ed25519_public_key{state_,
																	std::string{scope},
																	std::string{signer_id},
																	std::string{key_fingerprint},
																	std::move(*public_key),
																	std::move(*trust_digest)};
	}
} // namespace cxxlens::runtime
