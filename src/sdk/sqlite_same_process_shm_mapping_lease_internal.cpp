#include "sqlite_same_process_shm_mapping_lease_internal.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
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
				if (!valid_writer_request(request))
					return sqlite_shm_unexpected(
						rejection((request.caller_extend == 0 || request.caller_extend == 1)
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
					const auto attachment = find_attachment_epoch_locked(request.attachment);
					if (attachment != writer_attachments_.end())
					{
						if (attachment->identity != request.attachment)
							return sqlite_shm_unexpected(rejection(
								sqlite_shm_lease_rejection_reason::receipt_mismatch,
								sqlite_shm_lease_recovery_action::deny_before_native_map));
						if (attachment->retired)
							return sqlite_shm_unexpected(stale_token(
								sqlite_shm_lease_recovery_action::deny_before_native_map));
						if (attachment_cleanup_admitted_locked(*attachment))
							return sqlite_shm_unexpected(rejection(
								sqlite_shm_lease_rejection_reason::retiring,
								sqlite_shm_lease_recovery_action::deny_before_native_map));
					}
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
										std::nullopt});
					try
					{
						register_attachment_member_locked(request.attachment, *token);
					}
					catch (...)
					{
						writers_.pop_back();
						throw;
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
				inflight.native_result_observed_ = true;
				try
				{
					std::scoped_lock lock{mutex_};
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
					const auto& map = *writer->receipt;
					if (gate->receipt.family() != family_ ||
						gate->receipt.connection_token() != map.request().connection_token ||
						gate->receipt.open_epoch() != map.open_epoch())
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::
										  remove_pending_then_confirm_native_cleanup));
					if (generation_ &&
						generation_->phase != sqlite_shm_mapping_generation_phase::live)
						return sqlite_shm_unexpected(rejection(
							generation_->handoff_count == 0U
								? sqlite_shm_lease_rejection_reason::retiring
								: sqlite_shm_lease_rejection_reason::successor_handoff_live,
							sqlite_shm_lease_recovery_action::
								remove_pending_then_confirm_native_cleanup));

					const auto holder_token = allocate_token_locked();
					if (!holder_token)
					{
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}

					std::uint64_t generation{};
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
						generation_record installed;
						installed.value = generation;
						installed.phase = sqlite_shm_mapping_generation_phase::live;
						installed.sealed_shm_size = map.mapping().sealed_shm_size;
						installed.pages.push_back(map.mapping());
						installed.authorities.push_back({map, gate->receipt});
						generation_ = std::move(installed);
					}
					else
					{
						generation = generation_->value;
						if (!join_mapping_locked(map))
							return sqlite_shm_unexpected(
								rejection(sqlite_shm_lease_rejection_reason::mapping_mismatch,
										  sqlite_shm_lease_recovery_action::
											  remove_pending_then_confirm_native_cleanup));
						generation_->authorities.push_back({map, gate->receipt});
					}

					holders_.push_back({*holder_token,
										generation,
										holder_phase::active,
										map,
										gate->receipt,
										std::nullopt});
					if (!retoken_attachment_member_locked(pending.token_, *holder_token))
					{
						holders_.back().phase = holder_phase::terminal_quarantined;
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					writers_.erase(writer);
					pending.disarm();
					return sqlite_shm_writer_holder{
						shared_from_this(),
						sqlite_shm_lease_token_identity{*holder_token},
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
				try
				{
					std::scoped_lock lock{mutex_};
					if (!owns(inflight.state_, inflight.token_))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::outer_ioerr_no_retry));
					if (inflight.native_result_observed_)
						return sqlite_shm_unexpected(ambiguous());
					const auto found = find_by_token(writers_, inflight.token_);
					if (found == writers_.end() || found->phase != writer_phase::inflight)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::outer_ioerr_no_retry));
					if (!release_attachment_member_locked(inflight.token_, false))
					{
						found->phase = writer_phase::terminal_quarantined;
						inflight.disarm();
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					writers_.erase(found);
					inflight.disarm();
					return {};
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_writer_cleanup_obligation>
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
					if (live_attachment_member_count_for_token_locked(post_native.token_) > 1U)
					{
						found->phase = writer_phase::terminal_quarantined;
						post_native.disarm();
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					if (!admit_cleanup_callback_locked(found->request.callback, callback))
					{
						found->phase = writer_phase::terminal_quarantined;
						post_native.disarm();
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto token = post_native.token_;
					const auto generation = generation_ ? generation_->value : 0U;
					auto state = post_native.state_;
					// The transient terminal phase consumes the only cleanup-admission attempt.
					// If receipt storage throws, no later call can reissue native cleanup.
					found->phase = writer_phase::terminal_quarantined;
					post_native.disarm();
					found->cleanup_callback = callback;
					found->phase = writer_phase::cleanup_obligation;
					return sqlite_shm_writer_cleanup_obligation{
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

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_writer_cleanup_obligation>
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
					if (live_attachment_member_count_for_token_locked(pending.token_) > 1U)
					{
						found->phase = writer_phase::terminal_quarantined;
						pending.disarm();
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					if (!admit_cleanup_callback_locked(found->request.callback, callback))
					{
						found->phase = writer_phase::terminal_quarantined;
						pending.disarm();
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto token = pending.token_;
					const auto generation = generation_ ? generation_->value : 0U;
					auto state = shared_from_this();
					found->phase = writer_phase::terminal_quarantined;
					pending.disarm();
					found->cleanup_callback = callback;
					found->phase = writer_phase::cleanup_obligation;
					return sqlite_shm_writer_cleanup_obligation{
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
			complete_writer_cleanup(sqlite_shm_writer_cleanup_obligation& cleanup,
									const sqlite_shm_callback_execution_receipt& callback,
									const sqlite_shm_native_cleanup_outcome outcome) noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					if (!owns(cleanup.state_, cleanup.token_))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					const auto writer = find_by_token(writers_, cleanup.token_);
					if (writer != writers_.end() &&
						writer->phase == writer_phase::cleanup_obligation)
					{
						const auto callback_matches =
							writer->cleanup_callback && *writer->cleanup_callback == callback;
						if (!callback_matches ||
							outcome != sqlite_shm_native_cleanup_outcome::confirmed_success ||
							is_quarantined_locked())
						{
							writer->phase = writer_phase::terminal_quarantined;
							cleanup.disarm();
							quarantine_locked();
							return sqlite_shm_unexpected(ambiguous());
						}
						if (!release_attachment_member_locked(cleanup.token_, true))
						{
							writer->phase = writer_phase::terminal_quarantined;
							cleanup.disarm();
							quarantine_locked();
							return sqlite_shm_unexpected(ambiguous());
						}
						writers_.erase(writer);
						cleanup.disarm();
						return {};
					}
					return complete_holder_cleanup_locked(cleanup, callback, outcome);
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
					if (live_attachment_member_count_for_token_locked(holder.token_) > 1U)
					{
						// Full one-unmap attachment cleanup is introduced by the next DF-0206
						// slice. Until then, never expose the old per-map cleanup path for a
						// multi-member attachment.
						found->phase = holder_phase::terminal_quarantined;
						holder.disarm();
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					if (!valid_callback(callback))
					{
						found->phase = holder_phase::terminal_quarantined;
						holder.disarm();
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					if (!callback_can_start_locked(callback))
					{
						found->phase = holder_phase::terminal_quarantined;
						holder.disarm();
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto quarantined_drain = is_quarantined_locked();
					const auto generation = holder.generation_;
					const auto token = holder.token_;
					const auto remaining_holders = active_holder_count_locked() - 1U;
					auto state = shared_from_this();
					found->phase = holder_phase::terminal_quarantined;
					holder.disarm();
					found->release_callback = callback;
					found->phase = remaining_holders == 0U
						? holder_phase::last_waiting
						: holder_phase::nonlast_native_cleanup_admitted;
					auto cleanup = sqlite_shm_writer_cleanup_obligation{
						std::move(state),
						sqlite_shm_lease_token_identity{token},
						sqlite_shm_mapping_generation_identity{generation}};
					if (remaining_holders != 0U)
						return sqlite_shm_writer_release{
							sqlite_shm_writer_retirement_decision::not_last_holder,
							generation,
							std::move(cleanup)};
					if (quarantined_drain)
						quarantine_locked();
					else
						generation_->phase = sqlite_shm_mapping_generation_phase::retiring;
					const auto decision = retirement_decision_locked(callback, token);
					if (decision == sqlite_shm_writer_retirement_decision::ready)
						found->phase = holder_phase::last_native_cleanup_admitted;
					else if (decision ==
							 sqlite_shm_writer_retirement_decision::quarantine_same_thread)
						found->phase = holder_phase::terminal_quarantined;
					return sqlite_shm_writer_release{decision, generation, std::move(cleanup)};
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_writer_retirement_result>
			poll_retirement(const sqlite_shm_writer_cleanup_obligation& cleanup,
							const sqlite_shm_callback_execution_receipt& callback) noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					if (!owns(cleanup.state_, cleanup.token_) || cleanup.generation_ == 0U)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::outer_ioerr_no_retry));
					const auto holder = find_by_token(holders_, cleanup.token_);
					if (holder == holders_.end() || holder->generation != cleanup.generation_ ||
						!holder->release_callback || *holder->release_callback != callback)
					{
						if (holder != holders_.end())
							holder->phase = holder_phase::terminal_quarantined;
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto generation = cleanup.generation_;
					if (holder->phase == holder_phase::terminal_quarantined)
						return sqlite_shm_writer_retirement_result{
							sqlite_shm_writer_retirement_decision::quarantined, generation};
					if (holder->phase != holder_phase::last_waiting)
					{
						holder->phase = holder_phase::terminal_quarantined;
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					if (!generation_ || generation_->value != generation)
					{
						holder->phase = holder_phase::terminal_quarantined;
						quarantine_locked();
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::outer_ioerr_no_retry));
					}
					if (generation_->phase != sqlite_shm_mapping_generation_phase::retiring &&
						generation_->phase != sqlite_shm_mapping_generation_phase::quarantined)
					{
						holder->phase = holder_phase::terminal_quarantined;
						quarantine_locked();
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::stale_generation,
									  sqlite_shm_lease_recovery_action::outer_ioerr_no_retry));
					}
					const auto decision = retirement_decision_locked(callback, cleanup.token_);
					if (decision == sqlite_shm_writer_retirement_decision::ready)
						holder->phase = holder_phase::last_native_cleanup_admitted;
					else if (decision ==
							 sqlite_shm_writer_retirement_decision::quarantine_same_thread)
						holder->phase = holder_phase::terminal_quarantined;
					return sqlite_shm_writer_retirement_result{decision, generation};
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<void>
			fail_retirement_wait(const sqlite_shm_writer_cleanup_obligation& cleanup,
								 const sqlite_shm_callback_execution_receipt& callback,
								 const sqlite_shm_retirement_wait_failure failure) noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					if (!owns(cleanup.state_, cleanup.token_) || cleanup.generation_ == 0U)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					const auto holder = find_by_token(holders_, cleanup.token_);
					if (holder == holders_.end() || holder->generation != cleanup.generation_ ||
						holder->phase != holder_phase::last_waiting || !holder->release_callback ||
						*holder->release_callback != callback ||
						(failure != sqlite_shm_retirement_wait_failure::timeout &&
						 failure != sqlite_shm_retirement_wait_failure::unknown) ||
						!generation_ || generation_->value != cleanup.generation_ ||
						(generation_->phase != sqlite_shm_mapping_generation_phase::retiring &&
						 generation_->phase != sqlite_shm_mapping_generation_phase::quarantined))
					{
						if (holder != holders_.end())
							holder->phase = holder_phase::terminal_quarantined;
						quarantine_locked();
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					}
					holder->phase = holder_phase::terminal_quarantined;
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
						output.generation_authority_count = generation_->authorities.size();
					}
					output.eligibility_count = eligibilities_.size();
					output.writer_inflight_count = static_cast<std::size_t>(std::ranges::count_if(
						writers_,
						[](const writer_record& writer)
						{
							return writer.phase != writer_phase::cleanup_obligation &&
								writer.phase != writer_phase::terminal_quarantined;
						}));
					output.writer_cleanup_count = static_cast<std::size_t>(
						std::ranges::count_if(writers_,
											  [](const writer_record& writer)
											  {
												  return writer.phase ==
													  writer_phase::cleanup_obligation ||
													  writer.phase ==
													  writer_phase::terminal_quarantined;
											  }) +
						std::ranges::count_if(holders_,
											  [](const holder_record& holder)
											  {
												  return holder.phase != holder_phase::active;
											  }));
					output.writer_holder_count = active_holder_count_locked();
					output.writer_attachment_identity_count = writer_attachments_.size();
					for (const auto& attachment : writer_attachments_)
					{
						output.writer_attachment_member_count += attachment.members.size();
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
							case lease_token_kind::writer_cleanup:
							{
								const auto writer = find_by_token(writers_, token);
								if (writer != writers_.end())
									writer->phase = writer_phase::terminal_quarantined;
								else
								{
									const auto holder = find_by_token(holders_, token);
									if (holder != holders_.end())
										holder->phase = holder_phase::terminal_quarantined;
								}
								break;
							}
							case lease_token_kind::holder:
							{
								const auto holder = find_by_token(holders_, token);
								if (holder != holders_.end())
									holder->phase = holder_phase::terminal_quarantined;
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
				cleanup_obligation,
				terminal_quarantined,
			};

			struct writer_native_transition_injected_failure
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
				nonlast_native_cleanup_admitted,
				last_waiting,
				last_native_cleanup_admitted,
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
				std::optional<sqlite_shm_callback_execution_receipt> cleanup_callback;
			};

			struct holder_record
			{
				std::uint64_t token{};
				std::uint64_t generation{};
				holder_phase phase{holder_phase::active};
				sqlite_shm_verified_writer_post_map_receipt map_receipt;
				sqlite_shm_verified_writer_eligibility_receipt eligibility_receipt;
				std::optional<sqlite_shm_callback_execution_receipt> release_callback;
			};

			struct writer_attachment_member_record
			{
				std::uint64_t original_token{};
				std::uint64_t live_token{};
				bool confirmed_native_cleanup{};
			};

			struct writer_attachment_record
			{
				sqlite_shm_native_attachment_identity identity;
				std::vector<writer_attachment_member_record> members;
				bool retired{};
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
											  const std::uint64_t token)
			{
				auto attachment = find_attachment_epoch_locked(identity);
				if (attachment == writer_attachments_.end())
				{
					writer_attachment_record record{identity, {}, false};
					record.members.push_back({token, token, false});
					writer_attachments_.push_back(std::move(record));
					return;
				}
				attachment->members.push_back({token, token, false});
			}

			[[nodiscard]] bool
			retoken_attachment_member_locked(const std::uint64_t old_token,
											 const std::uint64_t new_token) noexcept
			{
				for (auto& attachment : writer_attachments_)
				{
					const auto member =
						std::find_if(attachment.members.begin(),
									 attachment.members.end(),
									 [old_token](const writer_attachment_member_record& value)
									 {
										 return value.live_token == old_token;
									 });
					if (member != attachment.members.end())
					{
						member->live_token = new_token;
						return true;
					}
				}
				return false;
			}

			[[nodiscard]] std::size_t
			live_attachment_member_count_for_token_locked(const std::uint64_t token) const noexcept
			{
				for (const auto& attachment : writer_attachments_)
				{
					if (std::ranges::none_of(attachment.members,
											 [token](const writer_attachment_member_record& value)
											 {
												 return value.live_token == token;
											 }))
						continue;
					return static_cast<std::size_t>(
						std::ranges::count_if(attachment.members,
											  [](const writer_attachment_member_record& value)
											  {
												  return value.live_token != 0U;
											  }));
				}
				return 0U;
			}

			[[nodiscard]] bool attachment_cleanup_admitted_locked(
				const writer_attachment_record& attachment) const noexcept
			{
				for (const auto& member : attachment.members)
				{
					if (member.live_token == 0U)
						continue;
					const auto writer = find_by_token(writers_, member.live_token);
					if (writer != writers_.end() &&
						writer->phase == writer_phase::cleanup_obligation)
						return true;
					const auto holder = find_by_token(holders_, member.live_token);
					if (holder != holders_.end() &&
						(holder->phase == holder_phase::nonlast_native_cleanup_admitted ||
						 holder->phase == holder_phase::last_waiting ||
						 holder->phase == holder_phase::last_native_cleanup_admitted))
						return true;
				}
				return false;
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
						attachment.retired = true;
					return true;
				}
				return false;
			}

			[[nodiscard]] std::optional<std::uint64_t> allocate_token_locked() noexcept
			{
				if (token_exhausted_)
					return std::nullopt;
				const auto output = next_token_;
				if (next_token_ == std::numeric_limits<std::uint64_t>::max())
					token_exhausted_ = true;
				else
					++next_token_;
				return output;
			}

			[[nodiscard]] bool callback_can_start_locked(
				const sqlite_shm_callback_execution_receipt& callback) const noexcept
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
						[&ordered_after](const writer_record& writer)
						{
							return (writer.phase == writer_phase::inflight ||
									writer.phase == writer_phase::post_native_mapping ||
									writer.phase == writer_phase::cleanup_obligation) &&
								!ordered_after(writer.cleanup_callback ? *writer.cleanup_callback
																	   : writer.request.callback);
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
				if (std::ranges::any_of(holders_,
										[&ordered_after](const holder_record& holder)
										{
											return holder.phase != holder_phase::active &&
												holder.phase !=
												holder_phase::terminal_quarantined &&
												holder.release_callback &&
												!ordered_after(*holder.release_callback);
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

			[[nodiscard]] std::optional<sqlite_shm_lease_rejection>
			blocked_locked(const sqlite_shm_lease_recovery_action action) const noexcept
			{
				if (is_quarantined_locked())
					return rejection(sqlite_shm_lease_rejection_reason::quarantined, action);
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

			[[nodiscard]] bool
			join_mapping_locked(const sqlite_shm_verified_writer_post_map_receipt& receipt)
			{
				auto page = find_page_locked(receipt.mapping().page_number);
				if (page != generation_->pages.end())
					return same_mapping_page(*page, receipt.mapping()) &&
						receipt.mapping().sealed_shm_size == generation_->sealed_shm_size;
				if (receipt.extend_pair() != sqlite_shm_writer_extend_pair::one_one ||
					generation_->pages.empty() ||
					receipt.mapping().page_size != generation_->pages.front().page_size ||
					receipt.mapping().sealed_shm_size < generation_->sealed_shm_size)
					return false;
				generation_->pages.push_back(receipt.mapping());
				std::ranges::sort(generation_->pages,
								  {},
								  [](const sqlite_shm_mapping_tuple& value)
								  {
									  return value.page_number;
								  });
				generation_->sealed_shm_size = receipt.mapping().sealed_shm_size;
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

			[[nodiscard]] sqlite_shm_writer_retirement_decision
			retirement_decision_locked(const sqlite_shm_callback_execution_receipt& callback,
									   const std::uint64_t last_holder_token) noexcept
			{
				bool has_blocker = false;
				bool has_same_thread_blocker = false;
				const auto observe = [&callback, &has_blocker, &has_same_thread_blocker](
										 const sqlite_shm_callback_execution_receipt& active)
				{
					has_blocker = true;
					if (active.thread_identity == callback.thread_identity)
					{
						// A same-thread blocker cannot make progress while this callback waits.
						// Depth and invocation are retained here so a reordered or duplicate
						// callback cannot be mistaken for a cross-thread wait.
						const auto ordered_nested =
							callback.reentrancy_depth > active.reentrancy_depth &&
							callback.invocation_token != active.invocation_token;
						(void)ordered_nested;
						has_same_thread_blocker = true;
					}
				};

				for (const auto& writer : writers_)
				{
					if (writer.phase == writer_phase::terminal_quarantined)
						continue;
					observe(writer.cleanup_callback ? *writer.cleanup_callback
													: writer.request.callback);
				}
				for (const auto& reader : readers_)
				{
					if (reader.phase == reader_phase::terminal_quarantined)
						continue;
					observe(reader.cleanup_callback ? *reader.cleanup_callback
													: reader.request.callback);
				}
				for (const auto& holder : holders_)
				{
					if (holder.token == last_holder_token || holder.phase == holder_phase::active ||
						holder.phase == holder_phase::terminal_quarantined ||
						!holder.release_callback)
						continue;
					observe(*holder.release_callback);
				}

				if (has_same_thread_blocker)
				{
					quarantine_locked();
					return sqlite_shm_writer_retirement_decision::quarantine_same_thread;
				}
				if (has_blocker)
					return sqlite_shm_writer_retirement_decision::wait_for_inflight;
				return sqlite_shm_writer_retirement_decision::ready;
			}

			[[nodiscard]] sqlite_shm_lease_result<void>
			complete_holder_cleanup_locked(sqlite_shm_writer_cleanup_obligation& cleanup,
										   const sqlite_shm_callback_execution_receipt& callback,
										   const sqlite_shm_native_cleanup_outcome outcome) noexcept
			{
				const auto holder = find_by_token(holders_, cleanup.token_);
				if (holder == holders_.end() || holder->generation != cleanup.generation_ ||
					holder->phase == holder_phase::active)
					return sqlite_shm_unexpected(
						stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));

				const auto phase = holder->phase;
				const auto generation = holder->generation;
				const auto callback_matches =
					holder->release_callback && *holder->release_callback == callback;
				if (phase == holder_phase::last_waiting)
				{
					// Completion proves native cleanup was delegated before admission.
					holder->phase = holder_phase::terminal_quarantined;
					cleanup.disarm();
					quarantine_locked();
					return sqlite_shm_unexpected(ambiguous());
				}

				if (!callback_matches ||
					outcome != sqlite_shm_native_cleanup_outcome::confirmed_success ||
					is_quarantined_locked())
				{
					holder->phase = holder_phase::terminal_quarantined;
					cleanup.disarm();
					quarantine_locked();
					return sqlite_shm_unexpected(ambiguous());
				}

				if (phase == holder_phase::nonlast_native_cleanup_admitted)
				{
					if (!release_attachment_member_locked(cleanup.token_, true))
					{
						holder->phase = holder_phase::terminal_quarantined;
						cleanup.disarm();
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					holders_.erase(holder);
					cleanup.disarm();
					return {};
				}

				if (phase != holder_phase::last_native_cleanup_admitted || !generation_ ||
					generation_->value != generation ||
					generation_->phase != sqlite_shm_mapping_generation_phase::retiring ||
					!writers_.empty() || !readers_.empty() ||
					std::ranges::any_of(holders_,
										[&cleanup](const holder_record& value)
										{
											return value.token != cleanup.token_;
										}))
				{
					holder->phase = holder_phase::terminal_quarantined;
					cleanup.disarm();
					quarantine_locked();
					return sqlite_shm_unexpected(ambiguous());
				}

				if (!release_attachment_member_locked(cleanup.token_, true))
				{
					holder->phase = holder_phase::terminal_quarantined;
					cleanup.disarm();
					quarantine_locked();
					return sqlite_shm_unexpected(ambiguous());
				}
				holders_.erase(holder);
				cleanup.disarm();
				generation_->phase = sqlite_shm_mapping_generation_phase::retired;
				if (generation_->handoff_count == 0U)
					generation_.reset();
				return {};
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
						const auto writer = find_by_token(writers_, token);
						if (writer != writers_.end())
							return writer->phase == writer_phase::cleanup_obligation;
						const auto holder = find_by_token(holders_, token);
						return holder != holders_.end() &&
							(holder->phase == holder_phase::nonlast_native_cleanup_admitted ||
							 holder->phase == holder_phase::last_waiting ||
							 holder->phase == holder_phase::last_native_cleanup_admitted);
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
			std::vector<writer_record> writers_;
			std::vector<holder_record> holders_;
			std::vector<writer_attachment_record> writer_attachments_;
			std::vector<reader_record> readers_;
			std::vector<handoff_record> handoffs_;
			std::optional<generation_record> generation_;
			std::uint64_t next_token_{1U};
			bool token_exhausted_{};
			bool alive_{true};
			bool quarantined_{};
			bool fail_next_writer_native_transition_for_testing_{};
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
		  native_result_observed_{std::exchange(other.native_result_observed_, false)}
	{
	}

	bool sqlite_shm_writer_map_inflight::valid() const noexcept
	{
		return state_ != nullptr && token_ != 0U;
	}

	void sqlite_shm_writer_map_inflight::disarm() noexcept
	{
		token_ = 0U;
		native_result_observed_ = false;
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

	sqlite_shm_writer_cleanup_obligation::sqlite_shm_writer_cleanup_obligation(
		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
		const detail::sqlite_shm_lease_token_identity token,
		const detail::sqlite_shm_mapping_generation_identity generation) noexcept
		: state_{std::move(state)}, token_{token.value}, generation_{generation.value}
	{
	}

	sqlite_shm_writer_cleanup_obligation::~sqlite_shm_writer_cleanup_obligation() noexcept
	{
		if (state_)
			state_->abandon(lease_token_kind::writer_cleanup, token_);
	}

	sqlite_shm_writer_cleanup_obligation::sqlite_shm_writer_cleanup_obligation(
		sqlite_shm_writer_cleanup_obligation&& other) noexcept
		: state_{std::move(other.state_)}, token_{std::exchange(other.token_, 0U)},
		  generation_{std::exchange(other.generation_, 0U)}
	{
	}

	bool sqlite_shm_writer_cleanup_obligation::valid() const noexcept
	{
		return state_ != nullptr && token_ != 0U;
	}

	std::uint64_t sqlite_shm_writer_cleanup_obligation::generation() const noexcept
	{
		return generation_;
	}

	void sqlite_shm_writer_cleanup_obligation::disarm() noexcept
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
		sqlite_shm_writer_cleanup_obligation cleanup) noexcept
		: decision_{decision}, generation_{generation}, cleanup_{std::move(cleanup)}
	{
	}

	sqlite_shm_writer_release::~sqlite_shm_writer_release() noexcept = default;

	sqlite_shm_writer_release::sqlite_shm_writer_release(sqlite_shm_writer_release&& other) noexcept
		: decision_{std::exchange(other.decision_,
								  sqlite_shm_writer_retirement_decision::quarantined)},
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

	sqlite_shm_writer_cleanup_obligation& sqlite_shm_writer_release::cleanup() noexcept
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

	sqlite_shm_lease_result<sqlite_shm_writer_cleanup_obligation>
	sqlite_same_process_shm_mapping_lease_coordinator::begin_writer_cleanup(
		sqlite_shm_writer_post_native_mapping& rejected_mapping,
		const sqlite_shm_callback_execution_receipt& callback) noexcept
	{
		return state_->begin_writer_cleanup(rejected_mapping, callback);
	}

	sqlite_shm_lease_result<sqlite_shm_writer_cleanup_obligation>
	sqlite_same_process_shm_mapping_lease_coordinator::begin_writer_cleanup(
		sqlite_shm_pending_mapping& pending,
		const sqlite_shm_callback_execution_receipt& callback) noexcept
	{
		return state_->begin_writer_cleanup(pending, callback);
	}

	sqlite_shm_lease_result<void>
	sqlite_same_process_shm_mapping_lease_coordinator::complete_writer_cleanup(
		sqlite_shm_writer_cleanup_obligation& cleanup,
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
		const sqlite_shm_writer_cleanup_obligation& cleanup,
		const sqlite_shm_callback_execution_receipt& callback) noexcept
	{
		return state_->poll_retirement(cleanup, callback);
	}

	sqlite_shm_lease_result<void>
	sqlite_same_process_shm_mapping_lease_coordinator::fail_writer_retirement_wait(
		const sqlite_shm_writer_cleanup_obligation& cleanup,
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
} // namespace cxxlens::sdk
