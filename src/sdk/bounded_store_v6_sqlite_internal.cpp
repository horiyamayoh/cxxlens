#include "bounded_store_v6_sqlite_internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

#include "sqlite_connection_lifecycle_internal.hpp"
#include "sqlite_payload_streaming_internal.hpp"
#include "store_identity_internal.hpp"

namespace cxxlens::sdk::detail
{
	namespace
	{
		constexpr std::string_view frame_domain{"cxxlens/df-0200-partition-event-frame/v1"};
		constexpr std::string_view semantic_domain{"cxxlens/df-0200-semantic-projection/v1"};
		constexpr std::string_view export_domain{"cxxlens/df-0200-canonical-export/v1"};
		constexpr std::size_t frame_prefix_bytes = 17U;
		constexpr std::size_t frame_checksum_bytes = 32U;
		constexpr std::size_t frame_fixed_bytes = frame_prefix_bytes + frame_checksum_bytes;
		constexpr std::string_view schema_version{"cxxlens.sqlite-bounded-store-v6.v1"};
		constexpr std::string_view stage_table{"cxxlens_v6_stage"};
		constexpr std::string_view record_table{"cxxlens_v6_record"};

		constexpr int sqlite_ok = 0;
		constexpr int sqlite_row = 100;
		constexpr int sqlite_done = 101;
		constexpr int sqlite_null = 5;
		constexpr int sqlite_integer = 1;
		constexpr int sqlite_blob = 4;
		constexpr int sqlite_open_readwrite = 0x00000002;
		constexpr int sqlite_open_create = 0x00000004;
		constexpr int sqlite_open_privatecache = 0x00040000;
		constexpr int sqlite_open_fullmutex = 0x00010000;

		[[nodiscard]] error failure(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] error invariant(std::string field, std::string detail)
		{
			return failure("store.invariant-breach", std::move(field), std::move(detail));
		}

		[[nodiscard]] error corrupt(std::string field, std::string detail)
		{
			return failure("store.corrupt", std::move(field), std::move(detail));
		}

		[[nodiscard]] error resource(std::string field, std::string detail)
		{
			return failure("store.resource-limit", std::move(field), std::move(detail));
		}

		[[nodiscard]] error sqlite_failure(std::string detail)
		{
			return failure("store.sqlite-failure", "database", std::move(detail));
		}

		[[nodiscard]] bool checked_add(const std::uint64_t left,
									   const std::uint64_t right,
									   std::uint64_t& output) noexcept
		{
			if (right > std::numeric_limits<std::uint64_t>::max() - left)
				return false;
			output = left + right;
			return true;
		}

		[[nodiscard]] bool checked_increment(std::uint64_t& value) noexcept
		{
			return checked_add(value, 1U, value);
		}

		void append_u64(std::array<std::byte, 8U>& output, const std::uint64_t value) noexcept
		{
			for (std::size_t index{}; index < output.size(); ++index)
				output[index] = static_cast<std::byte>(value >> (56U - index * 8U));
		}

		[[nodiscard]] std::uint64_t read_u64(const std::span<const std::byte> input,
											 const std::size_t offset) noexcept
		{
			std::uint64_t value{};
			for (std::size_t index{}; index < 8U; ++index)
				value = (value << 8U) |
					static_cast<std::uint64_t>(
							std::to_integer<unsigned char>(input[offset + index]));
			return value;
		}

		[[nodiscard]] result<std::array<std::byte, 32U>>
		digest_to_bytes(const std::string_view value)
		{
			if (!value.starts_with("sha256:") || value.size() != 71U)
				return unexpected(corrupt("digest", "spelling"));
			std::array<std::byte, 32U> output{};
			for (std::size_t index{}; index < output.size(); ++index)
			{
				const auto high = value[7U + index * 2U];
				const auto low = value[8U + index * 2U];
				const auto nibble = [](const char byte) -> std::optional<unsigned char>
				{
					if (byte >= '0' && byte <= '9')
						return static_cast<unsigned char>(byte - '0');
					if (byte >= 'a' && byte <= 'f')
						return static_cast<unsigned char>(byte - 'a' + 10);
					return std::nullopt;
				};
				auto high_value = nibble(high);
				auto low_value = nibble(low);
				if (!high_value || !low_value)
					return unexpected(corrupt("digest", "hex"));
				output[index] = static_cast<std::byte>((*high_value << 4U) | *low_value);
			}
			return output;
		}

		[[nodiscard]] bool canonical_digest(const std::string_view value) noexcept
		{
			if (!value.starts_with("sha256:") || value.size() != 71U)
				return false;
			for (const auto byte : value.substr(7U))
				if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
					return false;
			return true;
		}

		[[nodiscard]] std::string digest_string(const std::array<std::byte, 32U>& bytes)
		{
			static constexpr char hex[] = "0123456789abcdef";
			std::string output{"sha256:"};
			output.reserve(71U);
			for (const auto byte : bytes)
			{
				const auto value = std::to_integer<unsigned char>(byte);
				output.push_back(hex[value >> 4U]);
				output.push_back(hex[value & 0x0fU]);
			}
			return output;
		}

		[[nodiscard]] result<void> hash_update_identity(sqlite_incremental_sha256& hash,
														const std::string_view domain,
														const bounded_store_v6_record_kind kind,
														const std::span<const std::byte> key,
														const std::span<const std::byte> payload)
		{
			std::array<std::byte, 8U> length{};
			append_u64(length, domain.size());
			if (auto updated = hash.update(length); !updated)
				return unexpected(std::move(updated.error()));
			if (auto updated = hash.update(std::span<const std::byte>{
					reinterpret_cast<const std::byte*>(domain.data()), domain.size()});
				!updated)
				return unexpected(std::move(updated.error()));
			const std::array kind_bytes{static_cast<std::byte>(kind)};
			if (auto updated = hash.update(kind_bytes); !updated)
				return unexpected(std::move(updated.error()));
			append_u64(length, key.size());
			if (auto updated = hash.update(length); !updated)
				return unexpected(std::move(updated.error()));
			if (auto updated = hash.update(key); !updated)
				return unexpected(std::move(updated.error()));
			append_u64(length, payload.size());
			if (auto updated = hash.update(length); !updated)
				return unexpected(std::move(updated.error()));
			if (auto updated = hash.update(payload); !updated)
				return unexpected(std::move(updated.error()));
			return {};
		}

		[[nodiscard]] result<std::vector<std::byte>>
		frame_order_key(const bounded_store_v6_record_kind kind,
						const std::span<const std::byte> key,
						const std::span<const std::byte> payload)
		{
			return canonical_binary(canonical_value::from_tuple(
				{canonical_value::from_bytes({static_cast<std::byte>(kind)}),
				 canonical_value::from_bytes({key.begin(), key.end()}),
				 canonical_value::from_bytes({payload.begin(), payload.end()})}));
		}

		struct decoded_frame
		{
			bounded_store_v6_record_extent extent;
			std::vector<std::byte> order_key;
			std::span<const std::byte> key;
			std::span<const std::byte> payload;
		};

		[[nodiscard]] result<decoded_frame> decode_frame(const std::span<const std::byte> frame)
		{
			if (frame.size() < frame_fixed_bytes ||
				frame.size() > bounded_store_v6_record_buffer_bytes ||
				!is_valid(static_cast<bounded_store_v6_record_kind>(
					std::to_integer<unsigned char>(frame[0U]))))
				return unexpected(corrupt("frame", "shape"));
			const auto kind = static_cast<bounded_store_v6_record_kind>(
				std::to_integer<unsigned char>(frame[0U]));
			const auto key_bytes = read_u64(frame, 1U);
			const auto payload_bytes = read_u64(frame, 9U);
			std::uint64_t expected{};
			if (!checked_add(frame_fixed_bytes, key_bytes, expected) ||
				!checked_add(expected, payload_bytes, expected) || expected != frame.size() ||
				key_bytes > static_cast<std::uint64_t>(frame.size()) ||
				payload_bytes > static_cast<std::uint64_t>(frame.size()))
				return unexpected(corrupt("frame", "length"));
			const auto projection_bytes = frame.size() - frame_checksum_bytes;
			const auto supplied = frame.subspan(projection_bytes, frame_checksum_bytes);
			sqlite_incremental_sha256 checksum;
			std::array<std::byte, 8U> length{};
			append_u64(length, frame_domain.size());
			if (auto updated = checksum.update(length); !updated)
				return unexpected(std::move(updated.error()));
			if (auto updated = checksum.update(std::span<const std::byte>{
					reinterpret_cast<const std::byte*>(frame_domain.data()), frame_domain.size()});
				!updated)
				return unexpected(std::move(updated.error()));
			append_u64(length, projection_bytes);
			if (auto updated = checksum.update(length); !updated)
				return unexpected(std::move(updated.error()));
			if (auto updated = checksum.update(frame.first(projection_bytes)); !updated)
				return unexpected(std::move(updated.error()));
			auto computed_digest = checksum.finish();
			if (!computed_digest)
				return unexpected(corrupt("frame", "checksum"));
			auto computed_bytes = digest_to_bytes(*computed_digest);
			if (!computed_bytes || std::ranges::equal(*computed_bytes, supplied) == false)
				return unexpected(corrupt("frame", "checksum"));
			const auto key = frame.subspan(frame_prefix_bytes, static_cast<std::size_t>(key_bytes));
			const auto payload = frame.subspan(frame_prefix_bytes + key_bytes,
											   static_cast<std::size_t>(payload_bytes));
			auto decoded_key = canonical_binary_decode(key);
			auto decoded_payload = canonical_binary_decode(payload);
			if (!decoded_key || !decoded_payload ||
				decoded_key->type != canonical_value::kind::ordered_tuple ||
				decoded_payload->type != canonical_value::kind::ordered_tuple)
				return unexpected(corrupt("frame", "semantic-tuple"));
			auto order = frame_order_key(kind, key, payload);
			if (!order)
				return unexpected(std::move(order.error()));
			return decoded_frame{
				{kind, key_bytes, payload_bytes, static_cast<std::uint64_t>(frame.size())},
				std::move(*order),
				key,
				payload};
		}

