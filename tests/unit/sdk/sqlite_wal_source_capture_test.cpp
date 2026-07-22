#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sdk/sqlite_payload_streaming_internal.hpp"
#include "sdk/sqlite_wal_source_capture_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
			throw std::runtime_error{std::string{message}};
	}

	[[nodiscard]] error test_error(const std::string_view detail)
	{
		return {"test.failure", "sqlite-wal-source-capture", std::string{detail}};
	}

	[[nodiscard]] sqlite_backend_opaque_identity identity(const std::string_view label)
	{
		sqlite_backend_opaque_identity output{"test.identity.v1", {}};
		for (const auto byte : label)
			output.bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
		require(!output.bytes.empty(), "test identity must not be empty");
		return output;
	}

	[[nodiscard]] std::string digest_bytes(const std::span<const std::byte> bytes)
	{
		sqlite_incremental_sha256 digest;
		if (!bytes.empty())
			require(digest.update(bytes).has_value(), "hash test bytes");
		auto finished = digest.finish();
		require(finished.has_value(), "finish test hash");
		return std::move(*finished);
	}

	enum class recheck_mutation : std::uint8_t
	{
		none,
		grow,
		flip_first_byte,
	};

	class fake_held_object final : public sqlite_backend_held_object
	{
	  public:
		fake_held_object(const sqlite_backend_file_role role,
						 const std::string& label,
						 std::vector<std::byte> bytes)
			: role_{role}, object_identity_{identity(std::string{label} + ".object")},
			  entry_identity_{identity(std::string{label} + ".entry")}, bytes_{std::move(bytes)}
		{
		}

		[[nodiscard]] sqlite_backend_file_role role() const noexcept override
		{
			return role_;
		}

		[[nodiscard]] const sqlite_backend_opaque_identity&
		object_identity() const noexcept override
		{
			return object_identity_;
		}

		[[nodiscard]] const sqlite_backend_opaque_identity&
		directory_entry_identity() const noexcept override
		{
			return entry_identity_;
		}

		[[nodiscard]] result<std::uint64_t> size() const override
		{
			++size_calls;
			if (fail_size_call && size_calls == *fail_size_call)
				return unexpected(test_error("size-io"));
			return static_cast<std::uint64_t>(bytes_.size());
		}

		[[nodiscard]] result<void> read_exact(const std::uint64_t offset,
											  const std::span<std::byte> output) const override
		{
			++read_calls;
			maximum_read_request = std::max(maximum_read_request, output.size());
			if (fail_read_call && read_calls == *fail_read_call)
				return unexpected(test_error("read-io"));
			std::uint64_t end{};
			if (!sqlite_checked_add_u64(offset, static_cast<std::uint64_t>(output.size()), end) ||
				end > static_cast<std::uint64_t>(bytes_.size()))
				return unexpected(test_error("read-range"));
			std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
						output.size(),
						output.begin());
			return {};
		}

		[[nodiscard]] result<std::string> sha256() const override
		{
			++sha_calls;
			if (fail_sha_call && sha_calls == *fail_sha_call)
				return unexpected(test_error("sha-io"));
			return digest_bytes(bytes_);
		}

		[[nodiscard]] result<std::shared_ptr<sqlite_backend_private_snapshot>>
		copy_exact(sqlite_backend_private_snapshot_builder&, std::span<std::byte>) const override
		{
			return unexpected(test_error("unexpected-private-snapshot-copy"));
		}

		[[nodiscard]] result<sqlite_backend_replacement_state>
		recheck_current_entry() const override
		{
			++recheck_calls;
			if (fail_recheck)
				return unexpected(test_error("recheck-io"));
			if (mutation == recheck_mutation::grow)
				bytes_.push_back(std::byte{0x7fU});
			else if (mutation == recheck_mutation::flip_first_byte && !bytes_.empty())
				bytes_.front() ^= std::byte{1U};
			return replacement;
		}

		mutable std::size_t size_calls{};
		mutable std::size_t sha_calls{};
		mutable std::size_t read_calls{};
		mutable std::size_t recheck_calls{};
		mutable std::size_t maximum_read_request{};
		std::optional<std::size_t> fail_size_call;
		std::optional<std::size_t> fail_sha_call;
		std::optional<std::size_t> fail_read_call;
		bool fail_recheck{};
		sqlite_backend_replacement_state replacement{
			sqlite_backend_replacement_state::exact_same_entry_and_object};
		recheck_mutation mutation{recheck_mutation::none};

	  private:
		sqlite_backend_file_role role_{};
		sqlite_backend_opaque_identity object_identity_;
		sqlite_backend_opaque_identity entry_identity_;
		mutable std::vector<std::byte> bytes_;
	};

	class fake_workspace final : public sqlite_wal_recovery_workspace
	{
	  public:
		fake_workspace(sqlite_wal_recovery_workspace_receipt receipt, const bool fail_verify)
			: receipt_{std::move(receipt)}, fail_verify_{fail_verify}
		{
		}

		[[nodiscard]] std::string_view database_path() const noexcept override
		{
			return "/test/private-main";
		}

		[[nodiscard]] std::string_view registered_vfs_name() const noexcept override
		{
			return "test-private-vfs";
		}

		[[nodiscard]] const void* vfs_implementation_identity() const noexcept override
		{
			return this;
		}

		[[nodiscard]] result<sqlite_wal_recovery_workspace_receipt>
		snapshot_receipt() const override
		{
			return receipt_;
		}

		[[nodiscard]] result<void> verify_sealed_objects() const override
		{
			if (fail_verify_)
				return unexpected(test_error("workspace-verify"));
			return {};
		}

	  private:
		sqlite_wal_recovery_workspace_receipt receipt_;
		bool fail_verify_{};
	};

	class fake_workspace_builder final : public sqlite_wal_recovery_workspace_builder
	{
	  public:
		[[nodiscard]] result<void> append(const sqlite_wal_recovery_copy_role role,
										  const std::span<const std::byte> bytes) override
		{
			++append_calls;
			maximum_append_request = std::max(maximum_append_request, bytes.size());
			if (fail_append_call && append_calls == *fail_append_call)
				return unexpected(test_error("builder-append"));
			auto& output =
				role == sqlite_wal_recovery_copy_role::main_database ? main_bytes : wal_bytes;
			output.insert(output.end(), bytes.begin(), bytes.end());
			return {};
		}

		[[nodiscard]] result<std::shared_ptr<sqlite_wal_recovery_workspace>>
		seal(sqlite_wal_recovery_workspace_expectation expectation) override
		{
			++seal_calls;
			sealed_expectation = expectation;
			if (fail_seal)
				return unexpected(test_error("builder-seal"));
			sqlite_wal_recovery_workspace_receipt receipt{
				"test.wal-recovery-workspace.v1",
				identity("source-capability"),
				expectation.main_database,
				expectation.authoritative_wal_prefix,
				expectation.source_wal_scan,
				{},
				{},
				{},
				!wal_bytes.empty(),
				true,
			};
			if (mismatch_receipt)
				receipt.main_database.byte_count += 1U;
			std::shared_ptr<sqlite_wal_recovery_workspace> output =
				std::make_shared<fake_workspace>(std::move(receipt), fail_verify);
			return output;
		}

		std::vector<std::byte> main_bytes;
		std::vector<std::byte> wal_bytes;
		std::optional<std::size_t> fail_append_call;
		std::optional<sqlite_wal_recovery_workspace_expectation> sealed_expectation;
		std::size_t append_calls{};
		std::size_t seal_calls{};
		std::size_t maximum_append_request{};
		bool fail_seal{};
		bool fail_verify{};
		bool mismatch_receipt{};
	};

	struct wal_checksum
	{
		std::uint32_t first{};
		std::uint32_t second{};
	};

	void append_big_endian(std::vector<std::byte>& output, const std::uint32_t value)
	{
		output.push_back(static_cast<std::byte>((value >> 24U) & 0xffU));
		output.push_back(static_cast<std::byte>((value >> 16U) & 0xffU));
		output.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
		output.push_back(static_cast<std::byte>(value & 0xffU));
	}

	[[nodiscard]] std::uint32_t little_endian_word(const std::span<const std::byte> bytes)
	{
		return std::to_integer<std::uint32_t>(bytes[0]) |
			(std::to_integer<std::uint32_t>(bytes[1]) << 8U) |
			(std::to_integer<std::uint32_t>(bytes[2]) << 16U) |
			(std::to_integer<std::uint32_t>(bytes[3]) << 24U);
	}

	void update_checksum(wal_checksum& checksum, const std::span<const std::byte> bytes)
	{
		require(bytes.size() % 8U == 0U, "checksum input alignment");
		for (std::size_t offset{}; offset < bytes.size(); offset += 8U)
		{
			checksum.first += little_endian_word(bytes.subspan(offset, 4U)) + checksum.second;
			checksum.second += little_endian_word(bytes.subspan(offset + 4U, 4U)) + checksum.first;
		}
	}

	[[nodiscard]] std::vector<std::byte> wal_header(wal_checksum& checksum)
	{
		std::vector<std::byte> output;
		append_big_endian(output, sqlite_wal_little_endian_checksum_magic);
		append_big_endian(output, sqlite_wal_format_version);
		append_big_endian(output, 512U);
		append_big_endian(output, 7U);
		append_big_endian(output, 0x11223344U);
		append_big_endian(output, 0x55667788U);
		update_checksum(checksum, output);
		append_big_endian(output, checksum.first);
		append_big_endian(output, checksum.second);
		return output;
	}

	[[nodiscard]] std::vector<std::byte> committed_wal()
	{
		wal_checksum checksum;
		auto output = wal_header(checksum);
		std::vector<std::byte> frame_header;
		append_big_endian(frame_header, 1U);
		append_big_endian(frame_header, 1U);
		append_big_endian(frame_header, 0x11223344U);
		append_big_endian(frame_header, 0x55667788U);
		std::vector<std::byte> page(512U);
		for (std::size_t index{}; index < page.size(); ++index)
			page[index] = static_cast<std::byte>((index * 19U + 3U) & 0xffU);
		update_checksum(checksum, std::span<const std::byte>{frame_header}.first(8U));
		update_checksum(checksum, page);
		append_big_endian(frame_header, checksum.first);
		append_big_endian(frame_header, checksum.second);
		output.insert(output.end(), frame_header.begin(), frame_header.end());
		output.insert(output.end(), page.begin(), page.end());
		return output;
	}

	[[nodiscard]] std::vector<std::byte> main_bytes()
	{
		std::vector<std::byte> output(sqlite_wal_source_capture_buffer_bound * 2U + 137U);
		for (std::size_t index{}; index < output.size(); ++index)
			output[index] = static_cast<std::byte>((index * 31U + 11U) & 0xffU);
		return output;
	}

	struct fixture_pair
	{
		std::shared_ptr<fake_held_object> main;
		std::shared_ptr<fake_held_object> wal;
	};

	[[nodiscard]] fixture_pair make_pair(std::vector<std::byte> wal)
	{
		return {std::make_shared<fake_held_object>(
					sqlite_backend_file_role::main_database, "main", main_bytes()),
				std::make_shared<fake_held_object>(
					sqlite_backend_file_role::write_ahead_log, "wal", std::move(wal))};
	}

	void verify_committed_torn_suffix_capture()
	{
		auto authoritative_wal = committed_wal();
		auto source_wal = authoritative_wal;
		for (std::uint8_t value = 1U; value <= 17U; ++value)
			source_wal.push_back(static_cast<std::byte>(value));
		auto fixture = make_pair(source_wal);
		fake_workspace_builder builder;
		auto captured = capture_sqlite_wal_source(fixture.main, fixture.wal, builder);
		require(captured.has_value() && captured->workspace && captured->workspace_receipt.sealed,
				"committed WAL capture failed");
		require(captured->full_source_main.byte_count == builder.main_bytes.size() &&
					captured->full_source_main.sha256 == digest_bytes(builder.main_bytes) &&
					captured->full_source_wal.byte_count == source_wal.size() &&
					captured->full_source_wal.sha256 == digest_bytes(source_wal),
				"full source receipts are not exact");
		require(
			captured->wal_scan.classification == sqlite_wal_scan_classification::committed_prefix &&
				captured->wal_scan.stop == sqlite_wal_scan_stop::torn_frame &&
				captured->wal_scan.authoritative_prefix_byte_count == authoritative_wal.size() &&
				builder.wal_bytes == authoritative_wal &&
				captured->workspace_receipt.authoritative_wal_prefix.byte_count ==
					authoritative_wal.size() &&
				captured->workspace_receipt.private_wal_present,
			"torn suffix entered the authoritative workspace prefix");
		require(fixture.main->size_calls == 2U && fixture.main->sha_calls == 2U &&
					fixture.main->recheck_calls == 1U && fixture.wal->size_calls == 2U &&
					fixture.wal->sha_calls == 2U && fixture.wal->recheck_calls == 1U &&
					fixture.main->maximum_read_request <= sqlite_wal_source_capture_buffer_bound &&
					fixture.wal->maximum_read_request <= sqlite_wal_source_capture_buffer_bound &&
					builder.maximum_append_request <= sqlite_wal_source_capture_buffer_bound &&
					builder.seal_calls == 1U && builder.sealed_expectation.has_value(),
				"capture did not use bounded reads or exact before/after rechecks");
	}

	void verify_no_commit_prefix_is_empty()
	{
		wal_checksum checksum;
		auto fixture = make_pair(wal_header(checksum));
		fake_workspace_builder builder;
		auto captured = capture_sqlite_wal_source(fixture.main, fixture.wal, builder);
		require(captured.has_value() &&
					captured->wal_scan.classification ==
						sqlite_wal_scan_classification::no_valid_commit &&
					captured->wal_scan.authoritative_prefix_byte_count == 0U &&
					builder.wal_bytes.empty() &&
					captured->workspace_receipt.authoritative_wal_prefix.byte_count == 0U &&
					!captured->workspace_receipt.private_wal_present,
				"valid no-commit WAL copied non-authoritative bytes");
	}

	void verify_empty_wal_uses_main_only()
	{
		auto fixture = make_pair({});
		fake_workspace_builder builder;
		auto captured = capture_sqlite_wal_source(fixture.main, fixture.wal, builder);
		require(captured.has_value() &&
				captured->wal_scan.classification == sqlite_wal_scan_classification::empty &&
				captured->wal_scan.authoritative_prefix_byte_count == 0U &&
				captured->full_source_wal.byte_count == 0U && builder.wal_bytes.empty() &&
				captured->workspace_receipt.authoritative_wal_prefix.byte_count == 0U &&
				!captured->workspace_receipt.private_wal_present,
			"empty WAL did not preserve an exact main-only recovery receipt");
	}

	void verify_drift_failures()
	{
		{
			auto fixture = make_pair(committed_wal());
			fixture.main->mutation = recheck_mutation::grow;
			fake_workspace_builder builder;
			auto captured = capture_sqlite_wal_source(fixture.main, fixture.wal, builder);
			require(!captured && captured.error().detail == "concurrent-source-change" &&
						builder.seal_calls == 0U,
					"main size drift was not distinguished before seal");
		}
		{
			auto fixture = make_pair(committed_wal());
			fixture.wal->mutation = recheck_mutation::flip_first_byte;
			fake_workspace_builder builder;
			auto captured = capture_sqlite_wal_source(fixture.main, fixture.wal, builder);
			require(!captured && captured.error().detail == "concurrent-source-change" &&
						builder.seal_calls == 0U,
					"WAL same-size hash drift was not distinguished before seal");
		}
		{
			auto fixture = make_pair(committed_wal());
			fixture.wal->replacement = sqlite_backend_replacement_state::replaced;
			fake_workspace_builder builder;
			auto captured = capture_sqlite_wal_source(fixture.main, fixture.wal, builder);
			require(!captured && captured.error().detail == "concurrent-source-change" &&
						builder.seal_calls == 0U,
					"held WAL entry replacement was not distinguished before seal");
		}
	}

	void verify_io_and_builder_failures()
	{
		{
			auto fixture = make_pair(committed_wal());
			fixture.main->fail_size_call = 1U;
			fake_workspace_builder builder;
			auto captured = capture_sqlite_wal_source(fixture.main, fixture.wal, builder);
			require(!captured && captured.error().code == "test.failure" &&
						captured.error().detail == "size-io" &&
						captured.error().detail != "concurrent-source-change" &&
						builder.append_calls == 0U && builder.seal_calls == 0U,
					"source size I/O failure was guessed to be drift");
		}
		{
			auto fixture = make_pair(committed_wal());
			fixture.wal->fail_read_call = 1U;
			fake_workspace_builder builder;
			auto captured = capture_sqlite_wal_source(fixture.main, fixture.wal, builder);
			require(!captured && captured.error().code == "test.failure" &&
						captured.error().detail == "read-io" &&
						captured.error().detail != "concurrent-source-change",
					"WAL read I/O failure was guessed to be drift");
		}
		{
			auto fixture = make_pair(committed_wal());
			fixture.main->fail_sha_call = 2U;
			fake_workspace_builder builder;
			auto captured = capture_sqlite_wal_source(fixture.main, fixture.wal, builder);
			require(!captured && captured.error().detail == "sha-io" &&
						captured.error().detail != "concurrent-source-change" &&
						builder.seal_calls == 0U,
					"after-copy hash I/O failure was guessed to be drift");
		}
		{
			auto fixture = make_pair(committed_wal());
			fake_workspace_builder builder;
			builder.fail_append_call = 1U;
			auto captured = capture_sqlite_wal_source(fixture.main, fixture.wal, builder);
			require(!captured && captured.error().detail == "builder-append" &&
						builder.seal_calls == 0U,
					"builder append failure was not preserved");
		}
		{
			auto fixture = make_pair(committed_wal());
			fake_workspace_builder builder;
			builder.fail_seal = true;
			auto captured = capture_sqlite_wal_source(fixture.main, fixture.wal, builder);
			require(!captured && captured.error().detail == "builder-seal" &&
						builder.seal_calls == 1U,
					"builder seal failure was not preserved");
		}
		{
			auto fixture = make_pair(committed_wal());
			fake_workspace_builder builder;
			builder.fail_verify = true;
			auto captured = capture_sqlite_wal_source(fixture.main, fixture.wal, builder);
			require(!captured && captured.error().detail == "workspace-verify",
					"workspace verification failure was not preserved");
		}
	}
} // namespace

int main()
{
	try
	{
		require(sqlite_wal_source_capture_buffer_bound == 65'536U,
				"source capture buffer bound drifted");
		verify_committed_torn_suffix_capture();
		verify_no_commit_prefix_is_empty();
		verify_empty_wal_uses_main_only();
		verify_drift_failures();
		verify_io_and_builder_failures();
		return 0;
	}
	catch (const std::exception& failure)
	{
		std::cerr << failure.what() << '\n';
		return 1;
	}
}
