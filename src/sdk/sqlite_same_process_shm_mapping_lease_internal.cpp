#include "sqlite_same_process_shm_mapping_lease_internal.hpp"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <limits>
#include <list>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

#include "sqlite_same_process_shm_mapping_registry_internal.hpp"

namespace cxxlens::sdk
{
	namespace
	{
		/** Stable SQLite ABI value of SQLITE_OK; this unit intentionally has no SQLite header. */
		enum class sqlite_native_map_status : int
		{
			ok = 0,
		};

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

		[[nodiscard]] bool valid_mapping(const sqlite_shm_mapping_tuple& mapping) noexcept
		{
			if (mapping.page_number < 0 || mapping.page_size <= 0 ||
				mapping.native_mapping == nullptr)
				return false;
			const auto page = static_cast<std::uint64_t>(mapping.page_number);
			const auto size = static_cast<std::uint64_t>(mapping.page_size);
			if (page > std::numeric_limits<std::uint64_t>::max() / size)
				return false;
			const auto offset = page * size;
			if (offset > std::numeric_limits<std::uint64_t>::max() - size)
				return false;
			return mapping.byte_offset == offset && mapping.byte_count == size &&
				mapping.sealed_shm_size >= offset + size;
		}

		[[nodiscard]] bool same_mapping_page(const sqlite_shm_mapping_tuple& left,
											 const sqlite_shm_mapping_tuple& right) noexcept
		{
			return left.page_number == right.page_number && left.page_size == right.page_size &&
				left.byte_offset == right.byte_offset && left.byte_count == right.byte_count &&
				left.native_mapping == right.native_mapping;
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
				valid_callback(request.callback) && request.page_number >= 0 &&
				request.page_size > 0 && (request.caller_extend == 0 || request.caller_extend == 1);
		}

		[[nodiscard]] bool
		valid_reader_request(const sqlite_shm_reader_map_request& request) noexcept
		{
			return valid_family(request.family) && valid_identity(request.alias_lifetime) &&
				valid_identity(request.connection_token) && valid_callback(request.callback) &&
				request.page_number >= 0 && request.page_size > 0 && request.caller_extend == 0;
		}

		[[nodiscard]] sqlite_shm_lease_rejection
		rejection(const sqlite_shm_lease_rejection_reason reason,
				  const sqlite_shm_lease_recovery_action action) noexcept
		{
			return {reason, action};
		}

		[[nodiscard]] sqlite_shm_lease_rejection
		sqlite_shm_unexpected(sqlite_shm_lease_rejection failure) noexcept
		{
			return failure;
		}

		enum class generation_failure : std::uint8_t
		{
			exhausted,
			unavailable,
		};

		struct generation_mint_result
		{
			std::uint64_t value{};
			generation_failure failure{generation_failure::unavailable};
			bool succeeded{};

			[[nodiscard]] explicit operator bool() const noexcept
			{
				return succeeded;
			}
			[[nodiscard]] std::uint64_t operator*() const noexcept
			{
				return value;
			}
			[[nodiscard]] generation_failure error() const noexcept
			{
				return failure;
			}
		};

		enum class lease_token_kind : std::uint8_t
		{
			eligibility,
			writer_inflight,
			writer_post_native,
			pending,
			writer_cleanup,
			holder,
			reader_inflight,
			reader_cleanup,
			handoff,
			reader_unmap,
		};
	} // namespace

	std::optional<sqlite_shm_native_attachment_identity>
	sqlite_shm_native_attachment_identity::bind(
		sqlite_shm_lease_family_binding family,
		sqlite_backend_opaque_identity alias_lifetime,
		sqlite_backend_opaque_identity connection_token,
		sqlite_backend_opaque_identity main_native_file_receipt,
		sqlite_backend_opaque_identity main_xopen_receipt,
		sqlite_backend_opaque_identity open_epoch,
		sqlite_backend_opaque_identity callback_cohort,
		sqlite_backend_opaque_identity attachment_epoch)
	{
		if (!valid_family(family) || !valid_identity(alias_lifetime) ||
			!valid_identity(connection_token) || !valid_identity(main_native_file_receipt) ||
			!valid_identity(main_xopen_receipt) || !valid_identity(open_epoch) ||
			!valid_identity(callback_cohort) || !valid_identity(attachment_epoch))
			return std::nullopt;
		return sqlite_shm_native_attachment_identity{std::move(family),
													 std::move(alias_lifetime),
													 std::move(connection_token),
													 std::move(main_native_file_receipt),
													 std::move(main_xopen_receipt),
													 std::move(open_epoch),
													 std::move(callback_cohort),
													 std::move(attachment_epoch)};
	}

	sqlite_shm_native_attachment_identity::sqlite_shm_native_attachment_identity(
		sqlite_shm_lease_family_binding family,
		sqlite_backend_opaque_identity alias_lifetime,
		sqlite_backend_opaque_identity connection_token,
		sqlite_backend_opaque_identity main_native_file_receipt,
		sqlite_backend_opaque_identity main_xopen_receipt,
		sqlite_backend_opaque_identity open_epoch,
		sqlite_backend_opaque_identity callback_cohort,
		sqlite_backend_opaque_identity attachment_epoch)
		: family_{std::move(family)}, alias_lifetime_{std::move(alias_lifetime)},
		  connection_token_{std::move(connection_token)},
		  main_native_file_receipt_{std::move(main_native_file_receipt)},
		  main_xopen_receipt_{std::move(main_xopen_receipt)}, open_epoch_{std::move(open_epoch)},
		  callback_cohort_{std::move(callback_cohort)},
		  attachment_epoch_{std::move(attachment_epoch)}
	{
	}