		struct sqlite_api
		{
			using open_fn = int (*)(const char*, void**, int, const char*);
			using close_fn = int (*)(void*);
			using errmsg_fn = const char* (*)(void*);
			using exec_fn =
				int (*)(void*, const char*, int (*)(void*, int, char**, char**), void*, char**);
			using free_fn = void (*)(void*);
			using prepare_fn = int (*)(void*, const char*, int, void**, const char**);
			using step_fn = int (*)(void*);
			using finalize_fn = int (*)(void*);
			using bind_text_fn = int (*)(void*, int, const char*, int, void (*)(void*));
			using bind_int64_fn = int (*)(void*, int, std::int64_t);
			using bind_blob64_fn = int (*)(void*, int, const void*, std::uint64_t, void (*)(void*));
			using bind_null_fn = int (*)(void*, int);
			using column_type_fn = int (*)(void*, int);
			using column_blob_fn = const void* (*)(void*, int);
			using column_bytes_fn = int (*)(void*, int);
			using column_int64_fn = std::int64_t (*)(void*, int);
			using column_text_fn = const unsigned char* (*)(void*, int);
			void* library{};
			open_fn open{};
			close_fn close{};
			errmsg_fn errmsg{};
			exec_fn exec{};
			free_fn free_memory{};
			prepare_fn prepare{};
			step_fn step{};
			finalize_fn finalize{};
			bind_text_fn bind_text{};
			bind_int64_fn bind_int64{};
			bind_blob64_fn bind_blob64{};
			bind_null_fn bind_null{};
			column_type_fn column_type{};
			column_blob_fn column_blob{};
			column_bytes_fn column_bytes{};
			column_int64_fn column_int64{};
			column_text_fn column_text{};
			bool owns_library{};
			~sqlite_api() noexcept
			{
#if defined(__unix__) || defined(__APPLE__)
				if (owns_library && library != nullptr)
					(void)dlclose(library);
#endif
			}
		};

		template <class Function>
		[[nodiscard]] bool resolve(void* library, const char* name, Function& output)
		{
#if defined(__unix__) || defined(__APPLE__)
			if (library == nullptr)
				return false;
			void* symbol = dlsym(library, name);
			if (symbol == nullptr)
				return false;
			output = reinterpret_cast<Function>(symbol);
			return true;
#else
			(void)library;
			(void)name;
			(void)output;
			return false;
#endif
		}

		[[nodiscard]] result<std::shared_ptr<sqlite_api>> load_sqlite()
		{
#if defined(__unix__) || defined(__APPLE__)
#if defined(__APPLE__)
			constexpr std::array candidates{"libsqlite3.dylib", "/usr/lib/libsqlite3.dylib"};
#else
			constexpr std::array candidates{"libsqlite3.so.0", "libsqlite3.so"};
#endif
			void* library{};
			for (const auto* candidate : candidates)
			{
				library = dlopen(candidate, RTLD_NOW | RTLD_LOCAL);
				if (library != nullptr)
					break;
			}
			if (library == nullptr)
				return unexpected(sqlite_failure("library"));
			auto api = std::make_shared<sqlite_api>();
			api->library = library;
			api->owns_library = true;
			if (!resolve(library, "sqlite3_open_v2", api->open) ||
				!resolve(library, "sqlite3_close_v2", api->close) ||
				!resolve(library, "sqlite3_errmsg", api->errmsg) ||
				!resolve(library, "sqlite3_exec", api->exec) ||
				!resolve(library, "sqlite3_free", api->free_memory) ||
				!resolve(library, "sqlite3_prepare_v2", api->prepare) ||
				!resolve(library, "sqlite3_step", api->step) ||
				!resolve(library, "sqlite3_finalize", api->finalize) ||
				!resolve(library, "sqlite3_bind_text", api->bind_text) ||
				!resolve(library, "sqlite3_bind_int64", api->bind_int64) ||
				!resolve(library, "sqlite3_bind_blob64", api->bind_blob64) ||
				!resolve(library, "sqlite3_bind_null", api->bind_null) ||
				!resolve(library, "sqlite3_column_type", api->column_type) ||
				!resolve(library, "sqlite3_column_blob", api->column_blob) ||
				!resolve(library, "sqlite3_column_bytes", api->column_bytes) ||
				!resolve(library, "sqlite3_column_int64", api->column_int64) ||
				!resolve(library, "sqlite3_column_text", api->column_text))
				return unexpected(sqlite_failure("symbols"));
			return api;
#else
			return unexpected(sqlite_failure("platform"));
#endif
		}

		struct sqlite_database
		{
			std::shared_ptr<sqlite_api> api;
			std::unique_ptr<sqlite_connection_lifecycle> connection;

			[[nodiscard]] void* handle() const noexcept
			{
				return connection ? connection->get() : nullptr;
			}
			[[nodiscard]] std::string message() const
			{
				const auto* value =
					api && api->errmsg && handle() ? api->errmsg(handle()) : nullptr;
				return value != nullptr ? value : "sqlite";
			}
			[[nodiscard]] result<void> execute(const std::string_view sql) const
			{
				if (!api || !handle() ||
					sql.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
					return unexpected(sqlite_failure("connection"));
				char* message{};
				const auto code =
					api->exec(handle(), std::string{sql}.c_str(), nullptr, nullptr, &message);
				if (code == sqlite_ok)
					return {};
				std::string detail = message != nullptr ? message : this->message();
				if (message != nullptr)
					api->free_memory(message);
				return unexpected(sqlite_failure(std::move(detail)));
			}
		};

		[[nodiscard]] result<std::shared_ptr<sqlite_database>>
		open_database(const std::shared_ptr<sqlite_api>& api, const std::string& path)
		{
			if (!api || path.empty())
				return unexpected(sqlite_failure("path"));
			auto output = std::make_shared<sqlite_database>();
			output->api = api;
			output->connection = std::make_unique<sqlite_connection_lifecycle>(
				nullptr,
				api->close,
				sqlite_connection_lifetime_pins{std::static_pointer_cast<void>(api), {}, {}, {}});
			void** slot = output->connection->open_handle_out_parameter();
			const auto code = api->open(path.c_str(),
										slot,
										sqlite_open_readwrite | sqlite_open_create |
											sqlite_open_privatecache | sqlite_open_fullmutex,
										nullptr);
			if (code != sqlite_ok || output->handle() == nullptr)
			{
				const auto detail = output->handle() != nullptr ? output->message() : "open";
				(void)output->connection->close_exactly_once();
				return unexpected(sqlite_failure(detail));
			}
			return output;
		}

		class sqlite_statement final
		{
		  public:
			sqlite_statement(std::shared_ptr<sqlite_database> database, void* statement)
				: database_{std::move(database)}, statement_{statement}
			{
			}
			sqlite_statement(const sqlite_statement&) = delete;
			sqlite_statement& operator=(const sqlite_statement&) = delete;
			sqlite_statement(sqlite_statement&& other) noexcept
				: database_{std::move(other.database_)},
				  statement_{std::exchange(other.statement_, nullptr)}
			{
			}
			sqlite_statement& operator=(sqlite_statement&& other) noexcept
			{
				if (this == &other)
					return *this;
				reset();
				database_ = std::move(other.database_);
				statement_ = std::exchange(other.statement_, nullptr);
				return *this;
			}
			~sqlite_statement() noexcept
			{
				reset();
			}

