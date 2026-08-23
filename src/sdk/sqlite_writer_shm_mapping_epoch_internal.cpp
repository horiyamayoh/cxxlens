#include "sqlite_writer_shm_mapping_epoch_internal.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sqlite_source_shm_readonly_preflight_internal.hpp"

namespace cxxlens::sdk
{
	namespace
	{
		[[nodiscard]] bool valid_identity(const sqlite_backend_opaque_identity& identity) noexcept
		{
			return !identity.profile.empty() && !identity.bytes.empty();
		}

		[[nodiscard]] bool distinct_identity(const sqlite_backend_opaque_identity& left,
											 const sqlite_backend_opaque_identity& right) noexcept
		{
			return left != right;
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
				(!binding.target_namespace_epoch_identity ||
				 valid_identity(*binding.target_namespace_epoch_identity)) &&
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

		void append_u64(std::vector<std::byte>& output, const std::uint64_t value)
		{
			for (auto shift = 56U;; shift -= 8U)
			{
				output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
				if (shift == 0U)
					break;
			}
		}

		void append_identity(std::vector<std::byte>& output,
							 const sqlite_backend_opaque_identity& identity)
		{
			append_u64(output, static_cast<std::uint64_t>(identity.profile.size()));
			for (const auto character : identity.profile)
				output.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
			append_u64(output, static_cast<std::uint64_t>(identity.bytes.size()));
			output.insert(output.end(), identity.bytes.begin(), identity.bytes.end());
		}

		[[nodiscard]] bool valid_platform_binding(
			const sqlite_writer_shm_mapping_epoch_platform_binding& binding) noexcept
		{
			if (!binding.target_namespace_epoch ||
				!valid_identity(binding.target_namespace_epoch->identity()) ||
				!valid_identity(binding.parent_namespace_identity) ||
				!valid_identity(binding.target_namespace_epoch->parent_namespace_identity()) ||
				binding.parent_namespace_identity !=
					binding.target_namespace_epoch->parent_namespace_identity() ||
				!valid_identity(binding.sqlite_source_id) ||
				!valid_identity(binding.wal_write_lock_receipt) ||
				!valid_identity(binding.effect_gate_receipt) ||
				!valid_identity(binding.effect_receipt))
				return false;
			if (binding.absent_filesystem_profile.has_value() !=
					binding.absent_mount_identity.has_value() ||
				(binding.absent_filesystem_profile &&
				 (!valid_identity(*binding.absent_filesystem_profile) ||
				  !valid_identity(*binding.absent_mount_identity))))
				return false;
			return binding.sqlite_source_id != binding.wal_write_lock_receipt &&
				binding.sqlite_source_id != binding.effect_gate_receipt &&
				binding.sqlite_source_id != binding.effect_receipt &&
				binding.wal_write_lock_receipt != binding.effect_gate_receipt &&
				binding.wal_write_lock_receipt != binding.effect_receipt &&
				binding.effect_gate_receipt != binding.effect_receipt;
		}

		[[nodiscard]] sqlite_backend_opaque_identity
		retained_epoch_receipt(const std::string_view label,
							   const sqlite_writer_shm_mapping_epoch_platform_binding& platform,
							   const sqlite_writer_shm_mapping_epoch_binding& binding,
							   const std::uint64_t sequence)
		{
			sqlite_backend_opaque_identity output;
			output.profile = "sqlite-source-shm-retained-writer-epoch.v1.";
			output.profile += label;
			append_identity(output.bytes, platform.target_namespace_epoch->identity());
			append_identity(output.bytes, platform.parent_namespace_identity);
			append_identity(output.bytes, binding.expected_shm_leaf);
			append_identity(output.bytes, binding.map_request.family.process_instance);
			append_identity(output.bytes, binding.map_request.family.shared_runtime_vfs_cohort);
			append_identity(output.bytes, binding.map_request.family.exact_file_family);
			append_identity(output.bytes, binding.map_request.alias_lifetime);
			append_identity(output.bytes, binding.map_request.connection_token);
			append_identity(output.bytes, binding.map_request.attachment.attachment_epoch());
			append_identity(output.bytes, binding.map_request.callback.invocation_token);
			append_u64(output.bytes, sequence);
			return output;
		}

		[[nodiscard]] sqlite_shm_lease_result<sqlite_writer_shm_stat_census>
		observe_retained_shm(const sqlite_writer_shm_mapping_epoch_platform_binding& platform,
							 const bool after_native_map = false) noexcept
		{
			try
			{
				if (!valid_platform_binding(platform))
					return rejection(sqlite_shm_lease_rejection_reason::invalid_identity,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				if (!after_native_map)
				{
					if (auto checked = platform.target_namespace_epoch->recheck(); !checked)
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
				if (auto stat =
						platform.target_namespace_epoch->observe_writer_shm_stat(after_native_map);
					stat)
				{
					if (stat->role != sqlite_backend_file_role::shared_memory ||
						stat->parent_namespace_identity != platform.parent_namespace_identity ||
						!valid_identity(stat->filesystem_profile) ||
						!valid_identity(stat->mount_identity))
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										 sqlite_shm_lease_recovery_action::
											 attempt_nonremoving_unmap_then_outer_ioerr);
					if (stat->state == sqlite_backend_entry_state::absent)
					{
						if (stat->object_identity || stat->directory_entry_identity ||
							stat->byte_count != 0U)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
											 sqlite_shm_lease_recovery_action::
												 attempt_nonremoving_unmap_then_outer_ioerr);
						return sqlite_writer_shm_stat_census{sqlite_writer_shm_entry_state::absent,
															 stat->parent_namespace_identity,
															 stat->filesystem_profile,
															 stat->mount_identity,
															 std::nullopt,
															 std::nullopt,
															 0U};
					}
					if (stat->state != sqlite_backend_entry_state::held_regular ||
						!stat->object_identity || !valid_identity(*stat->object_identity) ||
						!stat->directory_entry_identity ||
						!valid_identity(*stat->directory_entry_identity) || stat->byte_count == 0U)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										 sqlite_shm_lease_recovery_action::
											 attempt_nonremoving_unmap_then_outer_ioerr);
					return sqlite_writer_shm_stat_census{
						sqlite_writer_shm_entry_state::direct_regular,
						stat->parent_namespace_identity,
						stat->filesystem_profile,
						stat->mount_identity,
						stat->object_identity,
						stat->directory_entry_identity,
						stat->byte_count};
				}
				if (after_native_map)
				{
					if (auto checked = platform.target_namespace_epoch->recheck(); !checked)
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
				auto retained = platform.target_namespace_epoch->retained_entry(
					sqlite_backend_file_role::shared_memory);
				if (!retained)
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);

				const auto& entry = *retained;
				if (entry.role != sqlite_backend_file_role::shared_memory)
					return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									 sqlite_shm_lease_recovery_action::
										 attempt_nonremoving_unmap_then_outer_ioerr);
				if (entry.state == sqlite_backend_entry_state::absent)
				{
					if (!platform.absent_filesystem_profile ||
						!valid_identity(*platform.absent_filesystem_profile) ||
						!platform.absent_mount_identity ||
						!valid_identity(*platform.absent_mount_identity))
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										 sqlite_shm_lease_recovery_action::
											 attempt_nonremoving_unmap_then_outer_ioerr);
					return sqlite_writer_shm_stat_census{sqlite_writer_shm_entry_state::absent,
														 platform.parent_namespace_identity,
														 *platform.absent_filesystem_profile,
														 *platform.absent_mount_identity,
														 std::nullopt,
														 std::nullopt,
														 0U};
				}
				const auto& held = entry.held_object;
				if (entry.state != sqlite_backend_entry_state::held_regular ||
					!entry.direct_regular_entry || !entry.object_identity ||
					!entry.directory_entry_identity || !entry.object_filesystem_profile || !held ||
					held->role() != sqlite_backend_file_role::shared_memory ||
					held->object_identity() != *entry.object_identity ||
					held->directory_entry_identity() != *entry.directory_entry_identity ||
					held->object_filesystem_profile() != entry.object_filesystem_profile ||
					!held->object_mount_identity())
					return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									 sqlite_shm_lease_recovery_action::
										 attempt_nonremoving_unmap_then_outer_ioerr);
				if (auto checked = held->recheck_retained_object(); !checked)
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				auto size = held->size();
				if (!size)
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				sqlite_writer_shm_stat_census output{sqlite_writer_shm_entry_state::direct_regular,
													 platform.parent_namespace_identity,
													 *entry.object_filesystem_profile,
													 *held->object_mount_identity(),
													 entry.object_identity,
													 entry.directory_entry_identity,
													 *size};
				if (!valid_stat_census(output))
					return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									 sqlite_shm_lease_recovery_action::
										 attempt_nonremoving_unmap_then_outer_ioerr);
				return output;
			}
			catch (...)
			{
				return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								 sqlite_shm_lease_recovery_action::quarantine_no_retry);
			}
		}

		class retained_namespace_writer_shm_mapping_epoch_observation final
			: public sqlite_writer_shm_mapping_epoch_observation_port
		{
		  public:
			retained_namespace_writer_shm_mapping_epoch_observation(
				sqlite_writer_shm_mapping_epoch_platform_binding platform,
				sqlite_writer_shm_mapping_epoch_binding binding,
				sqlite_writer_shm_stat_census pre_stat,
				sqlite_backend_opaque_identity watch_receipt)
				: platform_{std::move(platform)}, binding_{std::move(binding)},
				  pre_stat_{std::move(pre_stat)}, watch_receipt_{std::move(watch_receipt)}
			{
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_writer_shm_mapping_epoch_post_observation>
			observe_after_native_map(const sqlite_writer_shm_mapping_epoch_binding& binding,
									 const sqlite_writer_shm_stat_census& pre_stat,
									 const volatile void* native_mapping) override
			{
				try
				{
					if (native_mapping == nullptr || binding != binding_ || pre_stat != pre_stat_ ||
						!valid_platform_binding(platform_))
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										 sqlite_shm_lease_recovery_action::
											 attempt_nonremoving_unmap_then_outer_ioerr);
					auto post_stat = observe_retained_shm(platform_, true);
					if (!post_stat)
						return post_stat.error();
					if (!checked_mapping_range(binding.map_request.page_number,
											   binding.map_request.page_size))
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										 sqlite_shm_lease_recovery_action::
											 attempt_nonremoving_unmap_then_outer_ioerr);

					const auto page = static_cast<std::uint64_t>(binding.map_request.page_number);
					const auto page_size =
						static_cast<std::uint64_t>(binding.map_request.page_size);
					const auto offset = page * page_size;
					const auto range_end = offset + page_size;
					if (range_end > post_stat->byte_count)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										 sqlite_shm_lease_recovery_action::
											 attempt_nonremoving_unmap_then_outer_ioerr);

					sqlite_writer_shm_namespace_event_census namespace_events;
					namespace_events.watch_epoch = watch_receipt_;
					namespace_events.expected_shm_leaf = binding.expected_shm_leaf;
					namespace_events.trusted_stat_watch_profile = true;

					sqlite_writer_shm_effect_census effects;
					effects.sqlite_source_id = platform_.sqlite_source_id;
					effects.callback_transcript = binding.map_request.callback.invocation_token;
					effects.wal_write_lock_receipt = platform_.wal_write_lock_receipt;
					effects.effect_gate_receipt = platform_.effect_gate_receipt;
					effects.effect_receipt = platform_.effect_receipt;
					effects.size_before = pre_stat_.byte_count;
					effects.size_after = post_stat->byte_count;
					effects.requested_range_end = range_end;
					effects.complete = true;
					effects.result_confirmed_success = true;

					const auto pair = classify_sqlite_shm_writer_extend_pair(
						binding.map_request.caller_extend, binding.delegated_extend);
					const auto same_direct_entry =
						pre_stat_.state == sqlite_writer_shm_entry_state::direct_regular &&
						post_stat->state == sqlite_writer_shm_entry_state::direct_regular &&
						pre_stat_.object_identity == post_stat->object_identity &&
						pre_stat_.directory_entry_identity == post_stat->directory_entry_identity;
					if (!pair)
						return rejection(sqlite_shm_lease_rejection_reason::invalid_extend_pair,
										 sqlite_shm_lease_recovery_action::
											 attempt_nonremoving_unmap_then_outer_ioerr);
					if (*pair == sqlite_shm_writer_extend_pair::zero_zero)
					{
						if (!same_direct_entry || post_stat->byte_count != pre_stat_.byte_count)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
											 sqlite_shm_lease_recovery_action::
												 attempt_nonremoving_unmap_then_outer_ioerr);
						return sqlite_writer_shm_mapping_epoch_post_observation{
							*post_stat,
							std::move(namespace_events),
							std::move(effects),
							sqlite_writer_shm_observed_transition::preexisting_unchanged};
					}
					if (*pair == sqlite_shm_writer_extend_pair::one_one && same_direct_entry)
					{
						if (post_stat->byte_count == pre_stat_.byte_count)
							return sqlite_writer_shm_mapping_epoch_post_observation{
								*post_stat,
								std::move(namespace_events),
								std::move(effects),
								sqlite_writer_shm_observed_transition::preexisting_preallocated};
						if (pre_stat_.byte_count < range_end && post_stat->byte_count == range_end)
						{
							effects.extend_count = sqlite_writer_shm_bounded_count::one;
							return sqlite_writer_shm_mapping_epoch_post_observation{
								*post_stat,
								std::move(namespace_events),
								std::move(effects),
								sqlite_writer_shm_observed_transition::preexisting_grown};
						}
					}
					if (*pair == sqlite_shm_writer_extend_pair::one_one &&
						pre_stat_.state == sqlite_writer_shm_entry_state::absent &&
						post_stat->state == sqlite_writer_shm_entry_state::direct_regular &&
						post_stat->byte_count == range_end)
					{
						namespace_events.expected_leaf_create =
							sqlite_writer_shm_bounded_count::one;
						effects.create_count = sqlite_writer_shm_bounded_count::one;
						return sqlite_writer_shm_mapping_epoch_post_observation{
							*post_stat,
							std::move(namespace_events),
							std::move(effects),
							sqlite_writer_shm_observed_transition::absent_created};
					}
					return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									 sqlite_shm_lease_recovery_action::
										 attempt_nonremoving_unmap_then_outer_ioerr);
				}
				catch (...)
				{
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

		  private:
			sqlite_writer_shm_mapping_epoch_platform_binding platform_;
			sqlite_writer_shm_mapping_epoch_binding binding_;
			sqlite_writer_shm_stat_census pre_stat_;
			sqlite_backend_opaque_identity watch_receipt_;
		};
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
				!valid_identity(control.semantic_receipt) ||
				!distinct_identity(control.native_lifetime_identity, control.semantic_receipt))
				return false;
			switch (control.role)
			{
				case sqlite_writer_shm_native_lifetime_role::main_database:
				case sqlite_writer_shm_native_lifetime_role::write_ahead_log:
					return control.native_xopen_receipt &&
						valid_identity(*control.native_xopen_receipt) &&
						distinct_identity(control.native_lifetime_identity,
										  *control.native_xopen_receipt) &&
						distinct_identity(control.semantic_receipt, *control.native_xopen_receipt);
				case sqlite_writer_shm_native_lifetime_role::retained_parent:
				case sqlite_writer_shm_native_lifetime_role::shared_memory_attachment:
					return !control.native_xopen_receipt;
			}
			return false;
		}

		class sqlite_writer_shm_generation_epoch_custody final
		{
		  public:
			~sqlite_writer_shm_generation_epoch_custody() noexcept
			{
				if (minter_)
					minter_->release_generation_authority();
			}

			[[nodiscard]] bool
			install(std::optional<sqlite_source_shm_target_namespace_epoch_borrow_minter>
						minter) noexcept
			{
				if (installed_ || !minter || !minter->valid())
					return false;
				minter_.emplace(std::move(*minter));
				if (!minter_->retain_generation_authority())
				{
					minter_.reset();
					return false;
				}
				installed_ = true;
				return true;
			}

			[[nodiscard]] bool valid() const noexcept
			{
				return installed_ && (generic_ || (minter_ && minter_->valid()));
			}

			[[nodiscard]] bool reader_borrow_capable() const noexcept
			{
				return installed_ && minter_ && minter_->valid();
			}

			[[nodiscard]] bool install_generic() noexcept
			{
				if (installed_ || canonical_target_)
					return false;
				installed_ = true;
				generic_ = true;
				return true;
			}

			[[nodiscard]] bool bind_canonical_target(
				const sqlite_shm_reader_attachment_target_identity& target) noexcept
			{
				if (canonical_target_ &&
					(canonical_target_->parent_namespace != target.parent_namespace ||
					 canonical_target_->shm_object != target.shm_object ||
					 canonical_target_->shm_entry != target.shm_entry ||
					 canonical_target_->filesystem != target.filesystem ||
					 canonical_target_->mount != target.mount))
					return false;
				canonical_target_ = target;
				return true;
			}

			[[nodiscard]] bool matches_canonical_target(
				const sqlite_shm_reader_attachment_target_identity& target) const noexcept
			{
				return canonical_target_ &&
					canonical_target_->parent_namespace == target.parent_namespace &&
					canonical_target_->shm_object == target.shm_object &&
					canonical_target_->shm_entry == target.shm_entry &&
					canonical_target_->filesystem == target.filesystem &&
					canonical_target_->mount == target.mount;
			}

			[[nodiscard]] bool canonical_target_bound() const noexcept
			{
				return canonical_target_.has_value();
			}

			[[nodiscard]] result<sqlite_source_shm_target_namespace_epoch_reader_borrow>
			mint(const sqlite_shm_reader_native_ok_projection_reservation& reservation)
			{
				if (!valid())
					return unexpected(error{"store.backend-unavailable",
											"sqlite",
											"source-shm-readonly-qualification"});
				return minter_->mint(reservation);
			}

		  private:
			std::optional<sqlite_source_shm_target_namespace_epoch_borrow_minter> minter_;
			std::optional<sqlite_shm_reader_attachment_target_identity> canonical_target_;
			bool installed_{};
			bool generic_{};
		};

		class sqlite_writer_shm_mapping_epoch_state final
			: public std::enable_shared_from_this<sqlite_writer_shm_mapping_epoch_state>
		{
		  public:
			sqlite_writer_shm_mapping_epoch_state(
				sqlite_writer_shm_mapping_epoch_request request,
				sqlite_writer_shm_mapping_epoch_preparation preparation,
				std::shared_ptr<sqlite_writer_shm_mapping_epoch_liveness> liveness)
				: request_{std::move(request)}, preparation_{std::move(preparation)},
				  liveness_{std::move(liveness)},
				  generation_custody_{
					  std::make_shared<sqlite_writer_shm_generation_epoch_custody>()}
			{
			}

			[[nodiscard]] bool lifetimes_valid() const noexcept
			{
				return liveness_ && liveness_->live.load(std::memory_order_acquire) &&
					request_.retained_parent.valid() && request_.main_native_file.valid() &&
					request_.wal_native_file.valid() && request_.shm_native_attachment.valid();
			}

			[[nodiscard]] bool
			valid_for_predelegation(const sqlite_shm_writer_map_request& request) const noexcept
			{
				return observation_available() && request_.binding.map_request == request;
			}

			[[nodiscard]] bool
			retains_exact_lifetimes(const sqlite_shm_writer_map_request& request) const noexcept
			{
				return lifetimes_valid() && request_.binding.map_request == request;
			}

			[[nodiscard]] result<sqlite_source_shm_target_namespace_epoch_reader_borrow>
			mint_reader_borrow(
				const sqlite_shm_reader_native_ok_projection_reservation& reservation)
			{
				if (!lifetimes_valid() || !preparation_.borrow_minter)
					return unexpected(error{"store.backend-unavailable",
											"sqlite",
											"source-shm-readonly-qualification"});
				return preparation_.borrow_minter->mint(reservation);
			}

			[[nodiscard]] bool reader_borrow_capable() const noexcept
			{
				return lifetimes_valid() && preparation_.borrow_minter.has_value();
			}

			[[nodiscard]] std::shared_ptr<sqlite_writer_shm_generation_epoch_custody>
			generation_custody() const noexcept
			{
				return generation_custody_;
			}

			[[nodiscard]] bool transfer_generation_custody() noexcept
			{
				return generation_custody_ && lifetimes_valid() &&
					generation_custody_->install(std::move(preparation_.borrow_minter));
			}

			[[nodiscard]] bool target_identity_matches(
				const sqlite_shm_reader_attachment_target_identity& target,
				const sqlite_shm_verified_writer_post_map_receipt& receipt) const noexcept
			{
				return lifetimes_valid() && request_.binding.map_request == receipt.request() &&
					request_.binding.target_namespace_epoch_identity &&
					*request_.binding.target_namespace_epoch_identity == target.namespace_epoch &&
					receipt.target_identity() && *receipt.target_identity() == target;
			}

			[[nodiscard]] bool attachment_cohort_compatible_with(
				const sqlite_writer_shm_mapping_epoch_state& other) const noexcept
			{
				const auto& left = request_.binding;
				const auto& right = other.request_.binding;
				return left.map_request.attachment == right.map_request.attachment &&
					left.expected_shm_leaf == right.expected_shm_leaf &&
					left.retained_parent_receipt == right.retained_parent_receipt &&
					left.wal_native_file_receipt == right.wal_native_file_receipt &&
					left.wal_xopen_receipt == right.wal_xopen_receipt &&
					left.shm_native_attachment_receipt == right.shm_native_attachment_receipt &&
					request_.retained_parent.native_lifetime_identity() ==
					other.request_.retained_parent.native_lifetime_identity() &&
					request_.main_native_file.native_lifetime_identity() ==
					other.request_.main_native_file.native_lifetime_identity() &&
					request_.wal_native_file.native_lifetime_identity() ==
					other.request_.wal_native_file.native_lifetime_identity() &&
					request_.shm_native_attachment.native_lifetime_identity() ==
					other.request_.shm_native_attachment.native_lifetime_identity() &&
					request_.retained_parent.control_.get() ==
					other.request_.retained_parent.control_.get() &&
					request_.main_native_file.control_.get() ==
					other.request_.main_native_file.control_.get() &&
					request_.wal_native_file.control_.get() ==
					other.request_.wal_native_file.control_.get() &&
					request_.shm_native_attachment.control_.get() ==
					other.request_.shm_native_attachment.control_.get();
			}

#if defined(CXXLENS_SQLITE_TEST_SUPPORT)
			void invalidate_for_testing() noexcept
			{
				if (liveness_)
					liveness_->live.store(false, std::memory_order_release);
			}
#endif

			[[nodiscard]] bool observation_available() const noexcept
			{
				return lifetimes_valid() && !sealed_.load(std::memory_order_acquire);
			}

			[[nodiscard]] sqlite_shm_lease_result<void>
			begin_authoritative_validation(const std::uint64_t seal_sequence) noexcept
			{
				if (authoritative_validation_attempted_.exchange(true, std::memory_order_acq_rel))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				if (seal_sequence == 0U || seal_sequence != seal_sequence_ ||
					!sealed_.load(std::memory_order_acquire))
					return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									 sqlite_shm_lease_recovery_action::
										 attempt_nonremoving_unmap_then_outer_ioerr);
				if (!lifetimes_valid())
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				return {};
			}

			[[nodiscard]] bool
			authoritative_validation_still_live(const std::uint64_t seal_sequence) const noexcept
			{
				return seal_sequence != 0U && seal_sequence == seal_sequence_ &&
					sealed_.load(std::memory_order_acquire) &&
					authoritative_validation_attempted_.load(std::memory_order_acquire) &&
					lifetimes_valid();
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
																   seal_sequence_,
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
			std::shared_ptr<sqlite_writer_shm_generation_epoch_custody> generation_custody_;
			const std::uint64_t seal_sequence_{1U};
			std::atomic_bool sealed_{false};
			std::atomic_bool authoritative_validation_attempted_{false};
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

	sqlite_shm_lease_result<std::pair<sqlite_writer_shm_native_lifetime_revoker,
									  sqlite_writer_shm_native_lifetime_source>>
	sqlite_writer_shm_native_lifetime_production_factory::create_source(
		const sqlite_writer_shm_native_lifetime_role role,
		sqlite_backend_opaque_identity native_lifetime_identity,
		sqlite_backend_opaque_identity semantic_receipt,
		std::optional<sqlite_backend_opaque_identity> native_xopen_receipt,
		const std::shared_ptr<void>& retained_owner) noexcept
	{
		const auto invalid = []
		{
			return sqlite_shm_lease_result<std::pair<sqlite_writer_shm_native_lifetime_revoker,
													 sqlite_writer_shm_native_lifetime_source>>{
				sqlite_shm_lease_rejection{
					sqlite_shm_lease_rejection_reason::invalid_identity,
					sqlite_shm_lease_recovery_action::deny_before_native_map}};
		};
		if (!retained_owner || !valid_identity(native_lifetime_identity) ||
			!valid_identity(semantic_receipt) ||
			!distinct_identity(native_lifetime_identity, semantic_receipt))
			return invalid();
		switch (role)
		{
			case sqlite_writer_shm_native_lifetime_role::main_database:
			case sqlite_writer_shm_native_lifetime_role::write_ahead_log:
				if (!native_xopen_receipt)
					return invalid();
				break;
			case sqlite_writer_shm_native_lifetime_role::retained_parent:
			case sqlite_writer_shm_native_lifetime_role::shared_memory_attachment:
				if (native_xopen_receipt)
					return invalid();
				break;
			default:
				return invalid();
		}
		if (native_xopen_receipt &&
			(!valid_identity(*native_xopen_receipt) ||
			 !distinct_identity(native_lifetime_identity, *native_xopen_receipt) ||
			 !distinct_identity(semantic_receipt, *native_xopen_receipt)))
			return invalid();
		try
		{
			auto control = std::make_shared<detail::sqlite_writer_shm_native_lifetime_control>();
			control->role = role;
			control->native_lifetime_identity = std::move(native_lifetime_identity);
			control->semantic_receipt = std::move(semantic_receipt);
			control->native_xopen_receipt = std::move(native_xopen_receipt);
			return std::pair{
				sqlite_writer_shm_native_lifetime_revoker{control},
				sqlite_writer_shm_native_lifetime_source{std::move(control), retained_owner}};
		}
		catch (const std::bad_alloc&)
		{
			return sqlite_shm_lease_result<std::pair<sqlite_writer_shm_native_lifetime_revoker,
													 sqlite_writer_shm_native_lifetime_source>>{
				sqlite_shm_lease_rejection{
					sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
					sqlite_shm_lease_recovery_action::deny_before_native_map}};
		}
		catch (const std::length_error&)
		{
			return sqlite_shm_lease_result<std::pair<sqlite_writer_shm_native_lifetime_revoker,
													 sqlite_writer_shm_native_lifetime_source>>{
				sqlite_shm_lease_rejection{
					sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
					sqlite_shm_lease_recovery_action::deny_before_native_map}};
		}
	}

	sqlite_writer_shm_generation_epoch_authority::sqlite_writer_shm_generation_epoch_authority(
		std::shared_ptr<detail::sqlite_writer_shm_mapping_epoch_state> state,
		std::shared_ptr<detail::sqlite_writer_shm_generation_epoch_custody> custody) noexcept
		: state_{std::move(state)}, custody_{std::move(custody)}
	{
	}

	sqlite_writer_shm_generation_epoch_authority::
		~sqlite_writer_shm_generation_epoch_authority() noexcept = default;

	sqlite_writer_shm_generation_epoch_authority::sqlite_writer_shm_generation_epoch_authority(
		sqlite_writer_shm_generation_epoch_authority&& other) noexcept = default;

	bool sqlite_writer_shm_generation_epoch_authority::valid() const noexcept
	{
		return custody_ && (custody_->valid() || (state_ && state_->lifetimes_valid()));
	}

	bool sqlite_writer_shm_generation_epoch_authority::target_identity_matches(
		const sqlite_shm_reader_attachment_target_identity& target,
		const sqlite_shm_verified_writer_post_map_receipt& receipt) const noexcept
	{
		// The generation authority is deliberately not a second copy of a
		// holder's request-bound proof.  The holder checks its own receipt and
		// namespace epoch; the generation only vouches for the canonical SHM
		// object identity shared by every holder that joins it.
		return receipt.target_identity() && matches_canonical_target(target);
	}

	bool sqlite_writer_shm_generation_epoch_authority::matches_canonical_target(
		const sqlite_shm_reader_attachment_target_identity& target) const noexcept
	{
		return valid() && custody_ && custody_->matches_canonical_target(target);
	}

	bool sqlite_writer_shm_generation_epoch_authority::canonical_target_bound() const noexcept
	{
		return custody_ && custody_->canonical_target_bound();
	}

	bool sqlite_writer_shm_generation_epoch_authority::reader_borrow_capable() const noexcept
	{
		return custody_ &&
			(custody_->reader_borrow_capable() || (state_ && state_->reader_borrow_capable()));
	}

	bool sqlite_writer_shm_generation_epoch_authority::same_custody_as(
		const sqlite_writer_shm_generation_epoch_authority& other) const noexcept
	{
		return custody_ && custody_ == other.custody_ && canonical_target_bound() &&
			other.canonical_target_bound();
	}

	bool sqlite_writer_shm_generation_epoch_authority::bind_canonical_target(
		const sqlite_shm_verified_writer_post_map_receipt& receipt) noexcept
	{
		// The installer has already passed its local pending-authority/receipt
		// validation.  Binding the generation must not re-apply that local
		// request predicate: a later holder may have a distinct namespace epoch
		// while addressing the same canonical SHM object.
		if (!state_ || !custody_ || !state_->lifetimes_valid())
			return false;
		// Legacy writer-only flows have no reader attachment target.  They may
		// publish a generation, but cannot subsequently mint a qualified reader
		// borrow because matches_canonical_target remains false.
		if (!receipt.target_identity())
			return !custody_->canonical_target_bound();
		if (custody_->canonical_target_bound() &&
			!custody_->matches_canonical_target(*receipt.target_identity()))
			return false;
		if (!custody_->valid() && !state_->transfer_generation_custody())
			return false;
		if (!custody_->bind_canonical_target(*receipt.target_identity()))
			return false;
		// The generation now owns the controller/minter.  Do not retain the
		// installer's native map census, watch, or local lifetime pins merely to
		// keep that controller alive; each writer member owns those independently.
		state_.reset();
		return true;
	}

	bool sqlite_writer_shm_generation_epoch_authority::bind_generic_custody() noexcept
	{
		if (!state_ || !custody_ || !state_->lifetimes_valid() ||
			custody_->canonical_target_bound())
			return false;
		if (!custody_->valid() && !custody_->install_generic())
			return false;
		state_.reset();
		return true;
	}

	result<sqlite_shm_writer_reader_borrow_mint_capability>
	sqlite_writer_shm_generation_epoch_authority::reserve_reader_borrow_mint(
		const std::uint64_t map_token,
		const std::uint64_t generation,
		const std::uint64_t holder_token) const
	{
		if (!custody_ || !custody_->valid() || map_token == 0U || generation == 0U ||
			holder_token == 0U)
			return unexpected(
				error{"store.backend-unavailable", "sqlite", "source-shm-readonly-qualification"});
		return sqlite_shm_writer_reader_borrow_mint_capability{
			custody_, sqlite_shm_writer_reader_borrow_tokens{map_token, generation, holder_token}};
	}

#if defined(CXXLENS_SQLITE_TEST_SUPPORT)
	void sqlite_writer_shm_generation_epoch_authority::invalidate_for_testing() noexcept
	{
		if (state_)
			state_->invalidate_for_testing();
	}
#endif

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

	bool sqlite_writer_shm_mapping_epoch_arm::valid_for_predelegation(
		const sqlite_shm_writer_map_request& request) const noexcept
	{
		return state_ && state_->valid_for_predelegation(request);
	}

	bool sqlite_writer_shm_mapping_epoch_arm::retains_exact_lifetimes(
		const sqlite_shm_writer_map_request& request) const noexcept
	{
		return state_ && state_->retains_exact_lifetimes(request);
	}

	bool sqlite_writer_shm_mapping_epoch_arm::target_identity_matches(
		const sqlite_shm_reader_attachment_target_identity& target,
		const sqlite_shm_verified_writer_post_map_receipt& receipt) const noexcept
	{
		return state_ && state_->target_identity_matches(target, receipt);
	}

	bool sqlite_writer_shm_mapping_epoch_arm::retains_exact_validated_receipt(
		const sqlite_shm_verified_writer_post_map_receipt& receipt) const noexcept
	{
		return matches_validated_receipt(receipt) &&
			state_->retains_exact_lifetimes(receipt.request()) &&
			state_->authoritative_validation_still_live(receipt.epoch_seal_sequence_);
	}

	bool sqlite_writer_shm_mapping_epoch_arm::matches_validated_receipt(
		const sqlite_shm_verified_writer_post_map_receipt& receipt) const noexcept
	{
		const auto receipt_state = receipt.epoch_state_.lock();
		return state_ && receipt_state.get() == state_.get() && receipt.epoch_seal_sequence_ != 0U;
	}

	bool sqlite_writer_shm_mapping_epoch_arm::attachment_cohort_compatible_with(
		const sqlite_writer_shm_mapping_epoch_arm& other) const noexcept
	{
		return state_ && other.state_ && state_->attachment_cohort_compatible_with(*other.state_);
	}

	result<sqlite_shm_writer_reader_borrow_mint_capability>
	sqlite_writer_shm_mapping_epoch_arm::reserve_reader_borrow_mint(
		const std::uint64_t map_token,
		const std::uint64_t generation,
		const std::uint64_t holder_token) const
	{
		if (!state_ || !state_->lifetimes_valid() || map_token == 0U || generation == 0U ||
			holder_token == 0U)
			return unexpected(
				error{"store.backend-unavailable", "sqlite", "source-shm-readonly-qualification"});
		return unexpected(
			error{"store.backend-unavailable", "sqlite", "source-shm-readonly-qualification"});
	}

	sqlite_writer_shm_generation_epoch_authority
	sqlite_writer_shm_mapping_epoch_arm::make_generation_authority() const noexcept
	{
		return sqlite_writer_shm_generation_epoch_authority{state_, state_->generation_custody()};
	}

	sqlite_shm_writer_reader_borrow_mint_capability::
		sqlite_shm_writer_reader_borrow_mint_capability(
			std::shared_ptr<detail::sqlite_writer_shm_generation_epoch_custody> custody,
			const sqlite_shm_writer_reader_borrow_tokens tokens) noexcept
		: custody_{std::move(custody)}, map_token_{tokens.map_token},
		  generation_{tokens.generation}, holder_token_{tokens.holder_token}
	{
	}

	sqlite_shm_writer_reader_borrow_mint_capability::
		~sqlite_shm_writer_reader_borrow_mint_capability() noexcept = default;

	sqlite_shm_writer_reader_borrow_mint_capability::
		sqlite_shm_writer_reader_borrow_mint_capability(
			sqlite_shm_writer_reader_borrow_mint_capability&&) noexcept = default;

	result<sqlite_source_shm_target_namespace_epoch_reader_borrow>
	sqlite_shm_writer_reader_borrow_mint_capability::mint(
		const sqlite_shm_reader_native_ok_projection_reservation& reservation)
	{
		if (!custody_ || !reservation.matches(map_token_, generation_, holder_token_))
			return unexpected(
				error{"store.backend-unavailable", "sqlite", "source-shm-readonly-qualification"});
		auto output = custody_->mint(reservation);
		if (output)
			disarm();
		return output;
	}

	void sqlite_shm_writer_reader_borrow_mint_capability::disarm() noexcept
	{
		map_token_ = 0U;
		generation_ = 0U;
		holder_token_ = 0U;
		custody_.reset();
	}

#if defined(CXXLENS_SQLITE_TEST_SUPPORT)
	void sqlite_writer_shm_mapping_epoch_arm::invalidate_for_testing() noexcept
	{
		if (state_)
			state_->invalidate_for_testing();
	}
#endif

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

	sqlite_shm_lease_result<std::shared_ptr<detail::sqlite_writer_shm_mapping_epoch_state>>
	sqlite_writer_shm_mapping_epoch_receipt::begin_authoritative_validation() const noexcept
	{
		const auto state = state_.lock();
		if (!state)
			return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
							 sqlite_shm_lease_recovery_action::quarantine_no_retry);
		try
		{
			auto begun = state->begin_authoritative_validation(seal_sequence_);
			if (!begun)
				return begun.error();
			return state;
		}
		catch (...)
		{
			return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
							 sqlite_shm_lease_recovery_action::quarantine_no_retry);
		}
	}

	bool sqlite_writer_shm_mapping_epoch_receipt::authoritative_validation_still_live(
		const std::shared_ptr<detail::sqlite_writer_shm_mapping_epoch_state>& state) const noexcept
	{
		const auto current = state_.lock();
		return state && current.get() == state.get() &&
			state->authoritative_validation_still_live(seal_sequence_);
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

	sqlite_retained_namespace_writer_shm_mapping_epoch_port::
		sqlite_retained_namespace_writer_shm_mapping_epoch_port(
			sqlite_writer_shm_mapping_epoch_platform_binding binding) noexcept
		: binding_{std::move(binding)}
	{
	}

	sqlite_shm_lease_result<sqlite_writer_shm_mapping_epoch_preparation>
	sqlite_retained_namespace_writer_shm_mapping_epoch_port::arm_watch_before_pre_stat(
		const sqlite_writer_shm_mapping_epoch_request& request)
	{
		try
		{
			if (!valid_platform_binding(binding_))
				return rejection(sqlite_shm_lease_rejection_reason::invalid_identity,
								 sqlite_shm_lease_recovery_action::deny_before_native_map);
			if (!classify_sqlite_shm_writer_extend_pair(request.binding.map_request.caller_extend,
														request.binding.delegated_extend))
				return rejection(sqlite_shm_lease_rejection_reason::invalid_request,
								 sqlite_shm_lease_recovery_action::deny_before_native_map);

			const auto sequence = next_arm_sequence_.fetch_add(1U, std::memory_order_relaxed);
			if (sequence == 0U)
				return rejection(sqlite_shm_lease_rejection_reason::generation_exhausted,
								 sqlite_shm_lease_recovery_action::deny_before_native_map);

			// The target epoch owns the retained parent ancestry watch. Rechecking it here is the
			// watch-arm boundary; no pathname resolution or duplicate descriptor is permitted.
			if (auto checked = binding_.target_namespace_epoch->recheck(); !checked)
				return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								 sqlite_shm_lease_recovery_action::deny_before_native_map);
			auto epoch_identity =
				retained_epoch_receipt("epoch", binding_, request.binding, sequence);
			auto watch_receipt =
				retained_epoch_receipt("watch", binding_, request.binding, sequence);
			if (!valid_identity(epoch_identity) || !valid_identity(watch_receipt) ||
				epoch_identity == watch_receipt)
				return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
								 sqlite_shm_lease_recovery_action::deny_before_native_map);

			auto pre_stat = observe_retained_shm(binding_);
			if (!pre_stat)
				return pre_stat.error();
			if (pre_stat->state != sqlite_writer_shm_entry_state::direct_regular &&
				pre_stat->state != sqlite_writer_shm_entry_state::absent)
				return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
								 sqlite_shm_lease_recovery_action::deny_before_native_map);

			auto observer =
				std::make_shared<retained_namespace_writer_shm_mapping_epoch_observation>(
					binding_, request.binding, *pre_stat, watch_receipt);
			std::optional<sqlite_source_shm_target_namespace_epoch_borrow_minter> borrow_minter;
			if (auto minted = make_sqlite_source_shm_target_namespace_epoch_borrow_minter(
					binding_.target_namespace_epoch);
				minted)
				borrow_minter.emplace(std::move(*minted));
			else if (request.binding.target_namespace_epoch_identity)
				return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								 sqlite_shm_lease_recovery_action::deny_before_native_map);
			return sqlite_writer_shm_mapping_epoch_preparation{std::move(epoch_identity),
															   std::move(watch_receipt),
															   std::move(*pre_stat),
															   std::move(observer),
															   std::move(borrow_minter)};
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
