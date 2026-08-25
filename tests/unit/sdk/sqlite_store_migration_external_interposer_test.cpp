// Integration coverage for SQLite migration failures which are only observable
// at the real filesystem boundary.  The fault library is loaded with
// LD_PRELOAD in a fresh child; the parent never shares its controller state or
// Store instance with the child.

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <dlfcn.h>

#if defined(__linux__)
#include <csignal>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#error "The external SQLite migration interposer test requires POSIX process and preload support"
#endif

#include <cxxlens/sdk.hpp>

#include "../../support/sqlite_store_fixture.hpp"
#include "../../support/sqlite_store_v3_scenario.hpp"

namespace
{
	using namespace cxxlens::sdk;
	using cxxlens::test::sqlite_v3_scenario::exact_v2_scenario;
	using cxxlens::test::sqlite_v3_scenario::make_engine;
	using cxxlens::test::sqlite_v3_scenario::selector;
	using sqlite_row = cxxlens::test::sqlite_fixture::row;

	using arm_function = int (*)(const char*, const char*);
	using status_function = int (*)();
	using count_function = unsigned long long (*)();

	constexpr std::string_view arm_symbol = "cxxlens_sqlite_migration_interposer_arm";
	constexpr std::string_view fired_symbol = "cxxlens_sqlite_migration_interposer_fired";
	constexpr std::string_view commit_delegate_symbol =
		"cxxlens_sqlite_migration_interposer_commit_delegate_seen";
	constexpr std::string_view ddl_count_symbol =
		"cxxlens_sqlite_migration_interposer_migration_ddl_calls";
	constexpr std::string_view ddl_delegate_count_symbol =
		"cxxlens_sqlite_migration_interposer_migration_ddl_delegate_calls";
	constexpr std::string_view commit_count_symbol =
		"cxxlens_sqlite_migration_interposer_commit_calls";
	constexpr std::string_view commit_delegate_count_symbol =
		"cxxlens_sqlite_migration_interposer_commit_delegate_calls";
	constexpr std::string_view close_count_symbol =
		"cxxlens_sqlite_migration_interposer_close_calls";
	constexpr std::string_view close_delegate_count_symbol =
		"cxxlens_sqlite_migration_interposer_close_delegate_calls";
	constexpr std::string_view close_delegate_ok_count_symbol =
		"cxxlens_sqlite_migration_interposer_close_delegate_ok_calls";

	[[noreturn]] void fail(std::string message)
	{
		throw std::runtime_error{std::move(message)};
	}

	void require(const bool condition, std::string message)
	{
		if (!condition)
			fail(std::move(message));
	}

	[[nodiscard]] std::string tuple(const error& value)
	{
		return value.code + "/" + value.field + "/" + value.detail;
	}

	struct exact_file_state
	{
		std::uintmax_t device{};
		std::uintmax_t inode{};
		std::uintmax_t byte_count{};
		std::uintmax_t mode{};
		std::vector<unsigned char> bytes;

		[[nodiscard]] bool operator==(const exact_file_state&) const = default;
	};

	struct exact_family_state
	{
		std::vector<std::string> directory_entries;
		std::map<std::string, exact_file_state, std::less<>> files;

		[[nodiscard]] bool operator==(const exact_family_state&) const = default;
	};

	[[nodiscard]] exact_family_state capture_exact_family(const std::filesystem::path& main_path)
	{
		const auto captured = cxxlens::test::sqlite_fixture::capture_files(main_path);
		exact_family_state output;
		output.directory_entries = captured.directory_entries;
		for (const auto& [leaf, bytes] : captured.bytes_by_name)
		{
			const auto path = main_path.parent_path() / leaf;
			struct stat metadata{};
			if (::lstat(path.c_str(), &metadata) != 0)
				throw std::system_error{
					errno, std::generic_category(), "lstat migration Store file"};
			require(S_ISREG(metadata.st_mode),
					"migration Store family contains a non-regular file");
			require(metadata.st_size >= 0 &&
						static_cast<std::uintmax_t>(metadata.st_size) == bytes.size(),
					"migration Store file size disagrees with its captured bytes");
			output.files.emplace(leaf,
								 exact_file_state{static_cast<std::uintmax_t>(metadata.st_dev),
												  static_cast<std::uintmax_t>(metadata.st_ino),
												  static_cast<std::uintmax_t>(metadata.st_size),
												  static_cast<std::uintmax_t>(metadata.st_mode),
												  bytes});
		}
		return output;
	}

