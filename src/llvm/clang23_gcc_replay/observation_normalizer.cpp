#include "observation_normalizer.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_declaration.hpp>
#include <cxxlens/relations/cc_entity.hpp>
#include <cxxlens/relations/cc_type.hpp>
#include <cxxlens/relations/source_span.hpp>

#include "source_authority_binder.hpp"

namespace cxxlens::detail::clang23_gcc_replay
{
	namespace
	{
		[[nodiscard]] sdk::error failure(std::string field, std::string detail)
		{
			return {"application-analysis.replay-observation-normalization-invalid",
					std::move(field),
					std::move(detail)};
		}

		[[nodiscard]] sdk::error resource(std::string detail)
		{
			return {"application-analysis.replay-observation-normalization-resource-limit",
					"observations",
					std::move(detail)};
		}

		[[nodiscard]] sdk::detached_cell
		symbol(sdk::scalar_kind kind, std::string parameter, std::string value)
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

		[[nodiscard]] sdk::detached_cell optional_bytes(const std::string_view value)
		{
			const auto bytes = std::as_bytes(std::span{value});
			auto output = sdk::detached_cell::bytes({bytes.begin(), bytes.end()});
			output.type.optional = true;
			return output;
		}

		[[nodiscard]] sdk::detached_cell
		canonical_symbol_set(std::string parameter, const std::span<const std::string> values)
		{
			std::vector<std::byte> encoded;
			for (const auto& value : values)
			{
				const auto length = static_cast<std::uint32_t>(value.size());
				for (std::size_t byte{}; byte < sizeof(length); ++byte)
					encoded.push_back(
						static_cast<std::byte>((length >> (byte * 8U)) & std::uint32_t{0xffU}));
				for (const auto character : value)
					encoded.push_back(
						static_cast<std::byte>(static_cast<unsigned char>(character)));
			}
			return {{sdk::scalar_kind::set, std::move(parameter), false},
					sdk::cell_state::present,
					sdk::scalar_value{std::move(encoded)},
					std::nullopt};
		}

		[[nodiscard]] bool requested(const sdk::detail::validated_gcc_replay_input& input,
									 const std::string_view descriptor)
		{
			return std::ranges::binary_search(input.value().requested_relation_descriptor_ids,
											  descriptor);
		}

		[[nodiscard]] auto source_key(const observed_source_span& value)
		{
			return std::tuple{std::string_view{value.logical_path},
							  value.begin,
							  value.end,
							  std::string_view{value.role}};
		}

		[[nodiscard]] const bound_source_span* find_source(const bound_observation_sources& bound,
														   const observed_source_span& value)
		{
			const auto found = std::ranges::lower_bound(bound.spans,
														source_key(value),
														{},
														[](const bound_source_span& candidate)
														{
															return source_key(candidate.observed);
														});
			return found != bound.spans.end() && source_key(found->observed) == source_key(value)
				? &*found
				: nullptr;
		}

