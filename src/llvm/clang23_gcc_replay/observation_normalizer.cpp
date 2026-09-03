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

#include <cxxlens/relations/cc_declaration.hpp>
#include <cxxlens/relations/cc_entity.hpp>
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
			same_rows(declarations, other.declarations) && unresolved == other.unresolved;
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
						return sdk::unexpected(std::move(row.error()));
					output.source_spans.push_back(std::move(*row));
				}
				sort_rows(output.source_spans);
			}

			const bool entities_requested =
				requested(input, cc::relations::entity::descriptor().id);
			const bool declarations_requested =
				requested(input, cc::relations::declaration::descriptor().id);
			std::map<std::string_view, std::string, std::less<>> entity_ids;
			if (entities_requested || declarations_requested)
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
						return sdk::unexpected(std::move(row.error()));
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
						return sdk::unexpected(std::move(row.error()));
					output.declarations.push_back(std::move(*row));
				}
				sort_rows(output.declarations);
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
