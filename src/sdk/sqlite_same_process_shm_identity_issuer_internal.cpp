#include "sqlite_same_process_shm_identity_issuer_internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <thread>
#include <utility>

namespace cxxlens::sdk
{
	namespace
	{
		static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
					  "the qualified fork profile requires a lock-free process epoch and issuer "
					  "sequence");
		static_assert(std::atomic<std::uint8_t>::is_always_lock_free,
					  "the qualified fork profile requires lock-free owner phases");
		static_assert(std::atomic<sqlite_shm_reader_lifecycle_owner_phase>::is_always_lock_free,
					  "the qualified fork profile requires a lock-free typed owner phase");
		static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
					  "the qualified fork profile requires lock-free role claims");
		static_assert(std::atomic<std::size_t>::is_always_lock_free,
					  "the qualified fork profile requires lock-free owner counters");
		static_assert(std::atomic_bool::is_always_lock_free,
					  "the qualified fork profile requires lock-free scope phases");

		[[nodiscard]] bool valid_identity(const sqlite_backend_opaque_identity& value) noexcept
		{
			return !value.profile.empty() && !value.bytes.empty();
		}

		[[nodiscard]] bool valid_family(const sqlite_shm_lease_family_binding& family) noexcept
		{
			return valid_identity(family.process_instance) &&
				valid_identity(family.shared_runtime_vfs_cohort) &&
				valid_identity(family.exact_file_family);
		}

		[[nodiscard]] bool
		valid_owner_kind(const sqlite_shm_reader_lifecycle_owner_kind value) noexcept
		{
			switch (value)
			{
				case sqlite_shm_reader_lifecycle_owner_kind::map:
				case sqlite_shm_reader_lifecycle_owner_kind::unpublished_cleanup:
				case sqlite_shm_reader_lifecycle_owner_kind::attachment:
				case sqlite_shm_reader_lifecycle_owner_kind::close:
				case sqlite_shm_reader_lifecycle_owner_kind::logical_ack:
				case sqlite_shm_reader_lifecycle_owner_kind::late_outer_unwind:
				case sqlite_shm_reader_lifecycle_owner_kind::session:
					return true;
			}
			return false;
		}

		[[nodiscard]] bool
		valid_callback_role(const sqlite_shm_reader_callback_identity_role value) noexcept
		{
			switch (value)
			{
				case sqlite_shm_reader_callback_identity_role::map:
				case sqlite_shm_reader_callback_identity_role::unpublished_cleanup_unmap:
				case sqlite_shm_reader_callback_identity_role::attachment_unmap:
				case sqlite_shm_reader_callback_identity_role::close:
				case sqlite_shm_reader_callback_identity_role::logical_ack_unmap:
				case sqlite_shm_reader_callback_identity_role::late_outer_unmap:
					return true;
			}
			return false;
		}

		[[nodiscard]] bool
		valid_effect_role(const sqlite_shm_reader_effect_identity_role value) noexcept
		{
			switch (value)
			{
				case sqlite_shm_reader_effect_identity_role::mapped_result:
				case sqlite_shm_reader_effect_identity_role::zero_attachment_result:
				case sqlite_shm_reader_effect_identity_role::native_unmap:
				case sqlite_shm_reader_effect_identity_role::latch_reset:
				case sqlite_shm_reader_effect_identity_role::native_close:
					return true;
			}
			return false;
		}

		[[nodiscard]] bool valid_session_terminal_role(
			const sqlite_shm_reader_session_terminal_identity_role value) noexcept
		{
			switch (value)
			{
				case sqlite_shm_reader_session_terminal_identity_role::success:
				case sqlite_shm_reader_session_terminal_identity_role::failure:
				case sqlite_shm_reader_session_terminal_identity_role::
					cancelled_before_authority_read:
					return true;
			}
			return false;
		}

		[[nodiscard]] bool
		scope_accepts_callback(const sqlite_shm_reader_lifecycle_owner_kind owner,
							   const sqlite_shm_reader_callback_identity_role role) noexcept
		{
			using owner_kind = sqlite_shm_reader_lifecycle_owner_kind;
			using callback_role = sqlite_shm_reader_callback_identity_role;
			return (owner == owner_kind::map && role == callback_role::map) ||
				(owner == owner_kind::unpublished_cleanup &&
				 role == callback_role::unpublished_cleanup_unmap) ||
				(owner == owner_kind::attachment && role == callback_role::attachment_unmap) ||
				(owner == owner_kind::close && role == callback_role::close) ||
				(owner == owner_kind::logical_ack && role == callback_role::logical_ack_unmap) ||
				(owner == owner_kind::late_outer_unwind && role == callback_role::late_outer_unmap);
		}

		[[nodiscard]] bool
		callback_accepts_effect(const sqlite_shm_reader_callback_identity_role callback,
								const sqlite_shm_reader_effect_identity_role effect) noexcept
		{
			using callback_role = sqlite_shm_reader_callback_identity_role;
			using effect_role = sqlite_shm_reader_effect_identity_role;
			switch (callback)
			{
				case callback_role::map:
					return effect == effect_role::mapped_result ||
						effect == effect_role::zero_attachment_result;
				case callback_role::unpublished_cleanup_unmap:
				case callback_role::attachment_unmap:
					return effect == effect_role::native_unmap ||
						effect == effect_role::latch_reset;
				case callback_role::close:
					return effect == effect_role::native_close;
				case callback_role::logical_ack_unmap:
				case callback_role::late_outer_unmap:
					return false;
			}
			return false;
		}

		template <class Value>
		[[nodiscard]] bool checked_increment(std::atomic<Value>& value) noexcept
		{
			auto observed = value.load(std::memory_order_acquire);
			while (observed != std::numeric_limits<Value>::max())
			{
				if (value.compare_exchange_weak(observed,
												static_cast<Value>(observed + 1U),
												std::memory_order_acq_rel,
												std::memory_order_acquire))
					return true;
			}
			return false;
		}

		template <class Value>
		[[nodiscard]] bool checked_decrement(std::atomic<Value>& value) noexcept
		{
			auto observed = value.load(std::memory_order_acquire);
			while (observed != 0U)
			{
				if (value.compare_exchange_weak(observed,
												static_cast<Value>(observed - 1U),
												std::memory_order_acq_rel,
												std::memory_order_acquire))
					return true;
			}
			return false;
		}

		[[nodiscard]] bool claim_role(std::atomic<std::uint32_t>& claims,
									  const std::uint8_t role) noexcept
		{
			if (role >= 32U)
				return false;
			const auto bit = static_cast<std::uint32_t>(1U) << role;
			auto observed = claims.load(std::memory_order_acquire);
			while ((observed & bit) == 0U)
			{
				if (claims.compare_exchange_weak(observed,
												 observed | bit,
												 std::memory_order_acq_rel,
												 std::memory_order_acquire))
					return true;
			}
			return false;
		}

		[[nodiscard]] bool
		claim_effect_role(std::atomic<std::uint32_t>& claims,
						  const sqlite_shm_reader_callback_identity_role callback,
						  const sqlite_shm_reader_effect_identity_role effect) noexcept
		{
			const auto role = static_cast<std::uint8_t>(effect);
			if (role >= 32U)
				return false;
			const auto bit = static_cast<std::uint32_t>(1U) << role;
			const auto mapped_bit = static_cast<std::uint32_t>(1U)
				<< static_cast<std::uint8_t>(sqlite_shm_reader_effect_identity_role::mapped_result);
			const auto zero_bit =
				static_cast<std::uint32_t>(1U) << static_cast<std::uint8_t>(
					sqlite_shm_reader_effect_identity_role::zero_attachment_result);
			auto observed = claims.load(std::memory_order_acquire);
			while ((observed & bit) == 0U)
			{
				if (callback == sqlite_shm_reader_callback_identity_role::map &&
					(observed & (mapped_bit | zero_bit)) != 0U)
					return false;
				if (claims.compare_exchange_weak(observed,
												 observed | bit,
												 std::memory_order_acq_rel,
												 std::memory_order_acquire))
					return true;
			}
			return false;
		}

		[[nodiscard]] bool claim_session_terminal_role(
			std::atomic<std::uint32_t>& claims,
			const sqlite_shm_reader_session_terminal_identity_role role) noexcept
		{
			const auto value = static_cast<std::uint8_t>(role);
			if (value >= 32U)
				return false;
			auto expected = std::uint32_t{0U};
			return claims.compare_exchange_strong(expected,
												  static_cast<std::uint32_t>(1U) << value,
												  std::memory_order_acq_rel,
												  std::memory_order_acquire);
		}

		[[nodiscard]] sqlite_shm_lease_rejection
		reject(const sqlite_shm_lease_rejection_reason reason,
			   const sqlite_shm_lease_recovery_action action =
				   sqlite_shm_lease_recovery_action::deny_before_native_map) noexcept
		{
			return {reason, action};
		}

		template <class Integer>
		void append_integer(std::vector<std::byte>& bytes, const Integer value)
		{
			for (std::size_t index = 0; index < sizeof(value); ++index)
				bytes.push_back(static_cast<std::byte>(
					(value >> static_cast<unsigned>(index * 8U)) & static_cast<Integer>(0xffU)));
		}

		std::atomic<std::uint64_t> next_issuer_incarnation{1U};

		template <class Control>
		[[nodiscard]] std::shared_ptr<Control> make_fork_safe_identity_control(
			const std::shared_ptr<std::atomic<std::uint64_t>>& process_epoch,
			const std::uint64_t expected_process_epoch)
		{
			return std::shared_ptr<Control>{
				new Control{},
				[process_epoch, expected_process_epoch](Control* control) noexcept
				{
					if (process_epoch &&
						process_epoch->load(std::memory_order_acquire) == expected_process_epoch)
						delete control;
				}};
		}

		[[nodiscard]] std::optional<std::uint64_t> allocate_issuer_incarnation() noexcept
		{
			auto observed = next_issuer_incarnation.load(std::memory_order_acquire);
			while (observed != 0U)
			{
				const auto replacement =
					observed == std::numeric_limits<std::uint64_t>::max() ? 0U : observed + 1U;
				if (next_issuer_incarnation.compare_exchange_weak(observed,
																  replacement,
																  std::memory_order_acq_rel,
																  std::memory_order_acquire))
					return observed;
			}
			return std::nullopt;
		}
	} // namespace

	namespace detail
	{
		enum class sqlite_shm_process_identity_record_phase : std::uint8_t
		{
			reserved,
			sealed,
			issuing_effect,
			retiring,
			retired,
			abandoned,
		};

		static_assert(std::atomic<sqlite_shm_process_identity_record_phase>::is_always_lock_free,
					  "the qualified fork profile requires lock-free record phases");

		struct sqlite_shm_reader_lifecycle_identity_scope_control
		{
			std::weak_ptr<sqlite_shm_process_identity_issuer_state> issuer;
			std::shared_ptr<std::atomic<std::uint64_t>> process_epoch;
			std::uint64_t expected_process_epoch{};
			std::weak_ptr<void> registry_state;
			std::shared_ptr<std::atomic_bool> registry_quarantine_latch;
			std::shared_ptr<std::atomic_bool> registry_issuer_owner_latch;
			std::shared_ptr<std::atomic_bool> family_authority_latch;
			std::uint64_t family_epoch{};
			std::uint64_t family_pin_token{};
			sqlite_shm_lease_family_binding family;
			sqlite_backend_opaque_identity callback_cohort;
			sqlite_backend_opaque_identity request_seal;
			sqlite_shm_reader_lifecycle_owner_coordinates coordinates;
			std::atomic_bool active{true};
			std::atomic_bool quarantined{false};
			std::atomic_bool qualified_completion{false};
			std::atomic<std::size_t> live_records{0U};
			std::atomic<std::uint32_t> issued_callback_roles{0U};
			std::atomic<std::uint32_t> issued_session_terminal_roles{0U};
			bool enforce_owner_phase{};
			std::shared_ptr<std::atomic<sqlite_shm_reader_lifecycle_owner_phase>> owner_phase;
			std::weak_ptr<sqlite_shm_reader_lifecycle_owner_abandonment_control> owner_abandonment;
		};

		struct sqlite_shm_process_identity_record_control
		{
			std::weak_ptr<sqlite_shm_process_identity_issuer_state> issuer;
			std::shared_ptr<std::atomic<std::uint64_t>> process_epoch;
			std::uint64_t expected_process_epoch{};
			std::shared_ptr<sqlite_shm_reader_lifecycle_identity_scope_control> scope;
			std::uint64_t sequence{};
			sqlite_shm_reader_lifecycle_identity_domain domain{
				sqlite_shm_reader_lifecycle_identity_domain::callback_invocation};
			std::uint8_t role{};
			std::weak_ptr<sqlite_shm_process_identity_record_control> parent_callback;
			std::atomic<std::size_t> live_children{0U};
			std::atomic<std::uint32_t> issued_effect_roles{0U};
			std::atomic<sqlite_shm_process_identity_record_phase> phase{
				sqlite_shm_process_identity_record_phase::reserved};
			sqlite_backend_opaque_identity thread_identity;
			std::uint64_t reentrancy_depth{};
			sqlite_backend_opaque_identity projection;
		};

		[[nodiscard]] bool scope_control_current(
			const std::shared_ptr<sqlite_shm_reader_lifecycle_identity_scope_control>&
				control) noexcept
		{
			if (!control || !control->process_epoch || control->expected_process_epoch == 0U ||
				control->process_epoch->load(std::memory_order_acquire) !=
					control->expected_process_epoch ||
				!control->active.load(std::memory_order_acquire) ||
				control->quarantined.load(std::memory_order_acquire) ||
				!control->registry_quarantine_latch ||
				control->registry_quarantine_latch->load(std::memory_order_acquire) ||
				!control->registry_issuer_owner_latch ||
				!control->registry_issuer_owner_latch->load(std::memory_order_acquire) ||
				!control->family_authority_latch || control->registry_state.expired() ||
				control->issuer.expired())
				return false;

			// Read the family latch before the owner phase. An admission owner needs both;
			// an already-owned qualified lifecycle remains present for its exact terminal path.
			const auto family_live =
				control->family_authority_latch->load(std::memory_order_acquire);
			if (!control->enforce_owner_phase)
				return family_live;
			if (!control->owner_phase)
				return false;
			const auto phase = control->owner_phase->load(std::memory_order_acquire);
			return phase == sqlite_shm_reader_lifecycle_owner_phase::owned ||
				(family_live && phase == sqlite_shm_reader_lifecycle_owner_phase::admission);
		}

		void abandon_scope_owner(
			const std::shared_ptr<sqlite_shm_reader_lifecycle_identity_scope_control>&
				control) noexcept
		{
			if (!control || !control->enforce_owner_phase || !control->process_epoch ||
				control->process_epoch->load(std::memory_order_acquire) !=
					control->expected_process_epoch)
				return;
			if (auto owner = control->owner_abandonment.lock())
				owner->abandon();
		}

		void
		quarantine_scope(const std::shared_ptr<sqlite_shm_reader_lifecycle_identity_scope_control>&
							 control) noexcept
		{
			if (!control)
				return;
			control->quarantined.store(true, std::memory_order_release);
			control->active.store(false, std::memory_order_release);
			abandon_scope_owner(control);
		}

		[[nodiscard]] bool decrement_accounting(
			std::atomic<std::size_t>& counter,
			const std::shared_ptr<sqlite_shm_reader_lifecycle_identity_scope_control>&
				scope) noexcept
		{
			const auto accounted = checked_decrement(counter);
			if (!accounted)
				quarantine_scope(scope);
			return accounted;
		}

		[[nodiscard]] bool increment_accounting(
			std::atomic<std::size_t>& counter,
			const std::shared_ptr<sqlite_shm_reader_lifecycle_identity_scope_control>&
				scope) noexcept
		{
			const auto accounted = checked_increment(counter);
			if (!accounted)
				quarantine_scope(scope);
			return accounted;
		}

		[[nodiscard]] bool record_parent_current(
			const std::shared_ptr<sqlite_shm_process_identity_record_control>& control) noexcept
		{
			if (!control ||
				control->domain !=
					sqlite_shm_reader_lifecycle_identity_domain::native_or_zero_effect)
				return true;
			const auto parent = control->parent_callback.lock();
			if (!parent || !scope_control_current(parent->scope))
				return false;
			const auto phase = parent->phase.load(std::memory_order_acquire);
			return phase == sqlite_shm_process_identity_record_phase::sealed ||
				phase == sqlite_shm_process_identity_record_phase::issuing_effect;
		}

		class sqlite_shm_process_identity_issuer_state final
			: public std::enable_shared_from_this<sqlite_shm_process_identity_issuer_state>
		{
		  public:
			struct counter_seed
			{
				std::uint64_t incarnation{};
				std::uint64_t first_sequence{};
			};

			sqlite_shm_process_identity_issuer_state(
				std::weak_ptr<void> registry_state,
				std::shared_ptr<std::atomic<std::uint64_t>> process_epoch,
				std::shared_ptr<std::atomic_bool> registry_quarantine_latch,
				std::shared_ptr<std::atomic_bool> registry_issuer_owner_latch,
				const std::uint64_t expected_process_epoch,
				sqlite_backend_opaque_identity process_instance,
				const counter_seed counters) noexcept
				: registry_state_{std::move(registry_state)},
				  process_epoch_{std::move(process_epoch)},
				  registry_quarantine_latch_{std::move(registry_quarantine_latch)},
				  registry_issuer_owner_latch_{std::move(registry_issuer_owner_latch)},
				  expected_process_epoch_{expected_process_epoch},
				  process_instance_{std::move(process_instance)},
				  incarnation_{counters.incarnation}, next_sequence_{counters.first_sequence}
			{
			}

			[[nodiscard]] bool current_before_owner_lock() const noexcept
			{
				return process_epoch_ && expected_process_epoch_ != 0U &&
					process_epoch_->load(std::memory_order_acquire) == expected_process_epoch_ &&
					registry_quarantine_latch_ &&
					!registry_quarantine_latch_->load(std::memory_order_acquire) &&
					registry_issuer_owner_latch_ &&
					registry_issuer_owner_latch_->load(std::memory_order_acquire);
			}

			[[nodiscard]] bool current() const noexcept
			{
				return current_before_owner_lock() && !registry_state_.expired();
			}

			[[nodiscard]] sqlite_shm_reader_lifecycle_identity_scope
			seal_scope(const sqlite_shm_lease_family_binding& family,
					   std::shared_ptr<std::atomic_bool> family_authority_latch,
					   const std::uint64_t family_epoch,
					   const std::uint64_t family_pin_token,
					   const sqlite_backend_opaque_identity& callback_cohort,
					   const sqlite_backend_opaque_identity& request_seal,
					   const sqlite_shm_reader_lifecycle_owner_coordinates& coordinates,
					   const bool enforce_owner_phase = false,
					   std::shared_ptr<std::atomic<sqlite_shm_reader_lifecycle_owner_phase>>
						   owner_phase = {},
					   std::weak_ptr<sqlite_shm_reader_lifecycle_owner_abandonment_control>
						   owner_abandonment = {})
			{
				if (!current_before_owner_lock() || !valid_family(family) ||
					family.process_instance != process_instance_ || !family_authority_latch ||
					!family_authority_latch->load(std::memory_order_acquire) ||
					family_epoch == 0U || family_pin_token == 0U ||
					!valid_identity(callback_cohort) || !valid_identity(request_seal) ||
					coordinates.registry_open_token == 0U ||
					!valid_owner_kind(coordinates.owner_kind) ||
					coordinates.lifecycle_owner_token == 0U ||
					(coordinates.writer_mapping_generation == 0U &&
					 coordinates.owner_kind != sqlite_shm_reader_lifecycle_owner_kind::close) ||
					registry_state_.expired() ||
					(enforce_owner_phase &&
					 (!owner_phase ||
					  owner_phase->load(std::memory_order_acquire) !=
						  sqlite_shm_reader_lifecycle_owner_phase::admission ||
					  owner_abandonment.expired())))
					return sqlite_shm_reader_lifecycle_identity_scope{nullptr};

				auto control = make_fork_safe_identity_control<
					sqlite_shm_reader_lifecycle_identity_scope_control>(process_epoch_,
																		expected_process_epoch_);
				control->issuer = weak_from_this();
				control->process_epoch = process_epoch_;
				control->expected_process_epoch = expected_process_epoch_;
				control->registry_state = registry_state_;
				control->registry_quarantine_latch = registry_quarantine_latch_;
				control->registry_issuer_owner_latch = registry_issuer_owner_latch_;
				control->family_authority_latch = std::move(family_authority_latch);
				control->family_epoch = family_epoch;
				control->family_pin_token = family_pin_token;
				control->family = family;
				control->callback_cohort = callback_cohort;
				control->request_seal = request_seal;
				control->coordinates = coordinates;
				control->enforce_owner_phase = enforce_owner_phase;
				control->owner_phase = std::move(owner_phase);
				control->owner_abandonment = std::move(owner_abandonment);
				if (!scope_control_current(control) || control->issuer.lock().get() != this)
				{
					control->active.store(false, std::memory_order_release);
					return sqlite_shm_reader_lifecycle_identity_scope{nullptr};
				}
				return sqlite_shm_reader_lifecycle_identity_scope{std::move(control)};
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_callback_identity_permit>
			reserve_callback(const sqlite_shm_reader_lifecycle_identity_scope& scope,
							 sqlite_shm_reader_callback_identity_role role,
							 sqlite_backend_opaque_identity thread_identity,
							 const std::uint64_t reentrancy_depth)
			{
				if (!current_before_owner_lock())
					return reject(sqlite_shm_lease_rejection_reason::stale_token);
				const auto scope_control = scope.control_;
				if (!scope_matches_control(scope_control) || !valid_identity(thread_identity) ||
					!valid_callback_role(role) ||
					!scope_accepts_callback(scope_control->coordinates.owner_kind, role))
					return reject(sqlite_shm_lease_rejection_reason::invalid_identity);
				if (!claim_role(scope_control->issued_callback_roles,
								static_cast<std::uint8_t>(role)))
					return reject(sqlite_shm_lease_rejection_reason::stale_token);

				const auto sequence = allocate_sequence();
				if (!sequence)
				{
					abandon_scope_owner(scope_control);
					return reject(sqlite_shm_lease_rejection_reason::generation_exhausted);
				}
				if (!current_before_owner_lock())
				{
					abandon_scope_owner(scope_control);
					return reject(sqlite_shm_lease_rejection_reason::stale_token);
				}

				std::shared_ptr<sqlite_shm_process_identity_record_control> control;
				bool scope_counted{};
				try
				{
					control =
						make_fork_safe_identity_control<sqlite_shm_process_identity_record_control>(
							process_epoch_, expected_process_epoch_);
					control->issuer = weak_from_this();
					control->process_epoch = process_epoch_;
					control->expected_process_epoch = expected_process_epoch_;
					control->scope = scope_control;
					control->sequence = *sequence;
					control->domain =
						sqlite_shm_reader_lifecycle_identity_domain::callback_invocation;
					control->role = static_cast<std::uint8_t>(role);
					control->thread_identity = std::move(thread_identity);
					control->reentrancy_depth = reentrancy_depth;
					control->projection = make_projection(
						*sequence,
						sqlite_shm_reader_lifecycle_identity_domain::callback_invocation,
						control->role,
						*scope_control);
					if (!increment_accounting(scope_control->live_records, scope_control))
					{
						abandon_scope_owner(scope_control);
						return reject(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									  sqlite_shm_lease_recovery_action::quarantine_no_retry);
					}
					scope_counted = true;
#if defined(CXXLENS_SQLITE_TEST_SUPPORT)
					pause_for_testing(sqlite_shm_identity_issuer_pause_point_for_testing::
										  reserve_after_scope_count);
#endif
					if (!scope_matches_control(scope_control))
					{
						const auto accounted =
							decrement_accounting(scope_control->live_records, scope_control);
						scope_counted = false;
						control->phase.store(sqlite_shm_process_identity_record_phase::abandoned,
											 std::memory_order_release);
						abandon_scope_owner(scope_control);
						return accounted
							? sqlite_shm_lease_result<
								  sqlite_shm_reader_callback_identity_permit>{reject(
								  sqlite_shm_lease_rejection_reason::stale_token)}
							: sqlite_shm_lease_result<sqlite_shm_reader_callback_identity_permit>{
								  reject(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry)};
					}
					auto output = sqlite_shm_reader_callback_identity_permit{control};
					return output;
				}
				catch (...)
				{
					if (scope_counted)
						abandon_record(control);
					quarantine_scope(scope_control);
					return reject(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								  sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_issued_reader_callback_identity>
			seal_callback(sqlite_shm_reader_callback_identity_permit& permit,
						  const sqlite_shm_reader_lifecycle_identity_scope& scope,
						  const sqlite_shm_reader_callback_identity_role role) noexcept
			{
				if (!current_before_owner_lock())
					return reject(sqlite_shm_lease_rejection_reason::stale_token);
				const auto scope_control = scope.control_;
				const auto permit_control = permit.control_;
				if (!scope_matches_control(scope_control) ||
					!record_matches(
						permit_control,
						scope,
						sqlite_shm_reader_lifecycle_identity_domain::callback_invocation,
						static_cast<std::uint8_t>(role),
						sqlite_shm_process_identity_record_phase::reserved))
					return reject(sqlite_shm_lease_rejection_reason::receipt_mismatch);
				try
				{
					auto receipt = sqlite_shm_callback_execution_receipt{
						permit_control->thread_identity,
						permit_control->reentrancy_depth,
						permit_control->projection,
					};
					auto expected = sqlite_shm_process_identity_record_phase::reserved;
					if (!permit_control->phase.compare_exchange_strong(
							expected,
							sqlite_shm_process_identity_record_phase::sealed,
							std::memory_order_acq_rel,
							std::memory_order_acquire))
						return reject(sqlite_shm_lease_rejection_reason::stale_token);
					if (!scope_matches_control(scope_control))
					{
						abandon_record(permit_control);
						return reject(sqlite_shm_lease_rejection_reason::stale_token);
					}
					auto control = std::move(permit.control_);
					auto output = sqlite_shm_issued_reader_callback_identity{std::move(control),
																			 std::move(receipt)};
					return output;
				}
				catch (...)
				{
					abandon_record(permit.control_);
					quarantine_scope(scope_control);
					return reject(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								  sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_issued_reader_effect_identity>
			issue_effect(const sqlite_shm_reader_lifecycle_identity_scope& scope,
						 const sqlite_shm_issued_reader_callback_identity& callback,
						 const sqlite_shm_reader_effect_identity_role role)
			{
				if (!current_before_owner_lock())
					return reject(sqlite_shm_lease_rejection_reason::stale_token);
				const auto scope_control = scope.control_;
				const auto callback_control = callback.control_;
				if (scope_control && scope_control->enforce_owner_phase &&
					(!scope_control->owner_phase ||
					 scope_control->owner_phase->load(std::memory_order_acquire) !=
						 sqlite_shm_reader_lifecycle_owner_phase::owned))
					return reject(sqlite_shm_lease_rejection_reason::stale_token);
				if (!scope_matches_control(scope_control) || !callback_control ||
					!callback_matches(scope, callback) || !valid_effect_role(role) ||
					!callback_accepts_effect(static_cast<sqlite_shm_reader_callback_identity_role>(
												 callback_control->role),
											 role))
					return reject(sqlite_shm_lease_rejection_reason::receipt_mismatch);
				auto callback_phase = sqlite_shm_process_identity_record_phase::sealed;
				if (!callback_control->phase.compare_exchange_strong(
						callback_phase,
						sqlite_shm_process_identity_record_phase::issuing_effect,
						std::memory_order_acq_rel,
						std::memory_order_acquire))
					return reject(sqlite_shm_lease_rejection_reason::retiring);
				if (!scope_matches_control(scope_control))
				{
					callback_control->phase.store(sqlite_shm_process_identity_record_phase::sealed,
												  std::memory_order_release);
					return reject(sqlite_shm_lease_rejection_reason::stale_token);
				}
#if defined(CXXLENS_SQLITE_TEST_SUPPORT)
				pause_for_testing(sqlite_shm_identity_issuer_pause_point_for_testing::
									  effect_after_callback_phase);
#endif
				const auto callback_role =
					static_cast<sqlite_shm_reader_callback_identity_role>(callback_control->role);
				if (!claim_effect_role(callback_control->issued_effect_roles, callback_role, role))
				{
					callback_control->phase.store(sqlite_shm_process_identity_record_phase::sealed,
												  std::memory_order_release);
					return reject(sqlite_shm_lease_rejection_reason::stale_token);
				}
				if (!increment_accounting(callback_control->live_children, scope_control))
				{
					callback_control->phase.store(sqlite_shm_process_identity_record_phase::sealed,
												  std::memory_order_release);
					abandon_scope_owner(scope_control);
					return reject(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								  sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}

				bool scope_counted{};
				bool wrapper_owns_counts{};
				std::shared_ptr<sqlite_shm_process_identity_record_control> control;
				try
				{
					const auto sequence = allocate_sequence();
					if (!sequence)
					{
						const auto accounted =
							decrement_accounting(callback_control->live_children, scope_control);
						callback_control->phase.store(
							sqlite_shm_process_identity_record_phase::sealed,
							std::memory_order_release);
						abandon_scope_owner(scope_control);
						return reject(
							accounted ? sqlite_shm_lease_rejection_reason::generation_exhausted
									  : sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
							accounted ? sqlite_shm_lease_recovery_action::deny_before_native_map
									  : sqlite_shm_lease_recovery_action::quarantine_no_retry);
					}
					if (!current_before_owner_lock())
					{
						const auto accounted =
							decrement_accounting(callback_control->live_children, scope_control);
						callback_control->phase.store(
							sqlite_shm_process_identity_record_phase::sealed,
							std::memory_order_release);
						abandon_scope_owner(scope_control);
						return reject(
							accounted ? sqlite_shm_lease_rejection_reason::stale_token
									  : sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
							accounted ? sqlite_shm_lease_recovery_action::deny_before_native_map
									  : sqlite_shm_lease_recovery_action::quarantine_no_retry);
					}

					control =
						make_fork_safe_identity_control<sqlite_shm_process_identity_record_control>(
							process_epoch_, expected_process_epoch_);
					control->issuer = weak_from_this();
					control->process_epoch = process_epoch_;
					control->expected_process_epoch = expected_process_epoch_;
					control->scope = scope_control;
					control->sequence = *sequence;
					control->domain =
						sqlite_shm_reader_lifecycle_identity_domain::native_or_zero_effect;
					control->role = static_cast<std::uint8_t>(role);
					control->parent_callback = callback_control;
					control->projection = make_projection(
						*sequence,
						sqlite_shm_reader_lifecycle_identity_domain::native_or_zero_effect,
						control->role,
						*scope_control);
					auto projection = control->projection;
					if (!increment_accounting(scope_control->live_records, scope_control))
					{
						(void)decrement_accounting(callback_control->live_children, scope_control);
						callback_control->phase.store(
							sqlite_shm_process_identity_record_phase::sealed,
							std::memory_order_release);
						abandon_scope_owner(scope_control);
						return reject(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									  sqlite_shm_lease_recovery_action::quarantine_no_retry);
					}
					scope_counted = true;
					if (callback_control->phase.load(std::memory_order_acquire) !=
							sqlite_shm_process_identity_record_phase::issuing_effect ||
						!scope_matches_control(scope_control))
					{
						const auto child_accounted =
							decrement_accounting(callback_control->live_children, scope_control);
						const auto scope_accounted =
							decrement_accounting(scope_control->live_records, scope_control);
						scope_counted = false;
						callback_control->phase.store(
							sqlite_shm_process_identity_record_phase::sealed,
							std::memory_order_release);
						abandon_scope_owner(scope_control);
						return reject(child_accounted && scope_accounted
										  ? sqlite_shm_lease_rejection_reason::stale_token
										  : sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									  child_accounted && scope_accounted
										  ? sqlite_shm_lease_recovery_action::deny_before_native_map
										  : sqlite_shm_lease_recovery_action::quarantine_no_retry);
					}
					control->phase.store(sqlite_shm_process_identity_record_phase::sealed,
										 std::memory_order_release);
					auto output =
						sqlite_shm_issued_reader_effect_identity{control, std::move(projection)};
					wrapper_owns_counts = true;
					callback_control->phase.store(sqlite_shm_process_identity_record_phase::sealed,
												  std::memory_order_release);
					return output;
				}
				catch (...)
				{
					callback_control->phase.store(sqlite_shm_process_identity_record_phase::sealed,
												  std::memory_order_release);
					if (!wrapper_owns_counts)
					{
						if (scope_counted && control)
							abandon_record(control);
						else
							(void)decrement_accounting(callback_control->live_children,
													   scope_control);
					}
					quarantine_scope(scope_control);
					return reject(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								  sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<
				sqlite_shm_issued_reader_session_terminal_identity>
			issue_session_terminal(const sqlite_shm_reader_lifecycle_identity_scope& scope,
								   const sqlite_shm_reader_session_terminal_identity_role role)
			{
				if (!current_before_owner_lock())
					return reject(sqlite_shm_lease_rejection_reason::stale_token);
				const auto scope_control = scope.control_;
				if (!scope_matches_control(scope_control) || !valid_session_terminal_role(role) ||
					scope_control->coordinates.owner_kind !=
						sqlite_shm_reader_lifecycle_owner_kind::session ||
					(scope_control->enforce_owner_phase &&
					 (!scope_control->owner_phase ||
					  scope_control->owner_phase->load(std::memory_order_acquire) !=
						  sqlite_shm_reader_lifecycle_owner_phase::owned)))
					return reject(sqlite_shm_lease_rejection_reason::invalid_identity);
				if (!claim_session_terminal_role(scope_control->issued_session_terminal_roles,
												 role))
					return reject(sqlite_shm_lease_rejection_reason::stale_token);

				const auto sequence = allocate_sequence();
				if (!sequence)
				{
					abandon_scope_owner(scope_control);
					return reject(sqlite_shm_lease_rejection_reason::generation_exhausted);
				}
				std::shared_ptr<sqlite_shm_process_identity_record_control> control;
				bool scope_counted{};
				try
				{
					control =
						make_fork_safe_identity_control<sqlite_shm_process_identity_record_control>(
							process_epoch_, expected_process_epoch_);
					control->issuer = weak_from_this();
					control->process_epoch = process_epoch_;
					control->expected_process_epoch = expected_process_epoch_;
					control->scope = scope_control;
					control->sequence = *sequence;
					control->domain = sqlite_shm_reader_lifecycle_identity_domain::session_terminal;
					control->role = static_cast<std::uint8_t>(role);
					control->projection = make_projection(
						*sequence,
						sqlite_shm_reader_lifecycle_identity_domain::session_terminal,
						control->role,
						*scope_control);
					if (!increment_accounting(scope_control->live_records, scope_control))
					{
						control->phase.store(sqlite_shm_process_identity_record_phase::abandoned,
											 std::memory_order_release);
						abandon_scope_owner(scope_control);
						return reject(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									  sqlite_shm_lease_recovery_action::quarantine_no_retry);
					}
					scope_counted = true;
					if (!scope_matches_control(scope_control) ||
						(scope_control->enforce_owner_phase &&
						 scope_control->owner_phase->load(std::memory_order_acquire) !=
							 sqlite_shm_reader_lifecycle_owner_phase::owned))
					{
						abandon_record(control);
						abandon_scope_owner(scope_control);
						return reject(sqlite_shm_lease_rejection_reason::stale_token);
					}
					control->phase.store(sqlite_shm_process_identity_record_phase::sealed,
										 std::memory_order_release);
					auto projection = control->projection;
					return sqlite_shm_issued_reader_session_terminal_identity{
						std::move(control), std::move(projection)};
				}
				catch (...)
				{
					if (control && scope_counted)
						abandon_record(control);
					else if (control)
						control->phase.store(sqlite_shm_process_identity_record_phase::abandoned,
											 std::memory_order_release);
					abandon_scope_owner(scope_control);
					return reject(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								  sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<void>
			validate_callback(const sqlite_shm_reader_lifecycle_identity_scope& scope,
							  const sqlite_shm_issued_reader_callback_identity& callback,
							  const sqlite_shm_reader_callback_identity_role role) const noexcept
			{
				if (!current_before_owner_lock())
					return reject(sqlite_shm_lease_rejection_reason::stale_token);
				return callback_matches(scope, callback) &&
						callback.control_->role == static_cast<std::uint8_t>(role) &&
						callback.receipt_.thread_identity == callback.control_->thread_identity &&
						callback.receipt_.reentrancy_depth == callback.control_->reentrancy_depth &&
						callback.receipt_.invocation_token == callback.control_->projection
					? sqlite_shm_lease_result<void>{}
					: sqlite_shm_lease_result<void>{
						  reject(sqlite_shm_lease_rejection_reason::receipt_mismatch)};
			}

			[[nodiscard]] bool qualified_scope_matches(
				const sqlite_shm_reader_lifecycle_identity_scope& scope,
				const sqlite_shm_lease_family_binding& family,
				const std::uint64_t family_epoch,
				const std::uint64_t family_pin_token,
				const sqlite_backend_opaque_identity& callback_cohort,
				const sqlite_backend_opaque_identity& request_seal,
				const sqlite_shm_reader_lifecycle_owner_coordinates& coordinates,
				const std::shared_ptr<std::atomic<sqlite_shm_reader_lifecycle_owner_phase>>&
					owner_phase,
				const std::weak_ptr<sqlite_shm_reader_lifecycle_owner_abandonment_control>&
					owner_abandonment) const noexcept
			{
				const auto control = scope.control_;
				const auto expected_abandonment = owner_abandonment.lock();
				const auto actual_abandonment =
					control ? control->owner_abandonment.lock() : nullptr;
				return scope_matches_control(control) && control->enforce_owner_phase &&
					control->family == family && control->family_epoch == family_epoch &&
					control->family_pin_token == family_pin_token &&
					control->callback_cohort == callback_cohort &&
					control->request_seal == request_seal && control->coordinates == coordinates &&
					control->owner_phase.get() == owner_phase.get() && expected_abandonment &&
					actual_abandonment.get() == expected_abandonment.get();
			}

			void complete_qualified_controls(
				const std::shared_ptr<sqlite_shm_reader_lifecycle_identity_scope_control>& scope,
				const std::shared_ptr<sqlite_shm_process_identity_record_control>&
					callback) noexcept
			{
				if (!scope || !callback || !scope->enforce_owner_phase ||
					callback->scope.get() != scope.get() || callback->issuer.lock().get() != this ||
					!scope->process_epoch ||
					scope->process_epoch->load(std::memory_order_acquire) !=
						scope->expected_process_epoch)
					return;
				scope->qualified_completion.store(true, std::memory_order_release);
				scope->active.store(false, std::memory_order_release);
				if (callback->live_children.load(std::memory_order_acquire) == 0U &&
					retire_record_phase(callback))
					(void)decrement_accounting(scope->live_records, scope);
			}

			[[nodiscard]] std::shared_ptr<sqlite_shm_reader_identity_completion_control>
			make_qualified_completion(const sqlite_shm_reader_lifecycle_identity_scope& scope,
									  const sqlite_shm_issued_reader_callback_identity& callback)
			{
				if (!validate_callback(
						scope, callback, sqlite_shm_reader_callback_identity_role::map))
					return {};
				struct completion final : sqlite_shm_reader_identity_completion_control
				{
					completion(
						std::weak_ptr<sqlite_shm_process_identity_issuer_state> issuer_value,
						std::shared_ptr<sqlite_shm_reader_lifecycle_identity_scope_control>
							scope_value,
						std::shared_ptr<sqlite_shm_process_identity_record_control> callback_value)
						: issuer{std::move(issuer_value)}, scope{std::move(scope_value)},
						  callback{std::move(callback_value)}
					{
					}
					std::weak_ptr<sqlite_shm_process_identity_issuer_state> issuer;
					std::shared_ptr<sqlite_shm_reader_lifecycle_identity_scope_control> scope;
					std::shared_ptr<sqlite_shm_process_identity_record_control> callback;
					void complete() noexcept override
					{
						if (auto state = issuer.lock())
							state->complete_qualified_controls(scope, callback);
					}
				};
				return std::make_shared<completion>(
					weak_from_this(), scope.control_, callback.control_);
			}

			[[nodiscard]] sqlite_shm_lease_result<void>
			validate_effect(const sqlite_shm_reader_lifecycle_identity_scope& scope,
							const sqlite_shm_issued_reader_callback_identity& callback,
							const sqlite_shm_issued_reader_effect_identity& effect,
							const sqlite_shm_reader_effect_identity_role role) const noexcept
			{
				if (!current_before_owner_lock())
					return reject(sqlite_shm_lease_rejection_reason::stale_token);
				return callback_matches(scope, callback) &&
						record_matches(
							effect.control_,
							scope,
							sqlite_shm_reader_lifecycle_identity_domain::native_or_zero_effect,
							static_cast<std::uint8_t>(role),
							sqlite_shm_process_identity_record_phase::sealed) &&
						effect.control_->parent_callback.lock().get() == callback.control_.get() &&
						effect.identity_ == effect.control_->projection
					? sqlite_shm_lease_result<void>{}
					: sqlite_shm_lease_result<void>{
						  reject(sqlite_shm_lease_rejection_reason::receipt_mismatch)};
			}

			[[nodiscard]] sqlite_shm_lease_result<
				sqlite_shm_reader_zero_effect_identity_validation_capability>
			// Each error() call is guarded by !result on a freshly returned two-alternative
			// result, so bad_variant_access is unreachable without changing result semantics.
			// NOLINTNEXTLINE(bugprone-exception-escape)
			validate_zero_effect_identity_for_registry(
				const sqlite_shm_reader_lifecycle_identity_scope& scope,
				const sqlite_shm_issued_reader_callback_identity& callback,
				const sqlite_shm_issued_reader_effect_identity& effect,
				const std::weak_ptr<sqlite_shm_reader_lifecycle_owner_abandonment_control>&
					expected_owner) const noexcept
			{
				if (!current_before_owner_lock())
					return reject(sqlite_shm_lease_rejection_reason::stale_token);
				const auto expected = expected_owner.lock();
				const auto actual =
					scope.control_ ? scope.control_->owner_abandonment.lock() : nullptr;
				if (!expected || !actual || actual.get() != expected.get())
					return reject(sqlite_shm_lease_rejection_reason::receipt_mismatch);
				const auto callback_validated = validate_callback(
					scope, callback, sqlite_shm_reader_callback_identity_role::map);
				if (!callback_validated)
					return callback_validated.error();
				const auto effect_validated =
					validate_effect(scope,
									callback,
									effect,
									sqlite_shm_reader_effect_identity_role::zero_attachment_result);
				if (!effect_validated)
					return effect_validated.error();
				return sqlite_shm_reader_zero_effect_identity_validation_capability{effect.control_,
																					actual};
			}

			[[nodiscard]] sqlite_shm_lease_result<
				sqlite_shm_reader_mapped_effect_identity_validation_capability>
			// Each error() call is guarded by !result on a freshly returned two-alternative
			// result, so bad_variant_access is unreachable without changing result semantics.
			// NOLINTNEXTLINE(bugprone-exception-escape)
			validate_mapped_effect_identity_for_registry(
				const sqlite_shm_reader_lifecycle_identity_scope& scope,
				const sqlite_shm_issued_reader_callback_identity& callback,
				const sqlite_shm_issued_reader_effect_identity& effect,
				const std::weak_ptr<sqlite_shm_reader_lifecycle_owner_abandonment_control>&
					expected_owner) const noexcept
			{
				if (!current_before_owner_lock())
					return reject(sqlite_shm_lease_rejection_reason::stale_token);
				const auto expected = expected_owner.lock();
				const auto actual =
					scope.control_ ? scope.control_->owner_abandonment.lock() : nullptr;
				if (!expected || !actual || actual.get() != expected.get())
					return reject(sqlite_shm_lease_rejection_reason::receipt_mismatch);
				const auto callback_validated = validate_callback(
					scope, callback, sqlite_shm_reader_callback_identity_role::map);
				if (!callback_validated)
					return callback_validated.error();
				const auto effect_validated = validate_effect(
					scope, callback, effect, sqlite_shm_reader_effect_identity_role::mapped_result);
				if (!effect_validated)
					return effect_validated.error();
				return sqlite_shm_reader_mapped_effect_identity_validation_capability{
					effect.control_, actual};
			}

			[[nodiscard]] bool zero_effect_capability_is_current(
				const std::shared_ptr<sqlite_shm_process_identity_record_control>& effect,
				const std::shared_ptr<sqlite_shm_reader_lifecycle_owner_abandonment_control>&
					expected_owner) const noexcept
			{
				if (!effect || !expected_owner || !scope_matches_control(effect->scope) ||
					effect->issuer.lock().get() != this ||
					effect->process_epoch.get() != process_epoch_.get() ||
					effect->expected_process_epoch != expected_process_epoch_ ||
					effect->domain !=
						sqlite_shm_reader_lifecycle_identity_domain::native_or_zero_effect ||
					effect->role !=
						static_cast<std::uint8_t>(
							sqlite_shm_reader_effect_identity_role::zero_attachment_result) ||
					effect->phase.load(std::memory_order_acquire) !=
						sqlite_shm_process_identity_record_phase::sealed ||
					!effect->scope->enforce_owner_phase || !effect->scope->owner_phase ||
					effect->scope->owner_phase->load(std::memory_order_acquire) !=
						sqlite_shm_reader_lifecycle_owner_phase::owned)
					return false;
				const auto actual_owner = effect->scope->owner_abandonment.lock();
				const auto parent = effect->parent_callback.lock();
				return actual_owner && actual_owner.get() == expected_owner.get() && parent &&
					parent->issuer.lock().get() == this &&
					parent->scope.get() == effect->scope.get() &&
					parent->domain ==
					sqlite_shm_reader_lifecycle_identity_domain::callback_invocation &&
					parent->role ==
					static_cast<std::uint8_t>(sqlite_shm_reader_callback_identity_role::map) &&
					parent->phase.load(std::memory_order_acquire) ==
					sqlite_shm_process_identity_record_phase::sealed;
			}

			[[nodiscard]] bool mapped_effect_capability_is_current(
				const std::shared_ptr<sqlite_shm_process_identity_record_control>& effect,
				const std::shared_ptr<sqlite_shm_reader_lifecycle_owner_abandonment_control>&
					expected_owner) const noexcept
			{
				if (!effect || !expected_owner || !scope_matches_control(effect->scope) ||
					effect->issuer.lock().get() != this ||
					effect->process_epoch.get() != process_epoch_.get() ||
					effect->expected_process_epoch != expected_process_epoch_ ||
					effect->domain !=
						sqlite_shm_reader_lifecycle_identity_domain::native_or_zero_effect ||
					effect->role !=
						static_cast<std::uint8_t>(
							sqlite_shm_reader_effect_identity_role::mapped_result) ||
					effect->phase.load(std::memory_order_acquire) !=
						sqlite_shm_process_identity_record_phase::sealed ||
					!effect->scope->enforce_owner_phase || !effect->scope->owner_phase ||
					effect->scope->owner_phase->load(std::memory_order_acquire) !=
						sqlite_shm_reader_lifecycle_owner_phase::owned)
					return false;
				const auto actual_owner = effect->scope->owner_abandonment.lock();
				const auto parent = effect->parent_callback.lock();
				return actual_owner && actual_owner.get() == expected_owner.get() && parent &&
					parent->issuer.lock().get() == this &&
					parent->scope.get() == effect->scope.get() &&
					parent->domain ==
					sqlite_shm_reader_lifecycle_identity_domain::callback_invocation &&
					parent->role ==
					static_cast<std::uint8_t>(sqlite_shm_reader_callback_identity_role::map) &&
					parent->phase.load(std::memory_order_acquire) ==
					sqlite_shm_process_identity_record_phase::sealed;
			}

			[[nodiscard]] sqlite_shm_lease_result<void> validate_session_terminal(
				const sqlite_shm_reader_lifecycle_identity_scope& scope,
				const sqlite_shm_issued_reader_session_terminal_identity& terminal,
				const sqlite_shm_reader_session_terminal_identity_role role) const noexcept
			{
				if (!current_before_owner_lock())
					return reject(sqlite_shm_lease_rejection_reason::stale_token);
				return valid_session_terminal_role(role) &&
						record_matches(
							terminal.control_,
							scope,
							sqlite_shm_reader_lifecycle_identity_domain::session_terminal,
							static_cast<std::uint8_t>(role),
							sqlite_shm_process_identity_record_phase::sealed) &&
						terminal.identity_ == terminal.control_->projection
					? sqlite_shm_lease_result<void>{}
					: sqlite_shm_lease_result<void>{
						  reject(sqlite_shm_lease_rejection_reason::receipt_mismatch)};
			}

			[[nodiscard]] sqlite_shm_lease_result<void>
			// error() is used only after !validated on a fresh two-alternative result.
			// NOLINTNEXTLINE(bugprone-exception-escape)
			retire_callback(const sqlite_shm_reader_lifecycle_identity_scope& scope,
							sqlite_shm_issued_reader_callback_identity& callback,
							const sqlite_shm_reader_callback_identity_role role) const noexcept
			{
				const auto control = callback.control_;
				if (!control)
					return reject(sqlite_shm_lease_rejection_reason::receipt_mismatch);
				const auto validated = validate_callback(scope, callback, role);
				if (!validated)
					return validated.error();
				if (control->scope->enforce_owner_phase)
				{
					abandon_scope_owner(control->scope);
					return reject(sqlite_shm_lease_rejection_reason::retiring);
				}
				auto expected = sqlite_shm_process_identity_record_phase::sealed;
				if (!control->phase.compare_exchange_strong(
						expected,
						sqlite_shm_process_identity_record_phase::retiring,
						std::memory_order_acq_rel,
						std::memory_order_acquire))
					return reject(sqlite_shm_lease_rejection_reason::stale_token);
				if (control->live_children.load(std::memory_order_acquire) != 0U)
				{
					expected = sqlite_shm_process_identity_record_phase::retiring;
					(void)control->phase.compare_exchange_strong(
						expected,
						sqlite_shm_process_identity_record_phase::sealed,
						std::memory_order_acq_rel,
						std::memory_order_acquire);
					return reject(sqlite_shm_lease_rejection_reason::retiring);
				}
				control->phase.store(sqlite_shm_process_identity_record_phase::retired,
									 std::memory_order_release);
				if (!decrement_accounting(control->scope->live_records, control->scope))
					return reject(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								  sqlite_shm_lease_recovery_action::quarantine_no_retry);
				return {};
			}

			[[nodiscard]] sqlite_shm_lease_result<void>
			// error() is used only after !validated on a fresh two-alternative result.
			// NOLINTNEXTLINE(bugprone-exception-escape)
			retire_effect(const sqlite_shm_reader_lifecycle_identity_scope& scope,
						  const sqlite_shm_issued_reader_callback_identity& callback,
						  sqlite_shm_issued_reader_effect_identity& effect,
						  const sqlite_shm_reader_effect_identity_role role) const noexcept
			{
				const auto validated = validate_effect(scope, callback, effect, role);
				if (!validated)
					return validated.error();
				const auto control = effect.control_;
				const auto parent = callback.control_;
				if (control->scope->enforce_owner_phase)
				{
					abandon_scope_owner(control->scope);
					return reject(sqlite_shm_lease_rejection_reason::retiring);
				}
				if (!retire_record_phase(control))
					return reject(sqlite_shm_lease_rejection_reason::stale_token);
				const auto scope_accounted =
					decrement_accounting(control->scope->live_records, control->scope);
				const auto parent_accounted =
					parent && decrement_accounting(parent->live_children, control->scope);
				if (!scope_accounted || !parent_accounted)
					return reject(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								  sqlite_shm_lease_recovery_action::quarantine_no_retry);
				return {};
			}

			// error() is used only after !validated on a fresh two-alternative result.
			// NOLINTNEXTLINE(bugprone-exception-escape)
			[[nodiscard]] sqlite_shm_lease_result<void> retire_session_terminal(
				const sqlite_shm_reader_lifecycle_identity_scope& scope,
				sqlite_shm_issued_reader_session_terminal_identity& terminal,
				const sqlite_shm_reader_session_terminal_identity_role role) const noexcept
			{
				const auto validated = validate_session_terminal(scope, terminal, role);
				if (!validated)
					return validated.error();
				const auto control = terminal.control_;
				if (control->scope->enforce_owner_phase)
				{
					abandon_scope_owner(control->scope);
					return reject(sqlite_shm_lease_rejection_reason::retiring);
				}
				if (!retire_record_phase(control))
					return reject(sqlite_shm_lease_rejection_reason::stale_token);
				if (!decrement_accounting(control->scope->live_records, control->scope))
					return reject(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								  sqlite_shm_lease_recovery_action::quarantine_no_retry);
				return {};
			}

			[[nodiscard]] sqlite_shm_lease_result<void>
			retire_scope(sqlite_shm_reader_lifecycle_identity_scope& scope) noexcept
			{
				if (!current_before_owner_lock())
					return reject(sqlite_shm_lease_rejection_reason::stale_token);
				if (!scope_matches(scope))
					return reject(sqlite_shm_lease_rejection_reason::receipt_mismatch);
				if (scope.control_->enforce_owner_phase)
				{
					abandon_scope_owner(scope.control_);
					return reject(sqlite_shm_lease_rejection_reason::retiring);
				}
				bool expected = true;
				if (!scope.control_->active.compare_exchange_strong(
						expected, false, std::memory_order_acq_rel, std::memory_order_acquire))
					return reject(sqlite_shm_lease_rejection_reason::stale_token);
				if (scope.control_->live_records.load(std::memory_order_acquire) != 0U)
				{
					scope.control_->active.store(true, std::memory_order_release);
					if (scope.control_->quarantined.load(std::memory_order_acquire))
					{
						scope.control_->active.store(false, std::memory_order_release);
						return reject(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									  sqlite_shm_lease_recovery_action::quarantine_no_retry);
					}
					return reject(sqlite_shm_lease_rejection_reason::retiring);
				}
				return {};
			}

#if defined(CXXLENS_SQLITE_TEST_SUPPORT)
			void exhaust_for_testing() noexcept
			{
				auto observed = next_sequence_.load(std::memory_order_acquire);
				while (
					observed != 0U &&
					!next_sequence_.compare_exchange_weak(observed,
														  std::numeric_limits<std::uint64_t>::max(),
														  std::memory_order_acq_rel,
														  std::memory_order_acquire))
				{
				}
			}

			void arm_pause_for_testing(
				const sqlite_shm_identity_issuer_pause_point_for_testing point) noexcept
			{
				test_pause_release_.store(false, std::memory_order_release);
				test_pause_entered_.store(false, std::memory_order_release);
				test_pause_point_.store(static_cast<std::uint8_t>(point),
										std::memory_order_release);
			}

			[[nodiscard]] bool pause_entered_for_testing(
				const sqlite_shm_identity_issuer_pause_point_for_testing point) const noexcept
			{
				return test_pause_point_.load(std::memory_order_acquire) ==
					static_cast<std::uint8_t>(point) &&
					test_pause_entered_.load(std::memory_order_acquire);
			}

			void release_pause_for_testing() noexcept
			{
				test_pause_point_.store(
					static_cast<std::uint8_t>(
						sqlite_shm_identity_issuer_pause_point_for_testing::none),
					std::memory_order_release);
				test_pause_release_.store(true, std::memory_order_release);
			}
#endif

			[[nodiscard]] const std::shared_ptr<std::atomic<std::uint64_t>>&
			process_epoch() const noexcept
			{
				return process_epoch_;
			}

			[[nodiscard]] std::uint64_t expected_process_epoch() const noexcept
			{
				return expected_process_epoch_;
			}

		  private:
#if defined(CXXLENS_SQLITE_TEST_SUPPORT)
			void pause_for_testing(
				const sqlite_shm_identity_issuer_pause_point_for_testing point) noexcept
			{
				if (test_pause_point_.load(std::memory_order_acquire) !=
					static_cast<std::uint8_t>(point))
					return;
				test_pause_entered_.store(true, std::memory_order_release);
				while (!test_pause_release_.load(std::memory_order_acquire))
					std::this_thread::yield();
			}
#endif

			[[nodiscard]] bool
			scope_matches(const sqlite_shm_reader_lifecycle_identity_scope& scope) const noexcept
			{
				return scope_matches_control(scope.control_);
			}

			[[nodiscard]] bool scope_matches_control(
				const std::shared_ptr<sqlite_shm_reader_lifecycle_identity_scope_control>& control)
				const noexcept
			{
				return current_before_owner_lock() && scope_control_current(control) &&
					control->family_epoch != 0U && control->family_pin_token != 0U &&
					control->expected_process_epoch == expected_process_epoch_ &&
					control->process_epoch.get() == process_epoch_.get() &&
					control->issuer.lock().get() == this &&
					control->family.process_instance == process_instance_;
			}

			[[nodiscard]] bool record_matches(
				const std::shared_ptr<sqlite_shm_process_identity_record_control>& record,
				const sqlite_shm_reader_lifecycle_identity_scope& scope,
				const sqlite_shm_reader_lifecycle_identity_domain domain,
				const std::uint8_t role,
				const sqlite_shm_process_identity_record_phase phase) const noexcept
			{
				return scope_matches(scope) && record &&
					record->scope.get() == scope.control_.get() &&
					record->issuer.lock().get() == this &&
					record->process_epoch.get() == process_epoch_.get() &&
					record->expected_process_epoch == expected_process_epoch_ &&
					record->sequence != 0U && record->domain == domain && record->role == role &&
					record->phase.load(std::memory_order_acquire) == phase;
			}

			[[nodiscard]] bool callback_matches(
				const sqlite_shm_reader_lifecycle_identity_scope& scope,
				const sqlite_shm_issued_reader_callback_identity& callback) const noexcept
			{
				if (!callback.control_ || !scope_matches(scope) ||
					callback.control_->scope.get() != scope.control_.get() ||
					callback.control_->issuer.lock().get() != this ||
					callback.control_->domain !=
						sqlite_shm_reader_lifecycle_identity_domain::callback_invocation ||
					callback.receipt_.invocation_token != callback.control_->projection)
					return false;
				const auto phase = callback.control_->phase.load(std::memory_order_acquire);
				return phase == sqlite_shm_process_identity_record_phase::sealed ||
					phase == sqlite_shm_process_identity_record_phase::issuing_effect;
			}

			[[nodiscard]] std::optional<std::uint64_t> allocate_sequence() noexcept
			{
				auto observed = next_sequence_.load(std::memory_order_acquire);
				while (observed != 0U)
				{
					const auto replacement =
						observed == std::numeric_limits<std::uint64_t>::max() ? 0U : observed + 1U;
					if (next_sequence_.compare_exchange_weak(observed,
															 replacement,
															 std::memory_order_acq_rel,
															 std::memory_order_acquire))
						return observed;
				}
				return std::nullopt;
			}

			static void append_opaque_identity(std::vector<std::byte>& bytes,
											   const sqlite_backend_opaque_identity& identity)
			{
				append_integer(bytes, static_cast<std::uint64_t>(identity.profile.size()));
				for (const auto value : identity.profile)
					bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
				append_integer(bytes, static_cast<std::uint64_t>(identity.bytes.size()));
				bytes.insert(bytes.end(), identity.bytes.begin(), identity.bytes.end());
			}

			[[nodiscard]] sqlite_backend_opaque_identity
			make_projection(const std::uint64_t sequence,
							const sqlite_shm_reader_lifecycle_identity_domain domain,
							const std::uint8_t role,
							const sqlite_shm_reader_lifecycle_identity_scope_control& scope) const
			{
				std::vector<std::byte> bytes;
				bytes.reserve(256U);
				append_opaque_identity(bytes, process_instance_);
				append_opaque_identity(bytes, scope.family.shared_runtime_vfs_cohort);
				append_opaque_identity(bytes, scope.family.exact_file_family);
				append_opaque_identity(bytes, scope.callback_cohort);
				append_opaque_identity(bytes, scope.request_seal);
				append_integer(bytes, incarnation_);
				append_integer(bytes, expected_process_epoch_);
				append_integer(bytes, scope.family_epoch);
				append_integer(bytes, scope.family_pin_token);
				append_integer(bytes, scope.coordinates.registry_open_token);
				bytes.push_back(static_cast<std::byte>(scope.coordinates.owner_kind));
				append_integer(bytes, scope.coordinates.lifecycle_owner_token);
				append_integer(bytes, scope.coordinates.writer_mapping_generation);
				append_integer(bytes, sequence);
				bytes.push_back(static_cast<std::byte>(domain));
				bytes.push_back(static_cast<std::byte>(role));
				return {"cxxlens.sqlite.reader-lifecycle.process-issued-identity.v1",
						std::move(bytes)};
			}

			[[nodiscard]] static bool retire_record_phase(
				const std::shared_ptr<sqlite_shm_process_identity_record_control>& record) noexcept
			{
				if (!record)
					return false;
				auto expected = sqlite_shm_process_identity_record_phase::sealed;
				return record->phase.compare_exchange_strong(
						   expected,
						   sqlite_shm_process_identity_record_phase::retired,
						   std::memory_order_acq_rel,
						   std::memory_order_acquire) &&
					record->scope;
			}

			static void abandon_record(
				const std::shared_ptr<sqlite_shm_process_identity_record_control>& record) noexcept
			{
				if (!record)
					return;
				// A qualified at-fork/process-port hook invalidates this lock-free epoch before
				// any child callback. The stale child must not lock an inherited weak owner or
				// touch parent accounting; its private address-space copy may be dropped as-is.
				if (!record->process_epoch ||
					record->process_epoch->load(std::memory_order_acquire) !=
						record->expected_process_epoch)
					return;
				if (!record->scope || !record->scope->registry_issuer_owner_latch ||
					!record->scope->registry_issuer_owner_latch->load(std::memory_order_acquire))
					return;
				auto phase = record->phase.load(std::memory_order_acquire);
				while (phase == sqlite_shm_process_identity_record_phase::reserved ||
					   phase == sqlite_shm_process_identity_record_phase::sealed)
				{
					const auto owner =
						record->scope ? record->scope->owner_abandonment.lock() : nullptr;
					// The shared owner disposition is the linearization point.  A proof
					// destructor must contest it before classifying its private record;
					// otherwise terminal completion could win between an early read and
					// the record CAS, incorrectly recording an abandonment.
					if (record->scope && record->scope->enforce_owner_phase && owner)
						owner->abandon();
					const auto completed = record->scope &&
						(record->scope->qualified_completion.load(std::memory_order_acquire) ||
						 (owner && owner->terminal_completion_claimed()));
					if (record->phase.compare_exchange_weak(
							phase,
							completed ? sqlite_shm_process_identity_record_phase::retired
									  : sqlite_shm_process_identity_record_phase::abandoned,
							std::memory_order_acq_rel,
							std::memory_order_acquire))
					{
						if (record->scope)
							(void)decrement_accounting(record->scope->live_records, record->scope);
						if (auto parent = record->parent_callback.lock())
							(void)decrement_accounting(parent->live_children, record->scope);
						if (!completed)
							abandon_scope_owner(record->scope);
						return;
					}
				}
			}

			friend class ::cxxlens::sdk::sqlite_shm_reader_callback_identity_permit;
			friend class ::cxxlens::sdk::sqlite_shm_issued_reader_callback_identity;
			friend class ::cxxlens::sdk::sqlite_shm_issued_reader_effect_identity;
			friend class ::cxxlens::sdk::sqlite_shm_issued_reader_session_terminal_identity;
			friend class ::cxxlens::sdk::
				sqlite_shm_reader_zero_effect_identity_validation_capability;
			friend class ::cxxlens::sdk::
				sqlite_shm_reader_mapped_effect_identity_validation_capability;
			friend void quarantine_stale_identity_issuer_state(
				sqlite_shm_process_identity_issuer_state*) noexcept;

			std::weak_ptr<void> registry_state_;
			std::shared_ptr<std::atomic<std::uint64_t>> process_epoch_;
			std::shared_ptr<std::atomic_bool> registry_quarantine_latch_;
			std::shared_ptr<std::atomic_bool> registry_issuer_owner_latch_;
			std::uint64_t expected_process_epoch_{};
			sqlite_backend_opaque_identity process_instance_;
			std::uint64_t incarnation_{};
			std::atomic<std::uint64_t> next_sequence_{1U};
#if defined(CXXLENS_SQLITE_TEST_SUPPORT)
			std::atomic<std::uint8_t> test_pause_point_{static_cast<std::uint8_t>(
				sqlite_shm_identity_issuer_pause_point_for_testing::none)};
			std::atomic_bool test_pause_entered_{false};
			std::atomic_bool test_pause_release_{true};
#endif
			// Non-owning link for the process-lifetime stale-epoch root. The custom deleter
			// never destroys an issuer state after fork/epoch loss.
			sqlite_shm_process_identity_issuer_state* quarantine_next_{};
		};

		struct sqlite_shm_identity_issuer_quarantine_sink
		{
			static_assert(
				std::atomic<sqlite_shm_process_identity_issuer_state*>::is_always_lock_free);
			std::atomic<sqlite_shm_process_identity_issuer_state*> head{nullptr};
		};
		sqlite_shm_identity_issuer_quarantine_sink identity_issuer_quarantine_sink_storage_instance;

		void quarantine_stale_identity_issuer_state(
			sqlite_shm_process_identity_issuer_state* state) noexcept
		{
			auto& sink = identity_issuer_quarantine_sink_storage_instance;
			auto* previous = sink.head.load(std::memory_order_acquire);
			do
			{
				state->quarantine_next_ = previous;
			} while (!sink.head.compare_exchange_weak(
				previous, state, std::memory_order_release, std::memory_order_acquire));
		}

		std::shared_ptr<sqlite_shm_process_identity_issuer_state>
		make_identity_issuer_state_for_registry(
			sqlite_shm_identity_issuer_registry_bindings bindings,
			const sqlite_backend_opaque_identity& process_instance,
			const std::uint64_t first_sequence)
		{
			const auto incarnation = allocate_issuer_incarnation();
			if (!incarnation)
				return {};
			const auto stale_child_epoch = bindings.process_epoch;
			return std::shared_ptr<sqlite_shm_process_identity_issuer_state>{
				new sqlite_shm_process_identity_issuer_state{
					std::move(bindings.registry_state),
					std::move(bindings.process_epoch),
					std::move(bindings.registry_quarantine_latch),
					std::move(bindings.registry_issuer_owner_latch),
					bindings.expected_process_epoch,
					process_instance,
					sqlite_shm_process_identity_issuer_state::counter_seed{*incarnation,
																		   first_sequence}},
				[stale_child_epoch, expected_process_epoch = bindings.expected_process_epoch](
					sqlite_shm_process_identity_issuer_state* state) noexcept
				{
					// Match the registry state's qualified-fork destruction discipline: once the
					// process-port hook invalidates the epoch, a child leaks its inherited state
					// instead of running heap/member destruction from a multi-threaded parent.
					if (stale_child_epoch &&
						stale_child_epoch->load(std::memory_order_acquire) ==
							expected_process_epoch)
						delete state;
					else
						quarantine_stale_identity_issuer_state(state);
				}};
		}

		sqlite_shm_reader_lifecycle_identity_scope seal_identity_scope_for_registry(
			const std::shared_ptr<sqlite_shm_process_identity_issuer_state>& state,
			const sqlite_shm_lease_family_binding& family,
			std::shared_ptr<std::atomic_bool> family_authority_latch,
			const std::uint64_t family_epoch,
			const std::uint64_t family_pin_token,
			const sqlite_backend_opaque_identity& callback_cohort,
			const sqlite_backend_opaque_identity& request_seal,
			const sqlite_shm_reader_lifecycle_owner_coordinates& coordinates)
		{
			return state->seal_scope(family,
									 std::move(family_authority_latch),
									 family_epoch,
									 family_pin_token,
									 callback_cohort,
									 request_seal,
									 coordinates);
		}

		sqlite_shm_reader_lifecycle_identity_scope seal_qualified_identity_scope_for_registry(
			const std::shared_ptr<sqlite_shm_process_identity_issuer_state>& state,
			const sqlite_shm_lease_family_binding& family,
			std::shared_ptr<std::atomic_bool> family_authority_latch,
			const std::uint64_t family_epoch,
			const std::uint64_t family_pin_token,
			const sqlite_backend_opaque_identity& callback_cohort,
			const sqlite_backend_opaque_identity& request_seal,
			const sqlite_shm_reader_lifecycle_owner_coordinates& coordinates,
			std::shared_ptr<std::atomic<sqlite_shm_reader_lifecycle_owner_phase>> owner_phase,
			std::weak_ptr<sqlite_shm_reader_lifecycle_owner_abandonment_control> owner_abandonment)
		{
			return state->seal_scope(family,
									 std::move(family_authority_latch),
									 family_epoch,
									 family_pin_token,
									 callback_cohort,
									 request_seal,
									 coordinates,
									 true,
									 std::move(owner_phase),
									 std::move(owner_abandonment));
		}

		bool qualified_identity_scope_matches_for_registry(
			const std::shared_ptr<sqlite_shm_process_identity_issuer_state>& state,
			const sqlite_shm_reader_lifecycle_identity_scope& scope,
			const sqlite_shm_lease_family_binding& family,
			const std::uint64_t family_epoch,
			const std::uint64_t family_pin_token,
			const sqlite_backend_opaque_identity& callback_cohort,
			const sqlite_backend_opaque_identity& request_seal,
			const sqlite_shm_reader_lifecycle_owner_coordinates& coordinates,
			const std::shared_ptr<std::atomic<sqlite_shm_reader_lifecycle_owner_phase>>&
				owner_phase,
			const std::weak_ptr<sqlite_shm_reader_lifecycle_owner_abandonment_control>&
				owner_abandonment) noexcept
		{
			return state &&
				state->qualified_scope_matches(scope,
											   family,
											   family_epoch,
											   family_pin_token,
											   callback_cohort,
											   request_seal,
											   coordinates,
											   owner_phase,
											   owner_abandonment);
		}

		sqlite_shm_lease_result<void> validate_callback_identity_for_registry(
			const std::shared_ptr<sqlite_shm_process_identity_issuer_state>& state,
			const sqlite_shm_reader_lifecycle_identity_scope& scope,
			const sqlite_shm_issued_reader_callback_identity& callback,
			const sqlite_shm_reader_callback_identity_role role) noexcept
		{
			return state ? state->validate_callback(scope, callback, role)
						 : sqlite_shm_lease_result<void>{
							   reject(sqlite_shm_lease_rejection_reason::stale_token)};
		}

		sqlite_shm_lease_result<sqlite_shm_reader_zero_effect_identity_validation_capability>
		validate_zero_effect_identity_for_registry(
			const std::shared_ptr<sqlite_shm_process_identity_issuer_state>& state,
			const sqlite_shm_reader_lifecycle_identity_scope& scope,
			const sqlite_shm_issued_reader_callback_identity& callback,
			const sqlite_shm_issued_reader_effect_identity& effect,
			const std::weak_ptr<sqlite_shm_reader_lifecycle_owner_abandonment_control>&
				expected_owner) noexcept
		{
			return state ? state->validate_zero_effect_identity_for_registry(
							   scope, callback, effect, expected_owner)
						 : sqlite_shm_lease_result<
							   sqlite_shm_reader_zero_effect_identity_validation_capability>{
							   reject(sqlite_shm_lease_rejection_reason::stale_token)};
		}

		sqlite_shm_lease_result<sqlite_shm_reader_mapped_effect_identity_validation_capability>
		validate_mapped_effect_identity_for_registry(
			const std::shared_ptr<sqlite_shm_process_identity_issuer_state>& state,
			const sqlite_shm_reader_lifecycle_identity_scope& scope,
			const sqlite_shm_issued_reader_callback_identity& callback,
			const sqlite_shm_issued_reader_effect_identity& effect,
			const std::weak_ptr<sqlite_shm_reader_lifecycle_owner_abandonment_control>&
				expected_owner) noexcept
		{
			return state ? state->validate_mapped_effect_identity_for_registry(
							   scope, callback, effect, expected_owner)
						 : sqlite_shm_lease_result<
							   sqlite_shm_reader_mapped_effect_identity_validation_capability>{
							   reject(sqlite_shm_lease_rejection_reason::stale_token)};
		}

		std::shared_ptr<sqlite_shm_reader_identity_completion_control>
		make_identity_completion_for_registry(
			const std::shared_ptr<sqlite_shm_process_identity_issuer_state>& state,
			const sqlite_shm_reader_lifecycle_identity_scope& scope,
			const sqlite_shm_issued_reader_callback_identity& callback)
		{
			return state ? state->make_qualified_completion(scope, callback) : nullptr;
		}

#if defined(CXXLENS_SQLITE_TEST_SUPPORT)
		void exhaust_identity_issuer_for_registry(
			const std::shared_ptr<sqlite_shm_process_identity_issuer_state>& state) noexcept
		{
			if (state)
				state->exhaust_for_testing();
		}
#endif
	} // namespace detail

	bool sqlite_shm_reader_lifecycle_identity_scope::valid() const noexcept
	{
		return detail::scope_control_current(control_);
	}

	sqlite_shm_reader_lifecycle_identity_scope::
		~sqlite_shm_reader_lifecycle_identity_scope() noexcept
	{
		if (control_)
		{
			detail::abandon_scope_owner(control_);
			control_->active.store(false, std::memory_order_release);
		}
	}

	sqlite_shm_reader_lifecycle_identity_scope::sqlite_shm_reader_lifecycle_identity_scope(
		std::shared_ptr<detail::sqlite_shm_reader_lifecycle_identity_scope_control>
			control) noexcept
		: control_{std::move(control)}
	{
	}

	sqlite_shm_reader_callback_identity_permit::
		~sqlite_shm_reader_callback_identity_permit() noexcept
	{
		detail::sqlite_shm_process_identity_issuer_state::abandon_record(control_);
	}

	sqlite_shm_reader_callback_identity_permit::sqlite_shm_reader_callback_identity_permit(
		sqlite_shm_reader_callback_identity_permit&& other) noexcept
		: control_{std::move(other.control_)}
	{
	}

	sqlite_shm_reader_callback_identity_permit::sqlite_shm_reader_callback_identity_permit(
		std::shared_ptr<detail::sqlite_shm_process_identity_record_control> control) noexcept
		: control_{std::move(control)}
	{
	}

	bool sqlite_shm_reader_callback_identity_permit::valid() const noexcept
	{
		return control_ && detail::scope_control_current(control_->scope) &&
			!control_->issuer.expired() &&
			control_->phase.load(std::memory_order_acquire) ==
			detail::sqlite_shm_process_identity_record_phase::reserved;
	}

	sqlite_shm_issued_reader_callback_identity::
		~sqlite_shm_issued_reader_callback_identity() noexcept
	{
		detail::sqlite_shm_process_identity_issuer_state::abandon_record(control_);
	}

	sqlite_shm_issued_reader_callback_identity::sqlite_shm_issued_reader_callback_identity(
		sqlite_shm_issued_reader_callback_identity&& other) noexcept
		: control_{std::move(other.control_)}, receipt_{std::move(other.receipt_)}
	{
	}

	sqlite_shm_issued_reader_callback_identity::sqlite_shm_issued_reader_callback_identity(
		std::shared_ptr<detail::sqlite_shm_process_identity_record_control> control,
		sqlite_shm_callback_execution_receipt receipt) noexcept
		: control_{std::move(control)}, receipt_{std::move(receipt)}
	{
	}

	bool sqlite_shm_issued_reader_callback_identity::valid() const noexcept
	{
		if (!control_ || !detail::scope_control_current(control_->scope) ||
			control_->issuer.expired())
			return false;
		const auto phase = control_->phase.load(std::memory_order_acquire);
		return phase == detail::sqlite_shm_process_identity_record_phase::sealed ||
			phase == detail::sqlite_shm_process_identity_record_phase::issuing_effect;
	}

	const sqlite_shm_callback_execution_receipt&
	sqlite_shm_issued_reader_callback_identity::receipt() const noexcept
	{
		return receipt_;
	}

	sqlite_shm_issued_reader_effect_identity::~sqlite_shm_issued_reader_effect_identity() noexcept
	{
		detail::sqlite_shm_process_identity_issuer_state::abandon_record(control_);
	}

	sqlite_shm_issued_reader_effect_identity::sqlite_shm_issued_reader_effect_identity(
		sqlite_shm_issued_reader_effect_identity&& other) noexcept
		: control_{std::move(other.control_)}, identity_{std::move(other.identity_)}
	{
	}

	sqlite_shm_issued_reader_effect_identity::sqlite_shm_issued_reader_effect_identity(
		std::shared_ptr<detail::sqlite_shm_process_identity_record_control> control,
		sqlite_backend_opaque_identity identity) noexcept
		: control_{std::move(control)}, identity_{std::move(identity)}
	{
	}

	bool sqlite_shm_issued_reader_effect_identity::valid() const noexcept
	{
		return control_ && detail::scope_control_current(control_->scope) &&
			!control_->issuer.expired() && detail::record_parent_current(control_) &&
			control_->phase.load(std::memory_order_acquire) ==
			detail::sqlite_shm_process_identity_record_phase::sealed;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_issued_reader_effect_identity::identity() const noexcept
	{
		return identity_;
	}

	sqlite_shm_reader_zero_effect_identity_validation_capability::
		sqlite_shm_reader_zero_effect_identity_validation_capability(
			std::shared_ptr<detail::sqlite_shm_process_identity_record_control> effect,
			std::weak_ptr<detail::sqlite_shm_reader_lifecycle_owner_abandonment_control>
				owner) noexcept
		: effect_{std::move(effect)}, owner_{std::move(owner)}
	{
	}

	bool sqlite_shm_reader_zero_effect_identity_validation_capability::matches_live_owner(
		const std::shared_ptr<detail::sqlite_shm_reader_lifecycle_owner_abandonment_control>& owner)
		const noexcept
	{
		if (!effect_ || !effect_->process_epoch ||
			effect_->process_epoch->load(std::memory_order_acquire) !=
				effect_->expected_process_epoch)
			return false;
		const auto expected = owner_.lock();
		const auto issuer = effect_->issuer.lock();
		return expected && owner && expected.get() == owner.get() && issuer &&
			issuer->zero_effect_capability_is_current(effect_, owner);
	}

	bool sqlite_shm_reader_zero_effect_identity_validation_capability::matches_effect_identity(
		const sqlite_backend_opaque_identity& identity) const noexcept
	{
		return effect_ && effect_->projection == identity;
	}

	sqlite_backend_opaque_identity
	sqlite_shm_reader_zero_effect_identity_validation_capability::copy_effect_identity() const
	{
		return effect_ ? effect_->projection : sqlite_backend_opaque_identity{};
	}

	sqlite_shm_reader_mapped_effect_identity_validation_capability::
		sqlite_shm_reader_mapped_effect_identity_validation_capability(
			std::shared_ptr<detail::sqlite_shm_process_identity_record_control> effect,
			std::weak_ptr<detail::sqlite_shm_reader_lifecycle_owner_abandonment_control>
				owner) noexcept
		: effect_{std::move(effect)}, owner_{std::move(owner)}
	{
	}

	bool sqlite_shm_reader_mapped_effect_identity_validation_capability::matches_live_owner(
		const std::shared_ptr<detail::sqlite_shm_reader_lifecycle_owner_abandonment_control>& owner)
		const noexcept
	{
		if (!effect_ || !effect_->process_epoch ||
			effect_->process_epoch->load(std::memory_order_acquire) !=
				effect_->expected_process_epoch)
			return false;
		const auto expected = owner_.lock();
		const auto issuer = effect_->issuer.lock();
		return expected && owner && expected.get() == owner.get() && issuer &&
			issuer->mapped_effect_capability_is_current(effect_, owner);
	}

	bool sqlite_shm_reader_mapped_effect_identity_validation_capability::matches_effect_identity(
		const sqlite_backend_opaque_identity& identity) const noexcept
	{
		return effect_ && effect_->projection == identity;
	}

	sqlite_backend_opaque_identity
	sqlite_shm_reader_mapped_effect_identity_validation_capability::copy_effect_identity() const
	{
		return effect_ ? effect_->projection : sqlite_backend_opaque_identity{};
	}

	sqlite_shm_issued_reader_session_terminal_identity::
		~sqlite_shm_issued_reader_session_terminal_identity() noexcept
	{
		detail::sqlite_shm_process_identity_issuer_state::abandon_record(control_);
	}

	sqlite_shm_issued_reader_session_terminal_identity::
		sqlite_shm_issued_reader_session_terminal_identity(
			sqlite_shm_issued_reader_session_terminal_identity&& other) noexcept
		: control_{std::move(other.control_)}, identity_{std::move(other.identity_)}
	{
	}

	sqlite_shm_issued_reader_session_terminal_identity::
		sqlite_shm_issued_reader_session_terminal_identity(
			std::shared_ptr<detail::sqlite_shm_process_identity_record_control> control,
			sqlite_backend_opaque_identity identity) noexcept
		: control_{std::move(control)}, identity_{std::move(identity)}
	{
	}

	bool sqlite_shm_issued_reader_session_terminal_identity::valid() const noexcept
	{
		return control_ && detail::scope_control_current(control_->scope) &&
			!control_->issuer.expired() &&
			control_->phase.load(std::memory_order_acquire) ==
			detail::sqlite_shm_process_identity_record_phase::sealed;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_issued_reader_session_terminal_identity::identity() const noexcept
	{
		return identity_;
	}

	sqlite_shm_process_global_identity_issuer::sqlite_shm_process_global_identity_issuer(
		std::weak_ptr<detail::sqlite_shm_process_identity_issuer_state> state,
		std::shared_ptr<std::atomic<std::uint64_t>> process_epoch,
		std::shared_ptr<std::atomic_bool> registry_issuer_owner_latch,
		const std::uint64_t expected_process_epoch) noexcept
		: state_{std::move(state)}, process_epoch_{std::move(process_epoch)},
		  registry_issuer_owner_latch_{std::move(registry_issuer_owner_latch)},
		  expected_process_epoch_{expected_process_epoch}
	{
	}

	bool sqlite_shm_process_global_identity_issuer::current_before_state_lock() const noexcept
	{
		return process_epoch_ && expected_process_epoch_ != 0U &&
			process_epoch_->load(std::memory_order_acquire) == expected_process_epoch_ &&
			registry_issuer_owner_latch_ &&
			registry_issuer_owner_latch_->load(std::memory_order_acquire);
	}

#if defined(CXXLENS_SQLITE_TEST_SUPPORT)
	void sqlite_shm_process_global_identity_issuer::set_scope_live_records_for_testing(
		const sqlite_shm_reader_lifecycle_identity_scope& scope, const std::size_t value) noexcept
	{
		if (scope.control_)
			scope.control_->live_records.store(value, std::memory_order_release);
	}

	std::size_t sqlite_shm_process_global_identity_issuer::scope_live_records_for_testing(
		const sqlite_shm_reader_lifecycle_identity_scope& scope) const noexcept
	{
		return scope.control_ ? scope.control_->live_records.load(std::memory_order_acquire) : 0U;
	}

	void sqlite_shm_process_global_identity_issuer::set_callback_live_children_for_testing(
		const sqlite_shm_issued_reader_callback_identity& callback,
		const std::size_t value) noexcept
	{
		if (callback.control_)
			callback.control_->live_children.store(value, std::memory_order_release);
	}

	std::size_t sqlite_shm_process_global_identity_issuer::callback_live_children_for_testing(
		const sqlite_shm_issued_reader_callback_identity& callback) const noexcept
	{
		return callback.control_ ? callback.control_->live_children.load(std::memory_order_acquire)
								 : 0U;
	}

	void sqlite_shm_process_global_identity_issuer::arm_pause_for_testing(
		const sqlite_shm_identity_issuer_pause_point_for_testing point) noexcept
	{
		if (!current_before_state_lock())
			return;
		if (const auto state = state_.lock())
			state->arm_pause_for_testing(point);
	}

	bool sqlite_shm_process_global_identity_issuer::pause_entered_for_testing(
		const sqlite_shm_identity_issuer_pause_point_for_testing point) const noexcept
	{
		if (!current_before_state_lock())
			return false;
		const auto state = state_.lock();
		return state && state->pause_entered_for_testing(point);
	}

	void sqlite_shm_process_global_identity_issuer::release_pause_for_testing() noexcept
	{
		if (!process_epoch_ ||
			process_epoch_->load(std::memory_order_acquire) != expected_process_epoch_)
			return;
		if (const auto state = state_.lock())
			state->release_pause_for_testing();
	}
#endif

	bool sqlite_shm_process_global_identity_issuer::valid() const noexcept
	{
		if (!current_before_state_lock())
			return false;
		const auto state = state_.lock();
		return state && state->current();
	}

	sqlite_shm_lease_result<sqlite_shm_reader_callback_identity_permit>
	sqlite_shm_process_global_identity_issuer::reserve_callback(
		const sqlite_shm_reader_lifecycle_identity_scope& scope,
		const sqlite_shm_reader_callback_identity_role role,
		sqlite_backend_opaque_identity thread_identity,
		const std::uint64_t reentrancy_depth)
	{
		if (!current_before_state_lock())
			return reject(sqlite_shm_lease_rejection_reason::stale_token);
		const auto state = state_.lock();
		return state
			? state->reserve_callback(scope, role, std::move(thread_identity), reentrancy_depth)
			: sqlite_shm_lease_result<sqlite_shm_reader_callback_identity_permit>{
				  reject(sqlite_shm_lease_rejection_reason::stale_token)};
	}

	sqlite_shm_lease_result<sqlite_shm_issued_reader_callback_identity>
	sqlite_shm_process_global_identity_issuer::seal_callback(
		sqlite_shm_reader_callback_identity_permit& permit,
		const sqlite_shm_reader_lifecycle_identity_scope& scope,
		const sqlite_shm_reader_callback_identity_role role) noexcept
	{
		if (!current_before_state_lock())
			return reject(sqlite_shm_lease_rejection_reason::stale_token);
		const auto state = state_.lock();
		return state ? state->seal_callback(permit, scope, role)
					 : sqlite_shm_lease_result<sqlite_shm_issued_reader_callback_identity>{
						   reject(sqlite_shm_lease_rejection_reason::stale_token)};
	}

	sqlite_shm_lease_result<sqlite_shm_issued_reader_effect_identity>
	sqlite_shm_process_global_identity_issuer::issue_effect(
		const sqlite_shm_reader_lifecycle_identity_scope& scope,
		const sqlite_shm_issued_reader_callback_identity& callback,
		const sqlite_shm_reader_effect_identity_role role)
	{
		if (!current_before_state_lock())
			return reject(sqlite_shm_lease_rejection_reason::stale_token);
		const auto state = state_.lock();
		return state ? state->issue_effect(scope, callback, role)
					 : sqlite_shm_lease_result<sqlite_shm_issued_reader_effect_identity>{
						   reject(sqlite_shm_lease_rejection_reason::stale_token)};
	}

	sqlite_shm_lease_result<sqlite_shm_issued_reader_session_terminal_identity>
	sqlite_shm_process_global_identity_issuer::issue_session_terminal(
		const sqlite_shm_reader_lifecycle_identity_scope& scope,
		const sqlite_shm_reader_session_terminal_identity_role role)
	{
		if (!current_before_state_lock())
			return reject(sqlite_shm_lease_rejection_reason::stale_token);
		const auto state = state_.lock();
		return state ? state->issue_session_terminal(scope, role)
					 : sqlite_shm_lease_result<sqlite_shm_issued_reader_session_terminal_identity>{
						   reject(sqlite_shm_lease_rejection_reason::stale_token)};
	}

	sqlite_shm_lease_result<void> sqlite_shm_process_global_identity_issuer::validate_callback(
		const sqlite_shm_reader_lifecycle_identity_scope& scope,
		const sqlite_shm_issued_reader_callback_identity& callback,
		const sqlite_shm_reader_callback_identity_role role) const noexcept
	{
		if (!current_before_state_lock())
			return reject(sqlite_shm_lease_rejection_reason::stale_token);
		const auto state = state_.lock();
		return state
			? state->validate_callback(scope, callback, role)
			: sqlite_shm_lease_result<void>{reject(sqlite_shm_lease_rejection_reason::stale_token)};
	}

	sqlite_shm_lease_result<void> sqlite_shm_process_global_identity_issuer::validate_effect(
		const sqlite_shm_reader_lifecycle_identity_scope& scope,
		const sqlite_shm_issued_reader_callback_identity& callback,
		const sqlite_shm_issued_reader_effect_identity& effect,
		const sqlite_shm_reader_effect_identity_role role) const noexcept
	{
		if (!current_before_state_lock())
			return reject(sqlite_shm_lease_rejection_reason::stale_token);
		const auto state = state_.lock();
		return state
			? state->validate_effect(scope, callback, effect, role)
			: sqlite_shm_lease_result<void>{reject(sqlite_shm_lease_rejection_reason::stale_token)};
	}

	sqlite_shm_lease_result<void>
	sqlite_shm_process_global_identity_issuer::validate_session_terminal(
		const sqlite_shm_reader_lifecycle_identity_scope& scope,
		const sqlite_shm_issued_reader_session_terminal_identity& terminal,
		const sqlite_shm_reader_session_terminal_identity_role role) const noexcept
	{
		if (!current_before_state_lock())
			return reject(sqlite_shm_lease_rejection_reason::stale_token);
		const auto state = state_.lock();
		return state
			? state->validate_session_terminal(scope, terminal, role)
			: sqlite_shm_lease_result<void>{reject(sqlite_shm_lease_rejection_reason::stale_token)};
	}

	sqlite_shm_lease_result<void> sqlite_shm_process_global_identity_issuer::retire_callback(
		const sqlite_shm_reader_lifecycle_identity_scope& scope,
		sqlite_shm_issued_reader_callback_identity& callback,
		const sqlite_shm_reader_callback_identity_role role) noexcept
	{
		if (!current_before_state_lock())
			return reject(sqlite_shm_lease_rejection_reason::stale_token);
		const auto state = state_.lock();
		return state
			? state->retire_callback(scope, callback, role)
			: sqlite_shm_lease_result<void>{reject(sqlite_shm_lease_rejection_reason::stale_token)};
	}

	sqlite_shm_lease_result<void> sqlite_shm_process_global_identity_issuer::retire_effect(
		const sqlite_shm_reader_lifecycle_identity_scope& scope,
		const sqlite_shm_issued_reader_callback_identity& callback,
		sqlite_shm_issued_reader_effect_identity& effect,
		const sqlite_shm_reader_effect_identity_role role) noexcept
	{
		if (!current_before_state_lock())
			return reject(sqlite_shm_lease_rejection_reason::stale_token);
		const auto state = state_.lock();
		return state
			? state->retire_effect(scope, callback, effect, role)
			: sqlite_shm_lease_result<void>{reject(sqlite_shm_lease_rejection_reason::stale_token)};
	}

	sqlite_shm_lease_result<void>
	sqlite_shm_process_global_identity_issuer::retire_session_terminal(
		const sqlite_shm_reader_lifecycle_identity_scope& scope,
		sqlite_shm_issued_reader_session_terminal_identity& terminal,
		const sqlite_shm_reader_session_terminal_identity_role role) noexcept
	{
		if (!current_before_state_lock())
			return reject(sqlite_shm_lease_rejection_reason::stale_token);
		const auto state = state_.lock();
		return state
			? state->retire_session_terminal(scope, terminal, role)
			: sqlite_shm_lease_result<void>{reject(sqlite_shm_lease_rejection_reason::stale_token)};
	}

	sqlite_shm_lease_result<void> sqlite_shm_process_global_identity_issuer::retire_scope(
		sqlite_shm_reader_lifecycle_identity_scope& scope) noexcept
	{
		if (!current_before_state_lock())
			return reject(sqlite_shm_lease_rejection_reason::stale_token);
		const auto state = state_.lock();
		return state
			? state->retire_scope(scope)
			: sqlite_shm_lease_result<void>{reject(sqlite_shm_lease_rejection_reason::stale_token)};
	}
} // namespace cxxlens::sdk
