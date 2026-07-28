#include "sqlite_same_process_shm_mapping_registry_internal.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "sqlite_writer_shm_mapping_epoch_internal.hpp"

namespace cxxlens::sdk
{
	namespace
	{
		[[nodiscard]] bool valid_identity(const sqlite_backend_opaque_identity& identity) noexcept
		{
			return !identity.profile.empty() && !identity.bytes.empty();
		}

		[[nodiscard]] bool valid_family(const sqlite_shm_lease_family_binding& family) noexcept
		{
			return valid_identity(family.process_instance) &&
				valid_identity(family.shared_runtime_vfs_cohort) &&
				valid_identity(family.exact_file_family);
		}

		[[nodiscard]] sqlite_shm_lease_rejection
		rejection(const sqlite_shm_lease_rejection_reason reason,
				  const sqlite_shm_lease_recovery_action action =
					  sqlite_shm_lease_recovery_action::deny_before_native_map) noexcept
		{
			return {reason, action};
		}

		[[nodiscard]] bool coordinator_is_completely_quiescent(
			const sqlite_shm_mapping_lease_snapshot& snapshot) noexcept
		{
			return !snapshot.quarantined &&
				snapshot.phase == sqlite_shm_mapping_generation_phase::empty &&
				!snapshot.generation.has_value() && snapshot.sealed_shm_size == 0U &&
				snapshot.mapping_page_count == 0U && snapshot.generation_authority_count == 0U &&
				snapshot.eligibility_count == 0U && snapshot.writer_inflight_count == 0U &&
				snapshot.writer_cleanup_count == 0U &&
				snapshot.writer_member_authority_count == 0U &&
				snapshot.writer_holder_count == 0U &&
				snapshot.writer_attachment_unresolved_count == 0U &&
				snapshot.writer_attachment_unresolved_member_count == 0U &&
				snapshot.reader_inflight_count == 0U && snapshot.reader_cleanup_count == 0U &&
				snapshot.reader_handoff_count == 0U && !snapshot.reader_admission_visible;
		}

		std::atomic<std::uint64_t> registry_state_destruction_count{0U};
	} // namespace

	namespace detail
	{
		struct sqlite_shm_registry_process_owner_seal
		{
			std::atomic<std::uint64_t> process_epoch{1U};
			std::atomic_bool claimed{false};
		};

		struct sqlite_shm_registry_runtime_owner_box
		{
			explicit sqlite_shm_registry_runtime_owner_box(std::shared_ptr<void> value)
				: owner{std::move(value)}
			{
			}

			std::shared_ptr<void> owner;
			std::atomic_bool release_armed{true};
		};

		enum class sqlite_shm_registry_activity_phase : std::uint8_t
		{
			active,
			clean_released,
			abandoned,
		};

		struct sqlite_shm_registry_activity_control
		{
			struct coordinates
			{
				std::uint64_t process_epoch{};
				std::uint64_t alias_token{};
				std::uint64_t family_epoch{};
				std::uint64_t family_pin_token{};
				std::uint64_t activity_token{};
			};

			sqlite_shm_registry_activity_control(
				std::weak_ptr<sqlite_shm_mapping_registry_state> registry_state_value,
				std::weak_ptr<sqlite_shm_registry_process_owner_seal> process_seal_value,
				std::shared_ptr<std::atomic_bool> emergency_latch_value,
				std::shared_ptr<std::atomic_bool> alias_authority_latch_value,
				std::shared_ptr<std::atomic_bool> family_authority_latch_value,
				sqlite_backend_opaque_identity process_instance_value,
				sqlite_shm_lease_family_binding family_value,
				sqlite_backend_opaque_identity alias_lifetime_value,
				const coordinates value)
				: registry_state{std::move(registry_state_value)},
				  process_seal{std::move(process_seal_value)},
				  emergency_latch{std::move(emergency_latch_value)},
				  alias_authority_latch{std::move(alias_authority_latch_value)},
				  family_authority_latch{std::move(family_authority_latch_value)},
				  process_instance{std::move(process_instance_value)},
				  family{std::move(family_value)}, alias_lifetime{std::move(alias_lifetime_value)},
				  process_epoch{value.process_epoch}, alias_token{value.alias_token},
				  family_epoch{value.family_epoch}, family_pin_token{value.family_pin_token},
				  activity_token{value.activity_token}
			{
			}

			[[nodiscard]] bool authority_valid_now() const noexcept
			{
				const auto state = registry_state.lock();
				const auto owner = process_seal.lock();
				return phase.load(std::memory_order_acquire) ==
					sqlite_shm_registry_activity_phase::active &&
					authority_valid.load(std::memory_order_acquire) && emergency_latch &&
					!emergency_latch->load(std::memory_order_acquire) && alias_authority_latch &&
					alias_authority_latch->load(std::memory_order_acquire) &&
					family_authority_latch &&
					family_authority_latch->load(std::memory_order_acquire) && state && owner &&
					owner->process_epoch.load(std::memory_order_acquire) == process_epoch;
			}

			const std::weak_ptr<sqlite_shm_mapping_registry_state> registry_state;
			const std::weak_ptr<sqlite_shm_registry_process_owner_seal> process_seal;
			const std::shared_ptr<std::atomic_bool> emergency_latch;
			const std::shared_ptr<std::atomic_bool> alias_authority_latch;
			const std::shared_ptr<std::atomic_bool> family_authority_latch;
			const sqlite_backend_opaque_identity process_instance;
			const sqlite_shm_lease_family_binding family;
			const sqlite_backend_opaque_identity alias_lifetime;
			const std::uint64_t process_epoch{};
			const std::uint64_t alias_token{};
			const std::uint64_t family_epoch{};
			const std::uint64_t family_pin_token{};
			const std::uint64_t activity_token{};
			std::atomic<sqlite_shm_registry_activity_phase> phase{
				sqlite_shm_registry_activity_phase::active};
			std::atomic_bool authority_valid{true};
			std::atomic_bool audit_seal_minted{false};
		};

		struct sqlite_shm_writer_member_authority_state
		{
			std::optional<sqlite_writer_shm_mapping_epoch_arm> epoch_arm;
			std::optional<sqlite_shm_registry_activity_seal> audit_seal;
			// Declared last so ambiguous destruction invalidates registry activity first while
			// the strong epoch arm is still retained.
			std::optional<sqlite_shm_registry_activity_pin> activity;
		};

