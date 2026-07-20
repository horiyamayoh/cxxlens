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

#include "materialization_identity.hpp"

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

		struct direct_basis_values
		{
			std::string materializer_semantics;
			std::string direct_basis;
			std::string canonical_transform;
			std::string base_transform;
		};

		struct final_occurrence
		{
			std::string claim_ref;
			sdk::claim value;
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
		validate_producer_authority(const validated_materialization_request& request,
									const materialization_producer_authority& authority)
		{
			const auto* tool = request.document.root().member("tool");
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

		[[nodiscard]] sdk::result<std::pair<sdk::claim_guarantee, std::string>>
		validate_guarantee(const validated_materialization_request& request,
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

		[[nodiscard]] sdk::result<direct_basis_values>
		make_direct_basis(const validated_materialization_request& request,
						  const materialization_producer_authority& authority,
						  const std::span<const materialization_semantic_task_context> contexts)
		{
			auto materializer = validate_producer_authority(request, authority);
			if (!materializer)
				return sdk::unexpected(std::move(materializer.error()));
			const auto& root = request.document.root();
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

			const auto& first = request.tasks.front().worker_input;
			auto basis =
				digest_projection("cxxlens.clang22-direct-materialization-basis.v1",
								  sdk::canonical_value::from_tuple({
									  text("cxxlens.clang22-direct-materialization-basis.v1"),
									  text(*materializer),
									  sdk::canonical_value::from_tuple({
										  text(std::string{*provider_id}),
										  text(std::string{*provider_version}),
										  text(std::string{*worker_semantics}),
										  sdk::canonical_value::from_integer(*protocol_major),
										  sdk::canonical_value::from_integer(*protocol_minor),
									  }),
									  sdk::canonical_value::from_tuple({
										  text(first.project),
										  text(first.project_catalog.catalog_id),
										  text(first.project_catalog.catalog_digest),
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
		catalog_entry_evidence(const validated_task_request& task)
		{
			const auto found = std::ranges::find(task.worker_input.project_catalog.compile_units,
												 task.worker_input.selected_catalog_compile_unit,
												 &sdk::catalog_compile_unit::compile_unit_id);
			if (found == task.worker_input.project_catalog.compile_units.end())
				return sdk::unexpected(claim_error(
					"materialization.task-binding-mismatch", "catalog-entry", "selected-missing"));
			return digest_projection(
				"cxxlens.clang22-catalog-entry-evidence.v1",
				object({{"catalog_compile_unit_id", text(found->compile_unit_id)},
						{"effective_invocation_digest", text(found->effective_invocation_digest)},
						{"source_digest", text(found->source_digest)},
						{"environment_digest", text(found->environment_digest)}}));
		}

		[[nodiscard]] sdk::result<std::string> base_evidence_for(const validated_task_request& task,
																 const sdk::detached_row& row,
																 const std::string_view row_digest)
		{
			if (row.descriptor_id == "build.project.v1")
				return base_source_evidence(
					{{"compile_context", task.worker_input.project_catalog.catalog_digest}});
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
				auto catalog = catalog_entry_evidence(task);
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

	sdk::result<sealed_materialization_claims> construct_materialization_claims(
		const validated_materialization_request& request,
		const std::span<const sealed_materialization_result> task_results,
		const materialization_producer_authority& producer_authority,
		const materialization_guarantee_authority& guarantee_authority)
	{
		if (request.tasks.empty() || task_results.size() != request.tasks.size())
			return sdk::unexpected(claim_error(
				"materialization.task-binding-mismatch", "task-results", "exact-request-census"));

		std::vector<materialization_semantic_task_context> contexts;
		contexts.reserve(request.tasks.size());
		std::set<context_key> unique_contexts;
		for (std::size_t index{}; index < request.tasks.size(); ++index)
		{
			const auto& task = request.tasks[index];
			const auto& result = task_results[index];
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
			auto context = task_context(task);
			if (!unique_contexts.insert(context_tuple(context)).second)
				return sdk::unexpected(claim_error(
					"materialization.task-binding-mismatch", "semantic-task-context", "duplicate"));
			contexts.push_back(std::move(context));
		}

		auto basis = make_direct_basis(request, producer_authority, contexts);
		if (!basis)
			return sdk::unexpected(std::move(basis.error()));
		auto guarantee_values = validate_guarantee(request, guarantee_authority);
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
			occurrences.push_back({std::move(final_ref), std::move(final), context, base});
			return {};
		};

		for (std::size_t task_index{}; task_index < request.tasks.size(); ++task_index)
		{
			const auto& task = request.tasks[task_index];
			const auto& result = task_results[task_index];
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
				auto evidence = base_evidence_for(task, row, *row_digest);
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
			return sdk::unexpected(claim_error("materialization.claim-invalid",
											   "complete-final-claim-batch",
											   "nonzero-unresolved-conflict-or-differential"));

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
			auto producer_basis = sdk::claim_input_basis_digest(occurrence.value.input_basis);
			if (!producer_basis)
				return sdk::unexpected(claim_error("materialization.claim-invalid",
												   occurrence.value.descriptor,
												   nested_error(producer_basis.error())));
			auto partition = partition_for(occurrence.value.descriptor,
										   occurrence.context,
										   occurrence.value.producer.semantic_contract,
										   std::move(*producer_basis),
										   false,
										   occurrence.base);
			if (!partition)
				return sdk::unexpected(std::move(partition.error()));
			auto [claim, inserted] =
				(*partition)->claims_by_ref.emplace(occurrence.claim_ref, occurrence.value);
			if (!inserted && !same_claim(claim->second, occurrence.value))
				return sdk::unexpected(claim_error("materialization.claim-invalid",
												   "partition.claim-ref",
												   "aliases-different-occurrence"));
			(*partition)->claim_contents.insert(occurrence.value.content);
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
} // namespace cxxlens::detail::clang22::materialization
