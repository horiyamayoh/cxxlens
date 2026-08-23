#include "sqlite_wave3_shm_lease_internal.hpp"

#include <limits>
#include <string_view>
#include <utility>

namespace cxxlens::sdk
{
	namespace
	{
		constexpr std::string_view error_code = "store.sqlite-failure";
		constexpr std::string_view error_field = "sqlite-wave3-shm-lease";

		[[nodiscard]] error lease_error(const std::string_view detail)
		{
			return {std::string{error_code}, std::string{error_field}, std::string{detail}};
		}

		[[nodiscard]] result<void> fail(const std::string_view detail)
		{
			return unexpected(lease_error(detail));
		}

		[[nodiscard]] std::string_view invalid_detail(const sqlite_wave3_lease_failure reason)
		{
			switch (reason)
			{
				case sqlite_wave3_lease_failure::invalid_identity:
					return "invalid-identity";
				case sqlite_wave3_lease_failure::invalid_transition:
					return "invalid-transition";
				case sqlite_wave3_lease_failure::continuity_mismatch:
					return "continuity-mismatch";
				case sqlite_wave3_lease_failure::duplicate_binding:
					return "duplicate-binding";
				case sqlite_wave3_lease_failure::registry_closed:
					return "registry-closed";
				case sqlite_wave3_lease_failure::registry_quarantined:
					return "registry-quarantined";
				case sqlite_wave3_lease_failure::registry_capacity:
					return "registry-capacity";
				case sqlite_wave3_lease_failure::stale_token:
					return "stale-token";
				case sqlite_wave3_lease_failure::live_use_owners:
					return "live-use-owners";
				case sqlite_wave3_lease_failure::use_bound_exceeded:
					return "use-bound-exceeded";
				case sqlite_wave3_lease_failure::missing_quarantine_detail:
					return "missing-quarantine-detail";
				case sqlite_wave3_lease_failure::generation_exhausted:
					return "generation-exhausted";
			}
			return "invalid-state";
		}

		[[nodiscard]] result<void> fail(const sqlite_wave3_lease_failure reason)
		{
			return fail(invalid_detail(reason));
		}
	} // namespace

	sqlite_wave3_shm_lease::sqlite_wave3_shm_lease(sqlite_wave3_identity_binding binding,
												   const std::uint64_t token)
		: binding_{std::move(binding)}, token_{token}, phase_{sqlite_wave3_lease_phase::reserved}
	{
	}

	sqlite_wave3_shm_lease::sqlite_wave3_shm_lease(sqlite_wave3_shm_lease&& other) noexcept
		: binding_{std::move(other.binding_)}, token_{std::exchange(other.token_, 0)},
		  phase_{std::exchange(other.phase_, sqlite_wave3_lease_phase::unresolved)},
		  active_use_owners_{std::exchange(other.active_use_owners_, 0)},
		  quarantine_detail_{std::move(other.quarantine_detail_)}
	{
	}

	bool sqlite_wave3_shm_lease::valid() const noexcept
	{
		return token_ != 0 && phase_ != sqlite_wave3_lease_phase::unresolved;
	}

	std::uint64_t sqlite_wave3_shm_lease::token() const noexcept
	{
		return token_;
	}

	sqlite_wave3_lease_phase sqlite_wave3_shm_lease::phase() const noexcept
	{
		return phase_;
	}

	const sqlite_wave3_identity_binding& sqlite_wave3_shm_lease::binding() const noexcept
	{
		return binding_;
	}

	std::size_t sqlite_wave3_shm_lease::active_use_owners() const noexcept
	{
		return active_use_owners_;
	}

	std::string_view sqlite_wave3_shm_lease::quarantine_detail() const noexcept
	{
		return quarantine_detail_;
	}

	result<void>
	sqlite_wave3_shm_lease::check_observed(const sqlite_wave3_identity_binding& observed) const
	{
		if (const auto valid = validate_sqlite_wave3_identity_continuity(binding_, observed);
			!valid)
		{
			return unexpected(lease_error("continuity-mismatch"));
		}
		return {};
	}

