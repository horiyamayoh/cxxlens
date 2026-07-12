#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "../binary_codec.hpp"
#include "../store_port.hpp"
#include "sqlite_api.hpp"

namespace cxxlens::detail::store
{
	namespace
	{
		[[nodiscard]] error sqlite_error(std::string code, std::string reason, const int native = 0)
		{
			error failure;
			failure.code.value = std::move(code);
			failure.message = "SQLite fact store operation failed";
			failure.scope = failure_scope::workspace;
			failure.attributes.emplace("backend", "sqlite");
			failure.attributes.emplace("reason", std::move(reason));
			failure.attributes.emplace("native_code", std::to_string(native));
			return failure;
		}

		[[nodiscard]] bool fact_less(const facts::detached_fact_record& left,
									 const facts::detached_fact_record& right)
		{
			return std::tuple{left.kind, left.stable_key, left.id.value()} <
				std::tuple{right.kind, right.stable_key, right.id.value()};
		}

		[[nodiscard]] bool metadata_compatible(const snapshot_metadata& left,
											   const snapshot_metadata& right)
		{
			return left.workspace_key == right.workspace_key &&
				left.schema_version == right.schema_version &&
				left.semantics_version == right.semantics_version &&
				left.adapter_id == right.adapter_id &&
				left.adapter_version == right.adapter_version &&
				left.extractor_versions == right.extractor_versions;
		}

		class database_handle
		{
		  public:
			database_handle(std::shared_ptr<const sqlite::api> api, sqlite::database* value)
				: api_{std::move(api)}, value_{value}
			{
			}
			~database_handle()
			{
				if (value_ != nullptr)
					(void)api_->close_v2(value_);
			}
			database_handle(const database_handle&) = delete;
			database_handle& operator=(const database_handle&) = delete;
			[[nodiscard]] sqlite::database* get() const noexcept
			{
				return value_;
			}

		  private:
			std::shared_ptr<const sqlite::api> api_;
			sqlite::database* value_{};
		};

		class statement_handle
		{
		  public:
			statement_handle(const sqlite::api& api, sqlite::statement* value)
				: api_{api}, value_{value}
			{
			}
			~statement_handle()
			{
				if (value_ != nullptr)
					(void)api_.finalize(value_);
			}
			statement_handle(const statement_handle&) = delete;
			statement_handle& operator=(const statement_handle&) = delete;
			statement_handle(statement_handle&& other) noexcept
				: api_{other.api_}, value_{other.value_}
			{
				other.value_ = nullptr;
			}
			statement_handle& operator=(statement_handle&&) = delete;
			[[nodiscard]] sqlite::statement* get() const noexcept
			{
				return value_;
			}

		  private:
			const sqlite::api& api_;
			sqlite::statement* value_{};
		};

		// SQL text and its stable failure reason intentionally travel as one checked operation.
		// NOLINTBEGIN(bugprone-easily-swappable-parameters)
		[[nodiscard]] result<void> execute(const sqlite::api& api,
										   sqlite::database* database,
										   const std::string_view sql,
										   const std::string_view reason)
		{
			char* message{};
			const std::string stable_sql{sql};
			const int status = api.exec(database, stable_sql.c_str(), nullptr, nullptr, &message);
			std::string native_message = message != nullptr ? message : "";
			if (message != nullptr)
				api.free_memory(message);
			if (status != sqlite::ok)
				return sqlite_error("facts.transaction-failed",
									std::string{reason} +
										(native_message.empty() ? "" : ":" + native_message),
									status);
			return {};
		}
		// NOLINTEND(bugprone-easily-swappable-parameters)

		[[nodiscard]] result<statement_handle>
		prepare(const sqlite::api& api, sqlite::database* database, const std::string_view sql)
		{
			sqlite::statement* statement{};
			const std::string stable_sql{sql};
			const int status =
				api.prepare_v2(database, stable_sql.c_str(), -1, &statement, nullptr);
			if (status != sqlite::ok || statement == nullptr)
				return sqlite_error("facts.store-corrupt", "prepare-failed", status);
			return statement_handle{api, statement};
		}