		class sqlite_shm_mapping_registry_state final
			: public std::enable_shared_from_this<sqlite_shm_mapping_registry_state>
		{
		  private:
			struct alias_record
			{
				alias_record(const std::uint64_t alias_token,
							 sqlite_shm_registry_alias_binding binding_value)
					: token{alias_token},
					  process_instance{std::move(binding_value.process_instance_)},
					  shared_runtime_vfs_cohort{
						  std::move(binding_value.shared_runtime_vfs_cohort_)},
					  alias_lifetime{std::move(binding_value.alias_lifetime_)},
					  runtime_lifetime{std::move(binding_value.runtime_lifetime_)},
					  activity_authority_latch{std::make_shared<std::atomic_bool>(true)}
				{
				}

				alias_record(alias_record&&) noexcept = default;
				alias_record& operator=(alias_record&&) = delete;
				alias_record(const alias_record&) = delete;
				alias_record& operator=(const alias_record&) = delete;

				std::uint64_t token{};
				sqlite_backend_opaque_identity process_instance;
				sqlite_backend_opaque_identity shared_runtime_vfs_cohort;
				sqlite_backend_opaque_identity alias_lifetime;
				sqlite_shm_registry_runtime_lifetime_pin runtime_lifetime;
				sqlite_backend_opaque_identity registration_epoch;
				sqlite_backend_opaque_identity unregistration_epoch;
				sqlite_shm_registry_alias_phase phase{sqlite_shm_registry_alias_phase::reserved};
				std::size_t active_family_pins{};
				std::size_t active_activities{};
				std::shared_ptr<std::atomic_bool> activity_authority_latch;
			};

			struct family_record
			{
				family_record(const std::uint64_t epoch,
							  sqlite_shm_lease_family_binding family_binding,
							  std::shared_ptr<sqlite_same_process_shm_mapping_lease_coordinator>
								  coordinator_value)
					: entry_epoch{epoch}, binding{std::move(family_binding)},
					  coordinator{std::move(coordinator_value)},
					  activity_authority_latch{std::make_shared<std::atomic_bool>(true)}
				{
				}

				std::uint64_t entry_epoch{};
				sqlite_shm_lease_family_binding binding;
				std::shared_ptr<sqlite_same_process_shm_mapping_lease_coordinator> coordinator;
				sqlite_shm_registry_family_phase phase{sqlite_shm_registry_family_phase::active};
				std::size_t active_family_pins{};
				std::size_t active_activities{};
				std::shared_ptr<std::atomic_bool> activity_authority_latch;
			};

			struct family_pin_record
			{
				std::uint64_t token{};
				std::uint64_t alias_token{};
				std::uint64_t family_epoch{};
				std::size_t active_activities{};
				bool active{};
				bool abandoned{};
			};

			struct activity_record
			{
				std::uint64_t token{};
				std::uint64_t alias_token{};
				std::uint64_t family_epoch{};
				std::uint64_t family_pin_token{};
				bool active{};
				std::shared_ptr<sqlite_shm_registry_activity_control> control;
			};

			struct initialization
			{
				std::uint64_t process_epoch{};
				std::uint64_t first_mapping_generation{};
			};

		  public:
			[[nodiscard]] static std::shared_ptr<sqlite_shm_mapping_registry_state>
			create(sqlite_backend_opaque_identity process_instance,
				   std::shared_ptr<sqlite_shm_registry_process_owner_seal> seal,
				   const std::uint64_t process_epoch,
				   const std::uint64_t first_mapping_generation)
			{
				auto* raw = new sqlite_shm_mapping_registry_state{
					std::move(process_instance),
					seal,
					{process_epoch, first_mapping_generation},
				};
				return std::shared_ptr<sqlite_shm_mapping_registry_state>{
					raw,
					[seal = std::move(seal),
					 process_epoch](sqlite_shm_mapping_registry_state* state) noexcept
					{
						if (seal->process_epoch.load(std::memory_order_acquire) == process_epoch)
						{
							delete state;
							registry_state_destruction_count.fetch_add(1U,
																	   std::memory_order_relaxed);
						}
					},
				};
			}

			~sqlite_shm_mapping_registry_state()
			{
				activity_emergency_latch_->store(true, std::memory_order_release);
			}

			[[nodiscard]] bool current(const std::uint64_t process_epoch) const noexcept
			{
				return process_epoch != 0U && process_epoch == process_epoch_ &&
					seal_->process_epoch.load(std::memory_order_acquire) == process_epoch_;
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_registry_runtime_lifetime_pin>
			adopt_runtime_lifetime(sqlite_backend_opaque_identity identity,
								   sqlite_backend_opaque_identity pin_identity,
								   std::shared_ptr<void> owner)
			{
				if (!current(process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token);
				if (!valid_identity(identity) || !valid_identity(pin_identity) || !owner ||
					owner.use_count() == 0)
					return rejection(sqlite_shm_lease_rejection_reason::invalid_identity);

				std::weak_ptr<void> owner_control{owner};
				auto* raw = new sqlite_shm_registry_runtime_owner_box{std::move(owner)};
				auto box = std::shared_ptr<sqlite_shm_registry_runtime_owner_box>{
					raw,
					[seal = seal_, process_epoch = process_epoch_](
						sqlite_shm_registry_runtime_owner_box* value) noexcept
					{
						if (seal->process_epoch.load(std::memory_order_acquire) == process_epoch &&
							value->release_armed.load(std::memory_order_acquire))
							delete value;
					},
				};
				auto candidate = sqlite_shm_registry_runtime_lifetime_pin{
					process_instance_,
					seal_,
					process_epoch_,
					std::move(identity),
					std::move(pin_identity),
					std::move(owner_control),
					std::move(box),
				};
				std::scoped_lock lock{mutex_};
				synchronize_activity_controls_locked();
				if (!current(process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token);
				if (admission_quarantined_locked())
					return rejection(sqlite_shm_lease_rejection_reason::quarantined,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);

				return candidate;
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_registry_alias_pin>
			reserve_alias(sqlite_shm_registry_alias_binding binding)
			{
				if (!current(process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token);
				if (!valid_identity(binding.process_instance_) ||
					!valid_identity(binding.shared_runtime_vfs_cohort_) ||
					!valid_identity(binding.alias_lifetime_) || !binding.runtime_lifetime_.valid())
					return rejection(sqlite_shm_lease_rejection_reason::invalid_identity);

				std::scoped_lock lock{mutex_};
				synchronize_activity_controls_locked();
				if (!current(process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token);
				if (admission_quarantined_locked())
					return rejection(sqlite_shm_lease_rejection_reason::quarantined,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				if (binding.process_instance_ != process_instance_ ||
					binding.runtime_lifetime_.process_instance_ != process_instance_ ||
					binding.runtime_lifetime_.process_seal_.get() != seal_.get() ||
					binding.runtime_lifetime_.process_epoch_ != process_epoch_ ||
					!binding.runtime_lifetime_.valid())
				{
					increment_audit_counter_locked(cross_binding_rejection_count_);
					return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch);
				}

				for (const auto& record : aliases_)
				{
					const bool reused_runtime_identity =
						record.runtime_lifetime.identity() == binding.runtime_lifetime_.identity();
					const bool reused_pin_identity = record.runtime_lifetime.pin_identity() ==
						binding.runtime_lifetime_.pin_identity();
					if (reused_runtime_identity || reused_pin_identity)
					{
						increment_audit_counter_locked(duplicate_rejection_count_);
						return rejection(sqlite_shm_lease_rejection_reason::stale_token);
					}
					if (record.alias_lifetime != binding.alias_lifetime_)
						continue;
					const bool exact = record.process_instance == binding.process_instance_ &&
						record.shared_runtime_vfs_cohort == binding.shared_runtime_vfs_cohort_ &&
						record.runtime_lifetime.identity() ==
							binding.runtime_lifetime_.identity() &&
						record.runtime_lifetime.pin_identity() ==
							binding.runtime_lifetime_.pin_identity();
					if (exact)
					{
						increment_audit_counter_locked(duplicate_rejection_count_);
						return rejection(sqlite_shm_lease_rejection_reason::stale_token);
					}
					increment_audit_counter_locked(cross_binding_rejection_count_);
					quarantine_registry_locked();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}

				std::uint64_t token{};
				if (!allocate_counter_locked(next_alias_token_, token))
					return counter_exhaustion_rejection();
				aliases_.emplace_back(token, std::move(binding));
				aliases_.back().runtime_lifetime.owner_box_->release_armed.store(
					false, std::memory_order_release);
				return sqlite_shm_registry_alias_pin{
					shared_from_this(),
					{process_epoch_, token},
				};
			}

			[[nodiscard]] sqlite_shm_lease_result<void>
			begin_alias_register(sqlite_shm_registry_alias_pin& pin) noexcept
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token);
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					auto* alias = current_alias_pin_locked(pin);
					if (alias == nullptr)
						return alias_pin_rejection_locked(pin);
					if (alias->phase != sqlite_shm_registry_alias_phase::reserved)
						return rejection(sqlite_shm_lease_rejection_reason::stale_token);
					alias->phase = sqlite_shm_registry_alias_phase::registering;
					return {};
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<void> confirm_alias_registered(
				sqlite_shm_registry_alias_pin& pin,
				const sqlite_shm_verified_alias_registration_receipt& receipt) noexcept
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token);
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					auto* alias = current_alias_pin_locked(pin);
					if (alias == nullptr)
						return alias_pin_rejection_locked(pin);
					if (alias->phase != sqlite_shm_registry_alias_phase::registering)
						return rejection(sqlite_shm_lease_rejection_reason::stale_token);
					if (!registration_receipt_matches_locked(*alias, receipt) ||
						receipt_epoch_seen_locked(receipt.registration_epoch()))
					{
						quarantine_alias_locked(*alias);
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					}
					alias->registration_epoch = receipt.registration_epoch();
					alias->phase = sqlite_shm_registry_alias_phase::registered;
					return {};
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<void>
			cancel_unregistered_alias(sqlite_shm_registry_alias_pin& pin) noexcept
			{
				std::shared_ptr<sqlite_shm_registry_runtime_owner_box> owner_to_release;
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token);
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					auto* alias = current_alias_pin_locked(pin);
					if (alias == nullptr)
						return alias_pin_rejection_locked(pin);
					if (alias->phase != sqlite_shm_registry_alias_phase::reserved ||
						alias->active_family_pins != 0U || alias->active_activities != 0U)
						return rejection(sqlite_shm_lease_rejection_reason::invalid_request);
					owner_to_release = detach_alias_locked(*alias);
					pin.disarm();
					return {};
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_registry_family_pin>
			install_or_join_family(sqlite_shm_registry_alias_pin& alias,
								   const sqlite_shm_lease_family_binding& family)
			{
				return pin_family(alias, family, true);
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_registry_family_pin>
			pin_existing_family(sqlite_shm_registry_alias_pin& alias,
								const sqlite_shm_lease_family_binding& family)
			{
				return pin_family(alias, family, false);
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_registry_activity_pin>
			acquire_activity(sqlite_shm_registry_family_pin& pin)
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token);
				std::scoped_lock lock{mutex_};
				synchronize_activity_controls_locked();
				synchronize_coordinator_quarantines_locked();
				if (admission_quarantined_locked())
					return rejection(sqlite_shm_lease_rejection_reason::quarantined,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				auto* family_pin = current_family_pin_locked(pin);
				auto* alias = find_alias_locked(pin.alias_token_);
				auto* family = find_family_epoch_locked(pin.family_epoch_);
				if (family_pin == nullptr || alias == nullptr || family == nullptr)
					return rejection(sqlite_shm_lease_rejection_reason::stale_token);
				if (alias->phase == sqlite_shm_registry_alias_phase::quarantined ||
					family->phase == sqlite_shm_registry_family_phase::quarantined)
					return rejection(sqlite_shm_lease_rejection_reason::quarantined,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				if (alias->phase != sqlite_shm_registry_alias_phase::registered ||
					family->phase != sqlite_shm_registry_family_phase::active)
					return rejection(sqlite_shm_lease_rejection_reason::retiring);
				if (!alias->activity_authority_latch ||
					!alias->activity_authority_latch->load(std::memory_order_acquire))
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				if (!exact_family_admission_visible_locked(*family))
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);

				std::uint64_t token{};
				if (!allocate_counter_locked(next_activity_token_, token))
					return counter_exhaustion_rejection();
				const sqlite_shm_registry_activity_control::coordinates coordinates{
					process_epoch_,
					alias->token,
					family->entry_epoch,
					family_pin->token,
					token,
				};
				auto control = std::make_shared<sqlite_shm_registry_activity_control>(
					weak_from_this(),
					seal_,
					activity_emergency_latch_,
					alias->activity_authority_latch,
					family->activity_authority_latch,
					process_instance_,
					family->binding,
					alias->alias_lifetime,
					coordinates);
				activities_.push_back(
					{token, alias->token, family->entry_epoch, family_pin->token, true, control});
				++family_pin->active_activities;
				++alias->active_activities;
				++family->active_activities;
				if (!control->authority_valid_now())
				{
					control->authority_valid.store(false, std::memory_order_release);
					control->phase.store(sqlite_shm_registry_activity_phase::clean_released,
										 std::memory_order_release);
					synchronize_activity_controls_locked();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
				return sqlite_shm_registry_activity_pin{weak_from_this(), std::move(control)};
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_writer_map_inflight>
			begin_writer_map(sqlite_shm_registry_family_pin& pin,
							 sqlite_writer_shm_mapping_epoch_arm arm,
							 const sqlite_shm_writer_map_request& request)
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token);

				try
				{
					// Allocate the opaque bundle before either mutex is acquired.
					auto storage = std::make_unique<sqlite_shm_writer_member_authority_state>();
					std::optional<sqlite_shm_writer_member_authority> authority;
					std::optional<sqlite_shm_lease_result<sqlite_shm_writer_map_inflight>> result;
					{
						std::scoped_lock lock{mutex_};
						synchronize_activity_controls_locked();
						synchronize_coordinator_quarantines_locked();
						if (admission_quarantined_locked())
							return rejection(sqlite_shm_lease_rejection_reason::quarantined,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);

						auto* family_pin = current_family_pin_locked(pin);
						auto* alias = find_alias_locked(pin.alias_token_);
						auto* family = find_family_epoch_locked(pin.family_epoch_);
						if (family_pin == nullptr || alias == nullptr || family == nullptr)
							return rejection(sqlite_shm_lease_rejection_reason::stale_token);
						if (alias->phase == sqlite_shm_registry_alias_phase::quarantined ||
							family->phase == sqlite_shm_registry_family_phase::quarantined)
							return rejection(sqlite_shm_lease_rejection_reason::quarantined,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if (alias->phase != sqlite_shm_registry_alias_phase::registered ||
							family->phase != sqlite_shm_registry_family_phase::active)
							return rejection(sqlite_shm_lease_rejection_reason::retiring);
						if (request.family != family->binding ||
							request.alias_lifetime != alias->alias_lifetime ||
							request.attachment.family() != family->binding ||
							request.attachment.alias_lifetime() != alias->alias_lifetime)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch);
						if (!alias->activity_authority_latch ||
							!alias->activity_authority_latch->load(std::memory_order_acquire) ||
							!exact_family_admission_visible_locked(*family))
							return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);

						std::uint64_t token{};
						if (!allocate_counter_locked(next_activity_token_, token))
							return counter_exhaustion_rejection();
						const sqlite_shm_registry_activity_control::coordinates coordinates{
							process_epoch_,
							alias->token,
							family->entry_epoch,
							family_pin->token,
							token,
						};
						auto control = std::make_shared<sqlite_shm_registry_activity_control>(
							weak_from_this(),
							seal_,
							activity_emergency_latch_,
							alias->activity_authority_latch,
							family->activity_authority_latch,
							process_instance_,
							family->binding,
							alias->alias_lifetime,
							coordinates);
						activities_.push_back({token,
											   alias->token,
											   family->entry_epoch,
											   family_pin->token,
											   true,
											   control});
						++family_pin->active_activities;
						++alias->active_activities;
						++family->active_activities;

						storage->activity.emplace(
							sqlite_shm_registry_activity_pin{weak_from_this(), control});
						auto audit = storage->activity->seal_for_audit();
						if (!audit)
						{
							control->authority_valid.store(false, std::memory_order_release);
							control->phase.store(sqlite_shm_registry_activity_phase::clean_released,
												 std::memory_order_release);
							synchronize_activity_controls_locked();
							return audit.error();
						}
						storage->epoch_arm.emplace(std::move(arm));
						storage->audit_seal.emplace(std::move(*audit));
						authority.emplace(sqlite_shm_writer_member_authority{std::move(storage)});
						if (!authority->valid_for_predelegation(request))
						{
							control->authority_valid.store(false, std::memory_order_release);
							control->phase.store(sqlite_shm_registry_activity_phase::clean_released,
												 std::memory_order_release);
							synchronize_activity_controls_locked();
							return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						}

						// Registry-to-lease is the only lock order. The authority remains owned
						// here until the coordinator's final no-throw publish edge.
						result.emplace(
							family->coordinator->begin_registry_writer_map(request, *authority));
						if (!*result)
						{
							auto expected = sqlite_shm_registry_activity_phase::active;
							if (!control->phase.compare_exchange_strong(
									expected,
									sqlite_shm_registry_activity_phase::clean_released,
									std::memory_order_acq_rel,
									std::memory_order_acquire))
								emergency_quarantine();
							control->authority_valid.store(false, std::memory_order_release);
							synchronize_activity_controls_locked();
						}
					}
					if (!result)
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					return std::move(*result);
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<void>
			release_activity(sqlite_shm_registry_activity_pin& pin) noexcept
			{
				try
				{
					if (!pin.control_)
						return rejection(sqlite_shm_lease_rejection_reason::stale_token);
					const auto owner_state = pin.state_.lock();
					if (!owner_state)
						return rejection(sqlite_shm_lease_rejection_reason::stale_token);
					if (owner_state.get() != this)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch);
					if (!current(pin.control_->process_epoch))
					{
						pin.disarm();
						return rejection(sqlite_shm_lease_rejection_reason::stale_token);
					}
					{
						std::scoped_lock lock{mutex_};
						synchronize_activity_controls_locked();
						synchronize_coordinator_quarantines_locked();
						auto* activity = current_activity_locked(pin);
						if (activity == nullptr)
							return current_pin_rejection(pin.control_->process_epoch);
						auto expected = sqlite_shm_registry_activity_phase::active;
						if (!pin.control_->phase.compare_exchange_strong(
								expected,
								sqlite_shm_registry_activity_phase::clean_released,
								std::memory_order_acq_rel,
								std::memory_order_acquire))
							return current_pin_rejection(pin.control_->process_epoch);
						pin.control_->authority_valid.store(false, std::memory_order_release);
						synchronize_activity_controls_locked();
						if (activity->active)
						{
							emergency_quarantine();
							return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						}
					}
					// The last activity-control owner and any retained native owners must never be
					// destroyed while the registry mutex is held.
					pin.disarm();
					return {};
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<void>
			release_family(sqlite_shm_registry_family_pin& pin) noexcept
			{
				try
				{
					if (!pin.state_)
						return rejection(sqlite_shm_lease_rejection_reason::stale_token);
					if (pin.state_.get() != this)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch);
					if (!current(pin.process_epoch_))
					{
						pin.disarm();
						return rejection(sqlite_shm_lease_rejection_reason::stale_token);
					}
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					synchronize_coordinator_quarantines_locked();
					auto* family_pin = current_family_pin_locked(pin);
					if (family_pin == nullptr)
						return current_pin_rejection(pin.process_epoch_);
					if (family_pin->active_activities != 0U)
						return rejection(sqlite_shm_lease_rejection_reason::retiring,
										 sqlite_shm_lease_recovery_action::
											 await_complete_attachment_gate_boundary);
					auto* family = find_family_epoch_locked(family_pin->family_epoch);
					if (family != nullptr && family->active_family_pins == 1U &&
						family->phase == sqlite_shm_registry_family_phase::active &&
						exact_family_admission_visible_locked(*family))
					{
						const auto coordinator_snapshot = family->coordinator->snapshot();
						if (coordinator_snapshot.quarantined)
							quarantine_family_locked(*family);
						else if (!coordinator_is_completely_quiescent(coordinator_snapshot))
							return rejection(sqlite_shm_lease_rejection_reason::retiring,
											 sqlite_shm_lease_recovery_action::
												 await_complete_attachment_gate_boundary);
					}
					const bool clean = release_family_locked(*family_pin);
					pin.disarm();
					if (!clean)
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					return {};
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<void>
			begin_alias_unregister(sqlite_shm_registry_alias_pin& pin) noexcept
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token);
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					auto* alias = current_alias_pin_locked(pin);
					if (alias == nullptr)
						return alias_pin_rejection_locked(pin);
					if (alias->phase != sqlite_shm_registry_alias_phase::registered)
						return rejection(sqlite_shm_lease_rejection_reason::stale_token);
					alias->phase = sqlite_shm_registry_alias_phase::unregistering;
					return {};
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<void>
			poll_alias_unregister(sqlite_shm_registry_alias_pin& pin) noexcept
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token);
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					auto* alias = current_alias_pin_locked(pin);
					if (alias == nullptr)
						return alias_pin_rejection_locked(pin);
					if (alias->phase != sqlite_shm_registry_alias_phase::unregistering)
						return rejection(sqlite_shm_lease_rejection_reason::stale_token);
					if (alias->active_family_pins != 0U || alias->active_activities != 0U)
						return rejection(sqlite_shm_lease_rejection_reason::retiring,
										 sqlite_shm_lease_recovery_action::
											 await_complete_attachment_gate_boundary);
					if (!alias_coordinators_quiescent_locked(*alias))
						return rejection(sqlite_shm_lease_rejection_reason::retiring,
										 sqlite_shm_lease_recovery_action::
											 await_complete_attachment_gate_boundary);
					return {};
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<void> confirm_alias_unregistered(
				sqlite_shm_registry_alias_pin& pin,
				const sqlite_shm_verified_alias_unregistration_receipt& receipt) noexcept
			{
				std::shared_ptr<sqlite_shm_registry_runtime_owner_box> owner_to_release;
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token);
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					auto* alias = current_alias_pin_locked(pin);
					if (alias == nullptr)
						return alias_pin_rejection_locked(pin);
					if (alias->phase != sqlite_shm_registry_alias_phase::unregistering)
						return rejection(sqlite_shm_lease_rejection_reason::stale_token);
					if (alias->active_family_pins != 0U || alias->active_activities != 0U)
						return rejection(sqlite_shm_lease_rejection_reason::retiring,
										 sqlite_shm_lease_recovery_action::
											 await_complete_attachment_gate_boundary);
					if (!alias_coordinators_quiescent_locked(*alias))
						return rejection(sqlite_shm_lease_rejection_reason::retiring,
										 sqlite_shm_lease_recovery_action::
											 await_complete_attachment_gate_boundary);
					if (!unregistration_receipt_matches_locked(*alias, receipt) ||
						receipt_epoch_seen_locked(receipt.unregistration_epoch()))
					{
						quarantine_alias_locked(*alias);
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					}
					alias->unregistration_epoch = receipt.unregistration_epoch();
					owner_to_release = detach_alias_locked(*alias);
					pin.disarm();
					return {};
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_mapping_registry_snapshot snapshot() noexcept
			{
				sqlite_shm_mapping_registry_snapshot output;
				output.process_epoch = process_epoch_;
				output.process_live = current(process_epoch_);
				output.generation_source_count = 1U;
				if (!output.process_live)
					return output;
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					synchronize_coordinator_quarantines_locked();
					output.registry_quarantined = registry_quarantined_ ||
						emergency_quarantined_.load(std::memory_order_acquire);
					output.alias_record_count = aliases_.size();
					output.family_record_count = families_.size();
					output.duplicate_rejection_count = duplicate_rejection_count_;
					output.cross_binding_rejection_count = cross_binding_rejection_count_;
					output.ambiguous_lookup_count = ambiguous_lookup_count_;

					for (const auto& alias : aliases_)
					{
						switch (alias.phase)
						{
							case sqlite_shm_registry_alias_phase::reserved:
								++output.reserved_alias_count;
								break;
							case sqlite_shm_registry_alias_phase::registering:
								++output.registering_alias_count;
								break;
							case sqlite_shm_registry_alias_phase::registered:
								++output.registered_alias_count;
								break;
							case sqlite_shm_registry_alias_phase::unregistering:
								++output.unregistering_alias_count;
								break;
							case sqlite_shm_registry_alias_phase::detached:
								++output.detached_alias_tombstone_count;
								break;
							case sqlite_shm_registry_alias_phase::quarantined:
								++output.quarantined_alias_count;
								break;
						}
					}
					for (const auto& family : families_)
					{
						switch (family.phase)
						{
							case sqlite_shm_registry_family_phase::active:
								++output.active_family_count;
								break;
							case sqlite_shm_registry_family_phase::retired:
								++output.retired_family_tombstone_count;
								break;
							case sqlite_shm_registry_family_phase::quarantined:
								++output.quarantined_family_count;
								break;
						}
					}
					for (const auto& pin : family_pins_)
						output.active_family_pin_count += pin.active ? 1U : 0U;
					for (const auto& activity : activities_)
						output.active_activity_pin_count += activity.active ? 1U : 0U;

					for (std::size_t index = 0; index < aliases_.size(); ++index)
					{
						const auto& candidate = aliases_[index];
						if (candidate.phase == sqlite_shm_registry_alias_phase::detached ||
							candidate.phase == sqlite_shm_registry_alias_phase::quarantined)
							continue;
						bool first = true;
						for (std::size_t prior = 0; prior < index; ++prior)
						{
							const auto& previous = aliases_[prior];
							if (previous.phase != sqlite_shm_registry_alias_phase::detached &&
								previous.phase != sqlite_shm_registry_alias_phase::quarantined &&
								previous.shared_runtime_vfs_cohort ==
									candidate.shared_runtime_vfs_cohort)
							{
								first = false;
								break;
							}
						}
						output.cohort_count += first ? 1U : 0U;
					}
					return output;
				}
				catch (...)
				{
					emergency_quarantine();
					output.registry_quarantined = true;
					return output;
				}
			}

			[[nodiscard]] sqlite_shm_mapping_registry_family_snapshot
			family_snapshot(const sqlite_shm_lease_family_binding& binding) noexcept
			{
				sqlite_shm_mapping_registry_family_snapshot output;
				if (!current(process_epoch_) || !valid_family(binding))
					return output;
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					synchronize_coordinator_quarantines_locked();
					family_record* singleton = nullptr;
					for (auto& family : families_)
					{
						if (family.binding != binding)
							continue;
						if (family.phase == sqlite_shm_registry_family_phase::active)
						{
							++output.exact_active_match_count;
							singleton = &family;
						}
						else if (family.phase == sqlite_shm_registry_family_phase::retired)
							++output.exact_retired_match_count;
						else if (family.phase == sqlite_shm_registry_family_phase::quarantined)
							++output.exact_quarantined_match_count;
					}
					if (output.exact_active_match_count > 1U ||
						(output.exact_active_match_count == 1U &&
						 output.exact_quarantined_match_count != 0U))
					{
						increment_audit_counter_locked(ambiguous_lookup_count_);
						quarantine_registry_locked();
						return output;
					}
					if (output.exact_active_match_count != 1U ||
						output.exact_quarantined_match_count != 0U || singleton == nullptr ||
						registry_quarantined_ ||
						emergency_quarantined_.load(std::memory_order_acquire))
						return output;
					if (!singleton->coordinator)
					{
						quarantine_registry_locked();
						return output;
					}

					output.entry_epoch = singleton->entry_epoch;
					output.alias_pin_count = singleton->active_family_pins;
					output.activity_pin_count = singleton->active_activities;
					output.phase = singleton->phase;
					output.lookup_visible = true;
					output.coordinator = singleton->coordinator->snapshot();
					output.coordinator_present = true;
					return output;
				}
				catch (...)
				{
					emergency_quarantine();
					return {};
				}
			}

			[[nodiscard]] bool alias_pin_valid(const sqlite_shm_registry_alias_pin& pin) noexcept
			{
				if (!current(pin.process_epoch_))
					return false;
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					return current_alias_pin_locked(pin) != nullptr;
				}
				catch (...)
				{
					emergency_quarantine();
					return false;
				}
			}

			[[nodiscard]] bool family_pin_valid(const sqlite_shm_registry_family_pin& pin) noexcept
			{
				if (!current(pin.process_epoch_))
					return false;
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					if (admission_quarantined_locked() || current_family_pin_locked(pin) == nullptr)
						return false;
					const auto* alias = find_alias_locked(pin.alias_token_);
					auto* family = find_family_epoch_locked(pin.family_epoch_);
					const bool alias_live = alias != nullptr &&
						(alias->phase == sqlite_shm_registry_alias_phase::registered ||
						 alias->phase == sqlite_shm_registry_alias_phase::unregistering);
					return alias_live && family != nullptr &&
						exact_family_admission_visible_locked(*family);
				}
				catch (...)
				{
					emergency_quarantine();
					return false;
				}
			}

			void abandon_alias(const sqlite_shm_registry_alias_pin& pin) noexcept
			{
				if (!current(pin.process_epoch_))
					return;
				std::shared_ptr<sqlite_shm_registry_runtime_owner_box> owner_to_release;
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					auto* alias = find_alias_locked(pin.token_);
					if (alias == nullptr)
						return;
					if (alias->phase == sqlite_shm_registry_alias_phase::reserved &&
						alias->active_family_pins == 0U && alias->active_activities == 0U)
					{
						owner_to_release = detach_alias_locked(*alias);
						return;
					}
					if (alias->phase != sqlite_shm_registry_alias_phase::detached)
						quarantine_alias_locked(*alias);
				}
				catch (...)
				{
					emergency_quarantine();
				}
			}

			void abandon_family(const sqlite_shm_registry_family_pin& family_pin) noexcept
			{
				if (!current(family_pin.process_epoch_))
					return;
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					auto* pin = find_family_pin_locked(family_pin.pin_token_);
					if (pin == nullptr || !pin->active ||
						pin->alias_token != family_pin.alias_token_ ||
						pin->family_epoch != family_pin.family_epoch_)
						return;
					if (pin->active_activities != 0U)
					{
						pin->abandoned = true;
						auto* alias = find_alias_locked(family_pin.alias_token_);
						auto* family = find_family_epoch_locked(family_pin.family_epoch_);
						if (alias != nullptr)
							quarantine_alias_locked(*alias);
						if (family != nullptr)
							quarantine_family_locked(*family);
						return;
					}
					(void)release_family_locked(*pin);
				}
				catch (...)
				{
					emergency_quarantine();
				}
			}

			void invalidate_process_instance() noexcept
			{
				auto observed = seal_->process_epoch.load(std::memory_order_acquire);
				while (observed == process_epoch_)
				{
					const auto replacement =
						observed == std::numeric_limits<std::uint64_t>::max() ? 0U : observed + 1U;
					if (seal_->process_epoch.compare_exchange_weak(observed,
																   replacement,
																   std::memory_order_acq_rel,
																   std::memory_order_acquire))
						return;
				}
			}

			void lock_mutex_for_fork_testing()
			{
				mutex_.lock();
			}

			void unlock_mutex_for_fork_testing() noexcept
			{
				mutex_.unlock();
			}

			[[nodiscard]] bool
			inject_duplicate_family(const sqlite_shm_lease_family_binding& binding) noexcept
			{
				if (!current(process_epoch_) || !valid_family(binding))
					return false;
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					if (binding.process_instance != process_instance_ || registry_quarantined_ ||
						emergency_quarantined_.load(std::memory_order_acquire))
						return false;
					std::size_t active_matches{};
					for (const auto& family : families_)
						active_matches += family.binding == binding &&
								family.phase == sqlite_shm_registry_family_phase::active
							? 1U
							: 0U;
					if (active_matches != 1U)
						return false;
					std::uint64_t epoch{};
					if (!allocate_counter_locked(next_family_epoch_, epoch))
						return false;
					families_.emplace_back(
						epoch,
						binding,
						std::make_shared<sqlite_same_process_shm_mapping_lease_coordinator>(
							binding, generations_));
					return true;
				}
				catch (...)
				{
					emergency_quarantine();
					return false;
				}
			}