	[[nodiscard]] std::string family_leaf(const std::filesystem::path& path,
										  const std::string_view suffix = {})
	{
		return path.filename().string() + std::string{suffix};
	}

	void require_regular_database_family(const exact_family_state& state,
										 const std::filesystem::path& path)
	{
		const auto main = family_leaf(path);
		const auto wal = family_leaf(path, "-wal");
		const auto shm = family_leaf(path, "-shm");
		const auto journal = family_leaf(path, "-journal");
		require(
			state.files.size() == 1U && state.files.contains(main) &&
				state.files.at(main).byte_count > 0U && !state.files.contains(journal) &&
				std::ranges::find(state.directory_entries, main) != state.directory_entries.end() &&
				std::ranges::find(state.directory_entries, wal) == state.directory_entries.end() &&
				std::ranges::find(state.directory_entries, shm) == state.directory_entries.end() &&
				std::ranges::find(state.directory_entries, journal) ==
					state.directory_entries.end(),
			"migration Store family is not an exact quiescent main database");
	}

	struct raw_store_census
	{
		std::vector<sqlite_row> publications;
		std::vector<sqlite_row> chunks;
		std::vector<sqlite_row> heads;
		std::vector<sqlite_row> metadata;
		std::vector<sqlite_row> schema;

		[[nodiscard]] bool operator==(const raw_store_census&) const = default;
	};

	[[nodiscard]] raw_store_census capture_raw_store_census(const std::filesystem::path& path)
	{
		cxxlens::test::sqlite_fixture::connection database{path, true};
		auto metadata = database.query("SELECT key,value FROM cxxlens_ng_metadata ORDER BY key");
		bool current_v3{};
		for (const auto& row : metadata)
			if (row.size() == 2U &&
				cxxlens::test::sqlite_fixture::text(row[0U]) == "physical_format")
				current_v3 = cxxlens::test::sqlite_fixture::text(row[1U]) ==
					"cxxlens.sqlite-semantic-store.v3";
		auto publications = current_v3
			? database.query(
				  "SELECT publication_id,series_id,snapshot_id,sequence,generation,parent,state,"
				  "payload_checksum,payload_byte_count,payload_chunk_count "
				  "FROM cxxlens_ng_publication ORDER BY publication_id")
			: database.query(
				  "SELECT publication_id,series_id,snapshot_id,sequence,generation,parent,state,"
				  "checksum,payload FROM cxxlens_ng_publication ORDER BY publication_id");
		auto chunks = current_v3
			? database.query(
				  "SELECT publication_id,generation,chunk_ordinal,byte_offset,byte_count,checksum,"
				  "payload FROM cxxlens_ng_payload_chunk "
				  "ORDER BY publication_id,generation,chunk_ordinal")
			: std::vector<sqlite_row>{};
		return {
			std::move(publications),
			std::move(chunks),
			database.query(
				"SELECT series_id,current_publication,sequence FROM cxxlens_ng_series_head "
				"ORDER BY series_id"),
			std::move(metadata),
			database.query(
				"SELECT type,name,tbl_name,rootpage,sql FROM sqlite_schema ORDER BY type,name")};
	}

