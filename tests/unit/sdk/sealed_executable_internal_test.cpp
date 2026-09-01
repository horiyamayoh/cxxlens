#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

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

		auto expired_request = request;
		expired_request.absolute_wall_deadline_ns = 0U;
		auto expired = open_sealed_executable(expired_request);
		require(!expired && expired.error().code == "runtime.sealed-executable-timeout",
				"expired absolute deadline did not fail before opening the image");

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
