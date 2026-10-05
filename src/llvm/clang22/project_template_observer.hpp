#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>
#include <cxxlens/sdk/relation.hpp>

namespace clang
{
	class Sema;
} // namespace clang
namespace cxxlens::detail::clang22
{
	struct project_template_limits
	{
		std::size_t maximum_operations{8'000'000U}, maximum_members{100'000U}, maximum_depth{128U},
			maximum_retained_bytes{64U * 1024U * 1024U};
		std::function<bool()> cancelled;
	};
	/** Compiler source locations stay in the current native job and are detached before AST
	 * release. */
	enum class template_native_source_role
	{
		declaration,
		expression
	};
	struct template_native_source
	{
		std::uint32_t begin{}, end{};
		template_native_source_role role{template_native_source_role::expression};
	};
	struct template_native_subject
	{
		std::string kind, semantic_usr, owner_usr, materialization_kind, argument_state,
			canonical_arguments, normalization_state, target_state, reason, occurrence_kind;
		template_native_source source, definition_source;
		std::optional<std::size_t> primary, owner, constraint_root;
		std::optional<bool> is_system, dependent;
		std::optional<std::uint64_t> closure_size_bits;
		std::vector<std::size_t> captures;
		std::string capture_state;
		bool complete{true};
	};
	struct template_native_constraint
	{
		std::size_t root_subject{};
		std::string path, kind, mapping_state, parameter_mapping, reason;
		std::uint64_t depth{};
		template_native_source source;
		std::vector<std::size_t> children;
		bool complete{true};
	};
	struct template_native_capture
	{
		std::size_t lambda{}, index{};
		std::string kind, captured_usr, reason;
		template_native_source source;
		std::optional<std::uint64_t> field_index, offset_bits, size_bits;
		bool complete{true};
	};
	struct template_native_frame
	{
		std::size_t ordinal{};
		std::optional<std::size_t> parent;
		std::string kind, entity_usr, template_usr, argument_state, canonical_arguments, reason;
		template_native_source source;
		std::uint64_t depth{};
		bool is_instantiation{}, ended{}, complete{true};
	};
	/** Original values only: no AST/Sema/Decl pointers survive callback detachment. */
	struct project_template_observations
	{
		std::vector<template_native_subject> subjects;
		std::vector<template_native_constraint> constraints;
		std::vector<template_native_capture> captures;
		std::vector<template_native_frame> frames;
		std::vector<std::size_t> active_frames;
		std::size_t operations{}, retained_bytes{};
		bool trace_installed{}, trace_initialised{}, trace_frozen{}, trace_partial{},
			subjects_partial{}, constraints_partial{}, captures_partial{};
	};
	/** Install before parsing; original begin/end contexts, not post-AST stack reconstruction. */
	void install_project_template_observer(clang::Sema&,
										   project_template_observations&,
										   project_template_limits = {});
	/** Freeze original parser trace, then detach actual AST/layout and compiler normalized
	 * constraints. */
	[[nodiscard]] sdk::result<void> observe_project_templates(clang::Sema&,
															  project_template_observations&,
															  project_template_limits = {});
	struct project_template_bindings
	{
		std::string compile_unit;
		std::function<std::string(std::string_view)> entity;
		std::function<std::string(template_native_source)> source;
	};
	/** Existing compiler/source bindings are supplied by the ordinary extractor. */
	[[nodiscard]] sdk::result<std::vector<sdk::detached_row>>
	detach_project_templates(const project_template_observations&,
							 const project_template_bindings&,
							 project_template_limits = {});
} // namespace cxxlens::detail::clang22