			void exhaust_counters() noexcept
			{
				if (!current(process_epoch_))
					return;
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					next_alias_token_ = std::numeric_limits<std::uint64_t>::max();
					next_family_epoch_ = std::numeric_limits<std::uint64_t>::max();
					next_family_pin_token_ = std::numeric_limits<std::uint64_t>::max();
					next_activity_token_ = std::numeric_limits<std::uint64_t>::max();
				}
				catch (...)
				{
					emergency_quarantine();
				}
			}

			void exhaust_counter(const sqlite_shm_registry_counter_for_testing counter) noexcept
			{
				if (!current(process_epoch_))
					return;
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					switch (counter)
					{
						case sqlite_shm_registry_counter_for_testing::alias_token:
							next_alias_token_ = std::numeric_limits<std::uint64_t>::max();
							break;
						case sqlite_shm_registry_counter_for_testing::family_epoch:
							next_family_epoch_ = std::numeric_limits<std::uint64_t>::max();
							break;
						case sqlite_shm_registry_counter_for_testing::family_pin_token:
							next_family_pin_token_ = std::numeric_limits<std::uint64_t>::max();
							break;
						case sqlite_shm_registry_counter_for_testing::activity_token:
							next_activity_token_ = std::numeric_limits<std::uint64_t>::max();
							break;
					}
				}
				catch (...)
				{
					emergency_quarantine();
				}
			}

			[[nodiscard]] sqlite_same_process_shm_mapping_lease_coordinator*
			coordinator_for_activity(const sqlite_shm_registry_activity_pin& pin) noexcept
			{
				if (!pin.control_ || !current(pin.control_->process_epoch) ||
					!pin.control_->authority_valid_now())
					return nullptr;
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					if (registry_quarantined_ ||
						emergency_quarantined_.load(std::memory_order_acquire) ||
						current_activity_locked(pin) == nullptr ||
						!pin.control_->authority_valid_now())
						return nullptr;
					auto* family = find_family_epoch_locked(pin.control_->family_epoch);
					return family != nullptr && exact_family_admission_visible_locked(*family)
						? family->coordinator.get()
						: nullptr;
				}
				catch (...)
				{
					emergency_quarantine();
					return nullptr;
				}
			}

			[[nodiscard]] sqlite_same_process_shm_mapping_lease_coordinator*
			coordinator_for_family(const sqlite_shm_lease_family_binding& binding) noexcept
			{
				if (!current(process_epoch_))
					return nullptr;
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					synchronize_coordinator_quarantines_locked();
					const auto found = std::find_if(families_.begin(),
													families_.end(),
													[&binding](const family_record& family)
													{
														return family.binding == binding;
													});
					return found != families_.end() &&
							found->phase == sqlite_shm_registry_family_phase::active &&
							exact_family_admission_visible_locked(*found)
						? found->coordinator.get()
						: nullptr;
				}
				catch (...)
				{
					emergency_quarantine();
					return nullptr;
				}
			}

			[[nodiscard]] bool
			activity_seal_matches(const sqlite_shm_registry_activity_seal& audit_seal,
								  const sqlite_backend_opaque_identity& process_instance,
								  const sqlite_shm_lease_family_binding& family,
								  const sqlite_backend_opaque_identity& alias_lifetime) noexcept
			{
				if (!current(process_epoch_))
					return false;
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					synchronize_coordinator_quarantines_locked();
					const auto control = audit_seal.control_.lock();
					if (!control || !control->audit_seal_minted.load(std::memory_order_acquire) ||
						!control->authority_valid_now() ||
						control->process_instance != process_instance ||
						control->process_instance != process_instance_ ||
						control->family != family || control->alias_lifetime != alias_lifetime)
						return false;
					auto* activity = find_activity_locked(control->activity_token);
					return activity != nullptr && activity->active &&
						activity->control.get() == control.get() &&
						activity_control_matches_record_locked(*activity);
				}
				catch (...)
				{
					emergency_quarantine();
					return false;
				}
			}

			[[nodiscard]] const void* generation_source_identity() const noexcept
			{
				return current(process_epoch_) ? generations_.get() : nullptr;
			}

			void lock_mutex_for_testing()
			{
				mutex_.lock();
			}

			void unlock_mutex_for_testing()
			{
				mutex_.unlock();
			}

		  private:
			sqlite_shm_mapping_registry_state(
				sqlite_backend_opaque_identity process_instance,
				std::shared_ptr<sqlite_shm_registry_process_owner_seal> seal,
				const initialization value)
				: process_instance_{std::move(process_instance)}, seal_{std::move(seal)},
				  process_epoch_{value.process_epoch},
				  generations_{std::make_shared<sqlite_shm_mapping_generation_source>(
					  value.first_mapping_generation)}
			{
			}

			[[nodiscard]] bool admission_quarantined_locked() const noexcept
			{
				return registry_quarantined_ ||
					emergency_quarantined_.load(std::memory_order_acquire);
			}

			[[nodiscard]] bool
			exact_family_admission_visible_locked(family_record& selected) noexcept
			{
				synchronize_coordinator_quarantines_locked();
				std::size_t active_matches{};
				bool quarantined_match{};
				for (const auto& family : families_)
				{
					if (family.binding != selected.binding)
						continue;
					active_matches +=
						family.phase == sqlite_shm_registry_family_phase::active ? 1U : 0U;
					quarantined_match = quarantined_match ||
						family.phase == sqlite_shm_registry_family_phase::quarantined;
				}
				if (active_matches > 1U || (active_matches == 1U && quarantined_match))
				{
					increment_audit_counter_locked(ambiguous_lookup_count_);
					quarantine_registry_locked();
					return false;
				}
				if (active_matches != 1U || quarantined_match ||
					selected.phase != sqlite_shm_registry_family_phase::active ||
					!selected.coordinator || !selected.activity_authority_latch ||
					!selected.activity_authority_latch->load(std::memory_order_acquire))
				{
					if (selected.phase == sqlite_shm_registry_family_phase::active &&
						!selected.coordinator)
						quarantine_registry_locked();
					return false;
				}
				return true;
			}

			void emergency_quarantine() noexcept
			{
				emergency_quarantined_.store(true, std::memory_order_release);
				activity_emergency_latch_->store(true, std::memory_order_release);
			}

			void increment_audit_counter_locked(std::size_t& counter) noexcept
			{
				if (counter == std::numeric_limits<std::size_t>::max())
				{
					quarantine_registry_locked();
					return;
				}
				++counter;
			}

			[[nodiscard]] bool allocate_counter_locked(std::uint64_t& next,
													   std::uint64_t& output) noexcept
			{
				if (next == 0U || next == std::numeric_limits<std::uint64_t>::max())
				{
					quarantine_registry_locked();
					return false;
				}
				output = next;
				++next;
				return true;
			}

			[[nodiscard]] sqlite_shm_lease_rejection counter_exhaustion_rejection() const noexcept
			{
				return rejection(sqlite_shm_lease_rejection_reason::generation_exhausted,
								 sqlite_shm_lease_recovery_action::quarantine_no_retry);
			}

			[[nodiscard]] sqlite_shm_lease_rejection
			current_pin_rejection(const std::uint64_t process_epoch) const noexcept
			{
				if (!current(process_epoch))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token);
				if (admission_quarantined_locked())
					return rejection(sqlite_shm_lease_rejection_reason::quarantined,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				return rejection(sqlite_shm_lease_rejection_reason::stale_token);
			}

			[[nodiscard]] sqlite_shm_lease_rejection
			alias_pin_rejection_locked(const sqlite_shm_registry_alias_pin& pin) noexcept
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token);
				if (admission_quarantined_locked())
					return rejection(sqlite_shm_lease_rejection_reason::quarantined,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				const auto* alias = find_alias_locked(pin.token_);
				if (alias != nullptr &&
					alias->phase == sqlite_shm_registry_alias_phase::quarantined)
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				return rejection(sqlite_shm_lease_rejection_reason::stale_token);
			}

			[[nodiscard]] bool
			activity_control_matches_record_locked(const activity_record& activity) noexcept
			{
				if (!activity.control || activity.token != activity.control->activity_token ||
					activity.alias_token != activity.control->alias_token ||
					activity.family_epoch != activity.control->family_epoch ||
					activity.family_pin_token != activity.control->family_pin_token ||
					activity.control->process_epoch != process_epoch_ ||
					activity.control->process_instance != process_instance_ ||
					activity.control->emergency_latch.get() != activity_emergency_latch_.get())
					return false;
				const auto owner_state = activity.control->registry_state.lock();
				const auto owner_seal = activity.control->process_seal.lock();
				const auto* alias = find_alias_locked(activity.alias_token);
				const auto* family = find_family_epoch_locked(activity.family_epoch);
				const auto* family_pin = find_family_pin_locked(activity.family_pin_token);
				return owner_state.get() == this && owner_seal.get() == seal_.get() &&
					alias != nullptr && family != nullptr && family_pin != nullptr &&
					activity.control->alias_authority_latch.get() ==
					alias->activity_authority_latch.get() &&
					activity.control->family_authority_latch.get() ==
					family->activity_authority_latch.get() &&
					activity.control->alias_lifetime == alias->alias_lifetime &&
					activity.control->family == family->binding &&
					family_pin->alias_token == activity.alias_token &&
					family_pin->family_epoch == activity.family_epoch;
			}

			void
			invalidate_activity_authority_for_alias_locked(const std::uint64_t alias_token) noexcept
			{
				auto* alias = find_alias_locked(alias_token);
				if (alias != nullptr && alias->activity_authority_latch)
					alias->activity_authority_latch->store(false, std::memory_order_release);
				for (auto& activity : activities_)
					if (activity.active && activity.control && activity.alias_token == alias_token)
						activity.control->authority_valid.store(false, std::memory_order_release);
			}

			void invalidate_activity_authority_for_family_locked(
				const std::uint64_t family_epoch) noexcept
			{
				auto* family = find_family_epoch_locked(family_epoch);
				if (family != nullptr && family->activity_authority_latch)
					family->activity_authority_latch->store(false, std::memory_order_release);
				for (auto& activity : activities_)
					if (activity.active && activity.control &&
						activity.family_epoch == family_epoch)
						activity.control->authority_valid.store(false, std::memory_order_release);
			}

			void synchronize_activity_controls_locked() noexcept
			{
				for (auto& activity : activities_)
				{
					if (!activity.active)
						continue;
					if (!activity_control_matches_record_locked(activity))
					{
						if (activity.control)
							activity.control->authority_valid.store(false,
																	std::memory_order_release);
						quarantine_registry_locked();
						continue;
					}
					const auto phase = activity.control->phase.load(std::memory_order_acquire);
					if (phase == sqlite_shm_registry_activity_phase::active)
						continue;
					activity.control->authority_valid.store(false, std::memory_order_release);
					if (phase == sqlite_shm_registry_activity_phase::clean_released)
					{
						release_activity_locked(activity);
						continue;
					}

					auto* alias = find_alias_locked(activity.alias_token);
					auto* family = find_family_epoch_locked(activity.family_epoch);
					if (alias == nullptr || family == nullptr)
						quarantine_registry_locked();
					else
					{
						alias->phase = sqlite_shm_registry_alias_phase::quarantined;
						family->phase = sqlite_shm_registry_family_phase::quarantined;
						invalidate_activity_authority_for_alias_locked(alias->token);
						invalidate_activity_authority_for_family_locked(family->entry_epoch);
						propagate_local_quarantine_locked();
					}
					release_activity_locked(activity);
				}
			}

			void quarantine_registry_locked() noexcept
			{
				registry_quarantined_ = true;
				activity_emergency_latch_->store(true, std::memory_order_release);
				for (auto& alias : aliases_)
					if (alias.activity_authority_latch)
						alias.activity_authority_latch->store(false, std::memory_order_release);
				for (auto& family : families_)
					if (family.activity_authority_latch)
						family.activity_authority_latch->store(false, std::memory_order_release);
				for (auto& activity : activities_)
					if (activity.control)
						activity.control->authority_valid.store(false, std::memory_order_release);
				for (auto& alias : aliases_)
					if (alias.phase != sqlite_shm_registry_alias_phase::detached)
						alias.phase = sqlite_shm_registry_alias_phase::quarantined;
				for (auto& family : families_)
					if (family.phase == sqlite_shm_registry_family_phase::active)
						family.phase = sqlite_shm_registry_family_phase::quarantined;
			}

			void quarantine_alias_locked(alias_record& alias) noexcept
			{
				alias.phase = sqlite_shm_registry_alias_phase::quarantined;
				invalidate_activity_authority_for_alias_locked(alias.token);
				propagate_local_quarantine_locked();
			}

			void quarantine_family_locked(family_record& family) noexcept
			{
				family.phase = sqlite_shm_registry_family_phase::quarantined;
				invalidate_activity_authority_for_family_locked(family.entry_epoch);
				propagate_local_quarantine_locked();
			}

			void propagate_local_quarantine_locked() noexcept
			{
				bool changed{};
				do
				{
					changed = false;
					for (const auto& pin : family_pins_)
					{
						auto* family = find_family_epoch_locked(pin.family_epoch);
						auto* alias = find_alias_locked(pin.alias_token);
						if (family == nullptr || alias == nullptr ||
							alias->phase == sqlite_shm_registry_alias_phase::detached)
							continue;
						if (family->phase == sqlite_shm_registry_family_phase::quarantined &&
							alias->phase != sqlite_shm_registry_alias_phase::quarantined)
						{
							alias->phase = sqlite_shm_registry_alias_phase::quarantined;
							invalidate_activity_authority_for_alias_locked(alias->token);
							changed = true;
						}
						if (alias->phase == sqlite_shm_registry_alias_phase::quarantined &&
							family->phase == sqlite_shm_registry_family_phase::active)
						{
							family->phase = sqlite_shm_registry_family_phase::quarantined;
							invalidate_activity_authority_for_family_locked(family->entry_epoch);
							changed = true;
						}
					}
				} while (changed);
			}

			void synchronize_coordinator_quarantines_locked() noexcept
			{
				for (auto& family : families_)
				{
					if (family.phase != sqlite_shm_registry_family_phase::active)
						continue;
					if (!family.coordinator)
					{
						quarantine_registry_locked();
						return;
					}
					if (family.coordinator->snapshot().quarantined)
						quarantine_family_locked(family);
				}
			}

			[[nodiscard]] std::shared_ptr<sqlite_shm_registry_runtime_owner_box>
			detach_alias_locked(alias_record& alias) noexcept
			{
				auto owner = std::move(alias.runtime_lifetime.owner_box_);
				if (owner)
					owner->release_armed.store(true, std::memory_order_release);
				alias.runtime_lifetime.process_seal_.reset();
				alias.runtime_lifetime.process_epoch_ = 0U;
				alias.phase = sqlite_shm_registry_alias_phase::detached;
				return owner;
			}

			[[nodiscard]] alias_record* find_alias_locked(const std::uint64_t token) noexcept
			{
				const auto found = std::find_if(aliases_.begin(),
												aliases_.end(),
												[token](const auto& value)
												{
													return value.token == token;
												});
				return found == aliases_.end() ? nullptr : &*found;
			}

			[[nodiscard]] family_record*
			find_family_epoch_locked(const std::uint64_t epoch) noexcept
			{
				const auto found = std::find_if(families_.begin(),
												families_.end(),
												[epoch](const auto& value)
												{
													return value.entry_epoch == epoch;
												});
				return found == families_.end() ? nullptr : &*found;
			}

			[[nodiscard]] family_pin_record*
			find_family_pin_locked(const std::uint64_t token) noexcept
			{
				const auto found = std::find_if(family_pins_.begin(),
												family_pins_.end(),
												[token](const auto& value)
												{
													return value.token == token;
												});
				return found == family_pins_.end() ? nullptr : &*found;
			}

			[[nodiscard]] activity_record* find_activity_locked(const std::uint64_t token) noexcept
			{
				const auto found = std::find_if(activities_.begin(),
												activities_.end(),
												[token](const auto& value)
												{
													return value.token == token;
												});
				return found == activities_.end() ? nullptr : &*found;
			}

			[[nodiscard]] alias_record*
			current_alias_pin_locked(const sqlite_shm_registry_alias_pin& pin) noexcept
			{
				if (!current(pin.process_epoch_) || pin.state_.get() != this)
					return nullptr;
				synchronize_coordinator_quarantines_locked();
				if (admission_quarantined_locked())
					return nullptr;
				auto* alias = find_alias_locked(pin.token_);
				return alias != nullptr &&
						alias->phase != sqlite_shm_registry_alias_phase::detached &&
						alias->phase != sqlite_shm_registry_alias_phase::quarantined &&
						alias->activity_authority_latch &&
						alias->activity_authority_latch->load(std::memory_order_acquire)
					? alias
					: nullptr;
			}

			[[nodiscard]] family_pin_record*
			current_family_pin_locked(const sqlite_shm_registry_family_pin& pin) noexcept
			{
				if (!current(pin.process_epoch_) || pin.state_.get() != this)
					return nullptr;
				auto* found = find_family_pin_locked(pin.pin_token_);
				return found != nullptr && found->active && !found->abandoned &&
						found->alias_token == pin.alias_token_ &&
						found->family_epoch == pin.family_epoch_
					? found
					: nullptr;
			}

			[[nodiscard]] activity_record*
			current_activity_locked(const sqlite_shm_registry_activity_pin& pin) noexcept
			{
				if (!pin.control_ || !current(pin.control_->process_epoch))
					return nullptr;
				const auto owner_state = pin.state_.lock();
				if (!owner_state || owner_state.get() != this)
					return nullptr;
				auto* found = find_activity_locked(pin.control_->activity_token);
				return found != nullptr && found->active &&
						found->control.get() == pin.control_.get() &&
						found->alias_token == pin.control_->alias_token &&
						found->family_epoch == pin.control_->family_epoch &&
						found->family_pin_token == pin.control_->family_pin_token &&
						activity_control_matches_record_locked(*found)
					? found
					: nullptr;
			}

			[[nodiscard]] bool registration_receipt_matches_locked(
				const alias_record& alias,
				const sqlite_shm_verified_alias_registration_receipt& receipt) const noexcept
			{
				return valid_identity(receipt.registration_epoch()) &&
					receipt.process_instance() == alias.process_instance &&
					receipt.shared_runtime_vfs_cohort() == alias.shared_runtime_vfs_cohort &&
					receipt.alias_lifetime() == alias.alias_lifetime &&
					receipt.runtime_lifetime_identity() == alias.runtime_lifetime.identity() &&
					receipt.runtime_lifetime_pin_identity() ==
					alias.runtime_lifetime.pin_identity();
			}

			[[nodiscard]] bool unregistration_receipt_matches_locked(
				const alias_record& alias,
				const sqlite_shm_verified_alias_unregistration_receipt& receipt) const noexcept
			{
				return valid_identity(receipt.unregistration_epoch()) &&
					receipt.process_instance() == alias.process_instance &&
					receipt.shared_runtime_vfs_cohort() == alias.shared_runtime_vfs_cohort &&
					receipt.alias_lifetime() == alias.alias_lifetime &&
					receipt.runtime_lifetime_identity() == alias.runtime_lifetime.identity() &&
					receipt.runtime_lifetime_pin_identity() ==
					alias.runtime_lifetime.pin_identity() &&
					receipt.registration_epoch() == alias.registration_epoch;
			}

			[[nodiscard]] bool
			receipt_epoch_seen_locked(const sqlite_backend_opaque_identity& epoch) const noexcept
			{
				if (!valid_identity(epoch))
					return true;
				for (const auto& alias : aliases_)
					if (alias.registration_epoch == epoch || alias.unregistration_epoch == epoch)
						return true;
				return false;
			}

			[[nodiscard]] bool
			alias_coordinators_quiescent_locked(const alias_record& alias) const noexcept
			{
				for (const auto& pin : family_pins_)
				{
					if (pin.alias_token != alias.token)
						continue;
					const auto family =
						std::find_if(families_.begin(),
									 families_.end(),
									 [epoch = pin.family_epoch](const auto& candidate)
									 {
										 return candidate.entry_epoch == epoch;
									 });
					if (family == families_.end())
						return false;
					if (family->phase == sqlite_shm_registry_family_phase::retired)
					{
						if (family->coordinator)
							return false;
						continue;
					}
					if (family->phase != sqlite_shm_registry_family_phase::active ||
						!family->coordinator ||
						!coordinator_is_completely_quiescent(family->coordinator->snapshot()))
						return false;
				}
				return true;
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_registry_family_pin>
			pin_family(sqlite_shm_registry_alias_pin& alias_pin,
					   const sqlite_shm_lease_family_binding& binding,
					   const bool install)
			{
				if (!current(alias_pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token);
				if (!valid_family(binding))
					return rejection(sqlite_shm_lease_rejection_reason::invalid_identity);
				std::scoped_lock lock{mutex_};
				synchronize_activity_controls_locked();
				if (admission_quarantined_locked())
					return rejection(sqlite_shm_lease_rejection_reason::quarantined,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				auto* alias = current_alias_pin_locked(alias_pin);
				if (alias == nullptr)
					return alias_pin_rejection_locked(alias_pin);
				if (alias->phase != sqlite_shm_registry_alias_phase::registered)
					return rejection(sqlite_shm_lease_rejection_reason::retiring);
				if (binding.process_instance != alias->process_instance ||
					binding.shared_runtime_vfs_cohort != alias->shared_runtime_vfs_cohort)
					return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch);

				for (const auto& pin : family_pins_)
				{
					if (!pin.active || pin.alias_token != alias->token)
						continue;
					const auto* family = find_family_epoch_locked(pin.family_epoch);
					if (family != nullptr && family->binding == binding)
					{
						increment_audit_counter_locked(duplicate_rejection_count_);
						return rejection(sqlite_shm_lease_rejection_reason::stale_token);
					}
				}

				family_record* active = nullptr;
				std::size_t active_matches{};
				bool quarantined_match{};
				bool created_family{};
				for (auto& family : families_)
				{
					if (family.binding != binding)
						continue;
					if (family.phase == sqlite_shm_registry_family_phase::active)
					{
						++active_matches;
						active = &family;
					}
					else if (family.phase == sqlite_shm_registry_family_phase::quarantined)
						quarantined_match = true;
				}
				if (active_matches > 1U || (active_matches == 1U && quarantined_match))
				{
					increment_audit_counter_locked(ambiguous_lookup_count_);
					quarantine_registry_locked();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
				if (quarantined_match)
					return rejection(sqlite_shm_lease_rejection_reason::quarantined,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				if (active_matches == 0U)
				{
					if (!install)
						return rejection(sqlite_shm_lease_rejection_reason::no_live_generation);
					std::uint64_t epoch{};
					if (!allocate_counter_locked(next_family_epoch_, epoch))
						return counter_exhaustion_rejection();
					families_.emplace_back(
						epoch,
						binding,
						std::make_shared<sqlite_same_process_shm_mapping_lease_coordinator>(
							binding, generations_));
					active = &families_.back();
					created_family = true;
				}
				if (active == nullptr || !exact_family_admission_visible_locked(*active))
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);

				std::uint64_t pin_token{};
				if (!allocate_counter_locked(next_family_pin_token_, pin_token))
					return counter_exhaustion_rejection();
				try
				{
					family_pins_.push_back(
						{pin_token, alias->token, active->entry_epoch, 0U, true, false});
				}
				catch (...)
				{
					if (created_family)
						active->phase = sqlite_shm_registry_family_phase::quarantined;
					quarantine_registry_locked();
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
				++alias->active_family_pins;
				++active->active_family_pins;
				return sqlite_shm_registry_family_pin{
					shared_from_this(),
					{
						process_epoch_,
						alias->token,
						active->entry_epoch,
						pin_token,
					},
				};
			}

			void release_activity_locked(activity_record& activity) noexcept
			{
				if (!activity.active)
					return;
				activity.active = false;
				auto* alias = find_alias_locked(activity.alias_token);
				auto* family = find_family_epoch_locked(activity.family_epoch);
				auto* family_pin = find_family_pin_locked(activity.family_pin_token);
				if (alias == nullptr || family == nullptr || family_pin == nullptr ||
					alias->active_activities == 0U || family->active_activities == 0U ||
					family_pin->active_activities == 0U)
				{
					quarantine_registry_locked();
					return;
				}
				--alias->active_activities;
				--family->active_activities;
				--family_pin->active_activities;
				if (family_pin->active_activities == 0U && family_pin->abandoned)
				{
					(void)release_family_locked(*family_pin);
				}
			}

			[[nodiscard]] bool release_family_locked(family_pin_record& pin) noexcept
			{
				if (!pin.active)
					return false;
				pin.active = false;
				auto* alias = find_alias_locked(pin.alias_token);
				auto* family = find_family_epoch_locked(pin.family_epoch);
				if (alias == nullptr || family == nullptr || alias->active_family_pins == 0U ||
					family->active_family_pins == 0U)
				{
					quarantine_registry_locked();
					return false;
				}
				--alias->active_family_pins;
				--family->active_family_pins;
				if (family->active_family_pins != 0U)
					return family->phase == sqlite_shm_registry_family_phase::active;
				if (family->active_activities != 0U)
				{
					quarantine_registry_locked();
					return false;
				}
				if (!exact_family_admission_visible_locked(*family))
					return false;
				const auto coordinator_snapshot = family->coordinator->snapshot();
				if (!coordinator_is_completely_quiescent(coordinator_snapshot))
				{
					quarantine_family_locked(*family);
					return false;
				}
				family->phase = sqlite_shm_registry_family_phase::retired;
				family->coordinator.reset();
				return true;
			}

			sqlite_backend_opaque_identity process_instance_;
			std::shared_ptr<sqlite_shm_registry_process_owner_seal> seal_;
			std::uint64_t process_epoch_{};
			std::shared_ptr<sqlite_shm_mapping_generation_source> generations_;
			std::shared_ptr<std::atomic_bool> activity_emergency_latch_{
				std::make_shared<std::atomic_bool>(false)};
			std::mutex mutex_;
			std::vector<alias_record> aliases_;
			std::vector<family_record> families_;
			std::vector<family_pin_record> family_pins_;
			std::vector<activity_record> activities_;
			std::uint64_t next_alias_token_{1U};
			std::uint64_t next_family_epoch_{1U};
			std::uint64_t next_family_pin_token_{1U};
			std::uint64_t next_activity_token_{1U};
			std::size_t duplicate_rejection_count_{};
			std::size_t cross_binding_rejection_count_{};
			std::size_t ambiguous_lookup_count_{};
			bool registry_quarantined_{};
			std::atomic_bool emergency_quarantined_{false};
		};
	} // namespace detail

	sqlite_shm_writer_member_authority::sqlite_shm_writer_member_authority(
		std::unique_ptr<detail::sqlite_shm_writer_member_authority_state> state) noexcept
		: state_{std::move(state)}
	{
	}

	sqlite_shm_writer_member_authority::~sqlite_shm_writer_member_authority() noexcept = default;

	sqlite_shm_writer_member_authority::sqlite_shm_writer_member_authority(
		sqlite_shm_writer_member_authority&& other) noexcept
		: state_{std::move(other.state_)}
	{
	}

	bool sqlite_shm_writer_member_authority::valid_for_predelegation(
		const sqlite_shm_writer_map_request& request) const noexcept
	{
		return state_ && state_->activity && state_->audit_seal && state_->epoch_arm &&
			state_->activity->valid() && state_->audit_seal->valid() &&
			state_->epoch_arm->valid_for_predelegation(request);
	}

	bool sqlite_shm_writer_member_authority::retains_exact_lifetimes(
		const sqlite_shm_writer_map_request& request) const noexcept
	{
		return state_ && state_->activity && state_->audit_seal && state_->epoch_arm &&
			state_->activity->valid() && state_->audit_seal->valid() &&
			state_->epoch_arm->retains_exact_lifetimes(request);
	}

	bool sqlite_shm_writer_member_authority::attachment_cohort_compatible_with(
		const sqlite_shm_writer_member_authority& other) const noexcept
	{
		return state_ && state_->epoch_arm && other.state_ && other.state_->epoch_arm &&
			state_->epoch_arm->attachment_cohort_compatible_with(*other.state_->epoch_arm);
	}

	void sqlite_shm_writer_member_authority::invalidate_epoch_for_testing() noexcept
	{
		if (state_ && state_->epoch_arm)
			state_->epoch_arm->invalidate_for_testing();
	}

	sqlite_shm_lease_result<void> sqlite_shm_writer_member_authority::release_activity() noexcept
	{
		if (!state_ || !state_->activity)
			return rejection(sqlite_shm_lease_rejection_reason::stale_token);
		const auto registry = state_->activity->state_.lock();
		if (!registry)
			return rejection(sqlite_shm_lease_rejection_reason::stale_token);
		auto released = registry->release_activity(*state_->activity);
		if (released)
		{
			// release_activity disarms the activity outside its mutex. Destroy the weak seal and
			// strong epoch arm only after that clean registry transition has completed.
			state_.reset();
		}
		return released;
	}

	sqlite_shm_registry_process_owner::sqlite_shm_registry_process_owner(
		sqlite_backend_opaque_identity process_instance)
		: process_instance_{std::move(process_instance)},
		  seal_{std::make_shared<detail::sqlite_shm_registry_process_owner_seal>()},
		  process_epoch_{seal_->process_epoch.load(std::memory_order_acquire)}
	{
	}

	sqlite_shm_registry_process_owner::sqlite_shm_registry_process_owner(
		sqlite_backend_opaque_identity process_instance,
		std::shared_ptr<detail::sqlite_shm_registry_process_owner_seal> seal,
		const std::uint64_t process_epoch) noexcept
		: process_instance_{std::move(process_instance)}, seal_{std::move(seal)},
		  process_epoch_{process_epoch}
	{
	}

	sqlite_shm_registry_process_owner::~sqlite_shm_registry_process_owner() noexcept = default;

	sqlite_shm_registry_process_owner::sqlite_shm_registry_process_owner(
		sqlite_shm_registry_process_owner&& other) noexcept
		: process_instance_{std::move(other.process_instance_)}, seal_{std::move(other.seal_)},
		  process_epoch_{std::exchange(other.process_epoch_, 0U)}
	{
	}

	bool sqlite_shm_registry_process_owner::valid() const noexcept
	{
		return valid_identity(process_instance_) && seal_ && process_epoch_ != 0U &&
			seal_->process_epoch.load(std::memory_order_acquire) == process_epoch_ &&
			!seal_->claimed.load(std::memory_order_acquire);
	}

	sqlite_shm_registry_runtime_lifetime_pin::sqlite_shm_registry_runtime_lifetime_pin(
		sqlite_backend_opaque_identity process_instance,
		std::shared_ptr<detail::sqlite_shm_registry_process_owner_seal> process_seal,
		const std::uint64_t process_epoch,
		sqlite_backend_opaque_identity identity,
		sqlite_backend_opaque_identity pin_identity,
		std::weak_ptr<void> owner_control,
		std::shared_ptr<detail::sqlite_shm_registry_runtime_owner_box> owner_box)
		: process_instance_{std::move(process_instance)}, identity_{std::move(identity)},
		  pin_identity_{std::move(pin_identity)}, process_seal_{std::move(process_seal)},
		  process_epoch_{process_epoch}, owner_control_{std::move(owner_control)},
		  owner_box_{std::move(owner_box)}
	{
	}

	sqlite_shm_registry_runtime_lifetime_pin::~sqlite_shm_registry_runtime_lifetime_pin() noexcept =
		default;

	sqlite_shm_registry_runtime_lifetime_pin::sqlite_shm_registry_runtime_lifetime_pin(
		sqlite_shm_registry_runtime_lifetime_pin&& other) noexcept
		: process_instance_{std::move(other.process_instance_)},
		  identity_{std::move(other.identity_)}, pin_identity_{std::move(other.pin_identity_)},
		  process_seal_{std::move(other.process_seal_)},
		  process_epoch_{std::exchange(other.process_epoch_, 0U)},
		  owner_control_{std::move(other.owner_control_)}, owner_box_{std::move(other.owner_box_)}
	{
	}

	bool sqlite_shm_registry_runtime_lifetime_pin::valid() const noexcept
	{
		return valid_identity(process_instance_) && valid_identity(identity_) &&
			valid_identity(pin_identity_) && process_seal_ && process_epoch_ != 0U &&
			process_seal_->process_epoch.load(std::memory_order_acquire) == process_epoch_ &&
			!owner_control_.expired() && owner_box_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_registry_runtime_lifetime_pin::identity() const noexcept
	{
		return identity_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_registry_runtime_lifetime_pin::pin_identity() const noexcept
	{
		return pin_identity_;
	}

	sqlite_shm_registry_alias_binding::sqlite_shm_registry_alias_binding(
		sqlite_backend_opaque_identity process_instance,
		sqlite_backend_opaque_identity shared_runtime_vfs_cohort,
		sqlite_backend_opaque_identity alias_lifetime,
		sqlite_shm_registry_runtime_lifetime_pin runtime_lifetime)
		: process_instance_{std::move(process_instance)},
		  shared_runtime_vfs_cohort_{std::move(shared_runtime_vfs_cohort)},
		  alias_lifetime_{std::move(alias_lifetime)}, runtime_lifetime_{std::move(runtime_lifetime)}
	{
	}

	sqlite_shm_registry_alias_binding::~sqlite_shm_registry_alias_binding() noexcept = default;

	sqlite_shm_registry_alias_binding::sqlite_shm_registry_alias_binding(
		sqlite_shm_registry_alias_binding&& other) noexcept
		: process_instance_{std::move(other.process_instance_)},
		  shared_runtime_vfs_cohort_{std::move(other.shared_runtime_vfs_cohort_)},
		  alias_lifetime_{std::move(other.alias_lifetime_)},
		  runtime_lifetime_{std::move(other.runtime_lifetime_)}
	{
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_registry_alias_binding::process_instance() const noexcept
	{
		return process_instance_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_registry_alias_binding::shared_runtime_vfs_cohort() const noexcept
	{
		return shared_runtime_vfs_cohort_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_registry_alias_binding::alias_lifetime() const noexcept
	{
		return alias_lifetime_;
	}

	const sqlite_shm_registry_runtime_lifetime_pin&
	sqlite_shm_registry_alias_binding::runtime_lifetime() const noexcept
	{
		return runtime_lifetime_;
	}

	sqlite_shm_verified_alias_registration_receipt::sqlite_shm_verified_alias_registration_receipt(
		sqlite_backend_opaque_identity process_instance,
		sqlite_backend_opaque_identity shared_runtime_vfs_cohort,
		sqlite_backend_opaque_identity alias_lifetime,
		sqlite_backend_opaque_identity runtime_lifetime_identity,
		sqlite_backend_opaque_identity runtime_lifetime_pin_identity,
		sqlite_backend_opaque_identity registration_epoch)
		: process_instance_{std::move(process_instance)},
		  shared_runtime_vfs_cohort_{std::move(shared_runtime_vfs_cohort)},
		  alias_lifetime_{std::move(alias_lifetime)},
		  runtime_lifetime_identity_{std::move(runtime_lifetime_identity)},
		  runtime_lifetime_pin_identity_{std::move(runtime_lifetime_pin_identity)},
		  registration_epoch_{std::move(registration_epoch)}
	{
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_verified_alias_registration_receipt::process_instance() const noexcept
	{
		return process_instance_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_verified_alias_registration_receipt::shared_runtime_vfs_cohort() const noexcept
	{
		return shared_runtime_vfs_cohort_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_verified_alias_registration_receipt::alias_lifetime() const noexcept
	{
		return alias_lifetime_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_verified_alias_registration_receipt::runtime_lifetime_identity() const noexcept
	{
		return runtime_lifetime_identity_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_verified_alias_registration_receipt::runtime_lifetime_pin_identity() const noexcept
	{
		return runtime_lifetime_pin_identity_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_verified_alias_registration_receipt::registration_epoch() const noexcept
	{
		return registration_epoch_;
	}

	sqlite_shm_verified_alias_unregistration_receipt::
		sqlite_shm_verified_alias_unregistration_receipt(
			sqlite_backend_opaque_identity process_instance,
			sqlite_backend_opaque_identity shared_runtime_vfs_cohort,
			sqlite_backend_opaque_identity alias_lifetime,
			sqlite_backend_opaque_identity runtime_lifetime_identity,
			sqlite_backend_opaque_identity runtime_lifetime_pin_identity,
			sqlite_backend_opaque_identity registration_epoch,
			sqlite_backend_opaque_identity unregistration_epoch)
		: process_instance_{std::move(process_instance)},
		  shared_runtime_vfs_cohort_{std::move(shared_runtime_vfs_cohort)},
		  alias_lifetime_{std::move(alias_lifetime)},
		  runtime_lifetime_identity_{std::move(runtime_lifetime_identity)},
		  runtime_lifetime_pin_identity_{std::move(runtime_lifetime_pin_identity)},
		  registration_epoch_{std::move(registration_epoch)},
		  unregistration_epoch_{std::move(unregistration_epoch)}
	{
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_verified_alias_unregistration_receipt::process_instance() const noexcept
	{
		return process_instance_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_verified_alias_unregistration_receipt::shared_runtime_vfs_cohort() const noexcept
	{
		return shared_runtime_vfs_cohort_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_verified_alias_unregistration_receipt::alias_lifetime() const noexcept
	{
		return alias_lifetime_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_verified_alias_unregistration_receipt::runtime_lifetime_identity() const noexcept
	{
		return runtime_lifetime_identity_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_verified_alias_unregistration_receipt::runtime_lifetime_pin_identity() const noexcept
	{
		return runtime_lifetime_pin_identity_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_verified_alias_unregistration_receipt::registration_epoch() const noexcept
	{
		return registration_epoch_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_verified_alias_unregistration_receipt::unregistration_epoch() const noexcept
	{
		return unregistration_epoch_;
	}

	sqlite_shm_registry_alias_pin::sqlite_shm_registry_alias_pin(
		std::shared_ptr<detail::sqlite_shm_mapping_registry_state> state,
		const coordinates value) noexcept
		: state_{std::move(state)}, process_epoch_{value.process_epoch}, token_{value.token}
	{
	}

	sqlite_shm_registry_alias_pin::~sqlite_shm_registry_alias_pin() noexcept
	{
		if (state_)
			state_->abandon_alias(*this);
	}

	sqlite_shm_registry_alias_pin::sqlite_shm_registry_alias_pin(
		sqlite_shm_registry_alias_pin&& other) noexcept
		: state_{std::move(other.state_)}, process_epoch_{std::exchange(other.process_epoch_, 0U)},
		  token_{std::exchange(other.token_, 0U)}
	{
	}

	bool sqlite_shm_registry_alias_pin::valid() const noexcept
	{
		return state_ && token_ != 0U && state_->alias_pin_valid(*this);
	}

	void sqlite_shm_registry_alias_pin::disarm() noexcept
	{
		state_.reset();
		process_epoch_ = 0U;
		token_ = 0U;
	}

	sqlite_shm_registry_family_pin::sqlite_shm_registry_family_pin(
		std::shared_ptr<detail::sqlite_shm_mapping_registry_state> state,
		const coordinates value) noexcept
		: state_{std::move(state)}, process_epoch_{value.process_epoch},
		  alias_token_{value.alias_token}, family_epoch_{value.family_epoch},
		  pin_token_{value.pin_token}
	{
	}

	sqlite_shm_registry_family_pin::~sqlite_shm_registry_family_pin() noexcept
	{
		if (state_)
			state_->abandon_family(*this);
	}

	sqlite_shm_registry_family_pin::sqlite_shm_registry_family_pin(
		sqlite_shm_registry_family_pin&& other) noexcept
		: state_{std::move(other.state_)}, process_epoch_{std::exchange(other.process_epoch_, 0U)},
		  alias_token_{std::exchange(other.alias_token_, 0U)},
		  family_epoch_{std::exchange(other.family_epoch_, 0U)},
		  pin_token_{std::exchange(other.pin_token_, 0U)}
	{
	}

	bool sqlite_shm_registry_family_pin::valid() const noexcept
	{
		return state_ && pin_token_ != 0U && state_->family_pin_valid(*this);
	}

	void sqlite_shm_registry_family_pin::disarm() noexcept
	{
		state_.reset();
		process_epoch_ = 0U;
		alias_token_ = 0U;
		family_epoch_ = 0U;
		pin_token_ = 0U;
	}

	sqlite_shm_registry_activity_seal::sqlite_shm_registry_activity_seal(
		std::weak_ptr<detail::sqlite_shm_registry_activity_control> control) noexcept
		: control_{std::move(control)}
	{
	}

	bool sqlite_shm_registry_activity_seal::valid() const noexcept
	{
		const auto control = control_.lock();
		return control && control->audit_seal_minted.load(std::memory_order_acquire) &&
			control->authority_valid_now();
	}

	sqlite_shm_registry_activity_pin::sqlite_shm_registry_activity_pin(
		std::weak_ptr<detail::sqlite_shm_mapping_registry_state> state,
		std::shared_ptr<detail::sqlite_shm_registry_activity_control> control) noexcept
		: state_{std::move(state)}, control_{std::move(control)}
	{
	}

	sqlite_shm_registry_activity_pin::~sqlite_shm_registry_activity_pin() noexcept
	{
		if (!control_)
			return;
		auto expected = detail::sqlite_shm_registry_activity_phase::active;
		if (control_->phase.compare_exchange_strong(
				expected,
				detail::sqlite_shm_registry_activity_phase::abandoned,
				std::memory_order_acq_rel,
				std::memory_order_acquire))
		{
			control_->authority_valid.store(false, std::memory_order_release);
			// Alias invalidation is deliberately conservative and may precede the exact family
			// boundary. The family-latch release below is the no-lock abandonment linearization
			// point: after it, every same-alias or same-family sibling is immediately invalid.
			if (control_->alias_authority_latch)
				control_->alias_authority_latch->store(false, std::memory_order_release);
			if (control_->family_authority_latch)
				control_->family_authority_latch->store(false, std::memory_order_release);
		}
	}

	sqlite_shm_registry_activity_pin::sqlite_shm_registry_activity_pin(
		sqlite_shm_registry_activity_pin&& other) noexcept
		: state_{std::move(other.state_)}, control_{std::move(other.control_)}
	{
	}

	bool sqlite_shm_registry_activity_pin::valid() const noexcept
	{
		return control_ && control_->authority_valid_now();
	}

	sqlite_shm_lease_result<sqlite_shm_registry_activity_seal>
	sqlite_shm_registry_activity_pin::seal_for_audit() noexcept
	{
		if (!control_ || !control_->authority_valid_now())
			return rejection(sqlite_shm_lease_rejection_reason::stale_token);
		bool expected{};
		if (!control_->audit_seal_minted.compare_exchange_strong(
				expected, true, std::memory_order_acq_rel, std::memory_order_acquire))
			return rejection(sqlite_shm_lease_rejection_reason::stale_token);
		if (!control_->authority_valid_now())
			return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
							 sqlite_shm_lease_recovery_action::quarantine_no_retry);
		return sqlite_shm_registry_activity_seal{
			std::weak_ptr<detail::sqlite_shm_registry_activity_control>{control_}};
	}

	void sqlite_shm_registry_activity_pin::disarm() noexcept
	{
		state_.reset();
		control_.reset();
	}

	sqlite_same_process_shm_mapping_registry::sqlite_same_process_shm_mapping_registry(
		std::shared_ptr<detail::sqlite_shm_mapping_registry_state> state) noexcept
		: state_{std::move(state)}
	{
	}

	sqlite_same_process_shm_mapping_registry::~sqlite_same_process_shm_mapping_registry() noexcept =
		default;

	sqlite_shm_lease_result<std::unique_ptr<sqlite_same_process_shm_mapping_registry>>
	sqlite_same_process_shm_mapping_registry::create(sqlite_shm_registry_process_owner owner)
	{
		return create_with_generation_for_testing(std::move(owner), 1U);
	}

	sqlite_shm_lease_result<std::unique_ptr<sqlite_same_process_shm_mapping_registry>>
	sqlite_same_process_shm_mapping_registry::create_with_generation_for_testing(
		sqlite_shm_registry_process_owner owner, const std::uint64_t first_mapping_generation)
	{
		if (!valid_identity(owner.process_instance_) || !owner.seal_ ||
			first_mapping_generation == 0U)
			return rejection(sqlite_shm_lease_rejection_reason::invalid_identity);
		const auto process_epoch = owner.process_epoch_;
		if (process_epoch == 0U ||
			owner.seal_->process_epoch.load(std::memory_order_acquire) != process_epoch)
			return rejection(sqlite_shm_lease_rejection_reason::stale_token);
		bool expected = false;
		if (!owner.seal_->claimed.compare_exchange_strong(
				expected, true, std::memory_order_acq_rel, std::memory_order_acquire))
			return rejection(sqlite_shm_lease_rejection_reason::stale_token);
		if (owner.seal_->process_epoch.load(std::memory_order_acquire) != process_epoch)
			return rejection(sqlite_shm_lease_rejection_reason::stale_token);

		auto state =
			detail::sqlite_shm_mapping_registry_state::create(std::move(owner.process_instance_),
															  std::move(owner.seal_),
															  process_epoch,
															  first_mapping_generation);
		return std::unique_ptr<sqlite_same_process_shm_mapping_registry>{
			new sqlite_same_process_shm_mapping_registry{std::move(state)}};
	}

	sqlite_shm_lease_result<sqlite_shm_registry_alias_pin>
	sqlite_same_process_shm_mapping_registry::reserve_alias(
		sqlite_shm_registry_alias_binding binding)
	{
		return state_->reserve_alias(std::move(binding));
	}

	sqlite_shm_lease_result<void> sqlite_same_process_shm_mapping_registry::begin_alias_register(
		sqlite_shm_registry_alias_pin& alias) noexcept
	{
		return state_->begin_alias_register(alias);
	}

	sqlite_shm_lease_result<void>
	sqlite_same_process_shm_mapping_registry::confirm_alias_registered(
		sqlite_shm_registry_alias_pin& alias,
		const sqlite_shm_verified_alias_registration_receipt& receipt) noexcept
	{
		return state_->confirm_alias_registered(alias, receipt);
	}

	sqlite_shm_lease_result<void>
	sqlite_same_process_shm_mapping_registry::cancel_unregistered_alias(
		sqlite_shm_registry_alias_pin& alias) noexcept
	{
		return state_->cancel_unregistered_alias(alias);
	}

	sqlite_shm_lease_result<sqlite_shm_registry_family_pin>
	sqlite_same_process_shm_mapping_registry::install_or_join_family(
		sqlite_shm_registry_alias_pin& alias, const sqlite_shm_lease_family_binding& family)
	{
		return state_->install_or_join_family(alias, family);
	}

	sqlite_shm_lease_result<sqlite_shm_registry_family_pin>
	sqlite_same_process_shm_mapping_registry::pin_existing_family(
		sqlite_shm_registry_alias_pin& alias, const sqlite_shm_lease_family_binding& family)
	{
		return state_->pin_existing_family(alias, family);
	}

	sqlite_shm_lease_result<sqlite_shm_registry_activity_pin>
	sqlite_same_process_shm_mapping_registry::acquire_activity(
		sqlite_shm_registry_family_pin& family)
	{
		return state_->acquire_activity(family);
	}

	sqlite_shm_lease_result<sqlite_shm_writer_map_inflight>
	sqlite_same_process_shm_mapping_registry::begin_writer_map(
		sqlite_shm_registry_family_pin& family,
		sqlite_writer_shm_mapping_epoch_arm arm,
		const sqlite_shm_writer_map_request& request)
	{
		return state_->begin_writer_map(family, std::move(arm), request);
	}

	sqlite_shm_lease_result<void> sqlite_same_process_shm_mapping_registry::release_activity(
		sqlite_shm_registry_activity_pin& activity) noexcept
	{
		return state_->release_activity(activity);
	}

	sqlite_shm_lease_result<void> sqlite_same_process_shm_mapping_registry::release_family(
		sqlite_shm_registry_family_pin& family) noexcept
	{
		return state_->release_family(family);
	}

	sqlite_shm_lease_result<void> sqlite_same_process_shm_mapping_registry::begin_alias_unregister(
		sqlite_shm_registry_alias_pin& alias) noexcept
	{
		return state_->begin_alias_unregister(alias);
	}

	sqlite_shm_lease_result<void> sqlite_same_process_shm_mapping_registry::poll_alias_unregister(
		sqlite_shm_registry_alias_pin& alias) noexcept
	{
		return state_->poll_alias_unregister(alias);
	}

	sqlite_shm_lease_result<void>
	sqlite_same_process_shm_mapping_registry::confirm_alias_unregistered(
		sqlite_shm_registry_alias_pin& alias,
		const sqlite_shm_verified_alias_unregistration_receipt& receipt) noexcept
	{
		return state_->confirm_alias_unregistered(alias, receipt);
	}

	sqlite_shm_mapping_registry_snapshot
	sqlite_same_process_shm_mapping_registry::snapshot() const noexcept
	{
		return state_ ? state_->snapshot() : sqlite_shm_mapping_registry_snapshot{};
	}

	sqlite_shm_mapping_registry_family_snapshot
	sqlite_same_process_shm_mapping_registry::family_snapshot(
		const sqlite_shm_lease_family_binding& family) const noexcept
	{
		return state_ ? state_->family_snapshot(family)
					  : sqlite_shm_mapping_registry_family_snapshot{};
	}

	void
	sqlite_same_process_shm_mapping_registry::invalidate_process_instance_for_testing() noexcept
	{
		if (state_)
			state_->invalidate_process_instance();
	}

	void sqlite_same_process_shm_mapping_registry::lock_registry_mutex_for_fork_testing()
	{
		state_->lock_mutex_for_fork_testing();
	}

	void sqlite_same_process_shm_mapping_registry::unlock_registry_mutex_for_fork_testing() noexcept
	{
		state_->unlock_mutex_for_fork_testing();
	}

	bool sqlite_same_process_shm_mapping_registry::inject_duplicate_family_for_testing(
		const sqlite_shm_lease_family_binding& family) noexcept
	{
		return state_ && state_->inject_duplicate_family(family);
	}

	void sqlite_same_process_shm_mapping_registry::exhaust_registry_counters_for_testing() noexcept
	{
		if (state_)
			state_->exhaust_counters();
	}

	void sqlite_same_process_shm_mapping_registry::exhaust_registry_counter_for_testing(
		const detail::sqlite_shm_registry_counter_for_testing counter) noexcept
	{
		if (state_)
			state_->exhaust_counter(counter);
	}

	std::uint64_t
	sqlite_same_process_shm_mapping_registry::state_destruction_count_for_testing() noexcept
	{
		return registry_state_destruction_count.load(std::memory_order_relaxed);
	}

	sqlite_shm_lease_result<sqlite_shm_registry_runtime_lifetime_pin>
	sqlite_same_process_shm_mapping_registry::adopt_runtime_lifetime_for_testing(
		sqlite_backend_opaque_identity identity,
		sqlite_backend_opaque_identity pin_identity,
		std::shared_ptr<void> owner)
	{
		return state_->adopt_runtime_lifetime(
			std::move(identity), std::move(pin_identity), std::move(owner));
	}

	sqlite_same_process_shm_mapping_lease_coordinator*
	sqlite_same_process_shm_mapping_registry::coordinator_for_activity_for_testing(
		const sqlite_shm_registry_activity_pin& activity) const noexcept
	{
		return state_ ? state_->coordinator_for_activity(activity) : nullptr;
	}

	sqlite_same_process_shm_mapping_lease_coordinator*
	sqlite_same_process_shm_mapping_registry::coordinator_for_family_for_testing(
		const sqlite_shm_lease_family_binding& family) const noexcept
	{
		return state_ ? state_->coordinator_for_family(family) : nullptr;
	}

	bool sqlite_same_process_shm_mapping_registry::activity_seal_matches_for_testing(
		const sqlite_shm_registry_activity_seal& audit_seal,
		const sqlite_backend_opaque_identity& process_instance,
		const sqlite_shm_lease_family_binding& family,
		const sqlite_backend_opaque_identity& alias_lifetime) noexcept
	{
		return state_ &&
			state_->activity_seal_matches(audit_seal, process_instance, family, alias_lifetime);
	}

	const void* sqlite_same_process_shm_mapping_registry::generation_source_identity_for_testing()
		const noexcept
	{
		return state_ ? state_->generation_source_identity() : nullptr;
	}

	void sqlite_same_process_shm_mapping_registry::lock_state_mutex_for_testing()
	{
		if (state_)
			state_->lock_mutex_for_testing();
	}

	void sqlite_same_process_shm_mapping_registry::unlock_state_mutex_for_testing()
	{
		if (state_)
			state_->unlock_mutex_for_testing();
	}
} // namespace cxxlens::sdk
