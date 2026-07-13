#pragma once

/** @file flow.hpp @brief Issue #50 non-installed flow Contract Candidate. */

#include "models.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/select.hpp>
#include <cxxlens/workspace.hpp>

namespace cxxlens::flow
{
	enum class cfg_availability_state : std::uint8_t
	{
		available,
		absent,
		unsupported,
		failed,
		partial,
		stale,
		variant_divergent,
	};

	struct cfg_availability
	{
		cfg_availability_state state{cfg_availability_state::absent};
		semantic_version provider_version;
		std::vector<build_variant_id> available_variants;
		std::vector<build_variant_id> unavailable_variants;
		std::vector<diagnostic> diagnostics;
		std::vector<unresolved> unresolved_items;
		coverage_report coverage;
	};

	enum class value_path_kind : std::uint8_t
	{
		whole_value,
		field,
		element,
		pointee,
		alias,
	};

	struct value_path
	{
		value_path_kind kind{value_path_kind::whole_value};
		std::vector<std::string> components;
		std::optional<std::size_t> argument_index;
	};

	struct flow_node
	{
		std::string id;
		std::string label;
		std::string kind;
		source_span source;
		std::optional<symbol_id> symbol;
		std::optional<build_variant_id> variant;
	};

	class flow_path
	{
	  public:
		[[nodiscard]] std::string_view id() const noexcept;
		[[nodiscard]] const flow_node& source() const noexcept;
		[[nodiscard]] const flow_node& sink() const noexcept;
		[[nodiscard]] std::span<const flow_node> steps() const noexcept;
		[[nodiscard]] confidence certainty() const noexcept;
		[[nodiscard]] result_guarantee guarantee() const noexcept;
		[[nodiscard]] const evidence& why() const noexcept;
		[[nodiscard]] std::string to_json() const;
		[[nodiscard]] std::string to_dot() const;

	  private:
		struct data;
		std::shared_ptr<const data> data_;
	};

	class source_model
	{
	  public:
		[[nodiscard]] static source_model call_return(select::call_selector call);
		[[nodiscard]] static source_model parameter(select::symbol_selector callable, std::size_t index);
		[[nodiscard]] static source_model receiver(select::call_selector call);
		[[nodiscard]] static source_model field(select::symbol_selector field);
		[[nodiscard]] static source_model global(select::symbol_selector variable);
		[[nodiscard]] source_model path(value_path value) const;
		[[nodiscard]] source_model metadata(
			std::string id,
			semantic_version version,
			std::string label,
			std::string category,
			std::int32_t priority = 0) const;

	  private:
		struct data;
		std::shared_ptr<const data> data_;
	};

	class sink_model
	{
	  public:
		[[nodiscard]] static sink_model call_argument(select::call_selector call, std::size_t index);
		[[nodiscard]] static sink_model call_receiver(select::call_selector call);
		[[nodiscard]] static sink_model return_value(select::symbol_selector callable);
		[[nodiscard]] static sink_model field_write(select::symbol_selector field);
		[[nodiscard]] static sink_model global_write(select::symbol_selector variable);
		[[nodiscard]] static sink_model dereference();
		[[nodiscard]] static sink_model array_index();
		[[nodiscard]] static sink_model format_string();
		[[nodiscard]] static sink_model command_execution();
		[[nodiscard]] static sink_model file_path_open();
		[[nodiscard]] sink_model path(value_path value) const;
		[[nodiscard]] sink_model metadata(
			std::string id,
			semantic_version version,
			std::string label,
			std::string category,
			std::int32_t priority = 0) const;

	  private:
		struct data;
		std::shared_ptr<const data> data_;
	};

	enum class barrier_effect : std::uint8_t
	{
		kill,
		transform,
		conditional,
		partial,
	};

	class barrier_model
	{
	  public:
		[[nodiscard]] static barrier_model validation_call(select::call_selector call);
		[[nodiscard]] static barrier_model sanitizer_call(select::call_selector call);
		[[nodiscard]] static barrier_model bounds_check();
		[[nodiscard]] static barrier_model null_check();
		[[nodiscard]] barrier_model effect(barrier_effect value, std::string output_label = {}) const;
		[[nodiscard]] barrier_model path(value_path value) const;
		[[nodiscard]] barrier_model metadata(
			std::string id,
			semantic_version version,
			std::string label,
			std::string category,
			std::int32_t priority = 0) const;

	  private:
		struct data;
		std::shared_ptr<const data> data_;
	};

	enum class flow_sensitivity : std::uint8_t
	{
		value,
		field,
		path,
		alias_bounded,
	};

	enum class unknown_call_policy : std::uint8_t
	{
		preserve_unresolved,
		conservative_propagation,
		reject,
	};

	struct flow_options
	{
		analysis_scope scope{analysis_scope::all()};
		std::size_t max_context_depth{3};
		std::size_t max_paths{1024};
		std::size_t max_steps{100000};
		std::size_t max_states{100000};
		std::size_t widening_after_iterations{32};
		flow_sensitivity sensitivity{flow_sensitivity::field};
		unknown_call_policy unknown_calls{unknown_call_policy::preserve_unresolved};
		execution_context execution;
	};

