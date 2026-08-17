#include "provider_task_v4.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace cxxlens::detail::clang22::source_closure
{
	namespace
	{
		constexpr std::array<std::uint32_t, 64U> k{
			0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
			0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
			0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
			0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
			0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
			0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
			0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
			0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U};

		[[nodiscard]] validation_error error(
			std::string code, std::string field = {}, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		class incremental_sha256
		{
		  public:
			void update(const std::span<const std::byte> bytes)
			{
				if (bytes.empty())
					return;
				if (total_bytes_ > std::numeric_limits<std::uint64_t>::max() - bytes.size())
					throw std::length_error{"sha256 byte count overflow"};
				total_bytes_ += bytes.size();
				std::size_t offset{};
				if (buffer_size_ != 0U)
				{
					const auto copied = std::min(buffer_.size() - buffer_size_, bytes.size());
					std::copy_n(bytes.data(), copied, buffer_.data() + buffer_size_);
					buffer_size_ += copied;
					offset += copied;
					if (buffer_size_ != buffer_.size())
						return;
					transform(buffer_.data());
					buffer_size_ = 0U;
				}
				while (bytes.size() - offset >= buffer_.size())
				{
					transform(bytes.data() + offset);
					offset += buffer_.size();
				}
				const auto remaining = bytes.size() - offset;
				if (remaining != 0U)
					std::copy_n(bytes.data() + offset, remaining, buffer_.data());
				buffer_size_ = remaining;
			}

			[[nodiscard]] std::string finish() const
			{
				auto copy = *this;
				const auto bit_length = copy.total_bytes_ * 8U;
				const std::byte marker{0x80U};
				copy.update_without_count({&marker, 1U});
				const std::byte zero{};
				while (copy.buffer_size_ != 56U)
					copy.update_without_count({&zero, 1U});
				std::array<std::byte, 8U> length{};
				for (std::size_t index = 0U; index < length.size(); ++index)
					length[length.size() - 1U - index] =
						static_cast<std::byte>((bit_length >> (index * 8U)) & 0xffU);
				copy.update_without_count(length);
				constexpr std::string_view hex{"0123456789abcdef"};
				std::string output{"sha256:"};
				output.reserve(71U);
				for (const auto word : copy.state_)
					for (int shift = 28; shift >= 0; shift -= 4)
						output.push_back(hex[(word >> shift) & 0x0fU]);
				return output;
			}

		  private:
			void update_without_count(const std::span<const std::byte> bytes)
			{
				std::size_t offset{};
				while (offset < bytes.size())
				{
					const auto copied = std::min(buffer_.size() - buffer_size_, bytes.size() - offset);
					std::copy_n(bytes.data() + offset, copied, buffer_.data() + buffer_size_);
					buffer_size_ += copied;
					offset += copied;
					if (buffer_size_ == buffer_.size())
					{
						transform(buffer_.data());
						buffer_size_ = 0U;
					}
				}
			}

			void transform(const std::byte* block)
			{
				std::array<std::uint32_t, 64U> w{};
				for (std::size_t index = 0U; index < 16U; ++index)
				{
					const auto offset = index * 4U;
					w[index] = (std::to_integer<std::uint32_t>(block[offset]) << 24U) |
						(std::to_integer<std::uint32_t>(block[offset + 1U]) << 16U) |
						(std::to_integer<std::uint32_t>(block[offset + 2U]) << 8U) |
						std::to_integer<std::uint32_t>(block[offset + 3U]);
				}
				for (std::size_t index = 16U; index < w.size(); ++index)
				{
					const auto s0 = std::rotr(w[index - 15U], 7) ^
						std::rotr(w[index - 15U], 18) ^ (w[index - 15U] >> 3U);
					const auto s1 = std::rotr(w[index - 2U], 17) ^
						std::rotr(w[index - 2U], 19) ^ (w[index - 2U] >> 10U);
					w[index] = w[index - 16U] + s0 + w[index - 7U] + s1;
				}
				auto [a, b, c, d, e, f, g, h] = state_;
				for (std::size_t index = 0U; index < w.size(); ++index)
				{
					const auto t1 = h +
						(std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) +
						((e & f) ^ (~e & g)) + k[index] + w[index];
					const auto t2 =
						(std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) +
						((a & b) ^ (a & c) ^ (b & c));
					h = g;
					g = f;
					f = e;
					e = d + t1;
					d = c;
					c = b;
					b = a;
					a = t1 + t2;
				}
				state_[0U] += a;
				state_[1U] += b;
				state_[2U] += c;
				state_[3U] += d;
				state_[4U] += e;
				state_[5U] += f;
				state_[6U] += g;
				state_[7U] += h;
			}

			std::array<std::uint32_t, 8U> state_{
				0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
				0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
			std::array<std::byte, 64U> buffer_{};
			std::size_t buffer_size_{};
			std::uint64_t total_bytes_{};
		};

		[[nodiscard]] std::array<std::byte, 8U> big_endian(const std::uint64_t value)
		{
			std::array<std::byte, 8U> result{};
			for (std::size_t index = 0U; index < result.size(); ++index)
				result[result.size() - 1U - index] =
					static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
			return result;
		}

		void identity_field(std::string& output, const std::string_view value)
		{
			std::array<char, 20U> digits{};
			const auto converted =
				std::to_chars(digits.data(), digits.data() + digits.size(), value.size());
			output.append(digits.data(), converted.ptr);
			output.push_back(':');
			output.append(value);
		}

		[[nodiscard]] std::string role_text(const file_role role)
		{
			switch (role)
			{
			case file_role::main_source: return "main-source";
			case file_role::project_header: return "project-header";
			case file_role::generated_header: return "generated-header";
			case file_role::forced_include: return "forced-include";
			case file_role::macro_file: return "macro-file";
			}
			return {};
		}

		class writer
		{
		  public:
			writer(task_v4_sink& sink, const std::stop_token cancellation)
				: sink_{sink}, cancellation_{cancellation}
			{
			}

			[[nodiscard]] std::expected<void, validation_error>
			raw(const std::span<const std::byte> bytes)
			{
				if (cancellation_.stop_requested())
					return std::unexpected(
						error("source-closure.task-v4-cancelled", "transport"));
				if (bytes_ > std::numeric_limits<std::uint64_t>::max() - bytes.size())
					return std::unexpected(
						error("source-closure.task-v4-size-overflow", "transport"));
				auto appended = sink_.append(bytes);
				if (!appended)
					return appended;
				hash_.update(bytes);
				bytes_ += bytes.size();
				return {};
			}

			[[nodiscard]] std::expected<void, validation_error>
			frame_header(const std::byte tag, const std::uint64_t length)
			{
				std::array<std::byte, 9U> header{};
				header[0U] = tag;
				const auto encoded = big_endian(length);
				std::ranges::copy(encoded, header.begin() + 1U);
				return raw(header);
			}

			[[nodiscard]] std::expected<void, validation_error>
			frame(const std::byte tag, const std::string_view value)
			{
				if (auto header = frame_header(tag, value.size()); !header)
					return header;
				return raw(std::as_bytes(std::span{value}));
			}

			[[nodiscard]] std::string digest() const { return hash_.finish(); }
			[[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }

		  private:
			task_v4_sink& sink_;
			std::stop_token cancellation_;
			incremental_sha256 hash_;
			std::uint64_t bytes_{};
		};

		[[nodiscard]] std::expected<void, validation_error>
		validate_task(const task_v4& task)
		{
			if (!task.closure || task.closure->snapshot_id.empty() ||
				task.closure->snapshot_digest.empty())
				return std::unexpected(
					error("source-closure.task-v4-invalid", "closure"));
			if (task.main_logical_path.empty() || task.logical_working_directory.empty())
				return std::unexpected(error("source-closure.task-v4-invalid", "path"));
			if (task.effective_arguments.size() < 2U ||
				task.effective_arguments.size() > 4096U ||
				task.effective_arguments.back() != task.main_logical_path)
				return std::unexpected(
					error("source-closure.task-v4-invalid", "effective_arguments"));
			const auto main_count =
				std::ranges::count_if(task.closure->files, [&](const file& item)
				{
					return item.role == file_role::main_source &&
						item.logical_path == task.main_logical_path;
				});
			if (main_count != 1U)
				return std::unexpected(error(
					"source-closure.task-v4-main-binding", "main_logical_path"));
			for (const auto& blob : task.closure->blobs)
				if (sha256_digest(blob.content) != blob.content_digest)
					return std::unexpected(error(
						"source-closure.task-v4-closure-mutated", "blob",
						blob.content_digest));
			return {};
		}
	} // namespace

	std::expected<std::string, validation_error>
	derive_task_v4_id(const task_v4& task)
	{
		if (auto valid = validate_task(task); !valid)
			return std::unexpected(std::move(valid.error()));
		std::string projection{"cxxlens.clang22.task-v4.identity.v1"};
		identity_field(projection, task.main_logical_path);
		identity_field(projection, task.logical_working_directory);
		identity_field(projection, task.closure->snapshot_digest);
		for (const auto& argument : task.effective_arguments)
			identity_field(projection, argument);
		return "task:semantic-v2:" +
			sha256_digest(std::as_bytes(std::span{projection}));
	}

	std::expected<task_v4_receipt, validation_error>
	encode_task_v4_streaming(
		const task_v4& task, task_v4_sink& sink, const task_v4_options options)
	{
		if (options.maximum_chunk_bytes == 0U ||
			options.maximum_chunk_bytes > 1024U * 1024U)
			return std::unexpected(error(
				"source-closure.task-v4-chunk-limit", "maximum_chunk_bytes"));
		if (options.cancellation.stop_requested())
			return std::unexpected(
				error("source-closure.task-v4-cancelled", "transport"));
		auto derived = derive_task_v4_id(task);
		if (!derived)
			return std::unexpected(std::move(derived.error()));
		if (task.task_id != *derived)
			return std::unexpected(error(
				"source-closure.task-v4-id-mismatch", "task_id", task.task_id));

		writer output{sink, options.cancellation};
		auto emit = [&](const std::byte tag, const std::string_view value)
			-> std::expected<void, validation_error> { return output.frame(tag, value); };
		if (auto value = emit(std::byte{0x01U}, "cxxlens.clang22.task.v4"); !value)
			return std::unexpected(std::move(value.error()));
		if (auto value = emit(std::byte{0x02U}, task.task_id); !value)
			return std::unexpected(std::move(value.error()));
		if (auto value = emit(std::byte{0x03U}, task.main_logical_path); !value)
			return std::unexpected(std::move(value.error()));
		if (auto value = emit(std::byte{0x04U}, task.logical_working_directory); !value)
			return std::unexpected(std::move(value.error()));
		if (auto value = emit(std::byte{0x05U}, task.closure->snapshot_digest); !value)
			return std::unexpected(std::move(value.error()));
		if (auto value = output.frame_header(
				std::byte{0x06U}, task.effective_arguments.size()); !value)
			return std::unexpected(std::move(value.error()));
		for (const auto& argument : task.effective_arguments)
			if (auto value = emit(std::byte{0x10U}, argument); !value)
				return std::unexpected(std::move(value.error()));
		if (auto value = output.frame_header(
				std::byte{0x20U}, task.closure->files.size()); !value)
			return std::unexpected(std::move(value.error()));
		for (const auto& file : task.closure->files)
		{
			if (auto value = emit(std::byte{0x21U}, file.logical_path); !value)
				return std::unexpected(std::move(value.error()));
			if (auto value = emit(std::byte{0x22U}, file.content_digest); !value)
				return std::unexpected(std::move(value.error()));
			const auto size = big_endian(file.size_bytes);
			if (auto value = output.frame_header(std::byte{0x23U}, size.size()); !value)
				return std::unexpected(std::move(value.error()));
			if (auto value = output.raw(size); !value)
				return std::unexpected(std::move(value.error()));
			if (auto value = emit(std::byte{0x24U}, role_text(file.role)); !value)
				return std::unexpected(std::move(value.error()));
			if (auto value = emit(std::byte{0x25U}, file.provenance_digest); !value)
				return std::unexpected(std::move(value.error()));
		}
		if (auto value = output.frame_header(
				std::byte{0x30U}, task.closure->blobs.size()); !value)
			return std::unexpected(std::move(value.error()));
		std::uint64_t chunks{};
		for (const auto& blob : task.closure->blobs)
		{
			if (auto value = emit(std::byte{0x31U}, blob.content_digest); !value)
				return std::unexpected(std::move(value.error()));
			if (auto value = output.frame_header(
					std::byte{0x32U}, blob.content.size()); !value)
				return std::unexpected(std::move(value.error()));
			for (std::size_t offset = 0U; offset < blob.content.size();)
			{
				const auto count = std::min(
					options.maximum_chunk_bytes, blob.content.size() - offset);
				if (auto value = output.raw(
						std::span{blob.content}.subspan(offset, count)); !value)
					return std::unexpected(std::move(value.error()));
				offset += count;
				++chunks;
			}
		}
		return task_v4_receipt{task.task_id, output.digest(),
			task.closure->snapshot_digest, output.bytes(), chunks};
	}
} // namespace cxxlens::detail::clang22::source_closure