	void require_complete_census(const raw_store_census& census,
								 const exact_v2_scenario& expected,
								 const bool current_v3)
	{
		using cxxlens::test::sqlite_fixture::integer;
		using cxxlens::test::sqlite_fixture::text;
		bool found_prior{};
		bool found_current{};
		bool found_noncommitted{};
		for (const auto& row : census.publications)
		{
			require(row.size() == (current_v3 ? 10U : 9U),
					"migration Store census publication width drifted");
			found_prior = found_prior || text(row[0U]) == expected.prior.publication_id;
			found_current = found_current || text(row[0U]) == expected.current.publication_id;
			found_noncommitted = found_noncommitted || integer(row[6U]) != 3;
		}
		const auto current_head = std::ranges::find_if(
			census.heads,
			[&](const auto& row)
			{
				return row.size() == 3U && text(row[1U]) == expected.current.publication_id;
			});
		require(found_prior && found_current && found_noncommitted &&
					current_head != census.heads.end() && !census.metadata.empty() &&
					!census.schema.empty() &&
					(current_v3 ? !census.chunks.empty() : census.chunks.empty()),
				"migration Store census lacks prior/current/diagnostic/head/schema authority");
	}

	struct interposer_api
	{
		arm_function arm{};
		status_function fired{};
		status_function commit_delegate_seen{};
		count_function ddl_calls{};
		count_function ddl_delegate_calls{};
		count_function commit_calls{};
		count_function commit_delegate_calls{};
		count_function close_calls{};
		count_function close_delegate_calls{};
		count_function close_delegate_ok_calls{};
	};

	[[nodiscard]] interposer_api load_interposer_api()
	{
		const auto resolve = [](const std::string_view symbol) -> void*
		{
			return ::dlsym(RTLD_DEFAULT, std::string{symbol}.c_str());
		};
		return {reinterpret_cast<arm_function>(resolve(arm_symbol)),
				reinterpret_cast<status_function>(resolve(fired_symbol)),
				reinterpret_cast<status_function>(resolve(commit_delegate_symbol)),
				reinterpret_cast<count_function>(resolve(ddl_count_symbol)),
				reinterpret_cast<count_function>(resolve(ddl_delegate_count_symbol)),
				reinterpret_cast<count_function>(resolve(commit_count_symbol)),
				reinterpret_cast<count_function>(resolve(commit_delegate_count_symbol)),
				reinterpret_cast<count_function>(resolve(close_count_symbol)),
				reinterpret_cast<count_function>(resolve(close_delegate_count_symbol)),
				reinterpret_cast<count_function>(resolve(close_delegate_ok_count_symbol))};
	}

	void require_interposer_api(const interposer_api& api)
	{
		require(api.arm != nullptr && api.fired != nullptr && api.commit_delegate_seen != nullptr &&
					api.ddl_calls != nullptr && api.ddl_delegate_calls != nullptr &&
					api.commit_calls != nullptr && api.commit_delegate_calls != nullptr &&
					api.close_calls != nullptr && api.close_delegate_calls != nullptr &&
					api.close_delegate_ok_calls != nullptr,
				"LD_PRELOAD migration interposer API is incomplete");
	}

	[[nodiscard]] std::string format_tuple(const store_compatibility& value)
	{
		return value.backend + "/" + std::to_string(value.readable_format.major) + "." +
			std::to_string(value.readable_format.minor) + "." +
			std::to_string(value.readable_format.patch) + "/" +
			(value.direct_open ? "direct" : "poisoned") + "/" +
			(value.migration_required ? "migration-required" : "ready");
	}

	void require_v2_compatibility(const store_compatibility& value)
	{
		require(value.backend == "sqlite" && value.readable_format.major == 2U &&
					value.readable_format.minor == 6U && value.readable_format.patch == 0U &&
					value.direct_open && value.migration_required,
				"unexpected v2 compatibility tuple: " + format_tuple(value));
	}

	void require_v3_compatibility(const store_compatibility& value)
	{
		require(value.backend == "sqlite" && value.readable_format.major == 3U &&
					value.readable_format.minor == 0U && value.readable_format.patch == 0U &&
					value.direct_open && !value.migration_required,
				"unexpected v3 compatibility tuple: " + format_tuple(value));
	}

