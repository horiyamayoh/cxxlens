#include "project_exceptional_exit_rows.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <tuple>

#include <cxxlens/relations/cc_exceptional_block.hpp>
#include <cxxlens/relations/cc_exceptional_exit.hpp>
#include <cxxlens/relations/cc_exceptional_successor.hpp>
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
			sdk::detached_row row(fields input,
								  const sdk::relation_descriptor& descriptor =
									  cc::relations::exceptional_exit::descriptor())
			{
				if (members++ >= limits.maximum_occurrences)
					throw sdk::error{"native.exceptional-exit-budget", "occurrences", "detachment"};
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
				key.type = descriptor.column(*descriptor.domain_identity.result_column)->type;
				result.cells.insert_or_assign(*descriptor.domain_identity.result_column,
											  std::move(key));
				if (auto valid = sdk::validate_row(descriptor, result); !valid)
					throw valid.error();
				if (auto valid = sdk::validate_domain_identity(descriptor, result); !valid)
					throw valid.error();
				return result;
			}
			sdk::detached_cell set(std::vector<std::string>& values,
								   std::string_view domain = "exceptional_exit_id")
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
				return {{sdk::scalar_kind::set, std::string(domain), true},
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
					const auto carrier_index = output.rows.size();
					output.rows.push_back(std::move(carrier));
					std::map<std::pair<std::uint64_t, std::uint64_t>, std::size_t> invokes;
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
						number("cleanup_registration_ordinal",
							   occurrence.cleanup_registration_ordinal);
						number("cleanup_emission_ordinal", occurrence.cleanup_emission_ordinal);
						if (!occurrence.cleanup_declaration.empty())
							item.emplace("cleanup_declaration",
										 bounds.text(occurrence.cleanup_declaration));
						if (!occurrence.cleanup_route.empty())
							item.emplace("cleanup_route", bounds.text(occurrence.cleanup_route));
						if (!occurrence.cleanup_profile.empty())
							item.emplace("cleanup_profile",
										 bounds.text(occurrence.cleanup_profile));
						if (!occurrence.cleanup_target.empty())
							item.emplace("cleanup_target", bounds.text(occurrence.cleanup_target));
						if (!occurrence.cleanup_target_usr.empty())
							item.emplace("cleanup_target_usr",
										 bounds.binary(occurrence.cleanup_target_usr));
						number("cleanup_target_dtor_type", occurrence.cleanup_target_dtor_type);
						if (!occurrence.cleanup_target_profile.empty())
							item.emplace("cleanup_target_profile",
										 bounds.text(occurrence.cleanup_target_profile));

						if (occurrence.eh_boundary_observed)
						{
							item.emplace("eh_selected_scope_kind",
										 bounds.text(occurrence.eh_selected_scope_kind));
							item.emplace("eh_disposition", bounds.text(occurrence.eh_disposition));
							item.emplace("eh_boundary_profile",
										 bounds.text("clang22-original-invoke-eh-boundary/1"));
							item.emplace("eh_boundary_state",
										 bounds.text(occurrence.eh_disposition == "unknown"
														 ? "unknown"
														 : "complete"));
							if (!occurrence.eh_boundary_declaration.empty())
								item.emplace("eh_boundary_declaration",
											 bounds.text(occurrence.eh_boundary_declaration));
						}
						boolean("is_invoke", occurrence.is_invoke);
						boolean("does_not_throw", occurrence.does_not_throw);
						boolean("does_not_return", occurrence.does_not_return);
						auto row = bounds.row(std::move(item));
						const auto& identity = std::get<std::string>(
							*row.cells.at("cc.exceptional_exit.v1.exit").value);
						bounds.retain(identity.size() + sizeof(std::string) * 2U);
						members.push_back(identity);
						if (occurrence.is_invoke == true && occurrence.block_ordinal &&
							occurrence.instruction_ordinal)
						{
							bounds.retain(sizeof(decltype(invokes)::value_type) + 128U);
							if (!invokes
									 .emplace(std::make_pair(*occurrence.block_ordinal,
															 *occurrence.instruction_ordinal),
											  output.rows.size())
									 .second)
								throw sdk::error{"native.exceptional-exit-invalid",
												 "invoke",
												 "duplicate-original-instruction"};
						}
						output.rows.push_back(std::move(row));
					}
					if (variant.topology_observed)
					{
						constexpr std::string_view topology =
							"clang22-original-lowering-topology/1";
						const auto& exit_descriptor = cc::relations::exceptional_exit::descriptor();
						const auto& block_descriptor =
							cc::relations::exceptional_block::descriptor();
						const auto& edge_descriptor =
							cc::relations::exceptional_successor::descriptor();
						auto put =
							[&](std::size_t index, std::string_view name, sdk::detached_cell cell)
						{
							const auto key = "cc.exceptional_exit.v1." + std::string(name);
							cell.type = exit_descriptor.column(key)->type;
							output.rows.at(index).cells.insert_or_assign(key, std::move(cell));
						};
						if (variant.topology_complete &&
							std::ranges::count(
								variant.blocks, true, &exceptional_original_block::is_entry) != 1)
							throw sdk::error{"native.exceptional-exit-invalid",
											 "entry",
											 "original-entry-required"};
						std::map<std::uint64_t, std::string> blocks;
						std::map<std::uint64_t, std::uint64_t> instruction_counts;
						std::set<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>>
							successor_slots;
						std::vector<std::string> block_ids, edge_ids;
						for (const auto& block : variant.blocks)
						{
							bounds.work();
							if (variant.topology_complete &&
								(!block.instruction_count || !block.terminator_opcode))
								throw sdk::error{"native.exceptional-exit-invalid",
												 "block",
												 "original-terminator-required"};
							bounds.retain(sizeof(decltype(instruction_counts)::value_type) + 128U);
							instruction_counts.emplace(block.ordinal, block.instruction_count);
							fields block_fields{
								{"compile_unit", bounds.text(bindings.compile_unit)},
								{"variant", bounds.text(carrier_id)},
								{"profile", bounds.text(topology)},
								{"ordinal", sdk::detached_cell::unsigned_integer(block.ordinal)},
								{"instruction_count",
								 sdk::detached_cell::unsigned_integer(block.instruction_count)},
								{"is_entry", sdk::detached_cell::boolean(block.is_entry)},
								{"membership_state", bounds.text("complete")}};
							if (block.terminator_opcode)
								block_fields.emplace(
									"terminator_opcode",
									sdk::detached_cell::unsigned_integer(*block.terminator_opcode));
							if (!block.terminator_kind.empty())
								block_fields.emplace("terminator_kind",
													 bounds.text(block.terminator_kind));
							auto row = bounds.row(std::move(block_fields), block_descriptor);
							const auto& key = std::get<std::string>(
								*row.cells.at("cc.exceptional_block.v1.block").value);
							bounds.retain(sizeof(decltype(blocks)::value_type) + key.size() * 3U +
										  256U);
							if (!blocks.emplace(block.ordinal, key).second)
								throw sdk::error{"native.exceptional-exit-invalid",
												 "blocks",
												 "duplicate-original-ordinal"};
							block_ids.push_back(key);
							if (block.is_entry)
								put(carrier_index, "lowered_entry", bounds.text(key));
							output.rows.push_back(std::move(row));
						}
						for (std::size_t index = carrier_index + 1U;
							 index <= carrier_index + variant.occurrences.size();
							 ++index)
						{
							const auto& original_occurrence =
								variant.occurrences.at(index - carrier_index - 1U);
							if (original_occurrence.block_ordinal &&
								(!blocks.contains(*original_occurrence.block_ordinal) ||
								 (original_occurrence.instruction_ordinal &&
								  *original_occurrence.instruction_ordinal >=
									  instruction_counts.at(*original_occurrence.block_ordinal))))
								throw sdk::error{"native.exceptional-exit-invalid",
												 "occurrence",
												 "foreign-original-placement"};
							if (original_occurrence.block_ordinal &&
								blocks.contains(*original_occurrence.block_ordinal))
								put(index,
									"lowered_block",
									bounds.text(blocks.at(*original_occurrence.block_ordinal)));
						}
						for (const auto& edge : variant.successors)
						{
							bounds.work();
							if (!blocks.contains(edge.from) || !blocks.contains(edge.to))
								throw sdk::error{"native.exceptional-exit-invalid",
												 "successor",
												 "foreign-original-block"};
							bounds.retain(sizeof(decltype(successor_slots)::value_type) + 128U);
							if (!successor_slots
									 .emplace(edge.from,
											  edge.terminator_instruction_ordinal,
											  edge.ordinal)
									 .second ||
								edge.terminator_instruction_ordinal >=
									instruction_counts.at(edge.from) ||
								edge.terminator_instruction_ordinal !=
									instruction_counts.at(edge.from) - 1U ||
								(edge.invoke ? !((edge.ordinal == 0U && edge.kind == "normal") ||
												 (edge.ordinal == 1U && edge.kind == "unwind"))
											 : edge.kind != "ordinary"))
								throw sdk::error{"native.exceptional-exit-invalid",
												 "successor",
												 "original-terminator-correspondence"};
							fields edge_fields{
								{"compile_unit", bounds.text(bindings.compile_unit)},
								{"variant", bounds.text(carrier_id)},
								{"profile", bounds.text(topology)},
								{"from_block", bounds.text(blocks.at(edge.from))},
								{"to_block", bounds.text(blocks.at(edge.to))},
								{"terminator_instruction_ordinal",
								 sdk::detached_cell::unsigned_integer(
									 edge.terminator_instruction_ordinal)},
								{"ordinal", sdk::detached_cell::unsigned_integer(edge.ordinal)},
								{"kind", bounds.text(edge.kind)},
								{"membership_state", bounds.text("complete")}};
							const auto invocation =
								invokes.find({edge.from, edge.terminator_instruction_ordinal});
							if (edge.invoke)
							{
								if (invocation == invokes.end())
									throw sdk::error{"native.exceptional-exit-invalid",
													 "successor",
													 "original-invoke-unavailable"};
								edge_fields.emplace(
									"invoke",
									bounds.text(std::get<std::string>(
										*output.rows.at(invocation->second)
											 .cells.at("cc.exceptional_exit.v1.exit")
											 .value)));
							}
							auto row = bounds.row(std::move(edge_fields), edge_descriptor);
							const auto& key = std::get<std::string>(
								*row.cells.at("cc.exceptional_successor.v1.successor").value);
							bounds.retain(key.size() + sizeof(std::string) * 2U);
							edge_ids.push_back(key);
							if (edge.invoke)
								put(invocation->second,
									edge.kind == "normal" ? "normal_successor" : "unwind_successor",
									bounds.text(key));
							output.rows.push_back(std::move(row));
						}
						put(carrier_index,
							"lowered_block_count",
							sdk::detached_cell::unsigned_integer(block_ids.size()));
						put(carrier_index,
							"lowered_block_ids",
							bounds.set(block_ids, "exceptional_block_id"));
						put(carrier_index,
							"lowered_successor_count",
							sdk::detached_cell::unsigned_integer(edge_ids.size()));
						put(carrier_index,
							"lowered_successor_ids",
							bounds.set(edge_ids, "exceptional_successor_id"));
						put(carrier_index, "lowered_topology_profile", bounds.text(topology));
						put(carrier_index,
							"lowered_topology_state",
							bounds.text(variant.topology_complete ? "complete" : "partial"));
						for (std::size_t index = carrier_index;
							 index <= carrier_index + variant.occurrences.size();
							 ++index)
						{
							if (auto valid =
									sdk::validate_row(exit_descriptor, output.rows.at(index));
								!valid)
								throw valid.error();
							if (auto valid = sdk::validate_domain_identity(exit_descriptor,
																		   output.rows.at(index));
								!valid)
								throw valid.error();
						}
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
