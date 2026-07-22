#include "materialization_occurrence.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <limits>
#include <new>
#include <ranges>
#include <span>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/memfd.h>
#include <sys/syscall.h>
#endif

#include "materialization_identity.hpp"
#include "materialization_io.hpp"
#include "materialization_json.hpp"

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		constexpr std::array<std::string_view, 13U> static_roles{
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
		constexpr std::array<std::string_view, 13U> static_paths{
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
		constexpr std::array<std::string_view, 6U> shared_roles{
			"base", "kernel", "query", "recipes", "provider-sdk", "clang22-provider-sdk"};
		constexpr std::array<std::string_view, 6U> shared_stems{
			"libcxxlens_base.so",
			"libcxxlens_kernel.so",
			"libcxxlens_query.so",
			"libcxxlens_recipes.so",
			"libcxxlens_provider_sdk.so",
			"libcxxlens_clang22_provider_sdk.so",
		};

		[[nodiscard]] sdk::error occurrence_error(std::string field, std::string detail)
		{
			return {"materialization.identity-mismatch", std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool lower_digest(const std::string_view value)
		{
			return value.size() == 71U && value.starts_with("sha256:") &&
				std::ranges::all_of(value.substr(7U),
									[](const char value)
									{
										return (value >= '0' && value <= '9') ||
											(value >= 'a' && value <= 'f');
									});
		}

		[[nodiscard]] bool revision(const std::string_view value)
		{
			return value.size() == 40U &&
				std::ranges::all_of(value,
									[](const char value)
									{
										return (value >= '0' && value <= '9') ||
											(value >= 'a' && value <= 'f');
									});
		}

		[[nodiscard]] sdk::result<std::string> member_text(const json_value& value,
														   const std::string_view member)
		{
			const auto* item = value.member(member);
			if (item == nullptr || item->as_string() == nullptr)
				return sdk::unexpected(
					occurrence_error(std::string{member}, "missing-or-not-string"));
			return *item->as_string();
		}

		[[nodiscard]] bool shared_library_path(const std::string_view value,
											   const std::string_view stem)
		{
			const auto prefix = value.starts_with("lib/") ? std::string_view{"lib/"}
				: value.starts_with("lib64/")			  ? std::string_view{"lib64/"}
														  : std::string_view{};
			if (prefix.empty() || !value.substr(prefix.size()).starts_with(stem))
				return false;
			auto suffix = value.substr(prefix.size() + stem.size());
			while (!suffix.empty())
			{
				if (suffix.front() != '.')
					return false;
				suffix.remove_prefix(1U);
				const auto end = suffix.find('.');
				const auto component = suffix.substr(0U, end);
				if (component.empty() ||
					!std::ranges::all_of(component,
										 [](const char value)
										 {
											 return value >= '0' && value <= '9';
										 }))
					return false;
				if (end == std::string_view::npos)
					break;
				suffix.remove_prefix(end);
			}
			return true;
		}

		struct measured_role_file
		{
			std::string digest;
			materialization_file_identity identity;
			materialization_owned_fd descriptor;
		};

		struct measured_descriptor_content
		{
			std::string digest;
			materialization_file_identity identity;
		};

		[[nodiscard]] sdk::result<std::vector<std::byte>>
		read_fd_bounded(const int descriptor, const std::uint64_t maximum)
		{
			auto identity = materialization_fd_identity(descriptor, true);
			if (!identity || identity->size_bytes > maximum ||
				identity->size_bytes > std::numeric_limits<std::size_t>::max())
				return sdk::unexpected(occurrence_error("manifest", "size"));
			try
			{
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
					if (count <= 0)
						return sdk::unexpected(occurrence_error("manifest", "read"));
					offset += static_cast<std::size_t>(count);
				}
				auto after = materialization_fd_identity(descriptor, true);
				if (!after || *after != *identity)
					return sdk::unexpected(occurrence_error("manifest", "object-replaced"));
				return output;
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(occurrence_error("manifest", "allocation"));
			}
		}

		[[nodiscard]] sdk::result<measured_descriptor_content>
		digest_stable_descriptor(const int descriptor, const std::string_view role)
		{
			auto before = materialization_fd_identity(descriptor, true);
			if (!before)
				return sdk::unexpected(std::move(before.error()));
			std::unique_ptr<materialization_digest_accumulator> accumulator;
			try
			{
				accumulator = make_materialization_sha256_accumulator();
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(occurrence_error(std::string{role}, "allocation"));
			}
			std::array<std::byte, 64U * 1024U> buffer{};
			std::uint64_t offset{};
			while (offset < before->size_bytes)
			{
				const auto remaining = before->size_bytes - offset;
				const auto requested =
					static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
				const auto count =
					::pread(descriptor, buffer.data(), requested, static_cast<off_t>(offset));
				if (count < 0 && errno == EINTR)
					continue;
				if (count <= 0)
					return sdk::unexpected(occurrence_error(std::string{role}, "read"));
				if (auto updated = accumulator->update(
						std::span{buffer}.first(static_cast<std::size_t>(count)));
					!updated)
					return sdk::unexpected(occurrence_error(std::string{role}, "digest-update"));
				offset += static_cast<std::uint64_t>(count);
			}
			auto digest = accumulator->finish();
			if (!digest)
				return sdk::unexpected(occurrence_error(std::string{role}, "digest-finalize"));
			auto after = materialization_fd_identity(descriptor, true);
			if (!after || *before != *after)
				return sdk::unexpected(occurrence_error(std::string{role}, "object-replaced"));
			return measured_descriptor_content{std::move(*digest), *before};
		}

		[[nodiscard]] sdk::result<materialization_owned_fd>
		immutable_verified_snapshot(const int source,
									const materialization_file_identity& expected_identity,
									const std::string_view expected_digest,
									const std::string_view role)
		{
#if defined(__linux__) && defined(SYS_memfd_create) && defined(F_ADD_SEALS) && \
	defined(F_GET_SEALS) && defined(F_SEAL_WRITE) && defined(F_SEAL_GROW) && \
	defined(F_SEAL_SHRINK) && defined(F_SEAL_SEAL)
			auto before = materialization_fd_identity(source, true);
			if (!before || *before != expected_identity)
				return sdk::unexpected(occurrence_error(std::string{role}, "object-replaced"));
			const auto snapshot_descriptor = static_cast<int>(::syscall(
				SYS_memfd_create, "cxxlens-occurrence-role", MFD_CLOEXEC | MFD_ALLOW_SEALING));
			if (snapshot_descriptor < 0)
				return sdk::unexpected(
					occurrence_error(std::string{role}, "immutable-snapshot-create"));
			materialization_owned_fd snapshot{snapshot_descriptor};
			if (::fchmod(snapshot.get(), static_cast<mode_t>(before->mode & 0777U)) != 0)
				return sdk::unexpected(
					occurrence_error(std::string{role}, "immutable-snapshot-mode"));

			std::unique_ptr<materialization_digest_accumulator> accumulator;
			try
			{
				accumulator = make_materialization_sha256_accumulator();
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(occurrence_error(std::string{role}, "allocation"));
			}
			std::array<std::byte, 64U * 1024U> buffer{};
			std::uint64_t offset{};
			while (offset < before->size_bytes)
			{
				const auto remaining = before->size_bytes - offset;
				const auto requested =
					static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
				const auto count =
					::pread(source, buffer.data(), requested, static_cast<off_t>(offset));
				if (count < 0 && errno == EINTR)
					continue;
				if (count <= 0)
					return sdk::unexpected(
						occurrence_error(std::string{role}, "immutable-snapshot-read"));
				const auto copied = static_cast<std::size_t>(count);
				if (auto updated = accumulator->update(std::span{buffer}.first(copied)); !updated)
					return sdk::unexpected(
						occurrence_error(std::string{role}, "immutable-snapshot-digest"));
				std::size_t written{};
				while (written < copied)
				{
					const auto output =
						::pwrite(snapshot.get(),
								 buffer.data() + written,
								 copied - written,
								 static_cast<off_t>(offset) + static_cast<off_t>(written));
					if (output < 0 && errno == EINTR)
						continue;
					if (output <= 0)
						return sdk::unexpected(
							occurrence_error(std::string{role}, "immutable-snapshot-write"));
					written += static_cast<std::size_t>(output);
				}
				offset += copied;
			}
			auto digest = accumulator->finish();
			auto after = materialization_fd_identity(source, true);
			if (!digest)
				return sdk::unexpected(
					occurrence_error(std::string{role}, "immutable-snapshot-digest"));
			if (!after || *before != *after || *after != expected_identity)
				return sdk::unexpected(occurrence_error(std::string{role}, "object-replaced"));
			if (*digest != expected_digest)
				return sdk::unexpected(occurrence_error(std::string{role}, "digest-mismatch"));

			constexpr int required_seals = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
			if (::fcntl(snapshot.get(), F_ADD_SEALS, required_seals) != 0)
				return sdk::unexpected(
					occurrence_error(std::string{role}, "immutable-snapshot-seal"));
			const auto snapshot_seals = ::fcntl(snapshot.get(), F_GET_SEALS);
			if (snapshot_seals < 0 || (snapshot_seals & required_seals) != required_seals)
				return sdk::unexpected(
					occurrence_error(std::string{role}, "immutable-snapshot-seal"));
			auto sealed = digest_stable_descriptor(snapshot.get(), role);
			if (!sealed || sealed->digest != expected_digest ||
				sealed->identity.size_bytes != expected_identity.size_bytes)
				return sdk::unexpected(
					occurrence_error(std::string{role}, "immutable-snapshot-verify"));
			return snapshot;
#else
			(void)source;
			(void)expected_identity;
			(void)expected_digest;
			return sdk::unexpected(
				occurrence_error(std::string{role}, "immutable-snapshot-unsupported"));
#endif
		}

		[[nodiscard]] sdk::result<measured_role_file>
		digest_stable_file(const int prefix, const materialization_occurrence_file& authority)
		{
			auto opened = open_materialization_beneath(prefix, authority.path, O_RDONLY);
			if (!opened)
				return sdk::unexpected(std::move(opened.error()));
			auto measured = digest_stable_descriptor(opened->get(), authority.role);
			if (!measured)
				return sdk::unexpected(std::move(measured.error()));
			auto snapshot = immutable_verified_snapshot(
				opened->get(), measured->identity, authority.digest, authority.role);
			auto rebound = open_materialization_beneath(prefix, authority.path, O_RDONLY);
			auto rebound_identity = rebound
				? materialization_fd_identity(rebound->get(), true)
				: sdk::result<materialization_file_identity>{
					  sdk::unexpected(occurrence_error(authority.role, "rebind"))};
			if (!snapshot)
				return sdk::unexpected(std::move(snapshot.error()));
			if (!rebound || !rebound_identity ||
				measured->identity.device != rebound_identity->device ||
				measured->identity.inode != rebound_identity->inode)
				return sdk::unexpected(occurrence_error(authority.role, "object-replaced"));
			return measured_role_file{
				std::move(measured->digest), measured->identity, std::move(*snapshot)};
		}

		[[nodiscard]] std::string prefix_observation(const materialization_file_identity& identity)
		{
			const auto value = std::string{"rooted-occurrence-v1\0", 21U} +
				std::to_string(identity.device) + "\0" + std::to_string(identity.inode);
			return sdk::content_digest(std::as_bytes(std::span{value}));
		}
	} // namespace

	sdk::result<materialization_occurrence_manifest>
	parse_materialization_occurrence_manifest(const std::span<const std::byte> bytes,
											  const std::string_view expected_configuration)
	{
		if (bytes.size() > 1024U * 1024U)
			return sdk::unexpected(occurrence_error("manifest", "maximum-bytes"));
		std::string raw{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
		json_limits limits;
		limits.max_input_bytes = 1024U * 1024U;
		limits.max_string_bytes = 4095U;
		limits.max_total_string_bytes = 256U * 1024U;
		limits.max_array_elements = 19U;
		limits.max_object_members = 7U;
		limits.max_total_values = 84U;
		auto document = parse_json_object(std::move(raw), limits);
		if (!document)
			return sdk::unexpected(occurrence_error("manifest", document.error().detail));
		const auto& root = document->root();
		constexpr std::array members{
			std::string_view{"schema"},
			std::string_view{"manifest_version"},
			std::string_view{"source_revision"},
			std::string_view{"source_tree"},
			std::string_view{"package_configuration"},
			std::string_view{"files"},
			std::string_view{"occurrence_payload_digest"},
		};
		if (!root.has_exact_members(members))
			return sdk::unexpected(occurrence_error("manifest", "member-set"));
		auto schema = member_text(root, "schema");
		auto version = member_text(root, "manifest_version");
		auto source_revision = member_text(root, "source_revision");
		auto source_tree = member_text(root, "source_tree");
		auto configuration = member_text(root, "package_configuration");
		auto payload_digest = member_text(root, "occurrence_payload_digest");
		if (!schema || !version || !source_revision || !source_tree || !configuration ||
			!payload_digest || *schema != "cxxlens.clang22-materializer-occurrence-manifest.v1" ||
			*version != "1.0.0" || !revision(*source_revision) || !revision(*source_tree) ||
			(*configuration != "static" && *configuration != "shared") ||
			*configuration != expected_configuration || !lower_digest(*payload_digest))
			return sdk::unexpected(occurrence_error("manifest", "authority"));
		const auto* files_value = root.member("files");
		const auto* files = files_value != nullptr ? files_value->as_array() : nullptr;
		const auto expected_size = *configuration == "static" ? 13U : 19U;
		if (files == nullptr || files->size() != expected_size)
			return sdk::unexpected(occurrence_error("files", "cardinality"));
		std::vector<materialization_occurrence_file> decoded;
		decoded.reserve(files->size());
		constexpr std::array file_members{
			std::string_view{"role"}, std::string_view{"path"}, std::string_view{"digest"}};
		for (std::size_t index{}; index < files->size(); ++index)
		{
			const auto& item = files->at(index);
			if (!item.has_exact_members(file_members))
				return sdk::unexpected(occurrence_error("files", "member-set"));
			auto role = member_text(item, "role");
			auto path = member_text(item, "path");
			auto digest = member_text(item, "digest");
			if (!role || !path || !digest || !lower_digest(*digest) ||
				!validate_materialization_relative_path(*path, 4095U, true))
				return sdk::unexpected(occurrence_error("files", "entry"));
			if (index < static_roles.size())
			{
				if (*role != static_roles[index] || *path != static_paths[index])
					return sdk::unexpected(occurrence_error("files", "static-order"));
			}
			else
			{
				const auto shared_index = index - static_roles.size();
				if (*configuration != "shared" || *role != shared_roles[shared_index] ||
					!shared_library_path(*path, shared_stems[shared_index]))
					return sdk::unexpected(occurrence_error("files", "shared-order"));
			}
			if (*path == materialization_occurrence_manifest_path)
				return sdk::unexpected(occurrence_error("files", "self-entry"));
			decoded.push_back({std::move(*role), std::move(*path), std::move(*digest)});
		}

		constexpr std::array removed{std::string_view{"occurrence_payload_digest"}};
		auto payload = object_without(root, removed);
		if (!payload)
			return sdk::unexpected(std::move(payload.error()));
		const auto payload_bytes = canonical_json(*payload);
		const auto expected_payload = sdk::content_digest(std::as_bytes(std::span{payload_bytes}));
		if (expected_payload != *payload_digest)
			return sdk::unexpected(occurrence_error("occurrence_payload_digest", "mismatch"));
		const auto inventory_bytes = canonical_json(*files_value);
		const auto inventory_digest =
			sdk::content_digest(std::as_bytes(std::span{inventory_bytes}));
		return materialization_occurrence_manifest{std::move(*source_revision),
												   std::move(*source_tree),
												   std::move(*configuration),
												   std::move(decoded),
												   std::move(*payload_digest),
												   inventory_digest};
	}

	measured_materialization_occurrence::measured_materialization_occurrence(
		materialization_owned_fd prefix,
		materialization_occurrence_manifest manifest,
		materialization_occurrence_receipt receipt,
		std::vector<materialization_owned_fd> role_descriptors)
		: prefix_{std::move(prefix)}, manifest_{std::move(manifest)}, receipt_{std::move(receipt)},
		  role_descriptors_{std::move(role_descriptors)}
	{
	}

	const materialization_occurrence_manifest&
	measured_materialization_occurrence::manifest() const noexcept
	{
		return manifest_;
	}

	const materialization_occurrence_receipt&
	measured_materialization_occurrence::receipt() const noexcept
	{
		return receipt_;
	}

	sdk::result<materialization_owned_fd>
	measured_materialization_occurrence::open_role(const std::string_view role) const
	{
		const auto found =
			std::ranges::find(manifest_.files, role, &materialization_occurrence_file::role);
		if (found == manifest_.files.end())
			return sdk::unexpected(occurrence_error("role", "missing"));
		const auto index = static_cast<std::size_t>(found - manifest_.files.begin());
		if (index >= role_descriptors_.size() || index >= receipt_.files.size() ||
			!role_descriptors_[index] || receipt_.files[index].authority != *found)
			return sdk::unexpected(occurrence_error(found->role, "measured-descriptor-missing"));
#if defined(__linux__) && defined(F_GET_SEALS) && defined(F_SEAL_WRITE) && defined(F_SEAL_GROW) && \
	defined(F_SEAL_SHRINK) && defined(F_SEAL_SEAL)
		constexpr int required_seals = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
		const auto retained_seals = ::fcntl(role_descriptors_[index].get(), F_GET_SEALS);
		auto retained_identity = materialization_fd_identity(role_descriptors_[index].get(), true);
		if (!retained_identity || retained_seals < 0 ||
			(retained_seals & required_seals) != required_seals)
			return sdk::unexpected(occurrence_error(found->role, "immutable-snapshot-seals"));
		const auto descriptor_path =
			std::string{"/proc/self/fd/"} + std::to_string(role_descriptors_[index].get());
		int reopened{};
		do
		{
			reopened = ::open(descriptor_path.c_str(), O_RDONLY | O_CLOEXEC);
		} while (reopened < 0 && errno == EINTR);
		if (reopened < 0)
			return sdk::unexpected(occurrence_error(found->role, "immutable-snapshot-reopen"));
		materialization_owned_fd output{reopened};
		auto identity = materialization_fd_identity(output.get(), true);
		const auto output_seals = ::fcntl(output.get(), F_GET_SEALS);
		if (!identity || identity->device != retained_identity->device ||
			identity->inode != retained_identity->inode ||
			identity->size_bytes != receipt_.files[index].identity.size_bytes || output_seals < 0 ||
			(output_seals & required_seals) != required_seals)
			return sdk::unexpected(occurrence_error(found->role, "immutable-snapshot-identity"));
		return output;
#else
		return sdk::unexpected(occurrence_error(found->role, "immutable-snapshot-unsupported"));
#endif
	}

	sdk::result<measured_materialization_occurrence>
	measure_materialization_occurrence(const materialization_occurrence_expectation& expected)
	{
		if (!revision(expected.source_revision) || !revision(expected.source_tree) ||
			(expected.package_configuration != "static" &&
			 expected.package_configuration != "shared") ||
			!lower_digest(expected.occurrence_manifest_digest) ||
			!lower_digest(expected.materializer_executable_digest) ||
			!lower_digest(expected.worker_executable_digest))
			return sdk::unexpected(occurrence_error("request-occurrence", "grammar"));
		const auto self_descriptor = ::open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
		if (self_descriptor < 0)
			return sdk::unexpected(occurrence_error("proc-self-exe", std::to_string(errno)));
		materialization_owned_fd self{self_descriptor};
		auto self_identity = materialization_fd_identity(self.get(), true);
		if (!self_identity)
			return sdk::unexpected(std::move(self_identity.error()));
		std::array<char, 4097U> link{};
		const auto link_size = ::readlink("/proc/self/exe", link.data(), link.size() - 1U);
		if (link_size <= 0 || static_cast<std::size_t>(link_size) >= link.size() - 1U)
			return sdk::unexpected(occurrence_error("proc-self-exe", "readlink"));
		const std::string_view executable_path{link.data(), static_cast<std::size_t>(link_size)};
		constexpr std::string_view suffix{"/bin/cxxlens-clang22-materialize"};
		if (executable_path.ends_with(" (deleted)") || !executable_path.ends_with(suffix) ||
			executable_path.size() == suffix.size())
			return sdk::unexpected(occurrence_error("proc-self-exe", "installed-suffix"));
		const auto prefix_path = executable_path.substr(0U, executable_path.size() - suffix.size());
		auto prefix =
			open_materialization_absolute_no_symlinks(prefix_path, O_RDONLY | O_DIRECTORY);
		if (!prefix)
			return sdk::unexpected(std::move(prefix.error()));
		auto prefix_identity = materialization_fd_identity(prefix->get(), false);
		auto rebound_self =
			open_materialization_beneath(prefix->get(), static_paths.front(), O_RDONLY);
		auto rebound_identity = rebound_self
			? materialization_fd_identity(rebound_self->get(), true)
			: sdk::result<materialization_file_identity>{
				  sdk::unexpected(occurrence_error("proc-self-exe", "rebind"))};
		if (!prefix_identity || !rebound_self || !rebound_identity ||
			self_identity->device != rebound_identity->device ||
			self_identity->inode != rebound_identity->inode)
			return sdk::unexpected(occurrence_error("proc-self-exe", "object-mismatch"));

		auto manifest_fd = open_materialization_beneath(
			prefix->get(), materialization_occurrence_manifest_path, O_RDONLY);
		if (!manifest_fd)
			return sdk::unexpected(std::move(manifest_fd.error()));
		auto manifest_identity = materialization_fd_identity(manifest_fd->get(), true);
		if (!manifest_identity)
			return sdk::unexpected(std::move(manifest_identity.error()));
		auto manifest_bytes = read_fd_bounded(manifest_fd->get(), 1024U * 1024U);
		if (!manifest_bytes)
			return sdk::unexpected(std::move(manifest_bytes.error()));
		auto rebound_manifest = open_materialization_beneath(
			prefix->get(), materialization_occurrence_manifest_path, O_RDONLY);
		auto rebound_manifest_identity = rebound_manifest
			? materialization_fd_identity(rebound_manifest->get(), true)
			: sdk::result<materialization_file_identity>{
				  sdk::unexpected(occurrence_error("manifest", "rebind"))};
		if (!rebound_manifest || !rebound_manifest_identity ||
			manifest_identity->device != rebound_manifest_identity->device ||
			manifest_identity->inode != rebound_manifest_identity->inode)
			return sdk::unexpected(occurrence_error("manifest", "object-replaced"));
		const auto manifest_digest = sdk::content_digest(*manifest_bytes);
		if (manifest_digest != expected.occurrence_manifest_digest)
			return sdk::unexpected(occurrence_error("occurrence_manifest_digest", "mismatch"));
		auto manifest = parse_materialization_occurrence_manifest(*manifest_bytes,
																  expected.package_configuration);
		if (!manifest)
			return sdk::unexpected(std::move(manifest.error()));
		if (manifest->source_revision != expected.source_revision ||
			manifest->source_tree != expected.source_tree)
			return sdk::unexpected(occurrence_error("manifest-source", "mismatch"));

		materialization_occurrence_receipt receipt;
		receipt.manifest_file_digest = manifest_digest;
		receipt.occurrence_payload_digest = manifest->occurrence_payload_digest;
		receipt.inventory_digest = manifest->inventory_digest;
		receipt.prefix_device_inode_observation_digest = prefix_observation(*prefix_identity);
		receipt.files.reserve(manifest->files.size());
		std::vector<materialization_owned_fd> role_descriptors;
		role_descriptors.reserve(manifest->files.size());
		for (const auto& file : manifest->files)
		{
			auto measured = digest_stable_file(prefix->get(), file);
			if (!measured || measured->digest != file.digest)
				return sdk::unexpected(occurrence_error(file.role, "digest-mismatch"));
			if (file.role == "materializer-executable" &&
				(measured->digest != expected.materializer_executable_digest ||
				 measured->identity.device != self_identity->device ||
				 measured->identity.inode != self_identity->inode))
				return sdk::unexpected(occurrence_error(file.role, "request-or-self-mismatch"));
			if (file.role == "worker-executable" &&
				measured->digest != expected.worker_executable_digest)
				return sdk::unexpected(occurrence_error(file.role, "request-mismatch"));
			receipt.files.push_back({file, measured->identity});
			role_descriptors.push_back(std::move(measured->descriptor));
		}
		return measured_materialization_occurrence{std::move(*prefix),
												   std::move(*manifest),
												   std::move(receipt),
												   std::move(role_descriptors)};
	}
} // namespace cxxlens::detail::clang22::materialization
