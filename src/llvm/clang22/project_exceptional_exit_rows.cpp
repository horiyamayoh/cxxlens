#include "project_exceptional_exit_rows.hpp"

#include <algorithm>
#include <limits>

#include <cxxlens/relations/cc_exceptional_exit.hpp>
namespace cxxlens::detail::clang22
{
	namespace
	{
		using fields = std::map<std::string, sdk::detached_cell, std::less<>>;
		constexpr std::string_view profile = "clang22-original-exceptional-occurrences/1";
		constexpr std::string_view lowering = "clang22-written-definition-analysis-lowering/1";
		struct writer
		{
			const exceptional_exit_limits& limits;
			std::size_t operations{}, bytes{}, members{};
			void work(std::size_t n = 1U)
			{
				if (limits.cancelled && limits.cancelled())
					throw sdk::error{
						"native.exceptional-exit-cancelled", "detachment", "cancelled"};
				if (n > limits.maximum_operations - std::min(operations, limits.maximum_operations))
					throw sdk::error{"native.exceptional-exit-budget", "operations", "detachment"};
				operations += n;
			}
			void retain(std::size_t n)
			{
				work();
				if (n >
					limits.maximum_retained_bytes - std::min(bytes, limits.maximum_retained_bytes))
					throw sdk::error{
						"native.exceptional-exit-budget", "retained-bytes", "detachment"};
				bytes += n;
			}
			sdk::detached_cell text(std::string_view value)
			{
				retain(value.size() + sizeof(std::string));
				return sdk::detached_cell::utf8(std::string(value));
			}
			sdk::detached_cell binary(std::string_view value)
			{
				retain(value.size() + sizeof(std::vector<std::byte>));
				std::vector<std::byte> encoded;
				encoded.reserve(value.size());
				for (char byte : value)
					encoded.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
				return sdk::detached_cell::bytes(std::move(encoded));
			}
			sdk::detached_row row(fields input)
			{
				if (members++ >= limits.maximum_occurrences)
					throw sdk::error{"native.exceptional-exit-budget", "occurrences", "detachment"};
				const auto& descriptor = cc::relations::exceptional_exit::descriptor();
				retain(sizeof(sdk::detached_row) +
					   descriptor.columns.size() * (sizeof(sdk::detached_cell) + 256U));
				sdk::detached_row result{descriptor.id, {}};
				for (const auto& column : descriptor.columns)
				{
					work();
					if (auto found = input.find(column.name); found != input.end())
					{
						found->second.type = column.type;
						result.cells.emplace(column.id, std::move(found->second));
					}
					else if (column.type.optional)
						result.cells.emplace(column.id, sdk::detached_cell::absent(column.type));
				}
				auto identity = sdk::derive_domain_identity(descriptor, result);
				if (!identity)
					throw identity.error();
				retain(identity->size());
				auto key = text(*identity);
				key.type = descriptor.column("cc.exceptional_exit.v1.exit")->type;
				result.cells.insert_or_assign("cc.exceptional_exit.v1.exit", std::move(key));
				if (auto valid = sdk::validate_row(descriptor, result); !valid)
					throw valid.error();
				if (auto valid = sdk::validate_domain_identity(descriptor, result); !valid)
					throw valid.error();
				return result;
			}
			sdk::detached_cell set(std::vector<std::string>& values)
			{
				work(values.size() * (sizeof(std::size_t) * 8U + 1U));
				std::ranges::sort(values);
				if (std::ranges::adjacent_find(values) != values.end())
					throw sdk::error{"native.exceptional-exit-invalid",
									 "occurrences",
									 "duplicate-original-identity"};
				std::vector<std::byte> out;
				for (const auto& value : values)
				{
					if (value.size() > std::numeric_limits<std::uint32_t>::max())
						throw sdk::error{"native.exceptional-exit-budget", "set", "length"};
					retain(4U + value.size());
					const auto n = static_cast<std::uint32_t>(value.size());
					for (unsigned shift = 0U; shift < 32U; shift += 8U)
						out.push_back(static_cast<std::byte>((n >> shift) & 255U));
					for (char byte : value)
						out.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
				}
				return {{sdk::scalar_kind::set, "exceptional_exit_id", true},
						sdk::cell_state::present,
						sdk::scalar_value{std::move(out)},
						std::nullopt};
			}
		};
	} // namespace
	sdk::result<project_exceptional_exit_rows>
	detach_project_exceptional_exits(const project_exceptional_exit_observations& original,
									 const exceptional_exit_row_bindings& bindings,
									 exceptional_exit_limits limits)
	{
		try
		{
			writer bounds{limits};
			project_exceptional_exit_rows output;
			if (!original.frozen || bindings.compile_unit.empty())
				return sdk::unexpected(sdk::error{"native.exceptional-exit-invalid",
												  "detachment",
												  "frozen-original-unit-required"});
			if (original.scopes.size() > limits.maximum_scopes)
				return sdk::unexpected(
					sdk::error{"native.exceptional-exit-budget", "scopes", "detachment"});
			for (const auto& scope : original.scopes)
			{
				bounds.work();
				if (scope.detail.empty() || scope.function.empty() ||
					scope.definition_source.empty())
				{
					bounds.retain(sizeof(std::string) + scope.owner_usr.size() + 64U);
					output.unbound_scopes.push_back(scope.owner_usr);
					continue;
				}
				bounds.retain(sizeof(exceptional_scope_facets) + scope.detail.size() +
							  scope.body_id.size() + 1024U);
				exceptional_scope_facets facets{scope.detail, scope.body_id, {}};
				std::vector<std::string> members;
				for (const auto& variant : scope.variants)
				{
					bounds.work();
					auto common = [&](std::size_t ordinal)
					{
						bounds.retain(2048U);
						return fields{
							{"compile_unit", bounds.text(bindings.compile_unit)},
							{"scope_detail", bounds.text(scope.detail)},
							{"function", bounds.text(scope.function)},
							{"definition_source", bounds.text(scope.definition_source)},
							{"variant_kind", bounds.text(variant.kind)},
							{"variant_index", sdk::detached_cell::unsigned_integer(variant.index)},
							{"variant_symbol", bounds.binary(variant.symbol)},
							{"ordinal", sdk::detached_cell::unsigned_integer(ordinal)},
							{"profile", bounds.text(profile)},
							{"lowering_profile", bounds.text(lowering)}};
					};
					auto value = common(0U);
					if (!scope.body_id.empty())
						value.emplace("body", bounds.text(scope.body_id));
					value.emplace("role", bounds.text("lowering_variant"));
					value.emplace("eligibility", bounds.text("excluded"));
					value.emplace("observation_state",
								  bounds.text(variant.complete ? "complete" : "partial"));
					if (!variant.reason.empty())
						value.emplace("reason", bounds.text(variant.reason));
					auto carrier = bounds.row(std::move(value));
					const auto carrier_id = std::get<std::string>(
						*carrier.cells.at("cc.exceptional_exit.v1.exit").value);
					bounds.retain(carrier_id.size() * 2U + sizeof(std::string) * 2U);
					members.push_back(carrier_id);
					output.rows.push_back(std::move(carrier));
					std::size_t ordinal{};
					for (const auto& occurrence : variant.occurrences)
					{
						bounds.work();
						auto item = common(++ordinal);
						item.emplace("variant", bounds.text(carrier_id));
						if (!scope.body_id.empty())
							item.emplace("body", bounds.text(scope.body_id));
						item.emplace("role", bounds.text(occurrence.role));
						item.emplace("eligibility", bounds.text(occurrence.eligibility));
						item.emplace("observation_state",
									 bounds.text(occurrence.eligibility == "unknown" ? "partial"
																					 : "complete"));
						if (occurrence.source.begin && occurrence.source.end && bindings.source)
						{
							const auto source = bindings.source(occurrence.source);
							if (!source.empty())
								item.emplace("source", bounds.text(source));
						}
						if (!occurrence.expression.empty())
							item.emplace("expression", bounds.text(occurrence.expression));
						if (!occurrence.target.empty())
							item.emplace("target", bounds.text(occurrence.target));
						if (!occurrence.target_usr.empty())
							item.emplace("target_usr", bounds.binary(occurrence.target_usr));
						auto number = [&](std::string name, const auto& n)
						{
							if (n)
								item.emplace(std::move(name),
											 sdk::detached_cell::unsigned_integer(*n));
						};
						auto boolean = [&](std::string name, const auto& n)
						{
							if (n)
								item.emplace(std::move(name), sdk::detached_cell::boolean(*n));
						};
						number("original_expression_ordinal",
							   occurrence.original_expression_ordinal);
						number("block_ordinal", occurrence.block_ordinal);
						number("instruction_ordinal", occurrence.instruction_ordinal);
						number("successor_ordinal", occurrence.successor_ordinal);
						number("intrinsic_id", occurrence.intrinsic_id);
						number("compiler_route", occurrence.compiler_route);
						number("emitter_methods", occurrence.emitter_methods);
						boolean("is_invoke", occurrence.is_invoke);
						boolean("does_not_throw", occurrence.does_not_throw);
						boolean("does_not_return", occurrence.does_not_return);
						auto row = bounds.row(std::move(item));
						const auto& identity = std::get<std::string>(
							*row.cells.at("cc.exceptional_exit.v1.exit").value);
						bounds.retain(identity.size() + sizeof(std::string) * 2U);
						members.push_back(identity);
						output.rows.push_back(std::move(row));
					}
				}
				auto add = [&](std::string name, sdk::detached_cell value)
				{
					facets.fields.emplace(std::move(name), std::move(value));
				};
				add("exceptional_exit_count", sdk::detached_cell::unsigned_integer(members.size()));
				add("exceptional_exit_ids", bounds.set(members));
				add("exceptional_exit_state",
					bounds.text(scope.complete				   ? "complete"
									: original.hooks_installed ? "partial"
															   : "unsupported"));
				add("exceptional_exit_profile", bounds.text(profile));
				add("exceptional_lowering_profile", bounds.text(lowering));
				output.scopes.push_back(std::move(facets));
			}
			return output;
		}
		catch (const sdk::error& error)
		{
			return sdk::unexpected(error);
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(sdk::error{
				"native.exceptional-exit-resource-exhausted", "detachment", "allocation"});
		}
		catch (const std::length_error&)
		{
			return sdk::unexpected(
				sdk::error{"native.exceptional-exit-budget", "detachment", "length"});
		}
	}
} // namespace cxxlens::detail::clang22