	void require_current(snapshot_store& store,
						 const relation_engine& engine,
						 const std::string_view expected_publication,
						 const std::string_view expected_snapshot = {})
	{
		auto current = store.current(selector(engine));
		require(current.has_value(),
				"current failed: " + (current ? std::string{} : tuple(current.error())));
		require(current->publication().publication_id == expected_publication,
				"current publication changed from the migration authority");
		if (!expected_snapshot.empty())
			require(current->id() == expected_snapshot,
					"current semantic snapshot changed from the migration authority");
	}

	void require_poisoned_result(snapshot_store& store, const relation_engine& engine)
	{
		const auto compatibility = store.compatibility();
		require(compatibility.backend == "sqlite" && compatibility.readable_format.major == 2U &&
					compatibility.readable_format.minor == 6U &&
					compatibility.readable_format.patch == 0U && !compatibility.direct_open &&
					compatibility.migration_required,
				"close-unknown compatibility did not poison direct operations: " +
					format_tuple(compatibility));
		auto current = store.current(selector(engine));
		require(!current &&
					tuple(current.error()) ==
						"store.backend-unavailable/sqlite-connection/reopen-required",
				"poisoned current returned an unexpected tuple: " +
					(current ? std::string{"success"} : tuple(current.error())));
	}

	struct preexisting_observer
	{
		snapshot_handle handle;
		row_cursor cursor;
		row_view view;
	};

	[[nodiscard]] preexisting_observer
	make_preexisting_observer(snapshot_store& store,
							  const relation_engine& engine,
							  const std::string_view expected_snapshot,
							  const std::string_view expected_publication)
	{
		auto handle_result = store.current(selector(engine));
		require(handle_result.has_value(), "close-unknown preexisting handle unavailable");
		auto handle = std::move(*handle_result);
		require(handle.id() == expected_snapshot &&
					handle.publication().publication_id == expected_publication,
				"close-unknown preexisting handle changed before migration");
		auto relation = engine.require("company.test.sqlite_v3_item", 1U);
		require(relation.has_value(), "close-unknown relation unavailable");
		auto cursor_result = handle.open(*relation);
		require(cursor_result.has_value(), "close-unknown preexisting cursor unavailable");
		auto cursor = std::move(*cursor_result);
		auto view_result = cursor.next();
		require(view_result.has_value() && view_result->has_value(),
				"close-unknown preexisting row view unavailable");
		auto view = **view_result;
		require(view.copy().has_value(), "close-unknown preexisting row view could not be copied");
		return {std::move(handle), std::move(cursor), std::move(view)};
	}

	void require_preexisting_observer(preexisting_observer& observer,
									  const std::string_view expected_snapshot,
									  const std::string_view expected_publication)
	{
		require(observer.handle.id() == expected_snapshot &&
					observer.handle.publication().publication_id == expected_publication,
				"close-unknown preexisting handle was invalidated");
		require(observer.view.copy().has_value(),
				"close-unknown preexisting cursor view was invalidated");
		auto next = observer.cursor.next();
		require(next.has_value() && next->has_value() && (**next).copy().has_value(),
				"close-unknown preexisting cursor could not advance to its next retained row");
		auto end = observer.cursor.next();
		require(end.has_value() && !end->has_value(),
				"close-unknown preexisting cursor did not retain its exact terminal state");
	}