	const sqlite_shm_lease_family_binding&
	sqlite_shm_native_attachment_identity::family() const noexcept
	{
		return family_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_native_attachment_identity::alias_lifetime() const noexcept
	{
		return alias_lifetime_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_native_attachment_identity::connection_token() const noexcept
	{
		return connection_token_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_native_attachment_identity::main_native_file_receipt() const noexcept
	{
		return main_native_file_receipt_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_native_attachment_identity::main_xopen_receipt() const noexcept
	{
		return main_xopen_receipt_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_native_attachment_identity::open_epoch() const noexcept
	{
		return open_epoch_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_native_attachment_identity::callback_cohort() const noexcept
	{
		return callback_cohort_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_native_attachment_identity::attachment_epoch() const noexcept
	{
		return attachment_epoch_;
	}

	std::optional<sqlite_shm_writer_extend_pair>
	classify_sqlite_shm_writer_extend_pair(const int caller_extend,
										   const int delegated_extend) noexcept
	{
		if (caller_extend == 1 && delegated_extend == 1)
			return sqlite_shm_writer_extend_pair::one_one;
		if (caller_extend == 0 && delegated_extend == 0)
			return sqlite_shm_writer_extend_pair::zero_zero;
		return std::nullopt;
	}

	sqlite_shm_verified_writer_native_map_receipt::sqlite_shm_verified_writer_native_map_receipt(
		const sqlite_shm_writer_map_inflight& inflight,
		const volatile void* native_mapping) noexcept
		: state_{inflight.state_}, token_{inflight.token_}, native_mapping_{native_mapping}
	{
	}

	const volatile void*
	sqlite_shm_verified_writer_native_map_receipt::native_mapping() const noexcept
	{
		return native_mapping_;
	}

	sqlite_shm_lease_result<sqlite_shm_verified_writer_native_map_receipt>
	sqlite_writer_shm_native_map_receipt_validator::validate(
		sqlite_shm_writer_map_inflight& inflight,
		const int native_status,
		const volatile void* native_mapping) noexcept
	{
		if (!inflight.valid())
			return sqlite_shm_unexpected(rejection(
				sqlite_shm_lease_rejection_reason::stale_token,
				native_mapping == nullptr ? sqlite_shm_lease_recovery_action::outer_ioerr_no_retry
										  : sqlite_shm_lease_recovery_action::quarantine_no_retry));

		// Validation is one-shot even for a null result. A later non-null replay must additionally
		// latch mapping observation so it can never be erased through the no-map transition.
		const auto already_validated = std::exchange(inflight.native_result_validated_, true);
		if (native_mapping != nullptr)
			inflight.native_result_observed_ = true;
		if (already_validated)
		{
			inflight.native_result_validation_ambiguous_ = true;
			return sqlite_shm_unexpected(
				rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
						  sqlite_shm_lease_recovery_action::quarantine_no_retry));
		}

		const auto native_ok = native_status == static_cast<int>(sqlite_native_map_status::ok);
		if (native_mapping == nullptr)
			return sqlite_shm_unexpected(
				rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
						  sqlite_shm_lease_recovery_action::outer_ioerr_no_retry));

		if (!native_ok)
			return sqlite_shm_unexpected(rejection(
				sqlite_shm_lease_rejection_reason::receipt_mismatch,
				sqlite_shm_lease_recovery_action::attempt_nonremoving_unmap_then_outer_ioerr));

		return sqlite_shm_verified_writer_native_map_receipt{inflight, native_mapping};
	}

	sqlite_shm_verified_writer_post_map_receipt::sqlite_shm_verified_writer_post_map_receipt(
		sqlite_shm_writer_map_request request,
		sqlite_backend_opaque_identity open_epoch,
		sqlite_shm_mapping_tuple mapping,
		const sqlite_shm_writer_extend_pair extend_pair,
		sqlite_backend_opaque_identity holder_specific_effect_receipt)
		: request_{std::move(request)}, open_epoch_{std::move(open_epoch)}, mapping_{mapping},
		  extend_pair_{extend_pair},
		  holder_specific_effect_receipt_{std::move(holder_specific_effect_receipt)}
	{
	}

	sqlite_shm_verified_writer_post_map_receipt::sqlite_shm_verified_writer_post_map_receipt(
		sqlite_shm_writer_map_request request,
		sqlite_backend_opaque_identity open_epoch,
		sqlite_shm_mapping_tuple mapping,
		const sqlite_shm_writer_extend_pair extend_pair,
		sqlite_backend_opaque_identity holder_specific_effect_receipt,
		std::weak_ptr<detail::sqlite_writer_shm_mapping_epoch_state> epoch_state,
		const std::uint64_t epoch_seal_sequence)
		: request_{std::move(request)}, open_epoch_{std::move(open_epoch)}, mapping_{mapping},
		  extend_pair_{extend_pair},
		  holder_specific_effect_receipt_{std::move(holder_specific_effect_receipt)},
		  epoch_state_{std::move(epoch_state)}, epoch_seal_sequence_{epoch_seal_sequence}
	{
	}

	const sqlite_shm_writer_map_request&
	sqlite_shm_verified_writer_post_map_receipt::request() const noexcept
	{
		return request_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_verified_writer_post_map_receipt::open_epoch() const noexcept
	{
		return open_epoch_;
	}

	const sqlite_shm_mapping_tuple&
	sqlite_shm_verified_writer_post_map_receipt::mapping() const noexcept
	{
		return mapping_;
	}

	sqlite_shm_writer_extend_pair
	sqlite_shm_verified_writer_post_map_receipt::extend_pair() const noexcept
	{
		return extend_pair_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_verified_writer_post_map_receipt::holder_specific_effect_receipt() const noexcept
	{
		return holder_specific_effect_receipt_;
	}

	sqlite_shm_verified_writer_eligibility_receipt::sqlite_shm_verified_writer_eligibility_receipt(
		sqlite_shm_lease_family_binding family,
		sqlite_backend_opaque_identity connection_token,
		sqlite_backend_opaque_identity open_epoch,
		sqlite_backend_effect_arm_receipt effect_gate,
		sqlite_backend_opaque_identity complete_current_v3_gate)
		: family_{std::move(family)}, connection_token_{std::move(connection_token)},
		  open_epoch_{std::move(open_epoch)}, effect_gate_{std::move(effect_gate)},
		  complete_current_v3_gate_{std::move(complete_current_v3_gate)}
	{
	}

	const sqlite_shm_lease_family_binding&
	sqlite_shm_verified_writer_eligibility_receipt::family() const noexcept
	{
		return family_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_verified_writer_eligibility_receipt::connection_token() const noexcept
	{
		return connection_token_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_verified_writer_eligibility_receipt::open_epoch() const noexcept
	{
		return open_epoch_;
	}

	const sqlite_backend_effect_arm_receipt&
	sqlite_shm_verified_writer_eligibility_receipt::effect_gate() const noexcept
	{
		return effect_gate_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_verified_writer_eligibility_receipt::complete_current_v3_gate() const noexcept
	{
		return complete_current_v3_gate_;
	}

	sqlite_shm_verified_reader_post_map_receipt::sqlite_shm_verified_reader_post_map_receipt(
		sqlite_shm_reader_map_request request,
		const std::uint64_t generation,
		sqlite_shm_mapping_tuple mapping,
		sqlite_backend_opaque_identity zero_resize_effect_receipt)
		: request_{std::move(request)}, generation_{generation}, mapping_{mapping},
		  zero_resize_effect_receipt_{std::move(zero_resize_effect_receipt)}
	{
	}

	const sqlite_shm_reader_map_request&
	sqlite_shm_verified_reader_post_map_receipt::request() const noexcept
	{
		return request_;
	}

	std::uint64_t sqlite_shm_verified_reader_post_map_receipt::generation() const noexcept
	{
		return generation_;
	}

	const sqlite_shm_mapping_tuple&
	sqlite_shm_verified_reader_post_map_receipt::mapping() const noexcept
	{
		return mapping_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_verified_reader_post_map_receipt::zero_resize_effect_receipt() const noexcept
	{
		return zero_resize_effect_receipt_;
	}

	struct sqlite_shm_mapping_generation_source::state
	{
		explicit state(const std::uint64_t first) : next{first}, exhausted{first == 0U} {}

		[[nodiscard]] generation_mint_result mint() noexcept
		{
			if (unavailable.load(std::memory_order_acquire))
				return {0U, generation_failure::unavailable, false};
			try
			{
				std::scoped_lock lock{mutex};
				if (unavailable.load(std::memory_order_relaxed))
					return {0U, generation_failure::unavailable, false};
				if (exhausted)
					return {0U, generation_failure::exhausted, false};
				const auto output = next;
				if (next == std::numeric_limits<std::uint64_t>::max())
					exhausted = true;
				else
					++next;
				return {output, generation_failure::unavailable, true};
			}
			catch (...)
			{
				unavailable.store(true, std::memory_order_release);
				return {0U, generation_failure::unavailable, false};
			}
		}

		std::mutex mutex;
		std::uint64_t next{};
		bool exhausted{};
		std::atomic_bool unavailable{false};
	};

	sqlite_shm_mapping_generation_source::sqlite_shm_mapping_generation_source(
		const std::uint64_t first_generation)
		: state_{std::make_shared<state>(first_generation)}
	{
	}

	sqlite_shm_mapping_generation_source::~sqlite_shm_mapping_generation_source() = default;

	namespace detail
	{
		class sqlite_shm_mapping_lease_state final
			: public std::enable_shared_from_this<sqlite_shm_mapping_lease_state>
		{
		  public:
			sqlite_shm_mapping_lease_state(
				sqlite_shm_lease_family_binding family,
				std::shared_ptr<sqlite_shm_mapping_generation_source> generations)
				: family_{std::move(family)}, generations_{std::move(generations)}
			{
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_writer_eligibility>
			install_eligibility(const sqlite_shm_verified_writer_eligibility_receipt& receipt)
			{
				if (!valid_eligibility(receipt))
					return sqlite_shm_unexpected(
						rejection(sqlite_shm_lease_rejection_reason::invalid_identity,
								  sqlite_shm_lease_recovery_action::deny_before_native_map));
				try
				{
					std::scoped_lock lock{mutex_};
					if (auto blocked = blocked_locked(
							sqlite_shm_lease_recovery_action::deny_before_native_map))
						return sqlite_shm_unexpected(*blocked);
					if (receipt.family() != family_)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					const auto token = allocate_token_locked();
					if (!token)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									  sqlite_shm_lease_recovery_action::quarantine_no_retry));
					eligibilities_.push_back({*token, receipt});
					return sqlite_shm_writer_eligibility{shared_from_this(), *token};
				}
				catch (...)
				{
					return sqlite_shm_unexpected(
						rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								  sqlite_shm_lease_recovery_action::deny_before_native_map));
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<void>
			revoke_eligibility(sqlite_shm_writer_eligibility& eligibility) noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					if (!owns(eligibility.state_, eligibility.token_))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::deny_before_native_map));
					const auto found = find_by_token(eligibilities_, eligibility.token_);
					if (found == eligibilities_.end())
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::deny_before_native_map));
					eligibilities_.erase(found);
					eligibility.disarm();
					return {};
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_writer_map_inflight>
			begin_writer(const sqlite_shm_writer_map_request& request)
			{
				return begin_writer(request, nullptr);
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_writer_map_inflight>
			begin_registry_writer(const sqlite_shm_writer_map_request& request,
								  sqlite_shm_writer_member_authority& authority)
			{
				if (!authority.valid_for_predelegation(request))
					return sqlite_shm_unexpected(
						rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
								  sqlite_shm_lease_recovery_action::deny_before_native_map));
				return begin_writer(request, &authority);
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_writer_map_inflight>
			begin_writer(const sqlite_shm_writer_map_request& request,
						 sqlite_shm_writer_member_authority* const authority)
			{
				if (!valid_writer_request(request))
					return sqlite_shm_unexpected(
						rejection((request.caller_extend == 0 || request.caller_extend == 1)
									  ? sqlite_shm_lease_rejection_reason::invalid_request
									  : sqlite_shm_lease_rejection_reason::invalid_extend_pair,
								  sqlite_shm_lease_recovery_action::deny_before_native_map));
				try
				{
					std::scoped_lock lock{mutex_};
					if (authority != nullptr && !authority->valid_for_predelegation(request))
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					if (auto blocked = blocked_locked(
							sqlite_shm_lease_recovery_action::deny_before_native_map))
						return sqlite_shm_unexpected(*blocked);
					if (request.family != family_)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					auto route = writer_route::map_before_gate;
					std::uint64_t positive_gate_token{};
					const auto attachment = find_attachment_epoch_locked(request.attachment);
					if (attachment != writer_attachments_.end())
					{
						if (attachment->identity != request.attachment)
							return sqlite_shm_unexpected(rejection(
								sqlite_shm_lease_rejection_reason::receipt_mismatch,
								sqlite_shm_lease_recovery_action::deny_before_native_map));
						if (attachment->phase == writer_attachment_phase::retired)
							return sqlite_shm_unexpected(stale_token(
								sqlite_shm_lease_recovery_action::deny_before_native_map));
						if (attachment->phase != writer_attachment_phase::collecting)
							return sqlite_shm_unexpected(rejection(
								sqlite_shm_lease_rejection_reason::retiring,
								sqlite_shm_lease_recovery_action::deny_before_native_map));
						if (attachment->gate_phase ==
							writer_attachment_gate_phase::positive_sealing)
						{
							const auto action = exact_live_positive_gate_binding_locked(*attachment)
								? sqlite_shm_lease_recovery_action::
									  await_complete_attachment_gate_boundary
								: sqlite_shm_lease_recovery_action::deny_before_native_map;
							return sqlite_shm_unexpected(rejection(
								sqlite_shm_lease_rejection_reason::pending_or_eligibility_only,
								action));
						}
						if (attachment->registry_bound_origin != (authority != nullptr))
							return sqlite_shm_unexpected(rejection(
								sqlite_shm_lease_rejection_reason::receipt_mismatch,
								sqlite_shm_lease_recovery_action::deny_before_native_map));
						if (attachment->gate_phase == writer_attachment_gate_phase::positive_active)
						{
							if (authority == nullptr ||
								!exact_live_positive_gate_binding_locked(*attachment))
								return sqlite_shm_unexpected(rejection(
									sqlite_shm_lease_rejection_reason::pending_or_eligibility_only,
									sqlite_shm_lease_recovery_action::deny_before_native_map));
							route = writer_route::gate_before_map;
							positive_gate_token = attachment->positive_gate_token;
						}
					}
					else if (has_nonretired_native_attachment_lineage_locked(request.attachment))
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					if (generation_)
					{
						if (generation_->phase == sqlite_shm_mapping_generation_phase::retiring)
							return sqlite_shm_unexpected(rejection(
								generation_->handoff_count == 0U
									? sqlite_shm_lease_rejection_reason::retiring
									: sqlite_shm_lease_rejection_reason::successor_handoff_live,
								sqlite_shm_lease_recovery_action::deny_before_native_map));
						if (generation_->phase == sqlite_shm_mapping_generation_phase::retired)
							return sqlite_shm_unexpected(rejection(
								sqlite_shm_lease_rejection_reason::successor_handoff_live,
								sqlite_shm_lease_recovery_action::deny_before_native_map));
					}
					if (!callback_can_start_locked(request.callback))
					{
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto token = allocate_token_locked();
					if (!token)
						return sqlite_shm_unexpected(ambiguous());
					writers_.push_back({*token,
										writer_phase::inflight,
										request,
										nullptr,
										std::nullopt,
										authority != nullptr,
										std::nullopt,
										route,
										positive_gate_token});
					try
					{
						register_attachment_member_locked(
							request.attachment, *token, authority != nullptr);
					}
					catch (...)
					{
						writers_.pop_back();
						throw;
					}
					if (authority != nullptr)
					{
						const auto incompatible_attachment_cohort = std::ranges::any_of(
							writers_,
							[token, &request, authority](const writer_record& writer)
							{
								return writer.token != *token && writer.registry_bound &&
									writer.request.attachment == request.attachment &&
									(!writer.member_authority ||
									 !authority->attachment_cohort_compatible_with(
										 *writer.member_authority));
							});
						if (incompatible_attachment_cohort)
						{
							(void)release_attachment_member_locked(*token, false);
							writers_.pop_back();
							return sqlite_shm_unexpected(rejection(
								sqlite_shm_lease_rejection_reason::receipt_mismatch,
								sqlite_shm_lease_recovery_action::deny_before_native_map));
						}
						if (std::exchange(fail_next_registry_writer_incoming_liveness_for_testing_,
										  false))
							authority->invalidate_epoch_for_testing();
						if (std::exchange(fail_next_registry_writer_existing_liveness_for_testing_,
										  false))
						{
							const auto existing =
								std::find_if(writers_.begin(),
											 writers_.end(),
											 [token](const writer_record& writer)
											 {
												 return writer.token != *token &&
													 writer.registry_bound &&
													 writer.member_authority.has_value();
											 });
							if (existing != writers_.end())
								existing->member_authority->invalidate_epoch_for_testing();
						}
						const auto existing_liveness_lost = std::ranges::any_of(
							writers_,
							[token](const writer_record& writer)
							{
								return writer.token != *token && writer.registry_bound &&
									(!writer.member_authority ||
									 !writer.member_authority->retains_exact_lifetimes(
										 writer.request));
							});
						if (existing_liveness_lost)
						{
							registry_member_admission_blocked_ = true;
							registry_member_sticky_quarantine_ = true;
							(void)release_attachment_member_locked(*token, false);
							writers_.pop_back();
							return sqlite_shm_unexpected(rejection(
								sqlite_shm_lease_rejection_reason::quarantined,
								sqlite_shm_lease_recovery_action::deny_before_native_map));
						}
						if (!authority->valid_for_predelegation(request))
						{
							(void)release_attachment_member_locked(*token, false);
							writers_.pop_back();
							return sqlite_shm_unexpected(rejection(
								sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								sqlite_shm_lease_recovery_action::deny_before_native_map));
						}
						static_assert(std::is_nothrow_move_constructible_v<
									  sqlite_shm_writer_member_authority>);
						writers_.back().member_authority.emplace(std::move(*authority));
					}
					return sqlite_shm_writer_map_inflight{shared_from_this(), *token};
				}
				catch (...)
				{
					return sqlite_shm_unexpected(
						rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								  sqlite_shm_lease_recovery_action::deny_before_native_map));
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_writer_post_native_mapping>
			record_native_mapping(
				sqlite_shm_writer_map_inflight& inflight,
				const sqlite_shm_verified_writer_native_map_receipt& receipt) noexcept
			{
				// Calling this transition asserts that the native callback has already produced a
				// mapping. Set the token-local latch before any operation which can throw so even
				// an internal transition failure can never be resolved as a pre-native no-map.
				const auto replaces_validated_no_map =
					inflight.native_result_validated_ && !inflight.native_result_observed_;
				const auto validation_ambiguous = inflight.native_result_validation_ambiguous_;
				inflight.native_result_validated_ = true;
				inflight.native_result_observed_ = true;
				try
				{
					std::scoped_lock lock{mutex_};
					if (replaces_validated_no_map || validation_ambiguous)
					{
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					if (std::exchange(fail_next_writer_native_transition_for_testing_, false))
						throw writer_native_transition_injected_failure{};
					const auto receipt_state = receipt.state_.lock();
					const auto receipt_matches = receipt_state.get() == this &&
						receipt.token_ != 0U && receipt.native_mapping_ != nullptr;
					if (!owns(inflight.state_, inflight.token_))
					{
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto found = find_by_token(writers_, inflight.token_);
					if (found == writers_.end() || found->phase != writer_phase::inflight ||
						!receipt_matches || receipt.token_ != inflight.token_)
					{
						quarantine_locked();
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::quarantine_no_retry));
					}
					const auto token = inflight.token_;
					auto state = inflight.state_;
					if (found->registry_bound &&
						(!found->member_authority ||
						 !found->member_authority->retains_exact_lifetimes(found->request)))
						registry_member_admission_blocked_ = true;
					found->native_mapping = receipt.native_mapping_;
					found->phase = writer_phase::post_native_mapping;
					inflight.disarm();
					return sqlite_shm_writer_post_native_mapping{std::move(state), token};
				}
				catch (...)
				{
					quarantine_after_native();
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_pending_mapping>
			install_pending(sqlite_shm_writer_post_native_mapping& post_native,
							const sqlite_shm_verified_writer_post_map_receipt& receipt)
			{
				try
				{
					std::scoped_lock lock{mutex_};
					if (!owns(post_native.state_, post_native.token_))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::
											attempt_nonremoving_unmap_then_outer_ioerr));
					const auto found = find_by_token(writers_, post_native.token_);
					if (found == writers_.end() ||
						found->phase != writer_phase::post_native_mapping ||
						found->native_mapping == nullptr)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::
											attempt_nonremoving_unmap_then_outer_ioerr));
					if (found->registry_bound)
						return sqlite_shm_unexpected(rejection(
							sqlite_shm_lease_rejection_reason::pending_or_eligibility_only,
							sqlite_shm_lease_recovery_action::
								attempt_nonremoving_unmap_then_outer_ioerr));
					if (auto blocked =
							blocked_locked(sqlite_shm_lease_recovery_action::
											   attempt_nonremoving_unmap_then_outer_ioerr))
						return sqlite_shm_unexpected(*blocked);
					if (!valid_writer_receipt(receipt) || receipt.request() != found->request ||
						receipt.mapping().native_mapping != found->native_mapping)
					{
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::
										  attempt_nonremoving_unmap_then_outer_ioerr));
					}
					found->receipt = receipt;
					found->phase = writer_phase::pending;
					const auto token = post_native.token_;
					auto state = post_native.state_;
					post_native.disarm();
					return sqlite_shm_pending_mapping{std::move(state), token};
				}
				catch (...)
				{
					quarantine_after_native();
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_pending_mapping>
			install_registry_pending(sqlite_shm_registry_family_pin& family,
									 sqlite_shm_writer_post_native_mapping& post_native,
									 sqlite_shm_verified_writer_post_map_receipt receipt) noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					const auto cleanup_action = sqlite_shm_lease_recovery_action::
						attempt_nonremoving_unmap_then_outer_ioerr;
					if (!owns(post_native.state_, post_native.token_))
						return sqlite_shm_unexpected(stale_token(cleanup_action));
					const auto found = find_by_token(writers_, post_native.token_);
					if (found == writers_.end() ||
						found->phase != writer_phase::post_native_mapping ||
						found->native_mapping == nullptr)
						return sqlite_shm_unexpected(stale_token(cleanup_action));
					if (found->route == writer_route::gate_before_map)
						return sqlite_shm_unexpected(rejection(
							sqlite_shm_lease_rejection_reason::pending_or_eligibility_only,
							cleanup_action));
					if (!found->registry_bound || !found->member_authority)
						return sqlite_shm_unexpected(rejection(
							sqlite_shm_lease_rejection_reason::receipt_mismatch, cleanup_action));
					if (!valid_writer_receipt(receipt) || receipt.request() != found->request ||
						receipt.mapping().native_mapping != found->native_mapping)
						return sqlite_shm_unexpected(rejection(
							sqlite_shm_lease_rejection_reason::receipt_mismatch, cleanup_action));
					if (is_quarantined_locked() || !alive_ || !generations_ ||
						!generations_->state_)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::quarantined,
									  sqlite_shm_lease_recovery_action::quarantine_no_retry));

					auto authority = found->member_authority->validate_pending_authority(
						family, found->request, receipt);
					if (authority ==
						detail::sqlite_shm_writer_pending_authority_status::determinate_mismatch)
						return sqlite_shm_unexpected(rejection(
							sqlite_shm_lease_rejection_reason::receipt_mismatch, cleanup_action));
					if (authority != detail::sqlite_shm_writer_pending_authority_status::exact)
					{
						registry_member_admission_blocked_ = true;
						registry_member_sticky_quarantine_ = true;
						return sqlite_shm_unexpected(ambiguous());
					}

					if (std::exchange(fail_next_registry_writer_pending_liveness_for_testing_,
									  false))
						found->member_authority->invalidate_epoch_for_testing();
					authority = found->member_authority->validate_pending_authority(
						family, found->request, receipt);
					if (authority != detail::sqlite_shm_writer_pending_authority_status::exact)
					{
						if (authority ==
							detail::sqlite_shm_writer_pending_authority_status::lifecycle_ambiguous)
						{
							registry_member_admission_blocked_ = true;
							registry_member_sticky_quarantine_ = true;
							return sqlite_shm_unexpected(ambiguous());
						}
						return sqlite_shm_unexpected(rejection(
							sqlite_shm_lease_rejection_reason::receipt_mismatch, cleanup_action));
					}

					static_assert(std::is_nothrow_move_constructible_v<
								  sqlite_shm_verified_writer_post_map_receipt>);
					found->receipt.emplace(std::move(receipt));
					found->phase = writer_phase::pending;
					const auto token = post_native.token_;
					auto state = post_native.state_;
					post_native.disarm();
					return sqlite_shm_pending_mapping{std::move(state), token};
				}
				catch (...)
				{
					quarantine_after_native();
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]]
			sqlite_shm_lease_result<sqlite_shm_positive_writer_attachment_gate_result>
			advance_positive_registry_writer_attachment_gate(
				sqlite_shm_registry_family_pin& family,
				const sqlite_shm_native_attachment_identity& identity,
				const std::span<sqlite_shm_pending_mapping*> pending,
				const sqlite_shm_writer_eligibility& eligibility)
			{
				const auto boundary_action =
					sqlite_shm_lease_recovery_action::await_complete_attachment_gate_boundary;
				if (!valid_native_attachment(identity) || identity.family() != family_)
					return sqlite_shm_unexpected(rejection(
						sqlite_shm_lease_rejection_reason::receipt_mismatch, boundary_action));
				try
				{
					{
						std::scoped_lock lock{mutex_};
						if (auto blocked = blocked_locked(boundary_action))
							return sqlite_shm_unexpected(*blocked);
						if (!owns(eligibility.state_, eligibility.token_))
							return sqlite_shm_unexpected(stale_token(boundary_action));
						const auto gate = find_by_token(eligibilities_, eligibility.token_);
						if (gate == eligibilities_.end())
							return sqlite_shm_unexpected(stale_token(boundary_action));
						if (gate->receipt.family() != family_ ||
							gate->receipt.connection_token() != identity.connection_token() ||
							gate->receipt.open_epoch() != identity.open_epoch())
							return sqlite_shm_unexpected(
								rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										  boundary_action));

						auto attachment = find_attachment_epoch_locked(identity);
						const auto creates_attachment = attachment == writer_attachments_.end();
						if (!creates_attachment &&
							(attachment->identity != identity ||
							 attachment->phase != writer_attachment_phase::collecting ||
							 !attachment->registry_bound_origin))
							return sqlite_shm_unexpected(
								rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										  boundary_action));
						if (creates_attachment &&
							has_nonretired_native_attachment_lineage_locked(identity))
							return sqlite_shm_unexpected(
								rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										  boundary_action));
						const auto token_bound_elsewhere = std::ranges::any_of(
							writer_attachments_,
							[&attachment, &eligibility, creates_attachment](
								const writer_attachment_record& candidate)
							{
								return candidate.positive_gate_token == eligibility.token_ &&
									(creates_attachment || &candidate != &*attachment);
							});
						if (token_bound_elsewhere)
							return sqlite_shm_unexpected(
								rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										  boundary_action));

						for (std::size_t index = 0U; index < pending.size(); ++index)
						{
							auto* const supplied = pending[index];
							if (supplied == nullptr || !owns(supplied->state_, supplied->token_))
								return sqlite_shm_unexpected(stale_token(boundary_action));
							for (std::size_t previous = 0U; previous < index; ++previous)
							{
								if (pending[previous]->token_ == supplied->token_)
									return sqlite_shm_unexpected(rejection(
										sqlite_shm_lease_rejection_reason::invalid_request,
										boundary_action));
							}
							const auto writer = find_by_token(writers_, supplied->token_);
							if (writer == writers_.end())
								return sqlite_shm_unexpected(stale_token(boundary_action));
							if (creates_attachment || writer->phase != writer_phase::pending ||
								!writer->registry_bound ||
								writer->route != writer_route::map_before_gate ||
								writer->positive_gate_token != 0U ||
								writer->request.attachment != identity)
								return sqlite_shm_unexpected(rejection(
									sqlite_shm_lease_rejection_reason::pending_or_eligibility_only,
									boundary_action));
							const auto target_member = std::find_if(
								attachment->members.begin(),
								attachment->members.end(),
								[&writer](const writer_attachment_member_record& member)
								{
									return member.live_token == writer->token;
								});
							const auto target_member_count =
								static_cast<std::size_t>(std::ranges::count_if(
									attachment->members,
									[&writer](const writer_attachment_member_record& member)
									{
										return member.live_token == writer->token;
									}));
							if (target_member == attachment->members.end() ||
								target_member_count != 1U ||
								target_member->original_token != writer->token ||
								target_member->confirmed_native_cleanup)
								return sqlite_shm_unexpected(rejection(
									sqlite_shm_lease_rejection_reason::pending_or_eligibility_only,
									boundary_action));
						}

						const auto binds_gate = creates_attachment ||
							attachment->gate_phase == writer_attachment_gate_phase::unsealed;
						if (!creates_attachment &&
							attachment->gate_phase == writer_attachment_gate_phase::unsealed &&
							(attachment->promotion_gate_receipt ||
							 attachment->positive_gate_token != 0U))
						{
							quarantine_locked();
							return sqlite_shm_unexpected(ambiguous());
						}
						std::optional<sqlite_shm_verified_writer_eligibility_receipt> prepared_gate;
						if (binds_gate)
							prepared_gate.emplace(gate->receipt);
						if (creates_attachment)
						{
							writer_attachments_.push_back({identity,
														   true,
														   {},
														   writer_attachment_phase::collecting,
														   0U,
														   0U,
														   std::nullopt,
														   {},
														   {},
														   std::nullopt});
							attachment = std::prev(writer_attachments_.end());
						}
						if (attachment->gate_phase == writer_attachment_gate_phase::unsealed)
						{
							attachment->promotion_gate_receipt.emplace(std::move(*prepared_gate));
							attachment->positive_gate_token = eligibility.token_;
							attachment->gate_phase = writer_attachment_gate_phase::positive_sealing;
						}
						else if (attachment->gate_phase ==
									 writer_attachment_gate_phase::positive_active ||
								 attachment->positive_gate_token != eligibility.token_ ||
								 !exact_live_positive_gate_binding_locked(*attachment))
							return sqlite_shm_unexpected(rejection(
								sqlite_shm_lease_rejection_reason::pending_or_eligibility_only,
								boundary_action));

						const auto has_preboundary_native_attempt = std::ranges::any_of(
							attachment->members,
							[this](const writer_attachment_member_record& member)
							{
								const auto writer = find_by_token(writers_, member.live_token);
								return writer != writers_.end() &&
									(writer->phase == writer_phase::inflight ||
									 writer->phase == writer_phase::post_native_mapping);
							});
						if (has_preboundary_native_attempt)
							return sqlite_shm_positive_writer_attachment_gate_result{
								sqlite_shm_positive_writer_attachment_gate_progress::waiting, {}};
						if (attachment->members.empty())
						{
							attachment->gate_phase = writer_attachment_gate_phase::positive_active;
							return sqlite_shm_positive_writer_attachment_gate_result{
								sqlite_shm_positive_writer_attachment_gate_progress::complete, {}};
						}
					}

					auto holders =
						promote_registry_writer_attachment_group(family, pending, eligibility);
					if (!holders)
						return sqlite_shm_unexpected(holders.error());
					return sqlite_shm_positive_writer_attachment_gate_result{
						sqlite_shm_positive_writer_attachment_gate_progress::complete,
						std::move(*holders)};
				}
				catch (...)
				{
					quarantine_after_native();
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_writer_holder>
			complete_gate_winning_registry_writer_map_before_callback_return(
				sqlite_shm_registry_family_pin& family,
				sqlite_shm_writer_post_native_mapping& post_native,
				const sqlite_shm_verified_writer_post_map_receipt& receipt)
			{
				const auto cleanup_action =
					sqlite_shm_lease_recovery_action::attempt_nonremoving_unmap_then_outer_ioerr;
				try
				{
					auto state = shared_from_this();
					std::list<holder_record> prepared_holders;
					std::scoped_lock lock{mutex_};
					if (!owns(post_native.state_, post_native.token_))
						return sqlite_shm_unexpected(stale_token(cleanup_action));
					const auto writer = find_by_token(writers_, post_native.token_);
					if (writer == writers_.end() ||
						writer->phase != writer_phase::post_native_mapping ||
						writer->native_mapping == nullptr)
						return sqlite_shm_unexpected(stale_token(cleanup_action));
					if (writer->route != writer_route::gate_before_map ||
						writer->positive_gate_token == 0U || !writer->registry_bound ||
						!writer->member_authority)
						return sqlite_shm_unexpected(rejection(
							sqlite_shm_lease_rejection_reason::pending_or_eligibility_only,
							cleanup_action));
					if (auto blocked = blocked_locked(cleanup_action))
						return sqlite_shm_unexpected(*blocked);
					if (!valid_writer_receipt(receipt) || receipt.request() != writer->request ||
						receipt.mapping().native_mapping != writer->native_mapping)
						return sqlite_shm_unexpected(rejection(
							sqlite_shm_lease_rejection_reason::receipt_mismatch, cleanup_action));

					const auto attachment = find_attachment_token_locked(writer->token);
					if (attachment == writer_attachments_.end() ||
						attachment->identity != writer->request.attachment ||
						attachment->phase != writer_attachment_phase::collecting ||
						!attachment->registry_bound_origin ||
						attachment->gate_phase != writer_attachment_gate_phase::positive_active ||
						attachment->positive_gate_token != writer->positive_gate_token ||
						!attachment->promotion_gate_receipt)
						return sqlite_shm_unexpected(rejection(
							sqlite_shm_lease_rejection_reason::pending_or_eligibility_only,
							cleanup_action));
					const auto exact_binding_count = static_cast<std::size_t>(std::ranges::count_if(
						writer_attachments_,
						[&attachment](const writer_attachment_record& candidate)
						{
							return candidate.positive_gate_token == attachment->positive_gate_token;
						}));
					const auto gate =
						find_by_token(eligibilities_, attachment->positive_gate_token);
					if (exact_binding_count != 1U || gate == eligibilities_.end() ||
						!same_eligibility_receipt(*attachment->promotion_gate_receipt,
												  gate->receipt))
						return sqlite_shm_unexpected(rejection(
							sqlite_shm_lease_rejection_reason::pending_or_eligibility_only,
							cleanup_action));
					if (gate->receipt.family() != family_ ||
						gate->receipt.connection_token() != writer->request.connection_token ||
						gate->receipt.open_epoch() != receipt.open_epoch())
						return sqlite_shm_unexpected(rejection(
							sqlite_shm_lease_rejection_reason::receipt_mismatch, cleanup_action));
					auto attachment_member =
						std::find_if(attachment->members.begin(),
									 attachment->members.end(),
									 [&writer](const writer_attachment_member_record& member)
									 {
										 return member.live_token == writer->token;
									 });
					if (attachment_member == attachment->members.end() ||
						attachment_member->original_token == 0U)
					{
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}

					auto authority = writer->member_authority->validate_pending_authority(
						family, writer->request, receipt);
					if (authority ==
						detail::sqlite_shm_writer_pending_authority_status::determinate_mismatch)
						return sqlite_shm_unexpected(rejection(
							sqlite_shm_lease_rejection_reason::receipt_mismatch, cleanup_action));
					if (authority != detail::sqlite_shm_writer_pending_authority_status::exact ||
						!writer->member_authority->retains_exact_lifetimes(writer->request))
					{
						registry_member_admission_blocked_ = true;
						registry_member_sticky_quarantine_ = true;
						return sqlite_shm_unexpected(ambiguous());
					}
					if (generation_ &&
						generation_->phase != sqlite_shm_mapping_generation_phase::live)
						return sqlite_shm_unexpected(rejection(
							generation_->handoff_count == 0U
								? sqlite_shm_lease_rejection_reason::retiring
								: sqlite_shm_lease_rejection_reason::successor_handoff_live,
							cleanup_action));

					generation_record candidate;
					if (generation_)
					{
						candidate = *generation_;
						if (!join_mapping(candidate, receipt))
							return sqlite_shm_unexpected(
								rejection(sqlite_shm_lease_rejection_reason::mapping_mismatch,
										  cleanup_action));
					}
					else
					{
						candidate.phase = sqlite_shm_mapping_generation_phase::live;
						candidate.sealed_shm_size = receipt.mapping().sealed_shm_size;
						candidate.pages.push_back(receipt.mapping());
					}
					candidate.authorities.push_back({receipt, gate->receipt, true});
					prepared_holders.push_back(
						{0U, 0U, holder_phase::active, receipt, gate->receipt, true, std::nullopt});
					if (!can_allocate_tokens_locked(1U))
					{
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}

					std::uint64_t generation = candidate.value;
					if (!generation_)
					{
						auto minted = generations_->state_->mint();
						if (!minted)
						{
							if (minted.error() == generation_failure::unavailable)
								quarantine_locked();
							return sqlite_shm_unexpected(rejection(
								minted.error() == generation_failure::exhausted
									? sqlite_shm_lease_rejection_reason::generation_exhausted
									: sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								minted.error() == generation_failure::exhausted
									? cleanup_action
									: sqlite_shm_lease_recovery_action::quarantine_no_retry));
						}
						generation = *minted;
						candidate.value = generation;
					}

					if (std::exchange(fail_next_registry_writer_pending_liveness_for_testing_,
									  false))
						writer->member_authority->invalidate_epoch_for_testing();
					const auto final_gate =
						find_by_token(eligibilities_, writer->positive_gate_token);
					authority = writer->member_authority->validate_pending_authority(
						family, writer->request, receipt);
					if (attachment->gate_phase != writer_attachment_gate_phase::positive_active ||
						attachment->positive_gate_token != writer->positive_gate_token ||
						final_gate == eligibilities_.end() ||
						!same_eligibility_receipt(*attachment->promotion_gate_receipt,
												  final_gate->receipt) ||
						!writer->member_authority->retains_exact_lifetimes(writer->request) ||
						authority != detail::sqlite_shm_writer_pending_authority_status::exact)
					{
						registry_member_admission_blocked_ = true;
						registry_member_sticky_quarantine_ = true;
						return sqlite_shm_unexpected(ambiguous());
					}

					static_assert(std::is_nothrow_move_constructible_v<generation_record>);
					static_assert(std::is_nothrow_move_constructible_v<holder_record>);
					static_assert(
						std::is_nothrow_move_constructible_v<sqlite_shm_writer_member_authority>);
					const auto holder_token = allocate_token_unchecked_locked();
					prepared_holders.front().token = holder_token;
					prepared_holders.front().generation = generation;
					prepared_holders.front().member_authority.emplace(
						std::move(*writer->member_authority));
					attachment_member->live_token = holder_token;
					generation_.reset();
					generation_.emplace(std::move(candidate));
					holders_.splice(holders_.end(), prepared_holders);
					writers_.erase(writer);
					post_native.disarm();
					return sqlite_shm_writer_holder{
						std::move(state),
						sqlite_shm_lease_token_identity{holder_token},
						sqlite_shm_mapping_generation_identity{generation}};
				}
				catch (...)
				{
					quarantine_after_native();
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<std::vector<sqlite_shm_writer_holder>>
			promote_registry_writer_attachment_group(
				sqlite_shm_registry_family_pin& family,
				const std::span<sqlite_shm_pending_mapping*> pending,
				const sqlite_shm_writer_eligibility& eligibility)
			{
				const auto cleanup_action =
					sqlite_shm_lease_recovery_action::remove_pending_then_confirm_native_cleanup;
				const auto boundary_action =
					sqlite_shm_lease_recovery_action::await_complete_attachment_gate_boundary;
				if (pending.empty())
					return sqlite_shm_unexpected(
						rejection(sqlite_shm_lease_rejection_reason::pending_or_eligibility_only,
								  boundary_action));
				try
				{
					std::vector<std::uint64_t> supplied_tokens;
					supplied_tokens.reserve(pending.size());
					for (auto* const member : pending)
					{
						if (member == nullptr || !owns(member->state_, member->token_))
							return sqlite_shm_unexpected(stale_token(boundary_action));
						if (std::ranges::find(supplied_tokens, member->token_) !=
							supplied_tokens.end())
							return sqlite_shm_unexpected(
								rejection(sqlite_shm_lease_rejection_reason::invalid_request,
										  boundary_action));
						supplied_tokens.push_back(member->token_);
					}

					auto state = shared_from_this();
					std::vector<sqlite_shm_writer_holder> output;
					output.reserve(pending.size());
					std::vector<std::list<writer_record>::iterator> selected_writers;
					selected_writers.reserve(pending.size());
					std::vector<writer_attachment_member_record*> selected_members;
					selected_members.reserve(pending.size());
					std::list<holder_record> prepared_holders;

					std::scoped_lock lock{mutex_};
					if (auto blocked = blocked_locked(cleanup_action))
						return sqlite_shm_unexpected(*blocked);
					if (!owns(eligibility.state_, eligibility.token_))
						return sqlite_shm_unexpected(stale_token(boundary_action));
					const auto gate = find_by_token(eligibilities_, eligibility.token_);
					if (gate == eligibilities_.end())
						return sqlite_shm_unexpected(stale_token(boundary_action));

					const auto first_writer = find_by_token(writers_, supplied_tokens.front());
					if (first_writer == writers_.end())
						return sqlite_shm_unexpected(stale_token(boundary_action));
					const auto attachment = find_attachment_token_locked(first_writer->token);
					if (attachment == writer_attachments_.end() ||
						attachment->phase != writer_attachment_phase::collecting ||
						!attachment->registry_bound_origin ||
						attachment->members.size() != pending.size())
						return sqlite_shm_unexpected(rejection(
							sqlite_shm_lease_rejection_reason::pending_or_eligibility_only,
							boundary_action));
					const auto token_bound_elsewhere = std::ranges::any_of(
						writer_attachments_,
						[&attachment, &eligibility](const writer_attachment_record& candidate)
						{
							return &candidate != &*attachment &&
								candidate.positive_gate_token == eligibility.token_;
						});
					if (token_bound_elsewhere)
						return sqlite_shm_unexpected(rejection(
							sqlite_shm_lease_rejection_reason::receipt_mismatch, boundary_action));
					if (attachment->gate_phase != writer_attachment_gate_phase::positive_sealing ||
						attachment->positive_gate_token != eligibility.token_ ||
						!attachment->promotion_gate_receipt ||
						!same_eligibility_receipt(*attachment->promotion_gate_receipt,
												  gate->receipt))
						return sqlite_shm_unexpected(rejection(
							sqlite_shm_lease_rejection_reason::receipt_mismatch, boundary_action));
					const auto complete = derive_complete_attachment_prefix_locked(*attachment);
					if (!complete || complete->has_inflight || complete->has_active_holder ||
						!complete->has_writer_native_member ||
						complete->tokens.size() != supplied_tokens.size() ||
						std::ranges::any_of(supplied_tokens,
											[&complete](const std::uint64_t token)
											{
												return std::ranges::find(complete->tokens, token) ==
													complete->tokens.end();
											}))
						return sqlite_shm_unexpected(rejection(
							sqlite_shm_lease_rejection_reason::pending_or_eligibility_only,
							boundary_action));

					sqlite_shm_writer_member_authority* cohort_authority = nullptr;
					for (auto& member : attachment->members)
					{
						if (member.live_token == 0U ||
							std::ranges::find(supplied_tokens, member.live_token) ==
								supplied_tokens.end())
							return sqlite_shm_unexpected(rejection(
								sqlite_shm_lease_rejection_reason::pending_or_eligibility_only,
								boundary_action));
						const auto writer = find_by_token(writers_, member.live_token);
						if (writer == writers_.end() || writer->phase != writer_phase::pending ||
							!writer->receipt || !writer->registry_bound ||
							!writer->member_authority ||
							writer->route != writer_route::map_before_gate ||
							writer->positive_gate_token != 0U ||
							writer->request.attachment != attachment->identity ||
							writer->native_mapping == nullptr ||
							!valid_writer_receipt(*writer->receipt) ||
							writer->receipt->request() != writer->request ||
							writer->receipt->mapping().native_mapping != writer->native_mapping)
							return sqlite_shm_unexpected(rejection(
								sqlite_shm_lease_rejection_reason::pending_or_eligibility_only,
								boundary_action));
						if (!writer->member_authority->retains_exact_lifetimes(writer->request))
						{
							registry_member_admission_blocked_ = true;
							registry_member_sticky_quarantine_ = true;
							return sqlite_shm_unexpected(ambiguous());
						}
						if (cohort_authority != nullptr &&
							!cohort_authority->attachment_cohort_compatible_with(
								*writer->member_authority))
							return sqlite_shm_unexpected(
								rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										  cleanup_action));
						if (cohort_authority == nullptr)
							cohort_authority = &*writer->member_authority;
						const auto authority = writer->member_authority->validate_pending_authority(
							family, writer->request, *writer->receipt);
						if (authority ==
							detail::sqlite_shm_writer_pending_authority_status::
								determinate_mismatch)
							return sqlite_shm_unexpected(
								rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										  cleanup_action));
						if (authority != detail::sqlite_shm_writer_pending_authority_status::exact)
						{
							registry_member_admission_blocked_ = true;
							registry_member_sticky_quarantine_ = true;
							return sqlite_shm_unexpected(ambiguous());
						}
						if (gate->receipt.family() != family_ ||
							gate->receipt.connection_token() != writer->request.connection_token ||
							gate->receipt.open_epoch() != writer->receipt->open_epoch())
							return sqlite_shm_unexpected(
								rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										  cleanup_action));
						selected_writers.push_back(writer);
						selected_members.push_back(&member);
					}

					if (generation_ &&
						generation_->phase != sqlite_shm_mapping_generation_phase::live)
						return sqlite_shm_unexpected(rejection(
							generation_->handoff_count == 0U
								? sqlite_shm_lease_rejection_reason::retiring
								: sqlite_shm_lease_rejection_reason::successor_handoff_live,
							cleanup_action));
					if (!can_allocate_tokens_locked(pending.size()))
					{
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}

					generation_record candidate;
					if (generation_)
						candidate = *generation_;
					for (const auto writer : selected_writers)
					{
						const auto& map = *writer->receipt;
						if (candidate.pages.empty())
						{
							candidate.phase = sqlite_shm_mapping_generation_phase::live;
							candidate.sealed_shm_size = map.mapping().sealed_shm_size;
							candidate.pages.push_back(map.mapping());
						}
						else if (!join_mapping(candidate, map))
							return sqlite_shm_unexpected(
								rejection(sqlite_shm_lease_rejection_reason::mapping_mismatch,
										  cleanup_action));
						candidate.authorities.push_back({map, gate->receipt, true});
						prepared_holders.push_back(
							{0U, 0U, holder_phase::active, map, gate->receipt, true, std::nullopt});
					}
					std::uint64_t generation = candidate.value;
					if (!generation_)
					{
						auto minted = generations_->state_->mint();
						if (!minted)
						{
							if (minted.error() == generation_failure::unavailable)
								quarantine_locked();
							return sqlite_shm_unexpected(rejection(
								minted.error() == generation_failure::exhausted
									? sqlite_shm_lease_rejection_reason::generation_exhausted
									: sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								minted.error() == generation_failure::exhausted
									? cleanup_action
									: sqlite_shm_lease_recovery_action::quarantine_no_retry));
						}
						generation = *minted;
						candidate.value = generation;
					}

					if (std::exchange(fail_next_registry_writer_pending_liveness_for_testing_,
									  false))
						selected_writers.back()->member_authority->invalidate_epoch_for_testing();
					const auto final_liveness_exact = std::ranges::all_of(
						selected_writers,
						[&family](const std::list<writer_record>::iterator writer)
						{
							return writer->member_authority &&
								writer->member_authority->retains_exact_lifetimes(
									writer->request) &&
								writer->member_authority->validate_pending_authority(
									family, writer->request, *writer->receipt) ==
								detail::sqlite_shm_writer_pending_authority_status::exact;
						});
					if (!final_liveness_exact)
					{
						// Minting is monotonic and non-reusing. A first-generation ID gap is
						// permitted so the exact final liveness scan can be the success
						// linearization immediately before the allocation-free/no-throw commit.
						registry_member_admission_blocked_ = true;
						registry_member_sticky_quarantine_ = true;
						return sqlite_shm_unexpected(ambiguous());
					}

					static_assert(std::is_nothrow_move_constructible_v<generation_record>);
					static_assert(std::is_nothrow_move_constructible_v<holder_record>);
					static_assert(
						std::is_nothrow_move_constructible_v<sqlite_shm_writer_member_authority>);
					static_assert(std::is_nothrow_move_constructible_v<
								  sqlite_shm_verified_writer_eligibility_receipt>);
					static_assert(std::is_nothrow_move_constructible_v<sqlite_shm_writer_holder>);

					auto prepared_holder = prepared_holders.begin();
					for (std::size_t index = 0U; index < selected_writers.size();
						 ++index, ++prepared_holder)
					{
						const auto token = allocate_token_unchecked_locked();
						prepared_holder->token = token;
						prepared_holder->generation = generation;
						prepared_holder->member_authority.emplace(
							std::move(*selected_writers[index]->member_authority));
						selected_members[index]->live_token = token;
						output.push_back(sqlite_shm_writer_holder{
							state,
							sqlite_shm_lease_token_identity{token},
							sqlite_shm_mapping_generation_identity{generation}});
					}
					generation_.reset();
					generation_.emplace(std::move(candidate));
					holders_.splice(holders_.end(), prepared_holders);
					for (const auto writer : selected_writers)
						writers_.erase(writer);
					attachment->gate_phase = writer_attachment_gate_phase::positive_active;
					for (auto* const member : pending)
						member->disarm();
					return output;
				}
				catch (...)
				{
					quarantine_after_native();
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_writer_holder>
			promote(sqlite_shm_pending_mapping& pending,
					const sqlite_shm_writer_eligibility& eligibility)
			{
				try
				{
					std::scoped_lock lock{mutex_};
					if (auto blocked =
							blocked_locked(sqlite_shm_lease_recovery_action::
											   remove_pending_then_confirm_native_cleanup))
						return sqlite_shm_unexpected(*blocked);
					if (!owns(pending.state_, pending.token_) ||
						!owns(eligibility.state_, eligibility.token_))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::
											remove_pending_then_confirm_native_cleanup));
					const auto writer = find_by_token(writers_, pending.token_);
					const auto gate = find_by_token(eligibilities_, eligibility.token_);
					if (writer == writers_.end() || writer->phase != writer_phase::pending ||
						!writer->receipt || gate == eligibilities_.end())
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::
											remove_pending_then_confirm_native_cleanup));
					if (writer->registry_bound && writer->member_authority)
						return sqlite_shm_unexpected(rejection(
							sqlite_shm_lease_rejection_reason::pending_or_eligibility_only,
							sqlite_shm_lease_recovery_action::
								await_complete_attachment_gate_boundary));
					if (writer->registry_bound || writer->member_authority)
					{
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto attachment = find_attachment_token_locked(pending.token_);
					if (attachment == writer_attachments_.end() ||
						attachment->phase != writer_attachment_phase::collecting)
					{
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto has_writer_sibling = std::ranges::any_of(
						writers_,
						[&attachment, &writer](const writer_record& candidate)
						{
							return candidate.token != writer->token &&
								candidate.request.attachment == attachment->identity &&
								(candidate.phase == writer_phase::inflight ||
								 candidate.phase == writer_phase::post_native_mapping ||
								 candidate.phase == writer_phase::pending);
						});
					if (has_writer_sibling)
						return sqlite_shm_unexpected(rejection(
							sqlite_shm_lease_rejection_reason::pending_or_eligibility_only,
							sqlite_shm_lease_recovery_action::
								await_complete_attachment_gate_boundary));
					const auto complete = derive_complete_attachment_prefix_locked(*attachment);
					if (!complete || complete->has_inflight ||
						std::ranges::count(complete->tokens, pending.token_) != 1)
					{
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					auto attachment_member =
						std::find_if(attachment->members.begin(),
									 attachment->members.end(),
									 [&pending](const writer_attachment_member_record& member)
									 {
										 return member.live_token == pending.token_;
									 });
					if (attachment_member == attachment->members.end())
					{
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto& map = *writer->receipt;
					if (gate->receipt.family() != family_ ||
						gate->receipt.connection_token() != map.request().connection_token ||
						gate->receipt.open_epoch() != map.open_epoch())
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::
										  remove_pending_then_confirm_native_cleanup));
					if (attachment->promotion_gate_receipt &&
						!same_eligibility_receipt(*attachment->promotion_gate_receipt,
												  gate->receipt))
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::
										  remove_pending_then_confirm_native_cleanup));
					std::optional<sqlite_shm_verified_writer_eligibility_receipt> first_gate_copy;
					if (!attachment->promotion_gate_receipt)
						first_gate_copy.emplace(gate->receipt);
					if (generation_ &&
						generation_->phase != sqlite_shm_mapping_generation_phase::live)
						return sqlite_shm_unexpected(rejection(
							generation_->handoff_count == 0U
								? sqlite_shm_lease_rejection_reason::retiring
								: sqlite_shm_lease_rejection_reason::successor_handoff_live,
							sqlite_shm_lease_recovery_action::
								remove_pending_then_confirm_native_cleanup));

					auto state = shared_from_this();
					generation_record candidate;
					if (generation_)
					{
						candidate = *generation_;
						if (!join_mapping(candidate, map))
							return sqlite_shm_unexpected(
								rejection(sqlite_shm_lease_rejection_reason::mapping_mismatch,
										  sqlite_shm_lease_recovery_action::
											  remove_pending_then_confirm_native_cleanup));
					}
					else
					{
						candidate.phase = sqlite_shm_mapping_generation_phase::live;
						candidate.sealed_shm_size = map.mapping().sealed_shm_size;
						candidate.pages.push_back(map.mapping());
					}
					candidate.authorities.push_back({map, gate->receipt, true});
					std::list<holder_record> prepared_holders;
					prepared_holders.push_back(
						{0U, 0U, holder_phase::active, map, gate->receipt, false, std::nullopt});
					if (!can_allocate_tokens_locked(1U))
					{
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}

					std::uint64_t generation = candidate.value;
					if (!generation_)
					{
						auto minted = generations_->state_->mint();
						if (!minted)
						{
							if (minted.error() == generation_failure::unavailable)
								quarantine_locked();
							return sqlite_shm_unexpected(rejection(
								minted.error() == generation_failure::exhausted
									? sqlite_shm_lease_rejection_reason::generation_exhausted
									: sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								minted.error() == generation_failure::exhausted
									? sqlite_shm_lease_recovery_action::
										  remove_pending_then_confirm_native_cleanup
									: sqlite_shm_lease_recovery_action::quarantine_no_retry));
						}
						generation = *minted;
						candidate.value = generation;
					}

					static_assert(std::is_nothrow_move_constructible_v<generation_record>);
					static_assert(std::is_nothrow_move_constructible_v<holder_record>);
					const auto holder_token = allocate_token_unchecked_locked();
					prepared_holders.front().token = holder_token;
					prepared_holders.front().generation = generation;
					attachment_member->live_token = holder_token;
					generation_.reset();
					generation_.emplace(std::move(candidate));
					holders_.splice(holders_.end(), prepared_holders);
					writers_.erase(writer);
					if (first_gate_copy)
					{
						static_assert(std::is_nothrow_move_constructible_v<
									  sqlite_shm_verified_writer_eligibility_receipt>);
						attachment->promotion_gate_receipt.emplace(std::move(*first_gate_copy));
					}
					pending.disarm();
					return sqlite_shm_writer_holder{
						std::move(state),
						sqlite_shm_lease_token_identity{holder_token},
						sqlite_shm_mapping_generation_identity{generation}};
				}
				catch (...)
				{
					quarantine_after_native();
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<void>
			resolve_writer_failure(sqlite_shm_writer_map_inflight& inflight) noexcept
			{
				std::optional<sqlite_shm_writer_member_authority> authority_to_release;
				try
				{
					{
						std::scoped_lock lock{mutex_};
						if (!owns(inflight.state_, inflight.token_))
							return sqlite_shm_unexpected(stale_token(
								sqlite_shm_lease_recovery_action::outer_ioerr_no_retry));
						if (inflight.native_result_observed_ ||
							inflight.native_result_validation_ambiguous_)
						{
							quarantine_locked();
							return sqlite_shm_unexpected(ambiguous());
						}
						const auto found = find_by_token(writers_, inflight.token_);
						if (found == writers_.end() || found->phase != writer_phase::inflight)
							return sqlite_shm_unexpected(stale_token(
								sqlite_shm_lease_recovery_action::outer_ioerr_no_retry));
						if (!release_attachment_member_locked(inflight.token_, false))
						{
							found->phase = writer_phase::terminal_quarantined;
							inflight.disarm();
							quarantine_locked();
							return sqlite_shm_unexpected(ambiguous());
						}
						if (found->member_authority)
						{
							if (found->registry_bound &&
								!found->member_authority->retains_exact_lifetimes(found->request))
							{
								registry_member_admission_blocked_ = true;
								registry_member_sticky_quarantine_ = true;
							}
							authority_to_release.emplace(std::move(*found->member_authority));
						}
						writers_.erase(found);
						inflight.disarm();
					}
					if (authority_to_release)
					{
						auto released = authority_to_release->release_activity();
						if (!released)
						{
							emergency_quarantine_.store(true, std::memory_order_release);
							return sqlite_shm_unexpected(released.error());
						}
					}
					return {};
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_writer_attachment_cleanup>
			begin_writer_cleanup(sqlite_shm_writer_post_native_mapping& post_native,
								 const sqlite_shm_callback_execution_receipt& callback) noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					if (!owns(post_native.state_, post_native.token_))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					const auto found = find_by_token(writers_, post_native.token_);
					if (found == writers_.end())
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					if (found->phase != writer_phase::post_native_mapping ||
						found->native_mapping == nullptr)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					if (found->registry_bound &&
						(!found->member_authority ||
						 !found->member_authority->retains_exact_lifetimes(found->request)))
						registry_member_admission_blocked_ = true;
					if (!admit_writer_cleanup_callback_locked(
							post_native.token_, found->request.callback, callback))
					{
						found->phase = writer_phase::terminal_quarantined;
						const auto attachment = find_attachment_token_locked(post_native.token_);
						if (attachment != writer_attachments_.end())
							attachment->phase = writer_attachment_phase::terminal_quarantined;
						post_native.disarm();
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto sealed =
						seal_attachment_cleanup_locked(post_native.token_, callback, false);
					if (sealed.status != attachment_seal_status::sealed)
					{
						found->phase = writer_phase::terminal_quarantined;
						post_native.disarm();
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					auto state = post_native.state_;
					post_native.disarm();
					return sqlite_shm_writer_attachment_cleanup{
						std::move(state),
						sqlite_shm_lease_token_identity{sealed.cleanup_token},
						sqlite_shm_mapping_generation_identity{sealed.generation}};
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_writer_attachment_cleanup>
			begin_writer_cleanup(sqlite_shm_pending_mapping& pending,
								 const sqlite_shm_callback_execution_receipt& callback) noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					if (!owns(pending.state_, pending.token_))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					const auto found = find_by_token(writers_, pending.token_);
					if (found == writers_.end())
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					if (found->phase != writer_phase::pending)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					if (!admit_writer_cleanup_callback_locked(
							pending.token_, found->request.callback, callback))
					{
						found->phase = writer_phase::terminal_quarantined;
						const auto attachment = find_attachment_token_locked(pending.token_);
						if (attachment != writer_attachments_.end())
							attachment->phase = writer_attachment_phase::terminal_quarantined;
						pending.disarm();
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto sealed =
						seal_attachment_cleanup_locked(pending.token_, callback, false);
					if (sealed.status != attachment_seal_status::sealed)
					{
						found->phase = writer_phase::terminal_quarantined;
						pending.disarm();
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					auto state = shared_from_this();
					pending.disarm();
					return sqlite_shm_writer_attachment_cleanup{
						std::move(state),
						sqlite_shm_lease_token_identity{sealed.cleanup_token},
						sqlite_shm_mapping_generation_identity{sealed.generation}};
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<void>
			complete_writer_cleanup(sqlite_shm_writer_attachment_cleanup& cleanup,
									const sqlite_shm_callback_execution_receipt& callback,
									const sqlite_shm_native_cleanup_outcome outcome) noexcept
			{
				std::list<writer_record> completed_writers;
				std::list<holder_record> completed_holders;
				try
				{
					{
						std::scoped_lock lock{mutex_};
						if (!owns(cleanup.state_, cleanup.token_))
							return sqlite_shm_unexpected(
								stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
						const auto attachment = find_attachment_cleanup_locked(cleanup.token_);
						if (attachment == writer_attachments_.end() ||
							attachment->cleanup_generation != cleanup.generation_)
							return sqlite_shm_unexpected(
								stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
						const auto ready_nonlast = attachment->phase ==
							writer_attachment_phase::nonlast_native_cleanup_admitted;
						const auto ready_last = attachment->phase ==
							writer_attachment_phase::last_native_cleanup_admitted;
						const auto callback_matches = attachment->cleanup_callback &&
							*attachment->cleanup_callback == callback;
						const auto complete_prefix_matches =
							attachment->sealed_complete_prefix.size() ==
								attachment->members.size() &&
							attachment->sealed_member_audit.size() == attachment->members.size() &&
							std::ranges::all_of(
								attachment->members,
								[this, &attachment](const writer_attachment_member_record& member)
								{
									if (member.live_token == 0U ||
										!prefix_contains_token(attachment->sealed_complete_prefix,
															   member.live_token))
										return false;
									const auto audit = std::find_if(
										attachment->sealed_member_audit.begin(),
										attachment->sealed_member_audit.end(),
										[&member](
											const writer_attachment_member_audit_record& value)
										{
											return value.original_token == member.original_token &&
												value.sealed_live_token == member.live_token;
										});
									if (audit == attachment->sealed_member_audit.end() ||
										audit->request.attachment != attachment->identity)
										return false;
									const auto writer = find_by_token(writers_, member.live_token);
									const auto holder = find_by_token(holders_, member.live_token);
									return (writer != writers_.end()) !=
										(holder != holders_.end()) &&
										(writer != writers_.end() ? writer->phase ==
													 writer_phase::attachment_cleanup_sealed &&
												 writer_matches_attachment_locked(*writer,
																				  *attachment)
																  : holder->phase ==
													 holder_phase::attachment_cleanup_sealed &&
												 holder_matches_attachment_locked(*holder,
																				  *attachment));
								});
						const auto target_authorities_inactive = !generation_ ||
							std::ranges::none_of(
								generation_->authorities,
								[&attachment](const generation_authority_record& authority)
								{
									return authority.active &&
										authority.map_receipt.request().attachment ==
										attachment->identity;
								});
						if ((!ready_nonlast && !ready_last) || !callback_matches ||
							!complete_prefix_matches || !target_authorities_inactive ||
							outcome != sqlite_shm_native_cleanup_outcome::confirmed_success ||
							is_quarantined_locked())
						{
							attachment->phase = writer_attachment_phase::terminal_quarantined;
							cleanup.disarm();
							quarantine_locked();
							return sqlite_shm_unexpected(ambiguous());
						}
						if (ready_last &&
							(!generation_ || generation_->value != cleanup.generation_ ||
							 generation_->phase != sqlite_shm_mapping_generation_phase::retiring ||
							 std::ranges::any_of(writers_,
												 [](const writer_record& writer)
												 {
													 return writer.phase !=
														 writer_phase::attachment_cleanup_sealed;
												 }) ||
							 !readers_.empty() ||
							 std::ranges::any_of(holders_,
												 [](const holder_record& holder)
												 {
													 return holder.phase !=
														 holder_phase::attachment_cleanup_sealed;
												 })))
						{
							attachment->phase = writer_attachment_phase::terminal_quarantined;
							cleanup.disarm();
							quarantine_locked();
							return sqlite_shm_unexpected(ambiguous());
						}

						static_assert(std::is_nothrow_move_constructible_v<writer_record>);
						static_assert(std::is_nothrow_move_constructible_v<holder_record>);
						static_assert(
							std::is_nothrow_destructible_v<sqlite_shm_callback_execution_receipt>);
						static_assert(std::is_nothrow_destructible_v<generation_record>);

						// The native outcome is now consumed. Enter a terminal transient and disarm
						// the only owner before the allocation-free/no-throw state commit.
						attachment->phase = writer_attachment_phase::completion_committing;
						cleanup.disarm();
						if (std::exchange(fail_next_writer_completion_transition_for_testing_,
										  false))
						{
							attachment->phase = writer_attachment_phase::terminal_quarantined;
							quarantine_locked();
							return sqlite_shm_unexpected(ambiguous());
						}
						for (auto& member : attachment->members)
						{
							member.live_token = 0U;
							member.confirmed_native_cleanup = true;
						}
						for (auto writer = writers_.begin(); writer != writers_.end();)
						{
							if (!prefix_contains_token(attachment->sealed_complete_prefix,
													   writer->token))
							{
								++writer;
								continue;
							}
							const auto completed = writer++;
							completed_writers.splice(completed_writers.end(), writers_, completed);
						}
						for (auto holder = holders_.begin(); holder != holders_.end();)
						{
							if (!prefix_contains_token(attachment->sealed_complete_prefix,
													   holder->token))
							{
								++holder;
								continue;
							}
							const auto completed = holder++;
							completed_holders.splice(completed_holders.end(), holders_, completed);
						}
						if (!registry_member_sticky_quarantine_ &&
							std::ranges::none_of(
								writers_,
								[](const writer_record& writer)
								{
									return writer.registry_bound &&
										(!writer.member_authority ||
										 !writer.member_authority->retains_exact_lifetimes(
											 writer.request));
								}) &&
							std::ranges::none_of(
								holders_,
								[](const holder_record& holder)
								{
									return holder.registry_bound &&
										(!holder.member_authority ||
										 !holder.member_authority->retains_exact_lifetimes(
											 holder.map_receipt.request()));
								}))
							registry_member_admission_blocked_ = false;
						attachment->phase = writer_attachment_phase::retired;
						attachment->cleanup_token = 0U;
						attachment->cleanup_callback.reset();
						if (ready_last)
						{
							generation_->phase = sqlite_shm_mapping_generation_phase::retired;
							if (generation_->handoff_count == 0U)
								generation_.reset();
						}
					}
					for (auto& writer : completed_writers)
					{
						if (!writer.member_authority)
							continue;
						auto released = writer.member_authority->release_activity();
						if (!released)
						{
							emergency_quarantine_.store(true, std::memory_order_release);
							return sqlite_shm_unexpected(released.error());
						}
					}
					for (auto& holder : completed_holders)
					{
						if (!holder.member_authority)
							continue;
						auto released = holder.member_authority->release_activity();
						if (!released)
						{
							emergency_quarantine_.store(true, std::memory_order_release);
							return sqlite_shm_unexpected(released.error());
						}
					}
					return {};
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_map_inflight>
			begin_reader(const sqlite_shm_reader_map_request& request)
			{
				if (!valid_reader_request(request))
					return sqlite_shm_unexpected(
						rejection(request.caller_extend == 0
									  ? sqlite_shm_lease_rejection_reason::invalid_request
									  : sqlite_shm_lease_rejection_reason::invalid_extend_pair,
								  sqlite_shm_lease_recovery_action::deny_before_native_map));
				try
				{
					std::scoped_lock lock{mutex_};
					if (auto blocked = blocked_locked(
							sqlite_shm_lease_recovery_action::deny_before_native_map))
						return sqlite_shm_unexpected(*blocked);
					if (request.family != family_)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					if (!generation_)
						return sqlite_shm_unexpected(rejection(
							eligibilities_.empty() && writers_.empty()
								? sqlite_shm_lease_rejection_reason::no_live_generation
								: sqlite_shm_lease_rejection_reason::pending_or_eligibility_only,
							sqlite_shm_lease_recovery_action::deny_before_native_map));
					if (generation_->phase != sqlite_shm_mapping_generation_phase::live)
						return sqlite_shm_unexpected(rejection(
							generation_->handoff_count == 0U
								? sqlite_shm_lease_rejection_reason::retiring
								: sqlite_shm_lease_rejection_reason::successor_handoff_live,
							sqlite_shm_lease_recovery_action::deny_before_native_map));
					const auto page = find_page_locked(request.page_number);
					if (page == generation_->pages.end() || page->page_size != request.page_size)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::mapping_mismatch,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					if (!callback_can_start_locked(request.callback))
					{
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto token = allocate_token_locked();
					if (!token)
						return sqlite_shm_unexpected(ambiguous());
					auto expected_mapping = *page;
					expected_mapping.sealed_shm_size = generation_->sealed_shm_size;
					readers_.push_back({*token,
										reader_phase::inflight,
										request,
										generation_->value,
										expected_mapping,
										generation_->pages.size(),
										std::nullopt,
										std::nullopt});
					return sqlite_shm_reader_map_inflight{
						shared_from_this(),
						sqlite_shm_lease_token_identity{*token},
						sqlite_shm_mapping_generation_identity{generation_->value}};
				}
				catch (...)
				{
					return sqlite_shm_unexpected(
						rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								  sqlite_shm_lease_recovery_action::deny_before_native_map));
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_handoff>
			promote_reader(sqlite_shm_reader_map_inflight& inflight,
						   const sqlite_shm_verified_reader_post_map_receipt& receipt)
			{
				try
				{
					std::scoped_lock lock{mutex_};
					if (!owns(inflight.state_, inflight.token_))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::
											attempt_nonremoving_unmap_then_outer_ioerr));
					const auto found = find_by_token(readers_, inflight.token_);
					if (found == readers_.end() || found->phase != reader_phase::inflight)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::
											attempt_nonremoving_unmap_then_outer_ioerr));
					found->phase = reader_phase::post_native_rejected;
					found->receipt = receipt;
					if (auto blocked =
							blocked_locked(sqlite_shm_lease_recovery_action::
											   attempt_nonremoving_unmap_then_outer_ioerr))
						return sqlite_shm_unexpected(*blocked);
					if (!valid_reader_receipt(receipt) || receipt.request() != found->request ||
						receipt.generation() != found->generation ||
						receipt.mapping() != found->expected_mapping || !generation_ ||
						generation_->value != found->generation ||
						(generation_->phase != sqlite_shm_mapping_generation_phase::live &&
						 generation_->phase != sqlite_shm_mapping_generation_phase::retiring) ||
						generation_->sealed_shm_size != found->expected_mapping.sealed_shm_size ||
						generation_->pages.size() != found->mapping_page_count)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::
										  attempt_nonremoving_unmap_then_outer_ioerr));
					const auto handoff_token = allocate_token_locked();
					if (!handoff_token)
					{
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					handoffs_.push_back({*handoff_token,
										 found->generation,
										 handoff_phase::active,
										 receipt,
										 std::nullopt});
					++generation_->handoff_count;
					const auto generation = found->generation;
					readers_.erase(found);
					inflight.disarm();
					return sqlite_shm_reader_handoff{
						shared_from_this(),
						sqlite_shm_lease_token_identity{*handoff_token},
						sqlite_shm_mapping_generation_identity{generation}};
				}
				catch (...)
				{
					quarantine_after_native();
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<void>
			resolve_reader_failure(sqlite_shm_reader_map_inflight& inflight) noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					if (!owns(inflight.state_, inflight.token_))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::outer_ioerr_no_retry));
					const auto found = find_by_token(readers_, inflight.token_);
					if (found == readers_.end() || found->phase != reader_phase::inflight)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::outer_ioerr_no_retry));
					readers_.erase(found);
					inflight.disarm();
					return {};
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_cleanup_obligation>
			begin_reader_cleanup(sqlite_shm_reader_map_inflight& inflight,
								 const sqlite_shm_callback_execution_receipt& callback) noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					if (!owns(inflight.state_, inflight.token_))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					const auto found = find_by_token(readers_, inflight.token_);
					if (found == readers_.end())
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					if (found->phase != reader_phase::post_native_rejected)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					if (!admit_cleanup_callback_locked(found->request.callback, callback))
					{
						found->phase = reader_phase::terminal_quarantined;
						inflight.disarm();
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto token = inflight.token_;
					const auto generation = found->generation;
					auto state = shared_from_this();
					found->phase = reader_phase::terminal_quarantined;
					inflight.disarm();
					found->cleanup_callback = callback;
					found->phase = reader_phase::cleanup_obligation;
					return sqlite_shm_reader_cleanup_obligation{
						std::move(state),
						sqlite_shm_lease_token_identity{token},
						sqlite_shm_mapping_generation_identity{generation}};
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<void>
			complete_reader_cleanup(sqlite_shm_reader_cleanup_obligation& cleanup,
									const sqlite_shm_callback_execution_receipt& callback,
									const sqlite_shm_native_cleanup_outcome outcome) noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					if (!owns(cleanup.state_, cleanup.token_))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					const auto found = find_by_token(readers_, cleanup.token_);
					if (found == readers_.end() ||
						found->phase != reader_phase::cleanup_obligation ||
						found->generation != cleanup.generation_)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					const auto callback_matches =
						found->cleanup_callback && *found->cleanup_callback == callback;
					if (!callback_matches ||
						outcome != sqlite_shm_native_cleanup_outcome::confirmed_success ||
						is_quarantined_locked())
					{
						found->phase = reader_phase::terminal_quarantined;
						cleanup.disarm();
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					readers_.erase(found);
					cleanup.disarm();
					return {};
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_unmap_obligation>
			begin_reader_unmap(sqlite_shm_reader_handoff& handoff,
							   const sqlite_shm_callback_execution_receipt& callback) noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					if (!owns(handoff.state_, handoff.token_))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					const auto found = find_by_token(handoffs_, handoff.token_);
					if (found == handoffs_.end() || found->generation != handoff.generation_)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					if (found->phase != handoff_phase::active)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					if (!callback_can_start_locked(callback))
					{
						found->phase = handoff_phase::terminal_quarantined;
						handoff.disarm();
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto token = handoff.token_;
					const auto generation = handoff.generation_;
					auto state = shared_from_this();
					found->phase = handoff_phase::terminal_quarantined;
					handoff.disarm();
					found->unmap_callback = callback;
					found->phase = handoff_phase::native_cleanup_admitted;
					return sqlite_shm_reader_unmap_obligation{
						std::move(state),
						sqlite_shm_lease_token_identity{token},
						sqlite_shm_mapping_generation_identity{generation}};
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<void>
			complete_reader_unmap(sqlite_shm_reader_unmap_obligation& unmap,
								  const sqlite_shm_callback_execution_receipt& callback,
								  const sqlite_shm_native_cleanup_outcome outcome) noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					if (!owns(unmap.state_, unmap.token_))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					const auto found = find_by_token(handoffs_, unmap.token_);
					if (found == handoffs_.end() || found->generation != unmap.generation_ ||
						found->phase != handoff_phase::native_cleanup_admitted)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					const auto callback_matches =
						found->unmap_callback && *found->unmap_callback == callback;
					if (!callback_matches ||
						outcome != sqlite_shm_native_cleanup_outcome::confirmed_success ||
						is_quarantined_locked())
					{
						found->phase = handoff_phase::terminal_quarantined;
						quarantine_locked();
						unmap.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					if (!generation_ || generation_->value != unmap.generation_ ||
						generation_->handoff_count == 0U)
					{
						found->phase = handoff_phase::terminal_quarantined;
						quarantine_locked();
						unmap.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					handoffs_.erase(found);
					--generation_->handoff_count;
					if (generation_->phase == sqlite_shm_mapping_generation_phase::retired &&
						generation_->handoff_count == 0U)
						generation_.reset();
					unmap.disarm();
					return {};
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_writer_release>
			release_holder(sqlite_shm_writer_holder& holder,
						   const sqlite_shm_callback_execution_receipt& callback) noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					if (!owns(holder.state_, holder.token_))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::outer_ioerr_no_retry));
					const auto found = find_by_token(holders_, holder.token_);
					if (found == holders_.end() || found->generation != holder.generation_ ||
						found->phase != holder_phase::active || !generation_ ||
						generation_->value != holder.generation_ ||
						(generation_->phase != sqlite_shm_mapping_generation_phase::live &&
						 generation_->phase != sqlite_shm_mapping_generation_phase::quarantined))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::outer_ioerr_no_retry));
					if (!valid_callback(callback))
					{
						found->phase = holder_phase::terminal_quarantined;
						const auto attachment = find_attachment_token_locked(holder.token_);
						if (attachment != writer_attachments_.end())
							attachment->phase = writer_attachment_phase::terminal_quarantined;
						holder.disarm();
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					if (!callback_can_start_locked(callback))
					{
						found->phase = holder_phase::terminal_quarantined;
						const auto attachment = find_attachment_token_locked(holder.token_);
						if (attachment != writer_attachments_.end())
							attachment->phase = writer_attachment_phase::terminal_quarantined;
						holder.disarm();
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto generation = holder.generation_;
					const auto sealed =
						seal_attachment_cleanup_locked(holder.token_, callback, true);
					if (sealed.status != attachment_seal_status::sealed)
					{
						found->phase = holder_phase::terminal_quarantined;
						holder.disarm();
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					auto state = shared_from_this();
					holder.disarm();
					auto cleanup = sqlite_shm_writer_attachment_cleanup{
						std::move(state),
						sqlite_shm_lease_token_identity{sealed.cleanup_token},
						sqlite_shm_mapping_generation_identity{generation}};
					return sqlite_shm_writer_release{
						sealed.decision, generation, std::move(cleanup)};
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_writer_retirement_result>
			poll_retirement(const sqlite_shm_writer_attachment_cleanup& cleanup,
							const sqlite_shm_callback_execution_receipt& callback) noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					if (!owns(cleanup.state_, cleanup.token_) || cleanup.generation_ == 0U)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::outer_ioerr_no_retry));
					const auto attachment = find_attachment_cleanup_locked(cleanup.token_);
					if (attachment == writer_attachments_.end() ||
						attachment->cleanup_generation != cleanup.generation_ ||
						!attachment->cleanup_callback || *attachment->cleanup_callback != callback)
					{
						if (attachment != writer_attachments_.end())
							attachment->phase = writer_attachment_phase::terminal_quarantined;
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto generation = cleanup.generation_;
					if (attachment->phase == writer_attachment_phase::terminal_quarantined)
						return sqlite_shm_writer_retirement_result{
							sqlite_shm_writer_retirement_decision::quarantined, generation};
					if (attachment->phase != writer_attachment_phase::last_waiting)
					{
						attachment->phase = writer_attachment_phase::terminal_quarantined;
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					if (!generation_ || generation_->value != generation)
					{
						attachment->phase = writer_attachment_phase::terminal_quarantined;
						quarantine_locked();
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::outer_ioerr_no_retry));
					}
					if (generation_->phase != sqlite_shm_mapping_generation_phase::retiring &&
						generation_->phase != sqlite_shm_mapping_generation_phase::quarantined)
					{
						attachment->phase = writer_attachment_phase::terminal_quarantined;
						quarantine_locked();
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::stale_generation,
									  sqlite_shm_lease_recovery_action::outer_ioerr_no_retry));
					}
					const auto decision = retirement_decision_for_prefix_locked(
						callback, attachment->sealed_complete_prefix);
					if (decision == sqlite_shm_writer_retirement_decision::ready)
						attachment->phase = writer_attachment_phase::last_native_cleanup_admitted;
					else if (decision ==
							 sqlite_shm_writer_retirement_decision::quarantine_same_thread)
						attachment->phase = writer_attachment_phase::terminal_quarantined;
					return sqlite_shm_writer_retirement_result{decision, generation};
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<void>
			fail_retirement_wait(const sqlite_shm_writer_attachment_cleanup& cleanup,
								 const sqlite_shm_callback_execution_receipt& callback,
								 const sqlite_shm_retirement_wait_failure failure) noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					if (!owns(cleanup.state_, cleanup.token_) || cleanup.generation_ == 0U)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					const auto attachment = find_attachment_cleanup_locked(cleanup.token_);
					if (attachment == writer_attachments_.end() ||
						attachment->cleanup_generation != cleanup.generation_ ||
						attachment->phase != writer_attachment_phase::last_waiting ||
						!attachment->cleanup_callback ||
						*attachment->cleanup_callback != callback ||
						(failure != sqlite_shm_retirement_wait_failure::timeout &&
						 failure != sqlite_shm_retirement_wait_failure::unknown) ||
						!generation_ || generation_->value != cleanup.generation_ ||
						(generation_->phase != sqlite_shm_mapping_generation_phase::retiring &&
						 generation_->phase != sqlite_shm_mapping_generation_phase::quarantined))
					{
						if (attachment != writer_attachments_.end())
							attachment->phase = writer_attachment_phase::terminal_quarantined;
						quarantine_locked();
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					}
					attachment->phase = writer_attachment_phase::terminal_quarantined;
					quarantine_locked();
					return sqlite_shm_unexpected(ambiguous());
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_mapping_lease_snapshot snapshot() const noexcept
			{
				sqlite_shm_mapping_lease_snapshot output;
				try
				{
					std::scoped_lock lock{mutex_};
					output.quarantined = is_quarantined_locked();
					output.phase = output.quarantined
						? sqlite_shm_mapping_generation_phase::quarantined
						: generation_ ? generation_->phase
									  : sqlite_shm_mapping_generation_phase::empty;
					if (generation_)
					{
						output.generation = generation_->value;
						output.sealed_shm_size = generation_->sealed_shm_size;
						output.mapping_page_count = generation_->pages.size();
						output.generation_authority_count = static_cast<std::size_t>(
							std::ranges::count_if(generation_->authorities,
												  [](const generation_authority_record& authority)
												  {
													  return authority.active;
												  }));
					}
					output.eligibility_count = eligibilities_.size();
					output.writer_inflight_count = static_cast<std::size_t>(std::ranges::count_if(
						writers_,
						[](const writer_record& writer)
						{
							return writer.phase != writer_phase::attachment_cleanup_sealed &&
								writer.phase != writer_phase::terminal_quarantined;
						}));
					output.writer_cleanup_count = static_cast<std::size_t>(std::ranges::count_if(
						writer_attachments_,
						[](const writer_attachment_record& attachment)
						{
							return attachment.phase ==
								writer_attachment_phase::nonlast_native_cleanup_admitted ||
								attachment.phase == writer_attachment_phase::last_waiting ||
								attachment.phase ==
								writer_attachment_phase::last_native_cleanup_admitted ||
								attachment.phase == writer_attachment_phase::terminal_quarantined;
						}));
					output.writer_member_authority_count =
						static_cast<std::size_t>(std::ranges::count_if(
							writers_,
							[](const writer_record& writer)
							{
								return writer.registry_bound && writer.member_authority.has_value();
							})) +
						static_cast<std::size_t>(std::ranges::count_if(
							holders_,
							[](const holder_record& holder)
							{
								return holder.registry_bound && holder.member_authority.has_value();
							}));
					output.writer_member_liveness_lost_count =
						static_cast<std::size_t>(std::ranges::count_if(
							writers_,
							[](const writer_record& writer)
							{
								return writer.registry_bound &&
									(!writer.member_authority ||
									 !writer.member_authority->retains_exact_lifetimes(
										 writer.request));
							})) +
						static_cast<std::size_t>(std::ranges::count_if(
							holders_,
							[](const holder_record& holder)
							{
								return holder.registry_bound &&
									(!holder.member_authority ||
									 !holder.member_authority->retains_exact_lifetimes(
										 holder.map_receipt.request()));
							}));
					if (registry_member_admission_blocked_ ||
						output.writer_member_liveness_lost_count != 0U)
					{
						output.quarantined = true;
						output.phase = sqlite_shm_mapping_generation_phase::quarantined;
					}
					output.writer_holder_count = active_holder_count_locked();
					output.writer_attachment_identity_count = writer_attachments_.size();
					for (const auto& attachment : writer_attachments_)
					{
						output.writer_attachment_member_count += attachment.members.size();
						output.writer_attachment_audit_member_count +=
							attachment.sealed_member_audit.size();
						output.writer_attachment_audit_native_mapping_count +=
							static_cast<std::size_t>(std::ranges::count_if(
								attachment.sealed_member_audit,
								[](const writer_attachment_member_audit_record& member)
								{
									return member.native_mapping != nullptr;
								}));
						output.writer_attachment_audit_post_map_count +=
							static_cast<std::size_t>(std::ranges::count_if(
								attachment.sealed_member_audit,
								[](const writer_attachment_member_audit_record& member)
								{
									return member.post_map_receipt.has_value();
								}));
						output.writer_attachment_audit_promotion_count +=
							static_cast<std::size_t>(std::ranges::count_if(
								attachment.sealed_member_audit,
								[](const writer_attachment_member_audit_record& member)
								{
									return member.promotion_receipt.has_value();
								}));
						const auto live_members = static_cast<std::size_t>(
							std::ranges::count_if(attachment.members,
												  [](const writer_attachment_member_record& member)
												  {
													  return member.live_token != 0U;
												  }));
						output.writer_attachment_unresolved_member_count += live_members;
						if (live_members != 0U)
							++output.writer_attachment_unresolved_count;
					}
					output.reader_inflight_count = static_cast<std::size_t>(std::ranges::count_if(
						readers_,
						[](const reader_record& reader)
						{
							return reader.phase != reader_phase::cleanup_obligation &&
								reader.phase != reader_phase::terminal_quarantined;
						}));
					output.reader_cleanup_count = static_cast<std::size_t>(
						std::ranges::count_if(readers_,
											  [](const reader_record& reader)
											  {
												  return reader.phase ==
													  reader_phase::cleanup_obligation ||
													  reader.phase ==
													  reader_phase::terminal_quarantined;
											  }) +
						std::ranges::count_if(handoffs_,
											  [](const handoff_record& handoff)
											  {
												  return handoff.phase != handoff_phase::active;
											  }));
					output.reader_handoff_count = handoffs_.size();
					output.reader_admission_visible = !output.quarantined && generation_ &&
						generation_->phase == sqlite_shm_mapping_generation_phase::live &&
						active_holder_count_locked() != 0U;
				}
				catch (...)
				{
					output.phase = sqlite_shm_mapping_generation_phase::quarantined;
					output.quarantined = true;
					output.reader_admission_visible = false;
				}
				return output;
			}

			void inject_writer_native_transition_failure_for_testing() noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					fail_next_writer_native_transition_for_testing_ = true;
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
				}
			}

			void inject_writer_completion_transition_failure_for_testing() noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					fail_next_writer_completion_transition_for_testing_ = true;
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
				}
			}

			void inject_writer_attachment_seal_failure_for_testing() noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					fail_next_writer_attachment_seal_for_testing_ = true;
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
				}
			}

			void inject_registry_writer_incoming_liveness_loss_for_testing() noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					fail_next_registry_writer_incoming_liveness_for_testing_ = true;
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
				}
			}

