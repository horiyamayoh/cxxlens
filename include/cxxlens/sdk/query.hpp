#pragma once

/** @file query.hpp @brief Typed and dynamic query surfaces normalized to one Logical Query IR. */

#include <concepts>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <cxxlens/sdk/store.hpp>

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
	/** @brief Bind a stable descriptor column to one query-local scan occurrence. */
	[[nodiscard]] result<column_ref> qualify(column_ref column, std::string_view source_alias);
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

	/** @brief Availability policy decoded from one schema-bound IR column reference. */
	enum class column_availability : std::uint8_t
	{
		require,
		absent_if_schema_missing,
	};

	/** @brief Occurrence-qualified IR column reference independent from generated C++ types. */
	struct ir_column_ref
	{
		std::string column_id;
		column_availability availability{column_availability::require};
		/** @brief Required scan occurrence alias in validated Logical Query IR. */
		std::string source_alias{}; // NOLINT(readability-redundant-member-init)
		[[nodiscard]] bool operator==(const ir_column_ref&) const = default;
	};

	/** @brief Exact typed literal decoded from canonical Logical Query IR. */
	struct ir_literal
	{
		std::string type;
		scalar_value value;
		[[nodiscard]] bool operator==(const ir_literal&) const = default;
	};

	/** @brief Closed predicate kind set used by the NG0 executor. */
	enum class predicate_kind : std::uint8_t
	{
		equals_present,
		column_equals_present,
		is_present,
		is_absent,
		is_unknown,
		all,
		any,
	};

	/** @brief Typed recursive predicate decoded with exact keys and no coercion. */
	struct decoded_predicate
	{
		predicate_kind kind{predicate_kind::is_present};
		std::optional<ir_column_ref> column;
		std::optional<ir_column_ref> left;
		std::optional<ir_column_ref> right;
		std::optional<ir_literal> literal_value;
		std::vector<decoded_predicate> operands;
		[[nodiscard]] bool operator==(const decoded_predicate&) const = default;
	};

	struct scan_arguments
	{
		std::string descriptor_id;
		std::string alias;
	};
	struct predicate_arguments
	{
		decoded_predicate predicate;
	};
	struct projection_item
	{
		ir_column_ref column;
		std::string output;
	};
	struct project_arguments
	{
		std::vector<projection_item> columns;
	};
	struct empty_arguments
	{
		[[nodiscard]] bool operator==(const empty_arguments&) const = default;
	};
	struct order_key
	{
		ir_column_ref column;
		bool ascending{true};
		std::vector<cell_state> cell_state_order;
	};
	struct order_arguments
	{
		std::vector<order_key> keys;
	};
	struct limit_arguments
	{
		std::uint64_t count{};
	};
	struct condition_arguments
	{
		std::string universe;
		std::vector<std::string> alternatives;
	};
	struct interpretation_arguments
	{
		std::string interpretation;
	};

	/** @brief Typed operator payload; operator ID still owns the exact variant correspondence. */
	using operator_arguments = std::variant<scan_arguments,
											predicate_arguments,
											project_arguments,
											empty_arguments,
											order_arguments,
											limit_arguments,
											condition_arguments,
											interpretation_arguments>;

	/** @brief Decode and independently validate one node's exact canonical argument object. */
	[[nodiscard]] result<operator_arguments> decode_arguments(const ir_node& node);

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
		[[nodiscard]] result<column_ref> bind_column(const column_ref& column) const;
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

	/** @brief Runtime limits excluded from Logical Query IR identity. */
	struct execution_budget
	{
		std::uint64_t max_rows_scanned{std::numeric_limits<std::uint64_t>::max()};
		std::uint64_t max_rows_output{std::numeric_limits<std::uint64_t>::max()};
		std::uint64_t max_intermediate_rows{std::numeric_limits<std::uint64_t>::max()};
		std::uint64_t max_memory_bytes{std::numeric_limits<std::uint64_t>::max()};
	};

	/** @brief Stable semantic checkpoint for schedule-independent cancellation observation. */
	struct execution_checkpoint
	{
		enum class phase : std::uint8_t
		{
			before_execution,
			before_publish_row,
		};
		phase current{phase::before_execution};
		std::uint64_t ordinal{};
	};

	/** @brief Synchronously borrowed cancellation probe; implementations must be noexcept. */
	class cancellation_probe
	{
	  public:
		virtual ~cancellation_probe() = default;
		[[nodiscard]] virtual bool
		stop_requested(const execution_checkpoint& checkpoint) const noexcept = 0;
	};

	/** @brief Standard stop-token adapter for the deterministic query checkpoints. */
	class stop_token_cancellation final : public cancellation_probe
	{
	  public:
		explicit stop_token_cancellation(std::stop_token token) noexcept;
		[[nodiscard]] bool
		stop_requested(const execution_checkpoint& checkpoint) const noexcept override;

	  private:
		std::stop_token token_;
	};

	/** @brief One execution request whose operational controls never alter the IR digest. */
	struct execution_request
	{
		execution_budget budget;
		const cancellation_probe* cancellation{};
	};

	/** @brief Execution status distinct from input completeness and closed-world proof. */
	enum class execution_status : std::uint8_t
	{
		complete,
		truncated,
		cancelled_with_partial,
		failed_before_result,
	};

	/** @brief One semantic row with canonical contributor evidence sets. */
	struct annotated_row
	{
		std::map<std::string, detached_cell, std::less<>> values;
		std::uint64_t multiplicity{1U};
		claim_condition presence;
		std::string interpretation;
		std::vector<std::string> claim_contributors;
		std::vector<claim_producer> producer_contracts;
		std::vector<std::string> provenance;
		std::vector<claim_guarantee> contributor_guarantees;
		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] std::string canonical_form() const;
	};

	/** @brief Structured input or execution partiality; never collapsed into an empty result. */
	struct query_unresolved
	{
		std::string code;
		std::string subject;
		std::string detail;
		[[nodiscard]] bool operator==(const query_unresolved&) const = default;
	};

	/** @brief Logical or physical explanation with an explicit authority-specific ID. */
	struct query_explanation
	{
		std::string id;
		std::string text;
		[[nodiscard]] bool operator==(const query_explanation&) const = default;
	};

	class result_row_cursor;

	/** @brief Immutable query result owning rows and all partiality/evidence side channels. */
	class query_result
	{
	  public:
		struct data;
		[[nodiscard]] result_row_cursor rows() const;
		[[nodiscard]] execution_status execution() const noexcept;
		[[nodiscard]] bool ordered() const noexcept;
		[[nodiscard]] bool inputs_complete() const noexcept;
		[[nodiscard]] bool closed() const noexcept;
		[[nodiscard]] std::span<const snapshot_query_coverage> input_coverage() const noexcept;
		/** @brief Exact certificates applied to every required query input range. */
		[[nodiscard]] std::span<const std::string> closure_ids() const noexcept;
		[[nodiscard]] std::span<const query_unresolved> unresolved_items() const noexcept;
		/** @brief Same-domain functional payload conflicts classified by descriptor projection. */
		[[nodiscard]] std::span<const claim_conflict> conflicts() const noexcept;
		/** @brief Cross-domain payload disagreements classified by the same claim-kernel law. */
		[[nodiscard]] std::span<const differential_disagreement>
		differential_disagreements() const noexcept;
		[[nodiscard]] std::span<const claim_producer> producer_contracts() const noexcept;
		[[nodiscard]] const claim_guarantee& summary_guarantee() const noexcept;
		[[nodiscard]] const query_explanation& explain_logical() const noexcept;
		[[nodiscard]] const query_explanation& explain_physical() const noexcept;
		[[nodiscard]] std::string_view logical_ir_digest() const noexcept;
		[[nodiscard]] std::string_view snapshot_id() const noexcept;
		[[nodiscard]] std::string canonical_form() const;

	  private:
		explicit query_result(std::shared_ptr<const data> data);
		std::shared_ptr<const data> data_;
		friend class reference_engine;
	};

	/** @brief Cursor-scoped result row view invalidated by cursor advance. */
	class result_row_view
	{
	  public:
		[[nodiscard]] result<annotated_row> copy() const;

	  private:
		result_row_view(const annotated_row* row,
						std::weak_ptr<const std::uint64_t> generation,
						std::uint64_t expected);
		const annotated_row* row_{};
		std::weak_ptr<const std::uint64_t> generation_;
		std::uint64_t expected_{};
		friend class result_row_cursor;
	};

	/** @brief Thread-affine bounded cursor over immutable query-result rows. */
	class result_row_cursor
	{
	  public:
		result_row_cursor(result_row_cursor&&) noexcept = default;
		result_row_cursor& operator=(result_row_cursor&&) noexcept = default;
		result_row_cursor(const result_row_cursor&) = delete;
		result_row_cursor& operator=(const result_row_cursor&) = delete;
		[[nodiscard]] result<std::optional<result_row_view>> next();

	  private:
		explicit result_row_cursor(std::shared_ptr<const query_result::data> result);
		std::shared_ptr<const query_result::data> result_;
		std::size_t index_{};
		std::thread::id owner_;
		std::shared_ptr<std::uint64_t> generation_;
		friend class query_result;
	};

	/** @brief Backend-independent deterministic NG0 reference query engine. */
	class reference_engine
	{
	  public:
		[[nodiscard]] static result<reference_engine> bind(snapshot_handle snapshot);
		[[nodiscard]] result<query_result> execute(const logical_query_ir& query,
												   execution_request request = {}) const;

	  private:
		explicit reference_engine(snapshot_handle snapshot);
		snapshot_handle snapshot_;
	};
} // namespace cxxlens::sdk::query