	int child_main(const std::string_view mode,
				   const std::filesystem::path& path,
				   const std::string_view expected_snapshot,
				   const std::string_view expected_publication,
				   const std::string_view expected_export)
	{
		const auto api = load_interposer_api();
		require_interposer_api(api);
		require(api.arm(path.c_str(), "observe-only") == 0, "interposer prepare arm failed");

		const auto engine = make_engine();
		auto store = open_sqlite_snapshot_store(path.string(), engine);
		require(store.has_value(),
				"interposer child could not open exact v2 source: " +
					(store ? std::string{} : tuple(store.error())));
		require_v2_compatibility(store->compatibility());
		require(api.arm(path.c_str(), std::string{mode}.c_str()) == 0,
				"interposer fault arm failed");

		std::optional<preexisting_observer> observer;
		if (mode == "close-after-commit")
			observer.emplace(
				make_preexisting_observer(*store, engine, expected_snapshot, expected_publication));

		const auto migrated = store->compact();
		require(api.fired() == 1, "interposer did not fire in the armed child");

		if (mode == "pre-effect-first-migration-ddl")
		{
			require(api.commit_delegate_seen() == 0 && api.ddl_calls() == 1U &&
						api.ddl_delegate_calls() == 0U && api.commit_calls() == 0U &&
						api.commit_delegate_calls() == 0U && api.close_calls() == 1U &&
						api.close_delegate_calls() == 1U && api.close_delegate_ok_calls() == 1U,
					"pre-effect boundary counts drifted: ddl=" + std::to_string(api.ddl_calls()) +
						'/' + std::to_string(api.ddl_delegate_calls()) +
						" commit=" + std::to_string(api.commit_calls()) + '/' +
						std::to_string(api.commit_delegate_calls()) +
						" close=" + std::to_string(api.close_calls()) + '/' +
						std::to_string(api.close_delegate_calls()) + '/' +
						std::to_string(api.close_delegate_ok_calls()));
			require(!migrated &&
						tuple(migrated.error()) ==
							"store.sqlite-failure/database/database or disk is full",
					"pre-effect failure returned an unexpected tuple: " +
						(migrated ? std::string{"success"} : tuple(migrated.error())));
			require_v2_compatibility(store->compatibility());
			require_current(*store, engine, expected_publication, expected_snapshot);
			auto exported = store->canonical_export(expected_snapshot);
			require(exported.has_value() && *exported == expected_export,
					"pre-effect same-process export changed from the exact source authority");
			return 0;
		}

		if (mode == "commit-after-delegate")
		{
			require(api.commit_delegate_seen() == 1 && api.ddl_calls() > 0U &&
						api.ddl_calls() == api.ddl_delegate_calls() && api.commit_calls() == 1U &&
						api.commit_delegate_calls() == 1U && api.close_calls() == 1U &&
						api.close_delegate_calls() == 1U && api.close_delegate_ok_calls() == 1U,
					"commit outcome was not observed after the real COMMIT delegate");
			require(migrated.has_value(),
					"after-delegate commit uncertainty was not idempotently recovered: " +
						(migrated ? std::string{} : tuple(migrated.error())));
			require_v3_compatibility(store->compatibility());
			require_current(*store, engine, expected_publication, expected_snapshot);
			return 0;
		}

		require(mode == "close-after-commit", "unknown migration interposer mode");
		require(api.commit_delegate_seen() == 1 && api.ddl_calls() > 0U &&
					api.ddl_calls() == api.ddl_delegate_calls() && api.commit_calls() == 1U &&
					api.commit_delegate_calls() == 1U && api.close_calls() == 1U &&
					api.close_delegate_calls() == 1U && api.close_delegate_ok_calls() == 1U,
				"close-unknown injection did not observe the committed migration COMMIT");
		require(!migrated &&
					tuple(migrated.error()) == "store.sqlite-failure/migration-recovery/opaque",
				"close-unknown migration returned an unexpected tuple: " +
					(migrated ? std::string{"success"} : tuple(migrated.error())));
		require_poisoned_result(*store, engine);
		require(observer.has_value(), "close-unknown observer was not retained");
		require_preexisting_observer(*observer, expected_snapshot, expected_publication);
		return 0;
	}

	class owned_child
	{
	  public:
		explicit owned_child(const pid_t value) noexcept : value_{value} {}
		owned_child(const owned_child&) = delete;
		owned_child& operator=(const owned_child&) = delete;
		~owned_child()
		{
			terminate_and_reap();
		}

