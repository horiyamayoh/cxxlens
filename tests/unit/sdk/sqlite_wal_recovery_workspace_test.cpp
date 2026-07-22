#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sdk/sqlite_payload_streaming_internal.hpp"
#include "sdk/sqlite_wal_receipt_internal.hpp"
#include "sdk/sqlite_wal_recovery_workspace_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;

	constexpr int sqlite_ok = 0;
	constexpr int sqlite_open_readwrite = 0x00000002;
	constexpr int sqlite_open_create = 0x00000004;
	constexpr int sqlite_open_uri = 0x00000040;
	constexpr int sqlite_open_main_database = 0x00000100;
	constexpr int sqlite_open_write_ahead_log = 0x00080000;

	struct sqlite3;
	using sqlite_exec_callback = int (*)(void*, int, char**, char**);
	using sqlite_find_function = void* (*)(const char*);
	using sqlite_register_function = int (*)(void*, int);
	using sqlite_unregister_function = int (*)(void*);
	using sqlite_open_function = int (*)(const char*, sqlite3**, int, const char*);
	using sqlite_close_function = int (*)(sqlite3*);
	using sqlite_exec_function =
		int (*)(sqlite3*, const char*, sqlite_exec_callback, void*, char**);
	using sqlite_free_function = void (*)(void*);

	sqlite_find_function sqlite_find{};
	sqlite_register_function sqlite_register{};
	sqlite_unregister_function sqlite_unregister{};

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
			throw std::runtime_error{std::string{message}};
	}

	template <class Function>
	[[nodiscard]] Function load_function(void* handle, const char* name)
	{
		static_assert(std::is_pointer_v<Function>);
		auto* raw = ::dlsym(handle, name);
		require(raw != nullptr, name);
		static_assert(sizeof(Function) == sizeof(raw));
		return std::bit_cast<Function>(raw);
	}

	void* find_bridge(const char* name)
	{
		return sqlite_find(name);
	}

	int register_bridge(void* value, const int make_default)
	{
		return sqlite_register(value, make_default);
	}

	int unregister_bridge(void* value)
	{
		return sqlite_unregister(value);
	}

	class sqlite_runtime
	{
	  public:
		sqlite_runtime()
		{
			auto* raw = ::dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
			require(raw != nullptr, "load SQLite runtime");
			lifetime_ = std::shared_ptr<void>{raw,
											  [](void* value)
											  {
												  (void)::dlclose(value);
											  }};
			sqlite_find = load_function<sqlite_find_function>(raw, "sqlite3_vfs_find");
			sqlite_register = load_function<sqlite_register_function>(raw, "sqlite3_vfs_register");
			sqlite_unregister =
				load_function<sqlite_unregister_function>(raw, "sqlite3_vfs_unregister");
			open = load_function<sqlite_open_function>(raw, "sqlite3_open_v2");
			close = load_function<sqlite_close_function>(raw, "sqlite3_close");
			exec = load_function<sqlite_exec_function>(raw, "sqlite3_exec");
			free = load_function<sqlite_free_function>(raw, "sqlite3_free");
			runtime_identity_ = raw;
			default_vfs_ = sqlite_find(nullptr);
			require(default_vfs_ != nullptr, "resolve SQLite default VFS exactly once");
		}

		~sqlite_runtime()
		{
			sqlite_find = nullptr;
			sqlite_register = nullptr;
			sqlite_unregister = nullptr;
		}

		sqlite_runtime(const sqlite_runtime&) = delete;
		sqlite_runtime& operator=(const sqlite_runtime&) = delete;

		[[nodiscard]] sqlite_private_snapshot_registry_binding registry() const
		{
			return {runtime_identity_,
					default_vfs_,
					find_bridge,
					register_bridge,
					unregister_bridge,
					lifetime_};
		}

		sqlite_open_function open{};
		sqlite_close_function close{};
		sqlite_exec_function exec{};
		sqlite_free_function free{};

	  private:
		std::shared_ptr<void> lifetime_;
		void* runtime_identity_{};
		void* default_vfs_{};
	};

	class sqlite_connection
	{
	  public:
		explicit sqlite_connection(sqlite_runtime& runtime) noexcept : runtime_{&runtime} {}

		~sqlite_connection()
		{
			close_noexcept();
		}

		sqlite_connection(const sqlite_connection&) = delete;
		sqlite_connection& operator=(const sqlite_connection&) = delete;

		void open(const std::string_view path, const int flags, const std::string_view vfs = {})
		{
			require(database_ == nullptr, "connection already open");
			const std::string owned_path{path};
			const std::string owned_vfs{vfs};
			const auto* vfs_name = owned_vfs.empty() ? nullptr : owned_vfs.c_str();
			const auto status = runtime_->open(owned_path.c_str(), &database_, flags, vfs_name);
			if (status != sqlite_ok)
			{
				if (database_ != nullptr)
				{
					(void)runtime_->close(database_);
					database_ = nullptr;
				}
				throw std::runtime_error{"SQLite open failed: " + std::to_string(status)};
			}
		}

		[[nodiscard]] int execute(const std::string_view sql,
								  sqlite_exec_callback callback = nullptr,
								  void* context = nullptr)
		{
			require(database_ != nullptr, "execute on closed connection");
			const std::string statement{sql};
			char* message{};
			const auto status =
				runtime_->exec(database_, statement.c_str(), callback, context, &message);
			if (message != nullptr)
			{
				last_error_ = message;
				runtime_->free(message);
			}
			else
				last_error_.clear();
			return status;
		}

		[[nodiscard]] const std::string& last_error() const noexcept
		{
			return last_error_;
		}

		void execute_ok(const std::string_view sql)
		{
			require(execute(sql) == sqlite_ok, sql);
		}

		void close()
		{
			require(database_ != nullptr, "connection already closed");
			auto* closing = std::exchange(database_, nullptr);
			require(runtime_->close(closing) == sqlite_ok, "SQLite close");
		}

		void close_noexcept() noexcept
		{
			if (database_ != nullptr)
			{
				auto* closing = std::exchange(database_, nullptr);
				(void)runtime_->close(closing);
			}
		}

	  private:
		sqlite_runtime* runtime_{};
		sqlite3* database_{};
		std::string last_error_;
	};

	class source_fixture
	{
	  public:
		explicit source_fixture(sqlite_runtime& runtime)
			: path_{"/tmp/cxxlens-wal-recovery-source-" + std::to_string(::getpid()) + ".sqlite"},
			  connection_{runtime}
		{
			remove_files();
			connection_.open(path_,
							 sqlite_open_readwrite | sqlite_open_create |
								 sqlite_wal_recovery_open_fullmutex |
								 sqlite_wal_recovery_open_privatecache);
			connection_.execute_ok("PRAGMA page_size=512;");
			connection_.execute_ok("PRAGMA journal_mode=WAL;");
			connection_.execute_ok("PRAGMA wal_autocheckpoint=0;");
			connection_.execute_ok("BEGIN IMMEDIATE; CREATE TABLE item(value INTEGER NOT NULL); "
								   "INSERT INTO item VALUES(7); COMMIT;");
		}

		~source_fixture()
		{
			connection_.close_noexcept();
			remove_files();
		}

		source_fixture(const source_fixture&) = delete;
		source_fixture& operator=(const source_fixture&) = delete;

		[[nodiscard]] const std::string& path() const noexcept
		{
			return path_;
		}

		[[nodiscard]] std::vector<std::byte> read_main() const
		{
			return read_file(path_);
		}

		[[nodiscard]] std::vector<std::byte> read_wal() const
		{
			return read_file(path_ + "-wal");
		}

		[[nodiscard]] static std::vector<std::byte> read_file_for_test(const std::string& path)
		{
			return read_file(path);
		}

		void checkpoint_full()
		{
			connection_.execute_ok("PRAGMA wal_checkpoint(FULL);");
		}

	  private:
		[[nodiscard]] static std::vector<std::byte> read_file(const std::string& path)
		{
			const auto descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
			require(descriptor >= 0, "open SQLite fixture bytes");
			struct stat observed
			{
			};
			if (::fstat(descriptor, &observed) != 0 || observed.st_size < 0)
			{
				(void)::close(descriptor);
				throw std::runtime_error{"stat SQLite fixture bytes"};
			}
			std::vector<std::byte> output(static_cast<std::size_t>(observed.st_size));
			std::size_t consumed{};
			while (consumed < output.size())
			{
				const auto count = ::pread(descriptor,
										   output.data() + consumed,
										   output.size() - consumed,
										   static_cast<off_t>(consumed));
				if (count > 0)
				{
					consumed += static_cast<std::size_t>(count);
					continue;
				}
				if (count < 0 && errno == EINTR)
					continue;
				(void)::close(descriptor);
				throw std::runtime_error{"read SQLite fixture bytes"};
			}
			require(::close(descriptor) == 0, "close SQLite fixture bytes");
			return output;
		}

		void remove_files() const noexcept
		{
			for (const auto suffix :
				 std::array<std::string_view, 4U>{"", "-wal", "-shm", "-journal"})
			{
				const auto target = path_ + std::string{suffix};
				(void)::unlink(target.c_str());
			}
		}

		std::string path_;
		sqlite_connection connection_;
	};

	class vector_cursor final : public sqlite_bounded_byte_source
	{
	  public:
		explicit vector_cursor(const std::span<const std::byte> bytes) noexcept : bytes_{bytes} {}

		result<std::size_t> read(const std::span<std::byte> output) override
		{
			if (offset_ == bytes_.size())
				return 0U;
			const auto count = std::min({output.size(), bytes_.size() - offset_, std::size_t{97U}});
			std::copy_n(
				bytes_.begin() + static_cast<std::ptrdiff_t>(offset_), count, output.begin());
			offset_ += count;
			return count;
		}

	  private:
		std::span<const std::byte> bytes_;
		std::size_t offset_{};
	};

	[[nodiscard]] sqlite_wal_scan_receipt scan_bytes(const std::span<const std::byte> bytes)
	{
		vector_cursor source{bytes};
		auto scanned = scan_sqlite_wal(source);
		require(scanned.has_value(), "scan SQLite WAL bytes");
		return *scanned;
	}

	[[nodiscard]] std::string sha256(const std::span<const std::byte> bytes)
	{
		sqlite_incremental_sha256 digest;
		if (!bytes.empty())
			require(digest.update(bytes).has_value(), "hash fixture bytes");
		auto finished = digest.finish();
		require(finished.has_value(), "finish fixture hash");
		return std::move(*finished);
	}

	[[nodiscard]] sqlite_backend_copy_receipt copy_receipt(const std::span<const std::byte> bytes)
	{
		return {static_cast<std::uint64_t>(bytes.size()), sha256(bytes)};
	}

	[[nodiscard]] sqlite_backend_opaque_identity source_token()
	{
		return {"test.sqlite-source-capability.v1",
				{std::byte{0x73U}, std::byte{0x71U}, std::byte{0x6cU}}};
	}

	void append_fragmented(sqlite_wal_recovery_workspace_builder& builder,
						   const sqlite_wal_recovery_copy_role role,
						   const std::span<const std::byte> bytes)
	{
		std::size_t offset{};
		std::size_t fragment{1U};
		while (offset < bytes.size())
		{
			const auto count = std::min(fragment, bytes.size() - offset);
			require(builder.append(role, bytes.subspan(offset, count)).has_value(),
					"append recovery workspace fragment");
			offset += count;
			fragment = fragment == 113U ? 1U : fragment + 1U;
		}
	}

	[[nodiscard]] std::shared_ptr<sqlite_wal_recovery_workspace>
	build_workspace(sqlite_runtime& runtime,
					const std::span<const std::byte> main_bytes,
					const std::span<const std::byte> wal_prefix,
					sqlite_wal_scan_receipt source_scan)
	{
		auto builder =
			make_sqlite_wal_recovery_workspace_builder(source_token(), runtime.registry());
		require(builder.has_value(), "create recovery workspace builder");
		append_fragmented(**builder, sqlite_wal_recovery_copy_role::main_database, main_bytes);
		append_fragmented(
			**builder, sqlite_wal_recovery_copy_role::authoritative_wal_prefix, wal_prefix);
		auto sealed =
			(*builder)->seal({copy_receipt(main_bytes), copy_receipt(wal_prefix), source_scan});
		require(sealed.has_value(), "seal recovery workspace");
		return std::move(*sealed);
	}

	struct row_result
	{
		int value{};
		bool seen{};
		bool invalid{};
	};

	int capture_integer(void* context, const int count, char** values, char**) noexcept
	{
		auto* row = static_cast<row_result*>(context);
		if (row == nullptr || count != 1 || values == nullptr || values[0] == nullptr || row->seen)
			return 1;
		const std::string_view spelling{values[0]};
		const auto parsed =
			std::from_chars(spelling.data(), spelling.data() + spelling.size(), row->value);
		row->invalid = parsed.ec != std::errc{} || parsed.ptr != spelling.data() + spelling.size();
		row->seen = !row->invalid;
		return row->invalid ? 1 : 0;
	}

	void require_value(sqlite_connection& connection,
					   const sqlite_wal_recovery_workspace& workspace)
	{
		row_result row;
		const auto status = connection.execute("SELECT value FROM item;", capture_integer, &row);
		if (status != sqlite_ok || !row.seen || row.invalid || row.value != 7)
		{
			auto receipt = workspace.snapshot_receipt();
			const auto accounting = receipt
				? " main=" + std::to_string(receipt->opens.main_success_count) +
					" wal-attempt=" + std::to_string(receipt->opens.wal_attempt_count) +
					" wal-success=" + std::to_string(receipt->opens.wal_success_count) +
					" journal-denied=" +
					std::to_string(receipt->effects.denied_rollback_journal_open_count) +
					" other-denied=" + std::to_string(receipt->effects.denied_other_open_count) +
					" shm-map=" + std::to_string(receipt->shm.map_request_count)
				: " receipt-unavailable";
			throw std::runtime_error{
				"recover exact committed row: status=" + std::to_string(status) +
				" message=" + connection.last_error() + accounting};
		}
	}

	void require_private_paths(const sqlite_wal_recovery_workspace& workspace)
	{
		const std::string main_path{workspace.database_path()};
		require(::access(main_path.c_str(), F_OK) != 0, "workspace main escaped to filesystem");
		require(::access((main_path + "-wal").c_str(), F_OK) != 0,
				"workspace WAL escaped to filesystem");
		require(::access((main_path + "-shm").c_str(), F_OK) != 0,
				"workspace SHM escaped to filesystem");
	}

	void exercise_committed_recovery(sqlite_runtime& runtime,
									 const std::vector<std::byte>& main_bytes,
									 const std::vector<std::byte>& original_wal,
									 const std::string& source_path)
	{
		auto torn_wal = original_wal;
		for (std::uint8_t value = 1U; value <= 17U; ++value)
			torn_wal.push_back(static_cast<std::byte>(value));
		auto scan = scan_bytes(torn_wal);
		require(scan.classification == sqlite_wal_scan_classification::committed_prefix &&
					scan.stop == sqlite_wal_scan_stop::torn_frame && scan.last_valid_commit &&
					scan.authoritative_prefix_byte_count == original_wal.size() &&
					scan.inspected_byte_count == torn_wal.size(),
				"torn suffix was not excluded from WAL authority");
		const auto prefix_size = static_cast<std::size_t>(scan.authoritative_prefix_byte_count);
		const auto prefix = std::span<const std::byte>{torn_wal}.first(prefix_size);
		auto workspace = build_workspace(runtime, main_bytes, prefix, scan);
		require_private_paths(*workspace);
		require(sqlite_find(std::string{workspace->registered_vfs_name()}.c_str()) ==
					workspace->vfs_implementation_identity(),
				"recovery VFS registration identity");

		{
			sqlite_connection recovery{runtime};
			recovery.open(workspace->database_path(),
						  sqlite_wal_recovery_required_open_flags,
						  workspace->registered_vfs_name());
			require_value(recovery, *workspace);
			recovery.execute_ok("BEGIN; SELECT value FROM item; COMMIT;");
			recovery.execute_ok("BEGIN; SELECT value FROM item; COMMIT;");
			require(recovery.execute("CREATE TABLE forbidden(value INTEGER);") != sqlite_ok,
					"recovery workspace admitted a persistent write");
			recovery.close();
		}

		require(workspace->verify_sealed_objects().has_value(),
				"sealed objects changed after SQLite");
		auto receipt = workspace->snapshot_receipt();
		require(receipt.has_value(), "snapshot recovery receipt");
		require(receipt->profile == "cxxlens.sqlite-wal-only-private-recovery-workspace.v1" &&
					receipt->source_capability_token == source_token() && receipt->sealed &&
					receipt->private_wal_present &&
					receipt->main_database == copy_receipt(main_bytes) &&
					receipt->authoritative_wal_prefix == copy_receipt(prefix) &&
					receipt->source_wal_scan.inspected_byte_count == torn_wal.size() &&
					receipt->source_wal_scan.authoritative_prefix_byte_count == prefix.size(),
				"workspace receipt lost exact source/copy binding");
		require(receipt->opens.main_attempt_count == 1U &&
					receipt->opens.main_success_count == 1U &&
					receipt->opens.main_readwrite_no_create &&
					(receipt->opens.last_main_input_flags & sqlite_open_readwrite) != 0 &&
					(receipt->opens.last_main_input_flags &
					 (sqlite_open_create | sqlite_open_uri)) == 0 &&
					(receipt->opens.last_main_input_flags & sqlite_open_main_database) != 0 &&
					receipt->opens.wal_attempt_count >= 1U &&
					receipt->opens.wal_success_count >= 1U && receipt->opens.wal_was_preexisting &&
					(receipt->opens.last_wal_input_flags & sqlite_open_write_ahead_log) != 0,
				"workspace open receipt did not prove main/WAL flags");
		require(receipt->shm.map_request_count >= 1U && receipt->shm.created_region_count >= 1U &&
					receipt->shm.private_byte_count > 0U && receipt->shm.lock_request_count >= 1U &&
					receipt->shm.unlock_request_count >= 1U && receipt->shm.barrier_count >= 1U &&
					receipt->shm.unmap_request_count >= 1U && receipt->shm.held_lock_count == 0U,
				"workspace did not account for private SHM recovery");
		const auto denied_bytes = receipt->effects.denied_main_write_count +
			receipt->effects.denied_main_truncate_count + receipt->effects.denied_wal_write_count +
			receipt->effects.denied_wal_truncate_count;
		require(receipt->effects.only_private_shm_mutation_permitted && denied_bytes >= 1U &&
					receipt->effects.denied_delete_count == 0U,
				"workspace did not deny and account for immutable-file effects");
		require(source_fixture::read_file_for_test(source_path) == main_bytes,
				"recovery mutated source main database");
		require(source_fixture::read_file_for_test(source_path + "-wal") == original_wal,
				"recovery mutated source WAL");
	}

	void exercise_main_alone(sqlite_runtime& runtime,
							 const std::vector<std::byte>& main_bytes,
							 const std::span<const std::byte> valid_wal_header,
							 const std::string& source_path,
							 const std::vector<std::byte>& source_wal)
	{
		auto scan = scan_bytes(valid_wal_header);
		require(scan.classification == sqlite_wal_scan_classification::no_valid_commit &&
					scan.stop == sqlite_wal_scan_stop::end_of_input && scan.header &&
					scan.authoritative_prefix_byte_count == 0U,
				"valid no-commit WAL header acquired authority");
		auto workspace = build_workspace(runtime, main_bytes, {}, scan);
		require_private_paths(*workspace);
		auto receipt = workspace->snapshot_receipt();
		require(receipt.has_value() && receipt->authoritative_wal_prefix.byte_count == 0U &&
					!receipt->private_wal_present && receipt->opens.main_attempt_count == 0U &&
					receipt->opens.wal_attempt_count == 0U && !receipt->opens.wal_was_preexisting &&
					receipt->effects.denied_other_open_count == 0U &&
					receipt->effects.only_private_shm_mutation_permitted &&
					workspace->verify_sealed_objects().has_value(),
				"no-commit workspace exposed or created a WAL object");
		require(source_fixture::read_file_for_test(source_path) == main_bytes &&
					source_fixture::read_file_for_test(source_path + "-wal") == source_wal,
				"main-alone classification mutated source main/WAL bytes");
	}

	void exercise_empty_wal_main_alone(sqlite_runtime& runtime,
									 const std::vector<std::byte>& main_bytes)
	{
		const std::vector<std::byte> empty_wal;
		auto scan = scan_bytes(empty_wal);
		require(scan.classification == sqlite_wal_scan_classification::empty &&
					scan.stop == sqlite_wal_scan_stop::end_of_input && !scan.header &&
					scan.inspected_byte_count == 0U &&
					scan.authoritative_prefix_byte_count == 0U,
			"empty WAL acquired recovery authority");
		auto workspace = build_workspace(runtime, main_bytes, {}, scan);
		auto receipt = workspace->snapshot_receipt();
		require(receipt.has_value() && receipt->source_wal_scan.classification ==
					sqlite_wal_scan_classification::empty &&
					receipt->authoritative_wal_prefix.byte_count == 0U &&
					!receipt->private_wal_present && receipt->opens.main_attempt_count == 0U &&
					receipt->opens.main_success_count == 0U &&
					!receipt->opens.main_readwrite_no_create &&
					receipt->opens.wal_attempt_count == 0U && receipt->shm.map_request_count == 0U &&
					workspace->verify_sealed_objects(),
			"empty WAL workspace did not remain main-only");
	}

	void exercise_seal_failure(sqlite_runtime& runtime,
							   const std::vector<std::byte>& main_bytes,
							   sqlite_wal_scan_receipt no_commit_scan)
	{
		auto builder =
			make_sqlite_wal_recovery_workspace_builder(source_token(), runtime.registry());
		require(builder.has_value(), "create failure-path builder");
		append_fragmented(**builder, sqlite_wal_recovery_copy_role::main_database, main_bytes);
		auto wrong = copy_receipt(main_bytes);
		wrong.sha256.back() = wrong.sha256.back() == '0' ? '1' : '0';
		auto rejected =
			(*builder)->seal({wrong, copy_receipt(std::span<const std::byte>{}), no_commit_scan});
		require(!rejected &&
					!(*builder)
						 ->append(sqlite_wal_recovery_copy_role::main_database,
								  std::span<const std::byte>{main_bytes}.first(1U))
						 .has_value(),
				"failed copy binding did not terminalize builder");
	}

	void exercise_bounded_append(sqlite_runtime& runtime)
	{
		auto builder =
			make_sqlite_wal_recovery_workspace_builder(source_token(), runtime.registry());
		require(builder.has_value(), "create bounded-append builder");
		const std::vector<std::byte> oversized(sqlite_wal_recovery_copy_buffer_bound + 1U);
		auto& target = **builder;
		require(!target.append(sqlite_wal_recovery_copy_role::main_database, oversized).has_value(),
				"workspace admitted an oversized copy window");
	}
} // namespace

