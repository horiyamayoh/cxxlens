#include "sqlite_wave3_wal_recovery_internal.hpp"

#include <algorithm>
#include <limits>
#include <string_view>
#include <utility>

namespace cxxlens::sdk
{
	namespace
	{
		constexpr std::size_t identity_bytes_bound = 4096U;
		constexpr std::size_t maximum_copy_bytes = 64U * 1024U;
		constexpr std::size_t maximum_chunk_count = 4096U;
		constexpr std::string_view error_code = "store.sqlite-failure";
		constexpr std::string_view error_field = "sqlite-wave3-wal-recovery";

		[[nodiscard]] error recovery_error(const std::string_view detail)
		{
			return {std::string{error_code}, std::string{error_field}, std::string{detail}};
		}

		[[nodiscard]] result<void> fail(const std::string_view detail)
		{
			return unexpected(recovery_error(detail));
		}

		[[nodiscard]] bool valid_identity(const sqlite_backend_opaque_identity& identity) noexcept
		{
			return !identity.profile.empty() && !identity.bytes.empty() &&
				identity.bytes.size() <= identity_bytes_bound;
		}

		[[nodiscard]] bool valid_digest(const std::string_view digest) noexcept
		{
			if (digest.size() != 64U)
			{
				return false;
			}
			return std::all_of(digest.begin(), digest.end(), [](const char value) {
				return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
			});
		}

		[[nodiscard]] bool valid_source(const sqlite_wave3_wal_source_identity& source) noexcept
		{
			return valid_identity(source.process_instance) &&
				valid_identity(source.shared_runtime_vfs_cohort) &&
				valid_identity(source.exact_file_family) && valid_identity(source.open_epoch) &&
				valid_identity(source.namespace_epoch) && source.fork_generation != 0;
		}
	} // namespace

	result<void> validate_sqlite_wave3_wal_source_identity(
		const sqlite_wave3_wal_source_identity& identity)
	{
		if (!valid_source(identity))
		{
			return fail("invalid-source-identity");
		}
		return {};
	}

	result<sqlite_wave3_wal_recovery_plan> plan_sqlite_wave3_wal_recovery(
		const sqlite_wave3_wal_recovery_input& input)
	{
		if (auto source = validate_sqlite_wave3_wal_source_identity(input.source); !source)
		{
			return unexpected(source.error());
		}
		if (input.main_size == 0 || !valid_digest(input.main_digest))
		{
			return unexpected(recovery_error("invalid-main-receipt"));
		}
		if (input.source_mutation_permitted)
		{
			return unexpected(recovery_error("source-mutation-permitted"));
		}
		if (!input.private_shm_only)
		{
			return unexpected(recovery_error("private-shm-required"));
		}
		if (input.max_copy_bytes == 0 || input.max_copy_bytes > maximum_copy_bytes)
		{
			return unexpected(recovery_error("invalid-copy-bound"));
		}
		if (input.wal_size > input.max_copy_bytes)
		{
			return unexpected(recovery_error("copy-bound-exceeded"));
		}

		sqlite_wave3_wal_recovery_route route{sqlite_wave3_wal_recovery_route::main_only};
		switch (input.wal_state)
		{
		case sqlite_wave3_wal_state::absent:
			if (input.wal_size != 0 || !input.wal_digest.empty() ||
				input.authoritative_prefix_bytes != 0 || input.native_readonly_cantinit ||
				input.native_readonly_mapping || input.read_lock_index != -1)
			{
				return unexpected(recovery_error("absent-wal-inconsistent"));
			}
			break;
		case sqlite_wave3_wal_state::size_zero:
			if (input.wal_size != 0 || !valid_digest(input.wal_digest) ||
				input.authoritative_prefix_bytes != 0 || input.native_readonly_mapping ||
				(input.native_readonly_cantinit && input.read_lock_index != 0) ||
				(!input.native_readonly_cantinit && input.read_lock_index != -1))
			{
				return unexpected(recovery_error("zero-wal-inconsistent"));
			}
			if (input.native_readonly_cantinit)
			{
				route = sqlite_wave3_wal_recovery_route::private_heap_index;
			}
			break;
		case sqlite_wave3_wal_state::valid_nonzero:
			if (input.wal_size == 0 || !valid_digest(input.wal_digest) ||
				input.authoritative_prefix_bytes != input.wal_size)
			{
				return unexpected(recovery_error("valid-wal-inconsistent"));
			}
			if (input.native_readonly_cantinit && input.native_readonly_mapping)
			{
				return unexpected(recovery_error("multiple-native-outcomes"));
			}
			if (input.native_readonly_cantinit)
			{
				if (input.read_lock_index != 0)
				{
					return unexpected(recovery_error("private-index-lock-mismatch"));
				}
				route = sqlite_wave3_wal_recovery_route::private_heap_index;
			}
			else if (input.native_readonly_mapping)
			{
				if (input.read_lock_index < 0)
				{
					return unexpected(recovery_error("native-mapping-lock-missing"));
				}
				route = sqlite_wave3_wal_recovery_route::native_readonly_mapping;
			}
			else
			{
				return unexpected(recovery_error("native-route-missing"));
			}
			break;
		case sqlite_wave3_wal_state::invalid:
			return unexpected(recovery_error("invalid-wal-state"));
		}

		return sqlite_wave3_wal_recovery_plan{input.source,
			route,
			input.main_size,
			input.wal_size,
			input.authoritative_prefix_bytes,
			input.main_digest,
			input.wal_digest,
			true,
			true,
			true};
	}