	class taint_policy
	{
	  public:
		[[nodiscard]] taint_policy label(std::string value) const;
		[[nodiscard]] taint_policy source(source_model value) const;
		[[nodiscard]] taint_policy sink(sink_model value) const;
		[[nodiscard]] taint_policy barrier(barrier_model value) const;
		[[nodiscard]] taint_policy propagate(std::string input_label, std::string output_label) const;
		[[nodiscard]] taint_policy interprocedural(bool value = true) const;
		[[nodiscard]] taint_policy precision(precision_level value) const;
		[[nodiscard]] result<void> validate() const;

	  private:
		struct data;
		std::shared_ptr<const data> data_;
	};

	class taint_report
	{
	  public:
		[[nodiscard]] std::span<const flow_path> paths() const noexcept;
		[[nodiscard]] const finding_set& findings() const noexcept;
		[[nodiscard]] const cfg_availability& cfg() const noexcept;
		[[nodiscard]] const coverage_report& coverage() const noexcept;
		[[nodiscard]] std::span<const unresolved> unresolved_items() const noexcept;
		[[nodiscard]] std::span<const diagnostic> diagnostics() const noexcept;
		[[nodiscard]] result_guarantee guarantee() const noexcept;
		[[nodiscard]] std::string to_json() const;
		[[nodiscard]] std::string to_sarif() const;

	  private:
		struct data;
		std::shared_ptr<const data> data_;
	};

	[[nodiscard]] result<cfg_availability> inspect_cfg(
		const workspace& workspace, analysis_scope scope = analysis_scope::all(), execution_context execution = {});
	[[nodiscard]] result<taint_report> run_taint(
		const workspace& workspace, taint_policy policy, flow_options options = {});

	enum class resource_transition_kind : std::uint8_t
	{
		acquire,
		release,
		transfer,
		borrow,
		use,
		escape,
		invalidate,
		error,
	};

	struct resource_transition
	{
		resource_transition_kind kind{resource_transition_kind::use};
		std::string from_state;
		std::string to_state;
		select::call_selector call;
		std::optional<std::size_t> object_argument;
		bool conditional{};
	};

	class resource_protocol
	{
	  public:
		[[nodiscard]] resource_protocol resource(std::string name) const;
		[[nodiscard]] resource_protocol initial_state(std::string state) const;
		[[nodiscard]] resource_protocol terminal_state(std::string state) const;
		[[nodiscard]] resource_protocol error_state(std::string state) const;
		[[nodiscard]] resource_protocol transition(resource_transition value) const;
		[[nodiscard]] resource_protocol acquire(select::call_selector call) const;
		[[nodiscard]] resource_protocol release(select::call_selector call) const;
		[[nodiscard]] resource_protocol use(select::call_selector call) const;
		[[nodiscard]] resource_protocol escape(select::call_selector call) const;
		[[nodiscard]] resource_protocol invalid_use_message(std::string value) const;
		[[nodiscard]] resource_protocol leak_message(std::string value) const;
		[[nodiscard]] result<void> validate() const;

	  private:
		struct data;
		std::shared_ptr<const data> data_;
	};

	class resource_report
	{
	  public:
		[[nodiscard]] const finding_set& findings() const noexcept;
		[[nodiscard]] std::span<const flow_path> counterexamples() const noexcept;
		[[nodiscard]] const cfg_availability& cfg() const noexcept;
		[[nodiscard]] const coverage_report& coverage() const noexcept;
		[[nodiscard]] std::span<const unresolved> unresolved_items() const noexcept;
		[[nodiscard]] std::span<const diagnostic> diagnostics() const noexcept;
		[[nodiscard]] result_guarantee guarantee() const noexcept;
		[[nodiscard]] std::string to_json() const;

	  private:
		struct data;
		std::shared_ptr<const data> data_;
	};

	[[nodiscard]] result<resource_report> check_resource_protocol(
		const workspace& workspace, resource_protocol protocol, flow_options options = {});

	struct effect_summary
	{
		symbol_id callable;
		std::string canonical_signature;
		std::string model_pack_id;
		semantic_version model_pack_version;
		std::vector<build_variant_id> variants;
		std::vector<models::effect_kind> effects;
		std::vector<std::size_t> input_to_return;
		std::vector<std::pair<std::size_t, std::size_t>> input_to_output;
		confidence certainty{confidence::possible};
		evidence why;
	};

	class effect_summary_report
	{
	  public:
		[[nodiscard]] std::span<const effect_summary> summaries() const noexcept;
		[[nodiscard]] const cfg_availability& cfg() const noexcept;
		[[nodiscard]] const coverage_report& coverage() const noexcept;
		[[nodiscard]] std::span<const unresolved> unresolved_items() const noexcept;
		[[nodiscard]] std::span<const diagnostic> diagnostics() const noexcept;
		[[nodiscard]] bool converged() const noexcept;
		[[nodiscard]] std::string to_json() const;

	  private:
		struct data;
		std::shared_ptr<const data> data_;
	};

	[[nodiscard]] result<effect_summary_report> build_effect_summaries(
		const workspace& workspace, analysis_scope scope, flow_options options = {});
} // namespace cxxlens::flow
