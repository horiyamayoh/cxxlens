#include "sqlite_connection_lifecycle_internal.hpp"

#include <atomic>
#include <utility>

namespace cxxlens::sdk
{
	struct sqlite_connection_lifecycle::state
	{
		void* connection{};
		sqlite_close_v2_callback close_v2{};
		sqlite_connection_lifetime_pins pins;
		state* quarantine_next{};
	};

	namespace
	{
		constexpr int sqlite_ok = 0;
		// Intentionally never reclaimed: every linked node retains an uncertain connection and
		// pins.
		std::atomic<void*> quarantine_head{};
	} // namespace

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
		: state_{std::make_unique<state>(connection, close_v2, std::move(pins), nullptr)}
	{
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
		if (owned == nullptr || owned->connection == nullptr)
			return sqlite_confirmed_close_token{sqlite_confirmed_close_kind::no_connection};

		if (owned->close_v2 == nullptr)
			return quarantine(std::move(owned),
							  sqlite_connection_quarantine_reason::close_callback_missing,
							  std::nullopt);

		try
		{
			const auto code = owned->close_v2(owned->connection);
			if (code == sqlite_ok)
				return sqlite_confirmed_close_token{sqlite_confirmed_close_kind::sqlite_ok};
			return quarantine(
				std::move(owned), sqlite_connection_quarantine_reason::close_non_ok, code);
		}
		catch (...)
		{
			return quarantine(std::move(owned),
							  sqlite_connection_quarantine_reason::close_callback_threw,
							  std::nullopt);
		}
	}

	sqlite_quarantined_connection
	sqlite_connection_lifecycle::quarantine(std::unique_ptr<state> owned,
											const sqlite_connection_quarantine_reason reason,
											const std::optional<int> sqlite_code) noexcept
	{
		auto* const node = owned.release();
		void* observed = quarantine_head.load(std::memory_order_relaxed);
		do
		{
			node->quarantine_next = static_cast<state*>(observed);
		} while (!quarantine_head.compare_exchange_weak(
			observed, node, std::memory_order_release, std::memory_order_relaxed));
		return sqlite_quarantined_connection{reason, sqlite_code};
	}

	void sqlite_connection_lifecycle::cleanup_noexcept() noexcept
	{
		if (state_ != nullptr)
			(void)close_exactly_once();
	}
} // namespace cxxlens::sdk
