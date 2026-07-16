#pragma once

/** @file relation.hpp @brief Runtime and generated relation authoring over one descriptor model. */

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include <cxxlens/sdk/common.hpp>

namespace cxxlens::sdk
{
	/** @brief Closed NG0 scalar kind registry; relation identity remains string based. */
	enum class scalar_kind : std::uint8_t
	{
		boolean,
		signed_integer,
		unsigned_integer,
		utf8_string,
		bytes,
		digest,
		semantic_version,
		typed_id,
		open_symbol,
		condition_ref,
		source_span_id,
		evidence_id,
	};

	/** @brief Exact detached column type. */
	struct value_type
	{
		scalar_kind scalar{scalar_kind::utf8_string};
		std::string parameter;
		bool optional{};
		[[nodiscard]] bool operator==(const value_type&) const = default;
		[[nodiscard]] std::string canonical_name() const;
	};

	/** @brief Relation column descriptor identified by a stable column ID. */
	struct column_descriptor
	{
		std::string id;
		std::string name;
		value_type type;
		bool required{true};
		[[nodiscard]] bool operator==(const column_descriptor&) const = default;
	};

	/** @brief Versioned relation descriptor shared by generated and dynamic APIs. */
	struct relation_descriptor
	{
		std::string id;
		std::string name;
		semantic_version version;
		std::uint32_t semantic_major{1U};
		std::string descriptor_digest;
		std::vector<column_descriptor> columns;

		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] result<column_descriptor> column(std::string_view name_or_id) const;
		[[nodiscard]] std::string canonical_form() const;
		[[nodiscard]] bool operator==(const relation_descriptor&) const = default;
	};

	/** @brief Stable reference to one descriptor column. */
	struct column_ref
	{
		std::string descriptor_id;
		std::string column_id;
		value_type type;
		[[nodiscard]] bool operator==(const column_ref&) const = default;
	};

	/** @brief Immutable handle for runtime-discovered relation metadata. */
	class dynamic_relation
	{
	  public:
		explicit dynamic_relation(std::shared_ptr<const relation_descriptor> descriptor);
		[[nodiscard]] const relation_descriptor& descriptor() const noexcept;
		[[nodiscard]] result<column_ref> column(std::string_view name_or_id) const;

	  private:
		std::shared_ptr<const relation_descriptor> descriptor_;
	};

	/** @brief Build-time registry with fail-closed duplicate and version handling. */
	class relation_registry
	{
	  public:
		[[nodiscard]] result<void> add(relation_descriptor descriptor);
		[[nodiscard]] result<dynamic_relation> require(std::string_view name,
													   std::uint32_t semantic_major) const;
		[[nodiscard]] std::vector<relation_descriptor> descriptors() const;

	  private:
		std::map<std::string, std::shared_ptr<const relation_descriptor>, std::less<>> descriptors_;
	};

	/** @brief Detached cell state; absence and semantic unknown are distinct. */
	enum class cell_state : std::uint8_t
	{
		present,
		absent,
		unknown,
	};

	/** @brief Pointer-free detached scalar storage. */
	using scalar_value =
		std::variant<bool, std::int64_t, std::uint64_t, std::string, std::vector<std::byte>>;

	/** @brief One typed detached cell. Native pointers are not representable. */
	struct detached_cell
	{
		value_type type;
		cell_state state{cell_state::present};
		std::optional<scalar_value> value;
		std::optional<std::string> unknown_reason;

		[[nodiscard]] static detached_cell boolean(bool value);
		[[nodiscard]] static detached_cell signed_integer(std::int64_t value);
		[[nodiscard]] static detached_cell unsigned_integer(std::uint64_t value);
		[[nodiscard]] static detached_cell utf8(std::string value);
		[[nodiscard]] static detached_cell bytes(std::vector<std::byte> value);
		[[nodiscard]] static detached_cell typed(std::string type, std::string value);
		[[nodiscard]] static detached_cell absent(value_type type);
		[[nodiscard]] static detached_cell unknown(value_type type, std::string reason);
		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] std::string canonical_form() const;
	};

	/** @brief Value-owned row detached from compiler and cursor lifetimes. */
	struct detached_row
	{
		std::string descriptor_id;
		std::map<std::string, detached_cell, std::less<>> cells;
		[[nodiscard]] std::string canonical_form() const;
	};

	/** @brief Descriptor-aware row builder and validator. */
	class row_builder
	{
	  public:
		explicit row_builder(relation_descriptor descriptor);
		[[nodiscard]] result<void> set(column_ref column, detached_cell value);
		[[nodiscard]] result<detached_row> finish() &&;

	  private:
		relation_descriptor descriptor_;
		detached_row row_;
	};

	/** @brief Independently validate a detached row against an exact descriptor. */
	[[nodiscard]] result<void> validate_row(const relation_descriptor& descriptor,
											const detached_row& row);

	/** @brief Immutable project input descriptor used by providers and snapshots. */
	struct project_catalog
	{
		std::string catalog_id;
		std::string catalog_digest;
		std::string logical_root;
		std::vector<std::string> compile_units;
		[[nodiscard]] result<void> validate() const;
	};

	class row_cursor;

	/** @brief Immutable, concurrent-readable in-memory snapshot handle. */
	class snapshot_handle
	{
	  public:
		struct data;
		[[nodiscard]] std::string_view id() const noexcept;
		[[nodiscard]] result<row_cursor> open(const dynamic_relation& relation) const;
		[[nodiscard]] bool empty() const noexcept;

	  private:
		explicit snapshot_handle(std::shared_ptr<const data> data);
		std::shared_ptr<const data> data_;
		friend class snapshot_builder;
	};

	/** @brief Cursor-scoped view invalidated by cursor advance. */
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

	/** @brief Thread-affine bounded cursor over an immutable snapshot partition. */
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

	/** @brief Draft builder that publishes one immutable snapshot atomically. */
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
