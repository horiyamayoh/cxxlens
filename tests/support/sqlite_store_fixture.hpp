#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#endif
#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

#include <cxxlens/sdk.hpp>

namespace cxxlens::test::sqlite_fixture
{
	inline constexpr std::int64_t chunk_maximum_bytes = 8'388'608;

	class runtime
	{
	  public:
		using database_handle = void*;
		using statement_handle = void*;
		using destructor_function = void (*)(void*);
		using open_function = int (*)(const char*, database_handle*, int, const char*);
		using close_function = int (*)(database_handle);
		using error_function = const char* (*)(database_handle);
		using execute_function = int (*)(
			database_handle, const char*, int (*)(void*, int, char**, char**), void*, char**);
		using free_function = void (*)(void*);
		using file_control_function = int (*)(database_handle, const char*, int, void*);
		using prepare_function =
			int (*)(database_handle, const char*, int, statement_handle*, const char**);
		using step_function = int (*)(statement_handle);
		using finalize_function = int (*)(statement_handle);
		using bind_text_function =
			int (*)(statement_handle, int, const char*, int, destructor_function);
		using bind_int64_function = int (*)(statement_handle, int, std::int64_t);
		using bind_blob64_function =
			int (*)(statement_handle, int, const void*, std::uint64_t, destructor_function);
		using bind_null_function = int (*)(statement_handle, int);
		using column_count_function = int (*)(statement_handle);
		using column_type_function = int (*)(statement_handle, int);
		using column_text_function = const unsigned char* (*)(statement_handle, int);
		using column_blob_function = const void* (*)(statement_handle, int);
		using column_bytes_function = int (*)(statement_handle, int);
		using column_int64_function = std::int64_t (*)(statement_handle, int);

		runtime()
		{
#if defined(__unix__) || defined(__APPLE__)
#if defined(__APPLE__)
			constexpr std::array candidates{"libsqlite3.dylib", "/usr/lib/libsqlite3.dylib"};
#else
			constexpr std::array candidates{"libsqlite3.so.0", "libsqlite3.so"};
#endif
			for (const auto* candidate : candidates)
			{
				library_ = ::dlopen(candidate, RTLD_NOW | RTLD_LOCAL);
				if (library_ != nullptr)
					break;
			}
			if (library_ == nullptr)
				throw std::runtime_error{"test SQLite runtime unavailable"};
			resolve("sqlite3_open_v2", open);
			resolve("sqlite3_close_v2", close);
			resolve("sqlite3_errmsg", errmsg);
			resolve("sqlite3_exec", execute);
			resolve("sqlite3_free", free_memory);
			resolve("sqlite3_file_control", file_control);
			resolve("sqlite3_prepare_v2", prepare);
			resolve("sqlite3_step", step);
			resolve("sqlite3_finalize", finalize);
			resolve("sqlite3_bind_text", bind_text);
			resolve("sqlite3_bind_int64", bind_int64);
			resolve("sqlite3_bind_blob64", bind_blob64);
			resolve("sqlite3_bind_null", bind_null);
			resolve("sqlite3_column_count", column_count);
			resolve("sqlite3_column_type", column_type);
			resolve("sqlite3_column_text", column_text);
			resolve("sqlite3_column_blob", column_blob);
			resolve("sqlite3_column_bytes", column_bytes);
			resolve("sqlite3_column_int64", column_int64);
#else
			throw std::runtime_error{"test SQLite runtime is unsupported on this platform"};
#endif
		}

		runtime(const runtime&) = delete;
		runtime& operator=(const runtime&) = delete;
		~runtime()
		{
#if defined(__unix__) || defined(__APPLE__)
			if (library_ != nullptr)
				(void)::dlclose(library_);
#endif
		}

		open_function open{};
		close_function close{};
		error_function errmsg{};
		execute_function execute{};
		free_function free_memory{};
		file_control_function file_control{};
		prepare_function prepare{};
		step_function step{};
		finalize_function finalize{};
		bind_text_function bind_text{};
		bind_int64_function bind_int64{};
		bind_blob64_function bind_blob64{};
		bind_null_function bind_null{};
		column_count_function column_count{};
		column_type_function column_type{};
		column_text_function column_text{};
		column_blob_function column_blob{};
		column_bytes_function column_bytes{};
		column_int64_function column_int64{};

	  private:
		template <class Function>
		void resolve(const char* name, Function& output)
		{
#if defined(__unix__) || defined(__APPLE__)
			const auto symbol = ::dlsym(library_, name);
			if (symbol == nullptr)
				throw std::runtime_error{std::string{"test SQLite symbol unavailable: "} + name};
			output = reinterpret_cast<Function>(symbol);
#else
			(void)name;
			(void)output;
#endif
		}

		void* library_{};
	};

	using blob = std::vector<std::byte>;
	using cell = std::variant<std::monostate, std::int64_t, std::string, blob>;
	using row = std::vector<cell>;

	class connection;

	class statement
	{
	  public:
		statement(connection& owner, std::string_view sql);
		statement(const statement&) = delete;
		statement& operator=(const statement&) = delete;
		statement(statement&&) = delete;
		statement& operator=(statement&&) = delete;
		~statement();

		void text(int index, std::string_view value);
		void integer(int index, std::int64_t value);
		void bytes(int index, const blob& value);
		void null(int index);
		[[nodiscard]] int step();
		[[nodiscard]] runtime::statement_handle native() const noexcept
		{
			return statement_;
		}

	  private:
		connection* owner_{};
		runtime::statement_handle statement_{};
	};

	class connection
	{
	  public:
		explicit connection(const std::filesystem::path& path, const bool read_only = false)
		{
			constexpr int sqlite_open_readonly = 0x00000001;
			constexpr int sqlite_open_readwrite = 0x00000002;
			constexpr int sqlite_open_fullmutex = 0x00010000;
			const auto flags =
				(read_only ? sqlite_open_readonly : sqlite_open_readwrite) | sqlite_open_fullmutex;
			const auto native_path = path.string();
			const auto result = api_.open(native_path.c_str(), &database_, flags, nullptr);
			if (result != sqlite_ok)
			{
				const std::string detail =
					database_ != nullptr ? api_.errmsg(database_) : "null database";
				if (database_ != nullptr)
				{
					(void)api_.close(database_);
					database_ = nullptr;
				}
				throw std::runtime_error{"test SQLite open failed: " + detail};
			}
		}

		connection(const connection&) = delete;
		connection& operator=(const connection&) = delete;
		~connection()
		{
			if (database_ != nullptr)
				(void)api_.close(database_);
		}