		[[nodiscard]] sdk::result<sdk::detached_row> source_span_row(const bound_source_span& value)
		{
			using relation = source::relations::span;
			relation::builder builder;
			for (auto result : {
					 builder.set<relation::span_column>(
						 sdk::detached_cell::typed("source_span_id", value.span_id)),
					 builder.set<relation::snapshot>(
						 sdk::detached_cell::typed("source_snapshot_id", value.source_snapshot_id)),
					 builder.set<relation::file>(
						 sdk::detached_cell::typed("file_id", value.file_id)),
					 builder.set<relation::begin>(
						 sdk::detached_cell::unsigned_integer(value.observed.begin)),
					 builder.set<relation::end>(
						 sdk::detached_cell::unsigned_integer(value.observed.end)),
					 builder.set<relation::role>(
						 symbol(sdk::scalar_kind::open_symbol, "source.range-role/1", value.role)),
					 builder.set<relation::read_only>(sdk::detached_cell::boolean(value.read_only)),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			auto row = std::move(builder).finish();
			if (!row)
				return sdk::unexpected(std::move(row.error()));
			if (auto valid = sdk::validate_domain_identity(relation::descriptor(), *row); !valid)
				return sdk::unexpected(std::move(valid.error()));
			return row;
		}

		[[nodiscard]] bool compatible(const observed_entity& left, const observed_entity& right)
		{
			return left.provider_local_key == right.provider_local_key && left.kind == right.kind &&
				left.qualified_name == right.qualified_name &&
				left.canonical_type == right.canonical_type;
		}

		[[nodiscard]] auto entity_preference(const observed_entity& value)
		{
			return std::tuple{!value.definition, source_key(value.source)};
		}

		[[nodiscard]] sdk::result<sdk::detached_row>
		entity_row(const sdk::detail::validated_gcc_replay_input& input,
				   const observed_entity& value,
				   const bound_source_span& source,
				   const bool include_anchor)
		{
			using relation = cc::relations::entity;
			auto structural =
				sdk::semantic_digest("cc.entity.structural-signature.v1", value.canonical_type);
			auto local = sdk::semantic_digest("clang23.gcc-replay.provider-local-entity.v1",
											  input.value().capture_bundle_digest + "\n" +
												  input.value().analysis_frontend + "\n" +
												  value.provider_local_key);
			if (!structural || !local)
				return sdk::unexpected(!structural ? std::move(structural.error())
												   : std::move(local.error()));

			relation::builder builder;
			for (auto result : {
					 builder.set<relation::entity_column>(
						 sdk::detached_cell::typed("cc_entity_id", "pending")),
					 builder.set<relation::canonicalization>(symbol(sdk::scalar_kind::closed_symbol,
																	"cc.canonicalization-state/1",
																	"provider_local")),
					 builder.set<relation::kind>(
						 symbol(sdk::scalar_kind::open_symbol, "cc.entity-kind/1", value.kind)),
					 builder.set<relation::structural_signature_digest>(
						 symbol(sdk::scalar_kind::digest, {}, std::move(*structural))),
					 builder.set<relation::provider_local_key>(optional_bytes(*local)),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			if (include_anchor)
				if (auto result = builder.set<relation::anchor>(
						optional_typed("source_span_id", source.span_id));
					!result)
					return sdk::unexpected(std::move(result.error()));
			if (!value.qualified_name.empty())
				if (auto result =
						builder.set<relation::qualified_name>(optional_utf8(value.qualified_name));
					!result)
					return sdk::unexpected(std::move(result.error()));
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
		declaration_row(const observed_declaration& value,
						const std::string_view entity,
						const bound_source_span& source)
		{
			using relation = cc::relations::declaration;
			relation::builder builder;
			for (auto result : {
					 builder.set<relation::declaration_column>(
						 sdk::detached_cell::typed("cc_declaration_id", "pending")),
					 builder.set<relation::entity>(
						 sdk::detached_cell::typed("cc_entity_id", std::string{entity})),
					 builder.set<relation::source>(
						 sdk::detached_cell::typed("source_span_id", source.span_id)),
					 builder.set<relation::kind>(symbol(
						 sdk::scalar_kind::open_symbol, "cc.declaration-kind/1", value.kind)),
					 builder.set<relation::storage>(symbol(
						 sdk::scalar_kind::open_symbol, "cc.storage-class/1", value.storage)),
					 builder.set<relation::linkage>(
						 symbol(sdk::scalar_kind::open_symbol, "cc.linkage/1", value.linkage)),
					 builder.set<relation::attributes>(
						 canonical_symbol_set("open_symbol<cc.attribute/1>", value.attributes)),
					 builder.set<relation::is_implicit>(
						 sdk::detached_cell::boolean(value.implicit)),
					 builder.set<relation::is_deleted>(sdk::detached_cell::boolean(value.deleted)),
					 builder.set<relation::is_defaulted>(
						 sdk::detached_cell::boolean(value.defaulted)),
					 builder.set<relation::is_friend>(
						 sdk::detached_cell::boolean(value.friend_declaration)),
					 builder.set<relation::is_exported>(
						 sdk::detached_cell::boolean(value.exported)),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			auto row = std::move(builder).finish();
			if (!row)
				return sdk::unexpected(std::move(row.error()));
			auto identity = sdk::derive_domain_identity(relation::descriptor(), *row);
			if (!identity)
				return sdk::unexpected(std::move(identity.error()));
			row->cells.at("cc.declaration.v1.declaration") =
				sdk::detached_cell::typed("cc_declaration_id", std::move(*identity));
			if (auto valid = sdk::validate_domain_identity(relation::descriptor(), *row); !valid)
				return sdk::unexpected(std::move(valid.error()));
			return row;
		}

		[[nodiscard]] sdk::result<std::vector<std::byte>>
		canonical_call_form(const observed_direct_call& value)
		{
			std::vector<sdk::canonical_value> origins;
			origins.reserve(value.origins.size());
			for (const auto& origin : value.origins)
				origins.push_back(sdk::canonical_value::from_tuple({
					sdk::canonical_value::from_string(origin.kind),
					sdk::canonical_value::from_string(origin.logical_path),
					sdk::canonical_value::from_integer(static_cast<std::int64_t>(origin.begin)),
					sdk::canonical_value::from_integer(static_cast<std::int64_t>(origin.end)),
					sdk::canonical_value::from_boolean(origin.read_only),
				}));
			return sdk::canonical_binary(sdk::canonical_value::from_tuple({
				value.caller_provider_local_key
					? sdk::canonical_value::from_string(*value.caller_provider_local_key)
					: sdk::canonical_value::null(),
				sdk::canonical_value::from_string(value.target_provider_local_key),
				sdk::canonical_value::from_string(value.kind),
				sdk::canonical_value::from_tuple({
					sdk::canonical_value::from_string(value.source.logical_path),
					sdk::canonical_value::from_integer(
						static_cast<std::int64_t>(value.source.begin)),
					sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.source.end)),
					sdk::canonical_value::from_string(value.source.role),
				}),
				sdk::canonical_value::from_tuple(std::move(origins)),
			}));
		}

		[[nodiscard]] sdk::result<std::string>
		type_component_signature(const observed_type::function_structure& structure)
		{
			std::vector<sdk::canonical_value> qualifiers;
			for (const auto& qualifier : structure.qualifiers)
				qualifiers.push_back(sdk::canonical_value::from_string(qualifier));
			std::vector<sdk::canonical_value> components;
			for (const auto& component : structure.components)
			{
				std::vector<sdk::canonical_value> component_qualifiers;
				for (const auto& qualifier : component.qualifiers)
					component_qualifiers.push_back(sdk::canonical_value::from_string(qualifier));
				components.push_back(sdk::canonical_value::from_tuple({
					sdk::canonical_value::from_string(component.role),
					sdk::canonical_value::from_integer(
						static_cast<std::int64_t>(component.ordinal)),
					sdk::canonical_value::from_string(component.constructor),
					sdk::canonical_value::from_tuple(std::move(component_qualifiers)),
				}));
			}
			auto canonical = sdk::canonical_binary(sdk::canonical_value::from_tuple({
				sdk::canonical_value::from_tuple(std::move(qualifiers)),
				sdk::canonical_value::from_tuple(std::move(components)),
				sdk::canonical_value::from_string(structure.calling_convention),
				sdk::canonical_value::from_string(structure.exception_specification),
				sdk::canonical_value::from_string(structure.ref_qualifier),
				sdk::canonical_value::from_boolean(structure.variadic),
			}));
			if (!canonical)
				return sdk::unexpected(std::move(canonical.error()));
			const auto bytes = std::string_view{reinterpret_cast<const char*>(canonical->data()),
												canonical->size()};
			return sdk::semantic_digest("cc.type.component-signature.v1", bytes);
		}

		[[nodiscard]] sdk::result<sdk::detached_row> type_row(const observed_type& value)
		{
			if (!value.structure)
				return sdk::unexpected(
					failure(value.provider_local_key, "type-structure-unavailable"));
			auto signature = type_component_signature(*value.structure);
			if (!signature)
				return sdk::unexpected(std::move(signature.error()));
			using relation = cc::relations::type;
			relation::builder builder;
			for (auto result : {
					 builder.set<relation::type_column>(
						 sdk::detached_cell::typed("cc_type_id", "pending")),
					 builder.set<relation::constructor>(symbol(sdk::scalar_kind::open_symbol,
															   "cc.type-constructor/1",
															   value.constructor)),
					 builder.set<relation::component_signature_digest>(
						 symbol(sdk::scalar_kind::digest, {}, std::move(*signature))),
					 builder.set<relation::qualifiers>(canonical_symbol_set(
						 "open_symbol<cc.type-qualifier/1>", value.structure->qualifiers)),
					 builder.set<relation::dependent>(sdk::detached_cell::boolean(value.dependent)),
					 builder.set<relation::spelling>(optional_utf8(value.canonical_spelling)),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			auto row = std::move(builder).finish();
			if (!row)
				return sdk::unexpected(std::move(row.error()));
			auto identity = sdk::derive_domain_identity(relation::descriptor(), *row);
			if (!identity)
				return sdk::unexpected(std::move(identity.error()));
			row->cells.at("cc.type.v1.type") =
				sdk::detached_cell::typed("cc_type_id", std::move(*identity));
			if (auto valid = sdk::validate_domain_identity(relation::descriptor(), *row); !valid)
				return sdk::unexpected(std::move(valid.error()));
			return row;
		}

		struct prepared_call
		{
			const observed_direct_call* observed{};
			const bound_source_span* source{};
			std::optional<std::string> caller;
			std::vector<std::byte> canonical_form;
		};

		[[nodiscard]] auto occurrence_class(const sdk::detail::validated_gcc_replay_input& input,
											const prepared_call& value)
		{
			return std::tuple{std::string_view{input.value().compile_unit_id},
							  std::string_view{value.source->span_id},
							  std::string_view{value.observed->kind},
							  value.caller};
		}

		[[nodiscard]] sdk::result<sdk::detached_row>
		call_site_row(const sdk::detail::validated_gcc_replay_input& input,
					  const prepared_call& value,
					  const std::uint64_t ordinal)
		{
			using relation = cc::relations::call_site;
			relation::builder builder;
			for (auto result : {
					 builder.set<relation::call>(
						 sdk::detached_cell::typed("cc_call_id", "pending")),
					 builder.set<relation::compile_unit>(sdk::detached_cell::typed(
						 "compile_unit_id", input.value().compile_unit_id)),
					 builder.set<relation::kind>(symbol(
						 sdk::scalar_kind::open_symbol, "cc.call-kind/1", value.observed->kind)),
					 builder.set<relation::source>(
						 sdk::detached_cell::typed("source_span_id", value.source->span_id)),
					 builder.set<relation::ordinal>(sdk::detached_cell::unsigned_integer(ordinal)),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			if (value.caller)
				if (auto result = builder.set<relation::caller>(
						optional_typed("cc_entity_id", *value.caller));
					!result)
					return sdk::unexpected(std::move(result.error()));
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
					 builder.set<relation::resolution>(symbol(sdk::scalar_kind::open_symbol,
															  "cc.direct-target-resolution/1",
															  "syntactic_direct")),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			auto row = std::move(builder).finish();
			if (!row)
				return sdk::unexpected(std::move(row.error()));
			return row;
		}

		void sort_rows(std::vector<sdk::detached_row>& rows)
		{
			std::ranges::sort(rows, {}, &sdk::detached_row::canonical_form);
			rows.erase(std::ranges::unique(
						   rows,
						   [](const sdk::detached_row& left, const sdk::detached_row& right)
						   {
							   return left.canonical_form() == right.canonical_form();
						   })
						   .begin(),
					   rows.end());
		}

		void sort_unresolved(std::vector<sdk::capture_gap>& values)
		{
			std::ranges::sort(values,
							  {},
							  [](const sdk::capture_gap& value)
							  {
								  return std::tie(value.field,
												  value.state,
												  value.reason,
												  value.completion_action);
							  });
			values.erase(std::ranges::unique(values).begin(), values.end());
		}
	} // namespace

	bool normalized_observation_candidates::operator==(
		const normalized_observation_candidates& other) const
	{
		const auto same_rows = [](const std::span<const sdk::detached_row> left,
								  const std::span<const sdk::detached_row> right)
		{
			return left.size() == right.size() &&
				std::ranges::equal(left,
								   right,
								   [](const sdk::detached_row& lhs, const sdk::detached_row& rhs)
								   {
									   return lhs.canonical_form() == rhs.canonical_form();
								   });
		};
		return replay_input_digest == other.replay_input_digest &&
			same_rows(source_spans, other.source_spans) && same_rows(entities, other.entities) &&
			same_rows(declarations, other.declarations) && same_rows(types, other.types) &&
			same_rows(call_sites, other.call_sites) &&
			same_rows(direct_targets, other.direct_targets) && unresolved == other.unresolved;
	}

	sdk::result<normalized_observation_candidates>
	normalize_observation_candidates(const sdk::detail::validated_gcc_replay_input& input,
									 const worker_observation_output& worker)
	{
		try
		{
			if (auto valid = validate_worker_observation_output(worker); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (worker.replay_input_digest != input.input_digest())
				return sdk::unexpected(failure("replay_input_digest", "worker-input-mismatch"));
			auto bound = bind_observation_sources(input, worker.observations);
			if (!bound)
				return sdk::unexpected(std::move(bound.error()));

			normalized_observation_candidates output;
			output.replay_input_digest = worker.replay_input_digest;
			output.unresolved = input.value().unresolved;
			for (const auto& limitation : worker.observations.limitations)
				output.unresolved.push_back(
					{"worker.observations",
					 "unavailable",
					 limitation,
					 "recapture-with-a-worker-that-can-detach-this-observation"});

			const bool source_spans_requested =
				requested(input, source::relations::span::descriptor().id);
			if (source_spans_requested)
			{
				output.source_spans.reserve(bound->spans.size());
				for (const auto& source : bound->spans)
				{
					auto row = source_span_row(source);
					if (!row)
						return sdk::unexpected(failure("source.span",
													   row.error().code + ":" + row.error().field +
														   ":" + row.error().detail));
					output.source_spans.push_back(std::move(*row));
				}
				sort_rows(output.source_spans);
			}

			const bool entities_requested =
				requested(input, cc::relations::entity::descriptor().id);
			const bool declarations_requested =
				requested(input, cc::relations::declaration::descriptor().id);
			const bool call_sites_requested =
				requested(input, cc::relations::call_site::descriptor().id);
			const bool direct_targets_requested =
				requested(input, cc::relations::call_direct_target::descriptor().id);
			const bool types_requested = requested(input, cc::relations::type::descriptor().id);
			std::map<std::string_view, std::string, std::less<>> entity_ids;
			if (entities_requested || declarations_requested || call_sites_requested ||
				direct_targets_requested || types_requested)
			{
				std::map<std::string_view, const observed_entity*, std::less<>> selected;
				for (const auto& entity : worker.observations.entities)
				{
					auto [found, inserted] = selected.emplace(entity.provider_local_key, &entity);
					if (inserted)
						continue;
					if (!compatible(*found->second, entity))
						return sdk::unexpected(
							failure(entity.provider_local_key, "incompatible-redeclaration"));
					if (entity_preference(entity) < entity_preference(*found->second))
						found->second = &entity;
				}
				if (entities_requested)
					output.entities.reserve(selected.size());
				for (const auto& [key, entity] : selected)
				{
					const auto* source = find_source(*bound, entity->source);
					if (source == nullptr)
						return sdk::unexpected(
							failure(entity->provider_local_key, "source-unbound"));
					auto row = entity_row(input, *entity, *source, source_spans_requested);
					if (!row)
						return sdk::unexpected(failure("cc.entity",
													   row.error().code + ":" + row.error().field +
														   ":" + row.error().detail));
					const auto& identity_cell = row->cells.at("cc.entity.v1.entity");
					const auto* identity = identity_cell.value
						? std::get_if<std::string>(&*identity_cell.value)
						: nullptr;
					if (identity == nullptr)
						return sdk::unexpected(
							failure(std::string{key}, "entity-identity-invalid"));
					entity_ids.emplace(key, *identity);
					if (entities_requested)
						output.entities.push_back(std::move(*row));
				}
				if (entities_requested)
					sort_rows(output.entities);
			}

			if (declarations_requested)
			{
				output.declarations.reserve(worker.observations.declarations.size());
				for (const auto& declaration : worker.observations.declarations)
				{
					const auto entity = entity_ids.find(declaration.entity_provider_local_key);
					if (entity == entity_ids.end())
						return sdk::unexpected(failure(declaration.entity_provider_local_key,
													   "declaration-entity-unbound"));
					const auto* source = find_source(*bound, declaration.source);
					if (source == nullptr)
						return sdk::unexpected(failure(declaration.entity_provider_local_key,
													   "declaration-source-unbound"));
					auto row = declaration_row(declaration, entity->second, *source);
					if (!row)
						return sdk::unexpected(failure("cc.declaration",
													   row.error().code + ":" + row.error().field +
														   ":" + row.error().detail));
					output.declarations.push_back(std::move(*row));
				}
				sort_rows(output.declarations);
			}

			if (types_requested)
			{
				std::map<std::string, sdk::detached_row, std::less<>> selected_types;
				for (const auto& type : worker.observations.types)
				{
					const auto owner = entity_ids.find(type.owning_entity_provider_local_key);
					if (owner == entity_ids.end())
						return sdk::unexpected(
							failure(type.owning_entity_provider_local_key, "type-owner-unbound"));
					if (!type.structure)
					{
						output.unresolved.push_back(
							{"cc.type.v1",
							 "unavailable",
							 "structural-type-unavailable:" + owner->second + ":" +
								 *type.unavailable_reason,
							 "use-a-qualified-native-gap-provider-for-this-type-structure"});
						continue;
					}
					auto row = type_row(type);
					if (!row)
						return sdk::unexpected(failure("cc.type",
													   row.error().code + ":" + row.error().field +
														   ":" + row.error().detail));
					const auto& identity_cell = row->cells.at("cc.type.v1.type");
					const auto* identity = identity_cell.value
						? std::get_if<std::string>(&*identity_cell.value)
						: nullptr;
					if (identity == nullptr)
						return sdk::unexpected(
							failure(type.provider_local_key, "type-identity-invalid"));
					auto [found, inserted] = selected_types.emplace(*identity, *row);
					if (!inserted && row->canonical_form() < found->second.canonical_form())
						found->second = std::move(*row);
				}
				output.types.reserve(selected_types.size());
				for (auto& [identity, row] : selected_types)
				{
					static_cast<void>(identity);
					output.types.push_back(std::move(row));
				}
				sort_rows(output.types);
			}

			if (call_sites_requested || direct_targets_requested)
			{
				std::vector<prepared_call> calls;
				calls.reserve(worker.observations.direct_calls.size());
				for (const auto& call : worker.observations.direct_calls)
				{
					const auto* source = find_source(*bound, call.source);
					if (source == nullptr)
						return sdk::unexpected(
							failure(call.target_provider_local_key, "call-source-unbound"));
					prepared_call prepared{&call, source, std::nullopt, {}};
					if (call.caller_provider_local_key)
					{
						const auto caller = entity_ids.find(*call.caller_provider_local_key);
						if (caller == entity_ids.end())
							return sdk::unexpected(
								failure(*call.caller_provider_local_key, "call-caller-unbound"));
						prepared.caller = caller->second;
					}
					auto canonical = canonical_call_form(call);
					if (!canonical)
						return sdk::unexpected(std::move(canonical.error()));
					prepared.canonical_form = std::move(*canonical);
					calls.push_back(std::move(prepared));
				}
				std::ranges::sort(
					calls,
					[&](const prepared_call& left, const prepared_call& right)
					{
						return std::tuple{occurrence_class(input, left), left.canonical_form} <
							std::tuple{occurrence_class(input, right), right.canonical_form};
					});
				if (call_sites_requested)
					output.call_sites.reserve(calls.size());
				if (direct_targets_requested)
					output.direct_targets.reserve(calls.size());
				using occurrence_type =
					decltype(occurrence_class(input, std::declval<const prepared_call&>()));
				std::optional<occurrence_type> previous;
				std::uint64_t ordinal{};
				for (const auto& call : calls)
				{
					const auto current = occurrence_class(input, call);
					if (!previous || *previous != current)
					{
						previous = current;
						ordinal = 0U;
					}
					auto site = call_site_row(input, call, ordinal++);
					if (!site)
						return sdk::unexpected(failure("cc.call_site",
													   site.error().code + ":" +
														   site.error().field + ":" +
														   site.error().detail));
					const auto& identity_cell = site->cells.at("cc.call_site.v1.call");
					const auto* identity = identity_cell.value
						? std::get_if<std::string>(&*identity_cell.value)
						: nullptr;
					if (identity == nullptr)
						return sdk::unexpected(failure(call.observed->target_provider_local_key,
													   "call-identity-invalid"));
					const auto call_id = *identity;
					if (call_sites_requested)
						output.call_sites.push_back(std::move(*site));
					if (!direct_targets_requested)
						continue;
					const auto target = entity_ids.find(call.observed->target_provider_local_key);
					if (target == entity_ids.end())
					{
						output.unresolved.push_back(
							{"cc.call_direct_target.v1.target",
							 "unavailable",
							 "direct-callee-entity-unbound:" +
								 call.observed->target_provider_local_key,
							 "recapture-with-a-worker-that-detaches-the-direct-callee-entity"});
						continue;
					}
					auto direct = direct_target_row(call_id, target->second);
					if (!direct)
						return sdk::unexpected(std::move(direct.error()));
					output.direct_targets.push_back(std::move(*direct));
				}
				sort_rows(output.call_sites);
				sort_rows(output.direct_targets);
			}
			sort_unresolved(output.unresolved);
			return output;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(resource("allocation"));
		}
		catch (const std::length_error&)
		{
			return sdk::unexpected(resource("allocation-length"));
		}
	}
} // namespace cxxlens::detail::clang23_gcc_replay
