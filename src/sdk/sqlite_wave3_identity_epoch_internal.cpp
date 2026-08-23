#include "sqlite_wave3_identity_epoch_internal.hpp"

#include <limits>
#include <string_view>

namespace cxxlens::sdk
{
	namespace
	{
		constexpr std::size_t identity_bytes_bound = 4096U;
		constexpr std::string_view error_code = "store.sqlite-failure";
		constexpr std::string_view error_field = "sqlite-wave3-identity";

		[[nodiscard]] error identity_error(const std::string_view detail)
		{
			return {std::string{error_code}, std::string{error_field}, std::string{detail}};
		}

		[[nodiscard]] result<void> phase_error(const std::string_view detail)
		{
			return unexpected(identity_error(detail));
		}

		[[nodiscard]] bool same_identity(const sqlite_backend_opaque_identity& lhs,
			const sqlite_backend_opaque_identity& rhs) noexcept
		{
			return lhs == rhs;
		}
	} // namespace

	sqlite_wave3_identity_failure
	inspect_sqlite_wave3_identity(const sqlite_wave3_identity_binding& binding) noexcept
	{
		const sqlite_backend_opaque_identity* identities[] = {
			&binding.process_instance,
			&binding.shared_runtime_vfs_cohort,
			&binding.exact_file_family,
			&binding.runtime_lifetime_pin,
			&binding.open_epoch,
			&binding.namespace_epoch,
			&binding.parent_namespace,
			&binding.shm_object,
			&binding.shm_entry,
			&binding.filesystem,
			&binding.mount,
		};
		for (const auto* identity : identities)
		{
			if (identity->profile.empty() || identity->bytes.empty())
			{
				return sqlite_wave3_identity_failure::missing_identity;
			}
			if (identity->bytes.size() > identity_bytes_bound)
			{
				return sqlite_wave3_identity_failure::identity_too_large;
			}
		}
		if (binding.fork_generation == 0 || binding.mapping_generation == 0)
		{
			return sqlite_wave3_identity_failure::invalid_epoch;
		}
		if (binding.page_size == 0 ||
			(binding.page_number != 0 &&
			 binding.page_size > (std::numeric_limits<std::uint64_t>::max() / binding.page_number)))
		{
			return sqlite_wave3_identity_failure::invalid_page;
		}
		return sqlite_wave3_identity_failure::none;
	}

	result<void> validate_sqlite_wave3_identity(const sqlite_wave3_identity_binding& binding)
	{
		switch (inspect_sqlite_wave3_identity(binding))
		{
		case sqlite_wave3_identity_failure::none:
			return {};
		case sqlite_wave3_identity_failure::missing_identity:
			return unexpected(identity_error("missing-identity"));
		case sqlite_wave3_identity_failure::identity_too_large:
			return unexpected(identity_error("identity-too-large"));
		case sqlite_wave3_identity_failure::invalid_epoch:
			return unexpected(identity_error("invalid-epoch"));
		case sqlite_wave3_identity_failure::invalid_page:
			return unexpected(identity_error("invalid-page"));
		case sqlite_wave3_identity_failure::continuity_mismatch:
			return unexpected(identity_error("continuity-mismatch"));
		}
		return unexpected(identity_error("invalid-state"));
	}

	result<void> validate_sqlite_wave3_identity_continuity(
		const sqlite_wave3_identity_binding& expected,
		const sqlite_wave3_identity_binding& observed)
	{
		if (const auto expected_failure = inspect_sqlite_wave3_identity(expected);
			expected_failure != sqlite_wave3_identity_failure::none)
		{
			return validate_sqlite_wave3_identity(expected);
		}
		if (const auto observed_failure = inspect_sqlite_wave3_identity(observed);
			observed_failure != sqlite_wave3_identity_failure::none)
		{
			return validate_sqlite_wave3_identity(observed);
		}
		if (!same_identity(expected.process_instance, observed.process_instance) ||
			!same_identity(expected.shared_runtime_vfs_cohort, observed.shared_runtime_vfs_cohort) ||
			!same_identity(expected.exact_file_family, observed.exact_file_family) ||
			!same_identity(expected.runtime_lifetime_pin, observed.runtime_lifetime_pin) ||
			!same_identity(expected.open_epoch, observed.open_epoch) ||
			!same_identity(expected.namespace_epoch, observed.namespace_epoch) ||
			!same_identity(expected.parent_namespace, observed.parent_namespace) ||
			!same_identity(expected.shm_object, observed.shm_object) ||
			!same_identity(expected.shm_entry, observed.shm_entry) ||
			!same_identity(expected.filesystem, observed.filesystem) ||
			!same_identity(expected.mount, observed.mount) ||
			expected.fork_generation != observed.fork_generation ||
			expected.mapping_generation != observed.mapping_generation ||
			expected.page_number != observed.page_number ||
			expected.page_size != observed.page_size)
		{
			return unexpected(identity_error("continuity-mismatch"));
		}
		return {};
	}

