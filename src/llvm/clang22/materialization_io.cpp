#include "materialization_io.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/memfd.h>
#include <sys/syscall.h>
#endif

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
#if defined(__linux__) && defined(SYS_memfd_create) && defined(MFD_CLOEXEC) && \
	defined(MFD_ALLOW_SEALING) && defined(F_ADD_SEALS) && defined(F_GET_SEALS) && \
	defined(F_SEAL_WRITE) && defined(F_SEAL_GROW) && defined(F_SEAL_SHRINK) && \
	defined(F_SEAL_SEAL)
		inline constexpr int required_memfd_seals =
			F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
#endif

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

		class anonymous_file_spool final : public materialization_replayable_spool
		{
		  public:
			anonymous_file_spool(
				const int descriptor,
				std::unique_ptr<materialization_digest_accumulator> expected_digest) noexcept
				: descriptor_{descriptor}, expected_digest_{std::move(expected_digest)}
			{
			}
			~anonymous_file_spool() override
			{
				if (descriptor_ >= 0)
					(void)::close(descriptor_);
			}

			materialization_io_result<void> append(const std::span<const std::byte> bytes) override
			{
				if (sealed_ || poisoned_ || !expected_digest_)
					return materialization_io_failure{
						materialization_io_failure_kind::invalid_configuration,
						materialization_io_operation::spool_write};
				std::size_t offset{};
				while (offset < bytes.size())
				{
					const auto count =
						::write(descriptor_, bytes.data() + offset, bytes.size() - offset);
					if (count < 0 && errno == EINTR)
						continue;
					if (count < 0)
					{
						poisoned_ = true;
						return materialization_io_failure{
							materialization_io_failure_kind::write,
							materialization_io_operation::spool_write};
					}
					if (count == 0 || static_cast<std::size_t>(count) > bytes.size() - offset)
					{
						poisoned_ = true;
						return materialization_io_failure{
							materialization_io_failure_kind::invalid_configuration,
							materialization_io_operation::spool_write};
					}
					offset += static_cast<std::size_t>(count);
				}
				if (auto updated = expected_digest_->update(bytes); !updated)
				{
					poisoned_ = true;
					return updated.error();
				}
				size_ += static_cast<std::uint64_t>(bytes.size());
				return {};
			}

			materialization_io_result<void> seal() override
			{
				if (poisoned_ || !expected_digest_)
					return materialization_io_failure{
						materialization_io_failure_kind::invalid_configuration,
						materialization_io_operation::spool_seal};
				if (sealed_)
					return {};
#if defined(__linux__) && defined(SYS_memfd_create) && defined(MFD_CLOEXEC) && \
	defined(MFD_ALLOW_SEALING) && defined(F_ADD_SEALS) && defined(F_GET_SEALS) && \
	defined(F_SEAL_WRITE) && defined(F_SEAL_GROW) && defined(F_SEAL_SHRINK) && \
	defined(F_SEAL_SEAL)
				if (::lseek(descriptor_, 0, SEEK_SET) < 0)
					return materialization_io_failure{materialization_io_failure_kind::spool,
													  materialization_io_operation::spool_seal};
				if (::fcntl(descriptor_, F_ADD_SEALS, required_memfd_seals) != 0)
					return materialization_io_failure{materialization_io_failure_kind::spool,
													  materialization_io_operation::spool_seal};
				const auto observed_seals = ::fcntl(descriptor_, F_GET_SEALS);
				if (observed_seals < 0)
					return materialization_io_failure{materialization_io_failure_kind::spool,
													  materialization_io_operation::spool_seal};
				if ((observed_seals & required_memfd_seals) != required_memfd_seals)
				{
					poisoned_ = true;
					return materialization_io_failure{
						materialization_io_failure_kind::invalid_configuration,
						materialization_io_operation::spool_seal};
				}
				struct stat status{};
				if (::fstat(descriptor_, &status) != 0)
				{
					poisoned_ = true;
					return materialization_io_failure{materialization_io_failure_kind::spool,
													  materialization_io_operation::spool_seal};
				}
				if (status.st_size < 0 || static_cast<std::uint64_t>(status.st_size) != size_)
				{
					poisoned_ = true;
					return materialization_io_failure{
						materialization_io_failure_kind::invalid_configuration,
						materialization_io_operation::spool_seal};
				}
				auto verified = verify_sealed_bytes();
				if (!verified)
				{
					poisoned_ = true;
					return verified.error();
				}
				expected_digest_.reset();
				sealed_ = true;
				return {};
#else
				return materialization_io_failure{
					materialization_io_failure_kind::invalid_configuration,
					materialization_io_operation::spool_seal};
#endif
			}

			materialization_io_result<std::size_t> read(std::span<std::byte> destination) override
			{
				if (!sealed())
					return materialization_io_failure{
						materialization_io_failure_kind::invalid_configuration,
						materialization_io_operation::spool_read};
				for (;;)
				{
					const auto count = ::read(descriptor_, destination.data(), destination.size());
					if (count < 0 && errno == EINTR)
						continue;
					if (count < 0)
						return materialization_io_failure{materialization_io_failure_kind::read,
														  materialization_io_operation::spool_read};
					if (static_cast<std::size_t>(count) > destination.size())
						return materialization_io_failure{
							materialization_io_failure_kind::invalid_configuration,
							materialization_io_operation::spool_read};
					return static_cast<std::size_t>(count);
				}
			}

			materialization_io_result<std::size_t>
			read_at(const std::uint64_t offset, std::span<std::byte> destination) override
			{
				if (poisoned_ || offset > size_)
					return materialization_io_failure{
						materialization_io_failure_kind::invalid_configuration,
						materialization_io_operation::spool_read};
				if (offset == size_ || destination.empty())
					return std::size_t{};
				const auto available = static_cast<std::size_t>(
					std::min<std::uint64_t>(size_ - offset, destination.size()));
				for (;;)
				{
					const auto count = ::pread(
						descriptor_, destination.data(), available, static_cast<off_t>(offset));
					if (count < 0 && errno == EINTR)
						continue;
					if (count < 0)
						return materialization_io_failure{materialization_io_failure_kind::read,
														  materialization_io_operation::spool_read};
					if (count == 0 || static_cast<std::size_t>(count) > available)
						return materialization_io_failure{
							materialization_io_failure_kind::invalid_configuration,
							materialization_io_operation::spool_read};
					return static_cast<std::size_t>(count);
				}
			}

			materialization_io_result<void> rewind() override
			{
				if (poisoned_)
					return materialization_io_failure{
						materialization_io_failure_kind::invalid_configuration,
						materialization_io_operation::spool_rewind};
				if (::lseek(descriptor_, 0, SEEK_SET) < 0)
					return materialization_io_failure{materialization_io_failure_kind::spool,
													  materialization_io_operation::spool_rewind};
				return {};
			}

			[[nodiscard]] std::uint64_t size_bytes() const noexcept override
			{
				return size_;
			}

			[[nodiscard]] bool sealed() const noexcept override
			{
				return sealed_ && !poisoned_;
			}

		  private:
			[[nodiscard]] materialization_io_result<void> verify_sealed_bytes()
			{
				try
				{
					auto expected = expected_digest_->finish();
					if (!expected)
						return expected.error();
					if (!exact_sha256(*expected))
						return materialization_io_failure{
							materialization_io_failure_kind::invalid_configuration,
							materialization_io_operation::digest_finalize};
					auto observed_digest = make_materialization_sha256_accumulator();
					if (!observed_digest)
						return materialization_io_failure{
							materialization_io_failure_kind::allocation,
							materialization_io_operation::buffer_allocation};
					std::array<std::byte, default_stream_chunk_bytes> buffer{};
					std::uint64_t offset{};
					while (offset < size_)
					{
						const auto requested = static_cast<std::size_t>(
							std::min<std::uint64_t>(size_ - offset, buffer.size()));
						ssize_t count{};
						do
						{
							count = ::pread(
								descriptor_, buffer.data(), requested, static_cast<off_t>(offset));
						} while (count < 0 && errno == EINTR);
						if (count < 0)
							return materialization_io_failure{
								materialization_io_failure_kind::read,
								materialization_io_operation::spool_read};
						if (count == 0 || static_cast<std::size_t>(count) > requested)
							return materialization_io_failure{
								materialization_io_failure_kind::invalid_configuration,
								materialization_io_operation::spool_read};
						if (auto updated = observed_digest->update(
								std::span{buffer}.first(static_cast<std::size_t>(count)));
							!updated)
							return updated.error();
						offset += static_cast<std::uint64_t>(count);
					}
					auto observed = observed_digest->finish();
					if (!observed)
						return observed.error();
					if (!exact_sha256(*observed))
						return materialization_io_failure{
							materialization_io_failure_kind::invalid_configuration,
							materialization_io_operation::digest_finalize};
					if (*observed != *expected)
						return materialization_io_failure{
							materialization_io_failure_kind::invalid_configuration,
							materialization_io_operation::spool_seal};
					return {};
				}
				catch (const std::bad_alloc&)
				{
					return materialization_io_failure{
						materialization_io_failure_kind::allocation,
						materialization_io_operation::buffer_allocation};
				}
			}

			int descriptor_{-1};
			std::unique_ptr<materialization_digest_accumulator> expected_digest_;
			std::uint64_t size_{};
			bool sealed_{};
			bool poisoned_{};
		};

		class incremental_sha256 final : public materialization_digest_accumulator
		{
		  public:
			materialization_io_result<void>
			update(std::span<const std::byte> input) noexcept override
			{
				total_bytes_ += static_cast<std::uint64_t>(input.size());
				if (pending_size_ != 0U)
				{
					const auto count = std::min(input.size(), block_bytes - pending_size_);
					std::ranges::copy(input.first(count), pending_.begin() + pending_size_);
					pending_size_ += count;
					input = input.subspan(count);
					if (pending_size_ != block_bytes)
						return {};
					transform(pending_);
					pending_size_ = 0U;
				}
				while (input.size() >= block_bytes)
				{
					transform(input.first(block_bytes));
					input = input.subspan(block_bytes);
				}
				std::ranges::copy(input, pending_.begin());
				pending_size_ = input.size();
				return {};
			}

			[[nodiscard]] materialization_io_result<std::string> finish() override
			{
				const auto bit_count = total_bytes_ * 8U;
				pending_[pending_size_++] = std::byte{0x80U};
				if (pending_size_ > 56U)
				{
					std::fill(pending_.begin() + pending_size_, pending_.end(), std::byte{});
					transform(pending_);
					pending_size_ = 0U;
				}
				std::fill(pending_.begin() + pending_size_, pending_.begin() + 56U, std::byte{});
				for (std::size_t index{}; index < 8U; ++index)
					pending_[56U + index] = static_cast<std::byte>(
						(bit_count >> (56U - static_cast<unsigned>(index * 8U))) & 0xffU);
				transform(pending_);

				constexpr std::string_view digits{"0123456789abcdef"};
				std::string output{"sha256:"};
				output.reserve(71U);
				for (const auto word : state_)
					for (std::uint32_t shift = 28U;; shift -= 4U)
					{
						output.push_back(digits[(word >> shift) & 0x0fU]);
						if (shift == 0U)
							break;
					}
				return output;
			}

		  private:
			static constexpr std::size_t block_bytes = 64U;
			static constexpr std::array<std::uint32_t, 64U> round_constants{
				0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
				0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
				0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
				0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
				0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
				0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
				0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
				0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
				0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
				0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
				0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
			};

			void transform(const std::span<const std::byte> block) noexcept
			{
				std::array<std::uint32_t, 64U> schedule{};
				for (std::size_t index{}; index < 16U; ++index)
				{
					const auto offset = index * 4U;
					schedule[index] = (std::to_integer<std::uint32_t>(block[offset]) << 24U) |
						(std::to_integer<std::uint32_t>(block[offset + 1U]) << 16U) |
						(std::to_integer<std::uint32_t>(block[offset + 2U]) << 8U) |
						std::to_integer<std::uint32_t>(block[offset + 3U]);
				}
				for (std::size_t index = 16U; index < schedule.size(); ++index)
				{
					const auto small_zero = std::rotr(schedule[index - 15U], 7) ^
						std::rotr(schedule[index - 15U], 18) ^ (schedule[index - 15U] >> 3U);
					const auto small_one = std::rotr(schedule[index - 2U], 17) ^
						std::rotr(schedule[index - 2U], 19) ^ (schedule[index - 2U] >> 10U);
					schedule[index] =
						schedule[index - 16U] + small_zero + schedule[index - 7U] + small_one;
				}

				auto [a, b, c, d, e, f, g, h] = state_;
				for (std::size_t index{}; index < schedule.size(); ++index)
				{
					const auto big_one = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
					const auto choose = (e & f) ^ (~e & g);
					const auto temporary_one =
						h + big_one + choose + round_constants[index] + schedule[index];
					const auto big_zero = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
					const auto majority = (a & b) ^ (a & c) ^ (b & c);
					const auto temporary_two = big_zero + majority;
					h = g;
					g = f;
					f = e;
					e = d + temporary_one;
					d = c;
					c = b;
					b = a;
					a = temporary_one + temporary_two;
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

			std::array<std::uint32_t, 8U> state_{0x6a09e667U,
												 0xbb67ae85U,
												 0x3c6ef372U,
												 0xa54ff53aU,
												 0x510e527fU,
												 0x9b05688cU,
												 0x1f83d9abU,
												 0x5be0cd19U};
			std::array<std::byte, block_bytes> pending_{};
			std::size_t pending_size_{};
			std::uint64_t total_bytes_{};
		};

		[[nodiscard]] materialization_io_failure
		failure(const materialization_io_failure_kind kind,
				const materialization_io_operation operation) noexcept
		{
			return {kind, operation};
		}
	} // namespace

	materialization_io_result<std::unique_ptr<materialization_replayable_spool>>
	make_materialization_private_spool()
	{
#if defined(__linux__) && defined(SYS_memfd_create) && defined(MFD_CLOEXEC) && \
	defined(MFD_ALLOW_SEALING) && defined(F_ADD_SEALS) && defined(F_GET_SEALS) && \
	defined(F_SEAL_WRITE) && defined(F_SEAL_GROW) && defined(F_SEAL_SHRINK) && \
	defined(F_SEAL_SEAL)
		const auto descriptor = static_cast<int>(::syscall(
			SYS_memfd_create, "cxxlens-materialization", MFD_CLOEXEC | MFD_ALLOW_SEALING));
		if (descriptor < 0)
			return materialization_io_failure{materialization_io_failure_kind::spool,
											  materialization_io_operation::spool_create};
		try
		{
			auto expected_digest = make_materialization_sha256_accumulator();
			if (!expected_digest)
			{
				(void)::close(descriptor);
				return materialization_io_failure{materialization_io_failure_kind::allocation,
												  materialization_io_operation::spool_create};
			}
			auto output =
				std::make_unique<anonymous_file_spool>(descriptor, std::move(expected_digest));
			return std::unique_ptr<materialization_replayable_spool>{std::move(output)};
		}
		catch (const std::bad_alloc&)
		{
			(void)::close(descriptor);
			return materialization_io_failure{materialization_io_failure_kind::allocation,
											  materialization_io_operation::spool_create};
		}
#else
		return materialization_io_failure{materialization_io_failure_kind::invalid_configuration,
										  materialization_io_operation::spool_create};
#endif
	}

	std::unique_ptr<materialization_digest_accumulator> make_materialization_sha256_accumulator()
	{
		return std::make_unique<incremental_sha256>();
	}

	materialization_io_result<std::string>
	digest_materialization_spool(materialization_replayable_spool& spool,
								 const std::size_t chunk_bytes)
	{
		if (!spool.sealed() || chunk_bytes == 0U || chunk_bytes > maximum_stream_chunk_bytes)
			return failure(materialization_io_failure_kind::invalid_configuration,
						   materialization_io_operation::configuration);
		try
		{
			std::vector<std::byte> buffer(chunk_bytes);
			incremental_sha256 digest;
			std::uint64_t offset{};
			while (offset < spool.size_bytes())
			{
				const auto remaining = spool.size_bytes() - offset;
				auto destination = std::span{buffer}.first(
					static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size())));
				auto received = spool.read_at(offset, destination);
				if (!received)
					return received.error();
				if (*received == 0U || *received > destination.size())
					return failure(materialization_io_failure_kind::invalid_configuration,
								   materialization_io_operation::spool_read);
				if (auto updated = digest.update(std::span{buffer}.first(*received)); !updated)
					return updated.error();
				offset += static_cast<std::uint64_t>(*received);
			}
			auto finished = digest.finish();
			if (!finished)
				return finished.error();
			if (!exact_sha256(*finished))
				return failure(materialization_io_failure_kind::invalid_configuration,
							   materialization_io_operation::digest_finalize);
			return std::move(*finished);
		}
		catch (const std::bad_alloc&)
		{
			return failure(materialization_io_failure_kind::allocation,
						   materialization_io_operation::buffer_allocation);
		}
	}

	materialization_io_result<raw_input_observation>
	capture_bounded_input(materialization_byte_reader& input,
						  materialization_private_spool& spool,
						  materialization_digest_accumulator& digest,
						  const bounded_input_options options)
	{
		constexpr auto maximum_hash_bytes = std::numeric_limits<std::uint64_t>::max() / 8U;
		if (options.chunk_bytes == 0U || options.chunk_bytes > maximum_stream_chunk_bytes ||
			options.byte_limit >= maximum_hash_bytes)
			return failure(materialization_io_failure_kind::invalid_configuration,
						   materialization_io_operation::configuration);

		const auto maximum_observed = options.byte_limit + 1U;
		const auto buffer_bytes = static_cast<std::size_t>(std::min<std::uint64_t>(
			maximum_observed, static_cast<std::uint64_t>(options.chunk_bytes)));
		auto operation = materialization_io_operation::buffer_allocation;
		try
		{
			std::vector<std::byte> buffer(buffer_bytes);
			std::uint64_t observed{};
			bool complete{};
			while (observed < maximum_observed)
			{
				const auto remaining = maximum_observed - observed;
				const auto requested = static_cast<std::size_t>(
					std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(buffer.size())));
				operation = materialization_io_operation::input_read;
				auto received = input.read(std::span{buffer}.first(requested));
				if (!received)
					return received.error();
				if (*received > requested)
					return failure(materialization_io_failure_kind::invalid_configuration,
								   materialization_io_operation::input_read);
				if (*received == 0U)
				{
					complete = true;
					break;
				}

				const auto bytes = std::span<const std::byte>{buffer}.first(*received);
				operation = materialization_io_operation::spool_write;
				auto written = spool.append(bytes);
				if (!written)
					return written.error();
				operation = materialization_io_operation::digest_update;
				auto updated = digest.update(bytes);
				if (!updated)
					return updated.error();
				observed += static_cast<std::uint64_t>(*received);
			}

			operation = materialization_io_operation::spool_seal;
			auto sealed = spool.seal();
			if (!sealed)
				return sealed.error();
			operation = materialization_io_operation::digest_finalize;
			auto finished = digest.finish();
			if (!finished)
				return finished.error();
			if (!exact_sha256(*finished))
				return failure(materialization_io_failure_kind::invalid_configuration,
							   materialization_io_operation::digest_finalize);
			return raw_input_observation{
				options.byte_limit, observed, std::move(*finished), complete};
		}
		catch (const std::bad_alloc&)
		{
			return failure(materialization_io_failure_kind::allocation, operation);
		}
		catch (const std::length_error&)
		{
			return failure(materialization_io_failure_kind::allocation, operation);
		}
	}

	materialization_io_result<raw_input_observation>
	capture_bounded_input(materialization_byte_reader& input,
						  materialization_private_spool& spool,
						  const bounded_input_options options)
	{
		incremental_sha256 digest;
		return capture_bounded_input(input, spool, digest, options);
	}
} // namespace cxxlens::detail::clang22::materialization
