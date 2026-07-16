#pragma once

/** @file store.hpp @brief Immutable semantic snapshots with memory and SQLite parity. */

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <cxxlens/sdk/claim.hpp>

namespace cxxlens::sdk
{
	/** @brief Exact authority tuple selecting one publication series. */
	struct snapshot_series_selector
	{
		std::string catalog_id;
		std::string channel_id;
		std::string engine_generation_id;
		std::string condition_universe_id;
		std::string relation_registry_digest;
		std::string interpretation_policy_digest;
		std::string trust_policy_digest;
		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] std::string id() const;
		[[nodiscard]] bool operator==(const snapshot_series_selector&) const = default;
	};

	/** @brief One explicit coverage classification stored with a partition. */
	struct snapshot_coverage_unit
	{
		std::string domain;
		std::string key;
		std::string state;
		std::string reason;
		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] std::string canonical_form() const;
		[[nodiscard]] bool operator==(const snapshot_coverage_unit&) const = default;
	};

	/** @brief Staged semantic partition; claims remain invisible until publication. */
	struct partition_draft
	{
		std::string relation_descriptor_id;
		std::string scope;
		claim_condition condition;
		std::string interpretation;
		std::string producer_semantics;
		std::string producer_input_basis_digest;
		std::string precision_profile;
		std::string assumption_set_id;
		std::vector<claim> claims;
		std::vector<snapshot_coverage_unit> coverage;
		std::vector<unresolved_reference> unresolved;
	};

	/** @brief Immutable identity-bearing projection of one published partition. */
	struct partition_manifest
	{
		std::string partition_id;
		std::string relation_descriptor_id;
		std::string input_basis_digest;
		std::string claim_set_digest;
		std::string coverage_digest;
		std::string content_digest;
		std::uint64_t claim_count{};
		bool complete{};
		[[nodiscard]] bool operator==(const partition_manifest&) const = default;
	};

	/** @brief Candidate for an exact completeness/closure certificate. */
	struct closure_candidate
	{
		std::string relation_descriptor_id;
		std::string subject_partition_id;
		std::string partition_content_digest;
		std::string coverage_digest;
		std::string key_domain_digest;
		claim_condition condition;
		std::string interpretation;
		std::string assumption_set_id;
		std::string closure_kind;
		std::string producer_semantics;
		std::string evidence_digest;
		[[nodiscard]] bool operator==(const closure_candidate&) const = default;
	};

	/** @brief Validated closure identity bound to exact partition content and coverage. */
	struct closure_certificate
	{
		std::string id;
		closure_candidate subject;
		[[nodiscard]] bool operator==(const closure_certificate&) const = default;
	};

	/** @brief Independently validate and derive the exact partition identity projection. */
	[[nodiscard]] result<partition_manifest> make_partition_manifest(const relation_engine& engine,
																	 const partition_draft& draft);
	/** @brief Independently validate an exact closure binding and derive its identity. */
	[[nodiscard]] result<closure_certificate>
	make_closure_certificate(const partition_manifest& partition, closure_candidate candidate);

	/** @brief Semantic manifest; operational publication fields are deliberately absent. */
	struct snapshot_manifest
	{
		std::string schema{"cxxlens.snapshot-manifest.v1"};
		std::string id;
		semantic_version snapshot_semantics_version{1U, 0U, 0U};
		std::string catalog_semantic_digest;
		std::string condition_universe_id;
		std::string relation_registry_digest;
		std::string interpretation_policy_digest;
		std::vector<partition_manifest> partitions;
		std::vector<std::string> closure_ids;
		[[nodiscard]] bool operator==(const snapshot_manifest&) const = default;
	};

	/** @brief Transaction input with an explicit compare-and-swap parent. */
	struct snapshot_draft
	{
		snapshot_series_selector series;
		semantic_version snapshot_semantics_version{1U, 0U, 0U};
		std::string catalog_semantic_digest;
		std::optional<std::string> expected_parent_publication;
	};

	/** @brief Publication lifecycle; only committed records are reader-visible. */
	enum class publication_state : std::uint8_t
	{
		created,
		staged,
		validating,
		committed,
		rejected,
		rolled_back,
	};

	/** @brief Operational record separated from semantic snapshot identity. */
	struct publication_record
	{
		std::string publication_id;
		std::string series_id;
		std::string snapshot_id;
		std::uint64_t sequence{};
		std::uint64_t physical_generation{};
		std::optional<std::string> parent_publication;
		publication_state state{publication_state::created};
		bool corrupt{};
		[[nodiscard]] bool operator==(const publication_record&) const = default;
	};

	/** @brief Backend compatibility without silent migration or fallback. */
	struct store_compatibility
	{
		std::string backend;
		semantic_version readable_format;
		bool direct_open{};
		bool migration_required{};
	};

	class row_cursor;
	class snapshot_writer;

	/** @brief Immutable concurrent-readable snapshot pinning one physical generation. */
	class snapshot_handle
	{
	  public:
		struct data;
		[[nodiscard]] std::string_view id() const noexcept;
		[[nodiscard]] const snapshot_manifest& manifest() const;
		[[nodiscard]] const publication_record& publication() const;
		[[nodiscard]] result<row_cursor> open(const dynamic_relation& relation) const;
		[[nodiscard]] bool empty() const noexcept;

	  private:
		explicit snapshot_handle(std::shared_ptr<const data> data);
		std::shared_ptr<const data> data_;
		friend class snapshot_store;
		friend class snapshot_builder;
		friend class snapshot_writer;
	};

	/** @brief Cursor-scoped row view invalidated by cursor advance. */
	class row_view
	{
	  public:
		[[nodiscard]] result<detached_row> copy() const;

	  private:
		row_view(const detached_row* row,
				 std::weak_ptr<const std::uint64_t> generation,
				 std::uint64_t expected);
		const detached_row* row_{};
		std::weak_ptr<const std::uint64_t> generation_;
		std::uint64_t expected_{};
		friend class row_cursor;
	};

	/** @brief Thread-affine cursor whose snapshot pin outlives backend statements/pages. */
	class row_cursor
	{
	  public:
		row_cursor(row_cursor&&) noexcept = default;
		row_cursor& operator=(row_cursor&&) noexcept = default;
		row_cursor(const row_cursor&) = delete;
		row_cursor& operator=(const row_cursor&) = delete;
		[[nodiscard]] result<std::optional<row_view>> next();

	  private:
		row_cursor(std::shared_ptr<const snapshot_handle::data> snapshot,
				   const std::vector<detached_row>* rows);
		std::shared_ptr<const snapshot_handle::data> snapshot_;
		const std::vector<detached_row>* rows_{};
		std::size_t index_{};
		std::thread::id owner_;
		std::shared_ptr<std::uint64_t> generation_;
		friend class snapshot_handle;
	};

	/** @brief Immutable snapshot store façade with interchangeable semantic backends. */
	class snapshot_store
	{
	  public:
		struct implementation;
		[[nodiscard]] result<snapshot_handle>
		current(const snapshot_series_selector& selector) const;
		[[nodiscard]] result<snapshot_handle> open(std::string_view snapshot_id) const;
		[[nodiscard]] result<snapshot_handle>
		open_publication(std::string_view publication_id) const;
		[[nodiscard]] result<snapshot_writer> begin(snapshot_draft draft);
		[[nodiscard]] store_compatibility compatibility() const;
		[[nodiscard]] result<void> compact();
		[[nodiscard]] result<std::string> canonical_export(std::string_view snapshot_id) const;
		[[nodiscard]] std::size_t retained_generation_count() const;

	  private:
		explicit snapshot_store(std::shared_ptr<implementation> implementation);
		std::shared_ptr<implementation> implementation_;
		friend result<snapshot_store> make_in_memory_snapshot_store(relation_engine);
		friend result<snapshot_store> open_sqlite_snapshot_store(const std::string&,
																 relation_engine);
		friend result<void> mark_publication_corrupt_for_testing(snapshot_store&, std::string_view);
	};

	/** @brief Transactional writer enforcing stage, independent validation, then atomic publish. */
	class snapshot_writer
	{
	  public:
		snapshot_writer(snapshot_writer&&) noexcept = default;
		snapshot_writer& operator=(snapshot_writer&&) noexcept = default;
		snapshot_writer(const snapshot_writer&) = delete;
		snapshot_writer& operator=(const snapshot_writer&) = delete;
		~snapshot_writer();
		[[nodiscard]] publication_state state() const noexcept;
		[[nodiscard]] result<void> stage(partition_draft partition);
		[[nodiscard]] result<void> add_closure(closure_candidate closure);
		[[nodiscard]] result<void> validate();
		[[nodiscard]] result<snapshot_handle> publish();
		void cancel() noexcept;

	  private:
		struct data;
		explicit snapshot_writer(std::unique_ptr<data> data);
		std::unique_ptr<data> data_;
		friend class snapshot_store;
	};

	/** @brief Construct the normative process-local reference backend. */
	[[nodiscard]] result<snapshot_store> make_in_memory_snapshot_store(relation_engine engine);
	/** @brief Open/create the normative SQLite backend at an explicit physical locator. */
	[[nodiscard]] result<snapshot_store>
	open_sqlite_snapshot_store(const std::string& database_path, relation_engine engine);
	/** @brief Fault injection used by conformance tests to verify fail-closed recovery. */
	[[nodiscard]] result<void>
	mark_publication_corrupt_for_testing(snapshot_store& store, std::string_view publication_id);

	/** @brief Compatibility builder retained as a one-snapshot adapter until Issue #72. */
	class snapshot_builder
	{
	  public:
		explicit snapshot_builder(relation_registry registry);
		[[nodiscard]] result<void> add(detached_row row);
		[[nodiscard]] result<snapshot_handle> publish() &&;

	  private:
		relation_registry registry_;
		std::map<std::string, std::vector<detached_row>, std::less<>> rows_;
	};
} // namespace cxxlens::sdk
