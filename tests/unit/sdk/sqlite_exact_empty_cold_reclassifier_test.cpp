#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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

#include "sdk/sqlite_exact_empty_cold_reclassifier_internal.hpp"

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
		return {"test.failure", "sqlite-exact-empty-cold", std::string{detail}};
	}

	[[nodiscard]] sqlite_backend_opaque_identity identity(const std::string_view label)
	{
		sqlite_backend_opaque_identity output{"test.identity.v1", {}};
		for (const auto value : label)
			output.bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
		require(!output.bytes.empty(), "test identity must not be empty");
		return output;
	}

	void write_be_u32(const std::span<std::byte> bytes,
					  const std::size_t offset,
					  const std::uint32_t value)
	{
		bytes[offset] = static_cast<std::byte>((value >> 24U) & 0xffU);
		bytes[offset + 1U] = static_cast<std::byte>((value >> 16U) & 0xffU);
		bytes[offset + 2U] = static_cast<std::byte>((value >> 8U) & 0xffU);
		bytes[offset + 3U] = static_cast<std::byte>(value & 0xffU);
	}

	[[nodiscard]] std::uint32_t read_be_u32(const std::span<const std::byte> bytes,
											const std::size_t offset)
	{
		return (std::to_integer<std::uint32_t>(bytes[offset]) << 24U) |
			(std::to_integer<std::uint32_t>(bytes[offset + 1U]) << 16U) |
			(std::to_integer<std::uint32_t>(bytes[offset + 2U]) << 8U) |
			std::to_integer<std::uint32_t>(bytes[offset + 3U]);
	}

	[[nodiscard]] std::uint32_t pager_record_checksum(const std::span<const std::byte> page,
													  const std::uint32_t nonce)
	{
		auto checksum = nonce;
		if (page.size() < 200U)
			return checksum;
		for (std::size_t index = page.size() - 200U; index > 0U;)
		{
			checksum += std::to_integer<std::uint32_t>(page[index]);
			if (index <= 200U)
				break;
			index -= 200U;
		}
		return checksum;
	}

	[[nodiscard]] std::vector<std::byte> exact_main(const bool post,
													const std::uint32_t change_counter = 1U)
	{
		constexpr std::uint32_t page_size = 4096U;
		std::vector<std::byte> bytes(page_size, std::byte{});
		constexpr std::string_view magic = "SQLite format 3\0";
		for (std::size_t index{}; index < magic.size(); ++index)
			bytes[index] = static_cast<std::byte>(static_cast<unsigned char>(magic[index]));
		bytes[16U] = std::byte{0x10U};
		bytes[17U] = std::byte{0x00U};
		bytes[18U] = post ? std::byte{1U} : std::byte{2U};
		bytes[19U] = post ? std::byte{1U} : std::byte{2U};
		bytes[21U] = std::byte{64U};
		bytes[22U] = std::byte{32U};
		bytes[23U] = std::byte{32U};
		write_be_u32(bytes,
					 24U,
					 post ? (change_counter == std::numeric_limits<std::uint32_t>::max()
								 ? std::uint32_t{}
								 : change_counter + 1U)
						  : change_counter);
		write_be_u32(bytes, 28U, 1U);
		write_be_u32(bytes, 44U, 4U);
		write_be_u32(bytes, 56U, 1U);
		if (post)
			write_be_u32(bytes,
						 92U,
						 change_counter == std::numeric_limits<std::uint32_t>::max()
							 ? std::uint32_t{}
							 : change_counter + 1U);
		bytes[100U] = std::byte{0x0dU};
		bytes[105U] = std::byte{0x10U};
		bytes[106U] = std::byte{};
		return bytes;
	}

	[[nodiscard]] std::vector<std::byte> exact_main_with_page_count(const bool post,
																	const std::uint32_t page_count)
	{
		require(page_count >= 1U, "page count fixture must be positive");
		auto bytes = exact_main(post);
		bytes.resize(bytes.size() * page_count, std::byte{});
		write_be_u32(bytes, 28U, page_count);
		write_be_u32(bytes, 36U, page_count - 1U);
		if (page_count > 1U)
		{
			write_be_u32(bytes, 32U, 2U);
			const auto trunk = std::size_t{4096U};
			write_be_u32(bytes, trunk + 4U, page_count - 2U);
			for (std::uint32_t index{3U}; index <= page_count; ++index)
				write_be_u32(bytes, trunk + 8U + (index - 3U) * 4U, index);
		}
		return bytes;
	}

	[[nodiscard]] std::vector<std::byte> hot_journal(const std::span<const std::byte> image,
													 const std::uint32_t sector_size = 512U,
													 const bool opaque_padding = false)
	{
		constexpr std::uint32_t nonce = 42U;
		constexpr std::uint32_t page_size = 4096U;
		const auto page_count = read_be_u32(image, 28U);
		const auto pages_per_sector = sector_size > page_size ? sector_size / page_size : 1U;
		const auto upper = std::min(page_count, pages_per_sector);
		std::vector<std::uint32_t> record_pages;
		for (std::uint32_t page{1U}; page <= upper; ++page)
			if (page != 0x40000000U / page_size + 1U)
				record_pages.push_back(page);
		const auto record_size = std::size_t{page_size} + 8U;
		std::vector<std::byte> journal(sector_size + record_pages.size() * record_size,
									   std::byte{});
		constexpr std::array<std::byte, 8U> magic{std::byte{0xd9U},
												  std::byte{0xd5U},
												  std::byte{0x05U},
												  std::byte{0xf9U},
												  std::byte{0x20U},
												  std::byte{0xa1U},
												  std::byte{0x63U},
												  std::byte{0xd7U}};
		std::ranges::copy(magic, journal.begin());
		write_be_u32(journal, 8U, static_cast<std::uint32_t>(record_pages.size()));
		write_be_u32(journal, 12U, nonce);
		write_be_u32(journal, 16U, page_count);
		write_be_u32(journal, 20U, sector_size);
		write_be_u32(journal, 24U, page_size);
		if (opaque_padding)
		{
			for (std::size_t offset{28U}; offset < sector_size; ++offset)
				journal[offset] = static_cast<std::byte>((offset * 17U) & 0xffU);
		}
		for (std::size_t index{}; index < record_pages.size(); ++index)
		{
			const auto page = record_pages[index];
			const auto record = static_cast<std::size_t>(sector_size) + index * record_size;
			write_be_u32(journal, record, page);
			const auto image_offset = static_cast<std::size_t>(page - 1U) * page_size;
			const auto source = image.subspan(image_offset, page_size);
			std::ranges::copy(source, journal.begin() + record + 4U);
			write_be_u32(journal, record + 4U + page_size, pager_record_checksum(source, nonce));
		}
		return journal;
	}

	[[nodiscard]] std::vector<std::byte> journal_prefix(const std::uint32_t sector_size,
														const bool opaque_padding)
	{
		constexpr std::uint32_t page_size = 4096U;
		constexpr std::uint32_t nonce = 42U;
		std::vector<std::byte> journal(sector_size, std::byte{});
		write_be_u32(journal, 12U, nonce);
		write_be_u32(journal, 16U, 1U);
		write_be_u32(journal, 20U, sector_size);
		write_be_u32(journal, 24U, page_size);
		if (opaque_padding)
		{
			for (std::size_t offset{28U}; offset < sector_size; ++offset)
				journal[offset] = static_cast<std::byte>((offset * 29U) & 0xffU);
		}
		return journal;
	}

	[[nodiscard]] std::vector<std::byte> invalidated_journal(const std::span<const std::byte> image)
	{
		constexpr std::uint32_t sector_size = 512U;
		constexpr std::uint32_t page_size = 4096U;
		constexpr std::uint32_t nonce = 42U;
		std::vector<std::byte> journal(sector_size + page_size + 8U, std::byte{});
		write_be_u32(journal, sector_size, 1U);
		std::ranges::copy(image, journal.begin() + sector_size + 4U);
		write_be_u32(journal, sector_size + 4U + page_size, pager_record_checksum(image, nonce));
		return journal;
	}

	class fake_held_object final : public sqlite_backend_held_object
	{
	  public:
		fake_held_object(const sqlite_backend_file_role role,
						 const std::string_view label,
						 std::vector<std::byte> bytes)
			: role_{role}, object_identity_{identity(std::string{label} + ".object")},
			  entry_identity_{identity(std::string{label} + ".entry")},
			  filesystem_profile_{identity("filesystem")}, mount_identity_{identity("mount")},
			  bytes_{std::move(bytes)}
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

		[[nodiscard]] const std::optional<sqlite_backend_opaque_identity>&
		object_filesystem_profile() const noexcept override
		{
			return filesystem_profile_;
		}

		[[nodiscard]] const std::optional<sqlite_backend_opaque_identity>&
		object_mount_identity() const noexcept override
		{
			return mount_identity_;
		}

		[[nodiscard]] result<void> recheck_retained_object() const override
		{
			if (retained_recheck_fails)
				return unexpected(test_error("retained-recheck"));
			return {};
		}

		[[nodiscard]] result<std::uint64_t> size() const override
		{
			return static_cast<std::uint64_t>(bytes_.size());
		}

		[[nodiscard]] result<void> read_exact(const std::uint64_t offset,
											  const std::span<std::byte> output) const override
		{
			if (throw_on_read)
				throw std::runtime_error{"injected read allocation/resource failure"};
			if (offset > bytes_.size() || output.size() > bytes_.size() - offset)
				return unexpected(test_error("read-range"));
			std::ranges::copy(bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
							  bytes_.begin() + static_cast<std::ptrdiff_t>(offset + output.size()),
							  output.begin());
			if (drift_after_first_read && !output.empty() && read_count_++ == 0U)
				bytes_[0] ^= std::byte{1U};
			return {};
		}

		[[nodiscard]] result<std::string> sha256() const override
		{
			return unexpected(test_error("unexpected-hash"));
		}

		[[nodiscard]] result<std::shared_ptr<sqlite_backend_private_snapshot>>
		copy_exact(sqlite_backend_private_snapshot_builder&, std::span<std::byte>) const override
		{
			return unexpected(test_error("unexpected-copy"));
		}

		[[nodiscard]] result<sqlite_backend_replacement_state>
		recheck_current_entry() const override
		{
			return replacement;
		}

		bool retained_recheck_fails{};
		bool throw_on_read{};
		bool drift_after_first_read{};
		sqlite_backend_replacement_state replacement{
			sqlite_backend_replacement_state::exact_same_entry_and_object};

	  private:
		sqlite_backend_file_role role_{};
		sqlite_backend_opaque_identity object_identity_;
		sqlite_backend_opaque_identity entry_identity_;
		std::optional<sqlite_backend_opaque_identity> filesystem_profile_;
		std::optional<sqlite_backend_opaque_identity> mount_identity_;
		mutable std::size_t read_count_{};
		mutable std::vector<std::byte> bytes_;
	};

	class fake_guard final : public sqlite_source_shm_namespace_guard
	{
	  public:
		[[nodiscard]] std::string_view logical_main_locator() const noexcept override
		{
			return "test-main";
		}

		[[nodiscard]] std::string_view anchored_main_locator() const noexcept override
		{
			return "test-main";
		}

		[[nodiscard]] const sqlite_backend_opaque_identity& identity() const noexcept override
		{
			return identity_;
		}

		[[nodiscard]] result<sqlite_backend_entry_observation>
		retained_entry(const sqlite_backend_file_role) const override
		{
			return unexpected(test_error("unexpected-retained-entry"));
		}

		[[nodiscard]] result<void> recheck() const override
		{
			if (!stable)
				return unexpected(test_error("namespace-drift"));
			return {};
		}

		[[nodiscard]] result<void> claim_target_epoch() override
		{
			return unexpected(test_error("classifier-must-not-claim"));
		}

		[[nodiscard]] result<void> finish() override
		{
			return unexpected(test_error("classifier-must-not-finish"));
		}

		bool stable{true};

	  private:
		sqlite_backend_opaque_identity identity_{::identity("namespace")};
	};

	struct fixture
	{
		std::shared_ptr<fake_held_object> main;
		std::shared_ptr<fake_held_object> wal;
		std::shared_ptr<fake_held_object> shm;
		std::shared_ptr<fake_held_object> journal;
		std::shared_ptr<fake_guard> guard;
		sqlite_backend_namespace_census census;
	};

	[[nodiscard]] sqlite_backend_entry_observation absent(const sqlite_backend_file_role role)
	{
		return {role, sqlite_backend_entry_state::absent, {}, {}, {}, {}, false};
	}

	[[nodiscard]] sqlite_backend_entry_observation
	retained(const std::shared_ptr<fake_held_object>& object)
	{
		return {object->role(),
				sqlite_backend_entry_state::held_regular,
				object->object_identity(),
				object->directory_entry_identity(),
				object,
				*object->object_filesystem_profile(),
				true};
	}

	[[nodiscard]] fixture
	make_fixture(const std::vector<std::byte>& main_bytes,
				 std::optional<std::vector<std::byte>> wal_bytes = std::nullopt,
				 std::optional<std::vector<std::byte>> journal_bytes = std::nullopt,
				 const bool shared_memory = false)
	{
		fixture output;
		output.main = std::make_shared<fake_held_object>(
			sqlite_backend_file_role::main_database, "main", main_bytes);
		if (wal_bytes)
			output.wal = std::make_shared<fake_held_object>(
				sqlite_backend_file_role::write_ahead_log, "wal", std::move(*wal_bytes));
		if (journal_bytes)
			output.journal = std::make_shared<fake_held_object>(
				sqlite_backend_file_role::rollback_journal, "journal", std::move(*journal_bytes));
		if (shared_memory)
			output.shm = std::make_shared<fake_held_object>(
				sqlite_backend_file_role::shared_memory, "shm", std::vector<std::byte>{});
		output.guard = std::make_shared<fake_guard>();
		output.census.profile = "default-filesystem-v1";
		output.census.entries = {
			retained(output.main),
			output.wal ? retained(output.wal) : absent(sqlite_backend_file_role::write_ahead_log),
			output.shm ? retained(output.shm) : absent(sqlite_backend_file_role::shared_memory),
			output.journal ? retained(output.journal)
						   : absent(sqlite_backend_file_role::rollback_journal),
		};
		output.census.source_shm_guard = output.guard;
		return output;
	}

	[[nodiscard]] sqlite_exact_empty_cold_classification
	classify(const fixture& value, const std::uint64_t maximum = 512U * 1024U * 1024U)
	{
		auto result = classify_sqlite_exact_empty_cold_observation(value.census, maximum);
		require(result.has_value(), "classifier unexpectedly returned a transport error");
		return *result;
	}

	void require_family(const fixture& value,
						const sqlite_exact_empty_cold_family expected,
						const sqlite_exact_empty_cold_route route,
						const std::string_view message)
	{
		const auto output = classify(value);
		require(output.failure == sqlite_exact_empty_cold_failure::none && output.observation,
				message);
		require(output.observation->family == expected && output.observation->route == route,
				message);
	}

	void check_seven_way_partition()
	{
		const auto pre = exact_main(false);
		const auto post = exact_main(true);
		require_family(make_fixture(pre),
					   sqlite_exact_empty_cold_family::f0,
					   sqlite_exact_empty_cold_route::live_normalizer,
					   "F0 exact current bytes were not partitioned");
		require_family(make_fixture(pre, std::vector<std::byte>{}),
					   sqlite_exact_empty_cold_family::fz_pre,
					   sqlite_exact_empty_cold_route::live_normalizer,
					   "FZ-pre exact zero WAL was not partitioned");
		require_family(make_fixture(post, std::vector<std::byte>{}),
					   sqlite_exact_empty_cold_family::fz_post,
					   sqlite_exact_empty_cold_route::fresh_rollback_read,
					   "FZ-post was not kept on the fresh rollback route");
		require_family(make_fixture(pre, std::nullopt, std::vector<std::byte>{}),
					   sqlite_exact_empty_cold_family::fp,
					   sqlite_exact_empty_cold_route::cleanup_then_fresh_read,
					   "FP empty journal prefix was not partitioned");
		require_family(make_fixture(pre, std::nullopt, hot_journal(pre)),
					   sqlite_exact_empty_cold_family::fh,
					   sqlite_exact_empty_cold_route::cleanup_then_fresh_read,
					   "FH hot journal was not partitioned");
		require_family(make_fixture(post, std::nullopt, invalidated_journal(pre)),
					   sqlite_exact_empty_cold_family::fi,
					   sqlite_exact_empty_cold_route::fresh_rollback_read,
					   "FI invalidated journal was not partitioned");
		require_family(make_fixture(post),
					   sqlite_exact_empty_cold_family::fo,
					   sqlite_exact_empty_cold_route::fresh_rollback_read,
					   "FO post bytes were not partitioned");
	}

	void check_sector_vectors_and_opaque_padding()
	{
		const auto pre = exact_main(false);
		const auto post = exact_main(true);
		for (const auto sector_size : {32U, 512U, 4096U, 65'536U})
		{
			auto hot = make_fixture(pre, std::nullopt, hot_journal(pre, sector_size, true));
			const auto output = classify(hot);
			require(output.failure == sqlite_exact_empty_cold_failure::none && output.observation,
					"legal journal sector was not classified");
			require(output.observation->family == sqlite_exact_empty_cold_family::fh &&
						output.observation->sector_size == sector_size &&
						output.observation->journal_record_count == 1U,
					"S/P journal vector or opaque padding changed classification");

			auto prefix = make_fixture(pre, std::nullopt, journal_prefix(sector_size, true));
			const auto prefix_output = classify(prefix);
			require(prefix_output.failure == sqlite_exact_empty_cold_failure::none &&
						prefix_output.observation &&
						prefix_output.observation->family == sqlite_exact_empty_cold_family::fp &&
						prefix_output.observation->sector_size == sector_size,
					"opaque journal prefix padding or sector boundary was rejected");
		}

		const auto large_pre = exact_main_with_page_count(false, 17U);
		const auto large_hot =
			make_fixture(large_pre, std::nullopt, hot_journal(large_pre, 65'536U));
		const auto large_output = classify(large_hot);
		require(large_output.failure == sqlite_exact_empty_cold_failure::none &&
					large_output.observation && large_output.observation->sector_size == 65'536U &&
					large_output.observation->journal_record_count == 16U,
				"large-sector journal did not retain the complete derived record set");

		const auto post_hot = make_fixture(post, std::nullopt, hot_journal(pre, 512U, true));
		const auto post_output = classify(post_hot);
		require(post_output.failure == sqlite_exact_empty_cold_failure::none &&
					post_output.observation &&
					post_output.observation->family == sqlite_exact_empty_cold_family::fh &&
					post_output.observation->main_form == sqlite_exact_empty_cold_main_form::post,
				"FH-post exact preimages were not admitted");
	}

	void check_counter_wrap_and_invalid_sector()
	{
		const auto pre = exact_main(false, std::numeric_limits<std::uint32_t>::max());
		const auto post = exact_main(true, std::numeric_limits<std::uint32_t>::max());
		const auto wrapped = make_fixture(post, std::nullopt, hot_journal(pre, 512U));
		const auto output = classify(wrapped);
		require(output.failure == sqlite_exact_empty_cold_failure::none && output.observation &&
					output.observation->family == sqlite_exact_empty_cold_family::fh,
				"UINT32_MAX to zero counter wrap was rejected");

		auto non_power = hot_journal(pre, 512U);
		write_be_u32(non_power, 20U, 1'000U);
		require(classify(make_fixture(pre, std::nullopt, std::move(non_power))).failure ==
					sqlite_exact_empty_cold_failure::unknown_journal,
				"non-power-of-two journal sector was accepted");
		auto below_minimum = hot_journal(pre, 512U);
		write_be_u32(below_minimum, 20U, 16U);
		require(classify(make_fixture(pre, std::nullopt, std::move(below_minimum))).failure ==
					sqlite_exact_empty_cold_failure::unknown_journal,
				"below-minimum journal sector was accepted");
	}

	void check_permutation_and_shape_failures()
	{
		auto permuted = make_fixture(exact_main(false));
		std::rotate(permuted.census.entries.begin(),
					permuted.census.entries.begin() + 2,
					permuted.census.entries.end());
		require_family(permuted,
					   sqlite_exact_empty_cold_family::f0,
					   sqlite_exact_empty_cold_route::live_normalizer,
					   "entry permutation changed the observation partition");

		auto mixed =
			make_fixture(exact_main(false), std::vector<std::byte>{}, std::vector<std::byte>{});
		require(classify(mixed).failure == sqlite_exact_empty_cold_failure::mixed_sidecars,
				"mixed WAL and journal were not fail-closed");

		auto orphan = make_fixture(exact_main(false), std::vector<std::byte>{});
		orphan.census.entries[0] = absent(sqlite_backend_file_role::main_database);
		require(classify(orphan).failure == sqlite_exact_empty_cold_failure::orphan_sidecar,
				"orphan sidecar was not fail-closed");

		auto shared = make_fixture(exact_main(false), std::nullopt, std::nullopt, true);
		require(classify(shared).failure == sqlite_exact_empty_cold_failure::shared_memory_present,
				"present SHM was not fail-closed");

		auto duplicate = make_fixture(exact_main(false));
		duplicate.census.entries[1].role = sqlite_backend_file_role::main_database;
		require(classify(duplicate).failure == sqlite_exact_empty_cold_failure::invalid_input,
				"duplicate role was accepted");

		auto unknown =
			make_fixture(exact_main(false), std::nullopt, std::vector<std::byte>{std::byte{1U}});
		require(classify(unknown).failure == sqlite_exact_empty_cold_failure::unknown_journal,
				"unknown journal was not fail-closed");

		auto ordinary_wal = make_fixture(exact_main(false), std::vector<std::byte>{std::byte{1U}});
		require(classify(ordinary_wal).failure ==
					sqlite_exact_empty_cold_failure::ordinary_wal_only,
				"ordinary nonzero WAL was admitted");

		auto rebound = make_fixture(exact_main(false));
		rebound.main->replacement = sqlite_backend_replacement_state::replaced;
		require(classify(rebound).failure ==
					sqlite_exact_empty_cold_failure::current_observation_changed,
				"rebound main was admitted");

		auto nonempty = make_fixture(std::vector<std::byte>(4096U, std::byte{0x7fU}));
		require(classify(nonempty).failure == sqlite_exact_empty_cold_failure::main_not_exact_empty,
				"nonempty main bytes were admitted");
	}

	void check_bounds_and_resource_failures()
	{
		auto fixture = make_fixture(exact_main(false));
		require(classify(fixture, 511U).failure == sqlite_exact_empty_cold_failure::size_limit,
				"below-minimum bound was not rejected");
		require(classify(fixture, 512U).failure == sqlite_exact_empty_cold_failure::size_limit,
				"main byte bound was not enforced");
		require(classify(fixture, 512U * 1024U * 1024U + 1U).failure ==
					sqlite_exact_empty_cold_failure::size_limit,
				"above-maximum bound was not rejected");

		fixture.main->throw_on_read = true;
		require(classify(fixture).failure == sqlite_exact_empty_cold_failure::resource_limit,
				"read/resource exception was not fail-closed");

		fixture = make_fixture(exact_main(false));
		fixture.guard->stable = false;
		require(classify(fixture).failure ==
					sqlite_exact_empty_cold_failure::current_observation_changed,
				"namespace drift was not fail-closed");

		auto sidecar_drift =
			make_fixture(exact_main(false), std::nullopt, hot_journal(exact_main(false)));
		sidecar_drift.journal->drift_after_first_read = true;
		require(classify(sidecar_drift).failure ==
					sqlite_exact_empty_cold_failure::current_observation_changed,
				"sidecar byte drift between retained reads was admitted");

		auto oversized_sidecar =
			make_fixture(exact_main(false), std::nullopt, std::vector<std::byte>{});
		oversized_sidecar.journal = std::make_shared<fake_held_object>(
			sqlite_backend_file_role::rollback_journal,
			"large-journal",
			std::vector<std::byte>(9U * 1024U * 1024U + 4097U, std::byte{}));
		oversized_sidecar.census.entries[3] = retained(oversized_sidecar.journal);
		require(classify(oversized_sidecar, 4096U).failure ==
					sqlite_exact_empty_cold_failure::size_limit,
				"sidecar resource bound was not enforced before parsing");
	}
} // namespace

int main()
{
	try
	{
		check_seven_way_partition();
		check_sector_vectors_and_opaque_padding();
		check_counter_wrap_and_invalid_sector();
		check_permutation_and_shape_failures();
		check_bounds_and_resource_failures();
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return EXIT_FAILURE;
	}
}
