#include "sqlite_connection_lifecycle_internal.hpp"

#include <array>
#include <type_traits>
#include <utility>

#include "sqlite_backend_observation_internal.hpp"
#include "store_operation_port_internal.hpp"

namespace cxxlens::sdk
{
	struct sqlite_connection_lifecycle::state
	{
		state(void* raw_connection,
			  const sqlite_close_v2_callback close_callback,
			  sqlite_connection_lifetime_pins lifetime_pins)
			: connection{raw_connection}, close_v2{close_callback}, pins{std::move(lifetime_pins)},
			  lifecycle_identity{std::make_shared<std::uint8_t>(0)}
		{
		}

		void* connection{};
		sqlite_close_v2_callback close_v2{};
		sqlite_connection_lifetime_pins pins;
		std::shared_ptr<const void> lifecycle_identity;
		std::shared_ptr<state> quarantine_self;
		std::atomic_bool quarantine_enqueued{false};
		bool logical_read_exact_empty{};
		std::shared_ptr<const sqlite_source_shm_namespace_guard> logical_read_namespace_guard;
		std::shared_ptr<const sqlite_backend_opaque_identity> logical_read_capability_token;
		std::shared_ptr<const sqlite_backend_opaque_identity> logical_read_parent_identity;
		// Non-owning link; quarantine_self remains the owner for process lifetime.
		state* quarantine_next{};
	};

	std::atomic<sqlite_connection_lifecycle::state*> sqlite_connection_lifecycle::quarantine_head_{
		nullptr};

	namespace
	{
		constexpr int sqlite_ok = 0;

		/**
		 * Compare a census entry with the entry retained by the namespace guard.  The guard is the
		 * source-read authority; accepting only the copied state from the caller would allow a
		 * synthetic census to mint the exact-empty terminal after a replacement or role splice.
		 */
		[[nodiscard]] bool
		same_guard_entry(const sqlite_backend_entry_observation& census,
						 const sqlite_backend_entry_observation& retained) noexcept
		{
			return census.role == retained.role && census.state == retained.state &&
				census.object_identity == retained.object_identity &&
				census.directory_entry_identity == retained.directory_entry_identity &&
				census.held_object.get() == retained.held_object.get() &&
				census.object_filesystem_profile == retained.object_filesystem_profile &&
				census.direct_regular_entry == retained.direct_regular_entry;
		}

		[[nodiscard]] bool
		exact_empty_census_is_authenticated(const sqlite_backend_namespace_census& census) noexcept
		{
			if (!census.source_shm_guard || census.profile.empty() ||
				census.capability_token.bytes.empty() ||
				census.parent_namespace_identity.bytes.empty())
				return false;
			try
			{
				constexpr std::array roles{sqlite_backend_file_role::main_database,
										   sqlite_backend_file_role::write_ahead_log,
										   sqlite_backend_file_role::shared_memory,
										   sqlite_backend_file_role::rollback_journal};
				for (const auto role : roles)
				{
					const sqlite_backend_entry_observation* selected{};
					for (const auto& entry : census.entries)
						if (entry.role == role)
						{
							if (selected != nullptr)
								return false;
							selected = &entry;
						}
					if (selected == nullptr)
						return false;
					auto retained = census.source_shm_guard->retained_entry(role);
					if (!retained || !same_guard_entry(*selected, *retained))
						return false;
					if (role == sqlite_backend_file_role::main_database)
					{
						if (selected->state != sqlite_backend_entry_state::held_regular ||
							!selected->held_object || !selected->object_identity ||
							!selected->directory_entry_identity ||
							!selected->direct_regular_entry ||
							selected->held_object->role() != role ||
							selected->held_object->object_identity() !=
								*selected->object_identity ||
							selected->held_object->directory_entry_identity() !=
								*selected->directory_entry_identity)
							return false;
					}
					else if (selected->state != sqlite_backend_entry_state::absent ||
							 selected->object_identity || selected->directory_entry_identity ||
							 selected->held_object || selected->object_filesystem_profile ||
							 selected->direct_regular_entry)
						return false;
				}
				return census.source_shm_guard->recheck().has_value();
			}
			catch (...)
			{
				return false;
			}
		}
	} // namespace

