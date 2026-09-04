#include "source_authority_binder.hpp"

#include <algorithm>
#include <map>
#include <new>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

#include <cxxlens/relations/source_span.hpp>

#include "sdk/source_identity_internal.hpp"

namespace cxxlens::detail::clang23_gcc_replay
{
	namespace
	{
		using source_member = sdk::detail::decoded_capture_source_member;

		[[nodiscard]] sdk::error failure(std::string field, std::string detail)
		{
			return {"application-analysis.replay-source-authority-invalid",
					std::move(field),
					std::move(detail)};
		}

		[[nodiscard]] sdk::error resource(std::string detail)
		{
			return {"application-analysis.replay-source-authority-resource-limit",
					"observations",
					std::move(detail)};
		}

		[[nodiscard]] sdk::detached_cell role_cell(const std::string_view role)
		{
			return {{sdk::scalar_kind::open_symbol, "source.range-role/1", false},
					sdk::cell_state::present,
					sdk::scalar_value{std::string{role}},
					std::nullopt};
		}

		[[nodiscard]] sdk::result<std::string> span_identity(const source_member& member,
															 const observed_source_span& observed)
		{
			using relation = source::relations::span;
			relation::builder builder;
			for (auto result : {
					 builder.set<relation::span_column>(
						 sdk::detached_cell::typed("source_span_id", "pending")),
					 builder.set<relation::snapshot>(sdk::detached_cell::typed(
						 "source_snapshot_id", *member.source_snapshot_id)),
					 builder.set<relation::file>(
						 sdk::detached_cell::typed("file_id", member.file_id)),
					 builder.set<relation::begin>(
						 sdk::detached_cell::unsigned_integer(observed.begin)),
					 builder.set<relation::end>(sdk::detached_cell::unsigned_integer(observed.end)),
					 builder.set<relation::role>(role_cell(observed.role)),
					 builder.set<relation::read_only>(
						 sdk::detached_cell::boolean(member.read_only)),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			auto row = std::move(builder).finish();
			if (!row)
				return sdk::unexpected(std::move(row.error()));
			return sdk::derive_domain_identity(relation::descriptor(), *row);
		}
	} // namespace

	sdk::result<bound_observation_sources>
	bind_observation_sources(const sdk::detail::validated_compiler_replay_input& input,
							 const observation_batch& observations)
	{
		try
		{
			std::map<std::string_view, const source_member*, std::less<>> members;
			for (const auto& member : input.value().source_members)
			{
				if (!member.logical_path.starts_with("project://") || member.file_id.empty() ||
					!member.source_snapshot_id || !member.encoding)
					return sdk::unexpected(
						failure(member.logical_path, "capture-identity-unavailable"));
				auto file =
					sdk::detail::derive_source_file_id(std::string_view{member.logical_path}.substr(
						std::string_view{"project://"}.size()));
				if (!file || *file != member.file_id)
					return sdk::unexpected(failure(member.logical_path, "file-identity-mismatch"));
				auto snapshot = sdk::detail::derive_source_snapshot_id(
					member.file_id, member.content_digest, *member.encoding);
				if (!snapshot || *snapshot != *member.source_snapshot_id)
					return sdk::unexpected(
						failure(member.logical_path, "snapshot-identity-mismatch"));
				if (!members.emplace(member.logical_path, &member).second)
					return sdk::unexpected(failure(member.logical_path, "duplicate-logical-path"));
			}

			std::size_t count{};
			for (const auto size : {observations.entities.size(),
									observations.declarations.size(),
									observations.direct_calls.size()})
			{
				if (size > observer_product_maximum_observations - count)
					return sdk::unexpected(resource("count"));
				count += size;
			}
			bound_observation_sources output;
			output.replay_input_digest = std::string{input.input_digest()};
			output.spans.reserve(count);
			auto bind = [&](const observed_source_span& observed) -> sdk::result<void>
			{
				if (observed.role != "spelling" && observed.role != "expansion")
					return sdk::unexpected(failure(observed.role, "unsupported-range-role"));
				const auto found = members.find(observed.logical_path);
				if (found == members.end())
					return sdk::unexpected(failure(observed.logical_path, "not-in-source-closure"));
				const auto& member = *found->second;
				if (observed.end < observed.begin || observed.end > member.content.size())
					return sdk::unexpected(failure(observed.logical_path, "range-out-of-bounds"));
				auto identity = span_identity(member, observed);
				if (!identity)
					return sdk::unexpected(std::move(identity.error()));
				output.spans.push_back({observed,
										std::move(*identity),
										*member.source_snapshot_id,
										member.file_id,
										observed.role,
										member.read_only});
				return {};
			};
			for (const auto& value : observations.entities)
				if (auto result = bind(value.source); !result)
					return sdk::unexpected(std::move(result.error()));
			for (const auto& value : observations.declarations)
				if (auto result = bind(value.source); !result)
					return sdk::unexpected(std::move(result.error()));
			for (const auto& value : observations.direct_calls)
			{
				if (auto result = bind(value.source); !result)
					return sdk::unexpected(std::move(result.error()));
				for (const auto& origin : value.origins)
				{
					if (origin.kind != "macro-spelling" && origin.kind != "macro-spelling-begin" &&
						origin.kind != "macro-spelling-end")
						return sdk::unexpected(failure(origin.kind, "origin-kind-invalid"));
					const auto found = members.find(origin.logical_path);
					if (found == members.end())
						return sdk::unexpected(
							failure(origin.logical_path, "origin-not-in-source-closure"));
					if (!origin.read_only || origin.end < origin.begin ||
						origin.end > found->second->content.size())
						return sdk::unexpected(
							failure(origin.logical_path, "origin-range-invalid"));
				}
			}

			std::ranges::sort(output.spans,
							  [](const auto& left, const auto& right)
							  {
								  return std::tie(left.observed.logical_path,
												  left.observed.begin,
												  left.observed.end,
												  left.observed.role) <
									  std::tie(right.observed.logical_path,
											   right.observed.begin,
											   right.observed.end,
											   right.observed.role);
							  });
			output.spans.erase(std::ranges::unique(output.spans).begin(), output.spans.end());
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