			[[nodiscard]] static result<sqlite_statement>
			prepare(const std::shared_ptr<sqlite_database>& database, const std::string_view sql)
			{
				if (!database || !database->api || !database->handle() ||
					sql.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
					return unexpected(sqlite_failure("prepare"));
				void* statement{};
				const auto code = database->api->prepare(database->handle(),
														 sql.data(),
														 static_cast<int>(sql.size()),
														 &statement,
														 nullptr);
				if (code != sqlite_ok || statement == nullptr)
					return unexpected(sqlite_failure(database->message()));
				return sqlite_statement{database, statement};
			}

			[[nodiscard]] result<void> bind_text(const int index, const std::string_view value)
			{
				if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
					database_->api->bind_text(statement_,
											  index,
											  value.data(),
											  static_cast<int>(value.size()),
											  transient()) != sqlite_ok)
					return unexpected(sqlite_failure(database_->message()));
				return {};
			}
			[[nodiscard]] result<void> bind_int64(const int index, const std::int64_t value)
			{
				if (database_->api->bind_int64(statement_, index, value) != sqlite_ok)
					return unexpected(sqlite_failure(database_->message()));
				return {};
			}
			[[nodiscard]] result<void> bind_blob(const int index,
												 const std::span<const std::byte> value)
			{
				const void* data = value.empty() ? nullptr : value.data();
				if (database_->api->bind_blob64(
						statement_, index, data, value.size(), transient()) != sqlite_ok)
					return unexpected(sqlite_failure(database_->message()));
				return {};
			}
			[[nodiscard]] result<void> bind_null(const int index)
			{
				if (database_->api->bind_null(statement_, index) != sqlite_ok)
					return unexpected(sqlite_failure(database_->message()));
				return {};
			}
			[[nodiscard]] int step() const
			{
				return database_->api->step(statement_);
			}
			[[nodiscard]] result<void> done() const
			{
				if (step() != sqlite_done)
					return unexpected(sqlite_failure(database_->message()));
				return {};
			}
			[[nodiscard]] int type(const int index) const
			{
				return database_->api->column_type(statement_, index);
			}
			[[nodiscard]] result<std::span<const std::byte>> blob_view(const int index) const
			{
				if (type(index) != sqlite_blob)
					return unexpected(corrupt("sqlite", "storage-class"));
				const auto bytes = database_->api->column_bytes(statement_, index);
				const auto* value = database_->api->column_blob(statement_, index);
				if (bytes < 0 || (bytes != 0 && value == nullptr))
					return unexpected(corrupt("sqlite", "blob"));
				return std::span{static_cast<const std::byte*>(value),
								 static_cast<std::size_t>(bytes)};
			}
			[[nodiscard]] result<std::string> text(const int index) const
			{
				if (type(index) != 3) // SQLITE_TEXT
					return unexpected(corrupt("sqlite", "storage-class"));
				const auto bytes = database_->api->column_bytes(statement_, index);
				const auto* value = database_->api->column_text(statement_, index);
				if (bytes < 0 || (bytes != 0 && value == nullptr))
					return unexpected(corrupt("sqlite", "text"));
				return std::string{reinterpret_cast<const char*>(value),
								   static_cast<std::size_t>(bytes)};
			}
			[[nodiscard]] result<std::optional<std::string>> optional_text(const int index) const
			{
				if (type(index) == sqlite_null)
					return std::optional<std::string>{};
				auto value = text(index);
				if (!value)
					return unexpected(std::move(value.error()));
				return std::optional<std::string>{std::move(*value)};
			}
			[[nodiscard]] result<std::int64_t> signed_value(const int index) const
			{
				if (type(index) != sqlite_integer)
					return unexpected(corrupt("sqlite", "storage-class"));
				return database_->api->column_int64(statement_, index);
			}

		  private:
			static void (*transient())(void*)
			{
				// SQLite's SQLITE_TRANSIENT is the never-invoked all-bits-one callback.
				return reinterpret_cast<void (*)(void*)>(static_cast<std::intptr_t>(-1));
			}
			void reset() noexcept
			{
				if (statement_ != nullptr && database_ && database_->api)
					(void)database_->api->finalize(statement_);
				statement_ = nullptr;
			}
			std::shared_ptr<sqlite_database> database_;
			void* statement_{};
		};

		[[nodiscard]] result<void>
		initialize_schema(const std::shared_ptr<sqlite_database>& database)
		{
			if (!database)
				return unexpected(sqlite_failure("connection"));
			for (const auto sql : std::array<std::string_view, 8U>{
					 "CREATE TABLE IF NOT EXISTS cxxlens_v6_metadata(key TEXT NOT NULL PRIMARY "
					 "KEY,value TEXT NOT NULL) STRICT, WITHOUT ROWID",
					 "CREATE TABLE IF NOT EXISTS cxxlens_v6_stage(session_id TEXT NOT NULL,ordinal "
					 "INTEGER NOT NULL,kind INTEGER NOT NULL,key BLOB NOT NULL,payload BLOB NOT "
					 "NULL,frame BLOB NOT NULL,order_key BLOB NOT NULL,PRIMARY "
					 "KEY(session_id,ordinal)) STRICT, WITHOUT ROWID",
					 "CREATE TABLE IF NOT EXISTS cxxlens_v6_publication(publication_id TEXT NOT "
					 "NULL PRIMARY KEY,series_id TEXT NOT NULL,snapshot_id TEXT NOT NULL,sequence "
					 "INTEGER NOT NULL,generation INTEGER NOT NULL,parent TEXT,state INTEGER NOT "
					 "NULL,corrupt INTEGER NOT NULL,partition_count INTEGER NOT NULL,row_count "
					 "INTEGER NOT NULL,claim_count INTEGER NOT NULL,coverage_count INTEGER NOT "
					 "NULL,unresolved_count INTEGER NOT NULL,semantic_digest TEXT NOT "
					 "NULL,export_digest TEXT NOT NULL,physical_digest TEXT NOT NULL,record_count "
					 "INTEGER NOT NULL,framed_bytes INTEGER NOT NULL,immutable_binding TEXT NOT "
					 "NULL) STRICT, WITHOUT ROWID",
					 "CREATE TABLE IF NOT EXISTS cxxlens_v6_record(publication_id TEXT NOT "
					 "NULL,ordinal INTEGER NOT NULL,kind INTEGER NOT NULL,key BLOB NOT "
					 "NULL,payload BLOB NOT NULL,frame BLOB NOT NULL,order_key BLOB NOT "
					 "NULL,PRIMARY KEY(publication_id,ordinal)) STRICT, WITHOUT ROWID",
					 "CREATE TABLE IF NOT EXISTS cxxlens_v6_head(series_id TEXT NOT NULL PRIMARY "
					 "KEY,publication_id TEXT NOT NULL,sequence INTEGER NOT NULL) STRICT, WITHOUT "
					 "ROWID",
					 "CREATE INDEX IF NOT EXISTS cxxlens_v6_stage_session ON "
					 "cxxlens_v6_stage(session_id,ordinal)",
					 "CREATE INDEX IF NOT EXISTS cxxlens_v6_record_publication ON "
					 "cxxlens_v6_record(publication_id,ordinal)",
					 "CREATE INDEX IF NOT EXISTS cxxlens_v6_publication_series ON "
					 "cxxlens_v6_publication(series_id,sequence)",
				 })
				if (auto created = database->execute(sql); !created)
					return created;
			auto marker = sqlite_statement::prepare(
				database,
				"INSERT OR IGNORE INTO cxxlens_v6_metadata(key,value) VALUES('schema',?1)");
			if (!marker)
				return unexpected(std::move(marker.error()));
			if (auto bound = marker->bind_text(1, schema_version); !bound)
				return bound;
			if (auto done = marker->done(); !done)
				return done;
			auto selected = sqlite_statement::prepare(
				database, "SELECT value FROM cxxlens_v6_metadata WHERE key='schema'");
			if (!selected)
				return unexpected(std::move(selected.error()));
			if (selected->step() != sqlite_row)
				return unexpected(sqlite_failure("schema-marker"));
			auto value = selected->text(0);
			if (!value || *value != schema_version || selected->step() != sqlite_done)
				return unexpected(corrupt("sqlite", "v6-schema-marker"));
			return {};
		}

