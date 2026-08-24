#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string_view>
#include <type_traits>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif

#include "sdk/store_operation_port_internal.hpp"
#include "store_operation_test_adapter.hpp"

namespace
{
	using namespace cxxlens::sdk;

	static_assert(std::is_trivially_copyable_v<store_write_outcome>);
	static_assert(std::is_trivially_copyable_v<store_sync_outcome>);
	static_assert(std::is_trivially_copyable_v<store_close_outcome>);
	static_assert(std::is_trivially_copyable_v<store_commit_outcome>);
	static_assert(std::is_trivially_copyable_v<store_sqlite_operation_binding>);
	static_assert(std::has_virtual_destructor_v<store_operation_port>);

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	struct sqlite_callback_state
	{
		int result{};
		std::size_t call_count{};
		bool throws{};
	};

	int invoke_sqlite_operation(void* const context)
	{
		auto& state = *static_cast<sqlite_callback_state*>(context);
		++state.call_count;
		if (state.throws)
			throw 1;
		return state.result;
	}

	class recording_store_operation_port final : public store_operation_port
	{
	  public:
		store_write_outcome write_exact(const int,
										const std::span<const std::byte> bytes) noexcept override
		{
			++write_calls;
			return {store_write_state::complete, bytes.size(), 0, !bytes.empty(), !bytes.empty()};
		}

		store_sync_outcome synchronize(const int, const store_sync_target target) noexcept override
		{
			++sync_calls;
			return {target, store_sync_state::durable, 0, true};
		}

		store_close_outcome close_descriptor(const int) noexcept override
		{
			++descriptor_close_calls;
			return {store_close_state::confirmed_closed, 0, true};
		}

		store_close_outcome close_sqlite(store_sqlite_operation_binding) noexcept override
		{
			++sqlite_close_calls;
			return {store_close_state::confirmed_closed, 0, true};
		}

		store_commit_outcome commit_sqlite(store_sqlite_operation_binding) noexcept override
		{
			++commit_calls;
			return {store_commit_state::committed, 0, true, true};
		}

		std::size_t write_calls{};
		std::size_t sync_calls{};
		std::size_t descriptor_close_calls{};
		std::size_t sqlite_close_calls{};
		std::size_t commit_calls{};
	};

	void check_default_filesystem_adapter()
	{
#if defined(__unix__) || defined(__APPLE__)
		default_store_operation_port port;
		std::array path{'/', 't', 'm', 'p', '/', 'c', 'x', 'x', 'l', 'e', 'n', 's', '-', 's', 't',
						'o', 'r', 'e', '-', 'o', 'p', '-', 'X', 'X', 'X', 'X', 'X', 'X', '\0'};
		const auto descriptor = ::mkstemp(path.data());
		require(descriptor >= 0, "could not create Store operation test file");

		constexpr std::array payload{std::byte{0x43},
									 std::byte{0x58},
									 std::byte{0x58},
									 std::byte{0x4c},
									 std::byte{0x45},
									 std::byte{0x4e},
									 std::byte{0x53}};
		const auto write = port.write_exact(descriptor, payload);
		require(write.completed(payload.size()) && write.operation_attempted &&
					write.effect_may_have_occurred,
				"default adapter did not report its complete exact write");
		const auto empty_write = port.write_exact(descriptor, std::span<const std::byte>{});
		require(empty_write.completed(0U) && !empty_write.operation_attempted &&
					!empty_write.effect_may_have_occurred,
				"empty write was reported as an attempted filesystem effect");

		const auto data_sync = port.synchronize(descriptor, store_sync_target::file_data);
		const auto file_sync = port.synchronize(descriptor, store_sync_target::file);
		require(data_sync.durable() && file_sync.durable(),
				"default adapter did not report file durability");
		require(port.close_descriptor(descriptor).confirmed(),
				"default adapter did not confirm descriptor close");

		const auto read_descriptor = ::open(path.data(), O_RDONLY | O_CLOEXEC);
		require(read_descriptor >= 0, "could not reopen Store operation test file");
		std::array<std::byte, payload.size()> observed{};
		std::size_t observed_count{};
		while (observed_count < observed.size())
		{
			const auto read_count = ::read(read_descriptor,
										   observed.data() + observed_count,
										   observed.size() - observed_count);
			if (read_count < 0 && errno == EINTR)
				continue;
			require(read_count > 0, "could not read exact-write bytes after reopen");
			observed_count += static_cast<std::size_t>(read_count);
		}
		require(observed == payload, "exact-write bytes did not survive reopen");
		require(port.close_descriptor(read_descriptor).confirmed(),
				"default adapter did not close read descriptor");

		const auto directory_descriptor = ::open("/tmp", O_RDONLY | O_CLOEXEC | O_DIRECTORY);
		require(directory_descriptor >= 0, "could not open Store operation parent directory");
		require(
			port.synchronize(directory_descriptor, store_sync_target::parent_directory).durable(),
			"default adapter did not report parent-directory durability");
		require(port.close_descriptor(directory_descriptor).confirmed(),
				"default adapter did not close parent-directory descriptor");
		require(::unlink(path.data()) == 0, "could not remove Store operation test file");

		const auto invalid_target = static_cast<store_sync_target>(255U);
		const auto rejected = port.synchronize(-1, invalid_target);
		require(rejected.state == store_sync_state::failed && rejected.native_code == EINVAL &&
					!rejected.operation_attempted,
				"invalid sync target reached the filesystem");
		require(port.close_descriptor(-1).state == store_close_state::not_attempted,
				"invalid descriptor was reported as a close attempt");
#endif
	}