	sqlite_authenticated_logical_read_terminal::sqlite_authenticated_logical_read_terminal(
		std::shared_ptr<const void> source_anchor_pin,
		std::shared_ptr<const void> lifecycle_identity,
		std::shared_ptr<const void> logical_read_namespace_guard,
		std::shared_ptr<const void> logical_read_capability_token,
		std::shared_ptr<const void> logical_read_parent_identity) noexcept
		: source_anchor_pin_{std::move(source_anchor_pin)},
		  lifecycle_identity_{std::move(lifecycle_identity)},
		  logical_read_namespace_guard_{std::move(logical_read_namespace_guard)},
		  logical_read_capability_token_{std::move(logical_read_capability_token)},
		  logical_read_parent_identity_{std::move(logical_read_parent_identity)}
	{
	}

	sqlite_authenticated_logical_read_terminal::sqlite_authenticated_logical_read_terminal(
		sqlite_authenticated_logical_read_terminal&& other) noexcept
		: source_anchor_pin_{std::move(other.source_anchor_pin_)},
		  lifecycle_identity_{std::move(other.lifecycle_identity_)},
		  logical_read_namespace_guard_{std::move(other.logical_read_namespace_guard_)},
		  logical_read_capability_token_{std::move(other.logical_read_capability_token_)},
		  logical_read_parent_identity_{std::move(other.logical_read_parent_identity_)},
		  valid_{std::exchange(other.valid_, false)}
	{
	}

	sqlite_authenticated_logical_read_terminal&
	sqlite_authenticated_logical_read_terminal::operator=(
		sqlite_authenticated_logical_read_terminal&& other) noexcept
	{
		if (this == &other)
			return *this;
		source_anchor_pin_ = std::move(other.source_anchor_pin_);
		lifecycle_identity_ = std::move(other.lifecycle_identity_);
		logical_read_namespace_guard_ = std::move(other.logical_read_namespace_guard_);
		logical_read_capability_token_ = std::move(other.logical_read_capability_token_);
		logical_read_parent_identity_ = std::move(other.logical_read_parent_identity_);
		valid_ = std::exchange(other.valid_, false);
		return *this;
	}

	bool sqlite_authenticated_logical_read_terminal::valid() const noexcept
	{
		return valid_ && source_anchor_pin_ != nullptr && lifecycle_identity_ != nullptr &&
			logical_read_namespace_guard_ != nullptr && logical_read_capability_token_ != nullptr &&
			logical_read_parent_identity_ != nullptr;
	}

	bool sqlite_authenticated_logical_read_terminal::exact_empty() const noexcept
	{
		return valid();
	}

	std::size_t sqlite_authenticated_logical_read_terminal::live_custody_count() const noexcept
	{
		return valid() ? 0U : 1U;
	}

	bool sqlite_authenticated_logical_read_terminal::zero_effect_callback_receipt() const noexcept
	{
		return valid();
	}

	const std::shared_ptr<const void>&
	sqlite_authenticated_logical_read_terminal::source_anchor_pin() const noexcept
	{
		return source_anchor_pin_;
	}

	bool sqlite_authenticated_logical_read_terminal::consume() noexcept
	{
		if (!valid())
			return false;
		valid_ = false;
		return true;
	}

