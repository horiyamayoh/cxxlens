#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <ranges>
#include <string>
#include <vector>

#include <cxxlens/provider/clang22.hpp>
#include <cxxlens/relations/build_compile_unit.hpp>
#include <cxxlens/relations/build_project.hpp>
#include <cxxlens/relations/build_toolchain_context.hpp>
#include <cxxlens/relations/build_variant.hpp>
#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_entity.hpp>
#include <cxxlens/relations/source_file.hpp>
#include <cxxlens/relations/source_span.hpp>

#include "llvm/clang22/provider_worker.hpp"

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::detail::clang22;

	void require(const bool condition, const std::string& message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] sdk::detached_cell
	symbol_cell(const sdk::scalar_kind kind, std::string parameter, std::string value)
	{
		return {{kind, std::move(parameter), false},
				sdk::cell_state::present,
				sdk::scalar_value{std::move(value)},
				std::nullopt};
	}

	[[nodiscard]] sdk::detached_cell optional_typed(std::string parameter, std::string value)
	{
		return {{sdk::scalar_kind::typed_id, std::move(parameter), true},
				sdk::cell_state::present,
				sdk::scalar_value{std::move(value)},
				std::nullopt};
	}

	[[nodiscard]] sdk::detached_cell optional_bytes(std::vector<std::byte> value)
	{
		return {{sdk::scalar_kind::bytes, {}, true},
				sdk::cell_state::present,
				sdk::scalar_value{std::move(value)},
				std::nullopt};
	}

	[[nodiscard]] sdk::detached_cell optional_utf8(std::string value)
	{
		return {{sdk::scalar_kind::utf8_string, {}, true},
				sdk::cell_state::present,
				sdk::scalar_value{std::move(value)},
				std::nullopt};
	}

	void set_primary_span(detached_observation& value,
						  std::string snapshot,
						  std::string file,
						  const std::uint64_t begin,
						  const std::uint64_t end,
						  std::string role,
						  const bool read_only = false)
	{
		auto identity = sdk::source_span_identity(snapshot, file, begin, end, role);
		require(identity.has_value(), "test source span identity failed");
		value.primary_span = materialization::observation_v2_primary_span{
			std::move(*identity),
			std::move(snapshot),
			std::move(file),
			begin,
			end,
			std::move(role),
			read_only,
		};
	}

	void bind_source_authority(observation_batch& value,
							   std::string snapshot = "source-snapshot:one",
							   std::string file = "file:stable",
							   const std::uint64_t source_size = 4096U)
	{
		value.materialization_authority = materialization::observation_v2_task_authority{
			value.unit, std::move(snapshot), std::move(file), source_size};
	}

	[[nodiscard]] detached_observation observation(const observation_kind kind,
												   std::string semantic_key)
	{
		detached_observation value;
		value.kind = kind;
		value.compile_unit = "cu-" + std::string(64U, 'a');
		value.semantic_key = std::move(semantic_key);
		if (kind != observation_kind::type)
			set_primary_span(value,
							 "source-snapshot:one",
							 "file:stable",
							 10U,
							 18U,
							 kind == observation_kind::entity ? "declaration" : "expression");
		return value;
	}

	[[nodiscard]] observation_batch batch()
	{
		observation_batch value;
		value.unit = "cu-" + std::string(64U, 'a');
		value.variant = "variant-" + std::string(64U, 'b');
		bind_source_authority(value);

		auto entity = observation(observation_kind::entity, "clang-usr:target");
		set_primary_span(entity, "source-snapshot:one", "file:stable", 10U, 18U, "declaration");
		entity.payload.emplace("symbol.kind", "function");
		entity.payload.emplace("symbol.qualified_name", "ns::target");
		entity.payload.emplace("symbol.signature", "int ()");

		auto type = observation(observation_kind::type, "type:int");
		type.payload.emplace("type.canonical", "int");

		auto call = observation(observation_kind::call, "call:main:12");
		set_primary_span(call, "source-snapshot:one", "file:stable", 10U, 18U, "expression");
		call.payload.emplace("call.kind", "direct_function");
		call.payload.emplace("call.direct_callee", "clang-usr:target");
		call.payload.emplace("call.direct_callee_kind", "function");
		call.payload.emplace("call.direct_callee_signature", "int ()");
		call.payload.emplace("call.direct_callee_qualified_name", "ns::target");
		value.observations = {std::move(entity), std::move(type), std::move(call)};
		require(value.validate().has_value(), "normalizer fixture batch is invalid");
		return value;
	}

	[[nodiscard]] clang22_task_input task_input()
	{
		const std::string source{"int target(); int main(){ return target(); }"};
		const std::vector<std::string> arguments{"clang++", "-std=c++23"};
		auto invocation = sdk::canonical_binary(sdk::canonical_value::from_tuple({
			sdk::canonical_value::from_string("cxxlens.clang22.effective-invocation.v1"),
			sdk::canonical_value::from_string("project://workspace"),
			sdk::canonical_value::from_tuple({sdk::canonical_value::from_string("clang++"),
											  sdk::canonical_value::from_string("-std=c++23")}),
		}));
		require(invocation.has_value(), "task invocation projection failed");
		const std::string invocation_bytes{reinterpret_cast<const char*>(invocation->data()),
										   invocation->size()};
		auto invocation_digest =
			sdk::semantic_digest("cxxlens.clang22.effective-invocation.v1", invocation_bytes);
		require(invocation_digest.has_value(), "task invocation digest failed");
		const auto source_digest = sdk::content_digest(std::as_bytes(std::span{source}));
		const std::string environment_digest =
			"sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
		auto catalog = sdk::project_catalog::make(
			"project://workspace",
			environment_digest,
			{{"catalog-unit:alpha", *invocation_digest, source_digest, environment_digest},
			 {"catalog-unit:beta",
			  "sha256:abababababababababababababababababababababababababababababababab",
			  "sha256:bcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbc",
			  environment_digest}});
		require(catalog.has_value(), "task project catalog failed");
		clang22_task_input output;
		output.project_catalog = std::move(*catalog);
		output.selected_catalog_compile_unit = "catalog-unit:alpha";
		output.toolchain_digest =
			"semantic-v2:sha256:"
			"7f3c5aa5ac281a5c00cf543ae51a36941a043f79b08988d4b928dcd9befb8c3c";
		output.toolchain = {
			"toolchain-family:clang",
			"toolchain-version:22.0.0",
			"target:x86_64-linux-gnu",
			"sha256:2222222222222222222222222222222222222222222222222222222222222222",
			std::nullopt,
			"sha256:3333333333333333333333333333333333333333333333333333333333333333",
			"sha256:4444444444444444444444444444444444444444444444444444444444444444",
		};
		output.variant_authority = {
			"language:cxx",
			"standard:cxx23",
			"target:x86_64-linux-gnu",
			"sha256:5555555555555555555555555555555555555555555555555555555555555555",
			"sha256:6666666666666666666666666666666666666666666666666666666666666666",
			"sha256:7777777777777777777777777777777777777777777777777777777777777777",
		};
		output.normalized_invocation_digest = std::move(*invocation_digest);
		output.environment_digest = environment_digest;
		output.language = "language:cxx";
		output.working_directory = "project://workspace";
		output.condition_universe = "condition-universe:alpha";
		output.condition = "condition:alpha";
		output.interpretation = "cc.clang22-canonical-1";
		output.logical_path = "project://input.cpp";
		output.source_content_digest = source_digest;
		output.source_content_base64 =
			"aW50IHRhcmdldCgpOyBpbnQgbWFpbigpeyByZXR1cm4gdGFyZ2V0KCk7IH0=";
		output.source_size_bytes = source.size();
		output.source_encoding = "utf8";
		output.source_read_only = true;
		output.source = source;
		output.arguments = arguments;
		output.requested_descriptors = {
			"cc.call_direct_target.v1",
			"cc.call_site.v1",
			"cc.entity.v1",
			"frontend.clang22.call_observation.v2",
			"frontend.clang22.entity_observation.v2",
			"frontend.clang22.type_observation.v2",
		};
		output.dependency_groups = {"canonical", "observation"};
		output.sandbox = {
			"enforced",
			"sha256:8888888888888888888888888888888888888888888888888888888888888888",
		};

		const auto derive = [](const sdk::relation_descriptor& descriptor,
							   sdk::result<sdk::detached_row> row,
							   const std::string& message)
		{
			require(row.has_value(), message + " row failed");
			auto identity = sdk::derive_domain_identity(descriptor, *row);
			require(identity.has_value(), message + " identity failed");
			return *identity;
		};
		const std::array file_projection{
			sdk::canonical_value::from_string("project"),
			sdk::canonical_value::from_string("input.cpp"),
			sdk::canonical_value::from_string("cxxlens.logical-path.v1"),
		};
		auto file = sdk::canonical_identity_digest("file", file_projection);
		require(file.has_value(), "task file identity failed");
		output.file = std::move(*file);
		const std::array line_index_projection{
			sdk::canonical_value::from_string("cxxlens.byte-line-index.v1"),
			sdk::canonical_value::from_string(output.source_content_digest),
			sdk::canonical_value::from_integer(static_cast<std::int64_t>(source.size())),
			sdk::canonical_value::from_tuple({sdk::canonical_value::from_integer(0)}),
		};
		auto line_index = sdk::canonical_identity_digest("line-index", line_index_projection);
		require(line_index.has_value(), "task line-index identity failed");
		output.line_index = std::move(*line_index);

		using project_relation = cxxlens::build::relations::project;
		project_relation::builder project_builder;
		require(project_builder
					.set<project_relation::project_column>(
						sdk::detached_cell::typed("project_id", "pending"))
					.has_value(),
				"task project result binding failed");
		require(project_builder
					.set<project_relation::catalog>(
						sdk::detached_cell::typed("catalog_id", output.project_catalog.catalog_id))
					.has_value(),
				"task project catalog binding failed");
		require(project_builder
					.set<project_relation::catalog_digest>(symbol_cell(
						sdk::scalar_kind::digest, {}, output.project_catalog.catalog_digest))
					.has_value(),
				"task project catalog digest binding failed");
		require(project_builder
					.set<project_relation::logical_root>(sdk::detached_cell::typed(
						"logical_path_id", output.project_catalog.logical_root))
					.has_value(),
				"task project logical-root binding failed");
		require(project_builder
					.set<project_relation::environment_digest>(symbol_cell(
						sdk::scalar_kind::digest, {}, output.project_catalog.environment_digest))
					.has_value(),
				"task project environment binding failed");
		output.project = derive(
			project_relation::descriptor(), std::move(project_builder).finish(), "task project");

		using toolchain_relation = cxxlens::build::relations::toolchain_context;
		toolchain_relation::builder toolchain_builder;
		require(toolchain_builder
					.set<toolchain_relation::toolchain>(
						sdk::detached_cell::typed("toolchain_context_id", "pending"))
					.has_value(),
				"task toolchain result binding failed");
		require(toolchain_builder
					.set<toolchain_relation::family>(symbol_cell(sdk::scalar_kind::open_symbol,
																 "build.toolchain-family/1",
																 output.toolchain.family))
					.has_value(),
				"task toolchain family binding failed");
		require(toolchain_builder
					.set<toolchain_relation::exact_version>(
						sdk::detached_cell::utf8(output.toolchain.exact_version))
					.has_value(),
				"task toolchain version binding failed");
		require(toolchain_builder
					.set<toolchain_relation::target_triple>(
						sdk::detached_cell::utf8(output.toolchain.target_triple))
					.has_value(),
				"task toolchain target binding failed");
		require(toolchain_builder
					.set<toolchain_relation::builtin_headers_digest>(symbol_cell(
						sdk::scalar_kind::digest, {}, output.toolchain.builtin_headers_digest))
					.has_value(),
				"task toolchain builtin binding failed");
		require(toolchain_builder
					.set<toolchain_relation::abi_digest>(
						symbol_cell(sdk::scalar_kind::digest, {}, output.toolchain.abi_digest))
					.has_value(),
				"task toolchain ABI binding failed");
		require(toolchain_builder
					.set<toolchain_relation::plugin_spec_digest>(symbol_cell(
						sdk::scalar_kind::digest, {}, output.toolchain.plugin_spec_digest))
					.has_value(),
				"task toolchain plugin binding failed");
		output.toolchain_context = derive(toolchain_relation::descriptor(),
										  std::move(toolchain_builder).finish(),
										  "task toolchain");

		using variant_relation = cxxlens::build::relations::variant;
		variant_relation::builder variant_builder;
		require(variant_builder
					.set<variant_relation::variant_column>(
						sdk::detached_cell::typed("build_variant_id", "pending"))
					.has_value(),
				"task variant result binding failed");
		require(variant_builder
					.set<variant_relation::project>(
						sdk::detached_cell::typed("project_id", output.project))
					.has_value(),
				"task variant project binding failed");
		require(variant_builder
					.set<variant_relation::toolchain>(
						sdk::detached_cell::typed("toolchain_context_id", output.toolchain_context))
					.has_value(),
				"task variant toolchain binding failed");
		require(variant_builder
					.set<variant_relation::language>(symbol_cell(sdk::scalar_kind::open_symbol,
																 "build.language/1",
																 output.variant_authority.language))
					.has_value(),
				"task variant language binding failed");
		require(variant_builder
					.set<variant_relation::language_standard>(
						symbol_cell(sdk::scalar_kind::open_symbol,
									"build.language-standard/1",
									output.variant_authority.language_standard))
					.has_value(),
				"task variant language-standard binding failed");
		require(variant_builder
					.set<variant_relation::target_triple>(
						sdk::detached_cell::utf8(output.variant_authority.target_triple))
					.has_value(),
				"task variant target binding failed");
		require(variant_builder
					.set<variant_relation::predefined_macros_digest>(
						symbol_cell(sdk::scalar_kind::digest,
									{},
									output.variant_authority.predefined_macros_digest))
					.has_value(),
				"task variant predefined-macros binding failed");
		require(
			variant_builder
				.set<variant_relation::include_search_digest>(symbol_cell(
					sdk::scalar_kind::digest, {}, output.variant_authority.include_search_digest))
				.has_value(),
			"task variant include-search binding failed");
		require(
			variant_builder
				.set<variant_relation::semantic_flags_digest>(symbol_cell(
					sdk::scalar_kind::digest, {}, output.variant_authority.semantic_flags_digest))
				.has_value(),
			"task variant semantic-flags binding failed");
		output.variant = derive(
			variant_relation::descriptor(), std::move(variant_builder).finish(), "task variant");

		using source_relation = cxxlens::source::relations::file;
		source_relation::builder source_builder;
		require(source_builder
					.set<source_relation::snapshot>(
						sdk::detached_cell::typed("source_snapshot_id", "pending"))
					.has_value(),
				"task source result binding failed");
		require(source_builder
					.set<source_relation::file_column>(
						sdk::detached_cell::typed("file_id", output.file))
					.has_value(),
				"task source file binding failed");
		require(source_builder
					.set<source_relation::project>(
						sdk::detached_cell::typed("project_id", output.project))
					.has_value(),
				"task source project binding failed");
		require(source_builder
					.set<source_relation::logical_path>(
						sdk::detached_cell::typed("logical_path_id", output.logical_path))
					.has_value(),
				"task source logical-path binding failed");
		require(source_builder
					.set<source_relation::content>(
						symbol_cell(sdk::scalar_kind::digest, {}, output.source_content_digest))
					.has_value(),
				"task source content binding failed");
		require(source_builder
					.set<source_relation::size>(
						sdk::detached_cell::unsigned_integer(output.source_size_bytes))
					.has_value(),
				"task source size binding failed");
		require(source_builder
					.set<source_relation::encoding>(symbol_cell(
						sdk::scalar_kind::open_symbol, "source.encoding/1", output.source_encoding))
					.has_value(),
				"task source encoding binding failed");
		require(source_builder
					.set<source_relation::line_index>(
						sdk::detached_cell::typed("line_index_id", output.line_index))
					.has_value(),
				"task source line-index binding failed");
		require(source_builder
					.set<source_relation::read_only>(
						sdk::detached_cell::boolean(output.source_read_only))
					.has_value(),
				"task source read-only binding failed");
		output.source_snapshot = derive(
			source_relation::descriptor(), std::move(source_builder).finish(), "task source");

		using compile_unit_relation = cxxlens::build::relations::compile_unit;
		compile_unit_relation::builder compile_unit_builder;
		require(compile_unit_builder
					.set<compile_unit_relation::compile_unit_column>(
						sdk::detached_cell::typed("compile_unit_id", "pending"))
					.has_value(),
				"task compile-unit result binding failed");
		require(compile_unit_builder
					.set<compile_unit_relation::project>(
						sdk::detached_cell::typed("project_id", output.project))
					.has_value(),
				"task compile-unit project binding failed");
		require(compile_unit_builder
					.set<compile_unit_relation::main_source>(
						sdk::detached_cell::typed("source_snapshot_id", output.source_snapshot))
					.has_value(),
				"task compile-unit source binding failed");
		require(compile_unit_builder
					.set<compile_unit_relation::variant>(
						sdk::detached_cell::typed("build_variant_id", output.variant))
					.has_value(),
				"task compile-unit variant binding failed");
		require(compile_unit_builder
					.set<compile_unit_relation::toolchain>(
						sdk::detached_cell::typed("toolchain_context_id", output.toolchain_context))
					.has_value(),
				"task compile-unit toolchain binding failed");
		require(compile_unit_builder
					.set<compile_unit_relation::effective_invocation_digest>(symbol_cell(
						sdk::scalar_kind::digest, {}, output.normalized_invocation_digest))
					.has_value(),
				"task compile-unit invocation binding failed");
		require(compile_unit_builder
					.set<compile_unit_relation::language>(symbol_cell(
						sdk::scalar_kind::open_symbol, "build.language/1", output.language))
					.has_value(),
				"task compile-unit language binding failed");
		require(compile_unit_builder
					.set<compile_unit_relation::working_directory>(
						sdk::detached_cell::typed("logical_path_id", output.working_directory))
					.has_value(),
				"task compile-unit working-directory binding failed");
		output.compile_unit = derive(compile_unit_relation::descriptor(),
									 std::move(compile_unit_builder).finish(),
									 "task compile unit");
		return output;
	}

	[[nodiscard]] std::vector<sdk::relation_descriptor> exact_task_descriptors()
	{
		return {
			cxxlens::cc::relations::call_direct_target::descriptor(),
			cxxlens::cc::relations::call_site::descriptor(),
			cxxlens::cc::relations::entity::descriptor(),
			call_observation_descriptor(),
			entity_observation_descriptor(),
			type_observation_descriptor(),
		};
	}

	void output_plan_contract()
	{
		const std::vector<provider_output_binding> expected{
			{provider_output_slot::call_direct_target, "cc.call_direct_target.v1", "canonical"},
			{provider_output_slot::call_site, "cc.call_site.v1", "canonical"},
			{provider_output_slot::entity, "cc.entity.v1", "canonical"},
			{provider_output_slot::call_observation,
			 "frontend.clang22.call_observation.v2",
			 "observation"},
			{provider_output_slot::entity_observation,
			 "frontend.clang22.entity_observation.v2",
			 "observation"},
			{provider_output_slot::type_observation,
			 "frontend.clang22.type_observation.v2",
			 "observation"},
		};
		auto plan = provider_output_plan();
		require(plan == expected && validate_provider_output_plan(plan).has_value(),
				"worker output plan differs from canonical-then-observation authority");
		std::vector<std::string> groups;
		for (const auto& binding : plan)
			if (groups.empty() || groups.back() != binding.dependency_group)
				groups.push_back(binding.dependency_group);
		require(groups == std::vector<std::string>{"canonical", "observation"},
				"worker dependency groups differ from exact contract order");
		auto descriptors = exact_task_descriptors();
		require(descriptors.size() == plan.size(), "task/output descriptor count differs");
		for (std::size_t index{}; index < plan.size(); ++index)
			require(descriptors[index].id == plan[index].descriptor_id,
					"task descriptor order differs from worker output order");

		auto missing = plan;
		missing.pop_back();
		require(!validate_provider_output_plan(missing), "missing output slot was accepted");

		auto duplicate = plan;
		duplicate[1U] = duplicate[0U];
		require(!validate_provider_output_plan(duplicate), "duplicate output slot was accepted");

		auto extra = plan;
		extra.push_back(plan.front());
		require(!validate_provider_output_plan(extra), "extra output slot was accepted");

		auto reordered = plan;
		std::swap(reordered[0U], reordered[1U]);
		require(!validate_provider_output_plan(reordered), "reordered output slots were accepted");

		auto regrouped = plan;
		regrouped[3U].dependency_group = "canonical";
		require(!validate_provider_output_plan(regrouped),
				"dependency-group boundary drift was accepted");
	}

	[[nodiscard]] detached_observation redeclaration(std::string semantic_key,
													 std::string signature,
													 const std::uint64_t begin,
													 const bool definition,
													 const bool canonical)
	{
		auto value = observation(observation_kind::entity, std::move(semantic_key));
		set_primary_span(
			value, "source-snapshot:one", "file:stable", begin, begin + 8U, "declaration");
		value.payload.emplace("symbol.kind", "function");
		value.payload.emplace("symbol.qualified_name", "ns::redeclared");
		value.payload.emplace("symbol.signature", std::move(signature));
		value.payload.emplace("symbol.is_definition", definition ? "true" : "false");
		value.payload.emplace("symbol.is_canonical_declaration", canonical ? "true" : "false");
		return value;
	}

	[[nodiscard]] std::string string_cell(const sdk::detached_row& row, const std::string& column)
	{
		const auto found = row.cells.find(column);
		require(found != row.cells.end() && found->second.value.has_value(),
				"expected target identity cell");
		const auto* value = std::get_if<std::string>(&*found->second.value);
		require(value != nullptr, "expected string target identity");
		return *value;
	}

	[[nodiscard]] bool boolean_cell(const sdk::detached_row& row, const std::string& column)
	{
		const auto found = row.cells.find(column);
		require(found != row.cells.end() && found->second.value.has_value(),
				"expected boolean identity cell");
		const auto* value = std::get_if<bool>(&*found->second.value);
		require(value != nullptr, "expected boolean identity value");
		return *value;
	}

	[[nodiscard]] std::vector<std::byte> bytes_cell(const sdk::detached_row& row,
													const std::string& column)
	{
		const auto found = row.cells.find(column);
		require(found != row.cells.end() && found->second.value.has_value(),
				"expected bytes identity cell");
		const auto* value = std::get_if<std::vector<std::byte>>(&*found->second.value);
		require(value != nullptr, "expected bytes identity value");
		return *value;
	}

	[[nodiscard]] std::string canonical_batch(const canonicalized_provider_batch& batch)
	{
		std::string output;
		for (const auto* rows : {
				 &batch.direct_targets,
				 &batch.call_sites,
				 &batch.entities,
				 &batch.call_observations,
				 &batch.entity_observations,
				 &batch.type_observations,
			 })
			for (const auto& row : *rows)
				output += row.canonical_form() + '\n';
		return output;
	}

	[[nodiscard]] declaration_identity_input fallback_identity(std::string kind,
															   std::string signature,
															   std::string anchor,
															   std::string template_value = {},
															   std::string constraint = {})
	{
		return {std::nullopt,
				"sha256:1111111111111111111111111111111111111111111111111111111111111111",
				std::move(kind),
				"ns::f",
				std::move(signature),
				std::move(template_value),
				std::move(constraint),
				"Namespace:ns:sha256:context",
				std::move(anchor)};
	}
} // namespace