int main()
{
	try
	{
		require(sqlite_wal_recovery_required_open_flags ==
						(sqlite_wal_recovery_open_readwrite | sqlite_wal_recovery_open_fullmutex |
						 sqlite_wal_recovery_open_privatecache) &&
					sqlite_wal_recovery_copy_buffer_bound == 65'536U &&
					sqlite_wal_parser_resident_byte_bound == 65'592U,
				"recovery flags or bounded-copy constants drifted");
		sqlite_runtime runtime;
		exercise_bounded_append(runtime);
		source_fixture source{runtime};
		const auto initial_main = source.read_main();
		const auto initial_wal = source.read_wal();
		require(!initial_main.empty() && initial_wal.size() > sqlite_wal_header_byte_count,
				"real SQLite did not produce a WAL fixture");
		exercise_committed_recovery(runtime, initial_main, initial_wal, source.path());

		source.checkpoint_full();
		const auto checkpointed_main = source.read_main();
		const auto checkpointed_wal = source.read_wal();
		const auto wal_header =
			std::span<const std::byte>{initial_wal}.first(sqlite_wal_header_byte_count);
		auto no_commit_scan = scan_bytes(wal_header);
		exercise_main_alone(
			runtime, checkpointed_main, wal_header, source.path(), checkpointed_wal);
		exercise_empty_wal_main_alone(runtime, checkpointed_main);
		exercise_seal_failure(runtime, checkpointed_main, no_commit_scan);
		return 0;
	}
	catch (const std::exception& failure)
	{
		std::cerr << failure.what() << '\n';
		return 1;
	}
}
