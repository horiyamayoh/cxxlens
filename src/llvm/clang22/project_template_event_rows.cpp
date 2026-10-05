#include "project_template_event_rows.hpp"

#include <algorithm>
#include <array>
#include <limits>
namespace cxxlens::detail::clang22
{
	namespace
	{
		using fields = std::map<std::string, sdk::detached_cell, std::less<>>;
		struct writer
		{
			const template_event_limits& limits;
			std::size_t operations{}, bytes{}, members{};
			void work(std::size_t n = 1U)
			{
				if (limits.cancelled && limits.cancelled())
					throw sdk::error{"native.template-event-cancelled", "detachment", "cancelled"};
				if (operations > limits.maximum_operations ||
					n > limits.maximum_operations - operations)
					throw sdk::error{"native.template-event-budget", "operations", "detachment"};
				operations += n;
			}
			void retain(std::size_t n)
			{
				work();
				if (bytes > limits.maximum_retained_bytes ||
					n > limits.maximum_retained_bytes - bytes)
					throw sdk::error{
						"native.template-event-budget", "retained-bytes", "detachment"};
				bytes += n;
			}
			void member()
			{
				work();
				if (members >= limits.maximum_events)
					throw sdk::error{"native.template-event-budget", "events", "detachment"};
				++members;
			}
			sdk::detached_cell text(std::string_view value)
			{
				retain(value.size() + sizeof(std::string));
				return sdk::detached_cell::utf8(std::string(value));
			}
			std::vector<std::byte> raw(std::string_view value)
			{
				retain(value.size());
				std::vector<std::byte> out;
				out.reserve(value.size());
				for (unsigned char byte : value)
					out.push_back(static_cast<std::byte>(byte));
				return out;
			}
			sdk::detached_cell binary(std::string_view value)
			{
				return sdk::detached_cell::bytes(raw(value));
			}
			std::string hex(std::string_view value)
			{
				if (value.size() > std::numeric_limits<std::size_t>::max() / 2U)
					throw sdk::error{"native.template-event-budget", "USR", "length"};
				retain(value.size() * 2U);
				std::string out;
				out.reserve(value.size() * 2U);
				constexpr auto digits = "0123456789abcdef";
				for (unsigned char byte : value)
				{
					out.push_back(digits[byte >> 4U]);
					out.push_back(digits[byte & 15U]);
				}
				return out;
			}
			sdk::detached_cell list(std::vector<std::string> values)
			{
				work(values.size() * (sizeof(std::size_t) * 8U + 1U));
				std::ranges::sort(values);
				values.erase(std::ranges::unique(values).begin(), values.end());
				std::vector<std::byte> out;
				for (const auto& value : values)
				{
					if (value.size() > std::numeric_limits<std::uint32_t>::max())
						throw sdk::error{"native.template-event-budget", "set", "length"};
					retain(4U + value.size());
					const auto n = static_cast<std::uint32_t>(value.size());
					for (unsigned shift = 0; shift < 32; shift += 8U)
						out.push_back(static_cast<std::byte>((n >> shift) & 255U));
					for (unsigned char byte : value)
						out.push_back(static_cast<std::byte>(byte));
				}
				return {{sdk::scalar_kind::set, {}, false},
						sdk::cell_state::present,
						sdk::scalar_value{std::move(out)},
						std::nullopt};
			}
			const sdk::relation_descriptor& descriptor(std::string_view id)
			{
				for (const auto& d : sdk::standard_relation_descriptors())
				{
					work();
					if (d.id == id)
						return d;
				}
				throw sdk::error{"native.template-event-unsupported",
								 std::string(id),
								 "descriptor-unregistered"};
			}
			sdk::detached_row
			row(const sdk::relation_descriptor& d, fields values, bool validate = true)
			{
				retain(sizeof(sdk::detached_row) + values.size() * sizeof(sdk::detached_cell));
				sdk::detached_row out{d.id, {}};
				for (const auto& column : d.columns)
				{
					work();
					if (auto i = values.find(column.name); i != values.end())
					{
						auto value = std::move(i->second);
						value.type = column.type;
						out.cells.emplace(column.id, std::move(value));
					}
					else if (column.type.optional)
						out.cells.emplace(column.id, sdk::detached_cell::absent(column.type));
				}
				if (!d.domain_identity.result_column)
					throw sdk::error{
						"native.template-event-invalid", d.id, "missing-result-identity"};
				auto id = sdk::derive_domain_identity(d, out);
				if (!id)
					throw id.error();
				retain(id->size());
				auto column = d.column(*d.domain_identity.result_column);
				if (!column)
					throw column.error();
				out.cells.insert_or_assign(
					column->id, sdk::detached_cell::typed(column->type.parameter, std::move(*id)));
				if (validate)
				{
					auto valid = sdk::validate_row(d, out);
					if (!valid)
						throw valid.error();
				}
				return out;
			}
			std::string identity(const sdk::relation_descriptor& d, fields values)
			{
				auto value = row(d, std::move(values), false);
				return std::get<std::string>(
					*value.cells.at(*d.domain_identity.result_column).value);
			}
		};
	} // namespace
	sdk::result<project_template_event_rows>
	detach_project_template_events(const project_template_event_observations& observations,
								   const template_event_bindings& bindings,
								   template_event_limits limits)
	{
		try
		{
			writer w{limits};
			w.operations = observations.operations;
			w.bytes = observations.retained_bytes;
			if (!observations.frozen || bindings.compile_unit.empty())
				throw sdk::error{"native.template-event-invalid", "detachment", "phase-or-unit"};
			const auto& candidate = w.descriptor("cc.template_candidate.v1");
			const auto& root = w.descriptor("cc.constant_evaluation_root.v1");
			const auto& call = w.descriptor("cc.constant_evaluated_call.v1");
			const auto& inventory = w.descriptor("cc.template_inventory.v1");
			project_template_event_rows out;
			constexpr auto candidate_profile = "clang22-final-candidate-substitution-events/1";
			constexpr auto evaluation_profile = "clang22-legacy-constant-evaluation/1";
			auto base = [&](std::size_t ordinal, std::string_view profile)
			{
				return fields{{"compile_unit", w.text(bindings.compile_unit)},
							  {"ordinal", sdk::detached_cell::unsigned_integer(ordinal)},
							  {"profile", w.text(profile)}};
			};
			std::vector<std::string> candidate_ids, root_ids, call_ids;
			auto ids = [&](const auto& values,
						   const auto& descriptor,
						   std::string_view profile,
						   auto& target)
			{
				for (const auto& value : values)
				{
					w.member();
					w.retain(sizeof(std::string));
					target.push_back(w.identity(descriptor, base(value.ordinal, profile)));
				}
			};
			for (const auto& original : observations.candidates)
			{
				w.member();
				w.retain(sizeof(std::string));
				auto value = base(original.ordinal, candidate_profile);
				value.emplace("route", w.text(original.route));
				candidate_ids.push_back(w.identity(candidate, std::move(value)));
			}
			for (std::size_t index = 0; index < observations.roots.size(); ++index)
			{
				w.member();
				w.retain(sizeof(std::string));
				root_ids.push_back(w.identity(root, base(index, evaluation_profile)));
			}
			ids(observations.calls, call, evaluation_profile, call_ids);
			auto source =
				[&](fields& value, std::string_view column, template_event_source original)
			{
				if (bindings.source && original.begin)
				{
					w.work();
					auto id = bindings.source(original);
					w.retain(id.size());
					if (!id.empty())
						value.emplace(column, w.text(id));
				}
			};
			auto entity = [&](fields& value, std::string_view column, std::string_view usr)
			{
				if (bindings.entity && !usr.empty())
				{
					w.work();
					auto id = bindings.entity(usr);
					w.retain(id.size());
					if (!id.empty())
						value.emplace(column, w.text(id));
				}
			};
			auto references = [&](const auto& ordinals, const auto& original_ids)
			{
				std::vector<std::string> refs;
				for (auto ordinal : ordinals)
				{
					w.work();
					if (ordinal >= original_ids.size())
						throw sdk::error{"native.template-event-invalid",
										 "membership",
										 "ordinal-outside-domain"};
					w.retain(sizeof(std::string) + original_ids[ordinal].size());
					refs.push_back(original_ids[ordinal]);
				}
				return refs;
			};
			for (const auto& original : observations.candidates)
			{
				w.work();
				auto value = base(original.ordinal, candidate_profile);
				value.emplace("candidate_usr", w.binary(original.candidate_usr));
				value.emplace("route", w.text(original.route));
				value.emplace("deduction_result", w.text(original.deduction_result));
				value.emplace("exclusion_disposition", w.text(original.disposition));
				value.emplace("completed", sdk::detached_cell::boolean(original.completed));
				value.emplace("binding_state",
							  w.text(original.binding_complete ? "complete" : "partial"));
				if (original.overload_failure)
					value.emplace("overload_failure",
								  sdk::detached_cell::unsigned_integer(*original.overload_failure));
				if (original.is_system)
					value.emplace("is_system", sdk::detached_cell::boolean(*original.is_system));
				source(value, "source", original.source);
				entity(value, "candidate_entity", original.candidate_usr);
				w.retain(sizeof(sdk::detached_row));
				out.rows.push_back(w.row(candidate, std::move(value)));
			}
			constexpr std::array<std::string_view, 4> modes{"constant_expression",
															"constant_expression_unevaluated",
															"constant_fold",
															"ignore_side_effects"};
			for (std::size_t index = 0; index < observations.roots.size(); ++index)
			{
				w.work();
				const auto& original = observations.roots[index];
				if (original.mode >= modes.size())
					throw sdk::error{
						"native.template-event-invalid", "mode", "unknown-original-mode"};
				auto value = base(index, evaluation_profile);
				value.emplace("mode", w.text(modes[original.mode]));
				value.emplace("interpreter", w.text(original.bytecode ? "bytecode" : "legacy"));
				value.emplace("requested_constant_context",
							  sdk::detached_cell::boolean(original.constant_context));
				value.emplace("potential_check",
							  sdk::detached_cell::boolean(original.potential_check));
				value.emplace("fold_failure", sdk::detached_cell::boolean(original.fold_failure));
				value.emplace("completion", w.text(original.completed ? "complete" : "partial"));
				value.emplace("call_count",
							  sdk::detached_cell::unsigned_integer(original.calls.size()));
				value.emplace("call_ids", w.list(references(original.calls, call_ids)));
				const bool closed = original.completed && !original.bytecode &&
					!(original.constant_context && !original.potential_check &&
					  original.fold_failure) &&
					observations.hooks_installed;
				value.emplace("call_state",
							  w.text(original.bytecode ? "unsupported"
										 : closed	   ? "complete"
													   : "partial"));
				if (!closed && !observations.evaluation_reason.empty())
					value.emplace("reason", w.text(observations.evaluation_reason));
				w.retain(sizeof(sdk::detached_row));
				out.rows.push_back(w.row(root, std::move(value)));
			}
			constexpr std::array<std::string_view, 7> kinds{"function",
															"builtin",
															"constructor",
															"destructor",
															"allocation",
															"deallocation",
															"pseudo_destructor"};
			for (const auto& original : observations.calls)
			{
				w.work();
				if (original.kind >= kinds.size())
					throw sdk::error{"native.template-event-invalid",
									 "invocation_kind",
									 "unknown-original-kind"};
				auto value = base(original.ordinal, evaluation_profile);
				value.emplace("root_count",
							  sdk::detached_cell::unsigned_integer(original.roots.size()));
				value.emplace("root_ids", w.list(references(original.roots, root_ids)));
				source(value, "source", original.source);
				if (!original.owner_usr.empty())
				{
					value.emplace("owner_usr", w.binary(original.owner_usr));
					entity(value, "owner_entity", original.owner_usr);
				}
				std::vector<std::string> targets;
				for (const auto& usr : original.callee_usrs)
				{
					w.retain(sizeof(std::string));
					targets.push_back(w.hex(usr));
				}
				value.emplace("callee_usr_hexes", w.list(std::move(targets)));
				value.emplace("invocation_kind", w.text(kinds[original.kind]));
				if (!original.expression_kind.empty())
					value.emplace("expression_kind", w.text(original.expression_kind));
				value.emplace("depth", sdk::detached_cell::unsigned_integer(original.depth));
				if (original.is_system)
					value.emplace("is_system", sdk::detached_cell::boolean(*original.is_system));
				source(value, "lifetime_source", original.lifetime_source);
				if (!original.lifetime_usr.empty())
					value.emplace("lifetime_usr", w.binary(original.lifetime_usr));
				std::vector<sdk::canonical_value> path;
				for (const auto& step : original.lifetime_path)
				{
					w.work();
					w.retain(sizeof(sdk::canonical_value) * 5U + step.kind.size() +
							 step.member_usr.size() + 32U);
					path.push_back(sdk::canonical_value::from_tuple(
						{sdk::canonical_value::from_string(step.kind),
						 sdk::canonical_value::from_bytes(w.raw(step.member_usr)),
						 sdk::canonical_value::from_string(std::to_string(step.index)),
						 sdk::canonical_value::from_boolean(step.virtual_base)}));
				}
				auto encoded =
					sdk::canonical_binary(sdk::canonical_value::from_tuple(std::move(path)));
				if (!encoded)
					throw encoded.error();
				w.retain(encoded->size());
				value.emplace("lifetime_path", sdk::detached_cell::bytes(std::move(*encoded)));
				value.emplace("membership_state", w.text("complete"));
				value.emplace("binding_state",
							  w.text(original.binding_complete ? "complete" : "partial"));
				w.retain(sizeof(sdk::detached_row));
				out.rows.push_back(w.row(call, std::move(value)));
			}
			auto facet = [&](std::string_view field, sdk::detached_cell value)
			{
				auto column = inventory.column(inventory.id + "." + std::string(field));
				if (!column)
					throw column.error();
				value.type = column->type;
				w.retain(column->id.size() + sizeof(sdk::detached_cell));
				out.inventory_facets.emplace(column->id, std::move(value));
			};
			facet("candidate_count", sdk::detached_cell::unsigned_integer(candidate_ids.size()));
			facet("candidate_ids", w.list(std::move(candidate_ids)));
			facet("candidate_state",
				  w.text(observations.candidate_census_complete ? "complete"
							 : observations.hooks_installed		? "partial"
																: "unsupported"));
			facet("candidate_profile", w.text(candidate_profile));
			facet("evaluation_root_count", sdk::detached_cell::unsigned_integer(root_ids.size()));
			facet("evaluation_root_ids", w.list(std::move(root_ids)));
			facet("evaluation_root_state",
				  w.text(observations.evaluation_census_complete ? "complete"
							 : observations.hooks_installed		 ? "partial"
																 : "unsupported"));
			facet("evaluation_root_profile", w.text(evaluation_profile));
			return out;
		}
		catch (const sdk::error& failure)
		{
			return failure;
		}
	}
} // namespace cxxlens::detail::clang22