	result<void> sqlite_wave3_shm_lease::begin_native(const sqlite_wave3_identity_binding& observed)
	{
		if (phase_ != sqlite_wave3_lease_phase::reserved)
		{
			return fail(sqlite_wave3_lease_failure::invalid_transition);
		}
		if (auto checked = check_observed(observed); !checked)
		{
			return checked;
		}
		phase_ = sqlite_wave3_lease_phase::native_started;
		return {};
	}

	result<void>
	sqlite_wave3_shm_lease::record_pending(const sqlite_wave3_identity_binding& observed)
	{
		if (phase_ != sqlite_wave3_lease_phase::native_started)
		{
			return fail(sqlite_wave3_lease_failure::invalid_transition);
		}
		if (auto checked = check_observed(observed); !checked)
		{
			return checked;
		}
		phase_ = sqlite_wave3_lease_phase::pending;
		return {};
	}

	result<void> sqlite_wave3_shm_lease::promote(const sqlite_wave3_identity_binding& observed)
	{
		if (phase_ != sqlite_wave3_lease_phase::pending)
		{
			return fail(sqlite_wave3_lease_failure::invalid_transition);
		}
		if (auto checked = check_observed(observed); !checked)
		{
			return checked;
		}
		phase_ = sqlite_wave3_lease_phase::promoted;
		return {};
	}

	result<void> sqlite_wave3_shm_lease::admit_use()
	{
		if (phase_ != sqlite_wave3_lease_phase::promoted)
		{
			return fail(sqlite_wave3_lease_failure::invalid_transition);
		}
		if (active_use_owners_ == 64U)
		{
			return fail(sqlite_wave3_lease_failure::use_bound_exceeded);
		}
		++active_use_owners_;
		return {};
	}

	result<void> sqlite_wave3_shm_lease::release_use()
	{
		if (phase_ != sqlite_wave3_lease_phase::promoted &&
			phase_ != sqlite_wave3_lease_phase::revoked &&
			phase_ != sqlite_wave3_lease_phase::draining)
		{
			return fail(sqlite_wave3_lease_failure::invalid_transition);
		}
		if (active_use_owners_ == 0)
		{
			return fail(sqlite_wave3_lease_failure::invalid_transition);
		}
		--active_use_owners_;
		return {};
	}

	result<void> sqlite_wave3_shm_lease::revoke()
	{
		if (phase_ != sqlite_wave3_lease_phase::reserved &&
			phase_ != sqlite_wave3_lease_phase::native_started &&
			phase_ != sqlite_wave3_lease_phase::pending &&
			phase_ != sqlite_wave3_lease_phase::promoted)
		{
			return fail(sqlite_wave3_lease_failure::invalid_transition);
		}
		phase_ = sqlite_wave3_lease_phase::revoked;
		return {};
	}

	result<void> sqlite_wave3_shm_lease::begin_drain()
	{
		if (phase_ != sqlite_wave3_lease_phase::revoked)
		{
			return fail(sqlite_wave3_lease_failure::invalid_transition);
		}
		if (active_use_owners_ != 0)
		{
			return fail(sqlite_wave3_lease_failure::live_use_owners);
		}
		phase_ = sqlite_wave3_lease_phase::draining;
		return {};
	}

	result<void> sqlite_wave3_shm_lease::retire()
	{
		if (phase_ != sqlite_wave3_lease_phase::draining || active_use_owners_ != 0)
		{
			return fail(sqlite_wave3_lease_failure::invalid_transition);
		}
		phase_ = sqlite_wave3_lease_phase::retired;
		return {};
	}

	result<void> sqlite_wave3_shm_lease::quarantine(std::string detail)
	{
		if (phase_ == sqlite_wave3_lease_phase::quarantined ||
			phase_ == sqlite_wave3_lease_phase::retired)
		{
			return fail(sqlite_wave3_lease_failure::invalid_transition);
		}
		if (detail.empty())
		{
			return fail(sqlite_wave3_lease_failure::missing_quarantine_detail);
		}
		quarantine_detail_ = std::move(detail);
		phase_ = sqlite_wave3_lease_phase::quarantined;
		return {};
	}