		struct scan_result
		{
			std::uint64_t record_count{};
			std::uint64_t framed_bytes{};
			std::array<std::byte, 32U> binary_sha256{};
			std::string binary_digest;
			bounded_store_v6_snapshot_observation snapshot;
			std::string immutable_binding;
		};

		[[nodiscard]] result<std::string> identity_binding(const std::string_view prefix,
														   const std::string_view selector_id,
														   const std::string_view object_id,
														   const std::string_view physical_digest,
														   const std::uint64_t record_count,
														   const std::uint64_t framed_bytes)
		{
			return canonical_identity_digest(
				"cxxlens.df-0200.sqlite-physical-binding.v1",
				std::array{canonical_value::from_string(std::string{prefix}),
						   canonical_value::from_string(std::string{selector_id}),
						   canonical_value::from_string(std::string{object_id}),
						   canonical_value::from_string(std::string{physical_digest}),
						   canonical_value::from_integer(static_cast<std::int64_t>(record_count)),
						   canonical_value::from_integer(static_cast<std::int64_t>(framed_bytes))});
		}

		[[nodiscard]] result<scan_result>
		scan_rows(const std::shared_ptr<sqlite_database>& database,
				  const std::string_view table,
				  const std::string_view id,
				  const snapshot_series_selector& selector)
		{
			if (!database || (table != stage_table && table != record_table))
				return unexpected(invariant("scan", "table"));
			const std::string sql = table == stage_table
				? "SELECT kind,key,payload,frame,order_key FROM cxxlens_v6_stage WHERE "
				  "session_id=?1 ORDER BY ordinal"
				: "SELECT kind,key,payload,frame,order_key FROM cxxlens_v6_record WHERE "
				  "publication_id=?1 ORDER BY ordinal";
			auto selected = sqlite_statement::prepare(database, sql);
			if (!selected)
				return unexpected(std::move(selected.error()));
			if (auto bound = selected->bind_text(1, id); !bound)
				return unexpected(std::move(bound.error()));

			sqlite_incremental_sha256 binary_hash;
			sqlite_incremental_sha256 semantic_hash;
			sqlite_incremental_sha256 export_hash;
			std::vector<std::byte> previous_order;
			bool began{};
			bool ended{};
			scan_result output;
			for (;;)
			{
				const auto code = selected->step();
				if (code == sqlite_done)
					break;
				if (code != sqlite_row)
					return unexpected(sqlite_failure(database->message()));
				auto kind_value = selected->signed_value(0);
				auto key = selected->blob_view(1);
				auto payload = selected->blob_view(2);
				auto frame = selected->blob_view(3);
				auto stored_order = selected->blob_view(4);
				if (!kind_value || !key || !payload || !frame || !stored_order || *kind_value < 1 ||
					*kind_value > 7)
					return unexpected(corrupt("sqlite", "record-columns"));
				std::vector<std::byte> frame_copy{frame->begin(), frame->end()};
				auto decoded = decode_frame(frame_copy);
				if (!decoded || static_cast<std::int64_t>(decoded->extent.kind) != *kind_value ||
					!std::ranges::equal(decoded->key, *key) ||
					!std::ranges::equal(decoded->payload, *payload) ||
					decoded->order_key !=
						std::vector<std::byte>{stored_order->begin(), stored_order->end()})
					return unexpected(corrupt("sqlite", "record-authentication"));
				const auto kind = decoded->extent.kind;
				if ((!began || ended) && kind != bounded_store_v6_record_kind::partition_begin)
					return unexpected(corrupt("sqlite", "partition-begin"));
				if (kind == bounded_store_v6_record_kind::partition_begin && began && !ended)
					return unexpected(corrupt("sqlite", "nested-partition"));
				if (ended && kind == bounded_store_v6_record_kind::partition_begin)
					previous_order.clear();
				if (!previous_order.empty() &&
					!std::lexicographical_compare(previous_order.begin(),
												  previous_order.end(),
												  decoded->order_key.begin(),
												  decoded->order_key.end()))
					return unexpected(corrupt("sqlite", "physical-order"));
				previous_order = decoded->order_key;
				if (auto updated = binary_hash.update(frame_copy); !updated)
					return unexpected(std::move(updated.error()));
				if (auto updated = hash_update_identity(
						semantic_hash, semantic_domain, kind, decoded->key, decoded->payload);
					!updated)
					return unexpected(std::move(updated.error()));
				if (auto updated = hash_update_identity(
						export_hash, export_domain, kind, decoded->key, decoded->payload);
					!updated)
					return unexpected(std::move(updated.error()));
				if (!checked_increment(output.record_count) ||
					!checked_add(
						output.framed_bytes, decoded->extent.framed_bytes, output.framed_bytes) ||
					output.framed_bytes > bounded_store_v6_max_aggregate_bytes)
					return unexpected(resource("scan", "counter"));
				began = true;
				ended = kind == bounded_store_v6_record_kind::partition_end;
				switch (kind)
				{
					case bounded_store_v6_record_kind::partition_begin:
						if (!checked_increment(output.snapshot.partition_count))
							return unexpected(resource("partition-count", "counter"));
						break;
					case bounded_store_v6_record_kind::claim_occurrence:
						if (!checked_increment(output.snapshot.claim_count))
							return unexpected(resource("claim-count", "counter"));
						break;
					case bounded_store_v6_record_kind::detached_row:
						if (!checked_increment(output.snapshot.row_count))
							return unexpected(resource("row-count", "counter"));
						break;
					case bounded_store_v6_record_kind::coverage:
						if (!checked_increment(output.snapshot.coverage_count))
							return unexpected(resource("coverage-count", "counter"));
						break;
					case bounded_store_v6_record_kind::unresolved:
						if (!checked_increment(output.snapshot.unresolved_count))
							return unexpected(resource("unresolved-count", "counter"));
						break;
					case bounded_store_v6_record_kind::claim_annotation:
					case bounded_store_v6_record_kind::partition_end:
						break;
				}
			}
			if (!began || !ended || output.record_count == 0U)
				return unexpected(corrupt("sqlite", "partition-end"));
			auto binary_digest = binary_hash.finish();
			auto semantic_digest_value = semantic_hash.finish();
			auto export_digest_value = export_hash.finish();
			if (!binary_digest || !semantic_digest_value || !export_digest_value)
				return unexpected(sqlite_failure("digest"));
			output.binary_digest = *binary_digest;
			output.snapshot.semantic_projection_digest = *semantic_digest_value;
			output.snapshot.canonical_export_digest = *export_digest_value;
			const auto fields = std::array{
				canonical_value::from_string(selector.id()),
				canonical_value::from_string(output.snapshot.semantic_projection_digest),
				canonical_value::from_string(output.snapshot.canonical_export_digest),
				canonical_value::from_string(std::to_string(output.snapshot.partition_count)),
				canonical_value::from_string(std::to_string(output.snapshot.claim_count)),
				canonical_value::from_string(std::to_string(output.snapshot.row_count)),
				canonical_value::from_string(std::to_string(output.snapshot.coverage_count)),
				canonical_value::from_string(std::to_string(output.snapshot.unresolved_count)),
			};
			auto snapshot_id =
				canonical_identity_digest("cxxlens.df-0200.memory-reference-snapshot.v1", fields);
			if (!snapshot_id)
				return unexpected(std::move(snapshot_id.error()));
			output.snapshot.snapshot_id = std::move(*snapshot_id);
			auto binding = identity_binding(table == stage_table ? "stage" : "publication",
											selector.id(),
											id,
											output.binary_digest,
											output.record_count,
											output.framed_bytes);
			if (!binding)
				return unexpected(std::move(binding.error()));
			output.immutable_binding = std::move(*binding);
			auto binary = digest_to_bytes(output.binary_digest);
			if (!binary)
				return unexpected(std::move(binary.error()));
			output.binary_sha256 = *binary;
			return output;
		}

		class sqlite_actual_cursor final : public bounded_store_v6_actual_cursor_source
		{
		  public:
			[[nodiscard]] static result<std::unique_ptr<sqlite_actual_cursor>>
			open(std::shared_ptr<sqlite_database> database,
				 const std::string_view session_id,
				 std::shared_ptr<const bounded_store_v6_physical_anchor> anchor,
				 std::string anchor_binding)
			{
				auto selected = sqlite_statement::prepare(
					database,
					"SELECT kind,key,payload,frame,order_key FROM cxxlens_v6_stage WHERE "
					"session_id=?1 ORDER BY ordinal");
				if (!selected)
					return unexpected(std::move(selected.error()));
				if (auto bound = selected->bind_text(1, session_id); !bound)
					return unexpected(std::move(bound.error()));
				return std::unique_ptr<sqlite_actual_cursor>{
					new sqlite_actual_cursor{std::move(database),
											 std::move(*selected),
											 std::move(anchor),
											 std::move(anchor_binding)}};
			}

