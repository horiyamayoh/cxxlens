#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

#if defined(__linux__) && defined(__GLIBC__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "runtime/sealed_executable_internal.hpp"

namespace
{
	using cxxlens::sdk::detail::canonical_open_descriptor_path;
	using cxxlens::sdk::detail::open_sealed_executable;
	using cxxlens::sdk::detail::sealed_executable_request;

	void require(const bool condition, const std::string& message)
	{
		if (!condition)
			throw std::runtime_error{message};
	}

	[[nodiscard]] bool canonical_sha256(const std::string& value)
	{
		if (value.size() != 71U || !value.starts_with("sha256:"))
			return false;
		for (const auto byte : value.substr(7U))
			if ((byte < '0' || byte > '9') && (byte < 'a' || byte > 'f'))
				return false;
		return true;
	}
} // namespace

int main()
{
#if defined(__linux__) && defined(__GLIBC__)
	namespace fs = std::filesystem;
	const auto root =
		fs::temp_directory_path() / ("cxxlens-sealed-executable-" + std::to_string(::getpid()));
	try
	{
		fs::create_directories(root / "bin");
		const auto executable = root / "bin" / "compiler";
		const auto alias = root / "compiler-link";
		fs::copy_file("/bin/true", executable, fs::copy_options::overwrite_existing);
		fs::create_symlink(executable, alias);

		const auto working_directory = (root / "bin").string();
		sealed_executable_request request;
		request.executable_path = "./compiler";
		request.working_directory = working_directory;
		request.maximum_image_bytes = 128U * 1024U * 1024U;
		request.maximum_canonical_path_bytes = 4096U;
		auto first = open_sealed_executable(request);
		auto second = open_sealed_executable(request);
		require(first && second, "explicit relative executable could not be sealed");
		require(canonical_sha256(first->digest()) && first->digest() == second->digest(),
				"sealed executable digest was invalid or nondeterministic");
		require(first->canonical_source_path() == fs::canonical(executable).string(),
				"canonical path did not come from the opened executable descriptor");
		require(first->byte_count() == fs::file_size(executable),
				"sealed executable byte count did not bind the complete image");
		auto fragmented_request = request;
		fragmented_request.read_chunk_bytes = 1U;
		auto fragmented = open_sealed_executable(fragmented_request);
		require(fragmented && fragmented->digest() == first->digest() &&
					fragmented->byte_count() == first->byte_count(),
				"fragmented executable reads changed measured identity");
		// Independent SHA256 reference values cover both padding boundaries,
		// multiple compression blocks and fragmented binary image reads.
		const std::array<std::pair<std::string, std::string_view>, 7U> vectors{
			{{std::string{},
			  "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
			 {std::string{"abc"},
			  "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
			 {std::string(55U, 'a'),
			  "sha256:9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
			 {std::string(56U, 'a'),
			  "sha256:b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
			 {std::string(64U, 'a'),
			  "sha256:ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
			 {std::string(65U, 'a'),
			  "sha256:635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"},
			 {std::string(1000000U, 'a'),
			  "sha256:cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"}}};
		const auto vector_path = root / "bin" / "hash-vector";
		const auto vector_name = vector_path.string();
		for (const auto& [contents, expected] : vectors)
		{
			{
				std::ofstream stream{vector_path, std::ios::binary | std::ios::trunc};
				stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
				require(static_cast<bool>(stream), "could not write SHA256 reference image");
			}
			fs::permissions(vector_path,
							fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
							fs::perm_options::replace);
			for (const std::size_t chunk : {1U, 55U, 64U, 65U, 65536U})
			{
				if (contents.size() > 65536U && chunk < 64U)
					continue;
				sealed_executable_request vector_request;
				vector_request.executable_path = vector_name;
				vector_request.maximum_image_bytes = 1000000U;
				vector_request.read_chunk_bytes = chunk;
				auto measured = open_sealed_executable(vector_request);
				require(measured && measured->digest() == expected &&
							measured->byte_count() == contents.size(),
						"sealed executable SHA256 did not match the independent reference");
			}
		}

		const auto seals = ::fcntl(first->native_handle(), F_GET_SEALS);
		const auto required_seals = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
		require(seals >= 0 && (seals & required_seals) == required_seals,
				"executable image was not irreversibly sealed");
		require(::write(first->native_handle(), "x", 1U) < 0,
				"sealed executable image remained writable");
		auto short_path = canonical_open_descriptor_path({first->native_handle(), 1U});
		require(!short_path && short_path.error().code == "runtime.descriptor-path-limit",
				"opened-descriptor path limit was not typed");
		auto invalid_descriptor = canonical_open_descriptor_path({-1, 4096U});
		require(!invalid_descriptor &&
					invalid_descriptor.error().code == "runtime.descriptor-path-failed",
				"invalid opened descriptor did not fail closed");

		const auto alias_path = alias.string();
		sealed_executable_request alias_request;
		alias_request.executable_path = alias_path;
		alias_request.maximum_canonical_path_bytes = 4096U;
		auto through_alias = open_sealed_executable(alias_request);
		require(through_alias &&
					through_alias->canonical_source_path() == fs::canonical(executable).string() &&
					through_alias->digest() == first->digest(),
				"symlink spelling changed opened-file identity");

		auto limited_request = request;
		limited_request.maximum_image_bytes = 1U;
		auto limited = open_sealed_executable(limited_request);
		require(!limited && limited.error().field == "executable-size",
				"executable image limit was not enforced before copying");
		auto zero_chunk_request = request;
		zero_chunk_request.read_chunk_bytes = 0U;
		auto zero_chunk = open_sealed_executable(zero_chunk_request);
		require(!zero_chunk && zero_chunk.error().field == "request" &&
					zero_chunk.error().detail == "invalid-read-chunk",
				"zero executable read chunk was accepted");

		auto expired_request = request;
		expired_request.absolute_wall_deadline_ns = 0U;
		auto expired = open_sealed_executable(expired_request);
		require(!expired && expired.error().code == "runtime.sealed-executable-timeout",
				"expired absolute deadline did not fail before opening the image");
		std::stop_source stopped;
		stopped.request_stop();
		auto cancelled_request = request;
		cancelled_request.cancellation = stopped.get_token();
		auto cancelled = open_sealed_executable(cancelled_request);
		require(!cancelled && cancelled.error().code == "runtime.sealed-executable-cancelled",
				"cancelled executable measurement did not stop before opening the image");

		const auto non_executable = root / "not-executable";
		fs::copy_file("/bin/true", non_executable, fs::copy_options::overwrite_existing);
		fs::permissions(non_executable, fs::perms::owner_read, fs::perm_options::replace);
		const auto non_executable_path = non_executable.string();
		sealed_executable_request wrong_type_request;
		wrong_type_request.executable_path = non_executable_path;
		auto wrong_type = open_sealed_executable(wrong_type_request);
		require(!wrong_type && wrong_type.error().field == "executable-type",
				"non-executable regular file was accepted");
		const auto directory_path = (root / "bin").string();
		sealed_executable_request directory_request;
		directory_request.executable_path = directory_path;
		auto directory = open_sealed_executable(directory_request);
		require(!directory && directory.error().field == "executable-type",
				"directory was accepted as an executable image");
		const auto missing_path = (root / "missing").string();
		sealed_executable_request missing_request;
		missing_request.executable_path = missing_path;
		auto missing = open_sealed_executable(missing_request);
		require(!missing && missing.error().field == "executable-open",
				"missing executable did not return a typed open failure");

		fs::remove_all(root);
		std::cout << "sealed executable authority tests passed\n";
		return EXIT_SUCCESS;
	}
	catch (const std::exception& exception)
	{
		fs::remove_all(root);
		std::cerr << exception.what() << '\n';
		return EXIT_FAILURE;
	}
#else
	sealed_executable_request request;
	request.executable_path = "/unsupported";
	auto unavailable = open_sealed_executable(request);
	require(!unavailable && unavailable.error().detail == "unsupported",
			"unsupported platform did not fail closed");
	return EXIT_SUCCESS;
#endif
}