		void exec(std::string_view sql)
		{
			char* message{};
			const std::string terminated{sql};
			const auto result =
				api_.execute(database_, terminated.c_str(), nullptr, nullptr, &message);
			if (result == sqlite_ok)
				return;
			const std::string detail = message != nullptr ? message : api_.errmsg(database_);
			if (message != nullptr)
				api_.free_memory(message);
			throw std::runtime_error{"test SQLite statement failed: " + detail + " [" + terminated +
									 ']'};
		}

		[[nodiscard]] std::vector<row> query(std::string_view sql)
		{
			statement prepared{*this, sql};
			std::vector<row> output;
			while (true)
			{
				const auto result = prepared.step();
				if (result == sqlite_done)
					return output;
				if (result != sqlite_row)
					fail("query step", result);
				const auto count = api_.column_count(prepared.native());
				auto& values = output.emplace_back();
				values.reserve(static_cast<std::size_t>(count));
				for (int index{}; index < count; ++index)
				{
					const auto type = api_.column_type(prepared.native(), index);
					if (type == sqlite_null)
						values.emplace_back(std::monostate{});
					else if (type == sqlite_integer)
						values.emplace_back(api_.column_int64(prepared.native(), index));
					else if (type == sqlite_text)
					{
						const auto* data = api_.column_text(prepared.native(), index);
						const auto size = api_.column_bytes(prepared.native(), index);
						values.emplace_back(std::string{reinterpret_cast<const char*>(data),
														static_cast<std::size_t>(size)});
					}
					else if (type == sqlite_blob)
					{
						const auto* data = static_cast<const std::byte*>(
							api_.column_blob(prepared.native(), index));
						const auto size = api_.column_bytes(prepared.native(), index);
						values.emplace_back(blob{data, data + static_cast<std::ptrdiff_t>(size)});
					}
					else
						throw std::runtime_error{"test SQLite returned an unsupported cell type"};
				}
			}
		}

		void close()
		{
			if (database_ == nullptr)
				return;
			const auto result = api_.close(database_);
			if (result != sqlite_ok)
				fail("close", result);
			database_ = nullptr;
		}

		void persist_wal()
		{
			constexpr int sqlite_fcntl_persist_wal = 10;
			int enabled{1};
			const auto result =
				api_.file_control(database_, nullptr, sqlite_fcntl_persist_wal, &enabled);
			if (result != sqlite_ok || enabled != 1)
				fail("persist WAL", result);
		}

		[[nodiscard]] runtime& api() noexcept
		{
			return api_;
		}
		[[nodiscard]] runtime::database_handle native() const noexcept
		{
			return database_;
		}
		[[noreturn]] void fail(std::string_view operation, int result) const
		{
			throw std::runtime_error{"test SQLite " + std::string{operation} + " failed (" +
									 std::to_string(result) + "): " + api_.errmsg(database_)};
		}

		static constexpr int sqlite_ok = 0;
		static constexpr int sqlite_row = 100;
		static constexpr int sqlite_done = 101;
		static constexpr int sqlite_integer = 1;
		static constexpr int sqlite_text = 3;
		static constexpr int sqlite_blob = 4;
		static constexpr int sqlite_null = 5;

	  private:
		runtime api_;
		runtime::database_handle database_{};
		friend class statement;
	};

	inline statement::statement(connection& owner, const std::string_view sql) : owner_{&owner}
	{
		const std::string terminated{sql};
		const char* tail{};
		const auto result = owner_->api().prepare(owner_->native(),
												  terminated.c_str(),
												  static_cast<int>(terminated.size()),
												  &statement_,
												  &tail);
		if (result != connection::sqlite_ok || statement_ == nullptr ||
			(tail != nullptr && *tail != '\0'))
			owner_->fail("prepare", result);
	}

	inline statement::~statement()
	{
		if (statement_ != nullptr)
			(void)owner_->api().finalize(statement_);
	}

	inline void statement::text(const int index, const std::string_view value)
	{
		const auto result =
			owner_->api().bind_text(statement_,
									index,
									value.data(),
									static_cast<int>(value.size()),
									reinterpret_cast<runtime::destructor_function>(-1));
		if (result != connection::sqlite_ok)
			owner_->fail("bind text", result);
	}

	inline void statement::integer(const int index, const std::int64_t value)
	{
		const auto result = owner_->api().bind_int64(statement_, index, value);
		if (result != connection::sqlite_ok)
			owner_->fail("bind integer", result);
	}

	inline void statement::bytes(const int index, const blob& value)
	{
		static constexpr std::byte empty_blob_sentinel{};
		const auto* data = value.empty() ? &empty_blob_sentinel : value.data();
		const auto result =
			owner_->api().bind_blob64(statement_,
									  index,
									  data,
									  value.size(),
									  reinterpret_cast<runtime::destructor_function>(-1));
		if (result != connection::sqlite_ok)
			owner_->fail("bind blob", result);
	}

	inline void statement::null(const int index)
	{
		const auto result = owner_->api().bind_null(statement_, index);
		if (result != connection::sqlite_ok)
			owner_->fail("bind null", result);
	}

	inline int statement::step()
	{
		return owner_->api().step(statement_);
	}

	[[nodiscard]] inline const std::string& text(const cell& value)
	{
		const auto* output = std::get_if<std::string>(&value);
		if (output == nullptr)
			throw std::runtime_error{"test SQLite cell is not TEXT"};
		return *output;
	}

	[[nodiscard]] inline std::int64_t integer(const cell& value)
	{
		const auto* output = std::get_if<std::int64_t>(&value);
		if (output == nullptr)
			throw std::runtime_error{"test SQLite cell is not INTEGER"};
		return *output;
	}

	[[nodiscard]] inline const blob& bytes(const cell& value)
	{
		const auto* output = std::get_if<blob>(&value);
		if (output == nullptr)
			throw std::runtime_error{"test SQLite cell is not BLOB"};
		return *output;
	}

	struct publication
	{
		std::string publication_id;
		std::string series_id;
		std::string snapshot_id;
		std::int64_t sequence{};
		std::int64_t generation{};
		std::optional<std::string> parent;
		std::int64_t state{};
		std::string checksum;
		std::int64_t payload_byte_count{};
		std::int64_t payload_chunk_count{};
		blob payload;
	};

	struct chunk
	{
		std::string publication_id;
		std::int64_t generation{};
		std::int64_t ordinal{};
		std::int64_t offset{};
		std::int64_t byte_count{};
		std::string checksum;
		blob payload;
	};

