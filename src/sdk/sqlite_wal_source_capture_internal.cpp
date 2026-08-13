#include "sqlite_wal_source_capture_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "sqlite_payload_streaming_internal.hpp"
#include "sqlite_wal_receipt_internal.hpp"

namespace cxxlens::sdk
{
	namespace
	{
		[[nodiscard]] error capture_binding_error(std::string detail)
		{
			return {"store.backend-unavailable", "sqlite-wal-source-capture", std::move(detail)};
		}

		[[nodiscard]] error capture_source_changed()
		{
			return {"store.sqlite-failure",
					"sqlite-initialization-sidecar",
					"concurrent-source-change"};
		}

		[[nodiscard]] error capture_unrecognized_source()
		{
			return {"store.sqlite-failure",
					"sqlite-initialization-sidecar",
					"unrecognized-preauthority-state"};
		}

		[[nodiscard]] bool canonical_sha256(const std::string_view value) noexcept
		{
			if (value.size() != 71U || !value.starts_with("sha256:"))
				return false;
			return std::ranges::all_of(value.substr(7U),
									   [](const char byte)
									   {
										   return (byte >= '0' && byte <= '9') ||
											   (byte >= 'a' && byte <= 'f');
									   });
		}

		[[nodiscard]] result<sqlite_backend_copy_receipt>
		observe_full_receipt(const sqlite_backend_held_object& source)
		{
			auto size = source.size();
			if (!size)
				return unexpected(std::move(size.error()));
			if (*size > sqlite_sha256_maximum_byte_count)
				return unexpected(capture_binding_error("source-accounting-overflow"));
			auto digest = source.sha256();
			if (!digest)
				return unexpected(std::move(digest.error()));
			if (!canonical_sha256(*digest))
				return unexpected(capture_binding_error("source-digest-spelling"));
			return sqlite_backend_copy_receipt{*size, std::move(*digest)};
		}

		class held_byte_source final : public sqlite_bounded_byte_source
		{
		  public:
			held_byte_source(const sqlite_backend_held_object& source,
							 const std::uint64_t byte_count) noexcept
				: source_{&source}, byte_count_{byte_count}
			{
			}

			result<std::size_t> read(const std::span<std::byte> output) override
			{
				if (offset_ == byte_count_)
					return 0U;
				if (source_ == nullptr || output.empty() || offset_ > byte_count_)
					return unexpected(capture_binding_error("source-read-state"));
				const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(
					output.size(),
					std::min<std::uint64_t>(sqlite_wal_source_capture_buffer_bound,
											byte_count_ - offset_)));
				std::uint64_t next{};
				if (wanted == 0U ||
					!sqlite_checked_add_u64(offset_, static_cast<std::uint64_t>(wanted), next) ||
					next > byte_count_)
					return unexpected(capture_binding_error("source-accounting-overflow"));
				if (auto read = source_->read_exact(offset_, output.first(wanted)); !read)
					return unexpected(std::move(read.error()));
				offset_ = next;
				return wanted;
			}

		  private:
			const sqlite_backend_held_object* source_{};
			std::uint64_t byte_count_{};
			std::uint64_t offset_{};
		};

		[[nodiscard]] result<sqlite_backend_copy_receipt>
		copy_exact_to_workspace(const sqlite_backend_held_object& source,
								const std::uint64_t byte_count,
								const sqlite_wal_recovery_copy_role role,
								sqlite_wal_recovery_workspace_builder& builder)
		{
			std::array<std::byte, sqlite_wal_source_capture_buffer_bound> buffer{};
			sqlite_incremental_sha256 digest;
			std::uint64_t offset{};
			while (offset < byte_count)
			{
				const auto wanted = static_cast<std::size_t>(
					std::min<std::uint64_t>(buffer.size(), byte_count - offset));
				std::uint64_t next{};
				if (wanted == 0U ||
					!sqlite_checked_add_u64(offset, static_cast<std::uint64_t>(wanted), next) ||
					next > byte_count)
					return unexpected(capture_binding_error("copy-accounting-overflow"));
				auto bytes = std::span{buffer}.first(wanted);
				if (auto read = source.read_exact(offset, bytes); !read)
					return unexpected(std::move(read.error()));
				if (auto updated = digest.update(bytes); !updated)
					return unexpected(std::move(updated.error()));
				if (auto appended = builder.append(role, bytes); !appended)
					return unexpected(std::move(appended.error()));
				offset = next;
			}
			auto checksum = digest.finish();
			if (!checksum)
				return unexpected(std::move(checksum.error()));
			return sqlite_backend_copy_receipt{byte_count, std::move(*checksum)};
		}

		[[nodiscard]] result<void> recheck_held_entry(const sqlite_backend_held_object& source)
		{
			auto replacement = source.recheck_current_entry();
			if (!replacement)
				return unexpected(std::move(replacement.error()));
			if (*replacement != sqlite_backend_replacement_state::exact_same_entry_and_object)
				return unexpected(capture_source_changed());
			return {};
		}

