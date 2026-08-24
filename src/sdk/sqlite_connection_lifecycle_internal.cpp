#include "sqlite_connection_lifecycle_internal.hpp"

#include <type_traits>
#include <utility>

#include "store_operation_port_internal.hpp"

namespace cxxlens::sdk
{
	struct sqlite_connection_lifecycle::state
	{
		state(void* raw_connection,
			  const sqlite_close_v2_callback close_callback,
			  sqlite_connection_lifetime_pins lifetime_pins)
			: connection{raw_connection}, close_v2{close_callback}, pins{std::move(lifetime_pins)}
		{
		}

		void* connection{};
		sqlite_close_v2_callback close_v2{};
		sqlite_connection_lifetime_pins pins;
		std::shared_ptr<state> quarantine_self;
		std::atomic_bool quarantine_enqueued{false};
		// Non-owning link; quarantine_self remains the owner for process lifetime.
		state* quarantine_next{};
	};

	std::atomic<sqlite_connection_lifecycle::state*> sqlite_connection_lifecycle::quarantine_head_{
		nullptr};

	namespace
	{
		constexpr int sqlite_ok = 0;
	} // namespace

	sqlite_authenticated_logical_read_terminal::sqlite_authenticated_logical_read_terminal(
		std::shared_ptr<const void> source_anchor_pin,
		const bool exact_empty,
		const std::size_t live_custody_count,
		const bool zero_effect_callback_receipt) noexcept
		: source_anchor_pin_{std::move(source_anchor_pin)}, exact_empty_{exact_empty},
		  live_custody_count_{live_custody_count},
		  zero_effect_callback_receipt_{zero_effect_callback_receipt}
	{
	}

	sqlite_authenticated_logical_read_terminal::sqlite_authenticated_logical_read_terminal(
		sqlite_authenticated_logical_read_terminal&& other) noexcept
		: source_anchor_pin_{std::move(other.source_anchor_pin_)}, exact_empty_{other.exact_empty_},
		  live_custody_count_{other.live_custody_count_},
		  zero_effect_callback_receipt_{other.zero_effect_callback_receipt_},
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
		exact_empty_ = other.exact_empty_;
		live_custody_count_ = other.live_custody_count_;
		zero_effect_callback_receipt_ = other.zero_effect_callback_receipt_;
		valid_ = std::exchange(other.valid_, false);
		return *this;
	}

	bool sqlite_authenticated_logical_read_terminal::valid() const noexcept
	{
		return valid_ && source_anchor_pin_ != nullptr;
	}

	bool sqlite_authenticated_logical_read_terminal::exact_empty() const noexcept
	{
		return exact_empty_;
	}

	std::size_t sqlite_authenticated_logical_read_terminal::live_custody_count() const noexcept
	{
		return live_custody_count_;
	}

	bool sqlite_authenticated_logical_read_terminal::zero_effect_callback_receipt() const noexcept
	{
		return zero_effect_callback_receipt_;
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
	sqlite_logical_read_terminal_issuer::issue(std::shared_ptr<const void> source_anchor_pin,
											   const bool exact_empty,
											   const std::size_t live_custody_count,
											   const bool zero_effect_callback_receipt) noexcept
	{
		if (!source_anchor_pin || !exact_empty || live_custody_count != 0U ||
			!zero_effect_callback_receipt)
			return std::nullopt;
		try
		{
			return std::optional<sqlite_authenticated_logical_read_terminal>{
				sqlite_authenticated_logical_read_terminal{std::move(source_anchor_pin),
														   exact_empty,
														   live_custody_count,
														   zero_effect_callback_receipt}};
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
			!terminal.zero_effect_callback_receipt() || !close_token.consume() ||
			!terminal.consume())
			return std::nullopt;
		try
		{
			sqlite_logical_read_receipt receipt{terminal.source_anchor_pin(),
												terminal.exact_empty(),
												true,
												terminal.live_custody_count(),
												terminal.zero_effect_callback_receipt()};
			return std::optional<sqlite_logical_read_receipt>{std::move(receipt)};
		}
		catch (...)
		{
			return std::nullopt;
		}
	}

	sqlite_confirmed_close_token::sqlite_confirmed_close_token(
		const sqlite_confirmed_close_kind kind) noexcept
		: kind_{kind}
	{
	}

	sqlite_confirmed_close_token::sqlite_confirmed_close_token(
		sqlite_confirmed_close_token&& other) noexcept
		: kind_{other.kind_}, valid_{std::exchange(other.valid_, false)}
	{
	}

	sqlite_confirmed_close_token&
	sqlite_confirmed_close_token::operator=(sqlite_confirmed_close_token&& other) noexcept
	{
		if (this == &other)
			return *this;
		kind_ = other.kind_;
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
				release_known_safe(owned);
				return sqlite_confirmed_close_token{sqlite_confirmed_close_kind::sqlite_ok};
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
			release_known_safe(owned);
			return sqlite_confirmed_close_token{sqlite_confirmed_close_kind::sqlite_ok};
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