int main()
{
	using namespace cxxlens::detail::clang22;
	using cxxlens::provider::clang22::detached_source_span;
	using cxxlens::provider::clang22::source_range_identity;
	output_plan_contract();

	const source_range_identity source_identity{"source-snapshot:one", "file:stable", "expression"};
	auto span = sdk::source_span_identity(
		source_identity.source_snapshot, source_identity.file, 10U, 18U, source_identity.role);
	auto changed_snapshot = sdk::source_span_identity(
		"source-snapshot:two", source_identity.file, 10U, 18U, source_identity.role);
	auto changed_file = sdk::source_span_identity(
		source_identity.source_snapshot, "file:other", 10U, 18U, source_identity.role);
	auto changed_role = sdk::source_span_identity(
		source_identity.source_snapshot, source_identity.file, 10U, 18U, "declaration");
	require(
		span && changed_snapshot && changed_file && changed_role && *span != *changed_snapshot &&
			*span != *changed_file && *span != *changed_role &&
			!sdk::source_span_identity({}, source_identity.file, 10U, 18U, source_identity.role),
		"source.span identity is not bound to snapshot/file/range role");
	detached_source_span original{source_identity.source_snapshot,
								  source_identity.file,
								  source_identity.role,
								  "src/a.cpp",
								  10U,
								  18U,
								  false,
								  *span,
								  {}};
	original.origin_chain = {
		{"macro-spelling", "/checkout/include/macros.hpp", 40U, 48U, true},
		{"macro-spelling", "include/wrapper.hpp", 12U, 20U, true},
	};
	auto relocated = original;
	relocated.logical_path = "relocated/src/a.cpp";
	relocated.origin_chain.front().logical_path = "/other-root/include/macros.hpp";
	require(original.validate().has_value() && relocated.validate().has_value() &&
				original.id == relocated.id,
			"logical root relocation changed source.span identity");
	auto editable_origin = original;
	editable_origin.origin_chain.front().read_only = false;
	require(!editable_origin.validate(), "macro spelling origin was not forced read-only");
	using source_span = cxxlens::source::relations::span;
	source_span::builder source_builder;
	for (auto result : {
			 source_builder.set<source_span::span_column>(
				 sdk::detached_cell::typed("source_span_id", *span)),
			 source_builder.set<source_span::snapshot>(
				 sdk::detached_cell::typed("source_snapshot_id", source_identity.source_snapshot)),
			 source_builder.set<source_span::file>(
				 sdk::detached_cell::typed("file_id", source_identity.file)),
			 source_builder.set<source_span::begin>(sdk::detached_cell::unsigned_integer(10U)),
			 source_builder.set<source_span::end>(sdk::detached_cell::unsigned_integer(18U)),
			 source_builder.set<source_span::role>(
				 {{sdk::scalar_kind::open_symbol, "source.range-role/1", false},
				  sdk::cell_state::present,
				  sdk::scalar_value{source_identity.role},
				  std::nullopt}),
			 source_builder.set<source_span::read_only>(sdk::detached_cell::boolean(false)),
		 })
		require(result.has_value(), "source.span authority row rejected shared identity");
	auto source_row = std::move(source_builder).finish();
	require(source_row.has_value(), "source.span builder rejected the shared identity row");
	auto derived_source = sdk::derive_domain_identity(source_span::descriptor(), *source_row);
	require(derived_source && *derived_source == *span &&
				sdk::validate_domain_identity(source_span::descriptor(), *source_row).has_value(),
			"source.span builder and descriptor identity diverged");
	auto invalid_source_row = *source_row;
	invalid_source_row.cells.at("source.span.v1.span") =
		sdk::detached_cell::typed("source_span_id", "source-span:wrong");
	require(!sdk::validate_domain_identity(source_span::descriptor(), invalid_source_row),
			"domain identity validator accepted a mismatched result ID");

	require(entity_observation_descriptor().validate().has_value(),
			"entity observation descriptor is invalid");
	require(type_observation_descriptor().validate().has_value(),
			"type observation descriptor is invalid");
	require(call_observation_descriptor().validate().has_value(),
			"call observation descriptor is invalid");

	auto usr = fallback_identity("function", "void (int)", "source:a");
	usr.usr = "c:@N@ns@F@f#I#";
	auto usr_identity = make_declaration_identity(usr);
	auto integer_overload =
		make_declaration_identity(fallback_identity("function", "void (int)", "source:overload"));
	auto double_overload = make_declaration_identity(
		fallback_identity("function", "void (double)", "source:overload"));
	auto same_redeclaration =
		make_declaration_identity(fallback_identity("function", "void (int)", "source:overload"));
	auto definition_preference_identity =
		make_declaration_identity(fallback_identity("function", "void (int)", "source:overload"));
	auto constructor = make_declaration_identity(
		fallback_identity("constructor", "void (int)", "source:constructor"));
	auto destructor = make_declaration_identity(
		fallback_identity("destructor", "void () noexcept", "source:destructor"));
	auto overloaded_operator = make_declaration_identity(
		fallback_identity("method", "bool (const ns::value &)", "source:operator"));
	auto conversion = make_declaration_identity(
		fallback_identity("method", "operator bool () const", "source:conversion"));
	auto template_primary = make_declaration_identity(
		fallback_identity("function", "void (T)", "source:template", "primary<T>"));
	auto template_int = make_declaration_identity(
		fallback_identity("function", "void (int)", "source:template", "specialization<int>"));
	auto template_double = make_declaration_identity(fallback_identity(
		"function", "void (double)", "source:template", "specialization<double>"));
	auto constrained_integral = make_declaration_identity(fallback_identity(
		"function", "void (T)", "source:constraint", "primary<T>", "integral<T>"));
	auto constrained_floating = make_declaration_identity(fallback_identity(
		"function", "void (T)", "source:constraint", "primary<T>", "floating_point<T>"));
	auto other_toolchain_input = fallback_identity("function", "void (int)", "source:overload");
	other_toolchain_input.toolchain_digest =
		"sha256:2222222222222222222222222222222222222222222222222222222222222222";
	auto other_toolchain = make_declaration_identity(other_toolchain_input);
	auto unanchored = fallback_identity("function", "void (int)", {});
	require(usr_identity && usr_identity->semantic_key == "clang-usr:c:@N@ns@F@f#I#" &&
				usr_identity->confidence == "exact-usr" && integer_overload && double_overload &&
				integer_overload->semantic_key != double_overload->semantic_key &&
				same_redeclaration && *same_redeclaration == *integer_overload && constructor &&
				definition_preference_identity &&
				*definition_preference_identity == *integer_overload && destructor &&
				overloaded_operator && conversion &&
				constructor->semantic_key != destructor->semantic_key &&
				overloaded_operator->semantic_key != conversion->semantic_key && template_primary &&
				template_int && template_double &&
				template_primary->semantic_key != template_int->semantic_key &&
				template_int->semantic_key != template_double->semantic_key &&
				constrained_integral && constrained_floating &&
				constrained_integral->semantic_key != constrained_floating->semantic_key &&
				other_toolchain &&
				other_toolchain->semantic_key != integer_overload->semantic_key &&
				!make_declaration_identity(unanchored),
			"USR failure identity projection collapsed declarations or fabricated an opaque key");

	observation_batch fallback_batch;
	fallback_batch.unit = "cu-" + std::string(64U, 'f');
	fallback_batch.variant = "variant-" + std::string(64U, 'e');
	bind_source_authority(fallback_batch, "source-snapshot:fallback", "file:fallback");
	auto fallback_entity_int =
		redeclaration(integer_overload->semantic_key, "void (int)", 10U, true, true);
	auto fallback_entity_double =
		redeclaration(double_overload->semantic_key, "void (double)", 30U, true, true);
	fallback_entity_int.compile_unit = fallback_batch.unit;
	fallback_entity_double.compile_unit = fallback_batch.unit;
	set_primary_span(
		fallback_entity_int, "source-snapshot:fallback", "file:fallback", 10U, 18U, "declaration");
	set_primary_span(fallback_entity_double,
					 "source-snapshot:fallback",
					 "file:fallback",
					 30U,
					 38U,
					 "declaration");
	fallback_entity_int.payload.emplace("symbol.identity_confidence", "structural-fallback");
	fallback_entity_double.payload.emplace("symbol.identity_confidence", "structural-fallback");
	const auto fallback_call = [&](const declaration_identity& identity,
								   const std::string_view signature,
								   const std::uint64_t begin)
	{
		auto value = observation(observation_kind::call, "call:fallback:" + std::to_string(begin));
		value.compile_unit = fallback_batch.unit;
		set_primary_span(
			value, "source-snapshot:fallback", "file:fallback", begin, begin + 4U, "expression");
		value.payload.emplace("call.kind", "direct_function");
		value.payload.emplace("call.direct_callee", identity.semantic_key);
		value.payload.emplace("call.direct_callee_kind", "function");
		value.payload.emplace("call.direct_callee_signature", signature);
		value.payload.emplace("call.direct_callee_qualified_name", "ns::f");
		value.payload.emplace("call.direct_callee_identity_confidence", identity.confidence);
		return value;
	};
	fallback_batch.observations = {
		std::move(fallback_entity_int),
		std::move(fallback_entity_double),
		fallback_call(*integer_overload, "void (int)", 50U),
		fallback_call(*double_overload, "void (double)", 70U),
	};
	auto fallback_result = canonicalize_provider_batch(
		fallback_batch,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	auto reversed_fallback = fallback_batch;
	std::ranges::reverse(reversed_fallback.observations);
	auto reversed_fallback_result = canonicalize_provider_batch(
		reversed_fallback,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(fallback_result && reversed_fallback_result && !fallback_result->exact_equivalence &&
				fallback_result->entities.size() == 2U &&
				fallback_result->direct_targets.size() == 2U &&
				fallback_result->unresolved.empty() &&
				string_cell(fallback_result->direct_targets.front(),
							"cc.call_direct_target.v1.target") !=
					string_cell(fallback_result->direct_targets.back(),
								"cc.call_direct_target.v1.target") &&
				fallback_result->direct_targets.front().canonical_form() ==
					reversed_fallback_result->direct_targets.front().canonical_form() &&
				fallback_result->direct_targets.back().canonical_form() ==
					reversed_fallback_result->direct_targets.back().canonical_form(),
			"fallback overloads lost entity/target identity or became order-dependent");
	auto opaque_batch = fallback_batch;
	opaque_batch.observations = {observation(observation_kind::call, "call:opaque")};
	opaque_batch.observations.front().compile_unit = opaque_batch.unit;
	set_primary_span(opaque_batch.observations.front(),
					 "source-snapshot:fallback",
					 "file:fallback",
					 90U,
					 94U,
					 "expression");
	opaque_batch.observations.front().payload.emplace("call.kind", "direct_function");
	opaque_batch.observations.front().payload.emplace("call.unresolved_reason",
													  "callee-identity-unavailable");
	auto opaque_result = canonicalize_provider_batch(
		opaque_batch,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(opaque_result && !opaque_result->exact_equivalence &&
				opaque_result->direct_targets.empty() && !opaque_result->unresolved.empty() &&
				opaque_result->unresolved.front().code == "provider.call-kind-target-inconsistent",
			"unanchored fallback fabricated or silently omitted a direct target");

	auto task = task_input();
	auto encoded = encode_task_input(task);
	require(encoded.has_value(), "task input encoding failed");
	const auto task_input_digest = sdk::content_digest(*encoded);
	require(task_input_digest ==
				"sha256:1a54c7f79747653e0395330797ff72823892443faa74417085eaa16d23111395",
			"task.v3 bytes differ from the materialization checker oracle: " + task_input_digest);
	auto decoded = decode_task_input(*encoded);
	require(decoded && decoded->source_snapshot == task.source_snapshot &&
				decoded->file == task.file && decoded->source == task.source &&
				decoded->arguments == task.arguments &&
				decoded->project_catalog.catalog_id == task.project_catalog.catalog_id &&
				decoded->selected_catalog_compile_unit == task.selected_catalog_compile_unit &&
				decoded->compile_unit == task.compile_unit,
			"task input did not round trip");
	auto condition_ref = provider_condition_ref_id(task);
	require(condition_ref &&
				*condition_ref ==
					"condition-ref:semantic-v2:sha256:"
					"40b414b417b0bd33e233c1c075fc69a57e7feb85d1530ab97b3799c25693f125",
			"worker condition-ref differs from the materialization checker oracle");
	auto portable_task = reconstruct_provider_task(
		task,
		exact_task_descriptors(),
		"sha256:1111111111111111111111111111111111111111111111111111111111111111");
	require(portable_task &&
				portable_task->task_id ==
					"task:semantic-v2:sha256:"
					"e7c1019d793afd1de2ae0228b452a48205523d29ec43e16632835032fc0c59bf" &&
				portable_task->project.catalog_id == task.project_catalog.catalog_id &&
				portable_task->condition == *condition_ref,
			"worker portable task differs from the shared/Python task identity oracle: " +
				(portable_task ? portable_task->task_id
							   : portable_task.error().code + "/" + portable_task.error().field +
						 "/" + portable_task.error().detail));
	auto condition_drift = task;
	condition_drift.condition_universe = "condition-universe:drift";
	auto drift_ref = provider_condition_ref_id(condition_drift);
	auto drift_encoding = encode_task_input(condition_drift);
	auto drift_task = reconstruct_provider_task(
		condition_drift,
		exact_task_descriptors(),
		"sha256:1111111111111111111111111111111111111111111111111111111111111111");
	require(drift_ref && drift_encoding && *drift_ref != *condition_ref && drift_task &&
				drift_task->task_id != portable_task->task_id &&
				sdk::content_digest(*drift_encoding) != sdk::content_digest(*encoded),
			"condition-universe drift retained the old condition-ref or task input identity");
	auto semantic_contract_drift = reconstruct_provider_task(
		task,
		exact_task_descriptors(),
		"sha256:2222222222222222222222222222222222222222222222222222222222222222");
	require(semantic_contract_drift && semantic_contract_drift->task_id != portable_task->task_id,
			"provider semantic-contract drift retained the old portable task identity");
	auto catalog_drift = task;
	catalog_drift.environment_digest =
		"sha256:9999999999999999999999999999999999999999999999999999999999999999";
	require(!encode_task_input(catalog_drift),
			"selected catalog entry environment drift was accepted");
	auto toolchain_digest_drift = task;
	toolchain_digest_drift.toolchain_digest =
		"sha256:1111111111111111111111111111111111111111111111111111111111111111";
	require(!encode_task_input(toolchain_digest_drift),
			"toolchain base-claim row digest drift was accepted");
	auto final_alias = task;
	final_alias.compile_unit = final_alias.selected_catalog_compile_unit;
	require(!encode_task_input(final_alias),
			"catalog-local identity was accepted as the final relation identity");
	auto final_identity_drift = task;
	final_identity_drift.compile_unit = "compile-unit:drift";
	require(!encode_task_input(final_identity_drift),
			"non-derived final compile-unit identity was accepted");
	auto file_identity_drift = task;
	file_identity_drift.file = "file:drift";
	require(!encode_task_input(file_identity_drift),
			"non-derived logical file identity was accepted");
	auto non_nfc_path = task;
	non_nfc_path.logical_path = "project://cafe\xCC\x81.cpp";
	auto non_nfc_encoding = encode_task_input(non_nfc_path);
	require(!non_nfc_encoding && non_nfc_encoding.error().field == "source.logical_path",
			"non-NFC source logical path did not fail at the path authority boundary");
	auto control_path = task;
	control_path.logical_path = "project://bad\npath.cpp";
	auto control_path_encoding = encode_task_input(control_path);
	require(!control_path_encoding && control_path_encoding.error().field == "source.logical_path",
			"source logical path accepted a schema-forbidden control scalar");
	auto line_index_identity_drift = task;
	line_index_identity_drift.line_index = "line-index:drift";
	require(!encode_task_input(line_index_identity_drift),
			"non-derived byte line-index identity was accepted");
	auto variant_identity_drift = task;
	variant_identity_drift.variant = "build-variant:drift";
	require(!encode_task_input(variant_identity_drift),
			"non-derived build variant identity was accepted");
	auto legacy_projection = sdk::canonical_binary_decode(*encoded);
	require(legacy_projection.has_value(), "task.v3 test projection did not decode");
	legacy_projection->tuple[0U].text = "cxxlens.clang22.task.v2";
	auto legacy_encoding = sdk::canonical_binary(*legacy_projection);
	require(legacy_encoding && !decode_task_input(*legacy_encoding),
			"legacy task.v2 marker remained adoptable");
	auto trailing = *encoded;
	trailing.push_back(std::byte{0});
	require(!decode_task_input(trailing), "task.v3 accepted trailing bytes");
	auto base64_spelling = task;
	base64_spelling.source_content_base64.back() = '=';
	base64_spelling.source_content_base64[base64_spelling.source_content_base64.size() - 2U] = '1';
	auto spelling_encoded = encode_task_input(base64_spelling);
	auto spelling_decoded = spelling_encoded ? decode_task_input(*spelling_encoded)
											 : sdk::result<clang22_task_input>{sdk::unexpected(
												   sdk::error{"sdk.test-setup", "task.v3", {}})};
	require(spelling_encoded && spelling_decoded && spelling_decoded->source == task.source &&
				spelling_decoded->source_content_base64 == base64_spelling.source_content_base64 &&
				sdk::content_digest(*spelling_encoded) != sdk::content_digest(*encoded),
			"task.v3 did not preserve the exact schema-valid base64 request spelling");
	auto missing_source_authority = task;
	missing_source_authority.source_snapshot.clear();
	require(!encode_task_input(missing_source_authority),
			"worker task fabricated a span identity without source authority");

	auto exact = canonicalize_provider_batch(
		batch(), "sha256:1111111111111111111111111111111111111111111111111111111111111111", true);
	require(exact && exact->exact_equivalence && exact->entity_observations.size() == 1U &&
				exact->type_observations.size() == 1U && exact->call_observations.size() == 1U &&
				exact->entities.size() == 1U && exact->call_sites.size() == 1U &&
				exact->direct_targets.size() == 1U && exact->unresolved.empty() &&
				string_cell(exact->call_sites.front(), "cc.call_site.v1.kind") == "direct_function",
			"exact Clang observation canonicalization failed");
	require(string_cell(exact->call_sites.front(), "cc.call_site.v1.source") == *span,
			"worker call-site hard reference differs from base source.span identity");
	auto missing_primary_bundle = batch();
	missing_primary_bundle.observations.front().primary_span.reset();
	missing_primary_bundle.observations.back().primary_span.reset();
	auto missing_primary_result = canonicalize_provider_batch(
		missing_primary_bundle,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(missing_primary_result && !missing_primary_result->exact_equivalence &&
				missing_primary_result->entity_observations.size() == 1U &&
				missing_primary_result->call_observations.size() == 1U &&
				missing_primary_result->entities.size() == 1U &&
				missing_primary_result->call_sites.empty() &&
				missing_primary_result->direct_targets.empty() &&
				missing_primary_result->unresolved.size() == 2U &&
				missing_primary_result->unresolved[0U].code == "provider.source-unavailable" &&
				missing_primary_result->unresolved[1U].code == "provider.source-unavailable" &&
				!boolean_cell(missing_primary_result->entity_observations.front(),
							  "frontend.clang22.entity_observation.v2.exact_equivalence") &&
				!boolean_cell(missing_primary_result->call_observations.front(),
							  "frontend.clang22.call_observation.v2.exact_equivalence") &&
				string_cell(missing_primary_result->entity_observations.front(),
							"frontend.clang22.entity_observation.v2.limitation") ==
					string_cell(missing_primary_result->call_observations.front(),
								"frontend.clang22.call_observation.v2.limitation"),
			"missing primary source authority was dropped or reported as exact");
	const auto& entity_row = exact->entities.front();
	const auto& call_row = exact->call_sites.front();
	require(
		sdk::validate_domain_identity(cxxlens::cc::relations::entity::descriptor(), entity_row) &&
			sdk::validate_domain_identity(cxxlens::cc::relations::call_site::descriptor(),
										  call_row),
		"worker standard row result ID differs from its descriptor projection");
	const std::array entity_reference{
		sdk::canonical_value::from_string("canonicalized"),
		sdk::canonical_value::from_string("function"),
		sdk::canonical_value::null(),
		sdk::canonical_value::from_string(
			string_cell(entity_row, "cc.entity.v1.structural_signature_digest")),
		sdk::canonical_value::from_string(string_cell(entity_row, "cc.entity.v1.toolchain")),
		sdk::canonical_value::from_bytes(bytes_cell(entity_row, "cc.entity.v1.provider_local_key")),
	};
	require(*sdk::canonical_identity_digest("cc-entity", entity_reference) ==
				string_cell(entity_row, "cc.entity.v1.entity"),
			"independent entity identity encoder differs from descriptor helper");
	const std::array call_reference{
		sdk::canonical_value::from_string(string_cell(call_row, "cc.call_site.v1.compile_unit")),
		sdk::canonical_value::from_string(string_cell(call_row, "cc.call_site.v1.source")),
		sdk::canonical_value::from_string("direct_function"),
		sdk::canonical_value::from_integer(0),
		sdk::canonical_value::null(),
	};
	require(*sdk::canonical_identity_digest("cc-call", call_reference) ==
				string_cell(call_row, "cc.call_site.v1.call"),
			"independent call identity encoder differs from descriptor helper");
	const auto entity_id = string_cell(entity_row, "cc.entity.v1.entity");
	for (auto mutation : {
			 std::pair{"cc.entity.v1.canonicalization",
					   symbol_cell(sdk::scalar_kind::closed_symbol,
								   "cc.canonicalization-state/1",
								   "provider_local")},
			 std::pair{"cc.entity.v1.kind",
					   symbol_cell(sdk::scalar_kind::open_symbol, "cc.entity-kind/1", "method")},
			 std::pair{"cc.entity.v1.semantic_owner",
					   optional_typed("cc_entity_id", "cc-entity:owner")},
			 std::pair{
				 "cc.entity.v1.structural_signature_digest",
				 symbol_cell(sdk::scalar_kind::digest, {}, "sha256:" + std::string(64U, 'f'))},
			 std::pair{"cc.entity.v1.toolchain",
					   optional_typed("toolchain_context_id", "toolchain-context:changed")},
			 std::pair{"cc.entity.v1.provider_local_key", optional_bytes({std::byte{0x42}})},
		 })
	{
		auto changed = entity_row;
		changed.cells.insert_or_assign(mutation.first, std::move(mutation.second));
		auto changed_id =
			sdk::derive_domain_identity(cxxlens::cc::relations::entity::descriptor(), changed);
		require(changed_id && *changed_id != entity_id,
				"entity projection mutation did not change identity: " +
					std::string{mutation.first});
	}
	auto changed_occurrence_anchor = entity_row;
	changed_occurrence_anchor.cells.insert_or_assign(
		"cc.entity.v1.anchor", optional_typed("source_span_id", "source-span:changed"));
	auto changed_occurrence_identity = sdk::derive_domain_identity(
		cxxlens::cc::relations::entity::descriptor(), changed_occurrence_anchor);
	require(changed_occurrence_identity && *changed_occurrence_identity == entity_id,
			"declaration occurrence anchor changed semantic entity identity");
	const auto call_id = string_cell(call_row, "cc.call_site.v1.call");
	for (auto mutation : {
			 std::pair{"cc.call_site.v1.compile_unit",
					   sdk::detached_cell::typed("compile_unit_id", "compile-unit:changed")},
			 std::pair{"cc.call_site.v1.source",
					   sdk::detached_cell::typed("source_span_id", "source-span:changed")},
			 std::pair{"cc.call_site.v1.kind",
					   symbol_cell(sdk::scalar_kind::open_symbol, "cc.call-kind/1", "virtual")},
			 std::pair{"cc.call_site.v1.ordinal", sdk::detached_cell::unsigned_integer(1U)},
			 std::pair{"cc.call_site.v1.caller",
					   optional_typed("cc_entity_id", "cc-entity:caller")},
		 })
	{
		auto changed = call_row;
		changed.cells.insert_or_assign(mutation.first, std::move(mutation.second));
		auto changed_id =
			sdk::derive_domain_identity(cxxlens::cc::relations::call_site::descriptor(), changed);
		require(changed_id && *changed_id != call_id,
				"call projection mutation did not change identity");
	}
	auto display_only = entity_row;
	display_only.cells.at("cc.entity.v1.qualified_name") = optional_utf8("renamed::display");
	auto display_identity =
		sdk::derive_domain_identity(cxxlens::cc::relations::entity::descriptor(), display_only);
	auto unchanged_identity =
		sdk::derive_domain_identity(cxxlens::cc::relations::entity::descriptor(), entity_row);
	require(display_identity && unchanged_identity && *display_identity == *unchanged_identity,
			"non-projection display field changed entity identity");
	auto hidden_variant = batch();
	hidden_variant.variant = "variant-" + std::string(64U, 'f');
	auto hidden_result = canonicalize_provider_batch(
		hidden_variant,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(hidden_result &&
				string_cell(hidden_result->entities.front(), "cc.entity.v1.entity") == entity_id &&
				string_cell(hidden_result->call_sites.front(), "cc.call_site.v1.call") == call_id,
			"hidden batch variant changed a standard relation ID");

	observation_batch redeclaration_batch;
	redeclaration_batch.unit = "cu-" + std::string(64U, 'a');
	redeclaration_batch.variant = "variant-" + std::string(64U, 'b');
	bind_source_authority(redeclaration_batch);
	redeclaration_batch.observations = {
		redeclaration("clang-usr:redeclared", "void ()", 10U, false, true),
		redeclaration("clang-usr:redeclared", "void ()", 40U, true, false),
	};
	auto canonical_redeclaration = canonicalize_provider_batch(
		redeclaration_batch,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(canonical_redeclaration && canonical_redeclaration->exact_equivalence &&
				canonical_redeclaration->entities.size() == 1U &&
				canonical_redeclaration->entity_observations.size() == 2U &&
				canonical_redeclaration->unresolved.empty() &&
				!canonical_redeclaration->entities.front().cells.contains("cc.entity.v1.anchor") &&
				string_cell(canonical_redeclaration->entity_observations.front(),
							"frontend.clang22.entity_observation.v2.observation") !=
					string_cell(canonical_redeclaration->entity_observations.back(),
								"frontend.clang22.entity_observation.v2.observation"),
			"redeclaration occurrences were not separated from one semantic entity");
	auto reversed_redeclarations = redeclaration_batch;
	std::ranges::reverse(reversed_redeclarations.observations);
	auto reversed_canonical = canonicalize_provider_batch(
		reversed_redeclarations,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(reversed_canonical && reversed_canonical->entities.size() == 1U &&
				reversed_canonical->entities.front().canonical_form() ==
					canonical_redeclaration->entities.front().canonical_form(),
			"redeclaration arrival order changed canonical entity payload");
	auto declarations_only = redeclaration_batch;
	declarations_only.observations.back() =
		redeclaration("clang-usr:redeclared", "void ()", 40U, false, false);
	auto declaration_result = canonicalize_provider_batch(
		declarations_only,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(declaration_result && declaration_result->entities.size() == 1U &&
				declaration_result->entity_observations.size() == 2U &&
				!declaration_result->entities.front().cells.contains("cc.entity.v1.anchor"),
			"declaration occurrence leaked into the semantic entity row");
	auto moved_anchor = declarations_only;
	moved_anchor.observations = {
		redeclaration("clang-usr:redeclared", "void ()", 80U, false, true)};
	auto moved_result = canonicalize_provider_batch(
		moved_anchor,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(moved_result &&
				string_cell(moved_result->entities.front(), "cc.entity.v1.entity") ==
					string_cell(declaration_result->entities.front(), "cc.entity.v1.entity") &&
				string_cell(moved_result->entities.front(),
							"cc.entity.v1.structural_signature_digest") ==
					string_cell(declaration_result->entities.front(),
								"cc.entity.v1.structural_signature_digest"),
			"source anchor movement changed structural signature identity");
	auto overloads = declarations_only;
	overloads.observations.push_back(
		redeclaration("clang-usr:redeclared-overload", "void (int)", 100U, true, true));
	auto overload_result = canonicalize_provider_batch(
		overloads, "sha256:1111111111111111111111111111111111111111111111111111111111111111", true);
	require(overload_result && overload_result->entities.size() == 2U,
			"distinct overload semantic keys were collapsed");
	auto incompatible = declarations_only;
	incompatible.observations.back() =
		redeclaration("clang-usr:redeclared", "int ()", 40U, true, false);
	auto incompatible_result = canonicalize_provider_batch(
		incompatible,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(incompatible_result && !incompatible_result->exact_equivalence &&
				incompatible_result->entities.size() == 1U &&
				incompatible_result->unresolved.size() == 1U &&
				incompatible_result->unresolved.front().code ==
					"provider.entity-redeclaration-incompatible",
			"incompatible redeclarations were not explicitly accounted");
	const auto same_tu_target =
		string_cell(exact->direct_targets.front(), "cc.call_direct_target.v1.target");

	auto cross_tu = batch();
	cross_tu.unit = "cu-" + std::string(64U, 'c');
	cross_tu.materialization_authority->final_relation_compile_unit_id = cross_tu.unit;
	cross_tu.observations.erase(std::ranges::remove_if(cross_tu.observations,
													   [](const detached_observation& value)
													   {
														   return value.kind !=
															   observation_kind::call;
													   })
									.begin(),
								cross_tu.observations.end());
	for (auto& value : cross_tu.observations)
		value.compile_unit = cross_tu.unit;
	require(cross_tu.validate().has_value(), "cross-TU call-only batch is invalid");
	auto cross_tu_result = canonicalize_provider_batch(
		cross_tu, "sha256:1111111111111111111111111111111111111111111111111111111111111111", true);
	require(cross_tu_result && cross_tu_result->entities.empty() &&
				cross_tu_result->direct_targets.size() == 1U &&
				cross_tu_result->unresolved.empty() &&
				string_cell(cross_tu_result->direct_targets.front(),
							"cc.call_direct_target.v1.target") == same_tu_target,
			"header/external/other-TU direct callee depended on a local entity row");
	auto relocated_definition = batch();
	relocated_definition.observations.erase(
		std::ranges::remove_if(relocated_definition.observations,
							   [](const detached_observation& value)
							   {
								   return value.kind != observation_kind::entity;
							   })
			.begin(),
		relocated_definition.observations.end());
	set_primary_span(relocated_definition.observations.front(),
					 "source-snapshot:relocated",
					 "file:definition",
					 400U,
					 440U,
					 "declaration");
	bind_source_authority(relocated_definition, "source-snapshot:relocated", "file:definition");
	auto relocated_definition_result = canonicalize_provider_batch(
		relocated_definition,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(relocated_definition_result && relocated_definition_result->entities.size() == 1U &&
				string_cell(relocated_definition_result->entities.front(), "cc.entity.v1.entity") ==
					same_tu_target,
			"definition source relocation changed semantic entity identity");
	auto legacy_forward_anchor = cross_tu;
	legacy_forward_anchor.observations.front().payload.emplace(
		"call.direct_callee_anchor",
		*sdk::source_span_identity("source-snapshot:caller", "file:caller", 1U, 9U, "declaration"));
	auto legacy_forward_result = canonicalize_provider_batch(
		legacy_forward_anchor,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(legacy_forward_result &&
				string_cell(legacy_forward_result->direct_targets.front(),
							"cc.call_direct_target.v1.target") == same_tu_target,
			"caller-local forward declaration anchor changed cross-TU target identity");

	struct cross_tu_identity_case
	{
		std::string semantic_key;
		std::string entity_kind;
		std::string signature;
		std::string call_kind;
	};
	const std::array identity_cases{
		cross_tu_identity_case{
			"clang-usr:overload-int", "function", "void (int)", "direct_function"},
		cross_tu_identity_case{
			"clang-usr:template-int", "function", "void (T<int>)", "direct_function"},
		cross_tu_identity_case{"clang-usr:member", "method", "void () const", "direct_member"},
		cross_tu_identity_case{"clang-usr:operator-plus", "method", "value (value)", "operator"},
	};
	for (std::size_t index = 0U; index < identity_cases.size(); ++index)
	{
		const auto& identity_case = identity_cases[index];
		observation_batch entity_only;
		entity_only.unit = "cu-" + std::string(63U, 'e') + std::to_string(index);
		entity_only.variant = cross_tu.variant;
		bind_source_authority(entity_only, "source-snapshot:definitions", "file:definitions");
		auto entity = observation(observation_kind::entity, identity_case.semantic_key);
		entity.compile_unit = entity_only.unit;
		set_primary_span(entity,
						 "source-snapshot:definitions",
						 "file:definitions",
						 100U + index * 20U,
						 110U + index * 20U,
						 "declaration");
		entity.payload.emplace("symbol.kind", identity_case.entity_kind);
		entity.payload.emplace("symbol.signature", identity_case.signature);
		entity_only.observations.push_back(std::move(entity));

		observation_batch call_only;
		call_only.unit = "cu-" + std::string(63U, 'f') + std::to_string(index);
		call_only.variant = cross_tu.variant;
		bind_source_authority(call_only);
		auto call = observation(observation_kind::call, "call:cross-tu:" + std::to_string(index));
		call.compile_unit = call_only.unit;
		call.payload.emplace("call.kind", identity_case.call_kind);
		call.payload.emplace("call.direct_callee", identity_case.semantic_key);
		call.payload.emplace("call.direct_callee_kind", identity_case.entity_kind);
		call.payload.emplace("call.direct_callee_signature", identity_case.signature);
		call_only.observations.push_back(std::move(call));
		auto entity_result = canonicalize_provider_batch(
			entity_only,
			"sha256:1111111111111111111111111111111111111111111111111111111111111111",
			true);
		auto call_result = canonicalize_provider_batch(
			call_only,
			"sha256:1111111111111111111111111111111111111111111111111111111111111111",
			true);
		require(entity_result && call_result && entity_result->entities.size() == 1U &&
					call_result->direct_targets.size() == 1U &&
					string_cell(entity_result->entities.front(), "cc.entity.v1.entity") ==
						string_cell(call_result->direct_targets.front(),
									"cc.call_direct_target.v1.target"),
				"cross-TU overload/template/member/operator target did not join entity");
	}
	auto macro_calls = cross_tu;
	macro_calls.observations.front().origins = {
		{"macro-spelling", "project://include/macros.hpp", 10, 18, true},
		{"macro-spelling", "project://include/wrapper.hpp", 20, 28, true},
	};
	auto second_expansion = macro_calls.observations.front();
	second_expansion.semantic_key = "call:macro-expansion:two";
	set_primary_span(
		second_expansion, "source-snapshot:one", "file:stable", 30U, 42U, "expression");
	macro_calls.observations.push_back(std::move(second_expansion));
	auto macro_result = canonicalize_provider_batch(
		macro_calls,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(macro_result && macro_result->call_sites.size() == 2U &&
				macro_result->direct_targets.size() == 2U &&
				macro_result->call_observations.size() == 2U &&
				!bytes_cell(macro_result->call_observations.front(),
							"frontend.clang22.call_observation.v2.source_origin_chain")
					 .empty() &&
				string_cell(macro_result->call_sites.front(), "cc.call_site.v1.call") !=
					string_cell(macro_result->call_sites.back(), "cc.call_site.v1.call"),
			"distinct macro expansion offsets were deduplicated or lost their origin chain");
	auto repeated_macro = cross_tu;
	repeated_macro.observations.front().semantic_key = "call:macro-twice";
	repeated_macro.observations.front().origins = {
		{"macro-spelling", "project://include/macros.hpp", 10, 18, true}};
	auto second_spelling_call = repeated_macro.observations.front();
	second_spelling_call.origins = {
		{"macro-spelling", "project://include/macros.hpp", 28, 36, true}};
	require(observation_dedup_key(repeated_macro.observations.front()) !=
				observation_dedup_key(second_spelling_call),
			"distinct macro spelling occurrences share the pre-normalization dedup key");
	repeated_macro.observations.push_back(std::move(second_spelling_call));
	auto repeated_macro_result = canonicalize_provider_batch(
		repeated_macro,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(repeated_macro_result && repeated_macro_result->call_observations.size() == 2U &&
				repeated_macro_result->call_sites.size() == 2U &&
				repeated_macro_result->direct_targets.size() == 2U &&
				string_cell(repeated_macro_result->call_sites.front(), "cc.call_site.v1.call") !=
					string_cell(repeated_macro_result->call_sites.back(), "cc.call_site.v1.call"),
			"one TWICE macro expansion collapsed its two same-callee calls");
	auto reversed_repeated_macro = repeated_macro;
	std::ranges::reverse(reversed_repeated_macro.observations);
	auto reversed_repeated_result = canonicalize_provider_batch(
		reversed_repeated_macro,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(reversed_repeated_result &&
				canonical_batch(*reversed_repeated_result) ==
					canonical_batch(*repeated_macro_result),
			"same-expansion macro call IDs depend on observation input order");

	auto whitespace_shifted = repeated_macro;
	whitespace_shifted.observations.front().origins.front().begin = 14;
	whitespace_shifted.observations.front().origins.front().end = 22;
	whitespace_shifted.observations.back().origins.front().begin = 42;
	whitespace_shifted.observations.back().origins.front().end = 50;
	auto whitespace_shifted_result = canonicalize_provider_batch(
		whitespace_shifted,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(
		whitespace_shifted_result && whitespace_shifted_result->call_sites.size() == 2U &&
			string_cell(whitespace_shifted_result->call_sites.front(), "cc.call_site.v1.call") ==
				string_cell(repeated_macro_result->call_sites.front(), "cc.call_site.v1.call") &&
			string_cell(whitespace_shifted_result->call_sites.back(), "cc.call_site.v1.call") ==
				string_cell(repeated_macro_result->call_sites.back(), "cc.call_site.v1.call"),
		"whitespace/comment spelling offset shift changed stable macro call ordinals");

	auto two_twice_expansions = repeated_macro;
	for (const auto& call : repeated_macro.observations)
	{
		auto second_invocation_call = call;
		set_primary_span(
			second_invocation_call, "source-snapshot:one", "file:stable", 50U, 62U, "expression");
		two_twice_expansions.observations.push_back(std::move(second_invocation_call));
	}
	auto two_twice_result = canonicalize_provider_batch(
		two_twice_expansions,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(two_twice_result && two_twice_result->call_sites.size() == 4U &&
				two_twice_result->direct_targets.size() == 4U,
			"two TWICE macro expansions did not preserve four call occurrences");

	auto distinct_callees = repeated_macro;
	distinct_callees.observations.back().semantic_key = "call:macro-other-callee";
	distinct_callees.observations.back().payload.insert_or_assign("call.direct_callee",
																  "clang-usr:other-target");
	auto distinct_callee_result = canonicalize_provider_batch(
		distinct_callees,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(distinct_callee_result && distinct_callee_result->call_sites.size() == 2U &&
				distinct_callee_result->direct_targets.size() == 2U,
			"same-expansion distinct callees were not preserved once each");
	auto relocated_macro = macro_calls;
	for (auto& call : relocated_macro.observations)
		call.origins.front().logical_path = "project://relocated/include/macros.hpp";
	auto relocated_macro_result = canonicalize_provider_batch(
		relocated_macro,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(relocated_macro_result && relocated_macro_result->call_sites.size() == 2U &&
				string_cell(relocated_macro_result->call_sites.front(), "cc.call_site.v1.call") ==
					string_cell(macro_result->call_sites.front(), "cc.call_site.v1.call") &&
				string_cell(relocated_macro_result->call_sites.back(), "cc.call_site.v1.call") ==
					string_cell(macro_result->call_sites.back(), "cc.call_site.v1.call") &&
				string_cell(relocated_macro_result->call_observations.front(),
							"frontend.clang22.call_observation.v2.observation") !=
					string_cell(macro_result->call_observations.front(),
								"frontend.clang22.call_observation.v2.observation"),
			"native macro origin did not bind observation authority or changed semantic call IDs");
	auto source_policy_changed = macro_calls;
	source_policy_changed.observations.front().primary_span->read_only =
		!source_policy_changed.observations.front().primary_span->read_only;
	require(observation_dedup_key(source_policy_changed.observations.front()) !=
				observation_dedup_key(macro_calls.observations.front()),
			"primary source policy is a hidden input outside the native-v2 dedup key");
	auto source_policy_result = canonicalize_provider_batch(
		source_policy_changed,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(source_policy_result &&
				string_cell(source_policy_result->call_observations.front(),
							"frontend.clang22.call_observation.v2.observation") !=
					string_cell(macro_result->call_observations.front(),
								"frontend.clang22.call_observation.v2.observation"),
			"primary source read-only policy was not bound into observation authority");

	auto origin_order = cross_tu;
	origin_order.observations.front().origins = {
		{"macro-expansion", "project://include/outer.hpp", 20, 28, true},
		{"macro-spelling", "project://include/inner.hpp", 10, 18, true},
	};
	auto reversed_origin_order = origin_order;
	std::ranges::reverse(reversed_origin_order.observations.front().origins);
	require(observation_dedup_key(origin_order.observations.front()) !=
				observation_dedup_key(reversed_origin_order.observations.front()),
			"origin-chain order is absent from the native-v2 dedup key");
	auto origin_order_result = canonicalize_provider_batch(
		origin_order,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	auto reversed_origin_order_result = canonicalize_provider_batch(
		reversed_origin_order,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(origin_order_result && reversed_origin_order_result &&
				string_cell(origin_order_result->call_sites.front(), "cc.call_site.v1.call") ==
					string_cell(reversed_origin_order_result->call_sites.front(),
								"cc.call_site.v1.call") &&
				string_cell(origin_order_result->call_observations.front(),
							"frontend.clang22.call_observation.v2.observation") !=
					string_cell(reversed_origin_order_result->call_observations.front(),
								"frontend.clang22.call_observation.v2.observation"),
			"origin-chain order was normalized away or leaked into the standard call ID");
	auto decoded_reversed_origin = materialization::decode_observation_v2_row(
		reversed_origin_order_result->call_observations.front(),
		*reversed_origin_order.materialization_authority);
	require(decoded_reversed_origin &&
				decoded_reversed_origin->origin_chain ==
					reversed_origin_order.observations.front().origins,
			"origin-chain order was not preserved through the v2 row codec");
	auto duplicate_origin = origin_order;
	duplicate_origin.observations.front().origins = {
		origin_order.observations.front().origins.front(),
		origin_order.observations.front().origins.front(),
	};
	auto duplicate_origin_result = canonicalize_provider_batch(
		duplicate_origin,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	auto decoded_duplicate_origin = duplicate_origin_result
		? materialization::decode_observation_v2_row(
			  duplicate_origin_result->call_observations.front(),
			  *duplicate_origin.materialization_authority)
		: sdk::result<materialization::decoded_observation_v2_row>{
			  sdk::unexpected(sdk::error{"sdk.test-setup", "duplicate-origin", {}})};
	require(decoded_duplicate_origin && decoded_duplicate_origin->origin_chain.size() == 2U &&
				decoded_duplicate_origin->origin_chain.front() ==
					decoded_duplicate_origin->origin_chain.back(),
			"duplicate native origins were deduplicated or dropped");
	auto writable_origin = origin_order;
	writable_origin.observations.front().origins.front().read_only = false;
	require(!writable_origin.observations.front().validate() &&
				!canonicalize_provider_batch(
					writable_origin,
					"sha256:1111111111111111111111111111111111111111111111111111111111111111",
					true),
			"writable native origin passed detached or batch validation");

	auto reordered = batch();
	std::ranges::reverse(reordered.observations);
	auto reordered_result = canonicalize_provider_batch(
		reordered, "sha256:1111111111111111111111111111111111111111111111111111111111111111", true);
	require(reordered_result && reordered_result->direct_targets.size() == 1U &&
				string_cell(reordered_result->direct_targets.front(),
							"cc.call_direct_target.v1.target") == same_tu_target,
			"target identity changed with observation order");
	require(reordered_result && canonical_batch(*reordered_result) == canonical_batch(*exact),
			"observation permutation changed canonical batch bytes");
	auto inserted_call = batch();
	auto unrelated = inserted_call.observations.back();
	unrelated.semantic_key = "call:unrelated-prefix";
	set_primary_span(unrelated, "source-snapshot:one", "file:stable", 1U, 5U, "expression");
	inserted_call.observations.insert(inserted_call.observations.begin(), std::move(unrelated));
	auto inserted_result = canonicalize_provider_batch(
		inserted_call,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(inserted_result.has_value(), "unrelated call insertion failed canonicalization");
	const auto original_source = string_cell(exact->call_sites.front(), "cc.call_site.v1.source");
	auto original_after_insertion = std::ranges::find_if(
		inserted_result->call_sites,
		[&](const sdk::detached_row& row)
		{
			return string_cell(row, "cc.call_site.v1.source") == original_source;
		});
	require(original_after_insertion != inserted_result->call_sites.end() &&
				string_cell(*original_after_insertion, "cc.call_site.v1.call") == call_id,
			"unrelated call insertion changed an existing call identity");
	auto same_span = batch();
	auto duplicate = same_span.observations.back();
	duplicate.semantic_key = "call:same-span-tie";
	same_span.observations.push_back(std::move(duplicate));
	auto same_span_result = canonicalize_provider_batch(
		same_span, "sha256:1111111111111111111111111111111111111111111111111111111111111111", true);
	std::ranges::reverse(same_span.observations);
	auto reversed_same_span = canonicalize_provider_batch(
		same_span, "sha256:1111111111111111111111111111111111111111111111111111111111111111", true);
	require(same_span_result && reversed_same_span && same_span_result->call_sites.size() == 2U &&
				canonical_batch(*same_span_result) == canonical_batch(*reversed_same_span) &&
				string_cell(same_span_result->call_sites.front(), "cc.call_site.v1.call") !=
					string_cell(same_span_result->call_sites.back(), "cc.call_site.v1.call"),
			"same-span canonical tie-break was absent or input-order dependent");
	auto framing_collision = batch();
	framing_collision.observations.clear();
	auto newline_semantic = observation(observation_kind::call, "a\n1:k0:");
	newline_semantic.payload.clear();
	auto payload_semantic = observation(observation_kind::call, "a");
	payload_semantic.payload.clear();
	payload_semantic.payload.emplace("k", "");
	require(newline_semantic.canonical_form() != payload_semantic.canonical_form(),
			"native-v2 order key collides across semantic-key and payload framing");
	framing_collision.observations = {newline_semantic, payload_semantic};
	auto framing_result = canonicalize_provider_batch(
		framing_collision,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	std::ranges::reverse(framing_collision.observations);
	auto reversed_framing_result = canonicalize_provider_batch(
		framing_collision,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(framing_result && reversed_framing_result && framing_result->call_sites.size() == 2U &&
				canonical_batch(*framing_result) == canonical_batch(*reversed_framing_result),
			"native-v2 framing collision made call ordinals input-order dependent");

	auto indirect = cross_tu;
	indirect.observations.front().payload.erase("call.direct_callee");
	indirect.observations.front().payload.insert_or_assign("call.kind", "indirect_function");
	indirect.observations.front().payload.emplace("call.unresolved_reason",
												  "function-pointer-target-not-modeled");
	auto indirect_result = canonicalize_provider_batch(
		indirect, "sha256:1111111111111111111111111111111111111111111111111111111111111111", true);
	require(indirect_result && indirect_result->direct_targets.empty() &&
				indirect_result->unresolved.size() == 1U &&
				indirect_result->unresolved.front().code == "provider.indirect-target-unresolved" &&
				string_cell(indirect_result->call_sites.front(), "cc.call_site.v1.kind") ==
					"indirect_function",
			"indirect call received a fabricated direct target");
	auto member_pointer = indirect;
	member_pointer.observations.front().payload.insert_or_assign("call.kind",
																 "indirect_member_pointer");
	member_pointer.observations.front().payload.insert_or_assign(
		"call.unresolved_reason", "member-pointer-target-not-modeled");
	auto member_pointer_result = canonicalize_provider_batch(
		member_pointer,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(member_pointer_result && member_pointer_result->direct_targets.empty() &&
				member_pointer_result->unresolved.size() == 1U &&
				string_cell(member_pointer_result->call_sites.front(), "cc.call_site.v1.kind") ==
					"indirect_member_pointer",
			"member-pointer call was not classified as indirect");
	auto dependent = indirect;
	dependent.observations.front().payload.insert_or_assign("call.kind", "dependent");
	dependent.observations.front().payload.insert_or_assign("call.unresolved_reason",
															"dependent-callee");
	auto dependent_result = canonicalize_provider_batch(
		dependent, "sha256:1111111111111111111111111111111111111111111111111111111111111111", true);
	require(dependent_result && dependent_result->direct_targets.empty() &&
				dependent_result->unresolved.size() == 1U &&
				dependent_result->unresolved.front().code == "provider.call-target-unresolved" &&
				string_cell(dependent_result->call_sites.front(), "cc.call_site.v1.kind") ==
					"dependent",
			"dependent call was not preserved as explicit unresolved");
	auto virtual_call = cross_tu;
	virtual_call.observations.front().payload.insert_or_assign("call.kind", "virtual_member");
	auto virtual_result = canonicalize_provider_batch(
		virtual_call,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(virtual_result && virtual_result->direct_targets.size() == 1U &&
				virtual_result->unresolved.empty() &&
				string_cell(virtual_result->call_sites.front(), "cc.call_site.v1.kind") ==
					"virtual_member",
			"virtual dispatch lost its static direct target or dynamic-dispatch kind");
	auto inconsistent = indirect;
	inconsistent.observations.front().payload.insert_or_assign("call.kind", "direct_function");
	auto inconsistent_result = canonicalize_provider_batch(
		inconsistent,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(inconsistent_result && !inconsistent_result->exact_equivalence &&
				inconsistent_result->direct_targets.empty() &&
				inconsistent_result->unresolved.size() == 1U &&
				inconsistent_result->unresolved.front().code ==
					"provider.call-kind-target-inconsistent",
			"direct kind without a direct callee was not rejected by the invariant");
	auto fabricated_indirect = cross_tu;
	fabricated_indirect.observations.front().payload.insert_or_assign("call.kind",
																	  "indirect_function");
	auto fabricated_result = canonicalize_provider_batch(
		fabricated_indirect,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(fabricated_result && !fabricated_result->exact_equivalence &&
				fabricated_result->direct_targets.empty() &&
				fabricated_result->unresolved.size() == 1U &&
				fabricated_result->unresolved.front().code ==
					"provider.call-kind-target-inconsistent",
			"indirect kind with a direct callee fabricated a direct-target row");

	const std::vector<std::string> gcc_specific{"g++", "-fabi-version=18", "-c", "input.cpp"};
	std::vector<std::string> limitations;
	require(!invocation_has_exact_equivalence(gcc_specific, limitations) &&
				limitations.front().starts_with("ignored-or-gcc-specific-option:"),
			"GCC-specific semantic option claimed exact equivalence");

	auto provider_local = canonicalize_provider_batch(
		batch(),
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		false,
		limitations);
	require(provider_local && !provider_local->exact_equivalence &&
				!provider_local->equivalence_limitations.empty() &&
				provider_local->entity_observations.front().canonical_form().contains(
					"ignored-or-gcc-specific-option"),
			"semantic loss was not preserved as provider-local evidence");
}
