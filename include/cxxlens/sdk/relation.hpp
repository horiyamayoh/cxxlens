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
	/** @brief Test exact membership in the closed scalar kind enum. */
	[[nodiscard]] constexpr bool is_valid(const scalar_kind value) noexcept
	{
		return value >= scalar_kind::boolean && value <= scalar_kind::set;
	}

	/** @brief Semantic role of a column in stable claim identity. */
	enum class column_role : std::uint8_t
	{
		claim_key,
		authoritative_payload,
		display,
		auxiliary,
	};
	/** @brief Test exact membership in the closed column role enum. */
	[[nodiscard]] constexpr bool is_valid(const column_role value) noexcept
	{
		return value >= column_role::claim_key && value <= column_role::auxiliary;
	}

	/** @brief Batch merge law declared by a relation descriptor. */
	enum class merge_mode : std::uint8_t
	{
		set,
		multiset,
		functional_assertion,
		keyed_union,
	};
	/** @brief Test exact membership in the closed merge mode enum. */
	[[nodiscard]] constexpr bool is_valid(const merge_mode value) noexcept
	{
		return value >= merge_mode::set && value <= merge_mode::keyed_union;
	}

	/** @brief Reference strength; soft references preserve their source claim. */
	enum class reference_strength : std::uint8_t
	{
		hard,
		soft_semantic,
	};
	/** @brief Test exact membership in the closed reference strength enum. */
	[[nodiscard]] constexpr bool is_valid(const reference_strength value) noexcept
	{
		return value >= reference_strength::hard && value <= reference_strength::soft_semantic;
	}

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

	/** @brief Accepted result-ID projection carried from one relation descriptor authority. */
	struct domain_identity_descriptor
	{
		std::optional<std::string> result_column;
		std::vector<std::string> projection;
		std::string contract;
		[[nodiscard]] bool operator==(const domain_identity_descriptor&) const = default;
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
		domain_identity_descriptor domain_identity;
		std::vector<relation_reference_descriptor> references;
		merge_mode merge{merge_mode::set};
		std::vector<std::string> conflict_columns;

		/** @brief Validate the exact runtime projection of the accepted relation IDL schema. */
		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] result<column_descriptor> column(std::string_view name_or_id) const;
		[[nodiscard]] std::string canonical_form() const;
		[[nodiscard]] bool operator==(const relation_descriptor&) const = default;
	};

	/** @brief Stable descriptor column, optionally bound to a query-local scan occurrence. */
	struct column_ref
	{
		std::string descriptor_id;
		std::string column_id;
		value_type type;
		/** @brief Query-local scan alias; empty only before a query builder binds the reference. */
		std::string source_alias{}; // NOLINT(readability-redundant-member-init)
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
	/** @brief Test exact membership in the closed cell state enum. */
	[[nodiscard]] constexpr bool is_valid(const cell_state value) noexcept
	{
		return value >= cell_state::present && value <= cell_state::unknown;
	}

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

	/** @brief Derive a result ID from the descriptor's ordered typed domain projection. */
	[[nodiscard]] result<std::string> derive_domain_identity(const relation_descriptor& descriptor,
															 const detached_row& row);

	/** @brief Independently require a row result ID to equal its descriptor projection. */
	[[nodiscard]] result<void> validate_domain_identity(const relation_descriptor& descriptor,
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

	/** @brief One exact compile-unit identity and its catalog authority digests. */
	struct catalog_compile_unit
	{
		/** @brief Stable compile-unit identity within the catalog. */
		std::string compile_unit_id;
		/** @brief Digest of the exact effective compiler invocation. */
		std::string effective_invocation_digest;
		/** @brief Digest of the exact source input bytes. */
		std::string source_digest;
		/** @brief Digest of the unit's effective environment authority. */
		std::string environment_digest;
		/** @brief Validate the typed identity and all exact digest grammars. */
		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] bool operator==(const catalog_compile_unit&) const = default;
	};

	/** @brief Immutable project input descriptor used by providers and snapshots. */
	struct project_catalog
	{
		/** @brief Content ID derived from the exact catalog digest. */
		std::string catalog_id;
		/** @brief Semantic digest of canonical_projection(). */
		std::string catalog_digest;
		/** @brief Logical, host-location-independent project root. */
		std::string logical_root;
		/** @brief Digest of the catalog-wide environment authority. */
		std::string environment_digest;
		/** @brief Compile units in canonical compile-unit-ID order. */
		std::vector<catalog_compile_unit> compile_units;
		/** @brief Canonicalize entries and derive the exact content-bound catalog identity. */
		[[nodiscard]] static result<project_catalog>
		make(std::string logical_root_value,
			 std::string environment_digest_value,
			 std::vector<catalog_compile_unit> compile_unit_values);
		/** @brief Encode the authoritative root/environment/compile-unit projection. */
		[[nodiscard]] result<std::vector<std::byte>> canonical_projection() const;
		/** @brief Recompute and compare the digest and catalog ID bottom-up. */
		[[nodiscard]] result<void> validate() const;
	};

} // namespace cxxlens::sdk
