#include "sqlite_writer_shm_mapping_epoch_internal.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

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

		[[nodiscard]] bool
		valid_native_attachment(const sqlite_shm_native_attachment_identity& attachment) noexcept
		{
			return valid_family(attachment.family()) &&
				valid_identity(attachment.alias_lifetime()) &&
				valid_identity(attachment.connection_token()) &&
				valid_identity(attachment.main_native_file_receipt()) &&
				valid_identity(attachment.main_xopen_receipt()) &&
				valid_identity(attachment.open_epoch()) &&
				valid_identity(attachment.callback_cohort()) &&
				valid_identity(attachment.attachment_epoch());
		}

		[[nodiscard]] bool
		valid_callback(const sqlite_shm_callback_execution_receipt& callback) noexcept
		{
			return valid_identity(callback.thread_identity) &&
				valid_identity(callback.invocation_token);
		}

		[[nodiscard]] bool checked_mapping_range(const int page_number,
												 const int page_size) noexcept
		{
			if (page_number < 0 || page_size <= 0)
				return false;
			const auto page = static_cast<std::uint64_t>(page_number);
			const auto size = static_cast<std::uint64_t>(page_size);
			const auto maximum = std::numeric_limits<std::uint64_t>::max();
			if (page > maximum / size)
				return false;
			const auto offset = page * size;
			return offset <= maximum - size;
		}

		[[nodiscard]] bool
		valid_writer_request(const sqlite_shm_writer_map_request& request) noexcept
		{
			return valid_family(request.family) && valid_identity(request.alias_lifetime) &&
				valid_identity(request.connection_token) &&
				valid_native_attachment(request.attachment) &&
				request.attachment.family() == request.family &&
				request.attachment.alias_lifetime() == request.alias_lifetime &&
				request.attachment.connection_token() == request.connection_token &&
				valid_callback(request.callback) &&
				checked_mapping_range(request.page_number, request.page_size) &&
				(request.caller_extend == 0 || request.caller_extend == 1);
		}

		[[nodiscard]] bool valid_stat_census(const sqlite_writer_shm_stat_census& census) noexcept
		{
			if (!valid_identity(census.parent_namespace_identity) ||
				!valid_identity(census.filesystem_profile) ||
				!valid_identity(census.mount_identity))
				return false;
			if (census.state == sqlite_writer_shm_entry_state::absent)
				return !census.object_identity && !census.directory_entry_identity &&
					census.byte_count == 0U;
			return census.state == sqlite_writer_shm_entry_state::direct_regular &&
				census.object_identity && valid_identity(*census.object_identity) &&
				census.directory_entry_identity && valid_identity(*census.directory_entry_identity);
		}

		[[nodiscard]] bool valid_pin(
			const sqlite_writer_shm_native_lifetime_pin& pin,
			const sqlite_writer_shm_native_lifetime_role role,
			const sqlite_backend_opaque_identity& semantic_receipt,
			const std::optional<sqlite_backend_opaque_identity>& native_xopen_receipt) noexcept
		{
			return pin.valid() && valid_identity(pin.native_lifetime_identity()) &&
				pin.role() == role && pin.semantic_receipt() == semantic_receipt &&
				pin.native_xopen_receipt() == native_xopen_receipt;
		}

		[[nodiscard]] bool
		valid_epoch_request(const sqlite_writer_shm_mapping_epoch_request& request) noexcept
		{
			const auto& binding = request.binding;
			const auto& map = binding.map_request;
			const auto distinct_pin_identities =
				request.retained_parent.pin_identity() != request.main_native_file.pin_identity() &&
				request.retained_parent.pin_identity() != request.wal_native_file.pin_identity() &&
				request.retained_parent.pin_identity() !=
					request.shm_native_attachment.pin_identity() &&
				request.main_native_file.pin_identity() != request.wal_native_file.pin_identity() &&
				request.main_native_file.pin_identity() !=
					request.shm_native_attachment.pin_identity() &&
				request.wal_native_file.pin_identity() !=
					request.shm_native_attachment.pin_identity();
			const auto distinct_native_lifetimes =
				request.retained_parent.native_lifetime_identity() !=
					request.main_native_file.native_lifetime_identity() &&
				request.retained_parent.native_lifetime_identity() !=
					request.wal_native_file.native_lifetime_identity() &&
				request.retained_parent.native_lifetime_identity() !=
					request.shm_native_attachment.native_lifetime_identity() &&
				request.main_native_file.native_lifetime_identity() !=
					request.wal_native_file.native_lifetime_identity() &&
				request.main_native_file.native_lifetime_identity() !=
					request.shm_native_attachment.native_lifetime_identity() &&
				request.wal_native_file.native_lifetime_identity() !=
					request.shm_native_attachment.native_lifetime_identity();
			return valid_writer_request(map) &&
				classify_sqlite_shm_writer_extend_pair(map.caller_extend, binding.delegated_extend)
					.has_value() &&
				valid_identity(binding.expected_shm_leaf) &&
				valid_identity(binding.retained_parent_receipt) &&
				valid_identity(binding.wal_native_file_receipt) &&
				valid_identity(binding.wal_xopen_receipt) &&
				valid_identity(binding.shm_native_attachment_receipt) &&
				valid_pin(request.retained_parent,
						  sqlite_writer_shm_native_lifetime_role::retained_parent,
						  binding.retained_parent_receipt,
						  std::nullopt) &&
				valid_pin(request.main_native_file,
						  sqlite_writer_shm_native_lifetime_role::main_database,
						  map.attachment.main_native_file_receipt(),
						  map.attachment.main_xopen_receipt()) &&
				valid_pin(request.wal_native_file,
						  sqlite_writer_shm_native_lifetime_role::write_ahead_log,
						  binding.wal_native_file_receipt,
						  binding.wal_xopen_receipt) &&
				valid_pin(request.shm_native_attachment,
						  sqlite_writer_shm_native_lifetime_role::shared_memory_attachment,
						  binding.shm_native_attachment_receipt,
						  std::nullopt) &&
				distinct_pin_identities && distinct_native_lifetimes;
		}

		[[nodiscard]] sqlite_shm_lease_rejection
		rejection(const sqlite_shm_lease_rejection_reason reason,
				  const sqlite_shm_lease_recovery_action action) noexcept
		{
			return {reason, action};
		}
	} // namespace

	namespace detail
	{
		class sqlite_writer_shm_lifetime_spin_mutex final
		{
		  public:
			void lock() noexcept
			{
				while (locked_.test_and_set(std::memory_order_acquire))
					locked_.wait(true, std::memory_order_relaxed);
			}

			void unlock() noexcept
			{
				locked_.clear(std::memory_order_release);
				locked_.notify_one();
			}

		  private:
			std::atomic_flag locked_ = ATOMIC_FLAG_INIT;
		};

		struct sqlite_writer_shm_mapping_epoch_liveness
		{
			std::atomic_bool live{true};
		};

		struct sqlite_writer_shm_native_lifetime_control
		{
			sqlite_writer_shm_lifetime_spin_mutex mutex;
			sqlite_writer_shm_native_lifetime_role role{
				sqlite_writer_shm_native_lifetime_role::retained_parent};
			sqlite_backend_opaque_identity native_lifetime_identity;
			sqlite_backend_opaque_identity semantic_receipt;
			std::optional<sqlite_backend_opaque_identity> native_xopen_receipt;
			std::uint64_t next_pin_sequence{1U};
			std::vector<std::weak_ptr<sqlite_writer_shm_mapping_epoch_liveness>> epoch_liveness;
			std::atomic_bool live{true};
		};

		[[nodiscard]] bool valid_native_lifetime_metadata(
			const sqlite_writer_shm_native_lifetime_control& control) noexcept
		{
			if (!valid_identity(control.native_lifetime_identity) ||
				!valid_identity(control.semantic_receipt))
				return false;
			switch (control.role)
			{
				case sqlite_writer_shm_native_lifetime_role::main_database:
				case sqlite_writer_shm_native_lifetime_role::write_ahead_log:
					return control.native_xopen_receipt &&
						valid_identity(*control.native_xopen_receipt);
				case sqlite_writer_shm_native_lifetime_role::retained_parent:
				case sqlite_writer_shm_native_lifetime_role::shared_memory_attachment:
					return !control.native_xopen_receipt;
			}
			return false;
		}

		class sqlite_writer_shm_mapping_epoch_state final
			: public std::enable_shared_from_this<sqlite_writer_shm_mapping_epoch_state>
		{
		  public:
			sqlite_writer_shm_mapping_epoch_state(
				sqlite_writer_shm_mapping_epoch_request request,
				sqlite_writer_shm_mapping_epoch_preparation preparation,
				std::shared_ptr<sqlite_writer_shm_mapping_epoch_liveness> liveness)
				: request_{std::move(request)}, preparation_{std::move(preparation)},
				  liveness_{std::move(liveness)}
			{
			}

			[[nodiscard]] bool lifetimes_valid() const noexcept
			{
				return liveness_ && liveness_->live.load(std::memory_order_acquire) &&
					request_.retained_parent.valid() && request_.main_native_file.valid() &&
					request_.wal_native_file.valid() && request_.shm_native_attachment.valid();
			}

			[[nodiscard]] bool observation_available() const noexcept
			{
				return lifetimes_valid() && !sealed_.load(std::memory_order_acquire);
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_writer_shm_mapping_epoch_receipt>
			seal(const volatile void* native_mapping) noexcept
			{
				if (sealed_.exchange(true, std::memory_order_acq_rel))
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				if (native_mapping == nullptr)
					return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									 sqlite_shm_lease_recovery_action::outer_ioerr_no_retry);
				if (!lifetimes_valid())
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);

				try
				{
					auto post = preparation_.observer->observe_after_native_map(
						request_.binding, preparation_.pre_stat, native_mapping);
					if (!post)
					{
						auto failure = post.error();
						failure.action = failure.reason ==
									sqlite_shm_lease_rejection_reason::lifecycle_ambiguous ||
								failure.reason == sqlite_shm_lease_rejection_reason::quarantined
							? sqlite_shm_lease_recovery_action::quarantine_no_retry
							: sqlite_shm_lease_recovery_action::
								  attempt_nonremoving_unmap_then_outer_ioerr;
						return failure;
					}
					if (!lifetimes_valid())
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					if (!valid_stat_census(post->stat) ||
						!valid_identity(post->namespace_events.watch_epoch) ||
						!valid_identity(post->namespace_events.expected_shm_leaf) ||
						post->namespace_events.watch_epoch != preparation_.watch_arm_receipt ||
						post->namespace_events.expected_shm_leaf !=
							request_.binding.expected_shm_leaf)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										 sqlite_shm_lease_recovery_action::
											 attempt_nonremoving_unmap_then_outer_ioerr);

					return sqlite_writer_shm_mapping_epoch_receipt{weak_from_this(),
																   1U,
																   preparation_.epoch_identity,
																   preparation_.watch_arm_receipt,
																   request_.binding,
																   preparation_.pre_stat,
																   std::move(*post),
																   native_mapping};
				}
				catch (...)
				{
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

		  private:
			sqlite_writer_shm_mapping_epoch_request request_;
			sqlite_writer_shm_mapping_epoch_preparation preparation_;
			std::shared_ptr<sqlite_writer_shm_mapping_epoch_liveness> liveness_;
			std::atomic_bool sealed_{false};
		};
	} // namespace detail

	sqlite_writer_shm_native_lifetime_revoker::sqlite_writer_shm_native_lifetime_revoker(
		std::shared_ptr<detail::sqlite_writer_shm_native_lifetime_control> control) noexcept
		: control_{std::move(control)}
	{
	}

	sqlite_writer_shm_native_lifetime_revoker::~sqlite_writer_shm_native_lifetime_revoker() noexcept
	{
		(void)revoke();
	}

	sqlite_writer_shm_native_lifetime_revoker::sqlite_writer_shm_native_lifetime_revoker(
		sqlite_writer_shm_native_lifetime_revoker&& other) noexcept
		: control_{std::move(other.control_)}
	{
	}

	bool sqlite_writer_shm_native_lifetime_revoker::valid() const noexcept
	{
		return control_ && control_->live.load(std::memory_order_acquire);
	}

	bool sqlite_writer_shm_native_lifetime_revoker::revoke() noexcept
	{
		if (!control_)
			return false;
		auto control = std::move(control_);
		std::lock_guard lock{control->mutex};
		const auto revoked = control->live.exchange(false, std::memory_order_acq_rel);
		for (const auto& weak_liveness : control->epoch_liveness)
			if (const auto liveness = weak_liveness.lock())
				liveness->live.store(false, std::memory_order_release);
		control->epoch_liveness.clear();
		return revoked;
	}

	sqlite_writer_shm_native_lifetime_pin::sqlite_writer_shm_native_lifetime_pin(
		std::shared_ptr<detail::sqlite_writer_shm_native_lifetime_control> control,
		std::shared_ptr<void> retained_owner,
		sqlite_backend_opaque_identity pin_identity) noexcept
		: control_{std::move(control)}, retained_owner_{std::move(retained_owner)},
		  pin_identity_{std::move(pin_identity)}
	{
	}

	sqlite_writer_shm_native_lifetime_pin::~sqlite_writer_shm_native_lifetime_pin() noexcept =
		default;

	sqlite_writer_shm_native_lifetime_pin::sqlite_writer_shm_native_lifetime_pin(
		sqlite_writer_shm_native_lifetime_pin&& other) noexcept
		: control_{std::move(other.control_)}, retained_owner_{std::move(other.retained_owner_)},
		  pin_identity_{std::move(other.pin_identity_)}
	{
	}

	bool sqlite_writer_shm_native_lifetime_pin::valid() const noexcept
	{
		return control_ && retained_owner_ && control_->live.load(std::memory_order_acquire) &&
			detail::valid_native_lifetime_metadata(*control_) && valid_identity(pin_identity_);
	}

	sqlite_writer_shm_native_lifetime_role
	sqlite_writer_shm_native_lifetime_pin::role() const noexcept
	{
		return control_ ? control_->role : sqlite_writer_shm_native_lifetime_role::retained_parent;
	}

	const sqlite_backend_opaque_identity&
	sqlite_writer_shm_native_lifetime_pin::native_lifetime_identity() const noexcept
	{
		static const sqlite_backend_opaque_identity empty;
		return control_ ? control_->native_lifetime_identity : empty;
	}

	const sqlite_backend_opaque_identity&
	sqlite_writer_shm_native_lifetime_pin::semantic_receipt() const noexcept
	{
		static const sqlite_backend_opaque_identity empty;
		return control_ ? control_->semantic_receipt : empty;
	}

	const std::optional<sqlite_backend_opaque_identity>&
	sqlite_writer_shm_native_lifetime_pin::native_xopen_receipt() const noexcept
	{
		static const std::optional<sqlite_backend_opaque_identity> empty;
		return control_ ? control_->native_xopen_receipt : empty;
	}

	const sqlite_backend_opaque_identity&
	sqlite_writer_shm_native_lifetime_pin::pin_identity() const noexcept
	{
		return pin_identity_;
	}

	bool sqlite_writer_shm_native_lifetime_pin::bind_epoch_liveness(
		const std::shared_ptr<detail::sqlite_writer_shm_mapping_epoch_liveness>& liveness)
	{
		if (!control_ || !liveness || !liveness->live.load(std::memory_order_acquire))
			return false;
		std::lock_guard lock{control_->mutex};
		std::erase_if(control_->epoch_liveness,
					  [](const auto& candidate)
					  {
						  return candidate.expired();
					  });
		if (!liveness->live.load(std::memory_order_acquire) ||
			!control_->live.load(std::memory_order_acquire))
		{
			liveness->live.store(false, std::memory_order_release);
			return false;
		}
		control_->epoch_liveness.emplace_back(liveness);
		return true;
	}

	sqlite_writer_shm_native_lifetime_source::sqlite_writer_shm_native_lifetime_source(
		std::shared_ptr<detail::sqlite_writer_shm_native_lifetime_control> control,
		const std::shared_ptr<void>& retained_owner) noexcept
		: control_{std::move(control)}, retained_owner_{retained_owner}
	{
	}

	sqlite_writer_shm_native_lifetime_source::~sqlite_writer_shm_native_lifetime_source() noexcept =
		default;

	sqlite_writer_shm_native_lifetime_source::sqlite_writer_shm_native_lifetime_source(
		sqlite_writer_shm_native_lifetime_source&& other) noexcept
		: control_{std::move(other.control_)}, retained_owner_{std::move(other.retained_owner_)}
	{
	}

	bool sqlite_writer_shm_native_lifetime_source::valid() const noexcept
	{
		return control_ && !retained_owner_.expired() &&
			control_->live.load(std::memory_order_acquire) &&
			detail::valid_native_lifetime_metadata(*control_);
	}

	sqlite_shm_lease_result<sqlite_writer_shm_native_lifetime_pin>
	sqlite_writer_shm_native_lifetime_source::mint_pin()
	{
		auto retained_owner = retained_owner_.lock();
		if (!control_ || !retained_owner)
			return rejection(sqlite_shm_lease_rejection_reason::invalid_identity,
							 sqlite_shm_lease_recovery_action::deny_before_native_map);
		std::lock_guard lock{control_->mutex};
		if (!control_->live.load(std::memory_order_acquire))
			return rejection(sqlite_shm_lease_rejection_reason::retiring,
							 sqlite_shm_lease_recovery_action::deny_before_native_map);
		if (!detail::valid_native_lifetime_metadata(*control_))
			return rejection(sqlite_shm_lease_rejection_reason::invalid_identity,
							 sqlite_shm_lease_recovery_action::deny_before_native_map);
		if (control_->next_pin_sequence == 0U)
			return rejection(sqlite_shm_lease_rejection_reason::generation_exhausted,
							 sqlite_shm_lease_recovery_action::deny_before_native_map);

		auto pin_identity = sqlite_backend_opaque_identity{
			control_->native_lifetime_identity.profile + ".writer-shm-mapping-pin.v1",
			control_->native_lifetime_identity.bytes};
		const auto sequence = control_->next_pin_sequence;
		for (auto shift = 56U;; shift -= 8U)
		{
			pin_identity.bytes.push_back(static_cast<std::byte>((sequence >> shift) & 0xffU));
			if (shift == 0U)
				break;
		}
		++control_->next_pin_sequence;
		return sqlite_writer_shm_native_lifetime_pin{
			control_, std::move(retained_owner), std::move(pin_identity)};
	}

	std::pair<sqlite_writer_shm_native_lifetime_revoker, sqlite_writer_shm_native_lifetime_source>
	sqlite_writer_shm_native_lifetime_test_factory::create_source(
		const sqlite_writer_shm_native_lifetime_role role,
		sqlite_backend_opaque_identity native_lifetime_identity,
		sqlite_backend_opaque_identity semantic_receipt,
		std::optional<sqlite_backend_opaque_identity> native_xopen_receipt,
		const std::shared_ptr<void>& retained_owner)
	{
		auto control = std::make_shared<detail::sqlite_writer_shm_native_lifetime_control>();
		control->role = role;
		control->native_lifetime_identity = std::move(native_lifetime_identity);
		control->semantic_receipt = std::move(semantic_receipt);
		control->native_xopen_receipt = std::move(native_xopen_receipt);
		return {
			sqlite_writer_shm_native_lifetime_revoker{control},
			sqlite_writer_shm_native_lifetime_source{std::move(control), retained_owner},
		};
	}

	sqlite_writer_shm_mapping_epoch_arm::sqlite_writer_shm_mapping_epoch_arm(
		std::shared_ptr<detail::sqlite_writer_shm_mapping_epoch_state> state) noexcept
		: state_{std::move(state)}
	{
	}

	sqlite_writer_shm_mapping_epoch_arm::~sqlite_writer_shm_mapping_epoch_arm() noexcept = default;

	sqlite_writer_shm_mapping_epoch_arm::sqlite_writer_shm_mapping_epoch_arm(
		sqlite_writer_shm_mapping_epoch_arm&& other) noexcept
		: state_{std::move(other.state_)}
	{
	}

	bool sqlite_writer_shm_mapping_epoch_arm::valid() const noexcept
	{
		return state_ && state_->lifetimes_valid();
	}

	sqlite_writer_shm_mapping_epoch_observer::sqlite_writer_shm_mapping_epoch_observer(
		std::weak_ptr<detail::sqlite_writer_shm_mapping_epoch_state> state) noexcept
		: state_{std::move(state)}
	{
	}

	sqlite_writer_shm_mapping_epoch_observer::~sqlite_writer_shm_mapping_epoch_observer() noexcept =
		default;

	sqlite_writer_shm_mapping_epoch_observer::sqlite_writer_shm_mapping_epoch_observer(
		sqlite_writer_shm_mapping_epoch_observer&& other) noexcept
		: state_{std::move(other.state_)}
	{
		other.state_.reset();
	}

	bool sqlite_writer_shm_mapping_epoch_observer::valid() const noexcept
	{
		const auto state = state_.lock();
		return state && state->observation_available();
	}

	sqlite_writer_shm_mapping_epoch_receipt::sqlite_writer_shm_mapping_epoch_receipt(
		std::weak_ptr<detail::sqlite_writer_shm_mapping_epoch_state> state,
		const std::uint64_t seal_sequence,
		sqlite_backend_opaque_identity epoch_identity,
		sqlite_backend_opaque_identity watch_arm_receipt,
		sqlite_writer_shm_mapping_epoch_binding binding,
		sqlite_writer_shm_stat_census pre_stat,
		sqlite_writer_shm_mapping_epoch_post_observation post_observation,
		const volatile void* native_mapping)
		: state_{std::move(state)}, seal_sequence_{seal_sequence},
		  epoch_identity_{std::move(epoch_identity)},
		  watch_arm_receipt_{std::move(watch_arm_receipt)}, binding_{std::move(binding)},
		  pre_stat_{std::move(pre_stat)}, post_observation_{std::move(post_observation)},
		  native_mapping_{native_mapping}
	{
	}

	const sqlite_backend_opaque_identity&
	sqlite_writer_shm_mapping_epoch_receipt::epoch_identity() const noexcept
	{
		return epoch_identity_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_writer_shm_mapping_epoch_receipt::watch_arm_receipt() const noexcept
	{
		return watch_arm_receipt_;
	}

	const sqlite_writer_shm_mapping_epoch_binding&
	sqlite_writer_shm_mapping_epoch_receipt::binding() const noexcept
	{
		return binding_;
	}

	const sqlite_writer_shm_stat_census&
	sqlite_writer_shm_mapping_epoch_receipt::pre_stat() const noexcept
	{
		return pre_stat_;
	}

	const sqlite_writer_shm_mapping_epoch_post_observation&
	sqlite_writer_shm_mapping_epoch_receipt::post_observation() const noexcept
	{
		return post_observation_;
	}

	const volatile void* sqlite_writer_shm_mapping_epoch_receipt::native_mapping() const noexcept
	{
		return native_mapping_;
	}

	sqlite_writer_shm_mapping_epoch_activation::sqlite_writer_shm_mapping_epoch_activation(
		sqlite_writer_shm_mapping_epoch_arm arm,
		sqlite_writer_shm_mapping_epoch_observer observer) noexcept
		: arm_{std::move(arm)}, observer_{std::move(observer)}
	{
	}

	sqlite_writer_shm_mapping_epoch_activation::sqlite_writer_shm_mapping_epoch_activation(
		sqlite_writer_shm_mapping_epoch_activation&& other) noexcept
		: arm_{std::move(other.arm_)}, observer_{std::move(other.observer_)}
	{
	}

	sqlite_writer_shm_mapping_epoch_arm
	sqlite_writer_shm_mapping_epoch_activation::take_arm() noexcept
	{
		return std::move(arm_);
	}

	sqlite_writer_shm_mapping_epoch_observer
	sqlite_writer_shm_mapping_epoch_activation::take_observer() noexcept
	{
		return std::move(observer_);
	}

	sqlite_shm_lease_result<sqlite_writer_shm_mapping_epoch_activation>
	sqlite_writer_shm_mapping_epoch_port::arm(
		sqlite_writer_shm_mapping_epoch_request request) noexcept
	{
		if (!valid_epoch_request(request))
			return rejection(
				classify_sqlite_shm_writer_extend_pair(request.binding.map_request.caller_extend,
													   request.binding.delegated_extend)
						.has_value()
					? sqlite_shm_lease_rejection_reason::invalid_request
					: sqlite_shm_lease_rejection_reason::invalid_extend_pair,
				sqlite_shm_lease_recovery_action::deny_before_native_map);

		try
		{
			auto liveness = std::make_shared<detail::sqlite_writer_shm_mapping_epoch_liveness>();
			if (!request.retained_parent.bind_epoch_liveness(liveness) ||
				!request.main_native_file.bind_epoch_liveness(liveness) ||
				!request.wal_native_file.bind_epoch_liveness(liveness) ||
				!request.shm_native_attachment.bind_epoch_liveness(liveness))
				return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								 sqlite_shm_lease_recovery_action::deny_before_native_map);
			if (!liveness->live.load(std::memory_order_acquire) || !valid_epoch_request(request))
				return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								 sqlite_shm_lease_recovery_action::deny_before_native_map);
			auto preparation = arm_watch_before_pre_stat(request);
			if (!preparation)
			{
				auto failure = preparation.error();
				failure.action = sqlite_shm_lease_recovery_action::deny_before_native_map;
				return failure;
			}
			if (!valid_identity(preparation->epoch_identity) ||
				!valid_identity(preparation->watch_arm_receipt) ||
				!valid_stat_census(preparation->pre_stat) || !preparation->observer)
				return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
								 sqlite_shm_lease_recovery_action::deny_before_native_map);
			if (!valid_epoch_request(request))
				return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								 sqlite_shm_lease_recovery_action::deny_before_native_map);
			auto state = std::make_shared<detail::sqlite_writer_shm_mapping_epoch_state>(
				std::move(request), std::move(*preparation), std::move(liveness));
			return sqlite_writer_shm_mapping_epoch_activation{
				sqlite_writer_shm_mapping_epoch_arm{state},
				sqlite_writer_shm_mapping_epoch_observer{state}};
		}
		catch (...)
		{
			return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
							 sqlite_shm_lease_recovery_action::deny_before_native_map);
		}
	}

	sqlite_shm_lease_result<sqlite_writer_shm_mapping_epoch_receipt>
	seal_sqlite_writer_shm_mapping_epoch(sqlite_writer_shm_mapping_epoch_observer& observer,
										 const volatile void* native_mapping) noexcept
	{
		const auto state = observer.state_.lock();
		if (!state)
			return rejection(sqlite_shm_lease_rejection_reason::stale_token,
							 sqlite_shm_lease_recovery_action::quarantine_no_retry);
		return state->seal(native_mapping);
	}
} // namespace cxxlens::sdk