	std::optional<sqlite_authenticated_logical_read_terminal>
	sqlite_logical_read_terminal_issuer::issue(
		const sqlite_confirmed_close_token& close_token,
		const sqlite_backend_namespace_census& source_census) noexcept
	{
		if (!close_token.valid() || !close_token.close_was_attempted() ||
			!close_token.logical_read_exact_empty_ || !close_token.authority_anchor_pin_ ||
			close_token.lifecycle_identity_ == nullptr ||
			close_token.logical_read_namespace_guard_ == nullptr ||
			close_token.logical_read_capability_token_ == nullptr ||
			close_token.logical_read_parent_identity_ == nullptr ||
			source_census.source_shm_guard == nullptr ||
			close_token.logical_read_namespace_guard_ !=
				std::static_pointer_cast<const void>(source_census.source_shm_guard))
			return std::nullopt;
		try
		{
			const auto* capability = static_cast<const sqlite_backend_opaque_identity*>(
				close_token.logical_read_capability_token_.get());
			const auto* parent = static_cast<const sqlite_backend_opaque_identity*>(
				close_token.logical_read_parent_identity_.get());
			if (capability == nullptr || parent == nullptr ||
				*capability != source_census.capability_token ||
				*parent != source_census.parent_namespace_identity ||
				!exact_empty_census_is_authenticated(source_census))
				return std::nullopt;
			const sqlite_backend_entry_observation* main{};
			const sqlite_backend_entry_observation* wal{};
			const sqlite_backend_entry_observation* shm{};
			const sqlite_backend_entry_observation* journal{};
			for (const auto& entry : source_census.entries)
			{
				const auto** selected = entry.role == sqlite_backend_file_role::main_database
					? &main
					: entry.role == sqlite_backend_file_role::write_ahead_log  ? &wal
					: entry.role == sqlite_backend_file_role::shared_memory	   ? &shm
					: entry.role == sqlite_backend_file_role::rollback_journal ? &journal
																			   : nullptr;
				if (selected == nullptr || *selected != nullptr)
					return std::nullopt;
				*selected = &entry;
			}
			if (main == nullptr || wal == nullptr || shm == nullptr || journal == nullptr ||
				main->state != sqlite_backend_entry_state::held_regular || !main->held_object ||
				!main->direct_regular_entry || !main->object_identity ||
				!main->directory_entry_identity ||
				wal->state != sqlite_backend_entry_state::absent ||
				shm->state != sqlite_backend_entry_state::absent ||
				journal->state != sqlite_backend_entry_state::absent ||
				std::static_pointer_cast<const void>(main->held_object) !=
					close_token.authority_anchor_pin_)
				return std::nullopt;
			return std::optional<sqlite_authenticated_logical_read_terminal>{
				sqlite_authenticated_logical_read_terminal{
					close_token.authority_anchor_pin_,
					close_token.lifecycle_identity_,
					close_token.logical_read_namespace_guard_,
					close_token.logical_read_capability_token_,
					close_token.logical_read_parent_identity_}};
		}
		catch (...)
		{
			return std::nullopt;
		}
	}

	sqlite_logical_read_receipt::sqlite_logical_read_receipt(
		std::shared_ptr<const void> source_anchor_pin,
		const bool exact_empty,
		const bool connection_closed,
		const std::size_t live_custody_count,
		const bool zero_effect_callback_receipt) noexcept
		: source_anchor_pin_{std::move(source_anchor_pin)}, exact_empty_{exact_empty},
		  connection_closed_{connection_closed}, live_custody_count_{live_custody_count},
		  zero_effect_callback_receipt_{zero_effect_callback_receipt}
	{
	}

	sqlite_logical_read_receipt::sqlite_logical_read_receipt(
		sqlite_logical_read_receipt&& other) noexcept
		: source_anchor_pin_{std::move(other.source_anchor_pin_)}, exact_empty_{other.exact_empty_},
		  connection_closed_{other.connection_closed_},
		  live_custody_count_{other.live_custody_count_},
		  zero_effect_callback_receipt_{other.zero_effect_callback_receipt_},
		  valid_{std::exchange(other.valid_, false)}
	{
	}

	sqlite_logical_read_receipt&
	sqlite_logical_read_receipt::operator=(sqlite_logical_read_receipt&& other) noexcept
	{
		if (this == &other)
			return *this;
		source_anchor_pin_ = std::move(other.source_anchor_pin_);
		exact_empty_ = other.exact_empty_;
		connection_closed_ = other.connection_closed_;
		live_custody_count_ = other.live_custody_count_;
		zero_effect_callback_receipt_ = other.zero_effect_callback_receipt_;
		valid_ = std::exchange(other.valid_, false);
		return *this;
	}