			[[nodiscard]] result<std::optional<bounded_store_v6_record_extent>>
			next_record() override
			{
				if (finished_ || !statement_ ||
					(!current_.empty() && current_offset_ != current_.size()))
					return unexpected(corrupt("physical-cursor", "phase"));
				current_.clear();
				current_offset_ = 0U;
				const auto code = statement_->step();
				if (code == sqlite_done)
				{
					eof_ = true;
					return std::optional<bounded_store_v6_record_extent>{};
				}
				if (code != sqlite_row)
					return unexpected(sqlite_failure(database_->message()));
				auto kind_value = statement_->signed_value(0);
				auto key = statement_->blob_view(1);
				auto payload = statement_->blob_view(2);
				auto frame = statement_->blob_view(3);
				auto stored_order = statement_->blob_view(4);
				if (!kind_value || *kind_value < 1 || *kind_value > 7 || !key || !payload ||
					!frame || !stored_order)
					return unexpected(corrupt("physical-cursor", "record-columns"));
				current_.assign(frame->begin(), frame->end());
				auto decoded = decode_frame(current_);
				if (!decoded || static_cast<std::int64_t>(decoded->extent.kind) != *kind_value ||
					!std::ranges::equal(decoded->key, *key) ||
					!std::ranges::equal(decoded->payload, *payload) ||
					decoded->order_key !=
						std::vector<std::byte>{stored_order->begin(), stored_order->end()})
					return unexpected(corrupt("physical-cursor", "record-authentication"));
				const auto kind = decoded->extent.kind;
				if ((!began_ || ended_) && kind != bounded_store_v6_record_kind::partition_begin)
					return unexpected(corrupt("physical-cursor", "partition-begin"));
				if (kind == bounded_store_v6_record_kind::partition_begin && began_ && !ended_)
					return unexpected(corrupt("physical-cursor", "nested-partition"));
				if (ended_ && kind == bounded_store_v6_record_kind::partition_begin)
					previous_order_.clear();
				if (!previous_order_.empty() &&
					!std::lexicographical_compare(previous_order_.begin(),
												  previous_order_.end(),
												  decoded->order_key.begin(),
												  decoded->order_key.end()))
					return unexpected(corrupt("physical-cursor", "order"));
				previous_order_ = decoded->order_key;
				if (auto updated = binary_hash_.update(current_); !updated)
					return unexpected(std::move(updated.error()));
				if (!checked_increment(record_count_) ||
					!checked_add(framed_bytes_, decoded->extent.framed_bytes, framed_bytes_) ||
					framed_bytes_ > bounded_store_v6_max_aggregate_bytes)
					return unexpected(resource("physical-cursor", "counter"));
				began_ = true;
				ended_ = kind == bounded_store_v6_record_kind::partition_end;
				return std::optional<bounded_store_v6_record_extent>{decoded->extent};
			}

			[[nodiscard]] result<std::size_t>
			read_record_bytes(const std::span<std::byte> destination) override
			{
				if (destination.empty())
					return unexpected(invariant("physical-cursor", "empty-window"));
				if (current_.empty())
					return unexpected(corrupt("physical-cursor", "missing-record"));
				if (current_offset_ == current_.size())
					return std::size_t{};
				const auto count = std::min(destination.size(), current_.size() - current_offset_);
				std::copy_n(current_.data() + current_offset_, count, destination.data());
				current_offset_ += count;
				return count;
			}

			[[nodiscard]] std::shared_ptr<const bounded_store_v6_physical_anchor>
			physical_anchor() const noexcept override
			{
				return anchor_;
			}

			[[nodiscard]] result<bounded_store_v6_physical_cursor_observation> finish() override
			{
				if (finished_ || !eof_ ||
					(!current_.empty() && current_offset_ != current_.size()) || !began_ || !ended_)
					return unexpected(corrupt("physical-cursor", "unfinished"));
				finished_ = true;
				auto digest = binary_hash_.finish();
				if (!digest)
					return unexpected(std::move(digest.error()));
				auto binary = digest_to_bytes(*digest);
				if (!binary)
					return unexpected(std::move(binary.error()));
				return bounded_store_v6_physical_cursor_observation{
					record_count_, framed_bytes_, *binary, anchor_binding_, true};
			}

		  private:
			sqlite_actual_cursor(std::shared_ptr<sqlite_database> database,
								 sqlite_statement statement,
								 std::shared_ptr<const bounded_store_v6_physical_anchor> anchor,
								 std::string anchor_binding)
				: database_{std::move(database)}, statement_{std::move(statement)},
				  anchor_{std::move(anchor)}, anchor_binding_{std::move(anchor_binding)}
			{
			}

			std::shared_ptr<sqlite_database> database_;
			std::optional<sqlite_statement> statement_;
			std::shared_ptr<const bounded_store_v6_physical_anchor> anchor_;
			std::string anchor_binding_;
			std::vector<std::byte> current_;
			std::vector<std::byte> previous_order_;
			sqlite_incremental_sha256 binary_hash_;
			std::size_t current_offset_{};
			std::uint64_t record_count_{};
			std::uint64_t framed_bytes_{};
			bool began_{};
			bool ended_{};
			bool eof_{};
			bool finished_{};
		};

		class sqlite_bounded_store_v6_backend final : public bounded_store_v6_backend_port
		{
		  public:
			sqlite_bounded_store_v6_backend(bounded_store_v6_session_metadata metadata,
											std::shared_ptr<sqlite_database> database)
				: metadata_{std::move(metadata)}, database_{std::move(database)}
			{
			}
			~sqlite_bounded_store_v6_backend() override = default;

			[[nodiscard]] bounded_store_v6_backend backend() const noexcept override
			{
				return bounded_store_v6_backend::sqlite;
			}

			[[nodiscard]] result<void> bind_physical_anchor(
				std::shared_ptr<const bounded_store_v6_physical_anchor> anchor) override
			{
				if (!anchor || anchor_ || !database_ || transaction_open_)
					return unexpected(invariant("sqlite", "anchor-phase"));
				auto binding = canonical_identity_digest(
					"cxxlens.df-0200.sqlite-physical-anchor.v1",
					std::array{canonical_value::from_string(metadata_.staging_session_id),
							   canonical_value::from_string(metadata_.selector.id()),
							   canonical_value::from_string(
								   metadata_.exact_sqlite_path.value_or(std::string{}))});
				if (!binding)
					return unexpected(std::move(binding.error()));
				if (auto begun = database_->execute("BEGIN IMMEDIATE;"); !begun)
					return begun;
				auto clear = sqlite_statement::prepare(
					database_, "DELETE FROM cxxlens_v6_stage WHERE session_id=?1");
				if (!clear)
				{
					(void)database_->execute("ROLLBACK;");
					return unexpected(std::move(clear.error()));
				}
				if (auto bound = clear->bind_text(1, metadata_.staging_session_id); !bound)
				{
					(void)database_->execute("ROLLBACK;");
					return bound;
				}
				if (auto done = clear->done(); !done)
				{
					(void)database_->execute("ROLLBACK;");
					return done;
				}
				anchor_ = std::move(anchor);
				anchor_binding_ = std::move(*binding);
				transaction_open_ = true;
				ordinal_ = 0U;
				return {};
			}

			[[nodiscard]] std::shared_ptr<const bounded_store_v6_physical_anchor>
			physical_anchor() const noexcept override
			{
				return anchor_;
			}

			[[nodiscard]] std::string_view physical_anchor_binding() const noexcept override
			{
				return anchor_binding_;
			}

			[[nodiscard]] result<void>
			begin_record(const bounded_store_v6_record_extent& extent) override
			{
				if (!transaction_open_ || sealed_ || record_open_ || !is_valid(extent.kind))
					return unexpected(invariant("sqlite", "record-phase"));
				auto checked = checked_bounded_store_v6_record_frame_bytes(extent.key_bytes,
																		   extent.payload_bytes);
				if (!checked || *checked != extent.framed_bytes ||
					*checked > bounded_store_v6_record_buffer_bytes)
					return unexpected(resource("record", "window"));
				current_extent_ = extent;
				current_.clear();
				current_.reserve(static_cast<std::size_t>(extent.framed_bytes));
				record_open_ = true;
				return {};
			}

			[[nodiscard]] result<void>
			append_record_bytes(const std::span<const std::byte> bytes) override
			{
				if (!record_open_ || current_.size() > current_extent_.framed_bytes ||
					bytes.size() > current_extent_.framed_bytes - current_.size())
					return unexpected(corrupt("sqlite", "record-overrun"));
				current_.insert(current_.end(), bytes.begin(), bytes.end());
				return {};
			}