	result<std::vector<sqlite_wave3_wal_recovery_chunk>> chunk_sqlite_wave3_wal_prefix(
		const std::span<const std::byte> bytes,
		const std::size_t chunk_bytes)
	{
		if (chunk_bytes == 0 || chunk_bytes > maximum_copy_bytes)
		{
			return unexpected(recovery_error("invalid-chunk-bound"));
		}
		if (bytes.size() > std::numeric_limits<std::uint64_t>::max())
		{
			return unexpected(recovery_error("prefix-size-overflow"));
		}
		const auto count = (bytes.size() + chunk_bytes - 1U) / chunk_bytes;
		if (count > maximum_chunk_count)
		{
			return unexpected(recovery_error("chunk-count-exceeded"));
		}
		std::vector<sqlite_wave3_wal_recovery_chunk> chunks;
		chunks.reserve(count);
		for (std::size_t offset = 0; offset < bytes.size(); offset += chunk_bytes)
		{
			const auto length = std::min(chunk_bytes, bytes.size() - offset);
			chunks.push_back({static_cast<std::uint64_t>(offset),
				static_cast<std::uint64_t>(length)});
		}
		return chunks;
	}

	sqlite_wave3_wal_recovery_session::sqlite_wave3_wal_recovery_session(
		sqlite_wave3_wal_recovery_plan plan)
		: plan_{std::move(plan)}, phase_{sqlite_wave3_wal_recovery_phase::planned}
	{
	}

	result<sqlite_wave3_wal_recovery_session> sqlite_wave3_wal_recovery_session::open(
		const sqlite_wave3_wal_recovery_input& input)
	{
		auto plan = plan_sqlite_wave3_wal_recovery(input);
		if (!plan)
		{
			return unexpected(plan.error());
		}
		return sqlite_wave3_wal_recovery_session{std::move(plan.value())};
	}

	sqlite_wave3_wal_recovery_phase sqlite_wave3_wal_recovery_session::phase() const noexcept
	{
		return phase_;
	}

	sqlite_wave3_wal_recovery_route sqlite_wave3_wal_recovery_session::route() const noexcept
	{
		return plan_.route;
	}

	const sqlite_wave3_wal_recovery_plan& sqlite_wave3_wal_recovery_session::plan() const noexcept
	{
		return plan_;
	}

	std::span<const std::byte> sqlite_wave3_wal_recovery_session::sealed_prefix() const noexcept
	{
		return sealed_prefix_;
	}

	std::string_view sqlite_wave3_wal_recovery_session::quarantine_detail() const noexcept
	{
		return quarantine_detail_;
	}

	result<void> sqlite_wave3_wal_recovery_session::seal_prefix(
		const std::span<const std::byte> bytes)
	{
		if (phase_ != sqlite_wave3_wal_recovery_phase::planned)
		{
			return fail("invalid-prefix-transition");
		}
		if (bytes.size() != plan_.authoritative_prefix_bytes)
		{
			return fail("prefix-size-mismatch");
		}
		sealed_prefix_.assign(bytes.begin(), bytes.end());
		phase_ = sqlite_wave3_wal_recovery_phase::prefix_sealed;
		return {};
	}

	result<void> sqlite_wave3_wal_recovery_session::mark_decoded_candidate()
	{
		if (phase_ != sqlite_wave3_wal_recovery_phase::prefix_sealed)
		{
			return fail("invalid-decode-transition");
		}
		phase_ = sqlite_wave3_wal_recovery_phase::decoded_candidate;
		return {};
	}

	result<void> sqlite_wave3_wal_recovery_session::revoke()
	{
		if (phase_ != sqlite_wave3_wal_recovery_phase::decoded_candidate)
		{
			return fail("invalid-revoke-transition");
		}
		phase_ = sqlite_wave3_wal_recovery_phase::revoked;
		return {};
	}

	result<void> sqlite_wave3_wal_recovery_session::close()
	{
		if (phase_ != sqlite_wave3_wal_recovery_phase::revoked)
		{
			return fail("invalid-close-transition");
		}
		phase_ = sqlite_wave3_wal_recovery_phase::closed;
		return {};
	}

	result<void> sqlite_wave3_wal_recovery_session::quarantine(std::string detail)
	{
		if (phase_ == sqlite_wave3_wal_recovery_phase::closed ||
			phase_ == sqlite_wave3_wal_recovery_phase::quarantined)
		{
			return fail("invalid-quarantine-transition");
		}
		if (detail.empty())
		{
			return fail("missing-quarantine-detail");
		}
		quarantine_detail_ = std::move(detail);
		phase_ = sqlite_wave3_wal_recovery_phase::quarantined;
		return {};
	}
} // namespace cxxlens::sdk