	[[nodiscard]] inline std::vector<chunk> read_chunks(connection& database)
	{
		const auto rows = database.query(
			"SELECT "
			"publication_id,generation,chunk_ordinal,byte_offset,byte_count,checksum,payload "
			"FROM cxxlens_ng_payload_chunk ORDER BY publication_id,generation,chunk_ordinal");
		std::vector<chunk> output;
		output.reserve(rows.size());
		for (const auto& value : rows)
		{
			if (value.size() != 7U)
				throw std::runtime_error{"test SQLite chunk column count drifted"};
			output.push_back({text(value[0U]),
							  integer(value[1U]),
							  integer(value[2U]),
							  integer(value[3U]),
							  integer(value[4U]),
							  text(value[5U]),
							  bytes(value[6U])});
		}
		return output;
	}

	[[nodiscard]] inline std::vector<publication> read_v3_publications(connection& database)
	{
		const auto rows = database.query(
			"SELECT publication_id,series_id,snapshot_id,sequence,generation,parent,state,"
			"payload_checksum,payload_byte_count,payload_chunk_count "
			"FROM cxxlens_ng_publication ORDER BY publication_id");
		const auto chunks = read_chunks(database);
		std::vector<publication> output;
		output.reserve(rows.size());
		for (const auto& value : rows)
		{
			if (value.size() != 10U)
				throw std::runtime_error{"test SQLite publication column count drifted"};
			publication record{text(value[0U]),
							   text(value[1U]),
							   text(value[2U]),
							   integer(value[3U]),
							   integer(value[4U]),
							   std::holds_alternative<std::monostate>(value[5U])
								   ? std::optional<std::string>{}
								   : std::optional<std::string>{text(value[5U])},
							   integer(value[6U]),
							   text(value[7U]),
							   integer(value[8U]),
							   integer(value[9U]),
							   {}};
			for (const auto& owned : chunks)
				if (owned.publication_id == record.publication_id &&
					owned.generation == record.generation)
				{
					if (owned.ordinal < 0 || owned.offset != owned.ordinal * chunk_maximum_bytes ||
						owned.byte_count != static_cast<std::int64_t>(owned.payload.size()) ||
						owned.checksum != sdk::content_digest(owned.payload))
						throw std::runtime_error{"test source v3 chunk is not canonical"};
					record.payload.insert(
						record.payload.end(), owned.payload.begin(), owned.payload.end());
				}
			if (record.payload_byte_count != static_cast<std::int64_t>(record.payload.size()) ||
				(record.state == 3 && record.checksum != sdk::content_digest(record.payload)))
				throw std::runtime_error{"test source v3 publication is not canonical"};
			output.push_back(std::move(record));
		}
		return output;
	}

	[[nodiscard]] inline std::vector<publication>
	read_v3_publications(const std::filesystem::path& path)
	{
		connection database{path, true};
		return read_v3_publications(database);
	}

	[[nodiscard]] inline std::vector<chunk> read_chunks(const std::filesystem::path& path)
	{
		connection database{path, true};
		return read_chunks(database);
	}

	[[nodiscard]] inline std::vector<publication>
	read_v2_publications(const std::filesystem::path& path)
	{
		connection database{path, true};
		const auto rows = database.query(
			"SELECT publication_id,series_id,snapshot_id,sequence,generation,parent,state,"
			"checksum,payload FROM cxxlens_ng_publication ORDER BY publication_id");
		std::vector<publication> output;
		for (const auto& value : rows)
		{
			if (value.size() != 9U)
				throw std::runtime_error{"test SQLite v2 publication column count drifted"};
			const auto& payload = bytes(value[8U]);
			output.push_back({text(value[0U]),
							  text(value[1U]),
							  text(value[2U]),
							  integer(value[3U]),
							  integer(value[4U]),
							  std::holds_alternative<std::monostate>(value[5U])
								  ? std::optional<std::string>{}
								  : std::optional<std::string>{text(value[5U])},
							  integer(value[6U]),
							  text(value[7U]),
							  static_cast<std::int64_t>(payload.size()),
							  payload.empty() ? 0 : 1,
							  payload});
		}
		return output;
	}

	struct head
	{
		std::string series_id;
		std::string publication_id;
		std::int64_t sequence{};
		[[nodiscard]] bool operator==(const head&) const = default;
	};

	[[nodiscard]] inline std::vector<head> read_heads(const std::filesystem::path& path)
	{
		connection database{path, true};
		const auto rows = database.query(
			"SELECT series_id,current_publication,sequence FROM cxxlens_ng_series_head ORDER BY "
			"series_id");
		std::vector<head> output;
		for (const auto& value : rows)
			output.push_back({text(value[0U]), text(value[1U]), integer(value[2U])});
		return output;
	}

	[[nodiscard]] inline std::vector<std::pair<std::string, std::string>>
	read_metadata(const std::filesystem::path& path)
	{
		connection database{path, true};
		const auto rows = database.query("SELECT key,value FROM cxxlens_ng_metadata ORDER BY key");
		std::vector<std::pair<std::string, std::string>> output;
		for (const auto& value : rows)
		{
			if (value.size() != 2U)
				throw std::runtime_error{"test SQLite metadata column count drifted"};
			output.emplace_back(text(value[0U]), text(value[1U]));
		}
		return output;
	}

	struct schema_object
	{
		std::string type;
		std::string name;
		std::string table;
		std::string sql;
		[[nodiscard]] bool operator==(const schema_object&) const = default;
	};

	[[nodiscard]] inline std::vector<schema_object>
	read_schema_objects(const std::filesystem::path& path)
	{
		connection database{path, true};
		const auto rows = database.query("SELECT type,name,tbl_name,sql FROM sqlite_schema "
										 "WHERE sql IS NOT NULL ORDER BY type,name");
		std::vector<schema_object> output;
		for (const auto& value : rows)
			output.push_back({text(value[0U]), text(value[1U]), text(value[2U]), text(value[3U])});
		return output;
	}

	inline void finish_bound_statement(connection& database, statement& prepared)
	{
		const auto result = prepared.step();
		if (result != connection::sqlite_done)
			database.fail("bound statement", result);
	}