			[[nodiscard]] result<void> finish_record() override
			{
				if (!record_open_ || current_.size() != current_extent_.framed_bytes)
					return unexpected(corrupt("sqlite", "record-short"));
				auto decoded = decode_frame(current_);
				if (!decoded || decoded->extent != current_extent_)
					return unexpected(decoded ? corrupt("sqlite", "extent")
											  : std::move(decoded.error()));
				if (ordinal_ == std::numeric_limits<std::uint64_t>::max())
					return unexpected(failure("store.counter-overflow", "record-ordinal"));
				auto inserted = sqlite_statement::prepare(
					database_,
					"INSERT INTO "
					"cxxlens_v6_stage(session_id,ordinal,kind,key,payload,frame,order_key) "
					"VALUES(?1,?2,?3,?4,?5,?6,?7)");
				if (!inserted)
					return unexpected(std::move(inserted.error()));
				if (auto bound = inserted->bind_text(1, metadata_.staging_session_id); !bound)
					return bound;
				if (auto bound = inserted->bind_int64(2, static_cast<std::int64_t>(ordinal_));
					!bound)
					return bound;
				if (auto bound =
						inserted->bind_int64(3, static_cast<std::int64_t>(current_extent_.kind));
					!bound)
					return bound;
				if (auto bound = inserted->bind_blob(4, decoded->key); !bound)
					return bound;
				if (auto bound = inserted->bind_blob(5, decoded->payload); !bound)
					return bound;
				if (auto bound = inserted->bind_blob(6, current_); !bound)
					return bound;
				if (auto bound = inserted->bind_blob(7, decoded->order_key); !bound)
					return bound;
				if (auto done = inserted->done(); !done)
					return done;
				++ordinal_;
				current_.clear();
				current_extent_ = {};
				record_open_ = false;
				return {};
			}

			[[nodiscard]] result<void> seal_staging() override
			{
				if (!transaction_open_ || sealed_ || record_open_ || ordinal_ == 0U)
					return unexpected(invariant("sqlite", "seal-phase"));
				if (auto committed = database_->execute("COMMIT;"); !committed)
					return committed;
				transaction_open_ = false;
				sealed_ = true;
				return {};
			}

			[[nodiscard]] result<bounded_store_v6_measured_projection>
			measured_projection() const override
			{
				if (!sealed_ || !anchor_)
					return unexpected(invariant("sqlite", "measurement-phase"));
				auto scanned = scan_rows(
					database_, stage_table, metadata_.staging_session_id, metadata_.selector);
				if (!scanned)
					return unexpected(std::move(scanned.error()));
				return bounded_store_v6_measured_projection{scanned->record_count,
															scanned->framed_bytes,
															scanned->binary_sha256,
															std::move(scanned->snapshot),
															std::move(scanned->immutable_binding)};
			}

			[[nodiscard]] result<std::unique_ptr<bounded_store_v6_actual_cursor_source>>
			open_actual_cursor() override
			{
				if (!sealed_ || !anchor_)
					return unexpected(invariant("sqlite", "cursor-phase"));
				auto cursor = sqlite_actual_cursor::open(
					database_, metadata_.staging_session_id, anchor_, anchor_binding_);
				if (!cursor)
					return unexpected(std::move(cursor.error()));
				return std::unique_ptr<bounded_store_v6_actual_cursor_source>{std::move(*cursor)};
			}

