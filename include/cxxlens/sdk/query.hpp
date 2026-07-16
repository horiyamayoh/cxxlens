#pragma once

/** @file query.hpp @brief Typed and dynamic query surfaces normalized to one Logical Query IR. */

#include <concepts>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/sdk/relation.hpp>

namespace cxxlens::sdk::query
{
	/** @brief Exact typed literal; implicit narrowing and string coercion are unavailable. */
	struct literal
	{
		detached_cell cell;
		[[nodiscard]] static literal boolean(bool value);
		[[nodiscard]] static literal signed_integer(std::int64_t value);
		[[nodiscard]] static literal unsigned_integer(std::uint64_t value);
		[[nodiscard]] static literal utf8(std::string value);
		[[nodiscard]] static literal typed(std::string type, std::string value);
		[[nodiscard]] static literal exact(value_type type, scalar_value value);
	};

	/** @brief Validated expression serialized by stable column IDs and typed literals. */
	struct expression
	{
		std::string canonical;
		std::vector<column_ref> referenced_columns;
	};

	/** @brief Build an exact present-value equality predicate. */
	[[nodiscard]] result<expression> equals_present(column_ref column, const literal& value);
	/** @brief Build an exact column equality predicate. */
	[[nodiscard]] result<expression> equals_present(column_ref left, column_ref right);
	/** @brief Test the present cell state. */
	[[nodiscard]] expression is_present(column_ref column);
	/** @brief Test the absent cell state. */
	[[nodiscard]] expression is_absent(column_ref column);
	/** @brief Test the semantic unknown cell state. */
	[[nodiscard]] expression is_unknown(column_ref column);
	/** @brief Canonical commutative conjunction. */
	[[nodiscard]] result<expression> all(std::span<const expression> expressions);
	/** @brief Canonical commutative disjunction. */
	[[nodiscard]] result<expression> any(std::span<const expression> expressions);

	/** @brief One stable Logical Query IR node. */
	struct ir_node
	{
		std::string id;
		std::string operator_id;
		std::vector<std::string> inputs;
		std::string arguments;
		[[nodiscard]] bool operator==(const ir_node&) const = default;
	};

	/** @brief Versioned logical IR independent from physical planning and source surface. */
	struct logical_query_ir
	{
		semantic_version version{1U, 0U, 0U};
		std::vector<relation_descriptor> relation_requirements;
		std::vector<ir_node> nodes;
		std::string root;
		std::vector<column_ref> output_schema;

		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] std::string canonical_form() const;
		[[nodiscard]] std::string digest() const;
		[[nodiscard]] bool operator==(const logical_query_ir&) const = default;
	};

	/** @brief Common builder used by generated typed and runtime dynamic entry points. */
	class builder
	{
	  public:
		[[nodiscard]] static result<builder> from(const relation_descriptor& descriptor,
												  std::string_view alias = {});
		[[nodiscard]] result<builder> where(expression predicate) &&;
		[[nodiscard]] result<builder> project(std::span<const column_ref> columns) &&;
		[[nodiscard]] result<builder> inner_join(builder right, expression predicate) &&;
		[[nodiscard]] result<builder> semi_join(builder right, expression predicate) &&;
		[[nodiscard]] result<builder> union_with(const builder& right) &&;
		[[nodiscard]] result<builder> distinct() &&;
		[[nodiscard]] result<builder> order_by(std::span<const column_ref> columns) &&;
		[[nodiscard]] result<builder> limit(std::uint64_t count) &&;
		[[nodiscard]] result<builder>
		condition_restrict(std::string_view universe, std::span<const std::string> alternatives) &&;
		[[nodiscard]] result<builder> interpretation_restrict(std::string_view interpretation) &&;
		[[nodiscard]] const logical_query_ir& ir() const noexcept;
		[[nodiscard]] logical_query_ir finish() &&;

	  private:
		explicit builder(logical_query_ir ir);
		[[nodiscard]] result<void> require_columns(std::span<const column_ref> columns) const;
		[[nodiscard]] std::string
		append(std::string operator_id, std::vector<std::string> inputs, std::string arguments);
		logical_query_ir ir_;
		bool total_ordered_{};
		bool projected_{};
		std::vector<std::string> order_keys_;
	};

	/** @brief Generated relation tag concept. */
	template <class T>
	concept relation_tag = requires {
		{ T::descriptor() } -> std::same_as<const relation_descriptor&>;
	};

	/** @brief Generated column tag concept. */
	template <class T>
	concept column_tag = requires {
		{ T::ref() } -> std::same_as<column_ref>;
	};

	/** @brief Start a typed query from a generated relation tag. */
	template <relation_tag Relation>
	[[nodiscard]] result<builder> from(std::string_view alias = {})
	{
		return builder::from(Relation::descriptor(), alias);
	}

	/** @brief Materialize a stable column reference from a generated column tag. */
	template <column_tag Column>
	[[nodiscard]] column_ref col()
	{
		return Column::ref();
	}

	/** @brief Runtime relation entry point using the same common builder. */
	struct dynamic_query
	{
		[[nodiscard]] static result<builder> from(const dynamic_relation& relation,
												  std::string_view alias = {})
		{
			return builder::from(relation.descriptor(), alias);
		}
	};
} // namespace cxxlens::sdk::query