	result<sqlite_wave3_shm_lease>
	sqlite_wave3_shm_lease_registry::reserve(sqlite_wave3_identity_binding binding)
	{
		std::lock_guard lock{mutex_};
		if (quarantined_)
		{
			return unexpected(
				lease_error(invalid_detail(sqlite_wave3_lease_failure::registry_quarantined)));
		}
		if (!admission_allowed_)
		{
			return unexpected(
				lease_error(invalid_detail(sqlite_wave3_lease_failure::registry_closed)));
		}
		if (entries_.size() >= capacity)
		{
			return unexpected(
				lease_error(invalid_detail(sqlite_wave3_lease_failure::registry_capacity)));
		}
		if (inspect_sqlite_wave3_identity(binding) != sqlite_wave3_identity_failure::none)
		{
			return unexpected(
				lease_error(invalid_detail(sqlite_wave3_lease_failure::invalid_identity)));
		}
		for (const auto& entry : entries_)
		{
			if (entry.binding == binding)
			{
				return unexpected(
					lease_error(invalid_detail(sqlite_wave3_lease_failure::duplicate_binding)));
			}
		}
		if (next_token_ == std::numeric_limits<std::uint64_t>::max())
		{
			return unexpected(
				lease_error(invalid_detail(sqlite_wave3_lease_failure::generation_exhausted)));
		}
		const auto token = next_token_++;
		entries_.push_back(entry{binding, token, false});
		return sqlite_wave3_shm_lease{std::move(binding), token};
	}

	result<void> sqlite_wave3_shm_lease_registry::retire(sqlite_wave3_shm_lease& lease)
	{
		std::lock_guard lock{mutex_};
		for (auto& entry : entries_)
		{
			if (entry.token != lease.token())
			{
				continue;
			}
			if (entry.binding != lease.binding())
			{
				return fail(sqlite_wave3_lease_failure::stale_token);
			}
			if (entry.terminal)
			{
				return fail(sqlite_wave3_lease_failure::stale_token);
			}
			if (lease.phase() != sqlite_wave3_lease_phase::retired &&
				lease.phase() != sqlite_wave3_lease_phase::quarantined)
			{
				return fail(sqlite_wave3_lease_failure::invalid_transition);
			}
			entry.terminal = true;
			return {};
		}
		return fail(sqlite_wave3_lease_failure::stale_token);
	}

	result<void> sqlite_wave3_shm_lease_registry::close_admission()
	{
		std::lock_guard lock{mutex_};
		if (quarantined_)
		{
			return fail(sqlite_wave3_lease_failure::registry_quarantined);
		}
		admission_allowed_ = false;
		return {};
	}

	result<void> sqlite_wave3_shm_lease_registry::quarantine()
	{
		std::lock_guard lock{mutex_};
		quarantined_ = true;
		admission_allowed_ = false;
		return {};
	}

	bool sqlite_wave3_shm_lease_registry::admission_allowed() const noexcept
	{
		std::lock_guard lock{mutex_};
		return admission_allowed_ && !quarantined_;
	}

	bool sqlite_wave3_shm_lease_registry::quarantined() const noexcept
	{
		std::lock_guard lock{mutex_};
		return quarantined_;
	}

	bool
	sqlite_wave3_shm_lease_registry::contains(const sqlite_wave3_identity_binding& binding) const
	{
		std::lock_guard lock{mutex_};
		for (const auto& entry : entries_)
		{
			if (entry.binding == binding)
			{
				return true;
			}
		}
		return false;
	}

	std::size_t sqlite_wave3_shm_lease_registry::size() const noexcept
	{
		std::lock_guard lock{mutex_};
		return entries_.size();
	}
} // namespace cxxlens::sdk
