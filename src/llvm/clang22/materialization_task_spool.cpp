#include "materialization_task_spool.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "materialization_admission_error.hpp"

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		constexpr std::size_t line_index_scan_bytes = 64U * 1024U;
		constexpr std::string_view line_index_contract = "cxxlens.byte-line-index.v1";
		constexpr char line_index_identity_domain[] = "cxxlens\0line-index\0v1\0";

		[[nodiscard]] sdk::error limit_error(std::string)
		{
			return materialization_admission_no_response();
		}

		[[nodiscard]] std::string_view
		operation_name(const materialization_io_operation operation) noexcept
		{
			switch (operation)
			{
				case materialization_io_operation::configuration:
					return "configuration";
				case materialization_io_operation::buffer_allocation:
					return "buffer-allocation";
				case materialization_io_operation::input_read:
					return "input-read";
				case materialization_io_operation::spool_write:
					return "spool-write";
				case materialization_io_operation::spool_seal:
					return "spool-seal";
				case materialization_io_operation::spool_create:
					return "spool-create";
				case materialization_io_operation::spool_read:
					return "spool-read";
				case materialization_io_operation::spool_rewind:
					return "spool-rewind";
				case materialization_io_operation::digest_update:
					return "digest-update";
				case materialization_io_operation::digest_finalize:
					return "digest-finalize";
			}
			return "unknown-operation";
		}

		[[nodiscard]] sdk::error io_error(const std::string_view field,
										  const materialization_io_failure& failure)
		{
			return materialization_admission_io_failure(
				failure, std::string{field}, std::string{operation_name(failure.operation)});
		}

		[[nodiscard]] bool exact_sha256(const std::string_view value) noexcept
		{
			return value.size() == 71U && value.starts_with("sha256:") &&
				std::ranges::all_of(value.substr(7U),
									[](const char byte)
									{
										return (byte >= '0' && byte <= '9') ||
											(byte >= 'a' && byte <= 'f');
									});
		}

		[[nodiscard]] constexpr std::uint64_t integer_width(std::uint64_t value) noexcept
		{
			std::uint64_t width{1U};
			while (value > 0xffU)
			{
				++width;
				value >>= 8U;
			}
			return width;
		}

		[[nodiscard]] constexpr std::uint64_t
		canonical_integer_bytes(const std::uint64_t value) noexcept
		{
			return 10U + integer_width(value);
		}

		[[nodiscard]] constexpr std::uint64_t
		canonical_string_bytes(const std::string_view value) noexcept
		{
			return 9U + static_cast<std::uint64_t>(value.size());
		}

		class digest_writer
		{
		  public:
			explicit digest_writer(materialization_digest_accumulator& digest) noexcept
				: digest_{digest}
			{
			}

			[[nodiscard]] sdk::result<void> append(const std::span<const std::byte> bytes)
			{
				std::size_t offset{};
				while (offset < bytes.size())
				{
					const auto count = std::min(bytes.size() - offset, buffer_.size() - size_);
					std::ranges::copy(bytes.subspan(offset, count), buffer_.begin() + size_);
					size_ += count;
					offset += count;
					if (size_ == buffer_.size())
						if (auto flushed = flush(); !flushed)
							return flushed;
				}
				return {};
			}

			[[nodiscard]] sdk::result<void> octet(const std::uint8_t value)
			{
				const std::array bytes{static_cast<std::byte>(value)};
				return append(bytes);
			}

			[[nodiscard]] sdk::result<void> length(const std::uint64_t value)
			{
				std::array<std::byte, 8U> bytes{};
				for (std::size_t index{}; index < bytes.size(); ++index)
					bytes[index] = static_cast<std::byte>(
						(value >> (56U - static_cast<unsigned>(index * 8U))) & 0xffU);
				return append(bytes);
			}

			[[nodiscard]] sdk::result<void> string(const std::string_view value)
			{
				if (auto tag = octet(0x04U); !tag)
					return tag;
				if (auto size = length(value.size()); !size)
					return size;
				return append(std::as_bytes(std::span{value.data(), value.size()}));
			}

			[[nodiscard]] sdk::result<void> nonnegative_integer(const std::uint64_t value)
			{
				if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
					return sdk::unexpected(materialization_admission_no_response());
				if (auto tag = octet(0x02U); !tag)
					return tag;
				if (auto sign = octet(0U); !sign)
					return sign;
				const auto width = integer_width(value);
				if (auto framed = length(width); !framed)
					return framed;
				std::array<std::byte, 8U> bytes{};
				for (std::uint64_t index{}; index < width; ++index)
					bytes[static_cast<std::size_t>(index)] =
						static_cast<std::byte>((value >> ((width - index - 1U) * 8U)) & 0xffU);
				return append(std::span{bytes}.first(static_cast<std::size_t>(width)));
			}

			[[nodiscard]] sdk::result<void> tuple_header(const std::uint64_t count)
			{
				if (auto tag = octet(0x05U); !tag)
					return tag;
				return length(count);
			}

			[[nodiscard]] sdk::result<std::string> finish()
			{
				if (auto flushed = flush(); !flushed)
					return sdk::unexpected(std::move(flushed.error()));
				auto output = digest_.finish();
				if (!output)
					return sdk::unexpected(io_error("line-index", output.error()));
				if (!exact_sha256(*output))
					return sdk::unexpected(materialization_admission_no_response());
				return std::move(*output);
			}

		  private:
			[[nodiscard]] sdk::result<void> flush()
			{
				if (size_ == 0U)
					return {};
				auto updated = digest_.update(std::span{buffer_}.first(size_));
				if (!updated)
					return sdk::unexpected(io_error("line-index", updated.error()));
				size_ = 0U;
				return {};
			}

			materialization_digest_accumulator& digest_;
			std::array<std::byte, 4096U> buffer_{};
			std::size_t size_{};
		};

		[[nodiscard]] sdk::result<std::string>
		sealed_line_index_identity(materialization_replayable_spool& storage,
								   const std::uint64_t size_bytes,
								   const std::uint64_t newline_count,
								   const std::uint64_t offset_items_bytes,
								   const std::string_view content_digest)
		{
			auto line_digest = make_materialization_sha256_accumulator();
			auto verification_digest = make_materialization_sha256_accumulator();
			if (!line_digest || !verification_digest)
				return sdk::unexpected(materialization_admission_no_response());
			digest_writer writer{*line_digest};
			const auto domain = std::string_view{line_index_identity_domain,
												 sizeof(line_index_identity_domain) - 1U};
			if (auto appended =
					writer.append(std::as_bytes(std::span{domain.data(), domain.size()}));
				!appended)
				return sdk::unexpected(std::move(appended.error()));

			const auto offsets_tuple_bytes = 9U + offset_items_bytes;
			if (auto header = writer.tuple_header(4U); !header)
				return sdk::unexpected(std::move(header.error()));
			if (auto framed = writer.length(canonical_string_bytes(line_index_contract)); !framed)
				return sdk::unexpected(std::move(framed.error()));
			if (auto contract = writer.string(line_index_contract); !contract)
				return sdk::unexpected(std::move(contract.error()));
			if (auto framed = writer.length(canonical_string_bytes(content_digest)); !framed)
				return sdk::unexpected(std::move(framed.error()));
			if (auto digest = writer.string(content_digest); !digest)
				return sdk::unexpected(std::move(digest.error()));
			if (auto framed = writer.length(canonical_integer_bytes(size_bytes)); !framed)
				return sdk::unexpected(std::move(framed.error()));
			if (auto size = writer.nonnegative_integer(size_bytes); !size)
				return sdk::unexpected(std::move(size.error()));
			if (auto framed = writer.length(offsets_tuple_bytes); !framed)
				return sdk::unexpected(std::move(framed.error()));
			if (auto offsets = writer.tuple_header(newline_count + 1U); !offsets)
				return sdk::unexpected(std::move(offsets.error()));
			if (auto framed = writer.length(canonical_integer_bytes(0U)); !framed)
				return sdk::unexpected(std::move(framed.error()));
			if (auto zero = writer.nonnegative_integer(0U); !zero)
				return sdk::unexpected(std::move(zero.error()));

			std::array<std::byte, line_index_scan_bytes> buffer{};
			std::uint64_t offset{};
			std::uint64_t observed_newlines{};
			std::uint64_t observed_items_bytes = 8U + canonical_integer_bytes(0U);
			while (offset < size_bytes)
			{
				auto read = storage.read_at(offset, buffer);
				if (!read)
					return sdk::unexpected(io_error("source", read.error()));
				if (*read == 0U || *read > buffer.size() ||
					static_cast<std::uint64_t>(*read) > size_bytes - offset)
					return sdk::unexpected(materialization_admission_no_response());
				const auto bytes = std::span{buffer}.first(*read);
				if (auto updated = verification_digest->update(bytes); !updated)
					return sdk::unexpected(io_error("source", updated.error()));
				for (std::size_t index{}; index < bytes.size(); ++index)
					if (bytes[index] == std::byte{'\n'})
					{
						const auto line_offset = offset + static_cast<std::uint64_t>(index) + 1U;
						++observed_newlines;
						observed_items_bytes += 8U + canonical_integer_bytes(line_offset);
						if (auto framed = writer.length(canonical_integer_bytes(line_offset));
							!framed)
							return sdk::unexpected(std::move(framed.error()));
						if (auto value = writer.nonnegative_integer(line_offset); !value)
							return sdk::unexpected(std::move(value.error()));
					}
				offset += static_cast<std::uint64_t>(*read);
			}
			if (observed_newlines != newline_count || observed_items_bytes != offset_items_bytes)
				return sdk::unexpected(materialization_admission_no_response());
			auto verified = verification_digest->finish();
			if (!verified)
				return sdk::unexpected(io_error("source", verified.error()));
			if (*verified != content_digest)
				return sdk::unexpected(materialization_admission_no_response());
			auto digest = writer.finish();
			if (!digest)
				return sdk::unexpected(std::move(digest.error()));
			return "line-index:" + *digest;
		}

		class source_spool_adapter final : public clang22_task_source_spool
		{
		  public:
			source_spool_adapter(
				std::unique_ptr<materialization_replayable_spool> storage,
				std::unique_ptr<materialization_digest_accumulator> content_digest) noexcept
				: storage_{std::move(storage)}, content_digest_{std::move(content_digest)}
			{
			}

			sdk::result<void> append(const std::span<const std::byte> bytes) override
			{
				if (sealed_ || poisoned_)
					return sdk::unexpected(materialization_admission_no_response());
				if (static_cast<std::uint64_t>(bytes.size()) >
					maximum_clang22_task_source_bytes - size_bytes_)
				{
					poisoned_ = true;
					return sdk::unexpected(limit_error("source"));
				}
				try
				{
					auto appended = storage_->append(bytes);
					if (!appended)
					{
						poisoned_ = true;
						return sdk::unexpected(io_error("source", appended.error()));
					}
					if (storage_->size_bytes() != size_bytes_ + bytes.size())
					{
						poisoned_ = true;
						return sdk::unexpected(materialization_admission_no_response());
					}
					auto hashed = content_digest_->update(bytes);
					if (!hashed)
					{
						poisoned_ = true;
						return sdk::unexpected(io_error("source", hashed.error()));
					}
					for (std::size_t index{}; index < bytes.size(); ++index)
						if (bytes[index] == std::byte{'\n'})
						{
							const auto offset =
								size_bytes_ + static_cast<std::uint64_t>(index) + 1U;
							++newline_count_;
							offset_items_bytes_ += 8U + canonical_integer_bytes(offset);
						}
					size_bytes_ += static_cast<std::uint64_t>(bytes.size());
					return {};
				}
				catch (const std::bad_alloc&)
				{
					poisoned_ = true;
					return sdk::unexpected(materialization_admission_no_response());
				}
			}

			sdk::result<clang22_task_source_receipt> seal() override
			{
				if (sealed_ || poisoned_)
					return sdk::unexpected(materialization_admission_no_response());
				try
				{
					auto storage_seal = storage_->seal();
					if (!storage_seal)
					{
						poisoned_ = true;
						return sdk::unexpected(io_error("source", storage_seal.error()));
					}
					if (!storage_->sealed() || storage_->size_bytes() != size_bytes_)
					{
						poisoned_ = true;
						return sdk::unexpected(materialization_admission_no_response());
					}
					auto digest = content_digest_->finish();
					if (!digest)
					{
						poisoned_ = true;
						return sdk::unexpected(io_error("source", digest.error()));
					}
					if (!exact_sha256(*digest))
					{
						poisoned_ = true;
						return sdk::unexpected(materialization_admission_no_response());
					}
					auto line_index = sealed_line_index_identity(
						*storage_, size_bytes_, newline_count_, offset_items_bytes_, *digest);
					if (!line_index)
					{
						poisoned_ = true;
						return sdk::unexpected(std::move(line_index.error()));
					}
					receipt_ = {size_bytes_, std::move(*digest), std::move(*line_index)};
					content_digest_.reset();
					sealed_ = true;
					return receipt_;
				}
				catch (const std::bad_alloc&)
				{
					poisoned_ = true;
					return sdk::unexpected(materialization_admission_no_response());
				}
			}

			sdk::result<std::size_t> read_at(const std::uint64_t offset,
											 const std::span<std::byte> destination) override
			{
				return replay(offset, destination, "source");
			}

			[[nodiscard]] std::uint64_t size_bytes() const noexcept override
			{
				return size_bytes_;
			}

			[[nodiscard]] bool sealed() const noexcept override
			{
				return sealed_ && !poisoned_ && storage_->sealed() &&
					storage_->size_bytes() == size_bytes_;
			}

			[[nodiscard]] const clang22_task_source_receipt& receipt() const noexcept override
			{
				return receipt_;
			}

		  private:
			[[nodiscard]] sdk::result<std::size_t> replay(const std::uint64_t offset,
														  const std::span<std::byte> destination,
														  const std::string_view field)
			{
				try
				{
					if (!sealed() || offset > size_bytes_)
					{
						poisoned_ = true;
						return sdk::unexpected(materialization_admission_no_response());
					}
					if (offset == size_bytes_ || destination.empty())
						return std::size_t{};
					auto read = storage_->read_at(offset, destination);
					if (!read)
					{
						poisoned_ = true;
						return sdk::unexpected(io_error(field, read.error()));
					}
					if (*read == 0U || *read > destination.size() ||
						static_cast<std::uint64_t>(*read) > size_bytes_ - offset)
					{
						poisoned_ = true;
						return sdk::unexpected(materialization_admission_no_response());
					}
					return *read;
				}
				catch (const std::bad_alloc&)
				{
					poisoned_ = true;
					return sdk::unexpected(materialization_admission_no_response());
				}
			}

			std::unique_ptr<materialization_replayable_spool> storage_;
			std::unique_ptr<materialization_digest_accumulator> content_digest_;
			clang22_task_source_receipt receipt_;
			std::uint64_t size_bytes_{};
			std::uint64_t newline_count_{};
			std::uint64_t offset_items_bytes_{8U + canonical_integer_bytes(0U)};
			bool sealed_{};
			bool poisoned_{};
		};

		class task_input_spool_adapter final : public clang22_task_input_spool
		{
		  public:
			explicit task_input_spool_adapter(
				std::unique_ptr<materialization_replayable_spool> storage) noexcept
				: storage_{std::move(storage)}
			{
			}

			sdk::result<void> append(const std::span<const std::byte> bytes) override
			{
				if (sealed_ || poisoned_)
					return sdk::unexpected(materialization_admission_no_response());
				if (static_cast<std::uint64_t>(bytes.size()) >
					maximum_clang22_task_input_bytes - size_bytes_)
				{
					poisoned_ = true;
					return sdk::unexpected(limit_error("task.v3"));
				}
				try
				{
					auto appended = storage_->append(bytes);
					if (!appended)
					{
						poisoned_ = true;
						return sdk::unexpected(io_error("task.v3", appended.error()));
					}
					if (storage_->size_bytes() != size_bytes_ + bytes.size())
					{
						poisoned_ = true;
						return sdk::unexpected(materialization_admission_no_response());
					}
					size_bytes_ += static_cast<std::uint64_t>(bytes.size());
					return {};
				}
				catch (const std::bad_alloc&)
				{
					poisoned_ = true;
					return sdk::unexpected(materialization_admission_no_response());
				}
			}

			sdk::result<void> seal() override
			{
				if (sealed_ || poisoned_)
					return sdk::unexpected(materialization_admission_no_response());
				try
				{
					auto sealed = storage_->seal();
					if (!sealed)
					{
						poisoned_ = true;
						return sdk::unexpected(io_error("task.v3", sealed.error()));
					}
					if (!storage_->sealed() || storage_->size_bytes() != size_bytes_)
					{
						poisoned_ = true;
						return sdk::unexpected(materialization_admission_no_response());
					}
					sealed_ = true;
					return {};
				}
				catch (const std::bad_alloc&)
				{
					poisoned_ = true;
					return sdk::unexpected(materialization_admission_no_response());
				}
			}

			sdk::result<std::size_t> read_at(const std::uint64_t offset,
											 const std::span<std::byte> destination) override
			{
				try
				{
					if (!sealed() || offset > size_bytes_)
					{
						poisoned_ = true;
						return sdk::unexpected(materialization_admission_no_response());
					}
					if (offset == size_bytes_ || destination.empty())
						return std::size_t{};
					auto read = storage_->read_at(offset, destination);
					if (!read)
					{
						poisoned_ = true;
						return sdk::unexpected(io_error("task.v3", read.error()));
					}
					if (*read == 0U || *read > destination.size() ||
						static_cast<std::uint64_t>(*read) > size_bytes_ - offset)
					{
						poisoned_ = true;
						return sdk::unexpected(materialization_admission_no_response());
					}
					return *read;
				}
				catch (const std::bad_alloc&)
				{
					poisoned_ = true;
					return sdk::unexpected(materialization_admission_no_response());
				}
			}

			[[nodiscard]] std::uint64_t size_bytes() const noexcept override
			{
				return size_bytes_;
			}

			[[nodiscard]] bool sealed() const noexcept override
			{
				return sealed_ && !poisoned_ && storage_->sealed() &&
					storage_->size_bytes() == size_bytes_;
			}

		  private:
			std::unique_ptr<materialization_replayable_spool> storage_;
			std::uint64_t size_bytes_{};
			bool sealed_{};
			bool poisoned_{};
		};
	} // namespace

	sdk::result<std::unique_ptr<clang22_task_source_spool>> make_materialization_task_source_spool()
	{
		auto storage = make_materialization_private_spool();
		if (!storage)
			return sdk::unexpected(io_error("source", storage.error()));
		try
		{
			return make_materialization_task_source_spool(
				std::move(*storage), make_materialization_sha256_accumulator());
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(materialization_admission_no_response());
		}
	}

	sdk::result<std::unique_ptr<clang22_task_source_spool>> make_materialization_task_source_spool(
		std::unique_ptr<materialization_replayable_spool> storage,
		std::unique_ptr<materialization_digest_accumulator> content_digest)
	{
		if (!storage || !content_digest || storage->sealed() || storage->size_bytes() != 0U)
			return sdk::unexpected(materialization_admission_no_response());
		try
		{
			return std::unique_ptr<clang22_task_source_spool>{
				std::make_unique<source_spool_adapter>(std::move(storage),
													   std::move(content_digest))};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(materialization_admission_no_response());
		}
	}

	sdk::result<std::unique_ptr<clang22_task_input_spool>> make_materialization_task_input_spool()
	{
		auto storage = make_materialization_private_spool();
		if (!storage)
			return sdk::unexpected(io_error("task.v3", storage.error()));
		return make_materialization_task_input_spool(std::move(*storage));
	}

	sdk::result<std::unique_ptr<clang22_task_input_spool>>
	make_materialization_task_input_spool(std::unique_ptr<materialization_replayable_spool> storage)
	{
		if (!storage || storage->sealed() || storage->size_bytes() != 0U)
			return sdk::unexpected(materialization_admission_no_response());
		try
		{
			return std::unique_ptr<clang22_task_input_spool>{
				std::make_unique<task_input_spool_adapter>(std::move(storage))};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(materialization_admission_no_response());
		}
	}
} // namespace cxxlens::detail::clang22::materialization
