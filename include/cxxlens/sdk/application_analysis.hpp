#pragma once

/** @file application_analysis.hpp @brief Experimental analysis of GCC/MSVC-built applications. */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/provider.hpp>
#include <cxxlens/sdk/store.hpp>

namespace cxxlens::sdk
{
	/** @brief Resource bounds applied before allocating from an external capture bundle. */
	struct import_limits
	{
		std::size_t maximum_bundle_bytes{std::size_t{64U} * 1024U * 1024U};
		std::size_t maximum_nesting_depth{32U};
		std::size_t maximum_compile_units{4096U};
		std::size_t maximum_arguments_per_unit{4096U};
		std::size_t maximum_auxiliary_files_per_unit{4096U};
		std::size_t maximum_environment_effects_per_unit{1024U};
		std::size_t maximum_string_bytes{4096U};
		std::size_t maximum_total_metadata_bytes{std::size_t{64U} * 1024U * 1024U};
		std::uint64_t maximum_source_closure_members{4096U};
		std::uint64_t maximum_source_closure_blobs{4096U};
		std::uint64_t maximum_source_closure_bytes{std::uint64_t{48U} * 1024U * 1024U};
		[[nodiscard]] result<void> validate() const;
	};

	/** @brief Field-level fidelity retained when capture cannot observe an exact value. */
	struct capture_gap
	{
		std::string field;
		std::string state;
		std::string reason;
		std::string completion_action;
		[[nodiscard]] bool operator==(const capture_gap&) const = default;
	};

	/** @brief Immutable, bounded, canonically decoded production build capture. */
	class capture_bundle
	{
	  public:
		struct implementation;
		[[nodiscard]] std::string_view digest() const noexcept;
		[[nodiscard]] std::string_view production_compiler() const noexcept;
		[[nodiscard]] std::string_view capture_adapter() const noexcept;
		[[nodiscard]] std::string_view target_abi() const noexcept;
		[[nodiscard]] std::string_view project_id() const noexcept;
		[[nodiscard]] std::size_t compile_unit_count() const noexcept;
		[[nodiscard]] std::span<const capture_gap> gaps() const noexcept;

	  private:
		explicit capture_bundle(std::shared_ptr<const implementation> value);
		std::shared_ptr<const implementation> value_;
		friend result<capture_bundle> decode_capture_bundle(std::span<const std::byte>,
															import_limits);
		friend result<class imported_project> import_capture(const capture_bundle&, import_limits);
	};

	/** @brief Replay fidelity of one production compiler option. */
	enum class replay_fidelity : std::uint8_t
	{
		exact,
		semantics_preserving,
		approximation,
		unsupported,
		nonsemantic,
	};
	[[nodiscard]] constexpr bool is_valid(const replay_fidelity value) noexcept
	{
		return value >= replay_fidelity::exact && value <= replay_fidelity::nonsemantic;
	}

	/** @brief One immutable compiler-neutral replay plan. */
	class replay_plan
	{
	  public:
		struct implementation;
		[[nodiscard]] std::string_view digest() const noexcept;
		[[nodiscard]] std::string_view capture_bundle_digest() const noexcept;
		[[nodiscard]] std::string_view compile_unit_id() const noexcept;
		[[nodiscard]] std::string_view analysis_frontend() const noexcept;
		[[nodiscard]] std::string_view target_abi() const noexcept;
		[[nodiscard]] std::span<const capture_gap> unresolved() const noexcept;

	  private:
		explicit replay_plan(std::shared_ptr<const implementation> value);
		std::shared_ptr<const implementation> value_;
		friend result<class imported_project> import_capture(const capture_bundle&, import_limits);
	};

	/** @brief Immutable project accepted from a validated capture and replay planner. */
	class imported_project
	{
	  public:
		struct implementation;
		[[nodiscard]] std::string_view id() const noexcept;
		[[nodiscard]] std::string_view capture_bundle_digest() const noexcept;
		[[nodiscard]] std::span<const replay_plan> replay_plans() const noexcept;
		[[nodiscard]] std::span<const capture_gap> unresolved() const noexcept;

