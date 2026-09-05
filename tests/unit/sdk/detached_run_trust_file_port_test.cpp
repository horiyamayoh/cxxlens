#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

#include "runtime/detached_run_trust_file_port_internal.hpp"

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
				("cxxlens detached trust " + std::to_string(std::rand()));
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
	const auto public_path = directory.path() / "trusted public key.raw";
	const auto public_key = key<detached_run_ed25519_public_key_bytes>(31U);
	write(public_path, public_key);
	const auto fingerprint = sdk::content_digest(public_key);
	const runtime::detached_run_trust_file_port trusted{"worker:clangcl23-production",
														public_path.string(),
														detached_run_public_key_state::trusted};
	auto loaded =
		trusted.lookup("detached-provider-run", "worker:clangcl23-production", fingerprint);
	require(loaded && loaded->state == detached_run_public_key_state::trusted &&
				loaded->scope == "detached-provider-run" &&
				loaded->signer_id == "worker:clangcl23-production" &&
				loaded->key_fingerprint == fingerprint && loaded->public_key == public_key &&
				loaded->trust_store_digest.starts_with("sha256:") &&
				loaded->trust_store_digest.size() == 71U,
			"exact trusted key tuple was not loaded");
	auto repeat =
		trusted.lookup("detached-provider-run", "worker:clangcl23-production", fingerprint);
	require(repeat && repeat->trust_store_digest == loaded->trust_store_digest,
			"trust store identity was not deterministic");

	const runtime::detached_run_trust_file_port revoked{"worker:clangcl23-production",
														public_path.string(),
														detached_run_public_key_state::revoked};
	auto revoked_key =
		revoked.lookup("detached-provider-run", "worker:clangcl23-production", fingerprint);
	require(revoked_key && revoked_key->state == detached_run_public_key_state::revoked &&
				revoked_key->trust_store_digest != loaded->trust_store_digest,
			"revocation authority was not bound into trust identity");
	const runtime::detached_run_trust_file_port invalid_state{
		"worker:clangcl23-production",
		public_path.string(),
		static_cast<detached_run_public_key_state>(255U)};
	require(
		!invalid_state.lookup("detached-provider-run", "worker:clangcl23-production", fingerprint),
		"unknown revocation state was accepted");

	require(!trusted.lookup("provider-package", "worker:clangcl23-production", fingerprint),
			"foreign trust scope was accepted");
	require(!trusted.lookup("detached-provider-run", "worker:other", fingerprint),
			"foreign signer was accepted");
	require(!trusted.lookup("detached-provider-run",
							"worker:clangcl23-production",
							"sha256:" + std::string(64U, '0')),
			"foreign key fingerprint was accepted");

	write(public_path, std::span{public_key}.first(public_key.size() - 1U));
	require(!trusted.lookup("detached-provider-run", "worker:clangcl23-production", fingerprint),
			"truncated public key was accepted");
	auto trailing = std::array<std::byte, detached_run_ed25519_public_key_bytes + 1U>{};
	write(public_path, trailing);
	require(!trusted.lookup("detached-provider-run", "worker:clangcl23-production", fingerprint),
			"public key trailing bytes were accepted");

	std::error_code ignored;
	std::filesystem::remove(public_path, ignored);
	const auto symlink_target = directory.path() / "symlink target.raw";
	write(symlink_target, public_key);
	std::filesystem::create_symlink(symlink_target, public_path, ignored);
	require(
		!ignored &&
			!trusted.lookup("detached-provider-run", "worker:clangcl23-production", fingerprint),
		"symlink public key authority was accepted");
	std::filesystem::remove(public_path, ignored);
	require(!trusted.lookup("detached-provider-run", "worker:clangcl23-production", fingerprint),
			"missing public key remained trusted");
	std::filesystem::create_directory(public_path, ignored);
	require(!trusted.lookup("detached-provider-run", "worker:clangcl23-production", fingerprint),
			"non-regular public key authority was accepted");
	return EXIT_SUCCESS;
}
