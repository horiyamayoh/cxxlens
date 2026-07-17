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
		closed_symbol,
		set,
	};

	/** @brief Semantic role of a column in stable claim identity. */
	enum class column_role : std::uint8_t
	{
		claim_key,
		authoritative_payload,
		display,
		auxiliary,
	};

	/** @brief Batch merge law declared by a relation descriptor. */
	enum class merge_mode : std::uint8_t
	{
		set,
		multiset,
		functional_assertion,
		keyed_union,
	};

	/** @brief Reference strength; soft references preserve their source claim. */
	enum class reference_strength : std::uint8_t
	{
		hard,
		soft_semantic,
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
		column_role role{column_role::authoritative_payload};
		[[nodiscard]] bool operator==(const column_descriptor&) const = default;
	};

	/** @brief Stable cross-relation key reference. */
	struct relation_reference_descriptor
	{
		std::vector<std::string> source_columns;
		std::string target_relation;
		std::vector<std::string> target_columns;
		reference_strength strength{reference_strength::soft_semantic};
		[[nodiscard]] bool operator==(const relation_reference_descriptor&) const = default;
	};

	/** @brief Versioned relation descriptor shared by generated and dynamic APIs. */
	struct relation_descriptor
	{
		std::string id;
		std::string name;
		semantic_version version;
		std::uint32_t semantic_major{1U};
		std::string semantics;
		std::string owner_namespace;
		std::string contract_canonical;
		std::string contract_digest;
		std::string descriptor_digest;
		std::vector<column_descriptor> columns;
		std::vector<std::string> key_columns;
		std::vector<relation_reference_descriptor> references;
		merge_mode merge{merge_mode::set};
		std::vector<std::string> conflict_columns;

		/** @brief Validate the exact runtime projection of the accepted relation IDL schema. */
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
		[[nodiscard]] result<class relation_engine> build(std::string generation);
		[[nodiscard]] bool frozen() const noexcept;

	  private:
		std::map<std::string, std::shared_ptr<const relation_descriptor>, std::less<>> descriptors_;
		std::shared_ptr<bool> frozen_{std::make_shared<bool>(false)};
	};

	/** @brief Immutable registry snapshot bound to an engine generation and digest. */
	class relation_engine
	{
	  public:
		[[nodiscard]] result<dynamic_relation> require(std::string_view name,
													   std::uint32_t semantic_major) const;
		[[nodiscard]] result<dynamic_relation> require_id(std::string_view descriptor_id) const;
		[[nodiscard]] std::vector<relation_descriptor> descriptors() const;
		[[nodiscard]] std::string_view registry_digest() const noexcept;
		[[nodiscard]] std::string_view generation() const noexcept;

	  private:
		relation_engine(
			std::map<std::string, std::shared_ptr<const relation_descriptor>, std::less<>>
				descriptors,
			std::string digest,
			std::string generation);
		std::map<std::string, std::shared_ptr<const relation_descriptor>, std::less<>> descriptors_;
		std::string registry_digest_;
		std::string generation_;
		friend class relation_registry;
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

	/** @brief Typed façade over the common descriptor-aware dynamic row builder. */
	template <class Relation>
	class static_row_builder
	{
	  public:
		static_row_builder() : builder_{Relation::descriptor()} {}
		template <class Column>
		[[nodiscard]] result<void> set(const detached_cell& value)
		{
			return builder_.set(Column::ref(), value);
		}
		template <class Column>
		[[nodiscard]] result<void> set(detached_cell&& value)
		{
			return builder_.set(Column::ref(), std::move(value));
		}
		[[nodiscard]] result<detached_row> finish() &&
		{
			return std::move(builder_).finish();
		}

	  private:
		row_builder builder_;
	};

	/** @brief Typed read façade that still returns detached, value-owned cells. */
	template <class Relation>
	class static_row_view
	{
	  public:
		explicit static_row_view(const detached_row& row) : row_{row} {}
		template <class Column>
		[[nodiscard]] result<detached_cell> get() const
		{
			if (row_.descriptor_id != Relation::descriptor().id)
				return unexpected(
					{"sdk.row-descriptor-mismatch", "descriptor_id", row_.descriptor_id});
			const auto reference = Column::ref();
			const auto found = row_.cells.find(reference.column_id);
			if (found == row_.cells.end())
			{
				if (reference.type.optional)
					return detached_cell::absent(reference.type);
				return unexpected({"sdk.required-cell-missing", reference.column_id, {}});
			}
			return found->second;
		}

	  private:
		const detached_row& row_;
	};

	/** @brief Immutable project input descriptor used by providers and snapshots. */
	struct project_catalog
	{
		std::string catalog_id;
		std::string catalog_digest;
		std::string logical_root;
		std::vector<std::string> compile_units;
		[[nodiscard]] result<void> validate() const;
	};

} // namespace cxxlens::sdk