			[[nodiscard]] result<bounded_store_v6_effect_result> publish_once() override
			{
				bounded_store_v6_effect_result output;
				if (!sealed_ || publish_called_ || !anchor_)
				{
					output.failure = invariant("sqlite", "publish-phase");
					return output;
				}
				publish_called_ = true;
				output.operation_attempted = true;
				auto measured = measured_projection();
				if (!measured)
				{
					output.failure = measured.error();
					output.effect_may_have_occurred = false;
					return output;
				}
				const auto& expected = metadata_.expected_head;
				std::uint64_t prior_sequence{};
				std::uint64_t prior_generation{};
				std::optional<std::string> prior_publication;
				if (auto begun = database_->execute("BEGIN IMMEDIATE;"); !begun)
				{
					output.failure = begun.error();
					output.effect_may_have_occurred = true;
					return output;
				}
				output.effect_may_have_occurred = true;
				auto current = sqlite_statement::prepare(
					database_,
					"SELECT h.publication_id,h.sequence,p.generation FROM cxxlens_v6_head h "
					"LEFT JOIN cxxlens_v6_publication p ON p.publication_id=h.publication_id "
					"WHERE h.series_id=?1");
				if (!current)
				{
					(void)database_->execute("ROLLBACK;");
					output.failure = current.error();
					return output;
				}
				if (auto bound = current->bind_text(1, metadata_.selector.id()); !bound)
				{
					(void)database_->execute("ROLLBACK;");
					output.failure = bound.error();
					return output;
				}
				const auto code = current->step();
				if (code == sqlite_row)
				{
					auto current_id = current->text(0);
					auto current_sequence = current->signed_value(1);
					auto current_generation = current->signed_value(2);
					if (!current_id || !current_sequence || !current_generation ||
						*current_sequence < 0 || *current_generation < 0)
					{
						(void)database_->execute("ROLLBACK;");
						output.failure = corrupt("sqlite", "head");
						return output;
					}
					prior_publication = std::move(*current_id);
					prior_sequence = static_cast<std::uint64_t>(*current_sequence);
					const bool parent_identity_matches = expected.publication &&
						*prior_publication == expected.publication->publication_id &&
						prior_sequence == expected.publication->sequence;
					const bool parent_generation_matches = expected.publication &&
						static_cast<std::uint64_t>(*current_generation) ==
							expected.publication->physical_generation;
					if (expected.value == bounded_store_v6_expected_head::kind::genesis ||
						!parent_identity_matches || !parent_generation_matches)
					{
						(void)database_->execute("ROLLBACK;");
						output.failure =
							expected.value == bounded_store_v6_expected_head::kind::genesis ||
								!parent_identity_matches
							? failure("store.publication-conflict", metadata_.selector.id())
							: corrupt("sqlite", "head-generation");
						output.effect_may_have_occurred = false;
						return output;
					}
					prior_generation = static_cast<std::uint64_t>(*current_generation);
				}
				else if (code == sqlite_done)
				{
					if (expected.value != bounded_store_v6_expected_head::kind::genesis)
					{
						(void)database_->execute("ROLLBACK;");
						output.failure =
							failure("store.publication-conflict", metadata_.selector.id());
						output.effect_may_have_occurred = false;
						return output;
					}
				}
				else
				{
					(void)database_->execute("ROLLBACK;");
					output.failure = sqlite_failure(database_->message());
					return output;
				}
				if (prior_sequence == std::numeric_limits<std::uint64_t>::max())
				{
					(void)database_->execute("ROLLBACK;");
					output.failure = failure("store.counter-overflow", "publication_sequence");
					return output;
				}
				if (prior_generation == std::numeric_limits<std::uint64_t>::max())
				{
					(void)database_->execute("ROLLBACK;");
					output.failure = failure("store.counter-overflow", "physical_generation");
					return output;
				}
				const auto sequence = prior_sequence + 1U;
				const auto generation = prior_generation + 1U;
				auto publication_id =
					publication_record_identity(metadata_.selector.id(),
												measured->candidate_snapshot.snapshot_id,
												sequence,
												prior_publication);
				if (!publication_id)
				{
					(void)database_->execute("ROLLBACK;");
					output.failure = sqlite_failure("publication-identity");
					return output;
				}
				auto binding = identity_binding("publication",
												metadata_.selector.id(),
												*publication_id,
												digest_string(measured->binary_sha256),
												measured->record_count,
												measured->framed_bytes);
				if (!binding)
				{
					(void)database_->execute("ROLLBACK;");
					output.failure = binding.error();
					return output;
				}
				auto insert_publication = sqlite_statement::prepare(
					database_,
					"INSERT INTO "
					"cxxlens_v6_publication(publication_id,series_id,snapshot_id,sequence,"
					"generation,parent,state,corrupt,partition_count,row_count,claim_count,"
					"coverage_count,unresolved_count,semantic_digest,export_digest,physical_digest,"
					"record_count,framed_bytes,immutable_binding) "
					"VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19)");
				if (!insert_publication)
				{
					(void)database_->execute("ROLLBACK;");
					output.failure = insert_publication.error();
					return output;
				}
				const auto physical_digest = digest_string(measured->binary_sha256);
				const auto bind_values = [&]() -> result<void>
				{
					for (const auto [index, value] :
						 std::array<std::pair<int, std::string_view>, 4U>{
							 {{1, *publication_id},
							  {2, metadata_.selector.id()},
							  {3, measured->candidate_snapshot.snapshot_id},
							  {14, measured->candidate_snapshot.semantic_projection_digest}}})
						if (auto bound = insert_publication->bind_text(index, value); !bound)
							return bound;
					if (auto bound =
							insert_publication->bind_int64(4, static_cast<std::int64_t>(sequence));
						!bound)
						return bound;
					if (auto bound = insert_publication->bind_int64(
							5, static_cast<std::int64_t>(generation));
						!bound)
						return bound;
					if (prior_publication)
					{
						if (auto bound = insert_publication->bind_text(6, *prior_publication);
							!bound)
							return bound;
					}
					else if (auto bound = insert_publication->bind_null(6); !bound)
						return bound;
					for (const auto [index, value] : std::array<std::pair<int, std::int64_t>, 8U>{
							 {{7, 3},
							  {8, 0},
							  {9,
							   static_cast<std::int64_t>(
								   measured->candidate_snapshot.partition_count)},
							  {10,
							   static_cast<std::int64_t>(measured->candidate_snapshot.row_count)},
							  {11,
							   static_cast<std::int64_t>(measured->candidate_snapshot.claim_count)},
							  {12,
							   static_cast<std::int64_t>(
								   measured->candidate_snapshot.coverage_count)},
							  {13,
							   static_cast<std::int64_t>(
								   measured->candidate_snapshot.unresolved_count)},
							  {17, static_cast<std::int64_t>(measured->record_count)}}})
						if (auto bound = insert_publication->bind_int64(index, value); !bound)
							return bound;
					if (auto bound = insert_publication->bind_text(
							15, measured->candidate_snapshot.canonical_export_digest);
						!bound)
						return bound;
					if (auto bound = insert_publication->bind_text(16, physical_digest); !bound)
						return bound;
					if (auto bound = insert_publication->bind_int64(
							18, static_cast<std::int64_t>(measured->framed_bytes));
						!bound)
						return bound;
					return insert_publication->bind_text(19, *binding);
				};
				if (auto bound = bind_values(); !bound || !insert_publication->done())
				{
					(void)database_->execute("ROLLBACK;");
					output.failure = bound ? sqlite_failure(database_->message()) : bound.error();
					return output;
				}
				auto copy_records = sqlite_statement::prepare(
					database_,
					"INSERT INTO "
					"cxxlens_v6_record(publication_id,ordinal,kind,key,payload,frame,order_key) "
					"SELECT ?1,ordinal,kind,key,payload,frame,order_key FROM cxxlens_v6_stage "
					"WHERE session_id=?2 ORDER BY ordinal");
				if (!copy_records)
				{
					(void)database_->execute("ROLLBACK;");
					output.failure = copy_records.error();
					return output;
				}
				if (auto bound = copy_records->bind_text(1, *publication_id); !bound)
				{
					(void)database_->execute("ROLLBACK;");
					output.failure = bound.error();
					return output;
				}
				if (auto bound = copy_records->bind_text(2, metadata_.staging_session_id); !bound)
				{
					(void)database_->execute("ROLLBACK;");
					output.failure = bound.error();
					return output;
				}
				if (auto done = copy_records->done(); !done)
				{
					(void)database_->execute("ROLLBACK;");
					output.failure = done.error();
					return output;
				}
				auto head = sqlite_statement::prepare(
					database_,
					expected.value == bounded_store_v6_expected_head::kind::genesis
						? "INSERT INTO cxxlens_v6_head(series_id,publication_id,sequence) "
						  "VALUES(?1,?2,?3)"
						: "UPDATE cxxlens_v6_head SET publication_id=?1,sequence=?2 WHERE "
						  "series_id=?3");
				if (!head)
				{
					(void)database_->execute("ROLLBACK;");
					output.failure = head.error();
					return output;
				}
				if (expected.value == bounded_store_v6_expected_head::kind::genesis)
				{
					if (auto bound = head->bind_text(1, metadata_.selector.id()); !bound)
					{
						(void)database_->execute("ROLLBACK;");
						output.failure = bound.error();
						return output;
					}
					if (auto bound = head->bind_text(2, *publication_id); !bound)
					{
						(void)database_->execute("ROLLBACK;");
						output.failure = bound.error();
						return output;
					}
					if (auto bound = head->bind_int64(3, static_cast<std::int64_t>(sequence));
						!bound)
					{
						(void)database_->execute("ROLLBACK;");
						output.failure = bound.error();
						return output;
					}
				}
				else
				{
					if (auto bound = head->bind_text(1, *publication_id); !bound)
					{
						(void)database_->execute("ROLLBACK;");
						output.failure = bound.error();
						return output;
					}
					if (auto bound = head->bind_int64(2, static_cast<std::int64_t>(sequence));
						!bound)
					{
						(void)database_->execute("ROLLBACK;");
						output.failure = bound.error();
						return output;
					}
					if (auto bound = head->bind_text(3, metadata_.selector.id()); !bound)
					{
						(void)database_->execute("ROLLBACK;");
						output.failure = bound.error();
						return output;
					}
				}
				if (auto done = head->done(); !done)
				{
					(void)database_->execute("ROLLBACK;");
					output.failure = done.error();
					return output;
				}
				if (auto committed = database_->execute("COMMIT;"); !committed)
				{
					(void)database_->execute("ROLLBACK;");
					output.failure = committed.error();
					return output;
				}
				published_id_ = *publication_id;
				published_ = true;
				output.publication = bounded_store_v6_publication_observation{
					*publication_id,
					metadata_.selector.id(),
					measured->candidate_snapshot.snapshot_id,
					sequence,
					generation,
					prior_publication,
					publication_state::committed,
					false};
				output.returned_handle = true;
				return output;
			}

			[[nodiscard]] result<bounded_store_v6_reopen_observation> reopen() override
			{
				if (!published_ || !sealed_ || published_id_.empty())
					return unexpected(invariant("sqlite", "reopen-phase"));
				auto api = load_sqlite();
				if (!api)
					return unexpected(std::move(api.error()));
				auto fresh_database =
					open_database(*api, metadata_.exact_sqlite_path.value_or(std::string{}));
				if (!fresh_database)
					return unexpected(std::move(fresh_database.error()));
				if (auto schema = initialize_schema(*fresh_database); !schema)
					return unexpected(std::move(schema.error()));
				static std::atomic<std::uint64_t> next_reopen_binding{1U};
				auto fresh_binding = canonical_identity_digest(
					"cxxlens.df-0200.sqlite-fresh-open.v1",
					std::array{canonical_value::from_string(metadata_.selector.id()),
							   canonical_value::from_string(published_id_),
							   canonical_value::from_integer(
								   static_cast<std::int64_t>(next_reopen_binding.fetch_add(1U)))});
				if (!fresh_binding)
					return unexpected(std::move(fresh_binding.error()));
				bounded_store_v6_reopen_observation observation;
				observation.factory_attempted = true;
				observation.fresh_backend_binding = *fresh_binding;
				const auto present = [&](const std::string_view id, const bool expected_candidate)
					-> result<bounded_store_v6_lookup_observation>
				{
					auto loaded = load_publication(*fresh_database, id);
					if (!loaded)
						return unexpected(std::move(loaded.error()));
					if (!*loaded)
					{
						bounded_store_v6_lookup_observation missing;
						missing.status = bounded_store_v6_lookup_observation::state::not_found;
						return missing;
					}
					auto scanned = scan_rows(*fresh_database, record_table, id, metadata_.selector);
					if (!scanned)
						return unexpected(std::move(scanned.error()));
					const auto& persisted = **loaded;
					if (persisted.snapshot != scanned->snapshot ||
						persisted.physical_digest != scanned->binary_digest ||
						persisted.record_count != scanned->record_count ||
						persisted.framed_bytes != scanned->framed_bytes ||
						(expected_candidate &&
						 persisted.publication.publication_id != published_id_))
						return unexpected(corrupt("sqlite", "reopen-publication"));
					bounded_store_v6_lookup_observation value;
					value.status = bounded_store_v6_lookup_observation::state::present;
					value.publication = persisted.publication;
					value.snapshot = persisted.snapshot;
					return value;
				};
				const auto head_query = [&]() -> result<std::optional<std::string>>
				{
					auto head = sqlite_statement::prepare(
						*fresh_database,
						"SELECT publication_id,sequence FROM cxxlens_v6_head WHERE series_id=?1");
					if (!head)
						return unexpected(std::move(head.error()));
					if (auto bound = head->bind_text(1, metadata_.selector.id()); !bound)
						return unexpected(std::move(bound.error()));
					const auto code = head->step();
					if (code == sqlite_done)
						return std::optional<std::string>{};
					if (code != sqlite_row)
						return unexpected(sqlite_failure((*fresh_database)->message()));
					auto id = head->text(0);
					auto sequence = head->signed_value(1);
					if (!id || !sequence || *sequence < 0 || head->step() != sqlite_done)
						return unexpected(corrupt("sqlite", "head"));
					return std::optional<std::string>{std::move(*id)};
				};
				auto current_id = head_query();
				if (!current_id)
					return unexpected(std::move(current_id.error()));
				if (*current_id)
				{
					auto current = present(**current_id, true);
					if (!current)
						return unexpected(std::move(current.error()));
					observation.current = std::move(*current);
				}
				else
					observation.current.status =
						bounded_store_v6_lookup_observation::state::not_found;
				if (metadata_.expected_head.publication)
				{
					auto parent =
						present(metadata_.expected_head.publication->publication_id, false);
					if (!parent)
						return unexpected(std::move(parent.error()));
					observation.expected_parent = std::move(*parent);
				}
				else
					observation.expected_parent.status =
						bounded_store_v6_lookup_observation::state::not_found;
				auto publication = present(published_id_, true);
				if (!publication)
					return unexpected(std::move(publication.error()));
				observation.publication = std::move(*publication);
				auto snapshot = present(published_id_, true);
				if (!snapshot)
					return unexpected(std::move(snapshot.error()));
				observation.snapshot = std::move(*snapshot);
				if (observation.snapshot.snapshot)
					observation.canonical_export_digest =
						observation.snapshot.snapshot->canonical_export_digest;
				return observation;
			}