	inline void downgrade_v3_to_exact_v2(const std::filesystem::path& path)
	{
		connection database{path};
		const auto publications = read_v3_publications(database);
		const auto head_rows = database.query(
			"SELECT series_id,current_publication,sequence FROM cxxlens_ng_series_head "
			"ORDER BY series_id");
		database.exec("BEGIN IMMEDIATE");
		try
		{
			database.exec(
				"DROP INDEX cxxlens_ng_payload_chunk_locator;"
				"DROP INDEX cxxlens_ng_publication_series;"
				"DROP TABLE cxxlens_ng_payload_chunk;"
				"DROP TABLE cxxlens_ng_series_head;"
				"DROP TABLE cxxlens_ng_publication;"
				"DROP TABLE cxxlens_ng_metadata;"
				"CREATE TABLE cxxlens_ng_metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
				"CREATE TABLE cxxlens_ng_publication(publication_id TEXT PRIMARY KEY,series_id "
				"TEXT NOT NULL,snapshot_id TEXT NOT NULL,sequence INTEGER NOT NULL,generation "
				"INTEGER NOT NULL,parent TEXT,state INTEGER NOT NULL,checksum TEXT NOT "
				"NULL,payload "
				"BLOB NOT NULL);"
				"CREATE TABLE cxxlens_ng_series_head(series_id TEXT PRIMARY "
				"KEY,current_publication "
				"TEXT NOT NULL,sequence INTEGER NOT NULL);"
				"CREATE INDEX cxxlens_ng_publication_series ON "
				"cxxlens_ng_publication(series_id,sequence);");
			{
				statement marker{database, "INSERT INTO cxxlens_ng_metadata VALUES(?,?)"};
				marker.text(1, "physical_format");
				marker.text(2, "cxxlens.sqlite-semantic-store.v2");
				finish_bound_statement(database, marker);
			}
			for (const auto& record : publications)
			{
				statement insert{database,
								 "INSERT INTO cxxlens_ng_publication VALUES(?,?,?,?,?,?,?,?,?)"};
				insert.text(1, record.publication_id);
				insert.text(2, record.series_id);
				insert.text(3, record.snapshot_id);
				insert.integer(4, record.sequence);
				insert.integer(5, record.generation);
				if (record.parent)
					insert.text(6, *record.parent);
				else
					insert.null(6);
				insert.integer(7, record.state);
				insert.text(8, record.checksum);
				insert.bytes(9, record.payload);
				finish_bound_statement(database, insert);
			}
			for (const auto& head : head_rows)
			{
				statement insert{database, "INSERT INTO cxxlens_ng_series_head VALUES(?,?,?)"};
				insert.text(1, text(head[0U]));
				insert.text(2, text(head[1U]));
				insert.integer(3, integer(head[2U]));
				finish_bound_statement(database, insert);
			}
			database.exec("COMMIT");
		}
		catch (...)
		{
			try
			{
				database.exec("ROLLBACK");
			}
			catch (...)
			{
			}
			throw;
		}
		database.exec("PRAGMA wal_checkpoint(TRUNCATE)");
		database.close();
	}

	inline void rewrite_v2_checksum(const std::filesystem::path& path,
									std::string_view publication_id,
									std::string_view checksum)
	{
		connection database{path};
		database.exec("BEGIN IMMEDIATE");
		statement update{database,
						 "UPDATE cxxlens_ng_publication SET checksum=? WHERE publication_id=?"};
		update.text(1, checksum);
		update.text(2, publication_id);
		finish_bound_statement(database, update);
		database.exec("COMMIT");
		database.exec("PRAGMA wal_checkpoint(TRUNCATE)");
		database.close();
	}

	inline void delete_chunk(const std::filesystem::path& path,
							 std::string_view publication_id,
							 const std::int64_t ordinal)
	{
		connection database{path};
		database.exec("BEGIN IMMEDIATE");
		statement update{database,
						 "DELETE FROM cxxlens_ng_payload_chunk WHERE publication_id=? AND "
						 "chunk_ordinal=?"};
		update.text(1, publication_id);
		update.integer(2, ordinal);
		finish_bound_statement(database, update);
		database.exec("COMMIT");
		database.exec("PRAGMA wal_checkpoint(TRUNCATE)");
		database.close();
	}

	inline void drift_chunk_offset(const std::filesystem::path& path,
								   std::string_view publication_id,
								   const std::int64_t ordinal)
	{
		connection database{path};
		database.exec("BEGIN IMMEDIATE");
		statement update{database,
						 "UPDATE cxxlens_ng_payload_chunk SET byte_offset=byte_offset+1 WHERE "
						 "publication_id=? AND chunk_ordinal=?"};
		update.text(1, publication_id);
		update.integer(2, ordinal);
		finish_bound_statement(database, update);
		database.exec("COMMIT");
		database.exec("PRAGMA wal_checkpoint(TRUNCATE)");
		database.close();
	}

	inline void rewrite_chunk_checksum(const std::filesystem::path& path,
									   std::string_view publication_id,
									   const std::int64_t ordinal)
	{
		connection database{path};
		database.exec("BEGIN IMMEDIATE");
		statement update{
			database,
			"UPDATE cxxlens_ng_payload_chunk SET checksum=? WHERE publication_id=? AND "
			"chunk_ordinal=?"};
		update.text(1, "sha256:0000000000000000000000000000000000000000000000000000000000000000");
		update.text(2, publication_id);
		update.integer(3, ordinal);
		finish_bound_statement(database, update);
		database.exec("COMMIT");
		database.exec("PRAGMA wal_checkpoint(TRUNCATE)");
		database.close();
	}