			void inject_registry_writer_existing_liveness_loss_for_testing() noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					fail_next_registry_writer_existing_liveness_for_testing_ = true;
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
				}
			}

			void inject_registry_writer_pending_liveness_loss_for_testing() noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					fail_next_registry_writer_pending_liveness_for_testing_ = true;
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
				}
			}

			void abandon(const lease_token_kind kind, const std::uint64_t token) noexcept
			{
				if (token == 0U)
					return;
				try
				{
					std::scoped_lock lock{mutex_};
					if (kind == lease_token_kind::eligibility)
					{
						std::erase_if(eligibilities_,
									  [token](const eligibility_record& value)
									  {
										  return value.token == token;
									  });
						return;
					}
					if (token_exists_locked(kind, token))
					{
						switch (kind)
						{
							case lease_token_kind::writer_inflight:
							case lease_token_kind::writer_post_native:
							case lease_token_kind::pending:
							{
								const auto writer = find_by_token(writers_, token);
								if (writer != writers_.end())
								{
									writer->phase = writer_phase::terminal_quarantined;
									if (kind != lease_token_kind::writer_inflight)
									{
										const auto attachment = find_attachment_token_locked(token);
										if (attachment != writer_attachments_.end())
											attachment->phase =
												writer_attachment_phase::terminal_quarantined;
									}
								}
								break;
							}
							case lease_token_kind::writer_cleanup:
							{
								const auto attachment = find_attachment_cleanup_locked(token);
								if (attachment != writer_attachments_.end())
									attachment->phase =
										writer_attachment_phase::terminal_quarantined;
								break;
							}
							case lease_token_kind::holder:
							{
								const auto holder = find_by_token(holders_, token);
								if (holder != holders_.end())
								{
									holder->phase = holder_phase::terminal_quarantined;
									const auto attachment = find_attachment_token_locked(token);
									if (attachment != writer_attachments_.end())
										attachment->phase =
											writer_attachment_phase::terminal_quarantined;
								}
								break;
							}
							case lease_token_kind::reader_inflight:
							case lease_token_kind::reader_cleanup:
							{
								const auto reader = find_by_token(readers_, token);
								if (reader != readers_.end())
									reader->phase = reader_phase::terminal_quarantined;
								break;
							}
							case lease_token_kind::handoff:
							case lease_token_kind::reader_unmap:
							{
								const auto handoff = find_by_token(handoffs_, token);
								if (handoff != handoffs_.end())
									handoff->phase = handoff_phase::terminal_quarantined;
								break;
							}
							case lease_token_kind::eligibility:
								break;
						}
						quarantine_locked();
					}
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
				}
			}

			void shutdown() noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					alive_ = false;
					if (!writers_.empty() || !holders_.empty() || !readers_.empty() ||
						!handoffs_.empty() || generation_)
						quarantine_locked();
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
				}
			}

		  private:
			enum class writer_phase : std::uint8_t
			{
				inflight,
				post_native_mapping,
				pending,
				attachment_cleanup_sealed,
				terminal_quarantined,
			};

			enum class writer_route : std::uint8_t
			{
				map_before_gate,
				gate_before_map,
			};

			enum class writer_attachment_gate_phase : std::uint8_t
			{
				unsealed,
				positive_sealing,
				positive_active,
			};

			struct writer_native_transition_injected_failure
			{
			};

			struct writer_attachment_seal_injected_failure
			{
			};

			enum class reader_phase : std::uint8_t
			{
				inflight,
				post_native_rejected,
				cleanup_obligation,
				terminal_quarantined,
			};

			enum class holder_phase : std::uint8_t
			{
				active,
				attachment_cleanup_sealed,
				terminal_quarantined,
			};

			enum class writer_attachment_phase : std::uint8_t
			{
				collecting,
				nonlast_native_cleanup_admitted,
				last_waiting,
				last_native_cleanup_admitted,
				completion_committing,
				retired,
				terminal_quarantined,
			};

			struct eligibility_record
			{
				std::uint64_t token{};
				sqlite_shm_verified_writer_eligibility_receipt receipt;
			};

			struct writer_record
			{
				std::uint64_t token{};
				writer_phase phase{writer_phase::inflight};
				sqlite_shm_writer_map_request request;
				const volatile void* native_mapping{};
				std::optional<sqlite_shm_verified_writer_post_map_receipt> receipt;
				bool registry_bound{};
				std::optional<sqlite_shm_writer_member_authority> member_authority;
				writer_route route{writer_route::map_before_gate};
				std::uint64_t positive_gate_token{};
			};

			struct holder_record
			{
				std::uint64_t token{};
				std::uint64_t generation{};
				holder_phase phase{holder_phase::active};
				sqlite_shm_verified_writer_post_map_receipt map_receipt;
				sqlite_shm_verified_writer_eligibility_receipt eligibility_receipt;
				bool registry_bound{};
				std::optional<sqlite_shm_writer_member_authority> member_authority;
			};

			struct writer_attachment_member_record
			{
				std::uint64_t original_token{};
				std::uint64_t live_token{};
				bool confirmed_native_cleanup{};
			};

			struct writer_attachment_member_audit_record
			{
				std::uint64_t original_token{};
				std::uint64_t sealed_live_token{};
				sqlite_shm_writer_map_request request;
				const volatile void* native_mapping{};
				std::optional<sqlite_shm_verified_writer_post_map_receipt> post_map_receipt;
				std::optional<sqlite_shm_verified_writer_eligibility_receipt> promotion_receipt;
			};

			struct writer_attachment_record
			{
				sqlite_shm_native_attachment_identity identity;
				bool registry_bound_origin{};
				std::vector<writer_attachment_member_record> members;
				writer_attachment_phase phase{writer_attachment_phase::collecting};
				std::uint64_t cleanup_token{};
				std::uint64_t cleanup_generation{};
				std::optional<sqlite_shm_callback_execution_receipt> cleanup_callback;
				std::vector<std::uint64_t> sealed_complete_prefix;
				std::vector<writer_attachment_member_audit_record> sealed_member_audit;
				std::optional<sqlite_shm_verified_writer_eligibility_receipt>
					promotion_gate_receipt;
				writer_attachment_gate_phase gate_phase{writer_attachment_gate_phase::unsealed};
				std::uint64_t positive_gate_token{};
			};

			struct reader_record
			{
				std::uint64_t token{};
				reader_phase phase{reader_phase::inflight};
				sqlite_shm_reader_map_request request;
				std::uint64_t generation{};
				sqlite_shm_mapping_tuple expected_mapping;
				std::size_t mapping_page_count{};
				std::optional<sqlite_shm_verified_reader_post_map_receipt> receipt;
				std::optional<sqlite_shm_callback_execution_receipt> cleanup_callback;
			};

			enum class handoff_phase : std::uint8_t
			{
				active,
				native_cleanup_admitted,
				terminal_quarantined,
			};

			struct handoff_record
			{
				std::uint64_t token{};
				std::uint64_t generation{};
				handoff_phase phase{handoff_phase::active};
				sqlite_shm_verified_reader_post_map_receipt post_map_receipt;
				std::optional<sqlite_shm_callback_execution_receipt> unmap_callback;
			};

			struct generation_authority_record
			{
				sqlite_shm_verified_writer_post_map_receipt map_receipt;
				sqlite_shm_verified_writer_eligibility_receipt eligibility_receipt;
				bool active{true};
			};

			struct generation_record
			{
				std::uint64_t value{};
				sqlite_shm_mapping_generation_phase phase{
					sqlite_shm_mapping_generation_phase::live};
				std::uint64_t sealed_shm_size{};
				std::vector<sqlite_shm_mapping_tuple> pages;
				std::vector<generation_authority_record> authorities;
				std::size_t handoff_count{};
			};

			template <class Records>
			[[nodiscard]] static Records::iterator find_by_token(Records& records,
																 const std::uint64_t token)
			{
				return std::find_if(records.begin(),
									records.end(),
									[token](const auto& value)
									{
										return value.token == token;
									});
			}

			template <class Records>
			[[nodiscard]] static Records::const_iterator find_by_token(const Records& records,
																	   const std::uint64_t token)
			{
				return std::find_if(records.begin(),
									records.end(),
									[token](const auto& value)
									{
										return value.token == token;
									});
			}

			template <class TokenState>
			[[nodiscard]] bool owns(const std::shared_ptr<TokenState>& state,
									const std::uint64_t token) const noexcept
			{
				return token != 0U && state.get() == this;
			}

			[[nodiscard]] static bool same_native_attachment_lineage(
				const sqlite_shm_native_attachment_identity& left,
				const sqlite_shm_native_attachment_identity& right) noexcept
			{
				return left.family() == right.family() &&
					left.alias_lifetime() == right.alias_lifetime() &&
					left.connection_token() == right.connection_token() &&
					left.main_native_file_receipt() == right.main_native_file_receipt() &&
					left.main_xopen_receipt() == right.main_xopen_receipt() &&
					left.open_epoch() == right.open_epoch() &&
					left.callback_cohort() == right.callback_cohort();
			}

			[[nodiscard]] bool has_nonretired_native_attachment_lineage_locked(
				const sqlite_shm_native_attachment_identity& identity) const noexcept
			{
				return std::ranges::any_of(
					writer_attachments_,
					[&identity](const writer_attachment_record& attachment)
					{
						return attachment.phase != writer_attachment_phase::retired &&
							same_native_attachment_lineage(attachment.identity, identity);
					});
			}

			[[nodiscard]] bool exact_live_positive_gate_binding_locked(
				const writer_attachment_record& attachment) const noexcept
			{
				if (attachment.positive_gate_token == 0U || !attachment.promotion_gate_receipt)
					return false;
				const auto exact_binding_count = static_cast<std::size_t>(std::ranges::count_if(
					writer_attachments_,
					[&attachment](const writer_attachment_record& candidate)
					{
						return candidate.positive_gate_token == attachment.positive_gate_token;
					}));
				const auto gate = find_by_token(eligibilities_, attachment.positive_gate_token);
				return exact_binding_count == 1U && gate != eligibilities_.end() &&
					same_eligibility_receipt(*attachment.promotion_gate_receipt, gate->receipt) &&
					gate->receipt.family() == attachment.identity.family() &&
					gate->receipt.connection_token() == attachment.identity.connection_token() &&
					gate->receipt.open_epoch() == attachment.identity.open_epoch();
			}

			[[nodiscard]] std::vector<writer_attachment_record>::iterator
			find_attachment_epoch_locked(const sqlite_shm_native_attachment_identity& identity)
			{
				return std::find_if(writer_attachments_.begin(),
									writer_attachments_.end(),
									[&identity](const writer_attachment_record& attachment)
									{
										return attachment.identity.attachment_epoch() ==
											identity.attachment_epoch();
									});
			}

			[[nodiscard]] std::vector<writer_attachment_record>::const_iterator
			find_attachment_epoch_locked(
				const sqlite_shm_native_attachment_identity& identity) const
			{
				return std::find_if(writer_attachments_.begin(),
									writer_attachments_.end(),
									[&identity](const writer_attachment_record& attachment)
									{
										return attachment.identity.attachment_epoch() ==
											identity.attachment_epoch();
									});
			}

			void
			register_attachment_member_locked(const sqlite_shm_native_attachment_identity& identity,
											  const std::uint64_t token,
											  const bool registry_bound)
			{
				auto attachment = find_attachment_epoch_locked(identity);
				if (attachment == writer_attachments_.end())
				{
					writer_attachment_record record{identity,
													registry_bound,
													{},
													writer_attachment_phase::collecting,
													0U,
													0U,
													std::nullopt,
													{},
													{},
													std::nullopt};
					record.members.push_back({token, token, false});
					writer_attachments_.push_back(std::move(record));
					return;
				}
				attachment->members.push_back({token, token, false});
			}

			[[nodiscard]] std::vector<writer_attachment_record>::iterator
			find_attachment_token_locked(const std::uint64_t token) noexcept
			{
				return std::find_if(writer_attachments_.begin(),
									writer_attachments_.end(),
									[token](const writer_attachment_record& attachment)
									{
										return std::ranges::any_of(
											attachment.members,
											[token](const writer_attachment_member_record& member)
											{
												return member.live_token == token;
											});
									});
			}

			[[nodiscard]] std::vector<writer_attachment_record>::iterator
			find_attachment_cleanup_locked(const std::uint64_t cleanup_token) noexcept
			{
				return std::find_if(writer_attachments_.begin(),
									writer_attachments_.end(),
									[cleanup_token](const writer_attachment_record& attachment)
									{
										return attachment.cleanup_token == cleanup_token;
									});
			}

			[[nodiscard]] bool writer_matches_attachment_locked(
				const writer_record& writer,
				const writer_attachment_record& attachment) const noexcept
			{
				return writer.request.attachment == attachment.identity;
			}

			[[nodiscard]] bool holder_matches_attachment_locked(
				const holder_record& holder,
				const writer_attachment_record& attachment) const noexcept
			{
				return holder.map_receipt.request().attachment == attachment.identity;
			}

			[[nodiscard]] bool attachment_has_native_mapping_locked(
				const writer_attachment_record& attachment) const noexcept
			{
				for (const auto& member : attachment.members)
				{
					if (member.live_token == 0U)
						continue;
					const auto writer = find_by_token(writers_, member.live_token);
					if (writer != writers_.end() &&
						(writer->phase == writer_phase::post_native_mapping ||
						 writer->phase == writer_phase::pending))
						return true;
					const auto holder = find_by_token(holders_, member.live_token);
					if (holder != holders_.end() && holder->phase == holder_phase::active)
						return true;
				}
				return false;
			}

			[[nodiscard]] bool attachment_has_active_holder_locked(
				const writer_attachment_record& attachment) const noexcept
			{
				return std::ranges::any_of(
					attachment.members,
					[this, &attachment](const writer_attachment_member_record& member)
					{
						if (member.live_token == 0U)
							return false;
						const auto holder = find_by_token(holders_, member.live_token);
						return holder != holders_.end() && holder->phase == holder_phase::active &&
							holder_matches_attachment_locked(*holder, attachment);
					});
			}

			[[nodiscard]] std::size_t live_attachment_group_count_locked(
				const writer_attachment_record* excluded) const noexcept
			{
				return static_cast<std::size_t>(std::ranges::count_if(
					writer_attachments_,
					[this, excluded](const writer_attachment_record& attachment)
					{
						if (&attachment == excluded)
							return false;
						if (attachment.phase == writer_attachment_phase::collecting)
							return attachment_has_active_holder_locked(attachment);
						return attachment.phase == writer_attachment_phase::last_waiting ||
							attachment.phase ==
							writer_attachment_phase::last_native_cleanup_admitted;
					}));
			}

			struct attachment_complete_prefix
			{
				std::vector<std::uint64_t> tokens;
				std::vector<writer_attachment_member_audit_record> audit;
				bool has_inflight{};
				bool has_active_holder{};
				bool has_writer_native_member{};
			};

			[[nodiscard]] std::optional<attachment_complete_prefix>
			derive_complete_attachment_prefix_locked(
				const writer_attachment_record& attachment) const
			{
				if (attachment.phase != writer_attachment_phase::collecting ||
					attachment.members.empty())
					return std::nullopt;

				attachment_complete_prefix output;
				output.tokens.reserve(attachment.members.size());
				output.audit.reserve(attachment.members.size());
				for (const auto& member : attachment.members)
				{
					if (member.original_token == 0U || member.live_token == 0U ||
						std::ranges::find(output.tokens, member.live_token) !=
							output.tokens.end() ||
						std::ranges::any_of(
							output.audit,
							[&member](const writer_attachment_member_audit_record& audit)
							{
								return audit.original_token == member.original_token;
							}))
						return std::nullopt;
					const auto writer = find_by_token(writers_, member.live_token);
					const auto holder = find_by_token(holders_, member.live_token);
					if ((writer != writers_.end()) == (holder != holders_.end()))
						return std::nullopt;
					if (writer != writers_.end())
					{
						if (!writer_matches_attachment_locked(*writer, attachment) ||
							(writer->phase != writer_phase::inflight &&
							 writer->phase != writer_phase::post_native_mapping &&
							 writer->phase != writer_phase::pending))
							return std::nullopt;
						if ((writer->phase == writer_phase::inflight &&
							 writer->native_mapping != nullptr) ||
							((writer->phase == writer_phase::post_native_mapping ||
							  writer->phase == writer_phase::pending) &&
							 writer->native_mapping == nullptr) ||
							(writer->phase == writer_phase::post_native_mapping &&
							 writer->receipt.has_value()) ||
							(writer->phase == writer_phase::pending &&
							 !writer->receipt.has_value()))
							return std::nullopt;
						output.has_inflight =
							output.has_inflight || writer->phase == writer_phase::inflight;
						output.has_writer_native_member = output.has_writer_native_member ||
							writer->phase == writer_phase::post_native_mapping ||
							writer->phase == writer_phase::pending;
						output.audit.push_back({member.original_token,
												member.live_token,
												writer->request,
												writer->native_mapping,
												writer->receipt,
												std::nullopt});
					}
					else if (!holder_matches_attachment_locked(*holder, attachment) ||
							 holder->phase != holder_phase::active)
						return std::nullopt;
					else
					{
						if (holder->registry_bound != attachment.registry_bound_origin ||
							(holder->registry_bound && !holder->member_authority))
							return std::nullopt;
						output.has_active_holder = true;
						output.audit.push_back({member.original_token,
												member.live_token,
												holder->map_receipt.request(),
												holder->map_receipt.mapping().native_mapping,
												holder->map_receipt,
												holder->eligibility_receipt});
					}
					output.tokens.push_back(member.live_token);
				}

				const auto represented = [&output](const std::uint64_t token)
				{
					return std::ranges::find(output.tokens, token) != output.tokens.end();
				};
				if (std::ranges::any_of(writers_,
										[&attachment, &represented](const writer_record& writer)
										{
											return writer.request.attachment ==
												attachment.identity &&
												writer.phase !=
												writer_phase::terminal_quarantined &&
												!represented(writer.token);
										}) ||
					std::ranges::any_of(holders_,
										[&attachment, &represented](const holder_record& holder)
										{
											return holder.map_receipt.request().attachment ==
												attachment.identity &&
												holder.phase !=
												holder_phase::terminal_quarantined &&
												!represented(holder.token);
										}))
					return std::nullopt;
				return output;
			}

			enum class attachment_seal_status : std::uint8_t
			{
				sealed,
				fail_closed,
				invalid,
			};

			struct attachment_seal_result
			{
				attachment_seal_status status{attachment_seal_status::invalid};
				sqlite_shm_writer_retirement_decision decision{
					sqlite_shm_writer_retirement_decision::quarantined};
				std::uint64_t cleanup_token{};
				std::uint64_t generation{};
			};

			[[nodiscard]] bool prefix_contains_token(const std::vector<std::uint64_t>& prefix,
													 const std::uint64_t token) const noexcept
			{
				return std::ranges::find(prefix, token) != prefix.end();
			}

			[[nodiscard]] static bool same_writer_post_map_receipt(
				const sqlite_shm_verified_writer_post_map_receipt& left,
				const sqlite_shm_verified_writer_post_map_receipt& right) noexcept
			{
				return left.request() == right.request() &&
					left.open_epoch() == right.open_epoch() && left.mapping() == right.mapping() &&
					left.extend_pair() == right.extend_pair() &&
					left.holder_specific_effect_receipt() == right.holder_specific_effect_receipt();
			}

			[[nodiscard]] bool target_pages_have_exact_redundant_support_locked(
				const writer_attachment_record& target,
				const attachment_complete_prefix& complete) const noexcept
			{
				return std::ranges::all_of(
					complete.audit,
					[this, &target](const writer_attachment_member_audit_record& target_member)
					{
						if (!target_member.promotion_receipt || !target_member.post_map_receipt)
							return true;
						return std::ranges::any_of(
							holders_,
							[this, &target, &target_member](const holder_record& candidate)
							{
								if (candidate.phase != holder_phase::active ||
									candidate.map_receipt.request().attachment == target.identity ||
									// sealed_shm_size is generation high-water, not page support
									// identity; exact support is page/range/native-pointer based.
									!same_mapping_page(candidate.map_receipt.mapping(),
													   target_member.post_map_receipt->mapping()))
									return false;
								const auto other_attachment = find_attachment_epoch_locked(
									candidate.map_receipt.request().attachment);
								return generation_ &&
									other_attachment != writer_attachments_.end() &&
									other_attachment->phase ==
									writer_attachment_phase::collecting &&
									std::ranges::any_of(
										   generation_->authorities,
										   [&candidate](
											   const generation_authority_record& authority)
										   {
											   return authority.active &&
												   same_writer_post_map_receipt(
														  authority.map_receipt,
														  candidate.map_receipt) &&
												   same_eligibility_receipt(
														  authority.eligibility_receipt,
														  candidate.eligibility_receipt);
										   });
							});
					});
			}

			[[nodiscard]] std::optional<std::vector<std::size_t>>
			derive_target_generation_authorities_locked(
				const writer_attachment_record& target,
				const attachment_complete_prefix& complete) const
			{
				std::vector<std::size_t> output;
				const auto promoted_count = static_cast<std::size_t>(
					std::ranges::count_if(complete.audit,
										  [](const writer_attachment_member_audit_record& member)
										  {
											  return member.promotion_receipt.has_value();
										  }));
				output.reserve(promoted_count);
				if (promoted_count == 0U)
					return output;
				if (!generation_)
					return std::nullopt;

				for (const auto& member : complete.audit)
				{
					if (!member.promotion_receipt || !member.post_map_receipt)
						continue;
					std::optional<std::size_t> exact_index;
					for (std::size_t index = 0U; index < generation_->authorities.size(); ++index)
					{
						const auto& authority = generation_->authorities[index];
						if (!authority.active || std::ranges::find(output, index) != output.end() ||
							authority.map_receipt.request().attachment != target.identity ||
							!same_writer_post_map_receipt(authority.map_receipt,
														  *member.post_map_receipt) ||
							!same_eligibility_receipt(authority.eligibility_receipt,
													  *member.promotion_receipt))
							continue;
						exact_index = index;
						break;
					}
					if (!exact_index)
						return std::nullopt;
					output.push_back(*exact_index);
				}
				const auto active_target_authorities = static_cast<std::size_t>(
					std::ranges::count_if(generation_->authorities,
										  [&target](const generation_authority_record& authority)
										  {
											  return authority.active &&
												  authority.map_receipt.request().attachment ==
												  target.identity;
										  }));
				if (output.size() != promoted_count || active_target_authorities != promoted_count)
					return std::nullopt;
				return output;
			}

			[[nodiscard]] sqlite_shm_writer_retirement_decision
			retirement_decision_for_prefix_locked(
				const sqlite_shm_callback_execution_receipt& callback,
				const std::vector<std::uint64_t>& excluded_prefix) noexcept
			{
				bool has_blocker = false;
				bool has_same_thread_blocker = false;
				const auto observe = [&callback, &has_blocker, &has_same_thread_blocker](
										 const sqlite_shm_callback_execution_receipt& active)
				{
					has_blocker = true;
					if (active.thread_identity == callback.thread_identity)
						has_same_thread_blocker = true;
				};

				for (const auto& writer : writers_)
				{
					if (prefix_contains_token(excluded_prefix, writer.token) ||
						writer.phase == writer_phase::attachment_cleanup_sealed ||
						writer.phase == writer_phase::terminal_quarantined)
						continue;
					observe(writer.request.callback);
				}
				for (const auto& attachment : writer_attachments_)
				{
					if (attachment.phase == writer_attachment_phase::collecting ||
						attachment.phase == writer_attachment_phase::retired ||
						attachment.phase == writer_attachment_phase::terminal_quarantined ||
						!attachment.cleanup_callback ||
						attachment.sealed_complete_prefix == excluded_prefix)
						continue;
					observe(*attachment.cleanup_callback);
				}
				for (const auto& reader : readers_)
				{
					if (reader.phase == reader_phase::terminal_quarantined)
						continue;
					observe(reader.cleanup_callback ? *reader.cleanup_callback
													: reader.request.callback);
				}
				if (has_same_thread_blocker)
					return sqlite_shm_writer_retirement_decision::quarantine_same_thread;
				if (has_blocker)
					return sqlite_shm_writer_retirement_decision::wait_for_inflight;
				return sqlite_shm_writer_retirement_decision::ready;
			}

			[[nodiscard]] attachment_seal_result
			seal_attachment_cleanup_locked(const std::uint64_t anchor_token,
										   const sqlite_shm_callback_execution_receipt& callback,
										   const bool retain_wait_owner) noexcept
			{
				auto attachment = find_attachment_token_locked(anchor_token);
				if (attachment == writer_attachments_.end() ||
					attachment->phase != writer_attachment_phase::collecting)
					return {};
				const auto fail_closed = [this, &attachment]() noexcept
				{
					attachment->phase = writer_attachment_phase::terminal_quarantined;
					quarantine_locked();
					return attachment_seal_result{
						attachment_seal_status::fail_closed,
						sqlite_shm_writer_retirement_decision::quarantined};
				};

				try
				{
					if (std::exchange(fail_next_writer_attachment_seal_for_testing_, false))
						throw writer_attachment_seal_injected_failure{};
					if (!attachment_has_native_mapping_locked(*attachment))
						return fail_closed();

					auto complete = derive_complete_attachment_prefix_locked(*attachment);
					if (!complete)
						return fail_closed();
					if (complete->has_inflight ||
						(complete->has_active_holder && complete->has_writer_native_member))
					{
						// Production-inert bounded fence: a same-attachment inflight boundary, or a
						// post-native failure mixed with live holder authority, is not yet a
						// complete attachment cleanup proof. Do not expose a retryable/partial
						// owner.
						attachment->phase = writer_attachment_phase::terminal_quarantined;
						quarantine_locked();
						return {attachment_seal_status::fail_closed,
								sqlite_shm_writer_retirement_decision::quarantined};
					}

					auto target_authorities =
						derive_target_generation_authorities_locked(*attachment, *complete);
					if (!target_authorities)
						return fail_closed();

					const auto remaining_groups = live_attachment_group_count_locked(&*attachment);
					const auto generation = generation_ ? generation_->value : 0U;
					auto decision = remaining_groups != 0U || generation == 0U
						? sqlite_shm_writer_retirement_decision::not_last_attachment
						: retirement_decision_for_prefix_locked(callback, complete->tokens);
					if (decision == sqlite_shm_writer_retirement_decision::not_last_attachment &&
						generation != 0U &&
						!target_pages_have_exact_redundant_support_locked(*attachment, *complete))
					{
						// Production-inert bounded fence: retiring the only support for any live
						// generation page needs reader-predelegate ordering that belongs to the
						// next slice. Quarantine before a native-ready owner exists.
						attachment->phase = writer_attachment_phase::terminal_quarantined;
						quarantine_locked();
						return {attachment_seal_status::fail_closed,
								sqlite_shm_writer_retirement_decision::quarantined};
					}
					if (decision == sqlite_shm_writer_retirement_decision::quarantined)
						return fail_closed();
					if (decision == sqlite_shm_writer_retirement_decision::wait_for_inflight &&
						!retain_wait_owner)
					{
						// This API cannot return a valid sealed wait owner. Consume the exact
						// pending/post-native boundary instead of allowing a later retry.
						return fail_closed();
					}

					auto callback_copy = callback;
					auto sealed_prefix = complete->tokens;
					auto sealed_audit = complete->audit;
					const auto cleanup_token = allocate_token_locked();
					if (!cleanup_token)
						return fail_closed();

					// All potentially allocating copies are complete before phase/token ownership
					// is sealed. Moving these standard-library values is noexcept for their
					// allocators.
					static_assert(std::is_nothrow_move_constructible_v<
								  sqlite_shm_callback_execution_receipt>);
					static_assert(std::is_nothrow_move_assignable_v<std::vector<std::uint64_t>>);
					static_assert(std::is_nothrow_move_assignable_v<
								  std::vector<writer_attachment_member_audit_record>>);
					attachment->cleanup_callback.emplace(std::move(callback_copy));
					attachment->sealed_complete_prefix = std::move(sealed_prefix);
					attachment->sealed_member_audit = std::move(sealed_audit);
					attachment->cleanup_token = *cleanup_token;
					attachment->cleanup_generation = generation;
					for (const auto authority_index : *target_authorities)
						generation_->authorities[authority_index].active = false;
					for (const auto token : attachment->sealed_complete_prefix)
					{
						const auto writer = find_by_token(writers_, token);
						if (writer != writers_.end())
							writer->phase = writer_phase::attachment_cleanup_sealed;
						else
							find_by_token(holders_, token)->phase =
								holder_phase::attachment_cleanup_sealed;
					}
					if (decision == sqlite_shm_writer_retirement_decision::quarantine_same_thread)
					{
						attachment->phase = writer_attachment_phase::terminal_quarantined;
						quarantine_locked();
					}
					else if (decision == sqlite_shm_writer_retirement_decision::wait_for_inflight)
						attachment->phase = writer_attachment_phase::last_waiting;
					else if (remaining_groups != 0U || generation == 0U)
						attachment->phase =
							writer_attachment_phase::nonlast_native_cleanup_admitted;
					else
						attachment->phase = writer_attachment_phase::last_native_cleanup_admitted;
					if (generation != 0U && remaining_groups == 0U &&
						decision != sqlite_shm_writer_retirement_decision::quarantine_same_thread)
						generation_->phase = sqlite_shm_mapping_generation_phase::retiring;
					return {attachment_seal_status::sealed, decision, *cleanup_token, generation};
				}
				catch (...)
				{
					return fail_closed();
				}
			}

			[[nodiscard]] bool
			release_attachment_member_locked(const std::uint64_t token,
											 const bool confirmed_native_cleanup) noexcept
			{
				for (auto& attachment : writer_attachments_)
				{
					const auto member =
						std::find_if(attachment.members.begin(),
									 attachment.members.end(),
									 [token](const writer_attachment_member_record& value)
									 {
										 return value.live_token == token;
									 });
					if (member == attachment.members.end())
						continue;
					if (!confirmed_native_cleanup)
					{
						attachment.members.erase(member);
						return true;
					}
					member->live_token = 0U;
					member->confirmed_native_cleanup = true;
					if (std::ranges::none_of(attachment.members,
											 [](const writer_attachment_member_record& value)
											 {
												 return value.live_token != 0U;
											 }))
						attachment.phase = writer_attachment_phase::retired;
					return true;
				}
				return false;
			}

			[[nodiscard]] std::optional<std::uint64_t> allocate_token_locked() noexcept
			{
				if (token_exhausted_)
					return std::nullopt;
				return allocate_token_unchecked_locked();
			}

			[[nodiscard]] bool can_allocate_tokens_locked(const std::size_t count) const noexcept
			{
				if (token_exhausted_ || count == 0U)
					return false;
				const auto available = std::numeric_limits<std::uint64_t>::max() - next_token_ + 1U;
				return count <= available;
			}

			[[nodiscard]] std::uint64_t allocate_token_unchecked_locked() noexcept
			{
				const auto output = next_token_;
				if (next_token_ == std::numeric_limits<std::uint64_t>::max())
					token_exhausted_ = true;
				else
					++next_token_;
				return output;
			}

			[[nodiscard]] bool
			callback_can_start_locked(const sqlite_shm_callback_execution_receipt& callback,
									  const std::uint64_t excluded_writer_token = 0U) const noexcept
			{
				if (!valid_callback(callback))
					return false;
				const auto ordered_after =
					[&callback](const sqlite_shm_callback_execution_receipt& active)
				{
					return active.invocation_token != callback.invocation_token &&
						(active.thread_identity != callback.thread_identity ||
						 callback.reentrancy_depth > active.reentrancy_depth);
				};
				if (std::ranges::any_of(
						writers_,
						[&ordered_after, excluded_writer_token](const writer_record& writer)
						{
							return writer.token != excluded_writer_token &&
								(writer.phase == writer_phase::inflight ||
								 writer.phase == writer_phase::post_native_mapping) &&
								!ordered_after(writer.request.callback);
						}))
					return false;
				if (std::ranges::any_of(
						writer_attachments_,
						[&ordered_after](const writer_attachment_record& attachment)
						{
							return attachment.phase != writer_attachment_phase::collecting &&
								attachment.phase != writer_attachment_phase::retired &&
								attachment.phase != writer_attachment_phase::terminal_quarantined &&
								attachment.cleanup_callback &&
								!ordered_after(*attachment.cleanup_callback);
						}))
					return false;
				if (std::ranges::any_of(readers_,
										[&ordered_after](const reader_record& reader)
										{
											return reader.phase !=
												reader_phase::terminal_quarantined &&
												!ordered_after(reader.cleanup_callback
																   ? *reader.cleanup_callback
																   : reader.request.callback);
										}))
					return false;
				return std::ranges::none_of(handoffs_,
											[&ordered_after](const handoff_record& handoff)
											{
												return handoff.phase ==
													handoff_phase::native_cleanup_admitted &&
													handoff.unmap_callback &&
													!ordered_after(*handoff.unmap_callback);
											});
			}

			[[nodiscard]] bool admit_cleanup_callback_locked(
				const sqlite_shm_callback_execution_receipt& original,
				const sqlite_shm_callback_execution_receipt& cleanup) const noexcept
			{
				return cleanup == original || callback_can_start_locked(cleanup);
			}

			[[nodiscard]] bool admit_writer_cleanup_callback_locked(
				const std::uint64_t anchor_writer_token,
				const sqlite_shm_callback_execution_receipt& original,
				const sqlite_shm_callback_execution_receipt& cleanup) const noexcept
			{
				return cleanup == original ? callback_can_start_locked(cleanup, anchor_writer_token)
										   : callback_can_start_locked(cleanup);
			}

			[[nodiscard]] std::optional<sqlite_shm_lease_rejection>
			blocked_locked(const sqlite_shm_lease_recovery_action action) noexcept
			{
				if (is_quarantined_locked())
					return rejection(sqlite_shm_lease_rejection_reason::quarantined, action);
				const auto exact_member_liveness_lost =
					std::ranges::any_of(
						writers_,
						[](const writer_record& writer)
						{
							return writer.registry_bound &&
								(!writer.member_authority ||
								 !writer.member_authority->retains_exact_lifetimes(writer.request));
						}) ||
					std::ranges::any_of(holders_,
										[](const holder_record& holder)
										{
											return holder.registry_bound &&
												(!holder.member_authority ||
												 !holder.member_authority->retains_exact_lifetimes(
													 holder.map_receipt.request()));
										});
				if (registry_member_admission_blocked_ || exact_member_liveness_lost)
				{
					registry_member_admission_blocked_ = true;
					registry_member_sticky_quarantine_ = true;
					return rejection(sqlite_shm_lease_rejection_reason::quarantined, action);
				}
				if (!alive_ || !generations_ || !generations_->state_)
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 action);
				return std::nullopt;
			}

			[[nodiscard]] bool is_quarantined_locked() const noexcept
			{
				return quarantined_ || emergency_quarantine_.load(std::memory_order_acquire);
			}

			void quarantine_locked() noexcept
			{
				quarantined_ = true;
				if (generation_)
					generation_->phase = sqlite_shm_mapping_generation_phase::quarantined;
			}

			void quarantine_after_native() noexcept
			{
				emergency_quarantine_.store(true, std::memory_order_release);
				try
				{
					std::scoped_lock lock{mutex_};
					quarantine_locked();
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
				}
			}

			[[nodiscard]] static sqlite_shm_lease_rejection
			stale_token(const sqlite_shm_lease_recovery_action action) noexcept
			{
				return rejection(sqlite_shm_lease_rejection_reason::stale_token, action);
			}

			[[nodiscard]] static sqlite_shm_lease_rejection ambiguous() noexcept
			{
				return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								 sqlite_shm_lease_recovery_action::quarantine_no_retry);
			}

			[[nodiscard]] static bool
			same_effect_gate_receipt(const sqlite_backend_effect_arm_receipt& left,
									 const sqlite_backend_effect_arm_receipt& right) noexcept
			{
				return left.profile == right.profile &&
					left.capability_token == right.capability_token &&
					left.connection_token == right.connection_token &&
					left.canonical_vfs_locator == right.canonical_vfs_locator &&
					left.prerequisite_receipt == right.prerequisite_receipt &&
					left.validation_receipt == right.validation_receipt &&
					left.stage == right.stage && left.sequence == right.sequence &&
					left.armed_after_underlying_exclusive_lock ==
					right.armed_after_underlying_exclusive_lock;
			}

			[[nodiscard]] static bool same_eligibility_receipt(
				const sqlite_shm_verified_writer_eligibility_receipt& left,
				const sqlite_shm_verified_writer_eligibility_receipt& right) noexcept
			{
				return left.family() == right.family() &&
					left.connection_token() == right.connection_token() &&
					left.open_epoch() == right.open_epoch() &&
					same_effect_gate_receipt(left.effect_gate(), right.effect_gate()) &&
					left.complete_current_v3_gate() == right.complete_current_v3_gate();
			}

			[[nodiscard]] bool valid_eligibility(
				const sqlite_shm_verified_writer_eligibility_receipt& receipt) const noexcept
			{
				const auto& effect = receipt.effect_gate();
				return valid_family(receipt.family()) &&
					valid_identity(receipt.connection_token()) &&
					valid_identity(receipt.open_epoch()) &&
					valid_identity(receipt.complete_current_v3_gate()) &&
					valid_identity(effect.capability_token) &&
					valid_identity(effect.connection_token) &&
					valid_identity(effect.prerequisite_receipt) &&
					valid_identity(effect.validation_receipt) &&
					!effect.canonical_vfs_locator.empty() &&
					effect.connection_token == receipt.connection_token() &&
					effect.stage == sqlite_backend_effect_stage::fully_armed &&
					effect.sequence != 0U;
			}

			[[nodiscard]] bool valid_writer_receipt(
				const sqlite_shm_verified_writer_post_map_receipt& receipt) const noexcept
			{
				if (!valid_writer_request(receipt.request()) ||
					!valid_identity(receipt.open_epoch()) || !valid_mapping(receipt.mapping()) ||
					!valid_identity(receipt.holder_specific_effect_receipt()) ||
					receipt.open_epoch() != receipt.request().attachment.open_epoch() ||
					receipt.mapping().page_number != receipt.request().page_number ||
					receipt.mapping().page_size != receipt.request().page_size)
					return false;
				return (receipt.extend_pair() == sqlite_shm_writer_extend_pair::one_one &&
						receipt.request().caller_extend == 1) ||
					(receipt.extend_pair() == sqlite_shm_writer_extend_pair::zero_zero &&
					 receipt.request().caller_extend == 0);
			}

			[[nodiscard]] bool valid_reader_receipt(
				const sqlite_shm_verified_reader_post_map_receipt& receipt) const noexcept
			{
				return valid_reader_request(receipt.request()) && receipt.generation() != 0U &&
					valid_mapping(receipt.mapping()) &&
					valid_identity(receipt.zero_resize_effect_receipt());
			}

			[[nodiscard]] static bool
			join_mapping(generation_record& generation,
						 const sqlite_shm_verified_writer_post_map_receipt& receipt)
			{
				const auto page =
					std::find_if(generation.pages.begin(),
								 generation.pages.end(),
								 [&receipt](const sqlite_shm_mapping_tuple& value)
								 {
									 return value.page_number == receipt.mapping().page_number;
								 });
				if (page != generation.pages.end())
					return same_mapping_page(*page, receipt.mapping()) &&
						receipt.mapping().sealed_shm_size == generation.sealed_shm_size;
				if (receipt.extend_pair() != sqlite_shm_writer_extend_pair::one_one ||
					generation.pages.empty() ||
					receipt.mapping().page_size != generation.pages.front().page_size ||
					receipt.mapping().sealed_shm_size < generation.sealed_shm_size)
					return false;
				generation.pages.push_back(receipt.mapping());
				std::ranges::sort(generation.pages,
								  {},
								  [](const sqlite_shm_mapping_tuple& value)
								  {
									  return value.page_number;
								  });
				generation.sealed_shm_size = receipt.mapping().sealed_shm_size;
				return true;
			}

			[[nodiscard]] std::vector<sqlite_shm_mapping_tuple>::iterator
			find_page_locked(const int page_number)
			{
				return std::find_if(generation_->pages.begin(),
									generation_->pages.end(),
									[page_number](const sqlite_shm_mapping_tuple& value)
									{
										return value.page_number == page_number;
									});
			}

			[[nodiscard]] std::size_t active_holder_count_locked() const noexcept
			{
				return static_cast<std::size_t>(
					std::ranges::count_if(holders_,
										  [](const holder_record& holder)
										  {
											  return holder.phase == holder_phase::active;
										  }));
			}

			[[nodiscard]] bool token_exists_locked(const lease_token_kind kind,
												   const std::uint64_t token) const
			{
				switch (kind)
				{
					case lease_token_kind::eligibility:
						return find_by_token(eligibilities_, token) != eligibilities_.end();
					case lease_token_kind::writer_inflight:
					{
						const auto found = find_by_token(writers_, token);
						return found != writers_.end() && found->phase == writer_phase::inflight;
					}
					case lease_token_kind::writer_post_native:
					{
						const auto found = find_by_token(writers_, token);
						return found != writers_.end() &&
							found->phase == writer_phase::post_native_mapping;
					}
					case lease_token_kind::pending:
					{
						const auto found = find_by_token(writers_, token);
						return found != writers_.end() && found->phase == writer_phase::pending;
					}
					case lease_token_kind::writer_cleanup:
					{
						const auto attachment =
							std::find_if(writer_attachments_.begin(),
										 writer_attachments_.end(),
										 [token](const writer_attachment_record& value)
										 {
											 return value.cleanup_token == token;
										 });
						return attachment != writer_attachments_.end() &&
							attachment->phase != writer_attachment_phase::retired &&
							attachment->phase != writer_attachment_phase::terminal_quarantined;
					}
					case lease_token_kind::holder:
					{
						const auto holder = find_by_token(holders_, token);
						return holder != holders_.end() && holder->phase == holder_phase::active;
					}
					case lease_token_kind::reader_inflight:
					{
						const auto reader = find_by_token(readers_, token);
						return reader != readers_.end() &&
							(reader->phase == reader_phase::inflight ||
							 reader->phase == reader_phase::post_native_rejected);
					}
					case lease_token_kind::reader_cleanup:
					{
						const auto reader = find_by_token(readers_, token);
						return reader != readers_.end() &&
							reader->phase == reader_phase::cleanup_obligation;
					}
					case lease_token_kind::handoff:
					{
						const auto handoff = find_by_token(handoffs_, token);
						return handoff != handoffs_.end() &&
							handoff->phase == handoff_phase::active;
					}
					case lease_token_kind::reader_unmap:
					{
						const auto handoff = find_by_token(handoffs_, token);
						return handoff != handoffs_.end() &&
							handoff->phase == handoff_phase::native_cleanup_admitted;
					}
				}
				return true;
			}

			mutable std::mutex mutex_;
			sqlite_shm_lease_family_binding family_;
			std::shared_ptr<sqlite_shm_mapping_generation_source> generations_;
			std::vector<eligibility_record> eligibilities_;
			// Node stability prevents an unrelated erase from move-assigning over a live
			// registry-bound authority while the lease mutex is held.
			std::list<writer_record> writers_;
			std::list<holder_record> holders_;
			std::vector<writer_attachment_record> writer_attachments_;
			std::vector<reader_record> readers_;
			std::vector<handoff_record> handoffs_;
			std::optional<generation_record> generation_;
			std::uint64_t next_token_{1U};
			bool token_exhausted_{};
			bool alive_{true};
			bool quarantined_{};
			bool fail_next_writer_native_transition_for_testing_{};
			bool fail_next_writer_attachment_seal_for_testing_{};
			bool fail_next_writer_completion_transition_for_testing_{};
			bool registry_member_admission_blocked_{};
			bool registry_member_sticky_quarantine_{};
			bool fail_next_registry_writer_incoming_liveness_for_testing_{};
			bool fail_next_registry_writer_existing_liveness_for_testing_{};
			bool fail_next_registry_writer_pending_liveness_for_testing_{};
			std::atomic_bool emergency_quarantine_{false};
		};
	} // namespace detail

	sqlite_shm_writer_eligibility::sqlite_shm_writer_eligibility(
		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
		const std::uint64_t token) noexcept
		: state_{std::move(state)}, token_{token}
	{
	}

	sqlite_shm_writer_eligibility::~sqlite_shm_writer_eligibility() noexcept
	{
		if (state_)
			state_->abandon(lease_token_kind::eligibility, token_);
	}

	sqlite_shm_writer_eligibility::sqlite_shm_writer_eligibility(
		sqlite_shm_writer_eligibility&& other) noexcept
		: state_{std::move(other.state_)}, token_{std::exchange(other.token_, 0U)}
	{
	}

	bool sqlite_shm_writer_eligibility::valid() const noexcept
	{
		return state_ != nullptr && token_ != 0U;
	}

	void sqlite_shm_writer_eligibility::disarm() noexcept
	{
		token_ = 0U;
		state_.reset();
	}

	sqlite_shm_writer_map_inflight::sqlite_shm_writer_map_inflight(
		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
		const std::uint64_t token) noexcept
		: state_{std::move(state)}, token_{token}
	{
	}

	sqlite_shm_writer_map_inflight::~sqlite_shm_writer_map_inflight() noexcept
	{
		if (state_)
			state_->abandon(lease_token_kind::writer_inflight, token_);
	}

	sqlite_shm_writer_map_inflight::sqlite_shm_writer_map_inflight(
		sqlite_shm_writer_map_inflight&& other) noexcept
		: state_{std::move(other.state_)}, token_{std::exchange(other.token_, 0U)},
		  native_result_validated_{std::exchange(other.native_result_validated_, false)},
		  native_result_observed_{std::exchange(other.native_result_observed_, false)},
		  native_result_validation_ambiguous_{
			  std::exchange(other.native_result_validation_ambiguous_, false)}
	{
	}

	bool sqlite_shm_writer_map_inflight::valid() const noexcept
	{
		return state_ != nullptr && token_ != 0U;
	}

	void sqlite_shm_writer_map_inflight::disarm() noexcept
	{
		token_ = 0U;
		native_result_validated_ = false;
		native_result_observed_ = false;
		native_result_validation_ambiguous_ = false;
		state_.reset();
	}

	sqlite_shm_writer_post_native_mapping::sqlite_shm_writer_post_native_mapping(
		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
		const std::uint64_t token) noexcept
		: state_{std::move(state)}, token_{token}
	{
	}

	sqlite_shm_writer_post_native_mapping::~sqlite_shm_writer_post_native_mapping() noexcept
	{
		if (state_)
			state_->abandon(lease_token_kind::writer_post_native, token_);
	}

	sqlite_shm_writer_post_native_mapping::sqlite_shm_writer_post_native_mapping(
		sqlite_shm_writer_post_native_mapping&& other) noexcept
		: state_{std::move(other.state_)}, token_{std::exchange(other.token_, 0U)}
	{
	}

	bool sqlite_shm_writer_post_native_mapping::valid() const noexcept
	{
		return state_ != nullptr && token_ != 0U;
	}

	void sqlite_shm_writer_post_native_mapping::disarm() noexcept
	{
		token_ = 0U;
		state_.reset();
	}

	sqlite_shm_pending_mapping::sqlite_shm_pending_mapping(
		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
		const std::uint64_t token) noexcept
		: state_{std::move(state)}, token_{token}
	{
	}

	sqlite_shm_pending_mapping::~sqlite_shm_pending_mapping() noexcept
	{
		if (state_)
			state_->abandon(lease_token_kind::pending, token_);
	}

	sqlite_shm_pending_mapping::sqlite_shm_pending_mapping(
		sqlite_shm_pending_mapping&& other) noexcept
		: state_{std::move(other.state_)}, token_{std::exchange(other.token_, 0U)}
	{
	}

	bool sqlite_shm_pending_mapping::valid() const noexcept
	{
		return state_ != nullptr && token_ != 0U;
	}

	void sqlite_shm_pending_mapping::disarm() noexcept
	{
		token_ = 0U;
		state_.reset();
	}

	sqlite_shm_writer_attachment_cleanup::sqlite_shm_writer_attachment_cleanup(
		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
		const detail::sqlite_shm_lease_token_identity token,
		const detail::sqlite_shm_mapping_generation_identity generation) noexcept
		: state_{std::move(state)}, token_{token.value}, generation_{generation.value}
	{
	}

	sqlite_shm_writer_attachment_cleanup::~sqlite_shm_writer_attachment_cleanup() noexcept
	{
		if (state_)
			state_->abandon(lease_token_kind::writer_cleanup, token_);
	}

	sqlite_shm_writer_attachment_cleanup::sqlite_shm_writer_attachment_cleanup(
		sqlite_shm_writer_attachment_cleanup&& other) noexcept
		: state_{std::move(other.state_)}, token_{std::exchange(other.token_, 0U)},
		  generation_{std::exchange(other.generation_, 0U)}
	{
	}

	bool sqlite_shm_writer_attachment_cleanup::valid() const noexcept
	{
		return state_ != nullptr && token_ != 0U;
	}

	std::uint64_t sqlite_shm_writer_attachment_cleanup::generation() const noexcept
	{
		return generation_;
	}

	void sqlite_shm_writer_attachment_cleanup::disarm() noexcept
	{
		token_ = 0U;
		generation_ = 0U;
		state_.reset();
	}

	sqlite_shm_writer_holder::sqlite_shm_writer_holder(
		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
		const detail::sqlite_shm_lease_token_identity token,
		const detail::sqlite_shm_mapping_generation_identity generation) noexcept
		: state_{std::move(state)}, token_{token.value}, generation_{generation.value}
	{
	}

	sqlite_shm_writer_holder::~sqlite_shm_writer_holder() noexcept
	{
		if (state_)
			state_->abandon(lease_token_kind::holder, token_);
	}

	sqlite_shm_writer_holder::sqlite_shm_writer_holder(sqlite_shm_writer_holder&& other) noexcept
		: state_{std::move(other.state_)}, token_{std::exchange(other.token_, 0U)},
		  generation_{std::exchange(other.generation_, 0U)}
	{
	}

	bool sqlite_shm_writer_holder::valid() const noexcept
	{
		return state_ != nullptr && token_ != 0U && generation_ != 0U;
	}

	std::uint64_t sqlite_shm_writer_holder::generation() const noexcept
	{
		return generation_;
	}

	void sqlite_shm_writer_holder::disarm() noexcept
	{
		token_ = 0U;
		generation_ = 0U;
		state_.reset();
	}

	sqlite_shm_reader_map_inflight::sqlite_shm_reader_map_inflight(
		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
		const detail::sqlite_shm_lease_token_identity token,
		const detail::sqlite_shm_mapping_generation_identity generation) noexcept
		: state_{std::move(state)}, token_{token.value}, generation_{generation.value}
	{
	}

	sqlite_shm_reader_map_inflight::~sqlite_shm_reader_map_inflight() noexcept
	{
		if (state_)
			state_->abandon(lease_token_kind::reader_inflight, token_);
	}

	sqlite_shm_reader_map_inflight::sqlite_shm_reader_map_inflight(
		sqlite_shm_reader_map_inflight&& other) noexcept
		: state_{std::move(other.state_)}, token_{std::exchange(other.token_, 0U)},
		  generation_{std::exchange(other.generation_, 0U)}
	{
	}

	bool sqlite_shm_reader_map_inflight::valid() const noexcept
	{
		return state_ != nullptr && token_ != 0U && generation_ != 0U;
	}

	std::uint64_t sqlite_shm_reader_map_inflight::generation() const noexcept
	{
		return generation_;
	}

	void sqlite_shm_reader_map_inflight::disarm() noexcept
	{
		token_ = 0U;
		generation_ = 0U;
		state_.reset();
	}

	sqlite_shm_reader_cleanup_obligation::sqlite_shm_reader_cleanup_obligation(
		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
		const detail::sqlite_shm_lease_token_identity token,
		const detail::sqlite_shm_mapping_generation_identity generation) noexcept
		: state_{std::move(state)}, token_{token.value}, generation_{generation.value}
	{
	}

	sqlite_shm_reader_cleanup_obligation::~sqlite_shm_reader_cleanup_obligation() noexcept
	{
		if (state_)
			state_->abandon(lease_token_kind::reader_cleanup, token_);
	}

	sqlite_shm_reader_cleanup_obligation::sqlite_shm_reader_cleanup_obligation(
		sqlite_shm_reader_cleanup_obligation&& other) noexcept
		: state_{std::move(other.state_)}, token_{std::exchange(other.token_, 0U)},
		  generation_{std::exchange(other.generation_, 0U)}
	{
	}

	bool sqlite_shm_reader_cleanup_obligation::valid() const noexcept
	{
		return state_ != nullptr && token_ != 0U && generation_ != 0U;
	}

	std::uint64_t sqlite_shm_reader_cleanup_obligation::generation() const noexcept
	{
		return generation_;
	}

	void sqlite_shm_reader_cleanup_obligation::disarm() noexcept
	{
		token_ = 0U;
		generation_ = 0U;
		state_.reset();
	}

	sqlite_shm_reader_handoff::sqlite_shm_reader_handoff(
		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
		const detail::sqlite_shm_lease_token_identity token,
		const detail::sqlite_shm_mapping_generation_identity generation) noexcept
		: state_{std::move(state)}, token_{token.value}, generation_{generation.value}
	{
	}

	sqlite_shm_reader_handoff::~sqlite_shm_reader_handoff() noexcept
	{
		if (state_)
			state_->abandon(lease_token_kind::handoff, token_);
	}

	sqlite_shm_reader_handoff::sqlite_shm_reader_handoff(sqlite_shm_reader_handoff&& other) noexcept
		: state_{std::move(other.state_)}, token_{std::exchange(other.token_, 0U)},
		  generation_{std::exchange(other.generation_, 0U)}
	{
	}

	bool sqlite_shm_reader_handoff::valid() const noexcept
	{
		return state_ != nullptr && token_ != 0U && generation_ != 0U;
	}

	std::uint64_t sqlite_shm_reader_handoff::generation() const noexcept
	{
		return generation_;
	}

	void sqlite_shm_reader_handoff::disarm() noexcept
	{
		token_ = 0U;
		generation_ = 0U;
		state_.reset();
	}

	sqlite_shm_reader_unmap_obligation::sqlite_shm_reader_unmap_obligation(
		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
		const detail::sqlite_shm_lease_token_identity token,
		const detail::sqlite_shm_mapping_generation_identity generation) noexcept
		: state_{std::move(state)}, token_{token.value}, generation_{generation.value}
	{
	}

	sqlite_shm_reader_unmap_obligation::~sqlite_shm_reader_unmap_obligation() noexcept
	{
		if (state_)
			state_->abandon(lease_token_kind::reader_unmap, token_);
	}

	sqlite_shm_reader_unmap_obligation::sqlite_shm_reader_unmap_obligation(
		sqlite_shm_reader_unmap_obligation&& other) noexcept
		: state_{std::move(other.state_)}, token_{std::exchange(other.token_, 0U)},
		  generation_{std::exchange(other.generation_, 0U)}
	{
	}

	bool sqlite_shm_reader_unmap_obligation::valid() const noexcept
	{
		return state_ != nullptr && token_ != 0U && generation_ != 0U;
	}

	std::uint64_t sqlite_shm_reader_unmap_obligation::generation() const noexcept
	{
		return generation_;
	}

	void sqlite_shm_reader_unmap_obligation::disarm() noexcept
	{
		token_ = 0U;
		generation_ = 0U;
		state_.reset();
	}

	sqlite_shm_writer_release::sqlite_shm_writer_release(
		const sqlite_shm_writer_retirement_decision decision,
		const std::uint64_t generation,
		sqlite_shm_writer_attachment_cleanup cleanup) noexcept
		: decision_{decision}, generation_{generation}, cleanup_{std::move(cleanup)}
	{
	}

	sqlite_shm_writer_release::~sqlite_shm_writer_release() noexcept = default;

	sqlite_shm_writer_release::sqlite_shm_writer_release(sqlite_shm_writer_release&& other) noexcept
		: decision_{
			  std::exchange(other.decision_, sqlite_shm_writer_retirement_decision::quarantined)},
		  generation_{std::exchange(other.generation_, 0U)}, cleanup_{std::move(other.cleanup_)}
	{
	}

	sqlite_shm_writer_retirement_decision sqlite_shm_writer_release::decision() const noexcept
	{
		return decision_;
	}

	std::uint64_t sqlite_shm_writer_release::generation() const noexcept
	{
		return generation_;
	}

	sqlite_shm_writer_attachment_cleanup& sqlite_shm_writer_release::cleanup() noexcept
	{
		return cleanup_;
	}

	sqlite_same_process_shm_mapping_lease_coordinator::
		sqlite_same_process_shm_mapping_lease_coordinator(
			sqlite_shm_lease_family_binding family,
			std::shared_ptr<sqlite_shm_mapping_generation_source> generations)
		: state_{std::make_shared<detail::sqlite_shm_mapping_lease_state>(std::move(family),
																		  std::move(generations))}
	{
	}

	sqlite_same_process_shm_mapping_lease_coordinator::
		~sqlite_same_process_shm_mapping_lease_coordinator() noexcept
	{
		if (state_)
			state_->shutdown();
	}

	sqlite_shm_lease_result<sqlite_shm_writer_eligibility>
	sqlite_same_process_shm_mapping_lease_coordinator::install_writer_eligibility(
		const sqlite_shm_verified_writer_eligibility_receipt& receipt)
	{
		return state_->install_eligibility(receipt);
	}

	sqlite_shm_lease_result<void>
	sqlite_same_process_shm_mapping_lease_coordinator::revoke_writer_eligibility(
		sqlite_shm_writer_eligibility& eligibility) noexcept
	{
		return state_->revoke_eligibility(eligibility);
	}

	sqlite_shm_lease_result<sqlite_shm_writer_map_inflight>
	sqlite_same_process_shm_mapping_lease_coordinator::begin_writer_map(
		const sqlite_shm_writer_map_request& request)
	{
		return state_->begin_writer(request);
	}

	sqlite_shm_lease_result<sqlite_shm_writer_map_inflight>
	sqlite_same_process_shm_mapping_lease_coordinator::begin_registry_writer_map(
		const sqlite_shm_writer_map_request& request, sqlite_shm_writer_member_authority& authority)
	{
		return state_->begin_registry_writer(request, authority);
	}

	sqlite_shm_lease_result<sqlite_shm_writer_post_native_mapping>
	sqlite_same_process_shm_mapping_lease_coordinator::record_writer_native_mapping(
		sqlite_shm_writer_map_inflight& inflight,
		const sqlite_shm_verified_writer_native_map_receipt& receipt) noexcept
	{
		return state_->record_native_mapping(inflight, receipt);
	}

	sqlite_shm_lease_result<sqlite_shm_pending_mapping>
	sqlite_same_process_shm_mapping_lease_coordinator::install_pending(
		sqlite_shm_writer_post_native_mapping& post_native,
		const sqlite_shm_verified_writer_post_map_receipt& receipt)
	{
		return state_->install_pending(post_native, receipt);
	}

	sqlite_shm_lease_result<sqlite_shm_pending_mapping>
	sqlite_same_process_shm_mapping_lease_coordinator::install_registry_writer_pending(
		sqlite_shm_registry_family_pin& family,
		sqlite_shm_writer_post_native_mapping& post_native,
		sqlite_shm_verified_writer_post_map_receipt receipt)
	{
		return state_->install_registry_pending(family, post_native, std::move(receipt));
	}

	sqlite_shm_lease_result<std::vector<sqlite_shm_writer_holder>>
	sqlite_same_process_shm_mapping_lease_coordinator::promote_registry_writer_attachment_group(
		sqlite_shm_registry_family_pin& family,
		const std::span<sqlite_shm_pending_mapping*> pending,
		const sqlite_shm_writer_eligibility& eligibility)
	{
		return state_->promote_registry_writer_attachment_group(family, pending, eligibility);
	}

	sqlite_shm_lease_result<sqlite_shm_positive_writer_attachment_gate_result>
	sqlite_same_process_shm_mapping_lease_coordinator::
		advance_positive_registry_writer_attachment_gate(
			sqlite_shm_registry_family_pin& family,
			const sqlite_shm_native_attachment_identity& attachment,
			const std::span<sqlite_shm_pending_mapping*> pending,
			const sqlite_shm_writer_eligibility& eligibility)
	{
		return state_->advance_positive_registry_writer_attachment_gate(
			family, attachment, pending, eligibility);
	}

	sqlite_shm_lease_result<sqlite_shm_writer_holder>
	sqlite_same_process_shm_mapping_lease_coordinator::
		complete_gate_winning_registry_writer_map_before_callback_return(
			sqlite_shm_registry_family_pin& family,
			sqlite_shm_writer_post_native_mapping& post_native,
			const sqlite_shm_verified_writer_post_map_receipt& receipt)
	{
		return state_->complete_gate_winning_registry_writer_map_before_callback_return(
			family, post_native, receipt);
	}

	sqlite_shm_lease_result<sqlite_shm_writer_holder>
	sqlite_same_process_shm_mapping_lease_coordinator::promote_writer(
		sqlite_shm_pending_mapping& pending, const sqlite_shm_writer_eligibility& eligibility)
	{
		return state_->promote(pending, eligibility);
	}

	sqlite_shm_lease_result<void>
	sqlite_same_process_shm_mapping_lease_coordinator::resolve_writer_map_failure(
		sqlite_shm_writer_map_inflight& inflight) noexcept
	{
		return state_->resolve_writer_failure(inflight);
	}

	sqlite_shm_lease_result<sqlite_shm_writer_attachment_cleanup>
	sqlite_same_process_shm_mapping_lease_coordinator::begin_writer_cleanup(
		sqlite_shm_writer_post_native_mapping& rejected_mapping,
		const sqlite_shm_callback_execution_receipt& callback) noexcept
	{
		return state_->begin_writer_cleanup(rejected_mapping, callback);
	}

	sqlite_shm_lease_result<sqlite_shm_writer_attachment_cleanup>
	sqlite_same_process_shm_mapping_lease_coordinator::begin_writer_cleanup(
		sqlite_shm_pending_mapping& pending,
		const sqlite_shm_callback_execution_receipt& callback) noexcept
	{
		return state_->begin_writer_cleanup(pending, callback);
	}

	sqlite_shm_lease_result<void>
	sqlite_same_process_shm_mapping_lease_coordinator::complete_writer_cleanup(
		sqlite_shm_writer_attachment_cleanup& cleanup,
		const sqlite_shm_callback_execution_receipt& callback,
		const sqlite_shm_native_cleanup_outcome outcome) noexcept
	{
		return state_->complete_writer_cleanup(cleanup, callback, outcome);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_map_inflight>
	sqlite_same_process_shm_mapping_lease_coordinator::begin_reader_map(
		const sqlite_shm_reader_map_request& request)
	{
		return state_->begin_reader(request);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_handoff>
	sqlite_same_process_shm_mapping_lease_coordinator::promote_reader(
		sqlite_shm_reader_map_inflight& inflight,
		const sqlite_shm_verified_reader_post_map_receipt& receipt)
	{
		return state_->promote_reader(inflight, receipt);
	}

	sqlite_shm_lease_result<void>
	sqlite_same_process_shm_mapping_lease_coordinator::resolve_reader_map_failure(
		sqlite_shm_reader_map_inflight& inflight) noexcept
	{
		return state_->resolve_reader_failure(inflight);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_cleanup_obligation>
	sqlite_same_process_shm_mapping_lease_coordinator::begin_reader_cleanup(
		sqlite_shm_reader_map_inflight& rejected_mapping,
		const sqlite_shm_callback_execution_receipt& callback) noexcept
	{
		return state_->begin_reader_cleanup(rejected_mapping, callback);
	}

	sqlite_shm_lease_result<void>
	sqlite_same_process_shm_mapping_lease_coordinator::complete_reader_cleanup(
		sqlite_shm_reader_cleanup_obligation& cleanup,
		const sqlite_shm_callback_execution_receipt& callback,
		const sqlite_shm_native_cleanup_outcome outcome) noexcept
	{
		return state_->complete_reader_cleanup(cleanup, callback, outcome);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_unmap_obligation>
	sqlite_same_process_shm_mapping_lease_coordinator::begin_reader_unmap(
		sqlite_shm_reader_handoff& handoff,
		const sqlite_shm_callback_execution_receipt& callback) noexcept
	{
		return state_->begin_reader_unmap(handoff, callback);
	}

	sqlite_shm_lease_result<void>
	sqlite_same_process_shm_mapping_lease_coordinator::complete_reader_unmap(
		sqlite_shm_reader_unmap_obligation& unmap,
		const sqlite_shm_callback_execution_receipt& callback,
		const sqlite_shm_native_cleanup_outcome outcome) noexcept
	{
		return state_->complete_reader_unmap(unmap, callback, outcome);
	}

	sqlite_shm_lease_result<sqlite_shm_writer_release>
	sqlite_same_process_shm_mapping_lease_coordinator::release_writer_holder(
		sqlite_shm_writer_holder& holder,
		const sqlite_shm_callback_execution_receipt& callback) noexcept
	{
		return state_->release_holder(holder, callback);
	}

	sqlite_shm_lease_result<sqlite_shm_writer_retirement_result>
	sqlite_same_process_shm_mapping_lease_coordinator::poll_writer_retirement(
		const sqlite_shm_writer_attachment_cleanup& cleanup,
		const sqlite_shm_callback_execution_receipt& callback) noexcept
	{
		return state_->poll_retirement(cleanup, callback);
	}

	sqlite_shm_lease_result<void>
	sqlite_same_process_shm_mapping_lease_coordinator::fail_writer_retirement_wait(
		const sqlite_shm_writer_attachment_cleanup& cleanup,
		const sqlite_shm_callback_execution_receipt& callback,
		const sqlite_shm_retirement_wait_failure failure) noexcept
	{
		return state_->fail_retirement_wait(cleanup, callback, failure);
	}

	sqlite_shm_mapping_lease_snapshot
	sqlite_same_process_shm_mapping_lease_coordinator::snapshot() const noexcept
	{
		return state_->snapshot();
	}

	void sqlite_same_process_shm_mapping_lease_coordinator::
		inject_writer_native_transition_failure_for_testing() noexcept
	{
		state_->inject_writer_native_transition_failure_for_testing();
	}

	void sqlite_same_process_shm_mapping_lease_coordinator::
		inject_writer_completion_transition_failure_for_testing() noexcept
	{
		state_->inject_writer_completion_transition_failure_for_testing();
	}

	void sqlite_same_process_shm_mapping_lease_coordinator::
		inject_writer_attachment_seal_failure_for_testing() noexcept
	{
		state_->inject_writer_attachment_seal_failure_for_testing();
	}

	void sqlite_same_process_shm_mapping_lease_coordinator::
		inject_registry_writer_incoming_liveness_loss_for_testing() noexcept
	{
		state_->inject_registry_writer_incoming_liveness_loss_for_testing();
	}

	void sqlite_same_process_shm_mapping_lease_coordinator::
		inject_registry_writer_existing_liveness_loss_for_testing() noexcept
	{
		state_->inject_registry_writer_existing_liveness_loss_for_testing();
	}

	void sqlite_same_process_shm_mapping_lease_coordinator::
		inject_registry_writer_pending_liveness_loss_for_testing() noexcept
	{
		state_->inject_registry_writer_pending_liveness_loss_for_testing();
	}
} // namespace cxxlens::sdk
