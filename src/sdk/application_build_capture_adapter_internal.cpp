#include "application_build_capture_adapter_internal.hpp"

#include <algorithm>
#include <array>
#include <ranges>
#include <string_view>
#include <tuple>

#include <cxxlens/relations/build_compile_unit.hpp>
#include <cxxlens/relations/build_project.hpp>
#include <cxxlens/relations/build_toolchain_context.hpp>
#include <cxxlens/relations/build_variant.hpp>

#include "compiler_replay_input_internal.hpp"

namespace cxxlens::sdk::detail
{
	namespace
	{
		[[nodiscard]] error unavailable(std::string field, std::string detail)
		{
			return {"application-analysis.materialization-unavailable",
					std::move(field),
					std::move(detail)};
		}

		[[nodiscard]] result<std::string> semantic_tuple(std::string_view domain,
														 std::vector<canonical_value> values)
		{
			auto encoded = canonical_binary(canonical_value::from_tuple(std::move(values)));
			if (!encoded)
				return unexpected(std::move(encoded.error()));
			return semantic_digest(
				domain,
				std::string{reinterpret_cast<const char*>(encoded->data()), encoded->size()});
		}

		[[nodiscard]] const capture_gap* find_gap(const std::span<const capture_gap> gaps,
												  const std::string_view field)
		{
			const auto found = std::ranges::find(gaps, field, &capture_gap::field);
			return found == gaps.end() ? nullptr : &*found;
		}

		template <class value_type>
		[[nodiscard]] captured_value<value_type>
		missing_value(const std::span<const capture_gap> gaps,
					  const std::string_view field,
					  std::string default_reason,
					  std::string default_action)
		{
			if (const auto* gap = find_gap(gaps, field); gap != nullptr)
			{
				if (gap->state == "redacted")
					return captured_value<value_type>::redacted(gap->reason,
																gap->completion_action);
				return captured_value<value_type>::unavailable(gap->reason, gap->completion_action);
			}
			return captured_value<value_type>::unavailable(std::move(default_reason),
														   std::move(default_action));
		}

		[[nodiscard]] captured_value<std::string>
		captured_string(const decoded_capture_environment_effect& input)
		{
			if (input.state == "observed" && input.semantic_value)
				return captured_value<std::string>::observed(*input.semantic_value);
			if (input.state == "derived" && input.semantic_value)
				return captured_value<std::string>::derived(*input.semantic_value);
			if (input.state == "redacted" && !input.semantic_value)
				return captured_value<std::string>::redacted(input.reason, input.completion_action);
			return captured_value<std::string>::unavailable(input.reason, input.completion_action);
		}

		[[nodiscard]] replay_option_class option_class(const replay_fidelity value)
		{
			switch (value)
			{
				case replay_fidelity::exact:
					return replay_option_class::exact;
				case replay_fidelity::semantics_preserving:
					return replay_option_class::semantics_preserving;
				case replay_fidelity::approximation:
					return replay_option_class::approximation;
				case replay_fidelity::unsupported:
					return replay_option_class::unsupported;
				case replay_fidelity::nonsemantic:
					return replay_option_class::nonsemantic;
			}
			return replay_option_class::unsupported;
		}

		[[nodiscard]] result<std::string>
		semantic_flags_digest(const std::span<const normalized_build_option> options)
		{
			std::vector<canonical_value> values;
			values.reserve(options.size());
			for (const auto& option : options)
				if (option.classification != replay_option_class::nonsemantic)
					values.push_back(canonical_value::from_tuple({
						canonical_value::from_string(option.token),
						canonical_value::from_integer(
							static_cast<std::int64_t>(option.classification)),
						canonical_value::from_string(option.reason),
						canonical_value::from_string(option.completion_action),
					}));
			return semantic_tuple("application-analysis-semantic-flags.v1", std::move(values));
		}

		[[nodiscard]] captured_value<std::vector<build_capture_auxiliary_file>>
		auxiliary_files(const std::vector<decoded_capture_auxiliary_file>& input,
						const std::span<const capture_gap> gaps,
						const std::string_view field)
		{
			if (input.empty())
				if (const auto* gap = find_gap(gaps, field); gap != nullptr)
					return missing_value<std::vector<build_capture_auxiliary_file>>(
						gaps, field, gap->reason, gap->completion_action);
			std::vector<build_capture_auxiliary_file> output;
			output.reserve(input.size());
			for (std::size_t index{}; index < input.size(); ++index)
			{
				const auto& value = input[index];
				const auto digest_field =
					std::string{field} + "[" + std::to_string(index) + "].content_digest";
				output.push_back({value.logical_path,
								  value.content_digest
									  ? captured_value<std::string>::observed(*value.content_digest)
									  : missing_value<std::string>(gaps,
																   digest_field,
																   "auxiliary-content-unavailable",
																   "recapture-auxiliary-file"),
								  value.size_bytes,
								  value.parent_index});
			}
			return captured_value<std::vector<build_capture_auxiliary_file>>::observed(
				std::move(output));
		}

