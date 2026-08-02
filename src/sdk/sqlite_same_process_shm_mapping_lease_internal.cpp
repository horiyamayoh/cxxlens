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

		constexpr int sqlite_ioerr_status = 10;
		constexpr int sqlite_readonly_status = 8;
		constexpr int sqlite_readonly_cantinit_status = sqlite_readonly_status | (5 << 8);
		constexpr int sqlite_primary_status_mask = 0xff;
		constexpr int sqlite_error_primary_status_first = 1;
		constexpr int sqlite_error_primary_status_last = 26;

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

		[[nodiscard]] bool valid_reader_open_epoch_binding(
			const sqlite_shm_reader_open_epoch_binding& binding) noexcept
		{
			return valid_family(binding.family) && valid_identity(binding.runtime_lifetime_pin) &&
				valid_identity(binding.alias_lifetime) &&
				valid_identity(binding.connection_token) &&
				valid_identity(binding.main_native_file_receipt) &&
				valid_identity(binding.main_xopen_receipt) && valid_identity(binding.open_epoch) &&
				valid_identity(binding.callback_cohort);
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

		[[nodiscard]] bool valid_reader_native_attachment(
			const sqlite_shm_reader_attachment_reservation_identity& attachment) noexcept
		{
			return valid_family(attachment.family()) &&
				valid_identity(attachment.runtime_lifetime_pin()) &&
				valid_identity(attachment.alias_lifetime()) &&
				valid_identity(attachment.connection_token()) &&
				valid_identity(attachment.main_native_file_receipt()) &&
				valid_identity(attachment.main_xopen_receipt()) &&
				valid_identity(attachment.open_epoch()) &&
				attachment.writer_mapping_generation() != 0U &&
				valid_identity(attachment.callback_cohort()) &&
				valid_identity(attachment.attachment_epoch());
		}

		[[nodiscard]] bool valid_observed_reader_native_attachment(
			const sqlite_shm_reader_native_attachment_identity& attachment) noexcept
		{
			return valid_reader_native_attachment(attachment.expected()) &&
				valid_identity(attachment.observed_shm_object_receipt()) &&
				valid_identity(attachment.observed_shm_entry_receipt()) &&
				valid_identity(attachment.observed_device_receipt()) &&
				valid_identity(attachment.observed_mount_receipt());
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

		[[nodiscard]] bool valid_reader_attachment_map_request(
			const sqlite_shm_reader_attachment_map_request& request) noexcept
		{
			return valid_family(request.family) && valid_identity(request.alias_lifetime) &&
				valid_identity(request.connection_token) &&
				valid_reader_native_attachment(request.expected_attachment) &&
				request.expected_attachment.family() == request.family &&
				request.expected_attachment.alias_lifetime() == request.alias_lifetime &&
				request.expected_attachment.connection_token() == request.connection_token &&
				valid_callback(request.callback) && request.page_number >= 0 &&
				request.page_size > 0 && request.caller_extend == 0;
		}

		[[nodiscard]] bool valid_reader_zero_attachment_receipt(
			const sqlite_shm_verified_reader_attachment_zero_effect_receipt& receipt) noexcept
		{
			if (!valid_reader_attachment_map_request(receipt.request()) ||
				receipt.delegated_extend() != 0 ||
				!valid_identity(receipt.zero_attachment_effect_receipt()) ||
				receipt.native_status() < 0)
				return false;

			const auto native_status = receipt.native_status();
			const auto primary_status = native_status & sqlite_primary_status_mask;
			const auto closed_error_status = primary_status >= sqlite_error_primary_status_first &&
				primary_status <= sqlite_error_primary_status_last;
			switch (receipt.kind())
			{
				case sqlite_shm_reader_attachment_zero_effect_kind::exact_no_attachment_change:
					return closed_error_status && primary_status != sqlite_readonly_status &&
						receipt.native_mapping() == nullptr;
				case sqlite_shm_reader_attachment_zero_effect_kind::
					exact_protocol_invalid_no_attachment:
					return (native_status == static_cast<int>(sqlite_native_map_status::ok) &&
							receipt.native_mapping() == nullptr) ||
						(primary_status == sqlite_readonly_status &&
						 native_status != sqlite_readonly_status &&
						 native_status != sqlite_readonly_cantinit_status &&
						 receipt.native_mapping() == nullptr) ||
						(native_status == sqlite_readonly_cantinit_status &&
						 receipt.native_mapping() != nullptr) ||
						(closed_error_status && primary_status != sqlite_readonly_status &&
						 receipt.native_mapping() != nullptr);
			}
			return false;
		}

		[[nodiscard]] bool valid_reader_predecessor_map_receipt(
			const sqlite_shm_verified_reader_predecessor_map_receipt& receipt) noexcept
		{
			if (!valid_reader_attachment_map_request(receipt.request()) ||
				receipt.delegated_extend() != 0 || !valid_identity(receipt.native_effect_receipt()))
				return false;

			switch (receipt.kind())
			{
				case sqlite_shm_reader_predecessor_map_kind::exact_predecessor_no_attachment_route:
					return (receipt.native_status() == sqlite_readonly_status ||
							receipt.native_status() == sqlite_readonly_cantinit_status) &&
						receipt.native_mapping() == nullptr && !receipt.observed_attachment();
				case sqlite_shm_reader_predecessor_map_kind::exact_predecessor_mapped_route:
					return receipt.native_status() == sqlite_readonly_status &&
						receipt.native_mapping() != nullptr && receipt.observed_attachment() &&
						valid_observed_reader_native_attachment(*receipt.observed_attachment()) &&
						receipt.observed_attachment()->expected() ==
						receipt.request().expected_attachment;
			}
			return false;
		}

		[[nodiscard]] bool valid_closed_sqlite_status(const int status) noexcept
		{
			if (status < 0)
				return false;
			if (status == static_cast<int>(sqlite_native_map_status::ok))
				return true;
			const auto primary_status = status & sqlite_primary_status_mask;
			return primary_status >= sqlite_error_primary_status_first &&
				primary_status <= sqlite_error_primary_status_last;
		}

		[[nodiscard]] bool valid_reader_predecessor_unmap_terminal_receipt(
			const sqlite_shm_verified_reader_predecessor_unmap_terminal_receipt& receipt) noexcept
		{
			if (!valid_callback(receipt.callback()) || receipt.caller_delete_flag() != 0 ||
				receipt.delegated_delete_flag() != 0)
				return false;
			switch (receipt.evidence_kind())
			{
				case sqlite_shm_reader_unmap_evidence_kind::exact_native_result:
					return receipt.native_status() &&
						valid_closed_sqlite_status(*receipt.native_status()) &&
						receipt.native_effect_receipt() &&
						valid_identity(*receipt.native_effect_receipt());
				case sqlite_shm_reader_unmap_evidence_kind::throw_or_unknown:
					return !receipt.native_status() && !receipt.native_effect_receipt();
			}
			return false;
		}

		[[nodiscard]] bool valid_reader_unpublished_cleanup_receipt(
			const sqlite_shm_verified_reader_unpublished_cleanup_receipt& receipt) noexcept
		{
			if (!valid_reader_attachment_map_request(receipt.request()) ||
				!valid_callback(receipt.session_request().execution) ||
				!valid_identity(receipt.session_request().read_transaction_epoch) ||
				!valid_identity(receipt.session_request().decode_attempt) ||
				!valid_identity(receipt.session_request().authority_read_receipt) ||
				receipt.generation() == 0U ||
				!valid_closed_sqlite_status(receipt.native_status()) ||
				receipt.delegated_extend() != 0 ||
				!valid_observed_reader_native_attachment(receipt.observed_attachment()) ||
				receipt.observed_attachment().expected() != receipt.request().expected_attachment ||
				receipt.session_request().attachment != receipt.request().expected_attachment ||
				!valid_identity(receipt.mapped_effect_receipt()) ||
				receipt.mapped_effect_receipt() == receipt.request().callback.invocation_token ||
				!valid_identity(receipt.session_no_pointer_terminal_receipt()) ||
				receipt.session_no_pointer_terminal_receipt() == receipt.mapped_effect_receipt() ||
				receipt.session_no_pointer_terminal_receipt() ==
					receipt.request().callback.invocation_token)
				return false;
			switch (receipt.kind())
			{
				case sqlite_shm_reader_unpublished_cleanup_entry_kind::
					exact_mapped_validation_failure:
					return receipt.native_status() ==
						static_cast<int>(sqlite_native_map_status::ok) &&
						receipt.native_mapping() != nullptr;
				case sqlite_shm_reader_unpublished_cleanup_entry_kind::
					exact_protocol_invalid_mapped:
				{
					const auto native_status = receipt.native_status();
					const auto primary_status = native_status & sqlite_primary_status_mask;
					if (receipt.native_mapping() != nullptr)
						return native_status != static_cast<int>(sqlite_native_map_status::ok) &&
							native_status != sqlite_readonly_status;
					return native_status == static_cast<int>(sqlite_native_map_status::ok) ||
						(primary_status == sqlite_readonly_status &&
						 native_status != sqlite_readonly_status &&
						 native_status != sqlite_readonly_cantinit_status);
				}
			}
			return false;
		}

		[[nodiscard]] bool valid_reader_unpublished_cleanup_terminal_receipt(
			const sqlite_shm_verified_reader_unpublished_cleanup_terminal_receipt& receipt) noexcept
		{
			if (!valid_callback(receipt.callback()) || receipt.caller_delete_flag() != 0 ||
				receipt.delegated_delete_flag() != 0)
				return false;
			switch (receipt.evidence_kind())
			{
				case sqlite_shm_reader_unpublished_cleanup_evidence_kind::exact_native_result:
					if (!receipt.native_status() ||
						!valid_closed_sqlite_status(*receipt.native_status()) ||
						!receipt.native_effect_receipt() ||
						!valid_identity(*receipt.native_effect_receipt()))
						return false;
					if (*receipt.native_status() == static_cast<int>(sqlite_native_map_status::ok))
						return receipt.latch_reset_receipt() &&
							valid_identity(*receipt.latch_reset_receipt()) &&
							*receipt.latch_reset_receipt() != *receipt.native_effect_receipt();
					return !receipt.latch_reset_receipt();
				case sqlite_shm_reader_unpublished_cleanup_evidence_kind::throw_or_unknown:
					return !receipt.native_status() && !receipt.native_effect_receipt() &&
						!receipt.latch_reset_receipt();
			}
			return false;
		}

		[[nodiscard]] bool valid_reader_unmap_terminal_receipt(
			const sqlite_shm_verified_reader_unmap_terminal_receipt& receipt) noexcept
		{
			if (!valid_callback(receipt.callback()) || receipt.caller_delete_flag() != 0 ||
				receipt.delegated_delete_flag() != 0)
				return false;
			switch (receipt.evidence_kind())
			{
				case sqlite_shm_reader_unmap_evidence_kind::exact_native_result:
					if (!receipt.native_status() ||
						!valid_closed_sqlite_status(*receipt.native_status()) ||
						!receipt.native_effect_receipt() ||
						!valid_identity(*receipt.native_effect_receipt()))
						return false;
					if (*receipt.native_status() == static_cast<int>(sqlite_native_map_status::ok))
						return receipt.latch_reset_receipt() &&
							valid_identity(*receipt.latch_reset_receipt());
					return !receipt.latch_reset_receipt();
				case sqlite_shm_reader_unmap_evidence_kind::throw_or_unknown:
					return !receipt.native_status() && !receipt.native_effect_receipt() &&
						!receipt.latch_reset_receipt();
			}
			return false;
		}

		[[nodiscard]] bool valid_reader_close_terminal_receipt(
			const sqlite_shm_verified_reader_close_terminal_receipt& receipt) noexcept
		{
			if (!valid_callback(receipt.callback()))
				return false;
			switch (receipt.evidence_kind())
			{
				case sqlite_shm_reader_close_evidence_kind::exact_native_result:
					return receipt.native_status() &&
						valid_closed_sqlite_status(*receipt.native_status()) &&
						receipt.native_effect_receipt() &&
						valid_identity(*receipt.native_effect_receipt());
				case sqlite_shm_reader_close_evidence_kind::throw_or_unknown:
					return !receipt.native_status() && !receipt.native_effect_receipt();
			}
			return false;
		}

		[[nodiscard]] bool
		valid_reader_session_request(const sqlite_shm_reader_session_request& request) noexcept
		{
			return valid_reader_native_attachment(request.attachment) &&
				valid_callback(request.execution) &&
				valid_identity(request.read_transaction_epoch) &&
				valid_identity(request.decode_attempt) &&
				valid_identity(request.authority_read_receipt);
		}

		[[nodiscard]] bool valid_reader_pre_sqlite_session_request(
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

		[[nodiscard]] bool reader_attachment_matches_pre_sqlite_request(
			const sqlite_shm_reader_attachment_reservation_identity& attachment,
			const sqlite_shm_reader_pre_sqlite_session_request& request) noexcept
		{
			return attachment.family() == request.family &&
				attachment.alias_lifetime() == request.alias_lifetime &&
				attachment.connection_token() == request.connection_token &&
				attachment.main_native_file_receipt() == request.main_native_file_receipt &&
				attachment.main_xopen_receipt() == request.main_xopen_receipt &&
				attachment.open_epoch() == request.open_epoch &&
				attachment.callback_cohort() == request.callback_cohort;
		}

		[[nodiscard]] bool reader_open_matches_pre_sqlite_request(
			const sqlite_shm_reader_open_epoch_binding& open,
			const sqlite_shm_reader_pre_sqlite_session_request& request) noexcept
		{
			return open.family == request.family && open.alias_lifetime == request.alias_lifetime &&
				open.connection_token == request.connection_token &&
				open.main_native_file_receipt == request.main_native_file_receipt &&
				open.main_xopen_receipt == request.main_xopen_receipt &&
				open.open_epoch == request.open_epoch &&
				open.callback_cohort == request.callback_cohort;
		}

		[[nodiscard]] bool
		same_reader_session_owner_key(const sqlite_shm_reader_session_request& left,
									  const sqlite_shm_reader_session_request& right) noexcept
		{
			return left.attachment == right.attachment &&
				left.read_transaction_epoch == right.read_transaction_epoch &&
				left.decode_attempt == right.decode_attempt;
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
			reader_unpublished_cleanup,
			reader_unmap,
			reader_close,
			reader_session,
			reader_attachment_map_inflight,
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

	std::optional<sqlite_shm_reader_attachment_reservation_identity>
	sqlite_shm_reader_attachment_reservation_identity::bind(
		sqlite_shm_lease_family_binding family,
		sqlite_backend_opaque_identity runtime_lifetime_pin,
		sqlite_backend_opaque_identity alias_lifetime,
		sqlite_backend_opaque_identity connection_token,
		sqlite_backend_opaque_identity main_native_file_receipt,
		sqlite_backend_opaque_identity main_xopen_receipt,
		sqlite_backend_opaque_identity open_epoch,
		const std::uint64_t writer_mapping_generation,
		sqlite_backend_opaque_identity callback_cohort,
		sqlite_backend_opaque_identity attachment_epoch,
		const std::uint64_t registry_open_token)
	{
		if (!valid_family(family) || !valid_identity(runtime_lifetime_pin) ||
			!valid_identity(alias_lifetime) || !valid_identity(connection_token) ||
			!valid_identity(main_native_file_receipt) || !valid_identity(main_xopen_receipt) ||
			!valid_identity(open_epoch) || writer_mapping_generation == 0U ||
			!valid_identity(callback_cohort) || !valid_identity(attachment_epoch))
			return std::nullopt;
		return sqlite_shm_reader_attachment_reservation_identity{
			std::move(family),
			std::move(runtime_lifetime_pin),
			std::move(alias_lifetime),
			std::move(connection_token),
			std::move(main_native_file_receipt),
			std::move(main_xopen_receipt),
			std::move(open_epoch),
			writer_mapping_generation,
			std::move(callback_cohort),
			std::move(attachment_epoch),
			registry_open_token};
	}

	sqlite_shm_reader_attachment_reservation_identity::
		sqlite_shm_reader_attachment_reservation_identity(
			sqlite_shm_lease_family_binding family,
			sqlite_backend_opaque_identity runtime_lifetime_pin,
			sqlite_backend_opaque_identity alias_lifetime,
			sqlite_backend_opaque_identity connection_token,
			sqlite_backend_opaque_identity main_native_file_receipt,
			sqlite_backend_opaque_identity main_xopen_receipt,
			sqlite_backend_opaque_identity open_epoch,
			const std::uint64_t writer_mapping_generation,
			sqlite_backend_opaque_identity callback_cohort,
			sqlite_backend_opaque_identity attachment_epoch,
			const std::uint64_t registry_open_token)
		: family_{std::move(family)}, runtime_lifetime_pin_{std::move(runtime_lifetime_pin)},
		  alias_lifetime_{std::move(alias_lifetime)},
		  connection_token_{std::move(connection_token)},
		  main_native_file_receipt_{std::move(main_native_file_receipt)},
		  main_xopen_receipt_{std::move(main_xopen_receipt)}, open_epoch_{std::move(open_epoch)},
		  writer_mapping_generation_{writer_mapping_generation},
		  callback_cohort_{std::move(callback_cohort)},
		  attachment_epoch_{std::move(attachment_epoch)}, registry_open_token_{registry_open_token}
	{
	}

	const sqlite_shm_lease_family_binding&
	sqlite_shm_reader_attachment_reservation_identity::family() const noexcept
	{
		return family_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_reader_attachment_reservation_identity::runtime_lifetime_pin() const noexcept
	{
		return runtime_lifetime_pin_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_reader_attachment_reservation_identity::alias_lifetime() const noexcept
	{
		return alias_lifetime_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_reader_attachment_reservation_identity::connection_token() const noexcept
	{
		return connection_token_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_reader_attachment_reservation_identity::main_native_file_receipt() const noexcept
	{
		return main_native_file_receipt_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_reader_attachment_reservation_identity::main_xopen_receipt() const noexcept
	{
		return main_xopen_receipt_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_reader_attachment_reservation_identity::open_epoch() const noexcept
	{
		return open_epoch_;
	}

	std::uint64_t
	sqlite_shm_reader_attachment_reservation_identity::writer_mapping_generation() const noexcept
	{
		return writer_mapping_generation_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_reader_attachment_reservation_identity::callback_cohort() const noexcept
	{
		return callback_cohort_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_reader_attachment_reservation_identity::attachment_epoch() const noexcept
	{
		return attachment_epoch_;
	}

	std::uint64_t
	sqlite_shm_reader_attachment_reservation_identity::registry_open_token() const noexcept
	{
		return registry_open_token_;
	}

	sqlite_shm_reader_native_attachment_identity::sqlite_shm_reader_native_attachment_identity(
		sqlite_shm_reader_attachment_reservation_identity expected,
		sqlite_backend_opaque_identity observed_shm_object_receipt,
		sqlite_backend_opaque_identity observed_shm_entry_receipt,
		sqlite_backend_opaque_identity observed_device_receipt,
		sqlite_backend_opaque_identity observed_mount_receipt)
		: expected_{std::move(expected)},
		  observed_shm_object_receipt_{std::move(observed_shm_object_receipt)},
		  observed_shm_entry_receipt_{std::move(observed_shm_entry_receipt)},
		  observed_device_receipt_{std::move(observed_device_receipt)},
		  observed_mount_receipt_{std::move(observed_mount_receipt)}
	{
	}

	const sqlite_shm_reader_attachment_reservation_identity&
	sqlite_shm_reader_native_attachment_identity::expected() const noexcept
	{
		return expected_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_reader_native_attachment_identity::observed_shm_object_receipt() const noexcept
	{
		return observed_shm_object_receipt_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_reader_native_attachment_identity::observed_shm_entry_receipt() const noexcept
	{
		return observed_shm_entry_receipt_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_reader_native_attachment_identity::observed_device_receipt() const noexcept
	{
		return observed_device_receipt_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_reader_native_attachment_identity::observed_mount_receipt() const noexcept
	{
		return observed_mount_receipt_;
	}

	sqlite_shm_reader_session_terminal_receipt::sqlite_shm_reader_session_terminal_receipt(
		sqlite_shm_reader_session_request request,
		const sqlite_shm_reader_session_terminal_kind kind,
		sqlite_backend_opaque_identity terminal_receipt,
		const bool authority_read_closed,
		const bool no_live_shm_lock)
		: request_{std::move(request)}, kind_{kind}, terminal_receipt_{std::move(terminal_receipt)},
		  authority_read_closed_{authority_read_closed}, no_live_shm_lock_{no_live_shm_lock}
	{
	}

	const sqlite_shm_reader_session_request&
	sqlite_shm_reader_session_terminal_receipt::request() const noexcept
	{
		return request_;
	}

	sqlite_shm_reader_session_terminal_kind
	sqlite_shm_reader_session_terminal_receipt::kind() const noexcept
	{
		return kind_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_reader_session_terminal_receipt::terminal_receipt() const noexcept
	{
		return terminal_receipt_;
	}

	bool sqlite_shm_reader_session_terminal_receipt::authority_read_closed() const noexcept
	{
		return authority_read_closed_;
	}

	bool sqlite_shm_reader_session_terminal_receipt::no_live_shm_lock() const noexcept
	{
		return no_live_shm_lock_;
	}

	sqlite_shm_verified_reader_attachment_zero_effect_receipt::
		sqlite_shm_verified_reader_attachment_zero_effect_receipt(
			const sqlite_shm_reader_attachment_map_inflight& inflight,
			const sqlite_shm_reader_attachment_zero_effect_kind kind,
			sqlite_shm_reader_attachment_map_request request,
			const int native_status,
			const volatile void* native_mapping,
			const int delegated_extend,
			sqlite_backend_opaque_identity zero_attachment_effect_receipt)
		: state_{inflight.state_}, token_{inflight.token_}, kind_{kind},
		  request_{std::move(request)}, native_status_{native_status},
		  native_mapping_{native_mapping}, delegated_extend_{delegated_extend},
		  zero_attachment_effect_receipt_{std::move(zero_attachment_effect_receipt)}
	{
	}

	sqlite_shm_reader_attachment_zero_effect_kind
	sqlite_shm_verified_reader_attachment_zero_effect_receipt::kind() const noexcept
	{
		return kind_;
	}

	const sqlite_shm_reader_attachment_map_request&
	sqlite_shm_verified_reader_attachment_zero_effect_receipt::request() const noexcept
	{
		return request_;
	}

	int sqlite_shm_verified_reader_attachment_zero_effect_receipt::native_status() const noexcept
	{
		return native_status_;
	}

	const volatile void*
	sqlite_shm_verified_reader_attachment_zero_effect_receipt::native_mapping() const noexcept
	{
		return native_mapping_;
	}

	int sqlite_shm_verified_reader_attachment_zero_effect_receipt::delegated_extend() const noexcept
	{
		return delegated_extend_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_verified_reader_attachment_zero_effect_receipt::zero_attachment_effect_receipt()
		const noexcept
	{
		return zero_attachment_effect_receipt_;
	}

	sqlite_shm_reader_attachment_zero_effect_result::
		sqlite_shm_reader_attachment_zero_effect_result(
			const sqlite_shm_reader_attachment_zero_effect_kind kind,
			const int native_status) noexcept
		: kind_{kind}, native_status_{native_status}
	{
	}

	sqlite_shm_reader_attachment_zero_effect_kind
	sqlite_shm_reader_attachment_zero_effect_result::kind() const noexcept
	{
		return kind_;
	}

	int sqlite_shm_reader_attachment_zero_effect_result::native_status() const noexcept
	{
		return native_status_;
	}

	const volatile void*
	sqlite_shm_reader_attachment_zero_effect_result::native_mapping() const noexcept
	{
		return nullptr;
	}

	int sqlite_shm_reader_opaque_attachment_uncertainty_result::outward_status() const noexcept
	{
		return sqlite_ioerr_status;
	}

	const volatile void*
	sqlite_shm_reader_opaque_attachment_uncertainty_result::native_mapping() const noexcept
	{
		return nullptr;
	}

	sqlite_shm_verified_reader_predecessor_map_receipt::
		sqlite_shm_verified_reader_predecessor_map_receipt(
			const sqlite_shm_reader_attachment_map_inflight& inflight,
			const sqlite_shm_reader_predecessor_map_kind kind,
			sqlite_shm_reader_attachment_map_request request,
			const int native_status,
			const volatile void* native_mapping,
			const int delegated_extend,
			std::optional<sqlite_shm_reader_native_attachment_identity> observed_attachment,
			sqlite_backend_opaque_identity native_effect_receipt)
		: state_{inflight.state_}, token_{inflight.token_}, kind_{kind},
		  request_{std::move(request)}, native_status_{native_status},
		  native_mapping_{native_mapping}, delegated_extend_{delegated_extend},
		  observed_attachment_{std::move(observed_attachment)},
		  native_effect_receipt_{std::move(native_effect_receipt)}
	{
	}

	sqlite_shm_reader_predecessor_map_kind
	sqlite_shm_verified_reader_predecessor_map_receipt::kind() const noexcept
	{
		return kind_;
	}

	const sqlite_shm_reader_attachment_map_request&
	sqlite_shm_verified_reader_predecessor_map_receipt::request() const noexcept
	{
		return request_;
	}

	int sqlite_shm_verified_reader_predecessor_map_receipt::native_status() const noexcept
	{
		return native_status_;
	}

	const volatile void*
	sqlite_shm_verified_reader_predecessor_map_receipt::native_mapping() const noexcept
	{
		return native_mapping_;
	}

	int sqlite_shm_verified_reader_predecessor_map_receipt::delegated_extend() const noexcept
	{
		return delegated_extend_;
	}

	const std::optional<sqlite_shm_reader_native_attachment_identity>&
	sqlite_shm_verified_reader_predecessor_map_receipt::observed_attachment() const noexcept
	{
		return observed_attachment_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_verified_reader_predecessor_map_receipt::native_effect_receipt() const noexcept
	{
		return native_effect_receipt_;
	}

	// These private lineage coordinates are copied from one validated terminal record; the sole
	// state call site names each field and preserves the protocol order.
	// NOLINTBEGIN(bugprone-easily-swappable-parameters)
	sqlite_shm_reader_predecessor_map_result::sqlite_shm_reader_predecessor_map_result(
		std::weak_ptr<detail::sqlite_shm_mapping_lease_state> state,
		const std::uint64_t token,
		const std::uint64_t reservation_token,
		const std::uint64_t generation,
		const sqlite_shm_reader_predecessor_map_kind kind,
		const int native_status,
		const volatile void* native_mapping) noexcept
		: state_{std::move(state)}, token_{token}, reservation_token_{reservation_token},
		  generation_{generation}, kind_{kind}, native_status_{native_status},
		  native_mapping_{native_mapping}
	{
	}
	// NOLINTEND(bugprone-easily-swappable-parameters)

	sqlite_shm_reader_predecessor_map_kind
	sqlite_shm_reader_predecessor_map_result::kind() const noexcept
	{
		return kind_;
	}

	int sqlite_shm_reader_predecessor_map_result::native_status() const noexcept
	{
		return native_status_;
	}

	const volatile void* sqlite_shm_reader_predecessor_map_result::native_mapping() const noexcept
	{
		return native_mapping_;
	}

	sqlite_shm_reader_existing_group_predecessor_mismatch_result::
		sqlite_shm_reader_existing_group_predecessor_mismatch_result(
			const int native_status) noexcept
		: native_status_{native_status}
	{
	}

	int sqlite_shm_reader_existing_group_predecessor_mismatch_result::native_status() const noexcept
	{
		return native_status_;
	}

	int
	sqlite_shm_reader_existing_group_predecessor_mismatch_result::outward_status() const noexcept
	{
		return sqlite_ioerr_status;
	}

	const volatile void*
	sqlite_shm_reader_existing_group_predecessor_mismatch_result::native_mapping() const noexcept
	{
		return nullptr;
	}

	// The two delete flags deliberately retain SQLite's exact caller/delegated callback tuple.
	// NOLINTBEGIN(bugprone-easily-swappable-parameters)
	sqlite_shm_verified_reader_predecessor_unmap_terminal_receipt::
		sqlite_shm_verified_reader_predecessor_unmap_terminal_receipt(
			const sqlite_shm_reader_predecessor_map_result& predecessor,
			sqlite_shm_callback_execution_receipt callback,
			const sqlite_shm_reader_unmap_evidence_kind evidence_kind,
			std::optional<int> native_status,
			const int caller_delete_flag,
			const int delegated_delete_flag,
			std::optional<sqlite_backend_opaque_identity> native_effect_receipt)
		: state_{predecessor.state_}, token_{predecessor.token_},
		  reservation_token_{predecessor.reservation_token_}, generation_{predecessor.generation_},
		  callback_{std::move(callback)}, evidence_kind_{evidence_kind},
		  native_status_{native_status}, caller_delete_flag_{caller_delete_flag},
		  delegated_delete_flag_{delegated_delete_flag},
		  native_effect_receipt_{std::move(native_effect_receipt)}
	{
	}
	// NOLINTEND(bugprone-easily-swappable-parameters)

	const sqlite_shm_callback_execution_receipt&
	sqlite_shm_verified_reader_predecessor_unmap_terminal_receipt::callback() const noexcept
	{
		return callback_;
	}

	sqlite_shm_reader_unmap_evidence_kind
	sqlite_shm_verified_reader_predecessor_unmap_terminal_receipt::evidence_kind() const noexcept
	{
		return evidence_kind_;
	}

	std::optional<int>
	sqlite_shm_verified_reader_predecessor_unmap_terminal_receipt::native_status() const noexcept
	{
		return native_status_;
	}

	int sqlite_shm_verified_reader_predecessor_unmap_terminal_receipt::caller_delete_flag()
		const noexcept
	{
		return caller_delete_flag_;
	}

	int sqlite_shm_verified_reader_predecessor_unmap_terminal_receipt::delegated_delete_flag()
		const noexcept
	{
		return delegated_delete_flag_;
	}

	const std::optional<sqlite_backend_opaque_identity>&
	sqlite_shm_verified_reader_predecessor_unmap_terminal_receipt::native_effect_receipt()
		const noexcept
	{
		return native_effect_receipt_;
	}

	sqlite_shm_reader_predecessor_unmap_terminal_result::
		sqlite_shm_reader_predecessor_unmap_terminal_result(
			const sqlite_shm_reader_unmap_terminal_kind kind,
			const sqlite_shm_reader_unmap_evidence_kind evidence_kind,
			std::optional<int> native_status,
			std::optional<sqlite_backend_opaque_identity> native_effect_receipt) noexcept
		: kind_{kind}, evidence_kind_{evidence_kind}, native_status_{native_status},
		  outward_status_{kind == sqlite_shm_reader_unmap_terminal_kind::retired_confirmed
							  ? static_cast<int>(sqlite_native_map_status::ok)
							  : evidence_kind ==
									  sqlite_shm_reader_unmap_evidence_kind::exact_native_result &&
								  native_status &&
								  *native_status != static_cast<int>(sqlite_native_map_status::ok)
							  ? *native_status
							  : sqlite_ioerr_status},
		  native_effect_receipt_{std::move(native_effect_receipt)}
	{
	}

	sqlite_shm_reader_unmap_terminal_kind
	sqlite_shm_reader_predecessor_unmap_terminal_result::kind() const noexcept
	{
		return kind_;
	}

	sqlite_shm_reader_unmap_evidence_kind
	sqlite_shm_reader_predecessor_unmap_terminal_result::evidence_kind() const noexcept
	{
		return evidence_kind_;
	}

	std::optional<int>
	sqlite_shm_reader_predecessor_unmap_terminal_result::native_status() const noexcept
	{
		return native_status_;
	}

	int sqlite_shm_reader_predecessor_unmap_terminal_result::outward_status() const noexcept
	{
		return outward_status_;
	}

	const std::optional<sqlite_backend_opaque_identity>&
	sqlite_shm_reader_predecessor_unmap_terminal_result::native_effect_receipt() const noexcept
	{
		return native_effect_receipt_;
	}

	// The private constructor mirrors the validated native callback evidence tuple; named fields at
	// its validator-only call sites preserve the protocol order.
	// NOLINTBEGIN(bugprone-easily-swappable-parameters)
	sqlite_shm_verified_reader_unpublished_cleanup_receipt::
		sqlite_shm_verified_reader_unpublished_cleanup_receipt(
			const sqlite_shm_reader_attachment_map_inflight& inflight,
			const sqlite_shm_reader_unpublished_cleanup_entry_kind kind,
			sqlite_shm_reader_attachment_map_request request,
			sqlite_shm_reader_session_request session_request,
			const std::uint64_t generation,
			const int native_status,
			const volatile void* native_mapping,
			const int delegated_extend,
			sqlite_shm_reader_native_attachment_identity observed_attachment,
			sqlite_backend_opaque_identity mapped_effect_receipt,
			sqlite_backend_opaque_identity session_no_pointer_terminal_receipt)
		: state_{inflight.state_}, token_{inflight.token_}, kind_{kind},
		  request_{std::move(request)}, session_request_{std::move(session_request)},
		  generation_{generation}, native_status_{native_status}, native_mapping_{native_mapping},
		  delegated_extend_{delegated_extend}, observed_attachment_{std::move(observed_attachment)},
		  mapped_effect_receipt_{std::move(mapped_effect_receipt)},
		  session_no_pointer_terminal_receipt_{std::move(session_no_pointer_terminal_receipt)}
	{
	}
	// NOLINTEND(bugprone-easily-swappable-parameters)

	sqlite_shm_reader_unpublished_cleanup_entry_kind
	sqlite_shm_verified_reader_unpublished_cleanup_receipt::kind() const noexcept
	{
		return kind_;
	}

	const sqlite_shm_reader_attachment_map_request&
	sqlite_shm_verified_reader_unpublished_cleanup_receipt::request() const noexcept
	{
		return request_;
	}

	const sqlite_shm_reader_session_request&
	sqlite_shm_verified_reader_unpublished_cleanup_receipt::session_request() const noexcept
	{
		return session_request_;
	}

	std::uint64_t
	sqlite_shm_verified_reader_unpublished_cleanup_receipt::generation() const noexcept
	{
		return generation_;
	}

	int sqlite_shm_verified_reader_unpublished_cleanup_receipt::native_status() const noexcept
	{
		return native_status_;
	}

	const volatile void*
	sqlite_shm_verified_reader_unpublished_cleanup_receipt::native_mapping() const noexcept
	{
		return native_mapping_;
	}

	int sqlite_shm_verified_reader_unpublished_cleanup_receipt::delegated_extend() const noexcept
	{
		return delegated_extend_;
	}

	const sqlite_shm_reader_native_attachment_identity&
	sqlite_shm_verified_reader_unpublished_cleanup_receipt::observed_attachment() const noexcept
	{
		return observed_attachment_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_verified_reader_unpublished_cleanup_receipt::mapped_effect_receipt() const noexcept
	{
		return mapped_effect_receipt_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_verified_reader_unpublished_cleanup_receipt::session_no_pointer_terminal_receipt()
		const noexcept
	{
		return session_no_pointer_terminal_receipt_;
	}

	// The two delete flags deliberately retain SQLite's exact caller/delegated callback tuple.
	// NOLINTBEGIN(bugprone-easily-swappable-parameters)
	sqlite_shm_verified_reader_unpublished_cleanup_terminal_receipt::
		sqlite_shm_verified_reader_unpublished_cleanup_terminal_receipt(
			const sqlite_shm_reader_unpublished_cleanup_obligation& cleanup,
			sqlite_shm_callback_execution_receipt callback,
			const sqlite_shm_reader_unpublished_cleanup_evidence_kind evidence_kind,
			std::optional<int> native_status,
			const int caller_delete_flag,
			const int delegated_delete_flag,
			std::optional<sqlite_backend_opaque_identity> native_effect_receipt,
			std::optional<sqlite_backend_opaque_identity> latch_reset_receipt)
		: state_{cleanup.state_}, token_{cleanup.token_}, generation_{cleanup.generation_},
		  callback_{std::move(callback)}, evidence_kind_{evidence_kind},
		  native_status_{native_status}, caller_delete_flag_{caller_delete_flag},
		  delegated_delete_flag_{delegated_delete_flag},
		  native_effect_receipt_{std::move(native_effect_receipt)},
		  latch_reset_receipt_{std::move(latch_reset_receipt)}
	{
	}
	// NOLINTEND(bugprone-easily-swappable-parameters)

	const sqlite_shm_callback_execution_receipt&
	sqlite_shm_verified_reader_unpublished_cleanup_terminal_receipt::callback() const noexcept
	{
		return callback_;
	}

	sqlite_shm_reader_unpublished_cleanup_evidence_kind
	sqlite_shm_verified_reader_unpublished_cleanup_terminal_receipt::evidence_kind() const noexcept
	{
		return evidence_kind_;
	}

	std::optional<int>
	sqlite_shm_verified_reader_unpublished_cleanup_terminal_receipt::native_status() const noexcept
	{
		return native_status_;
	}

	int sqlite_shm_verified_reader_unpublished_cleanup_terminal_receipt::caller_delete_flag()
		const noexcept
	{
		return caller_delete_flag_;
	}

	int sqlite_shm_verified_reader_unpublished_cleanup_terminal_receipt::delegated_delete_flag()
		const noexcept
	{
		return delegated_delete_flag_;
	}

	const std::optional<sqlite_backend_opaque_identity>&
	sqlite_shm_verified_reader_unpublished_cleanup_terminal_receipt::native_effect_receipt()
		const noexcept
	{
		return native_effect_receipt_;
	}

	const std::optional<sqlite_backend_opaque_identity>&
	sqlite_shm_verified_reader_unpublished_cleanup_terminal_receipt::latch_reset_receipt()
		const noexcept
	{
		return latch_reset_receipt_;
	}

	sqlite_shm_reader_unpublished_cleanup_terminal_result::
		sqlite_shm_reader_unpublished_cleanup_terminal_result(
			const sqlite_shm_reader_unpublished_cleanup_terminal_kind kind,
			const sqlite_shm_reader_unpublished_cleanup_evidence_kind evidence_kind,
			std::optional<int> native_status,
			std::optional<sqlite_backend_opaque_identity> native_effect_receipt,
			std::optional<sqlite_backend_opaque_identity> latch_reset_receipt) noexcept
		: kind_{kind}, evidence_kind_{evidence_kind}, native_status_{native_status},
		  outward_status_{
			  kind == sqlite_shm_reader_unpublished_cleanup_terminal_kind::terminal_quarantined &&
					  evidence_kind ==
						  sqlite_shm_reader_unpublished_cleanup_evidence_kind::
							  exact_native_result &&
					  native_status &&
					  *native_status != static_cast<int>(sqlite_native_map_status::ok)
				  ? *native_status
				  : sqlite_ioerr_status},
		  native_effect_receipt_{std::move(native_effect_receipt)},
		  latch_reset_receipt_{std::move(latch_reset_receipt)}
	{
	}

	sqlite_shm_reader_unpublished_cleanup_terminal_kind
	sqlite_shm_reader_unpublished_cleanup_terminal_result::kind() const noexcept
	{
		return kind_;
	}

	sqlite_shm_reader_unpublished_cleanup_evidence_kind
	sqlite_shm_reader_unpublished_cleanup_terminal_result::evidence_kind() const noexcept
	{
		return evidence_kind_;
	}

	std::optional<int>
	sqlite_shm_reader_unpublished_cleanup_terminal_result::native_status() const noexcept
	{
		return native_status_;
	}

	int sqlite_shm_reader_unpublished_cleanup_terminal_result::outward_status() const noexcept
	{
		return outward_status_;
	}

	const std::optional<sqlite_backend_opaque_identity>&
	sqlite_shm_reader_unpublished_cleanup_terminal_result::native_effect_receipt() const noexcept
	{
		return native_effect_receipt_;
	}

	const std::optional<sqlite_backend_opaque_identity>&
	sqlite_shm_reader_unpublished_cleanup_terminal_result::latch_reset_receipt() const noexcept
	{
		return latch_reset_receipt_;
	}

	sqlite_shm_reader_logical_ack_result::sqlite_shm_reader_logical_ack_result(
		const int stored_cleanup_status) noexcept
		: outward_status_{stored_cleanup_status}
	{
	}

	detail::sqlite_shm_reader_logical_ack_phase
	sqlite_shm_reader_logical_ack_result::phase() const noexcept
	{
		return phase_;
	}

	int sqlite_shm_reader_logical_ack_result::outward_status() const noexcept
	{
		return outward_status_;
	}

	bool sqlite_shm_reader_logical_ack_result::delegated_native_effect() const noexcept
	{
		return false;
	}

	sqlite_shm_verified_reader_unmap_terminal_receipt::
		sqlite_shm_verified_reader_unmap_terminal_receipt(
			const sqlite_shm_reader_unmap_obligation& unmap,
			sqlite_shm_callback_execution_receipt callback,
			const sqlite_shm_reader_unmap_evidence_kind evidence_kind,
			std::optional<int> native_status,
			// The flags preserve SQLite's caller/delegated boundary as separate evidence.
			// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
			const int caller_delete_flag,
			const int delegated_delete_flag,
			std::optional<sqlite_backend_opaque_identity> native_effect_receipt,
			std::optional<sqlite_backend_opaque_identity> latch_reset_receipt)
		: state_{unmap.state_}, token_{unmap.token_}, generation_{unmap.generation_},
		  callback_{std::move(callback)}, evidence_kind_{evidence_kind},
		  native_status_{native_status}, caller_delete_flag_{caller_delete_flag},
		  delegated_delete_flag_{delegated_delete_flag},
		  native_effect_receipt_{std::move(native_effect_receipt)},
		  latch_reset_receipt_{std::move(latch_reset_receipt)}
	{
	}

	sqlite_shm_verified_reader_unmap_terminal_receipt::
		sqlite_shm_verified_reader_unmap_terminal_receipt(
			const sqlite_shm_reader_live_close_obligation& close,
			sqlite_shm_callback_execution_receipt callback,
			const sqlite_shm_reader_unmap_evidence_kind evidence_kind,
			std::optional<int> native_status,
			// The flags preserve SQLite's caller/delegated boundary as separate evidence.
			// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
			const int caller_delete_flag,
			const int delegated_delete_flag,
			std::optional<sqlite_backend_opaque_identity> native_effect_receipt,
			std::optional<sqlite_backend_opaque_identity> latch_reset_receipt)
		: state_{close.state_}, token_{close.group_token_}, generation_{close.generation_},
		  callback_{std::move(callback)}, evidence_kind_{evidence_kind},
		  native_status_{native_status}, caller_delete_flag_{caller_delete_flag},
		  delegated_delete_flag_{delegated_delete_flag},
		  native_effect_receipt_{std::move(native_effect_receipt)},
		  latch_reset_receipt_{std::move(latch_reset_receipt)}
	{
	}

	const sqlite_shm_callback_execution_receipt&
	sqlite_shm_verified_reader_unmap_terminal_receipt::callback() const noexcept
	{
		return callback_;
	}

	sqlite_shm_reader_unmap_evidence_kind
	sqlite_shm_verified_reader_unmap_terminal_receipt::evidence_kind() const noexcept
	{
		return evidence_kind_;
	}

	std::optional<int>
	sqlite_shm_verified_reader_unmap_terminal_receipt::native_status() const noexcept
	{
		return native_status_;
	}

	int sqlite_shm_verified_reader_unmap_terminal_receipt::caller_delete_flag() const noexcept
	{
		return caller_delete_flag_;
	}

	int sqlite_shm_verified_reader_unmap_terminal_receipt::delegated_delete_flag() const noexcept
	{
		return delegated_delete_flag_;
	}

	const std::optional<sqlite_backend_opaque_identity>&
	sqlite_shm_verified_reader_unmap_terminal_receipt::native_effect_receipt() const noexcept
	{
		return native_effect_receipt_;
	}

	const std::optional<sqlite_backend_opaque_identity>&
	sqlite_shm_verified_reader_unmap_terminal_receipt::latch_reset_receipt() const noexcept
	{
		return latch_reset_receipt_;
	}

	sqlite_shm_reader_unmap_terminal_result::sqlite_shm_reader_unmap_terminal_result(
		const sqlite_shm_reader_unmap_terminal_kind kind,
		const sqlite_shm_reader_unmap_evidence_kind evidence_kind,
		std::optional<int> native_status,
		std::optional<sqlite_backend_opaque_identity> native_effect_receipt,
		std::optional<sqlite_backend_opaque_identity> latch_reset_receipt) noexcept
		: kind_{kind}, evidence_kind_{evidence_kind}, native_status_{native_status},
		  outward_status_{kind == sqlite_shm_reader_unmap_terminal_kind::retired_confirmed
							  ? static_cast<int>(sqlite_native_map_status::ok)
							  : evidence_kind ==
									  sqlite_shm_reader_unmap_evidence_kind::exact_native_result &&
								  native_status &&
								  *native_status != static_cast<int>(sqlite_native_map_status::ok)
							  ? *native_status
							  : sqlite_ioerr_status},
		  native_effect_receipt_{std::move(native_effect_receipt)},
		  latch_reset_receipt_{std::move(latch_reset_receipt)}
	{
	}

	sqlite_shm_reader_unmap_terminal_kind
	sqlite_shm_reader_unmap_terminal_result::kind() const noexcept
	{
		return kind_;
	}

	sqlite_shm_reader_unmap_evidence_kind
	sqlite_shm_reader_unmap_terminal_result::evidence_kind() const noexcept
	{
		return evidence_kind_;
	}

	std::optional<int> sqlite_shm_reader_unmap_terminal_result::native_status() const noexcept
	{
		return native_status_;
	}

	int sqlite_shm_reader_unmap_terminal_result::outward_status() const noexcept
	{
		return outward_status_;
	}

	const std::optional<sqlite_backend_opaque_identity>&
	sqlite_shm_reader_unmap_terminal_result::native_effect_receipt() const noexcept
	{
		return native_effect_receipt_;
	}

	const std::optional<sqlite_backend_opaque_identity>&
	sqlite_shm_reader_unmap_terminal_result::latch_reset_receipt() const noexcept
	{
		return latch_reset_receipt_;
	}

	sqlite_shm_verified_reader_close_terminal_receipt::
		sqlite_shm_verified_reader_close_terminal_receipt(
			const sqlite_shm_reader_close_obligation& close,
			sqlite_shm_callback_execution_receipt callback,
			const sqlite_shm_reader_close_evidence_kind evidence_kind,
			std::optional<int> native_status,
			std::optional<sqlite_backend_opaque_identity> native_effect_receipt)
		: state_{close.state_}, owner_token_{close.owner_token_},
		  registry_open_token_{close.registry_open_token_}, callback_{std::move(callback)},
		  evidence_kind_{evidence_kind}, native_status_{native_status},
		  native_effect_receipt_{std::move(native_effect_receipt)}
	{
	}

	sqlite_shm_verified_reader_close_terminal_receipt::
		sqlite_shm_verified_reader_close_terminal_receipt(
			const sqlite_shm_reader_live_close_obligation& close,
			sqlite_shm_callback_execution_receipt callback,
			const sqlite_shm_reader_close_evidence_kind evidence_kind,
			std::optional<int> native_status,
			std::optional<sqlite_backend_opaque_identity> native_effect_receipt)
		: state_{close.state_}, owner_token_{close.close_owner_token_},
		  registry_open_token_{close.registry_open_token_}, callback_{std::move(callback)},
		  evidence_kind_{evidence_kind}, native_status_{native_status},
		  native_effect_receipt_{std::move(native_effect_receipt)}
	{
	}

	const sqlite_shm_callback_execution_receipt&
	sqlite_shm_verified_reader_close_terminal_receipt::callback() const noexcept
	{
		return callback_;
	}

	sqlite_shm_reader_close_evidence_kind
	sqlite_shm_verified_reader_close_terminal_receipt::evidence_kind() const noexcept
	{
		return evidence_kind_;
	}

	std::optional<int>
	sqlite_shm_verified_reader_close_terminal_receipt::native_status() const noexcept
	{
		return native_status_;
	}

	const std::optional<sqlite_backend_opaque_identity>&
	sqlite_shm_verified_reader_close_terminal_receipt::native_effect_receipt() const noexcept
	{
		return native_effect_receipt_;
	}

	sqlite_shm_reader_close_terminal_result::sqlite_shm_reader_close_terminal_result(
		const sqlite_shm_reader_close_terminal_kind kind,
		const sqlite_shm_reader_close_route route,
		const sqlite_shm_reader_close_evidence_kind evidence_kind,
		std::optional<int> native_status,
		std::optional<sqlite_backend_opaque_identity> native_effect_receipt) noexcept
		: kind_{kind}, route_{route}, evidence_kind_{evidence_kind}, native_status_{native_status},
		  outward_status_{kind == sqlite_shm_reader_close_terminal_kind::closed
							  ? static_cast<int>(sqlite_native_map_status::ok)
							  : evidence_kind ==
									  sqlite_shm_reader_close_evidence_kind::exact_native_result &&
								  native_status &&
								  *native_status != static_cast<int>(sqlite_native_map_status::ok)
							  ? *native_status
							  : sqlite_ioerr_status},
		  native_effect_receipt_{std::move(native_effect_receipt)}
	{
	}

	sqlite_shm_reader_close_terminal_kind
	sqlite_shm_reader_close_terminal_result::kind() const noexcept
	{
		return kind_;
	}

	sqlite_shm_reader_close_route sqlite_shm_reader_close_terminal_result::route() const noexcept
	{
		return route_;
	}

	sqlite_shm_reader_close_evidence_kind
	sqlite_shm_reader_close_terminal_result::evidence_kind() const noexcept
	{
		return evidence_kind_;
	}

	std::optional<int> sqlite_shm_reader_close_terminal_result::native_status() const noexcept
	{
		return native_status_;
	}

	int sqlite_shm_reader_close_terminal_result::outward_status() const noexcept
	{
		return outward_status_;
	}

	const std::optional<sqlite_backend_opaque_identity>&
	sqlite_shm_reader_close_terminal_result::native_effect_receipt() const noexcept
	{
		return native_effect_receipt_;
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

	sqlite_shm_verified_reader_attachment_post_map_receipt::
		sqlite_shm_verified_reader_attachment_post_map_receipt(
			sqlite_shm_reader_attachment_map_request request,
			const std::uint64_t generation,
			sqlite_shm_mapping_tuple mapping,
			sqlite_shm_reader_native_attachment_identity observed_attachment,
			sqlite_backend_opaque_identity zero_resize_effect_receipt)
		: request_{std::move(request)}, generation_{generation}, mapping_{mapping},
		  observed_attachment_{std::move(observed_attachment)},
		  zero_resize_effect_receipt_{std::move(zero_resize_effect_receipt)}
	{
	}

	const sqlite_shm_reader_attachment_map_request&
	sqlite_shm_verified_reader_attachment_post_map_receipt::request() const noexcept
	{
		return request_;
	}

	std::uint64_t
	sqlite_shm_verified_reader_attachment_post_map_receipt::generation() const noexcept
	{
		return generation_;
	}

	const sqlite_shm_mapping_tuple&
	sqlite_shm_verified_reader_attachment_post_map_receipt::mapping() const noexcept
	{
		return mapping_;
	}

	const sqlite_shm_reader_native_attachment_identity&
	sqlite_shm_verified_reader_attachment_post_map_receipt::observed_attachment() const noexcept
	{
		return observed_attachment_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_verified_reader_attachment_post_map_receipt::zero_resize_effect_receipt()
		const noexcept
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

	struct sqlite_shm_reader_lifecycle_sequence_source::state
	{
		struct mint_result
		{
			std::uint64_t first{};
			std::uint64_t last{};
			std::uint64_t terminal_slot{};
			generation_failure failure{generation_failure::unavailable};
			bool succeeded{};
		};

		struct terminal_reservation_result
		{
			std::array<std::uint64_t, 4> slots{};
			generation_failure failure{generation_failure::unavailable};
			bool succeeded{};
		};

		explicit state(const std::uint64_t first) : next{first}, exhausted{first == 0U} {}

		[[nodiscard]] mint_result
		consume_terminal_reservation(const std::uint64_t slot,
									 const std::uint64_t joined_slot = 0U) noexcept
		{
			if (slot == 0U || slot == joined_slot)
				return {0U, 0U, 0U, generation_failure::unavailable, false};
			try
			{
				std::scoped_lock lock{mutex};
				const auto found =
					std::find(active_terminal_slots.begin(), active_terminal_slots.end(), slot);
				const auto joined = joined_slot == 0U ? active_terminal_slots.end()
													  : std::find(active_terminal_slots.begin(),
																  active_terminal_slots.end(),
																  joined_slot);
				if (found == active_terminal_slots.end() ||
					(joined_slot != 0U && joined == active_terminal_slots.end()))
					return {0U, 0U, 0U, generation_failure::unavailable, false};
				const auto sequence = next;
				if (joined_slot != 0U)
				{
					const auto first_index = static_cast<std::size_t>(
						std::distance(active_terminal_slots.begin(), found));
					const auto joined_index = static_cast<std::size_t>(
						std::distance(active_terminal_slots.begin(), joined));
					active_terminal_slots.erase(
						active_terminal_slots.begin() +
						static_cast<std::ptrdiff_t>(std::max(first_index, joined_index)));
					active_terminal_slots.erase(
						active_terminal_slots.begin() +
						static_cast<std::ptrdiff_t>(std::min(first_index, joined_index)));
				}
				else
					active_terminal_slots.erase(found);
				last_issued = sequence;
				if (sequence == std::numeric_limits<std::uint64_t>::max())
					exhausted = true;
				else
					++next;
				return {sequence, sequence, slot, generation_failure::unavailable, true};
			}
			catch (...)
			{
				unavailable.store(true, std::memory_order_release);
				return {0U, 0U, 0U, generation_failure::unavailable, false};
			}
		}

		[[nodiscard]] terminal_reservation_result
		reserve_admission_binding(const std::size_t count) noexcept
		{
			if (count == 0U || count > 4U || unavailable.load(std::memory_order_acquire))
				return {};
			try
			{
				std::scoped_lock lock{mutex};
				if (unavailable.load(std::memory_order_relaxed) || exhausted ||
					terminal_slot_exhausted)
					return {{},
							exhausted || terminal_slot_exhausted ? generation_failure::exhausted
																 : generation_failure::unavailable,
							false};
				const auto remaining = std::numeric_limits<std::uint64_t>::max() - next + 1U;
				const auto reserved =
					active_terminal_slots.size() + active_admission_binding_slots.size();
				if (reserved > remaining || count > remaining - reserved ||
					count - 1U > std::numeric_limits<std::uint64_t>::max() - next_terminal_slot)
				{
					exhausted = true;
					return {{}, generation_failure::exhausted, false};
				}
				active_admission_binding_slots.reserve(active_admission_binding_slots.size() +
													   count);
				active_terminal_slots.reserve(active_terminal_slots.size() +
											  active_admission_binding_slots.size() + count);
				terminal_reservation_result result;
				for (std::size_t index = 0; index < count; ++index)
				{
					result.slots[index] = next_terminal_slot++;
					active_admission_binding_slots.push_back(result.slots[index]);
				}
				if (next_terminal_slot == 0U)
					terminal_slot_exhausted = true;
				result.succeeded = true;
				return result;
			}
			catch (...)
			{
				unavailable.store(true, std::memory_order_release);
				return {};
			}
		}

		[[nodiscard]] terminal_reservation_result
		reserve_terminal_reservations(const std::size_t count) noexcept
		{
			if (count == 0U || count > 4U || unavailable.load(std::memory_order_acquire))
				return {};
			try
			{
				std::scoped_lock lock{mutex};
				if (unavailable.load(std::memory_order_relaxed) || exhausted ||
					terminal_slot_exhausted)
					return {{},
							exhausted || terminal_slot_exhausted ? generation_failure::exhausted
																 : generation_failure::unavailable,
							false};
				const auto remaining = std::numeric_limits<std::uint64_t>::max() - next + 1U;
				const auto reserved =
					active_terminal_slots.size() + active_admission_binding_slots.size();
				if (reserved > remaining || count > remaining - reserved ||
					count - 1U > std::numeric_limits<std::uint64_t>::max() - next_terminal_slot)
				{
					exhausted = true;
					return {{}, generation_failure::exhausted, false};
				}
				active_terminal_slots.reserve(active_terminal_slots.size() + count);
				terminal_reservation_result result;
				auto slot = result.slots.begin();
				for (std::size_t reservations_left = count; reservations_left != 0U;
					 --reservations_left, ++slot)
				{
					*slot = next_terminal_slot++;
					active_terminal_slots.push_back(*slot);
				}
				if (next_terminal_slot == 0U)
					terminal_slot_exhausted = true;
				result.succeeded = true;
				return result;
			}
			catch (...)
			{
				unavailable.store(true, std::memory_order_release);
				return {};
			}
		}

		/**
		 * Issues one admission sequence from a previously reserved binding and publishes every
		 * remaining binding slot as the corresponding future terminal permit. A source that
		 * became unavailable after reservation must still drain this already-bound admission.
		 */
		[[nodiscard]] mint_result
		commit_admission_binding(const std::span<const std::uint64_t> slots) noexcept
		{
			if (slots.size() < 2U)
				return {0U, 0U, 0U, generation_failure::unavailable, false};
			try
			{
				std::scoped_lock lock{mutex};
				for (std::size_t index = 0; index < slots.size(); ++index)
				{
					if (slots[index] == 0U ||
						std::ranges::find(slots.first(index), slots[index]) !=
							slots.first(index).end() ||
						std::find(active_admission_binding_slots.begin(),
								  active_admission_binding_slots.end(),
								  slots[index]) == active_admission_binding_slots.end())
						return {0U, 0U, 0U, generation_failure::unavailable, false};
				}
				const auto sequence = next;
				// Capacity for these moves was reserved before any binding state was published.
				for (const auto terminal_slot : slots.subspan(1U))
					active_terminal_slots.push_back(terminal_slot);
				std::erase_if(active_admission_binding_slots,
							  [slots](const std::uint64_t candidate)
							  {
								  return std::ranges::find(slots, candidate) != slots.end();
							  });
				last_issued = sequence;
				if (sequence == std::numeric_limits<std::uint64_t>::max())
					exhausted = true;
				else
					++next;
				return {sequence, sequence, slots[1], generation_failure::unavailable, true};
			}
			catch (...)
			{
				unavailable.store(true, std::memory_order_release);
				return {0U, 0U, 0U, generation_failure::unavailable, false};
			}
		}

		[[nodiscard]] mint_result
		consume_terminal_reservations(const std::span<const std::uint64_t> slots) noexcept
		{
			if (slots.empty())
				return {0U, 0U, 0U, generation_failure::unavailable, false};
			try
			{
				std::scoped_lock lock{mutex};
				for (std::size_t index = 0; index < slots.size(); ++index)
				{
					if (slots[index] == 0U ||
						std::ranges::find(slots.first(index), slots[index]) !=
							slots.first(index).end() ||
						std::find(active_terminal_slots.begin(),
								  active_terminal_slots.end(),
								  slots[index]) == active_terminal_slots.end())
						return {0U, 0U, 0U, generation_failure::unavailable, false};
				}
				const auto first = next;
				const auto width = static_cast<std::uint64_t>(slots.size() - 1U);
				if (width > std::numeric_limits<std::uint64_t>::max() - first)
					return {0U, 0U, 0U, generation_failure::unavailable, false};
				const auto last = first + width;
				std::erase_if(active_terminal_slots,
							  [slots](const std::uint64_t candidate)
							  {
								  return std::ranges::find(slots, candidate) != slots.end();
							  });
				last_issued = last;
				if (last == std::numeric_limits<std::uint64_t>::max())
					exhausted = true;
				else
					next = last + 1U;
				return {first, last, slots.front(), generation_failure::unavailable, true};
			}
			catch (...)
			{
				unavailable.store(true, std::memory_order_release);
				return {0U, 0U, 0U, generation_failure::unavailable, false};
			}
		}

		void cancel_terminal_reservation(const std::uint64_t slot) noexcept
		{
			if (slot == 0U)
				return;
			try
			{
				std::scoped_lock lock{mutex};
				const auto found =
					std::find(active_terminal_slots.begin(), active_terminal_slots.end(), slot);
				if (found != active_terminal_slots.end())
					active_terminal_slots.erase(found);
				const auto admission_binding = std::find(active_admission_binding_slots.begin(),
														 active_admission_binding_slots.end(),
														 slot);
				if (admission_binding != active_admission_binding_slots.end())
					active_admission_binding_slots.erase(admission_binding);
			}
			catch (...)
			{
				unavailable.store(true, std::memory_order_release);
			}
		}

		[[nodiscard]] std::uint64_t observed_last_issued() const noexcept
		{
			try
			{
				std::scoped_lock lock{mutex};
				return last_issued;
			}
			catch (...)
			{
				unavailable.store(true, std::memory_order_release);
				return 0U;
			}
		}

		[[nodiscard]] bool observed_exhausted() const noexcept
		{
			try
			{
				std::scoped_lock lock{mutex};
				return exhausted || unavailable.load(std::memory_order_relaxed);
			}
			catch (...)
			{
				unavailable.store(true, std::memory_order_release);
				return true;
			}
		}

		[[nodiscard]] std::size_t observed_outstanding_terminal_slot_count() const noexcept
		{
			try
			{
				std::scoped_lock lock{mutex};
				return active_terminal_slots.size();
			}
			catch (...)
			{
				unavailable.store(true, std::memory_order_release);
				return 0U;
			}
		}

		[[nodiscard]] std::vector<std::uint64_t> observed_terminal_slots() const
		{
			std::scoped_lock lock{mutex};
			auto output = active_terminal_slots;
			std::ranges::sort(output);
			return output;
		}

		void exhaust() noexcept
		{
			try
			{
				std::scoped_lock lock{mutex};
				exhausted = true;
			}
			catch (...)
			{
				unavailable.store(true, std::memory_order_release);
			}
		}

		void make_unavailable() noexcept
		{
			unavailable.store(true, std::memory_order_release);
		}

		mutable std::mutex mutex;
		std::uint64_t next{};
		std::uint64_t last_issued{};
		std::uint64_t next_terminal_slot{1U};
		std::vector<std::uint64_t> active_terminal_slots;
		std::vector<std::uint64_t> active_admission_binding_slots;
		bool exhausted{};
		bool terminal_slot_exhausted{};
		mutable std::atomic_bool unavailable{false};
	};

	sqlite_shm_reader_lifecycle_sequence_source::sqlite_shm_reader_lifecycle_sequence_source(
		const std::uint64_t first_sequence)
		: state_{std::make_shared<state>(first_sequence)}
	{
	}

	sqlite_shm_reader_lifecycle_sequence_source::~sqlite_shm_reader_lifecycle_sequence_source() =
		default;

	const void* sqlite_shm_reader_lifecycle_sequence_source::identity_for_testing() const noexcept
	{
		return state_.get();
	}

	std::uint64_t sqlite_shm_reader_lifecycle_sequence_source::observed_last_issued() const noexcept
	{
		return state_ ? state_->observed_last_issued() : 0U;
	}

	std::uint64_t
	sqlite_shm_reader_lifecycle_sequence_source::last_issued_for_testing() const noexcept
	{
		return observed_last_issued();
	}

	void sqlite_shm_reader_lifecycle_sequence_source::exhaust_for_testing() noexcept
	{
		if (state_)
			state_->exhaust();
	}

	void sqlite_shm_reader_lifecycle_sequence_source::make_unavailable_for_testing() noexcept
	{
		if (state_)
			state_->make_unavailable();
	}

	namespace detail
	{
		class sqlite_shm_mapping_lease_state final
			: public std::enable_shared_from_this<sqlite_shm_mapping_lease_state>
		{
		  public:
			sqlite_shm_mapping_lease_state(
				sqlite_shm_lease_family_binding family,
				std::shared_ptr<sqlite_shm_mapping_generation_source> generations,
				std::shared_ptr<sqlite_shm_reader_lifecycle_sequence_source>
					reader_lifecycle_sequences)
				: family_{std::move(family)}, generations_{std::move(generations)},
				  reader_lifecycle_sequences_{std::move(reader_lifecycle_sequences)}
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
							 std::ranges::any_of(reader_attachment_maps_,
												 [](const reader_attachment_map_record& map)
												 {
													 return map.retirement_blocker &&
														 map.phase !=
														 reader_phase::terminal_quarantined;
												 }) ||
							 std::ranges::any_of(
								 reader_sessions_,
								 [](const reader_session_record& session)
								 {
									 return session.phase ==
										 reader_session_record_phase::reserved_for_first_map;
								 }) ||
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

			[[nodiscard]] sqlite_shm_lease_result<void> register_registry_reader_open(
				const std::uint64_t registry_open_token,
				const std::shared_ptr<sqlite_shm_reader_open_lineage_seal>& seal,
				const sqlite_shm_reader_open_epoch_binding& binding,
				const sqlite_shm_reader_open_admission_guard* const admission_guard)
			{
				if (registry_open_token == 0U || !seal ||
					!valid_reader_open_epoch_binding(binding) ||
					(admission_guard != nullptr && !admission_guard->valid()))
					return sqlite_shm_unexpected(
						rejection(sqlite_shm_lease_rejection_reason::invalid_request,
								  sqlite_shm_lease_recovery_action::deny_before_native_map));
				try
				{
					std::scoped_lock lock{mutex_};
					if (auto blocked = blocked_locked(
							sqlite_shm_lease_recovery_action::deny_before_native_map))
						return sqlite_shm_unexpected(*blocked);
					if (binding.family != family_)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					if (!seal->authority_valid.load(std::memory_order_acquire))
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::quarantined,
									  sqlite_shm_lease_recovery_action::quarantine_no_retry));
					const auto compact_tombstone = std::find_if(
						reader_open_close_tombstones_.begin(),
						reader_open_close_tombstones_.end(),
						[registry_open_token,
						 &binding](const sqlite_shm_reader_open_epoch_close_tombstone& tombstone)
						{
							return tombstone.registry_open_token == registry_open_token ||
								tombstone.binding == binding;
						});
					if (compact_tombstone != reader_open_close_tombstones_.end())
					{
						if (compact_tombstone->registry_open_token == registry_open_token &&
							compact_tombstone->binding == binding)
							return sqlite_shm_unexpected(stale_token(
								sqlite_shm_lease_recovery_action::deny_before_native_map));
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					}
					if (std::ranges::any_of(
							registry_reader_opens_,
							[registry_open_token, &binding](const registry_reader_open_record& open)
							{
								return open.token == registry_open_token || open.binding == binding;
							}) ||
						std::ranges::any_of(reader_close_terminals_,
											[registry_open_token,
											 &binding](const reader_close_terminal_record& terminal)
											{
												return terminal.registry_open_token ==
													registry_open_token ||
													terminal.binding == binding;
											}))
					{
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					if (!can_allocate_tokens_locked(1U))
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									  sqlite_shm_lease_recovery_action::quarantine_no_retry));
					const auto sequence_binding =
						reserve_reader_lifecycle_admission_binding_locked(3U);
					if (!sequence_binding.succeeded)
						return sqlite_shm_unexpected(
							rejection(sequence_binding.failure == generation_failure::exhausted
										  ? sqlite_shm_lease_rejection_reason::generation_exhausted
										  : sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					reader_terminal_slot_guard origin_slot{
						reader_lifecycle_sequences_->state_.get(), sequence_binding.slots[0]};
					reader_terminal_slot_guard cut_slot{reader_lifecycle_sequences_->state_.get(),
														sequence_binding.slots[1]};
					reader_terminal_slot_guard terminal_slot{
						reader_lifecycle_sequences_->state_.get(), sequence_binding.slots[2]};
					const auto close_owner_token = next_token_;
					auto next_custodies = reader_custodies_;
					next_custodies.push_back({sqlite_shm_reader_custody_kind::connection_close,
											  sqlite_shm_reader_custody_state::live,
											  std::nullopt,
											  close_owner_token,
											  0U,
											  0U,
											  binding});
					std::list<registry_reader_open_record> prepared_opens;
					prepared_opens.emplace_back(registry_open_token,
												seal,
												binding,
												close_owner_token,
												sequence_binding.slots[1],
												sequence_binding.slots[2]);
					if (!seal->authority_valid.load(std::memory_order_acquire))
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::quarantined,
									  sqlite_shm_lease_recovery_action::quarantine_no_retry));
					// This acquire-load is the guarded registry admission linearization point.
					// A peer abandonment store ordered before it prevents all publication; a
					// later store revokes fresh authority but leaves this already-owned close
					// lineage drainable.
					if (admission_guard != nullptr && !admission_guard->admission_visible_now())
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::quarantined,
									  sqlite_shm_lease_recovery_action::quarantine_no_retry));
					const auto sequences = commit_reader_lifecycle_admission_binding_locked(
						std::span{sequence_binding.slots}.first(3U));
					if (!sequences.succeeded)
					{
						return sqlite_shm_unexpected(
							rejection(sequences.failure == generation_failure::exhausted
										  ? sqlite_shm_lease_rejection_reason::generation_exhausted
										  : sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					}
					(void)allocate_token_unchecked_locked();
					next_custodies.back().origin_sequence = sequences.first;
					prepared_opens.back().close_origin_sequence = sequences.first;
					registry_reader_opens_.splice(registry_reader_opens_.end(), prepared_opens);
					reader_custodies_.swap(next_custodies);
					reader_last_committed_sequence_ =
						std::max(reader_last_committed_sequence_, sequences.first);
					origin_slot.release();
					cut_slot.release();
					terminal_slot.release();
					return {};
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_live_close_obligation>
			begin_registry_reader_live_close(
				const std::uint64_t registry_open_token,
				const std::shared_ptr<sqlite_shm_reader_open_lineage_seal>& seal,
				const sqlite_shm_reader_open_epoch_binding& binding,
				sqlite_shm_reader_handoff& handoff,
				const sqlite_shm_reader_unmap_request& unmap_request,
				const sqlite_shm_reader_close_request& close_request) noexcept
			{
				std::uint64_t committed_cut_sequence{};
				if (registry_open_token == 0U || !seal ||
					!valid_reader_open_epoch_binding(binding) ||
					!valid_callback(unmap_request.callback) ||
					!valid_callback(close_request.callback) ||
					unmap_request.callback.invocation_token ==
						close_request.callback.invocation_token ||
					unmap_request.caller_delete_flag != 0 ||
					unmap_request.delegated_delete_flag != 0)
					return sqlite_shm_unexpected(
						rejection(sqlite_shm_lease_rejection_reason::invalid_request,
								  sqlite_shm_lease_recovery_action::deny_before_native_map));
				try
				{
					if (fail_next_reader_operation_mutex_acquire_for_testing_.exchange(
							false, std::memory_order_acq_rel))
						throw reader_operation_mutex_acquire_injected_failure{};
					std::scoped_lock lock{mutex_};
					if (!owns(handoff.state_, handoff.token_))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					const auto open = std::find_if(
						registry_reader_opens_.begin(),
						registry_reader_opens_.end(),
						[registry_open_token](const registry_reader_open_record& candidate)
						{
							return candidate.token == registry_open_token;
						});
					const auto group = find_by_token(reader_attachment_groups_, handoff.token_);
					if (open == registry_reader_opens_.end() ||
						group == reader_attachment_groups_.end())
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					if (open->seal.get() != seal.get() || open->binding != binding ||
						binding.family != family_ || !group->registry_bound ||
						group->expected.registry_open_token() != open->token ||
						!reader_attachment_matches_open_epoch_binding(group->expected,
																	  open->binding))
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::resubmit_via_bound_route));
					if (!seal->authority_valid.load(std::memory_order_acquire) ||
						emergency_quarantine_.load(std::memory_order_acquire))
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::quarantined,
									  sqlite_shm_lease_recovery_action::quarantine_no_retry));
					if (open->close_phase != sqlite_shm_reader_connection_close_phase::open ||
						group->generation != handoff.generation_ ||
						group->reservation_phase !=
							sqlite_shm_reader_attachment_reservation_phase::observed_present ||
						!group->observed_identity ||
						group->phase != reader_attachment_group_phase::active ||
						group->composite_close_owner_token != 0U ||
						group->composite_close_registry_open_token != 0U ||
						group->composite_close_cut_sequence != 0U)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::retiring,
									  sqlite_shm_lease_recovery_action::
										  await_complete_attachment_gate_boundary));
					for (const auto& candidate : reader_attachment_groups_)
					{
						if (&candidate == &*group || !candidate.registry_bound)
							continue;
						const auto token_matches =
							candidate.expected.registry_open_token() == open->token;
						const auto binding_matches = reader_attachment_matches_open_epoch_binding(
							candidate.expected, open->binding);
						if (!token_matches && !binding_matches)
							continue;
						if (token_matches != binding_matches ||
							!reader_local_phase1_compact_group_shape_is_exact_locked(candidate))
							return sqlite_shm_unexpected(
								rejection(sqlite_shm_lease_rejection_reason::retiring,
										  sqlite_shm_lease_recovery_action::
											  await_complete_attachment_gate_boundary));
					}
					if (!reader_group_custody_census_is_exact_locked(*group, false, false, true) ||
						!reader_open_close_custody_census_is_exact_locked(
							*open, false, false, false, &*group))
					{
						quarantine_reader_group_locked(
							*group,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						quarantine_reader_open_locked(
							*open,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						handoff.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto blockers = reader_live_close_cut_blocker_decision_locked(
						*group, unmap_request.callback, close_request.callback);
					if (blockers == reader_unmap_cut_blocker_decision::ambiguous)
					{
						quarantine_reader_group_locked(
							*group,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						quarantine_reader_open_locked(
							*open,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						handoff.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					if (unmap_request.callback.invocation_token ==
							close_request.callback.invocation_token ||
						!callback_can_start_locked(unmap_request.callback, 0U, group->token) ||
						!callback_can_start_locked(close_request.callback, 0U, group->token) ||
						reader_effect_identity_seen_locked(
							close_request.callback.invocation_token, 0U, 0U, 0U) ||
						reader_session_terminal_identity_seen_locked(
							close_request.callback.invocation_token))
					{
						quarantine_reader_group_locked(
							*group,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::presented_invalid);
						quarantine_reader_open_locked(
							*open,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::presented_invalid);
						handoff.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					if (std::exchange(fail_next_reader_unmap_begin_preparation_for_testing_,
									  false) ||
						std::exchange(fail_next_reader_close_begin_preparation_for_testing_, false))
						throw reader_unmap_begin_preparation_injected_failure{};

					auto prepared_unmap_callback = unmap_request.callback;
					auto prepared_close_callback = close_request.callback;
					auto prepared_state = shared_from_this();
					auto next_custodies = reader_custodies_;
					const auto connection_close = std::find_if(
						next_custodies.begin(),
						next_custodies.end(),
						[&open](const reader_custody_record& custody)
						{
							return custody.kind ==
								sqlite_shm_reader_custody_kind::connection_close &&
								custody.owner_token == open->close_owner_token &&
								custody.state == sqlite_shm_reader_custody_state::live &&
								!custody.attachment && custody.open_epoch &&
								*custody.open_epoch == open->binding;
						});
					const auto source_kind = group->existing_group_deferred_cleanup_required
						? sqlite_shm_reader_custody_kind::normal_or_deferred_unmap
						: sqlite_shm_reader_custody_kind::attachment_group_handoff;
					const auto group_source =
						std::find_if(next_custodies.begin(),
									 next_custodies.end(),
									 [&group, source_kind](const reader_custody_record& custody)
									 {
										 return custody.kind == source_kind &&
											 custody.owner_token == group->token &&
											 custody.attachment == group->expected &&
											 custody.state == sqlite_shm_reader_custody_state::live;
									 });
					if (connection_close == next_custodies.end() ||
						group_source == next_custodies.end() ||
						group->unmap_cut_sequence_slot == 0U ||
						group->unmap_terminal_sequence_slot == 0U ||
						open->close_cut_sequence_slot == 0U ||
						open->close_terminal_sequence_slot == 0U ||
						group->unmap_cut_sequence_slot == open->close_cut_sequence_slot)
					{
						quarantine_reader_group_locked(
							*group,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						quarantine_reader_open_locked(
							*open,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						handoff.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto connection_close_index = static_cast<std::size_t>(
						std::distance(next_custodies.begin(), connection_close));
					const auto group_source_index = static_cast<std::size_t>(
						std::distance(next_custodies.begin(), group_source));
					const auto composite_close_index = next_custodies.size();
					next_custodies.emplace_back(
						sqlite_shm_reader_custody_kind::close_cut_or_composite,
						sqlite_shm_reader_custody_state::live,
						group->expected,
						open->close_owner_token,
						0U,
						0U,
						open->binding);
					const auto unmap_cut_index = next_custodies.size();
					next_custodies.emplace_back(
						sqlite_shm_reader_custody_kind::unmap_cut,
						sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt,
						group->expected,
						group->token,
						0U,
						0U);
					const auto waiter_index = next_custodies.size();
					next_custodies.emplace_back(
						sqlite_shm_reader_custody_kind::bounded_waiter_or_continuation,
						sqlite_shm_reader_custody_state::live,
						group->expected,
						group->token,
						0U,
						0U);
					const auto reporter_index = next_custodies.size();
					next_custodies.emplace_back(sqlite_shm_reader_custody_kind::terminal_reporter,
												sqlite_shm_reader_custody_state::live,
												group->expected,
												group->token,
												0U,
												0U);
					const auto sequences = consume_reader_lifecycle_terminal_slots_locked(
						group->unmap_cut_sequence_slot, open->close_cut_sequence_slot);
					if (!sequences.succeeded)
					{
						quarantine_reader_group_locked(
							*group,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						quarantine_reader_open_locked(
							*open,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						handoff.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					committed_cut_sequence = sequences.first;
					group->unmap_cut_sequence_slot = 0U;
					open->close_cut_sequence_slot = 0U;
					next_custodies[connection_close_index].state =
						sqlite_shm_reader_custody_state::transferred_to_exact_successor;
					next_custodies[connection_close_index].destination_sequence = sequences.first;
					next_custodies[group_source_index].state =
						sqlite_shm_reader_custody_state::transferred_to_exact_successor;
					next_custodies[group_source_index].destination_sequence = sequences.first;
					next_custodies[composite_close_index].origin_sequence = sequences.first;
					next_custodies[unmap_cut_index].origin_sequence = sequences.first;
					next_custodies[unmap_cut_index].destination_sequence = sequences.first;
					next_custodies[waiter_index].origin_sequence = sequences.first;
					next_custodies[reporter_index].origin_sequence = sequences.first;
					group->unmap_callback.emplace(std::move(prepared_unmap_callback));
					group->unmap_caller_delete_flag = 0;
					group->unmap_delegated_delete_flag = 0;
					group->phase = blockers == reader_unmap_cut_blocker_decision::none
						? reader_attachment_group_phase::native_cleanup_admitted
						: reader_attachment_group_phase::unmap_cut_sealing;
					group->group_destination_sequence = sequences.first;
					group->unmap_cut_sequence = sequences.first;
					group->composite_close_owner_token = open->close_owner_token;
					group->composite_close_registry_open_token = open->token;
					group->composite_close_cut_sequence = sequences.first;
					open->close_phase = sqlite_shm_reader_connection_close_phase::close_admitted;
					open->close_route = sqlite_shm_reader_close_route::close_after_confirmed_unmap;
					open->close_cut_sequence = sequences.first;
					open->close_callback.emplace(std::move(prepared_close_callback));
					reader_custodies_.swap(next_custodies);
					handoff.disarm();
					reader_last_committed_sequence_ =
						std::max(reader_last_committed_sequence_, sequences.last);
					if (blockers == reader_unmap_cut_blocker_decision::same_thread_or_reentrant)
					{
						quarantine_reader_group_locked(
							*group,
							sequences.first,
							sqlite_shm_reader_terminal_quarantine_reason::peer_quarantine);
						quarantine_reader_open_locked(
							*open,
							sequences.first,
							sqlite_shm_reader_terminal_quarantine_reason::peer_quarantine);
						return sqlite_shm_unexpected(ambiguous());
					}
					if (!reader_group_custody_census_is_exact_locked(
							*group,
							true,
							true,
							blockers == reader_unmap_cut_blocker_decision::other_thread) ||
						!reader_open_close_custody_census_is_exact_locked(
							*open, true, false, false, &*group))
					{
						quarantine_reader_group_locked(
							*group,
							sequences.first,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						quarantine_reader_open_locked(
							*open,
							sequences.first,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						return sqlite_shm_unexpected(ambiguous());
					}
					return sqlite_shm_reader_live_close_obligation{
						std::move(prepared_state),
						sqlite_shm_lease_token_identity{group->token},
						sqlite_shm_mapping_generation_identity{group->generation},
						sqlite_shm_lease_token_identity{open->close_owner_token},
						open->token,
						blockers == reader_unmap_cut_blocker_decision::none
							? sqlite_shm_reader_live_close_obligation::phase::unmap_admitted
							: sqlite_shm_reader_live_close_obligation::phase::unmap_waiting};
				}
				catch (...)
				{
					try
					{
						std::scoped_lock lock{mutex_};
						const auto group = find_by_token(reader_attachment_groups_, handoff.token_);
						const auto open = std::find_if(
							registry_reader_opens_.begin(),
							registry_reader_opens_.end(),
							[registry_open_token](const registry_reader_open_record& candidate)
							{
								return candidate.token == registry_open_token;
							});
						if (committed_cut_sequence != 0U &&
							group != reader_attachment_groups_.end())
							quarantine_reader_group_locked(
								*group,
								committed_cut_sequence,
								sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						if (open != registry_reader_opens_.end())
							quarantine_reader_open_locked(
								*open,
								committed_cut_sequence,
								sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						if (committed_cut_sequence != 0U)
							handoff.disarm();
					}
					catch (...)
					{
						emergency_quarantine_.store(true, std::memory_order_release);
					}
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_unmap_cut_result>
			poll_registry_reader_live_close_unmap_cut(
				const std::uint64_t registry_open_token,
				const std::shared_ptr<sqlite_shm_reader_open_lineage_seal>& seal,
				const sqlite_shm_reader_open_epoch_binding& binding,
				sqlite_shm_reader_live_close_obligation& close,
				const sqlite_shm_callback_execution_receipt& close_callback) noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					if (close.terminal_presentation_disabled_ || !close.state_ ||
						close.state_.get() != this || close.group_token_ == 0U ||
						close.close_owner_token_ == 0U || close.registry_open_token_ == 0U ||
						close.phase_ == sqlite_shm_reader_live_close_obligation::phase::close_ready)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					if (registry_open_token != close.registry_open_token_)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::resubmit_via_bound_route));
					const auto group = find_by_token(reader_attachment_groups_, close.group_token_);
					const auto open = std::find_if(
						registry_reader_opens_.begin(),
						registry_reader_opens_.end(),
						[registry_open_token, &close](const registry_reader_open_record& candidate)
						{
							return candidate.token == registry_open_token &&
								candidate.close_owner_token == close.close_owner_token_;
						});
					const auto exact_binding = group != reader_attachment_groups_.end() &&
						open != registry_reader_opens_.end() &&
						registry_open_token == close.registry_open_token_ && seal &&
						valid_reader_open_epoch_binding(binding) &&
						open->seal.get() == seal.get() && open->binding == binding &&
						binding.family == family_ &&
						seal->authority_valid.load(std::memory_order_acquire) &&
						group->generation == close.generation_ && group->registry_bound &&
						group->reservation_phase ==
							sqlite_shm_reader_attachment_reservation_phase::observed_present &&
						group->observed_identity && group->unmap_callback && open->close_callback &&
						*open->close_callback == close_callback &&
						group->composite_close_owner_token == close.close_owner_token_ &&
						group->composite_close_registry_open_token == close.registry_open_token_ &&
						group->composite_close_cut_sequence != 0U &&
						group->composite_close_cut_sequence == group->unmap_cut_sequence &&
						group->composite_close_cut_sequence == open->close_cut_sequence &&
						group->expected.registry_open_token() == open->token &&
						reader_attachment_matches_open_epoch_binding(group->expected,
																	 open->binding) &&
						open->close_phase ==
							sqlite_shm_reader_connection_close_phase::close_admitted &&
						open->close_route ==
							sqlite_shm_reader_close_route::close_after_confirmed_unmap;
					if (!exact_binding)
					{
						if (group != reader_attachment_groups_.end())
							quarantine_reader_group_locked(
								*group,
								0U,
								sqlite_shm_reader_terminal_quarantine_reason::presented_invalid);
						if (open != registry_reader_opens_.end())
							quarantine_reader_open_locked(
								*open,
								0U,
								sqlite_shm_reader_terminal_quarantine_reason::presented_invalid);
						quarantine_locked();
						close.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					if (group->unmap_callback->invocation_token ==
							open->close_callback->invocation_token ||
						!callback_can_start_locked(
							*group->unmap_callback, 0U, group->token, open->token) ||
						!callback_can_start_locked(
							*open->close_callback, 0U, group->token, open->token))
					{
						quarantine_reader_group_locked(
							*group,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::presented_invalid);
						quarantine_reader_open_locked(
							*open,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::presented_invalid);
						close.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					if (close.phase_ ==
						sqlite_shm_reader_live_close_obligation::phase::unmap_admitted)
					{
						if (group->phase !=
								reader_attachment_group_phase::native_cleanup_admitted ||
							!reader_group_custody_census_is_exact_locked(*group, true, true) ||
							!reader_open_close_custody_census_is_exact_locked(
								*open, true, false, false, &*group))
						{
							quarantine_reader_group_locked(
								*group,
								0U,
								sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
							quarantine_reader_open_locked(
								*open,
								0U,
								sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
							close.disarm();
							return sqlite_shm_unexpected(ambiguous());
						}
						return sqlite_shm_reader_unmap_cut_result{
							sqlite_shm_reader_unmap_cut_progress::native_effect_ready,
							group->generation};
					}
					if (close.phase_ !=
							sqlite_shm_reader_live_close_obligation::phase::unmap_waiting ||
						group->phase != reader_attachment_group_phase::unmap_cut_sealing)
					{
						quarantine_reader_group_locked(
							*group,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						quarantine_reader_open_locked(
							*open,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						close.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto blockers = reader_live_close_cut_blocker_decision_locked(
						*group, *group->unmap_callback, *open->close_callback);
					if (blockers == reader_unmap_cut_blocker_decision::ambiguous ||
						blockers == reader_unmap_cut_blocker_decision::same_thread_or_reentrant)
					{
						quarantine_reader_group_locked(
							*group,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::peer_quarantine);
						quarantine_reader_open_locked(
							*open,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::peer_quarantine);
						close.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto waiting =
						blockers == reader_unmap_cut_blocker_decision::other_thread;
					if (!reader_group_custody_census_is_exact_locked(*group, true, true, waiting) ||
						!reader_open_close_custody_census_is_exact_locked(
							*open, true, false, false, &*group))
					{
						quarantine_reader_group_locked(
							*group,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						quarantine_reader_open_locked(
							*open,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						close.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					if (waiting)
						return sqlite_shm_reader_unmap_cut_result{
							sqlite_shm_reader_unmap_cut_progress::waiting, group->generation};
					group->phase = reader_attachment_group_phase::native_cleanup_admitted;
					close.phase_ = sqlite_shm_reader_live_close_obligation::phase::unmap_admitted;
					return sqlite_shm_reader_unmap_cut_result{
						sqlite_shm_reader_unmap_cut_progress::native_effect_ready,
						group->generation};
				}
				catch (...)
				{
					abandon_reader_live_close(close);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<void>
			fail_registry_reader_live_close_unmap_cut_wait(
				const std::uint64_t registry_open_token,
				const std::shared_ptr<sqlite_shm_reader_open_lineage_seal>& seal,
				const sqlite_shm_reader_open_epoch_binding& binding,
				sqlite_shm_reader_live_close_obligation& close,
				const sqlite_shm_callback_execution_receipt& close_callback,
				const sqlite_shm_retirement_wait_failure failure) noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					if (close.terminal_presentation_disabled_ || !close.state_ ||
						close.state_.get() != this || close.group_token_ == 0U ||
						close.close_owner_token_ == 0U || close.registry_open_token_ == 0U)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					if (registry_open_token != close.registry_open_token_)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::resubmit_via_bound_route));
					const auto group = find_by_token(reader_attachment_groups_, close.group_token_);
					const auto open = std::find_if(
						registry_reader_opens_.begin(),
						registry_reader_opens_.end(),
						[registry_open_token, &close](const registry_reader_open_record& candidate)
						{
							return candidate.token == registry_open_token &&
								candidate.close_owner_token == close.close_owner_token_;
						});
					const auto exact_wait = group != reader_attachment_groups_.end() &&
						open != registry_reader_opens_.end() &&
						registry_open_token == close.registry_open_token_ && seal &&
						valid_reader_open_epoch_binding(binding) &&
						open->seal.get() == seal.get() && open->binding == binding &&
						binding.family == family_ &&
						seal->authority_valid.load(std::memory_order_acquire) &&
						close.phase_ ==
							sqlite_shm_reader_live_close_obligation::phase::unmap_waiting &&
						group->generation == close.generation_ && group->registry_bound &&
						group->reservation_phase ==
							sqlite_shm_reader_attachment_reservation_phase::observed_present &&
						group->phase == reader_attachment_group_phase::unmap_cut_sealing &&
						group->observed_identity && group->unmap_callback && open->close_callback &&
						*open->close_callback == close_callback &&
						group->composite_close_owner_token == close.close_owner_token_ &&
						group->composite_close_registry_open_token == close.registry_open_token_ &&
						group->composite_close_cut_sequence != 0U &&
						group->composite_close_cut_sequence == group->unmap_cut_sequence &&
						group->composite_close_cut_sequence == open->close_cut_sequence &&
						group->expected.registry_open_token() == open->token &&
						reader_attachment_matches_open_epoch_binding(group->expected,
																	 open->binding) &&
						open->close_phase ==
							sqlite_shm_reader_connection_close_phase::close_admitted &&
						open->close_route ==
							sqlite_shm_reader_close_route::close_after_confirmed_unmap &&
						(failure == sqlite_shm_retirement_wait_failure::timeout ||
						 failure == sqlite_shm_retirement_wait_failure::unknown) &&
						reader_group_custody_census_is_exact_locked(*group, true, true, true) &&
						reader_open_close_custody_census_is_exact_locked(
												*open, true, false, false, &*group);
					if (!exact_wait)
					{
						if (group != reader_attachment_groups_.end())
							quarantine_reader_group_locked(
								*group,
								0U,
								sqlite_shm_reader_terminal_quarantine_reason::presented_invalid);
						if (open != registry_reader_opens_.end())
							quarantine_reader_open_locked(
								*open,
								0U,
								sqlite_shm_reader_terminal_quarantine_reason::presented_invalid);
						quarantine_locked();
						close.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					quarantine_reader_group_locked(
						*group,
						0U,
						sqlite_shm_reader_terminal_quarantine_reason::native_non_ok_or_unknown);
					quarantine_reader_open_locked(
						*open,
						0U,
						sqlite_shm_reader_terminal_quarantine_reason::native_non_ok_or_unknown);
					close.disarm();
					return sqlite_shm_unexpected(ambiguous());
				}
				catch (...)
				{
					abandon_reader_live_close(close);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_close_obligation>
			begin_registry_reader_close(
				const std::uint64_t registry_open_token,
				const std::shared_ptr<sqlite_shm_reader_open_lineage_seal>& seal,
				const sqlite_shm_reader_open_epoch_binding& binding,
				const sqlite_shm_reader_close_request& request,
				std::optional<sqlite_shm_reader_attachment_authority>* completed_activity =
					nullptr) noexcept
			{
				std::uint64_t committed_close_cut_sequence{};
				bool consumed_awaiting_ack_after_slot{};
				if (registry_open_token == 0U || !seal ||
					!valid_reader_open_epoch_binding(binding) ||
					!valid_callback(request.callback) ||
					(completed_activity && *completed_activity))
					return sqlite_shm_unexpected(
						rejection(sqlite_shm_lease_rejection_reason::invalid_request,
								  sqlite_shm_lease_recovery_action::deny_before_native_map));
				try
				{
					if (fail_next_reader_operation_mutex_acquire_for_testing_.exchange(
							false, std::memory_order_acq_rel))
						throw reader_operation_mutex_acquire_injected_failure{};
					std::scoped_lock lock{mutex_};
					const auto open = std::find_if(
						registry_reader_opens_.begin(),
						registry_reader_opens_.end(),
						[registry_open_token](const registry_reader_open_record& candidate)
						{
							return candidate.token == registry_open_token;
						});
					if (open == registry_reader_opens_.end())
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					if (open->seal.get() != seal.get() || open->binding != binding ||
						binding.family != family_)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::resubmit_via_bound_route));
					if (!seal->authority_valid.load(std::memory_order_acquire))
					{
						quarantine_reader_open_locked(
							*open,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::peer_quarantine);
						return sqlite_shm_unexpected(ambiguous());
					}
					if (emergency_quarantine_.load(std::memory_order_acquire))
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::quarantined,
									  sqlite_shm_lease_recovery_action::quarantine_no_retry));
					if (open->close_phase != sqlite_shm_reader_connection_close_phase::open)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					const auto route = reader_phase1_close_route_locked(*open);
					auto phase1_group = reader_attachment_groups_.end();
					if (!route)
					{
						for (auto group = reader_attachment_groups_.begin();
							 group != reader_attachment_groups_.end();
							 ++group)
						{
							if (!group->registry_bound)
								continue;
							const auto token_matches =
								group->expected.registry_open_token() == open->token;
							const auto binding_matches =
								reader_attachment_matches_open_epoch_binding(group->expected,
																			 open->binding);
							if (!token_matches && !binding_matches)
								continue;
							if (token_matches != binding_matches)
								return sqlite_shm_unexpected(
									rejection(sqlite_shm_lease_rejection_reason::retiring,
											  sqlite_shm_lease_recovery_action::
												  await_complete_attachment_gate_boundary));
							const auto exact_unformed = token_matches == binding_matches &&
								group->generation == group->expected.writer_mapping_generation() &&
								group->reservation_phase ==
									sqlite_shm_reader_attachment_reservation_phase::reserved &&
								group->phase == reader_attachment_group_phase::active &&
								!group->observed_identity && group->members.empty() &&
								group->audits.empty() && group->composite_close_owner_token == 0U &&
								group->composite_close_registry_open_token == 0U &&
								group->composite_close_cut_sequence == 0U &&
								group->composite_close_wait_resolution_sequence_slot == 0U &&
								group->composite_close_wait_resolution_sequence == 0U;
							if (!exact_unformed)
								return sqlite_shm_unexpected(
									rejection(sqlite_shm_lease_rejection_reason::retiring,
											  sqlite_shm_lease_recovery_action::
												  await_complete_attachment_gate_boundary));
							if (phase1_group != reader_attachment_groups_.end())
							{
								quarantine_reader_open_locked(
									*open,
									0U,
									sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
								return sqlite_shm_unexpected(ambiguous());
							}
							phase1_group = group;
						}
						if (phase1_group == reader_attachment_groups_.end())
							return sqlite_shm_unexpected(
								rejection(sqlite_shm_lease_rejection_reason::retiring,
										  sqlite_shm_lease_recovery_action::
											  await_complete_attachment_gate_boundary));
					}
					const auto phase1_waiting = phase1_group != reader_attachment_groups_.end();
					const auto phase1_blockers = phase1_waiting
						? reader_phase1_close_cut_blocker_decision_locked(
							  *phase1_group, *open, request.callback, false)
						: reader_unmap_cut_blocker_decision::none;
					if (phase1_waiting &&
						phase1_blockers != reader_unmap_cut_blocker_decision::other_thread &&
						phase1_blockers !=
							reader_unmap_cut_blocker_decision::same_thread_or_reentrant)
					{
						quarantine_reader_group_locked(
							*phase1_group,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						quarantine_reader_open_locked(
							*open,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						return sqlite_shm_unexpected(ambiguous());
					}
					auto awaiting_ack_group = reader_attachment_groups_.end();
					for (auto group = reader_attachment_groups_.begin();
						 group != reader_attachment_groups_.end();
						 ++group)
					{
						if (!group->registry_bound ||
							group->expected.registry_open_token() != open->token ||
							!reader_attachment_matches_open_epoch_binding(group->expected,
																		  open->binding) ||
							group->logical_ack_phase !=
								sqlite_shm_reader_logical_ack_phase::awaiting_sqlite_ack)
							continue;
						if (awaiting_ack_group != reader_attachment_groups_.end())
						{
							quarantine_reader_open_locked(
								*open,
								0U,
								sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
							return sqlite_shm_unexpected(ambiguous());
						}
						awaiting_ack_group = group;
					}
					const auto consumes_awaiting_ack =
						awaiting_ack_group != reader_attachment_groups_.end();
					const auto closes_active_predecessor = route &&
						*route == sqlite_shm_reader_close_route::close_existing_predecessor;
					if (consumes_awaiting_ack && closes_active_predecessor)
					{
						quarantine_reader_open_locked(
							*open,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						return sqlite_shm_unexpected(ambiguous());
					}
					if (consumes_awaiting_ack && completed_activity == nullptr)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::resubmit_via_bound_route));
					if (!reader_open_close_custody_census_is_exact_locked(
							*open,
							false,
							consumes_awaiting_ack,
							closes_active_predecessor,
							phase1_waiting ? &*phase1_group : nullptr))
					{
						quarantine_reader_open_locked(
							*open,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						return sqlite_shm_unexpected(ambiguous());
					}
					if (reader_callback_was_completed_locked(request.callback) ||
						!callback_can_start_locked(
							request.callback, 0U, phase1_waiting ? phase1_group->token : 0U) ||
						reader_effect_identity_seen_locked(
							request.callback.invocation_token, 0U, 0U, 0U) ||
						reader_session_terminal_identity_seen_locked(
							request.callback.invocation_token))
					{
						quarantine_reader_open_locked(
							*open,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::presented_invalid);
						return sqlite_shm_unexpected(ambiguous());
					}
					if (std::exchange(fail_next_reader_close_begin_preparation_for_testing_, false))
						throw reader_close_begin_preparation_injected_failure{};
					const auto wait_resolution_slot = phase1_waiting
						? reserve_reader_lifecycle_terminal_slots_locked(1U)
						: reader_terminal_slot_reservation_batch{};
					if (phase1_waiting &&
						(!wait_resolution_slot.succeeded ||
						 wait_resolution_slot.slots.front() == 0U ||
						 std::ranges::any_of(std::span{wait_resolution_slot.slots}.subspan(1U),
											 [](const std::uint64_t slot)
											 {
												 return slot != 0U;
											 })))
						return sqlite_shm_unexpected(
							rejection(wait_resolution_slot.failure == generation_failure::exhausted
										  ? sqlite_shm_lease_rejection_reason::generation_exhausted
										  : sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					reader_terminal_slot_guard wait_resolution_slot_guard{
						reader_lifecycle_sequences_ ? reader_lifecycle_sequences_->state_.get()
													: nullptr,
						phase1_waiting ? wait_resolution_slot.slots.front() : 0U};

					auto prepared_callback = request.callback;
					auto prepared_state = shared_from_this();
					auto next_custodies = reader_custodies_;
					const auto connection_close = std::find_if(
						next_custodies.begin(),
						next_custodies.end(),
						[&open](const reader_custody_record& custody)
						{
							return custody.kind ==
								sqlite_shm_reader_custody_kind::connection_close &&
								custody.owner_token == open->close_owner_token &&
								custody.state == sqlite_shm_reader_custody_state::live &&
								!custody.attachment && custody.open_epoch &&
								*custody.open_epoch == open->binding;
						});
					if (connection_close == next_custodies.end())
					{
						quarantine_reader_open_locked(
							*open,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto connection_close_index = static_cast<std::size_t>(
						std::distance(next_custodies.begin(), connection_close));
					std::optional<std::size_t> logical_ack_custody_index;
					std::optional<std::size_t> runtime_pin_custody_index;
					if (consumes_awaiting_ack)
					{
						for (std::size_t index = 0U; index < next_custodies.size(); ++index)
						{
							const auto& custody = next_custodies[index];
							if (custody.attachment != awaiting_ack_group->expected ||
								custody.owner_token != awaiting_ack_group->token ||
								custody.state != sqlite_shm_reader_custody_state::live)
								continue;
							if (custody.kind == sqlite_shm_reader_custody_kind::logical_ack &&
								!logical_ack_custody_index)
								logical_ack_custody_index = index;
							else if (
								custody.kind ==
									sqlite_shm_reader_custody_kind::
										runtime_vfs_namespace_generation_native_mapping_lifetime_pin &&
								!runtime_pin_custody_index)
								runtime_pin_custody_index = index;
							else
								return sqlite_shm_unexpected(ambiguous());
						}
						if (!logical_ack_custody_index || !runtime_pin_custody_index ||
							awaiting_ack_group->logical_ack_sequence_slot ==
								open->close_cut_sequence_slot)
							return sqlite_shm_unexpected(ambiguous());
					}
					next_custodies.emplace_back(
						sqlite_shm_reader_custody_kind::close_cut_or_composite,
						sqlite_shm_reader_custody_state::live,
						std::nullopt,
						open->close_owner_token,
						0U,
						0U,
						open->binding);
					if (phase1_waiting)
					{
						next_custodies.emplace_back(
							sqlite_shm_reader_custody_kind::bounded_waiter_or_continuation,
							sqlite_shm_reader_custody_state::live,
							phase1_group->expected,
							phase1_group->token,
							0U,
							0U);
						next_custodies.emplace_back(
							sqlite_shm_reader_custody_kind::terminal_reporter,
							sqlite_shm_reader_custody_state::live,
							phase1_group->expected,
							phase1_group->token,
							0U,
							0U);
					}
					if (open->initial_close_cut_sequence_slot == 0U ||
						open->initial_close_terminal_sequence_slot == 0U ||
						open->initial_close_cut_sequence_slot ==
							open->initial_close_terminal_sequence_slot ||
						open->close_cut_sequence_slot != open->initial_close_cut_sequence_slot ||
						open->close_terminal_sequence_slot !=
							open->initial_close_terminal_sequence_slot)
					{
						quarantine_reader_open_locked(
							*open,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto sequences = consumes_awaiting_ack
						? consume_reader_lifecycle_terminal_slots_locked(
							  open->close_cut_sequence_slot,
							  awaiting_ack_group->logical_ack_sequence_slot)
						: consume_reader_lifecycle_terminal_slot_locked(
							  open->close_cut_sequence_slot);
					if (!sequences.succeeded)
					{
						quarantine_reader_open_locked(
							*open,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						return sqlite_shm_unexpected(ambiguous());
					}
					open->close_cut_sequence_slot = 0U;
					if (consumes_awaiting_ack)
						awaiting_ack_group->logical_ack_sequence_slot = 0U;
					committed_close_cut_sequence = sequences.first;
					consumed_awaiting_ack_after_slot = consumes_awaiting_ack;
					if (std::ranges::any_of(
							reader_attachment_groups_,
							[&open, &sequences, phase1_waiting, &phase1_group](
								const reader_attachment_group_record& group)
							{
								if (!group.registry_bound)
									return false;
								const auto token_matches =
									group.expected.registry_open_token() == open->token;
								const auto binding_matches =
									reader_attachment_matches_open_epoch_binding(group.expected,
																				 open->binding);
								return (token_matches || binding_matches) &&
									(!token_matches || !binding_matches ||
									 (&group != (phase1_waiting ? &*phase1_group : nullptr) &&
									  group.reservation_destination_sequence >= sequences.first));
							}))
					{
						if (consumes_awaiting_ack)
							quarantine_reader_group_locked(
								*awaiting_ack_group,
								sequences.first,
								sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						quarantine_reader_open_locked(
							*open,
							sequences.first,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						return sqlite_shm_unexpected(ambiguous());
					}
					next_custodies[connection_close_index].state =
						sqlite_shm_reader_custody_state::transferred_to_exact_successor;
					next_custodies[connection_close_index].destination_sequence = sequences.first;
					const auto close_custody_count = phase1_waiting ? 3U : 1U;
					for (auto iterator = next_custodies.end() -
							 static_cast<std::ptrdiff_t>(close_custody_count);
						 iterator != next_custodies.end();
						 ++iterator)
						iterator->origin_sequence = sequences.first;
					if (consumes_awaiting_ack)
					{
						next_custodies[*logical_ack_custody_index].state =
							sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt;
						next_custodies[*logical_ack_custody_index].destination_sequence =
							sequences.first;
						next_custodies[*runtime_pin_custody_index].state =
							sqlite_shm_reader_custody_state::transferred_to_durable_tombstone;
						next_custodies[*runtime_pin_custody_index].destination_sequence =
							sequences.first;
						awaiting_ack_group->logical_ack_phase =
							sqlite_shm_reader_logical_ack_phase::consumed_by_close;
						awaiting_ack_group->logical_ack_sequence = sequences.first;
						static_assert(std::is_nothrow_move_constructible_v<
									  sqlite_shm_reader_attachment_authority>);
						completed_activity->emplace(
							std::move(*awaiting_ack_group->registry_activity_authority));
						awaiting_ack_group->registry_activity_authority.reset();
					}
					open->close_phase = sqlite_shm_reader_connection_close_phase::close_admitted;
					open->close_route = route;
					open->close_cut_sequence = sequences.first;
					static_assert(std::is_nothrow_move_constructible_v<
								  sqlite_shm_callback_execution_receipt>);
					open->close_callback.emplace(std::move(prepared_callback));
					if (phase1_waiting)
					{
						phase1_group->composite_close_owner_token = open->close_owner_token;
						phase1_group->composite_close_registry_open_token = open->token;
						phase1_group->composite_close_cut_sequence = sequences.first;
						phase1_group->composite_close_wait_resolution_sequence_slot =
							wait_resolution_slot.slots.front();
						phase1_group->composite_close_wait_resolution_sequence = 0U;
					}
					reader_custodies_.swap(next_custodies);
					wait_resolution_slot_guard.release();
					reader_last_committed_sequence_ =
						std::max(reader_last_committed_sequence_, sequences.last);
					if (phase1_waiting &&
						phase1_blockers ==
							reader_unmap_cut_blocker_decision::same_thread_or_reentrant)
					{
						quarantine_reader_group_locked(
							*phase1_group,
							sequences.first,
							sqlite_shm_reader_terminal_quarantine_reason::presented_invalid);
						quarantine_reader_open_locked(
							*open,
							sequences.first,
							sqlite_shm_reader_terminal_quarantine_reason::presented_invalid);
						return sqlite_shm_unexpected(ambiguous());
					}
					return sqlite_shm_reader_close_obligation{
						std::move(prepared_state),
						sqlite_shm_lease_token_identity{open->close_owner_token},
						open->token,
						route,
						phase1_waiting
							? sqlite_shm_reader_close_obligation::phase::reservation_waiting
							: sqlite_shm_reader_close_obligation::phase::close_ready,
						phase1_waiting ? phase1_group->token : 0U,
						phase1_waiting ? phase1_group->generation : 0U,
						phase1_waiting ? wait_resolution_slot.slots.front() : 0U};
				}
				catch (...)
				{
					try
					{
						if (fail_next_reader_recovery_mutex_reacquire_for_testing_.exchange(
								false, std::memory_order_acq_rel))
							throw reader_recovery_mutex_reacquire_injected_failure{};
						std::scoped_lock lock{mutex_};
						const auto open = std::find_if(
							registry_reader_opens_.begin(),
							registry_reader_opens_.end(),
							[registry_open_token](const registry_reader_open_record& candidate)
							{
								return candidate.token == registry_open_token;
							});
						if (consumed_awaiting_ack_after_slot && committed_close_cut_sequence != 0U)
							for (auto& group : reader_attachment_groups_)
							{
								if (!group.registry_bound ||
									group.expected.registry_open_token() != registry_open_token ||
									!reader_attachment_matches_open_epoch_binding(group.expected,
																				  binding))
									continue;
								quarantine_reader_group_locked(
									group,
									committed_close_cut_sequence,
									sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
							}
						if (open != registry_reader_opens_.end() &&
							open->seal.get() == seal.get() && open->binding == binding)
							quarantine_reader_open_locked(
								*open,
								committed_close_cut_sequence,
								sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
					}
					catch (...)
					{
						emergency_quarantine_.store(true, std::memory_order_release);
					}
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_close_cut_result>
			poll_registry_reader_close_cut(
				const std::uint64_t registry_open_token,
				const std::shared_ptr<sqlite_shm_reader_open_lineage_seal>& seal,
				const sqlite_shm_reader_open_epoch_binding& binding,
				sqlite_shm_reader_close_obligation& close,
				const sqlite_shm_callback_execution_receipt& close_callback,
				std::optional<sqlite_shm_reader_attachment_authority>* completed_activity) noexcept
			{
				if (registry_open_token == 0U || !seal ||
					!valid_reader_open_epoch_binding(binding) || !valid_callback(close_callback) ||
					completed_activity == nullptr || completed_activity->has_value())
					return sqlite_shm_unexpected(
						rejection(sqlite_shm_lease_rejection_reason::invalid_request,
								  sqlite_shm_lease_recovery_action::quarantine_no_retry));
				try
				{
					if (fail_next_reader_operation_mutex_acquire_for_testing_.exchange(
							false, std::memory_order_acq_rel))
						throw reader_operation_mutex_acquire_injected_failure{};
					std::scoped_lock lock{mutex_};
					if (!owns(close.state_, close.owner_token_))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					const auto open = std::find_if(
						registry_reader_opens_.begin(),
						registry_reader_opens_.end(),
						[registry_open_token](const registry_reader_open_record& candidate)
						{
							return candidate.token == registry_open_token;
						});
					const auto group =
						find_by_token(reader_attachment_groups_, close.reservation_token_);
					const auto exact_owner = open != registry_reader_opens_.end() &&
						group != reader_attachment_groups_.end() &&
						open->seal.get() == seal.get() && open->binding == binding &&
						binding.family == family_ && close.registry_open_token_ == open->token &&
						close.owner_token_ == open->close_owner_token && !close.route_ &&
						close.phase_ ==
							sqlite_shm_reader_close_obligation::phase::reservation_waiting &&
						close.generation_ == group->generation &&
						close.wait_resolution_sequence_slot_ != 0U &&
						close.wait_resolution_sequence_slot_ ==
							group->composite_close_wait_resolution_sequence_slot &&
						open->close_phase ==
							sqlite_shm_reader_connection_close_phase::close_admitted &&
						!open->close_route && open->close_callback &&
						*open->close_callback == close_callback &&
						reader_phase1_close_cut_is_exact_locked(*group, *open) &&
						reader_open_close_custody_census_is_exact_locked(
												 *open, true, false, false, &*group);
					if (!exact_owner)
					{
						if (group != reader_attachment_groups_.end())
							quarantine_reader_group_locked(
								*group,
								0U,
								sqlite_shm_reader_terminal_quarantine_reason::presented_invalid);
						else if (open != registry_reader_opens_.end())
							quarantine_reader_open_locked(
								*open,
								0U,
								sqlite_shm_reader_terminal_quarantine_reason::presented_invalid);
						close.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto blockers = reader_phase1_close_cut_blocker_decision_locked(
						*group, *open, close_callback, true);
					if (blockers == reader_unmap_cut_blocker_decision::other_thread)
						return sqlite_shm_reader_close_cut_result{
							sqlite_shm_reader_close_cut_progress::waiting, std::nullopt};
					if (blockers != reader_unmap_cut_blocker_decision::none)
					{
						quarantine_reader_group_locked(
							*group,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						close.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}

					std::optional<sqlite_shm_reader_close_route> route;
					const auto consumes_ack = group->logical_ack_phase ==
						sqlite_shm_reader_logical_ack_phase::awaiting_sqlite_ack;
					if (group->reservation_phase ==
							sqlite_shm_reader_attachment_reservation_phase::revoked_no_map &&
						reader_phase1_revoked_group_shape_is_exact_locked(*group, *open))
						route = sqlite_shm_reader_close_route::close_without_group;
					else if (group->reservation_phase ==
								 sqlite_shm_reader_attachment_reservation_phase::
									 predecessor_route_active &&
							 reader_local_predecessor_group_shape_is_exact_locked(*group, false))
						route = sqlite_shm_reader_close_route::close_existing_predecessor;
					else if (consumes_ack &&
							 reader_local_awaiting_ack_group_shape_is_exact_locked(*group))
						route = sqlite_shm_reader_close_route::close_after_confirmed_unmap;
					if (!route)
					{
						quarantine_reader_group_locked(
							*group,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						close.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}

					auto next_custodies = reader_custodies_;
					std::optional<std::size_t> waiter_index;
					std::optional<std::size_t> reporter_index;
					std::optional<std::size_t> ack_index;
					std::optional<std::size_t> runtime_pin_index;
					for (std::size_t index = 0U; index < next_custodies.size(); ++index)
					{
						const auto& custody = next_custodies[index];
						if (custody.attachment != group->expected ||
							custody.owner_token != group->token ||
							custody.state != sqlite_shm_reader_custody_state::live)
							continue;
						if (custody.kind ==
								sqlite_shm_reader_custody_kind::bounded_waiter_or_continuation &&
							!waiter_index)
							waiter_index = index;
						else if (custody.kind ==
									 sqlite_shm_reader_custody_kind::terminal_reporter &&
								 !reporter_index)
							reporter_index = index;
						else if (consumes_ack &&
								 custody.kind == sqlite_shm_reader_custody_kind::logical_ack &&
								 !ack_index)
							ack_index = index;
						else if (
							consumes_ack &&
							custody.kind ==
								sqlite_shm_reader_custody_kind::
									runtime_vfs_namespace_generation_native_mapping_lifetime_pin &&
							!runtime_pin_index)
							runtime_pin_index = index;
					}
					if (!waiter_index || !reporter_index ||
						(consumes_ack &&
						 (!ack_index || !runtime_pin_index || !group->registry_activity_authority)))
					{
						quarantine_reader_group_locked(
							*group,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						close.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					const std::array slots{
						consumes_ack ? group->logical_ack_sequence_slot
									 : group->composite_close_wait_resolution_sequence_slot,
						group->composite_close_wait_resolution_sequence_slot};
					const auto sequences = consumes_ack
						? consume_reader_lifecycle_terminal_slots_locked(slots)
						: consume_reader_lifecycle_terminal_slot_locked(slots.front());
					if (!sequences.succeeded)
					{
						quarantine_reader_group_locked(
							*group,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						close.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto resolution_sequence = sequences.last;
					next_custodies[*waiter_index].state =
						sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt;
					next_custodies[*waiter_index].destination_sequence = resolution_sequence;
					next_custodies[*reporter_index].state =
						sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt;
					next_custodies[*reporter_index].destination_sequence = resolution_sequence;
					if (consumes_ack)
					{
						group->logical_ack_sequence_slot = 0U;
						group->logical_ack_sequence = sequences.first;
						group->logical_ack_phase =
							sqlite_shm_reader_logical_ack_phase::consumed_by_close;
						next_custodies[*ack_index].state =
							sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt;
						next_custodies[*ack_index].destination_sequence = sequences.first;
						next_custodies[*runtime_pin_index].state =
							sqlite_shm_reader_custody_state::transferred_to_durable_tombstone;
						next_custodies[*runtime_pin_index].destination_sequence = sequences.first;
						static_assert(std::is_nothrow_move_constructible_v<
									  sqlite_shm_reader_attachment_authority>);
						completed_activity->emplace(std::move(*group->registry_activity_authority));
						group->registry_activity_authority.reset();
					}
					group->composite_close_wait_resolution_sequence_slot = 0U;
					group->composite_close_wait_resolution_sequence = resolution_sequence;
					open->close_route = route;
					close.route_ = route;
					close.phase_ = sqlite_shm_reader_close_obligation::phase::close_ready;
					close.reservation_token_ = 0U;
					close.generation_ = 0U;
					close.wait_resolution_sequence_slot_ = 0U;
					reader_custodies_.swap(next_custodies);
					reader_last_committed_sequence_ =
						std::max(reader_last_committed_sequence_, sequences.last);
					return sqlite_shm_reader_close_cut_result{
						sqlite_shm_reader_close_cut_progress::native_effect_ready, route};
				}
				catch (...)
				{
					quarantine_reader_close_terminal(close);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<void> fail_registry_reader_close_cut_wait(
				const std::uint64_t registry_open_token,
				const std::shared_ptr<sqlite_shm_reader_open_lineage_seal>& seal,
				const sqlite_shm_reader_open_epoch_binding& binding,
				sqlite_shm_reader_close_obligation& close,
				const sqlite_shm_callback_execution_receipt& close_callback,
				const sqlite_shm_retirement_wait_failure failure) noexcept
			{
				if (registry_open_token == 0U || !seal ||
					!valid_reader_open_epoch_binding(binding) || !valid_callback(close_callback) ||
					(failure != sqlite_shm_retirement_wait_failure::timeout &&
					 failure != sqlite_shm_retirement_wait_failure::unknown))
					return sqlite_shm_unexpected(
						rejection(sqlite_shm_lease_rejection_reason::invalid_request,
								  sqlite_shm_lease_recovery_action::quarantine_no_retry));
				try
				{
					std::scoped_lock lock{mutex_};
					if (!owns(close.state_, close.owner_token_))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					const auto open = std::find_if(
						registry_reader_opens_.begin(),
						registry_reader_opens_.end(),
						[registry_open_token](const registry_reader_open_record& candidate)
						{
							return candidate.token == registry_open_token;
						});
					const auto group =
						find_by_token(reader_attachment_groups_, close.reservation_token_);
					const auto exact_wait = open != registry_reader_opens_.end() &&
						group != reader_attachment_groups_.end() &&
						open->seal.get() == seal.get() && open->binding == binding &&
						binding.family == family_ && close.registry_open_token_ == open->token &&
						close.owner_token_ == open->close_owner_token && !close.route_ &&
						close.phase_ ==
							sqlite_shm_reader_close_obligation::phase::reservation_waiting &&
						close.generation_ == group->generation &&
						close.wait_resolution_sequence_slot_ != 0U &&
						close.wait_resolution_sequence_slot_ ==
							group->composite_close_wait_resolution_sequence_slot &&
						open->close_callback && *open->close_callback == close_callback &&
						reader_phase1_close_cut_is_exact_locked(*group, *open) &&
						reader_open_close_custody_census_is_exact_locked(
												*open, true, false, false, &*group);
					if (!exact_wait)
					{
						if (group != reader_attachment_groups_.end())
							quarantine_reader_group_locked(
								*group,
								0U,
								sqlite_shm_reader_terminal_quarantine_reason::presented_invalid);
						else if (open != registry_reader_opens_.end())
							quarantine_reader_open_locked(
								*open,
								0U,
								sqlite_shm_reader_terminal_quarantine_reason::presented_invalid);
						close.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					quarantine_reader_group_locked(
						*group,
						0U,
						sqlite_shm_reader_terminal_quarantine_reason::native_non_ok_or_unknown);
					close.disarm();
					return sqlite_shm_unexpected(ambiguous());
				}
				catch (...)
				{
					quarantine_reader_close_terminal(close);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_close_terminal_result>
			complete_registry_reader_close(
				const std::uint64_t registry_open_token,
				const std::shared_ptr<sqlite_shm_reader_open_lineage_seal>& seal,
				const sqlite_shm_reader_open_epoch_binding& binding,
				sqlite_shm_reader_close_obligation& close,
				const sqlite_shm_verified_reader_close_terminal_receipt& receipt,
				std::optional<sqlite_shm_reader_attachment_authority>* completed_activity =
					nullptr) noexcept
			{
				if (registry_open_token == 0U || !seal ||
					!valid_reader_open_epoch_binding(binding) ||
					(completed_activity && completed_activity->has_value()))
				{
					if (owns(close.state_, close.owner_token_))
					{
						quarantine_reader_close_terminal(close);
						return sqlite_shm_unexpected(ambiguous());
					}
					return sqlite_shm_unexpected(
						rejection(sqlite_shm_lease_rejection_reason::invalid_request,
								  sqlite_shm_lease_recovery_action::quarantine_no_retry));
				}
				if (emergency_quarantine_.load(std::memory_order_acquire) ||
					close.terminal_presentation_disabled_)
				{
					if (owns(close.state_, close.owner_token_))
						close.disable_terminal_presentation();
					return sqlite_shm_unexpected(ambiguous());
				}
				try
				{
					if (fail_next_reader_operation_mutex_acquire_for_testing_.exchange(
							false, std::memory_order_acq_rel))
						throw reader_operation_mutex_acquire_injected_failure{};
					std::scoped_lock lock{mutex_};
					if (!owns(close.state_, close.owner_token_))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					const auto quarantine_exact_owned_presentation = [&]() noexcept
					{
						const auto owned_open =
							std::find_if(registry_reader_opens_.begin(),
										 registry_reader_opens_.end(),
										 [&close](const registry_reader_open_record& candidate)
										 {
											 return candidate.token == close.registry_open_token_ &&
												 candidate.close_owner_token == close.owner_token_;
										 });
						if (owned_open != registry_reader_opens_.end())
						{
							quarantine_reader_open_locked(
								*owned_open,
								0U,
								sqlite_shm_reader_terminal_quarantine_reason::presented_invalid);
							close.disarm();
						}
						else
						{
							close.disable_terminal_presentation();
							quarantine_locked();
						}
					};
					const auto open = std::find_if(
						registry_reader_opens_.begin(),
						registry_reader_opens_.end(),
						[registry_open_token](const registry_reader_open_record& candidate)
						{
							return candidate.token == registry_open_token;
						});
					if (open == registry_reader_opens_.end())
					{
						quarantine_exact_owned_presentation();
						return sqlite_shm_unexpected(ambiguous());
					}
					if (open->seal.get() != seal.get() || open->binding != binding ||
						binding.family != family_ ||
						close.registry_open_token_ != registry_open_token ||
						close.owner_token_ != open->close_owner_token || !open->close_route ||
						close.route_ != *open->close_route ||
						close.phase_ != sqlite_shm_reader_close_obligation::phase::close_ready ||
						close.reservation_token_ != 0U || close.generation_ != 0U ||
						close.wait_resolution_sequence_slot_ != 0U)
					{
						quarantine_exact_owned_presentation();
						return sqlite_shm_unexpected(ambiguous());
					}
					auto active_predecessor = reader_attachment_groups_.end();
					if (*open->close_route ==
						sqlite_shm_reader_close_route::close_existing_predecessor)
					{
						for (auto group = reader_attachment_groups_.begin();
							 group != reader_attachment_groups_.end();
							 ++group)
						{
							if (!group->registry_bound ||
								group->expected.registry_open_token() != open->token ||
								!reader_attachment_matches_open_epoch_binding(group->expected,
																			  open->binding) ||
								group->reservation_phase !=
									sqlite_shm_reader_attachment_reservation_phase::
										predecessor_route_active)
								continue;
							if (active_predecessor != reader_attachment_groups_.end())
							{
								quarantine_reader_group_locked(
									*group,
									0U,
									sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
								quarantine_reader_group_locked(
									*active_predecessor,
									0U,
									sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
								quarantine_reader_open_locked(
									*open,
									0U,
									sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
								close.disarm();
								return sqlite_shm_unexpected(ambiguous());
							}
							active_predecessor = group;
						}
						if (completed_activity == nullptr ||
							active_predecessor == reader_attachment_groups_.end() ||
							!reader_local_predecessor_group_shape_is_exact_locked(
								*active_predecessor, false))
						{
							if (active_predecessor != reader_attachment_groups_.end())
								quarantine_reader_group_locked(
									*active_predecessor,
									0U,
									sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
							quarantine_reader_open_locked(
								*open,
								0U,
								sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
							close.disarm();
							return sqlite_shm_unexpected(ambiguous());
						}
					}
					auto composite_group = reader_attachment_groups_.end();
					for (auto group = reader_attachment_groups_.begin();
						 group != reader_attachment_groups_.end();
						 ++group)
					{
						if (group->composite_close_owner_token != open->close_owner_token &&
							group->composite_close_registry_open_token != open->token)
							continue;
						const auto exact_phase1_resolution =
							reader_phase1_close_resolution_is_exact_locked(*group, *open);
						const auto exact_phase1_route = exact_phase1_resolution &&
							((*open->close_route ==
								  sqlite_shm_reader_close_route::close_without_group &&
							  group->reservation_phase ==
								  sqlite_shm_reader_attachment_reservation_phase::revoked_no_map &&
							  reader_local_phase1_compact_group_shape_is_exact_locked(*group)) ||
							 (*open->close_route ==
								  sqlite_shm_reader_close_route::close_after_confirmed_unmap &&
							  group->reservation_phase ==
								  sqlite_shm_reader_attachment_reservation_phase::
									  unpublished_cleanup_confirmed &&
							  reader_local_phase1_compact_group_shape_is_exact_locked(*group)) ||
							 (*open->close_route ==
								  sqlite_shm_reader_close_route::close_existing_predecessor &&
							  group == active_predecessor &&
							  reader_local_predecessor_group_shape_is_exact_locked(*group, false)));
						const auto exact_live_route = group->reservation_phase ==
								sqlite_shm_reader_attachment_reservation_phase::retired_confirmed &&
							group->phase ==
								reader_attachment_group_phase::native_cleanup_confirmed &&
							reader_local_phase1_compact_group_shape_is_exact_locked(*group) &&
							*open->close_route ==
								sqlite_shm_reader_close_route::close_after_confirmed_unmap;
						if (composite_group != reader_attachment_groups_.end() ||
							group->composite_close_owner_token != open->close_owner_token ||
							group->composite_close_registry_open_token != open->token ||
							group->composite_close_cut_sequence != open->close_cut_sequence ||
							group->expected.registry_open_token() != open->token ||
							!reader_attachment_matches_open_epoch_binding(group->expected,
																		  open->binding) ||
							(!exact_phase1_route && !exact_live_route))
						{
							quarantine_reader_group_locked(
								*group,
								0U,
								sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
							quarantine_reader_open_locked(
								*open,
								0U,
								sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
							close.disarm();
							return sqlite_shm_unexpected(ambiguous());
						}
						composite_group = group;
					}
					const auto phase1_predecessor_resolution =
						active_predecessor != reader_attachment_groups_.end() &&
						composite_group == active_predecessor &&
						reader_phase1_close_resolution_is_exact_locked(*active_predecessor, *open);

					auto prepared_receipt = receipt;
					auto terminal_receipt = receipt;
					auto result_effect = receipt.native_effect_receipt();
					const auto result_evidence_kind = receipt.evidence_kind();
					const auto result_native_status = receipt.native_status();
					const auto receipt_state = prepared_receipt.state_.lock();
					const auto replayed_effect = prepared_receipt.native_effect_receipt() &&
						(reader_close_effect_identity_seen_locked(
							 *prepared_receipt.native_effect_receipt(), open->close_owner_token) ||
						 reader_callback_invocation_was_seen_locked(
							 *prepared_receipt.native_effect_receipt()) ||
						 reader_session_terminal_identity_seen_locked(
							 *prepared_receipt.native_effect_receipt()));
					if (!receipt_state || receipt_state.get() != this ||
						prepared_receipt.owner_token_ != close.owner_token_ ||
						prepared_receipt.registry_open_token_ != registry_open_token ||
						!valid_reader_close_terminal_receipt(prepared_receipt) ||
						open->close_phase !=
							sqlite_shm_reader_connection_close_phase::close_admitted ||
						open->initial_close_cut_sequence_slot == 0U ||
						open->initial_close_terminal_sequence_slot == 0U ||
						open->initial_close_cut_sequence_slot ==
							open->initial_close_terminal_sequence_slot ||
						open->close_cut_sequence_slot != 0U ||
						open->close_terminal_sequence_slot !=
							open->initial_close_terminal_sequence_slot ||
						!open->close_callback ||
						*open->close_callback != prepared_receipt.callback() || replayed_effect ||
						(prepared_receipt.native_effect_receipt() &&
						 *prepared_receipt.native_effect_receipt() ==
							 prepared_receipt.callback().invocation_token) ||
						!reader_open_close_custody_census_is_exact_locked(
							*open,
							true,
							false,
							active_predecessor != reader_attachment_groups_.end()))
					{
						quarantine_reader_open_locked(
							*open,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::presented_invalid);
						if (active_predecessor != reader_attachment_groups_.end())
							quarantine_reader_group_locked(
								*active_predecessor,
								open->close_terminal_sequence,
								sqlite_shm_reader_terminal_quarantine_reason::presented_invalid);
						close.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					if (reader_phase1_close_route_locked(*open) != open->close_route)
					{
						quarantine_reader_open_locked(
							*open,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						if (active_predecessor != reader_attachment_groups_.end())
							quarantine_reader_group_locked(
								*active_predecessor,
								open->close_terminal_sequence,
								sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						close.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					static_assert(std::is_nothrow_move_constructible_v<
								  sqlite_shm_verified_reader_close_terminal_receipt>);
					open->close_terminal_receipt.emplace(std::move(prepared_receipt));
					if (std::exchange(fail_next_reader_close_post_receipt_state_for_testing_,
									  false))
						throw reader_close_post_receipt_state_injected_failure{};

					auto next_custodies = reader_custodies_;
					const auto close_cut = std::find_if(
						next_custodies.begin(),
						next_custodies.end(),
						[&open](const reader_custody_record& custody)
						{
							return custody.kind ==
								sqlite_shm_reader_custody_kind::close_cut_or_composite &&
								custody.owner_token == open->close_owner_token &&
								custody.state == sqlite_shm_reader_custody_state::live &&
								!custody.attachment && custody.open_epoch &&
								*custody.open_epoch == open->binding &&
								custody.origin_sequence == open->close_cut_sequence;
						});
					if (close_cut == next_custodies.end() ||
						open->close_terminal_sequence_slot == 0U)
						throw reader_close_post_receipt_state_injected_failure{};
					const auto close_cut_index =
						static_cast<std::size_t>(std::distance(next_custodies.begin(), close_cut));
					std::optional<std::size_t> predecessor_runtime_pin_index;
					if (active_predecessor != reader_attachment_groups_.end())
					{
						for (std::size_t index = 0U; index < next_custodies.size(); ++index)
						{
							const auto& custody = next_custodies[index];
							if (custody.attachment != active_predecessor->expected ||
								custody.owner_token != active_predecessor->token ||
								custody.state != sqlite_shm_reader_custody_state::live)
								continue;
							if (custody.kind !=
									sqlite_shm_reader_custody_kind::
										runtime_vfs_namespace_generation_native_mapping_lifetime_pin ||
								predecessor_runtime_pin_index)
								throw reader_close_post_receipt_state_injected_failure{};
							predecessor_runtime_pin_index = index;
						}
						if (!predecessor_runtime_pin_index ||
							!active_predecessor->registry_activity_authority ||
							!active_predecessor->registry_activity_authority
								 ->retains_exact_owned_drain_lifetimes(
									 active_predecessor->expected))
							throw reader_close_post_receipt_state_injected_failure{};
					}
					const auto exact_ok = terminal_receipt.evidence_kind() ==
							sqlite_shm_reader_close_evidence_kind::exact_native_result &&
						terminal_receipt.native_status() &&
						*terminal_receipt.native_status() ==
							static_cast<int>(sqlite_native_map_status::ok);
					std::list<reader_close_terminal_record> prepared_terminals;
					prepared_terminals.push_back(
						{open->token,
						 open->close_owner_token,
						 open->binding,
						 *open->close_route,
						 exact_ok ? sqlite_shm_reader_close_terminal_kind::closed
								  : sqlite_shm_reader_close_terminal_kind::terminal_quarantined,
						 std::move(terminal_receipt),
						 exact_ok ? static_cast<int>(sqlite_native_map_status::ok)
							 : result_evidence_kind ==
									 sqlite_shm_reader_close_evidence_kind::exact_native_result &&
								 result_native_status &&
								 *result_native_status !=
									 static_cast<int>(sqlite_native_map_status::ok)
							 ? *result_native_status
							 : sqlite_ioerr_status,
						 exact_ok ? sqlite_shm_reader_terminal_quarantine_reason::none
								  : sqlite_shm_reader_terminal_quarantine_reason::
										native_non_ok_or_unknown,
						 open->close_origin_sequence,
						 open->close_cut_sequence,
						 0U});
					const auto sequences = consume_reader_lifecycle_terminal_slot_locked(
						open->close_terminal_sequence_slot);
					if (!sequences.succeeded)
						throw reader_close_post_receipt_state_injected_failure{};
					open->close_terminal_sequence_slot = 0U;
					open->close_terminal_sequence = sequences.first;
					prepared_terminals.back().terminal_sequence = sequences.first;
					const auto injected_failure =
						std::exchange(fail_next_reader_close_terminal_commit_for_testing_, false);
					const auto terminal_kind = exact_ok && !injected_failure
						? sqlite_shm_reader_close_terminal_kind::closed
						: sqlite_shm_reader_close_terminal_kind::terminal_quarantined;
					const auto custody_terminal =
						terminal_kind == sqlite_shm_reader_close_terminal_kind::closed
						? sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt
						: sqlite_shm_reader_custody_state::transferred_to_durable_tombstone;
					next_custodies[close_cut_index].state = custody_terminal;
					next_custodies[close_cut_index].destination_sequence = sequences.first;
					if (active_predecessor != reader_attachment_groups_.end())
					{
						const auto predecessor_custody_terminal =
							terminal_kind == sqlite_shm_reader_close_terminal_kind::closed &&
								!phase1_predecessor_resolution
							? sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt
							: sqlite_shm_reader_custody_state::transferred_to_durable_tombstone;
						next_custodies[*predecessor_runtime_pin_index].state =
							predecessor_custody_terminal;
						next_custodies[*predecessor_runtime_pin_index].destination_sequence =
							sequences.first;
						active_predecessor->predecessor_close_terminal_sequence = sequences.first;
						active_predecessor->reservation_destination_sequence = sequences.first;
						active_predecessor->reservation_phase =
							terminal_kind == sqlite_shm_reader_close_terminal_kind::closed
							? sqlite_shm_reader_attachment_reservation_phase::
								  predecessor_route_retired_confirmed
							: sqlite_shm_reader_attachment_reservation_phase::terminal_quarantined;
						active_predecessor->phase =
							terminal_kind == sqlite_shm_reader_close_terminal_kind::closed
							? reader_attachment_group_phase::native_cleanup_confirmed
							: reader_attachment_group_phase::terminal_quarantined;
						active_predecessor->quarantine_reason =
							terminal_kind == sqlite_shm_reader_close_terminal_kind::closed
							? sqlite_shm_reader_terminal_quarantine_reason::none
							: injected_failure
							? sqlite_shm_reader_terminal_quarantine_reason::injected_commit_failure
							: sqlite_shm_reader_terminal_quarantine_reason::
								  native_non_ok_or_unknown;
						if (terminal_kind == sqlite_shm_reader_close_terminal_kind::closed)
						{
							static_assert(std::is_nothrow_move_constructible_v<
										  sqlite_shm_reader_attachment_authority>);
							completed_activity->emplace(
								std::move(*active_predecessor->registry_activity_authority));
							active_predecessor->registry_activity_authority.reset();
						}
					}
					open->close_phase =
						terminal_kind == sqlite_shm_reader_close_terminal_kind::closed
						? sqlite_shm_reader_connection_close_phase::closed
						: sqlite_shm_reader_connection_close_phase::terminal_quarantined;
					open->close_terminal_sequence = sequences.first;
					open->quarantine_reason =
						terminal_kind == sqlite_shm_reader_close_terminal_kind::closed
						? sqlite_shm_reader_terminal_quarantine_reason::none
						: injected_failure
						? sqlite_shm_reader_terminal_quarantine_reason::injected_commit_failure
						: sqlite_shm_reader_terminal_quarantine_reason::native_non_ok_or_unknown;
					if (injected_failure)
					{
						prepared_terminals.back().kind =
							sqlite_shm_reader_close_terminal_kind::terminal_quarantined;
						prepared_terminals.back().outward_status = sqlite_ioerr_status;
						prepared_terminals.back().quarantine_reason =
							sqlite_shm_reader_terminal_quarantine_reason::injected_commit_failure;
					}
					reader_close_terminals_.splice(reader_close_terminals_.end(),
												   prepared_terminals);
					reader_custodies_.swap(next_custodies);
					close.disarm();
					reader_last_committed_sequence_ =
						std::max(reader_last_committed_sequence_, sequences.last);
					if (terminal_kind ==
						sqlite_shm_reader_close_terminal_kind::terminal_quarantined)
						quarantine_locked();
					if (injected_failure)
						return sqlite_shm_unexpected(ambiguous());
					return sqlite_shm_reader_close_terminal_result{terminal_kind,
																   *open->close_route,
																   result_evidence_kind,
																   result_native_status,
																   std::move(result_effect)};
				}
				catch (...)
				{
					quarantine_reader_close_terminal(close);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<void> release_registry_reader_open(
				const std::uint64_t registry_open_token,
				const std::shared_ptr<sqlite_shm_reader_open_lineage_seal>& seal) noexcept
			{
				if (registry_open_token == 0U || !seal)
					return sqlite_shm_unexpected(
						rejection(sqlite_shm_lease_rejection_reason::invalid_request,
								  sqlite_shm_lease_recovery_action::deny_before_native_map));
				try
				{
					std::scoped_lock lock{mutex_};
					const auto found =
						std::find_if(registry_reader_opens_.begin(),
									 registry_reader_opens_.end(),
									 [registry_open_token](const registry_reader_open_record& open)
									 {
										 return open.token == registry_open_token;
									 });
					if (found == registry_reader_opens_.end())
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::deny_before_native_map));
					if (found->seal.get() != seal.get() ||
						!seal->authority_valid.load(std::memory_order_acquire))
					{
						registry_member_sticky_quarantine_ = true;
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::quarantined,
									  sqlite_shm_lease_recovery_action::quarantine_no_retry));
					}
					if (found->close_phase != sqlite_shm_reader_connection_close_phase::closed ||
						!found->close_route || !found->close_terminal_receipt ||
						found->close_terminal_sequence == 0U || found->close_cut_sequence == 0U ||
						found->close_cut_sequence_slot != 0U ||
						found->close_terminal_sequence_slot != 0U)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::retiring,
									  sqlite_shm_lease_recovery_action::
										  await_complete_attachment_gate_boundary));
					const auto attachment_relates_open =
						[&found](
							const sqlite_shm_reader_attachment_reservation_identity& attachment)
					{
						return attachment.registry_open_token() == found->token ||
							reader_attachment_matches_open_epoch_binding(attachment,
																		 found->binding);
					};
					const auto attachment_exactly_retains_open =
						[&found](
							const sqlite_shm_reader_attachment_reservation_identity& attachment)
					{
						return attachment.registry_open_token() == found->token &&
							reader_attachment_matches_open_epoch_binding(attachment,
																		 found->binding);
					};
					if (std::ranges::any_of(
							reader_sessions_,
							[&attachment_relates_open](const reader_session_record& session)
							{
								return session.registry_bound &&
									attachment_relates_open(session.request.attachment);
							}) ||
						std::ranges::any_of(
							reader_attachment_groups_,
							[this,
							 &found,
							 &attachment_relates_open,
							 &attachment_exactly_retains_open](
								const reader_attachment_group_record& group)
							{
								if (!group.registry_bound ||
									!attachment_relates_open(group.expected))
									return false;
								const auto predecessor_retired_by_close = group.reservation_phase ==
										sqlite_shm_reader_attachment_reservation_phase::
											predecessor_route_retired_confirmed &&
									group.predecessor_close_terminal_sequence ==
										found->close_terminal_sequence &&
									group.reservation_destination_sequence ==
										found->close_terminal_sequence;
								const auto retired_by_composite_close = group.reservation_phase ==
										sqlite_shm_reader_attachment_reservation_phase::
											retired_confirmed &&
									group.phase ==
										reader_attachment_group_phase::native_cleanup_confirmed &&
									group.composite_close_owner_token == found->close_owner_token &&
									group.composite_close_registry_open_token == found->token &&
									group.composite_close_cut_sequence ==
										found->close_cut_sequence &&
									group.unmap_terminal_sequence > found->close_cut_sequence &&
									group.unmap_terminal_sequence <
										found->close_terminal_sequence &&
									group.reservation_destination_sequence ==
										group.unmap_terminal_sequence;
								const auto resolved_phase1_close =
									reader_phase1_close_resolution_is_exact_locked(group, *found) &&
									group.composite_close_wait_resolution_sequence <
										found->close_terminal_sequence &&
									((*found->close_route ==
										  sqlite_shm_reader_close_route::close_without_group &&
									  group.reservation_phase ==
										  sqlite_shm_reader_attachment_reservation_phase::
											  revoked_no_map) ||
									 (*found->close_route ==
										  sqlite_shm_reader_close_route::
											  close_after_confirmed_unmap &&
									  group.reservation_phase ==
										  sqlite_shm_reader_attachment_reservation_phase::
											  unpublished_cleanup_confirmed) ||
									 (*found->close_route ==
										  sqlite_shm_reader_close_route::
											  close_existing_predecessor &&
									  predecessor_retired_by_close));
								const auto exact_close_order = predecessor_retired_by_close ||
									retired_by_composite_close || resolved_phase1_close ||
									group.reservation_destination_sequence <
										found->close_cut_sequence;
								return !attachment_exactly_retains_open(group.expected) ||
									!reader_local_phase1_compact_group_shape_is_exact_locked(
										group) ||
									!exact_close_order;
							}) ||
						std::ranges::any_of(
							reader_attachment_maps_,
							[&attachment_relates_open](const reader_attachment_map_record& map)
							{
								return map.registry_bound &&
									attachment_relates_open(map.request.expected_attachment);
							}))
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::retiring,
									  sqlite_shm_lease_recovery_action::
										  await_complete_attachment_gate_boundary));
					const auto exact_terminal =
						[&found](const reader_close_terminal_record& candidate)
					{
						return candidate.registry_open_token == found->token &&
							candidate.close_owner_token == found->close_owner_token &&
							candidate.binding == found->binding &&
							candidate.route == *found->close_route &&
							candidate.kind == sqlite_shm_reader_close_terminal_kind::closed &&
							candidate.receipt.callback() ==
							found->close_terminal_receipt->callback() &&
							candidate.receipt.evidence_kind() ==
							found->close_terminal_receipt->evidence_kind() &&
							candidate.receipt.native_status() ==
							found->close_terminal_receipt->native_status() &&
							candidate.receipt.native_effect_receipt() ==
							found->close_terminal_receipt->native_effect_receipt() &&
							candidate.outward_status ==
							static_cast<int>(sqlite_native_map_status::ok) &&
							candidate.quarantine_reason ==
							sqlite_shm_reader_terminal_quarantine_reason::none &&
							candidate.origin_sequence == found->close_origin_sequence &&
							candidate.cut_sequence == found->close_cut_sequence &&
							candidate.terminal_sequence == found->close_terminal_sequence;
					};
					const auto terminal = std::find_if(reader_close_terminals_.begin(),
													   reader_close_terminals_.end(),
													   exact_terminal);
					const auto exact_terminal_count =
						std::ranges::count_if(reader_close_terminals_, exact_terminal);
					const auto live_or_undurable_close_custody = std::ranges::any_of(
						reader_custodies_,
						[&found, &attachment_relates_open, &attachment_exactly_retains_open](
							const reader_custody_record& custody)
						{
							if (custody.attachment && attachment_relates_open(*custody.attachment))
								return !attachment_exactly_retains_open(*custody.attachment) ||
									custody.state == sqlite_shm_reader_custody_state::live ||
									custody.destination_sequence == 0U;
							if (custody.owner_token == found->close_owner_token ||
								(custody.open_epoch && *custody.open_epoch == found->binding))
								return custody.owner_token != found->close_owner_token ||
									custody.attachment || !custody.open_epoch ||
									*custody.open_epoch != found->binding ||
									custody.state == sqlite_shm_reader_custody_state::live ||
									custody.destination_sequence == 0U;
							return false;
						});
					const auto exact_close_custody = [&found](const reader_custody_record& custody)
					{
						if (custody.owner_token != found->close_owner_token || custody.attachment ||
							!custody.open_epoch || *custody.open_epoch != found->binding)
							return false;
						const auto exact_connection_close =
							custody.kind == sqlite_shm_reader_custody_kind::connection_close &&
							custody.state ==
								sqlite_shm_reader_custody_state::transferred_to_exact_successor &&
							custody.origin_sequence == found->close_origin_sequence &&
							custody.destination_sequence == found->close_cut_sequence;
						const auto exact_close_cut = custody.kind ==
								sqlite_shm_reader_custody_kind::close_cut_or_composite &&
							custody.state ==
								sqlite_shm_reader_custody_state::
									consumed_with_exact_terminal_receipt &&
							custody.origin_sequence == found->close_cut_sequence &&
							custody.destination_sequence == found->close_terminal_sequence;
						return exact_connection_close || exact_close_cut;
					};
					const auto close_custody_count = static_cast<std::size_t>(
						std::ranges::count_if(reader_custodies_, exact_close_custody));
					const auto unexpected_close_custody = std::ranges::any_of(
						reader_custodies_,
						[&found, &exact_close_custody](const reader_custody_record& custody)
						{
							const auto matches = custody.owner_token == found->close_owner_token ||
								(custody.open_epoch && *custody.open_epoch == found->binding);
							return matches && !exact_close_custody(custody);
						});
					if (terminal == reader_close_terminals_.end() ||
						terminal->receipt.evidence_kind() !=
							sqlite_shm_reader_close_evidence_kind::exact_native_result ||
						!terminal->receipt.native_status() ||
						*terminal->receipt.native_status() !=
							static_cast<int>(sqlite_native_map_status::ok) ||
						!terminal->receipt.native_effect_receipt() ||
						!valid_identity(*terminal->receipt.native_effect_receipt()) ||
						!valid_callback(terminal->receipt.callback()) ||
						exact_terminal_count != 1 || live_or_undurable_close_custody ||
						close_custody_count != 2U || unexpected_close_custody)
					{
						quarantine_reader_open_locked(
							*found,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						return sqlite_shm_unexpected(ambiguous());
					}
					auto next_tombstones = reader_open_close_tombstones_;
					auto next_custodies = reader_custodies_;
					const sqlite_shm_reader_open_epoch_close_tombstone compact{
						found->token,
						found->close_owner_token,
						found->binding,
						{{terminal->receipt.callback().invocation_token},
						 {*terminal->receipt.native_effect_receipt()},
						 {},
						 false},
						found->close_origin_sequence,
						found->close_cut_sequence,
						found->close_terminal_sequence};
					if (std::ranges::any_of(
							next_tombstones,
							[&compact](const sqlite_shm_reader_open_epoch_close_tombstone& existing)
							{
								return existing.registry_open_token ==
									compact.registry_open_token ||
									existing.close_owner_token == compact.close_owner_token ||
									existing.binding == compact.binding ||
									reader_replay_identity_tombstones_overlap(
										   existing.replay_identities, compact.replay_identities);
							}) ||
						!reader_close_replay_identity_tombstone_is_exact(compact.replay_identities))
					{
						quarantine_reader_open_locked(
							*found,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						return sqlite_shm_unexpected(ambiguous());
					}
					next_tombstones.push_back(compact);
					std::erase_if(next_custodies, exact_close_custody);
					if (reader_custodies_.size() - next_custodies.size() != 2U)
					{
						quarantine_reader_open_locked(
							*found,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						return sqlite_shm_unexpected(ambiguous());
					}
					reader_open_close_tombstones_.swap(next_tombstones);
					reader_custodies_.swap(next_custodies);
					reader_close_terminals_.erase(terminal);
					registry_reader_opens_.erase(found);
					return {};
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_session_admission>
			admit_registry_reader_session(
				sqlite_shm_registry_family_pin& family,
				const std::uint64_t registry_open_token,
				const sqlite_shm_reader_pre_sqlite_session_request& request,
				sqlite_shm_reader_candidate_authority_minter& candidate_minter)
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
				if (!valid_reader_pre_sqlite_session_request(request) || registry_open_token == 0U)
					return rejected(
						rejection(sqlite_shm_lease_rejection_reason::invalid_request,
								  sqlite_shm_lease_recovery_action::deny_before_native_map));
				try
				{
					std::scoped_lock lock{mutex_};
					if (auto blocked = blocked_locked(
							sqlite_shm_lease_recovery_action::deny_before_native_map))
						return rejected(*blocked);
					if (request.family != family_)
						return rejected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					if (has_live_legacy_reader_lineage_locked())
						return rejected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					const auto reader_open =
						std::find_if(registry_reader_opens_.begin(),
									 registry_reader_opens_.end(),
									 [registry_open_token](const registry_reader_open_record& open)
									 {
										 return open.token == registry_open_token;
									 });
					if (reader_open == registry_reader_opens_.end())
						return rejected(
							stale_token(sqlite_shm_lease_recovery_action::deny_before_native_map));
					if (!reader_open->seal ||
						!reader_open->seal->authority_valid.load(std::memory_order_acquire) ||
						reader_open->close_phase !=
							sqlite_shm_reader_connection_close_phase::open ||
						!reader_open_matches_pre_sqlite_request(reader_open->binding, request))
						return rejected(
							rejection(sqlite_shm_lease_rejection_reason::retiring,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));

					auto active_group = reader_attachment_groups_.end();
					std::size_t active_group_count{};
					bool matching_unresolved_group{};
					for (auto group = reader_attachment_groups_.begin();
						 group != reader_attachment_groups_.end();
						 ++group)
					{
						if (!group->registry_bound ||
							group->expected.registry_open_token() != registry_open_token)
							continue;
						if (group->reservation_phase ==
								sqlite_shm_reader_attachment_reservation_phase::observed_present &&
							group->observed_identity &&
							group->phase == reader_attachment_group_phase::active &&
							!group->existing_group_deferred_cleanup_required)
						{
							++active_group_count;
							active_group = group;
						}
						else if (!reader_reservation_is_compactable(*group))
							matching_unresolved_group = true;
					}
					if (active_group_count > 1U ||
						(active_group_count != 0U && matching_unresolved_group))
					{
						quarantine_locked();
						return rejected(ambiguous());
					}
					if (active_group_count == 1U)
					{
						if (active_group == reader_attachment_groups_.end() ||
							!active_group->registry_bound ||
							!reader_attachment_matches_pre_sqlite_request(active_group->expected,
																		  request) ||
							!active_group->registry_activity_authority ||
							!active_group->registry_activity_authority->validate_active_authority(
								family, active_group->expected))
						{
							quarantine_locked();
							return rejected(ambiguous());
						}
						if (!can_allocate_tokens_locked(1U))
							return rejected(ambiguous());
						const auto binding = reserve_reader_lifecycle_admission_binding_locked(2U);
						if (!binding.succeeded)
							return rejected(rejection(
								binding.failure == generation_failure::exhausted
									? sqlite_shm_lease_rejection_reason::generation_exhausted
									: sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								sqlite_shm_lease_recovery_action::deny_before_native_map));
						reader_terminal_slot_guard admission_slot{
							reader_lifecycle_sequences_->state_.get(), binding.slots[0]};
						reader_terminal_slot_guard terminal_slot{
							reader_lifecycle_sequences_->state_.get(), binding.slots[1]};
						auto proposal_request = sqlite_shm_reader_session_request{
							active_group->expected,
							request.execution,
							request.read_transaction_epoch,
							request.decode_attempt,
							request.authority_read_receipt,
						};
						if (std::ranges::any_of(
								reader_sessions_,
								[&proposal_request](const reader_session_record& session)
								{
									return same_reader_session_owner_key(session.request,
																		 proposal_request);
								}) ||
							std::ranges::any_of(
								reader_session_terminals_,
								[&proposal_request](const reader_session_terminal_record& terminal)
								{
									return same_reader_session_owner_key(terminal.receipt.request(),
																		 proposal_request);
								}))
							return rejected(rejection(
								sqlite_shm_lease_rejection_reason::stale_token,
								sqlite_shm_lease_recovery_action::deny_before_native_map));

						std::vector<sqlite_shm_reader_cached_member_identity> captured_members;
						captured_members.reserve(active_group->members.size());
						for (const auto& member : active_group->members)
							captured_members.push_back(member.identity);
						std::vector<std::uint64_t> captured_audits;
						captured_audits.reserve(active_group->audits.size());
						for (const auto& audit : active_group->audits)
							captured_audits.push_back(audit.map_attempt_token);
						const auto token = next_token_;
						auto next_custodies = reader_custodies_;
						next_custodies.push_back({sqlite_shm_reader_custody_kind::use_session,
												  sqlite_shm_reader_custody_state::live,
												  proposal_request.attachment,
												  token,
												  0U,
												  0U});
						std::list<reader_session_record> prepared_sessions;
						prepared_sessions.push_back(
							{token,
							 active_group->generation,
							 reader_session_record_phase::active_group_owner,
							 proposal_request,
							 active_group->token,
							 true,
							 std::move(captured_members),
							 std::move(captured_audits),
							 sqlite_shm_reader_session_reservation_phase::promoted_to_group_owner,
							 0U,
							 0U,
							 0U,
							 0U,
							 std::nullopt,
							 sqlite_shm_reader_terminal_quarantine_reason::none});
						auto admission_state = shared_from_this();
						const auto sequences = commit_reader_lifecycle_admission_binding_locked(
							std::span{binding.slots}.first(2U));
						if (!sequences.succeeded)
							return rejected(rejection(
								sequences.failure == generation_failure::exhausted
									? sqlite_shm_lease_rejection_reason::generation_exhausted
									: sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								sqlite_shm_lease_recovery_action::deny_before_native_map));
						(void)allocate_token_unchecked_locked();
						next_custodies.back().owner_token = token;
						next_custodies.back().origin_sequence = sequences.first;
						prepared_sessions.back().lifecycle_origin_sequence = sequences.first;
						prepared_sessions.back().lifecycle_destination_sequence = sequences.first;
						prepared_sessions.back().terminal_sequence_slot = binding.slots[1];
						reader_sessions_.splice(reader_sessions_.end(), prepared_sessions);
						reader_custodies_.swap(next_custodies);
						reader_last_committed_sequence_ =
							std::max(reader_last_committed_sequence_, sequences.first);
						admission_slot.release();
						terminal_slot.release();
						return sqlite_shm_reader_session_admission{
							sqlite_shm_reader_session_admission_kind::active_group_owner_admitted,
							std::move(proposal_request),
							sqlite_shm_reader_session{
								std::move(admission_state),
								sqlite_shm_lease_token_identity{token},
								sqlite_shm_mapping_generation_identity{active_group->generation},
								sqlite_shm_reader_session_phase::active_group_owner,
							},
							std::nullopt,
						};
					}
					if (matching_unresolved_group)
						return rejected(
							rejection(sqlite_shm_lease_rejection_reason::retiring,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));

					if (!generation_)
						return sqlite_shm_reader_session_admission{
							sqlite_shm_reader_session_admission_kind::
								existing_or_ordinary_predecessor_zero_proposal_custody,
							std::nullopt,
							std::nullopt,
							std::nullopt,
						};
					if (generation_->phase != sqlite_shm_mapping_generation_phase::live)
						return rejected(rejection(
							generation_->phase == sqlite_shm_mapping_generation_phase::retiring
								? sqlite_shm_lease_rejection_reason::retiring
								: sqlite_shm_lease_rejection_reason::successor_handoff_live,
							sqlite_shm_lease_recovery_action::deny_before_native_map));
					const auto exact_live_local_generation =
						std::ranges::any_of(holders_,
											[this](const holder_record& holder)
											{
												return holder.phase == holder_phase::active &&
													holder.generation == generation_->value &&
													holder.registry_bound;
											});
					if (!exact_live_local_generation)
						return sqlite_shm_reader_session_admission{
							sqlite_shm_reader_session_admission_kind::
								existing_or_ordinary_predecessor_zero_proposal_custody,
							std::nullopt,
							std::nullopt,
							std::nullopt,
						};

					if (!can_allocate_tokens_locked(2U))
						return rejected(ambiguous());
					const auto binding = reserve_reader_lifecycle_admission_binding_locked(2U);
					if (!binding.succeeded)
						return rejected(
							rejection(binding.failure == generation_failure::exhausted
										  ? sqlite_shm_lease_rejection_reason::generation_exhausted
										  : sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					reader_terminal_slot_guard admission_slot{
						reader_lifecycle_sequences_->state_.get(), binding.slots[0]};
					reader_terminal_slot_guard terminal_slot{
						reader_lifecycle_sequences_->state_.get(), binding.slots[1]};
					auto candidate = candidate_minter.mint(generation_->value);
					if (!candidate)
						return rejected(candidate.error());
					reader_candidate_authority_cancel_guard candidate_guard{candidate_minter,
																			candidate->authority};
					if (!valid_reader_session_request(candidate->request) ||
						!reader_attachment_matches_pre_sqlite_request(candidate->request.attachment,
																	  request) ||
						candidate->request.attachment.writer_mapping_generation() !=
							generation_->value ||
						!candidate->authority.valid_for_predelegation(candidate->request) ||
						!candidate->authority.validate_active_authority(
							family, candidate->request.attachment))
					{
						quarantine_locked();
						return rejected(ambiguous());
					}
					if (std::ranges::any_of(
							reader_sessions_,
							[&candidate](const reader_session_record& session)
							{
								return session.request.attachment.attachment_epoch() ==
									candidate->request.attachment.attachment_epoch();
							}) ||
						std::ranges::any_of(
							reader_attachment_groups_,
							[&candidate](const reader_attachment_group_record& group)
							{
								return group.expected.attachment_epoch() ==
									candidate->request.attachment.attachment_epoch();
							}))
					{
						quarantine_locked();
						return rejected(ambiguous());
					}
					const auto token = next_token_;
					const auto reservation_token = next_token_ + 1U;
					auto next_custodies = reader_custodies_;
					next_custodies.push_back(
						{sqlite_shm_reader_custody_kind::use_session_reservation,
						 sqlite_shm_reader_custody_state::live,
						 candidate->request.attachment,
						 token,
						 0U,
						 0U});
					next_custodies.push_back(
						{sqlite_shm_reader_custody_kind::
							 runtime_vfs_namespace_generation_native_mapping_lifetime_pin,
						 sqlite_shm_reader_custody_state::live,
						 candidate->request.attachment,
						 reservation_token,
						 0U,
						 0U});
					std::list<reader_attachment_group_record> prepared_groups;
					prepared_groups.emplace_back(
						reservation_token, generation_->value, candidate->request.attachment);
					prepared_groups.back().registry_bound = true;
					std::list<reader_session_record> prepared_sessions;
					prepared_sessions.push_back(
						{token,
						 generation_->value,
						 reader_session_record_phase::reserved_for_first_map,
						 candidate->request,
						 reservation_token,
						 true,
						 {},
						 {},
						 sqlite_shm_reader_session_reservation_phase::reserved_before_sqlite,
						 0U,
						 0U,
						 0U,
						 0U,
						 std::nullopt,
						 sqlite_shm_reader_terminal_quarantine_reason::none});
					auto admission_state = shared_from_this();
					static_assert(std::is_nothrow_move_constructible_v<
								  sqlite_shm_reader_attachment_authority>);
					candidate_guard.release();
					prepared_groups.back().registry_activity_authority.emplace(
						std::move(candidate->authority));
					const auto sequences = commit_reader_lifecycle_admission_binding_locked(
						std::span{binding.slots}.first(2U));
					if (!sequences.succeeded)
					{
						candidate_minter.cancel(
							*prepared_groups.back().registry_activity_authority);
						return rejected(
							rejection(sequences.failure == generation_failure::exhausted
										  ? sqlite_shm_lease_rejection_reason::generation_exhausted
										  : sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					}
					(void)allocate_token_unchecked_locked();
					(void)allocate_token_unchecked_locked();
					next_custodies[next_custodies.size() - 2U].origin_sequence = sequences.first;
					next_custodies.back().origin_sequence = sequences.first;
					prepared_groups.back().reservation_origin_sequence = sequences.first;
					prepared_sessions.back().lifecycle_origin_sequence = sequences.first;
					prepared_sessions.back().terminal_sequence_slot = binding.slots[1];
					reader_attachment_groups_.splice(reader_attachment_groups_.end(),
													 prepared_groups);
					reader_sessions_.splice(reader_sessions_.end(), prepared_sessions);
					reader_custodies_.swap(next_custodies);
					reader_last_committed_sequence_ =
						std::max(reader_last_committed_sequence_, sequences.first);
					admission_slot.release();
					terminal_slot.release();
					return sqlite_shm_reader_session_admission{
						sqlite_shm_reader_session_admission_kind::
							reserved_for_local_proposal_candidate,
						std::move(candidate->request),
						sqlite_shm_reader_session{
							std::move(admission_state),
							sqlite_shm_lease_token_identity{token},
							sqlite_shm_mapping_generation_identity{generation_->value},
							sqlite_shm_reader_session_phase::reserved_for_first_map,
						},
						std::nullopt,
					};
				}
				catch (...)
				{
					return rejected(
						rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								  sqlite_shm_lease_recovery_action::deny_before_native_map));
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_session>
			begin_reader_session(const sqlite_shm_reader_session_request& request)
			{
				if (!valid_reader_session_request(request))
					return sqlite_shm_unexpected(
						rejection(sqlite_shm_lease_rejection_reason::invalid_request,
								  sqlite_shm_lease_recovery_action::deny_before_native_map));
				try
				{
					std::scoped_lock lock{mutex_};
					if (auto blocked = blocked_locked(
							sqlite_shm_lease_recovery_action::deny_before_native_map))
						return sqlite_shm_unexpected(*blocked);
					if (request.attachment.family() != family_)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					if (has_live_registry_reader_lineage_locked())
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					const auto epoch_collides =
						[&request](
							const sqlite_shm_reader_attachment_reservation_identity& existing)
					{
						return existing.attachment_epoch() ==
							request.attachment.attachment_epoch() &&
							existing != request.attachment;
					};
					if (std::ranges::any_of(reader_sessions_,
											[&epoch_collides](const reader_session_record& session)
											{
												return epoch_collides(session.request.attachment);
											}) ||
						std::ranges::any_of(
							reader_session_terminals_,
							[&epoch_collides](const reader_session_terminal_record& terminal)
							{
								return epoch_collides(terminal.receipt.request().attachment);
							}) ||
						std::ranges::any_of(
							reader_attachment_zero_effect_terminals_,
							[&epoch_collides](
								const reader_attachment_zero_effect_terminal_record& terminal)
							{
								return epoch_collides(terminal.session_request.attachment);
							}) ||
						std::ranges::any_of(
							reader_attachment_groups_,
							[&epoch_collides](const reader_attachment_group_record& group)
							{
								return epoch_collides(group.expected);
							}))
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					if (std::ranges::any_of(
							reader_sessions_,
							[&request](const reader_session_record& session)
							{
								return session.phase !=
									reader_session_record_phase::terminal_quarantined &&
									same_reader_session_owner_key(session.request, request);
							}))
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					if (std::ranges::any_of(
							reader_session_terminals_,
							[&request](const reader_session_terminal_record& terminal)
							{
								return same_reader_session_owner_key(terminal.receipt.request(),
																	 request);
							}))
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::stale_token,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));

					const auto group = std::find_if(
						reader_attachment_groups_.begin(),
						reader_attachment_groups_.end(),
						[&request](const reader_attachment_group_record& value)
						{
							return value.reservation_phase ==
								sqlite_shm_reader_attachment_reservation_phase::observed_present &&
								value.observed_identity &&
								value.phase == reader_attachment_group_phase::active &&
								!value.existing_group_deferred_cleanup_required &&
								value.expected == request.attachment;
						});
					const auto joins_existing_group = group != reader_attachment_groups_.end();
					if (joins_existing_group && group->registry_bound)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					if (!joins_existing_group &&
						std::ranges::any_of(reader_attachment_groups_,
											[&request](const reader_attachment_group_record& value)
											{
												return value.expected == request.attachment &&
													value.existing_group_deferred_cleanup_required;
											}))
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::retiring,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					if (!joins_existing_group &&
						std::ranges::any_of(reader_attachment_groups_,
											[&request](const reader_attachment_group_record& value)
											{
												return value.expected == request.attachment;
											}))
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::stale_token,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					std::uint64_t generation{};
					std::uint64_t reservation_token{};
					auto public_phase = sqlite_shm_reader_session_phase::reserved_for_first_map;
					auto record_phase = reader_session_record_phase::reserved_for_first_map;
					auto lifecycle_phase =
						sqlite_shm_reader_session_reservation_phase::reserved_before_sqlite;
					if (joins_existing_group)
					{
						generation = group->generation;
						reservation_token = group->token;
						public_phase = sqlite_shm_reader_session_phase::active_group_owner;
						record_phase = reader_session_record_phase::active_group_owner;
						lifecycle_phase =
							sqlite_shm_reader_session_reservation_phase::promoted_to_group_owner;
					}
					else
					{
						if (std::ranges::any_of(
								reader_session_terminals_,
								[&request](const reader_session_terminal_record& terminal)
								{
									return terminal.origin_phase ==
										reader_session_record_phase::reserved_for_first_map &&
										terminal.receipt.request().attachment == request.attachment;
								}))
							return sqlite_shm_unexpected(rejection(
								sqlite_shm_lease_rejection_reason::stale_token,
								sqlite_shm_lease_recovery_action::deny_before_native_map));
						if (std::ranges::any_of(
								reader_attachment_zero_effect_terminals_,
								[&request](
									const reader_attachment_zero_effect_terminal_record& terminal)
								{
									return terminal.revoked_first_reservation &&
										terminal.session_request.attachment == request.attachment;
								}))
							return sqlite_shm_unexpected(rejection(
								sqlite_shm_lease_rejection_reason::stale_token,
								sqlite_shm_lease_recovery_action::deny_before_native_map));
						// A non-zero registry-open token is meaningful only on the
						// registry-authenticated route.  Preserve exact local tombstone
						// precedence above, then reject copied-token use before any allocation
						// or lifecycle mutation.
						if (request.attachment.registry_open_token() != 0U)
							return sqlite_shm_unexpected(rejection(
								sqlite_shm_lease_rejection_reason::receipt_mismatch,
								sqlite_shm_lease_recovery_action::deny_before_native_map));
						if (std::ranges::any_of(
								reader_sessions_,
								[&request](const reader_session_record& session)
								{
									return session.phase ==
										reader_session_record_phase::reserved_for_first_map &&
										session.request.attachment == request.attachment;
								}))
							return sqlite_shm_unexpected(rejection(
								sqlite_shm_lease_rejection_reason::pending_or_eligibility_only,
								sqlite_shm_lease_recovery_action::deny_before_native_map));
						if (!generation_)
							return sqlite_shm_unexpected(rejection(
								eligibilities_.empty() && writers_.empty()
									? sqlite_shm_lease_rejection_reason::no_live_generation
									: sqlite_shm_lease_rejection_reason::
										  pending_or_eligibility_only,
								sqlite_shm_lease_recovery_action::deny_before_native_map));
						if (request.attachment.writer_mapping_generation() != generation_->value)
							return sqlite_shm_unexpected(rejection(
								sqlite_shm_lease_rejection_reason::stale_generation,
								sqlite_shm_lease_recovery_action::deny_before_native_map));
						if (generation_->phase != sqlite_shm_mapping_generation_phase::live)
							return sqlite_shm_unexpected(rejection(
								generation_->handoff_count == 0U
									? sqlite_shm_lease_rejection_reason::retiring
									: sqlite_shm_lease_rejection_reason::successor_handoff_live,
								sqlite_shm_lease_recovery_action::deny_before_native_map));
						if (active_holder_count_locked() == 0U)
							return sqlite_shm_unexpected(rejection(
								sqlite_shm_lease_rejection_reason::pending_or_eligibility_only,
								sqlite_shm_lease_recovery_action::deny_before_native_map));
						generation = generation_->value;
					}

					const auto required_tokens = joins_existing_group ? 1U : 2U;
					if (!can_allocate_tokens_locked(required_tokens))
						return sqlite_shm_unexpected(ambiguous());
					const auto binding = reserve_reader_lifecycle_admission_binding_locked(2U);
					if (!binding.succeeded)
						return sqlite_shm_unexpected(
							rejection(binding.failure == generation_failure::exhausted
										  ? sqlite_shm_lease_rejection_reason::generation_exhausted
										  : sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					reader_terminal_slot_guard admission_slot{
						reader_lifecycle_sequences_->state_.get(), binding.slots[0]};
					reader_terminal_slot_guard terminal_slot{
						reader_lifecycle_sequences_->state_.get(), binding.slots[1]};
					std::vector<sqlite_shm_reader_cached_member_identity> captured_members;
					std::vector<std::uint64_t> captured_audits;
					if (joins_existing_group)
					{
						captured_members.reserve(group->members.size());
						for (const auto& member : group->members)
							captured_members.push_back(member.identity);
						captured_audits.reserve(group->audits.size());
						for (const auto& audit : group->audits)
							captured_audits.push_back(audit.map_attempt_token);
					}
					const auto token = next_token_;
					if (!joins_existing_group)
						reservation_token = next_token_ + 1U;
					auto next_custodies = reader_custodies_;
					next_custodies.push_back(
						{joins_existing_group
							 ? sqlite_shm_reader_custody_kind::use_session
							 : sqlite_shm_reader_custody_kind::use_session_reservation,
						 sqlite_shm_reader_custody_state::live,
						 request.attachment,
						 token,
						 0U,
						 0U});
					std::list<reader_session_record> prepared_sessions;
					prepared_sessions.push_back(
						{token,
						 generation,
						 record_phase,
						 request,
						 reservation_token,
						 false,
						 std::move(captured_members),
						 std::move(captured_audits),
						 lifecycle_phase,
						 0U,
						 0U,
						 0U,
						 0U,
						 std::nullopt,
						 sqlite_shm_reader_terminal_quarantine_reason::none});
					std::list<reader_attachment_group_record> prepared_groups;
					if (!joins_existing_group)
						prepared_groups.emplace_back(
							reservation_token, generation, request.attachment);
					auto admitted_state = shared_from_this();
					const auto sequences = commit_reader_lifecycle_admission_binding_locked(
						std::span{binding.slots}.first(2U));
					if (!sequences.succeeded)
						return sqlite_shm_unexpected(
							rejection(sequences.failure == generation_failure::exhausted
										  ? sqlite_shm_lease_rejection_reason::generation_exhausted
										  : sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					for (std::size_t index = 0; index < required_tokens; ++index)
						(void)allocate_token_unchecked_locked();
					next_custodies.back().owner_token = token;
					next_custodies.back().origin_sequence = sequences.first;
					prepared_sessions.back().lifecycle_origin_sequence = sequences.first;
					prepared_sessions.back().lifecycle_destination_sequence =
						joins_existing_group ? sequences.first : 0U;
					prepared_sessions.back().terminal_sequence_slot = binding.slots[1];
					if (!joins_existing_group)
					{
						prepared_groups.back().reservation_origin_sequence = sequences.first;
						reader_attachment_groups_.splice(reader_attachment_groups_.end(),
														 prepared_groups);
					}
					reader_sessions_.splice(reader_sessions_.end(), prepared_sessions);
					reader_custodies_.swap(next_custodies);
					reader_last_committed_sequence_ =
						std::max(reader_last_committed_sequence_, sequences.first);
					admission_slot.release();
					terminal_slot.release();
					return sqlite_shm_reader_session{
						std::move(admitted_state),
						sqlite_shm_lease_token_identity{token},
						sqlite_shm_mapping_generation_identity{generation},
						public_phase};
				}
				catch (...)
				{
					return sqlite_shm_unexpected(
						rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								  sqlite_shm_lease_recovery_action::deny_before_native_map));
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_mapping_tuple>
			authenticate_reader_cached_member_use(
				const sqlite_shm_reader_session& session,
				const sqlite_shm_reader_cached_member_identity& member,
				sqlite_shm_registry_family_pin* registry_family = nullptr) noexcept
			{
				if (!valid_reader_native_attachment(member.attachment_) ||
					member.group_token_ == 0U || member.generation_ == 0U ||
					member.member_token_ == 0U || !valid_mapping(member.mapping_))
					return sqlite_shm_unexpected(
						rejection(sqlite_shm_lease_rejection_reason::invalid_request,
								  sqlite_shm_lease_recovery_action::deny_before_native_map));
				try
				{
					std::scoped_lock lock{mutex_};
					if (!owns(session.state_, session.token_) ||
						session.terminal_presentation_disabled_)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::deny_before_native_map));
					if (is_quarantined_locked())
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::quarantined,
									  sqlite_shm_lease_recovery_action::quarantine_no_retry));

					const auto owner = find_by_token(reader_sessions_, session.token_);
					if (owner == reader_sessions_.end() ||
						owner->phase != reader_session_record_phase::active_group_owner ||
						session.phase_ != sqlite_shm_reader_session_phase::active_group_owner ||
						owner->generation != session.generation_)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::deny_before_native_map));
					const auto registry_route = registry_family != nullptr;
					if (owner->registry_bound != registry_route ||
						(registry_route && has_live_legacy_reader_lineage_locked()) ||
						(!registry_route && has_live_registry_reader_lineage_locked()))
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));

					const auto group = find_by_token(reader_attachment_groups_, owner->group_token);
					if (group == reader_attachment_groups_.end() ||
						group->phase != reader_attachment_group_phase::active ||
						group->reservation_phase !=
							sqlite_shm_reader_attachment_reservation_phase::observed_present ||
						group->existing_group_deferred_cleanup_required ||
						!group->observed_identity)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::retiring,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					if (group->token != member.group_token_ ||
						group->generation != member.generation_ ||
						group->expected != member.attachment_ ||
						owner->group_token != member.group_token_ ||
						owner->generation != member.generation_ ||
						owner->request.attachment != member.attachment_)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					if (registry_route &&
						(!group->registry_bound || !group->registry_activity_authority ||
						 !group->registry_activity_authority->validate_active_authority(
							 *registry_family, group->expected)))
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									  sqlite_shm_lease_recovery_action::quarantine_no_retry));

					const auto group_member = std::ranges::find(
						group->members, member, &reader_attachment_group_member_record::identity);
					const auto captured_member = std::ranges::find(owner->captured_members, member);
					if (group_member == group->members.end() ||
						captured_member == owner->captured_members.end())
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					if (group->members.size() < owner->captured_members.size() ||
						group->audits.size() < owner->captured_audits.size())
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									  sqlite_shm_lease_recovery_action::quarantine_no_retry));
					for (std::size_t index = 0; index < owner->captured_members.size(); ++index)
						if (group->members[index].identity != owner->captured_members[index])
							return sqlite_shm_unexpected(
								rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										  sqlite_shm_lease_recovery_action::quarantine_no_retry));
					for (std::size_t index = 0; index < owner->captured_audits.size(); ++index)
						if (group->audits[index].map_attempt_token != owner->captured_audits[index])
							return sqlite_shm_unexpected(
								rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
										  sqlite_shm_lease_recovery_action::quarantine_no_retry));
					return member.mapping_;
				}
				catch (...)
				{
					return sqlite_shm_unexpected(
						rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								  sqlite_shm_lease_recovery_action::quarantine_no_retry));
				}
			}

			[[nodiscard]]
			sqlite_shm_lease_result<sqlite_shm_reader_attachment_map_inflight> begin_reader(
				sqlite_shm_reader_session& session,
				const sqlite_shm_reader_attachment_map_request& request,
				sqlite_shm_registry_family_pin* registry_family = nullptr,
				sqlite_shm_reader_map_predelegate_minter* registry_predelegate_minter = nullptr)
			{
				if (!valid_reader_attachment_map_request(request))
					return sqlite_shm_unexpected(
						rejection(request.caller_extend == 0
									  ? sqlite_shm_lease_rejection_reason::invalid_request
									  : sqlite_shm_lease_rejection_reason::invalid_extend_pair,
								  sqlite_shm_lease_recovery_action::deny_before_native_map));
				std::optional<sqlite_shm_reader_map_predelegate_authority> predelegate;
				try
				{
					std::scoped_lock lock{mutex_};
					if (!owns(session.state_, session.token_))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::deny_before_native_map));
					if (auto blocked = blocked_locked(
							sqlite_shm_lease_recovery_action::deny_before_native_map))
						return sqlite_shm_unexpected(*blocked);
					const auto owner = find_by_token(reader_sessions_, session.token_);
					if (owner == reader_sessions_.end() ||
						owner->phase == reader_session_record_phase::terminal_quarantined ||
						owner->generation != session.generation_ ||
						owner->request.attachment != request.expected_attachment ||
						request.family != family_)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::deny_before_native_map));
					const auto registry_route =
						registry_family != nullptr && registry_predelegate_minter != nullptr;
					if ((registry_route && has_live_legacy_reader_lineage_locked()) ||
						(!registry_route && has_live_registry_reader_lineage_locked()))
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					if (owner->registry_bound != registry_route)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					if (std::ranges::any_of(reader_attachment_maps_,
											[&owner](const reader_attachment_map_record& map)
											{
												return map.session_token == owner->token &&
													map.phase != reader_phase::terminal_quarantined;
											}))
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					if (!generation_ || generation_->value != owner->generation)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::stale_generation,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));

					auto expected = generation_->pages.end();
					auto group = reader_attachment_groups_.end();
					auto retirement_blocker = true;
					if (owner->phase == reader_session_record_phase::active_group_owner)
					{
						group = find_by_token(reader_attachment_groups_, owner->group_token);
						if (group != reader_attachment_groups_.end() &&
							group->existing_group_deferred_cleanup_required)
							return sqlite_shm_unexpected(rejection(
								sqlite_shm_lease_rejection_reason::retiring,
								sqlite_shm_lease_recovery_action::deny_before_native_map));
						if (group == reader_attachment_groups_.end() ||
							group->phase != reader_attachment_group_phase::active ||
							group->generation != owner->generation ||
							group->expected != owner->request.attachment)
							return sqlite_shm_unexpected(stale_token(
								sqlite_shm_lease_recovery_action::deny_before_native_map));
						if (registry_route &&
							(!group->registry_bound || !group->registry_activity_authority ||
							 !group->registry_activity_authority->validate_active_authority(
								 *registry_family, group->expected)))
						{
							quarantine_reader_session_locked(*owner);
							return sqlite_shm_unexpected(ambiguous());
						}
						const auto member = std::find_if(
							group->members.begin(),
							group->members.end(),
							[&request](const reader_attachment_group_member_record& value)
							{
								return value.identity.mapping().page_number == request.page_number;
							});
						if (member != group->members.end())
						{
							retirement_blocker = false;
							if (member->identity.mapping().page_size != request.page_size)
								return sqlite_shm_unexpected(rejection(
									sqlite_shm_lease_rejection_reason::mapping_mismatch,
									sqlite_shm_lease_recovery_action::deny_before_native_map));
							expected = std::find_if(generation_->pages.begin(),
													generation_->pages.end(),
													[&member](const sqlite_shm_mapping_tuple& value)
													{
														return same_mapping_page(
															value, member->identity.mapping());
													});
						}
					}
					else
					{
						group = find_by_token(reader_attachment_groups_, owner->group_token);
						if (owner->phase != reader_session_record_phase::reserved_for_first_map ||
							group == reader_attachment_groups_.end() ||
							group->expected != owner->request.attachment ||
							group->reservation_phase !=
								sqlite_shm_reader_attachment_reservation_phase::reserved ||
							group->observed_identity || group->registry_bound != registry_route)
							return sqlite_shm_unexpected(stale_token(
								sqlite_shm_lease_recovery_action::deny_before_native_map));
						if (registry_route &&
							(group->composite_close_owner_token != 0U ||
							 group->composite_close_registry_open_token != 0U ||
							 group->composite_close_cut_sequence != 0U ||
							 group->composite_close_wait_resolution_sequence_slot != 0U ||
							 group->composite_close_wait_resolution_sequence != 0U))
						{
							const auto open = std::find_if(
								registry_reader_opens_.begin(),
								registry_reader_opens_.end(),
								[&group](const registry_reader_open_record& candidate)
								{
									return candidate.token == group->expected.registry_open_token();
								});
							if (open != registry_reader_opens_.end() &&
								reader_phase1_close_cut_is_exact_locked(*group, *open))
								return sqlite_shm_unexpected(rejection(
									sqlite_shm_lease_rejection_reason::retiring,
									sqlite_shm_lease_recovery_action::deny_before_native_map));
							quarantine_reader_session_locked(*owner);
							return sqlite_shm_unexpected(ambiguous());
						}
					}
					if (expected == generation_->pages.end())
					{
						if (generation_->phase != sqlite_shm_mapping_generation_phase::live)
							return sqlite_shm_unexpected(rejection(
								generation_->handoff_count == 0U
									? sqlite_shm_lease_rejection_reason::retiring
									: sqlite_shm_lease_rejection_reason::successor_handoff_live,
								sqlite_shm_lease_recovery_action::deny_before_native_map));
						expected = find_page_locked(request.page_number);
					}
					if (expected == generation_->pages.end() ||
						expected->page_size != request.page_size)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::mapping_mismatch,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					if (reader_callback_was_completed_locked(request.callback))
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::stale_token,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					if (!callback_can_start_locked(request.callback))
					{
						quarantine_reader_session_locked(*owner);
						return sqlite_shm_unexpected(ambiguous());
					}
					if (!can_allocate_tokens_locked(1U))
						return sqlite_shm_unexpected(ambiguous());
					const auto first_map =
						owner->phase == reader_session_record_phase::reserved_for_first_map;
					const auto binding =
						reserve_reader_lifecycle_admission_binding_locked(first_map ? 4U : 2U);
					if (!binding.succeeded)
						return sqlite_shm_unexpected(
							rejection(binding.failure == generation_failure::exhausted
										  ? sqlite_shm_lease_rejection_reason::generation_exhausted
										  : sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					reader_terminal_slot_guard admission_slot{
						reader_lifecycle_sequences_->state_.get(), binding.slots[0]};
					reader_terminal_slot_guard terminal_slot{
						reader_lifecycle_sequences_->state_.get(), binding.slots[1]};
					reader_terminal_slot_guard group_cut_slot{
						reader_lifecycle_sequences_->state_.get(), binding.slots[2]};
					reader_terminal_slot_guard group_terminal_slot{
						reader_lifecycle_sequences_->state_.get(), binding.slots[3]};
					auto expected_mapping = *expected;
					expected_mapping.sealed_shm_size = generation_->sealed_shm_size;
					auto next_custodies = reader_custodies_;
					next_custodies.push_back({sqlite_shm_reader_custody_kind::map_attempt,
											  sqlite_shm_reader_custody_state::live,
											  owner->request.attachment,
											  next_token_,
											  0U,
											  0U});
					if (registry_route && retirement_blocker)
					{
						auto minted = registry_predelegate_minter->mint(request);
						if (!minted)
							return sqlite_shm_unexpected(minted.error());
						predelegate.emplace(std::move(*minted));
						if (!predelegate->valid_for_predelegation(request) ||
							!predelegate->validate_active_authority(*registry_family, request))
						{
							registry_predelegate_minter->cancel(*predelegate);
							quarantine_reader_session_locked(*owner);
							return sqlite_shm_unexpected(ambiguous());
						}
					}
					const auto token = next_token_;
					std::list<reader_attachment_map_record> prepared_maps;
					prepared_maps.push_back({token,
											 reader_phase::inflight,
											 request,
											 owner->generation,
											 expected_mapping,
											 generation_->pages.size(),
											 std::nullopt,
											 std::nullopt,
											 owner->token,
											 owner->group_token,
											 retirement_blocker,
											 registry_route,
											 std::nullopt,
											 0U,
											 0U});
					auto admitted_state = shared_from_this();
					if (predelegate)
					{
						static_assert(std::is_nothrow_move_constructible_v<
									  sqlite_shm_reader_map_predelegate_authority>);
						prepared_maps.back().registry_predelegate_authority.emplace(
							std::move(*predelegate));
					}
					const auto sequences = commit_reader_lifecycle_admission_binding_locked(
						std::span{binding.slots}.first(first_map ? 4U : 2U));
					if (!sequences.succeeded)
					{
						if (prepared_maps.back().registry_predelegate_authority)
							registry_predelegate_minter->cancel(
								*prepared_maps.back().registry_predelegate_authority);
						return sqlite_shm_unexpected(
							rejection(sequences.failure == generation_failure::exhausted
										  ? sqlite_shm_lease_rejection_reason::generation_exhausted
										  : sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					}
					(void)allocate_token_unchecked_locked();
					next_custodies.back().owner_token = token;
					next_custodies.back().origin_sequence = sequences.first;
					prepared_maps.back().admission_sequence = sequences.first;
					prepared_maps.back().terminal_sequence_slot = binding.slots[1];
					prepared_maps.back().potential_group_cut_sequence_slot = binding.slots[2];
					prepared_maps.back().potential_group_terminal_sequence_slot = binding.slots[3];
					reader_attachment_maps_.splice(reader_attachment_maps_.end(), prepared_maps);
					reader_custodies_.swap(next_custodies);
					reader_last_committed_sequence_ =
						std::max(reader_last_committed_sequence_, sequences.first);
					admission_slot.release();
					terminal_slot.release();
					group_cut_slot.release();
					group_terminal_slot.release();
					return sqlite_shm_reader_attachment_map_inflight{
						std::move(admitted_state),
						sqlite_shm_lease_token_identity{token},
						sqlite_shm_mapping_generation_identity{owner->generation}};
				}
				catch (...)
				{
					if (predelegate && registry_predelegate_minter)
						registry_predelegate_minter->cancel(*predelegate);
					return sqlite_shm_unexpected(
						rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								  sqlite_shm_lease_recovery_action::deny_before_native_map));
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_map_commit> commit_reader(
				sqlite_shm_reader_attachment_map_inflight& inflight,
				const sqlite_shm_verified_reader_attachment_post_map_receipt& receipt,
				sqlite_shm_reader_session& session,
				sqlite_shm_registry_family_pin* registry_family = nullptr,
				std::optional<sqlite_shm_reader_map_predelegate_authority>* completed_predelegate =
					nullptr)
			{
				if (emergency_quarantine_.load(std::memory_order_acquire) ||
					inflight.terminal_presentation_disabled_ ||
					session.terminal_presentation_disabled_)
				{
					if (owns(inflight.state_, inflight.token_))
						inflight.disable_terminal_presentation();
					if (owns(session.state_, session.token_))
						session.disable_terminal_presentation();
					return sqlite_shm_unexpected(ambiguous());
				}
				try
				{
					if (fail_next_reader_operation_mutex_acquire_for_testing_.exchange(
							false, std::memory_order_acq_rel))
						throw reader_operation_mutex_acquire_injected_failure{};
					std::scoped_lock lock{mutex_};
					if (!owns(inflight.state_, inflight.token_) ||
						!owns(session.state_, session.token_))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::
											attempt_nonremoving_unmap_then_outer_ioerr));
					const auto map_attempt =
						find_by_token(reader_attachment_maps_, inflight.token_);
					const auto owner = find_by_token(reader_sessions_, session.token_);
					if (map_attempt == reader_attachment_maps_.end() ||
						map_attempt->phase != reader_phase::inflight ||
						map_attempt->session_token != session.token_ ||
						owner == reader_sessions_.end() ||
						owner->phase == reader_session_record_phase::terminal_quarantined ||
						owner->generation != inflight.generation_ ||
						owner->generation != session.generation_)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::
											attempt_nonremoving_unmap_then_outer_ioerr));
					const auto registry_route =
						registry_family != nullptr && completed_predelegate != nullptr;
					if (map_attempt->registry_bound != registry_route ||
						owner->registry_bound != registry_route ||
						(registry_route && completed_predelegate->has_value()))
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::resubmit_via_bound_route));
					if (map_attempt->unpublished_cleanup_required)
						return sqlite_shm_unexpected(rejection(
							sqlite_shm_lease_rejection_reason::unpublished_cleanup_required,
							sqlite_shm_lease_recovery_action::
								attempt_nonremoving_unmap_then_outer_ioerr));
					auto prepared_terminal_receipt = receipt;
					const auto quarantine_terminal =
						[&](const sqlite_shm_reader_terminal_quarantine_reason reason)
						-> sqlite_shm_lease_result<sqlite_shm_reader_map_commit>
					{
						map_attempt->quarantine_reason = reason;
						quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																	 owner->token);
						inflight.disarm();
						session.disarm();
						return reason ==
								sqlite_shm_reader_terminal_quarantine_reason::presented_invalid
							? sqlite_shm_unexpected(
								  rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
											sqlite_shm_lease_recovery_action::quarantine_no_retry))
							: sqlite_shm_unexpected(ambiguous());
					};
					const auto map_effect_identity_reused =
						reader_map_effect_identity_seen_locked(receipt.zero_resize_effect_receipt(),
															   map_attempt->token) ||
						reader_callback_invocation_was_seen_locked(
							receipt.zero_resize_effect_receipt()) ||
						reader_session_terminal_identity_seen_locked(
							receipt.zero_resize_effect_receipt());
					if (map_effect_identity_reused)
					{
						map_attempt->quarantine_reason =
							sqlite_shm_reader_terminal_quarantine_reason::presented_invalid;
						quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																	 owner->token);
						inflight.disarm();
						session.disarm();
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::quarantine_no_retry));
					}
					if (!valid_reader_attachment_receipt(receipt) ||
						receipt.request() != map_attempt->request ||
						receipt.generation() != map_attempt->generation ||
						receipt.mapping() != map_attempt->expected_mapping)
						return quarantine_terminal(
							sqlite_shm_reader_terminal_quarantine_reason::presented_invalid);
					static_assert(std::is_nothrow_move_constructible_v<
								  sqlite_shm_verified_reader_attachment_post_map_receipt>);
					map_attempt->receipt.emplace(std::move(prepared_terminal_receipt));
					map_attempt->quarantine_reason =
						sqlite_shm_reader_terminal_quarantine_reason::internal_failure;
					const auto reservation =
						find_by_token(reader_attachment_groups_, owner->group_token);
					if (reservation == reader_attachment_groups_.end() ||
						reservation->expected != owner->request.attachment || !generation_ ||
						generation_->value != map_attempt->generation ||
						generation_->sealed_shm_size !=
							map_attempt->expected_mapping.sealed_shm_size ||
						generation_->pages.size() != map_attempt->mapping_page_count)
						return quarantine_terminal(
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
					const auto expected_predelegate_authority =
						registry_route && map_attempt->retirement_blocker;
					const auto exact_terminal_bindings = !registry_route ||
						(reservation->registry_bound && reservation->registry_activity_authority &&
						 reservation->registry_activity_authority
							 ->retains_exact_owned_drain_lifetimes(owner->request.attachment) &&
						 map_attempt->registry_predelegate_authority.has_value() ==
							 expected_predelegate_authority &&
						 (!map_attempt->registry_predelegate_authority ||
						  map_attempt->registry_predelegate_authority
							  ->retains_exact_owned_terminal_lifetimes(*registry_family,
																	   map_attempt->request)));
					if (!exact_terminal_bindings)
					{
						map_attempt->quarantine_reason =
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure;
						quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																	 owner->token);
						inflight.disarm();
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					if (registry_route &&
						owner->phase == reader_session_record_phase::reserved_for_first_map)
					{
						const auto has_close_cut = reservation->composite_close_owner_token != 0U ||
							reservation->composite_close_registry_open_token != 0U ||
							reservation->composite_close_cut_sequence != 0U ||
							reservation->composite_close_wait_resolution_sequence_slot != 0U ||
							reservation->composite_close_wait_resolution_sequence != 0U;
						if (has_close_cut)
						{
							const auto open = std::find_if(
								registry_reader_opens_.begin(),
								registry_reader_opens_.end(),
								[&reservation](const registry_reader_open_record& candidate)
								{
									return candidate.token ==
										reservation->expected.registry_open_token();
								});
							if (open == registry_reader_opens_.end() ||
								!reader_phase1_close_cut_is_exact_locked(*reservation, *open))
								return quarantine_terminal(
									sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
							map_attempt->unpublished_cleanup_required = true;
							return sqlite_shm_unexpected(rejection(
								sqlite_shm_lease_rejection_reason::unpublished_cleanup_required,
								sqlite_shm_lease_recovery_action::
									attempt_nonremoving_unmap_then_outer_ioerr));
						}
					}
					if (is_quarantined_locked())
					{
						map_attempt->quarantine_reason =
							sqlite_shm_reader_terminal_quarantine_reason::peer_quarantine;
						if (owner->phase == reader_session_record_phase::active_group_owner)
						{
							quarantine_reader_map_attempt_only_locked(
								*map_attempt,
								sqlite_shm_reader_terminal_quarantine_reason::peer_quarantine);
							inflight.disarm();
						}
						else
						{
							quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																		 owner->token);
							inflight.disarm();
							session.disarm();
						}
						return sqlite_shm_unexpected(ambiguous());
					}
					if (registry_route)
					{
						if (map_attempt->retirement_blocker !=
								map_attempt->registry_predelegate_authority.has_value() ||
							(map_attempt->registry_predelegate_authority &&
							 (!map_attempt->registry_predelegate_authority->valid_for_predelegation(
								  map_attempt->request) ||
							  !map_attempt->registry_predelegate_authority
								   ->validate_active_authority(*registry_family,
															   map_attempt->request))))
							return quarantine_terminal(
								sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						if (owner->phase == reader_session_record_phase::reserved_for_first_map)
						{
							if (!reservation->registry_activity_authority ||
								!reservation->registry_activity_authority
									 ->validate_active_authority(*registry_family,
																 owner->request.attachment))
								return quarantine_terminal(
									sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						}
						else
						{
							const auto bound_group =
								find_by_token(reader_attachment_groups_, owner->group_token);
							if (bound_group == reader_attachment_groups_.end() ||
								!bound_group->registry_bound ||
								!bound_group->registry_activity_authority ||
								!bound_group->registry_activity_authority
									 ->validate_active_authority(*registry_family,
																 bound_group->expected))
								return quarantine_terminal(
									sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						}
					}
					const auto attempt_token = map_attempt->token;
					const auto generation = map_attempt->generation;
					auto mapping = receipt.mapping();
					if (owner->phase == reader_session_record_phase::active_group_owner)
					{
						const auto group = reservation;
						const auto has_composite_metadata =
							group != reader_attachment_groups_.end() &&
							(group->composite_close_owner_token != 0U ||
							 group->composite_close_registry_open_token != 0U ||
							 group->composite_close_cut_sequence != 0U);
						const auto composite_close = group != reader_attachment_groups_.end() &&
							reader_live_close_composite_is_exact_locked(*group);
						if (group == reader_attachment_groups_.end() ||
							group->reservation_phase !=
								sqlite_shm_reader_attachment_reservation_phase::observed_present ||
							(group->phase != reader_attachment_group_phase::active &&
							 !composite_close) ||
							(has_composite_metadata && !composite_close) ||
							group->expected != owner->request.attachment ||
							!group->observed_identity ||
							*group->observed_identity != receipt.observed_attachment() ||
							map_attempt->group_token != group->token)
							return quarantine_terminal(
								sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						if (composite_close)
						{
							if (!reader_group_custody_census_is_exact_locked(
									*group, true, true, true))
								return quarantine_terminal(
									sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
							auto next_audits = group->audits;
							std::vector<std::uint64_t> next_session_audits;
							auto next_custodies = reader_custodies_;
							const auto attempt_custody = std::find_if(
								next_custodies.begin(),
								next_custodies.end(),
								[attempt_token](const reader_custody_record& custody)
								{
									return custody.kind ==
										sqlite_shm_reader_custody_kind::map_attempt &&
										custody.owner_token == attempt_token &&
										custody.state == sqlite_shm_reader_custody_state::live;
								});
							if (attempt_custody == next_custodies.end() || !map_attempt->receipt)
								return quarantine_terminal(
									sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
							next_audits.push_back(
								{attempt_token,
								 owner->token,
								 sqlite_shm_reader_map_commit_kind::suppressed_after_cut,
								 *map_attempt->receipt,
								 map_attempt->admission_sequence,
								 0U});
							next_session_audits.reserve(next_audits.size());
							for (const auto& audit : next_audits)
								next_session_audits.push_back(audit.map_attempt_token);
							const auto sequences = consume_reader_lifecycle_terminal_slot_locked(
								map_attempt->terminal_sequence_slot);
							if (!sequences.succeeded)
								throw reader_map_terminal_commit_injected_failure{};
							map_attempt->terminal_sequence_slot = 0U;
							map_attempt->terminal_sequence = sequences.first;
							next_audits.back().terminal_sequence = sequences.first;
							attempt_custody->state = sqlite_shm_reader_custody_state::
								consumed_with_exact_terminal_receipt;
							attempt_custody->destination_sequence = sequences.first;
							if (std::exchange(fail_next_reader_map_terminal_commit_for_testing_,
											  false))
							{
								map_attempt->quarantine_reason =
									sqlite_shm_reader_terminal_quarantine_reason::
										injected_commit_failure;
								throw reader_map_terminal_commit_injected_failure{};
							}
							if (map_attempt->registry_predelegate_authority)
							{
								static_assert(std::is_nothrow_move_constructible_v<
											  sqlite_shm_reader_map_predelegate_authority>);
								completed_predelegate->emplace(
									std::move(*map_attempt->registry_predelegate_authority));
								map_attempt->registry_predelegate_authority.reset();
							}
							group->audits.swap(next_audits);
							owner->captured_audits.swap(next_session_audits);
							if (!group->existing_group_deferred_cleanup_required)
							{
								group->existing_group_deferred_cleanup_required = true;
								group->existing_group_deferred_cleanup_sequence = sequences.first;
							}
							reader_custodies_.swap(next_custodies);
							reader_attachment_maps_.erase(map_attempt);
							inflight.disarm();
							reader_last_committed_sequence_ =
								std::max(reader_last_committed_sequence_, sequences.last);
							return sqlite_shm_unexpected(
								rejection(sqlite_shm_lease_rejection_reason::retiring,
										  sqlite_shm_lease_recovery_action::outer_ioerr_no_retry));
						}
						const auto member = std::find_if(
							group->members.begin(),
							group->members.end(),
							[&mapping](const reader_attachment_group_member_record& value)
							{
								return value.identity.mapping().page_number == mapping.page_number;
							});
						auto kind = sqlite_shm_reader_map_commit_kind::new_member;
						if (member != group->members.end())
						{
							if (member->identity.mapping() != mapping)
								return quarantine_terminal(
									sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
							kind = sqlite_shm_reader_map_commit_kind::existing_member_revalidation;
						}
						auto cached_member = member != group->members.end()
							? member->identity
							: sqlite_shm_reader_cached_member_identity{
								  group->expected,
								  sqlite_shm_lease_token_identity{group->token},
								  sqlite_shm_mapping_generation_identity{generation},
								  sqlite_shm_lease_token_identity{attempt_token},
								  mapping};
						auto next_members = group->members;
						auto next_audits = group->audits;
						if (member == group->members.end())
						{
							if (generation_->phase != sqlite_shm_mapping_generation_phase::live &&
								generation_->phase != sqlite_shm_mapping_generation_phase::retiring)
								return quarantine_terminal(
									sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
							next_members.push_back({cached_member});
						}
						next_audits.push_back({attempt_token,
											   owner->token,
											   kind,
											   receipt,
											   map_attempt->admission_sequence,
											   0U});
						std::vector<sqlite_shm_reader_cached_member_identity> next_session_members;
						next_session_members.reserve(next_members.size());
						for (const auto& next_member : next_members)
							next_session_members.push_back(next_member.identity);
						std::vector<std::uint64_t> next_session_audits;
						next_session_audits.reserve(next_audits.size());
						for (const auto& audit : next_audits)
							next_session_audits.push_back(audit.map_attempt_token);
						auto next_custodies = reader_custodies_;
						const auto attempt_custody = std::find_if(
							next_custodies.begin(),
							next_custodies.end(),
							[attempt_token](const reader_custody_record& custody)
							{
								return custody.kind ==
									sqlite_shm_reader_custody_kind::map_attempt &&
									custody.owner_token == attempt_token &&
									custody.state == sqlite_shm_reader_custody_state::live;
							});
						if (attempt_custody == next_custodies.end())
							return quarantine_terminal(
								sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						if (registry_route &&
							(!group->registry_activity_authority ||
							 !group->registry_activity_authority->validate_active_authority(
								 *registry_family, group->expected) ||
							 (map_attempt->registry_predelegate_authority &&
							  !map_attempt->registry_predelegate_authority
								   ->validate_active_authority(*registry_family,
															   map_attempt->request))))
							return quarantine_terminal(
								sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						const auto sequences = consume_reader_lifecycle_terminal_slot_locked(
							map_attempt->terminal_sequence_slot);
						if (!sequences.succeeded)
							return quarantine_terminal(
								sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						map_attempt->terminal_sequence_slot = 0U;
						map_attempt->terminal_sequence = sequences.first;
						next_audits.back().terminal_sequence = sequences.first;
						attempt_custody->state =
							sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt;
						attempt_custody->destination_sequence = sequences.first;
						if (std::exchange(fail_next_reader_map_terminal_commit_for_testing_, false))
						{
							map_attempt->quarantine_reason =
								sqlite_shm_reader_terminal_quarantine_reason::
									injected_commit_failure;
							throw reader_map_terminal_commit_injected_failure{};
						}
						group->members.swap(next_members);
						group->audits.swap(next_audits);
						owner->captured_members.swap(next_session_members);
						owner->captured_audits.swap(next_session_audits);
						reader_custodies_.swap(next_custodies);
						if (map_attempt->registry_predelegate_authority)
						{
							static_assert(std::is_nothrow_move_constructible_v<
										  sqlite_shm_reader_map_predelegate_authority>);
							completed_predelegate->emplace(
								std::move(*map_attempt->registry_predelegate_authority));
							map_attempt->registry_predelegate_authority.reset();
						}
						reader_attachment_maps_.erase(map_attempt);
						inflight.disarm();
						reader_last_committed_sequence_ =
							std::max(reader_last_committed_sequence_, sequences.last);
						return sqlite_shm_reader_map_commit{
							kind, mapping, std::move(cached_member), std::nullopt};
					}

					if (owner->phase != reader_session_record_phase::reserved_for_first_map ||
						map_attempt->group_token != owner->group_token ||
						reservation->reservation_phase !=
							sqlite_shm_reader_attachment_reservation_phase::reserved ||
						reservation->observed_identity ||
						std::ranges::any_of(
							reader_attachment_groups_,
							[&owner, &reservation](const reader_attachment_group_record& group)
							{
								return &group != &*reservation &&
									group.reservation_phase ==
									sqlite_shm_reader_attachment_reservation_phase::
										observed_present &&
									group.expected == owner->request.attachment;
							}) ||
						(generation_->phase != sqlite_shm_mapping_generation_phase::live &&
						 generation_->phase != sqlite_shm_mapping_generation_phase::retiring))
						return quarantine_terminal(
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
					auto observed_identity = receipt.observed_attachment();
					auto cached_member = sqlite_shm_reader_cached_member_identity{
						owner->request.attachment,
						sqlite_shm_lease_token_identity{reservation->token},
						sqlite_shm_mapping_generation_identity{generation},
						sqlite_shm_lease_token_identity{attempt_token},
						mapping};
					std::vector<reader_attachment_group_member_record> next_members{
						{cached_member}};
					std::vector<reader_attachment_group_audit_record> next_audits{
						{attempt_token,
						 owner->token,
						 sqlite_shm_reader_map_commit_kind::first_member,
						 receipt,
						 map_attempt->admission_sequence,
						 0U}};
					std::vector<sqlite_shm_reader_cached_member_identity> captured_members{
						cached_member};
					std::vector<std::uint64_t> captured_audits{attempt_token};
					auto next_custodies = reader_custodies_;
					const auto attempt_custody = std::find_if(
						next_custodies.begin(),
						next_custodies.end(),
						[attempt_token](const reader_custody_record& custody)
						{
							return custody.kind == sqlite_shm_reader_custody_kind::map_attempt &&
								custody.owner_token == attempt_token &&
								custody.state == sqlite_shm_reader_custody_state::live;
						});
					const auto session_custody = std::find_if(
						next_custodies.begin(),
						next_custodies.end(),
						[&owner](const reader_custody_record& custody)
						{
							return custody.kind ==
								sqlite_shm_reader_custody_kind::use_session_reservation &&
								custody.owner_token == owner->token &&
								custody.state == sqlite_shm_reader_custody_state::live;
						});
					if (attempt_custody == next_custodies.end() ||
						session_custody == next_custodies.end())
						return quarantine_terminal(
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
					const auto attempt_custody_index = static_cast<std::size_t>(
						std::distance(next_custodies.begin(), attempt_custody));
					const auto session_custody_index = static_cast<std::size_t>(
						std::distance(next_custodies.begin(), session_custody));
					next_custodies.push_back(
						{sqlite_shm_reader_custody_kind::attachment_group_handoff,
						 sqlite_shm_reader_custody_state::live,
						 owner->request.attachment,
						 reservation->token,
						 0U,
						 0U});
					next_custodies.push_back(
						{sqlite_shm_reader_custody_kind::generation_group_count,
						 sqlite_shm_reader_custody_state::live,
						 owner->request.attachment,
						 reservation->token,
						 0U,
						 0U});
					next_custodies.push_back({sqlite_shm_reader_custody_kind::use_session,
											  sqlite_shm_reader_custody_state::live,
											  owner->request.attachment,
											  owner->token,
											  0U,
											  0U});
					next_custodies.push_back(
						{sqlite_shm_reader_custody_kind::exact_present_attachment,
						 sqlite_shm_reader_custody_state::live,
						 owner->request.attachment,
						 reservation->token,
						 0U,
						 0U});
					if (registry_route &&
						(!reservation->registry_activity_authority ||
						 !reservation->registry_activity_authority->validate_active_authority(
							 *registry_family, owner->request.attachment) ||
						 !map_attempt->registry_predelegate_authority ||
						 !map_attempt->registry_predelegate_authority->validate_active_authority(
							 *registry_family, map_attempt->request)))
						return quarantine_terminal(
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
					const auto injected_map_terminal_commit =
						std::exchange(fail_next_reader_map_terminal_commit_for_testing_, false);
					if (injected_map_terminal_commit)
					{
						map_attempt->quarantine_reason =
							sqlite_shm_reader_terminal_quarantine_reason::injected_commit_failure;
						if (registry_route)
						{
							map_attempt->unpublished_cleanup_required = true;
							return sqlite_shm_unexpected(rejection(
								sqlite_shm_lease_rejection_reason::unpublished_cleanup_required,
								sqlite_shm_lease_recovery_action::
									attempt_nonremoving_unmap_then_outer_ioerr));
						}
					}
					auto committed_state = shared_from_this();
					const auto sequences = consume_reader_lifecycle_terminal_slot_locked(
						map_attempt->terminal_sequence_slot);
					if (!sequences.succeeded)
						return quarantine_terminal(
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
					map_attempt->terminal_sequence_slot = 0U;
					map_attempt->terminal_sequence = sequences.first;
					reservation->unmap_cut_sequence_slot =
						std::exchange(map_attempt->potential_group_cut_sequence_slot, 0U);
					reservation->unmap_terminal_sequence_slot =
						std::exchange(map_attempt->potential_group_terminal_sequence_slot, 0U);
					next_audits.back().terminal_sequence = sequences.first;
					next_custodies[attempt_custody_index].state =
						sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt;
					next_custodies[attempt_custody_index].destination_sequence = sequences.first;
					next_custodies[session_custody_index].state =
						sqlite_shm_reader_custody_state::transferred_to_exact_successor;
					next_custodies[session_custody_index].destination_sequence = sequences.first;
					for (auto iterator = next_custodies.end() - 4; iterator != next_custodies.end();
						 ++iterator)
						iterator->origin_sequence = sequences.first;
					if (injected_map_terminal_commit)
						throw reader_map_terminal_commit_injected_failure{};
					static_assert(std::is_nothrow_move_constructible_v<
								  sqlite_shm_reader_native_attachment_identity>);
					reservation->observed_identity.emplace(std::move(observed_identity));
					reservation->members.swap(next_members);
					reservation->audits.swap(next_audits);
					reservation->reservation_phase =
						sqlite_shm_reader_attachment_reservation_phase::observed_present;
					reservation->reservation_destination_sequence = sequences.first;
					reservation->group_origin_sequence = sequences.first;
					reservation->phase = reader_attachment_group_phase::active;
					++generation_->handoff_count;
					owner->phase = reader_session_record_phase::active_group_owner;
					owner->captured_members.swap(captured_members);
					owner->captured_audits.swap(captured_audits);
					owner->lifecycle_phase =
						sqlite_shm_reader_session_reservation_phase::promoted_to_group_owner;
					owner->lifecycle_destination_sequence = sequences.first;
					reader_custodies_.swap(next_custodies);
					session.promote_to_active();
					if (map_attempt->registry_predelegate_authority)
					{
						completed_predelegate->emplace(
							std::move(*map_attempt->registry_predelegate_authority));
						map_attempt->registry_predelegate_authority.reset();
					}
					reader_attachment_maps_.erase(map_attempt);
					inflight.disarm();
					reader_last_committed_sequence_ =
						std::max(reader_last_committed_sequence_, sequences.last);
					return sqlite_shm_reader_map_commit{
						sqlite_shm_reader_map_commit_kind::first_member,
						mapping,
						std::move(cached_member),
						sqlite_shm_reader_handoff{
							std::move(committed_state),
							sqlite_shm_lease_token_identity{reservation->token},
							sqlite_shm_mapping_generation_identity{generation}}};
				}
				catch (...)
				{
					quarantine_reader_map_terminal_commit(inflight, session);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]]
			sqlite_shm_lease_result<sqlite_shm_reader_attachment_zero_effect_result>
			complete_reader_zero_attachment(
				sqlite_shm_reader_attachment_map_inflight& inflight,
				const sqlite_shm_verified_reader_attachment_zero_effect_receipt& receipt,
				sqlite_shm_reader_session& session,
				sqlite_shm_registry_family_pin* registry_family = nullptr,
				std::optional<sqlite_shm_reader_map_predelegate_authority>* completed_predelegate =
					nullptr,
				std::optional<sqlite_shm_reader_attachment_authority>* completed_candidate =
					nullptr)
			{
				if (emergency_quarantine_.load(std::memory_order_acquire) ||
					inflight.terminal_presentation_disabled_ ||
					session.terminal_presentation_disabled_)
				{
					if (owns(inflight.state_, inflight.token_))
						inflight.disable_terminal_presentation();
					if (owns(session.state_, session.token_))
						session.disable_terminal_presentation();
					return sqlite_shm_unexpected(ambiguous());
				}
				try
				{
					if (fail_next_reader_operation_mutex_acquire_for_testing_.exchange(
							false, std::memory_order_acq_rel))
						throw reader_operation_mutex_acquire_injected_failure{};
					std::scoped_lock lock{mutex_};
					if (!owns(inflight.state_, inflight.token_) ||
						!owns(session.state_, session.token_))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));

					const auto map_attempt =
						find_by_token(reader_attachment_maps_, inflight.token_);
					const auto owner = find_by_token(reader_sessions_, session.token_);
					if (map_attempt == reader_attachment_maps_.end() ||
						map_attempt->phase != reader_phase::inflight ||
						map_attempt->session_token != session.token_ ||
						owner == reader_sessions_.end() ||
						owner->phase == reader_session_record_phase::terminal_quarantined ||
						owner->generation != inflight.generation_ ||
						owner->generation != session.generation_ ||
						owner->request.attachment != map_attempt->request.expected_attachment)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));

					const auto registry_route = registry_family != nullptr &&
						completed_predelegate != nullptr && completed_candidate != nullptr;
					if (map_attempt->registry_bound != registry_route ||
						owner->registry_bound != registry_route ||
						(registry_route &&
						 (completed_predelegate->has_value() || completed_candidate->has_value())))
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::resubmit_via_bound_route));
					// Copy every fallible receipt field after route selection and before mutation.
					auto prepared_receipt = receipt;
					auto pending_receipt = receipt;
					const auto zero_effect_identity_reused =
						reader_map_effect_identity_seen_locked(
							prepared_receipt.zero_attachment_effect_receipt(),
							map_attempt->token) ||
						reader_callback_invocation_was_seen_locked(
							prepared_receipt.zero_attachment_effect_receipt()) ||
						reader_session_terminal_identity_seen_locked(
							prepared_receipt.zero_attachment_effect_receipt());
					if (zero_effect_identity_reused)
					{
						map_attempt->quarantine_reason =
							sqlite_shm_reader_terminal_quarantine_reason::presented_invalid;
						quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																	 owner->token);
						inflight.disarm();
						session.disarm();
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::quarantine_no_retry));
					}

					const auto receipt_state = prepared_receipt.state_.lock();
					if (!receipt_state || receipt_state.get() != this ||
						prepared_receipt.token_ != map_attempt->token ||
						prepared_receipt.request() != map_attempt->request ||
						!valid_reader_zero_attachment_receipt(prepared_receipt))
					{
						map_attempt->quarantine_reason =
							sqlite_shm_reader_terminal_quarantine_reason::presented_invalid;
						quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																	 owner->token);
						inflight.disarm();
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					static_assert(std::is_nothrow_move_constructible_v<
								  sqlite_shm_verified_reader_attachment_zero_effect_receipt>);
					map_attempt->zero_effect_receipt.emplace(std::move(pending_receipt));
					map_attempt->quarantine_reason =
						sqlite_shm_reader_terminal_quarantine_reason::internal_failure;
					if (!generation_ || generation_->value != map_attempt->generation ||
						generation_->sealed_shm_size !=
							map_attempt->expected_mapping.sealed_shm_size ||
						generation_->pages.size() != map_attempt->mapping_page_count)
					{
						quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																	 owner->token);
						inflight.disarm();
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}

					const auto first_reservation =
						owner->phase == reader_session_record_phase::reserved_for_first_map;
					auto group = find_by_token(reader_attachment_groups_, owner->group_token);
					if (group == reader_attachment_groups_.end() ||
						group->expected != owner->request.attachment)
					{
						quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																	 owner->token);
						inflight.disarm();
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					if (first_reservation)
					{
						if (map_attempt->group_token != group->token ||
							group->reservation_phase !=
								sqlite_shm_reader_attachment_reservation_phase::reserved ||
							group->observed_identity ||
							std::ranges::any_of(
								reader_attachment_groups_,
								[&owner, &group](const reader_attachment_group_record& candidate)
								{
									return &candidate != &*group &&
										candidate.reservation_phase ==
										sqlite_shm_reader_attachment_reservation_phase::
											observed_present &&
										candidate.expected == owner->request.attachment;
								}) ||
							(generation_->phase != sqlite_shm_mapping_generation_phase::live &&
							 generation_->phase != sqlite_shm_mapping_generation_phase::retiring &&
							 generation_->phase !=
								 sqlite_shm_mapping_generation_phase::quarantined))
						{
							quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																		 owner->token);
							inflight.disarm();
							session.disarm();
							return sqlite_shm_unexpected(ambiguous());
						}
					}
					else
					{
						if (owner->phase != reader_session_record_phase::active_group_owner)
						{
							quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																		 owner->token);
							inflight.disarm();
							session.disarm();
							return sqlite_shm_unexpected(ambiguous());
						}
						if (group->reservation_phase !=
								sqlite_shm_reader_attachment_reservation_phase::observed_present ||
							!group->observed_identity ||
							(group->phase != reader_attachment_group_phase::active &&
							 group->phase != reader_attachment_group_phase::unmap_cut_sealing) ||
							group->generation != owner->generation ||
							group->expected != owner->request.attachment ||
							map_attempt->group_token != group->token)
						{
							quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																		 owner->token);
							inflight.disarm();
							session.disarm();
							return sqlite_shm_unexpected(ambiguous());
						}
					}

					const auto expected_predelegate_authority =
						registry_route && map_attempt->retirement_blocker;
					if (group->registry_activity_authority.has_value() !=
							(registry_route &&
							 (first_reservation ||
							  group->reservation_phase ==
								  sqlite_shm_reader_attachment_reservation_phase::
									  observed_present)) ||
						map_attempt->registry_predelegate_authority.has_value() !=
							expected_predelegate_authority ||
						group->registry_bound != registry_route)
					{
						quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																	 owner->token);
						inflight.disarm();
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}

					if (registry_route)
					{
						if ((map_attempt->registry_predelegate_authority &&
							 !map_attempt->registry_predelegate_authority
								  ->retains_exact_owned_terminal_lifetimes(*registry_family,
																		   map_attempt->request)) ||
							(first_reservation &&
							 (!group->registry_activity_authority ||
							  !group->registry_activity_authority
								   ->retains_exact_owned_drain_lifetimes(
									   owner->request.attachment))) ||
							(!first_reservation &&
							 (!group->registry_bound || !group->registry_activity_authority ||
							  !group->registry_activity_authority
								   ->retains_exact_owned_drain_lifetimes(group->expected))))
						{
							quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																		 owner->token);
							inflight.disarm();
							session.disarm();
							return sqlite_shm_unexpected(ambiguous());
						}
					}

					const auto attempt_token = map_attempt->token;
					const auto generation = map_attempt->generation;
					const auto owner_token = owner->token;
					auto terminal_session_request = owner->request;
					const auto session_origin_sequence = owner->lifecycle_origin_sequence;
					const auto result_kind = prepared_receipt.kind();
					const auto result_status = result_kind ==
							sqlite_shm_reader_attachment_zero_effect_kind::
								exact_no_attachment_change
						? prepared_receipt.native_status()
						: sqlite_ioerr_status;
					auto next_custodies = reader_custodies_;
					const auto attempt_custody = std::find_if(
						next_custodies.begin(),
						next_custodies.end(),
						[attempt_token](const reader_custody_record& custody)
						{
							return custody.kind == sqlite_shm_reader_custody_kind::map_attempt &&
								custody.owner_token == attempt_token &&
								custody.state == sqlite_shm_reader_custody_state::live;
						});
					if (attempt_custody == next_custodies.end())
					{
						quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																	 owner->token);
						inflight.disarm();
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto attempt_custody_index = static_cast<std::size_t>(
						std::distance(next_custodies.begin(), attempt_custody));
					std::optional<std::size_t> session_custody_index;
					std::optional<std::size_t> runtime_custody_index;
					if (first_reservation)
					{
						const auto session_custody = std::find_if(
							next_custodies.begin(),
							next_custodies.end(),
							[owner_token](const reader_custody_record& custody)
							{
								return custody.kind ==
									sqlite_shm_reader_custody_kind::use_session_reservation &&
									custody.owner_token == owner_token &&
									custody.state == sqlite_shm_reader_custody_state::live;
							});
						if (session_custody == next_custodies.end())
						{
							quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																		 owner->token);
							inflight.disarm();
							session.disarm();
							return sqlite_shm_unexpected(ambiguous());
						}
						session_custody_index = static_cast<std::size_t>(
							std::distance(next_custodies.begin(), session_custody));
						if (registry_route)
						{
							const auto runtime_custody = std::find_if(
								next_custodies.begin(),
								next_custodies.end(),
								[&group](const reader_custody_record& custody)
								{
									return custody.kind ==
										sqlite_shm_reader_custody_kind::
											runtime_vfs_namespace_generation_native_mapping_lifetime_pin &&
										custody.owner_token == group->token &&
										custody.state == sqlite_shm_reader_custody_state::live;
								});
							if (runtime_custody == next_custodies.end())
							{
								quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																			 owner->token);
								inflight.disarm();
								session.disarm();
								return sqlite_shm_unexpected(ambiguous());
							}
							runtime_custody_index = static_cast<std::size_t>(
								std::distance(next_custodies.begin(), runtime_custody));
						}
					}
					std::list<reader_attachment_zero_effect_terminal_record> prepared_terminals;
					prepared_terminals.push_back({attempt_token,
												  generation,
												  owner_token,
												  first_reservation,
												  std::move(terminal_session_request),
												  std::move(prepared_receipt),
												  session_origin_sequence,
												  map_attempt->admission_sequence,
												  0U});
					const auto sequences = first_reservation
						? consume_reader_lifecycle_terminal_slots_locked(
							  map_attempt->terminal_sequence_slot, owner->terminal_sequence_slot)
						: consume_reader_lifecycle_terminal_slot_locked(
							  map_attempt->terminal_sequence_slot);
					if (!sequences.succeeded)
					{
						quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																	 owner->token);
						inflight.disarm();
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					map_attempt->terminal_sequence_slot = 0U;
					map_attempt->terminal_sequence = sequences.first;
					if (first_reservation)
					{
						owner->terminal_sequence_slot = 0U;
						owner->pending_terminal_sequence = sequences.first;
					}
					next_custodies[attempt_custody_index].state =
						sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt;
					next_custodies[attempt_custody_index].destination_sequence = sequences.first;
					if (session_custody_index)
					{
						next_custodies[*session_custody_index].state =
							sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt;
						next_custodies[*session_custody_index].destination_sequence =
							sequences.first;
					}
					if (runtime_custody_index)
					{
						next_custodies[*runtime_custody_index].state =
							sqlite_shm_reader_custody_state::transferred_to_durable_tombstone;
						next_custodies[*runtime_custody_index].destination_sequence =
							sequences.first;
					}
					prepared_terminals.back().terminal_sequence = sequences.first;
					reader_attachment_zero_effect_terminals_.splice(
						reader_attachment_zero_effect_terminals_.end(), prepared_terminals);
					if (std::exchange(fail_next_reader_map_terminal_commit_for_testing_, false))
					{
						map_attempt->quarantine_reason =
							sqlite_shm_reader_terminal_quarantine_reason::injected_commit_failure;
						throw reader_map_terminal_commit_injected_failure{};
					}

					if (first_reservation)
					{
						cancel_reader_lifecycle_terminal_slot_locked(
							map_attempt->potential_group_cut_sequence_slot);
						cancel_reader_lifecycle_terminal_slot_locked(
							map_attempt->potential_group_terminal_sequence_slot);
						map_attempt->potential_group_cut_sequence_slot = 0U;
						map_attempt->potential_group_terminal_sequence_slot = 0U;
					}

					if (map_attempt->registry_predelegate_authority)
					{
						static_assert(std::is_nothrow_move_constructible_v<
									  sqlite_shm_reader_map_predelegate_authority>);
						completed_predelegate->emplace(
							std::move(*map_attempt->registry_predelegate_authority));
						map_attempt->registry_predelegate_authority.reset();
					}
					if (first_reservation && group->registry_activity_authority)
					{
						static_assert(std::is_nothrow_move_constructible_v<
									  sqlite_shm_reader_attachment_authority>);
						completed_candidate->emplace(
							std::move(*group->registry_activity_authority));
						group->registry_activity_authority.reset();
					}

					reader_attachment_maps_.erase(map_attempt);
					inflight.disarm();
					if (first_reservation)
					{
						group->reservation_phase =
							sqlite_shm_reader_attachment_reservation_phase::revoked_no_map;
						group->reservation_destination_sequence = sequences.first;
						owner->lifecycle_phase =
							sqlite_shm_reader_session_reservation_phase::consumed_no_pointer;
						owner->lifecycle_destination_sequence = sequences.first;
						reader_sessions_.erase(owner);
						session.disarm();
					}
					reader_custodies_.swap(next_custodies);
					reader_last_committed_sequence_ =
						std::max(reader_last_committed_sequence_, sequences.last);
					return sqlite_shm_reader_attachment_zero_effect_result{result_kind,
																		   result_status};
				}
				catch (...)
				{
					quarantine_reader_map_terminal_commit(inflight, session);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<
				sqlite_shm_reader_opaque_attachment_uncertainty_result>
			complete_reader_opaque_attachment_uncertainty(
				sqlite_shm_reader_attachment_map_inflight& inflight,
				sqlite_shm_reader_session& session,
				sqlite_shm_registry_family_pin* registry_family = nullptr)
			{
				if (emergency_quarantine_.load(std::memory_order_acquire) ||
					inflight.terminal_presentation_disabled_ ||
					session.terminal_presentation_disabled_)
				{
					if (owns(inflight.state_, inflight.token_))
						inflight.disable_terminal_presentation();
					if (owns(session.state_, session.token_))
						session.disable_terminal_presentation();
					return sqlite_shm_unexpected(ambiguous());
				}
				try
				{
					if (fail_next_reader_operation_mutex_acquire_for_testing_.exchange(
							false, std::memory_order_acq_rel))
						throw reader_operation_mutex_acquire_injected_failure{};
					std::scoped_lock lock{mutex_};
					if (!owns(inflight.state_, inflight.token_) ||
						!owns(session.state_, session.token_))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));

					const auto map_attempt =
						find_by_token(reader_attachment_maps_, inflight.token_);
					const auto owner = find_by_token(reader_sessions_, session.token_);
					const auto first_reservation = owner != reader_sessions_.end() &&
						owner->phase == reader_session_record_phase::reserved_for_first_map;
					const auto existing_group = owner != reader_sessions_.end() &&
						owner->phase == reader_session_record_phase::active_group_owner;
					if (map_attempt == reader_attachment_maps_.end() ||
						map_attempt->phase != reader_phase::inflight ||
						map_attempt->session_token != session.token_ ||
						owner == reader_sessions_.end() ||
						(!first_reservation && !existing_group) ||
						owner->generation != inflight.generation_ ||
						owner->generation != session.generation_ ||
						owner->request.attachment != map_attempt->request.expected_attachment)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));

					const auto registry_route = registry_family != nullptr;
					if (map_attempt->registry_bound != registry_route ||
						owner->registry_bound != registry_route)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::quarantine_no_retry));

					auto reservation = find_by_token(reader_attachment_groups_, owner->group_token);
					if (reservation == reader_attachment_groups_.end() ||
						map_attempt->group_token != reservation->token ||
						reservation->expected != owner->request.attachment ||
						reservation->generation != owner->generation ||
						reservation->registry_bound != registry_route ||
						map_attempt->registry_predelegate_authority.has_value() !=
							(registry_route && map_attempt->retirement_blocker))
					{
						quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																	 owner->token);
						inflight.disarm();
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}

					if (existing_group)
					{
						const auto cut_sealing =
							reservation->phase == reader_attachment_group_phase::unmap_cut_sealing;
						const auto has_composite_metadata =
							reservation->composite_close_owner_token != 0U ||
							reservation->composite_close_registry_open_token != 0U ||
							reservation->composite_close_cut_sequence != 0U;
						const auto composite_close = cut_sealing &&
							reader_live_close_composite_is_exact_locked(*reservation);
						if (reservation->reservation_phase !=
								sqlite_shm_reader_attachment_reservation_phase::observed_present ||
							(!cut_sealing &&
							 reservation->phase != reader_attachment_group_phase::active) ||
							!reservation->observed_identity || reservation->members.empty() ||
							reservation->audits.empty() ||
							owner->lifecycle_phase !=
								sqlite_shm_reader_session_reservation_phase::
									promoted_to_group_owner ||
							map_attempt->potential_group_cut_sequence_slot != 0U ||
							map_attempt->potential_group_terminal_sequence_slot != 0U ||
							(has_composite_metadata && !composite_close) ||
							!reader_group_custody_census_is_exact_locked(
								*reservation, cut_sealing, composite_close, true) ||
							(registry_route &&
							 (!reservation->registry_activity_authority ||
							  !reservation->registry_activity_authority
								   ->retains_exact_owned_drain_lifetimes(reservation->expected) ||
							  (map_attempt->retirement_blocker &&
							   !map_attempt->registry_predelegate_authority
									->retains_exact_owned_terminal_lifetimes(
										*registry_family, map_attempt->request)))))
						{
							map_attempt->quarantine_reason =
								sqlite_shm_reader_terminal_quarantine_reason::internal_failure;
							quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																		 owner->token);
							inflight.disarm();
							session.disarm();
							return sqlite_shm_unexpected(ambiguous());
						}

						map_attempt->quarantine_reason =
							sqlite_shm_reader_terminal_quarantine_reason::native_non_ok_or_unknown;
						quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																	 owner->token);
						inflight.disarm();
						session.disarm();
						if (registry_route)
						{
							auto open = std::find_if(
								registry_reader_opens_.begin(),
								registry_reader_opens_.end(),
								[&reservation](const registry_reader_open_record& candidate)
								{
									return candidate.token ==
										reservation->expected.registry_open_token() &&
										reader_attachment_matches_open_epoch_binding(
											   reservation->expected, candidate.binding);
								});
							if (open == registry_reader_opens_.end())
							{
								emergency_quarantine_.store(true, std::memory_order_release);
								return sqlite_shm_unexpected(ambiguous());
							}
							quarantine_reader_open_locked(
								*open,
								reservation->reservation_destination_sequence,
								sqlite_shm_reader_terminal_quarantine_reason::
									native_non_ok_or_unknown);
						}
						return sqlite_shm_reader_opaque_attachment_uncertainty_result{};
					}

					if (!first_reservation ||
						reservation->reservation_phase !=
							sqlite_shm_reader_attachment_reservation_phase::reserved ||
						reservation->observed_identity || !reservation->members.empty() ||
						!reservation->audits.empty() ||
						reservation->registry_activity_authority.has_value() != registry_route)
					{
						quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																	 owner->token);
						inflight.disarm();
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					if (registry_route &&
						(!reservation->registry_activity_authority
							  ->retains_exact_owned_drain_lifetimes(reservation->expected) ||
						 (map_attempt->retirement_blocker &&
						  !map_attempt->registry_predelegate_authority
							   ->retains_exact_owned_terminal_lifetimes(*registry_family,
																		map_attempt->request))))
					{
						quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																	 owner->token);
						inflight.disarm();
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}

					const auto attempt_token = map_attempt->token;
					const auto session_token = owner->token;
					const auto reservation_token = reservation->token;
					auto next_custodies = reader_custodies_;
					const auto attempt_custody = std::find_if(
						next_custodies.begin(),
						next_custodies.end(),
						[attempt_token](const reader_custody_record& custody)
						{
							return custody.kind == sqlite_shm_reader_custody_kind::map_attempt &&
								custody.owner_token == attempt_token &&
								custody.state == sqlite_shm_reader_custody_state::live;
						});
					const auto session_custody = std::find_if(
						next_custodies.begin(),
						next_custodies.end(),
						[session_token](const reader_custody_record& custody)
						{
							return custody.kind ==
								sqlite_shm_reader_custody_kind::use_session_reservation &&
								custody.owner_token == session_token &&
								custody.state == sqlite_shm_reader_custody_state::live;
						});
					auto runtime_custody = next_custodies.end();
					if (registry_route)
						runtime_custody = std::find_if(
							next_custodies.begin(),
							next_custodies.end(),
							[reservation_token](const reader_custody_record& custody)
							{
								return custody.kind ==
									sqlite_shm_reader_custody_kind::
										runtime_vfs_namespace_generation_native_mapping_lifetime_pin &&
									custody.owner_token == reservation_token &&
									custody.state == sqlite_shm_reader_custody_state::live;
							});
					if (attempt_custody == next_custodies.end() ||
						session_custody == next_custodies.end() ||
						(registry_route && runtime_custody == next_custodies.end()))
					{
						quarantine_reader_map_terminal_commit_locked(attempt_token, session_token);
						inflight.disarm();
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto attempt_custody_index = static_cast<std::size_t>(
						std::distance(next_custodies.begin(), attempt_custody));
					const auto session_custody_index = static_cast<std::size_t>(
						std::distance(next_custodies.begin(), session_custody));
					const auto runtime_custody_index = registry_route
						? std::optional<std::size_t>{static_cast<std::size_t>(
							  std::distance(next_custodies.begin(), runtime_custody))}
						: std::nullopt;
					next_custodies.emplace_back(
						sqlite_shm_reader_custody_kind::opaque_attachment_uncertainty,
						sqlite_shm_reader_custody_state::live,
						reservation->expected,
						reservation_token,
						map_attempt->admission_sequence,
						0U);
					std::list<reader_opaque_attachment_uncertainty_record> prepared;
					prepared.push_back({attempt_token,
										owner->generation,
										session_token,
										reservation_token,
										map_attempt->request,
										owner->request,
										owner->lifecycle_origin_sequence,
										map_attempt->admission_sequence,
										0U,
										0U,
										registry_route,
										std::nullopt});

					const auto sequences = consume_reader_lifecycle_terminal_slots_locked(
						map_attempt->terminal_sequence_slot, owner->terminal_sequence_slot);
					if (!sequences.succeeded)
					{
						quarantine_reader_map_terminal_commit_locked(attempt_token, session_token);
						inflight.disarm();
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					map_attempt->terminal_sequence_slot = 0U;
					owner->terminal_sequence_slot = 0U;
					map_attempt->terminal_sequence = sequences.first;
					owner->pending_terminal_sequence = sequences.last;
					cancel_reader_lifecycle_terminal_slot_locked(
						map_attempt->potential_group_cut_sequence_slot);
					cancel_reader_lifecycle_terminal_slot_locked(
						map_attempt->potential_group_terminal_sequence_slot);
					map_attempt->potential_group_cut_sequence_slot = 0U;
					map_attempt->potential_group_terminal_sequence_slot = 0U;

					next_custodies[attempt_custody_index].state =
						sqlite_shm_reader_custody_state::transferred_to_durable_tombstone;
					next_custodies[attempt_custody_index].destination_sequence = sequences.first;
					next_custodies[session_custody_index].state =
						sqlite_shm_reader_custody_state::transferred_to_durable_tombstone;
					next_custodies[session_custody_index].destination_sequence = sequences.last;
					if (runtime_custody_index)
					{
						next_custodies[*runtime_custody_index].state =
							sqlite_shm_reader_custody_state::transferred_to_durable_tombstone;
						next_custodies[*runtime_custody_index].destination_sequence =
							sequences.last;
					}
					next_custodies.back().state =
						sqlite_shm_reader_custody_state::transferred_to_durable_tombstone;
					next_custodies.back().destination_sequence = sequences.last;
					prepared.back().map_terminal_sequence = sequences.first;
					prepared.back().terminal_sequence = sequences.last;
					if (map_attempt->registry_predelegate_authority)
					{
						static_assert(std::is_nothrow_move_constructible_v<
									  sqlite_shm_reader_map_predelegate_authority>);
						prepared.back().registry_predelegate_authority.emplace(
							std::move(*map_attempt->registry_predelegate_authority));
						map_attempt->registry_predelegate_authority.reset();
					}

					reservation->reservation_phase =
						sqlite_shm_reader_attachment_reservation_phase::terminal_quarantined;
					reservation->reservation_destination_sequence = sequences.last;
					reservation->phase = reader_attachment_group_phase::terminal_quarantined;
					reservation->quarantine_reason =
						sqlite_shm_reader_terminal_quarantine_reason::native_non_ok_or_unknown;
					reader_attachment_maps_.erase(map_attempt);
					reader_sessions_.erase(owner);
					reader_custodies_.swap(next_custodies);
					reader_opaque_attachment_uncertainties_.splice(
						reader_opaque_attachment_uncertainties_.end(), prepared);
					inflight.disarm();
					session.disarm();

					if (registry_route)
					{
						quarantine_reader_group_locked(
							*reservation,
							sequences.last,
							sqlite_shm_reader_terminal_quarantine_reason::native_non_ok_or_unknown);
						const auto open = std::find_if(
							registry_reader_opens_.begin(),
							registry_reader_opens_.end(),
							[&reservation](const registry_reader_open_record& candidate)
							{
								return candidate.token ==
									reservation->expected.registry_open_token() &&
									reader_attachment_matches_open_epoch_binding(
										   reservation->expected, candidate.binding);
							});
						if (open == registry_reader_opens_.end())
							emergency_quarantine_.store(true, std::memory_order_release);
						else
							quarantine_reader_open_locked(
								*open,
								sequences.last,
								sqlite_shm_reader_terminal_quarantine_reason::
									native_non_ok_or_unknown);
					}
					quarantine_locked();
					reader_last_committed_sequence_ =
						std::max(reader_last_committed_sequence_, sequences.last);
					return sqlite_shm_reader_opaque_attachment_uncertainty_result{};
				}
				catch (...)
				{
					quarantine_reader_map_terminal_commit(inflight, session);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_predecessor_map_result>
			complete_reader_predecessor_map(
				sqlite_shm_reader_attachment_map_inflight& inflight,
				const sqlite_shm_verified_reader_predecessor_map_receipt& receipt,
				sqlite_shm_reader_session& session,
				sqlite_shm_registry_family_pin* registry_family = nullptr,
				std::optional<sqlite_shm_reader_map_predelegate_authority>* completed_predelegate =
					nullptr,
				std::optional<sqlite_shm_reader_attachment_authority>* completed_candidate =
					nullptr)
			{
				if (emergency_quarantine_.load(std::memory_order_acquire) ||
					inflight.terminal_presentation_disabled_ ||
					session.terminal_presentation_disabled_)
				{
					if (owns(inflight.state_, inflight.token_))
						inflight.disable_terminal_presentation();
					if (owns(session.state_, session.token_))
						session.disable_terminal_presentation();
					return sqlite_shm_unexpected(ambiguous());
				}
				try
				{
					if (fail_next_reader_operation_mutex_acquire_for_testing_.exchange(
							false, std::memory_order_acq_rel))
						throw reader_operation_mutex_acquire_injected_failure{};
					std::scoped_lock lock{mutex_};
					if (!owns(inflight.state_, inflight.token_) ||
						!owns(session.state_, session.token_))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));

					const auto map_attempt =
						find_by_token(reader_attachment_maps_, inflight.token_);
					const auto owner = find_by_token(reader_sessions_, session.token_);
					if (map_attempt == reader_attachment_maps_.end() ||
						map_attempt->phase != reader_phase::inflight ||
						map_attempt->session_token != session.token_ ||
						owner == reader_sessions_.end() ||
						owner->phase != reader_session_record_phase::reserved_for_first_map ||
						owner->generation != inflight.generation_ ||
						owner->generation != session.generation_ ||
						owner->request.attachment != map_attempt->request.expected_attachment)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));

					const auto registry_route = registry_family != nullptr &&
						completed_predelegate != nullptr && completed_candidate != nullptr;
					if (map_attempt->registry_bound != registry_route ||
						owner->registry_bound != registry_route ||
						(registry_route &&
						 (completed_predelegate->has_value() || completed_candidate->has_value())))
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::resubmit_via_bound_route));

					const auto& prepared_receipt = receipt;
					auto terminal_receipt = receipt;
					const auto effect_identity_reused =
						reader_map_effect_identity_seen_locked(
							prepared_receipt.native_effect_receipt(), map_attempt->token) ||
						reader_callback_invocation_was_seen_locked(
							prepared_receipt.native_effect_receipt()) ||
						reader_session_terminal_identity_seen_locked(
							prepared_receipt.native_effect_receipt());
					const auto receipt_state = prepared_receipt.state_.lock();
					if (effect_identity_reused || !receipt_state || receipt_state.get() != this ||
						prepared_receipt.token_ != map_attempt->token ||
						prepared_receipt.request() != map_attempt->request ||
						!valid_reader_predecessor_map_receipt(prepared_receipt))
					{
						map_attempt->quarantine_reason =
							sqlite_shm_reader_terminal_quarantine_reason::presented_invalid;
						quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																	 owner->token);
						inflight.disarm();
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					map_attempt->quarantine_reason =
						sqlite_shm_reader_terminal_quarantine_reason::internal_failure;
					if (!generation_ || generation_->value != map_attempt->generation ||
						generation_->sealed_shm_size !=
							map_attempt->expected_mapping.sealed_shm_size ||
						generation_->pages.size() != map_attempt->mapping_page_count)
					{
						quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																	 owner->token);
						inflight.disarm();
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}

					auto reservation = find_by_token(reader_attachment_groups_, owner->group_token);
					if (reservation == reader_attachment_groups_.end() ||
						map_attempt->group_token != reservation->token ||
						reservation->expected != owner->request.attachment ||
						reservation->generation != owner->generation ||
						reservation->reservation_phase !=
							sqlite_shm_reader_attachment_reservation_phase::reserved ||
						reservation->observed_identity || !reservation->members.empty() ||
						!reservation->audits.empty() ||
						reservation->registry_bound != registry_route ||
						reservation->registry_activity_authority.has_value() != registry_route ||
						map_attempt->registry_predelegate_authority.has_value() != registry_route)
					{
						quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																	 owner->token);
						inflight.disarm();
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					if (registry_route &&
						(!map_attempt->registry_predelegate_authority
							  ->retains_exact_owned_terminal_lifetimes(*registry_family,
																	   map_attempt->request) ||
						 !reservation->registry_activity_authority
							  ->retains_exact_owned_drain_lifetimes(reservation->expected)))
					{
						quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																	 owner->token);
						inflight.disarm();
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}

					const auto attempt_token = map_attempt->token;
					const auto owner_token = owner->token;
					const auto generation = owner->generation;
					const auto session_origin_sequence = owner->lifecycle_origin_sequence;
					auto terminal_session_request = owner->request;
					const auto result_kind = prepared_receipt.kind();
					const auto result_status = result_kind ==
							sqlite_shm_reader_predecessor_map_kind::
								exact_predecessor_no_attachment_route
						? sqlite_readonly_cantinit_status
						: sqlite_readonly_status;
					const auto* result_mapping = prepared_receipt.native_mapping();

					auto next_custodies = reader_custodies_;
					const auto attempt_custody = std::find_if(
						next_custodies.begin(),
						next_custodies.end(),
						[attempt_token](const reader_custody_record& custody)
						{
							return custody.kind == sqlite_shm_reader_custody_kind::map_attempt &&
								custody.owner_token == attempt_token &&
								custody.state == sqlite_shm_reader_custody_state::live;
						});
					const auto session_custody = std::find_if(
						next_custodies.begin(),
						next_custodies.end(),
						[owner_token](const reader_custody_record& custody)
						{
							return custody.kind ==
								sqlite_shm_reader_custody_kind::use_session_reservation &&
								custody.owner_token == owner_token &&
								custody.state == sqlite_shm_reader_custody_state::live;
						});
					if (attempt_custody == next_custodies.end() ||
						session_custody == next_custodies.end())
					{
						quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																	 owner->token);
						inflight.disarm();
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto attempt_custody_index = static_cast<std::size_t>(
						std::distance(next_custodies.begin(), attempt_custody));
					const auto session_custody_index = static_cast<std::size_t>(
						std::distance(next_custodies.begin(), session_custody));

					std::list<reader_predecessor_map_terminal_record> prepared_terminals;
					prepared_terminals.push_back({attempt_token,
												  generation,
												  owner_token,
												  reservation->token,
												  std::move(terminal_session_request),
												  std::move(terminal_receipt),
												  session_origin_sequence,
												  map_attempt->admission_sequence,
												  0U});
					const auto sequences = consume_reader_lifecycle_terminal_slots_locked(
						map_attempt->terminal_sequence_slot, owner->terminal_sequence_slot);
					if (!sequences.succeeded)
					{
						quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																	 owner->token);
						inflight.disarm();
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					map_attempt->terminal_sequence_slot = 0U;
					map_attempt->terminal_sequence = sequences.first;
					owner->terminal_sequence_slot = 0U;
					owner->pending_terminal_sequence = sequences.first;
					prepared_terminals.back().terminal_sequence = sequences.first;
					next_custodies[attempt_custody_index].state =
						sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt;
					next_custodies[attempt_custody_index].destination_sequence = sequences.first;
					next_custodies[session_custody_index].state =
						sqlite_shm_reader_custody_state::transferred_to_exact_successor;
					next_custodies[session_custody_index].destination_sequence = sequences.first;
					if (std::exchange(fail_next_reader_map_terminal_commit_for_testing_, false))
					{
						map_attempt->quarantine_reason =
							sqlite_shm_reader_terminal_quarantine_reason::injected_commit_failure;
						throw reader_map_terminal_commit_injected_failure{};
					}

					cancel_reader_lifecycle_terminal_slot_locked(
						map_attempt->potential_group_cut_sequence_slot);
					cancel_reader_lifecycle_terminal_slot_locked(
						map_attempt->potential_group_terminal_sequence_slot);
					map_attempt->potential_group_cut_sequence_slot = 0U;
					map_attempt->potential_group_terminal_sequence_slot = 0U;
					if (map_attempt->registry_predelegate_authority)
					{
						static_assert(std::is_nothrow_move_constructible_v<
									  sqlite_shm_reader_map_predelegate_authority>);
						completed_predelegate->emplace(
							std::move(*map_attempt->registry_predelegate_authority));
						map_attempt->registry_predelegate_authority.reset();
					}

					reservation->reservation_phase =
						sqlite_shm_reader_attachment_reservation_phase::predecessor_route_active;
					reservation->reservation_destination_sequence = sequences.first;
					owner->lifecycle_phase = sqlite_shm_reader_session_reservation_phase::
						transferred_to_existing_predecessor;
					owner->lifecycle_destination_sequence = sequences.first;
					reader_predecessor_map_terminals_.splice(
						reader_predecessor_map_terminals_.end(), prepared_terminals);
					reader_attachment_maps_.erase(map_attempt);
					reader_sessions_.erase(owner);
					reader_custodies_.swap(next_custodies);
					inflight.disarm();
					session.disarm();
					reader_last_committed_sequence_ =
						std::max(reader_last_committed_sequence_, sequences.last);
					return sqlite_shm_reader_predecessor_map_result{weak_from_this(),
																	attempt_token,
																	reservation->token,
																	generation,
																	result_kind,
																	result_status,
																	result_mapping};
				}
				catch (...)
				{
					quarantine_reader_map_terminal_commit(inflight, session);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<
				sqlite_shm_reader_existing_group_predecessor_mismatch_result>
			complete_reader_existing_group_predecessor_mismatch(
				sqlite_shm_reader_attachment_map_inflight& inflight,
				const sqlite_shm_verified_reader_predecessor_map_receipt& receipt,
				sqlite_shm_reader_session& session,
				sqlite_shm_registry_family_pin* registry_family = nullptr,
				std::optional<sqlite_shm_reader_map_predelegate_authority>* completed_predelegate =
					nullptr)
			{
				if (emergency_quarantine_.load(std::memory_order_acquire) ||
					inflight.terminal_presentation_disabled_ ||
					session.terminal_presentation_disabled_)
				{
					if (owns(inflight.state_, inflight.token_))
						inflight.disable_terminal_presentation();
					if (owns(session.state_, session.token_))
						session.disable_terminal_presentation();
					return sqlite_shm_unexpected(ambiguous());
				}
				try
				{
					if (fail_next_reader_operation_mutex_acquire_for_testing_.exchange(
							false, std::memory_order_acq_rel))
						throw reader_operation_mutex_acquire_injected_failure{};
					std::scoped_lock lock{mutex_};
					if (!owns(inflight.state_, inflight.token_) ||
						!owns(session.state_, session.token_))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));

					const auto map_attempt =
						find_by_token(reader_attachment_maps_, inflight.token_);
					const auto owner = find_by_token(reader_sessions_, session.token_);
					if (map_attempt == reader_attachment_maps_.end() ||
						map_attempt->phase != reader_phase::inflight ||
						map_attempt->session_token != session.token_ ||
						owner == reader_sessions_.end() ||
						owner->phase != reader_session_record_phase::active_group_owner ||
						owner->generation != inflight.generation_ ||
						owner->generation != session.generation_)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));

					const auto registry_route =
						registry_family != nullptr && completed_predelegate != nullptr;
					if ((registry_family != nullptr || completed_predelegate != nullptr) !=
							registry_route ||
						map_attempt->registry_bound != registry_route ||
						owner->registry_bound != registry_route ||
						(registry_route && completed_predelegate->has_value()))
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::resubmit_via_bound_route));

					const auto receipt_state = receipt.state_.lock();
					const auto effect_reused =
						reader_map_effect_identity_seen_locked(receipt.native_effect_receipt(),
															   map_attempt->token) ||
						reader_callback_invocation_was_seen_locked(
							receipt.native_effect_receipt()) ||
						reader_session_terminal_identity_seen_locked(
							receipt.native_effect_receipt());
					if (!receipt_state || receipt_state.get() != this ||
						receipt.token_ != map_attempt->token ||
						receipt.kind() !=
							sqlite_shm_reader_predecessor_map_kind::
								exact_predecessor_mapped_route ||
						receipt.request() != map_attempt->request ||
						!valid_reader_predecessor_map_receipt(receipt) || effect_reused)
					{
						map_attempt->quarantine_reason =
							sqlite_shm_reader_terminal_quarantine_reason::presented_invalid;
						quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																	 owner->token);
						inflight.disarm();
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}

					const auto group = find_by_token(reader_attachment_groups_, owner->group_token);
					const auto cut_sealing = group != reader_attachment_groups_.end() &&
						group->phase == reader_attachment_group_phase::unmap_cut_sealing;
					const auto has_composite_metadata = group != reader_attachment_groups_.end() &&
						(group->composite_close_owner_token != 0U ||
						 group->composite_close_registry_open_token != 0U ||
						 group->composite_close_cut_sequence != 0U);
					const auto composite_close = group != reader_attachment_groups_.end() &&
						cut_sealing && reader_live_close_composite_is_exact_locked(*group);
					if (!generation_ || generation_->value != owner->generation ||
						group == reader_attachment_groups_.end() ||
						map_attempt->group_token != group->token ||
						group->generation != owner->generation ||
						group->expected != owner->request.attachment ||
						group->reservation_phase !=
							sqlite_shm_reader_attachment_reservation_phase::observed_present ||
						(group->phase != reader_attachment_group_phase::active && !cut_sealing) ||
						!group->observed_identity || !receipt.observed_attachment() ||
						*group->observed_identity != *receipt.observed_attachment() ||
						group->members.empty() || group->audits.empty() ||
						(group->existing_group_deferred_cleanup_required !=
						 (group->existing_group_deferred_cleanup_sequence != 0U)) ||
						(group->existing_group_deferred_cleanup_required &&
						 (!composite_close ||
						  group->existing_group_deferred_cleanup_sequence <=
							  group->group_origin_sequence)) ||
						group->registry_bound != registry_route ||
						map_attempt->potential_group_cut_sequence_slot != 0U ||
						map_attempt->potential_group_terminal_sequence_slot != 0U ||
						(has_composite_metadata && !composite_close))
					{
						map_attempt->quarantine_reason =
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure;
						quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																	 owner->token);
						inflight.disarm();
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					if (registry_route &&
						(!group->registry_activity_authority ||
						 !group->registry_activity_authority->retains_exact_owned_drain_lifetimes(
							 group->expected) ||
						 map_attempt->registry_predelegate_authority.has_value() !=
							 map_attempt->retirement_blocker ||
						 (map_attempt->registry_predelegate_authority &&
						  !map_attempt->registry_predelegate_authority
							   ->retains_exact_owned_terminal_lifetimes(*registry_family,
																		map_attempt->request))))
					{
						map_attempt->quarantine_reason =
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure;
						quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																	 owner->token);
						inflight.disarm();
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					if (!reader_group_custody_census_is_exact_locked(
							*group, cut_sealing, composite_close, true))
					{
						map_attempt->quarantine_reason =
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure;
						quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																	 owner->token);
						inflight.disarm();
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}

					const auto attempt_token = map_attempt->token;
					auto next_custodies = reader_custodies_;
					const auto attempt_custody = std::find_if(
						next_custodies.begin(),
						next_custodies.end(),
						[attempt_token](const reader_custody_record& custody)
						{
							return custody.kind == sqlite_shm_reader_custody_kind::map_attempt &&
								custody.owner_token == attempt_token &&
								custody.state == sqlite_shm_reader_custody_state::live;
						});
					const auto handoff_custody = std::find_if(
						next_custodies.begin(),
						next_custodies.end(),
						[&group](const reader_custody_record& custody)
						{
							return custody.kind ==
								sqlite_shm_reader_custody_kind::attachment_group_handoff &&
								custody.owner_token == group->token &&
								custody.state == sqlite_shm_reader_custody_state::live;
						});
					const auto existing_unmap = std::find_if(
						next_custodies.begin(),
						next_custodies.end(),
						[&group](const reader_custody_record& custody)
						{
							return custody.owner_token == group->token &&
								custody.attachment == group->expected &&
								custody.kind ==
								sqlite_shm_reader_custody_kind::normal_or_deferred_unmap &&
								custody.state == sqlite_shm_reader_custody_state::live;
						});
					const auto composite_owner = std::find_if(
						next_custodies.begin(),
						next_custodies.end(),
						[&group](const reader_custody_record& custody)
						{
							return custody.kind ==
								sqlite_shm_reader_custody_kind::close_cut_or_composite &&
								custody.owner_token == group->composite_close_owner_token &&
								custody.attachment == group->expected && custody.open_epoch &&
								reader_attachment_matches_open_epoch_binding(group->expected,
																			 *custody.open_epoch) &&
								custody.state == sqlite_shm_reader_custody_state::live &&
								custody.origin_sequence == group->composite_close_cut_sequence &&
								custody.destination_sequence == 0U;
						});
					if (attempt_custody == next_custodies.end() ||
						(cut_sealing &&
						 (handoff_custody != next_custodies.end() ||
						  (composite_close ? existing_unmap != next_custodies.end() ||
								   composite_owner == next_custodies.end()
										   : existing_unmap == next_custodies.end() ||
								   composite_owner != next_custodies.end()))) ||
						(!cut_sealing &&
						 (handoff_custody == next_custodies.end() ||
						  existing_unmap != next_custodies.end() ||
						  composite_owner != next_custodies.end())))
					{
						map_attempt->quarantine_reason =
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure;
						quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																	 owner->token);
						inflight.disarm();
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto attempt_index = static_cast<std::size_t>(
						std::distance(next_custodies.begin(), attempt_custody));
					std::optional<std::size_t> handoff_index;
					std::optional<std::size_t> minted_unmap_index;
					if (!cut_sealing)
					{
						handoff_index = static_cast<std::size_t>(
							std::distance(next_custodies.begin(), handoff_custody));
						minted_unmap_index = next_custodies.size();
						next_custodies.emplace_back(
							sqlite_shm_reader_custody_kind::normal_or_deferred_unmap,
							sqlite_shm_reader_custody_state::live,
							group->expected,
							group->token,
							0U,
							0U);
					}
					std::list<reader_existing_group_predecessor_mismatch_terminal_record>
						prepared_terminals;
					prepared_terminals.push_back({attempt_token,
												  owner->generation,
												  owner->token,
												  group->token,
												  receipt,
												  map_attempt->admission_sequence,
												  0U});
					const auto sequences = consume_reader_lifecycle_terminal_slot_locked(
						map_attempt->terminal_sequence_slot);
					if (!sequences.succeeded)
						throw reader_map_terminal_commit_injected_failure{};
					map_attempt->terminal_sequence_slot = 0U;
					map_attempt->terminal_sequence = sequences.first;
					prepared_terminals.back().terminal_sequence = sequences.first;
					next_custodies[attempt_index].state =
						sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt;
					next_custodies[attempt_index].destination_sequence = sequences.first;
					if (handoff_index)
					{
						next_custodies[*handoff_index].state =
							sqlite_shm_reader_custody_state::transferred_to_exact_successor;
						next_custodies[*handoff_index].destination_sequence = sequences.first;
						next_custodies[*minted_unmap_index].origin_sequence = sequences.first;
					}
					if (std::exchange(fail_next_reader_map_terminal_commit_for_testing_, false))
					{
						map_attempt->quarantine_reason =
							sqlite_shm_reader_terminal_quarantine_reason::injected_commit_failure;
						throw reader_map_terminal_commit_injected_failure{};
					}
					if (map_attempt->registry_predelegate_authority)
					{
						completed_predelegate->emplace(
							std::move(*map_attempt->registry_predelegate_authority));
						map_attempt->registry_predelegate_authority.reset();
					}
					if (!group->existing_group_deferred_cleanup_required)
					{
						group->existing_group_deferred_cleanup_required = true;
						group->existing_group_deferred_cleanup_sequence = sequences.first;
					}
					reader_existing_group_predecessor_mismatch_terminals_.splice(
						reader_existing_group_predecessor_mismatch_terminals_.end(),
						prepared_terminals);
					reader_custodies_.swap(next_custodies);
					reader_attachment_maps_.erase(map_attempt);
					inflight.disarm();
					reader_last_committed_sequence_ =
						std::max(reader_last_committed_sequence_, sequences.last);
					return sqlite_shm_reader_existing_group_predecessor_mismatch_result{
						receipt.native_status()};
				}
				catch (...)
				{
					quarantine_reader_map_terminal_commit(inflight, session);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<
				sqlite_shm_reader_predecessor_unmap_terminal_result>
			complete_reader_predecessor_unmap(
				const sqlite_shm_verified_reader_predecessor_unmap_terminal_receipt& receipt,
				const std::uint64_t registry_open_token = 0U,
				const std::shared_ptr<sqlite_shm_reader_open_lineage_seal>& seal = nullptr,
				const sqlite_shm_reader_open_epoch_binding* binding = nullptr,
				std::optional<sqlite_shm_reader_attachment_authority>* completed_activity =
					nullptr) noexcept
			{
				const auto registry_route = registry_open_token != 0U && seal &&
					binding != nullptr && completed_activity != nullptr;
				if ((registry_open_token != 0U || seal || binding != nullptr ||
					 completed_activity != nullptr) != registry_route ||
					(registry_route && completed_activity->has_value()))
					return sqlite_shm_unexpected(
						rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
								  sqlite_shm_lease_recovery_action::resubmit_via_bound_route));
				try
				{
					if (fail_next_reader_operation_mutex_acquire_for_testing_.exchange(
							false, std::memory_order_acq_rel))
						throw reader_operation_mutex_acquire_injected_failure{};
					std::scoped_lock lock{mutex_};
					const auto receipt_state = receipt.state_.lock();
					const auto map_terminal = std::find_if(
						reader_predecessor_map_terminals_.begin(),
						reader_predecessor_map_terminals_.end(),
						[&receipt](const reader_predecessor_map_terminal_record& terminal)
						{
							return terminal.token == receipt.token_;
						});
					const auto group =
						find_by_token(reader_attachment_groups_, receipt.reservation_token_);
					if (!receipt_state || receipt_state.get() != this || receipt.token_ == 0U ||
						receipt.reservation_token_ == 0U || receipt.generation_ == 0U ||
						map_terminal == reader_predecessor_map_terminals_.end() ||
						group == reader_attachment_groups_.end())
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));

					const auto registry_binding_matches = !registry_route ||
						(group->expected.registry_open_token() == registry_open_token &&
						 reader_attachment_matches_open_epoch_binding(group->expected, *binding) &&
						 binding->family == family_);
					const auto open = registry_route
						? std::find_if(
							  registry_reader_opens_.begin(),
							  registry_reader_opens_.end(),
							  [registry_open_token](const registry_reader_open_record& candidate)
							  {
								  return candidate.token == registry_open_token;
							  })
						: registry_reader_opens_.end();
					if (group->registry_bound != registry_route || !registry_binding_matches ||
						(registry_route &&
						 (open == registry_reader_opens_.end() || open->seal.get() != seal.get() ||
						  open->binding != *binding)))
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::resubmit_via_bound_route));
					if (group->reservation_phase ==
							sqlite_shm_reader_attachment_reservation_phase::
								predecessor_route_retired_confirmed &&
						(group->predecessor_unmap_terminal_receipt ||
						 group->predecessor_close_terminal_sequence != 0U))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					if (registry_route &&
						(open->close_phase != sqlite_shm_reader_connection_close_phase::open ||
						 !seal->authority_valid.load(std::memory_order_acquire)))
					{
						quarantine_reader_group_locked(
							*group,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::peer_quarantine);
						quarantine_reader_open_locked(
							*open,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::peer_quarantine);
						return sqlite_shm_unexpected(ambiguous());
					}
					if (group->reservation_phase ==
						sqlite_shm_reader_attachment_reservation_phase::terminal_quarantined)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::quarantined,
									  sqlite_shm_lease_recovery_action::quarantine_no_retry));

					auto prepared_receipt = receipt;
					auto result_effect = receipt.native_effect_receipt();
					const auto result_evidence_kind = receipt.evidence_kind();
					const auto result_native_status = receipt.native_status();
					const auto replayed_effect = prepared_receipt.native_effect_receipt() &&
						(reader_unmap_evidence_identity_seen_locked(
							 *prepared_receipt.native_effect_receipt(), group->token) ||
						 reader_callback_invocation_was_seen_locked(
							 *prepared_receipt.native_effect_receipt()) ||
						 reader_session_terminal_identity_seen_locked(
							 *prepared_receipt.native_effect_receipt()));
					const auto callback_cross_domain_replay =
						reader_effect_identity_seen_locked(
							prepared_receipt.callback().invocation_token, 0U, 0U, 0U) ||
						reader_session_terminal_identity_seen_locked(
							prepared_receipt.callback().invocation_token);
					const auto exact_map_binding =
						map_terminal->generation == receipt.generation_ &&
						map_terminal->session_request.attachment == group->expected &&
						map_terminal->receipt.request().expected_attachment == group->expected;
					if (!exact_map_binding || group->generation != receipt.generation_ ||
						group->reservation_phase !=
							sqlite_shm_reader_attachment_reservation_phase::
								predecessor_route_active ||
						group->phase != reader_attachment_group_phase::active ||
						group->observed_identity || !group->members.empty() ||
						!group->audits.empty() || group->predecessor_unmap_terminal_receipt ||
						group->predecessor_unmap_terminal_sequence != 0U ||
						group->predecessor_close_terminal_sequence != 0U ||
						!valid_reader_predecessor_unmap_terminal_receipt(prepared_receipt) ||
						!callback_can_start_locked(prepared_receipt.callback()) ||
						callback_cross_domain_replay || replayed_effect ||
						(prepared_receipt.native_effect_receipt() &&
						 *prepared_receipt.native_effect_receipt() ==
							 prepared_receipt.callback().invocation_token) ||
						std::ranges::any_of(reader_sessions_,
											[&group](const reader_session_record& session)
											{
												return session.request.attachment ==
													group->expected;
											}) ||
						std::ranges::any_of(reader_attachment_maps_,
											[&group](const reader_attachment_map_record& map)
											{
												return map.request.expected_attachment ==
													group->expected;
											}))
					{
						quarantine_reader_group_locked(
							*group,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::presented_invalid);
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}

					auto next_custodies = reader_custodies_;
					std::optional<std::size_t> runtime_pin_index;
					for (std::size_t index = 0U; index < next_custodies.size(); ++index)
					{
						const auto& custody = next_custodies[index];
						if (custody.attachment != group->expected ||
							custody.state != sqlite_shm_reader_custody_state::live)
							continue;
						if (registry_route &&
							custody.kind ==
								sqlite_shm_reader_custody_kind::
									runtime_vfs_namespace_generation_native_mapping_lifetime_pin &&
							custody.owner_token == group->token && !runtime_pin_index)
							runtime_pin_index = index;
						else
						{
							quarantine_reader_group_locked(
								*group,
								0U,
								sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
							quarantine_locked();
							return sqlite_shm_unexpected(ambiguous());
						}
					}
					if (registry_route != runtime_pin_index.has_value() ||
						group->registry_activity_authority.has_value() != registry_route ||
						(registry_route &&
						 !group->registry_activity_authority->retains_exact_owned_drain_lifetimes(
							 group->expected)))
					{
						quarantine_reader_group_locked(
							*group,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}

					const auto terminal_slot = reserve_reader_lifecycle_terminal_slots_locked(1U);
					if (!terminal_slot.succeeded || terminal_slot.slots.front() == 0U ||
						std::ranges::any_of(std::span{terminal_slot.slots}.subspan(1U),
											[](const std::uint64_t slot)
											{
												return slot != 0U;
											}))
						throw reader_predecessor_unmap_terminal_commit_injected_failure{};
					reader_terminal_slot_guard terminal_slot_guard{
						reader_lifecycle_sequences_ ? reader_lifecycle_sequences_->state_.get()
													: nullptr,
						terminal_slot.slots.front()};
					const auto sequences =
						consume_reader_lifecycle_terminal_slot_locked(terminal_slot.slots.front());
					if (!sequences.succeeded)
						throw reader_predecessor_unmap_terminal_commit_injected_failure{};
					terminal_slot_guard.release();
					static_assert(std::is_nothrow_move_constructible_v<
								  sqlite_shm_verified_reader_predecessor_unmap_terminal_receipt>);
					group->predecessor_unmap_terminal_receipt.emplace(std::move(prepared_receipt));
					group->predecessor_unmap_terminal_sequence = sequences.first;
					const auto injected_failure =
						std::exchange(fail_next_reader_unmap_terminal_commit_for_testing_, false);
					const auto exact_ok = result_evidence_kind ==
							sqlite_shm_reader_unmap_evidence_kind::exact_native_result &&
						result_native_status &&
						*result_native_status == static_cast<int>(sqlite_native_map_status::ok);
					const auto retired_confirmed = exact_ok && !injected_failure;
					if (runtime_pin_index)
					{
						next_custodies[*runtime_pin_index].state = retired_confirmed
							? sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt
							: sqlite_shm_reader_custody_state::transferred_to_durable_tombstone;
						next_custodies[*runtime_pin_index].destination_sequence = sequences.first;
					}
					group->reservation_phase = retired_confirmed
						? sqlite_shm_reader_attachment_reservation_phase::
							  predecessor_route_retired_confirmed
						: sqlite_shm_reader_attachment_reservation_phase::terminal_quarantined;
					group->reservation_destination_sequence = sequences.first;
					group->phase = retired_confirmed
						? reader_attachment_group_phase::native_cleanup_confirmed
						: reader_attachment_group_phase::terminal_quarantined;
					group->quarantine_reason = retired_confirmed
						? sqlite_shm_reader_terminal_quarantine_reason::none
						: injected_failure
						? sqlite_shm_reader_terminal_quarantine_reason::injected_commit_failure
						: sqlite_shm_reader_terminal_quarantine_reason::native_non_ok_or_unknown;
					if (retired_confirmed && group->registry_activity_authority)
					{
						completed_activity->emplace(std::move(*group->registry_activity_authority));
						group->registry_activity_authority.reset();
					}
					reader_custodies_.swap(next_custodies);
					reader_last_committed_sequence_ =
						std::max(reader_last_committed_sequence_, sequences.last);
					const auto terminal_kind = retired_confirmed
						? sqlite_shm_reader_unmap_terminal_kind::retired_confirmed
						: sqlite_shm_reader_unmap_terminal_kind::terminal_quarantined;
					if (!retired_confirmed)
						quarantine_locked();
					return sqlite_shm_reader_predecessor_unmap_terminal_result{
						terminal_kind,
						result_evidence_kind,
						result_native_status,
						std::move(result_effect)};
				}
				catch (...)
				{
					try
					{
						std::scoped_lock lock{mutex_};
						const auto group =
							find_by_token(reader_attachment_groups_, receipt.reservation_token_);
						if (group != reader_attachment_groups_.end())
							quarantine_reader_group_locked(
								*group,
								0U,
								sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						quarantine_locked();
					}
					catch (...)
					{
						emergency_quarantine_.store(true, std::memory_order_release);
					}
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_unpublished_cleanup_obligation>
			begin_registry_reader_unpublished_cleanup(
				sqlite_shm_registry_family_pin& registry_family,
				sqlite_shm_reader_attachment_map_inflight& inflight,
				const sqlite_shm_verified_reader_unpublished_cleanup_receipt& receipt,
				sqlite_shm_reader_session& session,
				std::optional<sqlite_shm_reader_map_predelegate_authority>& completed_predelegate)
			{
				if (emergency_quarantine_.load(std::memory_order_acquire) ||
					inflight.terminal_presentation_disabled_ ||
					session.terminal_presentation_disabled_ || completed_predelegate)
					return sqlite_shm_unexpected(ambiguous());
				try
				{
					if (fail_next_reader_operation_mutex_acquire_for_testing_.exchange(
							false, std::memory_order_acq_rel))
						throw reader_operation_mutex_acquire_injected_failure{};
					std::scoped_lock lock{mutex_};
					if (!owns(inflight.state_, inflight.token_) ||
						!owns(session.state_, session.token_))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));

					const auto map_attempt =
						find_by_token(reader_attachment_maps_, inflight.token_);
					const auto owner = find_by_token(reader_sessions_, session.token_);
					if (map_attempt == reader_attachment_maps_.end() ||
						map_attempt->phase != reader_phase::inflight ||
						map_attempt->session_token != session.token_ ||
						!map_attempt->registry_bound || owner == reader_sessions_.end() ||
						owner->phase != reader_session_record_phase::reserved_for_first_map ||
						!owner->registry_bound || owner->generation != inflight.generation_ ||
						owner->generation != session.generation_ ||
						owner->group_token != map_attempt->group_token)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));

					auto prepared_receipt = receipt;
					const auto receipt_state = prepared_receipt.state_.lock();
					if (!receipt_state || receipt_state.get() != this ||
						prepared_receipt.token_ != map_attempt->token ||
						prepared_receipt.request() != map_attempt->request ||
						prepared_receipt.session_request() != owner->request ||
						prepared_receipt.generation() != map_attempt->generation ||
						!valid_reader_unpublished_cleanup_receipt(prepared_receipt) ||
						reader_map_effect_identity_seen_locked(
							prepared_receipt.mapped_effect_receipt(), map_attempt->token) ||
						reader_callback_invocation_was_seen_locked(
							prepared_receipt.mapped_effect_receipt()) ||
						reader_session_terminal_identity_seen_locked(
							prepared_receipt.mapped_effect_receipt()) ||
						reader_session_terminal_identity_seen_locked(
							prepared_receipt.session_no_pointer_terminal_receipt()) ||
						reader_effect_identity_seen_locked(
							prepared_receipt.session_no_pointer_terminal_receipt(), 0U, 0U, 0U) ||
						reader_callback_invocation_was_seen_locked(
							prepared_receipt.session_no_pointer_terminal_receipt()) ||
						(map_attempt->unpublished_cleanup_required &&
						 (!map_attempt->receipt ||
						  prepared_receipt.kind() !=
							  sqlite_shm_reader_unpublished_cleanup_entry_kind::
								  exact_mapped_validation_failure ||
						  prepared_receipt.mapped_effect_receipt() !=
							  map_attempt->receipt->zero_resize_effect_receipt() ||
						  prepared_receipt.observed_attachment() !=
							  map_attempt->receipt->observed_attachment() ||
						  prepared_receipt.native_mapping() !=
							  map_attempt->receipt->mapping().native_mapping)) ||
						(!map_attempt->unpublished_cleanup_required && map_attempt->receipt))
					{
						map_attempt->quarantine_reason =
							sqlite_shm_reader_terminal_quarantine_reason::presented_invalid;
						quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																	 owner->token);
						inflight.disarm();
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}

					const auto group = find_by_token(reader_attachment_groups_, owner->group_token);
					const auto exact_group = group != reader_attachment_groups_.end() &&
						group->token == map_attempt->group_token &&
						group->expected == owner->request.attachment &&
						group->expected == map_attempt->request.expected_attachment &&
						group->generation == map_attempt->generation && group->registry_bound &&
						group->reservation_phase ==
							sqlite_shm_reader_attachment_reservation_phase::reserved &&
						group->phase == reader_attachment_group_phase::active &&
						!group->observed_identity && !group->unpublished_cleanup_receipt &&
						group->members.empty() && group->audits.empty() &&
						group->logical_ack_phase ==
							sqlite_shm_reader_logical_ack_phase::not_applicable &&
						group->registry_activity_authority &&
						group->registry_activity_authority->retains_exact_owned_drain_lifetimes(
							group->expected) &&
						group->registry_activity_authority->validate_active_authority(
							registry_family, group->expected);
					const auto exact_predelegate = map_attempt->retirement_blocker &&
						map_attempt->registry_predelegate_authority &&
						map_attempt->registry_predelegate_authority
							->retains_exact_owned_terminal_lifetimes(registry_family,
																	 map_attempt->request) &&
						map_attempt->registry_predelegate_authority->validate_active_authority(
							registry_family, map_attempt->request);
					const auto exact_single_session =
						std::ranges::count_if(reader_sessions_,
											  [&owner](const reader_session_record& candidate)
											  {
												  return candidate.request.attachment ==
													  owner->request.attachment;
											  }) == 1;
					const auto exact_single_map =
						std::ranges::count_if(
							reader_attachment_maps_,
							[&owner](const reader_attachment_map_record& candidate)
							{
								return candidate.request.expected_attachment ==
									owner->request.attachment;
							}) == 1;
					const auto exact_single_reservation =
						std::ranges::count_if(
							reader_attachment_groups_,
							[&owner](const reader_attachment_group_record& candidate)
							{
								return candidate.expected == owner->request.attachment;
							}) == 1;
					const auto exact_zero_generation_group_count = std::ranges::none_of(
						reader_custodies_,
						[&owner](const reader_custody_record& custody)
						{
							return custody.attachment == owner->request.attachment &&
								custody.kind ==
								sqlite_shm_reader_custody_kind::generation_group_count &&
								custody.state == sqlite_shm_reader_custody_state::live;
						});
					const auto exact_zero_published_handoff = std::ranges::none_of(
						reader_custodies_,
						[&owner](const reader_custody_record& custody)
						{
							return custody.attachment == owner->request.attachment &&
								custody.kind ==
								sqlite_shm_reader_custody_kind::attachment_group_handoff &&
								custody.state == sqlite_shm_reader_custody_state::live;
						});
					if (!exact_group || !exact_predelegate || !exact_single_session ||
						!exact_single_map || !exact_single_reservation ||
						!exact_zero_generation_group_count || !exact_zero_published_handoff ||
						!generation_ || generation_->value != owner->generation ||
						(generation_->phase != sqlite_shm_mapping_generation_phase::live &&
						 generation_->phase != sqlite_shm_mapping_generation_phase::retiring))
					{
						quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																	 owner->token);
						inflight.disarm();
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto close_open = std::find_if(
						registry_reader_opens_.begin(),
						registry_reader_opens_.end(),
						[&group](const registry_reader_open_record& candidate)
						{
							return candidate.token == group->expected.registry_open_token();
						});
					const auto phase1_close_cut = close_open != registry_reader_opens_.end() &&
						reader_phase1_close_cut_is_exact_locked(*group, *close_open);
					const auto has_close_cut = group->composite_close_owner_token != 0U ||
						group->composite_close_registry_open_token != 0U ||
						group->composite_close_cut_sequence != 0U ||
						group->composite_close_wait_resolution_sequence_slot != 0U ||
						group->composite_close_wait_resolution_sequence != 0U;
					if (has_close_cut && !phase1_close_cut)
					{
						quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																	 owner->token);
						inflight.disarm();
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}

					auto prepared_callback = prepared_receipt.request().callback;
					auto prepared_state = shared_from_this();
					auto next_custodies = reader_custodies_;
					std::optional<std::size_t> map_custody_index;
					std::optional<std::size_t> session_custody_index;
					std::optional<std::size_t> runtime_pin_custody_index;
					for (std::size_t index = 0U; index < next_custodies.size(); ++index)
					{
						const auto& custody = next_custodies[index];
						if (custody.attachment != group->expected ||
							custody.state != sqlite_shm_reader_custody_state::live)
							continue;
						if (custody.kind == sqlite_shm_reader_custody_kind::map_attempt &&
							custody.owner_token == map_attempt->token && !map_custody_index)
							map_custody_index = index;
						else if (custody.kind ==
									 sqlite_shm_reader_custody_kind::use_session_reservation &&
								 custody.owner_token == owner->token && !session_custody_index)
							session_custody_index = index;
						else if (
							custody.kind ==
								sqlite_shm_reader_custody_kind::
									runtime_vfs_namespace_generation_native_mapping_lifetime_pin &&
							custody.owner_token == group->token && !runtime_pin_custody_index)
							runtime_pin_custody_index = index;
						else if (phase1_close_cut &&
								 (custody.kind ==
									  sqlite_shm_reader_custody_kind::
										  bounded_waiter_or_continuation ||
								  custody.kind ==
									  sqlite_shm_reader_custody_kind::terminal_reporter) &&
								 custody.owner_token == group->token &&
								 custody.origin_sequence == group->composite_close_cut_sequence &&
								 custody.destination_sequence == 0U)
							continue;
						else
						{
							quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																		 owner->token);
							inflight.disarm();
							session.disarm();
							return sqlite_shm_unexpected(ambiguous());
						}
					}
					if (!map_custody_index || !session_custody_index ||
						!runtime_pin_custody_index || map_attempt->terminal_sequence_slot == 0U ||
						owner->terminal_sequence_slot == 0U ||
						map_attempt->potential_group_cut_sequence_slot == 0U ||
						map_attempt->potential_group_terminal_sequence_slot == 0U)
					{
						quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																	 owner->token);
						inflight.disarm();
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}

					next_custodies.emplace_back(
						sqlite_shm_reader_custody_kind::exact_present_attachment,
						sqlite_shm_reader_custody_state::live,
						group->expected,
						group->token,
						0U,
						0U);
					next_custodies.emplace_back(sqlite_shm_reader_custody_kind::unpublished_cleanup,
												sqlite_shm_reader_custody_state::live,
												group->expected,
												group->token,
												0U,
												0U);
					next_custodies.emplace_back(
						sqlite_shm_reader_custody_kind::unmap_cut,
						sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt,
						group->expected,
						group->token,
						0U,
						0U);
					const std::array terminal_slots{
						map_attempt->terminal_sequence_slot,
						owner->terminal_sequence_slot,
						map_attempt->potential_group_cut_sequence_slot,
					};
					const auto sequences =
						consume_reader_lifecycle_terminal_slots_locked(terminal_slots);
					if (!sequences.succeeded)
					{
						quarantine_reader_map_terminal_commit_locked(map_attempt->token,
																	 owner->token);
						inflight.disarm();
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto map_terminal_sequence = sequences.first;
					const auto session_terminal_sequence = sequences.first + 1U;
					const auto cleanup_cut_sequence = sequences.last;
					map_attempt->terminal_sequence_slot = 0U;
					map_attempt->terminal_sequence = map_terminal_sequence;
					owner->terminal_sequence_slot = 0U;
					owner->pending_terminal_sequence = session_terminal_sequence;
					map_attempt->potential_group_cut_sequence_slot = 0U;
					group->unmap_terminal_sequence_slot =
						std::exchange(map_attempt->potential_group_terminal_sequence_slot, 0U);
					next_custodies[*map_custody_index].state =
						sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt;
					next_custodies[*map_custody_index].destination_sequence = map_terminal_sequence;
					next_custodies[*session_custody_index].state =
						sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt;
					next_custodies[*session_custody_index].destination_sequence =
						session_terminal_sequence;
					for (auto iterator = next_custodies.end() - 3; iterator != next_custodies.end();
						 ++iterator)
						iterator->origin_sequence = cleanup_cut_sequence;
					next_custodies.back().destination_sequence = cleanup_cut_sequence;

					static_assert(std::is_nothrow_move_constructible_v<
								  sqlite_shm_verified_reader_unpublished_cleanup_receipt>);
					static_assert(std::is_nothrow_move_constructible_v<
								  sqlite_shm_reader_map_predelegate_authority>);
					group->unpublished_cleanup_receipt.emplace(std::move(prepared_receipt));
					group->unpublished_cleanup_callback.emplace(std::move(prepared_callback));
					group->reservation_phase = sqlite_shm_reader_attachment_reservation_phase::
						unpublished_cleanup_admitted;
					group->reservation_destination_sequence = cleanup_cut_sequence;
					group->phase = reader_attachment_group_phase::native_cleanup_admitted;
					group->unpublished_cleanup_cut_sequence = cleanup_cut_sequence;
					group->unpublished_cleanup_map_token = map_attempt->token;
					group->unpublished_cleanup_session_token = owner->token;
					group->unpublished_cleanup_map_admission_sequence =
						map_attempt->admission_sequence;
					group->unpublished_cleanup_map_terminal_sequence = map_terminal_sequence;
					group->unpublished_cleanup_session_origin_sequence =
						owner->lifecycle_origin_sequence;
					group->unpublished_cleanup_session_terminal_sequence =
						session_terminal_sequence;
					owner->lifecycle_phase =
						sqlite_shm_reader_session_reservation_phase::consumed_no_pointer;
					owner->lifecycle_destination_sequence = session_terminal_sequence;
					completed_predelegate.emplace(
						std::move(*map_attempt->registry_predelegate_authority));
					map_attempt->registry_predelegate_authority.reset();
					const auto token = group->token;
					const auto generation = group->generation;
					reader_attachment_maps_.erase(map_attempt);
					reader_sessions_.erase(owner);
					reader_custodies_.swap(next_custodies);
					inflight.disarm();
					session.disarm();
					reader_last_committed_sequence_ =
						std::max(reader_last_committed_sequence_, sequences.last);
					return sqlite_shm_reader_unpublished_cleanup_obligation{
						std::move(prepared_state),
						sqlite_shm_lease_token_identity{token},
						sqlite_shm_mapping_generation_identity{generation}};
				}
				catch (...)
				{
					quarantine_reader_map_terminal_commit(inflight, session);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]]
			sqlite_shm_lease_result<sqlite_shm_reader_unpublished_cleanup_terminal_result>
			complete_registry_reader_unpublished_cleanup(
				sqlite_shm_reader_unpublished_cleanup_obligation& cleanup,
				const sqlite_shm_verified_reader_unpublished_cleanup_terminal_receipt&
					receipt) noexcept
			{
				if (emergency_quarantine_.load(std::memory_order_acquire) ||
					cleanup.terminal_presentation_disabled_)
					return sqlite_shm_unexpected(ambiguous());
				try
				{
					if (fail_next_reader_operation_mutex_acquire_for_testing_.exchange(
							false, std::memory_order_acq_rel))
						throw reader_operation_mutex_acquire_injected_failure{};
					std::scoped_lock lock{mutex_};
					if (!owns(cleanup.state_, cleanup.token_))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					const auto group = find_by_token(reader_attachment_groups_, cleanup.token_);
					if (group == reader_attachment_groups_.end() ||
						group->generation != cleanup.generation_ || !group->registry_bound ||
						group->reservation_phase !=
							sqlite_shm_reader_attachment_reservation_phase::
								unpublished_cleanup_admitted ||
						group->phase != reader_attachment_group_phase::native_cleanup_admitted ||
						!group->unpublished_cleanup_receipt ||
						!group->unpublished_cleanup_callback ||
						group->logical_ack_phase !=
							sqlite_shm_reader_logical_ack_phase::not_applicable)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));

					auto prepared_receipt = receipt;
					const auto& terminal_receipt = receipt;
					auto result_effect = receipt.native_effect_receipt();
					auto result_latch = receipt.latch_reset_receipt();
					const auto result_evidence_kind = receipt.evidence_kind();
					const auto result_native_status = receipt.native_status();
					const auto receipt_state = prepared_receipt.state_.lock();
					const auto replayed_effect = prepared_receipt.native_effect_receipt() &&
						(reader_unmap_evidence_identity_seen_locked(
							 *prepared_receipt.native_effect_receipt(), group->token) ||
						 reader_callback_invocation_was_seen_locked(
							 *prepared_receipt.native_effect_receipt()) ||
						 reader_session_terminal_identity_seen_locked(
							 *prepared_receipt.native_effect_receipt()));
					const auto replayed_latch = prepared_receipt.latch_reset_receipt() &&
						(reader_unmap_evidence_identity_seen_locked(
							 *prepared_receipt.latch_reset_receipt(), group->token) ||
						 reader_callback_invocation_was_seen_locked(
							 *prepared_receipt.latch_reset_receipt()) ||
						 reader_session_terminal_identity_seen_locked(
							 *prepared_receipt.latch_reset_receipt()));
					const auto exact_zero_record_census = !group->observed_identity &&
						group->members.empty() && group->audits.empty() &&
						std::ranges::none_of(reader_sessions_,
											 [&group](const reader_session_record& candidate)
											 {
												 return candidate.request.attachment ==
													 group->expected;
											 }) &&
						std::ranges::none_of(reader_attachment_maps_,
											 [&group](const reader_attachment_map_record& candidate)
											 {
												 return candidate.request.expected_attachment ==
													 group->expected;
											 }) &&
						std::ranges::count_if(
							reader_attachment_groups_,
							[&group](const reader_attachment_group_record& candidate)
							{
								return candidate.expected == group->expected;
							}) == 1 &&
						std::ranges::none_of(
							reader_custodies_,
							[&group](const reader_custody_record& custody)
							{
								return custody.attachment == group->expected &&
									custody.state == sqlite_shm_reader_custody_state::live &&
									(custody.kind ==
										 sqlite_shm_reader_custody_kind::use_session_reservation ||
									 custody.kind == sqlite_shm_reader_custody_kind::use_session ||
									 custody.kind ==
										 sqlite_shm_reader_custody_kind::generation_group_count ||
									 custody.kind ==
										 sqlite_shm_reader_custody_kind::attachment_group_handoff);
							});
					if (!receipt_state || receipt_state.get() != this ||
						prepared_receipt.token_ != cleanup.token_ ||
						prepared_receipt.generation_ != cleanup.generation_ ||
						!valid_reader_unpublished_cleanup_terminal_receipt(prepared_receipt) ||
						prepared_receipt.callback() != *group->unpublished_cleanup_callback ||
						(prepared_receipt.native_effect_receipt() &&
						 *prepared_receipt.native_effect_receipt() ==
							 prepared_receipt.callback().invocation_token) ||
						(prepared_receipt.latch_reset_receipt() &&
						 *prepared_receipt.latch_reset_receipt() ==
							 prepared_receipt.callback().invocation_token) ||
						replayed_effect || replayed_latch ||
						group->unmap_terminal_sequence_slot == 0U || !exact_zero_record_census)
					{
						quarantine_reader_group_locked(
							*group,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::presented_invalid);
						cleanup.disarm();
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto close_open = std::find_if(
						registry_reader_opens_.begin(),
						registry_reader_opens_.end(),
						[&group](const registry_reader_open_record& candidate)
						{
							return candidate.token == group->expected.registry_open_token();
						});
					const auto phase1_close_cut = close_open != registry_reader_opens_.end() &&
						reader_phase1_close_cut_is_exact_locked(*group, *close_open);
					const auto has_close_cut = group->composite_close_owner_token != 0U ||
						group->composite_close_registry_open_token != 0U ||
						group->composite_close_cut_sequence != 0U ||
						group->composite_close_wait_resolution_sequence_slot != 0U ||
						group->composite_close_wait_resolution_sequence != 0U;
					if (has_close_cut && !phase1_close_cut)
						throw reader_unpublished_cleanup_terminal_commit_injected_failure{};
					auto next_custodies = reader_custodies_;
					std::optional<std::size_t> exact_present_index;
					std::optional<std::size_t> cleanup_index;
					std::optional<std::size_t> runtime_pin_index;
					for (std::size_t index = 0U; index < next_custodies.size(); ++index)
					{
						const auto& custody = next_custodies[index];
						if (custody.attachment != group->expected ||
							custody.state != sqlite_shm_reader_custody_state::live)
							continue;
						if (custody.kind ==
								sqlite_shm_reader_custody_kind::exact_present_attachment &&
							custody.owner_token == group->token && !exact_present_index)
							exact_present_index = index;
						else if (custody.kind ==
									 sqlite_shm_reader_custody_kind::unpublished_cleanup &&
								 custody.owner_token == group->token && !cleanup_index)
							cleanup_index = index;
						else if (
							custody.kind ==
								sqlite_shm_reader_custody_kind::
									runtime_vfs_namespace_generation_native_mapping_lifetime_pin &&
							custody.owner_token == group->token && !runtime_pin_index)
							runtime_pin_index = index;
						else if (phase1_close_cut &&
								 (custody.kind ==
									  sqlite_shm_reader_custody_kind::
										  bounded_waiter_or_continuation ||
								  custody.kind ==
									  sqlite_shm_reader_custody_kind::terminal_reporter) &&
								 custody.owner_token == group->token &&
								 custody.origin_sequence == group->composite_close_cut_sequence &&
								 custody.destination_sequence == 0U)
							continue;
						else
							throw reader_unpublished_cleanup_terminal_commit_injected_failure{};
					}
					if (!exact_present_index || !cleanup_index || !runtime_pin_index)
						throw reader_unpublished_cleanup_terminal_commit_injected_failure{};
					const auto exact_ok = terminal_receipt.evidence_kind() ==
							sqlite_shm_reader_unpublished_cleanup_evidence_kind::
								exact_native_result &&
						terminal_receipt.native_status() &&
						*terminal_receipt.native_status() ==
							static_cast<int>(sqlite_native_map_status::ok);
					const auto logical_ack_slot = exact_ok
						? reserve_reader_lifecycle_terminal_slots_locked(1U)
						: reader_terminal_slot_reservation_batch{};
					if (exact_ok &&
						(!logical_ack_slot.succeeded || logical_ack_slot.slots.front() == 0U ||
						 std::ranges::any_of(std::span{logical_ack_slot.slots}.subspan(1U),
											 [](const std::uint64_t slot)
											 {
												 return slot != 0U;
											 })))
						throw reader_unpublished_cleanup_terminal_commit_injected_failure{};
					reader_terminal_slot_guard logical_ack_slot_guard{
						reader_lifecycle_sequences_ ? reader_lifecycle_sequences_->state_.get()
													: nullptr,
						exact_ok ? logical_ack_slot.slots.front() : 0U};
					static_assert(std::is_nothrow_move_constructible_v<
								  sqlite_shm_verified_reader_unpublished_cleanup_terminal_receipt>);
					group->unpublished_cleanup_terminal_receipt.emplace(
						std::move(prepared_receipt));
					if (std::exchange(
							fail_next_reader_unpublished_cleanup_terminal_commit_for_testing_,
							false))
						throw reader_unpublished_cleanup_terminal_commit_injected_failure{};
					if (exact_ok)
						next_custodies.emplace_back(sqlite_shm_reader_custody_kind::logical_ack,
													sqlite_shm_reader_custody_state::live,
													group->expected,
													group->token,
													0U,
													0U);
					const auto sequences = consume_reader_lifecycle_terminal_slot_locked(
						group->unmap_terminal_sequence_slot);
					if (!sequences.succeeded)
						throw reader_unpublished_cleanup_terminal_commit_injected_failure{};
					group->unmap_terminal_sequence_slot = 0U;
					group->unpublished_cleanup_terminal_sequence = sequences.first;
					const auto custody_terminal = exact_ok
						? sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt
						: sqlite_shm_reader_custody_state::transferred_to_durable_tombstone;
					next_custodies[*exact_present_index].state = custody_terminal;
					next_custodies[*exact_present_index].destination_sequence = sequences.first;
					next_custodies[*cleanup_index].state = custody_terminal;
					next_custodies[*cleanup_index].destination_sequence = sequences.first;
					if (exact_ok)
						next_custodies.back().origin_sequence = sequences.first;
					else
					{
						next_custodies[*runtime_pin_index].state =
							sqlite_shm_reader_custody_state::transferred_to_durable_tombstone;
						next_custodies[*runtime_pin_index].destination_sequence = sequences.first;
					}
					group->reservation_phase = exact_ok
						? sqlite_shm_reader_attachment_reservation_phase::
							  unpublished_cleanup_confirmed
						: sqlite_shm_reader_attachment_reservation_phase::terminal_quarantined;
					group->reservation_destination_sequence = sequences.first;
					group->phase = exact_ok
						? reader_attachment_group_phase::native_cleanup_confirmed
						: reader_attachment_group_phase::terminal_quarantined;
					group->logical_ack_phase = exact_ok
						? sqlite_shm_reader_logical_ack_phase::awaiting_sqlite_ack
						: sqlite_shm_reader_logical_ack_phase::not_applicable;
					group->logical_ack_sequence_slot =
						exact_ok ? logical_ack_slot.slots.front() : 0U;
					group->quarantine_reason = exact_ok
						? sqlite_shm_reader_terminal_quarantine_reason::none
						: sqlite_shm_reader_terminal_quarantine_reason::native_non_ok_or_unknown;
					reader_custodies_.swap(next_custodies);
					logical_ack_slot_guard.release();
					cleanup.disarm();
					reader_last_committed_sequence_ =
						std::max(reader_last_committed_sequence_, sequences.last);
					const auto terminal_kind = exact_ok
						? sqlite_shm_reader_unpublished_cleanup_terminal_kind::confirmed
						: sqlite_shm_reader_unpublished_cleanup_terminal_kind::terminal_quarantined;
					if (!exact_ok)
						quarantine_locked();
					return sqlite_shm_reader_unpublished_cleanup_terminal_result{
						terminal_kind,
						result_evidence_kind,
						result_native_status,
						std::move(result_effect),
						std::move(result_latch)};
				}
				catch (...)
				{
					quarantine_reader_unpublished_cleanup_terminal(cleanup);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_logical_ack_result>
			consume_registry_reader_logical_ack(
				const std::uint64_t registry_open_token,
				const std::shared_ptr<sqlite_shm_reader_open_lineage_seal>& seal,
				const sqlite_shm_reader_open_epoch_binding& binding,
				const sqlite_shm_reader_logical_ack_request& request,
				std::optional<sqlite_shm_reader_attachment_authority>& completed_activity) noexcept
			{
				if (registry_open_token == 0U || !seal ||
					!valid_reader_open_epoch_binding(binding) ||
					!valid_callback(request.callback) || request.caller_delete_flag != 0 ||
					request.delegated_delete_flag != 0 || completed_activity)
					return sqlite_shm_unexpected(
						rejection(sqlite_shm_lease_rejection_reason::invalid_request,
								  sqlite_shm_lease_recovery_action::quarantine_no_retry));
				try
				{
					if (fail_next_reader_operation_mutex_acquire_for_testing_.exchange(
							false, std::memory_order_acq_rel))
						throw reader_operation_mutex_acquire_injected_failure{};
					std::scoped_lock lock{mutex_};
					const auto open = std::find_if(
						registry_reader_opens_.begin(),
						registry_reader_opens_.end(),
						[registry_open_token](const registry_reader_open_record& candidate)
						{
							return candidate.token == registry_open_token;
						});
					if (open == registry_reader_opens_.end())
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					if (open->seal.get() != seal.get() || open->binding != binding ||
						binding.family != family_)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::resubmit_via_bound_route));
					if (emergency_quarantine_.load(std::memory_order_acquire) ||
						!seal->authority_valid.load(std::memory_order_acquire))
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::quarantined,
									  sqlite_shm_lease_recovery_action::quarantine_no_retry));

					auto matching_group = reader_attachment_groups_.end();
					for (auto candidate = reader_attachment_groups_.begin();
						 candidate != reader_attachment_groups_.end();
						 ++candidate)
					{
						if (!candidate->registry_bound)
							continue;
						const auto token_matches =
							candidate->expected.registry_open_token() == registry_open_token;
						const auto binding_matches = reader_attachment_matches_open_epoch_binding(
							candidate->expected, binding);
						if (token_matches != binding_matches)
						{
							quarantine_locked();
							return sqlite_shm_unexpected(ambiguous());
						}
						if (!token_matches)
							continue;
						if (candidate->logical_ack_phase !=
							sqlite_shm_reader_logical_ack_phase::awaiting_sqlite_ack)
							continue;
						if (matching_group != reader_attachment_groups_.end())
						{
							quarantine_locked();
							return sqlite_shm_unexpected(ambiguous());
						}
						matching_group = candidate;
					}
					if (matching_group == reader_attachment_groups_.end())
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::outer_ioerr_no_retry));
					if (open->close_phase ==
							sqlite_shm_reader_connection_close_phase::close_admitted &&
						reader_phase1_close_cut_is_exact_locked(*matching_group, *open))
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::retiring,
									  sqlite_shm_lease_recovery_action::outer_ioerr_no_retry));
					if (open->close_phase != sqlite_shm_reader_connection_close_phase::open)
					{
						quarantine_reader_group_locked(
							*matching_group,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						quarantine_reader_open_locked(
							*open,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						return sqlite_shm_unexpected(ambiguous());
					}
					auto& group = *matching_group;
					const auto exact_cleanup_terminal =
						reader_local_awaiting_ack_group_shape_is_exact_locked(group);
					const auto callback_cross_domain_replay =
						reader_effect_identity_seen_locked(
							request.callback.invocation_token, 0U, 0U, 0U) ||
						reader_session_terminal_identity_seen_locked(
							request.callback.invocation_token);
					if (!exact_cleanup_terminal || !callback_can_start_locked(request.callback) ||
						callback_cross_domain_replay)
					{
						quarantine_reader_group_locked(
							group,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::presented_invalid);
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}

					auto prepared_callback = request.callback;
					auto next_custodies = reader_custodies_;
					std::optional<std::size_t> logical_ack_index;
					std::optional<std::size_t> runtime_pin_index;
					for (std::size_t index = 0U; index < next_custodies.size(); ++index)
					{
						const auto& custody = next_custodies[index];
						if (custody.attachment != group.expected ||
							custody.state != sqlite_shm_reader_custody_state::live)
							continue;
						if (custody.kind == sqlite_shm_reader_custody_kind::logical_ack &&
							custody.owner_token == group.token && !logical_ack_index)
							logical_ack_index = index;
						else if (
							custody.kind ==
								sqlite_shm_reader_custody_kind::
									runtime_vfs_namespace_generation_native_mapping_lifetime_pin &&
							custody.owner_token == group.token && !runtime_pin_index)
							runtime_pin_index = index;
						else
						{
							quarantine_reader_group_locked(
								group,
								0U,
								sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
							quarantine_locked();
							return sqlite_shm_unexpected(ambiguous());
						}
					}
					if (!logical_ack_index || !runtime_pin_index)
					{
						quarantine_reader_group_locked(
							group,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto sequences = consume_reader_lifecycle_terminal_slot_locked(
						group.logical_ack_sequence_slot);
					if (!sequences.succeeded)
						throw reader_logical_ack_commit_injected_failure{};
					group.logical_ack_sequence_slot = 0U;
					next_custodies[*logical_ack_index].state =
						sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt;
					next_custodies[*logical_ack_index].destination_sequence = sequences.first;
					next_custodies[*runtime_pin_index].state =
						sqlite_shm_reader_custody_state::transferred_to_durable_tombstone;
					next_custodies[*runtime_pin_index].destination_sequence = sequences.first;
					static_assert(std::is_nothrow_move_constructible_v<
								  sqlite_shm_callback_execution_receipt>);
					group.logical_ack_callback.emplace(std::move(prepared_callback));
					group.logical_ack_phase =
						sqlite_shm_reader_logical_ack_phase::consumed_by_exact_unmap;
					group.logical_ack_sequence = sequences.first;
					const auto stored_cleanup_status =
						*group.unpublished_cleanup_terminal_receipt->native_status();
					static_assert(std::is_nothrow_move_constructible_v<
								  sqlite_shm_reader_attachment_authority>);
					completed_activity.emplace(std::move(*group.registry_activity_authority));
					group.registry_activity_authority.reset();
					reader_custodies_.swap(next_custodies);
					reader_last_committed_sequence_ =
						std::max(reader_last_committed_sequence_, sequences.last);
					return sqlite_shm_reader_logical_ack_result{stored_cleanup_status};
				}
				catch (...)
				{
					try
					{
						std::scoped_lock lock{mutex_};
						const auto group =
							std::find_if(reader_attachment_groups_.begin(),
										 reader_attachment_groups_.end(),
										 [registry_open_token,
										  &binding](const reader_attachment_group_record& candidate)
										 {
											 return candidate.registry_bound &&
												 candidate.expected.registry_open_token() ==
												 registry_open_token &&
												 reader_attachment_matches_open_epoch_binding(
														candidate.expected, binding);
										 });
						if (group != reader_attachment_groups_.end())
							quarantine_reader_group_locked(
								*group,
								0U,
								sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						quarantine_locked();
					}
					catch (...)
					{
						emergency_quarantine_.store(true, std::memory_order_release);
					}
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<void> complete_reader_session(
				sqlite_shm_reader_session& session,
				const sqlite_shm_reader_session_terminal_receipt& receipt,
				std::optional<sqlite_shm_reader_attachment_authority>* completed_activity =
					nullptr) noexcept
			{
				std::uint64_t exact_group_token{};
				if (emergency_quarantine_.load(std::memory_order_acquire) ||
					session.terminal_presentation_disabled_)
				{
					if (owns(session.state_, session.token_))
						session.disable_terminal_presentation();
					return sqlite_shm_unexpected(ambiguous());
				}
				try
				{
					if (fail_next_reader_operation_mutex_acquire_for_testing_.exchange(
							false, std::memory_order_acq_rel))
						throw reader_operation_mutex_acquire_injected_failure{};
					std::scoped_lock lock{mutex_};
					if (!owns(session.state_, session.token_))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					const auto owner = find_by_token(reader_sessions_, session.token_);
					if (owner == reader_sessions_.end())
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					const auto registry_route = completed_activity != nullptr;
					if (owner->registry_bound != registry_route ||
						(registry_route && completed_activity->has_value()))
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::resubmit_via_bound_route));
					auto pending_receipt = receipt;
					const auto terminal_identity_reused =
						reader_session_terminal_identity_seen_locked(receipt.terminal_receipt()) ||
						reader_effect_identity_seen_locked(
							receipt.terminal_receipt(), 0U, 0U, 0U) ||
						reader_callback_invocation_was_seen_locked(receipt.terminal_receipt()) ||
						std::ranges::any_of(
							reader_sessions_,
							[&owner, &receipt](const reader_session_record& candidate)
							{
								return candidate.token != owner->token &&
									candidate.pending_terminal_receipt &&
									candidate.pending_terminal_receipt->terminal_receipt() ==
									receipt.terminal_receipt();
							}) ||
						std::ranges::any_of(
							reader_session_terminals_,
							[&receipt](const reader_session_terminal_record& terminal)
							{
								return terminal.receipt.terminal_receipt() ==
									receipt.terminal_receipt();
							});
					if (terminal_identity_reused)
					{
						owner->quarantine_reason =
							sqlite_shm_reader_terminal_quarantine_reason::presented_invalid;
						quarantine_reader_session_locked(*owner);
						session.disarm();
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::quarantine_no_retry));
					}
					const auto reserved =
						owner->phase == reader_session_record_phase::reserved_for_first_map;
					const auto valid_terminal_kind =
						receipt.kind() == sqlite_shm_reader_session_terminal_kind::success ||
						receipt.kind() == sqlite_shm_reader_session_terminal_kind::failure ||
						receipt.kind() ==
							sqlite_shm_reader_session_terminal_kind::
								cancelled_before_authority_read;
					if (owner->phase == reader_session_record_phase::terminal_quarantined ||
						owner->generation != session.generation_ ||
						receipt.request() != owner->request ||
						!valid_identity(receipt.terminal_receipt()) || !valid_terminal_kind ||
						!receipt.authority_read_closed() || !receipt.no_live_shm_lock() ||
						(!reserved &&
						 receipt.kind() ==
							 sqlite_shm_reader_session_terminal_kind::
								 cancelled_before_authority_read))
					{
						owner->quarantine_reason =
							sqlite_shm_reader_terminal_quarantine_reason::presented_invalid;
						quarantine_reader_session_locked(
							*owner,
							sqlite_shm_reader_terminal_quarantine_reason::presented_invalid);
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					static_assert(std::is_nothrow_move_constructible_v<
								  sqlite_shm_reader_session_terminal_receipt>);
					owner->pending_terminal_receipt.emplace(std::move(pending_receipt));
					owner->quarantine_reason =
						sqlite_shm_reader_terminal_quarantine_reason::internal_failure;
					exact_group_token = owner->group_token;
					if (std::ranges::any_of(reader_attachment_maps_,
											[&owner](const reader_attachment_map_record& map)
											{
												return map.session_token == owner->token &&
													map.phase != reader_phase::terminal_quarantined;
											}))
					{
						quarantine_reader_session_locked(
							*owner, sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto reservation =
						find_by_token(reader_attachment_groups_, owner->group_token);
					if (reservation == reader_attachment_groups_.end() ||
						reservation->expected != owner->request.attachment)
					{
						quarantine_reader_session_locked(
							*owner, sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto peer_drain_after_group_quarantine = !reserved &&
						reservation->reservation_phase ==
							sqlite_shm_reader_attachment_reservation_phase::terminal_quarantined &&
						reservation->phase == reader_attachment_group_phase::terminal_quarantined &&
						reservation->observed_identity.has_value();
					const auto drain_after_unmap_cut = !reserved &&
						reservation->reservation_phase ==
							sqlite_shm_reader_attachment_reservation_phase::observed_present &&
						reservation->phase == reader_attachment_group_phase::unmap_cut_sealing &&
						reservation->observed_identity.has_value();
					if ((reserved &&
						 (reservation->reservation_phase !=
							  sqlite_shm_reader_attachment_reservation_phase::reserved ||
						  reservation->observed_identity)) ||
						(!reserved && !peer_drain_after_group_quarantine &&
						 !drain_after_unmap_cut &&
						 (reservation->reservation_phase !=
							  sqlite_shm_reader_attachment_reservation_phase::observed_present ||
						  !reservation->observed_identity ||
						  reservation->phase != reader_attachment_group_phase::active)) ||
						(registry_route &&
						 (!reservation->registry_activity_authority ||
						  !reservation->registry_activity_authority
							   ->retains_exact_owned_drain_lifetimes(owner->request.attachment))))
					{
						quarantine_reader_session_locked(
							*owner, sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}

					auto next_custodies = reader_custodies_;
					const auto custody_kind = reserved
						? sqlite_shm_reader_custody_kind::use_session_reservation
						: sqlite_shm_reader_custody_kind::use_session;
					const auto custody =
						std::find_if(next_custodies.begin(),
									 next_custodies.end(),
									 [&owner, custody_kind](const reader_custody_record& value)
									 {
										 return value.kind == custody_kind &&
											 value.owner_token == owner->token &&
											 value.state == sqlite_shm_reader_custody_state::live;
									 });
					if (custody == next_custodies.end())
					{
						quarantine_reader_session_locked(
							*owner, sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto custody_index =
						static_cast<std::size_t>(std::distance(next_custodies.begin(), custody));
					std::optional<std::size_t> runtime_custody_index;
					if (reserved && registry_route)
					{
						const auto runtime_custody = std::find_if(
							next_custodies.begin(),
							next_custodies.end(),
							[&reservation](const reader_custody_record& value)
							{
								return value.kind ==
									sqlite_shm_reader_custody_kind::
										runtime_vfs_namespace_generation_native_mapping_lifetime_pin &&
									value.owner_token == reservation->token &&
									value.state == sqlite_shm_reader_custody_state::live;
							});
						if (runtime_custody == next_custodies.end())
						{
							quarantine_reader_session_locked(
								*owner,
								sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
							session.disarm();
							return sqlite_shm_unexpected(ambiguous());
						}
						runtime_custody_index = static_cast<std::size_t>(
							std::distance(next_custodies.begin(), runtime_custody));
					}
					std::list<reader_session_terminal_record> prepared_terminals;
					prepared_terminals.push_back(
						{owner->token,
						 owner->phase,
						 receipt,
						 reserved ? sqlite_shm_reader_session_reservation_phase::consumed_no_pointer
								  : owner->lifecycle_phase,
						 owner->lifecycle_origin_sequence,
						 0U,
						 0U});
					const auto sequences = consume_reader_lifecycle_terminal_slot_locked(
						owner->terminal_sequence_slot);
					if (!sequences.succeeded)
					{
						quarantine_reader_session_locked(
							*owner, sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						session.disarm();
						return sqlite_shm_unexpected(ambiguous());
					}
					owner->terminal_sequence_slot = 0U;
					owner->pending_terminal_sequence = sequences.first;
					prepared_terminals.back().lifecycle_destination_sequence =
						reserved ? sequences.first : owner->lifecycle_destination_sequence;
					prepared_terminals.back().terminal_sequence = sequences.first;
					next_custodies[custody_index].state =
						sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt;
					next_custodies[custody_index].destination_sequence = sequences.first;
					if (runtime_custody_index)
					{
						next_custodies[*runtime_custody_index].state =
							sqlite_shm_reader_custody_state::transferred_to_durable_tombstone;
						next_custodies[*runtime_custody_index].destination_sequence =
							sequences.first;
					}
					reader_session_terminals_.splice(reader_session_terminals_.end(),
													 prepared_terminals);
					if (std::exchange(fail_next_reader_session_terminal_commit_for_testing_, false))
					{
						owner->quarantine_reason =
							sqlite_shm_reader_terminal_quarantine_reason::injected_commit_failure;
						throw reader_session_terminal_commit_injected_failure{};
					}
					reader_custodies_.swap(next_custodies);
					if (reserved)
					{
						reservation->reservation_phase =
							sqlite_shm_reader_attachment_reservation_phase::revoked_no_map;
						reservation->reservation_destination_sequence = sequences.first;
						if (reservation->registry_activity_authority)
						{
							completed_activity->emplace(
								std::move(*reservation->registry_activity_authority));
							reservation->registry_activity_authority.reset();
						}
					}
					reader_sessions_.erase(owner);
					session.disarm();
					reader_last_committed_sequence_ =
						std::max(reader_last_committed_sequence_, sequences.last);
					return {};
				}
				catch (...)
				{
					quarantine_reader_session_terminal_commit(session, exact_group_token);
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
					if (has_live_registry_reader_lineage_locked())
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::deny_before_native_map));
					if (!generation_)
						return sqlite_shm_unexpected(rejection(
							eligibilities_.empty() && writers_.empty()
								? sqlite_shm_lease_rejection_reason::no_live_generation
								: sqlite_shm_lease_rejection_reason::pending_or_eligibility_only,
							sqlite_shm_lease_recovery_action::deny_before_native_map));
					if (std::ranges::any_of(
							reader_attachment_groups_,
							[&request](const reader_attachment_group_record& group)
							{
								const auto& attachment = group.expected;
								return group.registry_bound &&
									group.phase !=
									reader_attachment_group_phase::native_cleanup_confirmed &&
									attachment.family() == request.family &&
									attachment.alias_lifetime() == request.alias_lifetime &&
									attachment.connection_token() == request.connection_token;
							}))
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
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
					if (reader_callback_was_completed_locked(request.callback))
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::stale_token,
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
							   const sqlite_shm_callback_execution_receipt& callback,
							   const bool registry_route = false,
							   const bool typed_group_request = false,
							   const int caller_delete_flag = 0,
							   const int delegated_delete_flag = 0) noexcept
			{
				try
				{
					if (fail_next_reader_operation_mutex_acquire_for_testing_.exchange(
							false, std::memory_order_acq_rel))
						throw reader_operation_mutex_acquire_injected_failure{};
					std::scoped_lock lock{mutex_};
					if (!owns(handoff.state_, handoff.token_))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					const auto group = find_by_token(reader_attachment_groups_, handoff.token_);
					if (group != reader_attachment_groups_.end())
					{
						if (!typed_group_request || caller_delete_flag != 0 ||
							delegated_delete_flag != 0)
							return sqlite_shm_unexpected(rejection(
								sqlite_shm_lease_rejection_reason::invalid_request,
								sqlite_shm_lease_recovery_action::deny_before_native_map));
						if (group->registry_bound != registry_route)
							return sqlite_shm_unexpected(rejection(
								sqlite_shm_lease_rejection_reason::receipt_mismatch,
								sqlite_shm_lease_recovery_action::resubmit_via_bound_route));
						// A deterministic peer quarantine revokes fresh family admission but
						// does not consume another established group's already-owned drain.
						// Emergency/unknown state cannot safely delegate a new native call.
						if (emergency_quarantine_.load(std::memory_order_acquire))
							return sqlite_shm_unexpected(
								rejection(sqlite_shm_lease_rejection_reason::quarantined,
										  sqlite_shm_lease_recovery_action::quarantine_no_retry));
						if (group->generation != handoff.generation_ ||
							group->reservation_phase !=
								sqlite_shm_reader_attachment_reservation_phase::observed_present ||
							!group->observed_identity ||
							group->phase != reader_attachment_group_phase::active)
							return sqlite_shm_unexpected(
								stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
						if (!reader_group_custody_census_is_exact_locked(
								*group, false, false, true))
						{
							quarantine_reader_group_locked(*group);
							handoff.disarm();
							quarantine_locked();
							return sqlite_shm_unexpected(ambiguous());
						}
						const auto blockers =
							reader_unmap_cut_blocker_decision_locked(*group, callback);
						if (blockers == reader_unmap_cut_blocker_decision::ambiguous)
						{
							quarantine_reader_group_locked(*group);
							handoff.disarm();
							quarantine_locked();
							return sqlite_shm_unexpected(ambiguous());
						}
						if (!callback_can_start_locked(callback))
						{
							quarantine_reader_group_locked(*group);
							handoff.disarm();
							quarantine_locked();
							return sqlite_shm_unexpected(ambiguous());
						}
						if (std::exchange(fail_next_reader_unmap_begin_preparation_for_testing_,
										  false))
							throw reader_unmap_begin_preparation_injected_failure{};
						auto prepared_callback = callback;
						auto prepared_state = shared_from_this();
						auto next_custodies = reader_custodies_;
						const auto deferred_cleanup =
							group->existing_group_deferred_cleanup_required;
						const auto handoff_custody = std::find_if(
							next_custodies.begin(),
							next_custodies.end(),
							[&group](const reader_custody_record& custody)
							{
								return custody.kind ==
									sqlite_shm_reader_custody_kind::attachment_group_handoff &&
									custody.owner_token == group->token &&
									custody.state == sqlite_shm_reader_custody_state::live;
							});
						const auto deferred_custody = std::find_if(
							next_custodies.begin(),
							next_custodies.end(),
							[&group](const reader_custody_record& custody)
							{
								return custody.kind ==
									sqlite_shm_reader_custody_kind::normal_or_deferred_unmap &&
									custody.owner_token == group->token &&
									custody.state == sqlite_shm_reader_custody_state::live;
							});
						if ((!deferred_cleanup && handoff_custody == next_custodies.end()) ||
							(deferred_cleanup &&
							 (handoff_custody != next_custodies.end() ||
							  deferred_custody == next_custodies.end())))
						{
							group->phase = reader_attachment_group_phase::terminal_quarantined;
							handoff.disarm();
							quarantine_locked();
							return sqlite_shm_unexpected(ambiguous());
						}
						std::optional<std::size_t> handoff_custody_index;
						std::optional<std::size_t> minted_normal_unmap_index;
						if (!deferred_cleanup)
						{
							handoff_custody_index = static_cast<std::size_t>(
								std::distance(next_custodies.begin(), handoff_custody));
							minted_normal_unmap_index = next_custodies.size();
							next_custodies.emplace_back(
								sqlite_shm_reader_custody_kind::normal_or_deferred_unmap,
								sqlite_shm_reader_custody_state::live,
								group->expected,
								group->token,
								0U,
								0U);
						}
						const auto unmap_cut_index = next_custodies.size();
						next_custodies.emplace_back(
							sqlite_shm_reader_custody_kind::unmap_cut,
							sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt,
							group->expected,
							group->token,
							0U,
							0U);
						const auto waiter_index = next_custodies.size();
						next_custodies.emplace_back(
							sqlite_shm_reader_custody_kind::bounded_waiter_or_continuation,
							sqlite_shm_reader_custody_state::live,
							group->expected,
							group->token,
							0U,
							0U);
						const auto reporter_index = next_custodies.size();
						next_custodies.emplace_back(
							sqlite_shm_reader_custody_kind::terminal_reporter,
							sqlite_shm_reader_custody_state::live,
							group->expected,
							group->token,
							0U,
							0U);
						if (group->unmap_cut_sequence_slot == 0U ||
							group->unmap_terminal_sequence_slot == 0U ||
							group->unmap_cut_sequence_slot == group->unmap_terminal_sequence_slot)
						{
							quarantine_reader_group_locked(*group);
							handoff.disarm();
							quarantine_locked();
							return sqlite_shm_unexpected(ambiguous());
						}
						static_assert(std::is_nothrow_move_constructible_v<
									  sqlite_shm_callback_execution_receipt>);
						group->unmap_callback.emplace(std::move(prepared_callback));
						group->unmap_caller_delete_flag = caller_delete_flag;
						group->unmap_delegated_delete_flag = delegated_delete_flag;
						const auto sequences = consume_reader_lifecycle_terminal_slot_locked(
							group->unmap_cut_sequence_slot);
						if (!sequences.succeeded)
						{
							emergency_quarantine_.store(true, std::memory_order_release);
							quarantine_reader_group_locked(*group);
							handoff.disarm();
							quarantine_locked();
							return sqlite_shm_unexpected(ambiguous());
						}
						group->unmap_cut_sequence_slot = 0U;
						if (handoff_custody_index)
						{
							next_custodies[*handoff_custody_index].state =
								sqlite_shm_reader_custody_state::transferred_to_exact_successor;
							next_custodies[*handoff_custody_index].destination_sequence =
								sequences.first;
							next_custodies[*minted_normal_unmap_index].origin_sequence =
								sequences.first;
						}
						next_custodies[unmap_cut_index].origin_sequence = sequences.first;
						next_custodies[unmap_cut_index].destination_sequence = sequences.first;
						next_custodies[waiter_index].origin_sequence = sequences.first;
						next_custodies[reporter_index].origin_sequence = sequences.first;
						const auto token = handoff.token_;
						const auto generation = handoff.generation_;
						group->phase = blockers == reader_unmap_cut_blocker_decision::none
							? reader_attachment_group_phase::native_cleanup_admitted
							: reader_attachment_group_phase::unmap_cut_sealing;
						group->group_destination_sequence = sequences.first;
						group->unmap_cut_sequence = sequences.first;
						reader_custodies_.swap(next_custodies);
						handoff.disarm();
						reader_last_committed_sequence_ =
							std::max(reader_last_committed_sequence_, sequences.last);
						if (blockers == reader_unmap_cut_blocker_decision::same_thread_or_reentrant)
						{
							quarantine_reader_group_locked(
								*group,
								sequences.first,
								sqlite_shm_reader_terminal_quarantine_reason::peer_quarantine);
							quarantine_locked();
							return sqlite_shm_unexpected(ambiguous());
						}
						if (!reader_group_custody_census_is_exact_locked(
								*group,
								true,
								false,
								blockers == reader_unmap_cut_blocker_decision::other_thread))
						{
							quarantine_reader_group_locked(
								*group,
								sequences.first,
								sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
							quarantine_locked();
							return sqlite_shm_unexpected(ambiguous());
						}
						return sqlite_shm_reader_unmap_obligation{
							std::move(prepared_state),
							sqlite_shm_lease_token_identity{token},
							sqlite_shm_mapping_generation_identity{generation}};
					}
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
					quarantine_reader_unmap_begin(handoff);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_unmap_cut_result>
			poll_reader_unmap_cut(sqlite_shm_reader_unmap_obligation& unmap,
								  const sqlite_shm_callback_execution_receipt& callback,
								  const bool registry_route = false) noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					if (!owns(unmap.state_, unmap.token_) || unmap.composite_close_)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					const auto group = find_by_token(reader_attachment_groups_, unmap.token_);
					if (group != reader_attachment_groups_.end() &&
						group->registry_bound != registry_route)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::resubmit_via_bound_route));
					if (group == reader_attachment_groups_.end() ||
						group->generation != unmap.generation_ ||
						group->reservation_phase !=
							sqlite_shm_reader_attachment_reservation_phase::observed_present ||
						!group->observed_identity || !group->unmap_callback ||
						*group->unmap_callback != callback)
					{
						if (group != reader_attachment_groups_.end())
							quarantine_reader_unmap_locked(
								*group,
								unmap,
								sqlite_shm_reader_terminal_quarantine_reason::presented_invalid);
						return sqlite_shm_unexpected(ambiguous());
					}
					if (group->phase == reader_attachment_group_phase::native_cleanup_admitted)
					{
						if (!reader_group_custody_census_is_exact_locked(*group, true))
						{
							quarantine_reader_unmap_locked(
								*group,
								unmap,
								sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
							return sqlite_shm_unexpected(ambiguous());
						}
						return sqlite_shm_reader_unmap_cut_result{
							sqlite_shm_reader_unmap_cut_progress::native_effect_ready,
							group->generation};
					}
					if (group->phase != reader_attachment_group_phase::unmap_cut_sealing)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));

					const auto blockers =
						reader_unmap_cut_blocker_decision_locked(*group, callback);
					if (blockers == reader_unmap_cut_blocker_decision::ambiguous ||
						blockers == reader_unmap_cut_blocker_decision::same_thread_or_reentrant)
					{
						quarantine_reader_unmap_locked(
							*group,
							unmap,
							sqlite_shm_reader_terminal_quarantine_reason::peer_quarantine);
						return sqlite_shm_unexpected(ambiguous());
					}
					if (blockers == reader_unmap_cut_blocker_decision::other_thread)
					{
						if (!reader_group_custody_census_is_exact_locked(*group, true, false, true))
						{
							quarantine_reader_unmap_locked(
								*group,
								unmap,
								sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
							return sqlite_shm_unexpected(ambiguous());
						}
						return sqlite_shm_reader_unmap_cut_result{
							sqlite_shm_reader_unmap_cut_progress::waiting, group->generation};
					}
					if (!reader_group_custody_census_is_exact_locked(*group, true))
					{
						quarantine_reader_unmap_locked(
							*group,
							unmap,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						return sqlite_shm_unexpected(ambiguous());
					}
					group->phase = reader_attachment_group_phase::native_cleanup_admitted;
					return sqlite_shm_reader_unmap_cut_result{
						sqlite_shm_reader_unmap_cut_progress::native_effect_ready,
						group->generation};
				}
				catch (...)
				{
					quarantine_reader_unmap_terminal(unmap);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<void>
			fail_reader_unmap_cut_wait(sqlite_shm_reader_unmap_obligation& unmap,
									   const sqlite_shm_callback_execution_receipt& callback,
									   const sqlite_shm_retirement_wait_failure failure,
									   const bool registry_route = false) noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					if (!owns(unmap.state_, unmap.token_) || unmap.composite_close_)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					const auto group = find_by_token(reader_attachment_groups_, unmap.token_);
					if (group != reader_attachment_groups_.end() &&
						group->registry_bound != registry_route)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::resubmit_via_bound_route));
					if (group == reader_attachment_groups_.end() ||
						group->generation != unmap.generation_ ||
						group->phase != reader_attachment_group_phase::unmap_cut_sealing ||
						!group->unmap_callback || *group->unmap_callback != callback ||
						(failure != sqlite_shm_retirement_wait_failure::timeout &&
						 failure != sqlite_shm_retirement_wait_failure::unknown) ||
						!reader_group_custody_census_is_exact_locked(*group, true, false, true))
					{
						if (group != reader_attachment_groups_.end())
							quarantine_reader_unmap_locked(
								*group,
								unmap,
								sqlite_shm_reader_terminal_quarantine_reason::presented_invalid);
						return sqlite_shm_unexpected(ambiguous());
					}
					quarantine_reader_unmap_locked(
						*group,
						unmap,
						sqlite_shm_reader_terminal_quarantine_reason::native_non_ok_or_unknown);
					return sqlite_shm_unexpected(ambiguous());
				}
				catch (...)
				{
					quarantine_reader_unmap_terminal(unmap);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_unmap_terminal_result>
			complete_reader_unmap_exact(
				sqlite_shm_reader_unmap_obligation& unmap,
				const sqlite_shm_verified_reader_unmap_terminal_receipt& receipt,
				std::optional<sqlite_shm_reader_attachment_authority>* completed_activity = nullptr,
				const std::uint64_t registry_open_token = 0U,
				const std::shared_ptr<sqlite_shm_reader_open_lineage_seal>* seal = nullptr,
				const sqlite_shm_reader_open_epoch_binding* binding = nullptr) noexcept
			{
				if (emergency_quarantine_.load(std::memory_order_acquire) ||
					unmap.terminal_presentation_disabled_)
				{
					if (owns(unmap.state_, unmap.token_))
						unmap.disable_terminal_presentation();
					return sqlite_shm_unexpected(ambiguous());
				}
				try
				{
					if (fail_next_reader_operation_mutex_acquire_for_testing_.exchange(
							false, std::memory_order_acq_rel))
						throw reader_operation_mutex_acquire_injected_failure{};
					std::scoped_lock lock{mutex_};
					if (!owns(unmap.state_, unmap.token_))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					const auto group = find_by_token(reader_attachment_groups_, unmap.token_);
					if (group == reader_attachment_groups_.end())
					{
						unmap.disarm();
						emergency_quarantine_.store(true, std::memory_order_release);
						quarantine_locked();
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto registry_route = completed_activity != nullptr;
					const auto has_open_presentation =
						registry_open_token != 0U || seal != nullptr || binding != nullptr;
					if (group->registry_bound != registry_route ||
						(registry_route && completed_activity->has_value()) ||
						(unmap.composite_close_ != has_open_presentation) ||
						(unmap.composite_close_ !=
						 (group->composite_close_owner_token != 0U &&
						  group->composite_close_registry_open_token != 0U &&
						  group->composite_close_cut_sequence != 0U)))
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::resubmit_via_bound_route));
					if (unmap.composite_close_)
					{
						const auto open =
							std::find_if(registry_reader_opens_.begin(),
										 registry_reader_opens_.end(),
										 [registry_open_token,
										  &group](const registry_reader_open_record& candidate)
										 {
											 return candidate.token == registry_open_token &&
												 candidate.close_owner_token ==
												 group->composite_close_owner_token;
										 });
						const auto exact_open = seal != nullptr && *seal && binding != nullptr &&
							valid_reader_open_epoch_binding(*binding) &&
							registry_open_token == group->composite_close_registry_open_token &&
							open != registry_reader_opens_.end() &&
							open->seal.get() == seal->get() && open->binding == *binding &&
							binding->family == family_ &&
							(*seal)->authority_valid.load(std::memory_order_acquire) &&
							group->expected.registry_open_token() == open->token &&
							reader_attachment_matches_open_epoch_binding(group->expected,
																		 open->binding) &&
							open->close_phase ==
								sqlite_shm_reader_connection_close_phase::close_admitted &&
							open->close_route ==
								sqlite_shm_reader_close_route::close_after_confirmed_unmap;
						if (!exact_open)
						{
							quarantine_reader_group_locked(
								*group,
								0U,
								sqlite_shm_reader_terminal_quarantine_reason::presented_invalid);
							if (open != registry_reader_opens_.end())
								quarantine_reader_open_locked(
									*open,
									0U,
									sqlite_shm_reader_terminal_quarantine_reason::
										presented_invalid);
							quarantine_locked();
							unmap.disarm();
							return sqlite_shm_unexpected(ambiguous());
						}
					}
					if (group->phase == reader_attachment_group_phase::unmap_cut_sealing)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::retiring,
									  sqlite_shm_lease_recovery_action::
										  await_complete_attachment_gate_boundary));
					auto prepared_receipt = receipt;
					auto result_effect = receipt.native_effect_receipt();
					auto result_latch = receipt.latch_reset_receipt();
					const auto result_evidence_kind = receipt.evidence_kind();
					const auto result_native_status = receipt.native_status();
					const auto receipt_state = prepared_receipt.state_.lock();
					const auto replayed_effect = prepared_receipt.native_effect_receipt() &&
						(reader_unmap_evidence_identity_seen_locked(
							 *prepared_receipt.native_effect_receipt(), group->token) ||
						 reader_callback_invocation_was_seen_locked(
							 *prepared_receipt.native_effect_receipt()) ||
						 reader_session_terminal_identity_seen_locked(
							 *prepared_receipt.native_effect_receipt()));
					const auto replayed_latch = prepared_receipt.latch_reset_receipt() &&
						(reader_unmap_evidence_identity_seen_locked(
							 *prepared_receipt.latch_reset_receipt(), group->token) ||
						 reader_callback_invocation_was_seen_locked(
							 *prepared_receipt.latch_reset_receipt()) ||
						 reader_session_terminal_identity_seen_locked(
							 *prepared_receipt.latch_reset_receipt()));
					const auto aliased_effect_roles = prepared_receipt.native_effect_receipt() &&
						prepared_receipt.latch_reset_receipt() &&
						*prepared_receipt.native_effect_receipt() ==
							*prepared_receipt.latch_reset_receipt();
					if (!receipt_state || receipt_state.get() != this ||
						prepared_receipt.token_ != unmap.token_ ||
						prepared_receipt.generation_ != unmap.generation_ ||
						!valid_reader_unmap_terminal_receipt(prepared_receipt) ||
						!group->unmap_callback ||
						*group->unmap_callback != prepared_receipt.callback() ||
						group->unmap_caller_delete_flag != prepared_receipt.caller_delete_flag() ||
						group->unmap_delegated_delete_flag !=
							prepared_receipt.delegated_delete_flag() ||
						replayed_effect || replayed_latch || aliased_effect_roles)
					{
						quarantine_reader_unmap_locked(*group, unmap);
						return sqlite_shm_unexpected(ambiguous());
					}

					const auto exact_ok = prepared_receipt.evidence_kind() ==
							sqlite_shm_reader_unmap_evidence_kind::exact_native_result &&
						prepared_receipt.native_status() &&
						*prepared_receipt.native_status() ==
							static_cast<int>(sqlite_native_map_status::ok);
					static_assert(std::is_nothrow_move_constructible_v<
								  sqlite_shm_verified_reader_unmap_terminal_receipt>);
					group->unmap_terminal_receipt.emplace(std::move(prepared_receipt));
					if (std::exchange(fail_next_reader_unmap_post_receipt_state_for_testing_,
									  false))
					{
						quarantine_reader_unmap_locked(
							*group,
							unmap,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						return sqlite_shm_unexpected(ambiguous());
					}

					if (group->generation != unmap.generation_ ||
						group->reservation_phase !=
							sqlite_shm_reader_attachment_reservation_phase::observed_present ||
						!group->observed_identity ||
						group->phase != reader_attachment_group_phase::native_cleanup_admitted ||
						!generation_ || generation_->value != unmap.generation_ ||
						generation_->handoff_count == 0U ||
						(group->registry_bound != group->registry_activity_authority.has_value()) ||
						(group->registry_activity_authority &&
						 !group->registry_activity_authority->retains_exact_owned_drain_lifetimes(
							 group->expected)) ||
						!reader_group_custody_census_is_exact_locked(
							*group, true, unmap.composite_close_))
					{
						quarantine_reader_unmap_locked(
							*group,
							unmap,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						return sqlite_shm_unexpected(ambiguous());
					}

					auto next_custodies = reader_custodies_;
					const auto custody_index =
						[&next_custodies, &group](
							const sqlite_shm_reader_custody_kind kind) -> std::optional<std::size_t>
					{
						const auto found = std::find_if(
							next_custodies.begin(),
							next_custodies.end(),
							[kind, &group](const reader_custody_record& custody)
							{
								return custody.kind == kind &&
									custody.owner_token == group->token &&
									custody.state == sqlite_shm_reader_custody_state::live;
							});
						if (found == next_custodies.end())
							return std::nullopt;
						return static_cast<std::size_t>(
							std::distance(next_custodies.begin(), found));
					};
					const auto normal_unmap =
						custody_index(sqlite_shm_reader_custody_kind::normal_or_deferred_unmap);
					const auto composite_close = std::find_if(
						next_custodies.begin(),
						next_custodies.end(),
						[&group](const reader_custody_record& custody)
						{
							return custody.kind ==
								sqlite_shm_reader_custody_kind::close_cut_or_composite &&
								custody.owner_token == group->composite_close_owner_token &&
								custody.attachment == group->expected && custody.open_epoch &&
								reader_attachment_matches_open_epoch_binding(group->expected,
																			 *custody.open_epoch) &&
								custody.state == sqlite_shm_reader_custody_state::live &&
								custody.origin_sequence == group->composite_close_cut_sequence &&
								custody.destination_sequence == 0U;
						});
					const auto composite_close_index = composite_close == next_custodies.end()
						? std::optional<std::size_t>{}
						: std::optional<std::size_t>{static_cast<std::size_t>(
							  std::distance(next_custodies.begin(), composite_close))};
					const auto generation_count =
						custody_index(sqlite_shm_reader_custody_kind::generation_group_count);
					const auto exact_attachment =
						custody_index(sqlite_shm_reader_custody_kind::exact_present_attachment);
					const auto bounded_waiter = custody_index(
						sqlite_shm_reader_custody_kind::bounded_waiter_or_continuation);
					const auto terminal_reporter =
						custody_index(sqlite_shm_reader_custody_kind::terminal_reporter);
					const auto runtime_pin = group->registry_bound
						? custody_index(
							  sqlite_shm_reader_custody_kind::
								  runtime_vfs_namespace_generation_native_mapping_lifetime_pin)
						: std::optional<std::size_t>{};
					const auto sealed_unmap_cut = std::ranges::any_of(
						next_custodies,
						[&group](const reader_custody_record& custody)
						{
							return custody.kind == sqlite_shm_reader_custody_kind::unmap_cut &&
								custody.owner_token == group->token &&
								custody.state ==
								sqlite_shm_reader_custody_state::
									consumed_with_exact_terminal_receipt &&
								custody.origin_sequence != 0U &&
								custody.destination_sequence == custody.origin_sequence;
						});
					if ((!unmap.composite_close_ && !normal_unmap) || !bounded_waiter ||
						!terminal_reporter ||
						(unmap.composite_close_ && (!composite_close_index || normal_unmap)) ||
						!sealed_unmap_cut || !generation_count || !exact_attachment ||
						(group->registry_bound && !runtime_pin))
					{
						quarantine_reader_unmap_locked(
							*group,
							unmap,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto sequences = consume_reader_lifecycle_terminal_slot_locked(
						group->unmap_terminal_sequence_slot);
					if (!sequences.succeeded)
					{
						quarantine_reader_unmap_locked(
							*group,
							unmap,
							sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
						return sqlite_shm_unexpected(ambiguous());
					}
					group->unmap_terminal_sequence_slot = 0U;
					group->unmap_terminal_sequence = sequences.first;
					const auto injected_failure =
						std::exchange(fail_next_reader_unmap_terminal_commit_for_testing_, false);
					if (injected_failure)
					{
						group->quarantine_reason =
							sqlite_shm_reader_terminal_quarantine_reason::injected_commit_failure;
						quarantine_reader_unmap_locked(
							*group,
							unmap,
							sqlite_shm_reader_terminal_quarantine_reason::injected_commit_failure);
						return sqlite_shm_unexpected(ambiguous());
					}
					const auto retired_confirmed = exact_ok && !injected_failure;
					const auto custody_terminal = retired_confirmed
						? sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt
						: sqlite_shm_reader_custody_state::transferred_to_durable_tombstone;
					for (const auto index : {*generation_count, *exact_attachment})
					{
						next_custodies[index].state = custody_terminal;
						next_custodies[index].destination_sequence = sequences.first;
					}
					if (normal_unmap)
					{
						next_custodies[*normal_unmap].state = custody_terminal;
						next_custodies[*normal_unmap].destination_sequence = sequences.first;
					}
					for (const auto index : {bounded_waiter, terminal_reporter})
					{
						if (!index)
							continue;
						next_custodies[*index].state = custody_terminal;
						next_custodies[*index].destination_sequence = sequences.first;
					}
					if (composite_close_index)
					{
						if (retired_confirmed)
							next_custodies[*composite_close_index].attachment.reset();
						else
						{
							next_custodies[*composite_close_index].state = custody_terminal;
							next_custodies[*composite_close_index].destination_sequence =
								sequences.first;
						}
					}
					if (runtime_pin)
					{
						next_custodies[*runtime_pin].state =
							sqlite_shm_reader_custody_state::transferred_to_durable_tombstone;
						next_custodies[*runtime_pin].destination_sequence = sequences.first;
					}
					group->group_destination_sequence = sequences.first;
					group->reservation_destination_sequence = sequences.first;
					group->unmap_terminal_sequence = sequences.first;
					group->unmap_callback.reset();
					unmap.disarm();
					reader_custodies_.swap(next_custodies);
					reader_last_committed_sequence_ =
						std::max(reader_last_committed_sequence_, sequences.last);

					const auto terminal_kind = retired_confirmed
						? sqlite_shm_reader_unmap_terminal_kind::retired_confirmed
						: sqlite_shm_reader_unmap_terminal_kind::terminal_quarantined;
					if (retired_confirmed)
					{
						group->reservation_phase =
							sqlite_shm_reader_attachment_reservation_phase::retired_confirmed;
						group->phase = reader_attachment_group_phase::native_cleanup_confirmed;
						if (group->registry_activity_authority)
						{
							completed_activity->emplace(
								std::move(*group->registry_activity_authority));
							group->registry_activity_authority.reset();
						}
						--generation_->handoff_count;
						if (generation_->phase == sqlite_shm_mapping_generation_phase::retired &&
							generation_->handoff_count == 0U)
							generation_.reset();
					}
					else
					{
						group->quarantine_reason =
							sqlite_shm_reader_terminal_quarantine_reason::native_non_ok_or_unknown;
						group->reservation_phase =
							sqlite_shm_reader_attachment_reservation_phase::terminal_quarantined;
						group->phase = reader_attachment_group_phase::terminal_quarantined;
						if (unmap.composite_close_)
						{
							const auto open =
								std::find_if(registry_reader_opens_.begin(),
											 registry_reader_opens_.end(),
											 [&group](const registry_reader_open_record& candidate)
											 {
												 return candidate.token ==
													 group->composite_close_registry_open_token &&
													 candidate.close_owner_token ==
													 group->composite_close_owner_token;
											 });
							if (open != registry_reader_opens_.end())
								quarantine_reader_open_locked(
									*open,
									sequences.first,
									sqlite_shm_reader_terminal_quarantine_reason::
										native_non_ok_or_unknown);
						}
						quarantine_locked();
					}
					return sqlite_shm_reader_unmap_terminal_result{terminal_kind,
																   result_evidence_kind,
																   result_native_status,
																   std::move(result_effect),
																   std::move(result_latch)};
				}
				catch (...)
				{
					quarantine_reader_unmap_terminal(unmap);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<void>
			complete_reader_unmap(sqlite_shm_reader_unmap_obligation& unmap,
								  const sqlite_shm_callback_execution_receipt& callback,
								  const sqlite_shm_native_cleanup_outcome outcome) noexcept
			{
				if (emergency_quarantine_.load(std::memory_order_acquire) ||
					unmap.terminal_presentation_disabled_)
				{
					if (owns(unmap.state_, unmap.token_))
						unmap.disable_terminal_presentation();
					return sqlite_shm_unexpected(ambiguous());
				}
				try
				{
					if (fail_next_reader_operation_mutex_acquire_for_testing_.exchange(
							false, std::memory_order_acq_rel))
						throw reader_operation_mutex_acquire_injected_failure{};
					std::scoped_lock lock{mutex_};
					if (!owns(unmap.state_, unmap.token_))
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					if (std::exchange(fail_next_reader_coarse_unmap_terminal_for_testing_, false))
						throw reader_coarse_unmap_terminal_injected_failure{};
					if (const auto group = find_by_token(reader_attachment_groups_, unmap.token_);
						group != reader_attachment_groups_.end())
					{
						// A group cut already admits exactly one native call. Coarse terminal
						// evidence cannot authorize retirement or preserve retry authority.
						quarantine_reader_unmap_locked(
							*group,
							unmap,
							sqlite_shm_reader_terminal_quarantine_reason::presented_invalid);
						return sqlite_shm_unexpected(ambiguous());
					}
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
					quarantine_reader_unmap_terminal(unmap);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_unmap_terminal_result>
			complete_registry_reader_live_close_unmap(
				const std::uint64_t registry_open_token,
				const std::shared_ptr<sqlite_shm_reader_open_lineage_seal>& seal,
				const sqlite_shm_reader_open_epoch_binding& binding,
				sqlite_shm_reader_live_close_obligation& close,
				const sqlite_shm_verified_reader_unmap_terminal_receipt& receipt,
				std::optional<sqlite_shm_reader_attachment_authority>& completed_activity) noexcept
			{
				try
				{
					if (close.terminal_presentation_disabled_)
						return sqlite_shm_unexpected(ambiguous());
					if (!close.state_ || close.state_.get() != this || completed_activity)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::invalid_request,
									  sqlite_shm_lease_recovery_action::quarantine_no_retry));
					if (registry_open_token != close.registry_open_token_)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::resubmit_via_bound_route));
					if (close.phase_ ==
						sqlite_shm_reader_live_close_obligation::phase::unmap_waiting)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::retiring,
									  sqlite_shm_lease_recovery_action::
										  await_complete_attachment_gate_boundary));
					if (close.phase_ !=
						sqlite_shm_reader_live_close_obligation::phase::unmap_admitted)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::invalid_request,
									  sqlite_shm_lease_recovery_action::quarantine_no_retry));
					sqlite_shm_reader_unmap_obligation unmap{
						close.state_,
						sqlite_shm_lease_token_identity{close.group_token_},
						sqlite_shm_mapping_generation_identity{close.generation_},
						true};
					auto result = complete_reader_unmap_exact(
						unmap, receipt, &completed_activity, registry_open_token, &seal, &binding);
					if (!result)
					{
						if (unmap.token_ != 0U)
						{
							close.terminal_presentation_disabled_ =
								unmap.terminal_presentation_disabled_;
							unmap.disarm();
						}
						else
							close.disarm();
						return sqlite_shm_unexpected(result.error());
					}
					if (result->kind() == sqlite_shm_reader_unmap_terminal_kind::retired_confirmed)
						close.phase_ = sqlite_shm_reader_live_close_obligation::phase::close_ready;
					else
						close.disarm();
					return result;
				}
				catch (...)
				{
					abandon_reader_live_close(close);
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_reader_close_terminal_result>
			complete_registry_reader_live_close(
				const std::uint64_t registry_open_token,
				const std::shared_ptr<sqlite_shm_reader_open_lineage_seal>& seal,
				const sqlite_shm_reader_open_epoch_binding& binding,
				sqlite_shm_reader_live_close_obligation& close,
				const sqlite_shm_verified_reader_close_terminal_receipt& receipt) noexcept
			{
				try
				{
					if (close.terminal_presentation_disabled_ || !close.state_ ||
						close.state_.get() != this || close.close_owner_token_ == 0U ||
						close.group_token_ == 0U)
						return sqlite_shm_unexpected(
							stale_token(sqlite_shm_lease_recovery_action::quarantine_no_retry));
					if (registry_open_token != close.registry_open_token_)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
									  sqlite_shm_lease_recovery_action::resubmit_via_bound_route));
					if (close.phase_ != sqlite_shm_reader_live_close_obligation::phase::close_ready)
						return sqlite_shm_unexpected(
							rejection(sqlite_shm_lease_rejection_reason::retiring,
									  sqlite_shm_lease_recovery_action::
										  await_complete_attachment_gate_boundary));
					sqlite_shm_reader_close_obligation direct_close{
						close.state_,
						sqlite_shm_lease_token_identity{close.close_owner_token_},
						close.registry_open_token_,
						sqlite_shm_reader_close_route::close_after_confirmed_unmap};
					auto result = complete_registry_reader_close(
						registry_open_token, seal, binding, direct_close, receipt);
					if (!result)
					{
						if (direct_close.owner_token_ != 0U)
						{
							close.terminal_presentation_disabled_ =
								direct_close.terminal_presentation_disabled_;
							direct_close.disarm();
						}
						else
							close.disarm();
						return sqlite_shm_unexpected(result.error());
					}
					close.disarm();
					return result;
				}
				catch (...)
				{
					abandon_reader_live_close(close);
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
					output.reader_inflight_count =
						static_cast<std::size_t>(std::ranges::count_if(
							readers_,
							[](const reader_record& reader)
							{
								return reader.phase != reader_phase::cleanup_obligation &&
									reader.phase != reader_phase::terminal_quarantined;
							})) +
						static_cast<std::size_t>(std::ranges::count_if(
							reader_attachment_maps_,
							[](const reader_attachment_map_record& map)
							{
								return map.phase != reader_phase::terminal_quarantined;
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
						std::ranges::count_if(reader_attachment_maps_,
											  [](const reader_attachment_map_record& map)
											  {
												  return map.phase ==
													  reader_phase::terminal_quarantined;
											  }) +
						std::ranges::count_if(handoffs_,
											  [](const handoff_record& handoff)
											  {
												  return handoff.phase != handoff_phase::active;
											  }) +
						std::ranges::count_if(
							reader_attachment_groups_,
							[](const reader_attachment_group_record& group)
							{
								return group.phase ==
									reader_attachment_group_phase::unmap_cut_sealing ||
									group.phase ==
									reader_attachment_group_phase::native_cleanup_admitted ||
									group.phase ==
									reader_attachment_group_phase::terminal_quarantined;
							}));
					const auto live_group_count = static_cast<std::size_t>(std::ranges::count_if(
						reader_attachment_groups_,
						[](const reader_attachment_group_record& group)
						{
							return reader_reservation_has_unresolved_group(group);
						}));
					output.reader_handoff_count = handoffs_.size() + live_group_count;
					output.reader_attachment_group_count = live_group_count;
					for (const auto& group : reader_attachment_groups_)
					{
						if (group.reservation_phase ==
								sqlite_shm_reader_attachment_reservation_phase::observed_present &&
							(group.phase == reader_attachment_group_phase::active ||
							 group.phase == reader_attachment_group_phase::unmap_cut_sealing ||
							 group.phase == reader_attachment_group_phase::native_cleanup_admitted))
							output.reader_attachment_live_member_count += group.members.size();
						output.reader_attachment_audit_count += group.audits.size();
					}
					output.reader_session_reservation_count =
						static_cast<std::size_t>(std::ranges::count_if(
							reader_sessions_,
							[](const reader_session_record& session)
							{
								return session.phase ==
									reader_session_record_phase::reserved_for_first_map;
							}));
					output.reader_session_owner_count =
						static_cast<std::size_t>(std::ranges::count_if(
							reader_sessions_,
							[](const reader_session_record& session)
							{
								return session.phase ==
									reader_session_record_phase::active_group_owner;
							}));
					output.reader_session_terminal_count = reader_session_terminals_.size();
					output.reader_attachment_zero_effect_terminal_count =
						reader_attachment_zero_effect_terminals_.size();
					output.reader_opaque_attachment_uncertainty_count =
						reader_opaque_attachment_uncertainties_.size();
					output.reader_predecessor_map_terminal_count =
						reader_predecessor_map_terminals_.size();
					output.reader_predecessor_route_active_count =
						static_cast<std::size_t>(std::ranges::count_if(
							reader_attachment_groups_,
							[](const reader_attachment_group_record& group)
							{
								return group.reservation_phase ==
									sqlite_shm_reader_attachment_reservation_phase::
										predecessor_route_active;
							}));
					output.reader_predecessor_route_retired_count =
						static_cast<std::size_t>(std::ranges::count_if(
							reader_attachment_groups_,
							[](const reader_attachment_group_record& group)
							{
								return group.reservation_phase ==
									sqlite_shm_reader_attachment_reservation_phase::
										predecessor_route_retired_confirmed;
							}));
					output.reader_existing_group_deferred_cleanup_count = static_cast<
						std::size_t>(std::ranges::count_if(
						reader_attachment_groups_,
						[](const reader_attachment_group_record& group)
						{
							return group.existing_group_deferred_cleanup_required &&
								group.reservation_phase ==
								sqlite_shm_reader_attachment_reservation_phase::observed_present;
						}));
					output.reader_attachment_revoked_no_map_count =
						static_cast<std::size_t>(std::ranges::count_if(
							reader_attachment_groups_,
							[](const reader_attachment_group_record& group)
							{
								return group.reservation_phase ==
									sqlite_shm_reader_attachment_reservation_phase::revoked_no_map;
							}));
					output.reader_unpublished_cleanup_admitted_count =
						static_cast<std::size_t>(std::ranges::count_if(
							reader_attachment_groups_,
							[](const reader_attachment_group_record& group)
							{
								return group.reservation_phase ==
									sqlite_shm_reader_attachment_reservation_phase::
										unpublished_cleanup_admitted;
							}));
					output.reader_unpublished_cleanup_confirmed_count =
						static_cast<std::size_t>(std::ranges::count_if(
							reader_attachment_groups_,
							[](const reader_attachment_group_record& group)
							{
								return group.reservation_phase ==
									sqlite_shm_reader_attachment_reservation_phase::
										unpublished_cleanup_confirmed;
							}));
					output.reader_logical_ack_awaiting_count =
						static_cast<std::size_t>(std::ranges::count_if(
							reader_attachment_groups_,
							[](const reader_attachment_group_record& group)
							{
								return group.logical_ack_phase ==
									sqlite_shm_reader_logical_ack_phase::awaiting_sqlite_ack;
							}));
					output.reader_registry_bound_group_count =
						static_cast<std::size_t>(std::ranges::count_if(
							reader_attachment_groups_,
							[](const reader_attachment_group_record& group)
							{
								return group.registry_bound &&
									reader_reservation_has_unresolved_group(group);
							}));
					output.reader_registry_bound_session_count = static_cast<std::size_t>(
						std::ranges::count_if(reader_sessions_,
											  [](const reader_session_record& session)
											  {
												  return session.registry_bound;
											  }));
					output.reader_registry_open_count = registry_reader_opens_.size();
					output.reader_open_close_owner_count =
						static_cast<std::size_t>(std::ranges::count_if(
							registry_reader_opens_,
							[](const registry_reader_open_record& open)
							{
								return open.close_owner_token != 0U &&
									open.close_phase ==
									sqlite_shm_reader_connection_close_phase::open;
							}));
					output.reader_close_admitted_count =
						static_cast<std::size_t>(std::ranges::count_if(
							registry_reader_opens_,
							[](const registry_reader_open_record& open)
							{
								return open.close_phase ==
									sqlite_shm_reader_connection_close_phase::close_admitted;
							}));
					output.reader_close_terminal_count = reader_close_terminals_.size();
					output.reader_open_close_tombstone_count = reader_open_close_tombstones_.size();
					output.reader_registry_activity_authority_count =
						static_cast<std::size_t>(std::ranges::count_if(
							reader_attachment_groups_,
							[](const reader_attachment_group_record& group)
							{
								return group.registry_activity_authority.has_value();
							})) +
						static_cast<std::size_t>(std::ranges::count_if(
							reader_attachment_maps_,
							[](const reader_attachment_map_record& map)
							{
								return map.registry_predelegate_authority.has_value();
							})) +
						static_cast<std::size_t>(std::ranges::count_if(
							reader_opaque_attachment_uncertainties_,
							[](const reader_opaque_attachment_uncertainty_record& opaque)
							{
								return opaque.registry_predelegate_authority.has_value();
							}));
					output.reader_registry_activity_liveness_lost_count =
						reader_registry_liveness_lost_count_locked();
					if (output.reader_registry_activity_liveness_lost_count != 0U)
					{
						output.quarantined = true;
						output.phase = sqlite_shm_mapping_generation_phase::quarantined;
					}
					output.reader_admission_visible = !output.quarantined &&
						output.reader_logical_ack_awaiting_count == 0U &&
						(std::ranges::any_of(
							 reader_attachment_groups_,
							 [](const reader_attachment_group_record& group)
							 {
								 return group.reservation_phase ==
									 sqlite_shm_reader_attachment_reservation_phase::
										 observed_present &&
									 group.observed_identity &&
									 group.phase == reader_attachment_group_phase::active &&
									 !group.existing_group_deferred_cleanup_required;
							 }) ||
						 (generation_ &&
						  generation_->phase == sqlite_shm_mapping_generation_phase::live &&
						  active_holder_count_locked() != 0U));
				}
				catch (...)
				{
					output.phase = sqlite_shm_mapping_generation_phase::quarantined;
					output.quarantined = true;
					output.reader_admission_visible = false;
				}
				return output;
			}

			[[nodiscard]] sqlite_shm_reader_lifecycle_test_view
			reader_lifecycle_view_for_testing() const
			{
				std::scoped_lock lock{mutex_};
				sqlite_shm_reader_lifecycle_test_view output;
				output.sequence_source_identity = reader_lifecycle_sequences_
					? reader_lifecycle_sequences_->identity_for_testing()
					: nullptr;
				output.last_issued_sequence = reader_lifecycle_sequences_
					? reader_lifecycle_sequences_->observed_last_issued()
					: 0U;
				output.last_committed_sequence = reader_last_committed_sequence_;
				output.outstanding_terminal_permit_count =
					reader_lifecycle_sequences_ && reader_lifecycle_sequences_->state_
					? reader_lifecycle_sequences_->state_
						  ->observed_outstanding_terminal_slot_count()
					: 0U;
				if (reader_lifecycle_sequences_ && reader_lifecycle_sequences_->state_)
					output.outstanding_terminal_permit_slots =
						reader_lifecycle_sequences_->state_->observed_terminal_slots();
				output.sequence_source_exhausted = !reader_lifecycle_sequences_ ||
					!reader_lifecycle_sequences_->state_ ||
					reader_lifecycle_sequences_->state_->observed_exhausted();

				const auto add_event = [&output](const std::uint64_t sequence,
												 const sqlite_shm_reader_lifecycle_event_kind kind,
												 const std::uint64_t owner_token)
				{
					if (sequence != 0U)
						output.events.push_back({sequence, kind, owner_token});
				};
				output.open_epoch_close_compact_tombstone_count =
					reader_open_close_tombstones_.size();
				for (const auto& open : registry_reader_opens_)
				{
					output.open_epochs.push_back({open.token,
												  open.binding,
												  open.close_owner_token,
												  open.close_phase,
												  open.close_origin_sequence,
												  open.close_cut_sequence_slot,
												  open.close_terminal_sequence_slot,
												  open.initial_close_cut_sequence_slot,
												  open.initial_close_terminal_sequence_slot,
												  open.close_route,
												  open.close_cut_sequence,
												  open.close_terminal_sequence});
					add_event(open.close_cut_sequence,
							  sqlite_shm_reader_lifecycle_event_kind::close_cut,
							  open.close_owner_token);
					add_event(open.close_terminal_sequence,
							  sqlite_shm_reader_lifecycle_event_kind::cut_terminal_commit,
							  open.close_owner_token);
				}
				for (const auto& terminal : reader_close_terminals_)
				{
					output.close_terminals.push_back({terminal.registry_open_token,
													  terminal.binding,
													  terminal.close_owner_token,
													  terminal.route,
													  terminal.kind,
													  terminal.receipt.evidence_kind(),
													  terminal.receipt.native_status(),
													  terminal.outward_status,
													  terminal.receipt.native_effect_receipt(),
													  terminal.receipt.callback(),
													  true,
													  terminal.quarantine_reason,
													  terminal.terminal_sequence});
					add_event(terminal.cut_sequence,
							  sqlite_shm_reader_lifecycle_event_kind::close_cut,
							  terminal.close_owner_token);
					add_event(terminal.terminal_sequence,
							  sqlite_shm_reader_lifecycle_event_kind::cut_terminal_commit,
							  terminal.close_owner_token);
				}
				for (const auto& tombstone : reader_open_close_tombstones_)
				{
					add_event(tombstone.close_cut_sequence,
							  sqlite_shm_reader_lifecycle_event_kind::close_cut,
							  tombstone.close_owner_token);
					add_event(tombstone.terminal_sequence,
							  sqlite_shm_reader_lifecycle_event_kind::cut_terminal_commit,
							  tombstone.close_owner_token);
				}
				for (const auto& reservation : reader_attachment_groups_)
				{
					++output.attachment_reservation_phase_counts[static_cast<std::size_t>(
						reservation.reservation_phase)];
					output.attachment_reservations.push_back(
						{reservation.expected,
						 reservation.reservation_phase,
						 reservation.reservation_origin_sequence,
						 reservation.reservation_destination_sequence,
						 reservation.observed_identity.has_value(),
						 reservation.unpublished_cleanup_receipt
							 ? std::optional{reservation.unpublished_cleanup_receipt->kind()}
							 : std::nullopt,
						 reservation.logical_ack_phase,
						 reservation.unpublished_cleanup_cut_sequence,
						 reservation.unpublished_cleanup_terminal_sequence,
						 reservation.logical_ack_sequence,
						 reservation.unpublished_cleanup_session_origin_sequence,
						 reservation.unpublished_cleanup_session_terminal_sequence});
					const auto compactable = reader_reservation_is_compactable(reservation);
					output.compact_tombstone_count += static_cast<std::size_t>(compactable);
					if (reservation.observed_identity)
					{
						auto phase = sqlite_shm_reader_attachment_group_phase::active;
						switch (reservation.phase)
						{
							case reader_attachment_group_phase::active:
								phase = sqlite_shm_reader_attachment_group_phase::active;
								break;
							case reader_attachment_group_phase::unmap_cut_sealing:
								phase = sqlite_shm_reader_attachment_group_phase::unmap_cut_sealing;
								break;
							case reader_attachment_group_phase::native_cleanup_admitted:
								phase =
									sqlite_shm_reader_attachment_group_phase::native_unmap_admitted;
								break;
							case reader_attachment_group_phase::native_cleanup_confirmed:
								phase = sqlite_shm_reader_attachment_group_phase::
									native_unmap_confirmed;
								break;
							case reader_attachment_group_phase::terminal_quarantined:
								phase =
									sqlite_shm_reader_attachment_group_phase::terminal_quarantined;
								break;
						}
						output.attachment_groups.push_back(
							{reservation.expected,
							 phase,
							 reservation.group_origin_sequence,
							 reservation.group_destination_sequence,
							 reservation.unmap_cut_sequence_slot,
							 reservation.unmap_terminal_sequence_slot});
					}
					add_event(reservation.unmap_cut_sequence,
							  sqlite_shm_reader_lifecycle_event_kind::unmap_cut,
							  reservation.token);
					add_event(reservation.unmap_terminal_sequence,
							  sqlite_shm_reader_lifecycle_event_kind::cut_terminal_commit,
							  reservation.token);
					add_event(reservation.unpublished_cleanup_session_origin_sequence,
							  sqlite_shm_reader_lifecycle_event_kind::session_start_admission,
							  reservation.unpublished_cleanup_session_token);
					add_event(reservation.unpublished_cleanup_map_admission_sequence,
							  sqlite_shm_reader_lifecycle_event_kind::map_admission,
							  reservation.unpublished_cleanup_map_token);
					add_event(reservation.unpublished_cleanup_map_terminal_sequence,
							  sqlite_shm_reader_lifecycle_event_kind::map_terminal,
							  reservation.unpublished_cleanup_map_token);
					add_event(reservation.unpublished_cleanup_session_terminal_sequence,
							  sqlite_shm_reader_lifecycle_event_kind::use_session_terminal,
							  reservation.unpublished_cleanup_session_token);
					add_event(reservation.unpublished_cleanup_cut_sequence,
							  sqlite_shm_reader_lifecycle_event_kind::unmap_cut,
							  reservation.token);
					add_event(reservation.unpublished_cleanup_terminal_sequence,
							  sqlite_shm_reader_lifecycle_event_kind::cut_terminal_commit,
							  reservation.token);
					add_event(reservation.logical_ack_sequence,
							  sqlite_shm_reader_lifecycle_event_kind::cut_terminal_commit,
							  reservation.token);
					add_event(reservation.composite_close_wait_resolution_sequence,
							  sqlite_shm_reader_lifecycle_event_kind::cut_terminal_commit,
							  reservation.token);
					if (reservation.reservation_phase ==
							sqlite_shm_reader_attachment_reservation_phase::terminal_quarantined ||
						reservation.phase == reader_attachment_group_phase::terminal_quarantined)
					{
						std::optional<sqlite_shm_callback_execution_receipt> callback =
							reservation.unmap_callback;
						std::optional<sqlite_backend_opaque_identity> native_effect;
						std::optional<sqlite_shm_reader_unmap_evidence_kind> evidence_kind;
						std::optional<int> native_status;
						std::optional<sqlite_backend_opaque_identity> latch_reset;
						const auto exact_receipt = reservation.unmap_terminal_receipt.has_value();
						if (reservation.unmap_terminal_receipt)
						{
							callback = reservation.unmap_terminal_receipt->callback();
							evidence_kind = reservation.unmap_terminal_receipt->evidence_kind();
							native_status = reservation.unmap_terminal_receipt->native_status();
							native_effect =
								reservation.unmap_terminal_receipt->native_effect_receipt();
							latch_reset = reservation.unmap_terminal_receipt->latch_reset_receipt();
						}
						output.terminal_quarantines.push_back(
							{reservation.token,
							 reservation.expected,
							 reservation.quarantine_reason,
							 std::max({reservation.unmap_terminal_sequence,
									   reservation.reservation_destination_sequence,
									   reservation.group_destination_sequence}),
							 std::move(callback),
							 std::move(native_effect),
							 exact_receipt,
							 evidence_kind,
							 native_status,
							 std::move(latch_reset)});
					}
					for (const auto& audit : reservation.audits)
					{
						add_event(audit.admission_sequence,
								  sqlite_shm_reader_lifecycle_event_kind::map_admission,
								  audit.map_attempt_token);
						add_event(audit.terminal_sequence,
								  sqlite_shm_reader_lifecycle_event_kind::map_terminal,
								  audit.map_attempt_token);
					}
				}
				for (const auto& session : reader_sessions_)
				{
					++output.session_reservation_phase_counts[static_cast<std::size_t>(
						session.lifecycle_phase)];
					output.session_reservations.push_back({session.token,
														   session.request.attachment,
														   session.lifecycle_phase,
														   session.lifecycle_origin_sequence,
														   session.lifecycle_destination_sequence,
														   session.terminal_sequence_slot});
					add_event(session.lifecycle_origin_sequence,
							  sqlite_shm_reader_lifecycle_event_kind::session_start_admission,
							  session.token);
					if (session.lifecycle_phase ==
						sqlite_shm_reader_session_reservation_phase::promoted_to_group_owner)
						add_event(session.lifecycle_destination_sequence,
								  sqlite_shm_reader_lifecycle_event_kind::
									  use_session_owner_promotion_or_admission,
								  session.token);
					if (session.phase == reader_session_record_phase::terminal_quarantined ||
						session.lifecycle_phase ==
							sqlite_shm_reader_session_reservation_phase::terminal_quarantined)
					{
						add_event(session.pending_terminal_sequence,
								  sqlite_shm_reader_lifecycle_event_kind::use_session_terminal,
								  session.token);
						output.terminal_quarantines.push_back(
							{session.token,
							 session.request.attachment,
							 session.quarantine_reason,
							 session.pending_terminal_sequence,
							 std::nullopt,
							 std::nullopt,
							 session.pending_terminal_receipt.has_value(),
							 std::nullopt,
							 std::nullopt,
							 std::nullopt});
					}
				}
				for (const auto& terminal : reader_session_terminals_)
				{
					++output.session_reservation_phase_counts[static_cast<std::size_t>(
						terminal.lifecycle_phase)];
					output.session_reservations.push_back({terminal.session_token,
														   terminal.receipt.request().attachment,
														   terminal.lifecycle_phase,
														   terminal.lifecycle_origin_sequence,
														   terminal.lifecycle_destination_sequence,
														   0U});
					add_event(terminal.lifecycle_origin_sequence,
							  sqlite_shm_reader_lifecycle_event_kind::session_start_admission,
							  terminal.session_token);
					if (terminal.lifecycle_phase ==
						sqlite_shm_reader_session_reservation_phase::promoted_to_group_owner)
						add_event(terminal.lifecycle_destination_sequence,
								  sqlite_shm_reader_lifecycle_event_kind::
									  use_session_owner_promotion_or_admission,
								  terminal.session_token);
					add_event(terminal.terminal_sequence,
							  sqlite_shm_reader_lifecycle_event_kind::use_session_terminal,
							  terminal.session_token);
				}
				for (const auto& terminal : reader_attachment_zero_effect_terminals_)
				{
					output.zero_effect_terminals.push_back(
						{terminal.token,
						 terminal.receipt.request().expected_attachment,
						 terminal.terminal_sequence,
						 terminal.receipt.kind(),
						 terminal.receipt.native_status(),
						 terminal.receipt.request().callback,
						 terminal.receipt.zero_attachment_effect_receipt(),
						 true,
						 terminal.revoked_first_reservation});
					if (terminal.revoked_first_reservation)
					{
						++output.session_reservation_phase_counts[static_cast<std::size_t>(
							sqlite_shm_reader_session_reservation_phase::consumed_no_pointer)];
						output.session_reservations.push_back(
							{terminal.session_token,
							 terminal.session_request.attachment,
							 sqlite_shm_reader_session_reservation_phase::consumed_no_pointer,
							 terminal.session_origin_sequence,
							 terminal.terminal_sequence,
							 0U});
						add_event(terminal.session_origin_sequence,
								  sqlite_shm_reader_lifecycle_event_kind::session_start_admission,
								  terminal.session_token);
						add_event(terminal.terminal_sequence,
								  sqlite_shm_reader_lifecycle_event_kind::use_session_terminal,
								  terminal.session_token);
					}
					add_event(terminal.admission_sequence,
							  sqlite_shm_reader_lifecycle_event_kind::map_admission,
							  terminal.token);
					add_event(terminal.terminal_sequence,
							  sqlite_shm_reader_lifecycle_event_kind::map_terminal,
							  terminal.token);
				}
				for (const auto& opaque : reader_opaque_attachment_uncertainties_)
				{
					const auto reservation =
						find_by_token(reader_attachment_groups_, opaque.reservation_token);
					const auto candidate_retained =
						reservation != reader_attachment_groups_.end() &&
						reservation->expected == opaque.session_request.attachment &&
						reservation->registry_activity_authority.has_value();
					output.opaque_attachment_uncertainties.push_back(
						{opaque.map_token,
						 opaque.session_token,
						 opaque.reservation_token,
						 opaque.session_request.attachment,
						 opaque.session_origin_sequence,
						 opaque.map_admission_sequence,
						 opaque.map_terminal_sequence,
						 opaque.terminal_sequence,
						 opaque.registry_predelegate_authority.has_value(),
						 candidate_retained});
					++output.session_reservation_phase_counts[static_cast<std::size_t>(
						sqlite_shm_reader_session_reservation_phase::terminal_quarantined)];
					output.session_reservations.push_back(
						{opaque.session_token,
						 opaque.session_request.attachment,
						 sqlite_shm_reader_session_reservation_phase::terminal_quarantined,
						 opaque.session_origin_sequence,
						 opaque.terminal_sequence,
						 0U});
					add_event(opaque.session_origin_sequence,
							  sqlite_shm_reader_lifecycle_event_kind::session_start_admission,
							  opaque.session_token);
					add_event(opaque.map_admission_sequence,
							  sqlite_shm_reader_lifecycle_event_kind::map_admission,
							  opaque.map_token);
					add_event(opaque.map_terminal_sequence,
							  sqlite_shm_reader_lifecycle_event_kind::map_terminal,
							  opaque.map_token);
					add_event(opaque.terminal_sequence,
							  sqlite_shm_reader_lifecycle_event_kind::use_session_terminal,
							  opaque.session_token);
				}
				for (const auto& terminal : reader_predecessor_map_terminals_)
				{
					const auto reservation =
						find_by_token(reader_attachment_groups_, terminal.reservation_token);
					const auto* retirement_receipt =
						reservation != reader_attachment_groups_.end() &&
							reservation->predecessor_unmap_terminal_receipt
						? &*reservation->predecessor_unmap_terminal_receipt
						: nullptr;
					output.predecessor_map_terminals.push_back(
						{terminal.token,
						 terminal.receipt.request().expected_attachment,
						 terminal.terminal_sequence,
						 terminal.receipt.kind(),
						 terminal.receipt.native_status(),
						 terminal.receipt.native_mapping(),
						 terminal.receipt.request().callback,
						 terminal.receipt.native_effect_receipt(),
						 terminal.receipt.observed_attachment().has_value(),
						 true,
						 reservation != reader_attachment_groups_.end()
							 ? reservation->predecessor_unmap_terminal_sequence
							 : 0U,
						 retirement_receipt
							 ? std::optional<
								   sqlite_shm_callback_execution_receipt>{retirement_receipt
																			  ->callback()}
							 : std::nullopt,
						 retirement_receipt
							 ? std::optional<
								   sqlite_shm_reader_unmap_evidence_kind>{retirement_receipt
																			  ->evidence_kind()}
							 : std::nullopt,
						 retirement_receipt ? retirement_receipt->native_status() : std::nullopt,
						 retirement_receipt ? retirement_receipt->native_effect_receipt()
											: std::nullopt,
						 reservation != reader_attachment_groups_.end()
							 ? reservation->predecessor_close_terminal_sequence
							 : 0U});
					++output.session_reservation_phase_counts[static_cast<std::size_t>(
						sqlite_shm_reader_session_reservation_phase::
							transferred_to_existing_predecessor)];
					output.session_reservations.push_back(
						{terminal.session_token,
						 terminal.session_request.attachment,
						 sqlite_shm_reader_session_reservation_phase::
							 transferred_to_existing_predecessor,
						 terminal.session_origin_sequence,
						 terminal.terminal_sequence,
						 0U});
					add_event(terminal.session_origin_sequence,
							  sqlite_shm_reader_lifecycle_event_kind::session_start_admission,
							  terminal.session_token);
					add_event(terminal.admission_sequence,
							  sqlite_shm_reader_lifecycle_event_kind::map_admission,
							  terminal.token);
					add_event(terminal.terminal_sequence,
							  sqlite_shm_reader_lifecycle_event_kind::map_terminal,
							  terminal.token);
					if (reservation != reader_attachment_groups_.end())
					{
						add_event(reservation->predecessor_unmap_terminal_sequence,
								  sqlite_shm_reader_lifecycle_event_kind::cut_terminal_commit,
								  reservation->token);
						add_event(reservation->predecessor_close_terminal_sequence,
								  sqlite_shm_reader_lifecycle_event_kind::cut_terminal_commit,
								  reservation->token);
					}
				}
				for (const auto& map : reader_attachment_maps_)
				{
					if (map.phase != reader_phase::terminal_quarantined)
						output.map_attempts.push_back({map.token,
													   map.request.expected_attachment,
													   map.admission_sequence,
													   map.terminal_sequence_slot,
													   map.potential_group_cut_sequence_slot,
													   map.potential_group_terminal_sequence_slot});
					add_event(map.admission_sequence,
							  sqlite_shm_reader_lifecycle_event_kind::map_admission,
							  map.token);
					add_event(map.terminal_sequence,
							  sqlite_shm_reader_lifecycle_event_kind::map_terminal,
							  map.token);
					if (map.phase == reader_phase::terminal_quarantined)
					{
						std::optional<sqlite_shm_callback_execution_receipt> callback;
						std::optional<sqlite_backend_opaque_identity> native_effect;
						if (map.receipt)
						{
							callback = map.receipt->request().callback;
							native_effect = map.receipt->zero_resize_effect_receipt();
						}
						else if (map.zero_effect_receipt)
						{
							callback = map.zero_effect_receipt->request().callback;
							native_effect =
								map.zero_effect_receipt->zero_attachment_effect_receipt();
						}
						output.terminal_quarantines.push_back(
							{map.token,
							 map.request.expected_attachment,
							 map.quarantine_reason,
							 map.terminal_sequence,
							 std::move(callback),
							 std::move(native_effect),
							 map.receipt.has_value() || map.zero_effect_receipt.has_value(),
							 std::nullopt,
							 std::nullopt,
							 std::nullopt});
					}
				}
				for (const auto& custody : reader_custodies_)
				{
					++output.custody_state_counts[static_cast<std::size_t>(custody.state)];
					if (custody.state == sqlite_shm_reader_custody_state::live)
						++output.live_custody_kind_counts[static_cast<std::size_t>(custody.kind)];
				}
				std::ranges::sort(output.events,
								  [](const sqlite_shm_reader_lifecycle_event_test_view& left,
									 const sqlite_shm_reader_lifecycle_event_test_view& right)
								  {
									  if (left.sequence != right.sequence)
										  return left.sequence < right.sequence;
									  if (left.kind != right.kind)
										  return left.kind < right.kind;
									  return left.owner_token < right.owner_token;
								  });
				output.events.erase(
					std::unique(output.events.begin(),
								output.events.end(),
								[](const sqlite_shm_reader_lifecycle_event_test_view& left,
								   const sqlite_shm_reader_lifecycle_event_test_view& right)
								{
									return left.sequence == right.sequence &&
										left.kind == right.kind &&
										left.owner_token == right.owner_token;
								}),
					output.events.end());
				return output;
			}

			[[nodiscard]] sqlite_shm_lease_result<
				std::vector<sqlite_shm_reader_lifecycle_compact_tombstone>>
			export_registry_reader_lifecycle_tombstones() const
			{
				try
				{
					std::scoped_lock lock{mutex_};
					std::vector<sqlite_shm_reader_lifecycle_compact_tombstone> output;
					for (const auto& reservation : reader_attachment_groups_)
					{
						if (!reservation.registry_bound)
							continue;
						const auto exact_closed_open =
							[&reservation](
								const sqlite_shm_reader_open_epoch_close_tombstone& tombstone)
						{
							const auto& expected = reservation.expected;
							return tombstone.registry_open_token ==
								expected.registry_open_token() &&
								tombstone.binding.family == expected.family() &&
								tombstone.binding.runtime_lifetime_pin ==
								expected.runtime_lifetime_pin() &&
								tombstone.binding.alias_lifetime == expected.alias_lifetime() &&
								tombstone.binding.connection_token == expected.connection_token() &&
								tombstone.binding.main_native_file_receipt ==
								expected.main_native_file_receipt() &&
								tombstone.binding.main_xopen_receipt ==
								expected.main_xopen_receipt() &&
								tombstone.binding.open_epoch == expected.open_epoch() &&
								tombstone.binding.callback_cohort == expected.callback_cohort();
						};
						const auto closed_open = std::find_if(reader_open_close_tombstones_.begin(),
															  reader_open_close_tombstones_.end(),
															  exact_closed_open);
						const auto exact_closed_open_count =
							std::ranges::count_if(reader_open_close_tombstones_, exact_closed_open);
						const auto exact_compact_shape = reservation.token == 0U
							? reader_imported_compact_group_shape_is_exact_locked(reservation)
							: reader_local_phase1_compact_group_shape_is_exact_locked(reservation);
						const auto exact_cleanup_close_relation = [&]() noexcept
						{
							if (closed_open == reader_open_close_tombstones_.end())
								return false;
							const auto phase1_close = reservation.composite_close_owner_token ==
									closed_open->close_owner_token &&
								reservation.composite_close_registry_open_token ==
									closed_open->registry_open_token &&
								reservation.composite_close_cut_sequence ==
									closed_open->close_cut_sequence &&
								reservation.composite_close_wait_resolution_sequence >
									closed_open->close_cut_sequence &&
								reservation.composite_close_wait_resolution_sequence <
									closed_open->terminal_sequence;
							if (phase1_close)
							{
								if (reservation.reservation_phase ==
									sqlite_shm_reader_attachment_reservation_phase::revoked_no_map)
									return reservation.reservation_destination_sequence >
										closed_open->close_cut_sequence &&
										reservation.reservation_destination_sequence <
										reservation.composite_close_wait_resolution_sequence;
								if (reservation.reservation_phase ==
									sqlite_shm_reader_attachment_reservation_phase::
										unpublished_cleanup_confirmed)
									return reservation.logical_ack_phase ==
										sqlite_shm_reader_logical_ack_phase::consumed_by_close &&
										reservation.reservation_destination_sequence <
										reservation.logical_ack_sequence &&
										reservation.logical_ack_sequence <
										reservation.composite_close_wait_resolution_sequence;
								return reservation.reservation_phase ==
									sqlite_shm_reader_attachment_reservation_phase::
										predecessor_route_retired_confirmed &&
									reservation.reservation_destination_sequence ==
									closed_open->terminal_sequence;
							}
							if (reservation.reservation_phase ==
								sqlite_shm_reader_attachment_reservation_phase::
									predecessor_route_retired_confirmed)
							{
								if (reservation.token == 0U)
								{
									const auto retired_by_close =
										reservation.compact_replay_identities
											.callback_invocation_tokens.size() == 1U;
									return retired_by_close
										? reservation.reservation_destination_sequence ==
											closed_open->terminal_sequence
										: reservation.compact_replay_identities
													.callback_invocation_tokens.size() == 2U &&
											reservation.reservation_destination_sequence <
												closed_open->close_cut_sequence;
								}
								return reservation.predecessor_close_terminal_sequence != 0U
									? reservation.predecessor_close_terminal_sequence ==
											closed_open->terminal_sequence &&
										reservation.reservation_destination_sequence ==
											closed_open->terminal_sequence
									: reservation.predecessor_unmap_terminal_receipt &&
										reservation.reservation_destination_sequence <
											closed_open->close_cut_sequence;
							}
							if (reservation.reservation_phase !=
								sqlite_shm_reader_attachment_reservation_phase::
									unpublished_cleanup_confirmed)
							{
								const auto retained_unmap_terminal_sequence =
									reservation.token == 0U
									? reservation.reservation_destination_sequence
									: reservation.unmap_terminal_sequence;
								const auto composite_close =
									reservation.composite_close_owner_token ==
										closed_open->close_owner_token &&
									reservation.composite_close_registry_open_token ==
										closed_open->registry_open_token &&
									reservation.composite_close_cut_sequence ==
										closed_open->close_cut_sequence &&
									reservation.composite_close_registry_open_token ==
										reservation.expected.registry_open_token() &&
									retained_unmap_terminal_sequence ==
										reservation.reservation_destination_sequence &&
									retained_unmap_terminal_sequence >
										closed_open->close_cut_sequence &&
									retained_unmap_terminal_sequence <
										closed_open->terminal_sequence;
								return composite_close ||
									reservation.reservation_destination_sequence <
									closed_open->close_cut_sequence;
							}
							if (reservation.logical_ack_phase ==
								sqlite_shm_reader_logical_ack_phase::consumed_by_exact_unmap)
								return reservation.logical_ack_sequence <
									closed_open->close_cut_sequence &&
									reservation.reservation_destination_sequence <
									closed_open->close_cut_sequence;
							return reservation.logical_ack_phase ==
								sqlite_shm_reader_logical_ack_phase::consumed_by_close &&
								reservation.logical_ack_sequence ==
								closed_open->close_cut_sequence &&
								reservation.reservation_destination_sequence <
								closed_open->close_cut_sequence;
						}();
						if (!exact_compact_shape ||
							// A compact attachment row is authorized only by the unique retained
							// success tombstone for its exact authenticated xOpen epoch.
							exact_closed_open_count != 1 ||
							closed_open == reader_open_close_tombstones_.end() ||
							!exact_cleanup_close_relation ||
							reservation.reservation_origin_sequence == 0U ||
							reservation.reservation_destination_sequence <=
								reservation.reservation_origin_sequence ||
							std::ranges::count_if(
								reader_attachment_groups_,
								[&reservation](const reader_attachment_group_record& candidate)
								{
									return candidate.registry_bound &&
										candidate.expected == reservation.expected;
								}) != 1)
							return sqlite_shm_unexpected(ambiguous());
						const auto retains_live_custody = std::ranges::any_of(
							reader_custodies_,
							[&reservation](const reader_custody_record& custody)
							{
								return custody.attachment == reservation.expected &&
									(custody.state == sqlite_shm_reader_custody_state::live ||
									 custody.destination_sequence == 0U);
							});
						const auto retains_session = std::ranges::any_of(
							reader_sessions_,
							[&reservation](const reader_session_record& session)
							{
								return session.request.attachment == reservation.expected;
							});
						const auto retains_map = std::ranges::any_of(
							reader_attachment_maps_,
							[&reservation](const reader_attachment_map_record& map)
							{
								return map.request.expected_attachment == reservation.expected;
							});
						if (reservation.registry_activity_authority ||
							reservation.unmap_cut_sequence_slot != 0U ||
							reservation.unmap_terminal_sequence_slot != 0U ||
							retains_live_custody || retains_session || retains_map)
							return sqlite_shm_unexpected(ambiguous());
						auto replay = reservation.token == 0U
							? std::optional{reservation.compact_replay_identities}
							: reader_local_compact_replay_identities_locked(reservation);
						if (!replay ||
							!reader_replay_identity_tombstone_is_exact(
								*replay,
								reservation.reservation_phase,
								reservation.logical_ack_phase))
							return sqlite_shm_unexpected(ambiguous());
						if (reader_replay_identity_tombstones_overlap(
								*replay, closed_open->replay_identities) ||
							std::ranges::any_of(
								output,
								[&replay](
									const sqlite_shm_reader_lifecycle_compact_tombstone& existing)
								{
									return reader_replay_identity_tombstones_overlap(
										existing.replay_identities, *replay);
								}))
							return sqlite_shm_unexpected(ambiguous());
						output.push_back({reservation.expected,
										  reservation.reservation_phase,
										  reservation.reservation_origin_sequence,
										  reservation.reservation_destination_sequence,
										  std::move(*replay),
										  reservation.logical_ack_phase,
										  reservation.logical_ack_sequence,
										  reservation.unpublished_cleanup_session_terminal_sequence,
										  reservation.unpublished_cleanup_cut_sequence,
										  reservation.unpublished_cleanup_terminal_sequence,
										  reservation.composite_close_owner_token,
										  reservation.composite_close_registry_open_token,
										  reservation.composite_close_cut_sequence,
										  reservation.composite_close_wait_resolution_sequence});
					}
					return output;
				}
				catch (...)
				{
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<void> import_registry_reader_lifecycle_tombstones(
				const std::span<const sqlite_shm_reader_lifecycle_compact_tombstone> tombstones)
			{
				try
				{
					std::scoped_lock lock{mutex_};
					std::list<reader_attachment_group_record> prepared;
					std::uint64_t imported_last = reader_last_committed_sequence_;
					for (const auto& tombstone : tombstones)
					{
						const auto compactable = reader_reservation_is_compactable(tombstone);
						const auto cleanup_tombstone = tombstone.phase ==
							sqlite_shm_reader_attachment_reservation_phase::
								unpublished_cleanup_confirmed;
						const auto exact_cleanup_metadata = cleanup_tombstone
							? tombstone.unpublished_cleanup_session_terminal_sequence >
									tombstone.origin_sequence &&
								tombstone.unpublished_cleanup_session_terminal_sequence -
										tombstone.origin_sequence >
									2U &&
								tombstone.unpublished_cleanup_session_terminal_sequence !=
									std::numeric_limits<std::uint64_t>::max() &&
								tombstone.unpublished_cleanup_cut_sequence ==
									tombstone.unpublished_cleanup_session_terminal_sequence + 1U &&
								tombstone.unpublished_cleanup_terminal_sequence >
									tombstone.unpublished_cleanup_cut_sequence &&
								tombstone.destination_sequence ==
									tombstone.unpublished_cleanup_terminal_sequence &&
								tombstone.logical_ack_sequence >
									tombstone.unpublished_cleanup_terminal_sequence
							: tombstone.logical_ack_phase ==
									sqlite_shm_reader_logical_ack_phase::not_applicable &&
								tombstone.logical_ack_sequence == 0U &&
								tombstone.unpublished_cleanup_session_terminal_sequence == 0U &&
								tombstone.unpublished_cleanup_cut_sequence == 0U &&
								tombstone.unpublished_cleanup_terminal_sequence == 0U;
						const auto has_any_composite_metadata =
							tombstone.composite_close_owner_token != 0U ||
							tombstone.composite_close_registry_open_token != 0U ||
							tombstone.composite_close_cut_sequence != 0U;
						const auto exact_composite_metadata = !has_any_composite_metadata ||
							(tombstone.composite_close_owner_token != 0U &&
							 tombstone.composite_close_registry_open_token ==
								 tombstone.attachment.registry_open_token() &&
							 tombstone.composite_close_cut_sequence > tombstone.origin_sequence &&
							 ((tombstone.composite_close_wait_resolution_sequence == 0U &&
							   tombstone.composite_close_cut_sequence <
								   tombstone.destination_sequence &&
							   tombstone.phase ==
								   sqlite_shm_reader_attachment_reservation_phase::
									   retired_confirmed &&
							   tombstone.logical_ack_phase ==
								   sqlite_shm_reader_logical_ack_phase::not_applicable) ||
							  (tombstone.composite_close_wait_resolution_sequence >
								   tombstone.composite_close_cut_sequence &&
							   (tombstone.phase ==
									sqlite_shm_reader_attachment_reservation_phase::
										revoked_no_map ||
								tombstone.phase ==
									sqlite_shm_reader_attachment_reservation_phase::
										predecessor_route_retired_confirmed ||
								(tombstone.phase ==
									 sqlite_shm_reader_attachment_reservation_phase::
										 unpublished_cleanup_confirmed &&
								 tombstone.logical_ack_phase ==
									 sqlite_shm_reader_logical_ack_phase::consumed_by_close &&
								 tombstone.logical_ack_sequence <
									 tombstone.composite_close_wait_resolution_sequence)))));
						if (!compactable || !valid_reader_native_attachment(tombstone.attachment) ||
							!reader_replay_identity_tombstone_is_exact(
								tombstone.replay_identities,
								tombstone.phase,
								tombstone.logical_ack_phase) ||
							tombstone.attachment.family() != family_ ||
							tombstone.attachment.registry_open_token() == 0U ||
							tombstone.origin_sequence == 0U ||
							tombstone.destination_sequence <= tombstone.origin_sequence ||
							!exact_cleanup_metadata || !exact_composite_metadata ||
							!reader_lifecycle_sequences_ ||
							std::max(tombstone.destination_sequence,
									 std::max(tombstone.logical_ack_sequence,
											  tombstone.composite_close_wait_resolution_sequence)) >
								reader_lifecycle_sequences_->observed_last_issued())
							return sqlite_shm_unexpected(rejection(
								sqlite_shm_lease_rejection_reason::invalid_request,
								sqlite_shm_lease_recovery_action::deny_before_native_map));
						if (std::ranges::any_of(
								registry_reader_opens_,
								[&tombstone](const registry_reader_open_record& open)
								{
									return tombstone.attachment.registry_open_token() ==
										open.token ||
										reader_attachment_matches_open_epoch_binding(
											   tombstone.attachment, open.binding);
								}))
							return sqlite_shm_unexpected(ambiguous());
						const auto matches_attachment =
							[&tombstone](const reader_attachment_group_record& reservation)
						{
							return reservation.expected == tombstone.attachment;
						};
						const auto existing = std::find_if(reader_attachment_groups_.begin(),
														   reader_attachment_groups_.end(),
														   matches_attachment);
						const auto pending =
							std::find_if(prepared.begin(), prepared.end(), matches_attachment);
						const auto exact_match =
							[&tombstone](const reader_attachment_group_record& reservation)
						{
							return reservation.reservation_phase == tombstone.phase &&
								reservation.reservation_origin_sequence ==
								tombstone.origin_sequence &&
								reservation.reservation_destination_sequence ==
								tombstone.destination_sequence &&
								reservation.compact_replay_identities ==
								tombstone.replay_identities &&
								reservation.logical_ack_phase == tombstone.logical_ack_phase &&
								reservation.logical_ack_sequence ==
								tombstone.logical_ack_sequence &&
								reservation.unpublished_cleanup_session_terminal_sequence ==
								tombstone.unpublished_cleanup_session_terminal_sequence &&
								reservation.unpublished_cleanup_cut_sequence ==
								tombstone.unpublished_cleanup_cut_sequence &&
								reservation.unpublished_cleanup_terminal_sequence ==
								tombstone.unpublished_cleanup_terminal_sequence &&
								reservation.composite_close_owner_token ==
								tombstone.composite_close_owner_token &&
								reservation.composite_close_registry_open_token ==
								tombstone.composite_close_registry_open_token &&
								reservation.composite_close_cut_sequence ==
								tombstone.composite_close_cut_sequence &&
								reservation.composite_close_wait_resolution_sequence ==
								tombstone.composite_close_wait_resolution_sequence &&
								reservation.token == 0U && !reservation.observed_identity &&
								!reservation.registry_activity_authority;
						};
						if (existing != reader_attachment_groups_.end())
						{
							if (!exact_match(*existing) ||
								!reader_imported_compact_group_shape_is_exact_locked(*existing))
								return sqlite_shm_unexpected(ambiguous());
							continue;
						}
						if (pending != prepared.end())
						{
							if (!exact_match(*pending))
								return sqlite_shm_unexpected(ambiguous());
							continue;
						}
						const auto live_identity_seen =
							[this](const sqlite_backend_opaque_identity& identity)
						{
							return reader_callback_invocation_was_seen_locked(identity) ||
								reader_effect_identity_seen_locked(identity, 0U, 0U, 0U) ||
								reader_session_terminal_identity_seen_locked(identity);
						};
						const auto live_replay_collision =
							std::ranges::any_of(
								tombstone.replay_identities.callback_invocation_tokens,
								live_identity_seen) ||
							std::ranges::any_of(tombstone.replay_identities.effect_receipts,
												live_identity_seen) ||
							std::ranges::any_of(
								tombstone.replay_identities.session_terminal_receipts,
								live_identity_seen);
						const auto prepared_replay_collision = std::ranges::any_of(
							prepared,
							[&tombstone](const reader_attachment_group_record& reservation)
							{
								return reader_replay_identity_tombstones_overlap(
									reservation.compact_replay_identities,
									tombstone.replay_identities);
							});
						if (live_replay_collision || prepared_replay_collision)
							return sqlite_shm_unexpected(ambiguous());
						const auto epoch_collision =
							std::ranges::any_of(
								reader_attachment_groups_,
								[&tombstone](const reader_attachment_group_record& reservation)
								{
									return reservation.expected.attachment_epoch() ==
										tombstone.attachment.attachment_epoch() &&
										reservation.expected != tombstone.attachment;
								}) ||
							std::ranges::any_of(
								prepared,
								[&tombstone](const reader_attachment_group_record& reservation)
								{
									return reservation.expected.attachment_epoch() ==
										tombstone.attachment.attachment_epoch() &&
										reservation.expected != tombstone.attachment;
								});
						if (epoch_collision)
							return sqlite_shm_unexpected(rejection(
								sqlite_shm_lease_rejection_reason::receipt_mismatch,
								sqlite_shm_lease_recovery_action::deny_before_native_map));
						prepared.emplace_back(0U,
											  tombstone.attachment.writer_mapping_generation(),
											  tombstone.attachment);
						prepared.back().registry_bound = true;
						prepared.back().reservation_phase = tombstone.phase;
						prepared.back().reservation_origin_sequence = tombstone.origin_sequence;
						prepared.back().reservation_destination_sequence =
							tombstone.destination_sequence;
						prepared.back().compact_replay_identities = tombstone.replay_identities;
						prepared.back().logical_ack_phase = tombstone.logical_ack_phase;
						prepared.back().logical_ack_sequence = tombstone.logical_ack_sequence;
						prepared.back().unpublished_cleanup_session_origin_sequence =
							cleanup_tombstone ? tombstone.origin_sequence : 0U;
						prepared.back().unpublished_cleanup_session_terminal_sequence =
							tombstone.unpublished_cleanup_session_terminal_sequence;
						prepared.back().unpublished_cleanup_cut_sequence =
							tombstone.unpublished_cleanup_cut_sequence;
						prepared.back().unpublished_cleanup_terminal_sequence =
							tombstone.unpublished_cleanup_terminal_sequence;
						prepared.back().composite_close_owner_token =
							tombstone.composite_close_owner_token;
						prepared.back().composite_close_registry_open_token =
							tombstone.composite_close_registry_open_token;
						prepared.back().composite_close_cut_sequence =
							tombstone.composite_close_cut_sequence;
						prepared.back().composite_close_wait_resolution_sequence =
							tombstone.composite_close_wait_resolution_sequence;
						imported_last =
							std::max({imported_last,
									  tombstone.destination_sequence,
									  tombstone.logical_ack_sequence,
									  tombstone.composite_close_cut_sequence,
									  tombstone.composite_close_wait_resolution_sequence});
					}
					reader_attachment_groups_.splice(reader_attachment_groups_.end(), prepared);
					reader_last_committed_sequence_ = imported_last;
					return {};
				}
				catch (...)
				{
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<void> check_registry_reader_lifecycle_tombstone(
				const sqlite_shm_reader_attachment_reservation_identity& attachment) const noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					for (const auto& reservation : reader_attachment_groups_)
					{
						if (reservation.expected == attachment)
						{
							if (reader_reservation_is_compactable(reservation))
								return sqlite_shm_unexpected(stale_token(
									sqlite_shm_lease_recovery_action::deny_before_native_map));
							return {};
						}
						if (reservation.expected.attachment_epoch() ==
							attachment.attachment_epoch())
							return sqlite_shm_unexpected(rejection(
								sqlite_shm_lease_rejection_reason::receipt_mismatch,
								sqlite_shm_lease_recovery_action::deny_before_native_map));
					}
					return {};
				}
				catch (...)
				{
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<
				std::vector<sqlite_shm_reader_open_epoch_close_tombstone>>
			export_registry_reader_open_epoch_close_tombstones() const
			{
				try
				{
					std::scoped_lock lock{mutex_};
					for (const auto& tombstone : reader_open_close_tombstones_)
					{
						if (tombstone.registry_open_token == 0U ||
							tombstone.close_owner_token == 0U ||
							!valid_reader_open_epoch_binding(tombstone.binding) ||
							!reader_close_replay_identity_tombstone_is_exact(
								tombstone.replay_identities) ||
							tombstone.binding.family != family_ ||
							tombstone.origin_sequence == 0U ||
							tombstone.close_cut_sequence <= tombstone.origin_sequence ||
							tombstone.terminal_sequence <= tombstone.close_cut_sequence)
							return sqlite_shm_unexpected(ambiguous());
					}
					return reader_open_close_tombstones_;
				}
				catch (...)
				{
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<void>
			import_registry_reader_open_epoch_close_tombstones(
				const std::span<const sqlite_shm_reader_open_epoch_close_tombstone> tombstones)
			{
				try
				{
					std::scoped_lock lock{mutex_};
					auto prepared = reader_open_close_tombstones_;
					auto imported_last = reader_last_committed_sequence_;
					auto imported_next_token = next_token_;
					auto imported_max_token = false;
					const auto pre_import_next_token = next_token_;
					const auto observed_last = reader_lifecycle_sequences_
						? reader_lifecycle_sequences_->observed_last_issued()
						: 0U;
					for (const auto& tombstone : tombstones)
					{
						if (tombstone.registry_open_token == 0U ||
							tombstone.close_owner_token == 0U ||
							!valid_reader_open_epoch_binding(tombstone.binding) ||
							!reader_close_replay_identity_tombstone_is_exact(
								tombstone.replay_identities) ||
							tombstone.binding.family != family_ ||
							tombstone.origin_sequence == 0U ||
							tombstone.close_cut_sequence <= tombstone.origin_sequence ||
							tombstone.terminal_sequence <= tombstone.close_cut_sequence ||
							!reader_lifecycle_sequences_ ||
							tombstone.terminal_sequence > observed_last)
							return sqlite_shm_unexpected(rejection(
								sqlite_shm_lease_rejection_reason::invalid_request,
								sqlite_shm_lease_recovery_action::deny_before_native_map));

						const auto collides =
							[&tombstone](
								const sqlite_shm_reader_open_epoch_close_tombstone& candidate)
						{
							return candidate.registry_open_token == tombstone.registry_open_token ||
								candidate.close_owner_token == tombstone.close_owner_token ||
								candidate.binding == tombstone.binding ||
								reader_replay_identity_tombstones_overlap(
									   candidate.replay_identities, tombstone.replay_identities);
						};
						const auto existing =
							std::find_if(prepared.begin(), prepared.end(), collides);
						if (existing != prepared.end())
						{
							if (*existing != tombstone)
								return sqlite_shm_unexpected(ambiguous());
							continue;
						}

						const auto live_collision = std::ranges::any_of(
							registry_reader_opens_,
							[&tombstone](const registry_reader_open_record& open)
							{
								return open.token == tombstone.registry_open_token ||
									open.close_owner_token == tombstone.close_owner_token ||
									open.binding == tombstone.binding;
							});
						const auto terminal_collision = std::ranges::any_of(
							reader_close_terminals_,
							[&tombstone](const reader_close_terminal_record& terminal)
							{
								return terminal.registry_open_token ==
									tombstone.registry_open_token ||
									terminal.close_owner_token == tombstone.close_owner_token ||
									terminal.binding == tombstone.binding;
							});
						const auto live_replay_identity_seen =
							[this](const sqlite_backend_opaque_identity& identity)
						{
							return reader_callback_invocation_was_seen_locked(identity) ||
								reader_effect_identity_seen_locked(identity, 0U, 0U, 0U) ||
								reader_session_terminal_identity_seen_locked(identity);
						};
						const auto replay_collision =
							std::ranges::any_of(
								tombstone.replay_identities.callback_invocation_tokens,
								live_replay_identity_seen) ||
							std::ranges::any_of(tombstone.replay_identities.effect_receipts,
												live_replay_identity_seen) ||
							std::ranges::any_of(
								tombstone.replay_identities.session_terminal_receipts,
								live_replay_identity_seen);
						if (live_collision || terminal_collision || replay_collision)
							return sqlite_shm_unexpected(ambiguous());
						// Close-owner tokens are part of the durable stale identity.  A
						// reconstructed coordinator must not restart its local allocator below
						// any imported owner.  Import after that numeric range was already used
						// by this coordinator is ambiguous and therefore rejected.
						if (token_exhausted_ || tombstone.close_owner_token < pre_import_next_token)
							return sqlite_shm_unexpected(ambiguous());
						if (tombstone.close_owner_token ==
							std::numeric_limits<std::uint64_t>::max())
							imported_max_token = true;
						else
							imported_next_token =
								std::max(imported_next_token, tombstone.close_owner_token + 1U);

						prepared.push_back(tombstone);
						imported_last = std::max(imported_last, tombstone.terminal_sequence);
					}
					for (const auto& group : reader_attachment_groups_)
					{
						if (group.token != 0U || group.composite_close_owner_token == 0U)
							continue;
						const auto exact_composite_close =
							[&group](const sqlite_shm_reader_open_epoch_close_tombstone& close)
						{
							const auto exact_owner =
								close.close_owner_token == group.composite_close_owner_token &&
								close.registry_open_token ==
									group.composite_close_registry_open_token &&
								close.close_cut_sequence == group.composite_close_cut_sequence &&
								reader_attachment_matches_open_epoch_binding(group.expected,
																			 close.binding);
							if (!exact_owner)
								return false;
							if (group.composite_close_wait_resolution_sequence == 0U)
								return group.reservation_destination_sequence >
									close.close_cut_sequence &&
									group.reservation_destination_sequence <
									close.terminal_sequence;
							return group.composite_close_wait_resolution_sequence >
								close.close_cut_sequence &&
								group.composite_close_wait_resolution_sequence <
								close.terminal_sequence &&
								(group.reservation_phase ==
										 sqlite_shm_reader_attachment_reservation_phase::
											 predecessor_route_retired_confirmed
									 ? group.reservation_destination_sequence ==
										 close.terminal_sequence
									 : group.reservation_destination_sequence <
											 group.composite_close_wait_resolution_sequence &&
										 (group.reservation_phase !=
											  sqlite_shm_reader_attachment_reservation_phase::
												  unpublished_cleanup_confirmed ||
										  (group.logical_ack_phase ==
											   sqlite_shm_reader_logical_ack_phase::
												   consumed_by_close &&
										   group.logical_ack_sequence <
											   group.composite_close_wait_resolution_sequence)));
						};
						if (std::ranges::count_if(prepared, exact_composite_close) != 1)
							return sqlite_shm_unexpected(ambiguous());
					}
					reader_open_close_tombstones_.swap(prepared);
					reader_last_committed_sequence_ = imported_last;
					next_token_ = imported_next_token;
					token_exhausted_ = token_exhausted_ || imported_max_token;
					return {};
				}
				catch (...)
				{
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] sqlite_shm_lease_result<void>
			check_registry_reader_open_epoch_close_tombstone(
				const std::uint64_t registry_open_token,
				const sqlite_shm_reader_open_epoch_binding& binding) const noexcept
			{
				if (registry_open_token == 0U || !valid_reader_open_epoch_binding(binding) ||
					binding.family != family_)
					return sqlite_shm_unexpected(
						rejection(sqlite_shm_lease_rejection_reason::invalid_request,
								  sqlite_shm_lease_recovery_action::deny_before_native_map));
				try
				{
					std::scoped_lock lock{mutex_};
					for (const auto& tombstone : reader_open_close_tombstones_)
					{
						if (tombstone.registry_open_token == registry_open_token &&
							tombstone.binding == binding)
							return sqlite_shm_unexpected(stale_token(
								sqlite_shm_lease_recovery_action::deny_before_native_map));
						if (tombstone.registry_open_token == registry_open_token ||
							tombstone.binding == binding)
							return sqlite_shm_unexpected(rejection(
								sqlite_shm_lease_rejection_reason::receipt_mismatch,
								sqlite_shm_lease_recovery_action::deny_before_native_map));
					}
					return {};
				}
				catch (...)
				{
					return sqlite_shm_unexpected(ambiguous());
				}
			}

			[[nodiscard]] std::optional<sqlite_shm_reader_open_epoch_test_view>
			reader_open_epoch_view_for_testing(
				const std::uint64_t registry_open_token,
				const std::shared_ptr<sqlite_shm_reader_open_lineage_seal>& seal) const noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					const auto open = std::find_if(
						registry_reader_opens_.begin(),
						registry_reader_opens_.end(),
						[registry_open_token](const registry_reader_open_record& candidate)
						{
							return candidate.token == registry_open_token;
						});
					if (open == registry_reader_opens_.end() || !seal ||
						open->seal.get() != seal.get())
						return std::nullopt;
					return sqlite_shm_reader_open_epoch_test_view{
						open->token,
						open->binding,
						open->close_owner_token,
						open->close_phase,
						open->close_origin_sequence,
						open->close_cut_sequence_slot,
						open->close_terminal_sequence_slot,
						open->initial_close_cut_sequence_slot,
						open->initial_close_terminal_sequence_slot,
						open->close_route,
						open->close_cut_sequence,
						open->close_terminal_sequence};
				}
				catch (...)
				{
					return std::nullopt;
				}
			}

			void exhaust_reader_lifecycle_sequence_source_for_testing() noexcept
			{
				if (reader_lifecycle_sequences_)
					reader_lifecycle_sequences_->exhaust_for_testing();
			}

			void make_reader_lifecycle_sequence_source_unavailable_for_testing() noexcept
			{
				if (reader_lifecycle_sequences_)
					reader_lifecycle_sequences_->make_unavailable_for_testing();
			}

			void inject_reader_close_terminal_commit_failure_for_testing() noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					fail_next_reader_close_terminal_commit_for_testing_ = true;
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
				}
			}

			void inject_reader_close_post_receipt_state_failure_for_testing() noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					fail_next_reader_close_post_receipt_state_for_testing_ = true;
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
				}
			}

			void inject_reader_close_begin_preparation_failure_for_testing() noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					fail_next_reader_close_begin_preparation_for_testing_ = true;
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
				}
			}

			void inject_reader_unmap_terminal_commit_failure_for_testing() noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					fail_next_reader_unmap_terminal_commit_for_testing_ = true;
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
				}
			}

			void inject_reader_unmap_post_receipt_state_failure_for_testing() noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					fail_next_reader_unmap_post_receipt_state_for_testing_ = true;
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
				}
			}

			void inject_reader_unpublished_cleanup_terminal_commit_failure_for_testing() noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					fail_next_reader_unpublished_cleanup_terminal_commit_for_testing_ = true;
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
				}
			}

			void inject_reader_unmap_begin_preparation_failure_for_testing() noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					fail_next_reader_unmap_begin_preparation_for_testing_ = true;
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
				}
			}

			void inject_reader_coarse_unmap_terminal_exception_for_testing() noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					fail_next_reader_coarse_unmap_terminal_for_testing_ = true;
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
				}
			}

			void inject_reader_recovery_mutex_reacquire_failure_for_testing() noexcept
			{
				fail_next_reader_recovery_mutex_reacquire_for_testing_.store(
					true, std::memory_order_release);
			}

			void inject_reader_operation_mutex_acquire_failure_for_testing() noexcept
			{
				fail_next_reader_operation_mutex_acquire_for_testing_.store(
					true, std::memory_order_release);
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

			void inject_reader_map_terminal_commit_failure_for_testing() noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					fail_next_reader_map_terminal_commit_for_testing_ = true;
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
				}
			}

			void inject_reader_session_terminal_commit_failure_for_testing() noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					fail_next_reader_session_terminal_commit_for_testing_ = true;
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
				}
			}

			void inject_registry_reader_attachment_liveness_loss_for_testing() noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					for (auto& group : reader_attachment_groups_)
						if (group.registry_activity_authority)
						{
							group.registry_activity_authority->invalidate_activity_for_testing();
							return;
						}
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
				}
			}

			void inject_registry_reader_predelegate_liveness_loss_for_testing() noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					for (auto& map : reader_attachment_maps_)
						if (map.registry_predelegate_authority)
						{
							map.registry_predelegate_authority->invalidate_activity_for_testing();
							return;
						}
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
							case lease_token_kind::reader_unpublished_cleanup:
							case lease_token_kind::reader_unmap:
							{
								const auto handoff = find_by_token(handoffs_, token);
								if (handoff != handoffs_.end())
									handoff->phase = handoff_phase::terminal_quarantined;
								const auto group = find_by_token(reader_attachment_groups_, token);
								if (group != reader_attachment_groups_.end())
									quarantine_reader_group_locked(
										*group,
										0U,
										sqlite_shm_reader_terminal_quarantine_reason::
											owner_abandoned);
								break;
							}
							case lease_token_kind::reader_close:
							{
								const auto open = std::find_if(
									registry_reader_opens_.begin(),
									registry_reader_opens_.end(),
									[token](const registry_reader_open_record& candidate)
									{
										return candidate.close_owner_token == token;
									});
								if (open != registry_reader_opens_.end())
								{
									for (auto& group : reader_attachment_groups_)
										if (group.composite_close_owner_token == token &&
											group.composite_close_registry_open_token ==
												open->token)
											quarantine_reader_group_locked(
												group,
												0U,
												sqlite_shm_reader_terminal_quarantine_reason::
													owner_abandoned);
									quarantine_reader_open_locked(
										*open,
										0U,
										sqlite_shm_reader_terminal_quarantine_reason::
											owner_abandoned);
								}
								break;
							}
							case lease_token_kind::reader_session:
							{
								const auto session = find_by_token(reader_sessions_, token);
								if (session != reader_sessions_.end())
									quarantine_reader_session_locked(
										*session,
										sqlite_shm_reader_terminal_quarantine_reason::
											owner_abandoned);
								break;
							}
							case lease_token_kind::reader_attachment_map_inflight:
							{
								const auto map = find_by_token(reader_attachment_maps_, token);
								if (map != reader_attachment_maps_.end())
								{
									map->phase = reader_phase::terminal_quarantined;
									map->quarantine_reason =
										sqlite_shm_reader_terminal_quarantine_reason::
											owner_abandoned;
									const auto session =
										find_by_token(reader_sessions_, map->session_token);
									if (session != reader_sessions_.end())
										quarantine_reader_session_locked(
											*session,
											sqlite_shm_reader_terminal_quarantine_reason::
												owner_abandoned);
								}
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

			[[nodiscard]] bool
			reader_close_owner_handle_is_live(const std::uint64_t owner_token,
											  const std::uint64_t registry_open_token) noexcept
			{
				if (owner_token == 0U || registry_open_token == 0U)
					return false;
				try
				{
					std::scoped_lock lock{mutex_};
					if (!alive_)
						return false;
					const auto open =
						std::find_if(registry_reader_opens_.begin(),
									 registry_reader_opens_.end(),
									 [owner_token, registry_open_token](
										 const registry_reader_open_record& candidate)
									 {
										 return candidate.close_owner_token == owner_token &&
											 candidate.token == registry_open_token;
									 });
					return open != registry_reader_opens_.end() &&
						open->close_phase ==
						sqlite_shm_reader_connection_close_phase::close_admitted;
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
					return false;
				}
			}

			[[nodiscard]] bool reader_live_close_owner_handle_is_live(
				const std::uint64_t group_token,
				const std::uint64_t generation,
				const std::uint64_t close_owner_token,
				const std::uint64_t registry_open_token,
				const sqlite_shm_reader_live_close_obligation::phase phase) noexcept
			{
				if (group_token == 0U || generation == 0U || close_owner_token == 0U ||
					registry_open_token == 0U)
					return false;
				try
				{
					std::scoped_lock lock{mutex_};
					if (!alive_)
						return false;
					const auto group = find_by_token(reader_attachment_groups_, group_token);
					const auto open =
						std::find_if(registry_reader_opens_.begin(),
									 registry_reader_opens_.end(),
									 [close_owner_token, registry_open_token](
										 const registry_reader_open_record& candidate)
									 {
										 return candidate.close_owner_token == close_owner_token &&
											 candidate.token == registry_open_token;
									 });
					if (group == reader_attachment_groups_.end() ||
						open == registry_reader_opens_.end() || group->generation != generation ||
						group->composite_close_owner_token != close_owner_token ||
						group->composite_close_registry_open_token != registry_open_token ||
						open->close_phase !=
							sqlite_shm_reader_connection_close_phase::close_admitted ||
						open->close_route !=
							sqlite_shm_reader_close_route::close_after_confirmed_unmap)
						return false;
					if (phase == sqlite_shm_reader_live_close_obligation::phase::unmap_waiting)
						return group->reservation_phase ==
							sqlite_shm_reader_attachment_reservation_phase::observed_present &&
							group->phase == reader_attachment_group_phase::unmap_cut_sealing;
					if (phase == sqlite_shm_reader_live_close_obligation::phase::unmap_admitted)
						return group->reservation_phase ==
							sqlite_shm_reader_attachment_reservation_phase::observed_present &&
							group->phase == reader_attachment_group_phase::native_cleanup_admitted;
					return group->reservation_phase ==
						sqlite_shm_reader_attachment_reservation_phase::retired_confirmed &&
						group->phase == reader_attachment_group_phase::native_cleanup_confirmed;
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
					return false;
				}
			}

			void abandon_reader_live_close(sqlite_shm_reader_live_close_obligation& close) noexcept
			{
				if (close.group_token_ == 0U || close.close_owner_token_ == 0U)
					return;
				try
				{
					std::scoped_lock lock{mutex_};
					const auto group = find_by_token(reader_attachment_groups_, close.group_token_);
					const auto open = std::find_if(
						registry_reader_opens_.begin(),
						registry_reader_opens_.end(),
						[&close](const registry_reader_open_record& candidate)
						{
							return candidate.close_owner_token == close.close_owner_token_ &&
								candidate.token == close.registry_open_token_;
						});
					if (group != reader_attachment_groups_.end() &&
						(close.phase_ ==
							 sqlite_shm_reader_live_close_obligation::phase::unmap_waiting ||
						 close.phase_ ==
							 sqlite_shm_reader_live_close_obligation::phase::unmap_admitted))
						quarantine_reader_group_locked(
							*group,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::owner_abandoned);
					if (open != registry_reader_opens_.end())
						quarantine_reader_open_locked(
							*open,
							0U,
							sqlite_shm_reader_terminal_quarantine_reason::owner_abandoned);
					quarantine_locked();
					close.disarm();
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
					close.disable_terminal_presentation();
				}
			}

			void shutdown() noexcept
			{
				try
				{
					std::scoped_lock lock{mutex_};
					alive_ = false;
					if (!writers_.empty() || !holders_.empty() || !readers_.empty() ||
						!reader_attachment_maps_.empty() || !handoffs_.empty() ||
						!registry_reader_opens_.empty() || !reader_sessions_.empty() ||
						std::ranges::any_of(reader_attachment_groups_,
											[](const reader_attachment_group_record& group)
											{
												return !reader_reservation_is_compactable(group);
											}) ||
						generation_)
						quarantine_locked();
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
				}
			}

			[[nodiscard]] bool
			reader_unmap_native_effect_is_ready(const std::uint64_t token,
												const std::uint64_t generation) noexcept
			{
				if (token == 0U || generation == 0U)
					return false;
				try
				{
					std::scoped_lock lock{mutex_};
					const auto legacy = find_by_token(handoffs_, token);
					if (legacy != handoffs_.end())
						return legacy->generation == generation &&
							legacy->phase == handoff_phase::native_cleanup_admitted;
					const auto group = find_by_token(reader_attachment_groups_, token);
					return group != reader_attachment_groups_.end() &&
						group->generation == generation &&
						group->reservation_phase ==
						sqlite_shm_reader_attachment_reservation_phase::observed_present &&
						group->observed_identity &&
						group->phase == reader_attachment_group_phase::native_cleanup_admitted;
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
					return false;
				}
			}

			[[nodiscard]] bool reader_owner_handle_is_live(const lease_token_kind kind,
														   const std::uint64_t token,
														   const std::uint64_t generation) noexcept
			{
				if (token == 0U || generation == 0U)
					return false;
				try
				{
					std::scoped_lock lock{mutex_};
					if (!alive_)
						return false;
					switch (kind)
					{
						case lease_token_kind::reader_session:
						{
							const auto session = find_by_token(reader_sessions_, token);
							return session != reader_sessions_.end() &&
								session->generation == generation &&
								session->phase != reader_session_record_phase::terminal_quarantined;
						}
						case lease_token_kind::reader_attachment_map_inflight:
						{
							const auto map = find_by_token(reader_attachment_maps_, token);
							return map != reader_attachment_maps_.end() &&
								map->generation == generation &&
								map->phase == reader_phase::inflight;
						}
						case lease_token_kind::handoff:
						{
							const auto handoff = find_by_token(handoffs_, token);
							if (handoff != handoffs_.end())
								return handoff->generation == generation &&
									handoff->phase == handoff_phase::active;
							const auto group = find_by_token(reader_attachment_groups_, token);
							return group != reader_attachment_groups_.end() &&
								group->generation == generation &&
								group->reservation_phase ==
								sqlite_shm_reader_attachment_reservation_phase::observed_present &&
								group->observed_identity &&
								group->phase == reader_attachment_group_phase::active;
						}
						case lease_token_kind::reader_unpublished_cleanup:
						{
							const auto group = find_by_token(reader_attachment_groups_, token);
							return group != reader_attachment_groups_.end() &&
								group->generation == generation &&
								group->reservation_phase ==
								sqlite_shm_reader_attachment_reservation_phase::
									unpublished_cleanup_admitted &&
								group->phase ==
								reader_attachment_group_phase::native_cleanup_admitted;
						}
						case lease_token_kind::reader_unmap:
						{
							const auto handoff = find_by_token(handoffs_, token);
							if (handoff != handoffs_.end())
								return handoff->generation == generation &&
									handoff->phase == handoff_phase::native_cleanup_admitted;
							const auto group = find_by_token(reader_attachment_groups_, token);
							return group != reader_attachment_groups_.end() &&
								group->generation == generation &&
								group->reservation_phase ==
								sqlite_shm_reader_attachment_reservation_phase::observed_present &&
								group->observed_identity &&
								(group->phase == reader_attachment_group_phase::unmap_cut_sealing ||
								 group->phase ==
									 reader_attachment_group_phase::native_cleanup_admitted);
						}
						default:
							return false;
					}
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
					return false;
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

			struct reader_map_terminal_commit_injected_failure
			{
			};

			struct reader_session_terminal_commit_injected_failure
			{
			};

			struct reader_unpublished_cleanup_terminal_commit_injected_failure
			{
			};

			struct reader_predecessor_unmap_terminal_commit_injected_failure
			{
			};

			struct reader_logical_ack_commit_injected_failure
			{
			};

			struct reader_unmap_begin_preparation_injected_failure
			{
			};

			struct reader_close_begin_preparation_injected_failure
			{
			};

			struct reader_close_post_receipt_state_injected_failure
			{
			};

			struct reader_close_terminal_commit_injected_failure
			{
			};

			struct reader_coarse_unmap_terminal_injected_failure
			{
			};

			struct reader_recovery_mutex_reacquire_injected_failure
			{
			};

			struct reader_operation_mutex_acquire_injected_failure
			{
			};

			class reader_terminal_slot_guard
			{
			  public:
				reader_terminal_slot_guard(
					sqlite_shm_reader_lifecycle_sequence_source::state* source,
					const std::uint64_t slot) noexcept
					: source_{source}, slot_{slot}
				{
				}

				~reader_terminal_slot_guard()
				{
					if (source_ && slot_ != 0U)
						source_->cancel_terminal_reservation(slot_);
				}

				reader_terminal_slot_guard(const reader_terminal_slot_guard&) = delete;
				reader_terminal_slot_guard& operator=(const reader_terminal_slot_guard&) = delete;

				void release() noexcept
				{
					slot_ = 0U;
				}

			  private:
				sqlite_shm_reader_lifecycle_sequence_source::state* source_{};
				std::uint64_t slot_{};
			};

			class reader_candidate_authority_cancel_guard
			{
			  public:
				reader_candidate_authority_cancel_guard(
					sqlite_shm_reader_candidate_authority_minter& minter,
					sqlite_shm_reader_attachment_authority& authority) noexcept
					: minter_{&minter}, authority_{&authority}
				{
				}

				~reader_candidate_authority_cancel_guard()
				{
					if (minter_ && authority_)
						minter_->cancel(*authority_);
				}

				reader_candidate_authority_cancel_guard(
					const reader_candidate_authority_cancel_guard&) = delete;
				reader_candidate_authority_cancel_guard&
				operator=(const reader_candidate_authority_cancel_guard&) = delete;

				void release() noexcept
				{
					authority_ = nullptr;
				}

			  private:
				sqlite_shm_reader_candidate_authority_minter* minter_{};
				sqlite_shm_reader_attachment_authority* authority_{};
			};

			class reader_predelegate_authority_cancel_guard
			{
			  public:
				reader_predelegate_authority_cancel_guard(
					sqlite_shm_reader_map_predelegate_minter& minter,
					sqlite_shm_reader_map_predelegate_authority& authority) noexcept
					: minter_{&minter}, authority_{&authority}
				{
				}

				~reader_predelegate_authority_cancel_guard()
				{
					if (minter_ && authority_)
						minter_->cancel(*authority_);
				}

				reader_predelegate_authority_cancel_guard(
					const reader_predelegate_authority_cancel_guard&) = delete;
				reader_predelegate_authority_cancel_guard&
				operator=(const reader_predelegate_authority_cancel_guard&) = delete;

				void release() noexcept
				{
					authority_ = nullptr;
				}

			  private:
				sqlite_shm_reader_map_predelegate_minter* minter_{};
				sqlite_shm_reader_map_predelegate_authority* authority_{};
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

			struct registry_reader_open_record
			{
				registry_reader_open_record(
					const std::uint64_t registry_token,
					std::shared_ptr<sqlite_shm_reader_open_lineage_seal> lineage_seal,
					sqlite_shm_reader_open_epoch_binding open_binding,
					const std::uint64_t owner_token,
					const std::uint64_t cut_slot,
					const std::uint64_t terminal_slot)
					: token{registry_token}, seal{std::move(lineage_seal)},
					  binding{std::move(open_binding)}, close_owner_token{owner_token},
					  close_cut_sequence_slot{cut_slot},
					  close_terminal_sequence_slot{terminal_slot},
					  initial_close_cut_sequence_slot{cut_slot},
					  initial_close_terminal_sequence_slot{terminal_slot}
				{
				}

				std::uint64_t token{};
				std::shared_ptr<sqlite_shm_reader_open_lineage_seal> seal;
				sqlite_shm_reader_open_epoch_binding binding;
				std::uint64_t close_owner_token{};
				sqlite_shm_reader_connection_close_phase close_phase{
					sqlite_shm_reader_connection_close_phase::open};
				std::uint64_t close_origin_sequence{};
				std::uint64_t close_cut_sequence_slot{};
				std::uint64_t close_terminal_sequence_slot{};
				std::uint64_t initial_close_cut_sequence_slot{};
				std::uint64_t initial_close_terminal_sequence_slot{};
				std::optional<sqlite_shm_reader_close_route> close_route;
				std::uint64_t close_cut_sequence{};
				std::uint64_t close_terminal_sequence{};
				std::optional<sqlite_shm_callback_execution_receipt> close_callback;
				std::optional<sqlite_shm_verified_reader_close_terminal_receipt>
					close_terminal_receipt;
				sqlite_shm_reader_terminal_quarantine_reason quarantine_reason{
					sqlite_shm_reader_terminal_quarantine_reason::none};
			};

			struct reader_attachment_map_record
			{
				std::uint64_t token{};
				reader_phase phase{reader_phase::inflight};
				sqlite_shm_reader_attachment_map_request request;
				std::uint64_t generation{};
				sqlite_shm_mapping_tuple expected_mapping;
				std::size_t mapping_page_count{};
				std::optional<sqlite_shm_verified_reader_attachment_post_map_receipt> receipt;
				std::optional<sqlite_shm_verified_reader_attachment_zero_effect_receipt>
					zero_effect_receipt;
				std::uint64_t session_token{};
				std::uint64_t group_token{};
				bool retirement_blocker{};
				bool registry_bound{};
				std::optional<sqlite_shm_reader_map_predelegate_authority>
					registry_predelegate_authority;
				std::uint64_t admission_sequence{};
				std::uint64_t terminal_sequence_slot{};
				std::uint64_t terminal_sequence{};
				std::uint64_t potential_group_cut_sequence_slot{};
				std::uint64_t potential_group_terminal_sequence_slot{};
				bool unpublished_cleanup_required{};
				sqlite_shm_reader_terminal_quarantine_reason quarantine_reason{
					sqlite_shm_reader_terminal_quarantine_reason::none};
			};

			struct reader_attachment_zero_effect_terminal_record
			{
				std::uint64_t token{};
				std::uint64_t generation{};
				std::uint64_t session_token{};
				bool revoked_first_reservation{};
				sqlite_shm_reader_session_request session_request;
				sqlite_shm_verified_reader_attachment_zero_effect_receipt receipt;
				std::uint64_t session_origin_sequence{};
				std::uint64_t admission_sequence{};
				std::uint64_t terminal_sequence{};
			};

			struct reader_opaque_attachment_uncertainty_record
			{
				std::uint64_t map_token{};
				std::uint64_t generation{};
				std::uint64_t session_token{};
				std::uint64_t reservation_token{};
				sqlite_shm_reader_attachment_map_request map_request;
				sqlite_shm_reader_session_request session_request;
				std::uint64_t session_origin_sequence{};
				std::uint64_t map_admission_sequence{};
				std::uint64_t map_terminal_sequence{};
				std::uint64_t terminal_sequence{};
				bool registry_bound{};
				std::optional<sqlite_shm_reader_map_predelegate_authority>
					registry_predelegate_authority;
			};

			struct reader_predecessor_map_terminal_record
			{
				std::uint64_t token{};
				std::uint64_t generation{};
				std::uint64_t session_token{};
				std::uint64_t reservation_token{};
				sqlite_shm_reader_session_request session_request;
				sqlite_shm_verified_reader_predecessor_map_receipt receipt;
				std::uint64_t session_origin_sequence{};
				std::uint64_t admission_sequence{};
				std::uint64_t terminal_sequence{};
			};

			struct reader_existing_group_predecessor_mismatch_terminal_record
			{
				std::uint64_t token{};
				std::uint64_t generation{};
				std::uint64_t session_token{};
				std::uint64_t group_token{};
				sqlite_shm_verified_reader_predecessor_map_receipt receipt;
				std::uint64_t admission_sequence{};
				std::uint64_t terminal_sequence{};
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

			enum class reader_session_record_phase : std::uint8_t
			{
				reserved_for_first_map,
				active_group_owner,
				terminal_quarantined,
			};

			struct reader_session_record
			{
				std::uint64_t token{};
				std::uint64_t generation{};
				reader_session_record_phase phase{
					reader_session_record_phase::reserved_for_first_map};
				sqlite_shm_reader_session_request request;
				std::uint64_t group_token{};
				bool registry_bound{};
				std::vector<sqlite_shm_reader_cached_member_identity> captured_members;
				std::vector<std::uint64_t> captured_audits;
				sqlite_shm_reader_session_reservation_phase lifecycle_phase{
					sqlite_shm_reader_session_reservation_phase::reserved_before_sqlite};
				std::uint64_t lifecycle_origin_sequence{};
				std::uint64_t lifecycle_destination_sequence{};
				std::uint64_t terminal_sequence_slot{};
				std::uint64_t pending_terminal_sequence{};
				std::optional<sqlite_shm_reader_session_terminal_receipt> pending_terminal_receipt;
				sqlite_shm_reader_terminal_quarantine_reason quarantine_reason{
					sqlite_shm_reader_terminal_quarantine_reason::none};
			};

			enum class reader_attachment_group_phase : std::uint8_t
			{
				active,
				unmap_cut_sealing,
				native_cleanup_admitted,
				native_cleanup_confirmed,
				terminal_quarantined,
			};

			enum class reader_unmap_cut_blocker_decision : std::uint8_t
			{
				none,
				other_thread,
				same_thread_or_reentrant,
				ambiguous,
			};

			struct reader_attachment_group_member_record
			{
				sqlite_shm_reader_cached_member_identity identity;
			};

			struct reader_attachment_group_audit_record
			{
				std::uint64_t map_attempt_token{};
				std::uint64_t session_token{};
				sqlite_shm_reader_map_commit_kind kind{
					sqlite_shm_reader_map_commit_kind::existing_member_revalidation};
				sqlite_shm_verified_reader_attachment_post_map_receipt receipt;
				std::uint64_t admission_sequence{};
				std::uint64_t terminal_sequence{};
			};

			/**
			 * The one authoritative record for a reader attachment reservation and its optional
			 * observed group payload. A record is created before SQLite at first-session
			 * admission; positive map promotes this same node and zero-map/unmap terminalize it.
			 */
			struct reader_attachment_group_record
			{
				reader_attachment_group_record(
					const std::uint64_t record_token,
					const std::uint64_t record_generation,
					sqlite_shm_reader_attachment_reservation_identity record_expected)
					: token{record_token}, generation{record_generation},
					  expected{std::move(record_expected)}
				{
				}

				std::uint64_t token{};
				std::uint64_t generation{};
				reader_attachment_group_phase phase{reader_attachment_group_phase::active};
				sqlite_shm_reader_attachment_reservation_identity expected;
				std::optional<sqlite_shm_reader_native_attachment_identity> observed_identity;
				std::optional<sqlite_shm_verified_reader_unpublished_cleanup_receipt>
					unpublished_cleanup_receipt;
				std::vector<reader_attachment_group_member_record> members;
				std::vector<reader_attachment_group_audit_record> audits;
				std::optional<sqlite_shm_callback_execution_receipt> unpublished_cleanup_callback;
				std::optional<sqlite_shm_verified_reader_unpublished_cleanup_terminal_receipt>
					unpublished_cleanup_terminal_receipt;
				sqlite_shm_reader_logical_ack_phase logical_ack_phase{
					sqlite_shm_reader_logical_ack_phase::not_applicable};
				std::optional<sqlite_shm_callback_execution_receipt> logical_ack_callback;
				std::uint64_t unpublished_cleanup_cut_sequence{};
				std::uint64_t unpublished_cleanup_terminal_sequence{};
				std::uint64_t unpublished_cleanup_map_token{};
				std::uint64_t unpublished_cleanup_session_token{};
				std::uint64_t unpublished_cleanup_map_admission_sequence{};
				std::uint64_t unpublished_cleanup_map_terminal_sequence{};
				std::uint64_t unpublished_cleanup_session_origin_sequence{};
				std::uint64_t unpublished_cleanup_session_terminal_sequence{};
				std::uint64_t logical_ack_sequence_slot{};
				std::uint64_t logical_ack_sequence{};
				std::optional<sqlite_shm_callback_execution_receipt> unmap_callback;
				int unmap_caller_delete_flag{};
				int unmap_delegated_delete_flag{};
				bool registry_bound{};
				std::optional<sqlite_shm_reader_attachment_authority> registry_activity_authority;
				sqlite_shm_reader_attachment_reservation_phase reservation_phase{
					sqlite_shm_reader_attachment_reservation_phase::reserved};
				std::uint64_t reservation_origin_sequence{};
				std::uint64_t reservation_destination_sequence{};
				std::uint64_t group_origin_sequence{};
				std::uint64_t group_destination_sequence{};
				std::uint64_t unmap_cut_sequence{};
				std::uint64_t unmap_cut_sequence_slot{};
				std::uint64_t unmap_terminal_sequence{};
				std::uint64_t unmap_terminal_sequence_slot{};
				std::optional<sqlite_shm_verified_reader_unmap_terminal_receipt>
					unmap_terminal_receipt;
				std::optional<sqlite_shm_verified_reader_predecessor_unmap_terminal_receipt>
					predecessor_unmap_terminal_receipt;
				std::uint64_t predecessor_unmap_terminal_sequence{};
				std::uint64_t predecessor_close_terminal_sequence{};
				bool existing_group_deferred_cleanup_required{};
				std::uint64_t existing_group_deferred_cleanup_sequence{};
				std::uint64_t composite_close_owner_token{};
				std::uint64_t composite_close_registry_open_token{};
				std::uint64_t composite_close_cut_sequence{};
				std::uint64_t composite_close_wait_resolution_sequence_slot{};
				std::uint64_t composite_close_wait_resolution_sequence{};
				sqlite_shm_reader_replay_identity_tombstone compact_replay_identities;
				sqlite_shm_reader_terminal_quarantine_reason quarantine_reason{
					sqlite_shm_reader_terminal_quarantine_reason::none};
			};

			struct reader_session_terminal_record
			{
				std::uint64_t session_token{};
				reader_session_record_phase origin_phase{
					reader_session_record_phase::reserved_for_first_map};
				sqlite_shm_reader_session_terminal_receipt receipt;
				sqlite_shm_reader_session_reservation_phase lifecycle_phase{
					sqlite_shm_reader_session_reservation_phase::consumed_no_pointer};
				std::uint64_t lifecycle_origin_sequence{};
				std::uint64_t lifecycle_destination_sequence{};
				std::uint64_t terminal_sequence{};
			};

			struct reader_custody_record
			{
				reader_custody_record(
					const sqlite_shm_reader_custody_kind custody_kind,
					const sqlite_shm_reader_custody_state custody_state,
					std::optional<sqlite_shm_reader_attachment_reservation_identity>
						attachment_identity,
					const std::uint64_t token,
					const std::uint64_t origin,
					const std::uint64_t destination,
					std::optional<sqlite_shm_reader_open_epoch_binding> open_binding = std::nullopt)
					: kind{custody_kind}, state{custody_state},
					  attachment{std::move(attachment_identity)}, owner_token{token},
					  origin_sequence{origin}, destination_sequence{destination},
					  open_epoch{std::move(open_binding)}
				{
				}

				sqlite_shm_reader_custody_kind kind{sqlite_shm_reader_custody_kind::map_attempt};
				sqlite_shm_reader_custody_state state{sqlite_shm_reader_custody_state::live};
				std::optional<sqlite_shm_reader_attachment_reservation_identity> attachment;
				std::uint64_t owner_token{};
				std::uint64_t origin_sequence{};
				std::uint64_t destination_sequence{};
				std::optional<sqlite_shm_reader_open_epoch_binding> open_epoch;
			};

			struct reader_close_terminal_record
			{
				std::uint64_t registry_open_token{};
				std::uint64_t close_owner_token{};
				sqlite_shm_reader_open_epoch_binding binding;
				sqlite_shm_reader_close_route route{
					sqlite_shm_reader_close_route::close_without_group};
				sqlite_shm_reader_close_terminal_kind kind{
					sqlite_shm_reader_close_terminal_kind::terminal_quarantined};
				sqlite_shm_verified_reader_close_terminal_receipt receipt;
				int outward_status{};
				sqlite_shm_reader_terminal_quarantine_reason quarantine_reason{
					sqlite_shm_reader_terminal_quarantine_reason::none};
				std::uint64_t origin_sequence{};
				std::uint64_t cut_sequence{};
				std::uint64_t terminal_sequence{};
			};

			struct reader_lifecycle_sequence_batch
			{
				std::uint64_t first{};
				std::uint64_t last{};
				std::uint64_t terminal_slot{};
				generation_failure failure{generation_failure::unavailable};
				bool succeeded{};
			};

			struct reader_terminal_slot_reservation_batch
			{
				std::array<std::uint64_t, 4> slots{};
				generation_failure failure{generation_failure::unavailable};
				bool succeeded{};
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
				for (const auto& map : reader_attachment_maps_)
				{
					if (map.phase == reader_phase::terminal_quarantined || !map.retirement_blocker)
						continue;
					observe(map.request.callback);
				}
				if (std::ranges::any_of(reader_sessions_,
										[](const reader_session_record& session)
										{
											return session.phase ==
												reader_session_record_phase::reserved_for_first_map;
										}))
					has_blocker = true;
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

			[[nodiscard]] reader_lifecycle_sequence_batch
			consume_reader_lifecycle_terminal_slot_locked(const std::uint64_t slot) noexcept
			{
				if (!reader_lifecycle_sequences_ || !reader_lifecycle_sequences_->state_)
					return {};
				const auto minted =
					reader_lifecycle_sequences_->state_->consume_terminal_reservation(slot);
				if (!minted.succeeded)
					return {0U, 0U, 0U, minted.failure, false};
				return {minted.first,
						minted.last,
						minted.terminal_slot,
						generation_failure::unavailable,
						true};
			}

			[[nodiscard]] reader_lifecycle_sequence_batch
			consume_reader_lifecycle_terminal_slots_locked(const std::uint64_t slot,
														   const std::uint64_t joined_slot) noexcept
			{
				if (!reader_lifecycle_sequences_ || !reader_lifecycle_sequences_->state_)
					return {};
				const auto minted =
					reader_lifecycle_sequences_->state_->consume_terminal_reservation(slot,
																					  joined_slot);
				if (!minted.succeeded)
					return {0U, 0U, 0U, minted.failure, false};
				return {minted.first,
						minted.last,
						minted.terminal_slot,
						generation_failure::unavailable,
						true};
			}

			[[nodiscard]] reader_lifecycle_sequence_batch
			consume_reader_lifecycle_terminal_slots_locked(
				const std::span<const std::uint64_t> slots) noexcept
			{
				if (!reader_lifecycle_sequences_ || !reader_lifecycle_sequences_->state_)
					return {};
				const auto minted =
					reader_lifecycle_sequences_->state_->consume_terminal_reservations(slots);
				if (!minted.succeeded)
					return {0U, 0U, 0U, minted.failure, false};
				return {minted.first,
						minted.last,
						minted.terminal_slot,
						generation_failure::unavailable,
						true};
			}

			void cancel_reader_lifecycle_terminal_slot_locked(const std::uint64_t slot) noexcept
			{
				if (reader_lifecycle_sequences_ && reader_lifecycle_sequences_->state_)
					reader_lifecycle_sequences_->state_->cancel_terminal_reservation(slot);
			}

			[[nodiscard]] reader_terminal_slot_reservation_batch
			reserve_reader_lifecycle_admission_binding_locked(const std::size_t count) noexcept
			{
				if (!reader_lifecycle_sequences_ || !reader_lifecycle_sequences_->state_)
					return {};
				const auto reserved =
					reader_lifecycle_sequences_->state_->reserve_admission_binding(count);
				return {reserved.slots, reserved.failure, reserved.succeeded};
			}

			[[nodiscard]] reader_terminal_slot_reservation_batch
			reserve_reader_lifecycle_terminal_slots_locked(const std::size_t count) noexcept
			{
				if (!reader_lifecycle_sequences_ || !reader_lifecycle_sequences_->state_)
					return {};
				const auto reserved =
					reader_lifecycle_sequences_->state_->reserve_terminal_reservations(count);
				return {reserved.slots, reserved.failure, reserved.succeeded};
			}

			[[nodiscard]] reader_lifecycle_sequence_batch
			commit_reader_lifecycle_admission_binding_locked(
				const std::span<const std::uint64_t> slots) noexcept
			{
				if (!reader_lifecycle_sequences_ || !reader_lifecycle_sequences_->state_)
					return {};
				const auto minted =
					reader_lifecycle_sequences_->state_->commit_admission_binding(slots);
				if (!minted.succeeded)
					return {0U, 0U, 0U, minted.failure, false};
				return {minted.first,
						minted.last,
						minted.terminal_slot,
						generation_failure::unavailable,
						true};
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

			[[nodiscard]] bool reader_callback_invocation_was_seen_locked(
				const sqlite_backend_opaque_identity& invocation,
				const std::uint64_t excluded_writer_token = 0U,
				const std::uint64_t excluded_reader_group_token = 0U,
				const std::uint64_t excluded_registry_open_token = 0U) const noexcept
			{
				const auto replay_contains =
					[&invocation](const sqlite_shm_reader_replay_identity_tombstone& replay)
				{
					return std::ranges::find(replay.callback_invocation_tokens, invocation) !=
						replay.callback_invocation_tokens.end();
				};
				return std::ranges::any_of(
						   writers_,
						   [&invocation, excluded_writer_token](const writer_record& writer)
						   {
							   return writer.token != excluded_writer_token &&
								   writer.request.callback.invocation_token == invocation;
						   }) ||
					std::ranges::any_of(
						   holders_,
						   [&invocation](const holder_record& holder)
						   {
							   return holder.map_receipt.request().callback.invocation_token ==
								   invocation;
						   }) ||
					std::ranges::any_of(
						   writer_attachments_,
						   [&invocation](const writer_attachment_record& attachment)
						   {
							   return (attachment.cleanup_callback &&
									   attachment.cleanup_callback->invocation_token ==
										   invocation) ||
								   std::ranges::any_of(
										  attachment.sealed_member_audit,
										  [&invocation](
											  const writer_attachment_member_audit_record& audit)
										  {
											  return audit.request.callback.invocation_token ==
												  invocation;
										  });
						   }) ||
					std::ranges::any_of(
						   readers_,
						   [&invocation](const reader_record& reader)
						   {
							   return reader.request.callback.invocation_token == invocation ||
								   (reader.cleanup_callback &&
									reader.cleanup_callback->invocation_token == invocation);
						   }) ||
					std::ranges::any_of(reader_attachment_maps_,
										[&invocation](const reader_attachment_map_record& map)
										{
											return map.request.callback.invocation_token ==
												invocation;
										}) ||
					std::ranges::any_of(handoffs_,
										[&invocation](const handoff_record& handoff)
										{
											return handoff.post_map_receipt.request()
													   .callback.invocation_token == invocation ||
												(handoff.unmap_callback &&
												 handoff.unmap_callback->invocation_token ==
													 invocation);
										}) ||
					std::ranges::any_of(
						   reader_attachment_groups_,
						   [&invocation, excluded_reader_group_token, &replay_contains](
							   const reader_attachment_group_record& group)
						   {
							   return replay_contains(group.compact_replay_identities) ||
								   (group.unpublished_cleanup_receipt &&
									group.unpublished_cleanup_receipt->request()
											.callback.invocation_token == invocation) ||
								   (group.unpublished_cleanup_callback &&
									group.unpublished_cleanup_callback->invocation_token ==
										invocation) ||
								   (group.unpublished_cleanup_terminal_receipt &&
									group.unpublished_cleanup_terminal_receipt->callback()
											.invocation_token == invocation) ||
								   (group.logical_ack_callback &&
									group.logical_ack_callback->invocation_token == invocation) ||
								   (group.token != excluded_reader_group_token &&
									group.unmap_callback &&
									group.unmap_callback->invocation_token == invocation) ||
								   (group.unmap_terminal_receipt &&
									group.unmap_terminal_receipt->callback().invocation_token ==
										invocation) ||
								   (group.predecessor_unmap_terminal_receipt &&
									group.predecessor_unmap_terminal_receipt->callback()
											.invocation_token == invocation) ||
								   std::ranges::any_of(
										  group.audits,
										  [&invocation](
											  const reader_attachment_group_audit_record& audit)
										  {
											  return audit.receipt.request()
														 .callback.invocation_token == invocation;
										  });
						   }) ||
					std::ranges::any_of(
						   reader_attachment_zero_effect_terminals_,
						   [&invocation](
							   const reader_attachment_zero_effect_terminal_record& terminal)
						   {
							   return terminal.receipt.request().callback.invocation_token ==
								   invocation;
						   }) ||
					std::ranges::any_of(
						   reader_predecessor_map_terminals_,
						   [&invocation](const reader_predecessor_map_terminal_record& terminal)
						   {
							   return terminal.receipt.request().callback.invocation_token ==
								   invocation;
						   }) ||
					std::ranges::any_of(
						   reader_existing_group_predecessor_mismatch_terminals_,
						   [&invocation](
							   const reader_existing_group_predecessor_mismatch_terminal_record&
								   terminal)
						   {
							   return terminal.receipt.request().callback.invocation_token ==
								   invocation;
						   }) ||
					std::ranges::any_of(
						   registry_reader_opens_,
						   [&invocation,
							excluded_registry_open_token](const registry_reader_open_record& open)
						   {
							   return (open.token != excluded_registry_open_token &&
									   open.close_callback &&
									   open.close_callback->invocation_token == invocation) ||
								   (open.close_terminal_receipt &&
									open.close_terminal_receipt->callback().invocation_token ==
										invocation);
						   }) ||
					std::ranges::any_of(reader_close_terminals_,
										[&invocation](const reader_close_terminal_record& terminal)
										{
											return terminal.receipt.callback().invocation_token ==
												invocation;
										}) ||
					std::ranges::any_of(
						   reader_open_close_tombstones_,
						   [&replay_contains](
							   const sqlite_shm_reader_open_epoch_close_tombstone& tombstone)
						   {
							   return replay_contains(tombstone.replay_identities);
						   });
			}

			[[nodiscard]] bool reader_callback_was_completed_locked(
				const sqlite_shm_callback_execution_receipt& callback,
				const std::uint64_t excluded_writer_token = 0U,
				const std::uint64_t excluded_reader_group_token = 0U,
				const std::uint64_t excluded_registry_open_token = 0U) const noexcept
			{
				return reader_callback_invocation_was_seen_locked(callback.invocation_token,
																  excluded_writer_token,
																  excluded_reader_group_token,
																  excluded_registry_open_token);
			}

			[[nodiscard]] bool callback_can_start_locked(
				const sqlite_shm_callback_execution_receipt& callback,
				const std::uint64_t excluded_writer_token = 0U,
				const std::uint64_t excluded_reader_group_token = 0U,
				const std::uint64_t excluded_registry_open_token = 0U) const noexcept
			{
				if (!valid_callback(callback) ||
					reader_callback_was_completed_locked(callback,
														 excluded_writer_token,
														 excluded_reader_group_token,
														 excluded_registry_open_token) ||
					reader_effect_identity_seen_locked(callback.invocation_token, 0U, 0U, 0U) ||
					reader_session_terminal_identity_seen_locked(callback.invocation_token))
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
				if (std::ranges::any_of(reader_attachment_maps_,
										[&ordered_after, excluded_reader_group_token](
											const reader_attachment_map_record& map)
										{
											if (map.phase == reader_phase::terminal_quarantined ||
												(excluded_reader_group_token != 0U &&
												 map.group_token == excluded_reader_group_token))
												return false;
											return !ordered_after(map.request.callback);
										}))
					return false;
				return std::ranges::none_of(handoffs_,
											[&ordered_after](const handoff_record& handoff)
											{
												return handoff.phase ==
													handoff_phase::native_cleanup_admitted &&
													handoff.unmap_callback &&
													!ordered_after(*handoff.unmap_callback);
											}) &&
					std::ranges::none_of(
						   reader_attachment_groups_,
						   [&ordered_after, excluded_reader_group_token](
							   const reader_attachment_group_record& group)
						   {
							   return group.token != excluded_reader_group_token &&
								   group.phase ==
								   reader_attachment_group_phase::native_cleanup_admitted &&
								   ((group.unpublished_cleanup_callback &&
									 !ordered_after(*group.unpublished_cleanup_callback)) ||
									(group.unmap_callback &&
									 !ordered_after(*group.unmap_callback)));
						   }) &&
					std::ranges::none_of(
						   registry_reader_opens_,
						   [&ordered_after,
							excluded_registry_open_token](const registry_reader_open_record& open)
						   {
							   return open.token != excluded_registry_open_token &&
								   open.close_phase ==
								   sqlite_shm_reader_connection_close_phase::close_admitted &&
								   open.close_callback && !ordered_after(*open.close_callback);
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

			[[nodiscard]] std::size_t reader_registry_liveness_lost_count_locked() const noexcept
			{
				std::size_t lost{};
				for (const auto& open : registry_reader_opens_)
					if (!open.seal || !open.seal->authority_valid.load(std::memory_order_acquire))
						++lost;
				for (const auto& group : reader_attachment_groups_)
				{
					const auto expected_authority =
						group.registry_bound && !reader_reservation_is_compactable(group);
					if (group.registry_activity_authority.has_value() != expected_authority ||
						(group.registry_activity_authority &&
						 !group.registry_activity_authority->retains_exact_lifetimes(
							 group.expected)))
						++lost;
				}
				for (const auto& session : reader_sessions_)
				{
					const auto reserved =
						session.phase == reader_session_record_phase::reserved_for_first_map;
					auto invalid = false;
					if (session.registry_bound)
					{
						const auto group =
							find_by_token(reader_attachment_groups_, session.group_token);
						invalid = invalid || group == reader_attachment_groups_.end() ||
							!group->registry_bound ||
							group->expected != session.request.attachment ||
							(reserved ? group->reservation_phase !=
									 sqlite_shm_reader_attachment_reservation_phase::reserved
									  : group->reservation_phase !=
									 sqlite_shm_reader_attachment_reservation_phase::
										 observed_present) ||
							group->members.size() < session.captured_members.size() ||
							group->audits.size() < session.captured_audits.size();
						if (!invalid)
						{
							for (std::size_t index = 0; index < session.captured_members.size();
								 ++index)
								if (group->members[index].identity !=
									session.captured_members[index])
								{
									invalid = true;
									break;
								}
							for (std::size_t index = 0;
								 !invalid && index < session.captured_audits.size();
								 ++index)
								if (group->audits[index].map_attempt_token !=
									session.captured_audits[index])
									invalid = true;
						}
					}
					if (invalid)
						++lost;
				}
				for (const auto& map : reader_attachment_maps_)
				{
					const auto expected_authority = map.registry_bound && map.retirement_blocker;
					if (map.registry_predelegate_authority.has_value() != expected_authority ||
						(map.registry_predelegate_authority &&
						 !map.registry_predelegate_authority->valid_for_predelegation(map.request)))
						++lost;
				}
				return lost;
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
				const auto reader_registry_liveness_lost =
					reader_registry_liveness_lost_count_locked() != 0U;
				if (registry_member_admission_blocked_ || exact_member_liveness_lost ||
					reader_registry_liveness_lost)
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

			[[nodiscard]] static bool reader_reservation_is_compactable(
				const sqlite_shm_reader_attachment_reservation_phase phase) noexcept
			{
				using reservation_phase = sqlite_shm_reader_attachment_reservation_phase;
				return phase == reservation_phase::predecessor_route_retired_confirmed ||
					phase == reservation_phase::retired_confirmed ||
					phase == reservation_phase::revoked_no_map;
			}

			[[nodiscard]] static bool
			reader_reservation_is_compactable(const reader_attachment_group_record& group) noexcept
			{
				return reader_reservation_is_compactable(group.reservation_phase) ||
					(group.reservation_phase ==
						 sqlite_shm_reader_attachment_reservation_phase::
							 unpublished_cleanup_confirmed &&
					 (group.logical_ack_phase ==
						  sqlite_shm_reader_logical_ack_phase::consumed_by_exact_unmap ||
					  group.logical_ack_phase ==
						  sqlite_shm_reader_logical_ack_phase::consumed_by_close));
			}

			[[nodiscard]] static bool reader_reservation_is_compactable(
				const sqlite_shm_reader_lifecycle_compact_tombstone& tombstone) noexcept
			{
				return reader_reservation_is_compactable(tombstone.phase) ||
					(tombstone.phase ==
						 sqlite_shm_reader_attachment_reservation_phase::
							 unpublished_cleanup_confirmed &&
					 (tombstone.logical_ack_phase ==
						  sqlite_shm_reader_logical_ack_phase::consumed_by_exact_unmap ||
					  tombstone.logical_ack_phase ==
						  sqlite_shm_reader_logical_ack_phase::consumed_by_close));
			}

			[[nodiscard]] static bool reader_reservation_has_unresolved_group(
				const reader_attachment_group_record& group) noexcept
			{
				return group.observed_identity.has_value() &&
					!reader_reservation_is_compactable(group);
			}

			[[nodiscard]] static bool reader_attachment_matches_open_epoch_binding(
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

			[[nodiscard]] static bool reader_replay_identity_vector_is_exact(
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

			[[nodiscard]] static bool reader_replay_identity_tombstone_is_exact(
				const sqlite_shm_reader_replay_identity_tombstone& replay,
				const sqlite_shm_reader_attachment_reservation_phase phase,
				const sqlite_shm_reader_logical_ack_phase logical_ack_phase =
					sqlite_shm_reader_logical_ack_phase::not_applicable) noexcept
			{
				if (!reader_replay_identity_vector_is_exact(replay.callback_invocation_tokens) ||
					!reader_replay_identity_vector_is_exact(replay.effect_receipts) ||
					!reader_replay_identity_vector_is_exact(replay.session_terminal_receipts))
					return false;
				const auto domains_overlap =
					[](const std::vector<sqlite_backend_opaque_identity>& left,
					   const std::vector<sqlite_backend_opaque_identity>& right)
				{
					return std::ranges::any_of(
						left,
						[&right](const sqlite_backend_opaque_identity& identity)
						{
							return std::ranges::find(right, identity) != right.end();
						});
				};
				if (domains_overlap(replay.callback_invocation_tokens, replay.effect_receipts) ||
					domains_overlap(replay.callback_invocation_tokens,
									replay.session_terminal_receipts) ||
					domains_overlap(replay.effect_receipts, replay.session_terminal_receipts))
					return false;
				switch (phase)
				{
					case sqlite_shm_reader_attachment_reservation_phase::revoked_no_map:
						return logical_ack_phase ==
							sqlite_shm_reader_logical_ack_phase::not_applicable &&
							replay.session_terminal_receipts.empty() &&
							(replay.callback_free_terminal
								 ? replay.callback_invocation_tokens.empty() &&
									 replay.effect_receipts.empty()
								 : replay.callback_invocation_tokens.size() == 1U &&
									 replay.effect_receipts.size() == 1U);
					case sqlite_shm_reader_attachment_reservation_phase::retired_confirmed:
						return logical_ack_phase ==
							sqlite_shm_reader_logical_ack_phase::not_applicable &&
							!replay.callback_free_terminal &&
							replay.session_terminal_receipts.empty() &&
							replay.callback_invocation_tokens.size() >= 2U &&
							replay.effect_receipts.size() ==
							replay.callback_invocation_tokens.size() + 1U;
					case sqlite_shm_reader_attachment_reservation_phase::
						unpublished_cleanup_confirmed:
						return !replay.callback_free_terminal &&
							replay.effect_receipts.size() == 3U &&
							replay.session_terminal_receipts.size() == 1U &&
							((logical_ack_phase ==
								  sqlite_shm_reader_logical_ack_phase::consumed_by_exact_unmap &&
							  replay.callback_invocation_tokens.size() == 2U) ||
							 (logical_ack_phase ==
								  sqlite_shm_reader_logical_ack_phase::consumed_by_close &&
							  replay.callback_invocation_tokens.size() == 1U));
					case sqlite_shm_reader_attachment_reservation_phase::
						predecessor_route_retired_confirmed:
						return logical_ack_phase ==
							sqlite_shm_reader_logical_ack_phase::not_applicable &&
							!replay.callback_free_terminal &&
							replay.session_terminal_receipts.empty() &&
							(replay.callback_invocation_tokens.size() == 1U ||
							 replay.callback_invocation_tokens.size() == 2U) &&
							replay.effect_receipts.size() ==
							replay.callback_invocation_tokens.size();
					case sqlite_shm_reader_attachment_reservation_phase::reserved:
					case sqlite_shm_reader_attachment_reservation_phase::predecessor_route_active:
					case sqlite_shm_reader_attachment_reservation_phase::observed_present:
					case sqlite_shm_reader_attachment_reservation_phase::
						unpublished_cleanup_admitted:
					case sqlite_shm_reader_attachment_reservation_phase::terminal_quarantined:
						return false;
				}
				return false;
			}

			[[nodiscard]] static bool reader_close_replay_identity_tombstone_is_exact(
				const sqlite_shm_reader_replay_identity_tombstone& replay) noexcept
			{
				return !replay.callback_free_terminal &&
					replay.callback_invocation_tokens.size() == 1U &&
					replay.effect_receipts.size() == 1U &&
					replay.session_terminal_receipts.empty() &&
					reader_replay_identity_vector_is_exact(replay.callback_invocation_tokens) &&
					reader_replay_identity_vector_is_exact(replay.effect_receipts) &&
					reader_replay_identity_vector_is_exact(replay.session_terminal_receipts) &&
					replay.callback_invocation_tokens.front() != replay.effect_receipts.front();
			}

			[[nodiscard]] static bool reader_replay_identity_tombstones_overlap(
				const sqlite_shm_reader_replay_identity_tombstone& left,
				const sqlite_shm_reader_replay_identity_tombstone& right) noexcept
			{
				const auto right_contains = [&right](const sqlite_backend_opaque_identity& identity)
				{
					return std::ranges::find(right.callback_invocation_tokens, identity) !=
						right.callback_invocation_tokens.end() ||
						std::ranges::find(right.effect_receipts, identity) !=
						right.effect_receipts.end() ||
						std::ranges::find(right.session_terminal_receipts, identity) !=
						right.session_terminal_receipts.end();
				};
				return std::ranges::any_of(left.callback_invocation_tokens, right_contains) ||
					std::ranges::any_of(left.effect_receipts, right_contains) ||
					std::ranges::any_of(left.session_terminal_receipts, right_contains);
			}

			[[nodiscard]] bool reader_compact_group_common_shape_is_exact_locked(
				const reader_attachment_group_record& group) const noexcept
			{
				if (!group.registry_bound || group.expected.registry_open_token() == 0U ||
					group.generation == 0U ||
					group.generation != group.expected.writer_mapping_generation() ||
					!valid_reader_native_attachment(group.expected) ||
					group.reservation_origin_sequence == 0U ||
					group.reservation_destination_sequence <= group.reservation_origin_sequence ||
					group.registry_activity_authority || group.unmap_cut_sequence_slot != 0U ||
					group.unmap_terminal_sequence_slot != 0U ||
					group.logical_ack_sequence_slot != 0U ||
					group.composite_close_wait_resolution_sequence_slot != 0U ||
					std::ranges::count_if(reader_attachment_groups_,
										  [&group](const reader_attachment_group_record& candidate)
										  {
											  return candidate.registry_bound &&
												  candidate.expected == group.expected;
										  }) != 1)
					return false;
				if (std::ranges::any_of(reader_sessions_,
										[&group](const reader_session_record& session)
										{
											return session.request.attachment == group.expected;
										}) ||
					std::ranges::any_of(reader_attachment_maps_,
										[&group](const reader_attachment_map_record& map)
										{
											return map.request.expected_attachment ==
												group.expected;
										}) ||
					std::ranges::any_of(reader_custodies_,
										[&group](const reader_custody_record& custody)
										{
											return custody.attachment == group.expected &&
												(custody.state ==
													 sqlite_shm_reader_custody_state::live ||
												 custody.destination_sequence == 0U);
										}))
					return false;
				return true;
			}

			[[nodiscard]] bool reader_local_predecessor_group_shape_is_exact_locked(
				const reader_attachment_group_record& group, const bool retired) const noexcept
			{
				const auto has_close_cut = group.composite_close_owner_token != 0U ||
					group.composite_close_registry_open_token != 0U ||
					group.composite_close_cut_sequence != 0U ||
					group.composite_close_wait_resolution_sequence_slot != 0U ||
					group.composite_close_wait_resolution_sequence != 0U;
				const auto close_open =
					std::find_if(registry_reader_opens_.begin(),
								 registry_reader_opens_.end(),
								 [&group](const registry_reader_open_record& candidate)
								 {
									 return candidate.token == group.expected.registry_open_token();
								 });
				const auto phase1_close_waiting = close_open != registry_reader_opens_.end() &&
					reader_phase1_close_cut_is_exact_locked(group, *close_open);
				const auto phase1_close_resolved =
					(close_open != registry_reader_opens_.end() &&
					 reader_phase1_close_resolution_is_exact_locked(group, *close_open)) ||
					reader_phase1_close_resolution_tombstone_is_exact_locked(group);
				const auto phase1_close_cut = phase1_close_waiting || phase1_close_resolved;
				if (!group.registry_bound || group.token == 0U || group.generation == 0U ||
					group.generation != group.expected.writer_mapping_generation() ||
					!valid_reader_native_attachment(group.expected) ||
					group.reservation_origin_sequence == 0U || group.observed_identity ||
					!group.members.empty() || !group.audits.empty() ||
					group.unpublished_cleanup_receipt || group.unpublished_cleanup_callback ||
					group.unpublished_cleanup_terminal_receipt ||
					group.logical_ack_phase !=
						sqlite_shm_reader_logical_ack_phase::not_applicable ||
					group.logical_ack_callback || group.unpublished_cleanup_cut_sequence != 0U ||
					group.unpublished_cleanup_terminal_sequence != 0U ||
					group.unpublished_cleanup_map_token != 0U ||
					group.unpublished_cleanup_session_token != 0U ||
					group.unpublished_cleanup_map_admission_sequence != 0U ||
					group.unpublished_cleanup_map_terminal_sequence != 0U ||
					group.unpublished_cleanup_session_origin_sequence != 0U ||
					group.unpublished_cleanup_session_terminal_sequence != 0U ||
					group.logical_ack_sequence_slot != 0U || group.logical_ack_sequence != 0U ||
					group.unmap_callback || group.unmap_caller_delete_flag != 0 ||
					group.unmap_delegated_delete_flag != 0 || group.group_origin_sequence != 0U ||
					group.group_destination_sequence != 0U || group.unmap_cut_sequence != 0U ||
					group.unmap_cut_sequence_slot != 0U || group.unmap_terminal_sequence != 0U ||
					group.unmap_terminal_sequence_slot != 0U || group.unmap_terminal_receipt ||
					(has_close_cut && !phase1_close_cut) ||
					!group.compact_replay_identities.callback_invocation_tokens.empty() ||
					!group.compact_replay_identities.effect_receipts.empty() ||
					!group.compact_replay_identities.session_terminal_receipts.empty() ||
					group.compact_replay_identities.callback_free_terminal ||
					group.quarantine_reason != sqlite_shm_reader_terminal_quarantine_reason::none)
					return false;

				const auto matches_predecessor =
					[&group](const reader_predecessor_map_terminal_record& terminal)
				{
					return terminal.reservation_token == group.token &&
						terminal.generation == group.generation &&
						terminal.session_request.attachment == group.expected;
				};
				const auto predecessor = std::find_if(reader_predecessor_map_terminals_.begin(),
													  reader_predecessor_map_terminals_.end(),
													  matches_predecessor);
				if (predecessor == reader_predecessor_map_terminals_.end() ||
					std::ranges::count_if(reader_predecessor_map_terminals_, matches_predecessor) !=
						1 ||
					predecessor->token == 0U || predecessor->session_token == 0U ||
					predecessor->session_origin_sequence != group.reservation_origin_sequence ||
					predecessor->admission_sequence <= predecessor->session_origin_sequence ||
					predecessor->terminal_sequence <= predecessor->admission_sequence ||
					predecessor->receipt.request().expected_attachment != group.expected ||
					!valid_reader_predecessor_map_receipt(predecessor->receipt))
					return false;

				const auto exact_custody =
					[this, &group](const sqlite_shm_reader_custody_kind kind,
								   const sqlite_shm_reader_custody_state state,
								   const std::uint64_t owner_token,
								   const std::uint64_t origin,
								   const std::uint64_t destination)
				{
					return std::ranges::count_if(
							   reader_custodies_,
							   [&group, kind, state, owner_token, origin, destination](
								   const reader_custody_record& custody)
							   {
								   return custody.attachment == group.expected &&
									   custody.kind == kind && custody.state == state &&
									   custody.owner_token == owner_token &&
									   custody.origin_sequence == origin &&
									   custody.destination_sequence == destination;
							   }) == 1;
				};
				const auto exact_common_custody =
					std::ranges::count_if(reader_custodies_,
										  [&group](const reader_custody_record& custody)
										  {
											  return custody.attachment == group.expected;
										  }) == (phase1_close_cut ? 5 : 3) &&
					exact_custody(
						sqlite_shm_reader_custody_kind::map_attempt,
						sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt,
						predecessor->token,
						predecessor->admission_sequence,
						predecessor->terminal_sequence) &&
					exact_custody(sqlite_shm_reader_custody_kind::use_session_reservation,
								  sqlite_shm_reader_custody_state::transferred_to_exact_successor,
								  predecessor->session_token,
								  predecessor->session_origin_sequence,
								  predecessor->terminal_sequence) &&
					(!phase1_close_cut ||
					 (exact_custody(sqlite_shm_reader_custody_kind::bounded_waiter_or_continuation,
									phase1_close_waiting ? sqlite_shm_reader_custody_state::live
														 : sqlite_shm_reader_custody_state::
															   consumed_with_exact_terminal_receipt,
									group.token,
									group.composite_close_cut_sequence,
									phase1_close_waiting
										? 0U
										: group.composite_close_wait_resolution_sequence) &&
					  exact_custody(sqlite_shm_reader_custody_kind::terminal_reporter,
									phase1_close_waiting ? sqlite_shm_reader_custody_state::live
														 : sqlite_shm_reader_custody_state::
															   consumed_with_exact_terminal_receipt,
									group.token,
									group.composite_close_cut_sequence,
									phase1_close_waiting
										? 0U
										: group.composite_close_wait_resolution_sequence)));
				if (!exact_common_custody)
					return false;

				if (!retired)
					return group.phase == reader_attachment_group_phase::active &&
						group.reservation_phase ==
						sqlite_shm_reader_attachment_reservation_phase::predecessor_route_active &&
						group.reservation_destination_sequence == predecessor->terminal_sequence &&
						!group.predecessor_unmap_terminal_receipt &&
						group.predecessor_unmap_terminal_sequence == 0U &&
						group.predecessor_close_terminal_sequence == 0U &&
						group.registry_activity_authority &&
						group.registry_activity_authority->retains_exact_owned_drain_lifetimes(
							group.expected) &&
						exact_custody(
							   sqlite_shm_reader_custody_kind::
								   runtime_vfs_namespace_generation_native_mapping_lifetime_pin,
							   sqlite_shm_reader_custody_state::live,
							   group.token,
							   group.reservation_origin_sequence,
							   0U);

				if (!reader_compact_group_common_shape_is_exact_locked(group) ||
					group.phase != reader_attachment_group_phase::native_cleanup_confirmed ||
					group.reservation_phase !=
						sqlite_shm_reader_attachment_reservation_phase::
							predecessor_route_retired_confirmed)
					return false;
				if (group.predecessor_unmap_terminal_receipt)
				{
					const auto& receipt = *group.predecessor_unmap_terminal_receipt;
					const auto receipt_state = receipt.state_.lock();
					return group.predecessor_close_terminal_sequence == 0U &&
						group.predecessor_unmap_terminal_sequence >
						predecessor->terminal_sequence &&
						group.reservation_destination_sequence ==
						group.predecessor_unmap_terminal_sequence &&
						receipt_state.get() == this && receipt.token_ == predecessor->token &&
						receipt.reservation_token_ == group.token &&
						receipt.generation_ == group.generation &&
						valid_reader_predecessor_unmap_terminal_receipt(receipt) &&
						receipt.evidence_kind() ==
						sqlite_shm_reader_unmap_evidence_kind::exact_native_result &&
						receipt.native_status() &&
						*receipt.native_status() ==
						static_cast<int>(sqlite_native_map_status::ok) &&
						exact_custody(
							   sqlite_shm_reader_custody_kind::
								   runtime_vfs_namespace_generation_native_mapping_lifetime_pin,
							   sqlite_shm_reader_custody_state::
								   consumed_with_exact_terminal_receipt,
							   group.token,
							   group.reservation_origin_sequence,
							   group.predecessor_unmap_terminal_sequence);
				}

				if (group.predecessor_unmap_terminal_sequence != 0U ||
					group.predecessor_close_terminal_sequence <= predecessor->terminal_sequence ||
					group.reservation_destination_sequence !=
						group.predecessor_close_terminal_sequence ||
					!exact_custody(
						sqlite_shm_reader_custody_kind::
							runtime_vfs_namespace_generation_native_mapping_lifetime_pin,
						phase1_close_cut
							? sqlite_shm_reader_custody_state::transferred_to_durable_tombstone
							: sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt,
						group.token,
						group.reservation_origin_sequence,
						group.predecessor_close_terminal_sequence))
					return false;

				const auto exact_open = [&group](const registry_reader_open_record& open)
				{
					return open.token == group.expected.registry_open_token() &&
						reader_attachment_matches_open_epoch_binding(group.expected, open.binding);
				};
				const auto open = std::find_if(
					registry_reader_opens_.begin(), registry_reader_opens_.end(), exact_open);
				if (open != registry_reader_opens_.end())
					return open->close_phase == sqlite_shm_reader_connection_close_phase::closed &&
						open->close_route ==
						sqlite_shm_reader_close_route::close_existing_predecessor &&
						(phase1_close_cut
							 ? open->close_cut_sequence < predecessor->terminal_sequence &&
								 predecessor->terminal_sequence <
									 group.composite_close_wait_resolution_sequence
							 : open->close_cut_sequence > predecessor->terminal_sequence) &&
						open->close_terminal_sequence ==
						group.predecessor_close_terminal_sequence &&
						open->close_terminal_sequence > open->close_cut_sequence &&
						open->close_terminal_receipt &&
						open->close_terminal_receipt->evidence_kind() ==
						sqlite_shm_reader_close_evidence_kind::exact_native_result &&
						open->close_terminal_receipt->native_status() &&
						*open->close_terminal_receipt->native_status() ==
						static_cast<int>(sqlite_native_map_status::ok);

				const auto exact_tombstone =
					[&group](const sqlite_shm_reader_open_epoch_close_tombstone& tombstone)
				{
					return tombstone.registry_open_token == group.expected.registry_open_token() &&
						reader_attachment_matches_open_epoch_binding(group.expected,
																	 tombstone.binding);
				};
				const auto tombstone = std::find_if(reader_open_close_tombstones_.begin(),
													reader_open_close_tombstones_.end(),
													exact_tombstone);
				return tombstone != reader_open_close_tombstones_.end() &&
					std::ranges::count_if(reader_open_close_tombstones_, exact_tombstone) == 1 &&
					(phase1_close_cut
						 ? tombstone->close_cut_sequence < predecessor->terminal_sequence &&
							 predecessor->terminal_sequence <
								 group.composite_close_wait_resolution_sequence
						 : tombstone->close_cut_sequence > predecessor->terminal_sequence) &&
					tombstone->terminal_sequence == group.predecessor_close_terminal_sequence &&
					tombstone->terminal_sequence > tombstone->close_cut_sequence &&
					reader_close_replay_identity_tombstone_is_exact(tombstone->replay_identities);
			}

			[[nodiscard]] bool reader_imported_compact_group_shape_is_exact_locked(
				const reader_attachment_group_record& group) const noexcept
			{
				const auto has_any_composite_metadata = group.composite_close_owner_token != 0U ||
					group.composite_close_registry_open_token != 0U ||
					group.composite_close_cut_sequence != 0U;
				const auto exact_composite_metadata = !has_any_composite_metadata ||
					(group.composite_close_owner_token != 0U &&
					 group.composite_close_registry_open_token ==
						 group.expected.registry_open_token() &&
					 group.composite_close_cut_sequence > group.reservation_origin_sequence &&
					 ((group.composite_close_wait_resolution_sequence == 0U &&
					   group.composite_close_cut_sequence <
						   group.reservation_destination_sequence &&
					   group.reservation_phase ==
						   sqlite_shm_reader_attachment_reservation_phase::retired_confirmed &&
					   group.logical_ack_phase ==
						   sqlite_shm_reader_logical_ack_phase::not_applicable) ||
					  (group.composite_close_wait_resolution_sequence >
						   group.composite_close_cut_sequence &&
					   (group.reservation_phase ==
							sqlite_shm_reader_attachment_reservation_phase::revoked_no_map ||
						group.reservation_phase ==
							sqlite_shm_reader_attachment_reservation_phase::
								predecessor_route_retired_confirmed ||
						(group.reservation_phase ==
							 sqlite_shm_reader_attachment_reservation_phase::
								 unpublished_cleanup_confirmed &&
						 group.logical_ack_phase ==
							 sqlite_shm_reader_logical_ack_phase::consumed_by_close &&
						 group.logical_ack_sequence <
							 group.composite_close_wait_resolution_sequence)))));
				const auto common = reader_compact_group_common_shape_is_exact_locked(group) &&
					group.token == 0U && reader_reservation_is_compactable(group) &&
					exact_composite_metadata &&
					reader_replay_identity_tombstone_is_exact(group.compact_replay_identities,
															  group.reservation_phase,
															  group.logical_ack_phase) &&
					group.phase == reader_attachment_group_phase::active &&
					!group.observed_identity && group.members.empty() && group.audits.empty() &&
					!group.unpublished_cleanup_receipt && !group.unpublished_cleanup_callback &&
					!group.unpublished_cleanup_terminal_receipt && !group.logical_ack_callback &&
					!group.unmap_callback && group.unmap_caller_delete_flag == 0 &&
					group.unmap_delegated_delete_flag == 0 && group.group_origin_sequence == 0U &&
					group.group_destination_sequence == 0U && group.unmap_cut_sequence == 0U &&
					group.unmap_terminal_sequence == 0U && !group.unmap_terminal_receipt &&
					group.logical_ack_sequence_slot == 0U &&
					group.unpublished_cleanup_map_token == 0U &&
					group.unpublished_cleanup_session_token == 0U &&
					group.unpublished_cleanup_map_admission_sequence == 0U &&
					group.unpublished_cleanup_map_terminal_sequence == 0U &&
					group.quarantine_reason == sqlite_shm_reader_terminal_quarantine_reason::none;
				if (!common)
					return false;
				if (group.reservation_phase ==
					sqlite_shm_reader_attachment_reservation_phase::unpublished_cleanup_confirmed)
					return group.unpublished_cleanup_session_origin_sequence ==
						group.reservation_origin_sequence &&
						group.unpublished_cleanup_session_terminal_sequence >
						group.reservation_origin_sequence &&
						group.unpublished_cleanup_session_terminal_sequence -
							group.reservation_origin_sequence >
						2U &&
						group.unpublished_cleanup_session_terminal_sequence !=
						std::numeric_limits<std::uint64_t>::max() &&
						group.unpublished_cleanup_cut_sequence ==
						group.unpublished_cleanup_session_terminal_sequence + 1U &&
						group.unpublished_cleanup_terminal_sequence >
						group.unpublished_cleanup_cut_sequence &&
						group.reservation_destination_sequence ==
						group.unpublished_cleanup_terminal_sequence &&
						group.logical_ack_sequence > group.unpublished_cleanup_terminal_sequence;
				return group.logical_ack_phase ==
					sqlite_shm_reader_logical_ack_phase::not_applicable &&
					group.logical_ack_sequence == 0U &&
					group.unpublished_cleanup_cut_sequence == 0U &&
					group.unpublished_cleanup_terminal_sequence == 0U &&
					group.unpublished_cleanup_session_origin_sequence == 0U &&
					group.unpublished_cleanup_session_terminal_sequence == 0U;
			}

			[[nodiscard]] std::optional<sqlite_shm_reader_replay_identity_tombstone>
			reader_local_compact_replay_identities_locked(
				const reader_attachment_group_record& group) const
			{
				if (group.token == 0U)
					return std::nullopt;
				sqlite_shm_reader_replay_identity_tombstone replay;
				replay.callback_invocation_tokens.reserve(group.audits.size() + 3U);
				replay.effect_receipts.reserve(group.audits.size() + 4U);
				replay.session_terminal_receipts.reserve(1U);
				for (const auto& audit : group.audits)
				{
					replay.callback_invocation_tokens.push_back(
						audit.receipt.request().callback.invocation_token);
					replay.effect_receipts.push_back(audit.receipt.zero_resize_effect_receipt());
				}
				for (const auto& terminal : reader_attachment_zero_effect_terminals_)
				{
					if (terminal.session_request.attachment != group.expected)
						continue;
					replay.callback_invocation_tokens.push_back(
						terminal.receipt.request().callback.invocation_token);
					replay.effect_receipts.push_back(
						terminal.receipt.zero_attachment_effect_receipt());
				}
				const auto matches_existing_group_mismatch =
					[&group](
						const reader_existing_group_predecessor_mismatch_terminal_record& terminal)
				{
					return terminal.group_token == group.token &&
						terminal.receipt.request().expected_attachment == group.expected;
				};
				const auto mismatch_count =
					std::ranges::count_if(reader_existing_group_predecessor_mismatch_terminals_,
										  matches_existing_group_mismatch);
				const auto suppressed_count = std::ranges::count_if(
					group.audits,
					[](const reader_attachment_group_audit_record& audit)
					{
						return audit.kind ==
							sqlite_shm_reader_map_commit_kind::suppressed_after_cut;
					});
				if (group.existing_group_deferred_cleanup_required)
				{
					const auto first_terminal = group.existing_group_deferred_cleanup_sequence;
					std::size_t first_terminal_count{};
					if (first_terminal == 0U || mismatch_count + suppressed_count == 0U)
						return std::nullopt;
					for (const auto& terminal :
						 reader_existing_group_predecessor_mismatch_terminals_)
					{
						if (!matches_existing_group_mismatch(terminal))
							continue;
						if (terminal.token == 0U || terminal.generation != group.generation ||
							terminal.session_token == 0U ||
							terminal.admission_sequence <= group.group_origin_sequence ||
							terminal.terminal_sequence <= terminal.admission_sequence ||
							terminal.terminal_sequence < first_terminal ||
							!valid_reader_predecessor_map_receipt(terminal.receipt))
							return std::nullopt;
						first_terminal_count +=
							static_cast<std::size_t>(terminal.terminal_sequence == first_terminal);
						replay.callback_invocation_tokens.push_back(
							terminal.receipt.request().callback.invocation_token);
						replay.effect_receipts.push_back(terminal.receipt.native_effect_receipt());
					}
					for (const auto& audit : group.audits)
					{
						if (audit.kind != sqlite_shm_reader_map_commit_kind::suppressed_after_cut)
							continue;
						if (audit.map_attempt_token == 0U || audit.session_token == 0U ||
							audit.admission_sequence <= group.group_origin_sequence ||
							audit.terminal_sequence <= audit.admission_sequence ||
							audit.terminal_sequence < first_terminal ||
							!valid_reader_attachment_receipt(audit.receipt))
							return std::nullopt;
						first_terminal_count +=
							static_cast<std::size_t>(audit.terminal_sequence == first_terminal);
					}
					if (first_terminal_count != 1U)
						return std::nullopt;
				}
				else if (group.existing_group_deferred_cleanup_sequence != 0U ||
						 mismatch_count != 0 || suppressed_count != 0U)
					return std::nullopt;
				if (group.reservation_phase ==
					sqlite_shm_reader_attachment_reservation_phase::
						predecessor_route_retired_confirmed)
				{
					const auto predecessor = std::find_if(
						reader_predecessor_map_terminals_.begin(),
						reader_predecessor_map_terminals_.end(),
						[&group](const reader_predecessor_map_terminal_record& terminal)
						{
							return terminal.reservation_token == group.token &&
								terminal.session_request.attachment == group.expected;
						});
					if (predecessor == reader_predecessor_map_terminals_.end())
						return std::nullopt;
					replay.callback_invocation_tokens.push_back(
						predecessor->receipt.request().callback.invocation_token);
					replay.effect_receipts.push_back(predecessor->receipt.native_effect_receipt());
					if (group.predecessor_unmap_terminal_receipt)
					{
						if (group.predecessor_close_terminal_sequence != 0U ||
							!group.predecessor_unmap_terminal_receipt->native_effect_receipt())
							return std::nullopt;
						replay.callback_invocation_tokens.push_back(
							group.predecessor_unmap_terminal_receipt->callback().invocation_token);
						replay.effect_receipts.push_back(
							*group.predecessor_unmap_terminal_receipt->native_effect_receipt());
					}
					else if (group.predecessor_close_terminal_sequence == 0U)
						return std::nullopt;
				}
				else if (group.reservation_phase ==
						 sqlite_shm_reader_attachment_reservation_phase::retired_confirmed)
				{
					if (!group.unmap_terminal_receipt ||
						!group.unmap_terminal_receipt->native_effect_receipt() ||
						!group.unmap_terminal_receipt->latch_reset_receipt())
						return std::nullopt;
					replay.callback_invocation_tokens.push_back(
						group.unmap_terminal_receipt->callback().invocation_token);
					replay.effect_receipts.push_back(
						*group.unmap_terminal_receipt->native_effect_receipt());
					replay.effect_receipts.push_back(
						*group.unmap_terminal_receipt->latch_reset_receipt());
				}
				else if (group.reservation_phase ==
						 sqlite_shm_reader_attachment_reservation_phase::
							 unpublished_cleanup_confirmed)
				{
					if (!group.unpublished_cleanup_receipt ||
						!group.unpublished_cleanup_terminal_receipt ||
						!group.unpublished_cleanup_terminal_receipt->native_effect_receipt() ||
						!group.unpublished_cleanup_terminal_receipt->latch_reset_receipt() ||
						!group.unpublished_cleanup_callback ||
						*group.unpublished_cleanup_callback !=
							group.unpublished_cleanup_receipt->request().callback ||
						group.unpublished_cleanup_terminal_receipt->callback() !=
							*group.unpublished_cleanup_callback)
						return std::nullopt;
					replay.callback_invocation_tokens.push_back(
						group.unpublished_cleanup_receipt->request().callback.invocation_token);
					replay.effect_receipts.push_back(
						group.unpublished_cleanup_receipt->mapped_effect_receipt());
					replay.effect_receipts.push_back(
						*group.unpublished_cleanup_terminal_receipt->native_effect_receipt());
					replay.effect_receipts.push_back(
						*group.unpublished_cleanup_terminal_receipt->latch_reset_receipt());
					replay.session_terminal_receipts.push_back(
						group.unpublished_cleanup_receipt->session_no_pointer_terminal_receipt());
					if (group.logical_ack_phase ==
						sqlite_shm_reader_logical_ack_phase::consumed_by_exact_unmap)
					{
						if (!group.logical_ack_callback)
							return std::nullopt;
						replay.callback_invocation_tokens.push_back(
							group.logical_ack_callback->invocation_token);
					}
					else if (group.logical_ack_phase !=
								 sqlite_shm_reader_logical_ack_phase::consumed_by_close ||
							 group.logical_ack_callback)
						return std::nullopt;
				}
				else if (replay.callback_invocation_tokens.empty() &&
						 replay.effect_receipts.empty())
					replay.callback_free_terminal = true;
				if (!reader_replay_identity_tombstone_is_exact(
						replay, group.reservation_phase, group.logical_ack_phase))
					return std::nullopt;
				return replay;
			}

			[[nodiscard]] bool reader_local_compact_replay_census_is_exact_locked(
				const reader_attachment_group_record& group) const noexcept
			{
				const auto matches_zero_terminal =
					[&group](const reader_attachment_zero_effect_terminal_record& terminal)
				{
					return terminal.session_request.attachment == group.expected;
				};
				const auto matches_predecessor_terminal =
					[&group](const reader_predecessor_map_terminal_record& terminal)
				{
					return terminal.reservation_token == group.token &&
						terminal.session_request.attachment == group.expected;
				};
				const auto matches_existing_group_mismatch =
					[&group](
						const reader_existing_group_predecessor_mismatch_terminal_record& terminal)
				{
					return terminal.group_token == group.token &&
						terminal.receipt.request().expected_attachment == group.expected;
				};
				const auto callback_occurrences =
					[this,
					 &group,
					 &matches_zero_terminal,
					 &matches_predecessor_terminal,
					 &matches_existing_group_mismatch](
						const sqlite_backend_opaque_identity& identity)
				{
					auto count = static_cast<std::size_t>(std::ranges::count_if(
						group.audits,
						[&identity](const reader_attachment_group_audit_record& audit)
						{
							return audit.receipt.request().callback.invocation_token == identity;
						}));
					count += static_cast<std::size_t>(std::ranges::count_if(
						reader_attachment_zero_effect_terminals_,
						[&identity, &matches_zero_terminal](
							const reader_attachment_zero_effect_terminal_record& terminal)
						{
							return matches_zero_terminal(terminal) &&
								terminal.receipt.request().callback.invocation_token == identity;
						}));
					count += static_cast<std::size_t>(std::ranges::count_if(
						reader_predecessor_map_terminals_,
						[&identity, &matches_predecessor_terminal](
							const reader_predecessor_map_terminal_record& terminal)
						{
							return matches_predecessor_terminal(terminal) &&
								terminal.receipt.request().callback.invocation_token == identity;
						}));
					count += static_cast<std::size_t>(std::ranges::count_if(
						reader_existing_group_predecessor_mismatch_terminals_,
						[&identity, &matches_existing_group_mismatch](
							const reader_existing_group_predecessor_mismatch_terminal_record&
								terminal)
						{
							return matches_existing_group_mismatch(terminal) &&
								terminal.receipt.request().callback.invocation_token == identity;
						}));
					if (group.unmap_terminal_receipt &&
						group.unmap_terminal_receipt->callback().invocation_token == identity)
						++count;
					if (group.predecessor_unmap_terminal_receipt &&
						group.predecessor_unmap_terminal_receipt->callback().invocation_token ==
							identity)
						++count;
					return count;
				};
				const auto effect_occurrences = [this,
												 &group,
												 &matches_zero_terminal,
												 &matches_predecessor_terminal,
												 &matches_existing_group_mismatch](
													const sqlite_backend_opaque_identity& identity)
				{
					auto count = static_cast<std::size_t>(std::ranges::count_if(
						group.audits,
						[&identity](const reader_attachment_group_audit_record& audit)
						{
							return audit.receipt.zero_resize_effect_receipt() == identity;
						}));
					count += static_cast<std::size_t>(std::ranges::count_if(
						reader_attachment_zero_effect_terminals_,
						[&identity, &matches_zero_terminal](
							const reader_attachment_zero_effect_terminal_record& terminal)
						{
							return matches_zero_terminal(terminal) &&
								terminal.receipt.zero_attachment_effect_receipt() == identity;
						}));
					count += static_cast<std::size_t>(std::ranges::count_if(
						reader_predecessor_map_terminals_,
						[&identity, &matches_predecessor_terminal](
							const reader_predecessor_map_terminal_record& terminal)
						{
							return matches_predecessor_terminal(terminal) &&
								terminal.receipt.native_effect_receipt() == identity;
						}));
					count += static_cast<std::size_t>(std::ranges::count_if(
						reader_existing_group_predecessor_mismatch_terminals_,
						[&identity, &matches_existing_group_mismatch](
							const reader_existing_group_predecessor_mismatch_terminal_record&
								terminal)
						{
							return matches_existing_group_mismatch(terminal) &&
								terminal.receipt.native_effect_receipt() == identity;
						}));
					if (group.unmap_terminal_receipt)
					{
						if (group.unmap_terminal_receipt->native_effect_receipt() &&
							*group.unmap_terminal_receipt->native_effect_receipt() == identity)
							++count;
						if (group.unmap_terminal_receipt->latch_reset_receipt() &&
							*group.unmap_terminal_receipt->latch_reset_receipt() == identity)
							++count;
					}
					if (group.predecessor_unmap_terminal_receipt &&
						group.predecessor_unmap_terminal_receipt->native_effect_receipt() &&
						*group.predecessor_unmap_terminal_receipt->native_effect_receipt() ==
							identity)
						++count;
					return count;
				};
				for (const auto& audit : group.audits)
				{
					if (!valid_callback(audit.receipt.request().callback) ||
						!valid_identity(audit.receipt.zero_resize_effect_receipt()) ||
						callback_occurrences(audit.receipt.request().callback.invocation_token) !=
							1U ||
						effect_occurrences(audit.receipt.zero_resize_effect_receipt()) != 1U)
						return false;
				}
				for (const auto& terminal : reader_attachment_zero_effect_terminals_)
				{
					if (!matches_zero_terminal(terminal))
						continue;
					if (!valid_callback(terminal.receipt.request().callback) ||
						!valid_identity(terminal.receipt.zero_attachment_effect_receipt()) ||
						callback_occurrences(
							terminal.receipt.request().callback.invocation_token) != 1U ||
						effect_occurrences(terminal.receipt.zero_attachment_effect_receipt()) != 1U)
						return false;
				}
				const auto mismatch_count =
					std::ranges::count_if(reader_existing_group_predecessor_mismatch_terminals_,
										  matches_existing_group_mismatch);
				const auto suppressed_count = std::ranges::count_if(
					group.audits,
					[](const reader_attachment_group_audit_record& audit)
					{
						return audit.kind ==
							sqlite_shm_reader_map_commit_kind::suppressed_after_cut;
					});
				if (group.existing_group_deferred_cleanup_required)
				{
					const auto first_terminal = group.existing_group_deferred_cleanup_sequence;
					std::size_t first_terminal_count{};
					if (first_terminal == 0U || mismatch_count + suppressed_count == 0U)
						return false;
					for (const auto& terminal :
						 reader_existing_group_predecessor_mismatch_terminals_)
					{
						if (!matches_existing_group_mismatch(terminal))
							continue;
						if (terminal.token == 0U || terminal.generation != group.generation ||
							terminal.session_token == 0U ||
							terminal.admission_sequence <= group.group_origin_sequence ||
							terminal.terminal_sequence <= terminal.admission_sequence ||
							terminal.terminal_sequence < first_terminal ||
							!valid_reader_predecessor_map_receipt(terminal.receipt) ||
							callback_occurrences(
								terminal.receipt.request().callback.invocation_token) != 1U ||
							effect_occurrences(terminal.receipt.native_effect_receipt()) != 1U)
							return false;
						first_terminal_count +=
							static_cast<std::size_t>(terminal.terminal_sequence == first_terminal);
					}
					for (const auto& audit : group.audits)
					{
						if (audit.kind != sqlite_shm_reader_map_commit_kind::suppressed_after_cut)
							continue;
						if (audit.map_attempt_token == 0U || audit.session_token == 0U ||
							audit.admission_sequence <= group.group_origin_sequence ||
							audit.terminal_sequence <= audit.admission_sequence ||
							audit.terminal_sequence < first_terminal ||
							!valid_reader_attachment_receipt(audit.receipt) ||
							callback_occurrences(
								audit.receipt.request().callback.invocation_token) != 1U ||
							effect_occurrences(audit.receipt.zero_resize_effect_receipt()) != 1U)
							return false;
						first_terminal_count +=
							static_cast<std::size_t>(audit.terminal_sequence == first_terminal);
					}
					if (first_terminal_count != 1U)
						return false;
				}
				else if (group.existing_group_deferred_cleanup_sequence != 0U ||
						 mismatch_count != 0 || suppressed_count != 0U)
					return false;
				if (group.reservation_phase ==
					sqlite_shm_reader_attachment_reservation_phase::
						predecessor_route_retired_confirmed)
				{
					const auto predecessor = std::find_if(reader_predecessor_map_terminals_.begin(),
														  reader_predecessor_map_terminals_.end(),
														  matches_predecessor_terminal);
					if (predecessor == reader_predecessor_map_terminals_.end() ||
						std::ranges::count_if(reader_predecessor_map_terminals_,
											  matches_predecessor_terminal) != 1 ||
						callback_occurrences(
							predecessor->receipt.request().callback.invocation_token) != 1U ||
						effect_occurrences(predecessor->receipt.native_effect_receipt()) != 1U)
						return false;
					if (group.predecessor_unmap_terminal_receipt)
					{
						const auto& unmap = *group.predecessor_unmap_terminal_receipt;
						if (!unmap.native_effect_receipt() ||
							callback_occurrences(unmap.callback().invocation_token) != 1U ||
							effect_occurrences(*unmap.native_effect_receipt()) != 1U)
							return false;
					}
					const auto replay = reader_local_compact_replay_identities_locked(group);
					return replay &&
						reader_replay_identity_tombstone_is_exact(
							   *replay, group.reservation_phase, group.logical_ack_phase);
				}
				if (group.reservation_phase ==
					sqlite_shm_reader_attachment_reservation_phase::unpublished_cleanup_confirmed)
				{
					const auto close_open = std::find_if(
						registry_reader_opens_.begin(),
						registry_reader_opens_.end(),
						[&group](const registry_reader_open_record& candidate)
						{
							return candidate.token == group.expected.registry_open_token();
						});
					const auto phase1_close_resolution =
						(close_open != registry_reader_opens_.end() &&
						 reader_phase1_close_resolution_is_exact_locked(group, *close_open)) ||
						reader_phase1_close_resolution_tombstone_is_exact_locked(group);
					if (group.phase != reader_attachment_group_phase::native_cleanup_confirmed ||
						group.observed_identity || !group.members.empty() ||
						!group.audits.empty() || group.unmap_callback ||
						group.unmap_caller_delete_flag != 0 ||
						group.unmap_delegated_delete_flag != 0 || group.unmap_cut_sequence != 0U ||
						group.unmap_terminal_sequence != 0U || group.unmap_terminal_receipt ||
						!group.unpublished_cleanup_receipt || !group.unpublished_cleanup_callback ||
						!group.unpublished_cleanup_terminal_receipt ||
						group.logical_ack_sequence_slot != 0U || group.logical_ack_sequence == 0U ||
						std::ranges::any_of(reader_attachment_zero_effect_terminals_,
											matches_zero_terminal))
						return false;
					const auto cleanup_state = group.unpublished_cleanup_receipt->state_.lock();
					const auto terminal_state =
						group.unpublished_cleanup_terminal_receipt->state_.lock();
					const auto exact_sequence = group.unpublished_cleanup_session_origin_sequence ==
							group.reservation_origin_sequence &&
						group.unpublished_cleanup_map_token != 0U &&
						group.unpublished_cleanup_session_token != 0U &&
						group.unpublished_cleanup_map_token !=
							group.unpublished_cleanup_session_token &&
						group.unpublished_cleanup_map_admission_sequence >
							group.reservation_origin_sequence &&
						group.unpublished_cleanup_map_terminal_sequence >
							group.unpublished_cleanup_map_admission_sequence &&
						group.unpublished_cleanup_map_terminal_sequence !=
							std::numeric_limits<std::uint64_t>::max() &&
						group.unpublished_cleanup_session_terminal_sequence ==
							group.unpublished_cleanup_map_terminal_sequence + 1U &&
						group.unpublished_cleanup_session_terminal_sequence !=
							std::numeric_limits<std::uint64_t>::max() &&
						group.unpublished_cleanup_cut_sequence ==
							group.unpublished_cleanup_session_terminal_sequence + 1U &&
						group.unpublished_cleanup_terminal_sequence >
							group.unpublished_cleanup_cut_sequence &&
						group.reservation_destination_sequence ==
							group.unpublished_cleanup_terminal_sequence &&
						group.logical_ack_sequence > group.unpublished_cleanup_terminal_sequence;
					const auto exact_receipts = cleanup_state.get() == this &&
						group.unpublished_cleanup_receipt->token_ ==
							group.unpublished_cleanup_map_token &&
						group.unpublished_cleanup_receipt->generation() == group.generation &&
						valid_reader_unpublished_cleanup_receipt(
													*group.unpublished_cleanup_receipt) &&
						*group.unpublished_cleanup_callback ==
							group.unpublished_cleanup_receipt->request().callback &&
						terminal_state.get() == this &&
						group.unpublished_cleanup_terminal_receipt->token_ == group.token &&
						group.unpublished_cleanup_terminal_receipt->generation_ ==
							group.generation &&
						valid_reader_unpublished_cleanup_terminal_receipt(
													*group.unpublished_cleanup_terminal_receipt) &&
						group.unpublished_cleanup_terminal_receipt->callback() ==
							*group.unpublished_cleanup_callback &&
						group.unpublished_cleanup_terminal_receipt->evidence_kind() ==
							sqlite_shm_reader_unpublished_cleanup_evidence_kind::
								exact_native_result &&
						group.unpublished_cleanup_terminal_receipt->native_status() &&
						*group.unpublished_cleanup_terminal_receipt->native_status() ==
							static_cast<int>(sqlite_native_map_status::ok);
					const auto exact_custody =
						[this, &group](const sqlite_shm_reader_custody_kind kind,
									   const sqlite_shm_reader_custody_state state,
									   const std::uint64_t owner_token,
									   const std::uint64_t origin_sequence,
									   const std::uint64_t destination_sequence)
					{
						return std::ranges::count_if(
								   reader_custodies_,
								   [&group,
									kind,
									state,
									owner_token,
									origin_sequence,
									destination_sequence](const reader_custody_record& custody)
								   {
									   return custody.attachment == group.expected &&
										   custody.kind == kind && custody.state == state &&
										   custody.owner_token == owner_token &&
										   custody.origin_sequence == origin_sequence &&
										   custody.destination_sequence == destination_sequence;
								   }) == 1;
					};
					const auto exact_custody_census =
						std::ranges::count_if(reader_custodies_,
											  [&group](const reader_custody_record& custody)
											  {
												  return custody.attachment == group.expected;
											  }) == (phase1_close_resolution ? 9 : 7) &&
						exact_custody(
							sqlite_shm_reader_custody_kind::map_attempt,
							sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt,
							group.unpublished_cleanup_map_token,
							group.unpublished_cleanup_map_admission_sequence,
							group.unpublished_cleanup_map_terminal_sequence) &&
						exact_custody(
							sqlite_shm_reader_custody_kind::use_session_reservation,
							sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt,
							group.unpublished_cleanup_session_token,
							group.reservation_origin_sequence,
							group.unpublished_cleanup_session_terminal_sequence) &&
						exact_custody(
							sqlite_shm_reader_custody_kind::unmap_cut,
							sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt,
							group.token,
							group.unpublished_cleanup_cut_sequence,
							group.unpublished_cleanup_cut_sequence) &&
						exact_custody(
							sqlite_shm_reader_custody_kind::exact_present_attachment,
							sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt,
							group.token,
							group.unpublished_cleanup_cut_sequence,
							group.unpublished_cleanup_terminal_sequence) &&
						exact_custody(
							sqlite_shm_reader_custody_kind::unpublished_cleanup,
							sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt,
							group.token,
							group.unpublished_cleanup_cut_sequence,
							group.unpublished_cleanup_terminal_sequence) &&
						exact_custody(
							sqlite_shm_reader_custody_kind::logical_ack,
							sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt,
							group.token,
							group.unpublished_cleanup_terminal_sequence,
							group.logical_ack_sequence) &&
						exact_custody(
							sqlite_shm_reader_custody_kind::
								runtime_vfs_namespace_generation_native_mapping_lifetime_pin,
							sqlite_shm_reader_custody_state::transferred_to_durable_tombstone,
							group.token,
							group.reservation_origin_sequence,
							group.logical_ack_sequence);
					const auto replay = reader_local_compact_replay_identities_locked(group);
					return exact_sequence && exact_receipts && exact_custody_census && replay &&
						reader_replay_identity_tombstone_is_exact(
							   *replay, group.reservation_phase, group.logical_ack_phase);
				}
				if (group.reservation_phase ==
					sqlite_shm_reader_attachment_reservation_phase::revoked_no_map)
					return group.audits.empty() && !group.unmap_terminal_receipt &&
						std::ranges::count_if(reader_attachment_zero_effect_terminals_,
											  matches_zero_terminal) <= 1;
				if (group.reservation_phase !=
						sqlite_shm_reader_attachment_reservation_phase::retired_confirmed ||
					!group.unmap_terminal_receipt ||
					!valid_callback(group.unmap_terminal_receipt->callback()) ||
					!group.unmap_terminal_receipt->native_effect_receipt() ||
					!group.unmap_terminal_receipt->latch_reset_receipt())
					return false;
				return callback_occurrences(
						   group.unmap_terminal_receipt->callback().invocation_token) == 1U &&
					effect_occurrences(*group.unmap_terminal_receipt->native_effect_receipt()) ==
					1U &&
					effect_occurrences(*group.unmap_terminal_receipt->latch_reset_receipt()) == 1U;
			}

			[[nodiscard]] bool reader_local_phase1_compact_group_shape_is_exact_locked(
				const reader_attachment_group_record& group) const noexcept
			{
				const auto has_any_composite_metadata = group.composite_close_owner_token != 0U ||
					group.composite_close_registry_open_token != 0U ||
					group.composite_close_cut_sequence != 0U;
				const auto has_wait_resolution_metadata =
					group.composite_close_wait_resolution_sequence_slot != 0U ||
					group.composite_close_wait_resolution_sequence != 0U;
				const auto close_open =
					std::find_if(registry_reader_opens_.begin(),
								 registry_reader_opens_.end(),
								 [&group](const registry_reader_open_record& candidate)
								 {
									 return candidate.token == group.expected.registry_open_token();
								 });
				const auto exact_phase1_resolution =
					(close_open != registry_reader_opens_.end() &&
					 reader_phase1_close_resolution_is_exact_locked(group, *close_open)) ||
					reader_phase1_close_resolution_tombstone_is_exact_locked(group);
				const auto has_exact_composite_tuple = !has_any_composite_metadata ||
					(group.composite_close_owner_token != 0U &&
					 group.composite_close_registry_open_token != 0U &&
					 group.composite_close_cut_sequence != 0U);
				if (!reader_compact_group_common_shape_is_exact_locked(group) ||
					group.token == 0U || !has_exact_composite_tuple ||
					(has_wait_resolution_metadata && !exact_phase1_resolution) ||
					!group.compact_replay_identities.callback_invocation_tokens.empty() ||
					!group.compact_replay_identities.effect_receipts.empty() ||
					!group.compact_replay_identities.session_terminal_receipts.empty() ||
					group.compact_replay_identities.callback_free_terminal ||
					!reader_local_compact_replay_census_is_exact_locked(group) ||
					group.quarantine_reason != sqlite_shm_reader_terminal_quarantine_reason::none)
					return false;
				if (group.reservation_phase ==
					sqlite_shm_reader_attachment_reservation_phase::
						predecessor_route_retired_confirmed)
					return reader_local_predecessor_group_shape_is_exact_locked(group, true);
				if (group.reservation_phase ==
					sqlite_shm_reader_attachment_reservation_phase::unpublished_cleanup_confirmed)
					return (!has_any_composite_metadata || exact_phase1_resolution) &&
						reader_reservation_is_compactable(group);
				if (group.reservation_phase ==
					sqlite_shm_reader_attachment_reservation_phase::revoked_no_map)
					return group.phase == reader_attachment_group_phase::active &&
						!group.observed_identity && group.members.empty() && group.audits.empty() &&
						!group.unpublished_cleanup_receipt && !group.unpublished_cleanup_callback &&
						!group.unpublished_cleanup_terminal_receipt &&
						group.logical_ack_phase ==
						sqlite_shm_reader_logical_ack_phase::not_applicable &&
						!group.logical_ack_callback && group.unpublished_cleanup_map_token == 0U &&
						group.unpublished_cleanup_session_token == 0U &&
						group.unpublished_cleanup_map_admission_sequence == 0U &&
						group.unpublished_cleanup_map_terminal_sequence == 0U &&
						group.unpublished_cleanup_cut_sequence == 0U &&
						group.unpublished_cleanup_terminal_sequence == 0U &&
						group.unpublished_cleanup_session_origin_sequence == 0U &&
						group.unpublished_cleanup_session_terminal_sequence == 0U &&
						group.logical_ack_sequence_slot == 0U && group.logical_ack_sequence == 0U &&
						!group.unmap_callback && group.unmap_caller_delete_flag == 0 &&
						group.unmap_delegated_delete_flag == 0 &&
						group.group_origin_sequence == 0U &&
						group.group_destination_sequence == 0U && group.unmap_cut_sequence == 0U &&
						group.unmap_terminal_sequence == 0U && !group.unmap_terminal_receipt &&
						(!has_any_composite_metadata || exact_phase1_resolution) &&
						std::ranges::count_if(
							reader_attachment_zero_effect_terminals_,
							[&group](const reader_attachment_zero_effect_terminal_record& terminal)
							{
								return terminal.session_request.attachment == group.expected;
							}) <= 1;
				if (group.reservation_phase !=
						sqlite_shm_reader_attachment_reservation_phase::retired_confirmed ||
					group.phase != reader_attachment_group_phase::native_cleanup_confirmed ||
					!group.observed_identity ||
					!valid_observed_reader_native_attachment(*group.observed_identity) ||
					group.observed_identity->expected() != group.expected ||
					group.members.empty() || group.audits.empty() || group.unmap_callback ||
					group.unpublished_cleanup_receipt || group.unpublished_cleanup_callback ||
					group.unpublished_cleanup_terminal_receipt ||
					group.logical_ack_phase !=
						sqlite_shm_reader_logical_ack_phase::not_applicable ||
					group.logical_ack_callback || group.unpublished_cleanup_map_token != 0U ||
					group.unpublished_cleanup_session_token != 0U ||
					group.unpublished_cleanup_map_admission_sequence != 0U ||
					group.unpublished_cleanup_map_terminal_sequence != 0U ||
					group.unpublished_cleanup_cut_sequence != 0U ||
					group.unpublished_cleanup_terminal_sequence != 0U ||
					group.unpublished_cleanup_session_origin_sequence != 0U ||
					group.unpublished_cleanup_session_terminal_sequence != 0U ||
					group.logical_ack_sequence_slot != 0U || group.logical_ack_sequence != 0U ||
					group.unmap_caller_delete_flag != 0 || group.unmap_delegated_delete_flag != 0 ||
					!group.unmap_terminal_receipt)
					return false;
				const auto& receipt = *group.unmap_terminal_receipt;
				const auto receipt_state = receipt.state_.lock();
				return receipt_state.get() == this && receipt.token_ == group.token &&
					receipt.generation_ == group.generation &&
					valid_reader_unmap_terminal_receipt(receipt) &&
					receipt.evidence_kind() ==
					sqlite_shm_reader_unmap_evidence_kind::exact_native_result &&
					receipt.native_status() &&
					*receipt.native_status() == static_cast<int>(sqlite_native_map_status::ok) &&
					receipt.native_effect_receipt() && receipt.latch_reset_receipt() &&
					*receipt.native_effect_receipt() != *receipt.latch_reset_receipt() &&
					group.group_origin_sequence > group.reservation_origin_sequence &&
					(group.existing_group_deferred_cleanup_required ==
					 (group.existing_group_deferred_cleanup_sequence != 0U)) &&
					(!group.existing_group_deferred_cleanup_required ||
					 (group.existing_group_deferred_cleanup_sequence >
						  group.group_origin_sequence &&
					  group.existing_group_deferred_cleanup_sequence != group.unmap_cut_sequence &&
					  group.existing_group_deferred_cleanup_sequence <
						  group.unmap_terminal_sequence)) &&
					group.unmap_cut_sequence > group.group_origin_sequence &&
					group.unmap_terminal_sequence > group.unmap_cut_sequence &&
					group.reservation_destination_sequence == group.unmap_terminal_sequence &&
					group.group_destination_sequence == group.unmap_terminal_sequence &&
					(!has_any_composite_metadata ||
					 (group.composite_close_registry_open_token ==
						  group.expected.registry_open_token() &&
					  group.composite_close_cut_sequence == group.unmap_cut_sequence));
			}

			[[nodiscard]] bool reader_local_awaiting_ack_group_shape_is_exact_locked(
				const reader_attachment_group_record& group) const noexcept
			{
				const auto has_close_cut = group.composite_close_owner_token != 0U ||
					group.composite_close_registry_open_token != 0U ||
					group.composite_close_cut_sequence != 0U ||
					group.composite_close_wait_resolution_sequence_slot != 0U ||
					group.composite_close_wait_resolution_sequence != 0U;
				const auto close_open =
					std::find_if(registry_reader_opens_.begin(),
								 registry_reader_opens_.end(),
								 [&group](const registry_reader_open_record& candidate)
								 {
									 return candidate.token == group.expected.registry_open_token();
								 });
				const auto phase1_close_cut = close_open != registry_reader_opens_.end() &&
					reader_phase1_close_cut_is_exact_locked(group, *close_open);
				if (!group.registry_bound || group.token == 0U ||
					group.generation != group.expected.writer_mapping_generation() ||
					group.reservation_phase !=
						sqlite_shm_reader_attachment_reservation_phase::
							unpublished_cleanup_confirmed ||
					group.phase != reader_attachment_group_phase::native_cleanup_confirmed ||
					group.logical_ack_phase !=
						sqlite_shm_reader_logical_ack_phase::awaiting_sqlite_ack ||
					group.logical_ack_sequence_slot == 0U || group.logical_ack_sequence != 0U ||
					group.logical_ack_callback || !group.registry_activity_authority ||
					!group.registry_activity_authority->retains_exact_owned_drain_lifetimes(
						group.expected) ||
					group.quarantine_reason != sqlite_shm_reader_terminal_quarantine_reason::none ||
					group.observed_identity || !group.members.empty() || !group.audits.empty() ||
					group.unmap_callback || group.unmap_terminal_receipt ||
					group.unmap_cut_sequence != 0U || group.unmap_terminal_sequence != 0U ||
					(has_close_cut && !phase1_close_cut) ||
					!group.compact_replay_identities.callback_invocation_tokens.empty() ||
					!group.compact_replay_identities.effect_receipts.empty() ||
					!group.compact_replay_identities.session_terminal_receipts.empty() ||
					group.compact_replay_identities.callback_free_terminal ||
					!group.unpublished_cleanup_receipt || !group.unpublished_cleanup_callback ||
					!group.unpublished_cleanup_terminal_receipt ||
					std::ranges::any_of(reader_sessions_,
										[&group](const reader_session_record& session)
										{
											return session.request.attachment == group.expected;
										}) ||
					std::ranges::any_of(reader_attachment_maps_,
										[&group](const reader_attachment_map_record& map)
										{
											return map.request.expected_attachment ==
												group.expected;
										}) ||
					std::ranges::count_if(reader_attachment_groups_,
										  [&group](const reader_attachment_group_record& candidate)
										  {
											  return candidate.registry_bound &&
												  candidate.expected == group.expected;
										  }) != 1)
					return false;

				const auto cleanup_state = group.unpublished_cleanup_receipt->state_.lock();
				const auto terminal_state =
					group.unpublished_cleanup_terminal_receipt->state_.lock();
				const auto& native_effect =
					group.unpublished_cleanup_terminal_receipt->native_effect_receipt();
				const auto& latch_effect =
					group.unpublished_cleanup_terminal_receipt->latch_reset_receipt();
				if (cleanup_state.get() != this ||
					group.unpublished_cleanup_receipt->token_ !=
						group.unpublished_cleanup_map_token ||
					group.unpublished_cleanup_receipt->generation() != group.generation ||
					!valid_reader_unpublished_cleanup_receipt(*group.unpublished_cleanup_receipt) ||
					*group.unpublished_cleanup_callback !=
						group.unpublished_cleanup_receipt->request().callback ||
					terminal_state.get() != this ||
					group.unpublished_cleanup_terminal_receipt->token_ != group.token ||
					group.unpublished_cleanup_terminal_receipt->generation_ != group.generation ||
					!valid_reader_unpublished_cleanup_terminal_receipt(
						*group.unpublished_cleanup_terminal_receipt) ||
					group.unpublished_cleanup_terminal_receipt->callback() !=
						*group.unpublished_cleanup_callback ||
					group.unpublished_cleanup_terminal_receipt->evidence_kind() !=
						sqlite_shm_reader_unpublished_cleanup_evidence_kind::exact_native_result ||
					!group.unpublished_cleanup_terminal_receipt->native_status() ||
					*group.unpublished_cleanup_terminal_receipt->native_status() !=
						static_cast<int>(sqlite_native_map_status::ok) ||
					!native_effect || !latch_effect)
					return false;
				const auto& callback_identity =
					group.unpublished_cleanup_callback->invocation_token;
				const auto& mapped_effect =
					group.unpublished_cleanup_receipt->mapped_effect_receipt();
				const auto& session_terminal =
					group.unpublished_cleanup_receipt->session_no_pointer_terminal_receipt();
				if (callback_identity == mapped_effect || callback_identity == session_terminal ||
					callback_identity == *native_effect || callback_identity == *latch_effect ||
					mapped_effect == session_terminal || mapped_effect == *native_effect ||
					mapped_effect == *latch_effect || session_terminal == *native_effect ||
					session_terminal == *latch_effect || *native_effect == *latch_effect)
					return false;
				const auto exact_sequence = group.unpublished_cleanup_session_origin_sequence ==
						group.reservation_origin_sequence &&
					group.unpublished_cleanup_map_token != 0U &&
					group.unpublished_cleanup_session_token != 0U &&
					group.unpublished_cleanup_map_token !=
						group.unpublished_cleanup_session_token &&
					group.unpublished_cleanup_map_admission_sequence >
						group.reservation_origin_sequence &&
					group.unpublished_cleanup_map_terminal_sequence >
						group.unpublished_cleanup_map_admission_sequence &&
					group.unpublished_cleanup_map_terminal_sequence !=
						std::numeric_limits<std::uint64_t>::max() &&
					group.unpublished_cleanup_session_terminal_sequence ==
						group.unpublished_cleanup_map_terminal_sequence + 1U &&
					group.unpublished_cleanup_session_terminal_sequence !=
						std::numeric_limits<std::uint64_t>::max() &&
					group.unpublished_cleanup_cut_sequence ==
						group.unpublished_cleanup_session_terminal_sequence + 1U &&
					group.unpublished_cleanup_terminal_sequence >
						group.unpublished_cleanup_cut_sequence &&
					group.reservation_destination_sequence ==
						group.unpublished_cleanup_terminal_sequence;
				const auto exact_custody =
					[this, &group](const sqlite_shm_reader_custody_kind kind,
								   const sqlite_shm_reader_custody_state state,
								   const std::uint64_t owner_token,
								   const std::uint64_t origin_sequence,
								   const std::uint64_t destination_sequence)
				{
					return std::ranges::count_if(
							   reader_custodies_,
							   [&group,
								kind,
								state,
								owner_token,
								origin_sequence,
								destination_sequence](const reader_custody_record& custody)
							   {
								   return custody.attachment == group.expected &&
									   custody.kind == kind && custody.state == state &&
									   custody.owner_token == owner_token &&
									   custody.origin_sequence == origin_sequence &&
									   custody.destination_sequence == destination_sequence;
							   }) == 1;
				};
				return exact_sequence &&
					std::ranges::count_if(reader_custodies_,
										  [&group](const reader_custody_record& custody)
										  {
											  return custody.attachment == group.expected;
										  }) == (phase1_close_cut ? 9 : 7) &&
					exact_custody(
						   sqlite_shm_reader_custody_kind::map_attempt,
						   sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt,
						   group.unpublished_cleanup_map_token,
						   group.unpublished_cleanup_map_admission_sequence,
						   group.unpublished_cleanup_map_terminal_sequence) &&
					exact_custody(
						   sqlite_shm_reader_custody_kind::use_session_reservation,
						   sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt,
						   group.unpublished_cleanup_session_token,
						   group.reservation_origin_sequence,
						   group.unpublished_cleanup_session_terminal_sequence) &&
					exact_custody(
						   sqlite_shm_reader_custody_kind::unmap_cut,
						   sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt,
						   group.token,
						   group.unpublished_cleanup_cut_sequence,
						   group.unpublished_cleanup_cut_sequence) &&
					exact_custody(
						   sqlite_shm_reader_custody_kind::exact_present_attachment,
						   sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt,
						   group.token,
						   group.unpublished_cleanup_cut_sequence,
						   group.unpublished_cleanup_terminal_sequence) &&
					exact_custody(
						   sqlite_shm_reader_custody_kind::unpublished_cleanup,
						   sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt,
						   group.token,
						   group.unpublished_cleanup_cut_sequence,
						   group.unpublished_cleanup_terminal_sequence) &&
					exact_custody(sqlite_shm_reader_custody_kind::logical_ack,
								  sqlite_shm_reader_custody_state::live,
								  group.token,
								  group.unpublished_cleanup_terminal_sequence,
								  0U) &&
					exact_custody(sqlite_shm_reader_custody_kind::
									  runtime_vfs_namespace_generation_native_mapping_lifetime_pin,
								  sqlite_shm_reader_custody_state::live,
								  group.token,
								  group.reservation_origin_sequence,
								  0U) &&
					(!phase1_close_cut ||
					 (exact_custody(sqlite_shm_reader_custody_kind::bounded_waiter_or_continuation,
									sqlite_shm_reader_custody_state::live,
									group.token,
									group.composite_close_cut_sequence,
									0U) &&
					  exact_custody(sqlite_shm_reader_custody_kind::terminal_reporter,
									sqlite_shm_reader_custody_state::live,
									group.token,
									group.composite_close_cut_sequence,
									0U)));
			}

			[[nodiscard]] bool reader_phase1_close_cut_is_exact_locked(
				const reader_attachment_group_record& group,
				const registry_reader_open_record& open) const noexcept
			{
				if (!group.registry_bound || group.expected.registry_open_token() != open.token ||
					!reader_attachment_matches_open_epoch_binding(group.expected, open.binding) ||
					group.composite_close_owner_token != open.close_owner_token ||
					group.composite_close_registry_open_token != open.token ||
					group.composite_close_cut_sequence == 0U ||
					group.composite_close_cut_sequence != open.close_cut_sequence ||
					group.composite_close_wait_resolution_sequence_slot == 0U ||
					group.composite_close_wait_resolution_sequence != 0U ||
					open.close_phase != sqlite_shm_reader_connection_close_phase::close_admitted ||
					!open.close_callback)
					return false;
				const auto exact_live_custody =
					[this, &group](const sqlite_shm_reader_custody_kind kind)
				{
					return std::ranges::count_if(
							   reader_custodies_,
							   [&group, kind](const reader_custody_record& custody)
							   {
								   return custody.kind == kind &&
									   custody.state == sqlite_shm_reader_custody_state::live &&
									   custody.attachment == group.expected &&
									   custody.owner_token == group.token &&
									   custody.origin_sequence ==
									   group.composite_close_cut_sequence &&
									   custody.destination_sequence == 0U;
							   }) == 1;
				};
				return exact_live_custody(
						   sqlite_shm_reader_custody_kind::bounded_waiter_or_continuation) &&
					exact_live_custody(sqlite_shm_reader_custody_kind::terminal_reporter);
			}

			[[nodiscard]] bool reader_phase1_close_resolution_is_exact_locked(
				const reader_attachment_group_record& group,
				const registry_reader_open_record& open) const noexcept
			{
				if (!group.registry_bound || group.expected.registry_open_token() != open.token ||
					!reader_attachment_matches_open_epoch_binding(group.expected, open.binding) ||
					group.composite_close_owner_token != open.close_owner_token ||
					group.composite_close_registry_open_token != open.token ||
					group.composite_close_cut_sequence == 0U ||
					group.composite_close_cut_sequence != open.close_cut_sequence ||
					group.composite_close_wait_resolution_sequence_slot != 0U ||
					group.composite_close_wait_resolution_sequence <=
						group.composite_close_cut_sequence ||
					(open.close_phase != sqlite_shm_reader_connection_close_phase::close_admitted &&
					 open.close_phase != sqlite_shm_reader_connection_close_phase::closed) ||
					!open.close_route || !open.close_callback)
					return false;
				const auto exact_terminal_custody =
					[this, &group](const sqlite_shm_reader_custody_kind kind)
				{
					return std::ranges::count_if(
							   reader_custodies_,
							   [&group, kind](const reader_custody_record& custody)
							   {
								   return custody.kind == kind &&
									   custody.state ==
									   sqlite_shm_reader_custody_state::
										   consumed_with_exact_terminal_receipt &&
									   custody.attachment == group.expected &&
									   custody.owner_token == group.token &&
									   custody.origin_sequence ==
									   group.composite_close_cut_sequence &&
									   custody.destination_sequence ==
									   group.composite_close_wait_resolution_sequence;
							   }) == 1;
				};
				return exact_terminal_custody(
						   sqlite_shm_reader_custody_kind::bounded_waiter_or_continuation) &&
					exact_terminal_custody(sqlite_shm_reader_custody_kind::terminal_reporter);
			}

			[[nodiscard]] bool reader_phase1_close_resolution_tombstone_is_exact_locked(
				const reader_attachment_group_record& group) const noexcept
			{
				if (group.token == 0U || group.composite_close_owner_token == 0U ||
					group.composite_close_registry_open_token !=
						group.expected.registry_open_token() ||
					group.composite_close_cut_sequence == 0U ||
					group.composite_close_wait_resolution_sequence_slot != 0U ||
					group.composite_close_wait_resolution_sequence <=
						group.composite_close_cut_sequence)
					return false;
				const auto exact_close =
					[&group](const sqlite_shm_reader_open_epoch_close_tombstone& close)
				{
					return close.registry_open_token == group.composite_close_registry_open_token &&
						close.close_owner_token == group.composite_close_owner_token &&
						close.close_cut_sequence == group.composite_close_cut_sequence &&
						close.terminal_sequence > group.composite_close_wait_resolution_sequence &&
						reader_attachment_matches_open_epoch_binding(group.expected, close.binding);
				};
				const auto close = std::find_if(reader_open_close_tombstones_.begin(),
												reader_open_close_tombstones_.end(),
												exact_close);
				if (close == reader_open_close_tombstones_.end() ||
					std::ranges::count_if(reader_open_close_tombstones_, exact_close) != 1)
					return false;
				const auto exact_terminal_custody =
					[this, &group](const sqlite_shm_reader_custody_kind kind)
				{
					return std::ranges::count_if(
							   reader_custodies_,
							   [&group, kind](const reader_custody_record& custody)
							   {
								   return custody.kind == kind &&
									   custody.state ==
									   sqlite_shm_reader_custody_state::
										   consumed_with_exact_terminal_receipt &&
									   custody.attachment == group.expected &&
									   custody.owner_token == group.token &&
									   custody.origin_sequence ==
									   group.composite_close_cut_sequence &&
									   custody.destination_sequence ==
									   group.composite_close_wait_resolution_sequence;
							   }) == 1;
				};
				return exact_terminal_custody(
						   sqlite_shm_reader_custody_kind::bounded_waiter_or_continuation) &&
					exact_terminal_custody(sqlite_shm_reader_custody_kind::terminal_reporter);
			}

			[[nodiscard]] bool reader_phase1_revoked_group_shape_is_exact_locked(
				const reader_attachment_group_record& group,
				const registry_reader_open_record& open) const noexcept
			{
				if (!reader_phase1_close_cut_is_exact_locked(group, open) ||
					group.reservation_phase !=
						sqlite_shm_reader_attachment_reservation_phase::revoked_no_map ||
					group.phase != reader_attachment_group_phase::active ||
					group.observed_identity || !group.members.empty() || !group.audits.empty() ||
					group.unpublished_cleanup_receipt || group.unpublished_cleanup_callback ||
					group.unpublished_cleanup_terminal_receipt ||
					group.logical_ack_phase !=
						sqlite_shm_reader_logical_ack_phase::not_applicable ||
					group.logical_ack_callback || group.logical_ack_sequence_slot != 0U ||
					group.logical_ack_sequence != 0U || group.registry_activity_authority ||
					group.reservation_destination_sequence <= group.composite_close_cut_sequence ||
					std::ranges::any_of(reader_sessions_,
										[&group](const reader_session_record& session)
										{
											return session.request.attachment == group.expected;
										}) ||
					std::ranges::any_of(reader_attachment_maps_,
										[&group](const reader_attachment_map_record& map)
										{
											return map.request.expected_attachment ==
												group.expected;
										}))
					return false;
				for (const auto& custody : reader_custodies_)
				{
					if (custody.attachment != group.expected)
						continue;
					if (custody.state == sqlite_shm_reader_custody_state::live)
					{
						const auto exact_wait_owner = custody.owner_token == group.token &&
							(custody.kind ==
								 sqlite_shm_reader_custody_kind::bounded_waiter_or_continuation ||
							 custody.kind == sqlite_shm_reader_custody_kind::terminal_reporter) &&
							custody.origin_sequence == group.composite_close_cut_sequence &&
							custody.destination_sequence == 0U;
						if (!exact_wait_owner)
							return false;
					}
					else if (custody.destination_sequence == 0U)
						return false;
				}
				return std::ranges::count_if(
						   reader_attachment_zero_effect_terminals_,
						   [&group](const reader_attachment_zero_effect_terminal_record& terminal)
						   {
							   return terminal.session_request.attachment == group.expected;
						   }) <= 1;
			}

			[[nodiscard]] reader_unmap_cut_blocker_decision
			reader_phase1_close_cut_blocker_decision_locked(
				const reader_attachment_group_record& group,
				const registry_reader_open_record& open,
				const sqlite_shm_callback_execution_receipt& callback,
				const bool cut_installed) const noexcept
			{
				if (!group.registry_bound || group.token == 0U ||
					group.generation != group.expected.writer_mapping_generation() ||
					group.expected.registry_open_token() != open.token ||
					!reader_attachment_matches_open_epoch_binding(group.expected, open.binding) ||
					group.observed_identity || !group.members.empty() || !group.audits.empty() ||
					(cut_installed ? !reader_phase1_close_cut_is_exact_locked(group, open)
								   : group.composite_close_owner_token != 0U ||
							 group.composite_close_registry_open_token != 0U ||
							 group.composite_close_cut_sequence != 0U ||
							 group.composite_close_wait_resolution_sequence_slot != 0U ||
							 group.composite_close_wait_resolution_sequence != 0U))
					return reader_unmap_cut_blocker_decision::ambiguous;

				std::size_t blocker_count{};
				bool same_thread{};
				for (const auto& session : reader_sessions_)
				{
					const auto token_matches =
						session.request.attachment.registry_open_token() == open.token;
					const auto binding_matches = reader_attachment_matches_open_epoch_binding(
						session.request.attachment, open.binding);
					if (!token_matches && !binding_matches)
						continue;
					if (token_matches != binding_matches ||
						session.request.attachment != group.expected || !session.registry_bound ||
						session.phase != reader_session_record_phase::reserved_for_first_map ||
						session.group_token != group.token ||
						session.generation != group.generation ||
						session.lifecycle_phase !=
							sqlite_shm_reader_session_reservation_phase::reserved_before_sqlite ||
						std::ranges::count_if(
							reader_custodies_,
							[&group, &session](const reader_custody_record& custody)
							{
								return custody.kind ==
									sqlite_shm_reader_custody_kind::use_session_reservation &&
									custody.state == sqlite_shm_reader_custody_state::live &&
									custody.attachment == group.expected &&
									custody.owner_token == session.token &&
									custody.origin_sequence == session.lifecycle_origin_sequence &&
									custody.destination_sequence == 0U;
							}) != 1)
						return reader_unmap_cut_blocker_decision::ambiguous;
					++blocker_count;
					same_thread = same_thread ||
						session.request.execution.thread_identity == callback.thread_identity;
				}

				for (const auto& map : reader_attachment_maps_)
				{
					const auto token_matches =
						map.request.expected_attachment.registry_open_token() == open.token;
					const auto binding_matches = reader_attachment_matches_open_epoch_binding(
						map.request.expected_attachment, open.binding);
					if (!token_matches && !binding_matches)
						continue;
					if (token_matches != binding_matches ||
						map.request.expected_attachment != group.expected || !map.registry_bound ||
						map.phase != reader_phase::inflight || map.group_token != group.token ||
						map.generation != group.generation ||
						std::ranges::count_if(reader_custodies_,
											  [&group, &map](const reader_custody_record& custody)
											  {
												  return custody.kind ==
													  sqlite_shm_reader_custody_kind::map_attempt &&
													  custody.state ==
													  sqlite_shm_reader_custody_state::live &&
													  custody.attachment == group.expected &&
													  custody.owner_token == map.token &&
													  custody.origin_sequence ==
													  map.admission_sequence &&
													  custody.destination_sequence == 0U;
											  }) != 1)
						return reader_unmap_cut_blocker_decision::ambiguous;
					++blocker_count;
					same_thread = same_thread ||
						map.request.callback.thread_identity == callback.thread_identity;
				}

				if (group.reservation_phase ==
					sqlite_shm_reader_attachment_reservation_phase::unpublished_cleanup_admitted)
				{
					if (!cut_installed || blocker_count != 0U ||
						group.phase != reader_attachment_group_phase::native_cleanup_admitted ||
						!group.unpublished_cleanup_callback)
						return reader_unmap_cut_blocker_decision::ambiguous;
					++blocker_count;
					same_thread = same_thread ||
						group.unpublished_cleanup_callback->thread_identity ==
							callback.thread_identity;
				}
				else if (group.logical_ack_phase ==
						 sqlite_shm_reader_logical_ack_phase::awaiting_sqlite_ack)
				{
					if (!cut_installed || blocker_count != 0U)
						return reader_unmap_cut_blocker_decision::ambiguous;
				}
				else if (blocker_count == 0U &&
						 group.reservation_phase ==
							 sqlite_shm_reader_attachment_reservation_phase::reserved)
					return reader_unmap_cut_blocker_decision::ambiguous;

				if (blocker_count == 0U)
					return reader_unmap_cut_blocker_decision::none;
				if (callback.reentrancy_depth != 0U || same_thread)
					return reader_unmap_cut_blocker_decision::same_thread_or_reentrant;
				return reader_unmap_cut_blocker_decision::other_thread;
			}

			[[nodiscard]] std::optional<sqlite_shm_reader_close_route>
			reader_phase1_close_route_locked(const registry_reader_open_record& open) const noexcept
			{
				if (std::ranges::any_of(reader_sessions_,
										[&open](const reader_session_record& session)
										{
											return session.registry_bound &&
												(session.request.attachment.registry_open_token() ==
													 open.token ||
												 reader_attachment_matches_open_epoch_binding(
													 session.request.attachment, open.binding));
										}) ||
					std::ranges::any_of(
						reader_attachment_maps_,
						[&open](const reader_attachment_map_record& map)
						{
							return map.registry_bound &&
								(map.request.expected_attachment.registry_open_token() ==
									 open.token ||
								 reader_attachment_matches_open_epoch_binding(
									 map.request.expected_attachment, open.binding));
						}))
					return std::nullopt;

				auto route = sqlite_shm_reader_close_route::close_without_group;
				auto awaiting_ack_seen = false;
				auto active_predecessor_seen = false;
				for (const auto& group : reader_attachment_groups_)
				{
					if (!group.registry_bound)
						continue;
					const auto token_matches = group.expected.registry_open_token() == open.token;
					const auto binding_matches =
						reader_attachment_matches_open_epoch_binding(group.expected, open.binding);
					if (!token_matches && !binding_matches)
						continue;
					if (token_matches != binding_matches)
						return std::nullopt;
					if (group.reservation_phase ==
						sqlite_shm_reader_attachment_reservation_phase::predecessor_route_active)
					{
						if (awaiting_ack_seen || active_predecessor_seen ||
							!reader_local_predecessor_group_shape_is_exact_locked(group, false))
							return std::nullopt;
						active_predecessor_seen = true;
						route = sqlite_shm_reader_close_route::close_existing_predecessor;
						continue;
					}
					if (group.logical_ack_phase ==
						sqlite_shm_reader_logical_ack_phase::awaiting_sqlite_ack)
					{
						if (route == sqlite_shm_reader_close_route::close_existing_predecessor ||
							!reader_local_awaiting_ack_group_shape_is_exact_locked(group) ||
							(open.close_cut_sequence != 0U &&
							 group.reservation_destination_sequence >= open.close_cut_sequence))
							return std::nullopt;
						awaiting_ack_seen = true;
						route = sqlite_shm_reader_close_route::close_after_confirmed_unmap;
						continue;
					}
					if (!reader_local_phase1_compact_group_shape_is_exact_locked(group))
						return std::nullopt;
					if (open.close_cut_sequence != 0U &&
						group.reservation_destination_sequence >= open.close_cut_sequence)
					{
						const auto exact_live_composite_continuation =
							group.composite_close_owner_token == open.close_owner_token &&
							group.composite_close_registry_open_token == open.token &&
							group.composite_close_cut_sequence == open.close_cut_sequence &&
							group.reservation_phase ==
								sqlite_shm_reader_attachment_reservation_phase::retired_confirmed &&
							group.phase ==
								reader_attachment_group_phase::native_cleanup_confirmed &&
							group.unmap_terminal_sequence > open.close_cut_sequence;
						const auto exact_phase1_continuation =
							reader_phase1_close_resolution_is_exact_locked(group, open);
						if (!exact_live_composite_continuation && !exact_phase1_continuation)
							return std::nullopt;
					}
					if (group.reservation_phase ==
						sqlite_shm_reader_attachment_reservation_phase::
							predecessor_route_retired_confirmed)
					{
						if (route != sqlite_shm_reader_close_route::close_existing_predecessor)
							route = sqlite_shm_reader_close_route::close_after_confirmed_unmap;
						continue;
					}
					if (group.reservation_phase ==
						sqlite_shm_reader_attachment_reservation_phase::revoked_no_map)
						continue;
					if (group.reservation_phase ==
						sqlite_shm_reader_attachment_reservation_phase::retired_confirmed)
					{
						if (route != sqlite_shm_reader_close_route::close_existing_predecessor)
							route = sqlite_shm_reader_close_route::close_after_confirmed_unmap;
						continue;
					}
					if (group.reservation_phase ==
						sqlite_shm_reader_attachment_reservation_phase::
							unpublished_cleanup_confirmed)
					{
						if (route != sqlite_shm_reader_close_route::close_existing_predecessor)
							route = sqlite_shm_reader_close_route::close_after_confirmed_unmap;
						continue;
					}
					// Active proposal/composite, opaque, admitted cleanup, and every quarantined
					// reservation remain unrepresentable.
					return std::nullopt;
				}
				return route;
			}

			[[nodiscard]] bool reader_open_close_custody_census_is_exact_locked(
				const registry_reader_open_record& open,
				const bool close_admitted,
				const bool allow_awaiting_ack = false,
				const bool allow_active_predecessor = false,
				const reader_attachment_group_record* const ignored_live_group =
					nullptr) const noexcept
			{
				std::size_t live_connection_close{};
				std::size_t transferred_connection_close{};
				std::size_t live_close_cut{};
				std::size_t live_attachment_ack_custody{};
				std::size_t live_predecessor_pin_custody{};
				for (const auto& custody : reader_custodies_)
				{
					const auto attachment_token_matches_open = custody.attachment &&
						custody.attachment->registry_open_token() == open.token;
					const auto attachment_binding_matches_open = custody.attachment &&
						reader_attachment_matches_open_epoch_binding(*custody.attachment,
																	 open.binding);
					if (attachment_token_matches_open != attachment_binding_matches_open)
						return false;
					const auto attachment_matches_open =
						attachment_token_matches_open && attachment_binding_matches_open;
					const auto matches_open = custody.owner_token == open.close_owner_token ||
						(custody.open_epoch && *custody.open_epoch == open.binding) ||
						attachment_matches_open;
					if (!matches_open)
						continue;
					// The Phase-1 close routes are representable only after every
					// attachment-scoped custody for this open epoch has reached a durable
					// terminal.  Count against all custody kinds, not just the close-owner
					// rows, so a newly introduced attachment custody cannot silently escape
					// the close gate.
					if (attachment_matches_open)
					{
						if (ignored_live_group &&
							custody.attachment == ignored_live_group->expected)
						{
							const auto exact_composite = custody.kind ==
									sqlite_shm_reader_custody_kind::close_cut_or_composite &&
								custody.owner_token == open.close_owner_token &&
								custody.open_epoch && *custody.open_epoch == open.binding &&
								custody.state == sqlite_shm_reader_custody_state::live &&
								custody.origin_sequence == open.close_cut_sequence &&
								open.close_cut_sequence != 0U && custody.destination_sequence == 0U;
							if (exact_composite)
								++live_close_cut;
							continue;
						}
						if (custody.state == sqlite_shm_reader_custody_state::live)
						{
							if (custody.destination_sequence != 0U)
								return false;
							if (allow_awaiting_ack &&
								(custody.kind == sqlite_shm_reader_custody_kind::logical_ack ||
								 custody.kind ==
									 sqlite_shm_reader_custody_kind::
										 runtime_vfs_namespace_generation_native_mapping_lifetime_pin))
								++live_attachment_ack_custody;
							else if (
								allow_active_predecessor &&
								custody.kind ==
									sqlite_shm_reader_custody_kind::
										runtime_vfs_namespace_generation_native_mapping_lifetime_pin)
								++live_predecessor_pin_custody;
							else
								return false;
							continue;
						}
						if (custody.destination_sequence == 0U)
							return false;
						continue;
					}
					if (custody.owner_token != open.close_owner_token || custody.attachment ||
						!custody.open_epoch || *custody.open_epoch != open.binding)
						return false;
					if (custody.kind == sqlite_shm_reader_custody_kind::connection_close &&
						custody.state == sqlite_shm_reader_custody_state::live &&
						custody.origin_sequence == open.close_origin_sequence &&
						open.close_origin_sequence != 0U && custody.destination_sequence == 0U)
						++live_connection_close;
					else if (custody.kind == sqlite_shm_reader_custody_kind::connection_close &&
							 custody.state ==
								 sqlite_shm_reader_custody_state::transferred_to_exact_successor &&
							 custody.origin_sequence == open.close_origin_sequence &&
							 open.close_origin_sequence != 0U &&
							 custody.destination_sequence == open.close_cut_sequence &&
							 open.close_cut_sequence != 0U)
						++transferred_connection_close;
					else if (custody.kind ==
								 sqlite_shm_reader_custody_kind::close_cut_or_composite &&
							 custody.state == sqlite_shm_reader_custody_state::live &&
							 custody.origin_sequence == open.close_cut_sequence &&
							 open.close_cut_sequence != 0U && custody.destination_sequence == 0U)
						++live_close_cut;
					else
						return false;
				}
				if (allow_awaiting_ack && allow_active_predecessor)
					return false;
				const auto exact_attachment_custody = allow_awaiting_ack
					? live_attachment_ack_custody == 2U && live_predecessor_pin_custody == 0U
					: allow_active_predecessor
					? live_attachment_ack_custody == 0U && live_predecessor_pin_custody == 1U
					: live_attachment_ack_custody == 0U && live_predecessor_pin_custody == 0U;
				return exact_attachment_custody &&
					(close_admitted ? live_connection_close == 0U &&
							 transferred_connection_close == 1U && live_close_cut == 1U
									: live_connection_close == 1U &&
							 transferred_connection_close == 0U && live_close_cut == 0U);
			}

			void quarantine_reader_open_locked(
				registry_reader_open_record& open,
				std::uint64_t terminal_sequence = 0U,
				const sqlite_shm_reader_terminal_quarantine_reason reason =
					sqlite_shm_reader_terminal_quarantine_reason::presented_invalid) noexcept
			{
				std::array<std::uint64_t, 2> slots{};
				std::size_t slot_count{};
				if (open.close_cut_sequence_slot != 0U)
					slots[slot_count++] = open.close_cut_sequence_slot;
				if (open.close_terminal_sequence_slot != 0U)
					slots[slot_count++] = open.close_terminal_sequence_slot;
				if (slot_count != 0U)
				{
					const auto terminal = consume_reader_lifecycle_terminal_slots_locked(
						std::span{slots}.first(slot_count));
					if (terminal.succeeded)
						terminal_sequence = std::max(terminal_sequence, terminal.last);
					else
						emergency_quarantine_.store(true, std::memory_order_release);
					open.close_cut_sequence_slot = 0U;
					open.close_terminal_sequence_slot = 0U;
				}
				open.close_phase = sqlite_shm_reader_connection_close_phase::terminal_quarantined;
				if (open.quarantine_reason == sqlite_shm_reader_terminal_quarantine_reason::none)
					open.quarantine_reason = reason;
				if (terminal_sequence != 0U)
				{
					open.close_terminal_sequence =
						std::max(open.close_terminal_sequence, terminal_sequence);
					for (auto& custody : reader_custodies_)
					{
						if (custody.owner_token != open.close_owner_token || !custody.open_epoch ||
							*custody.open_epoch != open.binding ||
							custody.state != sqlite_shm_reader_custody_state::live)
							continue;
						custody.state =
							sqlite_shm_reader_custody_state::transferred_to_durable_tombstone;
						custody.destination_sequence = terminal_sequence;
					}
					reader_last_committed_sequence_ =
						std::max(reader_last_committed_sequence_, terminal_sequence);
				}
				quarantine_locked();
			}

			void quarantine_reader_composite_open_locked(
				const reader_attachment_group_record& group,
				const std::uint64_t terminal_sequence,
				const sqlite_shm_reader_terminal_quarantine_reason reason) noexcept
			{
				const auto has_any_composite_metadata = group.composite_close_owner_token != 0U ||
					group.composite_close_registry_open_token != 0U ||
					group.composite_close_cut_sequence != 0U;
				if (!has_any_composite_metadata)
					return;

				const auto open = std::find_if(
					registry_reader_opens_.begin(),
					registry_reader_opens_.end(),
					[&group](const registry_reader_open_record& candidate)
					{
						return candidate.token == group.expected.registry_open_token() &&
							reader_attachment_matches_open_epoch_binding(group.expected,
																		 candidate.binding);
					});
				if (open == registry_reader_opens_.end())
				{
					emergency_quarantine_.store(true, std::memory_order_release);
					quarantine_locked();
					return;
				}
				quarantine_reader_open_locked(*open, terminal_sequence, reason);
			}

			void
			quarantine_reader_close_terminal(sqlite_shm_reader_close_obligation& close) noexcept
			{
				const auto owned = owns(close.state_, close.owner_token_);
				bool transitioned{};
				try
				{
					if (fail_next_reader_recovery_mutex_reacquire_for_testing_.exchange(
							false, std::memory_order_acq_rel))
						throw reader_recovery_mutex_reacquire_injected_failure{};
					std::scoped_lock lock{mutex_};
					if (owns(close.state_, close.owner_token_))
					{
						const auto open =
							std::find_if(registry_reader_opens_.begin(),
										 registry_reader_opens_.end(),
										 [&close](const registry_reader_open_record& candidate)
										 {
											 return candidate.token == close.registry_open_token_ &&
												 candidate.close_owner_token == close.owner_token_;
										 });
						if (open != registry_reader_opens_.end())
						{
							for (auto& group : reader_attachment_groups_)
								if (group.composite_close_owner_token == open->close_owner_token &&
									group.composite_close_registry_open_token == open->token)
									quarantine_reader_group_locked(
										group,
										0U,
										sqlite_shm_reader_terminal_quarantine_reason::
											internal_failure);
							quarantine_reader_open_locked(
								*open,
								0U,
								sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
							if (open->close_route ==
								sqlite_shm_reader_close_route::close_existing_predecessor)
							{
								auto predecessor = reader_attachment_groups_.end();
								for (auto group = reader_attachment_groups_.begin();
									 group != reader_attachment_groups_.end();
									 ++group)
								{
									if (!group->registry_bound ||
										group->expected.registry_open_token() != open->token ||
										!reader_attachment_matches_open_epoch_binding(
											group->expected, open->binding) ||
										group->reservation_phase !=
											sqlite_shm_reader_attachment_reservation_phase::
												predecessor_route_active)
										continue;
									if (predecessor != reader_attachment_groups_.end())
										quarantine_reader_group_locked(
											*predecessor,
											open->close_terminal_sequence,
											sqlite_shm_reader_terminal_quarantine_reason::
												internal_failure);
									predecessor = group;
								}
								if (predecessor != reader_attachment_groups_.end())
									quarantine_reader_group_locked(
										*predecessor,
										open->close_terminal_sequence,
										sqlite_shm_reader_terminal_quarantine_reason::
											internal_failure);
							}
							transitioned = true;
							const auto terminal = std::find_if(
								reader_close_terminals_.begin(),
								reader_close_terminals_.end(),
								[&close](const reader_close_terminal_record& candidate)
								{
									return candidate.registry_open_token ==
										close.registry_open_token_ &&
										candidate.close_owner_token == close.owner_token_;
								});
							if (terminal != reader_close_terminals_.end())
							{
								terminal->kind =
									sqlite_shm_reader_close_terminal_kind::terminal_quarantined;
								terminal->outward_status = sqlite_ioerr_status;
								terminal->quarantine_reason =
									sqlite_shm_reader_terminal_quarantine_reason::internal_failure;
							}
							else if (open->close_terminal_receipt && open->close_route &&
									 open->close_origin_sequence != 0U &&
									 open->close_cut_sequence != 0U &&
									 open->close_terminal_sequence != 0U)
							{
								reader_close_terminals_.push_back(
									{open->token,
									 open->close_owner_token,
									 open->binding,
									 *open->close_route,
									 sqlite_shm_reader_close_terminal_kind::terminal_quarantined,
									 *open->close_terminal_receipt,
									 sqlite_ioerr_status,
									 sqlite_shm_reader_terminal_quarantine_reason::internal_failure,
									 open->close_origin_sequence,
									 open->close_cut_sequence,
									 open->close_terminal_sequence});
							}
						}
					}
					quarantine_locked();
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
				}
				// A completed recovery transition owns the durable tombstone.  On recovery-lock
				// failure the wrapper must retain its state/token so destruction can still
				// abandon the live close-cut custody; the emergency gate rejects every retry.
				if (transitioned)
					close.disarm();
				else if (owned)
					close.disable_terminal_presentation();
			}

			[[nodiscard]] reader_unmap_cut_blocker_decision
			reader_unmap_cut_blocker_decision_locked(
				const reader_attachment_group_record& group,
				const sqlite_shm_callback_execution_receipt& callback) const noexcept
			{
				std::size_t blocker_count{};
				bool same_thread{};
				for (const auto& session : reader_sessions_)
				{
					if (session.request.attachment != group.expected ||
						session.phase == reader_session_record_phase::terminal_quarantined)
						continue;
					if (session.phase != reader_session_record_phase::active_group_owner ||
						session.group_token != group.token ||
						session.generation != group.generation ||
						session.lifecycle_phase !=
							sqlite_shm_reader_session_reservation_phase::promoted_to_group_owner ||
						session.lifecycle_origin_sequence == 0U ||
						session.lifecycle_destination_sequence == 0U ||
						session.lifecycle_destination_sequence <
							session.lifecycle_origin_sequence ||
						session.pending_terminal_sequence != 0U ||
						session.pending_terminal_receipt ||
						!valid_reader_session_request(session.request) ||
						std::ranges::count_if(
							reader_custodies_,
							[&group, &session](const reader_custody_record& custody)
							{
								return custody.kind ==
									sqlite_shm_reader_custody_kind::use_session &&
									custody.state == sqlite_shm_reader_custody_state::live &&
									custody.attachment == group.expected &&
									custody.owner_token == session.token &&
									custody.origin_sequence ==
									session.lifecycle_destination_sequence &&
									custody.destination_sequence == 0U;
							}) != 1)
						return reader_unmap_cut_blocker_decision::ambiguous;
					++blocker_count;
					same_thread = same_thread ||
						session.request.execution.thread_identity == callback.thread_identity;
				}

				for (const auto& map : reader_attachment_maps_)
				{
					if (map.request.expected_attachment != group.expected ||
						map.phase == reader_phase::terminal_quarantined)
						continue;
					const auto session = find_by_token(reader_sessions_, map.session_token);
					if (map.phase != reader_phase::inflight || map.group_token != group.token ||
						map.generation != group.generation || map.admission_sequence == 0U ||
						map.terminal_sequence != 0U ||
						!valid_reader_attachment_map_request(map.request) ||
						session == reader_sessions_.end() ||
						session->request.attachment != group.expected ||
						std::ranges::count_if(reader_custodies_,
											  [&group, &map](const reader_custody_record& custody)
											  {
												  return custody.kind ==
													  sqlite_shm_reader_custody_kind::map_attempt &&
													  custody.state ==
													  sqlite_shm_reader_custody_state::live &&
													  custody.attachment == group.expected &&
													  custody.owner_token == map.token &&
													  custody.origin_sequence ==
													  map.admission_sequence &&
													  custody.destination_sequence == 0U;
											  }) != 1)
						return reader_unmap_cut_blocker_decision::ambiguous;
					++blocker_count;
					same_thread = same_thread ||
						map.request.callback.thread_identity == callback.thread_identity;
				}

				if (blocker_count == 0U)
					return reader_unmap_cut_blocker_decision::none;
				if (callback.reentrancy_depth != 0U || same_thread)
					return reader_unmap_cut_blocker_decision::same_thread_or_reentrant;
				return reader_unmap_cut_blocker_decision::other_thread;
			}

			[[nodiscard]] reader_unmap_cut_blocker_decision
			reader_live_close_cut_blocker_decision_locked(
				const reader_attachment_group_record& group,
				const sqlite_shm_callback_execution_receipt& unmap_callback,
				const sqlite_shm_callback_execution_receipt& close_callback) const noexcept
			{
				const auto unmap = reader_unmap_cut_blocker_decision_locked(group, unmap_callback);
				const auto close = reader_unmap_cut_blocker_decision_locked(group, close_callback);
				if (unmap == reader_unmap_cut_blocker_decision::ambiguous ||
					close == reader_unmap_cut_blocker_decision::ambiguous)
					return reader_unmap_cut_blocker_decision::ambiguous;
				if (unmap == reader_unmap_cut_blocker_decision::same_thread_or_reentrant ||
					close == reader_unmap_cut_blocker_decision::same_thread_or_reentrant)
					return reader_unmap_cut_blocker_decision::same_thread_or_reentrant;
				if (unmap == reader_unmap_cut_blocker_decision::other_thread ||
					close == reader_unmap_cut_blocker_decision::other_thread)
					return reader_unmap_cut_blocker_decision::other_thread;
				return reader_unmap_cut_blocker_decision::none;
			}

			[[nodiscard]] bool reader_live_close_composite_is_exact_locked(
				const reader_attachment_group_record& group) const noexcept
			{
				if (!group.registry_bound ||
					group.reservation_phase !=
						sqlite_shm_reader_attachment_reservation_phase::observed_present ||
					group.phase != reader_attachment_group_phase::unmap_cut_sealing ||
					!group.observed_identity || !group.unmap_callback ||
					group.composite_close_owner_token == 0U ||
					group.composite_close_registry_open_token == 0U ||
					group.composite_close_registry_open_token !=
						group.expected.registry_open_token() ||
					group.composite_close_cut_sequence == 0U ||
					group.composite_close_cut_sequence != group.unmap_cut_sequence)
					return false;
				const auto open = std::find_if(
					registry_reader_opens_.begin(),
					registry_reader_opens_.end(),
					[&group](const registry_reader_open_record& candidate)
					{
						return candidate.token == group.composite_close_registry_open_token &&
							candidate.close_owner_token == group.composite_close_owner_token;
					});
				return open != registry_reader_opens_.end() && open->seal &&
					open->seal->authority_valid.load(std::memory_order_acquire) &&
					reader_attachment_matches_open_epoch_binding(group.expected, open->binding) &&
					open->close_phase == sqlite_shm_reader_connection_close_phase::close_admitted &&
					open->close_route ==
					sqlite_shm_reader_close_route::close_after_confirmed_unmap &&
					open->close_cut_sequence == group.composite_close_cut_sequence &&
					open->close_callback.has_value();
			}

			[[nodiscard]] bool reader_group_custody_census_is_exact_locked(
				const reader_attachment_group_record& group,
				const bool unmap_cut_consumed,
				const bool composite_close = false,
				const bool allow_live_blockers = false) const noexcept
			{
				if (group.generation == 0U ||
					group.expected.writer_mapping_generation() != group.generation ||
					group.group_origin_sequence == 0U ||
					(composite_close !=
					 (group.composite_close_owner_token != 0U &&
					  group.composite_close_registry_open_token != 0U &&
					  group.composite_close_cut_sequence != 0U)))
					return false;
				if (std::ranges::count_if(reader_attachment_groups_,
										  [&group](const reader_attachment_group_record& candidate)
										  {
											  return candidate.expected == group.expected &&
												  !reader_reservation_is_compactable(candidate);
										  }) != 1)
					return false;
				std::array<std::size_t, sqlite_shm_reader_custody_kinds.size()> live_counts{};
				std::size_t sealed_cut_count{};
				std::size_t transferred_handoff_count{};
				std::size_t transferred_deferred_unmap_count{};
				const auto deferred_cleanup = group.existing_group_deferred_cleanup_required;
				const auto deferred_owner_origin = group.existing_group_deferred_cleanup_sequence;
				const auto deferred_after_cut = deferred_cleanup && unmap_cut_consumed &&
					group.unmap_cut_sequence != 0U &&
					deferred_owner_origin > group.unmap_cut_sequence;
				const auto unmap_owner_origin = deferred_after_cut ? group.unmap_cut_sequence
					: deferred_cleanup							   ? deferred_owner_origin
																   : group.unmap_cut_sequence;
				for (const auto& custody : reader_custodies_)
				{
					if (custody.owner_token == group.token && custody.attachment != group.expected)
						return false;
					if (custody.attachment != group.expected)
						continue;
					if (custody.state == sqlite_shm_reader_custody_state::live)
					{
						const auto exact_session_blocker =
							allow_live_blockers &&
							custody.kind == sqlite_shm_reader_custody_kind::use_session &&
							std::ranges::any_of(
								reader_sessions_,
								[&group, &custody](const reader_session_record& session)
								{
									return session.token == custody.owner_token &&
										session.phase ==
										reader_session_record_phase::active_group_owner &&
										session.group_token == group.token &&
										session.generation == group.generation &&
										session.request.attachment == group.expected &&
										custody.origin_sequence ==
										session.lifecycle_destination_sequence &&
										custody.destination_sequence == 0U;
								});
						const auto exact_map_blocker =
							allow_live_blockers &&
							custody.kind == sqlite_shm_reader_custody_kind::map_attempt &&
							std::ranges::any_of(
								reader_attachment_maps_,
								[&group, &custody](const reader_attachment_map_record& map)
								{
									return map.token == custody.owner_token &&
										map.phase == reader_phase::inflight &&
										map.group_token == group.token &&
										map.generation == group.generation &&
										map.request.expected_attachment == group.expected &&
										custody.origin_sequence == map.admission_sequence &&
										custody.destination_sequence == 0U;
								});
						if (exact_session_blocker || exact_map_blocker)
							continue;
						const auto exact_composite = composite_close &&
							custody.kind ==
								sqlite_shm_reader_custody_kind::close_cut_or_composite &&
							custody.owner_token == group.composite_close_owner_token &&
							custody.open_epoch &&
							reader_attachment_matches_open_epoch_binding(group.expected,
																		 *custody.open_epoch) &&
							custody.origin_sequence == group.composite_close_cut_sequence &&
							custody.destination_sequence == 0U;
						if (custody.owner_token != group.token && !exact_composite)
							return false;
						if ((custody.kind ==
								 sqlite_shm_reader_custody_kind::generation_group_count ||
							 custody.kind ==
								 sqlite_shm_reader_custody_kind::exact_present_attachment ||
							 custody.kind ==
								 sqlite_shm_reader_custody_kind::
									 runtime_vfs_namespace_generation_native_mapping_lifetime_pin) &&
							custody.origin_sequence == 0U)
							return false;
						if ((unmap_cut_consumed || deferred_cleanup) &&
							custody.kind ==
								sqlite_shm_reader_custody_kind::normal_or_deferred_unmap &&
							(custody.origin_sequence == 0U ||
							 custody.origin_sequence != unmap_owner_origin))
							return false;
						++live_counts.at(static_cast<std::size_t>(custody.kind));
					}
					if (custody.kind == sqlite_shm_reader_custody_kind::unmap_cut &&
						custody.owner_token == group.token &&
						custody.state ==
							sqlite_shm_reader_custody_state::consumed_with_exact_terminal_receipt &&
						custody.origin_sequence == group.unmap_cut_sequence &&
						custody.destination_sequence == group.unmap_cut_sequence &&
						group.unmap_cut_sequence != 0U)
						++sealed_cut_count;
					if (custody.kind == sqlite_shm_reader_custody_kind::attachment_group_handoff &&
						custody.owner_token == group.token &&
						custody.state ==
							sqlite_shm_reader_custody_state::transferred_to_exact_successor &&
						custody.destination_sequence == unmap_owner_origin &&
						unmap_owner_origin != 0U)
						++transferred_handoff_count;
					if (custody.kind == sqlite_shm_reader_custody_kind::normal_or_deferred_unmap &&
						custody.owner_token == group.token &&
						custody.state ==
							sqlite_shm_reader_custody_state::transferred_to_exact_successor &&
						custody.destination_sequence == group.composite_close_cut_sequence &&
						group.composite_close_cut_sequence != 0U)
						++transferred_deferred_unmap_count;
				}
				const auto count = [&live_counts](const sqlite_shm_reader_custody_kind kind)
				{
					return live_counts.at(static_cast<std::size_t>(kind));
				};
				for (const auto kind : sqlite_shm_reader_custody_kinds)
				{
					const auto expected =
						kind == sqlite_shm_reader_custody_kind::generation_group_count ||
							kind == sqlite_shm_reader_custody_kind::exact_present_attachment ||
							((unmap_cut_consumed || deferred_cleanup) && !composite_close &&
							 kind == sqlite_shm_reader_custody_kind::normal_or_deferred_unmap) ||
							(unmap_cut_consumed &&
							 (kind ==
								  sqlite_shm_reader_custody_kind::bounded_waiter_or_continuation ||
							  kind == sqlite_shm_reader_custody_kind::terminal_reporter)) ||
							(!unmap_cut_consumed && !deferred_cleanup &&
							 kind == sqlite_shm_reader_custody_kind::attachment_group_handoff) ||
							(group.registry_bound &&
							 kind ==
								 sqlite_shm_reader_custody_kind::
									 runtime_vfs_namespace_generation_native_mapping_lifetime_pin) ||
							(composite_close &&
							 kind == sqlite_shm_reader_custody_kind::close_cut_or_composite)
						? 1U
						: 0U;
					if (count(kind) != expected)
						return false;
				}
				if (unmap_cut_consumed)
					return sealed_cut_count == 1U && transferred_handoff_count == 1U &&
						transferred_deferred_unmap_count ==
						static_cast<std::size_t>(composite_close && deferred_cleanup &&
												 !deferred_after_cut);
				return sealed_cut_count == 0U &&
					transferred_handoff_count == static_cast<std::size_t>(deferred_cleanup);
			}

			void quarantine_reader_group_locked(
				reader_attachment_group_record& group,
				std::uint64_t terminal_sequence = 0U,
				const sqlite_shm_reader_terminal_quarantine_reason reason =
					sqlite_shm_reader_terminal_quarantine_reason::presented_invalid) noexcept
			{
				terminal_sequence = std::max({terminal_sequence,
											  group.unmap_terminal_sequence,
											  group.predecessor_unmap_terminal_sequence,
											  group.predecessor_close_terminal_sequence,
											  group.unpublished_cleanup_terminal_sequence,
											  group.logical_ack_sequence,
											  group.composite_close_wait_resolution_sequence});
				if (group.unmap_cut_sequence_slot != 0U || group.unmap_terminal_sequence_slot != 0U)
				{
					const auto terminal = group.unmap_cut_sequence_slot != 0U &&
							group.unmap_terminal_sequence_slot != 0U
						? consume_reader_lifecycle_terminal_slots_locked(
							  group.unmap_cut_sequence_slot, group.unmap_terminal_sequence_slot)
						: consume_reader_lifecycle_terminal_slot_locked(
							  group.unmap_cut_sequence_slot != 0U
								  ? group.unmap_cut_sequence_slot
								  : group.unmap_terminal_sequence_slot);
					if (terminal.succeeded)
					{
						group.unmap_cut_sequence_slot = 0U;
						group.unmap_terminal_sequence_slot = 0U;
						if (group.reservation_phase ==
								sqlite_shm_reader_attachment_reservation_phase::
									unpublished_cleanup_admitted ||
							group.unpublished_cleanup_receipt)
							group.unpublished_cleanup_terminal_sequence = terminal.first;
						else
							group.unmap_terminal_sequence = terminal.first;
						terminal_sequence = std::max(terminal_sequence, terminal.first);
					}
					else
						emergency_quarantine_.store(true, std::memory_order_release);
				}
				if (group.logical_ack_sequence_slot != 0U)
				{
					const auto ack = consume_reader_lifecycle_terminal_slot_locked(
						group.logical_ack_sequence_slot);
					if (ack.succeeded)
					{
						group.logical_ack_sequence_slot = 0U;
						group.logical_ack_sequence = ack.first;
						terminal_sequence = std::max(terminal_sequence, ack.first);
					}
					else
						emergency_quarantine_.store(true, std::memory_order_release);
				}
				if (group.composite_close_wait_resolution_sequence_slot != 0U)
				{
					const auto resolution = consume_reader_lifecycle_terminal_slot_locked(
						group.composite_close_wait_resolution_sequence_slot);
					if (resolution.succeeded)
					{
						group.composite_close_wait_resolution_sequence_slot = 0U;
						group.composite_close_wait_resolution_sequence = resolution.first;
						terminal_sequence = std::max(terminal_sequence, resolution.first);
					}
					else
						emergency_quarantine_.store(true, std::memory_order_release);
				}
				// A terminal group can no longer consume the speculative cut/terminal pair
				// reserved by an in-flight first map.  Keep only the map/session terminal
				// reservations needed to account for the already-started native callback.
				for (auto& map : reader_attachment_maps_)
				{
					if (map.group_token != group.token)
						continue;
					cancel_reader_lifecycle_terminal_slot_locked(
						map.potential_group_cut_sequence_slot);
					cancel_reader_lifecycle_terminal_slot_locked(
						map.potential_group_terminal_sequence_slot);
					map.potential_group_cut_sequence_slot = 0U;
					map.potential_group_terminal_sequence_slot = 0U;
				}
				if (!reader_reservation_is_compactable(group))
					group.reservation_phase =
						sqlite_shm_reader_attachment_reservation_phase::terminal_quarantined;
				group.phase = reader_attachment_group_phase::terminal_quarantined;
				if (group.quarantine_reason == sqlite_shm_reader_terminal_quarantine_reason::none)
					group.quarantine_reason = reason;
				if (terminal_sequence != 0U)
				{
					group.reservation_destination_sequence = terminal_sequence;
					group.group_destination_sequence = terminal_sequence;
					for (auto& custody : reader_custodies_)
					{
						if (custody.attachment != group.expected ||
							custody.owner_token != group.token ||
							custody.state != sqlite_shm_reader_custody_state::live)
							continue;
						custody.state =
							sqlite_shm_reader_custody_state::transferred_to_durable_tombstone;
						custody.destination_sequence = terminal_sequence;
					}
					reader_last_committed_sequence_ =
						std::max(reader_last_committed_sequence_, terminal_sequence);
				}
				quarantine_reader_composite_open_locked(group, terminal_sequence, reason);
			}

			void quarantine_reader_unmap_locked(
				reader_attachment_group_record& group,
				sqlite_shm_reader_unmap_obligation& unmap,
				const sqlite_shm_reader_terminal_quarantine_reason reason =
					sqlite_shm_reader_terminal_quarantine_reason::presented_invalid) noexcept
			{
				// Unknown post-native state becomes a durable conservative quarantine tombstone.
				quarantine_reader_group_locked(group, 0U, reason);
				if (group.composite_close_owner_token != 0U &&
					group.composite_close_registry_open_token != 0U)
				{
					const auto open = std::find_if(
						registry_reader_opens_.begin(),
						registry_reader_opens_.end(),
						[&group](const registry_reader_open_record& candidate)
						{
							return candidate.token == group.composite_close_registry_open_token &&
								candidate.close_owner_token == group.composite_close_owner_token;
						});
					if (open != registry_reader_opens_.end())
						quarantine_reader_open_locked(*open, group.unmap_terminal_sequence, reason);
				}
				unmap.disarm();
				quarantine_locked();
			}

			void quarantine_reader_unpublished_cleanup_terminal(
				sqlite_shm_reader_unpublished_cleanup_obligation& cleanup) noexcept
			{
				const auto owned = owns(cleanup.state_, cleanup.token_);
				bool transitioned{};
				try
				{
					if (fail_next_reader_recovery_mutex_reacquire_for_testing_.exchange(
							false, std::memory_order_acq_rel))
						throw reader_recovery_mutex_reacquire_injected_failure{};
					std::scoped_lock lock{mutex_};
					if (owns(cleanup.state_, cleanup.token_))
					{
						const auto group = find_by_token(reader_attachment_groups_, cleanup.token_);
						if (group != reader_attachment_groups_.end())
						{
							quarantine_reader_group_locked(
								*group,
								0U,
								sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
							transitioned = true;
						}
					}
					quarantine_locked();
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
				}
				if (transitioned)
					cleanup.disarm();
				else if (owned)
					cleanup.disable_terminal_presentation();
			}

			void
			quarantine_reader_unmap_terminal(sqlite_shm_reader_unmap_obligation& unmap) noexcept
			{
				const auto owned = owns(unmap.state_, unmap.token_);
				bool transitioned{};
				try
				{
					if (fail_next_reader_recovery_mutex_reacquire_for_testing_.exchange(
							false, std::memory_order_acq_rel))
						throw reader_recovery_mutex_reacquire_injected_failure{};
					std::scoped_lock lock{mutex_};
					if (owns(unmap.state_, unmap.token_))
					{
						const auto group = find_by_token(reader_attachment_groups_, unmap.token_);
						if (group != reader_attachment_groups_.end())
						{
							quarantine_reader_group_locked(
								*group,
								0U,
								sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
							transitioned = true;
						}
						else
						{
							const auto legacy_handoff = find_by_token(handoffs_, unmap.token_);
							if (legacy_handoff != handoffs_.end())
							{
								legacy_handoff->phase = handoff_phase::terminal_quarantined;
								transitioned = true;
							}
						}
					}
					quarantine_locked();
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
				}
				// Disarm only after custody reached a durable terminal row.  A recovery-lock
				// failure keeps the owner for destructor abandonment while the emergency gate
				// rejects a second terminal presentation.
				if (transitioned)
					unmap.disarm();
				else if (owned)
					unmap.disable_terminal_presentation();
			}

			void quarantine_reader_unmap_begin(sqlite_shm_reader_handoff& handoff) noexcept
			{
				bool transitioned{};
				try
				{
					if (fail_next_reader_recovery_mutex_reacquire_for_testing_.exchange(
							false, std::memory_order_acq_rel))
						throw reader_recovery_mutex_reacquire_injected_failure{};
					std::scoped_lock lock{mutex_};
					if (owns(handoff.state_, handoff.token_))
					{
						const auto group = find_by_token(reader_attachment_groups_, handoff.token_);
						if (group != reader_attachment_groups_.end())
						{
							quarantine_reader_group_locked(
								*group,
								0U,
								sqlite_shm_reader_terminal_quarantine_reason::internal_failure);
							transitioned = true;
						}
						else
						{
							const auto legacy_handoff = find_by_token(handoffs_, handoff.token_);
							if (legacy_handoff != handoffs_.end())
							{
								legacy_handoff->phase = handoff_phase::terminal_quarantined;
								transitioned = true;
							}
						}
					}
					quarantine_locked();
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
				}
				if (transitioned)
					handoff.disarm();
			}

			void quarantine_reader_session_locked(
				reader_session_record& session,
				const sqlite_shm_reader_terminal_quarantine_reason reason =
					sqlite_shm_reader_terminal_quarantine_reason::presented_invalid) noexcept
			{
				std::array<std::uint64_t, 2> active_slots{};
				std::size_t active_slot_count{};
				bool invalid_slot_census{};
				for (auto& map : reader_attachment_maps_)
				{
					if (map.session_token != session.token)
						continue;
					map.phase = reader_phase::terminal_quarantined;
					if (map.quarantine_reason == sqlite_shm_reader_terminal_quarantine_reason::none)
						map.quarantine_reason = reason;
					if (map.terminal_sequence_slot != 0U)
					{
						if (active_slot_count == active_slots.size())
							invalid_slot_census = true;
						else
							active_slots[active_slot_count++] = map.terminal_sequence_slot;
					}
				}
				if (session.terminal_sequence_slot != 0U)
				{
					if (active_slot_count == active_slots.size())
						invalid_slot_census = true;
					else
						active_slots[active_slot_count++] = session.terminal_sequence_slot;
				}
				const auto active_slot_span =
					std::span<const std::uint64_t>{active_slots}.first(active_slot_count);
				const auto terminals = !invalid_slot_census && active_slot_count != 0U
					? consume_reader_lifecycle_terminal_slots_locked(active_slot_span)
					: reader_lifecycle_sequence_batch{};
				if (invalid_slot_census || (active_slot_count != 0U && !terminals.succeeded))
					emergency_quarantine_.store(true, std::memory_order_release);

				std::uint64_t terminal_high_water = session.pending_terminal_sequence;
				if (!invalid_slot_census && (active_slot_count == 0U || terminals.succeeded))
				{
					for (auto& map : reader_attachment_maps_)
					{
						if (map.session_token != session.token)
							continue;
						if (map.terminal_sequence_slot != 0U)
						{
							const auto slot =
								std::ranges::find(active_slot_span, map.terminal_sequence_slot);
							if (slot == active_slot_span.end())
							{
								emergency_quarantine_.store(true, std::memory_order_release);
								continue;
							}
							map.terminal_sequence = terminals.first +
								static_cast<std::uint64_t>(std::distance(active_slot_span.begin(),
																		 slot));
							map.terminal_sequence_slot = 0U;
						}
						terminal_high_water = std::max(terminal_high_water, map.terminal_sequence);
					}
					if (session.terminal_sequence_slot != 0U)
					{
						const auto slot =
							std::ranges::find(active_slot_span, session.terminal_sequence_slot);
						if (slot != active_slot_span.end())
						{
							session.pending_terminal_sequence = terminals.first +
								static_cast<std::uint64_t>(std::distance(active_slot_span.begin(),
																		 slot));
							session.terminal_sequence_slot = 0U;
						}
						else
							emergency_quarantine_.store(true, std::memory_order_release);
					}
				}
				terminal_high_water =
					std::max(terminal_high_water, session.pending_terminal_sequence);
				session.phase = reader_session_record_phase::terminal_quarantined;
				session.lifecycle_phase =
					sqlite_shm_reader_session_reservation_phase::terminal_quarantined;
				if (session.quarantine_reason == sqlite_shm_reader_terminal_quarantine_reason::none)
					session.quarantine_reason = reason;
				session.lifecycle_destination_sequence = session.pending_terminal_sequence;

				for (auto& custody : reader_custodies_)
				{
					if (custody.state != sqlite_shm_reader_custody_state::live)
						continue;
					if ((custody.kind == sqlite_shm_reader_custody_kind::use_session_reservation ||
						 custody.kind == sqlite_shm_reader_custody_kind::use_session) &&
						custody.owner_token == session.token &&
						session.pending_terminal_sequence != 0U)
					{
						custody.state =
							sqlite_shm_reader_custody_state::transferred_to_durable_tombstone;
						custody.destination_sequence = session.pending_terminal_sequence;
						continue;
					}
					for (auto& map : reader_attachment_maps_)
					{
						if (map.session_token != session.token ||
							custody.kind != sqlite_shm_reader_custody_kind::map_attempt ||
							custody.owner_token != map.token || map.terminal_sequence == 0U)
							continue;
						custody.state =
							sqlite_shm_reader_custody_state::transferred_to_durable_tombstone;
						custody.destination_sequence = map.terminal_sequence;
						break;
					}
				}

				if (session.group_token != 0U)
				{
					const auto group =
						find_by_token(reader_attachment_groups_, session.group_token);
					if (group != reader_attachment_groups_.end())
					{
						quarantine_reader_group_locked(*group, terminal_high_water, reason);
						terminal_high_water =
							std::max(terminal_high_water, group->unmap_terminal_sequence);
					}
				}
				for (auto& map : reader_attachment_maps_)
				{
					if (map.session_token != session.token || terminal_high_water == 0U)
						continue;
					cancel_reader_lifecycle_terminal_slot_locked(
						map.potential_group_cut_sequence_slot);
					cancel_reader_lifecycle_terminal_slot_locked(
						map.potential_group_terminal_sequence_slot);
					map.potential_group_cut_sequence_slot = 0U;
					map.potential_group_terminal_sequence_slot = 0U;
				}
				reader_last_committed_sequence_ =
					std::max(reader_last_committed_sequence_, terminal_high_water);
				quarantine_locked();
			}

			void
			quarantine_reader_map_terminal_commit_locked(const std::uint64_t map_token,
														 const std::uint64_t session_token) noexcept
			{
				static_assert(
					std::is_nothrow_move_assignable_v<reader_attachment_group_audit_record>);
				std::uint64_t generation{};
				std::uint64_t group_token{};
				const sqlite_shm_reader_attachment_reservation_identity* expected_attachment{};
				auto quarantine_reason =
					sqlite_shm_reader_terminal_quarantine_reason::internal_failure;

				const auto map = find_by_token(reader_attachment_maps_, map_token);
				if (map != reader_attachment_maps_.end())
				{
					map->phase = reader_phase::terminal_quarantined;
					generation = map->generation;
					group_token = map->group_token;
					expected_attachment = &map->request.expected_attachment;
					if (map->quarantine_reason !=
						sqlite_shm_reader_terminal_quarantine_reason::none)
						quarantine_reason = map->quarantine_reason;
				}

				const auto session = find_by_token(reader_sessions_, session_token);
				if (session != reader_sessions_.end())
				{
					quarantine_reader_session_locked(*session, quarantine_reason);
					if (generation == 0U)
						generation = session->generation;
					if (session->group_token != 0U)
						group_token = session->group_token;
					if (expected_attachment == nullptr)
						expected_attachment = &session->request.attachment;
				}

				std::erase_if(
					reader_attachment_zero_effect_terminals_,
					[map_token](const reader_attachment_zero_effect_terminal_record& terminal)
					{
						return terminal.token == map_token;
					});
				for (auto& group : reader_attachment_groups_)
				{
					const auto has_attempt_audit = std::ranges::any_of(
						group.audits,
						[map_token](const reader_attachment_group_audit_record& audit)
						{
							return audit.map_attempt_token == map_token;
						});
					const auto matches_group_token =
						group_token != 0U && group.token == group_token;
					const auto matches_expected_attachment = expected_attachment != nullptr &&
						generation != 0U && group.generation == generation &&
						group.expected == *expected_attachment;
					if (!has_attempt_audit && !matches_group_token && !matches_expected_attachment)
						continue;

					quarantine_reader_group_locked(group);
					std::erase_if(group.audits,
								  [map_token](const reader_attachment_group_audit_record& audit)
								  {
									  return audit.map_attempt_token == map_token;
								  });
				}
				quarantine_locked();
			}

			void quarantine_reader_map_attempt_only_locked(
				reader_attachment_map_record& map,
				const sqlite_shm_reader_terminal_quarantine_reason reason) noexcept
			{
				map.phase = reader_phase::terminal_quarantined;
				if (map.quarantine_reason == sqlite_shm_reader_terminal_quarantine_reason::none)
					map.quarantine_reason = reason;
				if (map.potential_group_cut_sequence_slot != 0U ||
					map.potential_group_terminal_sequence_slot != 0U)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
					return;
				}
				if (map.terminal_sequence_slot != 0U)
				{
					const auto terminal =
						consume_reader_lifecycle_terminal_slot_locked(map.terminal_sequence_slot);
					if (!terminal.succeeded)
					{
						emergency_quarantine_.store(true, std::memory_order_release);
						return;
					}
					map.terminal_sequence_slot = 0U;
					map.terminal_sequence = terminal.first;
				}

				std::size_t transitioned{};
				for (auto& custody : reader_custodies_)
				{
					if (custody.kind != sqlite_shm_reader_custody_kind::map_attempt ||
						custody.owner_token != map.token ||
						custody.state != sqlite_shm_reader_custody_state::live)
						continue;
					custody.state =
						sqlite_shm_reader_custody_state::transferred_to_durable_tombstone;
					custody.destination_sequence = map.terminal_sequence;
					++transitioned;
				}
				if (map.terminal_sequence == 0U || transitioned != 1U)
					emergency_quarantine_.store(true, std::memory_order_release);
				reader_last_committed_sequence_ =
					std::max(reader_last_committed_sequence_, map.terminal_sequence);
				quarantine_locked();
			}

			void quarantine_reader_map_terminal_commit(
				sqlite_shm_reader_attachment_map_inflight& inflight,
				sqlite_shm_reader_session& session) noexcept
			{
				const auto map_token = inflight.token_;
				const auto session_token = session.token_;
				const auto inflight_owned = owns(inflight.state_, map_token);
				const auto session_owned = owns(session.state_, session_token);
				bool transitioned{};
				try
				{
					if (fail_next_reader_recovery_mutex_reacquire_for_testing_.exchange(
							false, std::memory_order_acq_rel))
						throw reader_recovery_mutex_reacquire_injected_failure{};
					std::scoped_lock lock{mutex_};
					if (owns(inflight.state_, map_token) && owns(session.state_, session_token))
					{
						quarantine_reader_map_terminal_commit_locked(map_token, session_token);
						transitioned = true;
					}
					quarantine_locked();
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
				}
				if (transitioned)
				{
					inflight.disarm();
					session.disarm();
				}
				else
				{
					if (inflight_owned)
						inflight.disable_terminal_presentation();
					if (session_owned)
						session.disable_terminal_presentation();
				}
			}

			void quarantine_reader_session_terminal_commit(sqlite_shm_reader_session& session,
														   std::uint64_t exact_group_token) noexcept
			{
				static_assert(std::is_nothrow_move_assignable_v<reader_session_terminal_record>);
				const auto session_token = session.token_;
				const auto owned = owns(session.state_, session_token);
				bool transitioned{};
				try
				{
					if (fail_next_reader_recovery_mutex_reacquire_for_testing_.exchange(
							false, std::memory_order_acq_rel))
						throw reader_recovery_mutex_reacquire_injected_failure{};
					std::scoped_lock lock{mutex_};
					if (owns(session.state_, session_token))
					{
						const auto owner = find_by_token(reader_sessions_, session_token);
						if (owner != reader_sessions_.end())
						{
							if (exact_group_token == 0U)
								exact_group_token = owner->group_token;
							const auto reason = owner->quarantine_reason !=
									sqlite_shm_reader_terminal_quarantine_reason::none
								? owner->quarantine_reason
								: sqlite_shm_reader_terminal_quarantine_reason::internal_failure;
							quarantine_reader_session_locked(*owner, reason);
							transitioned = true;
						}
						if (exact_group_token != 0U)
						{
							const auto group =
								find_by_token(reader_attachment_groups_, exact_group_token);
							if (group != reader_attachment_groups_.end())
							{
								const auto reason = owner != reader_sessions_.end() &&
										owner->quarantine_reason !=
											sqlite_shm_reader_terminal_quarantine_reason::none
									? owner->quarantine_reason
									: sqlite_shm_reader_terminal_quarantine_reason::
										  internal_failure;
								quarantine_reader_group_locked(*group, 0U, reason);
								transitioned = true;
							}
						}
						std::erase_if(
							reader_session_terminals_,
							[session_token](const reader_session_terminal_record& terminal)
							{
								return terminal.session_token == session_token;
							});
					}
					quarantine_locked();
				}
				catch (...)
				{
					emergency_quarantine_.store(true, std::memory_order_release);
				}
				if (transitioned)
					session.disarm();
				else if (owned)
					session.disable_terminal_presentation();
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

			[[nodiscard]] bool reader_session_terminal_identity_seen_locked(
				const sqlite_backend_opaque_identity& identity) const noexcept
			{
				const auto replay_contains =
					[&identity](const sqlite_shm_reader_replay_identity_tombstone& replay)
				{
					return std::ranges::find(replay.session_terminal_receipts, identity) !=
						replay.session_terminal_receipts.end();
				};
				return std::ranges::any_of(
						   reader_sessions_,
						   [&identity](const reader_session_record& session)
						   {
							   return session.pending_terminal_receipt &&
								   session.pending_terminal_receipt->terminal_receipt() == identity;
						   }) ||
					std::ranges::any_of(reader_session_terminals_,
										[&identity](const reader_session_terminal_record& terminal)
										{
											return terminal.receipt.terminal_receipt() == identity;
										}) ||
					std::ranges::any_of(
						   reader_attachment_groups_,
						   [&identity,
							&replay_contains](const reader_attachment_group_record& group)
						   {
							   return replay_contains(group.compact_replay_identities) ||
								   (group.unpublished_cleanup_receipt &&
									group.unpublished_cleanup_receipt
											->session_no_pointer_terminal_receipt() == identity);
						   });
			}

			// Local closed-world defense-in-depth; the production issuer remains responsible for
			// process-global cross-family/cross-kind nonreuse.
			[[nodiscard]] bool reader_effect_identity_seen_locked(
				const sqlite_backend_opaque_identity& identity,
				const std::uint64_t except_map_token,
				const std::uint64_t except_group_token,
				const std::uint64_t except_close_owner_token) const noexcept
			{
				if (std::ranges::any_of(writers_,
										[&identity](const writer_record& writer)
										{
											return writer.receipt &&
												writer.receipt->holder_specific_effect_receipt() ==
												identity;
										}) ||
					std::ranges::any_of(
						holders_,
						[&identity](const holder_record& holder)
						{
							return holder.map_receipt.holder_specific_effect_receipt() == identity;
						}) ||
					std::ranges::any_of(
						writer_attachments_,
						[&identity](const writer_attachment_record& attachment)
						{
							return std::ranges::any_of(
								attachment.sealed_member_audit,
								[&identity](const writer_attachment_member_audit_record& audit)
								{
									return audit.post_map_receipt &&
										audit.post_map_receipt->holder_specific_effect_receipt() ==
										identity;
								});
						}) ||
					std::ranges::any_of(readers_,
										[&identity](const reader_record& reader)
										{
											return reader.receipt &&
												reader.receipt->zero_resize_effect_receipt() ==
												identity;
										}) ||
					std::ranges::any_of(
						handoffs_,
						[&identity](const handoff_record& handoff)
						{
							return handoff.post_map_receipt.zero_resize_effect_receipt() ==
								identity;
						}) ||
					std::ranges::any_of(
						reader_sessions_,
						[&identity](const reader_session_record& session)
						{
							return session.pending_terminal_receipt &&
								session.pending_terminal_receipt->terminal_receipt() == identity;
						}) ||
					std::ranges::any_of(reader_session_terminals_,
										[&identity](const reader_session_terminal_record& terminal)
										{
											return terminal.receipt.terminal_receipt() == identity;
										}))
					return true;
				if (std::ranges::any_of(
						reader_attachment_maps_,
						[&identity, except_map_token](const reader_attachment_map_record& map)
						{
							return map.token != except_map_token &&
								((map.receipt &&
								  map.receipt->zero_resize_effect_receipt() == identity) ||
								 (map.zero_effect_receipt &&
								  map.zero_effect_receipt->zero_attachment_effect_receipt() ==
									  identity));
						}))
					return true;
				if (std::ranges::any_of(
						reader_attachment_groups_,
						[&identity](const reader_attachment_group_record& group)
						{
							return std::ranges::find(
									   group.compact_replay_identities.effect_receipts, identity) !=
								group.compact_replay_identities.effect_receipts.end() ||
								std::ranges::find(
									group.compact_replay_identities.session_terminal_receipts,
									identity) !=
								group.compact_replay_identities.session_terminal_receipts.end() ||
								(group.unpublished_cleanup_receipt &&
								 (group.unpublished_cleanup_receipt->mapped_effect_receipt() ==
									  identity ||
								  group.unpublished_cleanup_receipt
										  ->session_no_pointer_terminal_receipt() == identity)) ||
								std::ranges::any_of(
									   group.audits,
									   [&identity](
										   const reader_attachment_group_audit_record& audit)
									   {
										   return audit.receipt.zero_resize_effect_receipt() ==
											   identity;
									   });
						}))
					return true;
				if (std::ranges::any_of(
						reader_attachment_zero_effect_terminals_,
						[&identity](const reader_attachment_zero_effect_terminal_record& terminal)
						{
							return terminal.receipt.zero_attachment_effect_receipt() == identity;
						}) ||
					std::ranges::any_of(
						reader_predecessor_map_terminals_,
						[&identity](const reader_predecessor_map_terminal_record& terminal)
						{
							return terminal.receipt.native_effect_receipt() == identity;
						}) ||
					std::ranges::any_of(
						reader_existing_group_predecessor_mismatch_terminals_,
						[&identity](
							const reader_existing_group_predecessor_mismatch_terminal_record&
								terminal)
						{
							return terminal.receipt.native_effect_receipt() == identity;
						}) ||
					std::ranges::any_of(
						reader_attachment_groups_,
						[&identity, except_group_token](const reader_attachment_group_record& group)
						{
							if (group.token == except_group_token)
								return false;
							const auto unmap_seen = group.unmap_terminal_receipt &&
								((group.unmap_terminal_receipt->native_effect_receipt() &&
								  *group.unmap_terminal_receipt->native_effect_receipt() ==
									  identity) ||
								 (group.unmap_terminal_receipt->latch_reset_receipt() &&
								  *group.unmap_terminal_receipt->latch_reset_receipt() ==
									  identity));
							const auto cleanup_seen = group.unpublished_cleanup_terminal_receipt &&
								((group.unpublished_cleanup_terminal_receipt
									  ->native_effect_receipt() &&
								  *group.unpublished_cleanup_terminal_receipt
										  ->native_effect_receipt() == identity) ||
								 (group.unpublished_cleanup_terminal_receipt
									  ->latch_reset_receipt() &&
								  *group.unpublished_cleanup_terminal_receipt
										  ->latch_reset_receipt() == identity));
							const auto predecessor_seen =
								group.predecessor_unmap_terminal_receipt &&
								group.predecessor_unmap_terminal_receipt->native_effect_receipt() &&
								*group.predecessor_unmap_terminal_receipt
										->native_effect_receipt() == identity;
							return unmap_seen || cleanup_seen || predecessor_seen;
						}))
					return true;
				return std::ranges::any_of(
						   registry_reader_opens_,
						   [&identity,
							except_close_owner_token](const registry_reader_open_record& open)
						   {
							   return open.close_owner_token != except_close_owner_token &&
								   open.close_terminal_receipt &&
								   open.close_terminal_receipt->native_effect_receipt() &&
								   *open.close_terminal_receipt->native_effect_receipt() ==
								   identity;
						   }) ||
					std::ranges::any_of(
						   reader_close_terminals_,
						   [&identity,
							except_close_owner_token](const reader_close_terminal_record& terminal)
						   {
							   return terminal.close_owner_token != except_close_owner_token &&
								   terminal.receipt.native_effect_receipt() &&
								   *terminal.receipt.native_effect_receipt() == identity;
						   }) ||
					std::ranges::any_of(
						   reader_open_close_tombstones_,
						   [&identity](
							   const sqlite_shm_reader_open_epoch_close_tombstone& tombstone)
						   {
							   return std::ranges::find(tombstone.replay_identities.effect_receipts,
														identity) !=
								   tombstone.replay_identities.effect_receipts.end();
						   });
			}

			[[nodiscard]] bool reader_map_effect_identity_seen_locked(
				const sqlite_backend_opaque_identity& identity,
				const std::uint64_t except_map_token) const noexcept
			{
				return reader_effect_identity_seen_locked(identity, except_map_token, 0U, 0U);
			}

			[[nodiscard]] bool reader_unmap_evidence_identity_seen_locked(
				const sqlite_backend_opaque_identity& identity,
				const std::uint64_t except_group_token) const noexcept
			{
				return reader_effect_identity_seen_locked(identity, 0U, except_group_token, 0U);
			}

			[[nodiscard]] bool reader_close_effect_identity_seen_locked(
				const sqlite_backend_opaque_identity& identity,
				const std::uint64_t except_close_owner_token) const noexcept
			{
				return reader_effect_identity_seen_locked(
					identity, 0U, 0U, except_close_owner_token);
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

			[[nodiscard]] bool valid_reader_attachment_receipt(
				const sqlite_shm_verified_reader_attachment_post_map_receipt& receipt)
				const noexcept
			{
				return valid_reader_attachment_map_request(receipt.request()) &&
					receipt.generation() != 0U && valid_mapping(receipt.mapping()) &&
					valid_observed_reader_native_attachment(receipt.observed_attachment()) &&
					receipt.observed_attachment().expected() ==
					receipt.request().expected_attachment &&
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

			[[nodiscard]] bool has_live_registry_reader_lineage_locked() const noexcept
			{
				return std::ranges::any_of(
						   reader_sessions_,
						   [](const reader_session_record& session)
						   {
							   return session.registry_bound &&
								   session.phase !=
								   reader_session_record_phase::terminal_quarantined;
						   }) ||
					std::ranges::any_of(reader_attachment_groups_,
										[](const reader_attachment_group_record& group)
										{
											return group.registry_bound &&
												!reader_reservation_is_compactable(group);
										}) ||
					std::ranges::any_of(reader_attachment_maps_,
										[](const reader_attachment_map_record& map)
										{
											return map.registry_bound;
										});
			}

			[[nodiscard]] bool has_live_legacy_reader_lineage_locked() const noexcept
			{
				return !readers_.empty() || !handoffs_.empty() ||
					std::ranges::any_of(reader_sessions_,
										[](const reader_session_record& session)
										{
											return !session.registry_bound &&
												session.phase !=
												reader_session_record_phase::terminal_quarantined;
										}) ||
					std::ranges::any_of(reader_attachment_groups_,
										[](const reader_attachment_group_record& group)
										{
											return !group.registry_bound &&
												!reader_reservation_is_compactable(group);
										}) ||
					std::ranges::any_of(reader_attachment_maps_,
										[](const reader_attachment_map_record& map)
										{
											return !map.registry_bound;
										});
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
						if (handoff != handoffs_.end())
							return handoff->phase == handoff_phase::active;
						const auto group = find_by_token(reader_attachment_groups_, token);
						return group != reader_attachment_groups_.end() &&
							group->reservation_phase ==
							sqlite_shm_reader_attachment_reservation_phase::observed_present &&
							group->observed_identity &&
							group->phase == reader_attachment_group_phase::active;
					}
					case lease_token_kind::reader_unmap:
					{
						const auto handoff = find_by_token(handoffs_, token);
						if (handoff != handoffs_.end())
							return handoff->phase == handoff_phase::native_cleanup_admitted;
						const auto group = find_by_token(reader_attachment_groups_, token);
						return group != reader_attachment_groups_.end() &&
							group->reservation_phase ==
							sqlite_shm_reader_attachment_reservation_phase::observed_present &&
							group->observed_identity &&
							(group->phase == reader_attachment_group_phase::unmap_cut_sealing ||
							 group->phase ==
								 reader_attachment_group_phase::native_cleanup_admitted);
					}
					case lease_token_kind::reader_unpublished_cleanup:
					{
						const auto group = find_by_token(reader_attachment_groups_, token);
						return group != reader_attachment_groups_.end() &&
							group->reservation_phase ==
							sqlite_shm_reader_attachment_reservation_phase::
								unpublished_cleanup_admitted &&
							group->phase == reader_attachment_group_phase::native_cleanup_admitted;
					}
					case lease_token_kind::reader_close:
					{
						const auto open =
							std::find_if(registry_reader_opens_.begin(),
										 registry_reader_opens_.end(),
										 [token](const registry_reader_open_record& candidate)
										 {
											 return candidate.close_owner_token == token;
										 });
						return open != registry_reader_opens_.end() &&
							open->close_phase ==
							sqlite_shm_reader_connection_close_phase::close_admitted;
					}
					case lease_token_kind::reader_session:
					{
						const auto session = find_by_token(reader_sessions_, token);
						return session != reader_sessions_.end() &&
							session->phase != reader_session_record_phase::terminal_quarantined;
					}
					case lease_token_kind::reader_attachment_map_inflight:
					{
						const auto map = find_by_token(reader_attachment_maps_, token);
						return map != reader_attachment_maps_.end() &&
							map->phase == reader_phase::inflight;
					}
				}
				return true;
			}

			mutable std::mutex mutex_;
			sqlite_shm_lease_family_binding family_;
			std::shared_ptr<sqlite_shm_mapping_generation_source> generations_;
			std::shared_ptr<sqlite_shm_reader_lifecycle_sequence_source>
				reader_lifecycle_sequences_;
			std::vector<eligibility_record> eligibilities_;
			// Node stability prevents an unrelated erase from move-assigning over a live
			// registry-bound authority while the lease mutex is held.
			std::list<writer_record> writers_;
			std::list<holder_record> holders_;
			std::vector<writer_attachment_record> writer_attachments_;
			std::vector<reader_record> readers_;
			std::vector<handoff_record> handoffs_;
			std::list<registry_reader_open_record> registry_reader_opens_;
			std::list<reader_session_record> reader_sessions_;
			std::list<reader_attachment_group_record> reader_attachment_groups_;
			std::list<reader_attachment_map_record> reader_attachment_maps_;
			std::list<reader_session_terminal_record> reader_session_terminals_;
			std::list<reader_attachment_zero_effect_terminal_record>
				reader_attachment_zero_effect_terminals_;
			std::list<reader_opaque_attachment_uncertainty_record>
				reader_opaque_attachment_uncertainties_;
			std::list<reader_predecessor_map_terminal_record> reader_predecessor_map_terminals_;
			std::list<reader_existing_group_predecessor_mismatch_terminal_record>
				reader_existing_group_predecessor_mismatch_terminals_;
			std::list<reader_close_terminal_record> reader_close_terminals_;
			std::vector<sqlite_shm_reader_open_epoch_close_tombstone> reader_open_close_tombstones_;
			std::vector<reader_custody_record> reader_custodies_;
			std::optional<generation_record> generation_;
			std::uint64_t reader_last_committed_sequence_{};
			std::uint64_t next_token_{1U};
			bool token_exhausted_{};
			bool alive_{true};
			bool quarantined_{};
			bool fail_next_writer_native_transition_for_testing_{};
			bool fail_next_writer_attachment_seal_for_testing_{};
			bool fail_next_writer_completion_transition_for_testing_{};
			bool fail_next_reader_map_terminal_commit_for_testing_{};
			bool fail_next_reader_session_terminal_commit_for_testing_{};
			bool fail_next_reader_unmap_terminal_commit_for_testing_{};
			bool fail_next_reader_unmap_post_receipt_state_for_testing_{};
			bool fail_next_reader_unpublished_cleanup_terminal_commit_for_testing_{};
			bool fail_next_reader_unmap_begin_preparation_for_testing_{};
			bool fail_next_reader_coarse_unmap_terminal_for_testing_{};
			bool fail_next_reader_close_terminal_commit_for_testing_{};
			bool fail_next_reader_close_post_receipt_state_for_testing_{};
			bool fail_next_reader_close_begin_preparation_for_testing_{};
			std::atomic_bool fail_next_reader_operation_mutex_acquire_for_testing_{false};
			std::atomic_bool fail_next_reader_recovery_mutex_reacquire_for_testing_{false};
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

	sqlite_shm_reader_attachment_map_inflight::sqlite_shm_reader_attachment_map_inflight(
		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
		const detail::sqlite_shm_lease_token_identity token,
		const detail::sqlite_shm_mapping_generation_identity generation) noexcept
		: state_{std::move(state)}, token_{token.value}, generation_{generation.value}
	{
	}

	sqlite_shm_reader_attachment_map_inflight::~sqlite_shm_reader_attachment_map_inflight() noexcept
	{
		if (state_)
			state_->abandon(lease_token_kind::reader_attachment_map_inflight, token_);
	}

	sqlite_shm_reader_attachment_map_inflight::sqlite_shm_reader_attachment_map_inflight(
		sqlite_shm_reader_attachment_map_inflight&& other) noexcept
		: state_{std::move(other.state_)}, token_{std::exchange(other.token_, 0U)},
		  generation_{std::exchange(other.generation_, 0U)},
		  terminal_presentation_disabled_{
			  std::exchange(other.terminal_presentation_disabled_, false)}
	{
	}

	bool sqlite_shm_reader_attachment_map_inflight::valid() const noexcept
	{
		return !terminal_presentation_disabled_ && state_ != nullptr &&
			state_->reader_owner_handle_is_live(
				lease_token_kind::reader_attachment_map_inflight, token_, generation_);
	}

	std::uint64_t sqlite_shm_reader_attachment_map_inflight::generation() const noexcept
	{
		return generation_;
	}

	void sqlite_shm_reader_attachment_map_inflight::disable_terminal_presentation() noexcept
	{
		terminal_presentation_disabled_ = true;
	}

	void sqlite_shm_reader_attachment_map_inflight::disarm() noexcept
	{
		token_ = 0U;
		generation_ = 0U;
		terminal_presentation_disabled_ = false;
		state_.reset();
	}

	sqlite_shm_reader_session::sqlite_shm_reader_session(
		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
		const detail::sqlite_shm_lease_token_identity token,
		const detail::sqlite_shm_mapping_generation_identity generation,
		const sqlite_shm_reader_session_phase phase) noexcept
		: state_{std::move(state)}, token_{token.value}, generation_{generation.value},
		  phase_{phase}
	{
	}

	sqlite_shm_reader_session::~sqlite_shm_reader_session() noexcept
	{
		if (state_)
			state_->abandon(lease_token_kind::reader_session, token_);
	}

	sqlite_shm_reader_session::sqlite_shm_reader_session(sqlite_shm_reader_session&& other) noexcept
		: state_{std::move(other.state_)}, token_{std::exchange(other.token_, 0U)},
		  generation_{std::exchange(other.generation_, 0U)},
		  phase_{
			  std::exchange(other.phase_, sqlite_shm_reader_session_phase::reserved_for_first_map)},
		  terminal_presentation_disabled_{
			  std::exchange(other.terminal_presentation_disabled_, false)}
	{
	}

	bool sqlite_shm_reader_session::valid() const noexcept
	{
		return !terminal_presentation_disabled_ && state_ != nullptr &&
			state_->reader_owner_handle_is_live(
				lease_token_kind::reader_session, token_, generation_);
	}

	std::uint64_t sqlite_shm_reader_session::generation() const noexcept
	{
		return generation_;
	}

	sqlite_shm_reader_session_phase sqlite_shm_reader_session::phase() const noexcept
	{
		return phase_;
	}

	void sqlite_shm_reader_session::disable_terminal_presentation() noexcept
	{
		terminal_presentation_disabled_ = true;
	}

	void sqlite_shm_reader_session::promote_to_active() noexcept
	{
		phase_ = sqlite_shm_reader_session_phase::active_group_owner;
	}

	void sqlite_shm_reader_session::disarm() noexcept
	{
		token_ = 0U;
		generation_ = 0U;
		phase_ = sqlite_shm_reader_session_phase::reserved_for_first_map;
		terminal_presentation_disabled_ = false;
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

	sqlite_shm_reader_unpublished_cleanup_obligation::
		sqlite_shm_reader_unpublished_cleanup_obligation(
			std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
			const detail::sqlite_shm_lease_token_identity token,
			const detail::sqlite_shm_mapping_generation_identity generation) noexcept
		: state_{std::move(state)}, token_{token.value}, generation_{generation.value}
	{
	}

	sqlite_shm_reader_unpublished_cleanup_obligation::
		~sqlite_shm_reader_unpublished_cleanup_obligation() noexcept
	{
		if (state_)
			state_->abandon(lease_token_kind::reader_unpublished_cleanup, token_);
	}

	sqlite_shm_reader_unpublished_cleanup_obligation::
		sqlite_shm_reader_unpublished_cleanup_obligation(
			sqlite_shm_reader_unpublished_cleanup_obligation&& other) noexcept
		: state_{std::move(other.state_)}, token_{std::exchange(other.token_, 0U)},
		  generation_{std::exchange(other.generation_, 0U)},
		  terminal_presentation_disabled_{
			  std::exchange(other.terminal_presentation_disabled_, false)}
	{
	}

	bool sqlite_shm_reader_unpublished_cleanup_obligation::valid() const noexcept
	{
		return !terminal_presentation_disabled_ && state_ != nullptr &&
			state_->reader_owner_handle_is_live(
				lease_token_kind::reader_unpublished_cleanup, token_, generation_);
	}

	std::uint64_t sqlite_shm_reader_unpublished_cleanup_obligation::generation() const noexcept
	{
		return generation_;
	}

	void sqlite_shm_reader_unpublished_cleanup_obligation::disable_terminal_presentation() noexcept
	{
		terminal_presentation_disabled_ = true;
	}

	void sqlite_shm_reader_unpublished_cleanup_obligation::disarm() noexcept
	{
		token_ = 0U;
		generation_ = 0U;
		terminal_presentation_disabled_ = false;
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
		return state_ != nullptr &&
			state_->reader_owner_handle_is_live(lease_token_kind::handoff, token_, generation_);
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

	sqlite_shm_reader_cached_member_identity::sqlite_shm_reader_cached_member_identity(
		sqlite_shm_reader_attachment_reservation_identity attachment,
		const detail::sqlite_shm_lease_token_identity group_token,
		const detail::sqlite_shm_mapping_generation_identity generation,
		const detail::sqlite_shm_lease_token_identity member_token,
		sqlite_shm_mapping_tuple mapping) noexcept
		: attachment_{std::move(attachment)}, group_token_{group_token.value},
		  generation_{generation.value}, member_token_{member_token.value}, mapping_{mapping}
	{
	}

	const sqlite_shm_mapping_tuple&
	sqlite_shm_reader_cached_member_identity::mapping() const noexcept
	{
		return mapping_;
	}

	sqlite_shm_reader_map_commit::sqlite_shm_reader_map_commit(
		const sqlite_shm_reader_map_commit_kind kind,
		sqlite_shm_mapping_tuple mapping,
		sqlite_shm_reader_cached_member_identity cached_member,
		std::optional<sqlite_shm_reader_handoff> handoff) noexcept
		: kind_{kind}, mapping_{mapping}, cached_member_{std::move(cached_member)},
		  handoff_{std::move(handoff)}
	{
	}

	sqlite_shm_reader_map_commit::sqlite_shm_reader_map_commit(
		sqlite_shm_reader_map_commit&& other) noexcept
		: kind_{other.kind_}, mapping_{other.mapping_},
		  cached_member_{std::move(other.cached_member_)}
	{
		if (other.handoff_)
		{
			handoff_.emplace(std::move(*other.handoff_));
			other.handoff_.reset();
		}
	}

	sqlite_shm_reader_map_commit_kind sqlite_shm_reader_map_commit::kind() const noexcept
	{
		return kind_;
	}

	const sqlite_shm_mapping_tuple& sqlite_shm_reader_map_commit::mapping() const noexcept
	{
		return mapping_;
	}

	const sqlite_shm_reader_cached_member_identity&
	sqlite_shm_reader_map_commit::cached_member() const noexcept
	{
		return cached_member_;
	}

	bool sqlite_shm_reader_map_commit::formed_group() const noexcept
	{
		return kind_ == sqlite_shm_reader_map_commit_kind::first_member;
	}

	std::optional<sqlite_shm_reader_handoff> sqlite_shm_reader_map_commit::take_handoff() noexcept
	{
		if (!handoff_)
			return std::nullopt;
		std::optional<sqlite_shm_reader_handoff> output{std::move(*handoff_)};
		handoff_.reset();
		return output;
	}

	sqlite_shm_reader_unmap_obligation::sqlite_shm_reader_unmap_obligation(
		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
		const detail::sqlite_shm_lease_token_identity token,
		const detail::sqlite_shm_mapping_generation_identity generation,
		const bool composite_close) noexcept
		: state_{std::move(state)}, token_{token.value}, generation_{generation.value},
		  composite_close_{composite_close}
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
		  generation_{std::exchange(other.generation_, 0U)},
		  composite_close_{std::exchange(other.composite_close_, false)},
		  terminal_presentation_disabled_{
			  std::exchange(other.terminal_presentation_disabled_, false)}
	{
	}

	bool sqlite_shm_reader_unmap_obligation::valid() const noexcept
	{
		return !terminal_presentation_disabled_ && state_ != nullptr &&
			state_->reader_owner_handle_is_live(
				lease_token_kind::reader_unmap, token_, generation_);
	}

	bool sqlite_shm_reader_unmap_obligation::native_effect_ready() const noexcept
	{
		return !terminal_presentation_disabled_ && state_ != nullptr &&
			state_->reader_unmap_native_effect_is_ready(token_, generation_);
	}

	std::uint64_t sqlite_shm_reader_unmap_obligation::generation() const noexcept
	{
		return generation_;
	}

	void sqlite_shm_reader_unmap_obligation::disable_terminal_presentation() noexcept
	{
		terminal_presentation_disabled_ = true;
	}

	void sqlite_shm_reader_unmap_obligation::disarm() noexcept
	{
		token_ = 0U;
		generation_ = 0U;
		composite_close_ = false;
		terminal_presentation_disabled_ = false;
		state_.reset();
	}

	sqlite_shm_reader_close_obligation::sqlite_shm_reader_close_obligation(
		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
		const detail::sqlite_shm_lease_token_identity owner_token,
		const std::uint64_t registry_open_token,
		std::optional<sqlite_shm_reader_close_route> route,
		const phase initial_phase,
		const std::uint64_t reservation_token,
		const std::uint64_t generation,
		const std::uint64_t wait_resolution_sequence_slot) noexcept
		: state_{std::move(state)}, owner_token_{owner_token.value},
		  registry_open_token_{registry_open_token}, route_{std::move(route)},
		  phase_{initial_phase}, reservation_token_{reservation_token}, generation_{generation},
		  wait_resolution_sequence_slot_{wait_resolution_sequence_slot}
	{
	}

	sqlite_shm_reader_close_obligation::~sqlite_shm_reader_close_obligation() noexcept
	{
		if (state_)
			state_->abandon(lease_token_kind::reader_close, owner_token_);
	}

	sqlite_shm_reader_close_obligation::sqlite_shm_reader_close_obligation(
		sqlite_shm_reader_close_obligation&& other) noexcept
		: state_{std::move(other.state_)}, owner_token_{std::exchange(other.owner_token_, 0U)},
		  registry_open_token_{std::exchange(other.registry_open_token_, 0U)},
		  route_{std::move(other.route_)}, phase_{other.phase_},
		  reservation_token_{std::exchange(other.reservation_token_, 0U)},
		  generation_{std::exchange(other.generation_, 0U)},
		  wait_resolution_sequence_slot_{std::exchange(other.wait_resolution_sequence_slot_, 0U)},
		  terminal_presentation_disabled_{
			  std::exchange(other.terminal_presentation_disabled_, false)}
	{
	}

	bool sqlite_shm_reader_close_obligation::valid() const noexcept
	{
		return !terminal_presentation_disabled_ && state_ != nullptr &&
			state_->reader_close_owner_handle_is_live(owner_token_, registry_open_token_);
	}

	bool sqlite_shm_reader_close_obligation::native_effect_ready() const noexcept
	{
		return valid() && phase_ == phase::close_ready;
	}

	std::optional<sqlite_shm_reader_close_route>
	sqlite_shm_reader_close_obligation::route() const noexcept
	{
		return route_;
	}

	void sqlite_shm_reader_close_obligation::disable_terminal_presentation() noexcept
	{
		terminal_presentation_disabled_ = true;
	}

	void sqlite_shm_reader_close_obligation::disarm() noexcept
	{
		owner_token_ = 0U;
		registry_open_token_ = 0U;
		route_.reset();
		phase_ = phase::close_ready;
		reservation_token_ = 0U;
		generation_ = 0U;
		wait_resolution_sequence_slot_ = 0U;
		terminal_presentation_disabled_ = false;
		state_.reset();
	}

	sqlite_shm_reader_live_close_obligation::sqlite_shm_reader_live_close_obligation(
		std::shared_ptr<detail::sqlite_shm_mapping_lease_state> state,
		const detail::sqlite_shm_lease_token_identity group_token,
		const detail::sqlite_shm_mapping_generation_identity generation,
		const detail::sqlite_shm_lease_token_identity close_owner_token,
		const std::uint64_t registry_open_token,
		const phase initial_phase) noexcept
		: state_{std::move(state)}, group_token_{group_token.value}, generation_{generation.value},
		  close_owner_token_{close_owner_token.value}, registry_open_token_{registry_open_token},
		  phase_{initial_phase}
	{
	}

	sqlite_shm_reader_live_close_obligation::~sqlite_shm_reader_live_close_obligation() noexcept
	{
		if (state_)
			state_->abandon_reader_live_close(*this);
	}

	sqlite_shm_reader_live_close_obligation::sqlite_shm_reader_live_close_obligation(
		sqlite_shm_reader_live_close_obligation&& other) noexcept
		: state_{std::move(other.state_)}, group_token_{std::exchange(other.group_token_, 0U)},
		  generation_{std::exchange(other.generation_, 0U)},
		  close_owner_token_{std::exchange(other.close_owner_token_, 0U)},
		  registry_open_token_{std::exchange(other.registry_open_token_, 0U)}, phase_{other.phase_},
		  terminal_presentation_disabled_{
			  std::exchange(other.terminal_presentation_disabled_, false)}
	{
	}

	bool sqlite_shm_reader_live_close_obligation::valid() const noexcept
	{
		return !terminal_presentation_disabled_ && state_ != nullptr &&
			state_->reader_live_close_owner_handle_is_live(
				group_token_, generation_, close_owner_token_, registry_open_token_, phase_);
	}

	bool sqlite_shm_reader_live_close_obligation::close_ready() const noexcept
	{
		return valid() && phase_ == phase::close_ready;
	}

	bool sqlite_shm_reader_live_close_obligation::native_effect_ready() const noexcept
	{
		return valid() && phase_ == phase::unmap_admitted;
	}

	std::uint64_t sqlite_shm_reader_live_close_obligation::generation() const noexcept
	{
		return generation_;
	}

	void sqlite_shm_reader_live_close_obligation::disable_terminal_presentation() noexcept
	{
		terminal_presentation_disabled_ = true;
	}

	void sqlite_shm_reader_live_close_obligation::disarm() noexcept
	{
		group_token_ = 0U;
		generation_ = 0U;
		close_owner_token_ = 0U;
		registry_open_token_ = 0U;
		phase_ = phase::unmap_waiting;
		terminal_presentation_disabled_ = false;
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

	sqlite_shm_writer_attachment_cleanup& sqlite_shm_writer_release::cleanup() noexcept
	{
		return cleanup_;
	}

	sqlite_same_process_shm_mapping_lease_coordinator::
		sqlite_same_process_shm_mapping_lease_coordinator(
			sqlite_shm_lease_family_binding family,
			std::shared_ptr<sqlite_shm_mapping_generation_source> generations)
		: sqlite_same_process_shm_mapping_lease_coordinator{
			  std::move(family),
			  std::move(generations),
			  std::make_shared<sqlite_shm_reader_lifecycle_sequence_source>()}
	{
	}

	sqlite_same_process_shm_mapping_lease_coordinator::
		sqlite_same_process_shm_mapping_lease_coordinator(
			sqlite_shm_lease_family_binding family,
			std::shared_ptr<sqlite_shm_mapping_generation_source> generations,
			std::shared_ptr<sqlite_shm_reader_lifecycle_sequence_source> reader_lifecycle_sequences)
		: state_{std::make_shared<detail::sqlite_shm_mapping_lease_state>(
			  std::move(family), std::move(generations), std::move(reader_lifecycle_sequences))}
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

	sqlite_shm_lease_result<sqlite_shm_reader_session>
	sqlite_same_process_shm_mapping_lease_coordinator::begin_reader_session(
		const sqlite_shm_reader_session_request& request)
	{
		return state_->begin_reader_session(request);
	}

	sqlite_shm_lease_result<sqlite_shm_mapping_tuple>
	sqlite_same_process_shm_mapping_lease_coordinator::authenticate_reader_cached_member_use(
		const sqlite_shm_reader_session& session,
		const sqlite_shm_reader_cached_member_identity& member) noexcept
	{
		return state_->authenticate_reader_cached_member_use(session, member);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_session_admission>
	sqlite_same_process_shm_mapping_lease_coordinator::admit_registry_reader_session(
		sqlite_shm_registry_family_pin& family,
		const std::uint64_t registry_open_token,
		const sqlite_shm_reader_pre_sqlite_session_request& request,
		sqlite_shm_reader_candidate_authority_minter& candidate_minter)
	{
		return state_->admit_registry_reader_session(
			family, registry_open_token, request, candidate_minter);
	}

	sqlite_shm_lease_result<sqlite_shm_mapping_tuple>
	sqlite_same_process_shm_mapping_lease_coordinator::
		authenticate_registry_reader_cached_member_use(
			sqlite_shm_registry_family_pin& family,
			const sqlite_shm_reader_session& session,
			const sqlite_shm_reader_cached_member_identity& member) noexcept
	{
		return state_->authenticate_reader_cached_member_use(session, member, &family);
	}

	sqlite_shm_lease_result<void>
	sqlite_same_process_shm_mapping_lease_coordinator::register_registry_reader_open(
		const std::uint64_t registry_open_token,
		const std::shared_ptr<detail::sqlite_shm_reader_open_lineage_seal>& seal,
		const sqlite_shm_reader_open_epoch_binding& binding)
	{
		return state_->register_registry_reader_open(registry_open_token, seal, binding, nullptr);
	}

	sqlite_shm_lease_result<void>
	sqlite_same_process_shm_mapping_lease_coordinator::register_registry_reader_open(
		const std::uint64_t registry_open_token,
		const std::shared_ptr<detail::sqlite_shm_reader_open_lineage_seal>& seal,
		const sqlite_shm_reader_open_epoch_binding& binding,
		const detail::sqlite_shm_reader_open_admission_guard& admission_guard)
	{
		return state_->register_registry_reader_open(
			registry_open_token, seal, binding, &admission_guard);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_close_obligation>
	sqlite_same_process_shm_mapping_lease_coordinator::begin_registry_reader_close(
		const std::uint64_t registry_open_token,
		const std::shared_ptr<detail::sqlite_shm_reader_open_lineage_seal>& seal,
		const sqlite_shm_reader_open_epoch_binding& binding,
		const sqlite_shm_reader_close_request& request) noexcept
	{
		return state_->begin_registry_reader_close(registry_open_token, seal, binding, request);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_close_obligation>
	sqlite_same_process_shm_mapping_lease_coordinator::begin_registry_reader_close(
		const std::uint64_t registry_open_token,
		const std::shared_ptr<detail::sqlite_shm_reader_open_lineage_seal>& seal,
		const sqlite_shm_reader_open_epoch_binding& binding,
		const sqlite_shm_reader_close_request& request,
		std::optional<sqlite_shm_reader_attachment_authority>& completed_activity) noexcept
	{
		return state_->begin_registry_reader_close(
			registry_open_token, seal, binding, request, &completed_activity);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_close_cut_result>
	sqlite_same_process_shm_mapping_lease_coordinator::poll_registry_reader_close_cut(
		const std::uint64_t registry_open_token,
		const std::shared_ptr<detail::sqlite_shm_reader_open_lineage_seal>& seal,
		const sqlite_shm_reader_open_epoch_binding& binding,
		sqlite_shm_reader_close_obligation& close,
		const sqlite_shm_callback_execution_receipt& close_callback,
		std::optional<sqlite_shm_reader_attachment_authority>& completed_activity) noexcept
	{
		return state_->poll_registry_reader_close_cut(
			registry_open_token, seal, binding, close, close_callback, &completed_activity);
	}

	sqlite_shm_lease_result<void>
	sqlite_same_process_shm_mapping_lease_coordinator::fail_registry_reader_close_cut_wait(
		const std::uint64_t registry_open_token,
		const std::shared_ptr<detail::sqlite_shm_reader_open_lineage_seal>& seal,
		const sqlite_shm_reader_open_epoch_binding& binding,
		sqlite_shm_reader_close_obligation& close,
		const sqlite_shm_callback_execution_receipt& close_callback,
		const sqlite_shm_retirement_wait_failure failure) noexcept
	{
		return state_->fail_registry_reader_close_cut_wait(
			registry_open_token, seal, binding, close, close_callback, failure);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_close_terminal_result>
	sqlite_same_process_shm_mapping_lease_coordinator::complete_registry_reader_close(
		const std::uint64_t registry_open_token,
		const std::shared_ptr<detail::sqlite_shm_reader_open_lineage_seal>& seal,
		const sqlite_shm_reader_open_epoch_binding& binding,
		sqlite_shm_reader_close_obligation& close,
		const sqlite_shm_verified_reader_close_terminal_receipt& receipt) noexcept
	{
		return state_->complete_registry_reader_close(
			registry_open_token, seal, binding, close, receipt);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_close_terminal_result>
	sqlite_same_process_shm_mapping_lease_coordinator::complete_registry_reader_close(
		const std::uint64_t registry_open_token,
		const std::shared_ptr<detail::sqlite_shm_reader_open_lineage_seal>& seal,
		const sqlite_shm_reader_open_epoch_binding& binding,
		sqlite_shm_reader_close_obligation& close,
		const sqlite_shm_verified_reader_close_terminal_receipt& receipt,
		std::optional<sqlite_shm_reader_attachment_authority>& completed_activity) noexcept
	{
		return state_->complete_registry_reader_close(
			registry_open_token, seal, binding, close, receipt, &completed_activity);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_live_close_obligation>
	sqlite_same_process_shm_mapping_lease_coordinator::begin_registry_reader_live_close(
		const std::uint64_t registry_open_token,
		const std::shared_ptr<detail::sqlite_shm_reader_open_lineage_seal>& seal,
		const sqlite_shm_reader_open_epoch_binding& binding,
		sqlite_shm_reader_handoff& handoff,
		const sqlite_shm_reader_unmap_request& unmap_request,
		const sqlite_shm_reader_close_request& close_request) noexcept
	{
		return state_->begin_registry_reader_live_close(
			registry_open_token, seal, binding, handoff, unmap_request, close_request);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_unmap_cut_result>
	sqlite_same_process_shm_mapping_lease_coordinator::poll_registry_reader_live_close_unmap_cut(
		const std::uint64_t registry_open_token,
		const std::shared_ptr<detail::sqlite_shm_reader_open_lineage_seal>& seal,
		const sqlite_shm_reader_open_epoch_binding& binding,
		sqlite_shm_reader_live_close_obligation& close,
		const sqlite_shm_callback_execution_receipt& close_callback) noexcept
	{
		return state_->poll_registry_reader_live_close_unmap_cut(
			registry_open_token, seal, binding, close, close_callback);
	}

	sqlite_shm_lease_result<void> sqlite_same_process_shm_mapping_lease_coordinator::
		fail_registry_reader_live_close_unmap_cut_wait(
			const std::uint64_t registry_open_token,
			const std::shared_ptr<detail::sqlite_shm_reader_open_lineage_seal>& seal,
			const sqlite_shm_reader_open_epoch_binding& binding,
			sqlite_shm_reader_live_close_obligation& close,
			const sqlite_shm_callback_execution_receipt& close_callback,
			const sqlite_shm_retirement_wait_failure failure) noexcept
	{
		return state_->fail_registry_reader_live_close_unmap_cut_wait(
			registry_open_token, seal, binding, close, close_callback, failure);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_unmap_terminal_result>
	sqlite_same_process_shm_mapping_lease_coordinator::complete_registry_reader_live_close_unmap(
		const std::uint64_t registry_open_token,
		const std::shared_ptr<detail::sqlite_shm_reader_open_lineage_seal>& seal,
		const sqlite_shm_reader_open_epoch_binding& binding,
		sqlite_shm_reader_live_close_obligation& close,
		const sqlite_shm_verified_reader_unmap_terminal_receipt& receipt,
		std::optional<sqlite_shm_reader_attachment_authority>& completed_activity) noexcept
	{
		return state_->complete_registry_reader_live_close_unmap(
			registry_open_token, seal, binding, close, receipt, completed_activity);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_close_terminal_result>
	sqlite_same_process_shm_mapping_lease_coordinator::complete_registry_reader_live_close(
		const std::uint64_t registry_open_token,
		const std::shared_ptr<detail::sqlite_shm_reader_open_lineage_seal>& seal,
		const sqlite_shm_reader_open_epoch_binding& binding,
		sqlite_shm_reader_live_close_obligation& close,
		const sqlite_shm_verified_reader_close_terminal_receipt& receipt) noexcept
	{
		return state_->complete_registry_reader_live_close(
			registry_open_token, seal, binding, close, receipt);
	}

	sqlite_shm_lease_result<void>
	sqlite_same_process_shm_mapping_lease_coordinator::release_registry_reader_open(
		const std::uint64_t registry_open_token,
		const std::shared_ptr<detail::sqlite_shm_reader_open_lineage_seal>& seal) noexcept
	{
		return state_->release_registry_reader_open(registry_open_token, seal);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_attachment_map_inflight>
	sqlite_same_process_shm_mapping_lease_coordinator::begin_reader_map(
		sqlite_shm_reader_session& session, const sqlite_shm_reader_attachment_map_request& request)
	{
		return state_->begin_reader(session, request);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_attachment_map_inflight>
	sqlite_same_process_shm_mapping_lease_coordinator::begin_registry_reader_map(
		sqlite_shm_registry_family_pin& family,
		sqlite_shm_reader_session& session,
		const sqlite_shm_reader_attachment_map_request& request,
		sqlite_shm_reader_map_predelegate_minter& predelegate_minter)
	{
		return state_->begin_reader(session, request, &family, &predelegate_minter);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_map_commit>
	sqlite_same_process_shm_mapping_lease_coordinator::commit_reader_map(
		sqlite_shm_reader_attachment_map_inflight& inflight,
		const sqlite_shm_verified_reader_attachment_post_map_receipt& receipt,
		sqlite_shm_reader_session& session)
	{
		return state_->commit_reader(inflight, receipt, session);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_map_commit>
	sqlite_same_process_shm_mapping_lease_coordinator::commit_registry_reader_map(
		sqlite_shm_registry_family_pin& family,
		sqlite_shm_reader_attachment_map_inflight& inflight,
		const sqlite_shm_verified_reader_attachment_post_map_receipt& receipt,
		sqlite_shm_reader_session& session,
		std::optional<sqlite_shm_reader_map_predelegate_authority>& completed_predelegate)
	{
		return state_->commit_reader(inflight, receipt, session, &family, &completed_predelegate);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_attachment_zero_effect_result>
	sqlite_same_process_shm_mapping_lease_coordinator::complete_reader_zero_attachment_map(
		sqlite_shm_reader_attachment_map_inflight& inflight,
		const sqlite_shm_verified_reader_attachment_zero_effect_receipt& receipt,
		sqlite_shm_reader_session& session)
	{
		return state_->complete_reader_zero_attachment(inflight, receipt, session);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_attachment_zero_effect_result>
	sqlite_same_process_shm_mapping_lease_coordinator::complete_registry_reader_zero_attachment_map(
		sqlite_shm_registry_family_pin& family,
		sqlite_shm_reader_attachment_map_inflight& inflight,
		const sqlite_shm_verified_reader_attachment_zero_effect_receipt& receipt,
		sqlite_shm_reader_session& session,
		std::optional<sqlite_shm_reader_map_predelegate_authority>& completed_predelegate,
		std::optional<sqlite_shm_reader_attachment_authority>& completed_candidate)
	{
		return state_->complete_reader_zero_attachment(
			inflight, receipt, session, &family, &completed_predelegate, &completed_candidate);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_opaque_attachment_uncertainty_result>
	sqlite_same_process_shm_mapping_lease_coordinator::
		complete_reader_opaque_attachment_uncertainty(
			sqlite_shm_reader_attachment_map_inflight& inflight, sqlite_shm_reader_session& session)
	{
		return state_->complete_reader_opaque_attachment_uncertainty(inflight, session);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_opaque_attachment_uncertainty_result>
	sqlite_same_process_shm_mapping_lease_coordinator::
		complete_registry_reader_opaque_attachment_uncertainty(
			sqlite_shm_registry_family_pin& family,
			sqlite_shm_reader_attachment_map_inflight& inflight,
			sqlite_shm_reader_session& session)
	{
		return state_->complete_reader_opaque_attachment_uncertainty(inflight, session, &family);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_predecessor_map_result>
	sqlite_same_process_shm_mapping_lease_coordinator::complete_reader_predecessor_map(
		sqlite_shm_reader_attachment_map_inflight& inflight,
		const sqlite_shm_verified_reader_predecessor_map_receipt& receipt,
		sqlite_shm_reader_session& session)
	{
		return state_->complete_reader_predecessor_map(inflight, receipt, session);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_predecessor_map_result>
	sqlite_same_process_shm_mapping_lease_coordinator::complete_registry_reader_predecessor_map(
		sqlite_shm_registry_family_pin& family,
		sqlite_shm_reader_attachment_map_inflight& inflight,
		const sqlite_shm_verified_reader_predecessor_map_receipt& receipt,
		sqlite_shm_reader_session& session,
		std::optional<sqlite_shm_reader_map_predelegate_authority>& completed_predelegate,
		std::optional<sqlite_shm_reader_attachment_authority>& completed_candidate)
	{
		return state_->complete_reader_predecessor_map(
			inflight, receipt, session, &family, &completed_predelegate, &completed_candidate);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_existing_group_predecessor_mismatch_result>
	sqlite_same_process_shm_mapping_lease_coordinator::
		complete_reader_existing_group_predecessor_mismatch(
			sqlite_shm_reader_attachment_map_inflight& inflight,
			const sqlite_shm_verified_reader_predecessor_map_receipt& receipt,
			sqlite_shm_reader_session& session)
	{
		return state_->complete_reader_existing_group_predecessor_mismatch(
			inflight, receipt, session);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_existing_group_predecessor_mismatch_result>
	sqlite_same_process_shm_mapping_lease_coordinator::
		complete_registry_reader_existing_group_predecessor_mismatch(
			sqlite_shm_registry_family_pin& family,
			sqlite_shm_reader_attachment_map_inflight& inflight,
			const sqlite_shm_verified_reader_predecessor_map_receipt& receipt,
			sqlite_shm_reader_session& session,
			std::optional<sqlite_shm_reader_map_predelegate_authority>& completed_predelegate)
	{
		return state_->complete_reader_existing_group_predecessor_mismatch(
			inflight, receipt, session, &family, &completed_predelegate);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_predecessor_unmap_terminal_result>
	sqlite_same_process_shm_mapping_lease_coordinator::complete_reader_predecessor_unmap(
		const sqlite_shm_verified_reader_predecessor_unmap_terminal_receipt& receipt) noexcept
	{
		return state_->complete_reader_predecessor_unmap(receipt);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_predecessor_unmap_terminal_result>
	sqlite_same_process_shm_mapping_lease_coordinator::complete_registry_reader_predecessor_unmap(
		const std::uint64_t registry_open_token,
		const std::shared_ptr<detail::sqlite_shm_reader_open_lineage_seal>& seal,
		const sqlite_shm_reader_open_epoch_binding& binding,
		const sqlite_shm_verified_reader_predecessor_unmap_terminal_receipt& receipt,
		std::optional<sqlite_shm_reader_attachment_authority>& completed_activity) noexcept
	{
		return state_->complete_reader_predecessor_unmap(
			receipt, registry_open_token, seal, &binding, &completed_activity);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_unpublished_cleanup_obligation>
	sqlite_same_process_shm_mapping_lease_coordinator::begin_registry_reader_unpublished_cleanup(
		sqlite_shm_registry_family_pin& family,
		sqlite_shm_reader_attachment_map_inflight& inflight,
		const sqlite_shm_verified_reader_unpublished_cleanup_receipt& receipt,
		sqlite_shm_reader_session& session,
		std::optional<sqlite_shm_reader_map_predelegate_authority>& completed_predelegate)
	{
		return state_->begin_registry_reader_unpublished_cleanup(
			family, inflight, receipt, session, completed_predelegate);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_unpublished_cleanup_terminal_result>
	sqlite_same_process_shm_mapping_lease_coordinator::complete_registry_reader_unpublished_cleanup(
		sqlite_shm_reader_unpublished_cleanup_obligation& cleanup,
		const sqlite_shm_verified_reader_unpublished_cleanup_terminal_receipt& receipt) noexcept
	{
		return state_->complete_registry_reader_unpublished_cleanup(cleanup, receipt);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_logical_ack_result>
	sqlite_same_process_shm_mapping_lease_coordinator::consume_registry_reader_logical_ack(
		const std::uint64_t registry_open_token,
		const std::shared_ptr<detail::sqlite_shm_reader_open_lineage_seal>& seal,
		const sqlite_shm_reader_open_epoch_binding& binding,
		const sqlite_shm_reader_logical_ack_request& request,
		std::optional<sqlite_shm_reader_attachment_authority>& completed_activity) noexcept
	{
		return state_->consume_registry_reader_logical_ack(
			registry_open_token, seal, binding, request, completed_activity);
	}

	sqlite_shm_lease_result<void>
	sqlite_same_process_shm_mapping_lease_coordinator::complete_reader_session(
		sqlite_shm_reader_session& session,
		const sqlite_shm_reader_session_terminal_receipt& receipt) noexcept
	{
		return state_->complete_reader_session(session, receipt);
	}

	sqlite_shm_lease_result<void>
	sqlite_same_process_shm_mapping_lease_coordinator::complete_registry_reader_session(
		sqlite_shm_reader_session& session,
		const sqlite_shm_reader_session_terminal_receipt& receipt,
		std::optional<sqlite_shm_reader_attachment_authority>& completed_activity) noexcept
	{
		return state_->complete_reader_session(session, receipt, &completed_activity);
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

	sqlite_shm_lease_result<sqlite_shm_reader_unmap_obligation>
	sqlite_same_process_shm_mapping_lease_coordinator::begin_reader_unmap(
		sqlite_shm_reader_handoff& handoff, const sqlite_shm_reader_unmap_request& request) noexcept
	{
		return state_->begin_reader_unmap(handoff,
										  request.callback,
										  false,
										  true,
										  request.caller_delete_flag,
										  request.delegated_delete_flag);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_unmap_obligation>
	sqlite_same_process_shm_mapping_lease_coordinator::begin_registry_reader_unmap(
		sqlite_shm_reader_handoff& handoff,
		const sqlite_shm_callback_execution_receipt& callback) noexcept
	{
		return state_->begin_reader_unmap(handoff, callback, true, true, 0, 0);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_unmap_cut_result>
	sqlite_same_process_shm_mapping_lease_coordinator::poll_reader_unmap_cut(
		sqlite_shm_reader_unmap_obligation& unmap,
		const sqlite_shm_callback_execution_receipt& callback) noexcept
	{
		return state_->poll_reader_unmap_cut(unmap, callback, false);
	}

	sqlite_shm_lease_result<void>
	sqlite_same_process_shm_mapping_lease_coordinator::fail_reader_unmap_cut_wait(
		sqlite_shm_reader_unmap_obligation& unmap,
		const sqlite_shm_callback_execution_receipt& callback,
		const sqlite_shm_retirement_wait_failure failure) noexcept
	{
		return state_->fail_reader_unmap_cut_wait(unmap, callback, failure, false);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_unmap_cut_result>
	sqlite_same_process_shm_mapping_lease_coordinator::poll_registry_reader_unmap_cut(
		sqlite_shm_reader_unmap_obligation& unmap,
		const sqlite_shm_callback_execution_receipt& callback) noexcept
	{
		return state_->poll_reader_unmap_cut(unmap, callback, true);
	}

	sqlite_shm_lease_result<void>
	sqlite_same_process_shm_mapping_lease_coordinator::fail_registry_reader_unmap_cut_wait(
		sqlite_shm_reader_unmap_obligation& unmap,
		const sqlite_shm_callback_execution_receipt& callback,
		const sqlite_shm_retirement_wait_failure failure) noexcept
	{
		return state_->fail_reader_unmap_cut_wait(unmap, callback, failure, true);
	}

	sqlite_shm_lease_result<void>
	sqlite_same_process_shm_mapping_lease_coordinator::complete_reader_unmap(
		sqlite_shm_reader_unmap_obligation& unmap,
		const sqlite_shm_callback_execution_receipt& callback,
		const sqlite_shm_native_cleanup_outcome outcome) noexcept
	{
		return state_->complete_reader_unmap(unmap, callback, outcome);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_unmap_terminal_result>
	sqlite_same_process_shm_mapping_lease_coordinator::complete_reader_unmap(
		sqlite_shm_reader_unmap_obligation& unmap,
		const sqlite_shm_verified_reader_unmap_terminal_receipt& receipt) noexcept
	{
		return state_->complete_reader_unmap_exact(unmap, receipt);
	}

	sqlite_shm_lease_result<sqlite_shm_reader_unmap_terminal_result>
	sqlite_same_process_shm_mapping_lease_coordinator::complete_registry_reader_unmap(
		sqlite_shm_reader_unmap_obligation& unmap,
		const sqlite_shm_verified_reader_unmap_terminal_receipt& receipt,
		std::optional<sqlite_shm_reader_attachment_authority>& completed_activity) noexcept
	{
		return state_->complete_reader_unmap_exact(unmap, receipt, &completed_activity);
	}

	sqlite_shm_lease_result<std::vector<sqlite_shm_reader_lifecycle_compact_tombstone>>
	sqlite_same_process_shm_mapping_lease_coordinator::export_registry_reader_lifecycle_tombstones()
		const
	{
		return state_->export_registry_reader_lifecycle_tombstones();
	}

	sqlite_shm_lease_result<void>
	sqlite_same_process_shm_mapping_lease_coordinator::import_registry_reader_lifecycle_tombstones(
		const std::span<const sqlite_shm_reader_lifecycle_compact_tombstone> tombstones)
	{
		return state_->import_registry_reader_lifecycle_tombstones(tombstones);
	}

	sqlite_shm_lease_result<void>
	sqlite_same_process_shm_mapping_lease_coordinator::check_registry_reader_lifecycle_tombstone(
		const sqlite_shm_reader_attachment_reservation_identity& attachment) const noexcept
	{
		return state_->check_registry_reader_lifecycle_tombstone(attachment);
	}

	sqlite_shm_lease_result<std::vector<sqlite_shm_reader_open_epoch_close_tombstone>>
	sqlite_same_process_shm_mapping_lease_coordinator::
		export_registry_reader_open_epoch_close_tombstones() const
	{
		return state_->export_registry_reader_open_epoch_close_tombstones();
	}

	sqlite_shm_lease_result<void> sqlite_same_process_shm_mapping_lease_coordinator::
		import_registry_reader_open_epoch_close_tombstones(
			const std::span<const sqlite_shm_reader_open_epoch_close_tombstone> tombstones)
	{
		return state_->import_registry_reader_open_epoch_close_tombstones(tombstones);
	}

	sqlite_shm_lease_result<void> sqlite_same_process_shm_mapping_lease_coordinator::
		check_registry_reader_open_epoch_close_tombstone(
			const std::uint64_t registry_open_token,
			const sqlite_shm_reader_open_epoch_binding& binding) const noexcept
	{
		return state_->check_registry_reader_open_epoch_close_tombstone(registry_open_token,
																		binding);
	}

	sqlite_shm_reader_lifecycle_test_view
	sqlite_same_process_shm_mapping_lease_coordinator::reader_lifecycle_view_for_testing() const
	{
		return state_->reader_lifecycle_view_for_testing();
	}

	std::optional<sqlite_shm_reader_open_epoch_test_view>
	sqlite_same_process_shm_mapping_lease_coordinator::reader_open_epoch_view_for_testing(
		const std::uint64_t registry_open_token,
		const std::shared_ptr<detail::sqlite_shm_reader_open_lineage_seal>& seal) const noexcept
	{
		return state_->reader_open_epoch_view_for_testing(registry_open_token, seal);
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
		inject_reader_map_terminal_commit_failure_for_testing() noexcept
	{
		state_->inject_reader_map_terminal_commit_failure_for_testing();
	}

	void sqlite_same_process_shm_mapping_lease_coordinator::
		inject_reader_session_terminal_commit_failure_for_testing() noexcept
	{
		state_->inject_reader_session_terminal_commit_failure_for_testing();
	}

	void sqlite_same_process_shm_mapping_lease_coordinator::
		exhaust_reader_lifecycle_sequence_source_for_testing() noexcept
	{
		state_->exhaust_reader_lifecycle_sequence_source_for_testing();
	}

	void sqlite_same_process_shm_mapping_lease_coordinator::
		make_reader_lifecycle_sequence_source_unavailable_for_testing() noexcept
	{
		state_->make_reader_lifecycle_sequence_source_unavailable_for_testing();
	}

	void sqlite_same_process_shm_mapping_lease_coordinator::
		inject_reader_close_terminal_commit_failure_for_testing() noexcept
	{
		state_->inject_reader_close_terminal_commit_failure_for_testing();
	}

	void sqlite_same_process_shm_mapping_lease_coordinator::
		inject_reader_close_post_receipt_state_failure_for_testing() noexcept
	{
		state_->inject_reader_close_post_receipt_state_failure_for_testing();
	}

	void sqlite_same_process_shm_mapping_lease_coordinator::
		inject_reader_close_begin_preparation_failure_for_testing() noexcept
	{
		state_->inject_reader_close_begin_preparation_failure_for_testing();
	}

	void sqlite_same_process_shm_mapping_lease_coordinator::
		inject_reader_unmap_terminal_commit_failure_for_testing() noexcept
	{
		state_->inject_reader_unmap_terminal_commit_failure_for_testing();
	}

	void sqlite_same_process_shm_mapping_lease_coordinator::
		inject_reader_unmap_post_receipt_state_failure_for_testing() noexcept
	{
		state_->inject_reader_unmap_post_receipt_state_failure_for_testing();
	}

	void sqlite_same_process_shm_mapping_lease_coordinator::
		inject_reader_unpublished_cleanup_terminal_commit_failure_for_testing() noexcept
	{
		state_->inject_reader_unpublished_cleanup_terminal_commit_failure_for_testing();
	}

	void sqlite_same_process_shm_mapping_lease_coordinator::
		inject_reader_unmap_begin_preparation_failure_for_testing() noexcept
	{
		state_->inject_reader_unmap_begin_preparation_failure_for_testing();
	}

	void sqlite_same_process_shm_mapping_lease_coordinator::
		inject_reader_coarse_unmap_terminal_exception_for_testing() noexcept
	{
		state_->inject_reader_coarse_unmap_terminal_exception_for_testing();
	}

	void sqlite_same_process_shm_mapping_lease_coordinator::
		inject_reader_recovery_mutex_reacquire_failure_for_testing() noexcept
	{
		state_->inject_reader_recovery_mutex_reacquire_failure_for_testing();
	}

	void sqlite_same_process_shm_mapping_lease_coordinator::
		inject_reader_operation_mutex_acquire_failure_for_testing() noexcept
	{
		state_->inject_reader_operation_mutex_acquire_failure_for_testing();
	}

	void sqlite_same_process_shm_mapping_lease_coordinator::
		inject_registry_reader_attachment_liveness_loss_for_testing() noexcept
	{
		state_->inject_registry_reader_attachment_liveness_loss_for_testing();
	}

	void sqlite_same_process_shm_mapping_lease_coordinator::
		inject_registry_reader_predelegate_liveness_loss_for_testing() noexcept
	{
		state_->inject_registry_reader_predelegate_liveness_loss_for_testing();
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