		[[nodiscard]] result<void> create_schema(const sqlite::api& api, sqlite::database* database)
		{
			return execute(api,
						   database,
						   R"sql(
CREATE TABLE IF NOT EXISTS cxxlens_store_metadata(
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
) WITHOUT ROWID;
INSERT OR REPLACE INTO cxxlens_store_metadata(key,value) VALUES
  ('store_schema','cxxlens.sqlite-fact-store.v1'),
  ('binary_schema','cxxlens.fact-snapshot.binary.v1');
CREATE TABLE IF NOT EXISTS cxxlens_snapshots(
  generation INTEGER PRIMARY KEY,
  workspace_key TEXT NOT NULL,
  schema_version TEXT NOT NULL,
  semantics_version TEXT NOT NULL,
  adapter_id TEXT NOT NULL,
  adapter_version TEXT NOT NULL,
  payload BLOB NOT NULL
);
CREATE TABLE IF NOT EXISTS cxxlens_fact_index(
  generation INTEGER NOT NULL REFERENCES cxxlens_snapshots(generation) ON DELETE CASCADE,
  fact_id TEXT NOT NULL,
  kind INTEGER NOT NULL,
  stable_key TEXT NOT NULL,
  owner_id TEXT NOT NULL,
  file_id TEXT NOT NULL,
  source_begin INTEGER NOT NULL,
  source_end INTEGER NOT NULL,
  PRIMARY KEY(generation,fact_id)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS cxxlens_fact_kind_order
  ON cxxlens_fact_index(generation,kind,stable_key,fact_id);
CREATE INDEX IF NOT EXISTS cxxlens_fact_owner_order
  ON cxxlens_fact_index(generation,owner_id,kind,stable_key,fact_id);
CREATE INDEX IF NOT EXISTS cxxlens_fact_file_order
  ON cxxlens_fact_index(generation,file_id,source_begin,source_end,kind,fact_id);
CREATE TABLE IF NOT EXISTS cxxlens_edge_index(
  generation INTEGER NOT NULL REFERENCES cxxlens_snapshots(generation) ON DELETE CASCADE,
  fact_id TEXT NOT NULL,
  kind INTEGER NOT NULL,
  source_id TEXT NOT NULL,
  target_id TEXT NOT NULL,
  PRIMARY KEY(generation,fact_id)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS cxxlens_edge_forward_order
  ON cxxlens_edge_index(generation,kind,source_id,target_id,fact_id);
CREATE INDEX IF NOT EXISTS cxxlens_edge_reverse_order
  ON cxxlens_edge_index(generation,kind,target_id,source_id,fact_id);
CREATE TABLE IF NOT EXISTS cxxlens_coverage_index(
  generation INTEGER NOT NULL REFERENCES cxxlens_snapshots(generation) ON DELETE CASCADE,
  unit_kind TEXT NOT NULL,
  unit_id TEXT NOT NULL,
  state INTEGER NOT NULL,
  reason TEXT NOT NULL,
  PRIMARY KEY(generation,unit_kind,unit_id)
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS cxxlens_current_snapshot(
  singleton INTEGER PRIMARY KEY CHECK(singleton=1),
  generation INTEGER NOT NULL REFERENCES cxxlens_snapshots(generation)
);
)sql",
						   "create-schema-failed");
		}

		[[nodiscard]] result<void> bind_text(const sqlite::api& api,
											 sqlite::statement* statement,
											 const int index,
											 const std::string_view value)
		{
			if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
				return sqlite_error("facts.transaction-failed", "text-too-large");
			const int status = api.bind_text(
				statement, index, value.data(), static_cast<int>(value.size()), nullptr);
			if (status != sqlite::ok)
				return sqlite_error("facts.transaction-failed", "bind-text-failed", status);
			return {};
		}

		[[nodiscard]] std::string payload_value(const facts::detached_fact_record& fact,
												const std::string_view key)
		{
			if (const auto found = fact.payload.find(std::string{key}); found != fact.payload.end())
				return found->second;
			return {};
		}

		[[nodiscard]] std::string owner_id(const facts::detached_fact_record& fact)
		{
			switch (fact.kind)
			{
				case fact_kind::declaration:
				case fact_kind::definition:
					return payload_value(fact, "symbol.id");
				case fact_kind::reference:
					return payload_value(fact, "reference.owner");
				case fact_kind::call:
					return payload_value(fact, "call.caller");
				case fact_kind::inheritance:
					return payload_value(fact, "inheritance.derived");
				case fact_kind::override_relation:
					return payload_value(fact, "override.overriding");
				default:
					return payload_value(fact, "semantic.owner");
			}
		}

		[[nodiscard]] std::pair<std::string, std::string>
		edge_endpoints(const facts::detached_fact_record& fact)
		{
			switch (fact.kind)
			{
				case fact_kind::reference:
					return {payload_value(fact, "reference.owner"),
							payload_value(fact, "reference.target")};
				case fact_kind::call:
					return {payload_value(fact, "call.caller"),
							payload_value(fact, "call.direct_callee")};
				case fact_kind::inheritance:
					return {payload_value(fact, "inheritance.derived"),
							payload_value(fact, "inheritance.base")};
				case fact_kind::override_relation:
					return {payload_value(fact, "override.overriding"),
							payload_value(fact, "override.overridden")};
				default:
					return {};
			}
		}

		[[nodiscard]] result<void> insert_snapshot(const sqlite::api& api,
												   sqlite::database* database,
												   const snapshot_data& snapshot,
												   const std::span<const std::byte> payload)
		{
			auto insert = prepare(
				api,
				database,
				"INSERT INTO "
				"cxxlens_snapshots(generation,workspace_key,schema_version,semantics_version,"
				"adapter_id,adapter_version,payload) VALUES(?,?,?,?,?,?,?)");
			if (!insert)
				return std::move(insert.error());
			auto* statement = insert.value().get();
			if (api.bind_int64(
					statement, 1, static_cast<std::int64_t>(snapshot.metadata.generation)) !=
				sqlite::ok)
				return sqlite_error("facts.transaction-failed", "bind-generation-failed");
			for (const auto& [index, value] : std::array<std::pair<int, std::string_view>, 5>{
					 {{2, snapshot.metadata.workspace_key},
					  {3, snapshot.metadata.schema_version},
					  {4, snapshot.metadata.semantics_version},
					  {5, snapshot.metadata.adapter_id},
					  {6, snapshot.metadata.adapter_version}}})
				if (auto bound = bind_text(api, statement, index, value); !bound)
					return bound;
			const int blob_status =
				api.bind_blob64(statement, 7, payload.data(), payload.size(), nullptr);
			if (blob_status != sqlite::ok || api.step(statement) != sqlite::done)
				return sqlite_error(
					"facts.transaction-failed", "insert-snapshot-failed", blob_status);

			for (const auto& fact : snapshot.facts)
			{
				auto index_row =
					prepare(api,
							database,
							"INSERT INTO cxxlens_fact_index(generation,fact_id,kind,stable_key,"
							"owner_id,file_id,source_begin,source_end) VALUES(?,?,?,?,?,?,?,?)");
				if (!index_row)
					return std::move(index_row.error());
				auto* row_statement = index_row.value().get();
				if (api.bind_int64(row_statement,
								   1,
								   static_cast<std::int64_t>(snapshot.metadata.generation)) !=
						sqlite::ok ||
					api.bind_int64(row_statement, 3, static_cast<std::int64_t>(fact.kind)) !=
						sqlite::ok)
					return sqlite_error("facts.transaction-failed", "bind-index-number-failed");
				if (auto bound = bind_text(api, row_statement, 2, fact.id.value()); !bound)
					return bound;
				if (auto bound = bind_text(api, row_statement, 4, fact.stable_key); !bound)
					return bound;
				const auto semantic_owner = owner_id(fact);
				if (auto bound = bind_text(api, row_statement, 5, semantic_owner); !bound)
					return bound;
				const auto source_file = fact.source
					? std::string{fact.source->primary.begin.file.value()}
					: std::string{};
				if (auto bound = bind_text(api, row_statement, 6, source_file); !bound)
					return bound;
				const auto source_begin = fact.source
					? static_cast<std::int64_t>(fact.source->primary.begin.byte_offset)
					: std::int64_t{-1};
				const auto source_end = fact.source
					? static_cast<std::int64_t>(fact.source->primary.end.byte_offset)
					: std::int64_t{-1};
				if (api.bind_int64(row_statement, 7, source_begin) != sqlite::ok ||
					api.bind_int64(row_statement, 8, source_end) != sqlite::ok)
					return sqlite_error("facts.transaction-failed", "bind-index-range-failed");
				if (const int status = api.step(row_statement); status != sqlite::done)
					return sqlite_error("facts.transaction-failed", "insert-index-failed", status);

				const auto [source_id, target_id] = edge_endpoints(fact);
				if (!source_id.empty() || !target_id.empty())
				{
					auto edge =
						prepare(api,
								database,
								"INSERT INTO cxxlens_edge_index(generation,fact_id,kind,source_id,"
								"target_id) VALUES(?,?,?,?,?)");
					if (!edge)
						return std::move(edge.error());
					auto* edge_statement = edge.value().get();
					if (api.bind_int64(edge_statement,
									   1,
									   static_cast<std::int64_t>(snapshot.metadata.generation)) !=
							sqlite::ok ||
						api.bind_int64(edge_statement, 3, static_cast<std::int64_t>(fact.kind)) !=
							sqlite::ok)
						return sqlite_error("facts.transaction-failed", "bind-edge-number-failed");
					if (auto bound = bind_text(api, edge_statement, 2, fact.id.value()); !bound)
						return bound;
					if (auto bound = bind_text(api, edge_statement, 4, source_id); !bound)
						return bound;
					if (auto bound = bind_text(api, edge_statement, 5, target_id); !bound)
						return bound;
					if (const int status = api.step(edge_statement); status != sqlite::done)
						return sqlite_error(
							"facts.transaction-failed", "insert-edge-failed", status);
				}
			}

			for (const auto& unit : snapshot.coverage.units())
			{
				auto coverage = prepare(
					api,
					database,
					"INSERT INTO cxxlens_coverage_index(generation,unit_kind,unit_id,state,reason) "
					"VALUES(?,?,?,?,?)");
				if (!coverage)
					return std::move(coverage.error());
				auto* coverage_statement = coverage.value().get();
				if (api.bind_int64(coverage_statement,
								   1,
								   static_cast<std::int64_t>(snapshot.metadata.generation)) !=
						sqlite::ok ||
					api.bind_int64(coverage_statement, 4, static_cast<std::int64_t>(unit.state)) !=
						sqlite::ok)
					return sqlite_error("facts.transaction-failed", "bind-coverage-number-failed");
				if (auto bound = bind_text(api, coverage_statement, 2, unit.kind); !bound)
					return bound;
				if (auto bound = bind_text(api, coverage_statement, 3, unit.id); !bound)
					return bound;
				const auto reason = unit.reason.value_or("");
				if (auto bound = bind_text(api, coverage_statement, 5, reason); !bound)
					return bound;
				if (const int status = api.step(coverage_statement); status != sqlite::done)
					return sqlite_error(
						"facts.transaction-failed", "insert-coverage-failed", status);
			}

			auto publish =
				prepare(api,
						database,
						"INSERT INTO cxxlens_current_snapshot(singleton,generation) VALUES(1,?) "
						"ON CONFLICT(singleton) DO UPDATE SET generation=excluded.generation");
			if (!publish)
				return std::move(publish.error());
			if (api.bind_int64(publish.value().get(),
							   1,
							   static_cast<std::int64_t>(snapshot.metadata.generation)) !=
					sqlite::ok ||
				api.step(publish.value().get()) != sqlite::done)
				return sqlite_error("facts.transaction-failed", "publish-current-failed");
			return {};
		}

		[[nodiscard]] result<std::shared_ptr<snapshot_data>>
		load_current(const sqlite::api& api, sqlite::database* database)
		{
			auto selected =
				prepare(api,
						database,
						"SELECT s.payload FROM cxxlens_current_snapshot c JOIN cxxlens_snapshots s "
						"ON s.generation=c.generation WHERE c.singleton=1");
			if (!selected)
				return std::move(selected.error());
			const int status = api.step(selected.value().get());
			if (status != sqlite::row)
				return sqlite_error("facts.store-corrupt", "current-snapshot-missing", status);
			const int size = api.column_bytes(selected.value().get(), 0);
			const void* data = api.column_blob(selected.value().get(), 0);
			if (size <= 0 || data == nullptr)
				return sqlite_error("facts.store-corrupt", "snapshot-payload-empty");
			const auto bytes =
				std::span{static_cast<const std::byte*>(data), static_cast<std::size_t>(size)};
			return decode_snapshot(bytes);
		}

		struct sqlite_state
		{
			mutable std::mutex mutex;
			std::shared_ptr<const sqlite::api> api;
			std::unique_ptr<database_handle> database;
			std::shared_ptr<const snapshot_data> current;
			snapshot_metadata expected;
			compatibility_state compatibility{compatibility_state::compatible};
			bool writer{};
		};

		class sqlite_transaction final : public snapshot_transaction
		{
		  public:
			sqlite_transaction(std::shared_ptr<sqlite_state> shared,
							   snapshot_metadata metadata,
							   const transaction_fault fault)
				: shared_{std::move(shared)}, metadata_{std::move(metadata)}, fault_{fault}
			{
			}
			~sqlite_transaction() override
			{
				rollback();
			}
			[[nodiscard]] transaction_state state() const noexcept override
			{
				return state_;
			}

			result<void> stage(const facts::reduction_result& reduction) override
			{
				if (state_ != transaction_state::created)
					return sqlite_error("facts.transaction-failed", "illegal-stage-transition");
				if (auto checked = reduction.validate(); !checked)
					return fail("invalid-reduction");
				auto snapshot = std::make_shared<snapshot_data>();
				snapshot->metadata = metadata_;
				{
					const std::scoped_lock lock{shared_->mutex};
					snapshot->metadata.generation = shared_->current->metadata.generation + 1U;
				}
				snapshot->facts = reduction.facts;
				std::ranges::sort(snapshot->facts, fact_less);
				snapshot->coverage = reduction.coverage;
				auto encoded = encode_snapshot(*snapshot);
				if (!encoded)
					return fail("binary-encode-failed");
				staged_ = std::move(snapshot);
				encoded_ = std::move(encoded.value());
				state_ = transaction_state::staged;
				if (fault_ == transaction_fault::after_stage)
					return fail("injected-after-stage");
				return {};
			}

			result<void> validate() override
			{
				if (state_ != transaction_state::staged || !staged_)
					return sqlite_error("facts.transaction-failed", "illegal-validate-transition");
				auto decoded = decode_snapshot(encoded_);
				if (!decoded || decoded.value()->metadata != staged_->metadata ||
					decoded.value()->facts.size() != staged_->facts.size() ||
					decoded.value()->coverage.to_json() != staged_->coverage.to_json())
					return fail("binary-roundtrip-validation-failed");
				state_ = transaction_state::validated;
				if (fault_ == transaction_fault::after_validate)
					return fail("injected-after-validate");
				return {};
			}

			result<void> commit() override
			{
				if (state_ != transaction_state::validated || !staged_)
					return sqlite_error("facts.transaction-failed", "illegal-commit-transition");
				if (fault_ == transaction_fault::before_publish)
					return fail("injected-before-publish");
				if (auto inserted = insert_snapshot(
						*shared_->api, shared_->database->get(), *staged_, encoded_);
					!inserted)
					return fail("snapshot-insert-failed");
				if (fault_ == transaction_fault::during_publish)
					return fail("injected-during-publish");
				if (auto committed =
						execute(*shared_->api, shared_->database->get(), "COMMIT", "commit-failed");
					!committed)
					return fail("commit-failed");
				{
					const std::scoped_lock lock{shared_->mutex};
					shared_->current = std::move(staged_);
					shared_->writer = false;
				}
				owns_writer_ = false;
				state_ = transaction_state::committed;
				return {};
			}

			void rollback() noexcept override
			{
				if (!owns_writer_)
					return;
				(void)execute(
					*shared_->api, shared_->database->get(), "ROLLBACK", "rollback-failed");
				const std::scoped_lock lock{shared_->mutex};
				shared_->writer = false;
				owns_writer_ = false;
				staged_.reset();
				encoded_.clear();
				state_ = transaction_state::rolled_back;
			}

		  private:
			result<void> fail(std::string reason)
			{
				rollback();
				return sqlite_error("facts.transaction-failed", std::move(reason));
			}

			std::shared_ptr<sqlite_state> shared_;
			snapshot_metadata metadata_;
			transaction_fault fault_{transaction_fault::none};
			transaction_state state_{transaction_state::created};
			std::shared_ptr<snapshot_data> staged_;
			std::vector<std::byte> encoded_;
			bool owns_writer_{true};
		};

		class sqlite_store final : public fact_store_port
		{
		  public:
			explicit sqlite_store(std::shared_ptr<sqlite_state> shared) : shared_{std::move(shared)}
			{
			}
			[[nodiscard]] std::string backend_id() const override
			{
				return "sqlite";
			}

			[[nodiscard]] compatibility_state compatibility() const noexcept override
			{
				const std::scoped_lock lock{shared_->mutex};
				return shared_->compatibility;
			}

			[[nodiscard]] result<std::shared_ptr<const snapshot_data>> read() const override
			{
				const std::scoped_lock lock{shared_->mutex};
				if (shared_->compatibility == compatibility_state::rebuild_required)
					return sqlite_error("facts.cache-incompatible", "rebuild-required");
				if (shared_->compatibility == compatibility_state::corrupt)
					return sqlite_error("facts.store-corrupt", "integrity-check-failed");
				return shared_->current;
			}

			result<std::unique_ptr<snapshot_transaction>>
			begin(snapshot_metadata metadata, const transaction_fault fault) override
			{
				{
					const std::scoped_lock lock{shared_->mutex};
					if (shared_->compatibility != compatibility_state::compatible ||
						!metadata_compatible(metadata, shared_->expected))
						return sqlite_error("facts.cache-incompatible", "rebuild-required");
					if (shared_->writer)
						return sqlite_error("facts.transaction-failed", "writer-lock-conflict");
					shared_->writer = true;
				}
				if (auto begun = execute(*shared_->api,
										 shared_->database->get(),
										 "BEGIN IMMEDIATE",
										 "writer-lock-conflict");
					!begun)
				{
					const std::scoped_lock lock{shared_->mutex};
					shared_->writer = false;
					return sqlite_error("facts.transaction-failed", "writer-lock-conflict");
				}
				if (fault == transaction_fault::after_begin)
				{
					(void)execute(
						*shared_->api, shared_->database->get(), "ROLLBACK", "rollback-failed");
					const std::scoped_lock lock{shared_->mutex};
					shared_->writer = false;
					return sqlite_error("facts.transaction-failed", "injected-after-begin");
				}
				return std::unique_ptr<snapshot_transaction>{
					std::make_unique<sqlite_transaction>(shared_, std::move(metadata), fault)};
			}

			result<void> rebuild(snapshot_metadata metadata) override
			{
				const std::scoped_lock lock{shared_->mutex};
				if (shared_->writer)
					return sqlite_error("facts.transaction-failed", "writer-lock-conflict");
				if (shared_->compatibility == compatibility_state::corrupt)
					return sqlite_error("facts.store-corrupt", "corrupt-database-preserved");
				if (auto begun = execute(*shared_->api,
										 shared_->database->get(),
										 "BEGIN IMMEDIATE",
										 "rebuild-begin-failed");
					!begun)
					return begun;
				if (auto dropped = execute(*shared_->api,
										   shared_->database->get(),
										   "DROP TABLE IF EXISTS cxxlens_current_snapshot;"
										   "DROP TABLE IF EXISTS cxxlens_coverage_index;"
										   "DROP TABLE IF EXISTS cxxlens_edge_index;"
										   "DROP TABLE IF EXISTS cxxlens_fact_index;"
										   "DROP TABLE IF EXISTS cxxlens_snapshots;"
										   "DROP TABLE IF EXISTS cxxlens_store_metadata;",
										   "rebuild-drop-failed");
					!dropped)
				{
					(void)execute(
						*shared_->api, shared_->database->get(), "ROLLBACK", "rollback-failed");
					return dropped;
				}
				if (auto created = create_schema(*shared_->api, shared_->database->get()); !created)
				{
					(void)execute(
						*shared_->api, shared_->database->get(), "ROLLBACK", "rollback-failed");
					return created;
				}
				auto replacement = std::make_shared<snapshot_data>();
				replacement->metadata = std::move(metadata);
				replacement->metadata.generation = shared_->current->metadata.generation + 1U;
				auto encoded = encode_snapshot(*replacement);
				if (!encoded ||
					!insert_snapshot(*shared_->api,
									 shared_->database->get(),
									 *replacement,
									 extracted_span(encoded)))
				{
					(void)execute(
						*shared_->api, shared_->database->get(), "ROLLBACK", "rollback-failed");
					return sqlite_error("facts.transaction-failed", "rebuild-insert-failed");
				}
				if (auto committed = execute(
						*shared_->api, shared_->database->get(), "COMMIT", "rebuild-commit-failed");
					!committed)
					return committed;
				shared_->expected = replacement->metadata;
				shared_->current = std::move(replacement);
				shared_->compatibility = compatibility_state::compatible;
				return {};
			}

			result<void> compact() override
			{
				const std::scoped_lock lock{shared_->mutex};
				if (shared_->writer)
					return sqlite_error("facts.transaction-failed", "writer-lock-conflict");
				return execute(*shared_->api, shared_->database->get(), "VACUUM", "vacuum-failed");
			}

		  private:
			[[nodiscard]] static std::span<const std::byte>
			extracted_span(const result<std::vector<std::byte>>& encoded)
			{
				return encoded.value();
			}

			std::shared_ptr<sqlite_state> shared_;
		};

		[[nodiscard]] result<bool> schema_exists(const sqlite::api& api, sqlite::database* database)
		{
			auto query = prepare(api,
								 database,
								 "SELECT 1 FROM sqlite_master WHERE type='table' AND "
								 "name='cxxlens_current_snapshot'");
			if (!query)
				return std::move(query.error());
			const int status = api.step(query.value().get());
			if (status != sqlite::row && status != sqlite::done)
				return sqlite_error("facts.store-corrupt", "schema-probe-failed", status);
			return status == sqlite::row;
		}

		[[nodiscard]] result<bool> integrity_ok(const sqlite::api& api, sqlite::database* database)
		{
			auto query = prepare(api, database, "PRAGMA quick_check");
			if (!query)
				return false;
			if (api.step(query.value().get()) != sqlite::row)
				return false;
			const auto* text = api.column_text(query.value().get(), 0);
			return text != nullptr && std::string_view{reinterpret_cast<const char*>(text)} == "ok";
		}
	} // namespace

	result<std::shared_ptr<fact_store_port>> open_sqlite_store(const path& database_path,
															   const snapshot_metadata& expected)
	{
		if (expected.workspace_key.empty() || database_path.empty())
			return sqlite_error("facts.transaction-failed", "invalid-open-request");
		auto api = sqlite::load_api();
		if (!api)
			return std::move(api.error());
		sqlite::database* raw_database{};
		const std::string native_path = database_path.string();
		const int status = api.value()->open_v2(native_path.c_str(),
												&raw_database,
												sqlite::open_read_write | sqlite::open_create |
													sqlite::open_full_mutex,
												nullptr);
		if (status != sqlite::ok || raw_database == nullptr)
		{
			if (raw_database != nullptr)
				(void)api.value()->close_v2(raw_database);
			return sqlite_error("facts.backend-unavailable", "database-open-failed", status);
		}
		auto state = std::make_shared<sqlite_state>();
		state->api = api.value();
		state->database = std::make_unique<database_handle>(state->api, raw_database);
		state->expected = expected;
		(void)state->api->busy_timeout(raw_database, 0);
		if (!execute(*state->api,
					 raw_database,
					 "PRAGMA foreign_keys=ON;PRAGMA journal_mode=WAL;PRAGMA synchronous=FULL;",
					 "configure-database-failed"))
		{
			state->compatibility = compatibility_state::corrupt;
			return std::shared_ptr<fact_store_port>{
				std::make_shared<sqlite_store>(std::move(state))};
		}
		auto integrity = integrity_ok(*state->api, raw_database);
		if (!integrity || !integrity.value())
		{
			state->compatibility = compatibility_state::corrupt;
			return std::shared_ptr<fact_store_port>{
				std::make_shared<sqlite_store>(std::move(state))};
		}
		auto exists = schema_exists(*state->api, raw_database);
		if (!exists)
		{
			state->compatibility = compatibility_state::corrupt;
			return std::shared_ptr<fact_store_port>{
				std::make_shared<sqlite_store>(std::move(state))};
		}
		if (!exists.value())
		{
			if (auto begun = execute(
					*state->api, raw_database, "BEGIN IMMEDIATE", "initialize-begin-failed");
				!begun)
				return std::move(begun.error());
			if (auto created = create_schema(*state->api, raw_database); !created)
				return std::move(created.error());
			auto initial = std::make_shared<snapshot_data>();
			initial->metadata = expected;
			auto encoded = encode_snapshot(*initial);
			if (!encoded)
				return std::move(encoded.error());
			if (auto inserted =
					insert_snapshot(*state->api, raw_database, *initial, encoded.value());
				!inserted)
				return std::move(inserted.error());
			if (auto committed =
					execute(*state->api, raw_database, "COMMIT", "initialize-commit-failed");
				!committed)
				return std::move(committed.error());
			state->current = std::move(initial);
		}
		else
		{
			auto current = load_current(*state->api, raw_database);
			if (!current)
			{
				state->compatibility = compatibility_state::corrupt;
				return std::shared_ptr<fact_store_port>{
					std::make_shared<sqlite_store>(std::move(state))};
			}
			state->current = std::move(current.value());
			if (!metadata_compatible(state->current->metadata, expected))
				state->compatibility = compatibility_state::rebuild_required;
		}
		return std::shared_ptr<fact_store_port>{std::make_shared<sqlite_store>(std::move(state))};
	}
} // namespace cxxlens::detail::store
