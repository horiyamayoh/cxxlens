#include "sealed_executable_internal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#if defined(__linux__) && defined(__GLIBC__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cxxlens::sdk::detail
{
	namespace
	{
		[[nodiscard]] error sealed_error(std::string field, std::string detail = {})
		{
			return {"runtime.sealed-executable-failed", std::move(field), std::move(detail)};
		}

		[[nodiscard]] error timeout_error()
		{
			return {"runtime.sealed-executable-timeout", "executable", "wall-deadline"};
		}

		[[nodiscard]] error cancelled_error()
		{
			return {"runtime.sealed-executable-cancelled", "executable", "cancelled"};
		}

		[[nodiscard]] bool
		deadline_expired(const std::optional<std::uint64_t> absolute_wall_deadline_ns) noexcept
		{
			if (!absolute_wall_deadline_ns)
				return false;
			const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
								 std::chrono::steady_clock::now().time_since_epoch())
								 .count();
			return now < 0 || std::cmp_greater_equal(now, *absolute_wall_deadline_ns);
		}

#if defined(__linux__) && defined(__GLIBC__)
		class descriptor final
		{
		  public:
			explicit descriptor(const int value = -1) noexcept : value_{value} {}
			~descriptor()
			{
				if (value_ >= 0)
					(void)::close(value_);
			}
			descriptor(const descriptor&) = delete;
			descriptor& operator=(const descriptor&) = delete;
			descriptor(descriptor&& other) noexcept : value_{std::exchange(other.value_, -1)} {}
			descriptor& operator=(descriptor&& other) noexcept
			{
				if (this != &other)
				{
					if (value_ >= 0)
						(void)::close(value_);
					value_ = std::exchange(other.value_, -1);
				}
				return *this;
			}
			[[nodiscard]] int get() const noexcept
			{
				return value_;
			}
			[[nodiscard]] int release() noexcept
			{
				return std::exchange(value_, -1);
			}

		  private:
			int value_;
		};

		class streaming_sha256 final
		{
		  public:
			void update(const std::span<const std::byte> input) noexcept
			{
				total_bytes_ += static_cast<std::uint64_t>(input.size());
				auto remaining = input;
				if (pending_size_ != 0U)
				{
					const auto count = std::min(remaining.size(), block_bytes - pending_size_);
					std::ranges::copy(remaining.first(count), pending_.begin() + pending_size_);
					pending_size_ += count;
					remaining = remaining.subspan(count);
					if (pending_size_ == block_bytes)
					{
						transform(pending_);
						pending_size_ = 0U;
					}
				}
				while (remaining.size() >= block_bytes)
				{
					transform(remaining.first(block_bytes));
					remaining = remaining.subspan(block_bytes);
				}
				std::ranges::copy(remaining, pending_.begin());
				pending_size_ = remaining.size();
			}

			[[nodiscard]] std::string finish()
			{
				const auto bit_count = total_bytes_ * 8U;
				pending_.at(pending_size_++) = std::byte{0x80U};
				if (pending_size_ > 56U)
				{
					std::fill(pending_.begin() + pending_size_, pending_.end(), std::byte{});
					transform(pending_);
					pending_size_ = 0U;
				}
				std::fill(pending_.begin() + pending_size_, pending_.begin() + 56U, std::byte{});
				for (std::size_t index{}; index < 8U; ++index)
					pending_.at(56U + index) = static_cast<std::byte>(
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
					schedule.at(index) = (std::to_integer<std::uint32_t>(block[offset]) << 24U) |
						(std::to_integer<std::uint32_t>(block[offset + 1U]) << 16U) |
						(std::to_integer<std::uint32_t>(block[offset + 2U]) << 8U) |
						std::to_integer<std::uint32_t>(block[offset + 3U]);
				}
				for (std::size_t index = 16U; index < schedule.size(); ++index)
				{
					const auto small_zero = std::rotr(schedule.at(index - 15U), 7) ^
						std::rotr(schedule.at(index - 15U), 18) ^ (schedule.at(index - 15U) >> 3U);
					const auto small_one = std::rotr(schedule.at(index - 2U), 17) ^
						std::rotr(schedule.at(index - 2U), 19) ^ (schedule.at(index - 2U) >> 10U);
					schedule.at(index) =
						schedule.at(index - 16U) + small_zero + schedule.at(index - 7U) + small_one;
				}
				auto [a, b, c, d, e, f, g, h] = state_;
				for (std::size_t index{}; index < schedule.size(); ++index)
				{
					const auto big_one = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
					const auto choose = (e & f) ^ (~e & g);
					const auto first =
						h + big_one + choose + round_constants.at(index) + schedule.at(index);
					const auto big_zero = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
					const auto majority = (a & b) ^ (a & c) ^ (b & c);
					const auto second = big_zero + majority;
					h = g;
					g = f;
					f = e;
					e = d + first;
					d = c;
					c = b;
					b = a;
					a = first + second;
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
			std::array<std::byte, 64U> pending_{};
			std::size_t pending_size_{};
			std::uint64_t total_bytes_{};
		};

		[[nodiscard]] result<std::string>
		canonical_descriptor_path_impl(const canonical_descriptor_path_request request)
		{
			const auto [file, maximum_path_bytes] = request;
			if (maximum_path_bytes == 0U ||
				maximum_path_bytes >= std::numeric_limits<std::size_t>::max() - 1U)
				return unexpected({"runtime.descriptor-path-limit", "path", "path-bytes"});
			const auto link = "/proc/self/fd/" + std::to_string(file);
			std::vector<char> buffer(maximum_path_bytes + 1U);
			const auto count = ::readlink(link.c_str(), buffer.data(), maximum_path_bytes + 1U);
			if (count < 0)
				return unexpected({"runtime.descriptor-path-failed",
								   "path",
								   "readlink:" + std::to_string(errno)});
			if (std::cmp_greater(count, maximum_path_bytes))
				return unexpected({"runtime.descriptor-path-limit", "path", "path-bytes"});
			std::string output{buffer.data(), static_cast<std::size_t>(count)};
			if (output.empty() || !output.starts_with('/') || output.ends_with(" (deleted)"))
				return unexpected({"runtime.descriptor-path-failed", "path", "invalid"});
			return output;
		}
#endif
	} // namespace

	result<std::string>
	canonical_open_descriptor_path(const canonical_descriptor_path_request request)
	{
#if defined(__linux__) && defined(__GLIBC__)
		try
		{
			return canonical_descriptor_path_impl(request);
		}
		catch (const std::bad_alloc&)
		{
			return unexpected({"runtime.descriptor-path-failed", "path", "allocation"});
		}
		catch (const std::length_error&)
		{
			return unexpected({"runtime.descriptor-path-failed", "path", "allocation"});
		}
#else
		(void)request;
		return unexpected({"runtime.descriptor-path-failed", "path", "unsupported-platform"});
#endif
	}

	sealed_executable::sealed_executable(const int image,
										 std::string digest,
										 std::string canonical_source_path,
										 const std::uint64_t byte_count) noexcept
		: image_{image}, digest_{std::move(digest)},
		  canonical_source_path_{std::move(canonical_source_path)}, byte_count_{byte_count}
	{
	}

	sealed_executable::~sealed_executable()
	{
#if defined(__linux__) && defined(__GLIBC__)
		if (image_ >= 0)
			(void)::close(image_);
#endif
	}

	sealed_executable::sealed_executable(sealed_executable&& other) noexcept
		: image_{std::exchange(other.image_, -1)}, digest_{std::move(other.digest_)},
		  canonical_source_path_{std::move(other.canonical_source_path_)},
		  byte_count_{std::exchange(other.byte_count_, 0U)}
	{
	}

	sealed_executable& sealed_executable::operator=(sealed_executable&& other) noexcept
	{
		if (this != &other)
		{
#if defined(__linux__) && defined(__GLIBC__)
			if (image_ >= 0)
				(void)::close(image_);
#endif
			image_ = std::exchange(other.image_, -1);
			digest_ = std::move(other.digest_);
			canonical_source_path_ = std::move(other.canonical_source_path_);
			byte_count_ = std::exchange(other.byte_count_, 0U);
		}
		return *this;
	}

	int sealed_executable::native_handle() const noexcept
	{
		return image_;
	}

	const std::string& sealed_executable::digest() const noexcept
	{
		return digest_;
	}

	const std::string& sealed_executable::canonical_source_path() const noexcept
	{
		return canonical_source_path_;
	}

	std::uint64_t sealed_executable::byte_count() const noexcept
	{
		return byte_count_;
	}

	result<sealed_executable> open_sealed_executable(const sealed_executable_request& request)
	{
#if defined(__linux__) && defined(__GLIBC__)
		try
		{
			if (request.executable_path.empty() || request.executable_path.contains('\0') ||
				request.working_directory.contains('\0'))
				return unexpected(sealed_error("request", "invalid-path"));
			if (request.cancellation.stop_requested())
				return unexpected(cancelled_error());
			if (deadline_expired(request.absolute_wall_deadline_ns))
				return unexpected(timeout_error());

			descriptor directory;
			int source_value{-1};
			const bool relative = request.executable_path.front() != '/';
			const std::string executable_path{request.executable_path};
			if (relative && !request.working_directory.empty())
			{
				const std::string working_directory{request.working_directory};
				directory =
					descriptor{::open(working_directory.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC)};
				if (directory.get() < 0)
					return unexpected(
						sealed_error("working-directory-open", std::to_string(errno)));
				source_value =
					::openat(directory.get(), executable_path.c_str(), O_RDONLY | O_CLOEXEC);
			}
			else
				source_value = ::open(executable_path.c_str(), O_RDONLY | O_CLOEXEC);
			if (source_value < 0)
				return unexpected(sealed_error("executable-open", std::to_string(errno)));
			descriptor source{source_value};
			struct stat metadata{};
			if (::fstat(source.get(), &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
				(metadata.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0)
				return unexpected(sealed_error("executable-type", std::to_string(errno)));
			if (metadata.st_size < 0 ||
				(request.maximum_image_bytes &&
				 std::cmp_greater(metadata.st_size, *request.maximum_image_bytes)))
				return unexpected(sealed_error("executable-size", "image-bytes"));

			std::string canonical_path;
			if (request.maximum_canonical_path_bytes)
			{
				auto path = canonical_open_descriptor_path(
					{source.get(), *request.maximum_canonical_path_bytes});
				if (!path)
					return unexpected(sealed_error("canonical-path", path.error().detail));
				canonical_path = std::move(*path);
			}

			const int image_value =
				::memfd_create("cxxlens-executable", MFD_CLOEXEC | MFD_ALLOW_SEALING);
			if (image_value < 0)
				return unexpected(sealed_error("executable-memfd", std::to_string(errno)));
			descriptor image{image_value};
			streaming_sha256 measured;
			std::array<std::byte, 65536U> buffer{};
			std::uint64_t byte_count{};
			for (;;)
			{
				if (request.cancellation.stop_requested())
					return unexpected(cancelled_error());
				if (deadline_expired(request.absolute_wall_deadline_ns))
					return unexpected(timeout_error());
				const auto count = ::read(source.get(), buffer.data(), buffer.size());
				if (count == 0)
					break;
				if (count < 0)
				{
					if (errno == EINTR)
						continue;
					return unexpected(sealed_error("executable-read", std::to_string(errno)));
				}
				const auto received = static_cast<std::size_t>(count);
				if (request.maximum_image_bytes &&
					(byte_count > *request.maximum_image_bytes ||
					 received > *request.maximum_image_bytes - byte_count))
					return unexpected(sealed_error("executable-size", "image-bytes"));
				byte_count += static_cast<std::uint64_t>(received);
				measured.update(std::span<const std::byte>{buffer.data(), received});
				std::size_t offset{};
				while (offset < received)
				{
					if (request.cancellation.stop_requested())
						return unexpected(cancelled_error());
					const auto written =
						::write(image.get(), buffer.data() + offset, received - offset);
					if (written > 0)
					{
						offset += static_cast<std::size_t>(written);
						continue;
					}
					if (written < 0 && errno == EINTR)
						continue;
					return unexpected(sealed_error("executable-copy", std::to_string(errno)));
				}
			}
			if (::fchmod(image.get(), S_IRUSR | S_IXUSR) != 0 ||
				::fcntl(image.get(),
						F_ADD_SEALS,
						F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) != 0)
				return unexpected(sealed_error("executable-seal", std::to_string(errno)));
			return sealed_executable{
				image.release(), measured.finish(), std::move(canonical_path), byte_count};
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(sealed_error("allocation"));
		}
		catch (const std::length_error&)
		{
			return unexpected(sealed_error("allocation"));
		}
#else
		(void)request;
		return unexpected(sealed_error("platform", "unsupported"));
#endif
	}
} // namespace cxxlens::sdk::detail