			[[nodiscard]] result<void> abort_staging() override
			{
				if (cleanup_called_)
					return unexpected(invariant("sqlite", "cleanup-replayed"));
				cleanup_called_ = true;
				if (record_open_)
					return unexpected(corrupt("sqlite", "cleanup-open-record"));
				if (transaction_open_)
				{
					if (auto rolled_back = database_->execute("ROLLBACK;"); !rolled_back)
						return rolled_back;
					transaction_open_ = false;
				}
				auto deleted = sqlite_statement::prepare(
					database_, "DELETE FROM cxxlens_v6_stage WHERE session_id=?1");
				if (!deleted)
					return unexpected(std::move(deleted.error()));
				if (auto bound = deleted->bind_text(1, metadata_.staging_session_id); !bound)
					return bound;
				return deleted->done();
			}

		  private:
			struct persisted_publication
			{
				bounded_store_v6_publication_observation publication;
				bounded_store_v6_snapshot_observation snapshot;
				std::string physical_digest;
				std::uint64_t record_count{};
				std::uint64_t framed_bytes{};
				std::string immutable_binding;
			};

			[[nodiscard]] result<std::optional<persisted_publication>>
			load_publication(const std::shared_ptr<sqlite_database>& database,
							 const std::string_view id) const
			{
				auto selected = sqlite_statement::prepare(
					database,
					"SELECT "
					"publication_id,series_id,snapshot_id,sequence,generation,parent,state,corrupt,"
					"partition_count,row_count,claim_count,coverage_count,unresolved_count,"
					"semantic_digest,export_digest,physical_digest,record_count,framed_bytes,"
					"immutable_binding FROM cxxlens_v6_publication WHERE publication_id=?1");
				if (!selected)
					return unexpected(std::move(selected.error()));
				if (auto bound = selected->bind_text(1, id); !bound)
					return unexpected(std::move(bound.error()));
				const auto code = selected->step();
				if (code == sqlite_done)
					return std::optional<persisted_publication>{};
				if (code != sqlite_row)
					return unexpected(sqlite_failure(database->message()));
				persisted_publication output;
				auto publication_id = selected->text(0);
				auto series_id = selected->text(1);
				auto snapshot_id = selected->text(2);
				auto sequence = selected->signed_value(3);
				auto generation = selected->signed_value(4);
				auto parent = selected->optional_text(5);
				auto state = selected->signed_value(6);
				auto corrupt_value = selected->signed_value(7);
				auto partition_count = selected->signed_value(8);
				auto row_count = selected->signed_value(9);
				auto claim_count = selected->signed_value(10);
				auto coverage_count = selected->signed_value(11);
				auto unresolved_count = selected->signed_value(12);
				auto semantic = selected->text(13);
				auto export_digest = selected->text(14);
				auto physical = selected->text(15);
				auto record_count = selected->signed_value(16);
				auto framed_bytes = selected->signed_value(17);
				auto immutable = selected->text(18);
				const auto all_valid = publication_id && series_id && snapshot_id && sequence &&
					generation && parent && state && corrupt_value && partition_count &&
					row_count && claim_count && coverage_count && unresolved_count && semantic &&
					export_digest && physical && record_count && framed_bytes && immutable &&
					*sequence >= 0 && *generation >= 0 && *state >= 0 && *state <= 5 &&
					(*corrupt_value == 0 || *corrupt_value == 1) && *partition_count > 0 &&
					*row_count >= 0 && *claim_count >= 0 && *coverage_count >= 0 &&
					*unresolved_count >= 0 && *record_count > 0 && *framed_bytes > 0 &&
					canonical_digest(*semantic) && canonical_digest(*export_digest) &&
					canonical_digest(*physical) && !immutable->empty() &&
					*series_id == metadata_.selector.id() &&
					*state == static_cast<std::int64_t>(publication_state::committed) &&
					*corrupt_value == 0;
				if (!all_valid || selected->step() != sqlite_done)
					return unexpected(corrupt("sqlite", "publication-row"));
				output.publication = {*publication_id,
									  *series_id,
									  *snapshot_id,
									  static_cast<std::uint64_t>(*sequence),
									  static_cast<std::uint64_t>(*generation),
									  std::move(*parent),
									  static_cast<publication_state>(*state),
									  *corrupt_value != 0};
				output.snapshot = {*snapshot_id,
								   static_cast<std::uint64_t>(*partition_count),
								   static_cast<std::uint64_t>(*row_count),
								   static_cast<std::uint64_t>(*claim_count),
								   static_cast<std::uint64_t>(*coverage_count),
								   static_cast<std::uint64_t>(*unresolved_count),
								   *semantic,
								   *export_digest};
				output.physical_digest = *physical;
				output.record_count = static_cast<std::uint64_t>(*record_count);
				output.framed_bytes = static_cast<std::uint64_t>(*framed_bytes);
				output.immutable_binding = *immutable;
				auto identity = publication_record_identity(output.publication.series_id,
															output.publication.snapshot_id,
															output.publication.sequence,
															output.publication.parent_publication);
				if (!identity || *identity != output.publication.publication_id)
					return unexpected(corrupt("sqlite", "publication-identity"));
				return std::optional<persisted_publication>{std::move(output)};
			}

			bounded_store_v6_session_metadata metadata_;
			std::shared_ptr<sqlite_database> database_;
			std::shared_ptr<const bounded_store_v6_physical_anchor> anchor_;
			std::string anchor_binding_;
			std::string published_id_;
			std::vector<std::byte> current_;
			bounded_store_v6_record_extent current_extent_{};
			std::uint64_t ordinal_{};
			bool transaction_open_{};
			bool record_open_{};
			bool sealed_{};
			bool publish_called_{};
			bool published_{};
			bool cleanup_called_{};
		};
	} // namespace

	result<std::unique_ptr<bounded_store_v6_backend_port>>
	make_bounded_store_v6_sqlite_backend_port(bounded_store_v6_session_metadata metadata)
	{
		try
		{
			if (metadata.backend != bounded_store_v6_backend::sqlite ||
				!metadata.exact_sqlite_path || metadata.exact_sqlite_path->empty())
				return unexpected(invariant("sqlite", "metadata"));
			auto api = load_sqlite();
			if (!api)
				return unexpected(std::move(api.error()));
			auto database = open_database(*api, *metadata.exact_sqlite_path);
			if (!database)
				return unexpected(std::move(database.error()));
			if (auto schema = initialize_schema(*database); !schema)
				return unexpected(std::move(schema.error()));
			return std::unique_ptr<bounded_store_v6_backend_port>{
				new sqlite_bounded_store_v6_backend{std::move(metadata), std::move(*database)}};
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(resource("allocation", "unavailable"));
		}
		catch (const std::exception& exception)
		{
			return unexpected(invariant("sqlite", exception.what()));
		}
		catch (...)
		{
			return unexpected(invariant("sqlite", "non-standard-exception"));
		}
	}
} // namespace cxxlens::sdk::detail