		[[nodiscard]] result<std::string> require_exact(const std::optional<std::string>& value,
														const std::span<const capture_gap> gaps,
														const std::string_view field)
		{
			if (value)
				return *value;
			if (const auto* gap = find_gap(gaps, field); gap != nullptr)
				return unexpected(unavailable(std::string{field}, gap->reason));
			return unexpected(unavailable(std::string{field}, "capture-field-unavailable"));
		}

		[[nodiscard]] detached_cell digest_cell(std::string value)
		{
			return {{scalar_kind::digest, {}, false},
					cell_state::present,
					scalar_value{std::move(value)},
					{}};
		}

		[[nodiscard]] detached_cell symbol_cell(std::string contract, std::string value)
		{
			return {{scalar_kind::open_symbol, std::move(contract), false},
					cell_state::present,
					scalar_value{std::move(value)},
					{}};
		}

		[[nodiscard]] detached_cell optional_path(const std::optional<std::string>& value)
		{
			value_type type{scalar_kind::typed_id, "logical_path_id", true};
			return value
				? detached_cell{std::move(type), cell_state::present, scalar_value{*value}, {}}
				: detached_cell::absent(std::move(type));
		}

		[[nodiscard]] result<void> bind_relation_identities(build_capture_draft& draft)
		{
			detached_row project{
				build::relations::project::descriptor().id,
				{{"build.project.v1.project",
				  detached_cell::typed("project_id", "identity:pending")},
				 {"build.project.v1.catalog",
				  detached_cell::typed("catalog_id", draft.catalog.catalog_id)},
				 {"build.project.v1.catalog_digest", digest_cell(draft.catalog.catalog_digest)},
				 {"build.project.v1.logical_root",
				  detached_cell::typed("logical_path_id", draft.catalog.logical_root)},
				 {"build.project.v1.environment_digest",
				  digest_cell(draft.catalog.environment_digest)}}};
			auto project_id =
				derive_domain_identity(build::relations::project::descriptor(), project);
			if (!project_id)
				return unexpected(unavailable("build.project",
											  project_id.error().code + ":" +
												  project_id.error().field + ":" +
												  project_id.error().detail));
			draft.project_id = *project_id;

			detached_row toolchain{
				build::relations::toolchain_context::descriptor().id,
				{{"build.toolchain_context.v1.toolchain",
				  detached_cell::typed("toolchain_context_id", "identity:pending")},
				 {"build.toolchain_context.v1.family",
				  symbol_cell("build.toolchain-family/1", draft.toolchain.family)},
				 {"build.toolchain_context.v1.exact_version",
				  detached_cell::utf8(draft.toolchain.exact_version)},
				 {"build.toolchain_context.v1.target_triple",
				  detached_cell::utf8(draft.toolchain.target_triple)},
				 {"build.toolchain_context.v1.builtin_headers_digest",
				  digest_cell(draft.toolchain.builtin_headers_digest)},
				 {"build.toolchain_context.v1.sysroot", optional_path(draft.toolchain.sysroot)},
				 {"build.toolchain_context.v1.abi_digest", digest_cell(draft.toolchain.abi_digest)},
				 {"build.toolchain_context.v1.plugin_spec_digest",
				  digest_cell(draft.toolchain.plugin_spec_digest)}}};
			for (const auto& [column, cell] : toolchain.cells)
				if (auto valid = cell.validate(); !valid)
					return unexpected(unavailable(column,
												  valid.error().code + ":" + valid.error().field +
													  ":" + valid.error().detail));
			auto toolchain_id = derive_domain_identity(
				build::relations::toolchain_context::descriptor(), toolchain);
			if (!toolchain_id)
				return unexpected(unavailable("build.toolchain_context",
											  toolchain_id.error().code + ":" +
												  toolchain_id.error().field + ":" +
												  toolchain_id.error().detail));
			draft.toolchain_context_id = *toolchain_id;

			detached_row variant{
				build::relations::variant::descriptor().id,
				{{"build.variant.v1.variant",
				  detached_cell::typed("build_variant_id", "identity:pending")},
				 {"build.variant.v1.project", detached_cell::typed("project_id", draft.project_id)},
				 {"build.variant.v1.toolchain",
				  detached_cell::typed("toolchain_context_id", draft.toolchain_context_id)},
				 {"build.variant.v1.language",
				  symbol_cell("build.language/1", draft.variant.language)},
				 {"build.variant.v1.language_standard",
				  symbol_cell("build.language-standard/1", draft.variant.language_standard)},
				 {"build.variant.v1.target_triple",
				  detached_cell::utf8(draft.variant.target_triple)},
				 {"build.variant.v1.predefined_macros_digest",
				  digest_cell(draft.variant.predefined_macros_digest)},
				 {"build.variant.v1.include_search_digest",
				  digest_cell(draft.variant.include_search_digest)},
				 {"build.variant.v1.semantic_flags_digest",
				  digest_cell(draft.variant.semantic_flags_digest)}}};
			auto variant_id =
				derive_domain_identity(build::relations::variant::descriptor(), variant);
			if (!variant_id)
				return unexpected(unavailable("build.variant",
											  variant_id.error().code + ":" +
												  variant_id.error().field + ":" +
												  variant_id.error().detail));
			draft.build_variant_id = *variant_id;

			detached_row compile_unit{
				build::relations::compile_unit::descriptor().id,
				{{"build.compile_unit.v1.compile_unit",
				  detached_cell::typed("compile_unit_id", "identity:pending")},
				 {"build.compile_unit.v1.project",
				  detached_cell::typed("project_id", draft.project_id)},
				 {"build.compile_unit.v1.main_source",
				  detached_cell::typed("source_snapshot_id", draft.source.source_snapshot_id)},
				 {"build.compile_unit.v1.variant",
				  detached_cell::typed("build_variant_id", draft.build_variant_id)},
				 {"build.compile_unit.v1.toolchain",
				  detached_cell::typed("toolchain_context_id", draft.toolchain_context_id)},
				 {"build.compile_unit.v1.effective_invocation_digest",
				  digest_cell(draft.invocation.effective_invocation_digest)},
				 {"build.compile_unit.v1.language",
				  symbol_cell("build.language/1", draft.invocation.language)},
				 {"build.compile_unit.v1.working_directory",
				  detached_cell::typed("logical_path_id",
									   draft.invocation.logical_working_directory)}}};
			auto compile_unit_id =
				derive_domain_identity(build::relations::compile_unit::descriptor(), compile_unit);
			if (!compile_unit_id)
				return unexpected(unavailable("build.compile_unit",
											  compile_unit_id.error().code + ":" +
												  compile_unit_id.error().field + ":" +
												  compile_unit_id.error().detail));
			draft.compile_unit_id = *compile_unit_id;
			return {};
		}
	} // namespace