	bool sqlite_logical_read_receipt::valid() const noexcept
	{
		return valid_ && source_anchor_pin_ != nullptr;
	}

	bool sqlite_logical_read_receipt::exact_empty() const noexcept
	{
		return exact_empty_;
	}

	bool sqlite_logical_read_receipt::connection_closed() const noexcept
	{
		return connection_closed_;
	}

	std::size_t sqlite_logical_read_receipt::live_custody_count() const noexcept
	{
		return live_custody_count_;
	}

	bool sqlite_logical_read_receipt::zero_effect_callback_receipt() const noexcept
	{
		return zero_effect_callback_receipt_;
	}

	const std::shared_ptr<const void>&
	sqlite_logical_read_receipt::source_anchor_pin() const noexcept
	{
		return source_anchor_pin_;
	}

	bool sqlite_logical_read_receipt::consume() noexcept
	{
		if (!valid())
			return false;
		valid_ = false;
		return true;
	}

	std::optional<sqlite_logical_read_receipt>
	seal_sqlite_logical_read_receipt(sqlite_confirmed_close_token&& close_token,
									 sqlite_authenticated_logical_read_terminal&& terminal) noexcept
	{
		if (!close_token.valid() || !close_token.close_was_attempted() || !terminal.valid() ||
			!terminal.exact_empty() || terminal.live_custody_count() != 0U ||
			!terminal.zero_effect_callback_receipt() ||
			close_token.authority_anchor_pin_ != terminal.source_anchor_pin_ ||
			close_token.lifecycle_identity_ != terminal.lifecycle_identity_ ||
			close_token.logical_read_namespace_guard_ != terminal.logical_read_namespace_guard_ ||
			close_token.logical_read_capability_token_ != terminal.logical_read_capability_token_ ||
			close_token.logical_read_parent_identity_ != terminal.logical_read_parent_identity_)
			return std::nullopt;
		const auto exact_empty = terminal.exact_empty();
		const auto live_custody_count = terminal.live_custody_count();
		const auto zero_effect_callback_receipt = terminal.zero_effect_callback_receipt();
		auto source_anchor_pin = terminal.source_anchor_pin();
		if (!close_token.consume() || !terminal.consume())
			return std::nullopt;
		try
		{
			sqlite_logical_read_receipt receipt{std::move(source_anchor_pin),
												exact_empty,
												true,
												live_custody_count,
												zero_effect_callback_receipt};
			return std::optional<sqlite_logical_read_receipt>{std::move(receipt)};
		}
		catch (...)
		{
			return std::nullopt;
		}
	}

	sqlite_confirmed_close_token::sqlite_confirmed_close_token(
		const sqlite_confirmed_close_kind kind,
		std::shared_ptr<const void> authority_anchor_pin,
		std::shared_ptr<const void> lifecycle_identity,
		const bool logical_read_exact_empty,
		std::shared_ptr<const void> logical_read_namespace_guard,
		std::shared_ptr<const void> logical_read_capability_token,
		std::shared_ptr<const void> logical_read_parent_identity) noexcept
		: kind_{kind}, authority_anchor_pin_{std::move(authority_anchor_pin)},
		  lifecycle_identity_{lifecycle_identity},
		  logical_read_exact_empty_{logical_read_exact_empty},
		  logical_read_namespace_guard_{std::move(logical_read_namespace_guard)},
		  logical_read_capability_token_{std::move(logical_read_capability_token)},
		  logical_read_parent_identity_{std::move(logical_read_parent_identity)}
	{
	}