	void check_default_sqlite_callback_adapter()
	{
		default_store_operation_port port;
		sqlite_callback_state state{};
		const store_sqlite_operation_binding binding{&state, invoke_sqlite_operation, 0, 13};

		const auto committed = port.commit_sqlite(binding);
		require(committed.confirmed() && committed.operation_attempted &&
					committed.effect_may_have_occurred && state.call_count == 1U,
				"successful SQLite commit was not confirmed exactly once");

		state.result = 13;
		const auto full = port.commit_sqlite(binding);
		require(full.state == store_commit_state::resource_exhausted && full.native_code == 13 &&
					full.operation_attempted && full.effect_may_have_occurred &&
					state.call_count == 2U,
				"SQLite resource exhaustion lost its typed commit outcome");

		state.result = 5;
		const auto ambiguous_commit = port.commit_sqlite(binding);
		require(ambiguous_commit.state == store_commit_state::outcome_unknown &&
					ambiguous_commit.native_code == 5 && ambiguous_commit.operation_attempted &&
					ambiguous_commit.effect_may_have_occurred && state.call_count == 3U,
				"non-success SQLite commit was reduced to an ordinary failure");

		state.result = 0;
		const auto closed = port.close_sqlite(binding);
		require(closed.confirmed() && state.call_count == 4U,
				"successful SQLite close was not confirmed exactly once");
		state.result = 5;
		const auto ambiguous_close = port.close_sqlite(binding);
		require(ambiguous_close.state == store_close_state::outcome_unknown &&
					ambiguous_close.native_code == 5 && ambiguous_close.operation_attempted &&
					state.call_count == 5U,
				"non-success SQLite close was not quarantinable");

		state.throws = true;
		const auto throwing_close = port.close_sqlite(binding);
		const auto throwing_commit = port.commit_sqlite(binding);
		require(throwing_close.state == store_close_state::outcome_unknown &&
					throwing_close.operation_attempted &&
					throwing_commit.state == store_commit_state::outcome_unknown &&
					throwing_commit.operation_attempted &&
					throwing_commit.effect_may_have_occurred && state.call_count == 7U,
				"throwing SQLite operation escaped the typed ambiguity boundary");

		const auto missing_callback = port.commit_sqlite({&state, nullptr, 0, 13});
		const auto invalid_codes = port.close_sqlite({&state, invoke_sqlite_operation, 13, 13});
		require(missing_callback.state == store_commit_state::not_attempted &&
					!missing_callback.operation_attempted &&
					invalid_codes.state == store_close_state::not_attempted &&
					!invalid_codes.operation_attempted && state.call_count == 7U,
				"invalid SQLite binding invoked an operation callback");
	}

