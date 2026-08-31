#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <cxxlens/sdk.hpp>

#include "../../../src/sdk/sqlite_dynamic_loader_port_internal.hpp"
#include "../../support/sqlite_store_fixture.hpp"
#include "../../support/sqlite_store_v3_scenario.hpp"

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::test::sqlite_fixture;
	using namespace cxxlens::test::sqlite_v3_scenario;

	[[noreturn]] void fail(std::string message)
	{
		throw std::runtime_error{std::move(message)};
	}

	void require(const bool condition, std::string message)
	{
		if (!condition)
			fail(std::move(message));
	}

	enum class loader_fault
	{
		missing_symbol,
		partial_symbol_set,
		wrong_runtime_tuple,
		loader_failure,
	};

	class deterministic_loader final : public sdk::detail::sqlite_dynamic_loader_port
	{
	  public:
		deterministic_loader(
			std::shared_ptr<const sdk::detail::sqlite_dynamic_loader_port> delegate,
			loader_fault fault)
			: delegate_{std::move(delegate)}, fault_{fault}
		{
		}

		sdk::result<void*> open(const std::string_view candidate) const override
		{
			if (fault_ == loader_fault::loader_failure)
				return sdk::unexpected(
					sdk::error{"store.backend-unavailable", "sqlite", "library"});
			return delegate_->open(candidate);
		}

		void close(void* library) const noexcept override
		{
			delegate_->close(library);
		}

		[[nodiscard]] sdk::result<void*> resolve(void* library,
												 const std::string_view symbol) const override
		{
			if ((fault_ == loader_fault::missing_symbol && symbol == "sqlite3_sourceid") ||
				(fault_ == loader_fault::partial_symbol_set && symbol == "sqlite3_uri_key"))
				return static_cast<void*>(nullptr);
			auto resolved = delegate_->resolve(library, symbol);
			if (resolved && fault_ == loader_fault::wrong_runtime_tuple &&
				symbol == "sqlite3_uri_parameter")
				wrong_tuple_symbol_ = *resolved;
			return resolved;
		}

		[[nodiscard]] const void* image_identity(const void* symbol) const noexcept override
		{
			if (fault_ == loader_fault::wrong_runtime_tuple && symbol == wrong_tuple_symbol_)
				return reinterpret_cast<const void*>(1U);
			return delegate_->image_identity(symbol);
		}

	  private:
		std::shared_ptr<const sdk::detail::sqlite_dynamic_loader_port> delegate_;
		loader_fault fault_;
		mutable void* wrong_tuple_symbol_{};
	};

	void check_fault(const loader_fault fault, const std::string_view label)
	{
		auto engine = make_engine();
		temporary_directory directory{std::string{"sqlite-source-loader-"} + std::string{label}};
		const auto path = directory.path() / "exact-v2.sqlite";
		const auto expected = create_exact_v2_scenario(path, engine);
		active_wal_sidecar_fixture active{path, wal_source_authority::predecessor_v2};
		const auto before = capture_files(path);

		{
			auto baseline = sdk::open_sqlite_snapshot_store(path.string(), engine);
			require(baseline.has_value(), "system loader rejected the active source baseline");
			auto current = baseline->current(selector(engine));
			require(current && current->id() == expected.current.snapshot_id,
					"system loader changed the exact v2 logical authority");
		}

		auto system = sdk::detail::make_system_sqlite_dynamic_loader_port();
		auto injected = std::make_shared<deterministic_loader>(std::move(system), fault);
		auto opened = sdk::detail::open_sqlite_snapshot_store_with_loader_port(
			path.string(), engine, std::move(injected));
		require(!opened, std::string{label} + " was admitted for an active WAL/SHM source");
		if (fault == loader_fault::loader_failure)
			require(opened.error().code == "store.backend-unavailable" &&
						opened.error().field == "sqlite" && opened.error().detail == "library",
					"loader failure lost its typed unavailable tuple");
		else if (fault == loader_fault::wrong_runtime_tuple)
			require(opened.error().code == "store.backend-unavailable" &&
						opened.error().field == "sqlite" &&
						opened.error().detail == "runtime-binding",
					"wrong runtime tuple lost its typed binding rejection");
		else
			require(opened.error().code == "store.backend-unavailable" &&
						opened.error().field == "sqlite" &&
						opened.error().detail == "source-shm-readonly-qualification",
					std::string{label} + " lost the source-SHM fail-closed tuple");
		require(capture_files(path) == before,
				std::string{label} + " changed source bytes or directory membership");
	}
} // namespace

int main()
{
	try
	{
		check_fault(loader_fault::missing_symbol, "missing-symbol");
		check_fault(loader_fault::partial_symbol_set, "partial-symbol-set");
		check_fault(loader_fault::wrong_runtime_tuple, "wrong-runtime-tuple");
		check_fault(loader_fault::loader_failure, "loader-failure");
		return 0;
	}
	catch (const std::exception& failure)
	{
		std::cerr << failure.what() << '\n';
		return 1;
	}
}