	inline void shift_nonfinal_chunk_boundary(const std::filesystem::path& path,
											  std::string_view publication_id)
	{
		connection database{path};
		std::int64_t generation{};
		std::vector<blob> payloads;
		{
			statement select{
				database,
				"SELECT generation,chunk_ordinal,payload FROM cxxlens_ng_payload_chunk WHERE "
				"publication_id=? ORDER BY chunk_ordinal"};
			select.text(1, publication_id);
			while (true)
			{
				const auto result = select.step();
				if (result == connection::sqlite_done)
					break;
				if (result != connection::sqlite_row)
					database.fail("boundary-shift source query", result);
				const auto ordinal = database.api().column_int64(select.native(), 1);
				if (ordinal != static_cast<std::int64_t>(payloads.size()))
					throw std::runtime_error{"test boundary-shift source ordinal drifted"};
				if (payloads.empty())
					generation = database.api().column_int64(select.native(), 0);
				else if (database.api().column_int64(select.native(), 0) != generation)
					throw std::runtime_error{"test boundary-shift source generation drifted"};
				const auto* data =
					static_cast<const std::byte*>(database.api().column_blob(select.native(), 2));
				const auto size = database.api().column_bytes(select.native(), 2);
				payloads.emplace_back(data, data + static_cast<std::ptrdiff_t>(size));
			}
		}
		if (payloads.size() < 2U || payloads.back().empty() ||
			payloads.back().size() >= static_cast<std::size_t>(chunk_maximum_bytes))
			throw std::runtime_error{"test boundary-shift source has no partial final chunk"};
		for (std::size_t index{}; index + 1U < payloads.size(); ++index)
			if (payloads[index].size() != static_cast<std::size_t>(chunk_maximum_bytes))
				throw std::runtime_error{"test boundary-shift source nonfinal chunk drifted"};
		blob original_bytes;
		for (const auto& payload : payloads)
			original_bytes.insert(original_bytes.end(), payload.begin(), payload.end());

		// Move the last byte of each nonfinal chunk into the front of its successor.
		// This preserves byte concatenation, total length, chunk count, and the full
		// checksum while keeping every row within the physical 8 MiB CHECK.  Only the
		// canonical split and derived offsets/per-chunk checksums change.
		auto carry = payloads.front().back();
		payloads.front().pop_back();
		for (std::size_t index = 1U; index < payloads.size(); ++index)
		{
			std::optional<std::byte> next;
			if (index + 1U < payloads.size())
			{
				next = payloads[index].back();
				payloads[index].pop_back();
			}
			payloads[index].insert(payloads[index].begin(), carry);
			if (next)
				carry = *next;
		}
		blob shifted_bytes;
		for (const auto& payload : payloads)
			shifted_bytes.insert(shifted_bytes.end(), payload.begin(), payload.end());
		if (shifted_bytes != original_bytes)
			throw std::runtime_error{"test boundary shift changed the full payload byte stream"};

		database.exec("BEGIN IMMEDIATE");
		std::int64_t offset{};
		for (std::size_t index{}; index < payloads.size(); ++index)
		{
			const auto& payload = payloads[index];
			statement update{
				database,
				"UPDATE cxxlens_ng_payload_chunk SET byte_offset=?,byte_count=?,checksum=?,"
				"payload=? WHERE publication_id=? AND generation=? AND chunk_ordinal=?"};
			update.integer(1, offset);
			update.integer(2, static_cast<std::int64_t>(payload.size()));
			update.text(3, sdk::content_digest(payload));
			update.bytes(4, payload);
			update.text(5, publication_id);
			update.integer(6, generation);
			update.integer(7, static_cast<std::int64_t>(index));
			finish_bound_statement(database, update);
			offset += static_cast<std::int64_t>(payload.size());
		}
		database.exec("COMMIT");
		database.exec("PRAGMA wal_checkpoint(TRUNCATE)");
		database.close();
	}

	inline void insert_extra_chunk(const std::filesystem::path& path,
								   std::string_view publication_id)
	{
		connection database{path};
		std::int64_t generation{};
		std::int64_t ordinal{};
		std::string checksum;
		blob payload;
		{
			statement select{
				database,
				"SELECT p.generation,p.payload_chunk_count,c.checksum,c.payload FROM "
				"cxxlens_ng_publication AS p JOIN cxxlens_ng_payload_chunk AS c ON "
				"c.publication_id=p.publication_id AND c.generation=p.generation WHERE "
				"p.publication_id=? ORDER BY c.chunk_ordinal LIMIT 1"};
			select.text(1, publication_id);
			if (select.step() != connection::sqlite_row)
				throw std::runtime_error{"test extra chunk source unavailable"};
			generation = database.api().column_int64(select.native(), 0);
			ordinal = database.api().column_int64(select.native(), 1);
			const auto* checksum_data = database.api().column_text(select.native(), 2);
			const auto checksum_size = database.api().column_bytes(select.native(), 2);
			checksum.assign(reinterpret_cast<const char*>(checksum_data),
							static_cast<std::size_t>(checksum_size));
			const auto* payload_data =
				static_cast<const std::byte*>(database.api().column_blob(select.native(), 3));
			const auto payload_size = database.api().column_bytes(select.native(), 3);
			payload.assign(payload_data, payload_data + static_cast<std::ptrdiff_t>(payload_size));
		}
		database.exec("BEGIN IMMEDIATE");
		statement insert{database, "INSERT INTO cxxlens_ng_payload_chunk VALUES(?,?,?,?,?,?,?)"};
		insert.text(1, publication_id);
		insert.integer(2, generation);
		insert.integer(3, ordinal);
		insert.integer(4, ordinal * chunk_maximum_bytes);
		insert.integer(5, static_cast<std::int64_t>(payload.size()));
		insert.text(6, checksum);
		insert.bytes(7, payload);
		finish_bound_statement(database, insert);
		database.exec("COMMIT");
		database.exec("PRAGMA wal_checkpoint(TRUNCATE)");
		database.close();
	}

