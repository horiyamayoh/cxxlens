#include "provider_worker_v4_output_normalizer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_entity.hpp>

#include "observation_v2.hpp"

namespace cxxlens::detail::clang22
{
	namespace
	{
		[[nodiscard]] sdk::error
		failure(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool valid_text(const std::string_view value) noexcept
		{
			return !value.empty() && value.find('\0') == std::string_view::npos &&
				sdk::validate_utf8_text(value);
		}

		[[nodiscard]] std::vector<std::byte> bytes(const std::string_view value)
		{
			const auto input = std::as_bytes(std::span{value});
			return {input.begin(), input.end()};
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
			auto output = sdk::detached_cell::typed(std::move(parameter), std::move(value));
			output.type.optional = true;
			return output;
		}

		[[nodiscard]] sdk::detached_cell optional_utf8(std::string value)
		{
			auto output = sdk::detached_cell::utf8(std::move(value));
			output.type.optional = true;
			return output;
		}

		[[nodiscard]] sdk::detached_cell optional_bytes(std::vector<std::byte> value)
		{
			auto output = sdk::detached_cell::bytes(std::move(value));
			output.type.optional = true;
			return output;
		}

		[[nodiscard]] std::string_view
		entity_field(const provider_worker_v4_ast_observation& observation,
					 const std::string_view field)
		{
			const auto found = observation.payload.find(field);
			return found == observation.payload.end() ? std::string_view{} : found->second;
		}

		[[nodiscard]] unsigned
		entity_preference(const provider_worker_v4_ast_observation& observation)
		{
			if (entity_field(observation, "symbol.is_definition") == "true")
				return 0U;
			if (entity_field(observation, "symbol.is_canonical_declaration") == "true")
				return 1U;
			return 2U;
		}

		[[nodiscard]] bool compatible_redeclaration(const provider_worker_v4_ast_observation& left,
													const provider_worker_v4_ast_observation& right)
		{
			return entity_field(left, "symbol.kind") == entity_field(right, "symbol.kind") &&
				entity_field(left, "symbol.signature") == entity_field(right, "symbol.signature");
		}

		[[nodiscard]] bool call_kind_requires_direct_target(const std::string_view kind)
		{
			return kind == "direct_function" || kind == "direct_member" ||
				kind == "virtual_member" || kind == "operator";
		}

		[[nodiscard]] bool call_kind_forbids_direct_target(const std::string_view kind)
		{
			return kind == "indirect_function" || kind == "indirect_member_pointer" ||
				kind == "dependent" || kind == "unresolved";
		}

		[[nodiscard]] std::optional<std::string_view>
		source_span_id(const provider_worker_v4_ast_observation& observation)
		{
			if (!observation.primary_span)
				return std::nullopt;
			return observation.primary_span->span_id;
		}

		[[nodiscard]] std::string
		call_occurrence_class(const provider_worker_v4_ast_observation& observation)
		{
			std::ostringstream output;
			for (const auto value : {
					 std::string_view{observation.compile_unit},
					 source_span_id(observation).value_or(std::string_view{}),
					 entity_field(observation, "call.kind"),
					 entity_field(observation, "call.caller"),
				 })
				output << value.size() << ':' << value;
			return output.str();
		}

		[[nodiscard]] sdk::result<std::string> row_string(const sdk::detached_row& row,
														  const std::string_view column)
		{
			const auto found = row.cells.find(column);
			if (found == row.cells.end() || !found->second.value)
				return sdk::unexpected(
					failure("provider-worker-v4.canonical-row-invalid", std::string{column}));
			const auto* value = std::get_if<std::string>(&*found->second.value);
			if (value == nullptr)
				return sdk::unexpected(failure(
					"provider-worker-v4.canonical-row-invalid", std::string{column}, "not-string"));
			return *value;
		}

		[[nodiscard]] sdk::result<sdk::detached_row>
		entity_row(const provider_worker_v4_ast_observation& observation,
				   const std::string_view toolchain,
				   const bool exact)
		{
			using relation = cc::relations::entity;
			relation::builder builder;
			const auto kind = observation.payload.contains("symbol.kind")
				? observation.payload.at("symbol.kind")
				: "unknown";
			const auto signature = observation.payload.contains("symbol.signature")
				? observation.payload.at("symbol.signature")
				: observation.semantic_key;
			auto structural = sdk::semantic_digest("cc.entity.structural-signature.v1", signature);
			if (!structural)
				return sdk::unexpected(std::move(structural.error()));
			for (auto result : {
					 builder.set<relation::entity_column>(
						 sdk::detached_cell::typed("cc_entity_id", "pending")),
					 builder.set<relation::canonicalization>(
						 symbol_cell(sdk::scalar_kind::closed_symbol,
									 "cc.canonicalization-state/1",
									 exact ? "canonicalized" : "provider_local")),
					 builder.set<relation::kind>(
						 symbol_cell(sdk::scalar_kind::open_symbol, "cc.entity-kind/1", kind)),
					 builder.set<relation::structural_signature_digest>(
						 symbol_cell(sdk::scalar_kind::digest, {}, std::move(*structural))),
					 builder.set<relation::toolchain>(
						 optional_typed("toolchain_context_id", std::string{toolchain})),
					 builder.set<relation::provider_local_key>(
						 optional_bytes(bytes(observation.semantic_key))),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			const auto qualified_name = observation.payload.find("symbol.qualified_name");
			if (qualified_name != observation.payload.end() && !qualified_name->second.empty())
			{
				auto result =
					builder.set<relation::qualified_name>(optional_utf8(qualified_name->second));
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			}
			auto row = std::move(builder).finish();
			if (!row)
				return sdk::unexpected(std::move(row.error()));
			auto identity = sdk::derive_domain_identity(relation::descriptor(), *row);
			if (!identity)
				return sdk::unexpected(std::move(identity.error()));
			row->cells.at("cc.entity.v1.entity") =
				sdk::detached_cell::typed("cc_entity_id", std::move(*identity));
			if (auto valid = sdk::validate_domain_identity(relation::descriptor(), *row); !valid)
				return sdk::unexpected(std::move(valid.error()));
			return row;
		}

		[[nodiscard]] sdk::result<sdk::detached_row>
		call_site_row(const provider_worker_v4_ast_observation& observation,
					  const std::map<std::string, std::string, std::less<>>& entities,
					  const std::uint64_t ordinal)
		{
			if (!observation.primary_span)
				return sdk::unexpected(failure("provider-worker-v4.source-authority-unavailable",
											   observation.semantic_key));
			using relation = cc::relations::call_site;
			relation::builder builder;
			const auto caller = observation.payload.find("call.caller");
			const auto kind = observation.payload.contains("call.kind")
				? observation.payload.at("call.kind")
				: "unknown";
			for (auto result : {
					 builder.set<relation::call>(
						 sdk::detached_cell::typed("cc_call_id", "pending")),
					 builder.set<relation::compile_unit>(
						 sdk::detached_cell::typed("compile_unit_id", observation.compile_unit)),
					 builder.set<relation::kind>(
						 symbol_cell(sdk::scalar_kind::open_symbol, "cc.call-kind/1", kind)),
					 builder.set<relation::source>(sdk::detached_cell::typed(
						 "source_span_id", observation.primary_span->span_id)),
					 builder.set<relation::ordinal>(sdk::detached_cell::unsigned_integer(ordinal)),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			if (caller != observation.payload.end())
			{
				const auto found = entities.find(caller->second);
				if (found != entities.end())
				{
					auto result = builder.set<relation::caller>(
						optional_typed("cc_entity_id", found->second));
					if (!result)
						return sdk::unexpected(std::move(result.error()));
				}
			}
			if (const auto receiver = observation.payload.find("call.receiver_static_type");
				receiver != observation.payload.end() && !receiver->second.empty())
			{
				auto result = builder.set<relation::receiver_static_type>(
					optional_typed("cc_type_id", receiver->second));
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			}
			auto row = std::move(builder).finish();
			if (!row)
				return sdk::unexpected(std::move(row.error()));
			auto identity = sdk::derive_domain_identity(relation::descriptor(), *row);
			if (!identity)
				return sdk::unexpected(std::move(identity.error()));
			row->cells.at("cc.call_site.v1.call") =
				sdk::detached_cell::typed("cc_call_id", std::move(*identity));
			if (auto valid = sdk::validate_domain_identity(relation::descriptor(), *row); !valid)
				return sdk::unexpected(std::move(valid.error()));
			return row;
		}

		[[nodiscard]] sdk::result<sdk::detached_row>
		direct_target_row(const std::string_view call, const std::string_view target)
		{
			using relation = cc::relations::call_direct_target;
			relation::builder builder;
			for (auto result : {
					 builder.set<relation::call>(
						 sdk::detached_cell::typed("cc_call_id", std::string{call})),
					 builder.set<relation::target>(
						 sdk::detached_cell::typed("cc_entity_id", std::string{target})),
					 builder.set<relation::resolution>(symbol_cell(sdk::scalar_kind::open_symbol,
																   "cc.direct-target-resolution/1",
																   "syntactic_direct")),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			return std::move(builder).finish();
		}

		[[nodiscard]] sdk::result<provider_worker_v4_ast_observation>
		direct_callee_observation(const provider_worker_v4_ast_observation& call)
		{
			const auto key = call.payload.find("call.direct_callee");
			const auto kind = call.payload.find("call.direct_callee_kind");
			const auto signature = call.payload.find("call.direct_callee_signature");
			if (key == call.payload.end() || key->second.empty() || kind == call.payload.end() ||
				kind->second.empty() || signature == call.payload.end() ||
				signature->second.empty())
				return sdk::unexpected(
					failure("provider.direct-target-unresolved", call.semantic_key, "projection"));
			provider_worker_v4_ast_observation output;
			output.kind = provider_worker_v4_ast_observation_kind::entity;
			output.compile_unit = call.compile_unit;
			output.semantic_key = key->second;
			output.payload.emplace("symbol.kind", kind->second);
			output.payload.emplace("symbol.signature", signature->second);
			if (const auto confidence = call.payload.find("call.direct_callee_identity_confidence");
				confidence != call.payload.end() && !confidence->second.empty())
				output.payload.emplace("symbol.identity_confidence", confidence->second);
			if (const auto name = call.payload.find("call.direct_callee_qualified_name");
				name != call.payload.end() && !name->second.empty())
				output.payload.emplace("symbol.qualified_name", name->second);
			output.exact_equivalence = call.exact_equivalence;
			output.limitation = call.limitation;
			return output;
		}

		void sort_unique(std::vector<std::string>& values)
		{
			std::ranges::sort(values);
			values.erase(std::ranges::unique(values).begin(), values.end());
		}

		void add_limitation(std::vector<std::string>& values, std::string value)
		{
			if (!value.empty())
				values.push_back(std::move(value));
		}

		void add_unresolved(std::vector<sdk::provider::unresolved_item>& values,
							std::string code,
							std::string subject,
							std::string detail)
		{
			values.push_back({std::move(code), std::move(subject), std::move(detail)});
		}

		[[nodiscard]] sdk::result<void> cancellation_check(const std::stop_token& token)
		{
			if (token.stop_requested())
				return sdk::unexpected(
					failure("provider-worker-v4.output-cancelled", "normalizer", "stop-requested"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		normalize_impl(const provider_worker_v4_ast_observation_batch& input,
					   const provider_worker_v4_output_normalizer_options& options,
					   provider_worker_v4_normalized_output& output)
		{
			if (auto valid = cancellation_check(options.cancellation); !valid)
				return valid;
			std::size_t row_count{};
			std::size_t output_bytes{};
			auto append_row = [&](const std::size_t batch_index,
								  sdk::detached_row row) -> sdk::result<void>
			{
				if (auto valid = cancellation_check(options.cancellation); !valid)
					return valid;
				if (row_count >= options.limits.maximum_rows)
					return sdk::unexpected(
						failure("provider-worker-v4.output-limit", "rows", "maximum-rows"));
				const auto size = row.canonical_form().size();
				if (size > options.limits.maximum_output_bytes -
						std::min(output_bytes, options.limits.maximum_output_bytes))
					return sdk::unexpected(failure(
						"provider-worker-v4.output-limit", "bytes", "maximum-output-bytes"));
				if (output_bytes > options.limits.maximum_output_bytes - size)
					return sdk::unexpected(failure(
						"provider-worker-v4.output-limit", "bytes", "maximum-output-bytes"));
				output_bytes += size;
				++row_count;
				output.batches.at(batch_index).rows.push_back(std::move(row));
				return {};
			};

			std::vector<std::size_t> ordered;
			ordered.reserve(input.observations.size());
			for (std::size_t index{}; index < input.observations.size(); ++index)
				ordered.push_back(index);
			std::ranges::sort(ordered,
							  [&](const std::size_t left, const std::size_t right)
							  {
								  return input.observations[left].canonical_form() <
									  input.observations[right].canonical_form();
							  });

			for (const auto& limitation : options.invocation_limitations)
				add_limitation(output.limitations, limitation);
			for (const auto& diagnostic : input.diagnostics)
				add_limitation(output.limitations, "frontend-diagnostic:" + diagnostic);
			if (input.failed_count != 0U)
				add_limitation(output.limitations,
							   "frontend-failed-count:" + std::to_string(input.failed_count));
			for (const auto& observation : input.observations)
			{
				if (!observation.exact_equivalence && observation.limitation)
					add_limitation(output.limitations,
								   "observation-limitation:" + observation.semantic_key + ":" +
									   *observation.limitation);
				for (const auto field :
					 {std::string_view{"symbol.identity_confidence"},
					  std::string_view{"call.direct_callee_identity_confidence"}})
				{
					const auto confidence = observation.payload.find(field);
					if (confidence != observation.payload.end() &&
						confidence->second != "exact-usr")
						add_limitation(output.limitations,
									   "identity-confidence:" + confidence->second + ":" +
										   observation.semantic_key);
				}
			}

			std::map<std::string,
					 std::vector<const provider_worker_v4_ast_observation*>,
					 std::less<>>
				entity_groups;
			for (const auto& observation : input.observations)
				if (observation.kind == provider_worker_v4_ast_observation_kind::entity)
					entity_groups[observation.semantic_key].push_back(&observation);
			std::vector<const provider_worker_v4_ast_observation*> selected_entities;
			selected_entities.reserve(entity_groups.size());
			for (auto& [semantic_key, group] : entity_groups)
			{
				std::ranges::sort(
					group,
					[](const auto* left, const auto* right)
					{
						return std::tuple{entity_preference(*left), left->canonical_form()} <
							std::tuple{entity_preference(*right), right->canonical_form()};
					});
				const auto* selected = group.front();
				selected_entities.push_back(selected);
				if (std::ranges::any_of(group,
										[&](const auto* candidate)
										{
											return !compatible_redeclaration(*selected, *candidate);
										}))
				{
					add_limitation(output.limitations,
								   "incompatible-redeclaration:" + semantic_key);
					add_unresolved(output.unresolved,
								   "provider.entity-redeclaration-incompatible",
								   semantic_key,
								   "cc.entity");
				}
			}
			std::ranges::sort(selected_entities,
							  [](const auto* left, const auto* right)
							  {
								  return std::tuple{left->semantic_key, left->canonical_form()} <
									  std::tuple{right->semantic_key, right->canonical_form()};
							  });

			for (const auto& observation : input.observations)
			{
				if (observation.kind != provider_worker_v4_ast_observation_kind::type &&
					!observation.primary_span)
				{
					const auto kind =
						observation.kind == provider_worker_v4_ast_observation_kind::entity
						? "entity"
						: "call";
					add_limitation(output.limitations,
								   "source-authority-unavailable:" + std::string{kind} + ":" +
									   observation.semantic_key);
					add_unresolved(output.unresolved,
								   "provider.source-unavailable",
								   observation.semantic_key,
								   observation.kind ==
										   provider_worker_v4_ast_observation_kind::entity
									   ? "cc.entity"
									   : "cc.call_site");
				}
				if (observation.kind != provider_worker_v4_ast_observation_kind::call)
					continue;
				const auto kind = entity_field(observation, "call.kind");
				const auto target = entity_field(observation, "call.direct_callee");
				if ((target.empty() && call_kind_requires_direct_target(kind)) ||
					(!target.empty() && call_kind_forbids_direct_target(kind)))
					add_limitation(output.limitations,
								   "call-kind-target-inconsistent:" + observation.semantic_key);
			}
			sort_unique(output.limitations);
			output.exact_equivalence =
				options.invocation_exact && input.failed_count == 0U && output.limitations.empty();

			// Observation rows are copied from the validated observer output.  In particular, the
			// opaque payload_digest is not decoded and cannot become a second source of authority.
			for (const auto index : ordered)
			{
				const auto& observation = input.observations[index];
				if (observation.kind == provider_worker_v4_ast_observation_kind::entity)
				{
					if (auto valid = append_row(4U, input.rows[index]); !valid)
						return valid;
				}
			}
			for (const auto index : ordered)
			{
				const auto& observation = input.observations[index];
				if (observation.kind == provider_worker_v4_ast_observation_kind::type)
				{
					if (auto valid = append_row(5U, input.rows[index]); !valid)
						return valid;
				}
			}

			std::map<std::string, std::string, std::less<>> entity_ids;
			for (const auto* observation : selected_entities)
			{
				auto canonical = entity_row(
					*observation, options.toolchain_context_id, output.exact_equivalence);
				if (!canonical)
					return sdk::unexpected(std::move(canonical.error()));
				auto entity = row_string(*canonical, "cc.entity.v1.entity");
				if (!entity)
					return sdk::unexpected(std::move(entity.error()));
				entity_ids.emplace(observation->semantic_key, *entity);
				if (auto valid = append_row(2U, std::move(*canonical)); !valid)
					return valid;
			}

			std::vector<std::size_t> calls;
			for (const auto index : ordered)
				if (input.observations[index].kind == provider_worker_v4_ast_observation_kind::call)
					calls.push_back(index);
			std::ranges::sort(calls,
							  [&](const std::size_t left, const std::size_t right)
							  {
								  const auto& lhs = input.observations[left];
								  const auto& rhs = input.observations[right];
								  return std::tuple{call_occurrence_class(lhs),
													lhs.canonical_form()} <
									  std::tuple{call_occurrence_class(rhs), rhs.canonical_form()};
							  });
			std::string previous_class;
			std::uint64_t call_ordinal{};
			for (const auto index : calls)
			{
				const auto& observation = input.observations[index];
				const auto occurrence_class = call_occurrence_class(observation);
				if (occurrence_class != previous_class)
				{
					previous_class = occurrence_class;
					call_ordinal = 0U;
				}
				if (auto valid = append_row(3U, input.rows[index]); !valid)
					return valid;
				if (!observation.primary_span)
					continue;
				auto site = call_site_row(observation, entity_ids, call_ordinal++);
				if (!site)
					return sdk::unexpected(std::move(site.error()));
				auto call = row_string(*site, "cc.call_site.v1.call");
				if (!call)
					return sdk::unexpected(std::move(call.error()));
				if (auto valid = append_row(1U, std::move(*site)); !valid)
					return valid;

				const auto target = observation.payload.find("call.direct_callee");
				if (target == observation.payload.end() || target->second.empty())
				{
					const auto reason = observation.payload.contains("call.unresolved_reason")
						? observation.payload.at("call.unresolved_reason")
						: "no-direct-callee";
					const auto kind = entity_field(observation, "call.kind");
					const auto code = call_kind_requires_direct_target(kind)
						? "provider.call-kind-target-inconsistent"
						: kind.starts_with("indirect_") ? "provider.indirect-target-unresolved"
														: "provider.call-target-unresolved";
					add_unresolved(output.unresolved, code, *call, reason);
					continue;
				}
				if (call_kind_forbids_direct_target(entity_field(observation, "call.kind")))
				{
					add_unresolved(output.unresolved,
								   "provider.call-kind-target-inconsistent",
								   *call,
								   "unexpected-direct-callee");
					continue;
				}
				std::string target_id;
				const auto target_entity = entity_ids.find(target->second);
				if (target_entity != entity_ids.end())
					target_id = target_entity->second;
				else
				{
					auto target_observation = direct_callee_observation(observation);
					if (!target_observation)
					{
						add_unresolved(output.unresolved,
									   "provider.direct-target-unresolved",
									   *call,
									   "projection");
						continue;
					}
					auto target_row = entity_row(*target_observation,
												 options.toolchain_context_id,
												 output.exact_equivalence);
					if (!target_row)
						return sdk::unexpected(std::move(target_row.error()));
					auto projected = row_string(*target_row, "cc.entity.v1.entity");
					if (!projected)
						return sdk::unexpected(std::move(projected.error()));
					target_id = std::move(*projected);
				}
				auto direct = direct_target_row(*call, target_id);
				if (!direct)
					return sdk::unexpected(std::move(direct.error()));
				if (auto valid = append_row(0U, std::move(*direct)); !valid)
					return valid;
			}

			for (auto& batch : output.batches)
				std::ranges::sort(batch.rows,
								  {},
								  [](const sdk::detached_row& row)
								  {
									  return row.canonical_form();
								  });
			return {};
		}
	} // namespace

	sdk::result<void> provider_worker_v4_output_normalizer_limits::validate() const
	{
		if (maximum_observations == 0U || maximum_rows == 0U || maximum_output_bytes == 0U ||
			maximum_diagnostics == 0U)
			return sdk::unexpected(
				failure("provider-worker-v4.output-limit-invalid", "limits", "nonzero"));
		return {};
	}

	sdk::result<void> provider_worker_v4_output_normalizer_options::validate() const
	{
		if (!sdk::validate_strong_id(toolchain_context_id))
			return sdk::unexpected(
				failure("provider-worker-v4.output-authority-invalid", "toolchain_context_id"));
		if (auto valid = limits.validate(); !valid)
			return valid;
		for (const auto& limitation : invocation_limitations)
			if (!valid_text(limitation))
				return sdk::unexpected(failure("provider-worker-v4.output-authority-invalid",
											   "invocation_limitations"));
		return {};
	}

	sdk::result<void> provider_worker_v4_output_batch::validate() const
	{
		if (!valid_text(descriptor_id) || !valid_text(dependency_group_id) ||
			!valid_text(atomic_output_group_id) || !valid_text(batch_id) ||
			atomic_output_group_id != "clang22-atomic" || batch_id != descriptor_id + "-batch")
			return sdk::unexpected(
				failure("provider-worker-v4.output-batch-invalid", "binding", "authority"));
		const bool canonical = descriptor_id.starts_with("cc.");
		if (dependency_group_id != (canonical ? "canonical" : "observation"))
			return sdk::unexpected(
				failure("provider-worker-v4.output-batch-invalid", "dependency_group_id"));
		const sdk::relation_descriptor* descriptor{};
		if (descriptor_id == cc::relations::call_direct_target::descriptor().id)
			descriptor = &cc::relations::call_direct_target::descriptor();
		else if (descriptor_id == cc::relations::call_site::descriptor().id)
			descriptor = &cc::relations::call_site::descriptor();
		else if (descriptor_id == cc::relations::entity::descriptor().id)
			descriptor = &cc::relations::entity::descriptor();
		else if (descriptor_id == materialization::call_observation_v2_descriptor().id)
			descriptor = &materialization::call_observation_v2_descriptor();
		else if (descriptor_id == materialization::entity_observation_v2_descriptor().id)
			descriptor = &materialization::entity_observation_v2_descriptor();
		else if (descriptor_id == materialization::type_observation_v2_descriptor().id)
			descriptor = &materialization::type_observation_v2_descriptor();
		else
			return sdk::unexpected(
				failure("provider-worker-v4.output-batch-invalid", "descriptor_id", "unknown"));
		std::string previous;
		for (const auto& row : rows)
		{
			if (auto valid = sdk::validate_row(*descriptor, row); !valid)
				return valid;
			if (descriptor->id == cc::relations::entity::descriptor().id ||
				descriptor->id == cc::relations::call_site::descriptor().id ||
				descriptor->id == materialization::call_observation_v2_descriptor().id ||
				descriptor->id == materialization::entity_observation_v2_descriptor().id ||
				descriptor->id == materialization::type_observation_v2_descriptor().id)
			{
				if (auto valid = sdk::validate_domain_identity(*descriptor, row); !valid)
					return valid;
			}
			const auto canonical_form = row.canonical_form();
			if (!previous.empty() && previous >= canonical_form)
				return sdk::unexpected(failure(
					"provider-worker-v4.output-batch-invalid", "rows", "order-or-duplicate"));
			previous = canonical_form;
		}
		return {};
	}

	sdk::result<void> provider_worker_v4_normalized_output::validate() const
	{
		if (!valid_text(task_id) || !valid_text(task_v4_digest) ||
			!sdk::validate_strong_id(compile_unit))
			return sdk::unexpected(
				failure("provider-worker-v4.output-invalid", "identity", "task-or-compile-unit"));
		for (std::size_t index{}; index < batches.size(); ++index)
		{
			const auto& batch = batches.at(index);
			if (batch.descriptor_id != task_v4_output_descriptor_ids.at(index))
				return sdk::unexpected(
					failure("provider-worker-v4.output-invalid", "descriptor-order"));
			if (auto valid = batch.validate(); !valid)
				return valid;
		}
		for (const auto& item : unresolved)
			if (!item.code.starts_with("provider.") || !valid_text(item.subject) ||
				(item.detail.find('\0') != std::string::npos) ||
				!sdk::validate_utf8_text(item.detail))
				return sdk::unexpected(failure("provider-worker-v4.output-invalid", "unresolved"));
		for (std::size_t index{1U}; index < unresolved.size(); ++index)
			if (std::tie(unresolved[index - 1U].code,
						 unresolved[index - 1U].subject,
						 unresolved[index - 1U].detail) >= std::tie(unresolved[index].code,
																	unresolved[index].subject,
																	unresolved[index].detail))
				return sdk::unexpected(failure(
					"provider-worker-v4.output-invalid", "unresolved", "order-or-duplicate"));
		for (const auto& limitation : limitations)
			if (!valid_text(limitation))
				return sdk::unexpected(failure("provider-worker-v4.output-invalid", "limitations"));
		for (std::size_t index{1U}; index < limitations.size(); ++index)
			if (limitations[index - 1U] >= limitations[index])
				return sdk::unexpected(failure(
					"provider-worker-v4.output-invalid", "limitations", "order-or-duplicate"));
		if (exact_equivalence && !limitations.empty())
			return sdk::unexpected(
				failure("provider-worker-v4.output-invalid", "exact_equivalence", "limitations"));
		return {};
	}

	sdk::result<provider_worker_v4_normalized_output>
	normalize_provider_worker_v4_output(const provider_worker_v4_ast_observation_batch& input,
										const provider_worker_v4_output_normalizer_options& options)
	{
		try
		{
			if (auto valid = input.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (auto valid = options.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (input.observations.size() > options.limits.maximum_observations ||
				input.rows.size() > options.limits.maximum_observations ||
				input.diagnostics.size() > options.limits.maximum_diagnostics)
				return sdk::unexpected(failure("provider-worker-v4.output-limit",
											   "input",
											   "maximum-observations-or-diagnostics"));
			provider_worker_v4_normalized_output output;
			output.task_id = input.task_id;
			output.task_v4_digest = input.task_v4_digest;
			output.compile_unit = input.compile_unit;
			for (std::size_t index{}; index < output.batches.size(); ++index)
			{
				auto& batch = output.batches.at(index);
				batch.descriptor_id = std::string{task_v4_output_descriptor_ids.at(index)};
				batch.dependency_group_id = index < 3U ? "canonical" : "observation";
				batch.atomic_output_group_id = "clang22-atomic";
				batch.batch_id = batch.descriptor_id + "-batch";
			}
			if (auto valid = normalize_impl(input, options, output); !valid)
				return sdk::unexpected(std::move(valid.error()));
			sort_unique(output.limitations);
			std::ranges::sort(output.unresolved,
							  [](const auto& left, const auto& right)
							  {
								  return std::tie(left.code, left.subject, left.detail) <
									  std::tie(right.code, right.subject, right.detail);
							  });
			output.unresolved.erase(std::ranges::unique(output.unresolved).begin(),
									output.unresolved.end());
			if (auto valid = output.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			return output;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(
				failure("provider-worker-v4.output-allocation", "normalizer", "bad-alloc"));
		}
	}
} // namespace cxxlens::detail::clang22
