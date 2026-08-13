#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "sdk/sqlite_connection_lifecycle_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;
	static_assert(!std::is_copy_constructible_v<sqlite_connection_lifecycle>);
	static_assert(std::is_nothrow_move_constructible_v<sqlite_connection_lifecycle>);
	static_assert(std::is_nothrow_move_assignable_v<sqlite_connection_lifecycle>);
	static_assert(std::is_nothrow_destructible_v<sqlite_connection_lifecycle>);
	static_assert(noexcept(std::declval<sqlite_connection_lifecycle&>().close_exactly_once()));
	static_assert(!std::is_copy_constructible_v<sqlite_confirmed_close_token>);
	static_assert(!std::is_copy_constructible_v<sqlite_quarantined_connection>);

	void require(const bool condition, const std::string& message)
	{
		if (!condition)
			throw std::runtime_error{message};
	}

	struct pin_tracker
	{
		explicit pin_tracker(int& destruction_count) noexcept
			: destruction_count_{&destruction_count}
		{
		}

		~pin_tracker()
		{
			++*destruction_count_;
		}

		int* destruction_count_{};
	};

	struct observed_pins
	{
		std::weak_ptr<void> runtime;
		std::weak_ptr<void> vfs;
		std::weak_ptr<void> observation;
		std::weak_ptr<const void> authority_anchor;
	};

	[[nodiscard]] sqlite_connection_lifetime_pins make_pins(int& destruction_count,
															observed_pins& observed)
	{
		auto runtime_pin = std::make_shared<pin_tracker>(destruction_count);
		auto vfs_pin = std::make_shared<pin_tracker>(destruction_count);
		auto observation_pin = std::make_shared<pin_tracker>(destruction_count);
		auto authority_anchor_pin = std::make_shared<pin_tracker>(destruction_count);
		observed.runtime = runtime_pin;
		observed.vfs = vfs_pin;
		observed.observation = observation_pin;
		observed.authority_anchor = authority_anchor_pin;
		return {std::move(runtime_pin),
				std::move(vfs_pin),
				std::move(observation_pin),
				std::move(authority_anchor_pin)};
	}

	struct close_probe
	{
		int calls{};
		int result{};
		int* pin_destruction_count{};
		bool pins_alive_during_close{};
	};

	int probed_close(void* raw)
	{
		auto& probe = *static_cast<close_probe*>(raw);
		++probe.calls;
		probe.pins_alive_during_close =
			probe.pin_destruction_count != nullptr && *probe.pin_destruction_count == 0;
		return probe.result;
	}

	int throwing_close(void* raw)
	{
		auto& probe = *static_cast<close_probe*>(raw);
		++probe.calls;
		probe.pins_alive_during_close =
			probe.pin_destruction_count != nullptr && *probe.pin_destruction_count == 0;
		throw std::runtime_error{"synthetic close exception"};
	}

	void verify_null_handle_closes_zero_times()
	{
		int destroyed{};
		observed_pins pins;
		close_probe probe{0, 0, &destroyed, false};
		{
			sqlite_connection_lifecycle owner{nullptr, &probed_close, make_pins(destroyed, pins)};
			require(!owner.owns_connection() && owner.get() == nullptr,
					"null lifecycle reported an owned connection");
			auto outcome = owner.close_exactly_once();
			require(std::holds_alternative<sqlite_confirmed_close_token>(outcome),
					"null lifecycle did not produce a confirmed no-connection token");
			auto& token = std::get<sqlite_confirmed_close_token>(outcome);
			require(token.valid() && token.kind() == sqlite_confirmed_close_kind::no_connection &&
						!token.close_was_attempted(),
					"null lifecycle produced the wrong token");
			require(probe.calls == 0, "null lifecycle called close_v2");
			require(pins.runtime.expired() && pins.vfs.expired() && pins.observation.expired() &&
						pins.authority_anchor.expired() && destroyed == 4,
					"null lifecycle retained pins after explicit release");

			auto repeated = owner.close_exactly_once();
			require(std::holds_alternative<sqlite_confirmed_close_token>(repeated) &&
						!std::get<sqlite_confirmed_close_token>(repeated).close_was_attempted() &&
						probe.calls == 0,
					"repeated null close attempted close_v2");
		}
		require(probe.calls == 0, "null lifecycle destructor called close_v2");
	}

	void verify_explicit_success_closes_once_and_releases_pins()
	{
		int destroyed{};
		observed_pins pins;
		close_probe probe{0, 0, &destroyed, false};
		{
			sqlite_connection_lifecycle owner{&probe, &probed_close, make_pins(destroyed, pins)};
			require(owner.owns_connection() && owner.get() == &probe,
					"live lifecycle lost its raw connection");
			auto outcome = owner.close_exactly_once();
			require(std::holds_alternative<sqlite_confirmed_close_token>(outcome),
					"SQLITE_OK did not produce a confirmed-close token");
			auto& stored = std::get<sqlite_confirmed_close_token>(outcome);
			require(stored.valid() && stored.kind() == sqlite_confirmed_close_kind::sqlite_ok &&
						stored.close_was_attempted(),
					"SQLITE_OK produced the wrong close token");
			auto moved = std::move(stored);
			require(moved.valid(), "confirmed-close token lost proof state when moved");
			require(probe.calls == 1 && probe.pins_alive_during_close,
					"explicit close was not exact-once with pins retained through the call");
			require(pins.runtime.expired() && pins.vfs.expired() && pins.observation.expired() &&
						pins.authority_anchor.expired() && destroyed == 4,
					"confirmed close did not release all pins");
			require(!owner.owns_connection() && owner.get() == nullptr,
					"confirmed close retained the raw handle");
			(void)owner.close_exactly_once();
			require(probe.calls == 1, "second explicit close retried close_v2");
		}
		require(probe.calls == 1, "destructor retried a confirmed explicit close");
	}

	void verify_preopen_slot_owns_non_null_result_without_allocation()
	{
		int destroyed{};
		observed_pins pins;
		close_probe probe{0, 0, &destroyed, false};
		{
			sqlite_connection_lifecycle owner{nullptr, &probed_close, make_pins(destroyed, pins)};
			auto** slot = owner.open_handle_out_parameter();
			require(slot != nullptr && *slot == nullptr,
					"pre-open lifecycle did not expose its empty result slot");
			*slot = &probe;
			require(owner.owns_connection() && owner.get() == &probe &&
						owner.open_handle_out_parameter() == nullptr,
					"pre-open lifecycle did not adopt the returned handle exactly once");
		}
		require(probe.calls == 1 && probe.pins_alive_during_close && destroyed == 4,
				"pre-open result was not closed once with all pins retained");
	}

	void verify_move_and_destructor_close_exactly_once()
	{
		int first_destroyed{};
		int second_destroyed{};
		observed_pins first_pins;
		observed_pins second_pins;
		close_probe first{0, 0, &first_destroyed, false};
		close_probe second{0, 0, &second_destroyed, false};
		{
			sqlite_connection_lifecycle source{
				&first, &probed_close, make_pins(first_destroyed, first_pins)};
			sqlite_connection_lifecycle destination{
				&second, &probed_close, make_pins(second_destroyed, second_pins)};
			sqlite_connection_lifecycle transferred{std::move(source)};
			require(transferred.get() == &first,
					"move construction did not transfer the source connection");
			destination = std::move(transferred);
			require(second.calls == 1 && second.pins_alive_during_close && second_destroyed == 4,
					"move assignment did not close its displaced connection exactly once");
			require(first.calls == 0 && destination.get() == &first,
					"move assignment did not transfer the source connection");
		}
		require(first.calls == 1 && first.pins_alive_during_close && first_destroyed == 4,
				"destination destructor did not close the transferred connection once");
		require(second.calls == 1, "moved-from destructor retried the displaced connection");
		require(first_pins.runtime.expired() && first_pins.vfs.expired() &&
					first_pins.observation.expired() && first_pins.authority_anchor.expired() &&
					second_pins.runtime.expired() && second_pins.vfs.expired() &&
					second_pins.observation.expired() && second_pins.authority_anchor.expired(),
				"successful destructor cleanup retained lifetime pins");
	}

	void verify_non_ok_quarantines_without_retry_or_pin_release()
	{
		int destroyed{};
		observed_pins pins;
		close_probe probe{0, 5, &destroyed, false};
		{
			sqlite_connection_lifecycle owner{&probe, &probed_close, make_pins(destroyed, pins)};
			auto outcome = owner.close_exactly_once();
			require(std::holds_alternative<sqlite_quarantined_connection>(outcome),
					"non-OK close did not quarantine the connection");
			auto& stored = std::get<sqlite_quarantined_connection>(outcome);
			require(stored.valid() &&
						stored.reason() == sqlite_connection_quarantine_reason::close_non_ok &&
						stored.sqlite_code() == 5,
					"non-OK quarantine receipt lost its exact result");
			auto moved = std::move(stored);
			require(moved.valid() && moved.sqlite_code() == 5,
					"quarantine receipt did not transfer move-only proof state");
			require(probe.calls == 1 && probe.pins_alive_during_close,
					"non-OK close did not run once with pins retained");
			require(!pins.runtime.expired() && !pins.vfs.expired() && !pins.observation.expired() &&
						!pins.authority_anchor.expired() && destroyed == 0,
					"quarantine released one or more lifetime pins");
			(void)owner.close_exactly_once();
			require(probe.calls == 1, "quarantined owner retried close_v2");
		}
		require(probe.calls == 1 && !pins.runtime.expired() && !pins.vfs.expired() &&
					!pins.observation.expired() && !pins.authority_anchor.expired() &&
					destroyed == 0,
				"destruction released or retried a quarantined connection");
	}

	void verify_missing_callback_quarantines_without_pin_release()
	{
		int destroyed{};
		observed_pins pins;
		int connection_sentinel{};
		{
			sqlite_connection_lifecycle owner{
				&connection_sentinel, nullptr, make_pins(destroyed, pins)};
			auto outcome = owner.close_exactly_once();
			require(std::holds_alternative<sqlite_quarantined_connection>(outcome),
					"missing close callback did not quarantine the connection");
			const auto& stored = std::get<sqlite_quarantined_connection>(outcome);
			require(stored.valid() &&
						stored.reason() ==
							sqlite_connection_quarantine_reason::close_callback_missing &&
						!stored.sqlite_code().has_value(),
					"missing close callback quarantine receipt is not exact");
			require(!pins.runtime.expired() && !pins.vfs.expired() && !pins.observation.expired() &&
						!pins.authority_anchor.expired() && destroyed == 0,
					"missing close callback quarantine released lifetime pins");
		}
		require(!pins.runtime.expired() && !pins.vfs.expired() && !pins.observation.expired() &&
					!pins.authority_anchor.expired() && destroyed == 0,
				"missing close callback quarantine did not survive owner destruction");
	}

	void verify_noexcept_destructor_quarantines_callback_exception()
	{
		int destroyed{};
		observed_pins pins;
		close_probe probe{0, 0, &destroyed, false};
		{
			sqlite_connection_lifecycle owner{&probe, &throwing_close, make_pins(destroyed, pins)};
		}
		require(probe.calls == 1 && probe.pins_alive_during_close,
				"throwing close callback was not attempted exactly once");
		require(!pins.runtime.expired() && !pins.vfs.expired() && !pins.observation.expired() &&
					!pins.authority_anchor.expired() && destroyed == 0,
				"noexcept cleanup failed to quarantine all pins after an exception");
	}

	void verify_destructor_quarantines_non_ok_without_retry()
	{
		int destroyed{};
		observed_pins pins;
		close_probe probe{0, 17, &destroyed, false};
		{
			sqlite_connection_lifecycle owner{&probe, &probed_close, make_pins(destroyed, pins)};
		}
		require(probe.calls == 1 && probe.pins_alive_during_close,
				"destructor did not attempt a non-OK close exactly once");
		require(!pins.runtime.expired() && !pins.vfs.expired() && !pins.observation.expired() &&
					!pins.authority_anchor.expired() && destroyed == 0,
				"destructor released quarantined non-OK pins");
	}
} // namespace

int main()
{
	try
	{
		verify_null_handle_closes_zero_times();
		verify_preopen_slot_owns_non_null_result_without_allocation();
		verify_explicit_success_closes_once_and_releases_pins();
		verify_move_and_destructor_close_exactly_once();
		verify_non_ok_quarantines_without_retry_or_pin_release();
		verify_missing_callback_quarantines_without_pin_release();
		verify_noexcept_destructor_quarantines_callback_exception();
		verify_destructor_quarantines_non_ok_without_retry();
	}
	catch (const std::exception& exception)
	{
		std::cerr << exception.what() << '\n';
		return 1;
	}
	return 0;
}