	inline void insert_orphan_chunk(const std::filesystem::path& path,
									std::string_view source_publication_id)
	{
		connection database{path};
		const auto rows = database.query(
			"SELECT generation,chunk_ordinal,byte_offset,byte_count,checksum,payload FROM "
			"cxxlens_ng_payload_chunk ORDER BY publication_id,chunk_ordinal LIMIT 1");
		if (rows.size() != 1U || rows.front().size() != 6U)
			throw std::runtime_error{"test orphan chunk source unavailable"};
		(void)source_publication_id;
		database.exec("BEGIN IMMEDIATE");
		statement insert{database, "INSERT INTO cxxlens_ng_payload_chunk VALUES(?,?,?,?,?,?,?)"};
		insert.text(
			1,
			"publication:sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
		insert.integer(2, integer(rows.front()[0U]));
		insert.integer(3, integer(rows.front()[1U]));
		insert.integer(4, integer(rows.front()[2U]));
		insert.integer(5, integer(rows.front()[3U]));
		insert.text(6, text(rows.front()[4U]));
		insert.bytes(7, bytes(rows.front()[5U]));
		finish_bound_statement(database, insert);
		database.exec("COMMIT");
		database.exec("PRAGMA wal_checkpoint(TRUNCATE)");
		database.close();
	}

	struct database_files
	{
		std::vector<std::string> directory_entries;
		std::map<std::string, std::vector<unsigned char>, std::less<>> bytes_by_name;
		[[nodiscard]] bool operator==(const database_files&) const = default;
	};

	class temporary_directory
	{
	  public:
		explicit temporary_directory(std::string_view label)
		{
			static std::atomic<std::uint64_t> sequence{};
			const auto nonce = static_cast<std::uint64_t>(
				std::chrono::steady_clock::now().time_since_epoch().count());
			path_ = std::filesystem::temp_directory_path() /
				("cxxlens-" + std::string{label} + '-' + std::to_string(nonce) + '-' +
				 std::to_string(sequence.fetch_add(1U)));
			if (!std::filesystem::create_directory(path_))
				throw std::runtime_error{"test temporary directory creation failed"};
		}

		temporary_directory(const temporary_directory&) = delete;
		temporary_directory& operator=(const temporary_directory&) = delete;
		~temporary_directory()
		{
			std::error_code error;
			std::filesystem::remove_all(path_, error);
		}

		[[nodiscard]] const std::filesystem::path& path() const noexcept
		{
			return path_;
		}

	  private:
		std::filesystem::path path_;
	};

	[[nodiscard]] inline std::vector<unsigned char> read_file(const std::filesystem::path& path)
	{
		std::ifstream stream{path, std::ios::binary};
		if (!stream)
			throw std::runtime_error{"test fixture file could not be read: " + path.string()};
		return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
	}

	[[nodiscard]] inline database_files capture_files(const std::filesystem::path& input)
	{
		const auto path = std::filesystem::absolute(input);
		database_files output;
		for (const auto& entry : std::filesystem::directory_iterator{path.parent_path()})
			output.directory_entries.push_back(entry.path().filename().string());
		std::ranges::sort(output.directory_entries);
		for (const auto& suffix :
			 {std::string{}, std::string{"-wal"}, std::string{"-shm"}, std::string{"-journal"}})
		{
			const auto candidate = std::filesystem::path{path.string() + suffix};
			if (std::filesystem::is_regular_file(candidate))
				output.bytes_by_name.emplace(candidate.filename().string(), read_file(candidate));
		}
		return output;
	}

	inline void require_wal_header_and_quiescent_sidecars(const std::filesystem::path& path)
	{
		const auto files = capture_files(path);
		const auto main =
			files.bytes_by_name.find(std::filesystem::absolute(path).filename().string());
		if (main == files.bytes_by_name.end() || main->second.size() < 20U ||
			main->second[18U] != 2U || main->second[19U] != 2U)
			throw std::runtime_error{"test SQLite fixture is not a WAL-mode main database"};
		for (const auto& suffix :
			 {std::string{"-wal"}, std::string{"-shm"}, std::string{"-journal"}})
			if (files.bytes_by_name.contains(path.filename().string() + suffix))
				throw std::runtime_error{"test SQLite fixture sidecar is not quiescent"};
	}

	inline void quiesce_wal_sidecars(const std::filesystem::path& path)
	{
		connection database{path};
		database.exec("PRAGMA wal_checkpoint(TRUNCATE)");
		database.close();
		require_wal_header_and_quiescent_sidecars(path);
	}

	inline void remove_sqlite_database_files(const std::filesystem::path& path)
	{
		std::error_code ignored;
		for (const auto& suffix :
			 {std::string{}, std::string{"-wal"}, std::string{"-shm"}, std::string{"-journal"}})
			std::filesystem::remove(std::filesystem::path{path.string() + suffix}, ignored);
	}

	enum class wal_source_authority
	{
		current_v3,
		predecessor_v2,
	};

	namespace wal_fixture_detail
	{
		inline constexpr std::size_t copy_buffer_bound = 64U * 1024U;

		[[nodiscard]] inline std::filesystem::path sidecar_path(std::filesystem::path main_path,
																const std::string_view suffix)
		{
			main_path += suffix;
			return main_path;
		}

		inline void require_regular_file_larger_than(const std::filesystem::path& path,
													 const std::uintmax_t minimum_exclusive)
		{
			if (!std::filesystem::is_regular_file(path) ||
				std::filesystem::file_size(path) <= minimum_exclusive)
				throw std::runtime_error{"test SQLite fixture file is missing or too small: " +
										 path.string()};
		}

		inline void require_absent(const std::filesystem::path& path)
		{
			if (std::filesystem::exists(path))
				throw std::runtime_error{"test SQLite fixture unexpectedly exists: " +
										 path.string()};
		}

		inline void require_v3_authority(connection& database)
		{
			const auto format =
				database.query("SELECT value FROM cxxlens_ng_metadata WHERE key='physical_format'");
			const auto version = database.query(
				"SELECT value FROM cxxlens_ng_metadata WHERE key='physical_format_version'");
			if (format.size() != 1U || format.front().size() != 1U ||
				text(format.front().front()) != "cxxlens.sqlite-semantic-store.v3" ||
				version.size() != 1U || version.front().size() != 1U ||
				text(version.front().front()) != "3.0.0")
				throw std::runtime_error{"test SQLite WAL source is not valid v3 authority"};
		}

		inline void require_v2_authority(connection& database)
		{
			const auto metadata =
				database.query("SELECT key,value FROM cxxlens_ng_metadata ORDER BY key");
			if (metadata.size() != 1U || metadata.front().size() != 2U ||
				text(metadata.front()[0U]) != "physical_format" ||
				text(metadata.front()[1U]) != "cxxlens.sqlite-semantic-store.v2")
				throw std::runtime_error{"test SQLite WAL source is not valid v2 authority"};
		}

		inline void require_wal_source_authority(connection& database,
												 const wal_source_authority authority)
		{
			switch (authority)
			{
				case wal_source_authority::current_v3:
					require_v3_authority(database);
					return;
				case wal_source_authority::predecessor_v2:
					require_v2_authority(database);
					return;
			}
			throw std::runtime_error{"test SQLite WAL source authority is invalid"};
		}

		inline void commit_semantic_noop(connection& database, const wal_source_authority authority)
		{
			require_wal_source_authority(database, authority);
			const auto metadata_before =
				database.query("SELECT key,value FROM cxxlens_ng_metadata ORDER BY key");
			database.exec("PRAGMA wal_autocheckpoint=0");
			database.exec("PRAGMA synchronous=FULL");
			const auto journal_mode = database.query("PRAGMA journal_mode");
			const auto auto_checkpoint = database.query("PRAGMA wal_autocheckpoint");
			const auto synchronous = database.query("PRAGMA synchronous");
			if (journal_mode.size() != 1U || journal_mode.front().size() != 1U ||
				text(journal_mode.front().front()) != "wal" || auto_checkpoint.size() != 1U ||
				auto_checkpoint.front().size() != 1U ||
				integer(auto_checkpoint.front().front()) != 0 || synchronous.size() != 1U ||
				synchronous.front().size() != 1U || integer(synchronous.front().front()) != 2)
				throw std::runtime_error{"test SQLite WAL durability pragmas were not installed"};

			database.exec("BEGIN IMMEDIATE");
			try
			{
				database.exec("INSERT INTO cxxlens_ng_metadata(key,value) VALUES"
							  "('__cxxlens_test_wal_fixture_touch__','ephemeral');"
							  "DELETE FROM cxxlens_ng_metadata "
							  "WHERE key='__cxxlens_test_wal_fixture_touch__'");
				const auto changes = database.query("SELECT changes()");
				if (changes.size() != 1U || changes.front().size() != 1U ||
					integer(changes.front().front()) != 1)
					throw std::runtime_error{"test SQLite semantic no-op did not touch authority"};
				database.exec("COMMIT");
			}
			catch (...)
			{
				try
				{
					database.exec("ROLLBACK");
				}
				catch (...)
				{
				}
				throw;
			}
			require_wal_source_authority(database, authority);
			if (database.query("SELECT key,value FROM cxxlens_ng_metadata ORDER BY key") !=
				metadata_before)
				throw std::runtime_error{"test SQLite WAL transaction changed source semantics"};
		}

		inline void require_active_sidecars(const std::filesystem::path& main_path)
		{
			require_regular_file_larger_than(main_path, 0U);
			require_regular_file_larger_than(sidecar_path(main_path, "-wal"), 32U);
			require_regular_file_larger_than(sidecar_path(main_path, "-shm"), 0U);
			require_absent(sidecar_path(main_path, "-journal"));
		}

		inline void bounded_copy_file(const std::filesystem::path& source,
									  const std::filesystem::path& destination)
		{
			require_regular_file_larger_than(source, 0U);
			std::ifstream input{source, std::ios::binary};
			std::ofstream output{destination, std::ios::binary | std::ios::trunc};
			if (!input || !output)
				throw std::runtime_error{
					"test SQLite bounded file copy could not open its endpoints"};
			std::array<char, copy_buffer_bound> buffer{};
			while (input)
			{
				input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
				const auto count = input.gcount();
				if (count > 0)
					output.write(buffer.data(), count);
				if (input.bad() || !output)
					throw std::runtime_error{"test SQLite bounded file copy failed"};
			}
			output.close();
			if (!output || !std::filesystem::is_regular_file(destination) ||
				std::filesystem::file_size(destination) != std::filesystem::file_size(source))
				throw std::runtime_error{"test SQLite bounded file copy was not exact"};
		}

		class owned_database_files
		{
		  public:
			owned_database_files() = default;
			owned_database_files(const owned_database_files&) = delete;
			owned_database_files& operator=(const owned_database_files&) = delete;
			owned_database_files(owned_database_files&& other) noexcept
				: path_{std::move(other.path_)}, armed_{std::exchange(other.armed_, false)}
			{
			}
			owned_database_files& operator=(owned_database_files&&) = delete;
			~owned_database_files()
			{
				if (armed_)
					remove_sqlite_database_files(path_);
			}

			void arm(std::filesystem::path path)
			{
				if (armed_)
					throw std::runtime_error{"test SQLite fixture cleanup was already armed"};
				path_ = std::move(path);
				armed_ = true;
			}

		  private:
			std::filesystem::path path_;
			bool armed_{};
		};
	} // namespace wal_fixture_detail

	/**
	 * A valid selected-authority main+WAL+SHM source. Linux+OFD retains only a raw shared DMS lock
	 * after closing SQLite, preventing a same-process writable unixShmNode from weakening the
	 * READONLY/non-null qualification oracle. Other platforms retain the legacy live connection
	 * for cold-copy fixture construction only.
	 */
	class active_wal_sidecar_fixture
	{
	  public:
		explicit active_wal_sidecar_fixture(
			std::filesystem::path path,
			const wal_source_authority authority = wal_source_authority::current_v3)
			: path_{std::filesystem::absolute(std::move(path)).lexically_normal()},
			  database_{std::make_unique<connection>(path_)}
		{
			wal_fixture_detail::commit_semantic_noop(*database_, authority);
			database_->persist_wal();
			wal_fixture_detail::require_active_sidecars(path_);
#if defined(__linux__) && defined(F_OFD_SETLK)
			const auto shm_path = std::filesystem::path{path_.string() + "-shm"};
			const int descriptor =
				::open(shm_path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
			if (descriptor < 0)
				throw std::runtime_error{"test SQLite active SHM DMS open failed"};
			struct flock lock
			{
			};
			lock.l_type = F_RDLCK;
			lock.l_whence = SEEK_SET;
			lock.l_start = 128;
			lock.l_len = 1;
			if (::fcntl(descriptor, F_OFD_SETLK, &lock) != 0)
			{
				(void)::close(descriptor);
				throw std::runtime_error{"test SQLite active SHM DMS lock failed"};
			}
			try
			{
				database_->close();
				database_.reset();
				wal_fixture_detail::require_active_sidecars(path_);
			}
			catch (...)
			{
				(void)::close(descriptor);
				throw;
			}
			dms_descriptor_ = descriptor;
#else
			// The portable fixture retains its SQLite holder. Tests which require the qualified
			// READONLY/non-null profile are Linux+OFD-only and must expect typed unavailability
			// elsewhere rather than treating this same-process holder as qualification evidence.
#endif
		}

		active_wal_sidecar_fixture(const active_wal_sidecar_fixture&) = delete;
		active_wal_sidecar_fixture& operator=(const active_wal_sidecar_fixture&) = delete;
		active_wal_sidecar_fixture(active_wal_sidecar_fixture&&) = delete;
		active_wal_sidecar_fixture& operator=(active_wal_sidecar_fixture&&) = delete;
		~active_wal_sidecar_fixture()
		{
#if defined(__linux__) && defined(F_OFD_SETLK)
			if (dms_descriptor_ >= 0)
				(void)::close(dms_descriptor_);
#endif
		}

		[[nodiscard]] const std::filesystem::path& path() const noexcept
		{
			return path_;
		}

		void close()
		{
			if (database_)
			{
				database_->close();
				database_.reset();
			}
#if defined(__linux__) && defined(F_OFD_SETLK)
			if (dms_descriptor_ >= 0)
			{
				(void)::close(dms_descriptor_);
				dms_descriptor_ = -1;
			}
#endif
		}

	  private:
		std::filesystem::path path_;
		std::unique_ptr<connection> database_;
		int dms_descriptor_{-1};
	};

	/** A cold main+WAL/no-SHM copy. The destination file family is removed at destruction. */
	class cold_wal_only_copy_fixture
	{
	  public:
		cold_wal_only_copy_fixture(
			std::filesystem::path source_path,
			std::filesystem::path destination_path,
			const wal_source_authority authority = wal_source_authority::current_v3)
			: source_path_{std::filesystem::absolute(std::move(source_path)).lexically_normal()},
			  destination_path_{
				  std::filesystem::absolute(std::move(destination_path)).lexically_normal()}
		{
			if (source_path_ == destination_path_)
				throw std::runtime_error{"test SQLite cold WAL copy paths must be distinct"};
			for (const auto suffix : {std::string_view{},
									  std::string_view{"-wal"},
									  std::string_view{"-shm"},
									  std::string_view{"-journal"}})
				wal_fixture_detail::require_absent(
					wal_fixture_detail::sidecar_path(destination_path_, suffix));
			destination_cleanup_.arm(destination_path_);

			active_wal_sidecar_fixture source{source_path_, authority};
			wal_fixture_detail::bounded_copy_file(source.path(), destination_path_);
			wal_fixture_detail::bounded_copy_file(
				wal_fixture_detail::sidecar_path(source.path(), "-wal"),
				wal_fixture_detail::sidecar_path(destination_path_, "-wal"));
			wal_fixture_detail::require_absent(
				wal_fixture_detail::sidecar_path(destination_path_, "-shm"));
			source.close();
			require_cold_copy();
		}

		cold_wal_only_copy_fixture(const cold_wal_only_copy_fixture&) = delete;
		cold_wal_only_copy_fixture& operator=(const cold_wal_only_copy_fixture&) = delete;
		cold_wal_only_copy_fixture(cold_wal_only_copy_fixture&&) noexcept = default;
		cold_wal_only_copy_fixture& operator=(cold_wal_only_copy_fixture&&) = delete;
		~cold_wal_only_copy_fixture() = default;

		[[nodiscard]] const std::filesystem::path& source_path() const noexcept
		{
			return source_path_;
		}

		[[nodiscard]] const std::filesystem::path& path() const noexcept
		{
			return destination_path_;
		}

	  private:
		void require_cold_copy() const
		{
			wal_fixture_detail::require_regular_file_larger_than(destination_path_, 0U);
			wal_fixture_detail::require_regular_file_larger_than(
				wal_fixture_detail::sidecar_path(destination_path_, "-wal"), 32U);
			wal_fixture_detail::require_absent(
				wal_fixture_detail::sidecar_path(destination_path_, "-shm"));
			wal_fixture_detail::require_absent(
				wal_fixture_detail::sidecar_path(destination_path_, "-journal"));
		}

		std::filesystem::path source_path_;
		std::filesystem::path destination_path_;
		wal_fixture_detail::owned_database_files destination_cleanup_;
	};

	[[nodiscard]] inline cold_wal_only_copy_fixture
	make_cold_wal_only_copy(std::filesystem::path source_path,
							std::filesystem::path destination_path,
							const wal_source_authority authority = wal_source_authority::current_v3)
	{
		return cold_wal_only_copy_fixture{
			std::move(source_path), std::move(destination_path), authority};
	}

	inline void apply_quiescent_schema_mutation(const std::filesystem::path& path,
												std::string_view statements)
	{
		connection database{path};
		database.exec("BEGIN IMMEDIATE");
		try
		{
			database.exec(statements);
			database.exec("COMMIT");
		}
		catch (...)
		{
			try
			{
				database.exec("ROLLBACK");
			}
			catch (...)
			{
			}
			throw;
		}
		database.exec("PRAGMA wal_checkpoint(TRUNCATE)");
		database.close();
		require_wal_header_and_quiescent_sidecars(path);
	}

	inline void add_v3_chunk_layout_signal_to_v2(const std::filesystem::path& path)
	{
		apply_quiescent_schema_mutation(
			path,
			"CREATE TABLE cxxlens_ng_payload_chunk(publication_id TEXT NOT NULL,generation "
			"INTEGER NOT NULL,chunk_ordinal INTEGER NOT NULL,byte_offset INTEGER NOT "
			"NULL,byte_count INTEGER NOT NULL,checksum TEXT NOT NULL,payload BLOB NOT "
			"NULL,PRIMARY KEY(publication_id,generation,chunk_ordinal),CHECK(chunk_ordinal "
			"BETWEEN 0 AND 2199023255551),CHECK(byte_count BETWEEN 1 AND "
			"8388608),CHECK(length(payload)=byte_count)) STRICT, WITHOUT ROWID;"
			"CREATE INDEX cxxlens_ng_payload_chunk_locator ON "
			"cxxlens_ng_payload_chunk(publication_id,generation,chunk_ordinal);");
	}

	inline void add_v2_single_blob_layout_signal_to_empty_v3(const std::filesystem::path& path)
	{
		connection database{path, true};
		const auto counts = database.query("SELECT count(*) FROM cxxlens_ng_publication");
		database.close();
		if (counts.size() != 1U || counts.front().size() != 1U ||
			integer(counts.front().front()) != 0)
			throw std::runtime_error{"test mixed-layout v3 source is not empty"};
		apply_quiescent_schema_mutation(
			path,
			"DROP INDEX cxxlens_ng_publication_series;"
			"DROP TABLE cxxlens_ng_publication;"
			"CREATE TABLE cxxlens_ng_publication(publication_id TEXT NOT NULL PRIMARY "
			"KEY,series_id TEXT NOT NULL,snapshot_id TEXT NOT NULL,sequence INTEGER NOT "
			"NULL,generation INTEGER NOT NULL,parent TEXT,state INTEGER NOT NULL "
			"CHECK(state BETWEEN 0 AND 5),payload_checksum TEXT NOT "
			"NULL,payload_byte_count INTEGER NOT NULL,payload_chunk_count INTEGER NOT "
			"NULL CHECK(payload_chunk_count BETWEEN 0 AND 2199023255552),checksum TEXT NOT "
			"NULL,payload BLOB NOT NULL) STRICT, WITHOUT ROWID;"
			"CREATE INDEX cxxlens_ng_publication_series ON "
			"cxxlens_ng_publication(series_id,sequence);");
	}

	inline void make_v3_publication_series_index_descending(const std::filesystem::path& path)
	{
		apply_quiescent_schema_mutation(path,
										"DROP INDEX cxxlens_ng_publication_series;"
										"CREATE INDEX cxxlens_ng_publication_series ON "
										"cxxlens_ng_publication(series_id DESC,sequence);");
	}
} // namespace cxxlens::test::sqlite_fixture