	  private:
		explicit imported_project(std::shared_ptr<const implementation> value);
		std::shared_ptr<const implementation> value_;
		friend result<imported_project> import_capture(const capture_bundle&, import_limits);
		friend result<class materialization_result>
		materialize(snapshot_store&, const imported_project&, const class materialization_request&);
	};

	/** @brief Decode and independently validate one complete canonical capture bundle. */
	[[nodiscard]] result<capture_bundle> decode_capture_bundle(std::span<const std::byte> input,
															   import_limits limits = {});

	/** @brief Produce replay plans without reconstructing missing capture values. */
	[[nodiscard]] result<imported_project> import_capture(const capture_bundle& bundle,
														  import_limits limits = {});

	/** @brief Immutable host authority for one application materialization. */
	class materialization_request
	{
	  public:
		struct implementation;
		[[nodiscard]] static result<materialization_request>
		make(relation_engine engine,
			 snapshot_draft publication,
			 std::vector<std::string> relation_descriptor_ids,
			 std::string interpretation,
			 provider::provider_selection_request provider,
			 provider::execution_budget budget = {},
			 const std::stop_token& cancellation = {});
		[[nodiscard]] std::span<const std::string> relation_descriptor_ids() const noexcept;
		[[nodiscard]] std::string_view interpretation() const noexcept;
		[[nodiscard]] const provider::execution_budget& budget() const noexcept;

	  private:
		explicit materialization_request(std::shared_ptr<const implementation> value);
		std::shared_ptr<const implementation> value_;
		friend result<class materialization_result>
		materialize(snapshot_store&, const imported_project&, const materialization_request&);
	};

	/** @brief Public terminal after Store publication has either completed or not occurred. */
	enum class materialization_terminal : std::uint8_t
	{
		published_complete,
		published_partial,
		rejected,
		failed,
		cancelled,
	};
	[[nodiscard]] constexpr bool is_valid(const materialization_terminal value) noexcept
	{
		return value >= materialization_terminal::published_complete &&
			value <= materialization_terminal::cancelled;
	}

	/** @brief Exact provider/runtime binding retained with an application result. */
	struct application_analysis_provenance
	{
		std::string provider_id;
		semantic_version provider_version;
		std::string provider_binary_digest;
		std::string provider_semantics_digest;
		std::string task_input_digest;
		std::string replay_plan_digest;
		std::string runtime_receipt_digest;
		[[nodiscard]] bool operator==(const application_analysis_provenance&) const = default;
	};

	/** @brief Immutable materialization result preserving partiality and disagreements. */
	class materialization_result
	{
	  public:
		struct implementation;
		[[nodiscard]] materialization_terminal terminal() const noexcept;
		[[nodiscard]] const std::optional<snapshot_handle>& published_snapshot() const noexcept;
		[[nodiscard]] std::span<const provider::coverage_unit> coverage() const noexcept;
		[[nodiscard]] std::span<const provider::unresolved_item> unresolved() const noexcept;
		[[nodiscard]] std::span<const claim_conflict> conflicts() const noexcept;
		[[nodiscard]] std::span<const differential_disagreement>
		differential_disagreements() const noexcept;
		[[nodiscard]] const std::optional<application_analysis_provenance>&
		provenance() const noexcept;

	  private:
		explicit materialization_result(std::shared_ptr<const implementation> value);
		std::shared_ptr<const implementation> value_;
		friend result<materialization_result>
		materialize(snapshot_store&, const imported_project&, const materialization_request&);
	};

	/** @brief Materialize through the generic provider/runtime/writer path and publish at most
	 * once. */
	[[nodiscard]] result<materialization_result>
	materialize(snapshot_store& store,
				const imported_project& project,
				const materialization_request& request);
} // namespace cxxlens::sdk