		[[nodiscard]] bool same_wal_scan(const sqlite_wal_scan_receipt& left,
										 const sqlite_wal_scan_receipt& right) noexcept
		{
			return left.classification == right.classification && left.stop == right.stop &&
				left.header == right.header && left.last_valid_frame == right.last_valid_frame &&
				left.last_valid_commit == right.last_valid_commit &&
				left.inspected_byte_count == right.inspected_byte_count &&
				left.validated_prefix_byte_count == right.validated_prefix_byte_count &&
				left.authoritative_prefix_byte_count == right.authoritative_prefix_byte_count &&
				left.valid_frame_count == right.valid_frame_count &&
				left.valid_commit_count == right.valid_commit_count &&
				left.torn_remainder_byte_count == right.torn_remainder_byte_count;
		}
	} // namespace

	result<sqlite_wal_source_capture>
	capture_sqlite_wal_source(const std::shared_ptr<const sqlite_backend_held_object>& held_main,
							  const std::shared_ptr<const sqlite_backend_held_object>& held_wal,
							  sqlite_wal_recovery_workspace_builder& workspace_builder)
	{
		if (!held_main || !held_wal ||
			held_main->role() != sqlite_backend_file_role::main_database ||
			held_wal->role() != sqlite_backend_file_role::write_ahead_log ||
			held_main->object_identity() == held_wal->object_identity() ||
			held_main->directory_entry_identity() == held_wal->directory_entry_identity())
			return unexpected(capture_binding_error("source-binding"));

		auto main_before = observe_full_receipt(*held_main);
		if (!main_before)
			return unexpected(std::move(main_before.error()));
		auto wal_before = observe_full_receipt(*held_wal);
		if (!wal_before)
			return unexpected(std::move(wal_before.error()));
		if (main_before->byte_count == 0U)
			return unexpected(capture_unrecognized_source());

		held_byte_source wal_source{*held_wal, wal_before->byte_count};
		auto wal_scan = scan_sqlite_wal(wal_source);
		if (!wal_scan)
			return unexpected(std::move(wal_scan.error()));
		if (wal_scan->classification != sqlite_wal_scan_classification::empty &&
			wal_scan->classification != sqlite_wal_scan_classification::committed_prefix &&
			wal_scan->classification != sqlite_wal_scan_classification::no_valid_commit)
			return unexpected(capture_unrecognized_source());

		auto copied_main = copy_exact_to_workspace(*held_main,
												   main_before->byte_count,
												   sqlite_wal_recovery_copy_role::main_database,
												   workspace_builder);
		if (!copied_main)
			return unexpected(std::move(copied_main.error()));
		if (*copied_main != *main_before)
			return unexpected(capture_source_changed());

		auto copied_wal =
			copy_exact_to_workspace(*held_wal,
									wal_scan->authoritative_prefix_byte_count,
									sqlite_wal_recovery_copy_role::authoritative_wal_prefix,
									workspace_builder);
		if (!copied_wal)
			return unexpected(std::move(copied_wal.error()));

		if (auto stable = recheck_held_entry(*held_main); !stable)
			return unexpected(std::move(stable.error()));
		if (auto stable = recheck_held_entry(*held_wal); !stable)
			return unexpected(std::move(stable.error()));
		auto main_after = observe_full_receipt(*held_main);
		if (!main_after)
			return unexpected(std::move(main_after.error()));
		auto wal_after = observe_full_receipt(*held_wal);
		if (!wal_after)
			return unexpected(std::move(wal_after.error()));
		if (*main_after != *main_before || *wal_after != *wal_before)
			return unexpected(capture_source_changed());

		auto workspace = workspace_builder.seal({*main_before, *copied_wal, *wal_scan});
		if (!workspace)
			return unexpected(std::move(workspace.error()));
		if (!*workspace)
			return unexpected(capture_binding_error("workspace-pin"));
		if (auto verified = (*workspace)->verify_sealed_objects(); !verified)
			return unexpected(std::move(verified.error()));
		auto workspace_receipt = (*workspace)->snapshot_receipt();
		if (!workspace_receipt)
			return unexpected(std::move(workspace_receipt.error()));
		if (!workspace_receipt->sealed || workspace_receipt->main_database != *main_before ||
			workspace_receipt->authoritative_wal_prefix != *copied_wal ||
			workspace_receipt->private_wal_present != (copied_wal->byte_count != 0U) ||
			!same_wal_scan(workspace_receipt->source_wal_scan, *wal_scan))
			return unexpected(capture_binding_error("workspace-receipt"));

		return sqlite_wal_source_capture{std::move(*main_before),
										 std::move(*wal_before),
										 *wal_scan,
										 std::move(*workspace_receipt),
										 std::move(*workspace)};
	}
} // namespace cxxlens::sdk
