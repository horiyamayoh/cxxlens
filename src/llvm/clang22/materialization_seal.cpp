#include "materialization_seal.hpp"

#include <array>
#include <map>
#include <set>
#include <string>
#include <utility>

#include <cxxlens/relations/build_compile_unit.hpp>
#include <cxxlens/relations/build_project.hpp>
#include <cxxlens/relations/build_toolchain_context.hpp>
#include <cxxlens/relations/build_variant.hpp>
#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_entity.hpp>
#include <cxxlens/relations/source_file.hpp>
#include <cxxlens/relations/source_span.hpp>

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		constexpr std::string_view atomic_output_group_id = "clang22-atomic";

		[[nodiscard]] sdk::error
		seal_error(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] std::string nested_error(const sdk::error& error)
		{
			std::string output = error.code;
			if (!error.field.empty())
				output += ":" + error.field;
			if (!error.detail.empty())
				output += ":" + error.detail;
			return output;
		}

		[[nodiscard]] std::array<const sdk::relation_descriptor*, 6U> output_descriptors()
		{
			return {
				&cc::relations::call_direct_target::descriptor(),
				&cc::relations::call_site::descriptor(),
				&cc::relations::entity::descriptor(),
				&call_observation_v2_descriptor(),
				&entity_observation_v2_descriptor(),
				&type_observation_v2_descriptor(),
			};
		}

		[[nodiscard]] sdk::detached_cell string_cell(const sdk::value_type& type, std::string value)
		{
			return {
				type, sdk::cell_state::present, sdk::scalar_value{std::move(value)}, std::nullopt};
		}

		[[nodiscard]] sdk::result<void> add_string(sdk::detached_row& row,
												   const sdk::relation_descriptor& descriptor,
												   const std::string_view column_name,
												   std::string value)
		{
			auto column = descriptor.column(column_name);
			if (!column)
				return sdk::unexpected(seal_error("materialization.descriptor-binding-mismatch",
												  descriptor.id,
												  nested_error(column.error())));
			row.cells.emplace(column->id, string_cell(column->type, std::move(value)));
			return {};
		}

		[[nodiscard]] sdk::result<void> add_unsigned(sdk::detached_row& row,
													 const sdk::relation_descriptor& descriptor,
													 const std::string_view column_name,
													 const std::uint64_t value)
		{
			auto column = descriptor.column(column_name);
			if (!column)
				return sdk::unexpected(seal_error("materialization.descriptor-binding-mismatch",
												  descriptor.id,
												  nested_error(column.error())));
			auto cell = sdk::detached_cell::unsigned_integer(value);
			if (cell.type != column->type)
				return sdk::unexpected(seal_error(
					"materialization.descriptor-binding-mismatch", column->id, "uint64-type"));
			row.cells.emplace(column->id, std::move(cell));
			return {};
		}

		[[nodiscard]] sdk::result<void> add_boolean(sdk::detached_row& row,
													const sdk::relation_descriptor& descriptor,
													const std::string_view column_name,
													const bool value)
		{
			auto column = descriptor.column(column_name);
			if (!column)
				return sdk::unexpected(seal_error("materialization.descriptor-binding-mismatch",
												  descriptor.id,
												  nested_error(column.error())));
			auto cell = sdk::detached_cell::boolean(value);
			if (cell.type != column->type)
				return sdk::unexpected(seal_error(
					"materialization.descriptor-binding-mismatch", column->id, "bool-type"));
			row.cells.emplace(column->id, std::move(cell));
			return {};
		}

		[[nodiscard]] sdk::result<void> add_absent(sdk::detached_row& row,
												   const sdk::relation_descriptor& descriptor,
												   const std::string_view column_name)
		{
			auto column = descriptor.column(column_name);
			if (!column)
				return sdk::unexpected(seal_error("materialization.descriptor-binding-mismatch",
												  descriptor.id,
												  nested_error(column.error())));
			row.cells.emplace(column->id, sdk::detached_cell::absent(column->type));
			return {};
		}

		[[nodiscard]] sdk::result<sdk::detached_row>
		finish_identity_row(const sdk::relation_descriptor& descriptor,
							std::string expected_identity,
							sdk::detached_row row)
		{
			if (!descriptor.domain_identity.result_column)
				return sdk::unexpected(seal_error("materialization.descriptor-binding-mismatch",
												  descriptor.id,
												  "missing-result-column"));
			auto result_column = descriptor.column(*descriptor.domain_identity.result_column);
			if (!result_column)
				return sdk::unexpected(seal_error("materialization.descriptor-binding-mismatch",
												  descriptor.id,
												  nested_error(result_column.error())));
			row.cells.emplace(result_column->id, string_cell(result_column->type, "pending"));
			auto derived = sdk::derive_domain_identity(descriptor, row);
			if (!derived)
				return sdk::unexpected(seal_error("materialization.identity-mismatch",
												  descriptor.id,
												  nested_error(derived.error())));
			if (*derived != expected_identity)
				return sdk::unexpected(seal_error(
					"materialization.identity-mismatch", result_column->id, "task-authority"));
			row.cells.at(result_column->id) =
				string_cell(result_column->type, std::move(expected_identity));
			if (auto valid = sdk::validate_row(descriptor, row); !valid)
				return sdk::unexpected(seal_error("materialization.identity-mismatch",
												  descriptor.id,
												  nested_error(valid.error())));
			if (auto valid = sdk::validate_domain_identity(descriptor, row); !valid)
				return sdk::unexpected(seal_error("materialization.identity-mismatch",
												  descriptor.id,
												  nested_error(valid.error())));
			return row;
		}

		template <class Function>
		[[nodiscard]] sdk::result<sdk::detached_row>
		make_identity_row(const sdk::relation_descriptor& descriptor,
						  std::string expected_identity,
						  Function&& populate)
		{
			sdk::detached_row row{descriptor.id, {}};
			if (auto populated = std::forward<Function>(populate)(row); !populated)
				return sdk::unexpected(std::move(populated.error()));
			return finish_identity_row(descriptor, std::move(expected_identity), std::move(row));
		}

		[[nodiscard]] sdk::result<std::vector<sdk::detached_row>>
		base_claim_rows(const clang22_task_input& input)
		{
			std::vector<sdk::detached_row> rows;
			rows.reserve(5U);

			const auto& project = build::relations::project::descriptor();
			auto project_row = make_identity_row(
				project,
				input.project,
				[&](sdk::detached_row& row) -> sdk::result<void>
				{
					for (auto value : {
							 std::pair{std::string_view{"catalog"},
									   std::string_view{input.project_catalog.catalog_id}},
							 std::pair{std::string_view{"catalog_digest"},
									   std::string_view{input.project_catalog.catalog_digest}},
							 std::pair{std::string_view{"logical_root"},
									   std::string_view{input.project_catalog.logical_root}},
							 std::pair{std::string_view{"environment_digest"},
									   std::string_view{input.project_catalog.environment_digest}},
						 })
						if (auto added =
								add_string(row, project, value.first, std::string{value.second});
							!added)
							return added;
					return {};
				});
			if (!project_row)
				return sdk::unexpected(std::move(project_row.error()));
			rows.push_back(std::move(*project_row));

			const auto& toolchain = build::relations::toolchain_context::descriptor();
			auto toolchain_row = make_identity_row(
				toolchain,
				input.toolchain_context,
				[&](sdk::detached_row& row) -> sdk::result<void>
				{
					for (auto value : {
							 std::pair{std::string_view{"family"},
									   std::string_view{input.toolchain.family}},
							 std::pair{std::string_view{"exact_version"},
									   std::string_view{input.toolchain.exact_version}},
							 std::pair{std::string_view{"target_triple"},
									   std::string_view{input.toolchain.target_triple}},
							 std::pair{std::string_view{"builtin_headers_digest"},
									   std::string_view{input.toolchain.builtin_headers_digest}},
							 std::pair{std::string_view{"abi_digest"},
									   std::string_view{input.toolchain.abi_digest}},
							 std::pair{std::string_view{"plugin_spec_digest"},
									   std::string_view{input.toolchain.plugin_spec_digest}},
						 })
						if (auto added =
								add_string(row, toolchain, value.first, std::string{value.second});
							!added)
							return added;
					if (input.toolchain.sysroot)
						return add_string(row, toolchain, "sysroot", *input.toolchain.sysroot);
					return add_absent(row, toolchain, "sysroot");
				});
			if (!toolchain_row)
				return sdk::unexpected(std::move(toolchain_row.error()));
			rows.push_back(std::move(*toolchain_row));

			const auto& variant = build::relations::variant::descriptor();
			auto variant_row = make_identity_row(
				variant,
				input.variant,
				[&](sdk::detached_row& row) -> sdk::result<void>
				{
					for (auto value : {
							 std::pair{std::string_view{"project"},
									   std::string_view{input.project}},
							 std::pair{std::string_view{"toolchain"},
									   std::string_view{input.toolchain_context}},
							 std::pair{std::string_view{"language"},
									   std::string_view{input.variant_authority.language}},
							 std::pair{std::string_view{"language_standard"},
									   std::string_view{input.variant_authority.language_standard}},
							 std::pair{std::string_view{"target_triple"},
									   std::string_view{input.variant_authority.target_triple}},
							 std::pair{std::string_view{"predefined_macros_digest"},
									   std::string_view{
										   input.variant_authority.predefined_macros_digest}},
							 std::pair{
								 std::string_view{"include_search_digest"},
								 std::string_view{input.variant_authority.include_search_digest}},
							 std::pair{
								 std::string_view{"semantic_flags_digest"},
								 std::string_view{input.variant_authority.semantic_flags_digest}},
						 })
						if (auto added =
								add_string(row, variant, value.first, std::string{value.second});
							!added)
							return added;
					return {};
				});
			if (!variant_row)
				return sdk::unexpected(std::move(variant_row.error()));
			rows.push_back(std::move(*variant_row));

			const auto& source_descriptor = cxxlens::source::relations::file::descriptor();
			auto source_row = make_identity_row(
				source_descriptor,
				input.source_snapshot,
				[&](sdk::detached_row& row) -> sdk::result<void>
				{
					for (auto value : {
							 std::pair{std::string_view{"file"}, std::string_view{input.file}},
							 std::pair{std::string_view{"project"},
									   std::string_view{input.project}},
							 std::pair{std::string_view{"logical_path"},
									   std::string_view{input.logical_path}},
							 std::pair{std::string_view{"content"},
									   std::string_view{input.source_content_digest}},
							 std::pair{std::string_view{"encoding"},
									   std::string_view{input.source_encoding}},
							 std::pair{std::string_view{"line_index"},
									   std::string_view{input.line_index}},
						 })
						if (auto added = add_string(
								row, source_descriptor, value.first, std::string{value.second});
							!added)
							return added;
					if (auto added =
							add_unsigned(row, source_descriptor, "size", input.source_size_bytes);
						!added)
						return added;
					return add_boolean(row, source_descriptor, "read_only", input.source_read_only);
				});
			if (!source_row)
				return sdk::unexpected(std::move(source_row.error()));
			rows.push_back(std::move(*source_row));

			const auto& compile_unit = build::relations::compile_unit::descriptor();
			auto compile_unit_row = make_identity_row(
				compile_unit,
				input.compile_unit,
				[&](sdk::detached_row& row) -> sdk::result<void>
				{
					for (auto value : {
							 std::pair{std::string_view{"project"},
									   std::string_view{input.project}},
							 std::pair{std::string_view{"main_source"},
									   std::string_view{input.source_snapshot}},
							 std::pair{std::string_view{"variant"},
									   std::string_view{input.variant}},
							 std::pair{std::string_view{"toolchain"},
									   std::string_view{input.toolchain_context}},
							 std::pair{std::string_view{"effective_invocation_digest"},
									   std::string_view{input.normalized_invocation_digest}},
							 std::pair{std::string_view{"language"},
									   std::string_view{input.language}},
							 std::pair{std::string_view{"working_directory"},
									   std::string_view{input.working_directory}},
						 })
						if (auto added = add_string(
								row, compile_unit, value.first, std::string{value.second});
							!added)
							return added;
					return {};
				});
			if (!compile_unit_row)
				return sdk::unexpected(std::move(compile_unit_row.error()));
			rows.push_back(std::move(*compile_unit_row));
			return rows;
		}

		[[nodiscard]] sdk::result<sdk::detached_row>
		source_span_row(const observation_v2_primary_span& span)
		{
			const auto& descriptor = source::relations::span::descriptor();
			return make_identity_row(
				descriptor,
				span.span_id,
				[&](sdk::detached_row& row) -> sdk::result<void>
				{
					for (auto value : {
							 std::pair{std::string_view{"snapshot"},
									   std::string_view{span.snapshot}},
							 std::pair{std::string_view{"file"}, std::string_view{span.file}},
							 std::pair{std::string_view{"role"}, std::string_view{span.role}},
						 })
						if (auto added =
								add_string(row, descriptor, value.first, std::string{value.second});
							!added)
							return added;
					if (auto added = add_unsigned(row, descriptor, "begin", span.begin); !added)
						return added;
					if (auto added = add_unsigned(row, descriptor, "end", span.end); !added)
						return added;
					if (auto added = add_absent(row, descriptor, "origin"); !added)
						return added;
					return add_boolean(row, descriptor, "read_only", span.read_only);
				});
		}

		[[nodiscard]] sdk::result<std::string> required_string(const sdk::detached_row& row,
															   const std::string_view column_id)
		{
			const auto found = row.cells.find(column_id);
			if (found == row.cells.end() || found->second.state != sdk::cell_state::present ||
				!found->second.value)
				return sdk::unexpected(
					seal_error("materialization.claim-invalid", std::string{column_id}, "absent"));
			const auto* value = std::get_if<std::string>(&*found->second.value);
			if (value == nullptr)
				return sdk::unexpected(
					seal_error("materialization.claim-invalid", std::string{column_id}, "type"));
			return *value;
		}

		[[nodiscard]] sdk::result<void> validate_canonical_hard_references(
			const std::span<const sdk::provider::detail::sealed_provider_batch> batches,
			const clang22_task_input& input,
			const std::map<std::string, observation_v2_primary_span, std::less<>>& spans)
		{
			std::set<std::string, std::less<>> calls;
			for (const auto& row : batches[1U].rows())
			{
				auto compile_unit = required_string(row, "cc.call_site.v1.compile_unit");
				auto source = required_string(row, "cc.call_site.v1.source");
				auto call = required_string(row, "cc.call_site.v1.call");
				if (!compile_unit || !source || !call)
					return sdk::unexpected(!compile_unit ? std::move(compile_unit.error())
											   : !source ? std::move(source.error())
														 : std::move(call.error()));
				if (*compile_unit != input.compile_unit)
					return sdk::unexpected(seal_error("materialization.task-binding-mismatch",
													  "cc.call_site.v1.compile_unit",
													  "final-relation-compile-unit"));
				if (!spans.contains(*source))
					return sdk::unexpected(seal_error("materialization.span-invalid",
													  "cc.call_site.v1.source",
													  "missing-source-span"));
				calls.insert(std::move(*call));
			}
			for (const auto& row : batches[0U].rows())
			{
				auto call = required_string(row, "cc.call_direct_target.v1.call");
				if (!call)
					return sdk::unexpected(std::move(call.error()));
				if (!calls.contains(*call))
					return sdk::unexpected(seal_error("materialization.claim-invalid",
													  "cc.call_direct_target.v1.call",
													  "missing-call-site"));
			}
			return {};
		}
	} // namespace

	sealed_materialization_result::sealed_materialization_result(
		std::string provider_task_id,
		std::string task_input_digest,
		std::string provider_execution_id,
		std::string selected_catalog_compile_unit_id,
		std::string final_relation_compile_unit_id,
		sdk::provider::detail::sealed_provider_transcript provider_seal,
		std::vector<sdk::detached_row> base_claim_rows,
		std::vector<sdk::detached_row> source_span_claim_rows,
		std::vector<sealed_observation_v2_row> observation_rows)
		: provider_task_id_{std::move(provider_task_id)},
		  task_input_digest_{std::move(task_input_digest)},
		  provider_execution_id_{std::move(provider_execution_id)},
		  selected_catalog_compile_unit_id_{std::move(selected_catalog_compile_unit_id)},
		  final_relation_compile_unit_id_{std::move(final_relation_compile_unit_id)},
		  provider_seal_{std::move(provider_seal)}, base_claim_rows_{std::move(base_claim_rows)},
		  source_span_claim_rows_{std::move(source_span_claim_rows)},
		  observation_rows_{std::move(observation_rows)}
	{
	}

	std::string_view sealed_materialization_result::provider_task_id() const noexcept
	{
		return provider_task_id_;
	}

	std::string_view sealed_materialization_result::task_input_digest() const noexcept
	{
		return task_input_digest_;
	}

	std::string_view sealed_materialization_result::provider_execution_id() const noexcept
	{
		return provider_execution_id_;
	}

	std::string_view
	sealed_materialization_result::selected_catalog_compile_unit_id() const noexcept
	{
		return selected_catalog_compile_unit_id_;
	}

	std::string_view sealed_materialization_result::final_relation_compile_unit_id() const noexcept
	{
		return final_relation_compile_unit_id_;
	}

	const sdk::provider::detail::sealed_provider_transcript&
	sealed_materialization_result::provider_seal() const noexcept
	{
		return provider_seal_;
	}

	std::span<const sdk::detached_row>
	sealed_materialization_result::base_claim_rows() const noexcept
	{
		return base_claim_rows_;
	}

	std::span<const sdk::detached_row>
	sealed_materialization_result::source_span_claim_rows() const noexcept
	{
		return source_span_claim_rows_;
	}

	std::span<const sealed_observation_v2_row>
	sealed_materialization_result::observation_rows() const noexcept
	{
		return observation_rows_;
	}

	sdk::result<sealed_materialization_result> validate_and_seal_materialization(
		const validated_task_request& request,
		sdk::provider::detail::sealed_provider_transcript&& provider_seal)
	{
		if (auto valid = request.worker_input.validate(); !valid)
			return sdk::unexpected(seal_error(
				"materialization.task-binding-mismatch", "task.v3", nested_error(valid.error())));
		if (!sdk::validate_strong_id(request.provider_task_id) ||
			!sdk::validate_strong_id(request.provider_execution_id))
			return sdk::unexpected(
				seal_error("materialization.task-binding-mismatch", "execution-key", "strong-id"));
		auto encoded = encode_task_input(request.worker_input);
		if (!encoded || *encoded != request.worker_payload ||
			sdk::content_digest(request.worker_payload) != request.task_input_digest)
			return sdk::unexpected(
				seal_error("materialization.task-binding-mismatch",
						   "task_input_digest",
						   encoded ? "payload-or-digest" : nested_error(encoded.error())));
		if (auto valid = request.sandbox.validate(); !valid)
			return sdk::unexpected(seal_error(
				"materialization.task-binding-mismatch", "sandbox", nested_error(valid.error())));
		const auto expected_minimum = request.worker_input.sandbox.minimum == "certified"
			? sdk::provider::sandbox_assurance::certified
			: sdk::provider::sandbox_assurance::enforced;
		if (request.sandbox.minimum != expected_minimum ||
			request.sandbox.policy_digest != request.worker_input.sandbox.policy_digest)
			return sdk::unexpected(
				seal_error("materialization.task-binding-mismatch", "sandbox", "task-authority"));

		const auto expected_descriptors = output_descriptors();
		const auto batches = provider_seal.batches();
		if (batches.size() != expected_descriptors.size())
			return sdk::unexpected(
				seal_error("materialization.group-incomplete", "batches", "exact-six"));
		for (std::size_t index{}; index < expected_descriptors.size(); ++index)
		{
			const auto& expected = *expected_descriptors[index];
			const auto& batch = batches[index];
			if (batch.descriptor_id() != expected.id)
				return sdk::unexpected(
					seal_error("materialization.group-incomplete", "batches", "descriptor-order"));
			if (batch.descriptor_digest() != expected.descriptor_digest)
				return sdk::unexpected(seal_error(
					"materialization.descriptor-binding-mismatch", expected.id, "runtime-digest"));
			const std::string_view expected_group = index < 3U ? "canonical" : "observation";
			if (batch.task_id() != request.provider_task_id)
				return sdk::unexpected(seal_error(
					"materialization.task-binding-mismatch", expected.id, "provider-task-id"));
			if (batch.dependency_group_id() != expected_group ||
				batch.atomic_output_group_id() != atomic_output_group_id ||
				batch.batch_id() != expected.id + "-batch")
				return sdk::unexpected(seal_error(
					"materialization.group-incomplete", expected.id, "group-or-batch-binding"));
			for (const auto& row : batch.rows())
			{
				if (auto valid = sdk::validate_row(expected, row); !valid)
					return sdk::unexpected(seal_error(
						"materialization.claim-invalid", expected.id, nested_error(valid.error())));
				if (expected.domain_identity.result_column)
					if (auto valid = sdk::validate_domain_identity(expected, row); !valid)
						return sdk::unexpected(seal_error("materialization.claim-invalid",
														  expected.id,
														  nested_error(valid.error())));
			}
		}

		const observation_v2_task_authority task_authority{
			.final_relation_compile_unit_id = request.worker_input.compile_unit,
			.source_snapshot_id = request.worker_input.source_snapshot,
			.source_file_id = request.worker_input.file,
			.source_size_bytes = request.worker_input.source_size_bytes,
		};
		std::vector<sealed_observation_v2_row> observations;
		std::map<std::string, observation_v2_primary_span, std::less<>> spans;
		for (std::size_t batch_index = 3U; batch_index < batches.size(); ++batch_index)
		{
			const auto rows = batches[batch_index].rows();
			for (std::size_t row_index{}; row_index < rows.size(); ++row_index)
			{
				auto decoded = decode_observation_v2_row(rows[row_index], task_authority);
				if (!decoded)
				{
					if (decoded.error().field == "final_relation_compile_unit_id")
						return sdk::unexpected(seal_error("materialization.task-binding-mismatch",
														  "final_relation_compile_unit_id",
														  decoded.error().detail));
					return sdk::unexpected(std::move(decoded.error()));
				}
				if (decoded->primary_span)
				{
					auto [found, inserted] =
						spans.emplace(decoded->primary_span->span_id, *decoded->primary_span);
					if (!inserted && found->second != *decoded->primary_span)
						return sdk::unexpected(seal_error("materialization.span-invalid",
														  "source.span.v1.span",
														  "conflicting-full-bundle"));
				}
				observations.push_back({batch_index, row_index, std::move(*decoded)});
			}
		}

		if (auto valid = validate_canonical_hard_references(batches, request.worker_input, spans);
			!valid)
			return sdk::unexpected(std::move(valid.error()));
		auto base_rows = base_claim_rows(request.worker_input);
		if (!base_rows)
			return sdk::unexpected(std::move(base_rows.error()));
		std::vector<sdk::detached_row> span_rows;
		span_rows.reserve(spans.size());
		for (const auto& [span_id, span] : spans)
		{
			(void)span_id;
			auto row = source_span_row(span);
			if (!row)
				return sdk::unexpected(std::move(row.error()));
			span_rows.push_back(std::move(*row));
		}

		return sealed_materialization_result{
			request.provider_task_id,
			request.task_input_digest,
			request.provider_execution_id,
			request.worker_input.selected_catalog_compile_unit,
			request.worker_input.compile_unit,
			std::move(provider_seal),
			std::move(*base_rows),
			std::move(span_rows),
			std::move(observations),
		};
	}
} // namespace cxxlens::detail::clang22::materialization
