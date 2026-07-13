#pragma once

/** @file generate.hpp @brief Issue #49 non-installed generation Contract Candidate. */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../48/transform.hpp"

#include <cxxlens/facts.hpp>
#include <cxxlens/select.hpp>
#include <cxxlens/workspace.hpp>

namespace cxxlens::generate
{
	enum class surface_kind : std::uint16_t
	{
		record,
		method,
		static_method,
		constructor,
		destructor,
		conversion_operator,
		overloaded_operator,
		assignment_operator,
		function_template,
		member_template,
		field,
		static_data,
		nested_type,
		using_exposure,
		c_function,
		typedef_,
		enum_,
		macro_,
		unknown,
	};

	enum class decision_state : std::uint8_t
	{
		requested,
		accepted,
		excluded,
		unsupported,
		ambiguous,
		failed,
		deferred,
	};

	struct surface
	{
		surface_id id;
		surface_kind kind{surface_kind::unknown};
		symbol_id owner;
		std::optional<symbol_id> symbol;
		source_span source;
		std::string canonical_signature;
		provenance origin;
		std::vector<build_variant_id> variants;
	};

	struct surface_decision
	{
		surface_id surface;
		std::string axis;
		decision_state state{decision_state::requested};
		std::string stable_code;
		std::vector<std::string> suggested_actions;
		std::optional<std::string> payload_id;
		std::vector<artifact_id> artifacts;
		evidence why;
	};

	enum class artifact_kind : std::uint16_t
	{
		cpp_header,
		cpp_source,
		cmake_fragment,
		manifest,
		report,
		scenario,
		corpus,
		config,
		custom,
	};

	struct artifact_state
	{
		bool generated{};
		bool present{};
		bool publishable{};
		bool usable{};
		bool link_ready{};
		bool listed{};
		bool quarantined{};
		bool rolled_back{};
	};

	struct artifact_plan
	{
		artifact_id id;
		artifact_kind kind{artifact_kind::custom};
		path relative_path;
		std::string content;
		std::string content_digest;
		std::optional<std::string> expected_existing_digest;
		std::vector<surface_id> satisfies;
		std::vector<std::string> payload_ids;
		std::vector<artifact_id> dependencies;
		std::vector<build_variant_id> verified_variants;
		artifact_state state;
		evidence why;
	};

	class generation_result
	{
	  public:
		[[nodiscard]] transform::transaction_state state() const noexcept;
		[[nodiscard]] bool committed() const noexcept;
		[[nodiscard]] std::string_view transaction_id() const noexcept;
		[[nodiscard]] std::span<const artifact_plan> artifacts() const noexcept;
		[[nodiscard]] std::span<const path> emitted_files() const noexcept;
		[[nodiscard]] std::span<const path> quarantined_files() const noexcept;
		[[nodiscard]] std::span<const diagnostic> diagnostics() const noexcept;
		[[nodiscard]] std::span<const unresolved> unresolved_items() const noexcept;
		[[nodiscard]] const coverage_report& coverage() const noexcept;
		[[nodiscard]] std::string manifest_json() const;
		[[nodiscard]] std::string report_markdown() const;

	  private:
		struct data;
		std::shared_ptr<const data> data_;
	};

	class generation_plan
	{
	  public:
		[[nodiscard]] plan_id id() const noexcept;
		[[nodiscard]] bool valid() const noexcept;
		[[nodiscard]] std::span<const surface> census() const noexcept;
		[[nodiscard]] std::span<const surface_decision> decisions() const noexcept;
		[[nodiscard]] std::span<const artifact_plan> artifacts() const noexcept;
		[[nodiscard]] std::span<const diagnostic> diagnostics() const noexcept;
		[[nodiscard]] std::span<const unresolved> unresolved_items() const noexcept;
		[[nodiscard]] const coverage_report& coverage() const noexcept;
		[[nodiscard]] result<std::string> preview(std::size_t output_budget_bytes = 4 * 1024 * 1024) const;
		[[nodiscard]] std::string to_json() const;
		[[nodiscard]] std::string to_markdown() const;
		[[nodiscard]] result<generation_result> apply(
			workspace& workspace,
			transform::apply_mode mode = transform::apply_mode::dry_run,
			execution_context execution = {}) const;

	  private:
		struct data;
		std::shared_ptr<const data> data_;
	};

	namespace mock
	{
		enum class mock_framework : std::uint8_t
		{
			gmock,
			trompeloeil,
			custom,
		};

		enum class fake_strategy : std::uint8_t
		{
			none,
			subclass_fake,
			link_replacement,
			function_table,
		};

		enum class include_strategy : std::uint8_t
		{
			original_header,
			minimal_public,
			forward_declare_where_safe,
		};

		enum class mock_decision : std::uint8_t
		{
			direct_mock,
			proxy_mock,
			not_applicable,
			unsupported,
			suppressed,
		};

		enum class fake_definition_decision : std::uint8_t
		{
			dispatching_definition,
			definition_not_required,
			definition_only,
			unsupported_call_stub,
			unsupported,
			template_deferred,
		};

		enum class fake_dispatch_decision : std::uint8_t
		{
			dispatch_to_mock,
			dispatch_to_proxy,
			definition_only,
			unsupported_stub,
			not_required,
		};

		enum class link_effect : std::uint8_t
		{
			none,
			symbol_defined,
			symbol_not_required,
			symbol_missing,
			symbol_stubbed,
			requires_build_substitution,
			unknown,
		};

