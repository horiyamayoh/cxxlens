#include "sqlite_same_process_shm_mapping_registry_internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <utility>
#include <vector>

#include "sqlite_same_process_shm_identity_issuer_internal.hpp"
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

		[[nodiscard]] bool
		valid_callback(const sqlite_shm_callback_execution_receipt& callback) noexcept
		{
			return valid_identity(callback.thread_identity) &&
				valid_identity(callback.invocation_token);
		}

		[[nodiscard]] bool valid_reader_map_identity_pre_request(
			const sqlite_shm_reader_attachment_map_pre_request& request) noexcept
		{
			return valid_family(request.family) && valid_identity(request.alias_lifetime) &&
				valid_identity(request.connection_token) && request.page_number >= 0 &&
				request.page_size > 0 && request.caller_extend >= 0 &&
				request.expected_attachment.family() == request.family &&
				request.expected_attachment.alias_lifetime() == request.alias_lifetime &&
				request.expected_attachment.connection_token() == request.connection_token &&
				request.expected_attachment.registry_open_token() != 0U;
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
				snapshot.reader_handoff_count == 0U &&
				snapshot.reader_attachment_group_count == 0U &&
				snapshot.reader_attachment_live_member_count == 0U &&
				snapshot.reader_session_reservation_count == 0U &&
				snapshot.reader_session_owner_count == 0U &&
				snapshot.reader_unpublished_cleanup_admitted_count == 0U &&
				snapshot.reader_logical_ack_awaiting_count == 0U &&
				snapshot.reader_registry_bound_group_count == 0U &&
				snapshot.reader_registry_bound_session_count == 0U &&
				snapshot.reader_registry_open_count == 0U &&
				snapshot.reader_open_close_owner_count == 0U &&
				snapshot.reader_close_admitted_count == 0U &&
				snapshot.reader_close_terminal_count == 0U &&
				snapshot.reader_registry_activity_authority_count == 0U &&
				!snapshot.reader_admission_visible;
		}

		[[nodiscard]] bool valid_reader_pre_sqlite_request(
			const sqlite_shm_reader_pre_sqlite_session_request& request) noexcept
		{
			return valid_family(request.family) && valid_identity(request.alias_lifetime) &&
				valid_identity(request.connection_token) &&
				valid_identity(request.main_native_file_receipt) &&
				valid_identity(request.main_xopen_receipt) && valid_identity(request.open_epoch) &&
				valid_identity(request.callback_cohort) && valid_callback(request.execution) &&
				valid_identity(request.read_transaction_epoch) &&
				valid_identity(request.decode_attempt) &&
				valid_identity(request.authority_read_receipt);
		}

		[[nodiscard]] bool
		valid_reader_open_binding(const sqlite_shm_reader_open_binding& binding) noexcept
		{
			return valid_family(binding.family) && valid_identity(binding.alias_lifetime) &&
				valid_identity(binding.connection_token) &&
				valid_identity(binding.main_native_file_receipt) &&
				valid_identity(binding.main_xopen_receipt) && valid_identity(binding.open_epoch) &&
				valid_identity(binding.callback_cohort);
		}

		[[nodiscard]] bool reader_open_binding_matches_request(
			const sqlite_shm_reader_open_binding& binding,
			const sqlite_shm_reader_pre_sqlite_session_request& request) noexcept
		{
			return binding.family == request.family &&
				binding.alias_lifetime == request.alias_lifetime &&
				binding.connection_token == request.connection_token &&
				binding.main_native_file_receipt == request.main_native_file_receipt &&
				binding.main_xopen_receipt == request.main_xopen_receipt &&
				binding.open_epoch == request.open_epoch &&
				binding.callback_cohort == request.callback_cohort;
		}

		[[nodiscard]] sqlite_shm_reader_open_epoch_binding
		lease_reader_open_epoch_binding(const sqlite_shm_reader_open_binding& binding,
										const sqlite_backend_opaque_identity& runtime_lifetime_pin)
		{
			return {
				binding.family,
				runtime_lifetime_pin,
				binding.alias_lifetime,
				binding.connection_token,
				binding.main_native_file_receipt,
				binding.main_xopen_receipt,
				binding.open_epoch,
				binding.callback_cohort,
			};
		}

		[[nodiscard]] bool valid_lease_reader_open_epoch_binding(
			const sqlite_shm_reader_open_epoch_binding& binding) noexcept
		{
			return valid_family(binding.family) && valid_identity(binding.runtime_lifetime_pin) &&
				valid_identity(binding.alias_lifetime) &&
				valid_identity(binding.connection_token) &&
				valid_identity(binding.main_native_file_receipt) &&
				valid_identity(binding.main_xopen_receipt) && valid_identity(binding.open_epoch) &&
				valid_identity(binding.callback_cohort);
		}

		[[nodiscard]] bool reader_attachment_matches_open_epoch_binding(
			const sqlite_shm_reader_attachment_reservation_identity& attachment,
			const sqlite_shm_reader_open_epoch_binding& binding) noexcept
		{
			return attachment.family() == binding.family &&
				attachment.runtime_lifetime_pin() == binding.runtime_lifetime_pin &&
				attachment.alias_lifetime() == binding.alias_lifetime &&
				attachment.connection_token() == binding.connection_token &&
				attachment.main_native_file_receipt() == binding.main_native_file_receipt &&
				attachment.main_xopen_receipt() == binding.main_xopen_receipt &&
				attachment.open_epoch() == binding.open_epoch &&
				attachment.callback_cohort() == binding.callback_cohort;
		}

		[[nodiscard]] sqlite_backend_opaque_identity
		reader_attachment_epoch_identity(const std::uint64_t value)
		{
			std::array<std::byte, sizeof(value)> bytes{};
			for (std::size_t index = 0; index < bytes.size(); ++index)
				bytes[index] =
					static_cast<std::byte>((value >> static_cast<unsigned>(index * 8U)) & 0xffU);
			return {
				"cxxlens.sqlite.reader-attachment-epoch.registry-v1",
				{bytes.begin(), bytes.end()},
			};
		}

		[[nodiscard]] bool valid_reader_replay_identity_vector(
			const std::vector<sqlite_backend_opaque_identity>& identities) noexcept
		{
			for (auto left = identities.begin(); left != identities.end(); ++left)
			{
				if (!valid_identity(*left) ||
					std::find(std::next(left), identities.end(), *left) != identities.end())
					return false;
			}
			return true;
		}

		[[nodiscard]] bool reader_replay_identity_vectors_overlap(
			const std::vector<sqlite_backend_opaque_identity>& left,
			const std::vector<sqlite_backend_opaque_identity>& right) noexcept
		{
			return std::ranges::any_of(left,
									   [&right](const sqlite_backend_opaque_identity& identity)
									   {
										   return std::ranges::find(right, identity) != right.end();
									   });
		}

		[[nodiscard]] bool valid_reader_replay_identity_domains(
			const sqlite_shm_reader_replay_identity_tombstone& replay) noexcept
		{
			return valid_reader_replay_identity_vector(replay.callback_invocation_tokens) &&
				valid_reader_replay_identity_vector(replay.effect_receipts) &&
				valid_reader_replay_identity_vector(replay.session_terminal_receipts) &&
				!reader_replay_identity_vectors_overlap(replay.callback_invocation_tokens,
														replay.effect_receipts) &&
				!reader_replay_identity_vectors_overlap(replay.callback_invocation_tokens,
														replay.session_terminal_receipts) &&
				!reader_replay_identity_vectors_overlap(replay.effect_receipts,
														replay.session_terminal_receipts);
		}

		[[nodiscard]] bool valid_reader_lifecycle_replay_tombstone(
			const sqlite_shm_reader_lifecycle_compact_tombstone& tombstone) noexcept
		{
			const auto& replay = tombstone.replay_identities;
			if (!valid_reader_replay_identity_domains(replay))
				return false;
			using phase = detail::sqlite_shm_reader_attachment_reservation_phase;
			using ack_phase = detail::sqlite_shm_reader_logical_ack_phase;
			const auto has_no_unpublished_cleanup_sequence =
				tombstone.unpublished_cleanup_session_terminal_sequence == 0U &&
				tombstone.unpublished_cleanup_cut_sequence == 0U &&
				tombstone.unpublished_cleanup_terminal_sequence == 0U;
			if (tombstone.phase == phase::revoked_no_map)
				return tombstone.logical_ack_phase == ack_phase::not_applicable &&
					tombstone.logical_ack_sequence == 0U && has_no_unpublished_cleanup_sequence &&
					replay.session_terminal_receipts.empty() &&
					(replay.callback_free_terminal
						 ? replay.callback_invocation_tokens.empty() &&
							 replay.effect_receipts.empty()
						 : replay.callback_invocation_tokens.size() == 1U &&
							 replay.effect_receipts.size() == 1U);
			if (tombstone.phase == phase::retired_confirmed)
				return tombstone.logical_ack_phase == ack_phase::not_applicable &&
					tombstone.logical_ack_sequence == 0U && has_no_unpublished_cleanup_sequence &&
					replay.session_terminal_receipts.empty() && !replay.callback_free_terminal &&
					replay.callback_invocation_tokens.size() >= 2U &&
					replay.effect_receipts.size() == replay.callback_invocation_tokens.size() + 1U;
			if (tombstone.phase == phase::predecessor_route_retired_confirmed)
				return tombstone.logical_ack_phase == ack_phase::not_applicable &&
					tombstone.logical_ack_sequence == 0U && has_no_unpublished_cleanup_sequence &&
					replay.session_terminal_receipts.empty() && !replay.callback_free_terminal &&
					(replay.callback_invocation_tokens.size() == 1U ||
					 replay.callback_invocation_tokens.size() == 2U) &&
					replay.effect_receipts.size() == replay.callback_invocation_tokens.size();
			if (tombstone.phase == phase::unpublished_cleanup_confirmed)
			{
				const auto exact_unmap_ack =
					tombstone.logical_ack_phase == ack_phase::consumed_by_exact_unmap;
				const auto close_ack = tombstone.logical_ack_phase == ack_phase::consumed_by_close;
				return (exact_unmap_ack || close_ack) && !replay.callback_free_terminal &&
					replay.callback_invocation_tokens.size() == (exact_unmap_ack ? 2U : 1U) &&
					replay.effect_receipts.size() == 3U &&
					replay.session_terminal_receipts.size() == 1U &&
					tombstone.origin_sequence <
					tombstone.unpublished_cleanup_session_terminal_sequence &&
					tombstone.unpublished_cleanup_session_terminal_sequence -
						tombstone.origin_sequence >
					2U &&
					tombstone.unpublished_cleanup_session_terminal_sequence <
					tombstone.unpublished_cleanup_cut_sequence &&
					tombstone.unpublished_cleanup_cut_sequence -
						tombstone.unpublished_cleanup_session_terminal_sequence ==
					1U &&
					tombstone.unpublished_cleanup_cut_sequence <
					tombstone.unpublished_cleanup_terminal_sequence &&
					tombstone.unpublished_cleanup_terminal_sequence ==
					tombstone.destination_sequence &&
					tombstone.logical_ack_sequence >
					tombstone.unpublished_cleanup_terminal_sequence;
			}
			return false;
		}

		[[nodiscard]] bool valid_reader_close_replay_tombstone(
			const sqlite_shm_reader_open_epoch_close_tombstone& tombstone) noexcept
		{
			const auto& replay = tombstone.replay_identities;
			return !replay.callback_free_terminal &&
				replay.callback_invocation_tokens.size() == 1U &&
				replay.effect_receipts.size() == 1U && replay.session_terminal_receipts.empty() &&
				valid_reader_replay_identity_domains(replay);
		}

		[[nodiscard]] bool reader_replay_tombstones_overlap(
			const sqlite_shm_reader_replay_identity_tombstone& left,
			const sqlite_shm_reader_replay_identity_tombstone& right) noexcept
		{
			const std::array left_domains{
				&left.callback_invocation_tokens,
				&left.effect_receipts,
				&left.session_terminal_receipts,
			};
			const std::array right_domains{
				&right.callback_invocation_tokens,
				&right.effect_receipts,
				&right.session_terminal_receipts,
			};
			return std::ranges::any_of(left_domains,
									   [&right_domains](const auto* left_domain)
									   {
										   return std::ranges::any_of(
											   right_domains,
											   [left_domain](const auto* right_domain)
											   {
												   return reader_replay_identity_vectors_overlap(
													   *left_domain, *right_domain);
											   });
									   });
		}

		std::atomic<std::uint64_t> registry_state_destruction_count{0U};

		void append_framed_integer(std::vector<std::byte>& output, std::uint64_t value)
		{
			for (std::size_t index = 0U; index < sizeof(value); ++index)
			{
				output.push_back(static_cast<std::byte>(value & 0xffU));
				value >>= 8U;
			}
		}

		void append_framed_identity(
			std::vector<std::byte>& output, const sqlite_backend_opaque_identity& identity)
		{
			append_framed_integer(output, static_cast<std::uint64_t>(identity.profile.size()));
			for (const auto value : identity.profile)
				output.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
			append_framed_integer(output, static_cast<std::uint64_t>(identity.bytes.size()));
			output.insert(output.end(), identity.bytes.begin(), identity.bytes.end());
		}

		[[nodiscard]] sqlite_backend_opaque_identity make_reader_map_identity_request_seal(
			const sqlite_shm_reader_attachment_map_pre_request& request)
		{
			std::vector<std::byte> bytes;
			bytes.reserve(512U);
			append_framed_identity(bytes, request.family.process_instance);
			append_framed_identity(bytes, request.family.shared_runtime_vfs_cohort);
			append_framed_identity(bytes, request.family.exact_file_family);
			append_framed_identity(bytes, request.alias_lifetime);
			append_framed_identity(bytes, request.connection_token);
			const auto& attachment = request.expected_attachment;
			append_framed_identity(bytes, attachment.runtime_lifetime_pin());
			append_framed_identity(bytes, attachment.alias_lifetime());
			append_framed_identity(bytes, attachment.connection_token());
			append_framed_identity(bytes, attachment.main_native_file_receipt());
			append_framed_identity(bytes, attachment.main_xopen_receipt());
			append_framed_identity(bytes, attachment.open_epoch());
			append_framed_identity(bytes, attachment.callback_cohort());
			append_framed_identity(bytes, attachment.attachment_epoch());
			append_framed_integer(bytes, attachment.registry_open_token());
			append_framed_integer(bytes, attachment.writer_mapping_generation());
			append_framed_integer(bytes, static_cast<std::uint64_t>(
				static_cast<std::uint32_t>(request.page_number)));
			append_framed_integer(bytes, static_cast<std::uint64_t>(
				static_cast<std::uint32_t>(request.page_size)));
			append_framed_integer(bytes, static_cast<std::uint64_t>(
				static_cast<std::uint32_t>(request.caller_extend)));
			return {"cxxlens.sqlite.reader-map-request-seal.v1", std::move(bytes)};
		}
	} // namespace

	namespace detail
	{
		[[nodiscard]] std::shared_ptr<sqlite_shm_process_identity_issuer_state>
		make_identity_issuer_state_for_registry(
			std::weak_ptr<void> registry_state,
			std::shared_ptr<std::atomic<std::uint64_t>> process_epoch,
			std::shared_ptr<std::atomic_bool> registry_quarantine_latch,
			std::shared_ptr<std::atomic_bool> registry_issuer_owner_latch,
			std::uint64_t expected_process_epoch,
			const sqlite_backend_opaque_identity& process_instance,
			std::uint64_t first_sequence);
		[[nodiscard]] sqlite_shm_reader_lifecycle_identity_scope
		seal_identity_scope_for_registry(
			const std::shared_ptr<sqlite_shm_process_identity_issuer_state>& state,
			const sqlite_shm_lease_family_binding& family,
			std::shared_ptr<std::atomic_bool> family_authority_latch,
			std::uint64_t family_epoch,
			std::uint64_t family_pin_token,
			const sqlite_backend_opaque_identity& callback_cohort,
			const sqlite_backend_opaque_identity& request_seal,
			const sqlite_shm_reader_lifecycle_owner_coordinates& coordinates);
		[[nodiscard]] sqlite_shm_reader_lifecycle_identity_scope
		seal_qualified_identity_scope_for_registry(
			const std::shared_ptr<sqlite_shm_process_identity_issuer_state>& state,
			const sqlite_shm_lease_family_binding& family,
			std::shared_ptr<std::atomic_bool> family_authority_latch,
			std::uint64_t family_epoch,
			std::uint64_t family_pin_token,
			const sqlite_backend_opaque_identity& callback_cohort,
			const sqlite_backend_opaque_identity& request_seal,
			const sqlite_shm_reader_lifecycle_owner_coordinates& coordinates,
			std::shared_ptr<std::atomic<sqlite_shm_reader_lifecycle_owner_phase>> owner_phase,
			std::weak_ptr<sqlite_shm_reader_lifecycle_owner_abandonment_control>
				owner_abandonment);
		[[nodiscard]] bool qualified_identity_scope_matches_for_registry(
			const std::shared_ptr<sqlite_shm_process_identity_issuer_state>& state,
			const sqlite_shm_reader_lifecycle_identity_scope& scope,
			const sqlite_shm_lease_family_binding& family,
			std::uint64_t family_epoch,
			std::uint64_t family_pin_token,
			const sqlite_backend_opaque_identity& callback_cohort,
			const sqlite_backend_opaque_identity& request_seal,
			const sqlite_shm_reader_lifecycle_owner_coordinates& coordinates,
			const std::shared_ptr<std::atomic<sqlite_shm_reader_lifecycle_owner_phase>>& owner_phase,
			const std::weak_ptr<sqlite_shm_reader_lifecycle_owner_abandonment_control>&
				owner_abandonment) noexcept;
		[[nodiscard]] sqlite_shm_lease_result<void> validate_callback_identity_for_registry(
			const std::shared_ptr<sqlite_shm_process_identity_issuer_state>& state,
			const sqlite_shm_reader_lifecycle_identity_scope& scope,
			const sqlite_shm_issued_reader_callback_identity& callback,
			sqlite_shm_reader_callback_identity_role role) noexcept;
		[[nodiscard]]
		sqlite_shm_lease_result<sqlite_shm_reader_zero_effect_identity_validation_capability>
		validate_zero_effect_identity_for_registry(
			const std::shared_ptr<sqlite_shm_process_identity_issuer_state>& state,
			const sqlite_shm_reader_lifecycle_identity_scope& scope,
			const sqlite_shm_issued_reader_callback_identity& callback,
			const sqlite_shm_issued_reader_effect_identity& effect,
			const std::weak_ptr<sqlite_shm_reader_lifecycle_owner_abandonment_control>&
				expected_owner) noexcept;
		[[nodiscard]] std::shared_ptr<sqlite_shm_reader_identity_completion_control>
		make_identity_completion_for_registry(
			const std::shared_ptr<sqlite_shm_process_identity_issuer_state>& state,
			const sqlite_shm_reader_lifecycle_identity_scope& scope,
			const sqlite_shm_issued_reader_callback_identity& callback);
		void exhaust_identity_issuer_for_registry(
			const std::shared_ptr<sqlite_shm_process_identity_issuer_state>& state) noexcept;

		struct sqlite_shm_registry_process_owner_seal
		{
			const std::shared_ptr<std::atomic<std::uint64_t>> process_epoch{
				std::make_shared<std::atomic<std::uint64_t>>(1U)};
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
					owner->process_epoch->load(std::memory_order_acquire) == process_epoch;
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

		enum class sqlite_shm_reader_open_phase : std::uint8_t
		{
			active,
			clean_released,
			abandoned,
		};

		struct sqlite_shm_reader_open_control
		{
			struct coordinates
			{
				std::uint64_t process_epoch{};
				std::uint64_t alias_token{};
				std::uint64_t family_epoch{};
				std::uint64_t family_pin_token{};
				std::uint64_t open_token{};
			};

			sqlite_shm_reader_open_control(
				std::weak_ptr<sqlite_shm_mapping_registry_state> registry_state_value,
				std::weak_ptr<sqlite_shm_registry_process_owner_seal> process_seal_value,
				std::shared_ptr<std::atomic_bool> emergency_latch_value,
				std::shared_ptr<std::atomic_bool> alias_authority_latch_value,
				std::shared_ptr<std::atomic_bool> family_authority_latch_value,
				std::shared_ptr<sqlite_shm_reader_open_lineage_seal> lineage_seal_value,
				sqlite_shm_reader_open_binding binding_value,
				const coordinates value)
				: registry_state{std::move(registry_state_value)},
				  process_seal{std::move(process_seal_value)},
				  emergency_latch{std::move(emergency_latch_value)},
				  alias_authority_latch{std::move(alias_authority_latch_value)},
				  family_authority_latch{std::move(family_authority_latch_value)},
				  lineage_seal{std::move(lineage_seal_value)}, binding{std::move(binding_value)},
				  process_epoch{value.process_epoch}, alias_token{value.alias_token},
				  family_epoch{value.family_epoch}, family_pin_token{value.family_pin_token},
				  open_token{value.open_token}
			{
			}

			[[nodiscard]] bool authority_valid_now() const noexcept
			{
				const auto state = registry_state.lock();
				const auto owner = process_seal.lock();
				return phase.load(std::memory_order_acquire) ==
					sqlite_shm_reader_open_phase::active &&
					authority_valid.load(std::memory_order_acquire) && emergency_latch &&
					!emergency_latch->load(std::memory_order_acquire) && alias_authority_latch &&
					alias_authority_latch->load(std::memory_order_acquire) &&
					family_authority_latch &&
					family_authority_latch->load(std::memory_order_acquire) && lineage_seal &&
					lineage_seal->authority_valid.load(std::memory_order_acquire) && state &&
					owner && owner->process_epoch->load(std::memory_order_acquire) == process_epoch;
			}

			[[nodiscard]] bool retain_descendant() noexcept
			{
				auto current = descendant_authority_count.load(std::memory_order_acquire);
				while (current != std::numeric_limits<std::size_t>::max())
					if (descendant_authority_count.compare_exchange_weak(current,
																		 current + 1U,
																		 std::memory_order_acq_rel,
																		 std::memory_order_acquire))
						return true;
				return false;
			}

			[[nodiscard]] bool release_descendant() noexcept
			{
				auto current = descendant_authority_count.load(std::memory_order_acquire);
				while (current != 0U)
					if (descendant_authority_count.compare_exchange_weak(current,
																		 current - 1U,
																		 std::memory_order_acq_rel,
																		 std::memory_order_acquire))
						return true;
				return false;
			}

			const std::weak_ptr<sqlite_shm_mapping_registry_state> registry_state;
			const std::weak_ptr<sqlite_shm_registry_process_owner_seal> process_seal;
			const std::shared_ptr<std::atomic_bool> emergency_latch;
			const std::shared_ptr<std::atomic_bool> alias_authority_latch;
			const std::shared_ptr<std::atomic_bool> family_authority_latch;
			const std::shared_ptr<sqlite_shm_reader_open_lineage_seal> lineage_seal;
			const sqlite_shm_reader_open_binding binding;
			const std::uint64_t process_epoch{};
			const std::uint64_t alias_token{};
			const std::uint64_t family_epoch{};
			const std::uint64_t family_pin_token{};
			const std::uint64_t open_token{};
			std::atomic<sqlite_shm_reader_open_phase> phase{sqlite_shm_reader_open_phase::active};
			std::atomic_bool authority_valid{true};
			std::atomic_size_t descendant_authority_count{};
		};

		struct sqlite_shm_writer_member_authority_state
		{
			std::optional<sqlite_writer_shm_mapping_epoch_arm> epoch_arm;
			std::optional<sqlite_shm_registry_activity_seal> audit_seal;
			// Declared last so ambiguous destruction invalidates registry activity first while
			// the strong epoch arm is still retained.
			std::optional<sqlite_shm_registry_activity_pin> activity;
		};

		struct sqlite_shm_reader_attachment_authority_state
		{
			std::optional<sqlite_shm_reader_attachment_reservation_identity> attachment;
			std::shared_ptr<sqlite_shm_reader_open_control> open;
			std::optional<sqlite_shm_registry_activity_seal> audit_seal;
			// Declared last so ambiguous destruction invalidates registry activity while the
			// exact reservation identity and weak audit binding remain retained.
			std::optional<sqlite_shm_registry_activity_pin> activity;
		};

		struct sqlite_shm_reader_map_predelegate_authority_state
		{
			std::optional<sqlite_shm_reader_attachment_map_request> request;
			std::shared_ptr<sqlite_shm_reader_open_control> open;
			std::optional<sqlite_shm_registry_activity_seal> audit_seal;
			std::optional<sqlite_shm_registry_activity_pin> activity;
		};

		class sqlite_shm_mapping_registry_state final
			: public std::enable_shared_from_this<sqlite_shm_mapping_registry_state>
		{
			friend class ::cxxlens::sdk::sqlite_shm_reader_attachment_authority;
			friend class ::cxxlens::sdk::sqlite_shm_reader_candidate_authority_minter;
			friend class ::cxxlens::sdk::sqlite_shm_reader_map_predelegate_authority;
			friend class ::cxxlens::sdk::sqlite_shm_reader_map_predelegate_minter;
			friend class ::cxxlens::sdk::sqlite_shm_reader_open_authority;
			friend class ::cxxlens::sdk::sqlite_shm_writer_member_authority;

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
				std::size_t active_reader_opens{};
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
				std::size_t active_reader_opens{};
				std::size_t retired_reader_lifecycle_tombstone_count{};
				std::size_t retired_reader_open_epoch_close_tombstone_count{};
				bool reader_lifecycle_tombstones_exported{};
				bool reader_open_epoch_close_tombstones_exported{};
				std::shared_ptr<std::atomic_bool> activity_authority_latch;
				std::vector<std::weak_ptr<
					std::atomic<sqlite_shm_reader_lifecycle_owner_phase>>>
					reader_map_identity_phases;
			};

			struct family_pin_record
			{
				std::uint64_t token{};
				std::uint64_t alias_token{};
				std::uint64_t family_epoch{};
				std::size_t active_activities{};
				std::size_t active_reader_opens{};
				bool active{};
				bool abandoned{};
				std::shared_ptr<std::atomic_bool> identity_authority_latch;
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

			struct reader_open_record
			{
				std::uint64_t token{};
				std::uint64_t alias_token{};
				std::uint64_t family_epoch{};
				std::uint64_t family_pin_token{};
				bool active{};
				std::shared_ptr<sqlite_shm_reader_open_control> control;
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
						if (seal->process_epoch->load(std::memory_order_acquire) == process_epoch)
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
					seal_->process_epoch->load(std::memory_order_acquire) == process_epoch_;
			}

			[[nodiscard]] const std::shared_ptr<std::atomic<std::uint64_t>>&
			process_epoch_latch_for_identity_issuer() const noexcept
			{
				return seal_->process_epoch;
			}

			[[nodiscard]] const sqlite_backend_opaque_identity&
			process_instance_for_identity_issuer() const noexcept
			{
				return process_instance_;
			}

			[[nodiscard]] std::uint64_t process_epoch_for_identity_issuer() const noexcept
			{
				return process_epoch_;
			}

			[[nodiscard]] const std::shared_ptr<std::atomic_bool>&
			registry_quarantine_latch_for_identity_issuer() const noexcept
			{
				return activity_emergency_latch_;
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
						if (seal->process_epoch->load(std::memory_order_acquire) == process_epoch &&
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
				if (!fresh_admission_has_no_pending_reader_ack_locked(*family))
					return rejection(
						sqlite_shm_lease_rejection_reason::retiring,
						sqlite_shm_lease_recovery_action::await_complete_attachment_gate_boundary);

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

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_open_authority>
			acquire_reader_open(sqlite_shm_registry_family_pin& pin,
								const sqlite_shm_reader_open_binding& binding)
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token);
				if (!valid_reader_open_binding(binding))
					return rejection(sqlite_shm_lease_rejection_reason::invalid_request);
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					synchronize_reader_open_controls_locked();
					synchronize_coordinator_quarantines_locked();
					if (admission_quarantined_locked())
						return rejection(sqlite_shm_lease_rejection_reason::quarantined,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					auto* family_pin = current_family_pin_locked(pin);
					auto* alias = find_alias_locked(pin.alias_token_);
					auto* family = find_family_epoch_locked(pin.family_epoch_);
					if (family_pin == nullptr || alias == nullptr || family == nullptr)
						return rejection(sqlite_shm_lease_rejection_reason::stale_token);
					if (alias->phase != sqlite_shm_registry_alias_phase::registered ||
						family->phase != sqlite_shm_registry_family_phase::active)
						return rejection(sqlite_shm_lease_rejection_reason::retiring);
					if (binding.family != family->binding ||
						binding.alias_lifetime != alias->alias_lifetime)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch);
					if (!alias->runtime_lifetime.valid() || !alias->activity_authority_latch ||
						!alias->activity_authority_latch->load(std::memory_order_acquire) ||
						!exact_family_admission_visible_locked(*family))
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					if (!fresh_admission_has_no_pending_reader_ack_locked(*family))
						return rejection(sqlite_shm_lease_rejection_reason::retiring,
										 sqlite_shm_lease_recovery_action::
											 await_complete_attachment_gate_boundary);
					if (std::ranges::any_of(reader_opens_,
											[&binding](const reader_open_record& open)
											{
												return open.control &&
													open.control->binding == binding;
											}))
						return rejection(sqlite_shm_lease_rejection_reason::stale_token);

					std::uint64_t token{};
					if (!allocate_counter_locked(next_reader_open_token_, token))
						return counter_exhaustion_rejection();
					const sqlite_shm_reader_open_control::coordinates coordinates{
						process_epoch_,
						alias->token,
						family->entry_epoch,
						family_pin->token,
						token,
					};
					auto lineage_seal = std::make_shared<sqlite_shm_reader_open_lineage_seal>();
					auto control = std::make_shared<sqlite_shm_reader_open_control>(
						weak_from_this(),
						seal_,
						activity_emergency_latch_,
						alias->activity_authority_latch,
						family->activity_authority_latch,
						lineage_seal,
						binding,
						coordinates);
					const auto open_epoch_binding = lease_reader_open_epoch_binding(
						binding, alias->runtime_lifetime.pin_identity());
					static_assert(std::is_nothrow_move_constructible_v<reader_open_record>);
					reader_open_record prepared_open{
						token, alias->token, family->entry_epoch, family_pin->token, true, control};
					// Make registry publication allocation-free before the lease side mints its
					// orthogonal close owner.  There is then no observable registry record until
					// lease registration succeeds, and the final move cannot strand lease state.
					reader_opens_.reserve(reader_opens_.size() + 1U);
					if (!control->authority_valid_now())
					{
						control->authority_valid.store(false, std::memory_order_release);
						control->phase.store(sqlite_shm_reader_open_phase::clean_released,
											 std::memory_order_release);
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					}
					const sqlite_shm_reader_open_admission_guard admission_guard{
						activity_emergency_latch_,
						alias->activity_authority_latch,
						family->activity_authority_latch};
					auto registered = family->coordinator->register_registry_reader_open(
						token, lineage_seal, open_epoch_binding, admission_guard);
					if (!registered)
					{
						control->authority_valid.store(false, std::memory_order_release);
						control->phase.store(sqlite_shm_reader_open_phase::clean_released,
											 std::memory_order_release);
						if (registered.error().reason ==
								sqlite_shm_lease_rejection_reason::quarantined ||
							registered.error().reason ==
								sqlite_shm_lease_rejection_reason::lifecycle_ambiguous)
							quarantine_family_locked(*family);
						return registered.error();
					}
					reader_opens_.push_back(std::move(prepared_open));
					++family_pin->active_reader_opens;
					++alias->active_reader_opens;
					++family->active_reader_opens;
					return sqlite_shm_reader_open_authority{weak_from_this(), std::move(control)};
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
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
						if (!fresh_admission_has_no_pending_reader_ack_locked(*family))
							return rejection(sqlite_shm_lease_rejection_reason::retiring,
											 sqlite_shm_lease_recovery_action::
												 await_complete_attachment_gate_boundary);

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

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_session_admission>
			admit_reader_session_before_sqlite(
				sqlite_shm_registry_family_pin& pin,
				const sqlite_shm_reader_open_authority& open,
				const sqlite_shm_reader_pre_sqlite_session_request& request)
			{
				const auto rejected = [](const sqlite_shm_lease_rejection failure)
				{
					return sqlite_shm_reader_session_admission{
						sqlite_shm_reader_session_admission_kind::rejected_before_sqlite,
						std::nullopt,
						std::nullopt,
						failure,
					};
				};
				if (!current(pin.process_epoch_))
					return rejected(rejection(sqlite_shm_lease_rejection_reason::stale_token));
				if (!valid_reader_pre_sqlite_request(request))
					return rejected(rejection(sqlite_shm_lease_rejection_reason::invalid_request));

				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					synchronize_reader_open_controls_locked();
					synchronize_coordinator_quarantines_locked();
					if (pin.state_.get() != this)
						return rejected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch));
					if (admission_quarantined_locked())
						return rejected(
							rejection(sqlite_shm_lease_rejection_reason::quarantined,
									  sqlite_shm_lease_recovery_action::quarantine_no_retry));
					auto* family_pin = current_family_pin_locked(pin);
					auto* alias = find_alias_locked(pin.alias_token_);
					auto* family = find_family_epoch_locked(pin.family_epoch_);
					if (family_pin == nullptr || alias == nullptr || family == nullptr)
						return rejected(rejection(sqlite_shm_lease_rejection_reason::stale_token));
					if (alias->phase == sqlite_shm_registry_alias_phase::quarantined ||
						family->phase == sqlite_shm_registry_family_phase::quarantined)
						return rejected(
							rejection(sqlite_shm_lease_rejection_reason::quarantined,
									  sqlite_shm_lease_recovery_action::quarantine_no_retry));
					auto* reader_open = current_reader_open_locked(open);
					if (reader_open == nullptr)
						return rejected(rejection(sqlite_shm_lease_rejection_reason::stale_token));
					if (alias->phase != sqlite_shm_registry_alias_phase::registered ||
						family->phase != sqlite_shm_registry_family_phase::active ||
						!family->coordinator)
						return rejected(rejection(sqlite_shm_lease_rejection_reason::retiring));
					if (request.family != family->binding ||
						request.alias_lifetime != alias->alias_lifetime)
						return rejected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch));
					if (reader_open->alias_token != alias->token ||
						reader_open->family_epoch != family->entry_epoch ||
						reader_open->family_pin_token != family_pin->token ||
						!reader_open_binding_matches_request(reader_open->control->binding,
															 request))
						return rejected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch));
					if (!alias->runtime_lifetime.valid() || !alias->activity_authority_latch ||
						!alias->activity_authority_latch->load(std::memory_order_acquire) ||
						!exact_family_admission_visible_locked(*family))
						return rejected(
							rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									  sqlite_shm_lease_recovery_action::quarantine_no_retry));
					if (!fresh_admission_has_no_pending_reader_ack_locked(*family))
						return rejected(rejection(sqlite_shm_lease_rejection_reason::retiring,
												  sqlite_shm_lease_recovery_action::
													  await_complete_attachment_gate_boundary));

					sqlite_shm_reader_candidate_authority_minter minter{*this, pin, open, request};
					auto admitted = family->coordinator->admit_registry_reader_session(
						pin, reader_open->token, request, minter);
					if (!admitted &&
						(admitted.error().reason ==
							 sqlite_shm_lease_rejection_reason::lifecycle_ambiguous ||
						 admitted.error().reason == sqlite_shm_lease_rejection_reason::quarantined))
						synchronize_coordinator_quarantines_locked();
					return admitted;
				}
				catch (...)
				{
					emergency_quarantine();
					return rejected(
						rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								  sqlite_shm_lease_recovery_action::quarantine_no_retry));
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_mapping_tuple>
			authenticate_reader_cached_member_use(
				sqlite_shm_registry_family_pin& pin,
				const sqlite_shm_reader_session& session,
				const sqlite_shm_reader_cached_member_identity& member) noexcept
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
									 sqlite_shm_lease_recovery_action::deny_before_native_map);
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					synchronize_reader_open_controls_locked();
					synchronize_coordinator_quarantines_locked();
					if (pin.state_.get() != this)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										 sqlite_shm_lease_recovery_action::deny_before_native_map);
					if (admission_quarantined_locked())
						return rejection(sqlite_shm_lease_rejection_reason::quarantined,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);

					auto* family_pin = current_family_pin_locked(pin);
					auto* alias = find_alias_locked(pin.alias_token_);
					auto* family = find_family_epoch_locked(pin.family_epoch_);
					if (family_pin == nullptr || alias == nullptr || family == nullptr)
						return rejection(sqlite_shm_lease_rejection_reason::stale_token,
										 sqlite_shm_lease_recovery_action::deny_before_native_map);
					if (alias->phase == sqlite_shm_registry_alias_phase::quarantined ||
						family->phase == sqlite_shm_registry_family_phase::quarantined)
						return rejection(sqlite_shm_lease_rejection_reason::quarantined,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					if (alias->phase != sqlite_shm_registry_alias_phase::registered ||
						family->phase != sqlite_shm_registry_family_phase::active ||
						!family->coordinator)
						return rejection(sqlite_shm_lease_rejection_reason::retiring,
										 sqlite_shm_lease_recovery_action::deny_before_native_map);
					if (!alias->runtime_lifetime.valid() || !alias->activity_authority_latch ||
						!alias->activity_authority_latch->load(std::memory_order_acquire) ||
						!exact_family_admission_visible_locked(*family))
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);

					auto authenticated =
						family->coordinator->authenticate_registry_reader_cached_member_use(
							pin, session, member);
					if (!authenticated &&
						(authenticated.error().reason ==
							 sqlite_shm_lease_rejection_reason::lifecycle_ambiguous ||
						 authenticated.error().reason ==
							 sqlite_shm_lease_rejection_reason::quarantined))
						synchronize_coordinator_quarantines_locked();
					return authenticated;
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]]
			sqlite_shm_lease_result<sqlite_shm_reader_attachment_map_inflight>
			begin_reader_map(sqlite_shm_registry_family_pin& pin,
							 sqlite_shm_reader_session& session,
							 const sqlite_shm_reader_attachment_map_request& request)
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token);
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					synchronize_reader_open_controls_locked();
					synchronize_coordinator_quarantines_locked();
					if (pin.state_.get() != this)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch);
					if (admission_quarantined_locked())
						return rejection(sqlite_shm_lease_rejection_reason::quarantined,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					auto* family_pin = current_family_pin_locked(pin);
					auto* alias = find_alias_locked(pin.alias_token_);
					auto* family = find_family_epoch_locked(pin.family_epoch_);
					if (family_pin == nullptr || alias == nullptr || family == nullptr)
						return rejection(sqlite_shm_lease_rejection_reason::stale_token);
					if (alias->phase != sqlite_shm_registry_alias_phase::registered ||
						family->phase != sqlite_shm_registry_family_phase::active ||
						!family->coordinator)
						return rejection(sqlite_shm_lease_rejection_reason::retiring);
					if (request.family != family->binding ||
						request.alias_lifetime != alias->alias_lifetime ||
						request.expected_attachment.runtime_lifetime_pin() !=
							alias->runtime_lifetime.pin_identity())
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch);
					if (!alias->activity_authority_latch ||
						!alias->activity_authority_latch->load(std::memory_order_acquire) ||
						!exact_family_admission_visible_locked(*family))
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					if (!fresh_admission_has_no_pending_reader_ack_locked(*family))
						return rejection(sqlite_shm_lease_rejection_reason::retiring,
										 sqlite_shm_lease_recovery_action::
											 await_complete_attachment_gate_boundary);
					if (std::ranges::any_of(
							retired_reader_lifecycle_tombstones_,
							[&request](
								const sqlite_shm_reader_lifecycle_compact_tombstone& tombstone)
							{
								return tombstone.attachment == request.expected_attachment;
							}))
						return rejection(sqlite_shm_lease_rejection_reason::stale_token);
					auto tombstone_check =
						family->coordinator->check_registry_reader_lifecycle_tombstone(
							request.expected_attachment);
					if (!tombstone_check)
						return tombstone_check.error();

					sqlite_shm_reader_map_predelegate_minter minter{*this, pin};
					auto begun = family->coordinator->begin_registry_reader_map(
						pin, session, request, minter);
					if (!begun &&
						(begun.error().reason ==
							 sqlite_shm_lease_rejection_reason::lifecycle_ambiguous ||
						 begun.error().reason == sqlite_shm_lease_rejection_reason::quarantined))
						synchronize_coordinator_quarantines_locked();
					return begun;
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] bool exact_reader_map_identity_admission_locked(
				const sqlite_shm_registry_family_pin& pin,
				const sqlite_shm_reader_attachment_map_pre_request& request,
				family_record*& exact_family) noexcept
			{
				exact_family = nullptr;
				auto* family_pin = current_family_pin_locked(pin);
				auto* alias = find_alias_locked(pin.alias_token_);
				auto* family = find_family_epoch_locked(pin.family_epoch_);
				if (family_pin == nullptr || alias == nullptr || family == nullptr ||
					alias->phase != sqlite_shm_registry_alias_phase::registered ||
					family->phase != sqlite_shm_registry_family_phase::active ||
					!family->coordinator || request.family != family->binding ||
					request.alias_lifetime != alias->alias_lifetime ||
					request.connection_token != request.expected_attachment.connection_token() ||
					request.expected_attachment.runtime_lifetime_pin() !=
						alias->runtime_lifetime.pin_identity() || !alias->runtime_lifetime.valid() ||
					!alias->activity_authority_latch ||
					!alias->activity_authority_latch->load(std::memory_order_acquire) ||
					!exact_family_admission_visible_locked(*family))
					return false;
				auto* open =
					find_reader_open_locked(request.expected_attachment.registry_open_token());
				if (open == nullptr || !open->active || !open->control ||
					!reader_open_control_matches_record_locked(*open) ||
					!open->control->authority_valid_now() || open->alias_token != alias->token ||
					open->family_epoch != family->entry_epoch ||
					open->family_pin_token != family_pin->token ||
					open->control->binding.family != request.family ||
					open->control->binding.alias_lifetime != request.alias_lifetime ||
					open->control->binding.connection_token != request.connection_token ||
					open->control->binding.main_native_file_receipt !=
						request.expected_attachment.main_native_file_receipt() ||
					open->control->binding.main_xopen_receipt !=
						request.expected_attachment.main_xopen_receipt() ||
					open->control->binding.open_epoch !=
						request.expected_attachment.open_epoch() ||
					open->control->binding.callback_cohort !=
						request.expected_attachment.callback_cohort())
					return false;
				exact_family = family;
				return true;
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_attachment_map_prepared>
			prepare_reader_map_identity(
				sqlite_shm_registry_family_pin& pin,
				sqlite_shm_reader_session& session,
				const sqlite_shm_reader_attachment_map_pre_request& request)
			{
				if (!current(pin.process_epoch_) || !valid_reader_map_identity_pre_request(request))
					return rejection(sqlite_shm_lease_rejection_reason::invalid_request,
						sqlite_shm_lease_recovery_action::deny_before_native_map);
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					synchronize_reader_open_controls_locked();
					synchronize_coordinator_quarantines_locked();
					if (pin.state_.get() != this || admission_quarantined_locked())
						return rejection(sqlite_shm_lease_rejection_reason::quarantined,
							sqlite_shm_lease_recovery_action::quarantine_no_retry);
					family_record* family{};
					if (!exact_reader_map_identity_admission_locked(pin, request, family))
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
							sqlite_shm_lease_recovery_action::deny_before_native_map);
					if (!fresh_admission_has_no_pending_reader_ack_locked(*family))
						return rejection(sqlite_shm_lease_rejection_reason::retiring,
							sqlite_shm_lease_recovery_action::
								await_complete_attachment_gate_boundary);
					if (std::ranges::any_of(
							retired_reader_lifecycle_tombstones_,
							[&request](const auto& tombstone)
							{ return tombstone.attachment == request.expected_attachment; }))
						return rejection(sqlite_shm_lease_rejection_reason::stale_token,
							sqlite_shm_lease_recovery_action::deny_before_native_map);
					auto tombstone = family->coordinator->check_registry_reader_lifecycle_tombstone(
						request.expected_attachment);
					if (!tombstone)
						return tombstone.error();
					auto prepared = family->coordinator->prepare_registry_reader_map_identity(
						pin,
						session,
						request,
						sqlite_shm_reader_map_identity_prepare_capability{
							seal_->process_epoch,
							activity_emergency_latch_,
							process_epoch_,
							family->entry_epoch,
							pin.pin_token_,
							pin.alias_token_});
					if (!prepared)
						return prepared;
					auto phase = prepared->identity_owner_phase_for_registry();
					if (!phase)
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
							sqlite_shm_lease_recovery_action::quarantine_no_retry);
					std::erase_if(family->reader_map_identity_phases,
						[](const auto& weak_phase)
						{
							const auto candidate_phase = weak_phase.lock();
							return !candidate_phase ||
								candidate_phase->load(std::memory_order_acquire) !=
								sqlite_shm_reader_lifecycle_owner_phase::admission;
						});
					family->reader_map_identity_phases.emplace_back(phase);
					return prepared;
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
						sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_lifecycle_identity_scope>
			claim_reader_map_identity_scope(
				sqlite_shm_registry_family_pin& pin,
				const sqlite_shm_reader_attachment_map_prepared& prepared,
				const sqlite_shm_reader_attachment_map_pre_request& request,
				const std::shared_ptr<sqlite_shm_process_identity_issuer_state>& issuer)
			{
				bool claimed{};
				try
				{
					if (pin.state_.get() != this || !current(pin.process_epoch_) ||
						!valid_reader_map_identity_pre_request(request) ||
						!prepared.matches_identity_pre_request_for_registry(request) ||
						!prepared.matches_identity_registry_coordinates_for_registry(
							pin.family_epoch_, pin.pin_token_, pin.alias_token_,
							seal_->process_epoch, activity_emergency_latch_))
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
							sqlite_shm_lease_recovery_action::deny_before_native_map);
					const auto request_seal = make_reader_map_identity_request_seal(request);
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					synchronize_reader_open_controls_locked();
					synchronize_coordinator_quarantines_locked();
					if (pin.state_.get() != this || admission_quarantined_locked())
						return rejection(sqlite_shm_lease_rejection_reason::quarantined,
							sqlite_shm_lease_recovery_action::quarantine_no_retry);
					family_record* family{};
					if (!exact_reader_map_identity_admission_locked(pin, request, family) ||
						!prepared.matches_identity_pre_request_for_registry(request))
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
							sqlite_shm_lease_recovery_action::deny_before_native_map);
					const auto phase = prepared.identity_owner_phase_for_registry();
					const auto abandonment = prepared.identity_owner_abandonment_for_registry();
					std::erase_if(family->reader_map_identity_phases,
						[](const auto& weak_phase)
						{
							const auto candidate = weak_phase.lock();
							return !candidate || candidate->load(std::memory_order_acquire) !=
								sqlite_shm_reader_lifecycle_owner_phase::admission;
						});
					const auto registered_phase = std::ranges::any_of(
						family->reader_map_identity_phases,
						[&phase](const auto& weak_phase)
						{
							const auto candidate = weak_phase.lock();
							return candidate && candidate.get() == phase.get();
						});
					if (!phase || abandonment.expired() ||
						!registered_phase ||
						!prepared.claim_identity_scope_for_registry())
						return rejection(sqlite_shm_lease_rejection_reason::stale_token,
							sqlite_shm_lease_recovery_action::deny_before_native_map);
					claimed = true;
					auto scope = seal_qualified_identity_scope_for_registry(issuer,
						family->binding,
						pin.identity_authority_latch_,
						family->entry_epoch,
						pin.pin_token_,
						request.expected_attachment.callback_cohort(),
						request_seal,
						{request.expected_attachment.registry_open_token(),
						 sqlite_shm_reader_lifecycle_owner_kind::map,
						 prepared.token_,
						 prepared.generation_},
						phase,
						abandonment);
					if (!scope.valid())
					{
						prepared.abandon_identity_owner_for_registry();
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
							sqlite_shm_lease_recovery_action::quarantine_no_retry);
					}
					return scope;
				}
				catch (...)
				{
					if (claimed)
						prepared.abandon_identity_owner_for_registry();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
						claimed ? sqlite_shm_lease_recovery_action::quarantine_no_retry
							: sqlite_shm_lease_recovery_action::deny_before_native_map);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_attachment_map_inflight>
			bind_reader_map_identity(
				sqlite_shm_registry_family_pin& pin,
				sqlite_shm_reader_session& session,
				sqlite_shm_reader_attachment_map_prepared& prepared,
				const sqlite_shm_reader_lifecycle_identity_scope& scope,
				const sqlite_shm_issued_reader_callback_identity& callback,
				const std::shared_ptr<sqlite_shm_process_identity_issuer_state>& issuer)
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
						sqlite_shm_lease_recovery_action::deny_before_native_map);
				// Establish the non-throwing owner-coordinate cut before copying the
				// length-framed request.  An allocation failure for the exact owner is
				// fail-closed; a foreign pin remains completely non-mutating.
				const auto exact_presented_owner = pin.state_.get() == this &&
					prepared.matches_identity_registry_coordinates_for_registry(
						pin.family_epoch_, pin.pin_token_, pin.alias_token_,
						seal_->process_epoch, activity_emergency_latch_);
				try
				{
					const auto pre_request = prepared.identity_pre_request_for_registry();
					if (!pre_request || !valid_reader_map_identity_pre_request(*pre_request) ||
						!valid_callback(callback.receipt()))
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
							sqlite_shm_lease_recovery_action::deny_before_native_map);
					const auto request_seal = make_reader_map_identity_request_seal(*pre_request);
					const auto& receipt = callback.receipt();
					const sqlite_shm_reader_attachment_map_request request{
						pre_request->family,
						pre_request->alias_lifetime,
						pre_request->connection_token,
						pre_request->expected_attachment,
						receipt,
						pre_request->page_number,
						pre_request->page_size,
						pre_request->caller_extend};
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					synchronize_reader_open_controls_locked();
					synchronize_coordinator_quarantines_locked();
					if (pin.state_.get() != this ||
						!prepared.matches_identity_registry_coordinates_for_registry(
							pin.family_epoch_, pin.pin_token_, pin.alias_token_,
							seal_->process_epoch, activity_emergency_latch_))
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
							sqlite_shm_lease_recovery_action::deny_before_native_map);
					family_record* family{};
					if (admission_quarantined_locked() ||
						!exact_reader_map_identity_admission_locked(pin, *pre_request, family))
					{
						prepared.abandon_identity_owner_for_registry();
						return rejection(sqlite_shm_lease_rejection_reason::quarantined,
							sqlite_shm_lease_recovery_action::quarantine_no_retry);
					}
					if (!prepared.matches_identity_pre_request_for_registry(*pre_request))
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
							sqlite_shm_lease_recovery_action::deny_before_native_map);
					const auto phase = prepared.identity_owner_phase_for_registry();
					const auto abandonment = prepared.identity_owner_abandonment_for_registry();
					const sqlite_shm_reader_lifecycle_owner_coordinates coordinates{
						pre_request->expected_attachment.registry_open_token(),
						sqlite_shm_reader_lifecycle_owner_kind::map,
						prepared.token_,
						prepared.generation_};
					if (!qualified_identity_scope_matches_for_registry(issuer,
							scope,
							family->binding,
							family->entry_epoch,
							pin.pin_token_,
							pre_request->expected_attachment.callback_cohort(),
							request_seal,
							coordinates,
							phase,
							abandonment))
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
							sqlite_shm_lease_recovery_action::deny_before_native_map);
					auto validated = validate_callback_identity_for_registry(
						issuer, scope, callback, sqlite_shm_reader_callback_identity_role::map);
					if (!validated)
						return validated.error();
					auto completion =
						make_identity_completion_for_registry(issuer, scope, callback);
					if (!completion)
					{
						prepared.abandon_identity_owner_for_registry();
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
							sqlite_shm_lease_recovery_action::quarantine_no_retry);
					}
					auto control = prepared.identity_owner_control_for_registry();
					if (!control)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
							sqlite_shm_lease_recovery_action::deny_before_native_map);
					sqlite_shm_reader_map_predelegate_minter minter{*this, pin};
					auto bound = family->coordinator->bind_registry_reader_map_identity(
						pin,
						session,
						prepared,
						request,
						sqlite_shm_reader_map_identity_binding_capability{
							std::move(control), receipt, std::move(completion)},
						minter);
					if (bound)
					{
						std::erase_if(family->reader_map_identity_phases,
							[&phase](const auto& weak_phase)
							{
								const auto candidate = weak_phase.lock();
								return !candidate || candidate.get() == phase.get();
							});
					}
					else if (bound.error().reason ==
							sqlite_shm_lease_rejection_reason::lifecycle_ambiguous ||
						 bound.error().reason == sqlite_shm_lease_rejection_reason::quarantined)
						synchronize_coordinator_quarantines_locked();
					return bound;
				}
				catch (...)
				{
					if (exact_presented_owner)
						prepared.abandon_identity_owner_for_registry();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
						sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]]
			sqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_zero_effect_receipt>
			validate_reader_zero_attachment_effect(
				sqlite_shm_registry_family_pin& pin,
				const sqlite_shm_reader_attachment_map_inflight& inflight,
				const sqlite_shm_reader_lifecycle_identity_scope& scope,
				const sqlite_shm_issued_reader_callback_identity& callback,
				const sqlite_shm_issued_reader_effect_identity& effect,
				const int native_status,
				const volatile void* native_mapping,
				const int delegated_extend,
				const std::shared_ptr<sqlite_shm_process_identity_issuer_state>& issuer) noexcept
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
						sqlite_shm_lease_recovery_action::quarantine_no_retry);
				if (inflight.terminal_presentation_stale_for_registry())
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
						sqlite_shm_lease_recovery_action::quarantine_no_retry);
				try
				{
					std::scoped_lock lock{mutex_};
					if (pin.state_.get() != this)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
							sqlite_shm_lease_recovery_action::quarantine_no_retry);
					synchronize_activity_controls_locked();
					synchronize_reader_open_controls_locked();
					synchronize_coordinator_quarantines_locked();
					const auto exact_owner = inflight.qualified_identity_owned_for_registry(
						pin.family_epoch_, pin.pin_token_, pin.alias_token_, seal_->process_epoch,
						activity_emergency_latch_);
					if (!inflight.has_qualified_identity_for_registry())
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
							sqlite_shm_lease_recovery_action::quarantine_no_retry);
					if (!exact_owner)
						return rejection(sqlite_shm_lease_rejection_reason::stale_token,
							sqlite_shm_lease_recovery_action::quarantine_no_retry);
					auto* family_pin = current_family_pin_locked(pin);
					auto* alias = find_alias_locked(pin.alias_token_);
					auto* family = find_family_epoch_locked(pin.family_epoch_);
					if (family_pin == nullptr || alias == nullptr || family == nullptr ||
						!family->coordinator || admission_quarantined_locked())
						return rejection(sqlite_shm_lease_rejection_reason::stale_token,
							sqlite_shm_lease_recovery_action::quarantine_no_retry);
					auto capability = detail::validate_zero_effect_identity_for_registry(issuer,
						scope,
						callback,
						effect,
						inflight.qualified_identity_owner_abandonment_for_registry());
					if (!capability)
						return capability.error();
					return family->coordinator->validate_registry_reader_zero_attachment_effect(
						pin,
						inflight,
						std::move(*capability),
						native_status,
						native_mapping,
						delegated_extend);
				}
				catch (...)
				{
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
						sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]]
			sqlite_shm_lease_result<sqlite_shm_reader_late_close_outer_unwind_authority>
			mint_reader_late_close_outer_unwind_authority(
				sqlite_shm_registry_family_pin& pin,
				sqlite_shm_reader_attachment_map_inflight& inflight,
				sqlite_shm_reader_session& session,
				const sqlite_shm_callback_execution_receipt& expected_outer_unmap_callback)
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
								 sqlite_shm_lease_recovery_action::deny_before_native_map);
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					synchronize_reader_open_controls_locked();
					synchronize_coordinator_quarantines_locked();
					if (pin.state_.get() != this)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									 sqlite_shm_lease_recovery_action::deny_before_native_map);
					const auto exact_qualified_owner =
						inflight.qualified_identity_owned_for_registry(
							pin.family_epoch_, pin.pin_token_, pin.alias_token_,
							seal_->process_epoch, activity_emergency_latch_);
					if (inflight.has_qualified_identity_for_registry() && !exact_qualified_owner)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									 sqlite_shm_lease_recovery_action::deny_before_native_map);
					if (admission_quarantined_locked())
						return rejection(sqlite_shm_lease_rejection_reason::quarantined,
									 sqlite_shm_lease_recovery_action::deny_before_native_map);
					auto* family_pin = current_family_pin_locked(pin);
					auto* alias = find_alias_locked(pin.alias_token_);
					auto* family = find_family_epoch_locked(pin.family_epoch_);
					if (family_pin == nullptr || alias == nullptr || family == nullptr ||
						!family->coordinator ||
						alias->phase != sqlite_shm_registry_alias_phase::registered ||
						family->phase != sqlite_shm_registry_family_phase::active ||
						!exact_family_admission_visible_locked(*family))
						return rejection(sqlite_shm_lease_rejection_reason::retiring,
									 sqlite_shm_lease_recovery_action::deny_before_native_map);
					return family->coordinator
						->mint_registry_reader_late_close_outer_unwind_authority(
							pin, inflight, session, expected_outer_unmap_callback);
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<void>
			mark_reader_late_close_native_map_start(
				sqlite_shm_registry_family_pin& pin,
				sqlite_shm_reader_attachment_map_inflight& inflight,
				const sqlite_shm_reader_session& session,
				const sqlite_shm_reader_late_close_outer_unwind_authority& owner) noexcept
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
								 sqlite_shm_lease_recovery_action::deny_before_native_map);
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					synchronize_reader_open_controls_locked();
					synchronize_coordinator_quarantines_locked();
					if (pin.state_.get() != this)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									 sqlite_shm_lease_recovery_action::deny_before_native_map);
					const auto exact_qualified_owner =
						inflight.qualified_identity_owned_for_registry(
							pin.family_epoch_, pin.pin_token_, pin.alias_token_,
							seal_->process_epoch, activity_emergency_latch_);
					if (inflight.has_qualified_identity_for_registry() && !exact_qualified_owner)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									 sqlite_shm_lease_recovery_action::deny_before_native_map);
					if (admission_quarantined_locked())
						return rejection(sqlite_shm_lease_rejection_reason::quarantined,
									 sqlite_shm_lease_recovery_action::deny_before_native_map);
					auto* family_pin = current_family_pin_locked(pin);
					auto* alias = find_alias_locked(pin.alias_token_);
					auto* family = find_family_epoch_locked(pin.family_epoch_);
					if (family_pin == nullptr || alias == nullptr || family == nullptr ||
						!family->coordinator ||
						alias->phase != sqlite_shm_registry_alias_phase::registered ||
						family->phase != sqlite_shm_registry_family_phase::active ||
						!exact_family_admission_visible_locked(*family))
						return rejection(sqlite_shm_lease_rejection_reason::retiring,
									 sqlite_shm_lease_recovery_action::deny_before_native_map);
					return family->coordinator->mark_registry_reader_late_close_native_map_start(
						pin, inflight, session, owner);
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_map_commit>
			commit_reader_map(sqlite_shm_registry_family_pin& pin,
							  sqlite_shm_reader_attachment_map_inflight& inflight,
							  const sqlite_shm_verified_reader_attachment_post_map_receipt& receipt,
							  sqlite_shm_reader_session& session)
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
									 sqlite_shm_lease_recovery_action::
										 attempt_nonremoving_unmap_then_outer_ioerr);
				try
				{
					std::optional<sqlite_shm_reader_map_predelegate_authority>
						completed_predelegate;
					std::optional<sqlite_shm_lease_result<sqlite_shm_reader_map_commit>> result;
					{
						std::scoped_lock lock{mutex_};
						synchronize_activity_controls_locked();
						synchronize_coordinator_quarantines_locked();
						if (pin.state_.get() != this)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
											 sqlite_shm_lease_recovery_action::
												 attempt_nonremoving_unmap_then_outer_ioerr);
						auto* family_pin = current_family_pin_locked(pin);
						auto* alias = find_alias_locked(pin.alias_token_);
						auto* family = find_family_epoch_locked(pin.family_epoch_);
						const auto exact_qualified_owner =
							inflight.qualified_identity_owned_for_registry(
								pin.family_epoch_, pin.pin_token_, pin.alias_token_,
								seal_->process_epoch, activity_emergency_latch_);
						if (inflight.has_qualified_identity_for_registry() &&
							!exact_qualified_owner)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
								sqlite_shm_lease_recovery_action::
									attempt_nonremoving_unmap_then_outer_ioerr);
						if (family_pin == nullptr || alias == nullptr || family == nullptr ||
							!family->coordinator ||
							(exact_qualified_owner && admission_quarantined_locked()))
							return rejection(sqlite_shm_lease_rejection_reason::stale_token,
											 sqlite_shm_lease_recovery_action::
												 attempt_nonremoving_unmap_then_outer_ioerr);
						if (receipt.request().family != family->binding ||
							receipt.request().alias_lifetime != alias->alias_lifetime ||
							receipt.request().expected_attachment.runtime_lifetime_pin() !=
								alias->runtime_lifetime.pin_identity())
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
											 sqlite_shm_lease_recovery_action::
												 attempt_nonremoving_unmap_then_outer_ioerr);
						result.emplace(family->coordinator->commit_registry_reader_map(
							pin, inflight, receipt, session, completed_predelegate));
						if (!*result &&
							(result->error().reason ==
								 sqlite_shm_lease_rejection_reason::lifecycle_ambiguous ||
							 result->error().reason ==
								 sqlite_shm_lease_rejection_reason::quarantined))
							synchronize_coordinator_quarantines_locked();
					}
					if (!result)
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					if (completed_predelegate)
					{
						auto released = completed_predelegate->release_activity();
						if (!released)
						{
							emergency_quarantine();
							return released.error();
						}
					}
					if (!*result)
						return result->error();
					return std::move(**result);
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]]
			sqlite_shm_lease_result<sqlite_shm_reader_attachment_zero_effect_result>
			complete_reader_zero_attachment_map(
				sqlite_shm_registry_family_pin& pin,
				sqlite_shm_reader_attachment_map_inflight& inflight,
				const sqlite_shm_verified_reader_attachment_zero_effect_receipt& receipt,
				sqlite_shm_reader_session& session)
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				try
				{
					std::optional<sqlite_shm_reader_map_predelegate_authority>
						completed_predelegate;
					std::optional<sqlite_shm_reader_attachment_authority> completed_candidate;
					std::optional<
						sqlite_shm_lease_result<sqlite_shm_reader_attachment_zero_effect_result>>
						result;
					{
						std::scoped_lock lock{mutex_};
						synchronize_activity_controls_locked();
						synchronize_reader_open_controls_locked();
						synchronize_coordinator_quarantines_locked();
						if (pin.state_.get() != this)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						auto* family_pin = current_family_pin_locked(pin);
						auto* alias = find_alias_locked(pin.alias_token_);
						auto* family = find_family_epoch_locked(pin.family_epoch_);
						const auto exact_qualified_owner =
							inflight.qualified_identity_owned_for_registry(
								pin.family_epoch_, pin.pin_token_, pin.alias_token_,
								seal_->process_epoch, activity_emergency_latch_);
						if (inflight.has_qualified_identity_for_registry() &&
							!exact_qualified_owner)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
								sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if (family_pin == nullptr || alias == nullptr || family == nullptr ||
							!family->coordinator ||
							(exact_qualified_owner && admission_quarantined_locked()))
							return rejection(sqlite_shm_lease_rejection_reason::stale_token,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);

						// This is the terminal commit of an already-native-started attempt.
						// Do not apply a fresh-admission quarantine gate here.
						result.emplace(
							family->coordinator->complete_registry_reader_zero_attachment_map(
								pin,
								inflight,
								receipt,
								session,
								completed_predelegate,
								completed_candidate));
						if (!*result &&
							(result->error().reason ==
								 sqlite_shm_lease_rejection_reason::lifecycle_ambiguous ||
							 result->error().reason ==
								 sqlite_shm_lease_rejection_reason::quarantined))
							synchronize_coordinator_quarantines_locked();
					}
					if (!result)
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					if (!*result)
						return result->error();
					if (completed_predelegate)
					{
						auto released = completed_predelegate->release_activity();
						if (!released)
						{
							emergency_quarantine();
							return released.error();
						}
					}
					if (completed_candidate)
					{
						auto released = completed_candidate->release_activity();
						if (!released)
						{
							emergency_quarantine();
							return released.error();
						}
					}
					return std::move(**result);
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<
				sqlite_shm_reader_opaque_attachment_uncertainty_result>
			complete_reader_opaque_attachment_uncertainty(
				sqlite_shm_registry_family_pin& pin,
				sqlite_shm_reader_attachment_map_inflight& inflight,
				sqlite_shm_reader_session& session)
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				try
				{
					std::optional<sqlite_shm_lease_result<
						sqlite_shm_reader_opaque_attachment_uncertainty_result>>
						result;
					{
						std::scoped_lock lock{mutex_};
						synchronize_activity_controls_locked();
						synchronize_reader_open_controls_locked();
						synchronize_coordinator_quarantines_locked();
						if (pin.state_.get() != this)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						auto* family_pin = current_family_pin_locked(pin);
						auto* family = find_family_epoch_locked(pin.family_epoch_);
						const auto exact_qualified_owner =
							inflight.qualified_identity_owned_for_registry(
								pin.family_epoch_, pin.pin_token_, pin.alias_token_,
								seal_->process_epoch, activity_emergency_latch_);
						if (inflight.has_qualified_identity_for_registry() &&
							!exact_qualified_owner)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
								sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if (family_pin == nullptr || family == nullptr || !family->coordinator ||
							(exact_qualified_owner && admission_quarantined_locked()))
							return rejection(sqlite_shm_lease_rejection_reason::stale_token,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);

						// This is the terminal disposition of an already-native-started map. It
						// intentionally bypasses fresh-admission gates and retains the first-map
						// opaque owners or the existing-group/cut owners in durable quarantine.
						result.emplace(family->coordinator
										   ->complete_registry_reader_opaque_attachment_uncertainty(
											   pin, inflight, session));
						synchronize_coordinator_quarantines_locked();
					}
					if (!result)
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					return *result;
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_predecessor_map_result>
			complete_reader_predecessor_map(
				sqlite_shm_registry_family_pin& pin,
				sqlite_shm_reader_attachment_map_inflight& inflight,
				const sqlite_shm_verified_reader_predecessor_map_receipt& receipt,
				sqlite_shm_reader_session& session)
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				try
				{
					std::optional<sqlite_shm_reader_map_predelegate_authority>
						completed_predelegate;
					std::optional<sqlite_shm_reader_attachment_authority> completed_candidate;
					std::optional<sqlite_shm_lease_result<sqlite_shm_reader_predecessor_map_result>>
						result;
					{
						std::scoped_lock lock{mutex_};
						synchronize_activity_controls_locked();
						synchronize_reader_open_controls_locked();
						synchronize_coordinator_quarantines_locked();
						if (pin.state_.get() != this)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						auto* family_pin = current_family_pin_locked(pin);
						auto* alias = find_alias_locked(pin.alias_token_);
						auto* family = find_family_epoch_locked(pin.family_epoch_);
						const auto exact_qualified_owner =
							inflight.qualified_identity_owned_for_registry(
								pin.family_epoch_, pin.pin_token_, pin.alias_token_,
								seal_->process_epoch, activity_emergency_latch_);
						if (inflight.has_qualified_identity_for_registry() &&
							!exact_qualified_owner)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
								sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if (family_pin == nullptr || alias == nullptr || family == nullptr ||
							!family->coordinator ||
							(exact_qualified_owner && admission_quarantined_locked()))
							return rejection(sqlite_shm_lease_rejection_reason::stale_token,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);

						// This terminalizes an already-native-started attempt and transfers the
						// retained candidate lifetime to the existing byte-semantic route.
						result.emplace(
							family->coordinator->complete_registry_reader_predecessor_map(
								pin,
								inflight,
								receipt,
								session,
								completed_predelegate,
								completed_candidate));
						if (!*result &&
							(result->error().reason ==
								 sqlite_shm_lease_rejection_reason::lifecycle_ambiguous ||
							 result->error().reason ==
								 sqlite_shm_lease_rejection_reason::quarantined))
							synchronize_coordinator_quarantines_locked();
					}
					if (!result)
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					if (!*result)
						return result->error();
					if (completed_predelegate)
					{
						auto released = completed_predelegate->release_activity();
						if (!released)
						{
							emergency_quarantine();
							return released.error();
						}
					}
					// A predecessor-active reservation must retain the candidate activity pin
					// until an exact existing-route unmap or close confirms retirement.
					if (completed_candidate)
					{
						emergency_quarantine();
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					}
					return **result;
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<
				sqlite_shm_reader_existing_group_predecessor_mismatch_result>
			complete_reader_existing_group_predecessor_mismatch(
				sqlite_shm_registry_family_pin& pin,
				sqlite_shm_reader_attachment_map_inflight& inflight,
				const sqlite_shm_verified_reader_predecessor_map_receipt& receipt,
				sqlite_shm_reader_session& session)
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				try
				{
					std::optional<sqlite_shm_reader_map_predelegate_authority>
						completed_predelegate;
					std::optional<sqlite_shm_lease_result<
						sqlite_shm_reader_existing_group_predecessor_mismatch_result>>
						result;
					{
						std::scoped_lock lock{mutex_};
						synchronize_activity_controls_locked();
						synchronize_reader_open_controls_locked();
						synchronize_coordinator_quarantines_locked();
						if (pin.state_.get() != this)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						auto* family_pin = current_family_pin_locked(pin);
						auto* alias = find_alias_locked(pin.alias_token_);
						auto* family = find_family_epoch_locked(pin.family_epoch_);
						const auto exact_qualified_owner =
							inflight.qualified_identity_owned_for_registry(
								pin.family_epoch_, pin.pin_token_, pin.alias_token_,
								seal_->process_epoch, activity_emergency_latch_);
						if (inflight.has_qualified_identity_for_registry() &&
							!exact_qualified_owner)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
								sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if (family_pin == nullptr || alias == nullptr || family == nullptr ||
							!family->coordinator ||
							(exact_qualified_owner && admission_quarantined_locked()))
							return rejection(sqlite_shm_lease_rejection_reason::stale_token,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);

						result.emplace(
							family->coordinator
								->complete_registry_reader_existing_group_predecessor_mismatch(
									pin, inflight, receipt, session, completed_predelegate));
						if (!*result &&
							(result->error().reason ==
								 sqlite_shm_lease_rejection_reason::lifecycle_ambiguous ||
							 result->error().reason ==
								 sqlite_shm_lease_rejection_reason::quarantined))
							synchronize_coordinator_quarantines_locked();
					}
					if (!result)
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					if (!*result)
						return result->error();
					if (completed_predelegate)
					{
						auto released = completed_predelegate->release_activity();
						if (!released)
						{
							emergency_quarantine();
							return released.error();
						}
					}
					return **result;
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<
				sqlite_shm_reader_predecessor_unmap_terminal_result>
			complete_reader_predecessor_unmap(
				sqlite_shm_registry_family_pin& pin,
				const sqlite_shm_reader_open_authority& open,
				const sqlite_shm_verified_reader_predecessor_unmap_terminal_receipt&
					receipt) noexcept
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				try
				{
					std::optional<sqlite_shm_reader_attachment_authority> completed_activity;
					std::optional<sqlite_shm_lease_result<
						sqlite_shm_reader_predecessor_unmap_terminal_result>>
						result;
					{
						std::scoped_lock lock{mutex_};
						synchronize_activity_controls_locked();
						synchronize_reader_open_controls_locked();
						synchronize_coordinator_quarantines_locked();
						if (pin.state_.get() != this)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						auto* family_pin = current_family_pin_locked(pin);
						auto* reader_open = drainable_reader_open_locked(open);
						auto* alias = find_alias_locked(pin.alias_token_);
						auto* family = find_family_epoch_locked(pin.family_epoch_);
						if (family_pin == nullptr || reader_open == nullptr || alias == nullptr ||
							family == nullptr || !family->coordinator)
							return rejection(sqlite_shm_lease_rejection_reason::stale_token,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if (!exact_family_drain_visible_locked(*family))
							return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if (reader_open->alias_token != alias->token ||
							reader_open->family_epoch != family->entry_epoch ||
							reader_open->family_pin_token != family_pin->token ||
							reader_open->control->binding.family != family->binding ||
							reader_open->control->binding.alias_lifetime != alias->alias_lifetime)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if ((alias->phase != sqlite_shm_registry_alias_phase::registered &&
							 alias->phase != sqlite_shm_registry_alias_phase::unregistering &&
							 alias->phase != sqlite_shm_registry_alias_phase::quarantined) ||
							(family->phase != sqlite_shm_registry_family_phase::active &&
							 family->phase != sqlite_shm_registry_family_phase::quarantined))
							return rejection(sqlite_shm_lease_rejection_reason::quarantined,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);

						const auto binding = lease_reader_open_epoch_binding(
							reader_open->control->binding, alias->runtime_lifetime.pin_identity());
						result.emplace(
							family->coordinator->complete_registry_reader_predecessor_unmap(
								reader_open->token,
								reader_open->control->lineage_seal,
								binding,
								receipt,
								completed_activity));
						if ((!*result &&
							 (result->error().reason ==
								  sqlite_shm_lease_rejection_reason::lifecycle_ambiguous ||
							  result->error().reason ==
								  sqlite_shm_lease_rejection_reason::quarantined)) ||
							(*result &&
							 (*result)->kind() ==
								 sqlite_shm_reader_unmap_terminal_kind::terminal_quarantined))
							synchronize_coordinator_quarantines_locked();
					}
					if (!result)
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					if (!*result)
					{
						if (completed_activity)
							emergency_quarantine();
						return result->error();
					}
					const auto retired = (*result)->kind() ==
						sqlite_shm_reader_unmap_terminal_kind::retired_confirmed;
					if (retired != completed_activity.has_value())
					{
						emergency_quarantine();
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					}
					if (completed_activity)
					{
						auto released = completed_activity->release_activity();
						if (!released)
						{
							emergency_quarantine();
							return released.error();
						}
					}
					return **result;
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]]
			sqlite_shm_lease_result<sqlite_shm_reader_unpublished_cleanup_obligation>
			begin_reader_unpublished_cleanup(
				sqlite_shm_registry_family_pin& pin,
				sqlite_shm_reader_attachment_map_inflight& inflight,
				const sqlite_shm_verified_reader_unpublished_cleanup_receipt& receipt,
				sqlite_shm_reader_session& session)
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				try
				{
					std::optional<sqlite_shm_reader_map_predelegate_authority>
						completed_predelegate;
					std::optional<
						sqlite_shm_lease_result<sqlite_shm_reader_unpublished_cleanup_obligation>>
						result;
					{
						std::scoped_lock lock{mutex_};
						synchronize_activity_controls_locked();
						synchronize_reader_open_controls_locked();
						synchronize_coordinator_quarantines_locked();
						if (pin.state_.get() != this)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						auto* family_pin = current_family_pin_locked(pin);
						auto* alias = find_alias_locked(pin.alias_token_);
						auto* family = find_family_epoch_locked(pin.family_epoch_);
						const auto exact_qualified_owner =
							inflight.qualified_identity_owned_for_registry(
								pin.family_epoch_, pin.pin_token_, pin.alias_token_,
								seal_->process_epoch, activity_emergency_latch_);
						if (inflight.has_qualified_identity_for_registry() &&
							!exact_qualified_owner)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
								sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if (family_pin == nullptr || alias == nullptr || family == nullptr ||
							!family->coordinator ||
							(exact_qualified_owner && admission_quarantined_locked()))
							return rejection(sqlite_shm_lease_rejection_reason::stale_token,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if (!exact_family_drain_visible_locked(*family))
							return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						const auto& attachment = receipt.session_request().attachment;
						const auto late_close_drain = inflight.late_close_outer_unwind_armed();
						if (receipt.request().family != family->binding ||
							receipt.request().alias_lifetime != alias->alias_lifetime ||
							receipt.request().connection_token != attachment.connection_token() ||
							receipt.request().expected_attachment != attachment ||
							receipt.request().expected_attachment.runtime_lifetime_pin() !=
								alias->runtime_lifetime.pin_identity() ||
							!valid_identity(receipt.session_no_pointer_terminal_receipt()))
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						auto* reader_open =
							find_reader_open_locked(attachment.registry_open_token());
						if (reader_open == nullptr ||
							!reader_open->control ||
							!reader_open_control_matches_record_locked(*reader_open) ||
							reader_open->alias_token != alias->token ||
							reader_open->family_epoch != family->entry_epoch ||
							reader_open->family_pin_token != family_pin->token ||
							(!late_close_drain &&
							 (!reader_open->active ||
							  reader_open->control->phase.load(std::memory_order_acquire) !=
								  sqlite_shm_reader_open_phase::active)) ||
							!reader_open->control->lineage_seal ||
							(!late_close_drain &&
							 !reader_open->control->lineage_seal->authority_valid.load(
								 std::memory_order_acquire)) ||
							reader_open->control->binding.family != family->binding ||
							reader_open->control->binding.alias_lifetime != alias->alias_lifetime ||
							reader_open->control->binding.connection_token !=
								attachment.connection_token() ||
							reader_open->control->binding.main_native_file_receipt !=
								attachment.main_native_file_receipt() ||
							reader_open->control->binding.main_xopen_receipt !=
								attachment.main_xopen_receipt() ||
							reader_open->control->binding.open_epoch != attachment.open_epoch() ||
							reader_open->control->binding.callback_cohort !=
								attachment.callback_cohort())
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if ((alias->phase != sqlite_shm_registry_alias_phase::registered &&
							 alias->phase != sqlite_shm_registry_alias_phase::unregistering &&
							 alias->phase != sqlite_shm_registry_alias_phase::quarantined) ||
							(family->phase != sqlite_shm_registry_family_phase::active &&
							 family->phase != sqlite_shm_registry_family_phase::quarantined))
							return rejection(sqlite_shm_lease_rejection_reason::quarantined,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);

						// This is the terminal selection for an already-native-started first map.
						// It must remain drainable after ordinary peer quarantine and must transfer
						// the predelegate owner before the registry mutex is released.
						result.emplace(
							family->coordinator->begin_registry_reader_unpublished_cleanup(
								pin, inflight, receipt, session, completed_predelegate));
						if (!*result &&
							(result->error().reason ==
								 sqlite_shm_lease_rejection_reason::lifecycle_ambiguous ||
							 result->error().reason ==
								 sqlite_shm_lease_rejection_reason::quarantined))
							synchronize_coordinator_quarantines_locked();
					}
					if (!result)
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					if (!*result)
					{
						if (completed_predelegate)
							emergency_quarantine();
						return result->error();
					}
					if (!completed_predelegate && !(*result)->is_late_close_drain())
					{
						emergency_quarantine();
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					}
					if (completed_predelegate)
					{
						auto released = completed_predelegate->release_activity();
						if (!released)
						{
							emergency_quarantine();
							return released.error();
						}
					}
					return std::move(**result);
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]]
			sqlite_shm_lease_result<sqlite_shm_reader_unpublished_cleanup_terminal_result>
			complete_reader_unpublished_cleanup(
				sqlite_shm_registry_family_pin& pin,
				sqlite_shm_reader_unpublished_cleanup_obligation& cleanup,
				const sqlite_shm_verified_reader_unpublished_cleanup_terminal_receipt&
					receipt) noexcept
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					synchronize_reader_open_controls_locked();
					synchronize_coordinator_quarantines_locked();
					if (pin.state_.get() != this)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					auto* family_pin = current_family_pin_locked(pin);
					auto* alias = find_alias_locked(pin.alias_token_);
					auto* family = find_family_epoch_locked(pin.family_epoch_);
					if (family_pin == nullptr || alias == nullptr || family == nullptr ||
						!family->coordinator)
						return rejection(sqlite_shm_lease_rejection_reason::stale_token,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					if (!exact_family_drain_visible_locked(*family))
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					if ((alias->phase != sqlite_shm_registry_alias_phase::registered &&
						 alias->phase != sqlite_shm_registry_alias_phase::unregistering &&
						 alias->phase != sqlite_shm_registry_alias_phase::quarantined) ||
						(family->phase != sqlite_shm_registry_family_phase::active &&
						 family->phase != sqlite_shm_registry_family_phase::quarantined))
						return rejection(sqlite_shm_lease_rejection_reason::quarantined,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);

					auto completed =
						family->coordinator->complete_registry_reader_unpublished_cleanup(cleanup,
																						  receipt);
					if ((!completed &&
						 (completed.error().reason ==
							  sqlite_shm_lease_rejection_reason::lifecycle_ambiguous ||
						  completed.error().reason ==
							  sqlite_shm_lease_rejection_reason::quarantined)) ||
						(completed &&
						 completed->kind() ==
							 sqlite_shm_reader_unpublished_cleanup_terminal_kind::
								 terminal_quarantined))
						synchronize_coordinator_quarantines_locked();
					return completed;
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_logical_ack_result>
			consume_reader_logical_ack(
				sqlite_shm_registry_family_pin& pin,
				const sqlite_shm_reader_open_authority& open,
				const sqlite_shm_reader_logical_ack_request& request) noexcept
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				try
				{
					std::optional<sqlite_shm_reader_attachment_authority> completed_activity;
					std::optional<sqlite_shm_lease_result<sqlite_shm_reader_logical_ack_result>>
						result;
					{
						std::scoped_lock lock{mutex_};
						synchronize_activity_controls_locked();
						synchronize_reader_open_controls_locked();
						synchronize_coordinator_quarantines_locked();
						if (pin.state_.get() != this)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						auto* family_pin = current_family_pin_locked(pin);
						auto* reader_open = drainable_reader_open_locked(open);
						auto* alias = find_alias_locked(pin.alias_token_);
						auto* family = find_family_epoch_locked(pin.family_epoch_);
						if (family_pin == nullptr || reader_open == nullptr || alias == nullptr ||
							family == nullptr || !family->coordinator)
							return rejection(sqlite_shm_lease_rejection_reason::stale_token,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if (!exact_family_drain_visible_locked(*family))
							return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if (reader_open->alias_token != alias->token ||
							reader_open->family_epoch != family->entry_epoch ||
							reader_open->family_pin_token != family_pin->token ||
							reader_open->control->binding.family != family->binding ||
							reader_open->control->binding.alias_lifetime != alias->alias_lifetime)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if ((alias->phase != sqlite_shm_registry_alias_phase::registered &&
							 alias->phase != sqlite_shm_registry_alias_phase::unregistering &&
							 alias->phase != sqlite_shm_registry_alias_phase::quarantined) ||
							(family->phase != sqlite_shm_registry_family_phase::active &&
							 family->phase != sqlite_shm_registry_family_phase::quarantined))
							return rejection(sqlite_shm_lease_rejection_reason::quarantined,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);

						const auto binding = lease_reader_open_epoch_binding(
							reader_open->control->binding, alias->runtime_lifetime.pin_identity());
						result.emplace(family->coordinator->consume_registry_reader_logical_ack(
							reader_open->token,
							reader_open->control->lineage_seal,
							binding,
							request,
							completed_activity));
						if (!*result &&
							(result->error().reason ==
								 sqlite_shm_lease_rejection_reason::lifecycle_ambiguous ||
							 result->error().reason ==
								 sqlite_shm_lease_rejection_reason::quarantined))
							synchronize_coordinator_quarantines_locked();
					}
					if (!result)
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					if (!*result)
					{
						if (completed_activity)
							emergency_quarantine();
						return result->error();
					}
					if (!completed_activity)
					{
						emergency_quarantine();
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					}
					auto released = completed_activity->release_activity();
					if (!released)
					{
						emergency_quarantine();
						return released.error();
					}
					return **result;
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_late_close_logical_ack_result>
			consume_reader_late_close_logical_ack(
				sqlite_shm_registry_family_pin& pin,
				sqlite_shm_reader_late_close_outer_unwind_authority& owner,
				const sqlite_shm_reader_logical_ack_request& request) noexcept
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
								 sqlite_shm_lease_recovery_action::outer_ioerr_no_retry);
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					synchronize_reader_open_controls_locked();
					synchronize_coordinator_quarantines_locked();
					if (pin.state_.get() != this)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									 sqlite_shm_lease_recovery_action::outer_ioerr_no_retry);
					auto* family_pin = current_family_pin_locked(pin);
					auto* alias = find_alias_locked(pin.alias_token_);
					auto* family = find_family_epoch_locked(pin.family_epoch_);
					if (family_pin == nullptr || alias == nullptr || family == nullptr ||
						!family->coordinator || !exact_family_drain_visible_locked(*family))
						return rejection(sqlite_shm_lease_rejection_reason::stale_token,
									 sqlite_shm_lease_recovery_action::outer_ioerr_no_retry);
					if ((alias->phase != sqlite_shm_registry_alias_phase::registered &&
						 alias->phase != sqlite_shm_registry_alias_phase::unregistering &&
						 alias->phase != sqlite_shm_registry_alias_phase::quarantined) ||
						(family->phase != sqlite_shm_registry_family_phase::active &&
						 family->phase != sqlite_shm_registry_family_phase::quarantined))
						return rejection(sqlite_shm_lease_rejection_reason::quarantined,
									 sqlite_shm_lease_recovery_action::outer_ioerr_no_retry);
					return family->coordinator->consume_registry_reader_late_close_logical_ack(
						pin, owner, request);
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								 sqlite_shm_lease_recovery_action::outer_ioerr_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<void> complete_reader_session(
				sqlite_shm_registry_family_pin& pin,
				sqlite_shm_reader_session& session,
				const sqlite_shm_reader_session_terminal_receipt& receipt) noexcept
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				try
				{
					std::optional<sqlite_shm_reader_attachment_authority> completed_activity;
					std::optional<sqlite_shm_lease_result<void>> result;
					{
						std::scoped_lock lock{mutex_};
						synchronize_activity_controls_locked();
						synchronize_reader_open_controls_locked();
						synchronize_coordinator_quarantines_locked();
						if (pin.state_.get() != this)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						auto* family_pin = current_family_pin_locked(pin);
						auto* alias = find_alias_locked(pin.alias_token_);
						auto* family = find_family_epoch_locked(pin.family_epoch_);
						if (family_pin == nullptr || alias == nullptr || family == nullptr ||
							!family->coordinator)
							return rejection(sqlite_shm_lease_rejection_reason::stale_token,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if ((alias->phase != sqlite_shm_registry_alias_phase::registered &&
							 alias->phase != sqlite_shm_registry_alias_phase::unregistering &&
							 alias->phase != sqlite_shm_registry_alias_phase::quarantined) ||
							(family->phase != sqlite_shm_registry_family_phase::active &&
							 family->phase != sqlite_shm_registry_family_phase::quarantined))
						{
							quarantine_registry_locked();
							return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						}

						// Session terminal is an already-owned drain. A deterministic peer
						// quarantine revokes fresh admission but must not strand this owner.
						result.emplace(family->coordinator->complete_registry_reader_session(
							session, receipt, completed_activity));
						if (!*result &&
							(result->error().reason ==
								 sqlite_shm_lease_rejection_reason::lifecycle_ambiguous ||
							 result->error().reason ==
								 sqlite_shm_lease_rejection_reason::quarantined))
							synchronize_coordinator_quarantines_locked();
					}
					if (!result)
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					if (!*result)
					{
						if (completed_activity)
							emergency_quarantine();
						return result->error();
					}
					if (completed_activity)
					{
						auto released = completed_activity->release_activity();
						if (!released)
						{
							emergency_quarantine();
							return released.error();
						}
					}
					return {};
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_unmap_obligation>
			begin_reader_unmap(sqlite_shm_registry_family_pin& pin,
							   sqlite_shm_reader_handoff& handoff,
							   const sqlite_shm_callback_execution_receipt& callback) noexcept
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					synchronize_reader_open_controls_locked();
					synchronize_coordinator_quarantines_locked();
					if (pin.state_.get() != this)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					auto* family_pin = current_family_pin_locked(pin);
					auto* alias = find_alias_locked(pin.alias_token_);
					auto* family = find_family_epoch_locked(pin.family_epoch_);
					if (family_pin == nullptr || alias == nullptr || family == nullptr ||
						!family->coordinator)
						return rejection(sqlite_shm_lease_rejection_reason::stale_token,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					if ((alias->phase != sqlite_shm_registry_alias_phase::registered &&
						 alias->phase != sqlite_shm_registry_alias_phase::unregistering &&
						 alias->phase != sqlite_shm_registry_alias_phase::quarantined) ||
						(family->phase != sqlite_shm_registry_family_phase::active &&
						 family->phase != sqlite_shm_registry_family_phase::quarantined))
					{
						quarantine_registry_locked();
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					}

					auto begun =
						family->coordinator->begin_registry_reader_unmap(handoff, callback);
					if (!begun &&
						(begun.error().reason ==
							 sqlite_shm_lease_rejection_reason::lifecycle_ambiguous ||
						 begun.error().reason == sqlite_shm_lease_rejection_reason::quarantined))
						synchronize_coordinator_quarantines_locked();
					return begun;
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_unmap_cut_result>
			poll_reader_unmap_cut(sqlite_shm_registry_family_pin& pin,
								  sqlite_shm_reader_unmap_obligation& unmap,
								  const sqlite_shm_callback_execution_receipt& callback) noexcept
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					synchronize_reader_open_controls_locked();
					synchronize_coordinator_quarantines_locked();
					if (pin.state_.get() != this)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					auto* family_pin = current_family_pin_locked(pin);
					auto* alias = find_alias_locked(pin.alias_token_);
					auto* family = find_family_epoch_locked(pin.family_epoch_);
					if (family_pin == nullptr || alias == nullptr || family == nullptr ||
						!family->coordinator)
						return rejection(sqlite_shm_lease_rejection_reason::stale_token,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					auto result =
						family->coordinator->poll_registry_reader_unmap_cut(unmap, callback);
					if (!result)
						synchronize_coordinator_quarantines_locked();
					return result;
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<void>
			fail_reader_unmap_cut_wait(sqlite_shm_registry_family_pin& pin,
									   sqlite_shm_reader_unmap_obligation& unmap,
									   const sqlite_shm_callback_execution_receipt& callback,
									   const sqlite_shm_retirement_wait_failure failure) noexcept
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					synchronize_reader_open_controls_locked();
					synchronize_coordinator_quarantines_locked();
					if (pin.state_.get() != this)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					auto* family_pin = current_family_pin_locked(pin);
					auto* alias = find_alias_locked(pin.alias_token_);
					auto* family = find_family_epoch_locked(pin.family_epoch_);
					if (family_pin == nullptr || alias == nullptr || family == nullptr ||
						!family->coordinator)
						return rejection(sqlite_shm_lease_rejection_reason::stale_token,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					auto result = family->coordinator->fail_registry_reader_unmap_cut_wait(
						unmap, callback, failure);
					synchronize_coordinator_quarantines_locked();
					return result;
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_unmap_terminal_result>
			complete_reader_unmap(
				sqlite_shm_registry_family_pin& pin,
				sqlite_shm_reader_unmap_obligation& unmap,
				const sqlite_shm_verified_reader_unmap_terminal_receipt& receipt) noexcept
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				try
				{
					std::optional<sqlite_shm_reader_attachment_authority> completed_activity;
					std::optional<sqlite_shm_lease_result<sqlite_shm_reader_unmap_terminal_result>>
						result;
					{
						std::scoped_lock lock{mutex_};
						synchronize_activity_controls_locked();
						synchronize_reader_open_controls_locked();
						synchronize_coordinator_quarantines_locked();
						if (pin.state_.get() != this)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						auto* family_pin = current_family_pin_locked(pin);
						auto* alias = find_alias_locked(pin.alias_token_);
						auto* family = find_family_epoch_locked(pin.family_epoch_);
						if (family_pin == nullptr || alias == nullptr || family == nullptr ||
							!family->coordinator)
							return rejection(sqlite_shm_lease_rejection_reason::stale_token,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if ((alias->phase != sqlite_shm_registry_alias_phase::registered &&
							 alias->phase != sqlite_shm_registry_alias_phase::unregistering &&
							 alias->phase != sqlite_shm_registry_alias_phase::quarantined) ||
							(family->phase != sqlite_shm_registry_family_phase::active &&
							 family->phase != sqlite_shm_registry_family_phase::quarantined))
						{
							quarantine_registry_locked();
							return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						}

						result.emplace(family->coordinator->complete_registry_reader_unmap(
							unmap, receipt, completed_activity));
						if (!*result &&
							(result->error().reason ==
								 sqlite_shm_lease_rejection_reason::lifecycle_ambiguous ||
							 result->error().reason ==
								 sqlite_shm_lease_rejection_reason::quarantined))
							synchronize_coordinator_quarantines_locked();
						else if (*result &&
								 (*result)->kind() ==
									 sqlite_shm_reader_unmap_terminal_kind::terminal_quarantined)
							synchronize_coordinator_quarantines_locked();
					}
					if (!result)
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					if (!*result)
					{
						if (completed_activity)
							emergency_quarantine();
						return result->error();
					}
					const auto retired = (*result)->kind() ==
						sqlite_shm_reader_unmap_terminal_kind::retired_confirmed;
					if (retired != completed_activity.has_value())
					{
						emergency_quarantine();
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					}
					if (completed_activity)
					{
						auto released = completed_activity->release_activity();
						if (!released)
						{
							emergency_quarantine();
							return released.error();
						}
					}
					return std::move(**result);
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_live_close_obligation>
			begin_reader_live_close(sqlite_shm_registry_family_pin& pin,
									const sqlite_shm_reader_open_authority& open,
									sqlite_shm_reader_handoff& handoff,
									const sqlite_shm_reader_unmap_request& unmap_request,
									const sqlite_shm_reader_close_request& close_request) noexcept
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				try
				{
					std::optional<sqlite_shm_lease_result<sqlite_shm_reader_live_close_obligation>>
						result;
					{
						std::scoped_lock lock{mutex_};
						synchronize_activity_controls_locked();
						synchronize_reader_open_controls_locked();
						synchronize_coordinator_quarantines_locked();
						if (pin.state_.get() != this)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if (admission_quarantined_locked())
							return rejection(sqlite_shm_lease_rejection_reason::quarantined,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						auto* family_pin = current_family_pin_locked(pin);
						auto* reader_open = drainable_reader_open_locked(open);
						auto* alias = find_alias_locked(pin.alias_token_);
						auto* family = find_family_epoch_locked(pin.family_epoch_);
						if (family_pin == nullptr || reader_open == nullptr || alias == nullptr ||
							family == nullptr || !family->coordinator)
							return rejection(sqlite_shm_lease_rejection_reason::stale_token,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if (!exact_family_drain_visible_locked(*family))
							return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if (reader_open->alias_token != alias->token ||
							reader_open->family_epoch != family->entry_epoch ||
							reader_open->family_pin_token != family_pin->token ||
							reader_open->control->binding.family != family->binding ||
							reader_open->control->binding.alias_lifetime != alias->alias_lifetime)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if ((alias->phase != sqlite_shm_registry_alias_phase::registered &&
							 alias->phase != sqlite_shm_registry_alias_phase::unregistering &&
							 alias->phase != sqlite_shm_registry_alias_phase::quarantined) ||
							(family->phase != sqlite_shm_registry_family_phase::active &&
							 family->phase != sqlite_shm_registry_family_phase::quarantined))
							return rejection(sqlite_shm_lease_rejection_reason::quarantined,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						const auto binding = lease_reader_open_epoch_binding(
							reader_open->control->binding, alias->runtime_lifetime.pin_identity());
						result.emplace(family->coordinator->begin_registry_reader_live_close(
							reader_open->token,
							reader_open->control->lineage_seal,
							binding,
							handoff,
							unmap_request,
							close_request));
						if (!*result &&
							(result->error().reason ==
								 sqlite_shm_lease_rejection_reason::lifecycle_ambiguous ||
							 result->error().reason ==
								 sqlite_shm_lease_rejection_reason::quarantined))
							synchronize_coordinator_quarantines_locked();
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

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_unmap_cut_result>
			poll_reader_live_close_unmap_cut(
				sqlite_shm_registry_family_pin& pin,
				const sqlite_shm_reader_open_authority& open,
				sqlite_shm_reader_live_close_obligation& close,
				const sqlite_shm_callback_execution_receipt& close_callback) noexcept
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					synchronize_reader_open_controls_locked();
					synchronize_coordinator_quarantines_locked();
					if (pin.state_.get() != this)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					auto* family_pin = current_family_pin_locked(pin);
					auto* reader_open = drainable_reader_open_locked(open);
					auto* alias = find_alias_locked(pin.alias_token_);
					auto* family = find_family_epoch_locked(pin.family_epoch_);
					if (family_pin == nullptr || reader_open == nullptr || alias == nullptr ||
						family == nullptr || !family->coordinator)
						return rejection(sqlite_shm_lease_rejection_reason::stale_token,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					if (!exact_family_drain_visible_locked(*family))
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					if (reader_open->alias_token != alias->token ||
						reader_open->family_epoch != family->entry_epoch ||
						reader_open->family_pin_token != family_pin->token ||
						reader_open->control->binding.family != family->binding ||
						reader_open->control->binding.alias_lifetime != alias->alias_lifetime)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					const auto binding = lease_reader_open_epoch_binding(
						reader_open->control->binding, alias->runtime_lifetime.pin_identity());
					auto result = family->coordinator->poll_registry_reader_live_close_unmap_cut(
						reader_open->token,
						reader_open->control->lineage_seal,
						binding,
						close,
						close_callback);
					if (!result)
						synchronize_coordinator_quarantines_locked();
					return result;
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<void> fail_reader_live_close_unmap_cut_wait(
				sqlite_shm_registry_family_pin& pin,
				const sqlite_shm_reader_open_authority& open,
				sqlite_shm_reader_live_close_obligation& close,
				const sqlite_shm_callback_execution_receipt& close_callback,
				const sqlite_shm_retirement_wait_failure failure) noexcept
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					synchronize_reader_open_controls_locked();
					synchronize_coordinator_quarantines_locked();
					if (pin.state_.get() != this)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					auto* family_pin = current_family_pin_locked(pin);
					auto* reader_open = drainable_reader_open_locked(open);
					auto* alias = find_alias_locked(pin.alias_token_);
					auto* family = find_family_epoch_locked(pin.family_epoch_);
					if (family_pin == nullptr || reader_open == nullptr || alias == nullptr ||
						family == nullptr || !family->coordinator)
						return rejection(sqlite_shm_lease_rejection_reason::stale_token,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					if (!exact_family_drain_visible_locked(*family))
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					if (reader_open->alias_token != alias->token ||
						reader_open->family_epoch != family->entry_epoch ||
						reader_open->family_pin_token != family_pin->token ||
						reader_open->control->binding.family != family->binding ||
						reader_open->control->binding.alias_lifetime != alias->alias_lifetime)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					const auto binding = lease_reader_open_epoch_binding(
						reader_open->control->binding, alias->runtime_lifetime.pin_identity());
					auto result =
						family->coordinator->fail_registry_reader_live_close_unmap_cut_wait(
							reader_open->token,
							reader_open->control->lineage_seal,
							binding,
							close,
							close_callback,
							failure);
					synchronize_coordinator_quarantines_locked();
					return result;
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_close_obligation>
			begin_reader_close(sqlite_shm_registry_family_pin& pin,
							   const sqlite_shm_reader_open_authority& open,
							   const sqlite_shm_reader_close_request& request) noexcept
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				try
				{
					std::optional<sqlite_shm_reader_attachment_authority> completed_activity;
					std::optional<sqlite_shm_lease_result<sqlite_shm_reader_close_obligation>>
						result;
					{
						std::scoped_lock lock{mutex_};
						synchronize_activity_controls_locked();
						synchronize_reader_open_controls_locked();
						synchronize_coordinator_quarantines_locked();
						if (pin.state_.get() != this)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if (admission_quarantined_locked())
							return rejection(sqlite_shm_lease_rejection_reason::quarantined,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						auto* family_pin = current_family_pin_locked(pin);
						auto* reader_open = drainable_reader_open_locked(open);
						auto* alias = find_alias_locked(pin.alias_token_);
						auto* family = find_family_epoch_locked(pin.family_epoch_);
						if (family_pin == nullptr || reader_open == nullptr || alias == nullptr ||
							family == nullptr || !family->coordinator)
							return rejection(sqlite_shm_lease_rejection_reason::stale_token,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if (!exact_family_drain_visible_locked(*family))
							return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if (reader_open->alias_token != alias->token ||
							reader_open->family_epoch != family->entry_epoch ||
							reader_open->family_pin_token != family_pin->token ||
							reader_open->control->binding.family != family->binding ||
							reader_open->control->binding.alias_lifetime != alias->alias_lifetime)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if ((alias->phase != sqlite_shm_registry_alias_phase::registered &&
							 alias->phase != sqlite_shm_registry_alias_phase::unregistering &&
							 alias->phase != sqlite_shm_registry_alias_phase::quarantined) ||
							(family->phase != sqlite_shm_registry_family_phase::active &&
							 family->phase != sqlite_shm_registry_family_phase::quarantined))
							return rejection(sqlite_shm_lease_rejection_reason::quarantined,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);

						// Close is an already-minted open-epoch drain. Ordinary peer quarantine
						// revokes fresh admission through the alias/family latches, but does not
						// reconstruct or strand this exact move-only connection-close owner. A
						// pending logical acknowledgement is consumed in the same lease cut.
						const auto binding = lease_reader_open_epoch_binding(
							reader_open->control->binding, alias->runtime_lifetime.pin_identity());
						result.emplace(family->coordinator->begin_registry_reader_close(
							reader_open->token,
							reader_open->control->lineage_seal,
							binding,
							request,
							completed_activity));
						if (!*result &&
							(result->error().reason ==
								 sqlite_shm_lease_rejection_reason::lifecycle_ambiguous ||
							 result->error().reason ==
								 sqlite_shm_lease_rejection_reason::quarantined))
							synchronize_coordinator_quarantines_locked();
					}
					if (!result)
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					if (!*result)
					{
						if (completed_activity)
							emergency_quarantine();
						return result->error();
					}
					if (completed_activity)
					{
						auto released = completed_activity->release_activity();
						if (!released)
						{
							emergency_quarantine();
							return released.error();
						}
					}
					return std::move(**result);
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_close_cut_result>
			poll_reader_close_cut(
				sqlite_shm_registry_family_pin& pin,
				const sqlite_shm_reader_open_authority& open,
				sqlite_shm_reader_close_obligation& close,
				const sqlite_shm_callback_execution_receipt& close_callback) noexcept
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				try
				{
					std::optional<sqlite_shm_reader_attachment_authority> completed_activity;
					std::optional<sqlite_shm_lease_result<sqlite_shm_reader_close_cut_result>>
						result;
					{
						std::scoped_lock lock{mutex_};
						synchronize_activity_controls_locked();
						synchronize_reader_open_controls_locked();
						synchronize_coordinator_quarantines_locked();
						if (pin.state_.get() != this)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						auto* family_pin = current_family_pin_locked(pin);
						auto* reader_open = drainable_reader_open_locked(open);
						auto* alias = find_alias_locked(pin.alias_token_);
						auto* family = find_family_epoch_locked(pin.family_epoch_);
						if (family_pin == nullptr || reader_open == nullptr || alias == nullptr ||
							family == nullptr || !family->coordinator)
							return rejection(sqlite_shm_lease_rejection_reason::stale_token,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if (!exact_family_drain_visible_locked(*family))
							return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if (reader_open->alias_token != alias->token ||
							reader_open->family_epoch != family->entry_epoch ||
							reader_open->family_pin_token != family_pin->token ||
							reader_open->control->binding.family != family->binding ||
							reader_open->control->binding.alias_lifetime != alias->alias_lifetime)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						const auto binding = lease_reader_open_epoch_binding(
							reader_open->control->binding, alias->runtime_lifetime.pin_identity());
						result.emplace(family->coordinator->poll_registry_reader_close_cut(
							reader_open->token,
							reader_open->control->lineage_seal,
							binding,
							close,
							close_callback,
							completed_activity));
						if (!*result &&
							(result->error().reason ==
								 sqlite_shm_lease_rejection_reason::lifecycle_ambiguous ||
							 result->error().reason ==
								 sqlite_shm_lease_rejection_reason::quarantined))
							synchronize_coordinator_quarantines_locked();
					}
					if (!result)
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					if (!*result)
					{
						if (completed_activity)
							emergency_quarantine();
						return result->error();
					}
					const auto consumed_cleanup_activity = (*result)->progress ==
							sqlite_shm_reader_close_cut_progress::native_effect_ready &&
						(*result)->route ==
							sqlite_shm_reader_close_route::close_after_confirmed_unmap;
					if (consumed_cleanup_activity != completed_activity.has_value())
					{
						emergency_quarantine();
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					}
					if (completed_activity)
					{
						auto released = completed_activity->release_activity();
						if (!released)
						{
							emergency_quarantine();
							return released.error();
						}
					}
					return std::move(**result);
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<void>
			fail_reader_close_cut_wait(sqlite_shm_registry_family_pin& pin,
									   const sqlite_shm_reader_open_authority& open,
									   sqlite_shm_reader_close_obligation& close,
									   const sqlite_shm_callback_execution_receipt& close_callback,
									   const sqlite_shm_retirement_wait_failure failure) noexcept
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					synchronize_reader_open_controls_locked();
					synchronize_coordinator_quarantines_locked();
					if (pin.state_.get() != this)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					auto* family_pin = current_family_pin_locked(pin);
					auto* reader_open = drainable_reader_open_locked(open);
					auto* alias = find_alias_locked(pin.alias_token_);
					auto* family = find_family_epoch_locked(pin.family_epoch_);
					if (family_pin == nullptr || reader_open == nullptr || alias == nullptr ||
						family == nullptr || !family->coordinator)
						return rejection(sqlite_shm_lease_rejection_reason::stale_token,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					if (!exact_family_drain_visible_locked(*family))
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					if (reader_open->alias_token != alias->token ||
						reader_open->family_epoch != family->entry_epoch ||
						reader_open->family_pin_token != family_pin->token ||
						reader_open->control->binding.family != family->binding ||
						reader_open->control->binding.alias_lifetime != alias->alias_lifetime)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					const auto binding = lease_reader_open_epoch_binding(
						reader_open->control->binding, alias->runtime_lifetime.pin_identity());
					auto result = family->coordinator->fail_registry_reader_close_cut_wait(
						reader_open->token,
						reader_open->control->lineage_seal,
						binding,
						close,
						close_callback,
						failure);
					synchronize_coordinator_quarantines_locked();
					return result;
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_close_terminal_result>
			complete_reader_close(
				sqlite_shm_registry_family_pin& pin,
				const sqlite_shm_reader_open_authority& open,
				sqlite_shm_reader_close_obligation& close,
				const sqlite_shm_verified_reader_close_terminal_receipt& receipt) noexcept
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				try
				{
					std::optional<sqlite_shm_reader_attachment_authority> completed_activity;
					std::optional<sqlite_shm_lease_result<sqlite_shm_reader_close_terminal_result>>
						result;
					{
						std::scoped_lock lock{mutex_};
						synchronize_activity_controls_locked();
						synchronize_reader_open_controls_locked();
						synchronize_coordinator_quarantines_locked();
						if (pin.state_.get() != this)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if (admission_quarantined_locked())
							return rejection(sqlite_shm_lease_rejection_reason::quarantined,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						auto* family_pin = current_family_pin_locked(pin);
						auto* reader_open = drainable_reader_open_locked(open);
						auto* alias = find_alias_locked(pin.alias_token_);
						auto* family = find_family_epoch_locked(pin.family_epoch_);
						if (family_pin == nullptr || reader_open == nullptr || alias == nullptr ||
							family == nullptr || !family->coordinator)
							return rejection(sqlite_shm_lease_rejection_reason::stale_token,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if (!exact_family_drain_visible_locked(*family))
							return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if (reader_open->alias_token != alias->token ||
							reader_open->family_epoch != family->entry_epoch ||
							reader_open->family_pin_token != family_pin->token ||
							reader_open->control->binding.family != family->binding ||
							reader_open->control->binding.alias_lifetime != alias->alias_lifetime)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if ((alias->phase != sqlite_shm_registry_alias_phase::registered &&
							 alias->phase != sqlite_shm_registry_alias_phase::unregistering &&
							 alias->phase != sqlite_shm_registry_alias_phase::quarantined) ||
							(family->phase != sqlite_shm_registry_family_phase::active &&
							 family->phase != sqlite_shm_registry_family_phase::quarantined))
							return rejection(sqlite_shm_lease_rejection_reason::quarantined,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);

						const auto binding = lease_reader_open_epoch_binding(
							reader_open->control->binding, alias->runtime_lifetime.pin_identity());
						result.emplace(family->coordinator->complete_registry_reader_close(
							reader_open->token,
							reader_open->control->lineage_seal,
							binding,
							close,
							receipt,
							completed_activity));
						if ((!*result &&
							 (result->error().reason ==
								  sqlite_shm_lease_rejection_reason::lifecycle_ambiguous ||
							  result->error().reason ==
								  sqlite_shm_lease_rejection_reason::quarantined)) ||
							(*result &&
							 (*result)->kind() ==
								 sqlite_shm_reader_close_terminal_kind::terminal_quarantined))
							synchronize_coordinator_quarantines_locked();
					}
					if (!result)
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					if (!*result)
					{
						if (completed_activity)
							emergency_quarantine();
						return result->error();
					}
					const auto retired_predecessor =
						(*result)->kind() == sqlite_shm_reader_close_terminal_kind::closed &&
						(*result)->route() ==
							sqlite_shm_reader_close_route::close_existing_predecessor;
					if (retired_predecessor != completed_activity.has_value())
					{
						emergency_quarantine();
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					}
					if (completed_activity)
					{
						auto released = completed_activity->release_activity();
						if (!released)
						{
							emergency_quarantine();
							return released.error();
						}
					}
					return std::move(**result);
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_unmap_terminal_result>
			complete_reader_live_close_unmap(
				sqlite_shm_registry_family_pin& pin,
				const sqlite_shm_reader_open_authority& open,
				sqlite_shm_reader_live_close_obligation& close,
				const sqlite_shm_verified_reader_unmap_terminal_receipt& receipt) noexcept
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				try
				{
					std::optional<sqlite_shm_reader_attachment_authority> completed_activity;
					std::optional<sqlite_shm_lease_result<sqlite_shm_reader_unmap_terminal_result>>
						result;
					{
						std::scoped_lock lock{mutex_};
						synchronize_activity_controls_locked();
						synchronize_reader_open_controls_locked();
						synchronize_coordinator_quarantines_locked();
						if (pin.state_.get() != this)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						auto* family_pin = current_family_pin_locked(pin);
						auto* reader_open = drainable_reader_open_locked(open);
						auto* alias = find_alias_locked(pin.alias_token_);
						auto* family = find_family_epoch_locked(pin.family_epoch_);
						if (family_pin == nullptr || reader_open == nullptr || alias == nullptr ||
							family == nullptr || !family->coordinator)
							return rejection(sqlite_shm_lease_rejection_reason::stale_token,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if (!exact_family_drain_visible_locked(*family))
							return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if (reader_open->alias_token != alias->token ||
							reader_open->family_epoch != family->entry_epoch ||
							reader_open->family_pin_token != family_pin->token ||
							reader_open->control->binding.family != family->binding ||
							reader_open->control->binding.alias_lifetime != alias->alias_lifetime)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						const auto binding = lease_reader_open_epoch_binding(
							reader_open->control->binding, alias->runtime_lifetime.pin_identity());
						result.emplace(
							family->coordinator->complete_registry_reader_live_close_unmap(
								reader_open->token,
								reader_open->control->lineage_seal,
								binding,
								close,
								receipt,
								completed_activity));
						if ((!*result &&
							 (result->error().reason ==
								  sqlite_shm_lease_rejection_reason::lifecycle_ambiguous ||
							  result->error().reason ==
								  sqlite_shm_lease_rejection_reason::quarantined)) ||
							(*result &&
							 (*result)->kind() ==
								 sqlite_shm_reader_unmap_terminal_kind::terminal_quarantined))
							synchronize_coordinator_quarantines_locked();
					}
					if (!result)
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					if (!*result)
					{
						if (completed_activity)
							emergency_quarantine();
						return result->error();
					}
					const auto retired = (*result)->kind() ==
						sqlite_shm_reader_unmap_terminal_kind::retired_confirmed;
					if (retired != completed_activity.has_value())
					{
						emergency_quarantine();
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					}
					if (completed_activity)
					{
						auto released = completed_activity->release_activity();
						if (!released)
						{
							emergency_quarantine();
							return released.error();
						}
					}
					return std::move(**result);
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_close_terminal_result>
			complete_reader_live_close(
				sqlite_shm_registry_family_pin& pin,
				const sqlite_shm_reader_open_authority& open,
				sqlite_shm_reader_live_close_obligation& close,
				const sqlite_shm_verified_reader_close_terminal_receipt& receipt) noexcept
			{
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				try
				{
					std::optional<sqlite_shm_lease_result<sqlite_shm_reader_close_terminal_result>>
						result;
					{
						std::scoped_lock lock{mutex_};
						synchronize_activity_controls_locked();
						synchronize_reader_open_controls_locked();
						synchronize_coordinator_quarantines_locked();
						if (pin.state_.get() != this)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						auto* family_pin = current_family_pin_locked(pin);
						auto* reader_open = drainable_reader_open_locked(open);
						auto* alias = find_alias_locked(pin.alias_token_);
						auto* family = find_family_epoch_locked(pin.family_epoch_);
						if (family_pin == nullptr || reader_open == nullptr || alias == nullptr ||
							family == nullptr || !family->coordinator)
							return rejection(sqlite_shm_lease_rejection_reason::stale_token,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if (!exact_family_drain_visible_locked(*family))
							return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						if (reader_open->alias_token != alias->token ||
							reader_open->family_epoch != family->entry_epoch ||
							reader_open->family_pin_token != family_pin->token ||
							reader_open->control->binding.family != family->binding ||
							reader_open->control->binding.alias_lifetime != alias->alias_lifetime)
							return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						const auto binding = lease_reader_open_epoch_binding(
							reader_open->control->binding, alias->runtime_lifetime.pin_identity());
						result.emplace(family->coordinator->complete_registry_reader_live_close(
							reader_open->token,
							reader_open->control->lineage_seal,
							binding,
							close,
							receipt));
						if ((!*result &&
							 (result->error().reason ==
								  sqlite_shm_lease_rejection_reason::lifecycle_ambiguous ||
							  result->error().reason ==
								  sqlite_shm_lease_rejection_reason::quarantined)) ||
							(*result &&
							 (*result)->kind() ==
								 sqlite_shm_reader_close_terminal_kind::terminal_quarantined))
							synchronize_coordinator_quarantines_locked();
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

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_pending_mapping>
			install_writer_pending(sqlite_shm_registry_family_pin& pin,
								   sqlite_shm_writer_post_native_mapping& post_native,
								   const sqlite_shm_verified_writer_post_map_receipt& receipt)
			{
				const auto cleanup_action =
					sqlite_shm_lease_recovery_action::attempt_nonremoving_unmap_then_outer_ioerr;
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
									 cleanup_action);

				try
				{
					// All fallible receipt copying occurs before either mutex is acquired.
					auto prepared = receipt;
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					synchronize_reader_open_controls_locked();
					synchronize_coordinator_quarantines_locked();
					if (pin.state_.get() != this)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										 cleanup_action);
					if (admission_quarantined_locked())
						return rejection(sqlite_shm_lease_rejection_reason::quarantined,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);

					auto* family_pin = current_family_pin_locked(pin);
					auto* alias = find_alias_locked(pin.alias_token_);
					auto* family = find_family_epoch_locked(pin.family_epoch_);
					if (family_pin == nullptr || alias == nullptr || family == nullptr)
						return rejection(sqlite_shm_lease_rejection_reason::stale_token,
										 cleanup_action);
					if (alias->phase == sqlite_shm_registry_alias_phase::quarantined ||
						family->phase == sqlite_shm_registry_family_phase::quarantined)
						return rejection(sqlite_shm_lease_rejection_reason::quarantined,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					if (alias->phase != sqlite_shm_registry_alias_phase::registered ||
						family->phase != sqlite_shm_registry_family_phase::active ||
						!family->coordinator)
						return rejection(sqlite_shm_lease_rejection_reason::retiring,
										 cleanup_action);
					if (receipt.request().family != family->binding ||
						receipt.request().alias_lifetime != alias->alias_lifetime ||
						receipt.request().attachment.family() != family->binding ||
						receipt.request().attachment.alias_lifetime() != alias->alias_lifetime)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										 cleanup_action);
					if (!alias->activity_authority_latch ||
						!alias->activity_authority_latch->load(std::memory_order_acquire) ||
						!exact_family_admission_visible_locked(*family))
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);

					auto installed = family->coordinator->install_registry_writer_pending(
						pin, post_native, std::move(prepared));
					if (!installed &&
						(installed.error().reason ==
							 sqlite_shm_lease_rejection_reason::lifecycle_ambiguous ||
						 installed.error().reason ==
							 sqlite_shm_lease_rejection_reason::quarantined))
						synchronize_coordinator_quarantines_locked();
					return installed;
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]]
			sqlite_shm_lease_result<sqlite_shm_positive_writer_attachment_gate_result>
			advance_positive_writer_attachment_gate(
				sqlite_shm_registry_family_pin& pin,
				const sqlite_shm_native_attachment_identity& attachment,
				const std::span<sqlite_shm_pending_mapping*> pending,
				const sqlite_shm_writer_eligibility& eligibility)
			{
				const auto boundary_action =
					sqlite_shm_lease_recovery_action::await_complete_attachment_gate_boundary;
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
									 boundary_action);
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					synchronize_reader_open_controls_locked();
					synchronize_coordinator_quarantines_locked();
					if (pin.state_.get() != this)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										 boundary_action);
					if (admission_quarantined_locked())
						return rejection(sqlite_shm_lease_rejection_reason::quarantined,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);

					auto* family_pin = current_family_pin_locked(pin);
					auto* alias = find_alias_locked(pin.alias_token_);
					auto* family = find_family_epoch_locked(pin.family_epoch_);
					if (family_pin == nullptr || alias == nullptr || family == nullptr)
						return rejection(sqlite_shm_lease_rejection_reason::stale_token,
										 boundary_action);
					if (alias->phase == sqlite_shm_registry_alias_phase::quarantined ||
						family->phase == sqlite_shm_registry_family_phase::quarantined)
						return rejection(sqlite_shm_lease_rejection_reason::quarantined,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					if (alias->phase != sqlite_shm_registry_alias_phase::registered ||
						family->phase != sqlite_shm_registry_family_phase::active ||
						!family->coordinator)
						return rejection(sqlite_shm_lease_rejection_reason::retiring,
										 boundary_action);
					if (attachment.family() != family->binding ||
						attachment.alias_lifetime() != alias->alias_lifetime)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										 boundary_action);
					if (!alias->activity_authority_latch ||
						!alias->activity_authority_latch->load(std::memory_order_acquire) ||
						!exact_family_admission_visible_locked(*family))
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);

					auto completed =
						family->coordinator->advance_positive_registry_writer_attachment_gate(
							pin, attachment, pending, eligibility);
					if (!completed &&
						(completed.error().reason ==
							 sqlite_shm_lease_rejection_reason::lifecycle_ambiguous ||
						 completed.error().reason ==
							 sqlite_shm_lease_rejection_reason::quarantined))
						synchronize_coordinator_quarantines_locked();
					return completed;
				}
				catch (...)
				{
					emergency_quarantine();
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_writer_holder>
			complete_gate_winning_writer_map_before_callback_return(
				sqlite_shm_registry_family_pin& pin,
				sqlite_shm_writer_post_native_mapping& post_native,
				const sqlite_shm_verified_writer_post_map_receipt& receipt)
			{
				const auto cleanup_action =
					sqlite_shm_lease_recovery_action::attempt_nonremoving_unmap_then_outer_ioerr;
				if (!current(pin.process_epoch_))
					return rejection(sqlite_shm_lease_rejection_reason::stale_token,
									 cleanup_action);
				try
				{
					// Snapshot the fallibly copied caller-owned receipt before either mutex.
					// NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
					auto prepared = receipt;
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					synchronize_coordinator_quarantines_locked();
					if (pin.state_.get() != this)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										 cleanup_action);
					if (admission_quarantined_locked())
						return rejection(sqlite_shm_lease_rejection_reason::quarantined,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);

					auto* family_pin = current_family_pin_locked(pin);
					auto* alias = find_alias_locked(pin.alias_token_);
					auto* family = find_family_epoch_locked(pin.family_epoch_);
					if (family_pin == nullptr || alias == nullptr || family == nullptr)
						return rejection(sqlite_shm_lease_rejection_reason::stale_token,
										 cleanup_action);
					if (alias->phase == sqlite_shm_registry_alias_phase::quarantined ||
						family->phase == sqlite_shm_registry_family_phase::quarantined)
						return rejection(sqlite_shm_lease_rejection_reason::quarantined,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);
					if (alias->phase != sqlite_shm_registry_alias_phase::registered ||
						family->phase != sqlite_shm_registry_family_phase::active ||
						!family->coordinator)
						return rejection(sqlite_shm_lease_rejection_reason::retiring,
										 cleanup_action);
					if (receipt.request().family != family->binding ||
						receipt.request().alias_lifetime != alias->alias_lifetime ||
						receipt.request().attachment.family() != family->binding ||
						receipt.request().attachment.alias_lifetime() != alias->alias_lifetime)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
										 cleanup_action);
					if (!alias->activity_authority_latch ||
						!alias->activity_authority_latch->load(std::memory_order_acquire) ||
						!exact_family_admission_visible_locked(*family))
						return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										 sqlite_shm_lease_recovery_action::quarantine_no_retry);

					auto completed =
						family->coordinator
							->complete_gate_winning_registry_writer_map_before_callback_return(
								pin, post_native, prepared);
					if (!completed &&
						(completed.error().reason ==
							 sqlite_shm_lease_rejection_reason::lifecycle_ambiguous ||
						 completed.error().reason ==
							 sqlite_shm_lease_rejection_reason::quarantined))
						synchronize_coordinator_quarantines_locked();
					return completed;
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
			release_reader_open(sqlite_shm_reader_open_authority& open) noexcept
			{
				try
				{
					if (!open.control_)
						return rejection(sqlite_shm_lease_rejection_reason::stale_token);
					const auto owner_state = open.state_.lock();
					if (!owner_state)
						return rejection(sqlite_shm_lease_rejection_reason::stale_token);
					if (owner_state.get() != this)
						return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch);
					if (!current(open.control_->process_epoch))
					{
						open.disarm();
						return rejection(sqlite_shm_lease_rejection_reason::stale_token);
					}
					{
						std::scoped_lock lock{mutex_};
						synchronize_activity_controls_locked();
						synchronize_reader_open_controls_locked();
						synchronize_coordinator_quarantines_locked();
						auto* record = drainable_reader_open_locked(open);
						if (record == nullptr)
							return current_pin_rejection(open.control_->process_epoch);
						if (open.control_->descendant_authority_count.load(
								std::memory_order_acquire) != 0U)
							return rejection(sqlite_shm_lease_rejection_reason::retiring,
											 sqlite_shm_lease_recovery_action::
												 await_complete_attachment_gate_boundary);
						auto* family = find_family_epoch_locked(record->family_epoch);
						if (family == nullptr || !family->coordinator ||
							!exact_family_drain_visible_locked(*family))
							return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						auto expected = sqlite_shm_reader_open_phase::active;
						if (!open.control_->phase.compare_exchange_strong(
								expected,
								sqlite_shm_reader_open_phase::clean_released,
								std::memory_order_acq_rel,
								std::memory_order_acquire))
							return current_pin_rejection(open.control_->process_epoch);
						auto lineage_released = family->coordinator->release_registry_reader_open(
							record->token, open.control_->lineage_seal);
						if (!lineage_released)
						{
							open.control_->phase.store(sqlite_shm_reader_open_phase::active,
													   std::memory_order_release);
							if (lineage_released.error().reason ==
									sqlite_shm_lease_rejection_reason::quarantined ||
								lineage_released.error().reason ==
									sqlite_shm_lease_rejection_reason::lifecycle_ambiguous)
								quarantine_family_locked(*family);
							return lineage_released.error();
						}
						open.control_->authority_valid.store(false, std::memory_order_release);
						synchronize_reader_open_controls_locked();
						if (record->active)
						{
							emergency_quarantine();
							return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
											 sqlite_shm_lease_recovery_action::quarantine_no_retry);
						}
					}
					open.disarm();
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
					synchronize_reader_open_controls_locked();
					synchronize_coordinator_quarantines_locked();
					auto* family_pin = current_family_pin_locked(pin);
					if (family_pin == nullptr)
						return current_pin_rejection(pin.process_epoch_);
					if (family_pin->active_activities != 0U ||
						family_pin->active_reader_opens != 0U)
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
					synchronize_reader_open_controls_locked();
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
					synchronize_reader_open_controls_locked();
					auto* alias = current_alias_pin_locked(pin);
					if (alias == nullptr)
						return alias_pin_rejection_locked(pin);
					if (alias->phase != sqlite_shm_registry_alias_phase::unregistering)
						return rejection(sqlite_shm_lease_rejection_reason::stale_token);
					if (alias->active_family_pins != 0U || alias->active_activities != 0U ||
						alias->active_reader_opens != 0U)
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
					synchronize_reader_open_controls_locked();
					auto* alias = current_alias_pin_locked(pin);
					if (alias == nullptr)
						return alias_pin_rejection_locked(pin);
					if (alias->phase != sqlite_shm_registry_alias_phase::unregistering)
						return rejection(sqlite_shm_lease_rejection_reason::stale_token);
					if (alias->active_family_pins != 0U || alias->active_activities != 0U ||
						alias->active_reader_opens != 0U)
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
				output.reader_lifecycle_sequence_source_count = 1U;
				if (!output.process_live)
					return output;
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					synchronize_reader_open_controls_locked();
					synchronize_coordinator_quarantines_locked();
					output.registry_quarantined = registry_quarantined_ ||
						emergency_quarantined_.load(std::memory_order_acquire);
					output.alias_record_count = aliases_.size();
					output.family_record_count = families_.size();
					output.duplicate_rejection_count = duplicate_rejection_count_;
					output.cross_binding_rejection_count = cross_binding_rejection_count_;
					output.ambiguous_lookup_count = ambiguous_lookup_count_;
					output.retired_reader_lifecycle_tombstone_count =
						retired_reader_lifecycle_tombstones_.size();
					output.retired_reader_open_epoch_close_tombstone_count =
						retired_reader_open_epoch_close_tombstones_.size();
					output.reader_lifecycle_last_issued_sequence =
						reader_lifecycle_sequences_->observed_last_issued();

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
					for (const auto& open : reader_opens_)
						output.active_reader_open_count += open.active ? 1U : 0U;

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
					synchronize_reader_open_controls_locked();
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
						{
							++output.exact_retired_match_count;
							output.reader_lifecycle_compact_tombstone_count +=
								family.retired_reader_lifecycle_tombstone_count;
							output.reader_open_epoch_close_tombstone_count +=
								family.retired_reader_open_epoch_close_tombstone_count;
						}
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
					output.reader_open_count = singleton->active_reader_opens;
					output.phase = singleton->phase;
					output.lookup_visible = true;
					output.coordinator = singleton->coordinator->snapshot();
					const auto reader_lifecycle =
						singleton->coordinator->reader_lifecycle_view_for_testing();
					output.reader_lifecycle_compact_tombstone_count =
						reader_lifecycle.compact_tombstone_count;
					output.reader_open_epoch_close_tombstone_count =
						reader_lifecycle.open_epoch_close_compact_tombstone_count;
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

			[[nodiscard]] std::optional<sqlite_shm_lease_family_binding>
			family_binding_for_identity_scope(const sqlite_shm_registry_family_pin& pin) noexcept
			{
				if (!current(pin.process_epoch_))
					return std::nullopt;
				try
				{
					std::scoped_lock lock{mutex_};
					const auto* family_pin = current_family_pin_locked(pin);
					const auto* family = find_family_epoch_locked(pin.family_epoch_);
					if (family_pin == nullptr || family == nullptr ||
						!family_pin->identity_authority_latch ||
						!family_pin->identity_authority_latch->load(std::memory_order_acquire) ||
						family->phase != sqlite_shm_registry_family_phase::active)
						return std::nullopt;
					return family->binding;
				}
				catch (...)
				{
					emergency_quarantine();
					return std::nullopt;
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
					synchronize_reader_open_controls_locked();
					auto* alias = find_alias_locked(pin.token_);
					if (alias == nullptr)
						return;
					if (alias->phase == sqlite_shm_registry_alias_phase::reserved &&
						alias->active_family_pins == 0U && alias->active_activities == 0U &&
						alias->active_reader_opens == 0U)
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
					synchronize_reader_open_controls_locked();
					auto* pin = find_family_pin_locked(family_pin.pin_token_);
					if (pin == nullptr || !pin->active ||
						pin->alias_token != family_pin.alias_token_ ||
						pin->family_epoch != family_pin.family_epoch_)
						return;
					if (pin->active_activities != 0U || pin->active_reader_opens != 0U)
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
				auto observed = seal_->process_epoch->load(std::memory_order_acquire);
				while (observed == process_epoch_)
				{
					const auto replacement =
						observed == std::numeric_limits<std::uint64_t>::max() ? 0U : observed + 1U;
					if (seal_->process_epoch->compare_exchange_weak(observed,
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
					auto coordinator =
						std::make_shared<sqlite_same_process_shm_mapping_lease_coordinator>(
							binding, generations_, reader_lifecycle_sequences_);
					auto imported =
						import_retired_reader_lifecycle_tombstones_locked(*coordinator, binding);
					if (!imported)
						return false;
					auto imported_closes = import_retired_reader_open_epoch_close_tombstones_locked(
						*coordinator, binding);
					if (!imported_closes)
						return false;
					families_.emplace_back(epoch, binding, std::move(coordinator));
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
					next_reader_open_token_ = std::numeric_limits<std::uint64_t>::max();
					next_reader_attachment_epoch_ = std::numeric_limits<std::uint64_t>::max();
					reader_lifecycle_sequences_->exhaust_for_testing();
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
						case sqlite_shm_registry_counter_for_testing::reader_open_token:
							next_reader_open_token_ = std::numeric_limits<std::uint64_t>::max();
							break;
						case sqlite_shm_registry_counter_for_testing::reader_attachment_epoch:
							next_reader_attachment_epoch_ =
								std::numeric_limits<std::uint64_t>::max();
							break;
						case sqlite_shm_registry_counter_for_testing::reader_lifecycle_sequence:
							reader_lifecycle_sequences_->exhaust_for_testing();
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
					const auto found = std::find_if(
						families_.begin(),
						families_.end(),
						[&binding](const family_record& family)
						{
							return family.binding == binding &&
								family.phase == sqlite_shm_registry_family_phase::active;
						});
					return found != families_.end() && exact_family_admission_visible_locked(*found)
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

			[[nodiscard]] sqlite_shm_registry_reader_open_epoch_test_view
			reader_open_epoch_view(const sqlite_shm_reader_open_authority& open) noexcept
			{
				sqlite_shm_registry_reader_open_epoch_test_view output;
				if (!current(process_epoch_) || !open.control_)
					return output;
				const auto owner_state = open.state_.lock();
				if (!owner_state || owner_state.get() != this)
					return output;
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					synchronize_reader_open_controls_locked();
					synchronize_coordinator_quarantines_locked();
					const reader_open_record* exact_record = nullptr;
					for (const auto& record : reader_opens_)
					{
						if (record.token != open.control_->open_token)
							continue;
						++output.exact_record_match_count;
						exact_record = &record;
					}
					if (output.exact_record_match_count != 1U)
						return output;
					if (exact_record == nullptr ||
						exact_record->control.get() != open.control_.get())
						return output;
					output.registry_open_token = exact_record->token;
					output.binding = exact_record->control->binding;
					output.record_active = exact_record->active;
					const auto* alias = find_alias_locked(exact_record->alias_token);
					auto* family = find_family_epoch_locked(exact_record->family_epoch);
					const auto exact_family_drain_visible =
						family != nullptr && exact_family_drain_visible_locked(*family);
					if (alias != nullptr && family != nullptr && exact_family_drain_visible &&
						family->coordinator && exact_record->control->lineage_seal)
					{
						output.lease_open_epoch =
							family->coordinator->reader_open_epoch_view_for_testing(
								exact_record->token, exact_record->control->lineage_seal);
						if (output.lease_open_epoch)
							output.lease_binding_matches =
								output.lease_open_epoch->registry_open_token ==
									exact_record->token &&
								output.lease_open_epoch->binding ==
									lease_reader_open_epoch_binding(
										exact_record->control->binding,
										alias->runtime_lifetime.pin_identity());
					}
					output.lookup_visible = exact_record->active && exact_family_drain_visible &&
						reader_open_control_matches_record_locked(*exact_record) &&
						exact_record->control->authority_valid_now() &&
						output.lease_open_epoch.has_value() && output.lease_binding_matches;
					return output;
				}
				catch (...)
				{
					emergency_quarantine();
					return {};
				}
			}

			[[nodiscard]] const void* reader_lifecycle_sequence_source_identity() const noexcept
			{
				return current(process_epoch_) ? reader_lifecycle_sequences_->identity_for_testing()
											   : nullptr;
			}

			[[nodiscard]] std::uint64_t reader_lifecycle_last_issued_sequence() const noexcept
			{
				return current(process_epoch_)
					? reader_lifecycle_sequences_->last_issued_for_testing()
					: 0U;
			}

			[[nodiscard]] std::size_t reader_map_identity_phase_count(
				const sqlite_shm_lease_family_binding& binding) const noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					const auto family = std::find_if(
						families_.begin(), families_.end(),
						[&binding](const family_record& candidate)
						{ return candidate.binding == binding; });
					return family == families_.end() ? 0U
						: family->reader_map_identity_phases.size();
				}
				catch (...)
				{
					return std::numeric_limits<std::size_t>::max();
				}
			}

			void exhaust_reader_lifecycle_sequence_source() noexcept
			{
				if (!current(process_epoch_))
					return;
				try
				{
					std::scoped_lock lock{mutex_};
					synchronize_activity_controls_locked();
					reader_lifecycle_sequences_->exhaust_for_testing();
				}
				catch (...)
				{
					emergency_quarantine();
				}
			}

			[[nodiscard]] std::size_t retired_reader_lifecycle_tombstone_count() noexcept
			{
				if (!current(process_epoch_))
					return 0U;
				try
				{
					std::scoped_lock lock{mutex_};
					return retired_reader_lifecycle_tombstones_.size();
				}
				catch (...)
				{
					emergency_quarantine();
					return 0U;
				}
			}

			[[nodiscard]] bool retired_reader_lifecycle_tombstone_matches(
				const sqlite_shm_reader_attachment_reservation_identity& attachment) noexcept
			{
				if (!current(process_epoch_))
					return false;
				try
				{
					std::scoped_lock lock{mutex_};
					return std::ranges::any_of(
						retired_reader_lifecycle_tombstones_,
						[&attachment](
							const sqlite_shm_reader_lifecycle_compact_tombstone& tombstone)
						{
							return tombstone.attachment == attachment;
						});
				}
				catch (...)
				{
					emergency_quarantine();
					return false;
				}
			}

			[[nodiscard]] std::size_t retired_reader_open_epoch_close_tombstone_count() noexcept
			{
				if (!current(process_epoch_))
					return 0U;
				try
				{
					std::scoped_lock lock{mutex_};
					return retired_reader_open_epoch_close_tombstones_.size();
				}
				catch (...)
				{
					emergency_quarantine();
					return 0U;
				}
			}

			[[nodiscard]] bool retired_reader_open_epoch_close_tombstone_matches(
				const std::uint64_t registry_open_token,
				const sqlite_shm_reader_open_epoch_binding& binding) noexcept
			{
				if (!current(process_epoch_))
					return false;
				try
				{
					std::scoped_lock lock{mutex_};
					return std::ranges::any_of(
						retired_reader_open_epoch_close_tombstones_,
						[registry_open_token,
						 &binding](const sqlite_shm_reader_open_epoch_close_tombstone& tombstone)
						{
							return tombstone.registry_open_token == registry_open_token &&
								tombstone.binding == binding;
						});
				}
				catch (...)
				{
					emergency_quarantine();
					return false;
				}
			}

			void lock_mutex_for_testing()
			{
				mutex_.lock();
			}

			void unlock_mutex_for_testing()
			{
				mutex_.unlock();
			}

			[[nodiscard]] sqlite_shm_lease_result<
				sqlite_shm_reader_candidate_authority_minter::candidate>
			mint_reader_candidate_locked(
				const sqlite_shm_registry_family_pin& pin,
				const sqlite_shm_reader_open_authority& open,
				const sqlite_shm_reader_pre_sqlite_session_request& request,
				const std::uint64_t writer_generation)
			{
				auto* family_pin = current_family_pin_locked(pin);
				auto* alias = find_alias_locked(pin.alias_token_);
				auto* family = find_family_epoch_locked(pin.family_epoch_);
				auto* reader_open = current_reader_open_locked(open);
				if (family_pin == nullptr || alias == nullptr || family == nullptr ||
					reader_open == nullptr || writer_generation == 0U)
					return rejection(sqlite_shm_lease_rejection_reason::stale_token);
				if (request.family != family->binding ||
					request.alias_lifetime != alias->alias_lifetime ||
					reader_open->alias_token != alias->token ||
					reader_open->family_epoch != family->entry_epoch ||
					reader_open->family_pin_token != family_pin->token ||
					!reader_open_binding_matches_request(reader_open->control->binding, request) ||
					!alias->runtime_lifetime.valid())
					return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch);

				std::uint64_t epoch_value{};
				if (!allocate_counter_locked(next_reader_attachment_epoch_, epoch_value))
					return counter_exhaustion_rejection();
				auto attachment = sqlite_shm_reader_attachment_reservation_identity::bind(
					family->binding,
					alias->runtime_lifetime.pin_identity(),
					alias->alias_lifetime,
					request.connection_token,
					request.main_native_file_receipt,
					request.main_xopen_receipt,
					request.open_epoch,
					writer_generation,
					request.callback_cohort,
					reader_attachment_epoch_identity(epoch_value),
					reader_open->token);
				if (!attachment)
					return rejection(sqlite_shm_lease_rejection_reason::invalid_identity);

				auto proposal_request = sqlite_shm_reader_session_request{
					*attachment,
					request.execution,
					request.read_transaction_epoch,
					request.decode_attempt,
					request.authority_read_receipt,
				};
				auto storage = std::make_unique<sqlite_shm_reader_attachment_authority_state>();
				storage->attachment.emplace(*attachment);

				std::uint64_t activity_token{};
				if (!allocate_counter_locked(next_activity_token_, activity_token))
					return counter_exhaustion_rejection();
				const sqlite_shm_registry_activity_control::coordinates coordinates{
					process_epoch_,
					alias->token,
					family->entry_epoch,
					family_pin->token,
					activity_token,
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
				activities_.push_back({activity_token,
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
					sqlite_shm_reader_attachment_authority partial{std::move(storage)};
					cancel_reader_candidate_locked(partial);
					return audit.error();
				}
				storage->audit_seal.emplace(std::move(*audit));
				if (!reader_open->control->retain_descendant())
				{
					sqlite_shm_reader_attachment_authority partial{std::move(storage)};
					cancel_reader_candidate_locked(partial);
					return counter_exhaustion_rejection();
				}
				storage->open = reader_open->control;
				return sqlite_shm_reader_candidate_authority_minter::candidate{
					std::move(proposal_request),
					sqlite_shm_reader_attachment_authority{std::move(storage)},
				};
			}

			[[nodiscard]]
			sqlite_shm_lease_result<sqlite_shm_reader_map_predelegate_authority>
			mint_reader_map_predelegate_locked(
				const sqlite_shm_registry_family_pin& pin,
				const sqlite_shm_reader_attachment_map_request& request)
			{
				auto* family_pin = current_family_pin_locked(pin);
				auto* alias = find_alias_locked(pin.alias_token_);
				auto* family = find_family_epoch_locked(pin.family_epoch_);
				if (family_pin == nullptr || alias == nullptr || family == nullptr)
					return rejection(sqlite_shm_lease_rejection_reason::stale_token);
				if (request.family != family->binding ||
					request.alias_lifetime != alias->alias_lifetime ||
					request.expected_attachment.runtime_lifetime_pin() !=
						alias->runtime_lifetime.pin_identity())
					return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch);
				auto* reader_open =
					find_reader_open_locked(request.expected_attachment.registry_open_token());
				if (reader_open == nullptr || !reader_open->active || !reader_open->control ||
					!reader_open_control_matches_record_locked(*reader_open) ||
					!reader_open->control->authority_valid_now() ||
					reader_open->alias_token != alias->token ||
					reader_open->family_epoch != family->entry_epoch ||
					reader_open->family_pin_token != family_pin->token ||
					reader_open->control->binding.family != request.family ||
					reader_open->control->binding.alias_lifetime != request.alias_lifetime ||
					reader_open->control->binding.connection_token !=
						request.expected_attachment.connection_token() ||
					reader_open->control->binding.main_native_file_receipt !=
						request.expected_attachment.main_native_file_receipt() ||
					reader_open->control->binding.main_xopen_receipt !=
						request.expected_attachment.main_xopen_receipt() ||
					reader_open->control->binding.open_epoch !=
						request.expected_attachment.open_epoch() ||
					reader_open->control->binding.callback_cohort !=
						request.expected_attachment.callback_cohort())
					return rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch);

				auto storage =
					std::make_unique<sqlite_shm_reader_map_predelegate_authority_state>();
				storage->request.emplace(request);
				std::uint64_t activity_token{};
				if (!allocate_counter_locked(next_activity_token_, activity_token))
					return counter_exhaustion_rejection();
				const sqlite_shm_registry_activity_control::coordinates coordinates{
					process_epoch_,
					alias->token,
					family->entry_epoch,
					family_pin->token,
					activity_token,
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
				activities_.push_back({activity_token,
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
					sqlite_shm_reader_map_predelegate_authority partial{std::move(storage)};
					cancel_reader_map_predelegate_locked(partial);
					return audit.error();
				}
				storage->audit_seal.emplace(std::move(*audit));
				if (!reader_open->control->retain_descendant())
				{
					sqlite_shm_reader_map_predelegate_authority partial{std::move(storage)};
					cancel_reader_map_predelegate_locked(partial);
					return counter_exhaustion_rejection();
				}
				storage->open = reader_open->control;
				return sqlite_shm_reader_map_predelegate_authority{std::move(storage)};
			}

			void cancel_reader_candidate_locked(
				sqlite_shm_reader_attachment_authority& authority) noexcept
			{
				if (!authority.state_ || !authority.state_->activity)
					return;
				if (authority.state_->open && !authority.state_->open->release_descendant())
					emergency_quarantine();
				authority.state_->open.reset();
				cancel_reader_activity_locked(*authority.state_->activity);
				authority.state_.reset();
			}

			void cancel_reader_map_predelegate_locked(
				sqlite_shm_reader_map_predelegate_authority& authority) noexcept
			{
				if (!authority.state_ || !authority.state_->activity)
					return;
				if (authority.state_->open && !authority.state_->open->release_descendant())
					emergency_quarantine();
				authority.state_->open.reset();
				cancel_reader_activity_locked(*authority.state_->activity);
				authority.state_.reset();
			}

		  private:
			sqlite_shm_mapping_registry_state(
				sqlite_backend_opaque_identity process_instance,
				std::shared_ptr<sqlite_shm_registry_process_owner_seal> seal,
				const initialization value)
				: process_instance_{std::move(process_instance)}, seal_{std::move(seal)},
				  process_epoch_{value.process_epoch},
				  generations_{std::make_shared<sqlite_shm_mapping_generation_source>(
					  value.first_mapping_generation)},
				  reader_lifecycle_sequences_{
					  std::make_shared<sqlite_shm_reader_lifecycle_sequence_source>()}
			{
			}

			[[nodiscard]] bool admission_quarantined_locked() const noexcept
			{
				return registry_quarantined_ ||
					emergency_quarantined_.load(std::memory_order_acquire);
			}

			void cancel_reader_activity_locked(sqlite_shm_registry_activity_pin& activity) noexcept
			{
				if (!activity.control_)
					return;
				auto expected = sqlite_shm_registry_activity_phase::active;
				if (!activity.control_->phase.compare_exchange_strong(
						expected,
						sqlite_shm_registry_activity_phase::clean_released,
						std::memory_order_acq_rel,
						std::memory_order_acquire))
				{
					emergency_quarantine();
					return;
				}
				activity.control_->authority_valid.store(false, std::memory_order_release);
				synchronize_activity_controls_locked();
				activity.disarm();
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

			[[nodiscard]] bool fresh_admission_has_no_pending_reader_ack_locked(
				const family_record& selected) const noexcept
			{
				if (!selected.coordinator)
					return false;
				const auto snapshot = selected.coordinator->snapshot();
				return !snapshot.quarantined &&
					snapshot.reader_unpublished_cleanup_admitted_count == 0U &&
					snapshot.reader_logical_ack_awaiting_count == 0U;
			}

			[[nodiscard]] bool exact_family_drain_visible_locked(family_record& selected) noexcept
			{
				synchronize_coordinator_quarantines_locked();
				std::size_t drainable_matches{};
				bool selected_is_drainable{};
				for (const auto& family : families_)
				{
					if (family.binding != selected.binding)
						continue;
					const auto drainable =
						family.phase == sqlite_shm_registry_family_phase::active ||
						family.phase == sqlite_shm_registry_family_phase::quarantined;
					drainable_matches += static_cast<std::size_t>(drainable);
					selected_is_drainable =
						selected_is_drainable || (&family == &selected && drainable);
				}
				if (drainable_matches > 1U)
				{
					increment_audit_counter_locked(ambiguous_lookup_count_);
					quarantine_registry_locked();
					return false;
				}
				if (drainable_matches != 1U || !selected_is_drainable || !selected.coordinator)
				{
					if (selected_is_drainable && !selected.coordinator)
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

			[[nodiscard]] bool
			reader_open_control_matches_record_locked(const reader_open_record& open) noexcept
			{
				if (!open.control || open.token != open.control->open_token ||
					open.alias_token != open.control->alias_token ||
					open.family_epoch != open.control->family_epoch ||
					open.family_pin_token != open.control->family_pin_token ||
					open.control->process_epoch != process_epoch_ || !open.control->lineage_seal ||
					open.control->emergency_latch.get() != activity_emergency_latch_.get())
					return false;
				const auto owner_state = open.control->registry_state.lock();
				const auto owner_seal = open.control->process_seal.lock();
				const auto* alias = find_alias_locked(open.alias_token);
				const auto* family = find_family_epoch_locked(open.family_epoch);
				const auto* family_pin = find_family_pin_locked(open.family_pin_token);
				return owner_state.get() == this && owner_seal.get() == seal_.get() &&
					alias != nullptr && family != nullptr && family_pin != nullptr &&
					open.control->alias_authority_latch.get() ==
					alias->activity_authority_latch.get() &&
					open.control->family_authority_latch.get() ==
					family->activity_authority_latch.get() &&
					open.control->binding.family == family->binding &&
					open.control->binding.alias_lifetime == alias->alias_lifetime &&
					family_pin->alias_token == open.alias_token &&
					family_pin->family_epoch == open.family_epoch;
			}

			void release_reader_open_record_locked(reader_open_record& open) noexcept
			{
				if (!open.active)
					return;
				auto* alias = find_alias_locked(open.alias_token);
				auto* family = find_family_epoch_locked(open.family_epoch);
				auto* family_pin = find_family_pin_locked(open.family_pin_token);
				if (alias == nullptr || family == nullptr || family_pin == nullptr ||
					alias->active_reader_opens == 0U || family->active_reader_opens == 0U ||
					family_pin->active_reader_opens == 0U)
				{
					quarantine_registry_locked();
					return;
				}
				open.active = false;
				--alias->active_reader_opens;
				--family->active_reader_opens;
				--family_pin->active_reader_opens;
				if (family_pin->active_reader_opens == 0U && family_pin->active_activities == 0U &&
					family_pin->abandoned)
					(void)release_family_locked(*family_pin);
			}

			void synchronize_reader_open_controls_locked() noexcept
			{
				for (auto& open : reader_opens_)
				{
					if (!open.active)
						continue;
					if (!reader_open_control_matches_record_locked(open))
					{
						if (open.control)
							open.control->authority_valid.store(false, std::memory_order_release);
						quarantine_registry_locked();
						continue;
					}
					const auto phase = open.control->phase.load(std::memory_order_acquire);
					if (phase == sqlite_shm_reader_open_phase::active)
						continue;
					open.control->authority_valid.store(false, std::memory_order_release);
					if (phase == sqlite_shm_reader_open_phase::clean_released &&
						open.control->descendant_authority_count.load(std::memory_order_acquire) ==
							0U)
					{
						release_reader_open_record_locked(open);
						continue;
					}
					auto* alias = find_alias_locked(open.alias_token);
					auto* family = find_family_epoch_locked(open.family_epoch);
					if (alias == nullptr || family == nullptr)
						quarantine_registry_locked();
					else
					{
						alias->phase = sqlite_shm_registry_alias_phase::quarantined;
						deactivate_fresh_reader_map_identity_phases_locked(*family);
						family->phase = sqlite_shm_registry_family_phase::quarantined;
						invalidate_activity_authority_for_alias_locked(alias->token);
						invalidate_activity_authority_for_family_locked(family->entry_epoch);
						propagate_local_quarantine_locked();
					}
				}
			}

			void
			invalidate_identity_authority_for_pin_record(family_pin_record& pin) noexcept
			{
				if (!pin.identity_authority_latch)
					return;
				pin.identity_authority_latch->store(false, std::memory_order_release);
				pin.identity_authority_latch.reset();
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
				for (auto& pin : family_pins_)
					if (pin.alias_token == alias_token)
						invalidate_identity_authority_for_pin_record(pin);
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
				for (auto& pin : family_pins_)
					if (pin.family_epoch == family_epoch)
						invalidate_identity_authority_for_pin_record(pin);
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
						deactivate_fresh_reader_map_identity_phases_locked(*family);
						family->phase = sqlite_shm_registry_family_phase::quarantined;
						invalidate_activity_authority_for_alias_locked(alias->token);
						invalidate_activity_authority_for_family_locked(family->entry_epoch);
						propagate_local_quarantine_locked();
					}
					release_activity_locked(activity);
				}
			}

			void deactivate_fresh_reader_map_identity_phases_locked(family_record& family) noexcept
			{
				std::erase_if(family.reader_map_identity_phases,
					[](const auto& weak_phase)
					{
						const auto phase = weak_phase.lock();
						return !phase || phase->load(std::memory_order_acquire) !=
							sqlite_shm_reader_lifecycle_owner_phase::admission;
					});
				for (const auto& weak_phase : family.reader_map_identity_phases)
				{
					const auto phase = weak_phase.lock();
					if (!phase)
						continue;
					auto expected = sqlite_shm_reader_lifecycle_owner_phase::admission;
					(void)phase->compare_exchange_strong(
						expected, sqlite_shm_reader_lifecycle_owner_phase::inactive,
						std::memory_order_acq_rel, std::memory_order_acquire);
				}
				family.reader_map_identity_phases.clear();
			}

			void quarantine_registry_locked() noexcept
			{
				registry_quarantined_ = true;
				activity_emergency_latch_->store(true, std::memory_order_release);
				for (auto& family : families_)
					deactivate_fresh_reader_map_identity_phases_locked(family);
				for (auto& alias : aliases_)
					if (alias.activity_authority_latch)
						alias.activity_authority_latch->store(false, std::memory_order_release);
				for (auto& family : families_)
					if (family.activity_authority_latch)
						family.activity_authority_latch->store(false, std::memory_order_release);
				for (auto& pin : family_pins_)
					invalidate_identity_authority_for_pin_record(pin);
				for (auto& activity : activities_)
					if (activity.control)
						activity.control->authority_valid.store(false, std::memory_order_release);
				for (auto& open : reader_opens_)
					if (open.control)
						open.control->authority_valid.store(false, std::memory_order_release);
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
				deactivate_fresh_reader_map_identity_phases_locked(family);
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
					for (auto& pin : family_pins_)
					{
						auto* family = find_family_epoch_locked(pin.family_epoch);
						auto* alias = find_alias_locked(pin.alias_token);
						if (family == nullptr || alias == nullptr ||
							alias->phase == sqlite_shm_registry_alias_phase::detached)
							continue;
						if ((family->phase == sqlite_shm_registry_family_phase::quarantined ||
							 alias->phase == sqlite_shm_registry_alias_phase::quarantined))
							invalidate_identity_authority_for_pin_record(pin);
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
							deactivate_fresh_reader_map_identity_phases_locked(*family);
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
					const auto coordinator_snapshot = family.coordinator->snapshot();
					if (coordinator_snapshot.quarantined)
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

			[[nodiscard]] reader_open_record*
			find_reader_open_locked(const std::uint64_t token) noexcept
			{
				const auto found = std::find_if(reader_opens_.begin(),
												reader_opens_.end(),
												[token](const auto& value)
												{
													return value.token == token;
												});
				return found == reader_opens_.end() ? nullptr : &*found;
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

			[[nodiscard]] reader_open_record*
			drainable_reader_open_locked(const sqlite_shm_reader_open_authority& open) noexcept
			{
				// This is a terminal-drain lookup, never an admission lookup. It deliberately
				// ignores only the alias/family latches revoked by ordinary peer quarantine while
				// retaining the exact process, record/control, family-pin, and lineage-seal
				// checks. Global/emergency quarantine and abandonment remain terminal.
				if (!open.control_ || !current(open.control_->process_epoch) ||
					admission_quarantined_locked())
					return nullptr;
				const auto owner_state = open.state_.lock();
				if (!owner_state || owner_state.get() != this)
					return nullptr;
				auto* found = find_reader_open_locked(open.control_->open_token);
				if (found == nullptr || !found->active ||
					found->control.get() != open.control_.get() ||
					found->alias_token != open.control_->alias_token ||
					found->family_epoch != open.control_->family_epoch ||
					found->family_pin_token != open.control_->family_pin_token ||
					!reader_open_control_matches_record_locked(*found) ||
					open.control_->phase.load(std::memory_order_acquire) !=
						sqlite_shm_reader_open_phase::active ||
					!open.control_->authority_valid.load(std::memory_order_acquire) ||
					!open.control_->lineage_seal ||
					!open.control_->lineage_seal->authority_valid.load(std::memory_order_acquire))
					return nullptr;
				const auto* alias = find_alias_locked(found->alias_token);
				const auto* family = find_family_epoch_locked(found->family_epoch);
				const auto* family_pin = find_family_pin_locked(found->family_pin_token);
				const auto alias_drainable = alias != nullptr &&
					(alias->phase == sqlite_shm_registry_alias_phase::registered ||
					 alias->phase == sqlite_shm_registry_alias_phase::unregistering ||
					 alias->phase == sqlite_shm_registry_alias_phase::quarantined);
				const auto family_drainable = family != nullptr && family->coordinator &&
					(family->phase == sqlite_shm_registry_family_phase::active ||
					 family->phase == sqlite_shm_registry_family_phase::quarantined);
				return alias_drainable && family_drainable && family_pin != nullptr &&
						family_pin->active && !family_pin->abandoned
					? found
					: nullptr;
			}

			[[nodiscard]] reader_open_record*
			current_reader_open_locked(const sqlite_shm_reader_open_authority& open) noexcept
			{
				if (!open.control_ || !current(open.control_->process_epoch))
					return nullptr;
				const auto owner_state = open.state_.lock();
				if (!owner_state || owner_state.get() != this)
					return nullptr;
				auto* found = find_reader_open_locked(open.control_->open_token);
				return found != nullptr && found->active &&
						found->control.get() == open.control_.get() &&
						found->alias_token == open.control_->alias_token &&
						found->family_epoch == open.control_->family_epoch &&
						found->family_pin_token == open.control_->family_pin_token &&
						reader_open_control_matches_record_locked(*found) &&
						open.control_->authority_valid_now()
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

			[[nodiscard]] sqlite_shm_lease_result<void>
			import_retired_reader_lifecycle_tombstones_locked(
				sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
				const sqlite_shm_lease_family_binding& binding)
			{
				std::vector<sqlite_shm_reader_lifecycle_compact_tombstone> matching;
				for (const auto& tombstone : retired_reader_lifecycle_tombstones_)
					if (tombstone.attachment.family() == binding)
						matching.push_back(tombstone);
				if (matching.empty())
					return {};
				return coordinator.import_registry_reader_lifecycle_tombstones(matching);
			}

			[[nodiscard]] sqlite_shm_lease_result<void>
			import_retired_reader_open_epoch_close_tombstones_locked(
				sqlite_same_process_shm_mapping_lease_coordinator& coordinator,
				const sqlite_shm_lease_family_binding& binding)
			{
				std::vector<sqlite_shm_reader_open_epoch_close_tombstone> matching;
				for (const auto& tombstone : retired_reader_open_epoch_close_tombstones_)
					if (tombstone.binding.family == binding)
						matching.push_back(tombstone);
				if (matching.empty())
					return {};
				return coordinator.import_registry_reader_open_epoch_close_tombstones(matching);
			}

			[[nodiscard]] bool
			retain_reader_lifecycle_tombstones_locked(family_record& family) noexcept
			{
				if (family.reader_lifecycle_tombstones_exported)
					return true;
				if (!family.coordinator)
					return false;
				try
				{
					auto exported =
						family.coordinator->export_registry_reader_lifecycle_tombstones();
					if (!exported)
						return false;

					auto retained = retired_reader_lifecycle_tombstones_;
					const auto prior_retained_count = retained.size();
					retained.reserve(retained.size() + exported->size());
					for (const auto& tombstone : *exported)
					{
						const auto existing = std::ranges::find_if(
							retained,
							[&tombstone](
								const sqlite_shm_reader_lifecycle_compact_tombstone& candidate)
							{
								return candidate.attachment == tombstone.attachment ||
									reader_replay_tombstones_overlap(candidate.replay_identities,
																	 tombstone.replay_identities);
							});
						const auto close_replay_collision = std::ranges::any_of(
							retired_reader_open_epoch_close_tombstones_,
							[&tombstone](
								const sqlite_shm_reader_open_epoch_close_tombstone& candidate)
							{
								return reader_replay_tombstones_overlap(
									candidate.replay_identities, tombstone.replay_identities);
							});
						if (tombstone.attachment.family() != family.binding ||
							tombstone.attachment.registry_open_token() == 0U ||
							!valid_reader_lifecycle_replay_tombstone(tombstone) ||
							tombstone.origin_sequence == 0U ||
							tombstone.destination_sequence <= tombstone.origin_sequence ||
							tombstone.destination_sequence >
								reader_lifecycle_sequences_->observed_last_issued() ||
							tombstone.logical_ack_sequence >
								reader_lifecycle_sequences_->observed_last_issued() ||
							tombstone.composite_close_wait_resolution_sequence >
								reader_lifecycle_sequences_->observed_last_issued() ||
							close_replay_collision ||
							(existing != retained.end() && *existing != tombstone))
							return false;
						if (existing == retained.end())
							retained.push_back(tombstone);
					}
					retired_reader_lifecycle_tombstones_.swap(retained);
					family.retired_reader_lifecycle_tombstone_count =
						retired_reader_lifecycle_tombstones_.size() - prior_retained_count;
					family.reader_lifecycle_tombstones_exported = true;
					return true;
				}
				catch (...)
				{
					return false;
				}
			}

			[[nodiscard]] bool
			retain_reader_open_epoch_close_tombstones_locked(family_record& family) noexcept
			{
				if (family.reader_open_epoch_close_tombstones_exported)
					return true;
				if (!family.coordinator)
					return false;
				try
				{
					auto exported =
						family.coordinator->export_registry_reader_open_epoch_close_tombstones();
					if (!exported)
						return false;

					auto retained = retired_reader_open_epoch_close_tombstones_;
					const auto prior_retained_count = retained.size();
					retained.reserve(retained.size() + exported->size());
					for (const auto& tombstone : *exported)
					{
						const auto existing = std::ranges::find_if(
							retained,
							[&tombstone](
								const sqlite_shm_reader_open_epoch_close_tombstone& candidate)
							{
								return candidate.registry_open_token ==
									tombstone.registry_open_token ||
									(candidate.binding.family == tombstone.binding.family &&
									 candidate.close_owner_token == tombstone.close_owner_token) ||
									candidate.binding == tombstone.binding ||
									reader_replay_tombstones_overlap(candidate.replay_identities,
																	 tombstone.replay_identities);
							});
						const auto lifecycle_replay_collision = std::ranges::any_of(
							retired_reader_lifecycle_tombstones_,
							[&tombstone](
								const sqlite_shm_reader_lifecycle_compact_tombstone& candidate)
							{
								return reader_replay_tombstones_overlap(
									candidate.replay_identities, tombstone.replay_identities);
							});
						if (tombstone.binding.family != family.binding ||
							!valid_lease_reader_open_epoch_binding(tombstone.binding) ||
							!valid_reader_close_replay_tombstone(tombstone) ||
							tombstone.registry_open_token == 0U ||
							tombstone.close_owner_token == 0U || tombstone.origin_sequence == 0U ||
							tombstone.close_cut_sequence <= tombstone.origin_sequence ||
							tombstone.terminal_sequence <= tombstone.close_cut_sequence ||
							tombstone.terminal_sequence >
								reader_lifecycle_sequences_->observed_last_issued() ||
							lifecycle_replay_collision ||
							(existing != retained.end() && *existing != tombstone))
							return false;
						if (existing == retained.end())
							retained.push_back(tombstone);
					}
					for (const auto& lifecycle : retired_reader_lifecycle_tombstones_)
					{
						if (lifecycle.attachment.family() != family.binding)
							continue;
						const auto unpublished_cleanup = lifecycle.phase ==
							detail::sqlite_shm_reader_attachment_reservation_phase::
								unpublished_cleanup_confirmed;
						const auto predecessor = lifecycle.phase ==
							detail::sqlite_shm_reader_attachment_reservation_phase::
								predecessor_route_retired_confirmed;
						if (!unpublished_cleanup && !predecessor)
							continue;
						const auto exact_close =
							[&lifecycle](
								const sqlite_shm_reader_open_epoch_close_tombstone& candidate)
						{
							return lifecycle.attachment.registry_open_token() ==
								candidate.registry_open_token &&
								reader_attachment_matches_open_epoch_binding(lifecycle.attachment,
																			 candidate.binding);
						};
						const auto matching_close = std::ranges::find_if(retained, exact_close);
						if (matching_close == retained.end() ||
							std::ranges::count_if(retained, exact_close) != 1U)
							return false;
						if (predecessor)
						{
							const auto retired_by_close =
								lifecycle.replay_identities.callback_invocation_tokens.size() == 1U;
							if ((retired_by_close &&
								 lifecycle.destination_sequence !=
									 matching_close->terminal_sequence) ||
								(!retired_by_close &&
								 lifecycle.destination_sequence >=
									 matching_close->close_cut_sequence))
								return false;
							continue;
						}
						const auto exact_unmap_ack = lifecycle.logical_ack_phase ==
							detail::sqlite_shm_reader_logical_ack_phase::consumed_by_exact_unmap;
						const auto close_ack = lifecycle.logical_ack_phase ==
							detail::sqlite_shm_reader_logical_ack_phase::consumed_by_close;
						const auto phase1_close_ack = close_ack &&
							lifecycle.composite_close_owner_token ==
								matching_close->close_owner_token &&
							lifecycle.composite_close_registry_open_token ==
								matching_close->registry_open_token &&
							lifecycle.composite_close_cut_sequence ==
								matching_close->close_cut_sequence &&
							lifecycle.composite_close_wait_resolution_sequence >
								lifecycle.logical_ack_sequence &&
							lifecycle.composite_close_wait_resolution_sequence <
								matching_close->terminal_sequence;
						if ((exact_unmap_ack &&
							 lifecycle.logical_ack_sequence >=
								 matching_close->close_cut_sequence) ||
							(close_ack && !phase1_close_ack &&
							 lifecycle.logical_ack_sequence != matching_close->close_cut_sequence))
							return false;
					}
					retired_reader_open_epoch_close_tombstones_.swap(retained);
					family.retired_reader_open_epoch_close_tombstone_count =
						retired_reader_open_epoch_close_tombstones_.size() - prior_retained_count;
					family.reader_open_epoch_close_tombstones_exported = true;
					return true;
				}
				catch (...)
				{
					return false;
				}
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
					auto coordinator =
						std::make_shared<sqlite_same_process_shm_mapping_lease_coordinator>(
							binding, generations_, reader_lifecycle_sequences_);
					auto imported =
						import_retired_reader_lifecycle_tombstones_locked(*coordinator, binding);
					if (!imported)
						return imported.error();
					auto imported_closes = import_retired_reader_open_epoch_close_tombstones_locked(
						*coordinator, binding);
					if (!imported_closes)
						return imported_closes.error();
					families_.emplace_back(epoch, binding, std::move(coordinator));
					active = &families_.back();
					created_family = true;
				}
				if (active == nullptr || !exact_family_admission_visible_locked(*active))
					return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									 sqlite_shm_lease_recovery_action::quarantine_no_retry);
				if (!fresh_admission_has_no_pending_reader_ack_locked(*active))
					return rejection(
						sqlite_shm_lease_rejection_reason::retiring,
						sqlite_shm_lease_recovery_action::await_complete_attachment_gate_boundary);

				std::uint64_t pin_token{};
				if (!allocate_counter_locked(next_family_pin_token_, pin_token))
					return counter_exhaustion_rejection();
				try
				{
					auto identity_authority_latch = std::make_shared<std::atomic_bool>(true);
					family_pins_.push_back({pin_token,
						alias->token,
						active->entry_epoch,
						0U,
						0U,
						true,
						false,
						identity_authority_latch});
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
						family_pins_.back().identity_authority_latch,
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
				if (!pin.active || pin.active_activities != 0U || pin.active_reader_opens != 0U)
					return false;
				pin.active = false;
				invalidate_identity_authority_for_pin_record(pin);
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
				if (family->active_activities != 0U || family->active_reader_opens != 0U)
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
				if (!retain_reader_lifecycle_tombstones_locked(*family))
				{
					quarantine_family_locked(*family);
					return false;
				}
				if (!retain_reader_open_epoch_close_tombstones_locked(*family))
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
			std::shared_ptr<sqlite_shm_reader_lifecycle_sequence_source>
				reader_lifecycle_sequences_;
			std::shared_ptr<std::atomic_bool> activity_emergency_latch_{
				std::make_shared<std::atomic_bool>(false)};
			mutable std::mutex mutex_;
			std::vector<alias_record> aliases_;
			std::vector<family_record> families_;
			std::vector<family_pin_record> family_pins_;
			std::vector<activity_record> activities_;
			std::vector<reader_open_record> reader_opens_;
			std::vector<sqlite_shm_reader_lifecycle_compact_tombstone>
				retired_reader_lifecycle_tombstones_;
			std::vector<sqlite_shm_reader_open_epoch_close_tombstone>
				retired_reader_open_epoch_close_tombstones_;
			std::uint64_t next_alias_token_{1U};
			std::uint64_t next_family_epoch_{1U};
			std::uint64_t next_family_pin_token_{1U};
			std::uint64_t next_activity_token_{1U};
			std::uint64_t next_reader_open_token_{1U};
			std::uint64_t next_reader_attachment_epoch_{1U};
			std::size_t duplicate_rejection_count_{};
			std::size_t cross_binding_rejection_count_{};
			std::size_t ambiguous_lookup_count_{};
			bool registry_quarantined_{};
			std::atomic_bool emergency_quarantined_{false};
		};
	} // namespace detail

	sqlite_shm_reader_open_authority::sqlite_shm_reader_open_authority(
		std::weak_ptr<detail::sqlite_shm_mapping_registry_state> state,
		std::shared_ptr<detail::sqlite_shm_reader_open_control> control) noexcept
		: state_{std::move(state)}, control_{std::move(control)}
	{
	}

	sqlite_shm_reader_open_authority::~sqlite_shm_reader_open_authority() noexcept
	{
		if (!control_)
			return;
		// The coordinator-visible seal is the no-lock abandonment linearization point.
		// Once false, no direct/native admission may pass even before registry synchronization.
		if (control_->lineage_seal)
			control_->lineage_seal->authority_valid.store(false, std::memory_order_release);
		auto expected = detail::sqlite_shm_reader_open_phase::active;
		if (control_->phase.compare_exchange_strong(expected,
													detail::sqlite_shm_reader_open_phase::abandoned,
													std::memory_order_acq_rel,
													std::memory_order_acquire))
		{
			control_->authority_valid.store(false, std::memory_order_release);
			if (control_->alias_authority_latch)
				control_->alias_authority_latch->store(false, std::memory_order_release);
			if (control_->family_authority_latch)
				control_->family_authority_latch->store(false, std::memory_order_release);
		}
	}

	sqlite_shm_reader_open_authority::sqlite_shm_reader_open_authority(
		sqlite_shm_reader_open_authority&& other) noexcept
		: state_{std::move(other.state_)}, control_{std::move(other.control_)}
	{
	}

	bool sqlite_shm_reader_open_authority::valid() const noexcept
	{
		return control_ && control_->authority_valid_now();
	}

	void sqlite_shm_reader_open_authority::publish_abandonment_lineage_for_testing() noexcept
	{
		if (control_ && control_->lineage_seal)
			control_->lineage_seal->authority_valid.store(false, std::memory_order_release);
	}

	void sqlite_shm_reader_open_authority::disarm() noexcept
	{
		state_.reset();
		control_.reset();
	}

	sqlite_shm_reader_candidate_authority_minter::sqlite_shm_reader_candidate_authority_minter(
		detail::sqlite_shm_mapping_registry_state& registry,
		const sqlite_shm_registry_family_pin& family,
		const sqlite_shm_reader_open_authority& open,
		const sqlite_shm_reader_pre_sqlite_session_request& request) noexcept
		: registry_{&registry}, family_{&family}, open_{&open}, request_{&request}
	{
	}

	sqlite_shm_lease_result<sqlite_shm_reader_candidate_authority_minter::candidate>
	sqlite_shm_reader_candidate_authority_minter::mint(const std::uint64_t writer_generation)
	{
		return registry_->mint_reader_candidate_locked(
			*family_, *open_, *request_, writer_generation);
	}

	void sqlite_shm_reader_candidate_authority_minter::cancel(
		sqlite_shm_reader_attachment_authority& authority) noexcept
	{
		registry_->cancel_reader_candidate_locked(authority);
	}

	sqlite_shm_reader_map_predelegate_minter::sqlite_shm_reader_map_predelegate_minter(
		detail::sqlite_shm_mapping_registry_state& registry,
		const sqlite_shm_registry_family_pin& family) noexcept
		: registry_{&registry}, family_{&family}
	{
	}

	sqlite_shm_lease_result<sqlite_shm_reader_map_predelegate_authority>
	sqlite_shm_reader_map_predelegate_minter::mint(
		const sqlite_shm_reader_attachment_map_request& request)
	{
		return registry_->mint_reader_map_predelegate_locked(*family_, request);
	}

	void sqlite_shm_reader_map_predelegate_minter::cancel(
		sqlite_shm_reader_map_predelegate_authority& authority) noexcept
	{
		registry_->cancel_reader_map_predelegate_locked(authority);
	}

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

	detail::sqlite_shm_writer_pending_authority_status
	sqlite_shm_writer_member_authority::validate_pending_authority(
		const sqlite_shm_registry_family_pin& family,
		const sqlite_shm_writer_map_request& request,
		const sqlite_shm_verified_writer_post_map_receipt& receipt) const noexcept
	{
		using status = detail::sqlite_shm_writer_pending_authority_status;
		if (!state_ || !state_->activity || !state_->audit_seal || !state_->epoch_arm)
			return status::lifecycle_ambiguous;
		const auto& activity = *state_->activity;
		const auto control = activity.control_;
		const auto registry = activity.state_.lock();
		const auto audit_control = state_->audit_seal->control_.lock();
		if (!control || !registry || !family.state_)
			return status::lifecycle_ambiguous;

		const auto structural_coordinates_match = registry.get() == family.state_.get() &&
			control->registry_state.lock().get() == registry.get() &&
			control->process_epoch == family.process_epoch_ &&
			control->alias_token == family.alias_token_ &&
			control->family_epoch == family.family_epoch_ &&
			control->family_pin_token == family.pin_token_ &&
			control->process_instance == request.family.process_instance &&
			control->family == request.family &&
			control->alias_lifetime == request.alias_lifetime && receipt.request() == request;
		if (!structural_coordinates_match || !state_->epoch_arm->matches_validated_receipt(receipt))
			return status::determinate_mismatch;

		const auto* family_pin = registry->current_family_pin_locked(family);
		const auto* alias = registry->find_alias_locked(family.alias_token_);
		const auto* family_record = registry->find_family_epoch_locked(family.family_epoch_);
		const auto* activity_record = registry->current_activity_locked(activity);
		if (family_pin == nullptr || alias == nullptr || family_record == nullptr ||
			activity_record == nullptr || audit_control.get() != control.get() ||
			!activity.valid() || !state_->audit_seal->valid() ||
			alias->phase != sqlite_shm_registry_alias_phase::registered ||
			family_record->phase != sqlite_shm_registry_family_phase::active ||
			!alias->activity_authority_latch ||
			!alias->activity_authority_latch->load(std::memory_order_acquire) ||
			!family_record->activity_authority_latch ||
			!family_record->activity_authority_latch->load(std::memory_order_acquire) ||
			!state_->epoch_arm->retains_exact_validated_receipt(receipt))
			return status::lifecycle_ambiguous;
		return status::exact;
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

	sqlite_shm_reader_attachment_authority::sqlite_shm_reader_attachment_authority(
		std::unique_ptr<detail::sqlite_shm_reader_attachment_authority_state> state) noexcept
		: state_{std::move(state)}
	{
	}

	sqlite_shm_reader_attachment_authority::~sqlite_shm_reader_attachment_authority() noexcept
	{
		if (state_ && state_->activity && state_->activity->control_)
		{
			auto& control = *state_->activity->control_;
			auto expected = detail::sqlite_shm_registry_activity_phase::active;
			if (control.phase.compare_exchange_strong(
					expected,
					detail::sqlite_shm_registry_activity_phase::abandoned,
					std::memory_order_acq_rel,
					std::memory_order_acquire))
			{
				control.authority_valid.store(false, std::memory_order_release);
				if (control.alias_authority_latch)
					control.alias_authority_latch->store(false, std::memory_order_release);
				if (control.family_authority_latch)
					control.family_authority_latch->store(false, std::memory_order_release);
			}
		}
		if (state_ && state_->open && !state_->open->release_descendant())
			state_->open->emergency_latch->store(true, std::memory_order_release);
	}

	sqlite_shm_reader_attachment_authority::sqlite_shm_reader_attachment_authority(
		sqlite_shm_reader_attachment_authority&& other) noexcept
		: state_{std::move(other.state_)}
	{
	}

	bool sqlite_shm_reader_attachment_authority::valid_for_predelegation(
		const sqlite_shm_reader_session_request& request) const noexcept
	{
		return state_ && state_->attachment && state_->open && state_->audit_seal &&
			state_->activity && *state_->attachment == request.attachment &&
			state_->activity->valid() && state_->audit_seal->valid() &&
			state_->open->authority_valid_now() &&
			state_->open->open_token == request.attachment.registry_open_token();
	}

	bool sqlite_shm_reader_attachment_authority::retains_exact_lifetimes(
		const sqlite_shm_reader_attachment_reservation_identity& attachment) const noexcept
	{
		return state_ && state_->attachment && state_->open && state_->audit_seal &&
			state_->activity && *state_->attachment == attachment && state_->activity->valid() &&
			state_->audit_seal->valid() && state_->open->authority_valid_now() &&
			state_->open->open_token == attachment.registry_open_token();
	}

	bool sqlite_shm_reader_attachment_authority::retains_exact_owned_drain_lifetimes(
		const sqlite_shm_reader_attachment_reservation_identity& attachment) const noexcept
	{
		if (!state_ || !state_->attachment || !state_->open || !state_->audit_seal ||
			!state_->activity || *state_->attachment != attachment)
			return false;

		const auto& activity = *state_->activity;
		const auto& open = *state_->open;
		const auto activity_control = activity.control_;
		const auto audit_control = state_->audit_seal->control_.lock();
		const auto activity_registry = activity.state_.lock();
		const auto open_registry = open.registry_state.lock();
		const auto activity_process = activity_control
			? activity_control->process_seal.lock()
			: std::shared_ptr<detail::sqlite_shm_registry_process_owner_seal>{};
		const auto open_process = open.process_seal.lock();
		if (!activity_control || !audit_control || !activity_registry || !open_registry ||
			!activity_process || !open_process)
			return false;

		const auto& binding = open.binding;
		return audit_control.get() == activity_control.get() &&
			activity_control->audit_seal_minted.load(std::memory_order_acquire) &&
			activity_control->phase.load(std::memory_order_acquire) ==
			detail::sqlite_shm_registry_activity_phase::active &&
			open.phase.load(std::memory_order_acquire) ==
			detail::sqlite_shm_reader_open_phase::active &&
			open.authority_valid.load(std::memory_order_acquire) && open.lineage_seal &&
			open.lineage_seal->authority_valid.load(std::memory_order_acquire) &&
			open.descendant_authority_count.load(std::memory_order_acquire) != 0U &&
			activity_registry.get() == open_registry.get() &&
			activity_control->registry_state.lock().get() == activity_registry.get() &&
			activity_process.get() == open_process.get() &&
			activity_process->process_epoch->load(std::memory_order_acquire) ==
			activity_control->process_epoch &&
			activity_control->process_epoch != 0U && activity_control->alias_token != 0U &&
			activity_control->family_epoch != 0U && activity_control->family_pin_token != 0U &&
			activity_control->process_epoch == open.process_epoch &&
			activity_control->alias_token == open.alias_token &&
			activity_control->family_epoch == open.family_epoch &&
			activity_control->family_pin_token == open.family_pin_token &&
			activity_control->activity_token != 0U && open.open_token != 0U &&
			open.open_token == attachment.registry_open_token() &&
			activity_control->emergency_latch.get() == open.emergency_latch.get() &&
			activity_control->alias_authority_latch.get() == open.alias_authority_latch.get() &&
			activity_control->family_authority_latch.get() == open.family_authority_latch.get() &&
			activity_control->process_instance == attachment.family().process_instance &&
			activity_control->family == attachment.family() &&
			activity_control->alias_lifetime == attachment.alias_lifetime() &&
			binding.family == attachment.family() &&
			binding.alias_lifetime == attachment.alias_lifetime() &&
			binding.connection_token == attachment.connection_token() &&
			binding.main_native_file_receipt == attachment.main_native_file_receipt() &&
			binding.main_xopen_receipt == attachment.main_xopen_receipt() &&
			binding.open_epoch == attachment.open_epoch() &&
			binding.callback_cohort == attachment.callback_cohort();
	}

	bool sqlite_shm_reader_attachment_authority::validate_active_authority(
		const sqlite_shm_registry_family_pin& family,
		const sqlite_shm_reader_attachment_reservation_identity& attachment) const noexcept
	{
		if (!retains_exact_lifetimes(attachment))
			return false;
		const auto& activity = *state_->activity;
		const auto control = activity.control_;
		const auto registry = activity.state_.lock();
		const auto audit_control = state_->audit_seal->control_.lock();
		if (!control || !registry || !family.state_)
			return false;
		if (registry.get() != family.state_.get() ||
			control->registry_state.lock().get() != registry.get() ||
			control->process_epoch != family.process_epoch_ ||
			control->alias_token != family.alias_token_ ||
			control->family_epoch != family.family_epoch_ ||
			control->family_pin_token != family.pin_token_ ||
			control->process_instance != attachment.family().process_instance ||
			control->family != attachment.family() ||
			control->alias_lifetime != attachment.alias_lifetime())
			return false;
		const auto* family_pin = registry->current_family_pin_locked(family);
		const auto* alias = registry->find_alias_locked(family.alias_token_);
		const auto* family_record = registry->find_family_epoch_locked(family.family_epoch_);
		const auto* activity_record = registry->current_activity_locked(activity);
		const auto* open_record =
			registry->find_reader_open_locked(attachment.registry_open_token());
		return family_pin != nullptr && alias != nullptr && family_record != nullptr &&
			activity_record != nullptr && open_record != nullptr && open_record->active &&
			open_record->control.get() == state_->open.get() &&
			registry->reader_open_control_matches_record_locked(*open_record) &&
			audit_control.get() == control.get() &&
			alias->runtime_lifetime.pin_identity() == attachment.runtime_lifetime_pin() &&
			alias->phase == sqlite_shm_registry_alias_phase::registered &&
			family_record->phase == sqlite_shm_registry_family_phase::active && activity.valid() &&
			state_->audit_seal->valid();
	}

	void sqlite_shm_reader_attachment_authority::invalidate_activity_for_testing() noexcept
	{
		if (state_ && state_->activity && state_->activity->control_)
			state_->activity->control_->authority_valid.store(false, std::memory_order_release);
	}

	sqlite_shm_lease_result<void>
	sqlite_shm_reader_attachment_authority::release_activity() noexcept
	{
		if (!state_ || !state_->activity)
			return rejection(sqlite_shm_lease_rejection_reason::stale_token);
		const auto registry = state_->activity->state_.lock();
		if (!registry)
			return rejection(sqlite_shm_lease_rejection_reason::stale_token);
		auto released = registry->release_activity(*state_->activity);
		if (released)
		{
			if (state_->open && !state_->open->release_descendant())
			{
				state_->open->emergency_latch->store(true, std::memory_order_release);
				state_.reset();
				return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								 sqlite_shm_lease_recovery_action::quarantine_no_retry);
			}
			state_->open.reset();
			state_.reset();
		}
		return released;
	}

	sqlite_shm_reader_map_predelegate_authority::sqlite_shm_reader_map_predelegate_authority(
		std::unique_ptr<detail::sqlite_shm_reader_map_predelegate_authority_state> state) noexcept
		: state_{std::move(state)}
	{
	}

	sqlite_shm_reader_map_predelegate_authority::
		~sqlite_shm_reader_map_predelegate_authority() noexcept
	{
		if (state_ && state_->activity && state_->activity->control_)
		{
			auto& control = *state_->activity->control_;
			auto expected = detail::sqlite_shm_registry_activity_phase::active;
			if (control.phase.compare_exchange_strong(
					expected,
					detail::sqlite_shm_registry_activity_phase::abandoned,
					std::memory_order_acq_rel,
					std::memory_order_acquire))
			{
				control.authority_valid.store(false, std::memory_order_release);
				if (control.alias_authority_latch)
					control.alias_authority_latch->store(false, std::memory_order_release);
				if (control.family_authority_latch)
					control.family_authority_latch->store(false, std::memory_order_release);
			}
		}
		if (state_ && state_->open && !state_->open->release_descendant())
			state_->open->emergency_latch->store(true, std::memory_order_release);
	}

	sqlite_shm_reader_map_predelegate_authority::sqlite_shm_reader_map_predelegate_authority(
		sqlite_shm_reader_map_predelegate_authority&& other) noexcept
		: state_{std::move(other.state_)}
	{
	}

	bool sqlite_shm_reader_map_predelegate_authority::valid_for_predelegation(
		const sqlite_shm_reader_attachment_map_request& request) const noexcept
	{
		return state_ && state_->request && state_->open && state_->audit_seal &&
			state_->activity && *state_->request == request && state_->activity->valid() &&
			state_->audit_seal->valid() && state_->open->authority_valid_now() &&
			state_->open->open_token == request.expected_attachment.registry_open_token();
	}

	bool sqlite_shm_reader_map_predelegate_authority::validate_active_authority(
		const sqlite_shm_registry_family_pin& family,
		const sqlite_shm_reader_attachment_map_request& request) const noexcept
	{
		if (!valid_for_predelegation(request))
			return false;
		const auto& activity = *state_->activity;
		const auto control = activity.control_;
		const auto registry = activity.state_.lock();
		const auto audit_control = state_->audit_seal->control_.lock();
		if (!control || !registry || !family.state_)
			return false;
		if (registry.get() != family.state_.get() ||
			control->registry_state.lock().get() != registry.get() ||
			control->process_epoch != family.process_epoch_ ||
			control->alias_token != family.alias_token_ ||
			control->family_epoch != family.family_epoch_ ||
			control->family_pin_token != family.pin_token_ ||
			control->process_instance != request.family.process_instance ||
			control->family != request.family || control->alias_lifetime != request.alias_lifetime)
			return false;
		const auto* family_pin = registry->current_family_pin_locked(family);
		const auto* alias = registry->find_alias_locked(family.alias_token_);
		const auto* family_record = registry->find_family_epoch_locked(family.family_epoch_);
		const auto* activity_record = registry->current_activity_locked(activity);
		const auto* open_record =
			registry->find_reader_open_locked(request.expected_attachment.registry_open_token());
		return family_pin != nullptr && alias != nullptr && family_record != nullptr &&
			activity_record != nullptr && open_record != nullptr && open_record->active &&
			open_record->control.get() == state_->open.get() &&
			registry->reader_open_control_matches_record_locked(*open_record) &&
			audit_control.get() == control.get() &&
			alias->runtime_lifetime.pin_identity() ==
			request.expected_attachment.runtime_lifetime_pin() &&
			alias->phase == sqlite_shm_registry_alias_phase::registered &&
			family_record->phase == sqlite_shm_registry_family_phase::active && activity.valid() &&
			state_->audit_seal->valid();
	}

	bool sqlite_shm_reader_map_predelegate_authority::retains_exact_owned_terminal_lifetimes(
		const sqlite_shm_registry_family_pin& family,
		const sqlite_shm_reader_attachment_map_request& request) const noexcept
	{
		if (!state_ || !state_->request || !state_->open || !state_->audit_seal ||
			!state_->activity || *state_->request != request || !family.state_)
			return false;

		const auto& activity = *state_->activity;
		const auto& open = *state_->open;
		const auto activity_control = activity.control_;
		const auto audit_control = state_->audit_seal->control_.lock();
		const auto activity_registry = activity.state_.lock();
		const auto open_registry = open.registry_state.lock();
		const auto activity_process = activity_control
			? activity_control->process_seal.lock()
			: std::shared_ptr<detail::sqlite_shm_registry_process_owner_seal>{};
		const auto open_process = open.process_seal.lock();
		if (!activity_control || !audit_control || !activity_registry || !open_registry ||
			!activity_process || !open_process)
			return false;

		const auto& attachment = request.expected_attachment;
		const auto& binding = open.binding;
		return family.state_.get() == activity_registry.get() &&
			audit_control.get() == activity_control.get() &&
			activity_control->audit_seal_minted.load(std::memory_order_acquire) &&
			activity_control->phase.load(std::memory_order_acquire) ==
			detail::sqlite_shm_registry_activity_phase::active &&
			open.phase.load(std::memory_order_acquire) ==
			detail::sqlite_shm_reader_open_phase::active &&
			open.authority_valid.load(std::memory_order_acquire) && open.lineage_seal &&
			open.lineage_seal->authority_valid.load(std::memory_order_acquire) &&
			open.descendant_authority_count.load(std::memory_order_acquire) != 0U &&
			activity_registry.get() == open_registry.get() &&
			activity_control->registry_state.lock().get() == activity_registry.get() &&
			activity_process.get() == open_process.get() &&
			activity_process->process_epoch->load(std::memory_order_acquire) ==
			activity_control->process_epoch &&
			activity_control->process_epoch != 0U && activity_control->alias_token != 0U &&
			activity_control->family_epoch != 0U && activity_control->family_pin_token != 0U &&
			activity_control->process_epoch == family.process_epoch_ &&
			activity_control->alias_token == family.alias_token_ &&
			activity_control->family_epoch == family.family_epoch_ &&
			activity_control->family_pin_token == family.pin_token_ &&
			activity_control->process_epoch == open.process_epoch &&
			activity_control->alias_token == open.alias_token &&
			activity_control->family_epoch == open.family_epoch &&
			activity_control->family_pin_token == open.family_pin_token &&
			activity_control->activity_token != 0U && open.open_token != 0U &&
			open.open_token == attachment.registry_open_token() &&
			activity_control->emergency_latch.get() == open.emergency_latch.get() &&
			activity_control->alias_authority_latch.get() == open.alias_authority_latch.get() &&
			activity_control->family_authority_latch.get() == open.family_authority_latch.get() &&
			activity_control->process_instance == request.family.process_instance &&
			activity_control->family == request.family &&
			activity_control->alias_lifetime == request.alias_lifetime &&
			binding.family == request.family && binding.alias_lifetime == request.alias_lifetime &&
			binding.connection_token == request.connection_token &&
			binding.main_native_file_receipt == attachment.main_native_file_receipt() &&
			binding.main_xopen_receipt == attachment.main_xopen_receipt() &&
			binding.open_epoch == attachment.open_epoch() &&
			binding.callback_cohort == attachment.callback_cohort();
	}

	bool sqlite_shm_reader_map_predelegate_authority::retains_exact_owned_terminal_lifetimes(
		const void* const process_registry_instance,
		const sqlite_shm_reader_attachment_map_request& request) const noexcept
	{
		if (!state_ || !state_->request || !state_->open || !state_->audit_seal ||
			!state_->activity || *state_->request != request || process_registry_instance == nullptr)
			return false;

		const auto& activity = *state_->activity;
		const auto& open = *state_->open;
		const auto activity_control = activity.control_;
		const auto audit_control = state_->audit_seal->control_.lock();
		const auto activity_registry = activity.state_.lock();
		const auto open_registry = open.registry_state.lock();
		const auto activity_process = activity_control
			? activity_control->process_seal.lock()
			: std::shared_ptr<detail::sqlite_shm_registry_process_owner_seal>{};
		const auto open_process = open.process_seal.lock();
		if (!activity_control || !audit_control || !activity_registry || !open_registry ||
			!activity_process || !open_process)
			return false;

		const auto& attachment = request.expected_attachment;
		const auto& binding = open.binding;
		return process_registry_instance == activity_registry.get() &&
			audit_control.get() == activity_control.get() &&
			activity_control->audit_seal_minted.load(std::memory_order_acquire) &&
			activity_control->phase.load(std::memory_order_acquire) ==
				detail::sqlite_shm_registry_activity_phase::active &&
			open.phase.load(std::memory_order_acquire) ==
				detail::sqlite_shm_reader_open_phase::active &&
			open.authority_valid.load(std::memory_order_acquire) && open.lineage_seal &&
			open.lineage_seal->authority_valid.load(std::memory_order_acquire) &&
			open.descendant_authority_count.load(std::memory_order_acquire) != 0U &&
			activity_registry.get() == open_registry.get() &&
			activity_control->registry_state.lock().get() == activity_registry.get() &&
			activity_process.get() == open_process.get() &&
			activity_process->process_epoch->load(std::memory_order_acquire) ==
				activity_control->process_epoch &&
			activity_control->process_epoch != 0U && activity_control->alias_token != 0U &&
			activity_control->family_epoch != 0U && activity_control->family_pin_token != 0U &&
			activity_control->process_epoch == open.process_epoch &&
			activity_control->alias_token == open.alias_token &&
			activity_control->family_epoch == open.family_epoch &&
			activity_control->family_pin_token == open.family_pin_token &&
			activity_control->activity_token != 0U && open.open_token != 0U &&
			open.open_token == attachment.registry_open_token() &&
			activity_control->emergency_latch.get() == open.emergency_latch.get() &&
			activity_control->alias_authority_latch.get() == open.alias_authority_latch.get() &&
			activity_control->family_authority_latch.get() == open.family_authority_latch.get() &&
			activity_control->process_instance == request.family.process_instance &&
			activity_control->family == request.family &&
			activity_control->alias_lifetime == request.alias_lifetime &&
			binding.family == request.family && binding.alias_lifetime == request.alias_lifetime &&
			binding.connection_token == request.connection_token &&
			binding.main_native_file_receipt == attachment.main_native_file_receipt() &&
			binding.main_xopen_receipt == attachment.main_xopen_receipt() &&
			binding.open_epoch == attachment.open_epoch() &&
			binding.callback_cohort == attachment.callback_cohort();
	}

	void sqlite_shm_reader_map_predelegate_authority::invalidate_activity_for_testing() noexcept
	{
		if (state_ && state_->activity && state_->activity->control_)
			state_->activity->control_->authority_valid.store(false, std::memory_order_release);
	}

	sqlite_shm_lease_result<void>
	sqlite_shm_reader_map_predelegate_authority::release_activity() noexcept
	{
		if (!state_ || !state_->activity)
			return rejection(sqlite_shm_lease_rejection_reason::stale_token);
		const auto registry = state_->activity->state_.lock();
		if (!registry)
			return rejection(sqlite_shm_lease_rejection_reason::stale_token);
		auto released = registry->release_activity(*state_->activity);
		if (released)
		{
			if (state_->open && !state_->open->release_descendant())
			{
				state_->open->emergency_latch->store(true, std::memory_order_release);
				state_.reset();
				return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								 sqlite_shm_lease_recovery_action::quarantine_no_retry);
			}
			state_->open.reset();
			state_.reset();
		}
		return released;
	}

	sqlite_shm_reader_session_admission::sqlite_shm_reader_session_admission(
		const sqlite_shm_reader_session_admission_kind kind,
		std::optional<sqlite_shm_reader_session_request> proposal_request,
		std::optional<sqlite_shm_reader_session> session,
		std::optional<sqlite_shm_lease_rejection> rejection_value) noexcept
		: kind_{kind}, proposal_request_{std::move(proposal_request)}, session_{std::move(session)},
		  rejection_{std::move(rejection_value)}
	{
	}

	sqlite_shm_reader_session_admission::sqlite_shm_reader_session_admission(
		sqlite_shm_reader_session_admission&& other) noexcept
		: kind_{other.kind_}, proposal_request_{std::move(other.proposal_request_)},
		  session_{std::move(other.session_)}, rejection_{std::move(other.rejection_)}
	{
	}

	sqlite_shm_reader_session_admission_kind
	sqlite_shm_reader_session_admission::kind() const noexcept
	{
		return kind_;
	}

	bool sqlite_shm_reader_session_admission::has_proposal_custody() const noexcept
	{
		return (kind_ == sqlite_shm_reader_session_admission_kind::active_group_owner_admitted ||
				kind_ ==
					sqlite_shm_reader_session_admission_kind::
						reserved_for_local_proposal_candidate) &&
			proposal_request_.has_value() && session_.has_value();
	}

	const std::optional<sqlite_shm_reader_session_request>&
	sqlite_shm_reader_session_admission::proposal_request() const noexcept
	{
		return proposal_request_;
	}

	const std::optional<sqlite_shm_lease_rejection>&
	sqlite_shm_reader_session_admission::rejection() const noexcept
	{
		return rejection_;
	}

	std::optional<sqlite_shm_reader_session>
	sqlite_shm_reader_session_admission::take_session() noexcept
	{
		return std::exchange(session_, std::nullopt);
	}

	sqlite_shm_registry_process_owner::sqlite_shm_registry_process_owner(
		sqlite_backend_opaque_identity process_instance)
		: process_instance_{std::move(process_instance)},
		  seal_{std::make_shared<detail::sqlite_shm_registry_process_owner_seal>()},
		  process_epoch_{seal_->process_epoch->load(std::memory_order_acquire)}
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
			seal_->process_epoch->load(std::memory_order_acquire) == process_epoch_ &&
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
			process_seal_->process_epoch->load(std::memory_order_acquire) == process_epoch_ &&
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
		  pin_token_{value.pin_token},
		  identity_authority_latch_{std::move(value.identity_authority_latch)}
	{
	}

	sqlite_shm_registry_family_pin::~sqlite_shm_registry_family_pin() noexcept
	{
		if (identity_authority_latch_)
			identity_authority_latch_->store(false, std::memory_order_release);
		if (state_)
			state_->abandon_family(*this);
	}

	sqlite_shm_registry_family_pin::sqlite_shm_registry_family_pin(
		sqlite_shm_registry_family_pin&& other) noexcept
		: state_{std::move(other.state_)}, process_epoch_{std::exchange(other.process_epoch_, 0U)},
		  alias_token_{std::exchange(other.alias_token_, 0U)},
		  family_epoch_{std::exchange(other.family_epoch_, 0U)},
		  pin_token_{std::exchange(other.pin_token_, 0U)},
		  identity_authority_latch_{std::move(other.identity_authority_latch_)}
	{
	}

	bool sqlite_shm_registry_family_pin::valid() const noexcept
	{
		return state_ && pin_token_ != 0U && identity_authority_latch_ &&
			identity_authority_latch_->load(std::memory_order_acquire) &&
			state_->family_pin_valid(*this);
	}

	void sqlite_shm_registry_family_pin::disarm() noexcept
	{
		if (identity_authority_latch_)
			identity_authority_latch_->store(false, std::memory_order_release);
		identity_authority_latch_.reset();
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
		std::shared_ptr<detail::sqlite_shm_mapping_registry_state> state)
		: state_{std::move(state)},
		  identity_issuer_owner_latch_{std::make_shared<std::atomic_bool>(true)},
		  identity_issuer_state_{detail::make_identity_issuer_state_for_registry(
			  std::weak_ptr<void>{state_},
			  state_->process_epoch_latch_for_identity_issuer(),
			  state_->registry_quarantine_latch_for_identity_issuer(),
			  identity_issuer_owner_latch_,
			  state_->process_epoch_for_identity_issuer(),
			  state_->process_instance_for_identity_issuer(),
			  1U)}
	{
		if (!identity_issuer_state_)
			throw std::bad_alloc{};
	}

	sqlite_same_process_shm_mapping_registry::~sqlite_same_process_shm_mapping_registry() noexcept
	{
		identity_issuer_owner_latch_->store(false, std::memory_order_release);
	}

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
			owner.seal_->process_epoch->load(std::memory_order_acquire) != process_epoch)
			return rejection(sqlite_shm_lease_rejection_reason::stale_token);
		bool expected = false;
		if (!owner.seal_->claimed.compare_exchange_strong(
				expected, true, std::memory_order_acq_rel, std::memory_order_acquire))
			return rejection(sqlite_shm_lease_rejection_reason::stale_token);
		if (owner.seal_->process_epoch->load(std::memory_order_acquire) != process_epoch)
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

	sqlite_shm_lease_result<sqlite_shm_reader_session_admission>
	sqlite_same_process_shm_mapping_registry::admit_reader_session_before_sqlite(
		sqlite_shm_registry_family_pin& family,
		const sqlite_shm_reader_open_authority& open,
		const sqlite_shm_reader_pre_sqlite_session_request& request)
	{
		return state_->admit_reader_session_before_sqlite(family, open, request);
	}

	sqlite_shm_lease_result<sqlite_shm_mapping_tuple>
	sqlite_same_process_shm_mapping_registry::authenticate_reader_cached_member_use(
		sqlite_shm_registry_family_pin& family,
		const sqlite_shm_reader_session& session,
		const sqlite_shm_reader_cached_member_identity& member) noexcept
	{
		return state_->authenticate_reader_cached_member_use(family, session, member);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_attachment_map_inflight>
	sqlite_same_process_shm_mapping_registry::begin_reader_map(
		sqlite_shm_registry_family_pin& family,
		sqlite_shm_reader_session& session,
		const sqlite_shm_reader_attachment_map_request& request)
	{
		return state_->begin_reader_map(family, session, request);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_attachment_map_prepared>
	sqlite_same_process_shm_mapping_registry::prepare_reader_map_identity(
		sqlite_shm_registry_family_pin& family,
		sqlite_shm_reader_session& session,
		const sqlite_shm_reader_attachment_map_pre_request& request)
	{
		return state_->prepare_reader_map_identity(family, session, request);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_lifecycle_identity_scope>
	sqlite_same_process_shm_mapping_registry::claim_reader_map_identity_scope(
		sqlite_shm_registry_family_pin& family,
		const sqlite_shm_reader_attachment_map_prepared& prepared,
		const sqlite_shm_reader_attachment_map_pre_request& request)
	{
		return state_->claim_reader_map_identity_scope(
			family, prepared, request, identity_issuer_state_);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_attachment_map_inflight>
	sqlite_same_process_shm_mapping_registry::bind_reader_map_identity(
		sqlite_shm_registry_family_pin& family,
		sqlite_shm_reader_session& session,
		sqlite_shm_reader_attachment_map_prepared& prepared,
		const sqlite_shm_reader_lifecycle_identity_scope& scope,
		const sqlite_shm_issued_reader_callback_identity& callback)
	{
		return state_->bind_reader_map_identity(
			family, session, prepared, scope, callback, identity_issuer_state_);
	}

	sqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_zero_effect_receipt>
	sqlite_same_process_shm_mapping_registry::validate_reader_zero_attachment_effect(
		sqlite_shm_registry_family_pin& family,
		const sqlite_shm_reader_attachment_map_inflight& inflight,
		const sqlite_shm_reader_lifecycle_identity_scope& scope,
		const sqlite_shm_issued_reader_callback_identity& callback,
		const sqlite_shm_issued_reader_effect_identity& effect,
		const int native_status,
		const volatile void* native_mapping,
		const int delegated_extend) noexcept
	{
		return state_->validate_reader_zero_attachment_effect(family,
			inflight,
			scope,
			callback,
			effect,
			native_status,
			native_mapping,
			delegated_extend,
			identity_issuer_state_);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_late_close_outer_unwind_authority>
	sqlite_same_process_shm_mapping_registry::mint_reader_late_close_outer_unwind_authority(
		sqlite_shm_registry_family_pin& family,
		sqlite_shm_reader_attachment_map_inflight& inflight,
		sqlite_shm_reader_session& session,
		const sqlite_shm_callback_execution_receipt& expected_outer_unmap_callback)
	{
		return state_->mint_reader_late_close_outer_unwind_authority(
			family, inflight, session, expected_outer_unmap_callback);
	}

	sqlite_shm_lease_result<void>
	sqlite_same_process_shm_mapping_registry::mark_reader_late_close_native_map_start(
		sqlite_shm_registry_family_pin& family,
		sqlite_shm_reader_attachment_map_inflight& inflight,
		const sqlite_shm_reader_session& session,
		const sqlite_shm_reader_late_close_outer_unwind_authority& owner) noexcept
	{
		return state_->mark_reader_late_close_native_map_start(
			family, inflight, session, owner);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_map_commit>
	sqlite_same_process_shm_mapping_registry::commit_reader_map(
		sqlite_shm_registry_family_pin& family,
		sqlite_shm_reader_attachment_map_inflight& inflight,
		const sqlite_shm_verified_reader_attachment_post_map_receipt& receipt,
		sqlite_shm_reader_session& session)
	{
		return state_->commit_reader_map(family, inflight, receipt, session);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_attachment_zero_effect_result>
	sqlite_same_process_shm_mapping_registry::complete_reader_zero_attachment_map(
		sqlite_shm_registry_family_pin& family,
		sqlite_shm_reader_attachment_map_inflight& inflight,
		const sqlite_shm_verified_reader_attachment_zero_effect_receipt& receipt,
		sqlite_shm_reader_session& session)
	{
		return state_->complete_reader_zero_attachment_map(family, inflight, receipt, session);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_opaque_attachment_uncertainty_result>
	sqlite_same_process_shm_mapping_registry::complete_reader_opaque_attachment_uncertainty(
		sqlite_shm_registry_family_pin& family,
		sqlite_shm_reader_attachment_map_inflight& inflight,
		sqlite_shm_reader_session& session)
	{
		return state_->complete_reader_opaque_attachment_uncertainty(family, inflight, session);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_predecessor_map_result>
	sqlite_same_process_shm_mapping_registry::complete_reader_predecessor_map(
		sqlite_shm_registry_family_pin& family,
		sqlite_shm_reader_attachment_map_inflight& inflight,
		const sqlite_shm_verified_reader_predecessor_map_receipt& receipt,
		sqlite_shm_reader_session& session)
	{
		return state_->complete_reader_predecessor_map(family, inflight, receipt, session);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_existing_group_predecessor_mismatch_result>
	sqlite_same_process_shm_mapping_registry::complete_reader_existing_group_predecessor_mismatch(
		sqlite_shm_registry_family_pin& family,
		sqlite_shm_reader_attachment_map_inflight& inflight,
		const sqlite_shm_verified_reader_predecessor_map_receipt& receipt,
		sqlite_shm_reader_session& session)
	{
		return state_->complete_reader_existing_group_predecessor_mismatch(
			family, inflight, receipt, session);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_predecessor_unmap_terminal_result>
	sqlite_same_process_shm_mapping_registry::complete_reader_predecessor_unmap(
		sqlite_shm_registry_family_pin& family,
		const sqlite_shm_reader_open_authority& open,
		const sqlite_shm_verified_reader_predecessor_unmap_terminal_receipt& receipt) noexcept
	{
		return state_->complete_reader_predecessor_unmap(family, open, receipt);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_unpublished_cleanup_obligation>
	sqlite_same_process_shm_mapping_registry::begin_reader_unpublished_cleanup(
		sqlite_shm_registry_family_pin& family,
		sqlite_shm_reader_attachment_map_inflight& inflight,
		const sqlite_shm_verified_reader_unpublished_cleanup_receipt& receipt,
		sqlite_shm_reader_session& session)
	{
		return state_->begin_reader_unpublished_cleanup(family, inflight, receipt, session);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_unpublished_cleanup_terminal_result>
	sqlite_same_process_shm_mapping_registry::complete_reader_unpublished_cleanup(
		sqlite_shm_registry_family_pin& family,
		sqlite_shm_reader_unpublished_cleanup_obligation& cleanup,
		const sqlite_shm_verified_reader_unpublished_cleanup_terminal_receipt& receipt) noexcept
	{
		return state_->complete_reader_unpublished_cleanup(family, cleanup, receipt);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_logical_ack_result>
	sqlite_same_process_shm_mapping_registry::consume_reader_logical_ack(
		sqlite_shm_registry_family_pin& family,
		const sqlite_shm_reader_open_authority& open,
		const sqlite_shm_reader_logical_ack_request& request) noexcept
	{
		return state_->consume_reader_logical_ack(family, open, request);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_late_close_logical_ack_result>
	sqlite_same_process_shm_mapping_registry::consume_reader_late_close_logical_ack(
		sqlite_shm_registry_family_pin& family,
		sqlite_shm_reader_late_close_outer_unwind_authority& owner,
		const sqlite_shm_reader_logical_ack_request& request) noexcept
	{
		return state_->consume_reader_late_close_logical_ack(family, owner, request);
	}

	sqlite_shm_lease_result<void> sqlite_same_process_shm_mapping_registry::complete_reader_session(
		sqlite_shm_registry_family_pin& family,
		sqlite_shm_reader_session& session,
		const sqlite_shm_reader_session_terminal_receipt& receipt) noexcept
	{
		return state_->complete_reader_session(family, session, receipt);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_unmap_obligation>
	sqlite_same_process_shm_mapping_registry::begin_reader_unmap(
		sqlite_shm_registry_family_pin& family,
		sqlite_shm_reader_handoff& handoff,
		const sqlite_shm_callback_execution_receipt& callback) noexcept
	{
		return state_->begin_reader_unmap(family, handoff, callback);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_unmap_cut_result>
	sqlite_same_process_shm_mapping_registry::poll_reader_unmap_cut(
		sqlite_shm_registry_family_pin& family,
		sqlite_shm_reader_unmap_obligation& unmap,
		const sqlite_shm_callback_execution_receipt& callback) noexcept
	{
		return state_->poll_reader_unmap_cut(family, unmap, callback);
	}

	sqlite_shm_lease_result<void>
	sqlite_same_process_shm_mapping_registry::fail_reader_unmap_cut_wait(
		sqlite_shm_registry_family_pin& family,
		sqlite_shm_reader_unmap_obligation& unmap,
		const sqlite_shm_callback_execution_receipt& callback,
		const sqlite_shm_retirement_wait_failure failure) noexcept
	{
		return state_->fail_reader_unmap_cut_wait(family, unmap, callback, failure);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_unmap_terminal_result>
	sqlite_same_process_shm_mapping_registry::complete_reader_unmap(
		sqlite_shm_registry_family_pin& family,
		sqlite_shm_reader_unmap_obligation& unmap,
		const sqlite_shm_verified_reader_unmap_terminal_receipt& receipt) noexcept
	{
		return state_->complete_reader_unmap(family, unmap, receipt);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_close_obligation>
	sqlite_same_process_shm_mapping_registry::begin_reader_close(
		sqlite_shm_registry_family_pin& family,
		const sqlite_shm_reader_open_authority& open,
		const sqlite_shm_reader_close_request& request) noexcept
	{
		return state_->begin_reader_close(family, open, request);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_close_cut_result>
	sqlite_same_process_shm_mapping_registry::poll_reader_close_cut(
		sqlite_shm_registry_family_pin& family,
		const sqlite_shm_reader_open_authority& open,
		sqlite_shm_reader_close_obligation& close,
		const sqlite_shm_callback_execution_receipt& close_callback) noexcept
	{
		return state_->poll_reader_close_cut(family, open, close, close_callback);
	}

	sqlite_shm_lease_result<void>
	sqlite_same_process_shm_mapping_registry::fail_reader_close_cut_wait(
		sqlite_shm_registry_family_pin& family,
		const sqlite_shm_reader_open_authority& open,
		sqlite_shm_reader_close_obligation& close,
		const sqlite_shm_callback_execution_receipt& close_callback,
		const sqlite_shm_retirement_wait_failure failure) noexcept
	{
		return state_->fail_reader_close_cut_wait(family, open, close, close_callback, failure);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_close_terminal_result>
	sqlite_same_process_shm_mapping_registry::complete_reader_close(
		sqlite_shm_registry_family_pin& family,
		const sqlite_shm_reader_open_authority& open,
		sqlite_shm_reader_close_obligation& close,
		const sqlite_shm_verified_reader_close_terminal_receipt& receipt) noexcept
	{
		return state_->complete_reader_close(family, open, close, receipt);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_live_close_obligation>
	sqlite_same_process_shm_mapping_registry::begin_reader_live_close(
		sqlite_shm_registry_family_pin& family,
		const sqlite_shm_reader_open_authority& open,
		sqlite_shm_reader_handoff& handoff,
		const sqlite_shm_reader_unmap_request& unmap_request,
		const sqlite_shm_reader_close_request& close_request) noexcept
	{
		return state_->begin_reader_live_close(family, open, handoff, unmap_request, close_request);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_unmap_cut_result>
	sqlite_same_process_shm_mapping_registry::poll_reader_live_close_unmap_cut(
		sqlite_shm_registry_family_pin& family,
		const sqlite_shm_reader_open_authority& open,
		sqlite_shm_reader_live_close_obligation& close,
		const sqlite_shm_callback_execution_receipt& close_callback) noexcept
	{
		return state_->poll_reader_live_close_unmap_cut(family, open, close, close_callback);
	}

	sqlite_shm_lease_result<void>
	sqlite_same_process_shm_mapping_registry::fail_reader_live_close_unmap_cut_wait(
		sqlite_shm_registry_family_pin& family,
		const sqlite_shm_reader_open_authority& open,
		sqlite_shm_reader_live_close_obligation& close,
		const sqlite_shm_callback_execution_receipt& close_callback,
		const sqlite_shm_retirement_wait_failure failure) noexcept
	{
		return state_->fail_reader_live_close_unmap_cut_wait(
			family, open, close, close_callback, failure);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_unmap_terminal_result>
	sqlite_same_process_shm_mapping_registry::complete_reader_live_close_unmap(
		sqlite_shm_registry_family_pin& family,
		const sqlite_shm_reader_open_authority& open,
		sqlite_shm_reader_live_close_obligation& close,
		const sqlite_shm_verified_reader_unmap_terminal_receipt& receipt) noexcept
	{
		return state_->complete_reader_live_close_unmap(family, open, close, receipt);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_close_terminal_result>
	sqlite_same_process_shm_mapping_registry::complete_reader_live_close(
		sqlite_shm_registry_family_pin& family,
		const sqlite_shm_reader_open_authority& open,
		sqlite_shm_reader_live_close_obligation& close,
		const sqlite_shm_verified_reader_close_terminal_receipt& receipt) noexcept
	{
		return state_->complete_reader_live_close(family, open, close, receipt);
	}

	sqlite_shm_lease_result<sqlite_shm_pending_mapping>
	sqlite_same_process_shm_mapping_registry::install_writer_pending(
		sqlite_shm_registry_family_pin& family,
		sqlite_shm_writer_post_native_mapping& post_native,
		const sqlite_shm_verified_writer_post_map_receipt& receipt)
	{
		return state_->install_writer_pending(family, post_native, receipt);
	}

	sqlite_shm_lease_result<sqlite_shm_positive_writer_attachment_gate_result>
	sqlite_same_process_shm_mapping_registry::advance_positive_writer_attachment_gate(
		sqlite_shm_registry_family_pin& family,
		const sqlite_shm_native_attachment_identity& attachment,
		const std::span<sqlite_shm_pending_mapping*> pending,
		const sqlite_shm_writer_eligibility& eligibility)
	{
		return state_->advance_positive_writer_attachment_gate(
			family, attachment, pending, eligibility);
	}

	sqlite_shm_lease_result<sqlite_shm_writer_holder> sqlite_same_process_shm_mapping_registry::
		complete_gate_winning_writer_map_before_callback_return(
			sqlite_shm_registry_family_pin& family,
			sqlite_shm_writer_post_native_mapping& post_native,
			const sqlite_shm_verified_writer_post_map_receipt& receipt)
	{
		return state_->complete_gate_winning_writer_map_before_callback_return(
			family, post_native, receipt);
	}

	sqlite_shm_lease_result<void> sqlite_same_process_shm_mapping_registry::release_activity(
		sqlite_shm_registry_activity_pin& activity) noexcept
	{
		return state_->release_activity(activity);
	}

	sqlite_shm_lease_result<void> sqlite_same_process_shm_mapping_registry::release_reader_open(
		sqlite_shm_reader_open_authority& open) noexcept
	{
		return state_->release_reader_open(open);
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

	sqlite_shm_lease_result<sqlite_shm_reader_open_authority>
	sqlite_same_process_shm_mapping_registry::acquire_reader_open_for_testing(
		sqlite_shm_registry_family_pin& family, const sqlite_shm_reader_open_binding& binding)
	{
		return state_->acquire_reader_open(family, binding);
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

	sqlite_shm_registry_reader_open_epoch_test_view
	sqlite_same_process_shm_mapping_registry::reader_open_epoch_view_for_testing(
		const sqlite_shm_reader_open_authority& open) const noexcept
	{
		return state_ ? state_->reader_open_epoch_view(open)
					  : sqlite_shm_registry_reader_open_epoch_test_view{};
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

	const void* sqlite_same_process_shm_mapping_registry::
		reader_lifecycle_sequence_source_identity_for_testing() const noexcept
	{
		return state_ ? state_->reader_lifecycle_sequence_source_identity() : nullptr;
	}

	std::uint64_t
	sqlite_same_process_shm_mapping_registry::reader_lifecycle_last_issued_sequence_for_testing()
		const noexcept
	{
		return state_ ? state_->reader_lifecycle_last_issued_sequence() : 0U;
	}

	void sqlite_same_process_shm_mapping_registry::
		exhaust_reader_lifecycle_sequence_source_for_testing() noexcept
	{
		if (state_)
			state_->exhaust_reader_lifecycle_sequence_source();
	}

	std::size_t
	sqlite_same_process_shm_mapping_registry::retired_reader_lifecycle_tombstone_count_for_testing()
		const noexcept
	{
		return state_ ? state_->retired_reader_lifecycle_tombstone_count() : 0U;
	}

	bool sqlite_same_process_shm_mapping_registry::
		retired_reader_lifecycle_tombstone_matches_for_testing(
			const sqlite_shm_reader_attachment_reservation_identity& attachment) const noexcept
	{
		return state_ && state_->retired_reader_lifecycle_tombstone_matches(attachment);
	}

	std::size_t sqlite_same_process_shm_mapping_registry::
		retired_reader_open_epoch_close_tombstone_count_for_testing() const noexcept
	{
		return state_ ? state_->retired_reader_open_epoch_close_tombstone_count() : 0U;
	}

	bool sqlite_same_process_shm_mapping_registry::
		retired_reader_open_epoch_close_tombstone_matches_for_testing(
			const std::uint64_t registry_open_token,
			const sqlite_shm_reader_open_epoch_binding& binding) const noexcept
	{
		return state_ &&
			state_->retired_reader_open_epoch_close_tombstone_matches(registry_open_token, binding);
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

	sqlite_shm_process_global_identity_issuer
	sqlite_same_process_shm_mapping_registry::identity_issuer_for_testing() const noexcept
	{
		return sqlite_shm_process_global_identity_issuer{
			identity_issuer_state_,
			state_->process_epoch_latch_for_identity_issuer(),
			identity_issuer_owner_latch_,
			state_->process_epoch_for_identity_issuer()};
	}

	sqlite_shm_reader_lifecycle_identity_scope sqlite_same_process_shm_mapping_registry::
		seal_reader_lifecycle_identity_scope_for_testing(
			const sqlite_shm_registry_family_pin& family,
			const sqlite_backend_opaque_identity& callback_cohort,
			const sqlite_backend_opaque_identity& request_seal,
			const std::uint64_t registry_open_token,
			const sqlite_shm_reader_lifecycle_owner_kind owner_kind,
			const std::uint64_t lifecycle_owner_token,
			const std::uint64_t writer_mapping_generation) const
	{
		const auto exact_family = state_->family_binding_for_identity_scope(family);
		if (!exact_family || family.state_.get() != state_.get() ||
			!family.identity_authority_latch_ ||
			!family.identity_authority_latch_->load(std::memory_order_acquire))
			return detail::seal_identity_scope_for_registry(
				identity_issuer_state_,
				{},
				{},
				0U,
				0U,
				{},
				{},
				{});
		return detail::seal_identity_scope_for_registry(
			identity_issuer_state_,
			*exact_family,
			family.identity_authority_latch_,
			family.family_epoch_,
			family.pin_token_,
			callback_cohort,
			request_seal,
			{registry_open_token, owner_kind, lifecycle_owner_token, writer_mapping_generation});
	}

	void sqlite_same_process_shm_mapping_registry::exhaust_identity_issuer_for_testing() noexcept
	{
		detail::exhaust_identity_issuer_for_registry(identity_issuer_state_);
	}

	std::size_t sqlite_same_process_shm_mapping_registry::
		reader_map_identity_phase_count_for_testing(
			const sqlite_shm_lease_family_binding& family) const noexcept
	{
		return state_ ? state_->reader_map_identity_phase_count(family) : 0U;
	}
} // namespace cxxlens::sdk
