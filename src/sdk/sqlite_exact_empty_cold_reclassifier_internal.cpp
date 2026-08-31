#include "sqlite_exact_empty_cold_reclassifier_internal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace cxxlens::sdk
{
	namespace
	{
		using classification = sqlite_exact_empty_cold_classification;
		using failure = sqlite_exact_empty_cold_failure;
		using entry = sqlite_backend_entry_observation;

		constexpr std::uint64_t maximum_supported_main_bytes = std::uint64_t{512U} * 1024U * 1024U;
		constexpr std::uint64_t sidecar_slack_bytes = std::uint64_t{9U} * 1024U * 1024U;
		constexpr std::uint32_t minimum_page_size = 512U;
		constexpr std::uint32_t minimum_sector_size = 32U;
		constexpr std::uint32_t maximum_page_size = 65'536U;

		[[nodiscard]] classification rejected(const failure reason) noexcept
		{
			return classification{reason, std::nullopt};
		}

		[[nodiscard]] bool checked_add_size(const std::size_t left,
											const std::size_t right,
											std::size_t& output) noexcept
		{
			if (right > std::numeric_limits<std::size_t>::max() - left)
				return false;
			output = left + right;
			return true;
		}

		[[nodiscard]] bool checked_mul_size(const std::size_t left,
											const std::size_t right,
											std::size_t& output) noexcept
		{
			if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left)
				return false;
			output = left * right;
			return true;
		}

		[[nodiscard]] bool checked_add_u64(const std::uint64_t left,
										   const std::uint64_t right,
										   std::uint64_t& output) noexcept
		{
			if (right > std::numeric_limits<std::uint64_t>::max() - left)
				return false;
			output = left + right;
			return true;
		}

		[[nodiscard]] bool all_zero(const std::span<const std::byte> bytes) noexcept
		{
			return std::ranges::all_of(bytes,
									   [](const std::byte value)
									   {
										   return value == std::byte{};
									   });
		}

		[[nodiscard]] std::uint32_t read_be_u32(const std::span<const std::byte> bytes,
												const std::size_t offset) noexcept
		{
			return (std::to_integer<std::uint32_t>(bytes[offset]) << 24U) |
				(std::to_integer<std::uint32_t>(bytes[offset + 1U]) << 16U) |
				(std::to_integer<std::uint32_t>(bytes[offset + 2U]) << 8U) |
				std::to_integer<std::uint32_t>(bytes[offset + 3U]);
		}

		void write_be_u32(const std::span<std::byte> bytes,
						  const std::size_t offset,
						  const std::uint32_t value) noexcept
		{
			bytes[offset] = static_cast<std::byte>((value >> 24U) & 0xffU);
			bytes[offset + 1U] = static_cast<std::byte>((value >> 16U) & 0xffU);
			bytes[offset + 2U] = static_cast<std::byte>((value >> 8U) & 0xffU);
			bytes[offset + 3U] = static_cast<std::byte>(value & 0xffU);
		}

		[[nodiscard]] std::optional<std::uint32_t>
		role_index(const sqlite_backend_file_role role) noexcept
		{
			switch (role)
			{
				case sqlite_backend_file_role::main_database:
					return 0U;
				case sqlite_backend_file_role::write_ahead_log:
					return 1U;
				case sqlite_backend_file_role::shared_memory:
					return 2U;
				case sqlite_backend_file_role::rollback_journal:
					return 3U;
			}
			return std::nullopt;
		}

		[[nodiscard]] bool
		nonempty_identity(const sqlite_backend_opaque_identity& identity) noexcept
		{
			return !identity.profile.empty() && !identity.bytes.empty();
		}

		[[nodiscard]] bool is_absent(const entry& value) noexcept
		{
			return value.state == sqlite_backend_entry_state::absent && !value.object_identity &&
				!value.directory_entry_identity && !value.held_object &&
				!value.object_filesystem_profile && !value.direct_regular_entry;
		}

		[[nodiscard]] bool is_retained(const entry& value) noexcept
		{
			if (value.state != sqlite_backend_entry_state::held_regular ||
				!value.direct_regular_entry || !value.object_identity ||
				!value.directory_entry_identity || !value.held_object ||
				!value.object_filesystem_profile || !nonempty_identity(*value.object_identity) ||
				!nonempty_identity(*value.directory_entry_identity) ||
				value.held_object->role() != value.role ||
				value.held_object->object_identity() != *value.object_identity ||
				value.held_object->directory_entry_identity() != *value.directory_entry_identity)
				return false;
			const auto& object_filesystem = value.held_object->object_filesystem_profile();
			const auto& object_mount = value.held_object->object_mount_identity();
			return object_filesystem && object_mount &&
				*object_filesystem == *value.object_filesystem_profile &&
				nonempty_identity(*object_filesystem) && nonempty_identity(*object_mount);
		}

		struct read_outcome
		{
			std::vector<std::byte> bytes;
			failure reason{failure::none};
		};

		[[nodiscard]] read_outcome read_retained_entry(const entry& value,
													   const std::uint64_t maximum_bytes) noexcept
		{
			if (!is_retained(value))
				return {{}, failure::entry_not_retained};
			try
			{
				const auto& object = *value.held_object;
				if (!object.recheck_retained_object())
					return {{}, failure::current_observation_changed};
				auto replacement = object.recheck_current_entry();
				auto byte_count = object.size();
				if (!replacement ||
					*replacement != sqlite_backend_replacement_state::exact_same_entry_and_object ||
					!byte_count)
					return {{}, failure::current_observation_changed};
				if (*byte_count > maximum_bytes ||
					*byte_count > std::numeric_limits<std::size_t>::max())
					return {{}, failure::size_limit};

				const auto expected_size = *byte_count;
				std::vector<std::byte> first(static_cast<std::size_t>(expected_size));
				std::vector<std::byte> second(first.size());
				if ((!first.empty() && !object.read_exact(0U, first)))
					return {{}, failure::current_observation_changed};
				const auto second_size = object.size();
				if (!second_size || *second_size != expected_size ||
					(!second.empty() && !object.read_exact(0U, second)) || first != second)
					return {{}, failure::current_observation_changed};
				const auto final_size = object.size();
				if (!final_size || *final_size != expected_size)
					return {{}, failure::current_observation_changed};
				if (!object.recheck_retained_object())
					return {{}, failure::current_observation_changed};
				replacement = object.recheck_current_entry();
				if (!replacement ||
					*replacement != sqlite_backend_replacement_state::exact_same_entry_and_object ||
					!is_retained(value))
					return {{}, failure::current_observation_changed};
				return {std::move(first), failure::none};
			}
			catch (...)
			{
				return {{}, failure::resource_limit};
			}
		}

		struct main_profile
		{
			sqlite_exact_empty_cold_main_form form{};
			std::uint32_t page_size{};
			std::uint32_t page_count{};
		};

		[[nodiscard]] std::optional<main_profile> parse_main(const std::span<const std::byte> bytes,
															 failure& reason) noexcept
		{
			constexpr std::array<std::byte, 16U> magic{std::byte{'S'},
													   std::byte{'Q'},
													   std::byte{'L'},
													   std::byte{'i'},
													   std::byte{'t'},
													   std::byte{'e'},
													   std::byte{' '},
													   std::byte{'f'},
													   std::byte{'o'},
													   std::byte{'r'},
													   std::byte{'m'},
													   std::byte{'a'},
													   std::byte{'t'},
													   std::byte{' '},
													   std::byte{'3'},
													   std::byte{}};
			if (bytes.size() < 108U || !std::ranges::equal(magic, bytes.first(magic.size())))
			{
				reason = failure::main_not_exact_empty;
				return std::nullopt;
			}
			const auto encoded_page_size = (std::to_integer<std::uint32_t>(bytes[16U]) << 8U) |
				std::to_integer<std::uint32_t>(bytes[17U]);
			const auto page_size = encoded_page_size == 1U ? 65'536U : encoded_page_size;
			const auto page_count = read_be_u32(bytes, 28U);
			if (page_size < minimum_page_size || page_size > maximum_page_size ||
				!std::has_single_bit(page_size) || page_count == 0U ||
				static_cast<std::uint64_t>(page_size) * page_count != bytes.size())
			{
				reason = failure::main_not_exact_empty;
				return std::nullopt;
			}
			const auto write_version = std::to_integer<std::uint8_t>(bytes[18U]);
			const auto read_version = std::to_integer<std::uint8_t>(bytes[19U]);
			if (write_version != read_version || (write_version != 1U && write_version != 2U) ||
				bytes[20U] != std::byte{} || bytes[21U] != std::byte{64U} ||
				bytes[22U] != std::byte{32U} || bytes[23U] != std::byte{32U})
			{
				reason = failure::main_not_exact_empty;
				return std::nullopt;
			}

			const auto first_trunk = read_be_u32(bytes, 32U);
			const auto freelist_count = read_be_u32(bytes, 36U);
			const auto schema_format = read_be_u32(bytes, 44U);
			const auto text_encoding = read_be_u32(bytes, 56U);
			const auto content_offset = (std::to_integer<std::uint32_t>(bytes[105U]) << 8U) |
				std::to_integer<std::uint32_t>(bytes[106U]);
			const auto canonical_content_offset = page_size == 65'536U ? 0U : page_size;
			if (read_be_u32(bytes, 40U) != 0U || (schema_format != 0U && schema_format != 4U) ||
				read_be_u32(bytes, 52U) != 0U || (text_encoding != 0U && text_encoding != 1U) ||
				read_be_u32(bytes, 60U) != 0U || read_be_u32(bytes, 64U) != 0U ||
				read_be_u32(bytes, 68U) != 0U || !all_zero(bytes.subspan(72U, 20U)) ||
				bytes[100U] != std::byte{0x0dU} || read_be_u32(bytes, 101U) >> 16U != 0U ||
				((std::to_integer<std::uint32_t>(bytes[103U]) << 8U) |
				 std::to_integer<std::uint32_t>(bytes[104U])) != 0U ||
				content_offset != canonical_content_offset || bytes[107U] != std::byte{} ||
				!all_zero(bytes.subspan(108U, page_size - 108U)) ||
				freelist_count != page_count - 1U ||
				((freelist_count == 0U) != (first_trunk == 0U)))
			{
				reason = failure::main_not_exact_empty;
				return std::nullopt;
			}
			if (page_count > 1'048'576U)
			{
				reason = failure::resource_limit;
				return std::nullopt;
			}

			try
			{
				std::vector<bool> seen(static_cast<std::size_t>(page_count) + 1U, false);
				std::uint32_t seen_count{};
				for (auto trunk = first_trunk; trunk != 0U;)
				{
					if (trunk < 2U || trunk > page_count || seen[trunk])
					{
						reason = failure::main_not_exact_empty;
						return std::nullopt;
					}
					seen[trunk] = true;
					++seen_count;
					const auto page_offset = static_cast<std::size_t>(trunk - 1U) * page_size;
					const auto page = bytes.subspan(page_offset, page_size);
					const auto leaf_count = read_be_u32(page, 4U);
					if (seen_count > freelist_count || leaf_count > (page_size / 4U) - 2U ||
						leaf_count > freelist_count - seen_count)
					{
						reason = failure::main_not_exact_empty;
						return std::nullopt;
					}
					for (std::uint32_t index{}; index < leaf_count; ++index)
					{
						const auto leaf =
							read_be_u32(page, 8U + static_cast<std::size_t>(index) * 4U);
						if (leaf < 2U || leaf > page_count || seen[leaf])
						{
							reason = failure::main_not_exact_empty;
							return std::nullopt;
						}
						seen[leaf] = true;
						++seen_count;
					}
					if (seen_count > freelist_count)
					{
						reason = failure::main_not_exact_empty;
						return std::nullopt;
					}
					trunk = read_be_u32(page, 0U);
				}
				if (seen_count != freelist_count)
				{
					reason = failure::main_not_exact_empty;
					return std::nullopt;
				}
				for (std::uint32_t page{2U}; page <= page_count; ++page)
					if (!seen[page])
					{
						reason = failure::main_not_exact_empty;
						return std::nullopt;
					}
			}
			catch (...)
			{
				reason = failure::resource_limit;
				return std::nullopt;
			}
			reason = failure::none;
			return main_profile{write_version == 2U ? sqlite_exact_empty_cold_main_form::pre
													: sqlite_exact_empty_cold_main_form::post,
								page_size,
								page_count};
		}

		[[nodiscard]] bool exact_post_matches_pre(const std::span<const std::byte> pre,
												  const std::span<const std::byte> post) noexcept
		{
			failure pre_reason{failure::none};
			failure post_reason{failure::none};
			const auto pre_profile = parse_main(pre, pre_reason);
			const auto post_profile = parse_main(post, post_reason);
			if (!pre_profile || !post_profile ||
				pre_profile->form != sqlite_exact_empty_cold_main_form::pre ||
				post_profile->form != sqlite_exact_empty_cold_main_form::post ||
				pre_profile->page_size != post_profile->page_size ||
				pre_profile->page_count != post_profile->page_count || pre.size() != post.size() ||
				pre.size() < 100U)
				return false;
			try
			{
				auto expected = std::vector<std::byte>{pre.begin(), pre.end()};
				expected[18U] = std::byte{1U};
				expected[19U] = std::byte{1U};
				const auto counter = read_be_u32(pre, 24U);
				const auto next_counter = counter == std::numeric_limits<std::uint32_t>::max()
					? std::uint32_t{}
					: counter + 1U;
				write_be_u32(expected, 24U, next_counter);
				write_be_u32(expected, 92U, next_counter);
				std::ranges::copy(post.subspan(96U, 4U), expected.begin() + 96U);
				return expected == std::vector<std::byte>{post.begin(), post.end()};
			}
			catch (...)
			{
				return false;
			}
		}

		[[nodiscard]] std::optional<std::vector<std::uint32_t>>
		journal_record_pages(const std::uint32_t sector_size,
							 const std::uint32_t page_size,
							 const std::uint32_t page_count) noexcept
		{
			if (sector_size < minimum_sector_size || sector_size > maximum_page_size ||
				!std::has_single_bit(sector_size) || page_size < minimum_page_size ||
				page_size > maximum_page_size || !std::has_single_bit(page_size) ||
				page_count == 0U)
				return std::nullopt;
			const auto quotient = sector_size > page_size ? sector_size / page_size : 1U;
			const auto count = std::min(page_count, quotient);
			const auto locking_page = std::uint32_t{0x40000000U / page_size + 1U};
			try
			{
				std::vector<std::uint32_t> pages;
				pages.reserve(count);
				for (std::uint32_t page{1U}; page <= count; ++page)
					if (page != locking_page)
						pages.push_back(page);
				if (pages.empty() || pages.size() > 128U)
					return std::nullopt;
				return pages;
			}
			catch (...)
			{
				return std::nullopt;
			}
		}

		[[nodiscard]] std::uint32_t pager_record_checksum(const std::span<const std::byte> page,
														  const std::uint32_t nonce) noexcept
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

		// NOLINTBEGIN(bugprone-easily-swappable-parameters): journal wire-field order.
		[[nodiscard]] bool rebuild_pre_from_journal(const std::span<const std::byte> journal,
													const std::size_t record_offset,
													const std::uint32_t nonce,
													const std::span<const std::uint32_t> pages,
													const std::uint32_t page_size,
													const std::span<const std::byte> current_main,
													std::vector<std::byte>& reconstructed) noexcept
		{
			try
			{
				reconstructed.assign(current_main.begin(), current_main.end());
				std::size_t record_bytes{};
				if (!checked_add_size(static_cast<std::size_t>(page_size), 8U, record_bytes))
					return false;
				for (std::size_t index{}; index < pages.size(); ++index)
				{
					std::size_t delta{};
					std::size_t record{};
					if (!checked_mul_size(index, record_bytes, delta) ||
						!checked_add_size(record_offset, delta, record) ||
						record > journal.size() || record_bytes > journal.size() - record ||
						read_be_u32(journal, record) != pages[index])
						return false;
					const auto image = journal.subspan(record + 4U, page_size);
					if (read_be_u32(journal, record + 4U + page_size) !=
						pager_record_checksum(image, nonce))
						return false;
					std::size_t target{};
					if (!checked_mul_size(
							static_cast<std::size_t>(pages[index] - 1U), page_size, target) ||
						target > reconstructed.size() || page_size > reconstructed.size() - target)
						return false;
					std::ranges::copy(image, reconstructed.data() + target);
				}
				return true;
			}
			catch (...)
			{
				return false;
			}
		}
		// NOLINTEND(bugprone-easily-swappable-parameters)

		struct journal_profile
		{
			std::uint32_t sector_size{};
			std::uint32_t record_count{};
		};

		[[nodiscard]] std::optional<journal_profile>
		classify_hot_journal(const std::span<const std::byte> journal,
							 const main_profile& main,
							 const std::span<const std::byte> current_main) noexcept
		{
			constexpr std::array<std::byte, 8U> magic{std::byte{0xd9U},
													  std::byte{0xd5U},
													  std::byte{0x05U},
													  std::byte{0xf9U},
													  std::byte{0x20U},
													  std::byte{0xa1U},
													  std::byte{0x63U},
													  std::byte{0xd7U}};
			if (journal.size() < 28U || !std::ranges::equal(magic, journal.first(8U)))
				return std::nullopt;
			const auto nonce = read_be_u32(journal, 12U);
			const auto sector_size = read_be_u32(journal, 20U);
			const auto page_size = read_be_u32(journal, 24U);
			auto pages = journal_record_pages(sector_size, page_size, main.page_count);
			std::size_t records_bytes{};
			std::size_t expected_size{};
			if (!pages || page_size != main.page_size ||
				read_be_u32(journal, 8U) != pages->size() ||
				read_be_u32(journal, 16U) != main.page_count ||
				!checked_add_size(static_cast<std::size_t>(page_size), 8U, records_bytes) ||
				!checked_mul_size(pages->size(), records_bytes, records_bytes) ||
				!checked_add_size(sector_size, records_bytes, expected_size) ||
				journal.size() != expected_size)
				return std::nullopt;
			std::vector<std::byte> reconstructed;
			if (!rebuild_pre_from_journal(
					journal, sector_size, nonce, *pages, page_size, current_main, reconstructed))
				return std::nullopt;
			if (main.form == sqlite_exact_empty_cold_main_form::pre)
			{
				if (reconstructed !=
					std::vector<std::byte>{current_main.begin(), current_main.end()})
					return std::nullopt;
			}
			else if (!exact_post_matches_pre(reconstructed, current_main))
				return std::nullopt;
			return journal_profile{sector_size, static_cast<std::uint32_t>(pages->size())};
		}

		struct invalidated_match
		{
			std::optional<journal_profile> profile;
			bool ambiguous{};
		};

		[[nodiscard]] invalidated_match
		classify_invalidated_journal(const std::span<const std::byte> journal,
									 const main_profile& main,
									 const std::span<const std::byte> current_main) noexcept
		{
			if (main.form != sqlite_exact_empty_cold_main_form::post || journal.size() < 28U ||
				!all_zero(journal.first(28U)))
				return {};
			std::optional<journal_profile> admitted;
			for (std::uint32_t sector_size{minimum_sector_size};; sector_size <<= 1U)
			{
				auto pages = journal_record_pages(sector_size, main.page_size, main.page_count);
				std::size_t records_bytes{};
				std::size_t expected_size{};
				if (pages &&
					checked_add_size(static_cast<std::size_t>(main.page_size), 8U, records_bytes) &&
					checked_mul_size(pages->size(), records_bytes, records_bytes) &&
					checked_add_size(sector_size, records_bytes, expected_size) &&
					journal.size() == expected_size)
				{
					const auto first_record = static_cast<std::size_t>(sector_size);
					if (read_be_u32(journal, first_record) == pages->front())
					{
						const auto first_image = journal.subspan(first_record + 4U, main.page_size);
						const auto nonce =
							read_be_u32(journal, first_record + 4U + main.page_size) -
							pager_record_checksum(first_image, 0U);
						std::vector<std::byte> reconstructed;
						if (rebuild_pre_from_journal(journal,
													 sector_size,
													 nonce,
													 *pages,
													 main.page_size,
													 current_main,
													 reconstructed) &&
							exact_post_matches_pre(reconstructed, current_main))
						{
							if (admitted)
								return {std::nullopt, true};
							admitted = journal_profile{sector_size,
													   static_cast<std::uint32_t>(pages->size())};
						}
					}
				}
				if (sector_size == maximum_page_size)
					break;
			}
			return {admitted, false};
		}

		[[nodiscard]] std::optional<journal_profile>
		classify_journal_prefix(const std::span<const std::byte> journal,
								const main_profile& main,
								const std::span<const std::byte> current_main) noexcept
		{
			if (main.form != sqlite_exact_empty_cold_main_form::pre)
				return std::nullopt;
			if (journal.empty())
				return journal_profile{0U, 0U};
			if (journal.size() < 28U || !all_zero(journal.first(8U)) ||
				read_be_u32(journal, 8U) != 0U || read_be_u32(journal, 16U) != main.page_count ||
				read_be_u32(journal, 24U) != main.page_size)
				return std::nullopt;
			const auto nonce = read_be_u32(journal, 12U);
			const auto sector_size = read_be_u32(journal, 20U);
			auto pages = journal_record_pages(sector_size, main.page_size, main.page_count);
			if (!pages)
				return std::nullopt;
			try
			{
				std::size_t record_size{};
				std::size_t records_size{};
				std::size_t full_size{};
				if (!checked_add_size(static_cast<std::size_t>(main.page_size), 8U, record_size) ||
					!checked_mul_size(pages->size(), record_size, records_size) ||
					!checked_add_size(sector_size, records_size, full_size) ||
					journal.size() > full_size)
					return std::nullopt;
				std::vector<std::byte> expected(full_size, std::byte{});
				write_be_u32(expected, 12U, nonce);
				write_be_u32(expected, 16U, main.page_count);
				write_be_u32(expected, 20U, sector_size);
				write_be_u32(expected, 24U, main.page_size);
				const auto padding_end =
					std::min(journal.size(), static_cast<std::size_t>(sector_size));
				if (padding_end > 28U)
					std::ranges::copy(journal.subspan(28U, padding_end - 28U),
									  expected.begin() + 28U);
				std::vector<std::size_t> boundaries;
				const auto chunk_size = std::min(sector_size, main.page_size);
				for (std::size_t end{chunk_size}; end <= sector_size; end += chunk_size)
					boundaries.push_back(end);
				for (std::size_t index{}; index < pages->size(); ++index)
				{
					const auto record = static_cast<std::size_t>(sector_size) + index * record_size;
					const auto page = (*pages)[index];
					std::size_t source_offset{};
					if (!checked_mul_size(
							static_cast<std::size_t>(page - 1U), main.page_size, source_offset) ||
						source_offset > current_main.size() ||
						main.page_size > current_main.size() - source_offset)
						return std::nullopt;
					const auto source = current_main.subspan(source_offset, main.page_size);
					write_be_u32(expected, record, page);
					std::ranges::copy(source, expected.data() + record + 4U);
					write_be_u32(expected,
								 record + 4U + main.page_size,
								 pager_record_checksum(source, nonce));
					boundaries.push_back(record + 4U);
					boundaries.push_back(record + 4U + main.page_size);
					boundaries.push_back(record + 8U + main.page_size);
				}
				if (std::ranges::find(boundaries, journal.size()) == boundaries.end() ||
					!std::ranges::equal(journal, std::span{expected}.first(journal.size())))
					return std::nullopt;
				return journal_profile{sector_size, static_cast<std::uint32_t>(pages->size())};
			}
			catch (...)
			{
				return std::nullopt;
			}
		}

		[[nodiscard]] sqlite_exact_empty_cold_observation
		make_observation(const sqlite_exact_empty_cold_family family,
						 const sqlite_exact_empty_cold_route route,
						 const main_profile& main,
						 const sqlite_exact_empty_cold_sidecar_kind sidecar,
						 const std::uint64_t main_byte_count,
						 const std::uint64_t sidecar_byte_count,
						 const std::uint32_t sector_size,
						 const std::uint32_t record_count) noexcept
		{
			return {family,
					route,
					main.form,
					sidecar,
					main_byte_count,
					sidecar_byte_count,
					main.page_size,
					main.page_count,
					sector_size,
					record_count};
		}
	} // namespace

	result<sqlite_exact_empty_cold_classification> classify_sqlite_exact_empty_cold_observation(
		const sqlite_backend_namespace_census& source_census,
		const std::uint64_t maximum_main_bytes)
	{
		try
		{
			if (maximum_main_bytes < minimum_page_size ||
				maximum_main_bytes > maximum_supported_main_bytes)
				return rejected(failure::size_limit);
			if (source_census.profile != "default-filesystem-v1")
				return rejected(failure::unsupported_profile);
			if (!source_census.source_shm_guard || !source_census.source_shm_guard->recheck())
				return rejected(failure::current_observation_changed);

			std::array<const entry*, 4U> entries{};
			for (const auto& value : source_census.entries)
			{
				const auto index = role_index(value.role);
				if (!index || entries.at(*index) != nullptr)
					return rejected(failure::invalid_input);
				entries.at(*index) = &value;
			}
			if (std::ranges::any_of(entries,
									[](const entry* value)
									{
										return value == nullptr;
									}))
				return rejected(failure::invalid_input);

			const auto& main_entry = *entries[0U];
			const auto& wal_entry = *entries[1U];
			const auto& shm_entry = *entries[2U];
			const auto& journal_entry = *entries[3U];
			if (!is_absent(shm_entry))
				return rejected(failure::shared_memory_present);
			if (!is_retained(main_entry))
			{
				if (!is_absent(wal_entry) || !is_absent(journal_entry))
					return rejected(failure::orphan_sidecar);
				return rejected(failure::entry_not_retained);
			}
			const bool wal_absent = is_absent(wal_entry);
			const bool journal_absent = is_absent(journal_entry);
			const bool wal_held = is_retained(wal_entry);
			const bool journal_held = is_retained(journal_entry);
			if ((!wal_absent && !wal_held) || (!journal_absent && !journal_held))
				return rejected(failure::orphan_sidecar);
			if (wal_held && journal_held)
				return rejected(failure::mixed_sidecars);

			std::uint64_t maximum_sidecar_bytes{};
			if (!checked_add_u64(maximum_main_bytes, sidecar_slack_bytes, maximum_sidecar_bytes))
				return rejected(failure::arithmetic_overflow);
			const auto main_read = read_retained_entry(main_entry, maximum_main_bytes);
			if (main_read.reason != failure::none)
				return rejected(main_read.reason);
			failure main_reason{failure::none};
			const auto main = parse_main(main_read.bytes, main_reason);
			if (!main)
				return rejected(main_reason);

			std::uint64_t sidecar_byte_count{};
			std::uint32_t sector_size{};
			std::uint32_t record_count{};
			sqlite_exact_empty_cold_family family{sqlite_exact_empty_cold_family::f0};
			sqlite_exact_empty_cold_route route{sqlite_exact_empty_cold_route::live_normalizer};
			sqlite_exact_empty_cold_sidecar_kind sidecar{
				sqlite_exact_empty_cold_sidecar_kind::none};

			if (wal_held)
			{
				const auto wal_read = read_retained_entry(wal_entry, maximum_sidecar_bytes);
				if (wal_read.reason != failure::none)
					return rejected(wal_read.reason);
				sidecar_byte_count = wal_read.bytes.size();
				if (!wal_read.bytes.empty())
					return rejected(failure::ordinary_wal_only);
				sidecar = sqlite_exact_empty_cold_sidecar_kind::zero_wal;
				if (main->form == sqlite_exact_empty_cold_main_form::pre)
					family = sqlite_exact_empty_cold_family::fz_pre;
				else
				{
					family = sqlite_exact_empty_cold_family::fz_post;
					route = sqlite_exact_empty_cold_route::fresh_rollback_read;
				}
			}
			else if (journal_held)
			{
				const auto journal_read = read_retained_entry(journal_entry, maximum_sidecar_bytes);
				if (journal_read.reason != failure::none)
					return rejected(journal_read.reason);
				sidecar_byte_count = journal_read.bytes.size();
				if (auto hot = classify_hot_journal(journal_read.bytes, *main, main_read.bytes))
				{
					family = sqlite_exact_empty_cold_family::fh;
					route = sqlite_exact_empty_cold_route::cleanup_then_fresh_read;
					sidecar = sqlite_exact_empty_cold_sidecar_kind::hot_journal;
					sector_size = hot->sector_size;
					record_count = hot->record_count;
				}
				else
				{
					const auto invalidated =
						classify_invalidated_journal(journal_read.bytes, *main, main_read.bytes);
					if (invalidated.ambiguous)
						return rejected(failure::ambiguous_journal);
					if (invalidated.profile)
					{
						family = sqlite_exact_empty_cold_family::fi;
						route = sqlite_exact_empty_cold_route::fresh_rollback_read;
						sidecar = sqlite_exact_empty_cold_sidecar_kind::invalidated_journal;
						sector_size = invalidated.profile->sector_size;
						record_count = invalidated.profile->record_count;
					}
					else if (auto prefix = classify_journal_prefix(
								 journal_read.bytes, *main, main_read.bytes))
					{
						family = sqlite_exact_empty_cold_family::fp;
						route = sqlite_exact_empty_cold_route::cleanup_then_fresh_read;
						sidecar = sqlite_exact_empty_cold_sidecar_kind::journal_prefix;
						sector_size = prefix->sector_size;
						record_count = prefix->record_count;
					}
					else
						return rejected(failure::unknown_journal);
				}
			}
			else if (main->form == sqlite_exact_empty_cold_main_form::post)
			{
				family = sqlite_exact_empty_cold_family::fo;
				route = sqlite_exact_empty_cold_route::fresh_rollback_read;
			}

			if (!source_census.source_shm_guard->recheck() || !is_retained(main_entry))
				return rejected(failure::current_observation_changed);
			return classification{failure::none,
								  make_observation(family,
												   route,
												   *main,
												   sidecar,
												   main_read.bytes.size(),
												   sidecar_byte_count,
												   sector_size,
												   record_count)};
		}
		catch (...)
		{
			return rejected(failure::resource_limit);
		}
	}
} // namespace cxxlens::sdk