	sqlite_confirmed_close_token::sqlite_confirmed_close_token(
		sqlite_confirmed_close_token&& other) noexcept
		: kind_{other.kind_}, authority_anchor_pin_{std::move(other.authority_anchor_pin_)},
		  lifecycle_identity_{other.lifecycle_identity_},
		  logical_read_exact_empty_{other.logical_read_exact_empty_},
		  logical_read_namespace_guard_{std::move(other.logical_read_namespace_guard_)},
		  logical_read_capability_token_{std::move(other.logical_read_capability_token_)},
		  logical_read_parent_identity_{std::move(other.logical_read_parent_identity_)},
		  valid_{std::exchange(other.valid_, false)}
	{
	}

	sqlite_confirmed_close_token&
	sqlite_confirmed_close_token::operator=(sqlite_confirmed_close_token&& other) noexcept
	{
		if (this == &other)
			return *this;
		kind_ = other.kind_;
		authority_anchor_pin_ = std::move(other.authority_anchor_pin_);
		lifecycle_identity_ = other.lifecycle_identity_;
		logical_read_exact_empty_ = other.logical_read_exact_empty_;
		logical_read_namespace_guard_ = std::move(other.logical_read_namespace_guard_);
		logical_read_capability_token_ = std::move(other.logical_read_capability_token_);
		logical_read_parent_identity_ = std::move(other.logical_read_parent_identity_);
		valid_ = std::exchange(other.valid_, false);
		return *this;
	}

	bool sqlite_confirmed_close_token::valid() const noexcept
	{
		return valid_;
	}

	sqlite_confirmed_close_kind sqlite_confirmed_close_token::kind() const noexcept
	{
		return kind_;
	}

	bool sqlite_confirmed_close_token::close_was_attempted() const noexcept
	{
		return kind_ == sqlite_confirmed_close_kind::sqlite_ok;
	}

	bool sqlite_confirmed_close_token::logical_read_exact_empty() const noexcept
	{
		return logical_read_exact_empty_;
	}

	bool sqlite_confirmed_close_token::consume() noexcept
	{
		if (!valid() || !close_was_attempted())
			return false;
		valid_ = false;
		return true;
	}

	sqlite_quarantined_connection::sqlite_quarantined_connection(
		const sqlite_connection_quarantine_reason reason, std::optional<int> sqlite_code) noexcept
		: reason_{reason}, sqlite_code_{sqlite_code}
	{
	}

	sqlite_quarantined_connection::sqlite_quarantined_connection(
		sqlite_quarantined_connection&& other) noexcept
		: reason_{other.reason_}, sqlite_code_{other.sqlite_code_},
		  valid_{std::exchange(other.valid_, false)}
	{
	}

	sqlite_quarantined_connection&
	sqlite_quarantined_connection::operator=(sqlite_quarantined_connection&& other) noexcept
	{
		if (this == &other)
			return *this;
		reason_ = other.reason_;
		sqlite_code_ = other.sqlite_code_;
		valid_ = std::exchange(other.valid_, false);
		return *this;
	}

	bool sqlite_quarantined_connection::valid() const noexcept
	{
		return valid_;
	}

	sqlite_connection_quarantine_reason sqlite_quarantined_connection::reason() const noexcept
	{
		return reason_;
	}

	std::optional<int> sqlite_quarantined_connection::sqlite_code() const noexcept
	{
		return sqlite_code_;
	}

	sqlite_connection_lifecycle::sqlite_connection_lifecycle(
		void* connection,
		const sqlite_close_v2_callback close_v2,
		sqlite_connection_lifetime_pins pins)
		: state_{std::make_shared<state>(connection, close_v2, std::move(pins))}
	{
		// The open-result slot is handed to native code after construction. Arm its quarantine
		// owner now, while allocation can still fail without transferring the native handle.
		static_assert(std::is_nothrow_copy_assignable_v<std::shared_ptr<state>>);
		state_->quarantine_self = state_;
	}

	sqlite_connection_lifecycle::~sqlite_connection_lifecycle() noexcept
	{
		cleanup_noexcept();
	}

	sqlite_connection_lifecycle::sqlite_connection_lifecycle(
		sqlite_connection_lifecycle&& other) noexcept = default;

