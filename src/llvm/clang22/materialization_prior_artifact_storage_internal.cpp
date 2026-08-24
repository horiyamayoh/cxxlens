#include "materialization_prior_artifact_storage_internal.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		std::atomic<std::uint64_t> sidecar_attempt_counter{};

		[[nodiscard]] sdk::error artifact_error(std::string field, std::string detail = {})
		{
			return {"materialization.incremental-artifact-invalid",
					std::move(field),
					std::move(detail)};
		}

		[[nodiscard]] bool
		valid_artifact_limits(const materialization_prior_artifact_limits& limits) noexcept
		{
			return limits.max_bytes != 0U && limits.max_tasks != 0U &&
				limits.max_capture_bytes != 0U && limits.max_total_capture_bytes != 0U &&
				limits.max_batches_per_task != 0U && limits.max_chunks_per_batch != 0U &&
				limits.max_side_channel_records != 0U && limits.max_string_bytes != 0U &&
				limits.max_capture_bytes <= limits.max_bytes &&
				limits.max_total_capture_bytes <= limits.max_bytes;
		}

		[[nodiscard]] bool is_missing_sidecar(const sdk::error& error) noexcept
		{
			return error.code == "materialization.identity-mismatch" && error.field == "openat2" &&
				error.detail == std::to_string(ENOENT);
		}

		[[nodiscard]] bool is_errno_error(const sdk::error& error,
										  const std::string_view field,
										  const int value) noexcept
		{
			return error.code == "materialization.identity-mismatch" && error.field == field &&
				error.detail == std::to_string(value);
		}

		[[nodiscard]] sdk::result<std::unique_ptr<materialization_replayable_spool>>
		spool_sidecar(const materialization_owned_fd& file,
					  const materialization_prior_artifact_limits& limits)
		{
			auto identity = materialization_fd_identity(file.get(), true);
			if (!identity || identity->size_bytes == 0U || identity->size_bytes > limits.max_bytes)
				return sdk::unexpected(artifact_error("sidecar", "size"));
			auto storage = make_materialization_private_spool();
			if (!storage)
				return sdk::unexpected(artifact_error("sidecar", "spool-create"));
			try
			{
				std::vector<std::byte> buffer(default_stream_chunk_bytes);
				std::uint64_t offset{};
				while (offset < identity->size_bytes)
				{
					const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(
						identity->size_bytes - offset, static_cast<std::uint64_t>(buffer.size())));
					std::size_t received{};
					while (received < chunk)
					{
						const auto count =
							::read(file.get(), buffer.data() + received, chunk - received);
						if (count < 0 && errno == EINTR)
							continue;
						if (count <= 0 || static_cast<std::size_t>(count) > chunk - received)
							return sdk::unexpected(artifact_error("sidecar", "read"));
						received += static_cast<std::size_t>(count);
					}
					if (auto appended = (*storage)->append(std::span{buffer}.first(chunk));
						!appended)
						return sdk::unexpected(artifact_error("sidecar", "spool-write"));
					offset += chunk;
				}
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(artifact_error("sidecar", "allocation"));
			}
			auto final_identity = materialization_fd_identity(file.get(), true);
			if (!final_identity || *final_identity != *identity)
				return sdk::unexpected(artifact_error("sidecar", "immutable-conflict"));
			if (auto sealed = (*storage)->seal(); !sealed)
				return sdk::unexpected(artifact_error("sidecar", "spool-seal"));
			return std::move(*storage);
		}

		[[nodiscard]] sdk::result<void> write_all(const materialization_owned_fd& file,
												  const std::span<const std::byte> bytes)
		{
			std::size_t offset{};
			while (offset < bytes.size())
			{
				const auto count =
					::write(file.get(), bytes.data() + offset, bytes.size() - offset);
				if (count < 0 && errno == EINTR)
					continue;
				if (count <= 0 || static_cast<std::size_t>(count) > bytes.size() - offset)
					return sdk::unexpected(artifact_error("sidecar", "write"));
				offset += static_cast<std::size_t>(count);
			}
			return {};
		}

		[[nodiscard]] sdk::result<void> sync_sidecar_parent(const materialization_effect_root& root,
															const std::string_view path);

		[[nodiscard]] sdk::result<void>
		compare_sidecar_to_spool(const materialization_owned_fd& file,
								 materialization_replayable_spool& spool,
								 const materialization_prior_artifact_limits& limits)
		{
			auto identity = materialization_fd_identity(file.get(), true);
			if (!identity || identity->size_bytes != spool.size_bytes() ||
				identity->size_bytes > limits.max_bytes)
				return sdk::unexpected(artifact_error("sidecar", "immutable-conflict"));
			try
			{
				std::vector<std::byte> expected(default_stream_chunk_bytes);
				std::vector<std::byte> actual(default_stream_chunk_bytes);
				std::uint64_t offset{};
				while (offset < spool.size_bytes())
				{
					const auto remaining = spool.size_bytes() - offset;
					const auto chunk = static_cast<std::size_t>(
						std::min<std::uint64_t>(remaining, expected.size()));
					std::size_t received{};
					while (received < chunk)
					{
						const auto count =
							::read(file.get(), actual.data() + received, chunk - received);
						if (count < 0 && errno == EINTR)
							continue;
						if (count <= 0 || static_cast<std::size_t>(count) > chunk - received)
							return sdk::unexpected(artifact_error("sidecar", "read"));
						received += static_cast<std::size_t>(count);
					}
					auto read = spool.read_at(offset, std::span{expected}.first(chunk));
					if (!read || *read != chunk ||
						!std::ranges::equal(std::span{expected}.first(chunk),
											std::span{actual}.first(chunk)))
						return sdk::unexpected(artifact_error("sidecar", "immutable-conflict"));
					offset += chunk;
				}
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(artifact_error("sidecar", "allocation"));
			}
			return {};
		}

		[[nodiscard]] sdk::result<void> write_spool_sidecar(const materialization_owned_fd& file,
															materialization_replayable_spool& spool)
		{
			try
			{
				std::vector<std::byte> buffer(default_stream_chunk_bytes);
				std::uint64_t offset{};
				while (offset < spool.size_bytes())
				{
					const auto remaining = spool.size_bytes() - offset;
					const auto chunk =
						static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
					auto read = spool.read_at(offset, std::span{buffer}.first(chunk));
					if (!read || *read != chunk)
						return sdk::unexpected(artifact_error("sidecar", "spool-read"));
					if (auto written = write_all(file, std::span{buffer}.first(chunk)); !written)
						return written;
					offset += chunk;
				}
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(artifact_error("sidecar", "allocation"));
			}
			if (::fsync(file.get()) != 0)
				return sdk::unexpected(artifact_error("sidecar", "sync"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		install_sidecar_spool(const materialization_effect_root& root,
							  const std::string_view path,
							  materialization_replayable_spool& spool,
							  const materialization_prior_artifact_limits& limits)
		{
			auto existing = root.open_beneath(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
			if (existing)
				return compare_sidecar_to_spool(*existing, spool, limits);
			if (!is_missing_sidecar(existing.error()))
				return sdk::unexpected(std::move(existing.error()));

			if (!spool.sealed() || spool.size_bytes() == 0U ||
				spool.size_bytes() > limits.max_bytes)
				return sdk::unexpected(artifact_error("sidecar", "spool-lifecycle"));
			auto payload_digest = digest_materialization_spool(spool);
			if (!payload_digest || payload_digest->rfind("sha256:", 0U) != 0U ||
				payload_digest->size() <= 7U)
				return sdk::unexpected(artifact_error("sidecar", "payload-digest"));
			const auto attempt = sidecar_attempt_counter.fetch_add(1U, std::memory_order_relaxed);
			std::string temporary_path{path};
			std::string temporary_suffix{".tmp-"};
			temporary_suffix.append(payload_digest->substr(7U));
			temporary_suffix.push_back('-');
			temporary_suffix.append(std::to_string(static_cast<unsigned long long>(::getpid())));
			temporary_suffix.push_back('-');
			temporary_suffix.append(std::to_string(static_cast<unsigned long long>(attempt)));
			if (temporary_path.size() >
				std::numeric_limits<std::size_t>::max() - temporary_suffix.size())
				return sdk::unexpected(artifact_error("sidecar", "temporary-path-overflow"));
			temporary_path += temporary_suffix;
			if (auto valid = validate_materialization_relative_path(temporary_path, 4095U, true);
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			const auto remove_temporary = [&]() -> sdk::result<void>
			{
				auto removed = root.unlink_beneath(temporary_path);
				if (!removed && !is_errno_error(removed.error(), "unlinkat", ENOENT))
					return removed;
				return {};
			};

			auto temporary = root.open_beneath(
				temporary_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600U);
			if (temporary)
			{
				if (auto written = write_spool_sidecar(*temporary, spool); !written)
				{
					if (auto removed = remove_temporary(); !removed)
						return removed;
					return written;
				}
			}
			else if (is_errno_error(temporary.error(), "openat2", EEXIST))
				return sdk::unexpected(artifact_error("sidecar", "temporary-conflict"));
			else
				return sdk::unexpected(std::move(temporary.error()));

			auto renamed = root.rename_beneath(temporary_path, path);
			if (!renamed)
			{
				if (is_errno_error(renamed.error(), "renameat2", EEXIST) ||
					is_errno_error(renamed.error(), "renameat2", ENOENT))
				{
					auto raced = root.open_beneath(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
					if (!raced)
					{
						if (auto removed = remove_temporary(); !removed)
							return removed;
						return sdk::unexpected(std::move(raced.error()));
					}
					if (auto matching = compare_sidecar_to_spool(*raced, spool, limits); !matching)
					{
						if (auto removed = remove_temporary(); !removed)
							return removed;
						return sdk::unexpected(artifact_error("sidecar", "immutable-conflict"));
					}
					if (auto removed = remove_temporary(); !removed)
						return removed;
					return {};
				}
				if (auto removed = remove_temporary(); !removed)
					return removed;
				return renamed;
			}
			else if (auto synced = sync_sidecar_parent(root, path); !synced)
				return synced;
			return {};
		}

		[[nodiscard]] sdk::result<void> sync_sidecar_parent(const materialization_effect_root& root,
															const std::string_view path)
		{
			const auto separator = path.rfind('/');
			materialization_owned_fd parent;
			if (separator == std::string_view::npos)
			{
				auto duplicated = root.duplicate_directory();
				if (!duplicated)
					return sdk::unexpected(artifact_error("sidecar.parent", "duplicate"));
				parent = std::move(*duplicated);
			}
			else
			{
				auto opened = root.open_beneath(path.substr(0U, separator),
												O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
				if (!opened)
					return sdk::unexpected(artifact_error("sidecar.parent", "open"));
				parent = std::move(*opened);
			}
			if (!parent || ::fsync(parent.get()) != 0)
				return sdk::unexpected(artifact_error("sidecar.parent", "sync"));
			return {};
		}
	} // namespace

	bool prior_artifact_limits_valid(const materialization_prior_artifact_limits& limits) noexcept
	{
		return valid_artifact_limits(limits);
	}

	bool prior_artifact_sidecar_missing(const sdk::error& error) noexcept
	{
		return is_missing_sidecar(error);
	}

	sdk::result<std::unique_ptr<materialization_replayable_spool>>
	spool_prior_artifact_sidecar(const materialization_owned_fd& file,
								 const materialization_prior_artifact_limits& limits)
	{
		return spool_sidecar(file, limits);
	}

	sdk::result<void>
	install_prior_artifact_sidecar(const materialization_effect_root& root,
								   const std::string_view path,
								   materialization_replayable_spool& spool,
								   const materialization_prior_artifact_limits& limits)
	{
		return install_sidecar_spool(root, path, spool, limits);
	}
} // namespace cxxlens::detail::clang22::materialization
