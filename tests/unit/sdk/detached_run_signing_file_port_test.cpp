#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

#include "runtime/detached_run_signing_file_port_internal.hpp"

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
				("cxxlens detached signing " + std::to_string(std::rand()));
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
		require(static_cast<bool>(output), "test key write failed");
	}

	template <std::size_t Size>
	[[nodiscard]] std::array<std::byte, Size> key(const unsigned char offset)
	{
		std::array<std::byte, Size> output{};
		for (std::size_t index{}; index < output.size(); ++index)
			output[index] = static_cast<std::byte>(offset + static_cast<unsigned char>(index));
		return output;
	}
} // namespace

int main()
{
	using namespace cxxlens;
	using namespace sdk::detail;
	const temporary_directory directory;
	const auto private_path = directory.path() / "private key.raw";
	const auto public_path = directory.path() / "public key.raw";
	const auto private_key = key<detached_run_ed25519_private_key_bytes>(1U);
	const auto public_key = key<detached_run_ed25519_public_key_bytes>(65U);
	write(private_path, private_key);
	write(public_path, public_key);
	const runtime::detached_run_signing_file_port port{
		"worker:clangcl23-test", private_path.string(), public_path.string()};
	const std::string subject = "sha256:" + std::string(64U, 'a');
	auto loaded = port.load("detached-provider-run", subject);
	require(loaded && loaded->scope == "detached-provider-run" &&
				loaded->signer_id == "worker:clangcl23-test" &&
				loaded->private_key == private_key && loaded->public_key == public_key,
			"exact signing key tuple was not loaded");

	require(!port.load("provider-package", subject), "foreign signing scope was accepted");
	require(!port.load("detached-provider-run", "not-a-digest"),
			"invalid signed subject was accepted");
	const runtime::detached_run_signing_file_port invalid_signer{
		"not\na-strong-id", private_path.string(), public_path.string()};
	require(!invalid_signer.load("detached-provider-run", subject),
			"invalid signer identity was accepted");

	auto truncated = private_key;
	write(private_path, std::span{truncated}.first(truncated.size() - 1U));
	auto short_private = port.load("detached-provider-run", subject);
	require(!short_private && short_private.error().field == "private_key_file" &&
				short_private.error().detail == "truncated",
			"truncated private key was not rejected exactly");
	auto trailing = std::array<std::byte, detached_run_ed25519_private_key_bytes + 1U>{};
	write(private_path, trailing);
	auto long_private = port.load("detached-provider-run", subject);
	require(!long_private && long_private.error().detail == "trailing-bytes",
			"private key trailing bytes were accepted");
	write(private_path, private_key);

	write(public_path, std::span{public_key}.first(public_key.size() - 1U));
	auto short_public = port.load("detached-provider-run", subject);
	require(!short_public && short_public.error().field == "public_key_file" &&
				short_public.error().detail == "truncated",
			"truncated public key was not rejected exactly");
	auto public_trailing = std::array<std::byte, detached_run_ed25519_public_key_bytes + 1U>{};
	write(public_path, public_trailing);
	auto long_public = port.load("detached-provider-run", subject);
	require(!long_public && long_public.error().detail == "trailing-bytes",
			"public key trailing bytes were accepted");

	std::error_code ignored;
	std::filesystem::remove(public_path, ignored);
	auto missing_public = port.load("detached-provider-run", subject);
	require(!missing_public &&
				missing_public.error().code ==
					"application-analysis.detached-run-signing-material-unavailable" &&
				missing_public.error().field == "public_key_file",
			"missing public key was not unavailable");
	std::filesystem::create_directory(public_path, ignored);
	auto directory_public = port.load("detached-provider-run", subject);
	require(!directory_public && directory_public.error().field == "public_key_file" &&
				directory_public.error().detail == "not-regular",
			"non-regular public key authority was accepted");
	return EXIT_SUCCESS;
}