	sqlite_connection_lifecycle&
	sqlite_connection_lifecycle::operator=(sqlite_connection_lifecycle&& other) noexcept
	{
		if (this == &other)
			return *this;
		cleanup_noexcept();
		state_ = std::move(other.state_);
		return *this;
	}

	bool sqlite_connection_lifecycle::owns_connection() const noexcept
	{
		return state_ != nullptr && state_->connection != nullptr;
	}

	void* sqlite_connection_lifecycle::get() const noexcept
	{
		return state_ != nullptr ? state_->connection : nullptr;
	}

	void** sqlite_connection_lifecycle::open_handle_out_parameter() noexcept
	{
		return state_ != nullptr && state_->connection == nullptr ? &state_->connection : nullptr;
	}

	bool sqlite_connection_lifecycle::mark_logical_read_exact_empty(
		const sqlite_backend_namespace_census& source_census) noexcept
	{
		if (state_ == nullptr || state_->connection == nullptr || source_census.profile.empty() ||
			source_census.capability_token.bytes.empty() ||
			source_census.parent_namespace_identity.bytes.empty() ||
			source_census.source_shm_guard == nullptr || !state_->pins.authority_anchor)
			return false;
		try
		{
			const sqlite_backend_entry_observation* main{};
			const sqlite_backend_entry_observation* wal{};
			const sqlite_backend_entry_observation* shm{};
			const sqlite_backend_entry_observation* journal{};
			for (const auto& entry : source_census.entries)
			{
				const auto** selected = entry.role == sqlite_backend_file_role::main_database
					? &main
					: entry.role == sqlite_backend_file_role::write_ahead_log  ? &wal
					: entry.role == sqlite_backend_file_role::shared_memory	   ? &shm
					: entry.role == sqlite_backend_file_role::rollback_journal ? &journal
																			   : nullptr;
				if (selected == nullptr || *selected != nullptr)
					return false;
				*selected = &entry;
			}
			if (main == nullptr || wal == nullptr || shm == nullptr || journal == nullptr ||
				main->state != sqlite_backend_entry_state::held_regular || !main->held_object ||
				!main->direct_regular_entry || !main->object_identity ||
				!main->directory_entry_identity ||
				wal->state != sqlite_backend_entry_state::absent ||
				shm->state != sqlite_backend_entry_state::absent ||
				journal->state != sqlite_backend_entry_state::absent ||
				std::static_pointer_cast<const void>(main->held_object) !=
					state_->pins.authority_anchor ||
				!exact_empty_census_is_authenticated(source_census))
				return false;
			state_->logical_read_namespace_guard = source_census.source_shm_guard;
			state_->logical_read_capability_token =
				std::make_shared<const sqlite_backend_opaque_identity>(
					source_census.capability_token);
			state_->logical_read_parent_identity =
				std::make_shared<const sqlite_backend_opaque_identity>(
					source_census.parent_namespace_identity);
			state_->logical_read_exact_empty = true;
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	sqlite_connection_close_outcome sqlite_connection_lifecycle::close_exactly_once() noexcept
	{
		auto owned = std::move(state_);
		if (owned == nullptr)
			return sqlite_confirmed_close_token{sqlite_confirmed_close_kind::no_connection};
		if (owned->connection == nullptr)
		{
			release_known_safe(owned);
			return sqlite_confirmed_close_token{sqlite_confirmed_close_kind::no_connection};
		}

		if (owned->close_v2 == nullptr)
			return quarantine(
				owned, sqlite_connection_quarantine_reason::close_callback_missing, std::nullopt);

		try
		{
			const auto code = owned->close_v2(owned->connection);
			if (code == sqlite_ok)
			{
				auto anchor = owned->pins.authority_anchor;
				const auto identity = owned->lifecycle_identity;
				const auto exact_empty = owned->logical_read_exact_empty;
				const auto namespace_guard =
					std::static_pointer_cast<const void>(owned->logical_read_namespace_guard);
				const auto capability =
					std::static_pointer_cast<const void>(owned->logical_read_capability_token);
				const auto parent =
					std::static_pointer_cast<const void>(owned->logical_read_parent_identity);
				release_known_safe(owned);
				return sqlite_confirmed_close_token{sqlite_confirmed_close_kind::sqlite_ok,
													std::move(anchor),
													identity,
													exact_empty,
													std::move(namespace_guard),
													std::move(capability),
													std::move(parent)};
			}
			return quarantine(owned, sqlite_connection_quarantine_reason::close_non_ok, code);
		}
		catch (...)
		{
			return quarantine(
				owned, sqlite_connection_quarantine_reason::close_callback_threw, std::nullopt);
		}
	}

	sqlite_connection_close_outcome
	sqlite_connection_lifecycle::close_exactly_once(store_operation_port& operation_port) noexcept
	{
		auto owned = std::move(state_);
		if (owned == nullptr)
			return sqlite_confirmed_close_token{sqlite_confirmed_close_kind::no_connection};
		if (owned->connection == nullptr)
		{
			release_known_safe(owned);
			return sqlite_confirmed_close_token{sqlite_confirmed_close_kind::no_connection};
		}
		if (owned->close_v2 == nullptr)
			return quarantine(
				owned, sqlite_connection_quarantine_reason::close_callback_missing, std::nullopt);

		const auto outcome = operation_port.close_sqlite(store_sqlite_operation_binding{
			owned->connection,
			owned->close_v2,
			sqlite_ok,
		});
		if (outcome.confirmed())
		{
			auto anchor = owned->pins.authority_anchor;
			const auto identity = owned->lifecycle_identity;
			const auto exact_empty = owned->logical_read_exact_empty;
			const auto namespace_guard =
				std::static_pointer_cast<const void>(owned->logical_read_namespace_guard);
			const auto capability =
				std::static_pointer_cast<const void>(owned->logical_read_capability_token);
			const auto parent =
				std::static_pointer_cast<const void>(owned->logical_read_parent_identity);
			release_known_safe(owned);
			return sqlite_confirmed_close_token{sqlite_confirmed_close_kind::sqlite_ok,
												std::move(anchor),
												identity,
												exact_empty,
												std::move(namespace_guard),
												std::move(capability),
												std::move(parent)};
		}
		return quarantine(owned,
						  sqlite_connection_quarantine_reason::close_non_ok,
						  outcome.operation_attempted ? std::optional<int>{outcome.native_code}
													  : std::nullopt);
	}

	sqlite_quarantined_connection
	sqlite_connection_lifecycle::quarantine(std::shared_ptr<state>& owned,
											const sqlite_connection_quarantine_reason reason,
											const std::optional<int> sqlite_code) noexcept
	{
		// `quarantine_self` was armed before native code could fill the open-result slot. Keep the
		// quarantined state reachable through a process-lifetime sink instead of leaving an
		// unreachable self-cycle behind.
		if (owned)
		{
			static_assert(std::atomic<state*>::is_always_lock_free);
			bool expected = false;
			if (!owned->quarantine_enqueued.compare_exchange_strong(
					expected, true, std::memory_order_acq_rel, std::memory_order_acquire))
			{
				owned.reset();
				return sqlite_quarantined_connection{reason, sqlite_code};
			}
			auto* previous = quarantine_head_.load(std::memory_order_acquire);
			do
			{
				owned->quarantine_next = previous;
			} while (!quarantine_head_.compare_exchange_weak(
				previous, owned.get(), std::memory_order_release, std::memory_order_acquire));
			owned.reset();
		}
		return sqlite_quarantined_connection{reason, sqlite_code};
	}

	void sqlite_connection_lifecycle::release_known_safe(std::shared_ptr<state>& owned) noexcept
	{
		if (owned && !owned->quarantine_enqueued.load(std::memory_order_acquire))
			owned->quarantine_self.reset();
		owned.reset();
	}

	void sqlite_connection_lifecycle::cleanup_noexcept() noexcept
	{
		if (state_ != nullptr)
			(void)close_exactly_once();
	}
} // namespace cxxlens::sdk