	sqlite_wave3_epoch_phase sqlite_wave3_epoch_controller::phase() const noexcept
	{
		return phase_;
	}

	bool sqlite_wave3_epoch_controller::admission_allowed() const noexcept
	{
		return phase_ == sqlite_wave3_epoch_phase::armed ||
			phase_ == sqlite_wave3_epoch_phase::native_started;
	}

	bool sqlite_wave3_epoch_controller::terminal() const noexcept
	{
		return phase_ == sqlite_wave3_epoch_phase::sealed ||
			phase_ == sqlite_wave3_epoch_phase::revoked ||
			phase_ == sqlite_wave3_epoch_phase::quarantined;
	}

	const std::optional<sqlite_wave3_identity_binding>&
	sqlite_wave3_epoch_controller::binding() const noexcept
	{
		return binding_;
	}

	std::string_view sqlite_wave3_epoch_controller::quarantine_detail() const noexcept
	{
		return quarantine_detail_;
	}

	result<void> sqlite_wave3_epoch_controller::arm(sqlite_wave3_identity_binding binding)
	{
		if (phase_ != sqlite_wave3_epoch_phase::unresolved)
		{
			return phase_error("invalid-arm-transition");
		}
		if (auto valid = validate_sqlite_wave3_identity(binding); !valid)
		{
			return valid;
		}
		binding_ = std::move(binding);
		phase_ = sqlite_wave3_epoch_phase::armed;
		return {};
	}

	result<void> sqlite_wave3_epoch_controller::start_native(
		const sqlite_wave3_identity_binding& observed)
	{
		if (phase_ != sqlite_wave3_epoch_phase::armed || !binding_)
		{
			return phase_error("invalid-native-transition");
		}
		if (auto continuity = validate_sqlite_wave3_identity_continuity(*binding_, observed);
			!continuity)
		{
			return continuity;
		}
		phase_ = sqlite_wave3_epoch_phase::native_started;
		return {};
	}

	result<void> sqlite_wave3_epoch_controller::seal(
		const sqlite_wave3_identity_binding& observed)
	{
		if (phase_ != sqlite_wave3_epoch_phase::native_started || !binding_)
		{
			return phase_error("invalid-seal-transition");
		}
		if (auto continuity = validate_sqlite_wave3_identity_continuity(*binding_, observed);
			!continuity)
		{
			return continuity;
		}
		phase_ = sqlite_wave3_epoch_phase::sealed;
		return {};
	}

	result<void> sqlite_wave3_epoch_controller::revoke()
	{
		if (phase_ != sqlite_wave3_epoch_phase::armed &&
			phase_ != sqlite_wave3_epoch_phase::native_started)
		{
			return phase_error("invalid-revoke-transition");
		}
		phase_ = sqlite_wave3_epoch_phase::revoked;
		return {};
	}

	result<void> sqlite_wave3_epoch_controller::quarantine(std::string detail)
	{
		if (phase_ == sqlite_wave3_epoch_phase::quarantined)
		{
			return phase_error("already-quarantined");
		}
		if (detail.empty())
		{
			return phase_error("missing-quarantine-detail");
		}
		quarantine_detail_ = std::move(detail);
		phase_ = sqlite_wave3_epoch_phase::quarantined;
		return {};
	}
} // namespace cxxlens::sdk
