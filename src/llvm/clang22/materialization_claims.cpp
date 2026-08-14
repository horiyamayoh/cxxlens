#include "materialization_claims.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "materialization_admission_error.hpp"
#include "materialization_identity.hpp"
#include "sdk/claim_internal.hpp"

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		constexpr std::array<std::string_view, 5U> authority_paths{
			"schemas/cxxlens_ng_clang22_materialization_contract.schema.yaml",
			"schemas/cxxlens_ng_clang22_materialization_contract.yaml",
			"schemas/cxxlens_ng_clang22_materialization_report.schema.yaml",
			"schemas/cxxlens_ng_clang22_materialization_request.schema.yaml",
			"schemas/cxxlens_ng_relation_registry.yaml",
		};
		constexpr std::array<std::string_view, 6U> output_descriptor_ids{
			"cc.call_direct_target.v1",
			"cc.call_site.v1",
			"cc.entity.v1",
			"frontend.clang22.call_observation.v2",
			"frontend.clang22.entity_observation.v2",
			"frontend.clang22.type_observation.v2",
		};
		constexpr std::array<std::string_view, 6U> base_descriptor_ids{
			"build.project.v1",
			"build.toolchain_context.v1",
			"build.variant.v1",
			"source.file.v1",
			"build.compile_unit.v1",
			"source.span.v1",
		};
		constexpr std::string_view engine_generation_contract =
			"cxxlens.clang22-materialization-engine.v2";

		using context_key = std::array<std::string, 7U>;
		using partition_key = std::array<std::string, 8U>;
		constexpr std::array<std::string_view, 3U> v2_1_guarantee_modalities{
			"clang22.materialization-sealed.v1",
			"provider.transcript-sealed.v1",
			"sdk.claim-envelope-validated.v1"};

		struct direct_basis_values
		{
			std::string materializer_semantics;
			std::string direct_basis;
			std::string canonical_transform;
			std::string base_transform;
		};

		/** Request view shared by legacy adoption and the source-private v2.1 one-task adapter. */
		struct materialization_claim_request_view
		{
			const sdk::project_catalog& catalog;
			const sdk::relation_engine& engine;
			std::span<const validated_task_request> tasks;
			const json_value* document_root{};
		};

		struct final_occurrence
		{
			std::string claim_ref;
			materialization_semantic_task_context context;
			bool base{};
		};

		struct partition_accumulator
		{
			sdk::partition_draft draft;
			bool empty{};
			std::map<std::string, sdk::claim, std::less<>> claims_by_ref;
			std::set<std::string, std::less<>> claim_contents;
			std::map<std::string, sdk::snapshot_coverage_unit, std::less<>> coverage;
		};

		[[nodiscard]] sdk::error
		claim_error(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] std::string nested_error(const sdk::error& value)
		{
			return value.code + "/" + value.field + "/" + value.detail;
		}

		[[nodiscard]] bool lower_hex(const std::string_view value) noexcept
		{
			return std::ranges::all_of(value,
									   [](const char character)
									   {
										   return (character >= '0' && character <= '9') ||
											   (character >= 'a' && character <= 'f');
									   });
		}

		[[nodiscard]] bool content_digest(const std::string_view value) noexcept
		{
			constexpr std::string_view prefix{"sha256:"};
			return value.size() == prefix.size() + 64U && value.starts_with(prefix) &&
				lower_hex(value.substr(prefix.size()));
		}

		[[nodiscard]] bool revision(const std::string_view value) noexcept
		{
			return value.size() == 40U && lower_hex(value);
		}

		[[nodiscard]] bool sorted_unique(const std::vector<std::string>& values)
		{
			return std::ranges::is_sorted(values) &&
				std::ranges::adjacent_find(values) == values.end();
		}

		[[nodiscard]] sdk::canonical_value text(std::string value)
		{
			return sdk::canonical_value::from_string(std::move(value));
		}

		[[nodiscard]] sdk::canonical_value texts(const std::span<const std::string> values)
		{
			std::vector<sdk::canonical_value> output;
			output.reserve(values.size());
			for (const auto& value : values)
				output.push_back(text(value));
			return sdk::canonical_value::from_tuple(std::move(output));
		}

		[[nodiscard]] sdk::canonical_value
		object(std::vector<std::pair<std::string, sdk::canonical_value>> fields)
		{
			std::ranges::sort(fields, {}, &std::pair<std::string, sdk::canonical_value>::first);
			std::vector<sdk::canonical_value> output;
			output.reserve(fields.size());
			for (auto& [name, value] : fields)
				output.push_back(
					sdk::canonical_value::from_tuple({text(std::move(name)), std::move(value)}));
			return sdk::canonical_value::from_tuple(std::move(output));
		}

		[[nodiscard]] sdk::result<std::string>
		digest_projection(const std::string_view domain, const sdk::canonical_value& projection)
		{
			auto bytes = sdk::canonical_binary(projection);
			if (!bytes)
				return sdk::unexpected(std::move(bytes.error()));
			return sdk::semantic_digest(
				domain,
				std::string_view{reinterpret_cast<const char*>(bytes->data()), bytes->size()});
		}

		[[nodiscard]] sdk::result<std::string> identity(const std::string_view kind,
														std::vector<sdk::canonical_value> fields)
		{
			return sdk::canonical_identity_digest(kind, fields);
		}

		[[nodiscard]] std::vector<std::byte> byte_string(const std::string_view value)
		{
			const auto bytes = std::as_bytes(std::span{value.data(), value.size()});
			return {bytes.begin(), bytes.end()};
		}

		[[nodiscard]] std::string digest_text(const std::string_view value)
		{
			return sdk::content_digest(std::as_bytes(std::span{value.data(), value.size()}));
		}

		[[nodiscard]] sdk::result<std::string_view>
		json_text(const json_value& value, const std::string_view member, std::string field)
		{
			const auto* child = value.member(member);
			if (child == nullptr || child->as_string() == nullptr)
				return sdk::unexpected(
					claim_error("materialization.identity-mismatch", std::move(field), "string"));
			return std::string_view{*child->as_string()};
		}

		[[nodiscard]] sdk::result<std::int64_t>
		json_integer(const json_value& value, const std::string_view member, std::string field)
		{
			const auto* child = value.member(member);
			if (child == nullptr)
				return sdk::unexpected(
					claim_error("materialization.identity-mismatch", std::move(field), "integer"));
			if (const auto* signed_value = child->as_signed_integer())
				return *signed_value;
			if (const auto* unsigned_value = child->as_unsigned_integer();
				unsigned_value != nullptr &&
				*unsigned_value <=
					static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
				return static_cast<std::int64_t>(*unsigned_value);
			return sdk::unexpected(
				claim_error("materialization.identity-mismatch", std::move(field), "signed-int64"));
		}

		[[nodiscard]] context_key context_tuple(const materialization_semantic_task_context& value)
		{
			return {value.provider_task_id,
					value.task_input_digest,
					value.selected_catalog_compile_unit_id,
					value.compile_unit_id,
					value.condition_universe_id,
					value.condition_id,
					value.interpretation_domain};
		}

		[[nodiscard]] sdk::canonical_value
		context_tuple_value(const materialization_semantic_task_context& value)
		{
			const auto tuple = context_tuple(value);
			return texts(tuple);
		}

		[[nodiscard]] sdk::canonical_value
		context_object(const materialization_semantic_task_context& value)
		{
			return object({
				{"provider_task_id", text(value.provider_task_id)},
				{"task_input_digest", text(value.task_input_digest)},
				{"selected_catalog_compile_unit_id", text(value.selected_catalog_compile_unit_id)},
				{"compile_unit_id", text(value.compile_unit_id)},
				{"condition_universe_id", text(value.condition_universe_id)},
				{"condition_id", text(value.condition_id)},
				{"interpretation_domain", text(value.interpretation_domain)},
			});
		}

		[[nodiscard]] materialization_semantic_task_context
		task_context(const validated_task_request& value)
		{
			return {value.provider_task_id,
					value.task_input_digest,
					value.worker_input.selected_catalog_compile_unit,
					value.worker_input.compile_unit,
					value.worker_input.condition_universe,
					value.worker_input.condition,
					value.worker_input.interpretation};
		}

		[[nodiscard]] materialization_semantic_task_context
		task_context(const materialization_v2_1_task_metadata_receipt& value)
		{
			return {value.provider_task_id,
					value.task_input_digest,
					value.selected_catalog_compile_unit_id,
					value.final_relation_compile_unit_id,
					value.condition_universe_id,
					value.condition_id,
					value.interpretation_domain};
		}

		[[nodiscard]] bool same_catalog(const sdk::project_catalog& left,
										const sdk::project_catalog& right) noexcept
		{
			return left.catalog_id == right.catalog_id &&
				left.catalog_digest == right.catalog_digest &&
				left.logical_root == right.logical_root &&
				left.environment_digest == right.environment_digest &&
				left.compile_units == right.compile_units;
		}

		[[nodiscard]] bool same_task_input_binding(const clang22_task_input& left,
												   const clang22_task_input& right) noexcept
		{
			return same_catalog(left.project_catalog, right.project_catalog) &&
				left.selected_catalog_compile_unit == right.selected_catalog_compile_unit &&
				left.compile_unit == right.compile_unit && left.project == right.project &&
				left.variant == right.variant &&
				left.toolchain_context == right.toolchain_context &&
				left.toolchain_digest == right.toolchain_digest &&
				left.toolchain.family == right.toolchain.family &&
				left.toolchain.exact_version == right.toolchain.exact_version &&
				left.toolchain.target_triple == right.toolchain.target_triple &&
				left.toolchain.builtin_headers_digest == right.toolchain.builtin_headers_digest &&
				left.toolchain.sysroot == right.toolchain.sysroot &&
				left.toolchain.abi_digest == right.toolchain.abi_digest &&
				left.toolchain.plugin_spec_digest == right.toolchain.plugin_spec_digest &&
				left.variant_authority.language == right.variant_authority.language &&
				left.variant_authority.language_standard ==
				right.variant_authority.language_standard &&
				left.variant_authority.target_triple == right.variant_authority.target_triple &&
				left.variant_authority.predefined_macros_digest ==
				right.variant_authority.predefined_macros_digest &&
				left.variant_authority.include_search_digest ==
				right.variant_authority.include_search_digest &&
				left.variant_authority.semantic_flags_digest ==
				right.variant_authority.semantic_flags_digest &&
				left.normalized_invocation_digest == right.normalized_invocation_digest &&
				left.environment_digest == right.environment_digest &&
				left.language == right.language &&
				left.working_directory == right.working_directory &&
				left.condition_universe == right.condition_universe &&
				left.condition == right.condition && left.interpretation == right.interpretation &&
				left.source_snapshot == right.source_snapshot && left.file == right.file &&
				left.logical_path == right.logical_path &&
				left.source_content_digest == right.source_content_digest &&
				left.source_content_base64 == right.source_content_base64 &&
				left.source_size_bytes == right.source_size_bytes &&
				left.source_encoding == right.source_encoding &&
				left.line_index == right.line_index &&
				left.source_read_only == right.source_read_only && left.source == right.source &&
				left.arguments == right.arguments &&
				left.requested_descriptors == right.requested_descriptors &&
				left.dependency_groups == right.dependency_groups &&
				left.budget.wall_ms == right.budget.wall_ms &&
				left.budget.cpu_ms == right.budget.cpu_ms &&
				left.budget.address_space_bytes == right.budget.address_space_bytes &&
				left.budget.transport_bytes == right.budget.transport_bytes &&
				left.budget.output_bytes == right.budget.output_bytes &&
				left.budget.rows == right.budget.rows &&
				left.budget.diagnostics == right.budget.diagnostics &&
				left.budget.open_files == right.budget.open_files &&
				left.budget.subprocesses == right.budget.subprocesses &&
				left.sandbox.minimum == right.sandbox.minimum &&
				left.sandbox.policy_digest == right.sandbox.policy_digest;
		}

		[[nodiscard]] bool sandbox_minimum_matches(const sdk::provider::sandbox_assurance value,
												   const std::string_view expected) noexcept
		{
			return (value == sdk::provider::sandbox_assurance::enforced &&
					expected == "enforced") ||
				(value == sdk::provider::sandbox_assurance::certified && expected == "certified");
		}

		template <class Spool>
		[[nodiscard]] sdk::result<std::string> digest_sealed_task_spool(
			Spool& spool, const std::uint64_t maximum_size, const std::string_view field)
		{
			if (!spool.sealed() || spool.size_bytes() > maximum_size)
				return sdk::unexpected(claim_error(
					"materialization.task-binding-mismatch", std::string{field}, "spool"));
			auto digest = make_materialization_sha256_accumulator();
			if (!digest)
				return sdk::unexpected(materialization_admission_no_response());
			std::array<std::byte, default_stream_chunk_bytes> buffer{};
			std::uint64_t offset{};
			while (offset < spool.size_bytes())
			{
				const auto remaining = spool.size_bytes() - offset;
				const auto count = static_cast<std::size_t>(
					std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(buffer.size())));
				auto read = spool.read_at(offset, std::span{buffer}.first(count));
				if (!read)
					return sdk::unexpected(normalize_materialization_admission_spool_failure(
						std::move(read.error()), std::string{field}, "read"));
				if (*read == 0U || *read > count)
					return sdk::unexpected(claim_error(
						"materialization.task-binding-mismatch", std::string{field}, "read"));
				auto updated = digest->update(std::span{buffer}.first(*read));
				if (!updated)
					return sdk::unexpected(materialization_admission_io_failure(
						updated.error(), std::string{field}, "digest-update"));
				offset += static_cast<std::uint64_t>(*read);
			}
			auto finished = digest->finish();
			if (!finished)
				return sdk::unexpected(materialization_admission_io_failure(
					finished.error(), std::string{field}, "digest-finalize"));
			return std::move(*finished);
		}

		[[nodiscard]] sdk::result<void>
		validate_v2_1_task_window(const materialization_v2_1_claim_authority& authority,
								  const std::size_t task_index,
								  const materialization_v2_1_task_execution& task)
		{
			auto* request = authority.request();
			const auto* catalog = authority.catalog();
			const auto* engine = authority.engine();
			if (request == nullptr || catalog == nullptr || engine == nullptr)
				return sdk::unexpected(claim_error(
					"materialization.identity-mismatch", "claim-authority", "missing-owner"));
			const auto& admitted = request->request();
			if (authority.materialization_request_id() !=
					request->identity().materialization_request_id ||
				authority.task_count() != admitted.task_count() ||
				authority.worker_provider_id() != admitted.worker().provider_id ||
				authority.worker_semantic_contract_digest() !=
					admitted.worker().semantic_contract_digest ||
				authority.guarantee().approximation != "exact" ||
				authority.guarantee().scope != admitted.project_id() ||
				authority.guarantee().assumptions != authority.assumption_set_id() ||
				!std::ranges::equal(authority.guarantee().verification_modalities,
									v2_1_guarantee_modalities))
				return sdk::unexpected(claim_error("materialization.identity-mismatch",
												   "claim-authority",
												   "request-owner-or-profile"));
			if (auto valid = authority.guarantee().validate(); !valid)
				return sdk::unexpected(claim_error(
					"materialization.claim-invalid", "guarantee", nested_error(valid.error())));
			const auto& metadata = task.metadata;
			const auto& input = task.input;
			if (task_index >= authority.task_count() || metadata.task_index != task_index ||
				!task.source || !task.source->sealed() || !task.task_input ||
				!task.task_input->sealed() || task.source->receipt() != task.source_receipt ||
				!input.source.empty() || !input.source_content_base64.empty() ||
				metadata.project_id != input.project ||
				metadata.catalog_id != catalog->catalog_id ||
				metadata.catalog_digest != catalog->catalog_digest ||
				metadata.selected_catalog_compile_unit_id != input.selected_catalog_compile_unit ||
				metadata.final_relation_compile_unit_id != input.compile_unit ||
				metadata.variant_id != input.variant ||
				metadata.toolchain_context_id != input.toolchain_context ||
				metadata.toolchain_digest != input.toolchain_digest ||
				metadata.source_snapshot_id != input.source_snapshot ||
				metadata.file_id != input.file || metadata.logical_path != input.logical_path ||
				metadata.source_content_digest != input.source_content_digest ||
				metadata.source_size_bytes != input.source_size_bytes ||
				metadata.source_encoding != input.source_encoding ||
				metadata.line_index_id != input.line_index ||
				metadata.source_read_only != input.source_read_only ||
				metadata.condition_universe_id != input.condition_universe ||
				metadata.condition_id != input.condition ||
				metadata.interpretation_domain != input.interpretation ||
				!sandbox_minimum_matches(metadata.sandbox.minimum, input.sandbox.minimum) ||
				metadata.sandbox.policy_digest != input.sandbox.policy_digest)
				return sdk::unexpected(claim_error("materialization.task-binding-mismatch",
												   "task-window",
												   "sealed-source-authority"));

			auto expected = request->task_metadata_binding(task_index);
			if (!expected || expected->metadata != metadata ||
				!same_task_input_binding(expected->input, input))
				return sdk::unexpected(claim_error("materialization.task-binding-mismatch",
												   "task-window",
												   "request-replay-mismatch"));
			if (auto valid = input.validate_with_catalog(*catalog, task.source_receipt); !valid)
				return sdk::unexpected(claim_error("materialization.task-binding-mismatch",
												   "task-window",
												   nested_error(valid.error())));
			if (task.source->size_bytes() != task.source_receipt.size_bytes)
				return sdk::unexpected(
					claim_error("materialization.task-binding-mismatch", "source-spool", "size"));
			auto actual_source_digest = digest_sealed_task_spool(
				*task.source, maximum_clang22_task_source_bytes, "source-spool");
			if (!actual_source_digest)
				return sdk::unexpected(std::move(actual_source_digest.error()));
			if (*actual_source_digest != task.source_receipt.content_digest ||
				*actual_source_digest != input.source_content_digest)
				return sdk::unexpected(
					claim_error("materialization.task-binding-mismatch", "source-spool", "digest"));
			auto actual_task_input_digest = digest_sealed_task_spool(
				*task.task_input, maximum_clang22_task_input_bytes, "task-input-spool");
			if (!actual_task_input_digest)
				return sdk::unexpected(std::move(actual_task_input_digest.error()));
			if (*actual_task_input_digest != metadata.task_input_digest)
				return sdk::unexpected(claim_error(
					"materialization.task-binding-mismatch", "task-input-spool", "digest"));
			return {};
		}

		[[nodiscard]] sdk::claim_condition
		condition_for(const materialization_semantic_task_context& context)
		{
			return {context.condition_universe_id, {context.condition_id}};
		}

		[[nodiscard]] sdk::result<sdk::canonical_value>
		cell_projection(const sdk::detached_cell& cell)
		{
			if (cell.state == sdk::cell_state::absent)
				return sdk::canonical_value::null();
			if (cell.state != sdk::cell_state::present || !cell.value)
				return sdk::unexpected(claim_error(
					"materialization.claim-invalid", "base-row", "unknown-or-missing-cell"));
			return std::visit(
				[](const auto& value) -> sdk::result<sdk::canonical_value>
				{
					using value_type = std::decay_t<decltype(value)>;
					if constexpr (std::is_same_v<value_type, bool>)
						return sdk::canonical_value::from_boolean(value);
					else if constexpr (std::is_same_v<value_type, std::int64_t>)
						return sdk::canonical_value::from_integer(value);
					else if constexpr (std::is_same_v<value_type, std::uint64_t>)
					{
						if (value >
							static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
							return sdk::unexpected(claim_error("materialization.claim-invalid",
															   "base-row",
															   "signed-int64-overflow"));
						return sdk::canonical_value::from_integer(static_cast<std::int64_t>(value));
					}
					else if constexpr (std::is_same_v<value_type, std::string>)
						return sdk::canonical_value::from_string(value);
					else
						return sdk::unexpected(claim_error(
							"materialization.claim-invalid", "base-row", "unexpected-bytes"));
				},
				*cell.value);
		}

		[[nodiscard]] sdk::result<std::string> base_row_digest(const sdk::relation_engine& engine,
															   const sdk::detached_row& row)
		{
			auto relation = engine.require_id(row.descriptor_id);
			if (!relation)
				return sdk::unexpected(std::move(relation.error()));
			std::vector<std::pair<std::string, sdk::canonical_value>> fields;
			fields.reserve(relation->descriptor().columns.size());
			for (const auto& column : relation->descriptor().columns)
			{
				const auto found = row.cells.find(column.id);
				if (found == row.cells.end())
					return sdk::unexpected(claim_error(
						"materialization.claim-invalid", column.id, "base-cell-missing"));
				auto value = cell_projection(found->second);
				if (!value)
					return sdk::unexpected(std::move(value.error()));
				fields.emplace_back(column.name, std::move(*value));
			}
			return digest_projection("cxxlens.base-claim-row.v1",
									 object({{"descriptor_id", text(row.descriptor_id)},
											 {"row", object(std::move(fields))}}));
		}

		[[nodiscard]] sdk::result<std::string>
		span_bundle_digest(const observation_v2_primary_span& span)
		{
			if (span.begin > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
				span.end > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
				return sdk::unexpected(claim_error(
					"materialization.span-invalid", "primary-span", "signed-int64-overflow"));
			return digest_projection(
				"cxxlens.source-span-bundle.v2",
				object({{"span_id", text(span.span_id)},
						{"snapshot", text(span.snapshot)},
						{"file", text(span.file)},
						{"begin",
						 sdk::canonical_value::from_integer(static_cast<std::int64_t>(span.begin))},
						{"end",
						 sdk::canonical_value::from_integer(static_cast<std::int64_t>(span.end))},
						{"role", text(span.role)},
						{"read_only", sdk::canonical_value::from_boolean(span.read_only)}}));
		}

		[[nodiscard]] sdk::result<std::string>
		validate_producer_authority(const materialization_claim_request_view& request,
									const materialization_producer_authority& authority)
		{
			if (request.document_root == nullptr)
				return sdk::unexpected(
					claim_error("materialization.identity-mismatch", "tool", "missing"));
			const auto* tool = request.document_root->member("tool");
			if (tool == nullptr)
				return sdk::unexpected(
					claim_error("materialization.identity-mismatch", "tool", "missing"));
			for (const auto& [member, supplied] : {
					 std::pair{std::string_view{"executable"},
							   std::string_view{authority.executable}},
					 std::pair{std::string_view{"interface_version"},
							   std::string_view{authority.interface_version}},
					 std::pair{std::string_view{"distribution_version"},
							   std::string_view{authority.distribution_version}},
					 std::pair{std::string_view{"source_revision"},
							   std::string_view{authority.source_revision}},
					 std::pair{std::string_view{"source_tree"},
							   std::string_view{authority.source_tree}},
				 })
			{
				auto expected = json_text(*tool, member, "tool." + std::string{member});
				if (!expected)
					return sdk::unexpected(std::move(expected.error()));
				if (*expected != supplied)
					return sdk::unexpected(claim_error("materialization.identity-mismatch",
													   "producer-authority." + std::string{member},
													   "request-binding"));
			}
			if (!revision(authority.source_revision) || !revision(authority.source_tree))
				return sdk::unexpected(claim_error("materialization.identity-mismatch",
												   "producer-authority.source",
												   "revision-grammar"));
			if (authority.authority_bindings.size() != authority_paths.size())
				return sdk::unexpected(claim_error("materialization.identity-mismatch",
												   "producer-authority.bindings",
												   "exact-five"));

			std::vector<sdk::canonical_value> bindings;
			bindings.reserve(authority.authority_bindings.size());
			for (std::size_t index{}; index < authority_paths.size(); ++index)
			{
				const auto& binding = authority.authority_bindings[index];
				if (binding.path != authority_paths[index])
					return sdk::unexpected(claim_error("materialization.identity-mismatch",
													   "producer-authority.bindings",
													   "missing-extra-duplicate-or-order"));
				if (!content_digest(binding.content_digest))
					return sdk::unexpected(claim_error("materialization.identity-mismatch",
													   "producer-authority.digest",
													   binding.path));
				bindings.push_back(sdk::canonical_value::from_tuple(
					{text(binding.path), text(binding.content_digest)}));
			}
			return digest_projection("cxxlens.clang22-materializer-semantics.v1",
									 sdk::canonical_value::from_tuple({
										 text(authority.executable),
										 text(authority.interface_version),
										 text(authority.distribution_version),
										 text(authority.source_revision),
										 text(authority.source_tree),
										 sdk::canonical_value::from_tuple(std::move(bindings)),
									 }));
		}

		[[nodiscard]] sdk::result<std::string>
		validate_producer_authority(const materialization_v2_1_tool_authority& tool,
									const materialization_producer_authority& authority)
		{
			for (const auto& [member, expected, supplied] : {
					 std::tuple{std::string_view{"executable"},
								std::string_view{tool.executable},
								std::string_view{authority.executable}},
					 std::tuple{std::string_view{"interface_version"},
								std::string_view{tool.interface_version},
								std::string_view{authority.interface_version}},
					 std::tuple{std::string_view{"distribution_version"},
								std::string_view{tool.distribution_version},
								std::string_view{authority.distribution_version}},
					 std::tuple{std::string_view{"source_revision"},
								std::string_view{tool.source_revision},
								std::string_view{authority.source_revision}},
					 std::tuple{std::string_view{"source_tree"},
								std::string_view{tool.source_tree},
								std::string_view{authority.source_tree}},
				 })
			{
				if (expected != supplied)
					return sdk::unexpected(claim_error("materialization.identity-mismatch",
													   "producer-authority." + std::string{member},
													   "request-binding"));
			}
			if (!revision(authority.source_revision) || !revision(authority.source_tree))
				return sdk::unexpected(claim_error("materialization.identity-mismatch",
												   "producer-authority.source",
												   "revision-grammar"));
			if (authority.authority_bindings.size() != authority_paths.size())
				return sdk::unexpected(claim_error("materialization.identity-mismatch",
												   "producer-authority.bindings",
												   "exact-five"));

			std::vector<sdk::canonical_value> bindings;
			bindings.reserve(authority.authority_bindings.size());
			for (std::size_t index{}; index < authority_paths.size(); ++index)
			{
				const auto& binding = authority.authority_bindings[index];
				if (binding.path != authority_paths[index])
					return sdk::unexpected(claim_error("materialization.identity-mismatch",
													   "producer-authority.bindings",
													   "missing-extra-duplicate-or-order"));
				if (!content_digest(binding.content_digest))
					return sdk::unexpected(claim_error("materialization.identity-mismatch",
													   "producer-authority.digest",
													   binding.path));
				bindings.push_back(sdk::canonical_value::from_tuple(
					{text(binding.path), text(binding.content_digest)}));
			}
			return digest_projection("cxxlens.clang22-materializer-semantics.v1",
									 sdk::canonical_value::from_tuple({
										 text(authority.executable),
										 text(authority.interface_version),
										 text(authority.distribution_version),
										 text(authority.source_revision),
										 text(authority.source_tree),
										 sdk::canonical_value::from_tuple(std::move(bindings)),
									 }));
		}

		[[nodiscard]] sdk::result<std::pair<sdk::claim_guarantee, std::string>>
		validate_guarantee(const materialization_claim_request_view& request,
						   const materialization_guarantee_authority& authority)
		{
			if (!sorted_unique(authority.assumptions))
				return sdk::unexpected(claim_error("materialization.claim-invalid",
												   "guarantee.assumptions",
												   "canonical-sorted-unique"));
			for (const auto& value : authority.assumptions)
				if (auto valid = sdk::validate_strong_id(value); !valid)
					return sdk::unexpected(claim_error("materialization.claim-invalid",
													   "guarantee.assumptions",
													   nested_error(valid.error())));
			if (authority.verification_modalities.empty() ||
				!sorted_unique(authority.verification_modalities))
				return sdk::unexpected(claim_error("materialization.claim-invalid",
												   "guarantee.verification_modalities",
												   "nonempty-canonical-sorted-unique"));
			for (const auto& value : authority.verification_modalities)
				if (auto valid = sdk::validate_registered_symbol(value); !valid)
					return sdk::unexpected(claim_error("materialization.claim-invalid",
													   "guarantee.verification_modalities",
													   nested_error(valid.error())));

			auto assumption_digest = digest_projection("cxxlens.clang22-assumption-set.v1",
													   texts(authority.assumptions));
			if (!assumption_digest)
				return sdk::unexpected(std::move(assumption_digest.error()));
			if (request.tasks.empty())
				return sdk::unexpected(
					claim_error("materialization.task-binding-mismatch", "tasks", "empty"));
			const auto& scope = request.tasks.front().worker_input.project;
			if (std::ranges::any_of(request.tasks,
									[&](const validated_task_request& task)
									{
										return task.worker_input.project != scope;
									}))
				return sdk::unexpected(claim_error(
					"materialization.task-binding-mismatch", "tasks.project", "not-common"));
			std::string assumption_set = "assumption-set:" + *assumption_digest;
			sdk::claim_guarantee guarantee{
				"exact", scope, assumption_set, authority.verification_modalities};
			if (auto valid = guarantee.validate(); !valid)
				return sdk::unexpected(claim_error(
					"materialization.claim-invalid", "guarantee", nested_error(valid.error())));
			return std::pair{std::move(guarantee), std::move(assumption_set)};
		}

		[[nodiscard]] sdk::result<std::pair<sdk::claim_guarantee, std::string>>
		validate_guarantee(const std::string_view scope,
						   const materialization_guarantee_authority& authority)
		{
			if (!sorted_unique(authority.assumptions))
				return sdk::unexpected(claim_error("materialization.claim-invalid",
												   "guarantee.assumptions",
												   "canonical-sorted-unique"));
			for (const auto& value : authority.assumptions)
				if (auto valid = sdk::validate_strong_id(value); !valid)
					return sdk::unexpected(claim_error("materialization.claim-invalid",
													   "guarantee.assumptions",
													   nested_error(valid.error())));
			if (authority.verification_modalities.empty() ||
				!sorted_unique(authority.verification_modalities))
				return sdk::unexpected(claim_error("materialization.claim-invalid",
												   "guarantee.verification_modalities",
												   "nonempty-canonical-sorted-unique"));
			for (const auto& value : authority.verification_modalities)
				if (auto valid = sdk::validate_registered_symbol(value); !valid)
					return sdk::unexpected(claim_error("materialization.claim-invalid",
													   "guarantee.verification_modalities",
													   nested_error(valid.error())));

			auto assumption_digest = digest_projection("cxxlens.clang22-assumption-set.v1",
													   texts(authority.assumptions));
			if (!assumption_digest)
				return sdk::unexpected(std::move(assumption_digest.error()));
			std::string assumption_set = "assumption-set:" + *assumption_digest;
			sdk::claim_guarantee guarantee{
				"exact", std::string{scope}, assumption_set, authority.verification_modalities};
			if (auto valid = guarantee.validate(); !valid)
				return sdk::unexpected(claim_error(
					"materialization.claim-invalid", "guarantee", nested_error(valid.error())));
			return std::pair{std::move(guarantee), std::move(assumption_set)};
		}

		[[nodiscard]] sdk::result<direct_basis_values>
		make_direct_basis(const materialization_claim_request_view& request,
						  const materialization_producer_authority& authority,
						  const std::span<const materialization_semantic_task_context> contexts)
		{
			auto materializer = validate_producer_authority(request, authority);
			if (!materializer)
				return sdk::unexpected(std::move(materializer.error()));
			if (request.document_root == nullptr)
				return sdk::unexpected(
					claim_error("materialization.identity-mismatch", "worker", "missing"));
			const auto& root = *request.document_root;
			const auto* worker = root.member("worker");
			if (worker == nullptr)
				return sdk::unexpected(
					claim_error("materialization.identity-mismatch", "worker", "missing"));
			auto provider_id = json_text(*worker, "provider_id", "worker.provider_id");
			auto provider_version =
				json_text(*worker, "provider_version", "worker.provider_version");
			auto worker_semantics =
				json_text(*worker, "semantic_contract_digest", "worker.semantic_contract_digest");
			auto protocol_major = json_integer(*worker, "protocol_major", "worker.protocol_major");
			auto protocol_minor = json_integer(*worker, "protocol_minor", "worker.protocol_minor");
			std::optional<std::vector<std::string>> required_features;
			if (const auto* features_value = worker->member("required_features");
				features_value != nullptr)
			{
				const auto* features = features_value->as_array();
				if (features == nullptr)
					return sdk::unexpected(claim_error(
						"materialization.identity-mismatch", "worker.required_features", "array"));
				required_features.emplace();
				required_features->reserve(features->size());
				for (const auto& feature : *features)
				{
					if (feature.as_string() == nullptr)
						return sdk::unexpected(claim_error("materialization.identity-mismatch",
														   "worker.required_features",
														   "string"));
					required_features->push_back(*feature.as_string());
				}
				if (!sorted_unique(*required_features))
					return sdk::unexpected(claim_error("materialization.identity-mismatch",
													   "worker.required_features",
													   "canonical-sorted-unique"));
			}
			if (!provider_id || !provider_version || !worker_semantics || !protocol_major ||
				!protocol_minor)
				return sdk::unexpected(!provider_id			   ? std::move(provider_id.error())
										   : !provider_version ? std::move(provider_version.error())
										   : !worker_semantics ? std::move(worker_semantics.error())
										   : !protocol_major   ? std::move(protocol_major.error())
															   : std::move(protocol_minor.error()));

			auto admitted_descriptors = request.engine.descriptors();
			std::ranges::sort(admitted_descriptors, {}, &sdk::relation_descriptor::id);
			std::vector<sdk::canonical_value> descriptors;
			for (const auto& descriptor : admitted_descriptors)
				descriptors.push_back(object({
					{"descriptor_id", text(descriptor.id)},
					{"runtime_descriptor_digest", text(descriptor.descriptor_digest)},
				}));

			std::vector<std::pair<context_key, materialization_semantic_task_context>> ordered;
			ordered.reserve(contexts.size());
			for (const auto& context : contexts)
				ordered.emplace_back(context_tuple(context), context);
			std::ranges::sort(ordered, {}, &decltype(ordered)::value_type::first);
			std::vector<sdk::canonical_value> semantic_tasks;
			semantic_tasks.reserve(ordered.size());
			for (const auto& [key, context] : ordered)
			{
				(void)key;
				semantic_tasks.push_back(sdk::canonical_value::from_tuple(
					{context_object(context), text(context.task_input_digest)}));
			}

			std::vector<sdk::canonical_value> worker_projection{
				text(std::string{*provider_id}),
				text(std::string{*provider_version}),
				text(std::string{*worker_semantics}),
				sdk::canonical_value::from_integer(*protocol_major),
				sdk::canonical_value::from_integer(*protocol_minor),
			};
			if (required_features)
			{
				std::vector<sdk::canonical_value> feature_values;
				feature_values.reserve(required_features->size());
				for (const auto& feature : *required_features)
					feature_values.push_back(text(feature));
				worker_projection.push_back(
					sdk::canonical_value::from_tuple(std::move(feature_values)));
			}

			auto basis = digest_projection(
				"cxxlens.clang22-direct-materialization-basis.v1",
				sdk::canonical_value::from_tuple({
					text("cxxlens.clang22-direct-materialization-basis.v1"),
					text(*materializer),
					sdk::canonical_value::from_tuple(std::move(worker_projection)),
					sdk::canonical_value::from_tuple({
						text(request.tasks.front().worker_input.project),
						text(request.catalog.catalog_id),
						text(request.catalog.catalog_digest),
					}),
					sdk::canonical_value::from_tuple({
						text(std::string{engine_generation_contract}),
						text(std::string{request.engine.generation()}),
						text(std::string{request.engine.registry_digest()}),
						sdk::canonical_value::from_tuple(std::move(descriptors)),
					}),
					sdk::canonical_value::from_tuple(std::move(semantic_tasks)),
				}));
			if (!basis)
				return sdk::unexpected(std::move(basis.error()));
			const auto transform = [&](const std::string_view domain)
			{
				return digest_projection(
					domain,
					sdk::canonical_value::from_tuple(
						{text(std::string{domain}),
						 text(*materializer),
						 text(std::string{request.engine.registry_digest()})}));
			};
			auto canonical = transform("cxxlens.clang22-canonical-adoption-transform.v1");
			auto base = transform("cxxlens.clang22-base-ingestion-transform.v1");
			if (!canonical || !base)
				return sdk::unexpected(!canonical ? std::move(canonical.error())
												  : std::move(base.error()));
			return direct_basis_values{std::move(*materializer),
									   std::move(*basis),
									   std::move(*canonical),
									   std::move(*base)};
		}

		[[nodiscard]] sdk::result<direct_basis_values>
		make_direct_basis(const materialization_v2_1_tool_authority& tool,
						  const materialization_v2_1_worker_authority& worker,
						  const sdk::project_catalog& catalog,
						  const sdk::relation_engine& engine,
						  const std::string_view project_id,
						  const std::span<const materialization_semantic_task_context> contexts,
						  const materialization_producer_authority& authority)
		{
			auto materializer = validate_producer_authority(tool, authority);
			if (!materializer)
				return sdk::unexpected(std::move(materializer.error()));
			if (worker.protocol_major >
					static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
				worker.protocol_minor >
					static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
				return sdk::unexpected(claim_error("materialization.identity-mismatch",
												   "worker.protocol",
												   "signed-int64-overflow"));

			auto admitted_descriptors = engine.descriptors();
			std::ranges::sort(admitted_descriptors, {}, &sdk::relation_descriptor::id);
			std::vector<sdk::canonical_value> descriptors;
			for (const auto& descriptor : admitted_descriptors)
				descriptors.push_back(object({
					{"descriptor_id", text(descriptor.id)},
					{"runtime_descriptor_digest", text(descriptor.descriptor_digest)},
				}));

			std::vector<std::pair<context_key, materialization_semantic_task_context>> ordered;
			ordered.reserve(contexts.size());
			for (const auto& context : contexts)
				ordered.emplace_back(context_tuple(context), context);
			std::ranges::sort(ordered, {}, &decltype(ordered)::value_type::first);
			std::vector<sdk::canonical_value> semantic_tasks;
			semantic_tasks.reserve(ordered.size());
			for (const auto& [key, context] : ordered)
			{
				(void)key;
				semantic_tasks.push_back(sdk::canonical_value::from_tuple(
					{context_object(context), text(context.task_input_digest)}));
			}

			std::vector<sdk::canonical_value> worker_projection{
				text(worker.provider_id),
				text(worker.provider_version),
				text(worker.semantic_contract_digest),
				sdk::canonical_value::from_integer(
					static_cast<std::int64_t>(worker.protocol_major)),
				sdk::canonical_value::from_integer(
					static_cast<std::int64_t>(worker.protocol_minor)),
			};
			std::vector<sdk::canonical_value> feature_values;
			feature_values.reserve(worker.required_features.size());
			for (const auto& feature : worker.required_features)
				feature_values.push_back(text(feature));
			worker_projection.push_back(
				sdk::canonical_value::from_tuple(std::move(feature_values)));

			auto basis = digest_projection(
				"cxxlens.clang22-direct-materialization-basis.v1",
				sdk::canonical_value::from_tuple({
					text("cxxlens.clang22-direct-materialization-basis.v1"),
					text(*materializer),
					sdk::canonical_value::from_tuple(std::move(worker_projection)),
					sdk::canonical_value::from_tuple({
						text(std::string{project_id}),
						text(catalog.catalog_id),
						text(catalog.catalog_digest),
					}),
					sdk::canonical_value::from_tuple({
						text(std::string{engine_generation_contract}),
						text(std::string{engine.generation()}),
						text(std::string{engine.registry_digest()}),
						sdk::canonical_value::from_tuple(std::move(descriptors)),
					}),
					sdk::canonical_value::from_tuple(std::move(semantic_tasks)),
				}));
			if (!basis)
				return sdk::unexpected(std::move(basis.error()));
			const auto transform = [&](const std::string_view domain)
			{
				return digest_projection(domain,
										 sdk::canonical_value::from_tuple(
											 {text(std::string{domain}),
											  text(*materializer),
											  text(std::string{engine.registry_digest()})}));
			};
			auto canonical = transform("cxxlens.clang22-canonical-adoption-transform.v1");
			auto base = transform("cxxlens.clang22-base-ingestion-transform.v1");
			if (!canonical || !base)
				return sdk::unexpected(!canonical ? std::move(canonical.error())
												  : std::move(base.error()));
			return direct_basis_values{std::move(*materializer),
									   std::move(*basis),
									   std::move(*canonical),
									   std::move(*base)};
		}

		[[nodiscard]] sdk::result<void>
		validate_task_side_channels(const validated_task_request& request,
									const sealed_materialization_result& result)
		{
			// The generic transcript seal requires its transport-level task receipt in addition to
			// the specialization's three semantic coverage units.
			constexpr std::array<std::string_view, 4U> kinds{
				"cc.call-extraction", "cc.entity", "frontend.clang22.observation", "task"};
			const auto coverage = result.provider_seal().coverage();
			if (coverage.size() != kinds.size())
				return sdk::unexpected(claim_error("materialization.coverage-incomplete",
												   "provider.coverage",
												   "transport-task-plus-exact-three"));
			for (std::size_t index{}; index < kinds.size(); ++index)
				if (coverage[index].kind != kinds[index] ||
					coverage[index].id != request.provider_task_id ||
					coverage[index].state != "covered" || !coverage[index].reason.empty())
					return sdk::unexpected(claim_error("materialization.coverage-incomplete",
													   "provider.coverage",
													   "canonical-balanced-covered"));
			if (!result.provider_seal().unresolved().empty())
				return sdk::unexpected(claim_error("materialization.coverage-incomplete",
												   "provider.unresolved",
												   "qualified-zero"));
			const auto evidence = result.provider_seal().evidence();
			if (evidence.size() != 1U || evidence.front().kind != "provider.clang22.execution" ||
				evidence.front().subject != request.provider_task_id ||
				evidence.front().producer != "cxxlens.clang22.reference" ||
				evidence.front().summary != "exact")
				return sdk::unexpected(claim_error("materialization.claim-invalid",
												   "provider.evidence",
												   "complete-exact-provenance"));
			for (const auto& row : result.observation_rows())
			{
				if (!row.observation.exact_equivalence || row.observation.limitation)
					return sdk::unexpected(claim_error("materialization.coverage-incomplete",
													   "observation.exact_equivalence",
													   "qualified-exact-only"));
				if (row.observation.kind != observation_v2_kind::type &&
					!row.observation.primary_span)
					return sdk::unexpected(claim_error("materialization.span-invalid",
													   "observation.primary_span",
													   "qualified-bundle-required"));
			}
			return {};
		}

		[[nodiscard]] bool same_claim(const sdk::claim& left, const sdk::claim& right)
		{
			return left.descriptor == right.descriptor && left.semantic_key == right.semantic_key &&
				left.assertion == right.assertion && left.content == right.content &&
				left.row.descriptor_id == right.row.descriptor_id &&
				left.row.canonical_form() == right.row.canonical_form() &&
				left.presence == right.presence && left.interpretation == right.interpretation &&
				left.stage == right.stage && left.producer == right.producer &&
				left.input_basis == right.input_basis &&
				left.provenance_root == right.provenance_root &&
				left.guarantee.approximation == right.guarantee.approximation &&
				left.guarantee.scope == right.guarantee.scope &&
				left.guarantee.assumptions == right.guarantee.assumptions &&
				left.guarantee.verification_modalities == right.guarantee.verification_modalities;
		}

		[[nodiscard]] sdk::result<materialization_claim_envelope> make_envelope(std::string role,
																				sdk::claim value)
		{
			const auto row_form = value.row.canonical_form();
			auto row_ref = identity(
				"materialization-claim-row",
				{text(value.descriptor), sdk::canonical_value::from_bytes(byte_string(row_form))});
			if (!row_ref)
				return sdk::unexpected(std::move(row_ref.error()));
			auto singleton = sdk::claim_batch_content_digest(
				std::span<const sdk::claim>{&value, 1U}, {}, {}, {});
			if (!singleton)
				return sdk::unexpected(std::move(singleton.error()));
			auto claim_ref =
				identity("materialization-claim-envelope", {text(role), text(*singleton)});
			if (!claim_ref)
				return sdk::unexpected(std::move(claim_ref.error()));
			return materialization_claim_envelope{std::move(role),
												  std::move(*row_ref),
												  std::move(*claim_ref),
												  std::move(*singleton),
												  std::move(value)};
		}

		[[nodiscard]] sdk::result<std::string>
		add_envelope(std::map<std::string, materialization_claim_envelope, std::less<>>& envelopes,
					 std::map<std::string, std::pair<std::string, std::string>, std::less<>>& rows,
					 materialization_claim_envelope envelope)
		{
			const auto claim_ref = envelope.claim_ref;
			const auto row_identity =
				std::pair{envelope.value.descriptor, envelope.value.row.canonical_form()};
			auto [row, row_inserted] = rows.emplace(envelope.row_ref, row_identity);
			if (!row_inserted && row->second != row_identity)
				return sdk::unexpected(claim_error(
					"materialization.claim-invalid", "row_ref", "aliases-different-row"));
			const auto found = envelopes.find(claim_ref);
			if (found != envelopes.end())
			{
				const auto& prior = found->second;
				if (prior.claim_ref != claim_ref || prior.row_ref != envelope.row_ref ||
					prior.role != envelope.role ||
					prior.sdk_singleton_claim_batch_digest !=
						envelope.sdk_singleton_claim_batch_digest ||
					!same_claim(prior.value, envelope.value))
					return sdk::unexpected(claim_error("materialization.claim-invalid",
													   "claim_ref",
													   "aliases-different-occurrence"));
				return claim_ref;
			}
			envelopes.emplace(claim_ref, std::move(envelope));
			return claim_ref;
		}

		[[nodiscard]] sdk::result<std::string>
		worker_provenance(const std::string_view descriptor_id,
						  const materialization_semantic_task_context& context,
						  const std::string_view row_digest)
		{
			return digest_projection("cxxlens.clang22-fixture-provenance-edge.v2",
									 object({{"descriptor_id", text(std::string{descriptor_id})},
											 {"originating_task", context_object(context)},
											 {"row_digest", text(std::string{row_digest})}}));
		}

		[[nodiscard]] sdk::result<materialization_origin_association>
		make_association(std::string stored_claim_ref,
						 materialization_semantic_task_context context,
						 std::string sealed_row_digest,
						 std::optional<std::string> source_evidence_digest)
		{
			auto association = identity("materialization-claim-association",
										{text(stored_claim_ref),
										 context_tuple_value(context),
										 text(sealed_row_digest),
										 text(source_evidence_digest.value_or(""))});
			if (!association)
				return sdk::unexpected(std::move(association.error()));
			return materialization_origin_association{std::move(*association),
													  std::move(stored_claim_ref),
													  std::move(context),
													  std::move(sealed_row_digest),
													  std::move(source_evidence_digest)};
		}

		[[nodiscard]] sdk::result<std::string>
		base_source_evidence(std::vector<std::pair<std::string, std::string>> edges)
		{
			std::ranges::sort(edges);
			std::vector<sdk::canonical_value> projected;
			projected.reserve(edges.size());
			for (auto& [kind, subject] : edges)
				projected.push_back(object({{"kind", text(std::move(kind))},
											{"subject_digest", text(std::move(subject))}}));
			return digest_projection("cxxlens.clang22-base-source-evidence.v1",
									 sdk::canonical_value::from_tuple(std::move(projected)));
		}

		[[nodiscard]] sdk::result<std::string>
		catalog_entry_evidence(const materialization_claim_request_view& request,
							   const validated_task_request& task)
		{
			const auto found = std::ranges::find(request.catalog.compile_units,
												 task.worker_input.selected_catalog_compile_unit,
												 &sdk::catalog_compile_unit::compile_unit_id);
			if (found == request.catalog.compile_units.end())
				return sdk::unexpected(claim_error(
					"materialization.task-binding-mismatch", "catalog-entry", "selected-missing"));
			return digest_projection(
				"cxxlens.clang22-catalog-entry-evidence.v1",
				object({{"catalog_compile_unit_id", text(found->compile_unit_id)},
						{"effective_invocation_digest", text(found->effective_invocation_digest)},
						{"source_digest", text(found->source_digest)},
						{"environment_digest", text(found->environment_digest)}}));
		}

		[[nodiscard]] sdk::result<std::string>
		base_evidence_for(const materialization_claim_request_view& request,
						  const validated_task_request& task,
						  const sdk::detached_row& row,
						  const std::string_view row_digest)
		{
			if (row.descriptor_id == "build.project.v1")
				return base_source_evidence({{"compile_context", request.catalog.catalog_digest}});
			if (row.descriptor_id == "build.toolchain_context.v1")
				return base_source_evidence(
					{{"compile_context", task.worker_input.toolchain_digest}});
			if (row.descriptor_id == "build.variant.v1")
				return base_source_evidence({{"compile_context", std::string{row_digest}}});
			if (row.descriptor_id == "source.file.v1")
				return base_source_evidence(
					{{"source_observation", task.worker_input.source_content_digest}});
			if (row.descriptor_id == "build.compile_unit.v1")
			{
				auto catalog = catalog_entry_evidence(request, task);
				if (!catalog)
					return sdk::unexpected(std::move(catalog.error()));
				return base_source_evidence({{"compile_context", std::move(*catalog)}});
			}
			return sdk::unexpected(claim_error(
				"materialization.claim-invalid", row.descriptor_id, "unsupported-base-evidence"));
		}

		[[nodiscard]] sdk::result<std::string>
		span_source_evidence(const std::string& observation_row_digest,
							 const std::string& bundle_digest)
		{
			return base_source_evidence({{"dynamic_observation", observation_row_digest},
										 {"source_observation", bundle_digest}});
		}

		[[nodiscard]] sdk::result<std::string>
		semantic_task_key(const materialization_semantic_task_context& context)
		{
			auto tuple = context_tuple(context);
			std::vector<sdk::canonical_value> fields;
			fields.reserve(tuple.size());
			for (auto& value : tuple)
				fields.push_back(text(std::move(value)));
			return identity("materialization-task", std::move(fields));
		}

		[[nodiscard]] sdk::result<std::vector<sdk::snapshot_coverage_unit>>
		coverage_units(const materialization_semantic_task_context& context,
					   const std::string_view descriptor_id,
					   const bool base)
		{
			auto task = semantic_task_key(context);
			if (!task)
				return sdk::unexpected(std::move(task.error()));
			if (base)
			{
				auto key = identity("materialization-base-descriptor",
									{text(*task), text(std::string{descriptor_id})});
				if (!key)
					return sdk::unexpected(std::move(key.error()));
				return std::vector<sdk::snapshot_coverage_unit>{
					{"materialization.base-descriptor", std::move(*key), "covered", {}}};
			}
			const auto group = descriptor_id.starts_with("cc.") ? "canonical" : "observation";
			auto dependency =
				identity("materialization-dependency-group", {text(*task), text(group)});
			if (!dependency)
				return sdk::unexpected(std::move(dependency.error()));
			return std::vector<sdk::snapshot_coverage_unit>{
				{"materialization.task", *task, "covered", {}},
				{"materialization.dependency-group", std::move(*dependency), "covered", {}}};
		}

		[[nodiscard]] partition_key partition_identity_fields(const sdk::partition_draft& draft)
		{
			return {draft.relation_descriptor_id,
					draft.scope,
					draft.condition.canonical_form(),
					draft.interpretation,
					draft.producer_semantics,
					draft.producer_input_basis_digest,
					draft.precision_profile,
					draft.assumption_set_id};
		}

		[[nodiscard]] sdk::result<std::string>
		empty_partition_basis(const std::string& direct_basis,
							  const std::string_view descriptor_id,
							  const sdk::claim_condition& condition,
							  const std::string& interpretation,
							  const std::string& producer_semantics,
							  const std::string& transform_semantics)
		{
			auto semantic = digest_projection(
				"cxxlens.clang22-empty-partition-basis.v1",
				sdk::canonical_value::from_tuple({text(direct_basis),
												  text(std::string{descriptor_id}),
												  text(condition.canonical_form()),
												  text(interpretation),
												  text(producer_semantics),
												  text(transform_semantics)}));
			if (!semantic)
				return sdk::unexpected(std::move(semantic.error()));
			return sdk::claim_input_basis_digest(sdk::direct_claim_basis{std::move(*semantic)});
		}

		[[nodiscard]] sdk::result<std::string> required_row_string(const sdk::detached_row& row,
																   const std::string_view column_id)
		{
			const auto found = row.cells.find(column_id);
			if (found == row.cells.end() || found->second.state != sdk::cell_state::present ||
				!found->second.value)
				return sdk::unexpected(claim_error(
					"materialization.claim-invalid", std::string{column_id}, "missing"));
			const auto* value = std::get_if<std::string>(&*found->second.value);
			if (value == nullptr)
				return sdk::unexpected(
					claim_error("materialization.claim-invalid", std::string{column_id}, "type"));
			return *value;
		}
	} // namespace

	sealed_materialization_claims::sealed_materialization_claims(
		std::string materializer_semantics_digest,
		std::string direct_basis_digest,
		std::string canonical_adoption_transform_digest,
		std::string base_ingestion_transform_digest,
		std::string assumption_set_id,
		sdk::claim_batch_result final_claim_batch,
		std::vector<materialization_claim_envelope> claim_envelopes,
		std::vector<materialization_canonicalization_edge> canonicalization_edges,
		std::vector<materialization_origin_association> origin_associations,
		std::vector<materialization_claim_partition> partitions)
		: materializer_semantics_digest_{std::move(materializer_semantics_digest)},
		  direct_basis_digest_{std::move(direct_basis_digest)},
		  canonical_adoption_transform_digest_{std::move(canonical_adoption_transform_digest)},
		  base_ingestion_transform_digest_{std::move(base_ingestion_transform_digest)},
		  assumption_set_id_{std::move(assumption_set_id)},
		  final_claim_batch_{std::move(final_claim_batch)},
		  claim_envelopes_{std::move(claim_envelopes)},
		  canonicalization_edges_{std::move(canonicalization_edges)},
		  origin_associations_{std::move(origin_associations)}, partitions_{std::move(partitions)}
	{
	}

	std::string_view sealed_materialization_claims::materializer_semantics_digest() const noexcept
	{
		return materializer_semantics_digest_;
	}

	std::string_view sealed_materialization_claims::direct_basis_digest() const noexcept
	{
		return direct_basis_digest_;
	}

	std::string_view
	sealed_materialization_claims::canonical_adoption_transform_digest() const noexcept
	{
		return canonical_adoption_transform_digest_;
	}

	std::string_view sealed_materialization_claims::base_ingestion_transform_digest() const noexcept
	{
		return base_ingestion_transform_digest_;
	}

	std::string_view sealed_materialization_claims::assumption_set_id() const noexcept
	{
		return assumption_set_id_;
	}

	const sdk::claim_batch_result& sealed_materialization_claims::final_claim_batch() const noexcept
	{
		return final_claim_batch_;
	}

	std::span<const materialization_claim_envelope>
	sealed_materialization_claims::claim_envelopes() const noexcept
	{
		return claim_envelopes_;
	}

	std::span<const materialization_canonicalization_edge>
	sealed_materialization_claims::canonicalization_edges() const noexcept
	{
		return canonicalization_edges_;
	}

	std::span<const materialization_origin_association>
	sealed_materialization_claims::origin_associations() const noexcept
	{
		return origin_associations_;
	}

	std::span<const materialization_claim_partition>
	sealed_materialization_claims::partitions() const noexcept
	{
		return partitions_;
	}

	sdk::result<sealed_materialization_claims> construct_materialization_claims_from_loader(
		const validated_materialization_request& request,
		const materialization_task_result_loader& load,
		const materialization_producer_authority& producer_authority,
		const materialization_guarantee_authority& guarantee_authority)
	{
		if (request.tasks.empty() || !load)
			return sdk::unexpected(
				claim_error("materialization.task-binding-mismatch", "task-results", "loader"));
		if (auto valid = request.catalog.validate(); !valid)
			return sdk::unexpected(claim_error(
				"materialization.claim-invalid", "project-catalog", nested_error(valid.error())));

		std::vector<materialization_semantic_task_context> contexts;
		contexts.reserve(request.tasks.size());
		std::set<context_key> unique_contexts;
		for (std::size_t index{}; index < request.tasks.size(); ++index)
		{
			const auto& task = request.tasks[index];
			if (task.source_receipt)
			{
				if (!task.worker_payload.empty() || !task.worker_input.source.empty() ||
					!task.worker_input.source_content_base64.empty())
					return sdk::unexpected(claim_error("materialization.task-binding-mismatch",
													   "task.v3",
													   "streaming-residency-violation"));
				if (auto valid = task.worker_input.validate_with_catalog(request.catalog,
																		 *task.source_receipt);
					!valid)
					return sdk::unexpected(claim_error("materialization.task-binding-mismatch",
													   "task.v3",
													   nested_error(valid.error())));
			}
			else
			{
				if (auto valid = task.worker_input.validate(); !valid)
					return sdk::unexpected(claim_error("materialization.task-binding-mismatch",
													   "task.v3",
													   nested_error(valid.error())));
				auto encoded_task = encode_task_input(task.worker_input);
				if (!encoded_task || *encoded_task != task.worker_payload ||
					sdk::content_digest(task.worker_payload) != task.task_input_digest)
					return sdk::unexpected(claim_error("materialization.task-binding-mismatch",
													   "task.v3",
													   "payload-or-digest-rebinding"));
			}
			auto context = task_context(task);
			if (!unique_contexts.insert(context_tuple(context)).second)
				return sdk::unexpected(claim_error(
					"materialization.task-binding-mismatch", "semantic-task-context", "duplicate"));
			contexts.push_back(std::move(context));
		}

		const materialization_claim_request_view request_view{
			request.catalog, request.engine, request.tasks, &request.document.root()};
		auto basis = make_direct_basis(request_view, producer_authority, contexts);
		if (!basis)
			return sdk::unexpected(std::move(basis.error()));
		auto guarantee_values = validate_guarantee(request_view, guarantee_authority);
		if (!guarantee_values)
			return sdk::unexpected(std::move(guarantee_values.error()));
		const auto& guarantee = guarantee_values->first;
		const auto& assumption_set = guarantee_values->second;

		const auto* worker = request.document.root().member("worker");
		if (worker == nullptr)
			return sdk::unexpected(
				claim_error("materialization.identity-mismatch", "worker", "missing"));
		auto worker_id = json_text(*worker, "provider_id", "worker.provider_id");
		auto worker_semantics =
			json_text(*worker, "semantic_contract_digest", "worker.semantic_contract_digest");
		if (!worker_id || !worker_semantics)
			return sdk::unexpected(!worker_id ? std::move(worker_id.error())
											  : std::move(worker_semantics.error()));
		const sdk::claim_producer worker_producer{std::string{*worker_id},
												  std::string{*worker_semantics}};
		const sdk::claim_producer materializer_producer{"cxxlens.clang22.materializer",
														basis->materializer_semantics};

		std::map<std::string, materialization_claim_envelope, std::less<>> envelopes;
		std::map<std::string, std::pair<std::string, std::string>, std::less<>> claim_rows_by_ref;
		std::map<std::tuple<std::string, std::string, std::string>,
				 materialization_canonicalization_edge>
			edges;
		std::map<std::string, materialization_origin_association, std::less<>> associations;
		std::vector<final_occurrence> occurrences;
		sdk::claim_batch batch;
		std::vector<std::array<bool, 6U>> output_nonempty(request.tasks.size());
		std::set<std::pair<context_key, std::string>> base_seen;

		const auto record_claim = [&](const sdk::detached_row& row,
									  const materialization_semantic_task_context& context,
									  const std::string& provenance_root,
									  const sdk::claim_producer& precursor_producer,
									  const std::optional<std::string>& transform,
									  const std::string& sealed_row_digest,
									  const std::optional<std::string>& source_evidence_digest,
									  const bool base) -> sdk::result<void>
		{
			auto assertion =
				sdk::make_assertion(request.engine,
									sdk::observation{row,
													 condition_for(context),
													 context.interpretation_domain,
													 precursor_producer,
													 sdk::direct_claim_basis{basis->direct_basis},
													 provenance_root,
													 guarantee});
			if (!assertion)
				return sdk::unexpected(claim_error("materialization.claim-invalid",
												   row.descriptor_id,
												   nested_error(assertion.error())));

			sdk::claim final = *assertion;
			std::string final_ref;
			if (transform)
			{
				auto hidden = make_envelope("hidden_precursor", *assertion);
				if (!hidden)
					return sdk::unexpected(std::move(hidden.error()));
				auto hidden_ref = add_envelope(envelopes, claim_rows_by_ref, std::move(*hidden));
				if (!hidden_ref)
					return sdk::unexpected(std::move(hidden_ref.error()));
				auto canonical = sdk::make_canonical_claim(
					request.engine, *assertion, materializer_producer, row, *transform);
				if (!canonical)
					return sdk::unexpected(claim_error("materialization.claim-invalid",
													   row.descriptor_id,
													   nested_error(canonical.error())));
				final = std::move(*canonical);
				auto final_envelope = make_envelope("stored_final", final);
				if (!final_envelope)
					return sdk::unexpected(std::move(final_envelope.error()));
				final_ref = final_envelope->claim_ref;
				auto stored =
					add_envelope(envelopes, claim_rows_by_ref, std::move(*final_envelope));
				if (!stored)
					return sdk::unexpected(std::move(stored.error()));
				materialization_canonicalization_edge edge{*hidden_ref, final_ref, *transform};
				auto edge_key = std::tuple{
					edge.precursor_claim_ref, edge.final_claim_ref, edge.transform_semantics};
				auto [found, inserted] = edges.emplace(edge_key, edge);
				if (!inserted && found->second != edge)
					return sdk::unexpected(claim_error("materialization.claim-invalid",
													   "canonicalization-edge",
													   "identity-collision"));
			}
			else
			{
				auto final_envelope = make_envelope("stored_final", final);
				if (!final_envelope)
					return sdk::unexpected(std::move(final_envelope.error()));
				final_ref = final_envelope->claim_ref;
				auto stored =
					add_envelope(envelopes, claim_rows_by_ref, std::move(*final_envelope));
				if (!stored)
					return sdk::unexpected(std::move(stored.error()));
			}

			auto association =
				make_association(final_ref, context, sealed_row_digest, source_evidence_digest);
			if (!association)
				return sdk::unexpected(std::move(association.error()));
			auto [association_entry, association_inserted] =
				associations.emplace(association->association_id, *association);
			if (!association_inserted && association_entry->second != *association)
				return sdk::unexpected(claim_error(
					"materialization.claim-invalid", "origin-association", "identity-collision"));
			if (auto added = batch.add(final); !added)
				return sdk::unexpected(claim_error("materialization.claim-invalid",
												   row.descriptor_id,
												   nested_error(added.error())));
			// Keep only the identity/context needed to replay the occurrence into its
			// partition; retaining one full sdk::claim per occurrence would duplicate the
			// complete task result set before Store publication.
			occurrences.push_back({std::move(final_ref), context, base});
			return {};
		};

		for (std::size_t task_index{}; task_index < request.tasks.size(); ++task_index)
		{
			const auto& task = request.tasks[task_index];
			auto loaded = load(task_index);
			if (!loaded)
				return sdk::unexpected(std::move(loaded.error()));
			const auto& result = loaded->get();
			if (result.provider_task_id() != task.provider_task_id ||
				result.task_input_digest() != task.task_input_digest ||
				result.provider_execution_id() != task.provider_execution_id ||
				result.selected_catalog_compile_unit_id() !=
					task.worker_input.selected_catalog_compile_unit ||
				result.final_relation_compile_unit_id() != task.worker_input.compile_unit)
				return sdk::unexpected(claim_error("materialization.task-binding-mismatch",
												   "task-results",
												   "canonical-order-or-execution-binding"));
			if (auto valid = validate_task_side_channels(task, result); !valid)
				return sdk::unexpected(std::move(valid.error()));
			const auto& context = contexts[task_index];
			const auto batches = result.provider_seal().batches();
			for (std::size_t batch_index{}; batch_index < batches.size(); ++batch_index)
			{
				const auto rows = batches[batch_index].rows();
				output_nonempty[task_index][batch_index] = !rows.empty();
				for (std::size_t row_index{}; row_index < rows.size(); ++row_index)
				{
					const auto& row = rows[row_index];
					const auto row_digest = digest_text(row.canonical_form());
					auto provenance = worker_provenance(row.descriptor_id, context, row_digest);
					if (!provenance)
						return sdk::unexpected(std::move(provenance.error()));
					std::optional<std::string> source_evidence;
					if (batch_index >= 3U)
					{
						const auto decoded =
							std::ranges::find_if(result.observation_rows(),
												 [&](const sealed_observation_v2_row& value)
												 {
													 return value.batch_index == batch_index &&
														 value.row_index == row_index;
												 });
						if (decoded == result.observation_rows().end())
							return sdk::unexpected(claim_error("materialization.claim-invalid",
															   "observation-row",
															   "decoded-binding-missing"));
						if (decoded->observation.primary_span)
						{
							auto span = span_bundle_digest(*decoded->observation.primary_span);
							if (!span)
								return sdk::unexpected(std::move(span.error()));
							source_evidence = std::move(*span);
						}
						else if (decoded->observation.limitation)
							source_evidence = digest_text(*decoded->observation.limitation);
					}
					const std::optional<std::string> transform = batch_index < 3U
						? std::optional<std::string>{basis->canonical_transform}
						: std::nullopt;
					if (auto recorded = record_claim(row,
													 context,
													 *provenance,
													 worker_producer,
													 transform,
													 row_digest,
													 source_evidence,
													 false);
						!recorded)
						return sdk::unexpected(std::move(recorded.error()));
				}
			}

			const auto base_rows = result.base_claim_rows();
			if (base_rows.size() != base_descriptor_ids.size() - 1U)
				return sdk::unexpected(
					claim_error("materialization.claim-invalid", "base-rows", "exact-five"));
			for (std::size_t index{}; index < base_rows.size(); ++index)
			{
				const auto& row = base_rows[index];
				if (row.descriptor_id != base_descriptor_ids[index])
					return sdk::unexpected(claim_error(
						"materialization.claim-invalid", "base-rows", "dependency-order"));
				auto row_digest = base_row_digest(request.engine, row);
				if (!row_digest)
					return sdk::unexpected(std::move(row_digest.error()));
				auto evidence = base_evidence_for(request_view, task, row, *row_digest);
				if (!evidence)
					return sdk::unexpected(std::move(evidence.error()));
				if (auto recorded = record_claim(row,
												 context,
												 task.task_input_digest,
												 materializer_producer,
												 basis->base_transform,
												 *row_digest,
												 *evidence,
												 true);
					!recorded)
					return sdk::unexpected(std::move(recorded.error()));
				base_seen.emplace(context_tuple(context), row.descriptor_id);
			}

			std::map<std::string, const sdk::detached_row*, std::less<>> span_rows;
			for (const auto& row : result.source_span_claim_rows())
			{
				if (row.descriptor_id != base_descriptor_ids.back())
					return sdk::unexpected(claim_error(
						"materialization.span-invalid", "source-span-row", "descriptor"));
				auto span = required_row_string(row, "source.span.v1.span");
				if (!span)
					return sdk::unexpected(std::move(span.error()));
				if (!span_rows.emplace(*span, &row).second)
					return sdk::unexpected(claim_error(
						"materialization.span-invalid", "source-span-row", "duplicate-identity"));
			}
			std::set<std::string, std::less<>> used_spans;
			for (const auto& decoded : result.observation_rows())
			{
				if (!decoded.observation.primary_span)
					continue;
				const auto& span = *decoded.observation.primary_span;
				const auto found = span_rows.find(span.span_id);
				if (found == span_rows.end())
					return sdk::unexpected(claim_error(
						"materialization.span-invalid", "source-span-row", "bundle-row-missing"));
				const auto provider_batches = result.provider_seal().batches();
				if (decoded.batch_index >= provider_batches.size() ||
					decoded.row_index >= provider_batches[decoded.batch_index].rows().size())
					return sdk::unexpected(claim_error(
						"materialization.claim-invalid", "observation-row", "sealed-index"));
				const auto& observation_row =
					provider_batches[decoded.batch_index].rows()[decoded.row_index];
				const auto observation_digest = digest_text(observation_row.canonical_form());
				auto bundle = span_bundle_digest(span);
				if (!bundle)
					return sdk::unexpected(std::move(bundle.error()));
				auto row_digest = base_row_digest(request.engine, *found->second);
				if (!row_digest)
					return sdk::unexpected(std::move(row_digest.error()));
				auto evidence = span_source_evidence(observation_digest, *bundle);
				if (!evidence)
					return sdk::unexpected(std::move(evidence.error()));
				if (auto recorded = record_claim(*found->second,
												 context,
												 *bundle,
												 materializer_producer,
												 basis->base_transform,
												 *row_digest,
												 *evidence,
												 true);
					!recorded)
					return sdk::unexpected(std::move(recorded.error()));
				used_spans.insert(span.span_id);
				base_seen.emplace(context_tuple(context), std::string{base_descriptor_ids.back()});
			}
			if (used_spans.size() != span_rows.size())
				return sdk::unexpected(
					claim_error("materialization.span-invalid", "source-span-row", "orphan"));
		}

		auto committed = std::move(batch).commit(request.engine);
		if (!committed)
			return sdk::unexpected(claim_error("materialization.claim-invalid",
											   "complete-final-claim-batch",
											   nested_error(committed.error())));
		if (!committed->unresolved.empty() || !committed->conflicts.empty() ||
			!committed->differential_disagreements.empty())
		{
			return sdk::unexpected(claim_error("materialization.claim-invalid",
											   "complete-final-claim-batch",
											   "nonzero-unresolved-conflict-or-differential"));
		}

		std::map<std::string, const sdk::claim*, std::less<>> committed_claims_by_ref;
		for (const auto& claim : committed->claims)
		{
			auto envelope = make_envelope("stored_final", claim);
			if (!envelope)
				return sdk::unexpected(std::move(envelope.error()));
			auto [entry, inserted] = committed_claims_by_ref.emplace(envelope->claim_ref, &claim);
			if (!inserted && entry->second != &claim)
				return sdk::unexpected(claim_error("materialization.claim-invalid",
												   "complete-final-claim-batch",
												   "duplicate-claim-ref"));
		}

		std::map<partition_key, partition_accumulator> partition_groups;
		const auto partition_for = [&](const std::string_view descriptor_id,
									   const materialization_semantic_task_context& context,
									   std::string producer_semantics,
									   std::string producer_basis,
									   const bool empty,
									   const bool base) -> sdk::result<partition_accumulator*>
		{
			sdk::partition_draft identity_draft;
			identity_draft.relation_descriptor_id = descriptor_id;
			identity_draft.scope = guarantee.scope;
			identity_draft.condition = condition_for(context);
			identity_draft.interpretation = context.interpretation_domain;
			identity_draft.producer_semantics = std::move(producer_semantics);
			identity_draft.producer_input_basis_digest = std::move(producer_basis);
			identity_draft.precision_profile = guarantee.approximation;
			identity_draft.assumption_set_id = guarantee.assumptions;
			if (auto valid = identity_draft.condition.validate(); !valid)
				return sdk::unexpected(claim_error("materialization.claim-invalid",
												   "partition.condition",
												   nested_error(valid.error())));
			const auto key = partition_identity_fields(identity_draft);
			auto [entry, inserted] = partition_groups.try_emplace(key);
			if (inserted)
			{
				entry->second.draft = std::move(identity_draft);
				entry->second.empty = empty;
			}
			else if (entry->second.empty != empty)
				return sdk::unexpected(claim_error(
					"materialization.claim-invalid", "partition", "empty-nonempty-identity-alias"));
			auto coverage = coverage_units(context, descriptor_id, base);
			if (!coverage)
				return sdk::unexpected(std::move(coverage.error()));
			for (auto& unit : *coverage)
			{
				if (auto valid = unit.validate(); !valid)
					return sdk::unexpected(std::move(valid.error()));
				const auto canonical = unit.canonical_form();
				auto [found, added] = entry->second.coverage.emplace(canonical, unit);
				if (!added && found->second != unit)
					return sdk::unexpected(claim_error("materialization.claim-invalid",
													   "partition.coverage",
													   "identity-collision"));
			}
			return &entry->second;
		};

		for (const auto& occurrence : occurrences)
		{
			const auto committed_claim = committed_claims_by_ref.find(occurrence.claim_ref);
			if (committed_claim == committed_claims_by_ref.end())
				return sdk::unexpected(claim_error("materialization.claim-invalid",
												   "complete-final-claim-batch",
												   "occurrence-union"));
			const auto& claim_value = *committed_claim->second;
			auto producer_basis = sdk::claim_input_basis_digest(claim_value.input_basis);
			if (!producer_basis)
				return sdk::unexpected(claim_error("materialization.claim-invalid",
												   claim_value.descriptor,
												   nested_error(producer_basis.error())));
			auto partition = partition_for(claim_value.descriptor,
										   occurrence.context,
										   claim_value.producer.semantic_contract,
										   std::move(*producer_basis),
										   false,
										   occurrence.base);
			if (!partition)
				return sdk::unexpected(std::move(partition.error()));
			auto [claim, inserted] =
				(*partition)->claims_by_ref.emplace(occurrence.claim_ref, claim_value);
			if (!inserted && !same_claim(claim->second, claim_value))
				return sdk::unexpected(claim_error("materialization.claim-invalid",
												   "partition.claim-ref",
												   "aliases-different-occurrence"));
			(*partition)->claim_contents.insert(claim_value.content);
		}

		for (std::size_t task_index{}; task_index < contexts.size(); ++task_index)
		{
			const auto& context = contexts[task_index];
			for (std::size_t descriptor_index{}; descriptor_index < output_descriptor_ids.size();
				 ++descriptor_index)
			{
				if (output_nonempty[task_index][descriptor_index])
					continue;
				const bool canonical = descriptor_index < 3U;
				const std::string producer =
					canonical ? basis->materializer_semantics : std::string{*worker_semantics};
				const std::string transform = canonical ? basis->canonical_transform : producer;
				const auto condition = condition_for(context);
				auto empty_basis = empty_partition_basis(basis->direct_basis,
														 output_descriptor_ids[descriptor_index],
														 condition,
														 context.interpretation_domain,
														 producer,
														 transform);
				if (!empty_basis)
					return sdk::unexpected(std::move(empty_basis.error()));
				auto partition = partition_for(output_descriptor_ids[descriptor_index],
											   context,
											   producer,
											   std::move(*empty_basis),
											   true,
											   false);
				if (!partition)
					return sdk::unexpected(std::move(partition.error()));
			}

			for (const auto descriptor_id : base_descriptor_ids)
			{
				if (base_seen.contains({context_tuple(context), std::string{descriptor_id}}))
					continue;
				const auto condition = condition_for(context);
				auto empty_basis = empty_partition_basis(basis->direct_basis,
														 descriptor_id,
														 condition,
														 context.interpretation_domain,
														 basis->materializer_semantics,
														 basis->base_transform);
				if (!empty_basis)
					return sdk::unexpected(std::move(empty_basis.error()));
				auto partition = partition_for(descriptor_id,
											   context,
											   basis->materializer_semantics,
											   std::move(*empty_basis),
											   true,
											   true);
				if (!partition)
					return sdk::unexpected(std::move(partition.error()));
			}
		}

		std::map<std::string, std::uint64_t, std::less<>> association_count_by_ref;
		for (const auto& [association_id, association] : associations)
		{
			(void)association_id;
			++association_count_by_ref[association.stored_claim_ref];
		}
		std::vector<materialization_claim_partition> partitions;
		partitions.reserve(partition_groups.size());
		std::set<std::string, std::less<>> partition_final_refs;
		for (auto& [key, accumulator] : partition_groups)
		{
			(void)key;
			for (const auto& [claim_ref, claim] : accumulator.claims_by_ref)
			{
				accumulator.draft.claims.push_back(claim);
				partition_final_refs.insert(claim_ref);
			}
			for (const auto& [coverage_id, coverage] : accumulator.coverage)
			{
				(void)coverage_id;
				accumulator.draft.coverage.push_back(coverage);
			}
			auto manifest = sdk::make_partition_manifest(request.engine, accumulator.draft);
			if (!manifest)
				return sdk::unexpected(claim_error("materialization.claim-invalid",
												   "partition-manifest",
												   nested_error(manifest.error())));
			std::vector<std::string> refs;
			refs.reserve(accumulator.claims_by_ref.size());
			std::uint64_t association_count{};
			for (const auto& [claim_ref, claim] : accumulator.claims_by_ref)
			{
				(void)claim;
				refs.push_back(claim_ref);
				association_count += association_count_by_ref[claim_ref];
			}
			std::vector<std::string> contents{accumulator.claim_contents.begin(),
											  accumulator.claim_contents.end()};
			if (manifest->claim_count != contents.size() ||
				manifest->complete != !accumulator.coverage.empty())
				return sdk::unexpected(claim_error("materialization.claim-invalid",
												   "partition-manifest",
												   "census-or-completeness"));
			const sdk::snapshot_partition_binding binding{
				manifest->partition_id,
				accumulator.draft.relation_descriptor_id,
				accumulator.draft.scope,
				accumulator.draft.condition,
				accumulator.draft.interpretation,
				accumulator.draft.producer_semantics,
				accumulator.draft.producer_input_basis_digest,
				accumulator.draft.precision_profile,
				accumulator.draft.assumption_set_id,
			};
			partitions.push_back({std::move(accumulator.draft),
								  std::move(*manifest),
								  binding,
								  std::move(refs),
								  std::move(contents),
								  static_cast<std::uint64_t>(accumulator.claims_by_ref.size()),
								  association_count,
								  accumulator.empty});
		}
		std::ranges::sort(partitions,
						  [](const materialization_claim_partition& left,
							 const materialization_claim_partition& right)
						  {
							  return left.manifest.partition_id < right.manifest.partition_id;
						  });

		std::set<std::string, std::less<>> final_refs;
		std::set<std::string, std::less<>> hidden_refs;
		for (const auto& [claim_ref, envelope] : envelopes)
			(envelope.role == "stored_final" ? final_refs : hidden_refs).insert(claim_ref);
		if (partition_final_refs != final_refs)
			return sdk::unexpected(
				claim_error("materialization.claim-invalid", "partition-final-union", "not-exact"));
		for (const auto& claim_ref : final_refs)
			if (!association_count_by_ref.contains(claim_ref))
				return sdk::unexpected(claim_error(
					"materialization.claim-invalid", "origin-association", "stored-final-orphan"));
		for (const auto& [claim_ref, count] : association_count_by_ref)
			if (!final_refs.contains(claim_ref) || count == 0U)
				return sdk::unexpected(claim_error(
					"materialization.claim-invalid", "origin-association", "nonfinal-or-empty"));

		std::set<std::string, std::less<>> edge_precursors;
		std::set<std::string, std::less<>> edge_finals;
		for (const auto& [edge_key, edge] : edges)
		{
			(void)edge_key;
			if (!edge_precursors.insert(edge.precursor_claim_ref).second ||
				!edge_finals.insert(edge.final_claim_ref).second)
				return sdk::unexpected(claim_error(
					"materialization.claim-invalid", "canonicalization-edge", "not-one-to-one"));
		}
		if (edge_precursors != hidden_refs)
			return sdk::unexpected(claim_error(
				"materialization.claim-invalid", "canonicalization-edge", "hidden-coverage"));
		std::set<std::string, std::less<>> canonical_final_refs;
		for (const auto& claim_ref : final_refs)
			if (envelopes.at(claim_ref).value.stage == sdk::claim_stage::canonical_claim)
				canonical_final_refs.insert(claim_ref);
		if (edge_finals != canonical_final_refs)
			return sdk::unexpected(claim_error(
				"materialization.claim-invalid", "canonicalization-edge", "stored-final-coverage"));

		std::set<std::string, std::less<>> committed_refs;
		for (const auto& claim : committed->claims)
		{
			auto envelope = make_envelope("stored_final", claim);
			if (!envelope)
				return sdk::unexpected(std::move(envelope.error()));
			committed_refs.insert(std::move(envelope->claim_ref));
		}
		if (committed_refs != final_refs || committed->claims.size() != final_refs.size())
			return sdk::unexpected(claim_error(
				"materialization.claim-invalid", "complete-final-claim-batch", "occurrence-union"));

		std::vector<materialization_claim_envelope> envelope_values;
		envelope_values.reserve(envelopes.size());
		for (auto& [claim_ref, envelope] : envelopes)
		{
			(void)claim_ref;
			envelope_values.push_back(std::move(envelope));
		}
		std::vector<materialization_canonicalization_edge> edge_values;
		edge_values.reserve(edges.size());
		for (auto& [edge_key, edge] : edges)
		{
			(void)edge_key;
			edge_values.push_back(std::move(edge));
		}
		std::vector<materialization_origin_association> association_values;
		association_values.reserve(associations.size());
		for (auto& [association_id, association] : associations)
		{
			(void)association_id;
			association_values.push_back(std::move(association));
		}

		return sealed_materialization_claims{basis->materializer_semantics,
											 basis->direct_basis,
											 basis->canonical_transform,
											 basis->base_transform,
											 assumption_set,
											 std::move(*committed),
											 std::move(envelope_values),
											 std::move(edge_values),
											 std::move(association_values),
											 std::move(partitions)};
	}

	sdk::result<materialization_bounded_task_claims>
	construct_materialization_bounded_task_claims_impl(
		const materialization_claim_request_view& request,
		const std::size_t task_index,
		const sealed_materialization_result& result,
		const materialization_producer_authority* const producer_authority,
		const materialization_guarantee_authority* const guarantee_authority,
		const materialization_v2_1_claim_authority* const v2_authority)
	{
		try
		{
			if (request.tasks.empty() || (!v2_authority && task_index >= request.tasks.size()) ||
				(v2_authority && request.tasks.size() != 1U))
				return sdk::unexpected(
					claim_error("materialization.task-binding-mismatch", "task-results", "index"));
			if (auto valid = request.catalog.validate(); !valid)
				return sdk::unexpected(claim_error("materialization.claim-invalid",
												   "project-catalog",
												   nested_error(valid.error())));

			// The direct basis is request-wide semantic authority, but only the current sealed
			// result is retained below. Context metadata is small and bounded by request admission;
			// claim rows, envelopes, and occurrence payloads never enter this vector.
			std::vector<materialization_semantic_task_context> contexts;
			contexts.reserve(v2_authority ? 1U : request.tasks.size());
			std::set<context_key> unique_contexts;
			for (const auto& task : request.tasks)
			{
				if (task.source_receipt)
				{
					if (!task.worker_payload.empty() || !task.worker_input.source.empty() ||
						!task.worker_input.source_content_base64.empty())
						return sdk::unexpected(claim_error("materialization.task-binding-mismatch",
														   "task.v3",
														   "streaming-residency-violation"));
					if (auto valid = task.worker_input.validate_with_catalog(request.catalog,
																			 *task.source_receipt);
						!valid)
						return sdk::unexpected(claim_error("materialization.task-binding-mismatch",
														   "task.v3",
														   nested_error(valid.error())));
				}
				else
				{
					if (auto valid = task.worker_input.validate(); !valid)
						return sdk::unexpected(claim_error("materialization.task-binding-mismatch",
														   "task.v3",
														   nested_error(valid.error())));
					auto encoded_task = encode_task_input(task.worker_input);
					if (!encoded_task || *encoded_task != task.worker_payload ||
						sdk::content_digest(task.worker_payload) != task.task_input_digest)
						return sdk::unexpected(claim_error("materialization.task-binding-mismatch",
														   "task.v3",
														   "payload-or-digest-rebinding"));
				}
				auto context = task_context(task);
				if (!unique_contexts.insert(context_tuple(context)).second)
					return sdk::unexpected(claim_error("materialization.task-binding-mismatch",
													   "semantic-task-context",
													   "duplicate"));
				contexts.push_back(std::move(context));
			}

			std::optional<direct_basis_values> basis;
			std::optional<std::pair<sdk::claim_guarantee, std::string>> guarantee_values;
			if (v2_authority)
			{
				if (v2_authority->catalog() != &request.catalog ||
					v2_authority->engine() != &request.engine)
					return sdk::unexpected(claim_error(
						"materialization.identity-mismatch", "claim-authority", "request-owner"));
				basis.emplace(v2_authority->materializer_semantics_digest(),
							  v2_authority->direct_basis_digest(),
							  v2_authority->canonical_adoption_transform_digest(),
							  v2_authority->base_ingestion_transform_digest());
				guarantee_values.emplace(v2_authority->guarantee(),
										 v2_authority->assumption_set_id());
			}
			else
			{
				if (producer_authority == nullptr || guarantee_authority == nullptr)
					return sdk::unexpected(claim_error("materialization.claim-invalid",
													   "claim-authority",
													   "legacy-authority-missing"));
				auto legacy_basis = make_direct_basis(
					materialization_claim_request_view{
						request.catalog, request.engine, request.tasks, request.document_root},
					*producer_authority,
					contexts);
				if (!legacy_basis)
					return sdk::unexpected(std::move(legacy_basis.error()));
				basis.emplace(std::move(*legacy_basis));
				auto legacy_guarantee = validate_guarantee(
					materialization_claim_request_view{
						request.catalog, request.engine, request.tasks, request.document_root},
					*guarantee_authority);
				if (!legacy_guarantee)
					return sdk::unexpected(std::move(legacy_guarantee.error()));
				guarantee_values.emplace(std::move(*legacy_guarantee));
			}
			const auto& guarantee = guarantee_values->first;
			const auto& assumption_set = guarantee_values->second;

			std::string worker_id;
			std::string worker_semantics;
			if (v2_authority)
			{
				worker_id = v2_authority->worker_provider_id();
				worker_semantics = v2_authority->worker_semantic_contract_digest();
			}
			else
			{
				if (request.document_root == nullptr)
					return sdk::unexpected(
						claim_error("materialization.identity-mismatch", "worker", "missing"));
				const auto* worker = request.document_root->member("worker");
				if (worker == nullptr)
					return sdk::unexpected(
						claim_error("materialization.identity-mismatch", "worker", "missing"));
				auto worker_id_value = json_text(*worker, "provider_id", "worker.provider_id");
				auto worker_semantics_value = json_text(
					*worker, "semantic_contract_digest", "worker.semantic_contract_digest");
				if (!worker_id_value || !worker_semantics_value)
					return sdk::unexpected(!worker_id_value
											   ? std::move(worker_id_value.error())
											   : std::move(worker_semantics_value.error()));
				worker_id = std::string{*worker_id_value};
				worker_semantics = std::string{*worker_semantics_value};
			}
			const sdk::claim_producer worker_producer{worker_id, worker_semantics};
			const sdk::claim_producer materializer_producer{"cxxlens.clang22.materializer",
															basis->materializer_semantics};

			const auto& task = request.tasks[v2_authority ? 0U : task_index];
			if (result.provider_task_id() != task.provider_task_id ||
				result.task_input_digest() != task.task_input_digest ||
				result.provider_execution_id() != task.provider_execution_id ||
				result.selected_catalog_compile_unit_id() !=
					task.worker_input.selected_catalog_compile_unit ||
				result.final_relation_compile_unit_id() != task.worker_input.compile_unit)
				return sdk::unexpected(claim_error("materialization.task-binding-mismatch",
												   "task-results",
												   "canonical-order-or-execution-binding"));
			if (auto valid = validate_task_side_channels(task, result); !valid)
				return sdk::unexpected(std::move(valid.error()));
			const auto& context = contexts[v2_authority ? 0U : task_index];

			std::map<std::string, materialization_claim_envelope, std::less<>> envelopes;
			std::map<std::string, std::pair<std::string, std::string>, std::less<>>
				claim_rows_by_ref;
			std::map<std::tuple<std::string, std::string, std::string>,
					 materialization_canonicalization_edge>
				edges;
			std::map<std::string, materialization_origin_association, std::less<>> associations;
			std::array<bool, 6U> output_nonempty{};
			std::set<std::string, std::less<>> base_seen;
			std::map<partition_key, partition_accumulator> partition_groups;

			const auto partition_for = [&](const std::string_view descriptor_id,
										   const materialization_semantic_task_context& owner,
										   std::string producer_semantics,
										   std::string producer_basis,
										   const bool empty,
										   const bool base) -> sdk::result<partition_accumulator*>
			{
				sdk::partition_draft identity_draft;
				identity_draft.relation_descriptor_id = descriptor_id;
				identity_draft.scope = guarantee.scope;
				identity_draft.condition = condition_for(owner);
				identity_draft.interpretation = owner.interpretation_domain;
				identity_draft.producer_semantics = std::move(producer_semantics);
				identity_draft.producer_input_basis_digest = std::move(producer_basis);
				identity_draft.precision_profile = guarantee.approximation;
				identity_draft.assumption_set_id = guarantee.assumptions;
				if (auto valid = identity_draft.condition.validate(); !valid)
					return sdk::unexpected(claim_error("materialization.claim-invalid",
													   "partition.condition",
													   nested_error(valid.error())));
				const auto key = partition_identity_fields(identity_draft);
				auto [entry, inserted] = partition_groups.try_emplace(key);
				if (inserted)
				{
					entry->second.draft = std::move(identity_draft);
					entry->second.empty = empty;
				}
				else if (entry->second.empty != empty)
					return sdk::unexpected(claim_error("materialization.claim-invalid",
													   "partition",
													   "empty-nonempty-identity-alias"));
				auto coverage = coverage_units(owner, descriptor_id, base);
				if (!coverage)
					return sdk::unexpected(std::move(coverage.error()));
				for (auto& unit : *coverage)
				{
					if (auto valid = unit.validate(); !valid)
						return sdk::unexpected(claim_error("materialization.claim-invalid",
														   "partition.coverage",
														   nested_error(valid.error())));
					const auto canonical = unit.canonical_form();
					auto [found, added] = entry->second.coverage.emplace(canonical, unit);
					if (!added && found->second != unit)
						return sdk::unexpected(claim_error("materialization.claim-invalid",
														   "partition.coverage",
														   "identity-collision"));
				}
				return &entry->second;
			};

			const auto record_claim = [&](const sdk::detached_row& row,
										  const std::string& provenance_root,
										  const sdk::claim_producer& precursor_producer,
										  const std::optional<std::string>& transform,
										  const std::string& sealed_row_digest,
										  const std::optional<std::string>& source_evidence_digest,
										  const bool base) -> sdk::result<void>
			{
				auto assertion = sdk::make_assertion(
					request.engine,
					sdk::observation{row,
									 condition_for(context),
									 context.interpretation_domain,
									 precursor_producer,
									 sdk::direct_claim_basis{basis->direct_basis},
									 provenance_root,
									 guarantee});
				if (!assertion)
					return sdk::unexpected(claim_error("materialization.claim-invalid",
													   row.descriptor_id,
													   nested_error(assertion.error())));
				sdk::claim final = *assertion;
				std::string final_ref;
				if (transform)
				{
					auto hidden = make_envelope("hidden_precursor", *assertion);
					if (!hidden)
						return sdk::unexpected(std::move(hidden.error()));
					auto hidden_ref =
						add_envelope(envelopes, claim_rows_by_ref, std::move(*hidden));
					if (!hidden_ref)
						return sdk::unexpected(std::move(hidden_ref.error()));
					auto canonical = sdk::make_canonical_claim(
						request.engine, *assertion, materializer_producer, row, *transform);
					if (!canonical)
						return sdk::unexpected(claim_error("materialization.claim-invalid",
														   row.descriptor_id,
														   nested_error(canonical.error())));
					final = std::move(*canonical);
					auto final_envelope = make_envelope("stored_final", final);
					if (!final_envelope)
						return sdk::unexpected(std::move(final_envelope.error()));
					final_ref = final_envelope->claim_ref;
					auto stored =
						add_envelope(envelopes, claim_rows_by_ref, std::move(*final_envelope));
					if (!stored)
						return sdk::unexpected(std::move(stored.error()));
					materialization_canonicalization_edge edge{*hidden_ref, final_ref, *transform};
					auto edge_key = std::tuple{
						edge.precursor_claim_ref, edge.final_claim_ref, edge.transform_semantics};
					auto [found, inserted] = edges.emplace(edge_key, edge);
					if (!inserted && found->second != edge)
						return sdk::unexpected(claim_error("materialization.claim-invalid",
														   "canonicalization-edge",
														   "identity-collision"));
				}
				else
				{
					auto final_envelope = make_envelope("stored_final", final);
					if (!final_envelope)
						return sdk::unexpected(std::move(final_envelope.error()));
					final_ref = final_envelope->claim_ref;
					auto stored =
						add_envelope(envelopes, claim_rows_by_ref, std::move(*final_envelope));
					if (!stored)
						return sdk::unexpected(std::move(stored.error()));
				}

				if (auto valid = sdk::validate_claim(request.engine, final); !valid)
					return sdk::unexpected(claim_error("materialization.claim-invalid",
													   row.descriptor_id,
													   nested_error(valid.error())));
				auto association =
					make_association(final_ref, context, sealed_row_digest, source_evidence_digest);
				if (!association)
					return sdk::unexpected(std::move(association.error()));
				auto [association_entry, association_inserted] =
					associations.emplace(association->association_id, *association);
				if (!association_inserted && association_entry->second != *association)
					return sdk::unexpected(claim_error("materialization.claim-invalid",
													   "origin-association",
													   "identity-collision"));
				auto producer_basis = sdk::claim_input_basis_digest(final.input_basis);
				if (!producer_basis)
					return sdk::unexpected(std::move(producer_basis.error()));
				auto partition = partition_for(final.descriptor,
											   context,
											   final.producer.semantic_contract,
											   std::move(*producer_basis),
											   false,
											   base);
				if (!partition)
					return sdk::unexpected(std::move(partition.error()));
				auto [claim_entry, claim_inserted] =
					(*partition)->claims_by_ref.emplace(final_ref, final);
				if (!claim_inserted && !same_claim(claim_entry->second, final))
					return sdk::unexpected(claim_error("materialization.claim-invalid",
													   "partition.claim-ref",
													   "aliases-different-occurrence"));
				(*partition)->claim_contents.insert(final.content);
				return {};
			};

			const auto batches = result.provider_seal().batches();
			for (std::size_t batch_index{}; batch_index < batches.size(); ++batch_index)
			{
				const auto rows = batches[batch_index].rows();
				if (batch_index < output_nonempty.size())
					output_nonempty[batch_index] = !rows.empty();
				for (std::size_t row_index{}; row_index < rows.size(); ++row_index)
				{
					const auto& row = rows[row_index];
					const auto row_digest = digest_text(row.canonical_form());
					auto provenance = worker_provenance(row.descriptor_id, context, row_digest);
					if (!provenance)
						return sdk::unexpected(std::move(provenance.error()));
					std::optional<std::string> source_evidence;
					if (batch_index >= 3U)
					{
						const auto decoded =
							std::ranges::find_if(result.observation_rows(),
												 [&](const sealed_observation_v2_row& value)
												 {
													 return value.batch_index == batch_index &&
														 value.row_index == row_index;
												 });
						if (decoded == result.observation_rows().end())
							return sdk::unexpected(claim_error("materialization.claim-invalid",
															   "observation-row",
															   "decoded-binding-missing"));
						if (decoded->observation.primary_span)
						{
							auto span = span_bundle_digest(*decoded->observation.primary_span);
							if (!span)
								return sdk::unexpected(std::move(span.error()));
							source_evidence = std::move(*span);
						}
						else if (decoded->observation.limitation)
							source_evidence = digest_text(*decoded->observation.limitation);
					}
					const std::optional<std::string> transform = batch_index < 3U
						? std::optional<std::string>{basis->canonical_transform}
						: std::nullopt;
					if (auto recorded = record_claim(row,
													 *provenance,
													 worker_producer,
													 transform,
													 row_digest,
													 source_evidence,
													 false);
						!recorded)
						return sdk::unexpected(std::move(recorded.error()));
				}
			}

			const auto base_rows = result.base_claim_rows();
			if (base_rows.size() != base_descriptor_ids.size() - 1U)
				return sdk::unexpected(
					claim_error("materialization.claim-invalid", "base-rows", "exact-five"));
			for (std::size_t index{}; index < base_rows.size(); ++index)
			{
				const auto& row = base_rows[index];
				if (row.descriptor_id != base_descriptor_ids[index])
					return sdk::unexpected(claim_error(
						"materialization.claim-invalid", "base-rows", "dependency-order"));
				auto row_digest = base_row_digest(request.engine, row);
				if (!row_digest)
					return sdk::unexpected(std::move(row_digest.error()));
				auto evidence = base_evidence_for(request, task, row, *row_digest);
				if (!evidence)
					return sdk::unexpected(std::move(evidence.error()));
				if (auto recorded = record_claim(row,
												 task.task_input_digest,
												 materializer_producer,
												 basis->base_transform,
												 *row_digest,
												 *evidence,
												 true);
					!recorded)
					return sdk::unexpected(std::move(recorded.error()));
				base_seen.insert(std::string{row.descriptor_id});
			}

			std::map<std::string, const sdk::detached_row*, std::less<>> span_rows;
			for (const auto& row : result.source_span_claim_rows())
			{
				if (row.descriptor_id != base_descriptor_ids.back())
					return sdk::unexpected(claim_error(
						"materialization.span-invalid", "source-span-row", "descriptor"));
				auto span = required_row_string(row, "source.span.v1.span");
				if (!span)
					return sdk::unexpected(std::move(span.error()));
				if (!span_rows.emplace(*span, &row).second)
					return sdk::unexpected(claim_error(
						"materialization.span-invalid", "source-span-row", "duplicate-identity"));
			}
			std::set<std::string, std::less<>> used_spans;
			for (const auto& decoded : result.observation_rows())
			{
				if (!decoded.observation.primary_span)
					continue;
				const auto& span = *decoded.observation.primary_span;
				const auto found = span_rows.find(span.span_id);
				if (found == span_rows.end())
					return sdk::unexpected(claim_error(
						"materialization.span-invalid", "source-span-row", "bundle-row-missing"));
				if (decoded.batch_index >= batches.size() ||
					decoded.row_index >= batches[decoded.batch_index].rows().size())
					return sdk::unexpected(claim_error(
						"materialization.claim-invalid", "observation-row", "sealed-index"));
				const auto& observation_row =
					batches[decoded.batch_index].rows()[decoded.row_index];
				const auto observation_digest = digest_text(observation_row.canonical_form());
				auto bundle = span_bundle_digest(span);
				if (!bundle)
					return sdk::unexpected(std::move(bundle.error()));
				auto row_digest = base_row_digest(request.engine, *found->second);
				if (!row_digest)
					return sdk::unexpected(std::move(row_digest.error()));
				auto evidence = span_source_evidence(observation_digest, *bundle);
				if (!evidence)
					return sdk::unexpected(std::move(evidence.error()));
				if (auto recorded = record_claim(*found->second,
												 *bundle,
												 materializer_producer,
												 basis->base_transform,
												 *row_digest,
												 *evidence,
												 true);
					!recorded)
					return sdk::unexpected(std::move(recorded.error()));
				used_spans.insert(span.span_id);
				base_seen.insert(std::string{base_descriptor_ids.back()});
			}
			if (used_spans.size() != span_rows.size())
				return sdk::unexpected(
					claim_error("materialization.span-invalid", "source-span-row", "orphan"));

			// Add the six explicit output partitions and six base partitions even when their
			// current task has no rows. The empty basis remains content-bound and is never inferred
			// from a missing map entry.
			for (std::size_t descriptor_index{}; descriptor_index < output_descriptor_ids.size();
				 ++descriptor_index)
			{
				if (output_nonempty[descriptor_index])
					continue;
				const bool canonical = descriptor_index < 3U;
				const std::string producer =
					canonical ? basis->materializer_semantics : worker_semantics;
				const std::string transform = canonical ? basis->canonical_transform : producer;
				auto empty_basis = empty_partition_basis(basis->direct_basis,
														 output_descriptor_ids[descriptor_index],
														 condition_for(context),
														 context.interpretation_domain,
														 producer,
														 transform);
				if (!empty_basis)
					return sdk::unexpected(std::move(empty_basis.error()));
				if (auto partition = partition_for(output_descriptor_ids[descriptor_index],
												   context,
												   producer,
												   std::move(*empty_basis),
												   true,
												   false);
					!partition)
					return sdk::unexpected(std::move(partition.error()));
			}
			for (const auto descriptor_id : base_descriptor_ids)
			{
				if (base_seen.contains(std::string{descriptor_id}))
					continue;
				auto empty_basis = empty_partition_basis(basis->direct_basis,
														 descriptor_id,
														 condition_for(context),
														 context.interpretation_domain,
														 basis->materializer_semantics,
														 basis->base_transform);
				if (!empty_basis)
					return sdk::unexpected(std::move(empty_basis.error()));
				if (auto partition = partition_for(descriptor_id,
												   context,
												   basis->materializer_semantics,
												   std::move(*empty_basis),
												   true,
												   true);
					!partition)
					return sdk::unexpected(std::move(partition.error()));
			}

			// Apply the independent hard/soft reference and functional-conflict validator before
			// any typed partition leaves this task window. The implementation intentionally does
			// not call sdk::claim_batch::commit or import its verdict routine.
			std::vector<const sdk::claim*> reference_space;
			for (auto& [key, accumulator] : partition_groups)
				for (auto& [claim_ref, value] : accumulator.claims_by_ref)
				{
					(void)key;
					(void)claim_ref;
					reference_space.push_back(&value);
				}
			std::ranges::sort(reference_space,
							  [](const sdk::claim* left, const sdk::claim* right)
							  {
								  return left->content < right->content;
							  });
			for (const auto* value : reference_space)
			{
				auto descriptor = request.engine.require_id(value->descriptor);
				if (!descriptor)
					return sdk::unexpected(std::move(descriptor.error()));
				for (const auto& reference : descriptor->descriptor().references)
				{
					const auto absent = [&]()
					{
						return std::ranges::any_of(
							reference.source_columns,
							[&](const auto& column)
							{
								const auto found = value->row.cells.find(column);
								return found == value->row.cells.end() ||
									found->second.state == sdk::cell_state::absent;
							});
					};
					if (absent())
						continue;
					const auto matches = [&](const std::optional<std::string_view> element)
					{
						return std::ranges::any_of(
							reference_space,
							[&](const sdk::claim* target)
							{
								auto target_descriptor =
									request.engine.require_id(target->descriptor);
								if (!target_descriptor ||
									target_descriptor->descriptor().name !=
										reference.target_relation)
									return false;
								if (value->interpretation != target->interpretation ||
									value->presence.universe != target->presence.universe ||
									!std::ranges::includes(target->presence.fragments,
														   value->presence.fragments))
									return false;
								for (std::size_t index{}; index < reference.source_columns.size();
									 ++index)
								{
									const auto left =
										value->row.cells.find(reference.source_columns[index]);
									const auto right =
										target->row.cells.find(reference.target_columns[index]);
									if (left == value->row.cells.end() ||
										right == target->row.cells.end() ||
										left->second.state != sdk::cell_state::present ||
										right->second.state != sdk::cell_state::present ||
										!left->second.value || !right->second.value)
										return false;
									if (reference.container_elements)
									{
										if (!element)
											return false;
										const auto* target_value =
											std::get_if<std::string>(&*right->second.value);
										if (target_value == nullptr || *target_value != *element)
											return false;
									}
									else if (left->second.value != right->second.value)
										return false;
								}
								return true;
							});
					};
					bool resolved{};
					if (reference.container_elements)
					{
						const auto source = value->row.cells.find(reference.source_columns.front());
						std::vector<std::string> elements;
						if (source != value->row.cells.end() && source->second.value)
						{
							if (const auto* encoded =
									std::get_if<std::vector<std::byte>>(&*source->second.value))
							{
								for (std::size_t offset{}; offset < encoded->size();)
								{
									if (encoded->size() - offset < sizeof(std::uint32_t))
										break;
									std::uint32_t length{};
									for (std::size_t byte{}; byte < sizeof(length); ++byte)
										length |= std::to_integer<std::uint32_t>(
													  (*encoded)[offset + byte])
											<< (byte * 8U);
									offset += sizeof(length);
									if (length == 0U || length > encoded->size() - offset)
										break;
									elements.emplace_back(
										reinterpret_cast<const char*>(encoded->data() + offset),
										length);
									offset += length;
								}
							}
						}
						resolved = !elements.empty() &&
							std::ranges::all_of(elements,
												[&](const std::string& element)
												{
													return matches(std::string_view{element});
												});
					}
					else
						resolved = matches(std::nullopt);
					if (!resolved)
						return sdk::unexpected(
							claim_error("materialization.claim-invalid",
										value->descriptor,
										reference.strength == sdk::reference_strength::hard
											? "hard-reference-missing"
											: "soft-reference-unresolved"));
				}
			}

			for (std::size_t left_index{}; left_index < reference_space.size(); ++left_index)
				for (std::size_t right_index = left_index + 1U;
					 right_index < reference_space.size();
					 ++right_index)
				{
					const auto& left = *reference_space[left_index];
					const auto& right = *reference_space[right_index];
					if (left.content == right.content || left.descriptor != right.descriptor ||
						left.semantic_key != right.semantic_key)
						continue;
					auto descriptor = request.engine.require_id(left.descriptor);
					if (!descriptor ||
						descriptor->descriptor().merge != sdk::merge_mode::functional_assertion)
						continue;
					auto left_payload =
						sdk::detail::functional_payload_digest(descriptor->descriptor(), left.row);
					auto right_payload =
						sdk::detail::functional_payload_digest(descriptor->descriptor(), right.row);
					if (!left_payload || !right_payload)
						return sdk::unexpected(!left_payload ? std::move(left_payload.error())
															 : std::move(right_payload.error()));
					if (*left_payload == *right_payload)
						continue;
					auto overlap = left.presence.overlap(right.presence);
					if (!overlap)
						return sdk::unexpected(std::move(overlap.error()));
					if (!overlap->empty())
						return sdk::unexpected(
							claim_error("materialization.claim-invalid",
										left.descriptor,
										left.interpretation == right.interpretation
											? "functional-conflict"
											: "differential-disagreement"));
				}

			std::vector<materialization_claim_partition> partitions;
			partitions.reserve(partition_groups.size());
			std::map<std::string, std::uint64_t, std::less<>> association_count_by_ref;
			for (const auto& [association_id, association] : associations)
			{
				(void)association_id;
				++association_count_by_ref[association.stored_claim_ref];
			}
			for (auto& [key, accumulator] : partition_groups)
			{
				(void)key;
				for (const auto& [claim_ref, claim] : accumulator.claims_by_ref)
				{
					accumulator.draft.claims.push_back(claim);
					(void)claim_ref;
				}
				for (const auto& [coverage_id, coverage] : accumulator.coverage)
				{
					(void)coverage_id;
					accumulator.draft.coverage.push_back(coverage);
				}
				auto manifest = sdk::make_partition_manifest(request.engine, accumulator.draft);
				if (!manifest)
					return sdk::unexpected(claim_error("materialization.claim-invalid",
													   "partition-manifest",
													   nested_error(manifest.error())));
				std::vector<std::string> refs;
				refs.reserve(accumulator.claims_by_ref.size());
				std::uint64_t association_count{};
				for (const auto& [claim_ref, claim] : accumulator.claims_by_ref)
				{
					(void)claim;
					refs.push_back(claim_ref);
					association_count += association_count_by_ref[claim_ref];
				}
				std::vector<std::string> contents{accumulator.claim_contents.begin(),
												  accumulator.claim_contents.end()};
				if (manifest->claim_count != contents.size() ||
					manifest->complete != !accumulator.coverage.empty())
					return sdk::unexpected(claim_error("materialization.claim-invalid",
													   "partition-manifest",
													   "census-or-completeness"));
				const sdk::snapshot_partition_binding binding{
					manifest->partition_id,
					accumulator.draft.relation_descriptor_id,
					accumulator.draft.scope,
					accumulator.draft.condition,
					accumulator.draft.interpretation,
					accumulator.draft.producer_semantics,
					accumulator.draft.producer_input_basis_digest,
					accumulator.draft.precision_profile,
					accumulator.draft.assumption_set_id,
				};
				partitions.push_back({std::move(accumulator.draft),
									  std::move(*manifest),
									  binding,
									  std::move(refs),
									  std::move(contents),
									  static_cast<std::uint64_t>(accumulator.claims_by_ref.size()),
									  association_count,
									  accumulator.empty});
			}
			std::ranges::sort(partitions,
							  [](const materialization_claim_partition& left,
								 const materialization_claim_partition& right)
							  {
								  return left.manifest.partition_id < right.manifest.partition_id;
							  });
			if (partitions.empty())
				return sdk::unexpected(
					claim_error("materialization.claim-invalid", "partitions", "empty"));

			std::vector<materialization_claim_envelope> envelope_values;
			envelope_values.reserve(envelopes.size());
			for (auto& [claim_ref, envelope] : envelopes)
			{
				(void)claim_ref;
				envelope_values.push_back(std::move(envelope));
			}
			std::vector<materialization_canonicalization_edge> edge_values;
			edge_values.reserve(edges.size());
			for (auto& [edge_key, edge] : edges)
			{
				(void)edge_key;
				edge_values.push_back(std::move(edge));
			}
			std::vector<materialization_origin_association> association_values;
			association_values.reserve(associations.size());
			for (auto& [association_id, association] : associations)
			{
				(void)association_id;
				association_values.push_back(std::move(association));
			}

			return materialization_bounded_task_claims{basis->materializer_semantics,
													   basis->direct_basis,
													   basis->canonical_transform,
													   basis->base_transform,
													   assumption_set,
													   std::move(envelope_values),
													   std::move(edge_values),
													   std::move(association_values),
													   std::move(partitions)};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(v2_authority ? materialization_admission_no_response()
												: claim_error("materialization.spool-failure",
															  "bounded-claim-window",
															  "allocation"));
		}
	}

	sdk::result<materialization_bounded_task_claims> construct_materialization_bounded_task_claims(
		const validated_materialization_request& request,
		const std::size_t task_index,
		const sealed_materialization_result& result,
		const materialization_producer_authority& producer_authority,
		const materialization_guarantee_authority& guarantee_authority)
	{
		const materialization_claim_request_view request_view{
			request.catalog, request.engine, request.tasks, &request.document.root()};
		return construct_materialization_bounded_task_claims_impl(
			request_view, task_index, result, &producer_authority, &guarantee_authority, nullptr);
	}

	materialization_v2_1_claim_authority::materialization_v2_1_claim_authority(
		std::shared_ptr<materialization_v2_1_request_lifetime> lifetime,
		std::string materialization_request_id,
		const std::uint64_t task_count,
		std::string worker_provider_id,
		std::string worker_semantic_contract_digest,
		std::string materializer_semantics_digest,
		std::string direct_basis_digest,
		std::string canonical_adoption_transform_digest,
		std::string base_ingestion_transform_digest,
		sdk::claim_guarantee guarantee,
		std::string assumption_set_id)
		: lifetime_{std::move(lifetime)},
		  materialization_request_id_{std::move(materialization_request_id)},
		  task_count_{task_count}, worker_provider_id_{std::move(worker_provider_id)},
		  worker_semantic_contract_digest_{std::move(worker_semantic_contract_digest)},
		  materializer_semantics_digest_{std::move(materializer_semantics_digest)},
		  direct_basis_digest_{std::move(direct_basis_digest)},
		  canonical_adoption_transform_digest_{std::move(canonical_adoption_transform_digest)},
		  base_ingestion_transform_digest_{std::move(base_ingestion_transform_digest)},
		  guarantee_{std::move(guarantee)}, assumption_set_id_{std::move(assumption_set_id)}
	{
	}

	validated_materialization_request_v2_1*
	materialization_v2_1_claim_authority::request() const noexcept
	{
		return lifetime_ ? lifetime_->owner() : nullptr;
	}

	const sdk::project_catalog* materialization_v2_1_claim_authority::catalog() const noexcept
	{
		const auto* owner = request();
		return owner ? &owner->request().catalog() : nullptr;
	}

	const sdk::relation_engine* materialization_v2_1_claim_authority::engine() const noexcept
	{
		const auto* owner = request();
		return owner ? &owner->request().engine() : nullptr;
	}

	const std::string&
	materialization_v2_1_claim_authority::materialization_request_id() const noexcept
	{
		return materialization_request_id_;
	}

	std::uint64_t materialization_v2_1_claim_authority::task_count() const noexcept
	{
		return task_count_;
	}

	const std::string& materialization_v2_1_claim_authority::worker_provider_id() const noexcept
	{
		return worker_provider_id_;
	}

	const std::string&
	materialization_v2_1_claim_authority::worker_semantic_contract_digest() const noexcept
	{
		return worker_semantic_contract_digest_;
	}

	const std::string&
	materialization_v2_1_claim_authority::materializer_semantics_digest() const noexcept
	{
		return materializer_semantics_digest_;
	}

	const std::string& materialization_v2_1_claim_authority::direct_basis_digest() const noexcept
	{
		return direct_basis_digest_;
	}

	const std::string&
	materialization_v2_1_claim_authority::canonical_adoption_transform_digest() const noexcept
	{
		return canonical_adoption_transform_digest_;
	}

	const std::string&
	materialization_v2_1_claim_authority::base_ingestion_transform_digest() const noexcept
	{
		return base_ingestion_transform_digest_;
	}

	const sdk::claim_guarantee& materialization_v2_1_claim_authority::guarantee() const noexcept
	{
		return guarantee_;
	}

	const std::string& materialization_v2_1_claim_authority::assumption_set_id() const noexcept
	{
		return assumption_set_id_;
	}

	sdk::result<materialization_v2_1_claim_authority> make_materialization_v2_1_claim_authority(
		validated_materialization_request_v2_1& request,
		const materialization_producer_authority& producer_authority,
		const materialization_guarantee_authority& guarantee_authority)
	{
		try
		{
			const auto& lifetime = request.lifetime_token();
			if (!lifetime || lifetime->owner() != &request)
				return sdk::unexpected(claim_error(
					"materialization.identity-mismatch", "claim-authority", "lifetime"));
			const auto& admitted = request.request();
			if (!guarantee_authority.assumptions.empty() ||
				guarantee_authority.verification_modalities.size() !=
					v2_1_guarantee_modalities.size() ||
				!std::ranges::equal(guarantee_authority.verification_modalities,
									v2_1_guarantee_modalities))
				return sdk::unexpected(claim_error(
					"materialization.claim-invalid", "guarantee.profile", "v2.1-fixed-profile"));
			if (admitted.task_count() == 0U ||
				admitted.task_count() > std::numeric_limits<std::size_t>::max())
				return sdk::unexpected(
					claim_error("materialization.task-binding-mismatch", "tasks", "cardinality"));
			if (auto valid = admitted.catalog().validate(); !valid)
				return sdk::unexpected(claim_error("materialization.claim-invalid",
												   "project-catalog",
												   nested_error(valid.error())));

			std::vector<materialization_semantic_task_context> contexts;
			contexts.reserve(static_cast<std::size_t>(admitted.task_count()));
			std::set<context_key> unique_contexts;
			for (std::uint64_t index{}; index < admitted.task_count(); ++index)
			{
				auto binding = request.task_metadata_binding(index);
				if (!binding)
					return sdk::unexpected(std::move(binding.error()));
				if (binding->metadata.task_index != index || binding->input.source.size() != 0U ||
					binding->input.source_content_base64.size() != 0U ||
					binding->input.project != admitted.project_id())
					return sdk::unexpected(claim_error("materialization.task-binding-mismatch",
													   "task-metadata",
													   "source-project-or-order"));
				auto context = task_context(binding->metadata);
				if (!unique_contexts.insert(context_tuple(context)).second)
					return sdk::unexpected(claim_error("materialization.task-binding-mismatch",
													   "semantic-task-context",
													   "duplicate"));
				contexts.push_back(std::move(context));
			}

			auto basis = make_direct_basis(admitted.tool(),
										   admitted.worker(),
										   admitted.catalog(),
										   admitted.engine(),
										   admitted.project_id(),
										   contexts,
										   producer_authority);
			if (!basis)
				return sdk::unexpected(std::move(basis.error()));
			auto guarantee = validate_guarantee(admitted.project_id(), guarantee_authority);
			if (!guarantee)
				return sdk::unexpected(std::move(guarantee.error()));
			return materialization_v2_1_claim_authority{
				lifetime,
				request.identity().materialization_request_id,
				admitted.task_count(),
				admitted.worker().provider_id,
				admitted.worker().semantic_contract_digest,
				std::move(basis->materializer_semantics),
				std::move(basis->direct_basis),
				std::move(basis->canonical_transform),
				std::move(basis->base_transform),
				std::move(guarantee->first),
				std::move(guarantee->second)};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(materialization_admission_no_response());
		}
	}

	sdk::result<materialization_bounded_task_claims> construct_materialization_bounded_task_claims(
		const materialization_v2_1_claim_authority& authority,
		const std::size_t task_index,
		const materialization_v2_1_task_execution& task,
		const sealed_materialization_result& result)
	{
		try
		{
			if (auto valid = validate_v2_1_task_window(authority, task_index, task); !valid)
				return sdk::unexpected(std::move(valid.error()));

			validated_task_request current{task.input,
										   task.metadata.provider_task_id,
										   task.metadata.provider_execution_id,
										   task.metadata.task_input_digest,
										   task.metadata.sandbox,
										   {},
										   task.source_receipt};
			const std::span<const validated_task_request> current_task{&current, 1U};
			const auto* catalog = authority.catalog();
			const auto* engine = authority.engine();
			if (catalog == nullptr || engine == nullptr)
				return sdk::unexpected(
					claim_error("materialization.identity-mismatch", "claim-authority", "owner"));
			const materialization_claim_request_view request_view{
				*catalog, *engine, current_task, nullptr};
			return construct_materialization_bounded_task_claims_impl(
				request_view, task_index, result, nullptr, nullptr, &authority);
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(materialization_admission_no_response());
		}
	}

	sdk::result<sealed_materialization_claims> construct_materialization_claims(
		const validated_materialization_request& request,
		const std::span<const sealed_materialization_result> task_results,
		const materialization_producer_authority& producer_authority,
		const materialization_guarantee_authority& guarantee_authority)
	{
		if (task_results.size() != request.tasks.size())
			return sdk::unexpected(claim_error(
				"materialization.task-binding-mismatch", "task-results", "exact-request-census"));
		const materialization_task_result_loader load = [task_results](const std::size_t index)
			-> sdk::result<std::reference_wrapper<const sealed_materialization_result>>
		{
			if (index >= task_results.size())
				return sdk::unexpected(claim_error("materialization.task-binding-mismatch",
												   "task-results",
												   "exact-request-census"));
			return std::cref(task_results[index]);
		};
		return construct_materialization_claims_from_loader(
			request, load, producer_authority, guarantee_authority);
	}
} // namespace cxxlens::detail::clang22::materialization
