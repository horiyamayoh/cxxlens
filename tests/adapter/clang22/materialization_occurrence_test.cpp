#include "llvm/clang22/materialization_occurrence.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "llvm/clang22/materialization_identity.hpp"
#include "llvm/clang22/materialization_json.hpp"

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::detail::clang22::materialization;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] json_value string_value(std::string value)
	{
		auto output = json_value::string(std::move(value));
		require(output.has_value(), "test JSON string construction failed");
		return std::move(*output);
	}

	[[nodiscard]] json_value object_value(json_value::object_type value)
	{
		auto output = json_value::object(std::move(value));
		require(output.has_value(), "test JSON object construction failed");
		return std::move(*output);
	}

	constexpr std::array<std::string_view, 13U> roles{
		"materializer-executable",
		"worker-executable",
		"relation-registry",
		"project-catalog-contract",
		"portable-provider-task-contract",
		"provider-protocol",
		"provider-runtime-contract",
		"snapshot-store-contract",
		"sqlite-store-contract",
		"materialization-contract",
		"materialization-contract-schema",
		"materialization-request-schema",
		"materialization-report-schema",
	};
	constexpr std::array<std::string_view, 13U> paths{
		"bin/cxxlens-clang22-materialize",
		"bin/cxxlens-clang-worker-22",
		"share/cxxlens/schemas/cxxlens_ng_relation_registry.yaml",
		"share/cxxlens/schemas/cxxlens_ng_project_catalog_contract.yaml",
		"share/cxxlens/schemas/cxxlens_ng_portable_provider_task_contract.yaml",
		"share/cxxlens/schemas/cxxlens_ng_provider_protocol.yaml",
		"share/cxxlens/schemas/cxxlens_ng_provider_runtime_contract.yaml",
		"share/cxxlens/schemas/cxxlens_ng_snapshot_store_contract.yaml",
		"share/cxxlens/schemas/cxxlens_ng_sqlite_store_contract.yaml",
		"share/cxxlens/schemas/cxxlens_ng_clang22_materialization_contract.yaml",
		"share/cxxlens/schemas/cxxlens_ng_clang22_materialization_contract.schema.yaml",
		"share/cxxlens/schemas/cxxlens_ng_clang22_materialization_request.schema.yaml",
		"share/cxxlens/schemas/cxxlens_ng_clang22_materialization_report.schema.yaml",
	};
	constexpr std::string_view child_mode_name{"CXXLENS_OCCURRENCE_REPLACEMENT_CHILD"};
	constexpr std::string_view child_prefix_name{"CXXLENS_OCCURRENCE_REPLACEMENT_PREFIX"};
	constexpr std::string_view child_manifest_digest_name{
		"CXXLENS_OCCURRENCE_REPLACEMENT_MANIFEST_DIGEST"};
	constexpr std::string_view child_materializer_digest_name{
		"CXXLENS_OCCURRENCE_REPLACEMENT_MATERIALIZER_DIGEST"};
	constexpr std::string_view child_worker_digest_name{
		"CXXLENS_OCCURRENCE_REPLACEMENT_WORKER_DIGEST"};

	[[nodiscard]] json_value::array_type occurrence_files(const bool shared)
	{
		json_value::array_type files;
		for (std::size_t index{}; index < roles.size(); ++index)
			files.push_back(object_value({
				{"role", string_value(std::string{roles[index]})},
				{"path", string_value(std::string{paths[index]})},
				{"digest",
				 string_value("sha256:" + std::string(64U, static_cast<char>('a' + index % 6U)))},
			}));
		if (shared)
		{
			constexpr std::array<std::string_view, 6U> shared_roles{
				"base", "kernel", "query", "recipes", "provider-sdk", "clang22-provider-sdk"};
			constexpr std::array<std::string_view, 6U> shared_paths{
				"lib/libcxxlens_base.so.1",
				"lib64/libcxxlens_kernel.so.1.0",
				"lib/libcxxlens_query.so",
				"lib/libcxxlens_recipes.so.1",
				"lib64/libcxxlens_provider_sdk.so.1",
				"lib/libcxxlens_clang22_provider_sdk.so.1",
			};
			for (std::size_t index{}; index < shared_roles.size(); ++index)
				files.push_back(object_value({
					{"role", string_value(std::string{shared_roles[index]})},
					{"path", string_value(std::string{shared_paths[index]})},
					{"digest", string_value("sha256:" + std::string(64U, 'f'))},
				}));
		}
		return files;
	}

	[[nodiscard]] std::vector<std::byte> manifest_bytes(const bool shared,
														json_value::array_type files,
														const bool corrupt_payload = false)
	{
		json_value::object_type payload{
			{"schema", string_value("cxxlens.clang22-materializer-occurrence-manifest.v1")},
			{"manifest_version", string_value("1.0.0")},
			{"source_revision", string_value(std::string(40U, '1'))},
			{"source_tree", string_value(std::string(40U, '2'))},
			{"package_configuration", string_value(shared ? "shared" : "static")},
			{"files", json_value::array(std::move(files))},
		};
		const auto canonical_payload = canonical_json(object_value(payload));
		auto digest = sdk::content_digest(std::as_bytes(std::span{canonical_payload}));
		if (corrupt_payload)
			digest.back() = digest.back() == '0' ? '1' : '0';
		payload.emplace("occurrence_payload_digest", string_value(std::move(digest)));
		const auto bytes = canonical_json(object_value(std::move(payload)));
		const auto view = std::as_bytes(std::span{bytes});
		return {view.begin(), view.end()};
	}

	[[nodiscard]] std::vector<std::byte> bytes(const std::string_view value)
	{
		const auto view = std::as_bytes(std::span{value});
		return {view.begin(), view.end()};
	}

	void write_file(const std::filesystem::path& path,
					const std::span<const std::byte> content,
					const mode_t mode = 0600)
	{
		std::error_code filesystem_error;
		std::filesystem::create_directories(path.parent_path(), filesystem_error);
		require(!filesystem_error, "test occurrence parent directory creation failed");
		const auto descriptor =
			::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
		require(descriptor >= 0, "test occurrence file open failed");
		materialization_owned_fd output{descriptor};
		std::size_t offset{};
		while (offset < content.size())
		{
			const auto count =
				::write(output.get(), content.data() + offset, content.size() - offset);
			if (count < 0 && errno == EINTR)
				continue;
			require(count > 0, "test occurrence file write failed");
			offset += static_cast<std::size_t>(count);
		}
	}

	[[nodiscard]] std::vector<std::byte> read_file(const int descriptor)
	{
		auto identity = materialization_fd_identity(descriptor, true);
		require(identity && identity->size_bytes <= std::numeric_limits<std::size_t>::max(),
				"test occurrence file identity failed");
		std::vector<std::byte> output(static_cast<std::size_t>(identity->size_bytes));
		std::size_t offset{};
		while (offset < output.size())
		{
			const auto count = ::pread(descriptor,
									   output.data() + offset,
									   output.size() - offset,
									   static_cast<off_t>(offset));
			if (count < 0 && errno == EINTR)
				continue;
			require(count > 0, "test occurrence file read failed");
			offset += static_cast<std::size_t>(count);
		}
		return output;
	}

	[[nodiscard]] std::vector<std::byte> read_file(const std::filesystem::path& path)
	{
		const auto descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
		require(descriptor >= 0, "test occurrence path open failed");
		materialization_owned_fd input{descriptor};
		return read_file(input.get());
	}

	[[nodiscard]] std::string required_environment(const std::string_view name)
	{
		const auto* value = ::getenv(std::string{name}.c_str());
		require(value != nullptr && *value != '\0', "test occurrence child environment missing");
		return value;
	}

	[[nodiscard]] json_value::array_type
	installed_occurrence_files(const std::span<const std::string> digests)
	{
		require(digests.size() == roles.size(), "test occurrence digest census is incomplete");
		json_value::array_type files;
		for (std::size_t index{}; index < roles.size(); ++index)
			files.push_back(object_value({
				{"role", string_value(std::string{roles[index]})},
				{"path", string_value(std::string{paths[index]})},
				{"digest", string_value(digests[index])},
			}));
		return files;
	}

	void replacement_after_measurement_child()
	{
		const auto prefix = std::filesystem::path{required_environment(child_prefix_name)};
		const auto worker_digest = required_environment(child_worker_digest_name);
		const auto worker_path = prefix / std::string{paths[1U]};
		materialization_occurrence_expectation expected{
			.source_revision = std::string(40U, '1'),
			.source_tree = std::string(40U, '2'),
			.package_configuration = "static",
			.occurrence_manifest_digest = required_environment(child_manifest_digest_name),
			.materializer_executable_digest = required_environment(child_materializer_digest_name),
			.worker_executable_digest = worker_digest,
		};
		auto measured = measure_materialization_occurrence(expected);
		require(measured.has_value(), "installed occurrence measurement failed before overwrite");
		auto original_worker_bytes = bytes("authority:worker-executable\n");
		auto original_authority_bytes = bytes("authority:relation-registry\n");
		auto overwritten_authority_bytes = original_authority_bytes;
		std::ranges::fill(overwritten_authority_bytes, std::byte{'y'});
		const auto authority_path = prefix / std::string{paths[2U]};
		write_file(authority_path, overwritten_authority_bytes);
		require(sdk::content_digest(read_file(authority_path)) !=
					measured->manifest().files[2U].digest,
				"unopened authority mutation did not change installed bytes");

		std::set<std::pair<std::uint64_t, std::uint64_t>> snapshot_objects;
		for (const auto& file : measured->manifest().files)
		{
			auto opened = measured->open_role(file.role);
			require(opened && sdk::content_digest(read_file(opened->get())) == file.digest,
					"measured role did not map to its exact sealed authority bytes");
			const auto flags = ::fcntl(opened->get(), F_GETFL);
			require(flags >= 0 && (flags & O_ACCMODE) == O_RDONLY,
					"measured role handle was not independently read-only");
			auto identity = materialization_fd_identity(opened->get(), true);
			require(identity && snapshot_objects.emplace(identity->device, identity->inode).second,
					"two occurrence roles shared one sealed snapshot object");
		}
		require(snapshot_objects.size() == measured->manifest().files.size(),
				"sealed occurrence snapshot census is incomplete");

		auto overwritten_worker_bytes = original_worker_bytes;
		std::ranges::fill(overwritten_worker_bytes, std::byte{'x'});
		require(overwritten_worker_bytes.size() == original_worker_bytes.size(),
				"in-place replacement changed the test file size");
		auto immutable_worker = measured->open_role("worker-executable");
		require(immutable_worker.has_value(), "measured worker immutable snapshot creation failed");
		write_file(worker_path, overwritten_worker_bytes, 0700);
		require(sdk::content_digest(read_file(worker_path)) != worker_digest,
				"in-place overwrite did not change worker content");
		require(sdk::content_digest(read_file(immutable_worker->get())) == worker_digest,
				"post-validation source mutation changed the returned immutable snapshot");
#if defined(F_GET_SEALS) && defined(F_SEAL_WRITE) && defined(F_SEAL_GROW) && \
	defined(F_SEAL_SHRINK) && defined(F_SEAL_SEAL)
		constexpr int required_seals = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
		require((::fcntl(immutable_worker->get(), F_GET_SEALS) & required_seals) == required_seals,
				"returned occurrence snapshot is not irreversibly sealed");
#endif
		errno = 0;
		const std::byte attempted_write{'z'};
		require(::pwrite(immutable_worker->get(), &attempted_write, 1U, 0) < 0 &&
					(errno == EBADF || errno == EPERM),
				"returned occurrence snapshot remained writable after verification");
		auto overwritten = measured->open_role("worker-executable");
		require(overwritten && sdk::content_digest(read_file(overwritten->get())) == worker_digest,
				"post-measurement open did not return the already sealed authority bytes");
		auto first_snapshot_identity = materialization_fd_identity(immutable_worker->get(), true);
		auto second_snapshot_identity = materialization_fd_identity(overwritten->get(), true);
		require(first_snapshot_identity && second_snapshot_identity &&
					first_snapshot_identity->device == second_snapshot_identity->device &&
					first_snapshot_identity->inode == second_snapshot_identity->inode,
				"repeated role open allocated another immutable snapshot object");
		std::byte first_byte{};
		std::byte second_byte{};
		require(::read(immutable_worker->get(), &first_byte, 1U) == 1 &&
					::read(overwritten->get(), &second_byte, 1U) == 1 &&
					first_byte == original_worker_bytes.front() &&
					second_byte == original_worker_bytes.front(),
				"role snapshot handles did not have independent read offsets");

		write_file(worker_path, original_worker_bytes, 0700);
		write_file(authority_path, original_authority_bytes);
		require(sdk::content_digest(read_file(worker_path)) == worker_digest,
				"worker restoration before rename test failed");
		require(sdk::content_digest(read_file(authority_path)) ==
					measured->manifest().files[2U].digest,
				"authority restoration before rename test failed");
		auto rename_measured = measure_materialization_occurrence(expected);
		require(rename_measured.has_value(),
				"installed occurrence remeasurement failed before rename");
		const auto measured_worker = std::ranges::find(rename_measured->receipt().files,
													   "worker-executable",
													   [](const auto& file)
													   {
														   return file.authority.role;
													   });
		require(measured_worker != rename_measured->receipt().files.end(),
				"remeasured worker receipt is missing");
		const auto measured_identity = measured_worker->identity;

		const auto replacement_bytes = bytes("replacement-worker-bytes\n");
		const auto replacement_path = prefix / "bin/cxxlens-clang-worker-22.replacement";
		write_file(replacement_path, replacement_bytes, 0700);
		std::error_code filesystem_error;
		std::filesystem::rename(replacement_path, worker_path, filesystem_error);
		require(!filesystem_error, "measured worker replacement failed");
		require(sdk::content_digest(read_file(worker_path)) != worker_digest,
				"worker replacement did not change path content");

		auto reopened = rename_measured->open_role("worker-executable");
		require(
			reopened.has_value(),
			"retained measured inode could not produce an immutable snapshot after path rename");
		auto reopened_identity = materialization_fd_identity(reopened->get(), true);
		require(reopened_identity &&
					reopened_identity->size_bytes == measured_identity.size_bytes &&
					sdk::content_digest(read_file(reopened->get())) == worker_digest,
				"post-measurement role open returned the path replacement");
	}

	void replacement_after_measurement_negative()
	{
		std::array<char, 64U> directory_template{};
		constexpr std::string_view template_value{"/tmp/cxxlens-occurrence-XXXXXX"};
		std::ranges::copy(template_value, directory_template.begin());
		auto* directory = ::mkdtemp(directory_template.data());
		require(directory != nullptr, "test occurrence temporary directory creation failed");
		const std::filesystem::path prefix{directory};
		const auto materializer_path = prefix / std::string{paths.front()};
		std::error_code filesystem_error;
		std::filesystem::create_directories(materializer_path.parent_path(), filesystem_error);
		require(!filesystem_error, "test occurrence bin directory creation failed");
		std::filesystem::copy_file("/proc/self/exe",
								   materializer_path,
								   std::filesystem::copy_options::overwrite_existing,
								   filesystem_error);
		require(!filesystem_error, "test occurrence executable copy failed");
		std::filesystem::permissions(materializer_path,
									 std::filesystem::perms::owner_all,
									 std::filesystem::perm_options::replace,
									 filesystem_error);
		require(!filesystem_error, "test occurrence executable permission failed");

		for (std::size_t index{1U}; index < paths.size(); ++index)
		{
			const auto content =
				bytes(std::string{"authority:"} + std::string{roles[index]} + "\n");
			write_file(prefix / std::string{paths[index]}, content, index == 1U ? 0700 : 0600);
		}
		std::vector<std::string> digests;
		digests.reserve(paths.size());
		for (const auto path : paths)
			digests.push_back(sdk::content_digest(read_file(prefix / std::string{path})));
		auto manifest = manifest_bytes(false, installed_occurrence_files(digests));
		const auto manifest_path = prefix / std::string{materialization_occurrence_manifest_path};
		write_file(manifest_path, manifest);
		const auto manifest_digest = sdk::content_digest(manifest);

		require(::setenv(child_mode_name.data(), "1", 1) == 0 &&
					::setenv(child_prefix_name.data(), prefix.c_str(), 1) == 0 &&
					::setenv(child_manifest_digest_name.data(), manifest_digest.c_str(), 1) == 0 &&
					::setenv(child_materializer_digest_name.data(), digests[0U].c_str(), 1) == 0 &&
					::setenv(child_worker_digest_name.data(), digests[1U].c_str(), 1) == 0,
				"test occurrence child environment setup failed");
		const auto child = ::fork();
		require(child >= 0, "test occurrence child fork failed");
		if (child == 0)
		{
			::execl(
				materializer_path.c_str(), materializer_path.c_str(), static_cast<char*>(nullptr));
			::_exit(125);
		}
		(void)::unsetenv(child_mode_name.data());
		(void)::unsetenv(child_prefix_name.data());
		(void)::unsetenv(child_manifest_digest_name.data());
		(void)::unsetenv(child_materializer_digest_name.data());
		(void)::unsetenv(child_worker_digest_name.data());
		int status{};
		pid_t waited{};
		do
		{
			waited = ::waitpid(child, &status, 0);
		} while (waited < 0 && errno == EINTR);
		std::filesystem::remove_all(prefix, filesystem_error);
		require(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
				"replacement-after-measurement child failed");
	}

	void exact_manifest_closure()
	{
		for (const bool shared : {false, true})
		{
			auto bytes = manifest_bytes(shared, occurrence_files(shared));
			auto manifest =
				parse_materialization_occurrence_manifest(bytes, shared ? "shared" : "static");
			require(manifest && manifest->files.size() == (shared ? 18U : 12U) &&
						manifest->source_revision == std::string(40U, '1') &&
						manifest->source_tree == std::string(40U, '2') &&
						manifest->inventory_digest.starts_with("sha256:"),
					"exact static/shared occurrence manifest was rejected");
			require(!parse_materialization_occurrence_manifest(bytes, shared ? "static" : "shared"),
					"package-configuration swap was accepted");
		}
	}

	void negative_manifest_graph()
	{
		auto corrupt = manifest_bytes(false, occurrence_files(false), true);
		require(!parse_materialization_occurrence_manifest(corrupt, "static"),
				"occurrence payload digest drift was accepted");

		auto reordered = occurrence_files(false);
		std::swap(reordered[0U], reordered[1U]);
		auto reordered_bytes = manifest_bytes(false, std::move(reordered));
		require(!parse_materialization_occurrence_manifest(reordered_bytes, "static"),
				"ordered role/path inventory drift was accepted");

		auto self = occurrence_files(false);
		auto* path = self[0U].member("path");
		require(path != nullptr, "test occurrence path is missing");
		self[0U] = object_value({
			{"role", string_value("materializer-executable")},
			{"path", string_value(std::string{materialization_occurrence_manifest_path})},
			{"digest", string_value("sha256:" + std::string(64U, 'a'))},
		});
		auto self_bytes = manifest_bytes(false, std::move(self));
		require(!parse_materialization_occurrence_manifest(self_bytes, "static"),
				"occurrence manifest inventoried its own bytes");

		auto unsafe_dso = occurrence_files(true);
		unsafe_dso[12U] = object_value({
			{"role", string_value("base")},
			{"path", string_value("../lib/libcxxlens_base.so.1")},
			{"digest", string_value("sha256:" + std::string(64U, 'f'))},
		});
		auto unsafe_bytes = manifest_bytes(true, std::move(unsafe_dso));
		require(!parse_materialization_occurrence_manifest(unsafe_bytes, "shared"),
				"shared runtime DSO path escape was accepted");
	}
} // namespace

int main()
{
	if (::getenv(child_mode_name.data()) != nullptr)
	{
		replacement_after_measurement_child();
		return 0;
	}
	exact_manifest_closure();
	negative_manifest_graph();
	replacement_after_measurement_negative();
	return 0;
}