		[[nodiscard]] pid_t get() const noexcept
		{
			return value_;
		}

		void release() noexcept
		{
			value_ = -1;
		}

		void terminate_and_reap() noexcept
		{
			if (value_ <= 0)
				return;
			(void)::kill(value_, SIGKILL);
			int status{};
			while (::waitpid(value_, &status, 0) < 0 && errno == EINTR)
			{
			}
			value_ = -1;
		}

	  private:
		pid_t value_{-1};
	};

	void run_child(const std::filesystem::path& executable,
				   const std::filesystem::path& interposer,
				   const std::string_view mode,
				   const std::filesystem::path& path,
				   const std::string_view expected_snapshot,
				   const std::string_view expected_publication,
				   const std::string_view expected_export)
	{
		const auto process = ::fork();
		if (process < 0)
			throw std::system_error{errno, std::generic_category(), "fork migration outcome child"};
		if (process == 0)
		{
			if (::setenv("LD_PRELOAD", interposer.c_str(), 1) != 0)
				::_exit(125);
			const auto result = ::execl(executable.c_str(),
										executable.c_str(),
										"--child",
										std::string{mode}.c_str(),
										path.c_str(),
										std::string{expected_snapshot}.c_str(),
										std::string{expected_publication}.c_str(),
										std::string{expected_export}.c_str(),
										nullptr);
			(void)result;
			::_exit(126);
		}

		owned_child child{process};
		int status{};
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{45};
		for (;;)
		{
			const auto waited = ::waitpid(child.get(), &status, WNOHANG);
			if (waited < 0 && errno == EINTR)
				continue;
			if (waited < 0)
			{
				if (errno == ECHILD)
					child.release();
				throw std::system_error{
					errno, std::generic_category(), "wait migration outcome child"};
			}
			if (waited == child.get())
			{
				child.release();
				break;
			}
			if (std::chrono::steady_clock::now() >= deadline)
			{
				child.terminate_and_reap();
				fail("migration interposer child exceeded bounded runtime");
			}
			std::this_thread::sleep_for(std::chrono::milliseconds{20});
		}
		require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
				"migration interposer child failed for mode " + std::string{mode});
	}

	void check_parent_reopen(const std::filesystem::path& path,
							 const relation_engine& engine,
							 const std::string_view expected_snapshot,
							 const std::string_view expected_publication,
							 const std::string_view expected_export)
	{
		auto reopened = open_sqlite_snapshot_store(path.string(), engine);
		require(reopened.has_value(),
				"parent could not reopen the post-child SQLite source: " +
					(reopened ? std::string{} : tuple(reopened.error())));
		require_v3_compatibility(reopened->compatibility());
		require_current(*reopened, engine, expected_publication, expected_snapshot);
		auto exported = reopened->canonical_export(expected_snapshot);
		require(exported && *exported == expected_export,
				"parent reopen changed the canonical semantic authority");
	}

	void check_mode(const std::filesystem::path& executable,
					const std::filesystem::path& interposer,
					const std::filesystem::path& path,
					const exact_v2_scenario& expected,
					const raw_store_census& expected_v3_census,
					const std::string_view mode,
					const relation_engine& engine)
	{
		const auto before_census = capture_raw_store_census(path);
		require_complete_census(before_census, expected, false);
		cxxlens::test::sqlite_fixture::quiesce_wal_sidecars(path);
		const auto before_family = capture_exact_family(path);
		require_regular_database_family(before_family, path);
		run_child(executable,
				  interposer,
				  mode,
				  path,
				  expected.current.snapshot_id,
				  expected.current.publication_id,
				  expected.current_canonical_export);
		if (mode != "pre-effect-first-migration-ddl")
		{
			check_parent_reopen(path,
								engine,
								expected.current.snapshot_id,
								expected.current.publication_id,
								expected.current_canonical_export);
			const auto after_family = capture_exact_family(path);
			require_regular_database_family(after_family, path);
			check_parent_reopen(path,
								engine,
								expected.current.snapshot_id,
								expected.current.publication_id,
								expected.current_canonical_export);
			require(capture_exact_family(path) == after_family,
					"second direct reopen changed post-outcome source identity, size, bytes, or "
					"directory entries");
			const auto after_census = capture_raw_store_census(path);
			require_complete_census(after_census, expected, true);
			require(after_census == expected_v3_census,
					"post-outcome Store rows, chunks, heads, metadata, or schema differ from the "
					"normal migration candidate");
		}
		else
		{
			require(capture_exact_family(path) == before_family,
					"pre-effect failure changed source file identity, size, bytes, or directory "
					"entries");
			{
				auto reopened = open_sqlite_snapshot_store(path.string(), engine);
				require(reopened.has_value(), "pre-effect source could not reopen");
				require_v2_compatibility(reopened->compatibility());
				require_current(*reopened,
								engine,
								expected.current.publication_id,
								expected.current.snapshot_id);
				auto exported = reopened->canonical_export(expected.current.snapshot_id);
				require(exported && *exported == expected.current_canonical_export,
						"pre-effect source changed the canonical semantic authority");
			}
			require(capture_exact_family(path) == before_family,
					"pre-effect direct reopen changed the exact source family");
			require(capture_raw_store_census(path) == before_census,
					"pre-effect failure changed publication, head, metadata, or schema authority");
		}
	}
} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc >= 2 && std::string_view{argv[1]} == "--child")
		{
			require(argc == 7, "migration interposer child argument count drifted");
			return child_main(argv[2], argv[3], argv[4], argv[5], argv[6]);
		}
		require(argc == 2,
				"usage: sqlite_store_migration_external_interposer_test <interposer.so>");

		const auto executable = std::filesystem::absolute(argv[0]);
		const auto interposer = std::filesystem::absolute(argv[1]);
		require(std::filesystem::is_regular_file(interposer),
				"migration interposer shared object is missing: " + interposer.string());
		const auto engine = make_engine();
		cxxlens::test::sqlite_fixture::temporary_directory directory{
			"sqlite-migration-external-interposer"};
		cxxlens::test::sqlite_fixture::temporary_directory baseline_directory{
			"sqlite-migration-external-baseline"};
		const auto baseline_path = baseline_directory.path() / "normal.sqlite";
		const auto baseline_expected =
			cxxlens::test::sqlite_v3_scenario::create_exact_v2_scenario(baseline_path, engine);
		{
			auto baseline = open_sqlite_snapshot_store(baseline_path.string(), engine);
			require(baseline.has_value(),
					"normal migration baseline could not open exact v2 source");
			auto migrated = baseline->compact();
			require(migrated.has_value(),
					"normal migration baseline failed: " +
						(migrated ? std::string{} : tuple(migrated.error())));
			require_v3_compatibility(baseline->compatibility());
			require_current(*baseline,
							engine,
							baseline_expected.current.publication_id,
							baseline_expected.current.snapshot_id);
		}
		const auto expected_v3_census = capture_raw_store_census(baseline_path);
		require_complete_census(expected_v3_census, baseline_expected, true);

		for (const auto mode : {std::string_view{"pre-effect-first-migration-ddl"},
								std::string_view{"commit-after-delegate"},
								std::string_view{"close-after-commit"}})
		{
			const auto path = directory.path() / (std::string{mode} + ".sqlite");
			const auto expected =
				cxxlens::test::sqlite_v3_scenario::create_exact_v2_scenario(path, engine);
			require(expected.prior == baseline_expected.prior &&
						expected.current == baseline_expected.current &&
						expected.current_canonical_export ==
							baseline_expected.current_canonical_export,
					"migration test inputs are not semantically deterministic");
			check_mode(executable, interposer, path, expected, expected_v3_census, mode, engine);
		}
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