	result<validated_build_capture>
	make_application_build_capture(const imported_project::implementation& project,
								   const replay_plan::implementation& plan,
								   const build_capture_limits limits)
	{
		if (!project.capture || project.capture_bundle_digest != project.capture->digest ||
			plan.capture_bundle_digest != project.capture_bundle_digest)
			return unexpected(unavailable("binding", "imported-project-replay-mismatch"));
		if (auto valid = project.catalog.validate(); !valid)
			return unexpected(unavailable("project_catalog", valid.error().code));

		const auto unit_index = std::ranges::find(project.capture->projection.compile_units,
												  plan.compile_unit_id,
												  &decoded_capture_unit::compile_unit_id);
		if (unit_index == project.capture->projection.compile_units.end())
			return unexpected(unavailable("compile_unit", "not-in-imported-capture"));
		const auto index = static_cast<std::size_t>(
			std::distance(project.capture->projection.compile_units.begin(), unit_index));
		const auto& unit = *unit_index;
		if (plan.source_closure_digest != unit.source_closure_digest ||
			plan.target_abi != project.capture->projection.target_abi)
			return unexpected(unavailable("replay_plan", "capture-binding-mismatch"));
		if (auto valid = resolve_compiler_replay_frontend(
				plan.analysis_frontend, plan.target_abi, plan.effective_arguments);
			!valid)
			return unexpected(unavailable("replay_plan", valid.error().detail));
		const auto closure = std::ranges::find(project.capture->projection.source_closures,
											   unit.source_closure_id,
											   &decoded_capture_source_closure::id);
		if (closure == project.capture->projection.source_closures.end() ||
			closure->digest != unit.source_closure_digest || closure->members.empty())
			return unexpected(unavailable("source_closure", "binding-mismatch"));
		const auto source =
			std::ranges::find_if(closure->members,
								 [&](const decoded_capture_source_member& value)
								 {
									 return value.file_id == unit.source_file_id &&
										 value.content_digest == unit.source_content_digest &&
										 value.role == "main";
								 });
		if (source == closure->members.end())
			return unexpected(unavailable("source", "main-member-mismatch"));

		const auto prefix = "compile_units[" + std::to_string(index) + "].";
		auto source_snapshot = require_exact(
			unit.source_snapshot_id, project.capture->gaps, prefix + "source_snapshot");
		auto encoding = require_exact(source->encoding, project.capture->gaps, prefix + "encoding");
		auto language_standard = require_exact(
			unit.language_standard, project.capture->gaps, prefix + "language_standard");
		auto builtin_headers = require_exact(project.capture->projection.builtin_headers_digest,
											 project.capture->gaps,
											 "production_toolchain.builtin_headers_digest");
		auto abi = require_exact(project.capture->projection.abi_digest,
								 project.capture->gaps,
								 "production_toolchain.abi_digest");
		auto builtin_macros = require_exact(project.capture->projection.builtin_macros_digest,
											project.capture->gaps,
											"production_toolchain.builtin_macros_digest");
		auto include_search = require_exact(project.capture->projection.include_search_digest,
											project.capture->gaps,
											"production_toolchain.include_search_digest");
		if (!source_snapshot || !encoding || !language_standard || !builtin_headers || !abi ||
			!builtin_macros || !include_search)
			return unexpected(!source_snapshot		   ? std::move(source_snapshot.error())
								  : !encoding		   ? std::move(encoding.error())
								  : !language_standard ? std::move(language_standard.error())
								  : !builtin_headers   ? std::move(builtin_headers.error())
								  : !abi			   ? std::move(abi.error())
								  : !builtin_macros	   ? std::move(builtin_macros.error())
													   : std::move(include_search.error()));

		const auto catalog_unit = std::ranges::find(project.catalog.compile_units,
													plan.compile_unit_id,
													&catalog_compile_unit::compile_unit_id);
		if (catalog_unit == project.catalog.compile_units.end() ||
			catalog_unit->source_digest != unit.source_content_digest)
			return unexpected(unavailable("project_catalog", "compile-unit-binding-mismatch"));

		auto replay_frontend_digest =
			semantic_tuple("application-analysis-replay-frontend.v1",
						   {canonical_value::from_string(plan.analysis_frontend)});
		auto toolchain_digest = semantic_tuple(
			"application-analysis-toolchain.v1",
			{canonical_value::from_string(project.capture->projection.toolchain_family),
			 canonical_value::from_string(project.capture->projection.toolchain_version),
			 canonical_value::from_string(project.capture->projection.target_triple),
			 canonical_value::from_string(*builtin_headers),
			 canonical_value::from_string(*builtin_macros),
			 canonical_value::from_string(*include_search),
			 canonical_value::from_string(*abi)});
		if (!replay_frontend_digest || !toolchain_digest)
			return unexpected(!replay_frontend_digest ? std::move(replay_frontend_digest.error())
													  : std::move(toolchain_digest.error()));

		std::vector<normalized_build_option> options;
		options.reserve(plan.effective_arguments.size());
		for (const auto& mapping : plan.option_mappings)
		{
			if (mapping.replay_tokens.empty())
				options.push_back({mapping.production_token,
								   option_class(mapping.fidelity),
								   mapping.reason,
								   mapping.completion_action});
			else
				for (const auto& token : mapping.replay_tokens)
					options.push_back({token,
									   option_class(mapping.fidelity),
									   mapping.reason,
									   mapping.completion_action});
		}
		auto flags_digest = semantic_flags_digest(options);
		if (!flags_digest)
			return unexpected(std::move(flags_digest.error()));

		std::vector<build_capture_environment_effect> environment;
		environment.reserve(unit.environment_effects.size());
		for (const auto& effect : unit.environment_effects)
			environment.push_back({effect.name, captured_string(effect)});

		build_capture_draft draft;
		draft.project_id = project.capture->project_id;
		draft.catalog = project.catalog;
		draft.selected_catalog_compile_unit_id = plan.compile_unit_id;
		draft.compile_unit_id = plan.compile_unit_id;
		draft.toolchain_digest = std::move(*toolchain_digest);
		auto toolchain_context = canonical_identity_digest(
			"toolchain-context",
			std::array{canonical_value::from_string(draft.toolchain_digest),
					   canonical_value::from_string(plan.analysis_frontend)});
		auto variant = canonical_identity_digest(
			"build-variant",
			std::array{canonical_value::from_string(unit.language),
					   canonical_value::from_string(*language_standard),
					   canonical_value::from_string(project.capture->projection.target_triple),
					   canonical_value::from_string(*builtin_macros),
					   canonical_value::from_string(*include_search),
					   canonical_value::from_string(*flags_digest)});
		if (!toolchain_context || !variant)
			return unexpected(!toolchain_context ? std::move(toolchain_context.error())
												 : std::move(variant.error()));
		draft.toolchain_context_id = std::move(*toolchain_context);
		draft.build_variant_id = std::move(*variant);
		draft.toolchain = {
			project.capture->projection.toolchain_family,
			project.capture->projection.toolchain_version,
			project.capture->projection.target_triple,
			std::move(*builtin_headers),
			project.capture->projection.sysroot,
			std::move(*abi),
			std::move(*replay_frontend_digest),
			project.capture->projection.production_compiler_path
				? captured_value<std::string>::observed(
					  *project.capture->projection.production_compiler_path)
				: missing_value<std::string>(project.capture->gaps,
											 "production_toolchain.compiler_path",
											 "production-compiler-path-unavailable",
											 "recapture-with-shell-free-wrapper"),
			project.capture->projection.production_compiler_binary_digest
				? captured_value<std::string>::observed(
					  *project.capture->projection.production_compiler_binary_digest)
				: missing_value<std::string>(project.capture->gaps,
											 "production_toolchain.compiler_binary_digest",
											 "production-compiler-digest-unavailable",
											 "recapture-with-shell-free-wrapper")};
		draft.variant = {unit.language,
						 std::move(*language_standard),
						 project.capture->projection.target_triple,
						 std::move(*builtin_macros),
						 std::move(*include_search),
						 std::move(*flags_digest)};

		draft.invocation.original_arguments = unit.original_arguments
			? captured_value<std::vector<std::string>>::observed(*unit.original_arguments)
			: missing_value<std::vector<std::string>>(project.capture->gaps,
													  prefix + "original_argv",
													  "original-argv-unavailable",
													  "recapture-with-shell-free-wrapper");
		draft.invocation.normalized_semantic_options =
			captured_value<std::vector<normalized_build_option>>::derived(std::move(options));
		draft.invocation.effective_replay_arguments =
			captured_value<std::vector<std::string>>::derived(plan.effective_arguments);
		draft.invocation.response_files =
			auxiliary_files(unit.response_files, project.capture->gaps, prefix + "response_files");
		draft.invocation.config_files =
			auxiliary_files(unit.config_files, project.capture->gaps, prefix + "config_files");
		draft.invocation.environment_effects =
			captured_value<std::vector<build_capture_environment_effect>>::observed(
				std::move(environment));
		draft.invocation.effective_invocation_digest = catalog_unit->effective_invocation_digest;
		draft.invocation.environment_digest = catalog_unit->environment_digest;
		draft.invocation.language = unit.language;
		draft.invocation.logical_working_directory = unit.logical_working_directory;
		for (const auto& mapping : project.capture->projection.path_mappings)
			if (!mapping.physical_prefix.empty() && mapping.physical_prefix.front() == '/')
				draft.invocation.qualified_read_roots.push_back(mapping.physical_prefix);
		if (project.capture->projection.sysroot &&
			project.capture->projection.sysroot->starts_with('/'))
			draft.invocation.qualified_read_roots.push_back(*project.capture->projection.sysroot);
		std::ranges::sort(draft.invocation.qualified_read_roots);
		draft.invocation.qualified_read_roots.erase(
			std::ranges::unique(draft.invocation.qualified_read_roots).begin(),
			draft.invocation.qualified_read_roots.end());

		auto line_index = canonical_identity_digest(
			"line-index",
			std::array{canonical_value::from_string(*source_snapshot),
					   canonical_value::from_string(unit.source_content_digest),
					   canonical_value::from_string(*encoding)});
		if (!line_index)
			return unexpected(std::move(line_index.error()));
		draft.source = {std::move(*source_snapshot),
						unit.source_file_id,
						unit.source_logical_path,
						unit.source_content_digest,
						unit.source_size_bytes,
						std::move(*encoding),
						std::move(*line_index),
						source->read_only};
		draft.source_closure = {closure->id,
								closure->digest,
								closure->manifest_digest,
								closure->member_count,
								closure->blob_count,
								closure->unique_blob_bytes};
		if (auto bound = bind_relation_identities(draft); !bound)
			return unexpected(std::move(bound.error()));
		return validate_build_capture(std::move(draft), limits);
	}
} // namespace cxxlens::sdk::detail