	void check_tests_only_adapter_is_one_shot_and_typed()
	{
		using cxxlens::test::store_operation_ambiguity_side;

		recording_store_operation_port delegate;
		cxxlens::test::store_operation_test_adapter port{delegate};
		constexpr std::array payload{std::byte{0x01}, std::byte{0x02}};
		sqlite_callback_state callback_state{};
		const store_sqlite_operation_binding binding{
			&callback_state, invoke_sqlite_operation, 0, 13};

		port.inject_next_write_resource_exhaustion(ENOSPC);
		const auto disk_full = port.write_exact(11, payload);
		require(disk_full.state == store_write_state::resource_exhausted &&
					disk_full.native_code == ENOSPC && disk_full.operation_attempted &&
					!disk_full.effect_may_have_occurred && delegate.write_calls == 0U,
				"disk-full injection reached the production delegate");
		require(port.write_exact(11, payload).completed(payload.size()) &&
					delegate.write_calls == 1U && port.write_call_count() == 2U,
				"disk-full injection was not consumed exactly once");

		port.inject_next_sync_ambiguity(EIO, store_operation_ambiguity_side::before_delegate);
		const auto sync = port.synchronize(11, store_sync_target::parent_directory);
		require(sync.state == store_sync_state::outcome_unknown && sync.native_code == EIO &&
					sync.operation_attempted && delegate.sync_calls == 0U,
				"fsync ambiguity injection reached the production delegate");
		require(port.synchronize(11, store_sync_target::parent_directory).durable() &&
					delegate.sync_calls == 1U && port.sync_call_count() == 2U,
				"fsync ambiguity injection was not consumed exactly once");

		port.inject_next_descriptor_close_ambiguity(
			EINTR, store_operation_ambiguity_side::before_delegate);
		const auto descriptor_close = port.close_descriptor(11);
		require(descriptor_close.state == store_close_state::outcome_unknown &&
					descriptor_close.native_code == EINTR && descriptor_close.operation_attempted &&
					delegate.descriptor_close_calls == 0U,
				"descriptor-close ambiguity injection reached the production delegate");
		require(port.close_descriptor(11).confirmed() && delegate.descriptor_close_calls == 1U &&
					port.descriptor_close_call_count() == 2U,
				"descriptor-close ambiguity injection was not consumed exactly once");

		port.inject_next_sqlite_close_ambiguity(5, store_operation_ambiguity_side::before_delegate);
		const auto sqlite_close = port.close_sqlite(binding);
		require(sqlite_close.state == store_close_state::outcome_unknown &&
					sqlite_close.native_code == 5 && sqlite_close.operation_attempted &&
					delegate.sqlite_close_calls == 0U,
				"SQLite-close ambiguity injection reached the production delegate");
		require(port.close_sqlite(binding).confirmed() && delegate.sqlite_close_calls == 1U &&
					port.sqlite_close_call_count() == 2U,
				"SQLite-close ambiguity injection was not consumed exactly once");

		port.inject_next_commit_ambiguity(5, store_operation_ambiguity_side::before_delegate);
		const auto commit = port.commit_sqlite(binding);
		require(commit.state == store_commit_state::outcome_unknown && commit.native_code == 5 &&
					commit.operation_attempted && commit.effect_may_have_occurred &&
					delegate.commit_calls == 0U,
				"commit ambiguity injection reached the production delegate");
		require(port.commit_sqlite(binding).confirmed() && delegate.commit_calls == 1U &&
					port.commit_call_count() == 2U,
				"commit ambiguity injection was not consumed exactly once");

		port.inject_next_sync_ambiguity(EIO, store_operation_ambiguity_side::after_delegate);
		port.inject_next_descriptor_close_ambiguity(EINTR,
													store_operation_ambiguity_side::after_delegate);
		port.inject_next_sqlite_close_ambiguity(5, store_operation_ambiguity_side::after_delegate);
		port.inject_next_commit_ambiguity(5, store_operation_ambiguity_side::after_delegate);
		require(port.synchronize(11, store_sync_target::file).state ==
						store_sync_state::outcome_unknown &&
					port.close_descriptor(11).state == store_close_state::outcome_unknown &&
					port.close_sqlite(binding).state == store_close_state::outcome_unknown &&
					port.commit_sqlite(binding).state == store_commit_state::outcome_unknown &&
					delegate.sync_calls == 2U && delegate.descriptor_close_calls == 2U &&
					delegate.sqlite_close_calls == 2U && delegate.commit_calls == 2U,
				"after-delegate ambiguity did not preserve the possible effect side");
	}
} // namespace

int main()
{
	check_default_filesystem_adapter();
	check_default_sqlite_callback_adapter();
	check_tests_only_adapter_is_one_shot_and_typed();
	return 0;
}