		struct generation_options
		{
			mock_framework framework{mock_framework::gmock};
			fake_strategy fake{fake_strategy::none};
			path output_header;
			path output_source;
			include_strategy includes{include_strategy::minimal_public};
			bool format{true};
			bool verify_reparse{true};
			bool verify_compile{};
			bool verify_link{};
		};

		class generator
		{
		  public:
			[[nodiscard]] static generator for_class(std::string qualified_name);
			[[nodiscard]] static generator for_symbol(select::symbol_selector target);
			[[nodiscard]] static generator for_c_api_header(path header);
			[[nodiscard]] generator options(generation_options value) const;
			[[nodiscard]] generator include_method(select::symbol_selector method) const;
			[[nodiscard]] generator exclude_method(select::symbol_selector method) const;
			[[nodiscard]] result<generation_plan> plan(
				workspace& workspace, execution_context execution = {}) const;

		  private:
			struct data;
			std::shared_ptr<const data> data_;
		};
	} // namespace mock

	namespace method_harness
	{
		enum class ref_qualifier : std::uint8_t
		{
			none,
			lvalue,
			rvalue,
		};

		struct method_spec
		{
			std::string qualified_class_name;
			std::string method_name;
			std::vector<std::string> parameter_canonical_types;
			std::optional<std::string> return_canonical_type;
			bool is_const{};
			bool is_volatile{};
			ref_qualifier refq{ref_qualifier::none};
			std::optional<bool> is_noexcept;
			std::optional<std::string> template_arguments;
		};

		struct method_resolution
		{
			std::optional<symbol_id> selected;
			std::vector<symbol_id> candidates;
			std::vector<diagnostic> diagnostics;
			std::vector<unresolved> unresolved_items;
			evidence why;
			coverage_report coverage;
		};

		enum class extraction_class : std::uint8_t
		{
			kernel_safe,
			live_required,
			unsupported,
			diagnostic_only,
		};

		enum class dependency_kind : std::uint16_t
		{
			field,
			parameter,
			local,
			helper_function,
			virtual_call,
			global_state,
			allocation,
			io,
			synchronization,
			exception_path,
			this_escape,
			lifetime_sensitive,
			byte_operation,
			unknown,
		};

		struct method_feature
		{
			std::string id;
			dependency_kind kind{dependency_kind::unknown};
			std::string detail;
			source_span source;
			extraction_class classification{extraction_class::diagnostic_only};
			std::string reason_code;
		};

		struct method_inspection
		{
			symbol_id method;
			std::vector<method_feature> features;
			std::vector<symbol_id> dependencies;
			std::vector<unresolved> unresolved_items;
			evidence why;
			coverage_report coverage;
		};

		struct harness_options
		{
			path output_directory;
			bool generate_kernel{true};
			bool generate_live_reference{true};
			bool generate_scenario{true};
			bool differential_validation{true};
			bool verify_reparse{true};
		};

		[[nodiscard]] result<method_spec> parse_method_spec(std::string_view text);
		[[nodiscard]] result<method_resolution> resolve_method(
			const workspace& workspace, const method_spec& spec, execution_context execution = {});

		class generator
		{
		  public:
			[[nodiscard]] static generator for_method(method_spec spec);
			[[nodiscard]] static generator for_symbol(select::symbol_selector target);
			[[nodiscard]] generator options(harness_options value) const;
			[[nodiscard]] result<method_inspection> inspect(
				const workspace& workspace, execution_context execution = {}) const;
			[[nodiscard]] result<generation_plan> plan(
				workspace& workspace, execution_context execution = {}) const;

		  private:
			struct data;
			std::shared_ptr<const data> data_;
		};
	} // namespace method_harness

	namespace copy
	{
		enum class copy_policy : std::uint8_t
		{
			declarations_only,
			declarations_and_required_inline,
			public_surface,
			minimal_dependency_closure,
		};

		struct copy_options
		{
			copy_policy policy{copy_policy::minimal_dependency_closure};
			path output;
			bool allow_forward_declarations{true};
			bool reject_odr_risk{true};
			bool reject_macro_derived{true};
			bool verify_reparse{true};
		};

		[[nodiscard]] result<generation_plan> public_surface(
			workspace& workspace,
			select::symbol_selector target,
			copy_options options = {},
			execution_context execution = {});
		[[nodiscard]] result<generation_plan> required_types(
			workspace& workspace,
			std::vector<symbol_id> roots,
			copy_options options = {},
			execution_context execution = {});
	} // namespace copy

	namespace fuzz
	{
		enum class input_kind : std::uint8_t
		{
			bytes,
			string,
			integer,
			floating,
			enum_,
			aggregate,
			pointer_buffer,
			structured_sequence,
			custom,
			unsupported,
		};

		struct input_model
		{
			input_kind kind{input_kind::unsupported};
			std::optional<std::size_t> max_size;
			std::string decoder;
			std::optional<std::size_t> length_parameter;
			bool nullable{};
		};

		struct fuzz_options
		{
			path output_source;
			std::optional<path> seed_corpus_directory;
			std::size_t max_input_size{4096};
			bool add_sanitizer_profile{true};
			bool verify_reparse{true};
			bool verify_compile{};
		};

		class generator
		{
		  public:
			[[nodiscard]] static generator for_function(select::symbol_selector target);
			[[nodiscard]] generator input(std::size_t parameter, input_model model) const;
			[[nodiscard]] generator infer_inputs(bool value = true) const;
			[[nodiscard]] generator options(fuzz_options value) const;
			[[nodiscard]] result<generation_plan> plan(
				workspace& workspace, execution_context execution = {}) const;

		  private:
			struct data;
			std::shared_ptr<const data> data_;
		};
	} // namespace fuzz
} // namespace cxxlens::generate
