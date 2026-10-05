#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <tuple>

#include <cxxlens/relations/build_compile_unit.hpp>
#include <cxxlens/relations/cc_syntax_node.hpp>
#include <cxxlens/relations/source_file.hpp>
#include <cxxlens/relations/source_preprocessor_event.hpp>
#include <cxxlens/relations/source_preprocessor_inventory.hpp>
#include <cxxlens/relations/source_span.hpp>
#include <cxxlens/relations/source_token.hpp>
#include <cxxlens/relations/source_token_inventory.hpp>
#include <cxxlens/sdk/preprocessor.hpp>

#include "query_projection_plan_limits_internal.hpp"
#include "query_projection_rows_internal.hpp"

namespace cxxlens::sdk::query
{
	namespace
	{
		using key = std::array<std::string, 4U>;
		using owner_key = std::array<std::string, 8U>;
		using indices = std::vector<std::size_t>;
		constexpr std::array<std::string_view, 8U> relations{"build.compile_unit.v1",
															 "source.file.v1",
															 "source.span.v1",
															 "source.preprocessor_inventory.v1",
															 "source.preprocessor_event.v1",
															 "source.token_inventory.v1",
															 "source.token.v1",
															 "cc.syntax_node.v1"};
		struct failure
		{
			error value;
		};
		[[noreturn]] void fail(std::string field,
							   std::string detail,
							   std::string code = "sdk.preprocessor-input-invalid")
		{
			throw failure{{std::move(code), std::move(field), std::move(detail)}};
		}
		const detached_cell* cell(const annotated_row& row, std::string_view name)
		{
			const auto found = row.values.find("output." + std::string{name});
			return found != row.values.end() && found->second.state == cell_state::present &&
					found->second.value
				? &found->second
				: nullptr;
		}
		std::optional<std::string> optional_text(const annotated_row& row, std::string_view name)
		{
			const auto* c = cell(row, name);
			const auto* value = c ? std::get_if<std::string>(&*c->value) : nullptr;
			return value ? std::optional{*value} : std::nullopt;
		}
		std::string text(const annotated_row& row, std::string_view name)
		{
			return optional_text(row, name).value_or("");
		}
		std::optional<std::uint64_t> number(const annotated_row& row, std::string_view name)
		{
			const auto* c = cell(row, name);
			const auto* value = c ? std::get_if<std::uint64_t>(&*c->value) : nullptr;
			return value ? std::optional{*value} : std::nullopt;
		}
		std::optional<bool> boolean(const annotated_row& row, std::string_view name)
		{
			const auto* c = cell(row, name);
			const auto* value = c ? std::get_if<bool>(&*c->value) : nullptr;
			return value ? std::optional{*value} : std::nullopt;
		}
		key world(std::string id, const annotated_row& row, const std::string& variant)
		{
			return {std::move(id), row.presence.universe, variant, row.interpretation};
		}
		owner_key owner(const annotated_row& row,
						const std::string& variant,
						std::string file,
						std::string snapshot)
		{
			return {text(row, "compile_unit"),
					std::move(file),
					std::move(snapshot),
					text(row, "phase"),
					text(row, "profile"),
					row.presence.universe,
					variant,
					row.interpretation};
		}
		key event_world(const preprocessor_event& event, std::string id)
		{
			return {std::move(id), event.universe, event.variant, event.interpretation};
		}
		preprocessor_state state(std::string_view value)
		{
			if (value == "complete")
				return preprocessor_state::complete;
			if (value == "partial")
				return preprocessor_state::partial;
			if (value == "unavailable")
				return preprocessor_state::unavailable;
			if (value == "unsupported")
				return preprocessor_state::unsupported;
			if (value == "conflicting")
				return preprocessor_state::conflicting;
			return preprocessor_state::unknown;
		}
		void weaken(preprocessor_state& value, bool conflict)
		{
			if (conflict)
				value = preprocessor_state::conflicting;
			else if (value == preprocessor_state::complete)
				value = preprocessor_state::partial;
		}
		bool raw_primary(std::string_view kind)
		{
			constexpr std::array<std::string_view, 17U> kinds{"raw_define",
															  "raw_undef",
															  "raw_include",
															  "raw_if",
															  "raw_ifdef",
															  "raw_ifndef",
															  "raw_elif",
															  "raw_elifdef",
															  "raw_elifndef",
															  "raw_else",
															  "raw_endif",
															  "raw_pragma",
															  "raw_line",
															  "raw_error",
															  "raw_warning",
															  "raw_null",
															  "raw_unknown"};
			return std::ranges::find(kinds, kind) != kinds.end();
		}
		void canonical(std::vector<query_unresolved>& gaps)
		{
			std::ranges::sort(gaps,
							  {},
							  [](const auto& v)
							  {
								  return std::tie(v.code, v.subject, v.detail);
							  });
			gaps.erase(std::ranges::unique(gaps).begin(), gaps.end());
		}
		struct budget
		{
			finite_population_limits limits;
			std::stop_token stop;
			std::size_t operations{}, retained{}, references{}, members{};
			void tick(std::size_t n = 1U)
			{
				if (stop.stop_requested() || (limits.cancelled && limits.cancelled()))
					fail("projection", "stop-requested", "sdk.preprocessor-cancelled");
				if (operations > limits.maximum_operations ||
					n > limits.maximum_operations - operations)
					fail("operations", "limit-exceeded", "sdk.preprocessor-budget");
				operations += n;
			}
			void retain(std::size_t n)
			{
				if (retained > limits.maximum_retained_bytes ||
					n > limits.maximum_retained_bytes - retained)
					fail("retained-bytes", "limit-exceeded", "sdk.preprocessor-budget");
				retained += n;
			}
			void evidence(indices& target, const indices& refs)
			{
				tick(refs.size());
				if (references > limits.maximum_evidence_references ||
					refs.size() > limits.maximum_evidence_references - references)
					fail("evidence-references", "limit-exceeded", "sdk.preprocessor-budget");
				references += refs.size();
				retain(refs.size() * sizeof(std::size_t));
				target.insert(target.end(), refs.begin(), refs.end());
			}
			void member()
			{
				tick();
				if (members >= limits.maximum_members)
					fail("events", "limit-exceeded", "sdk.preprocessor-budget");
				++members;
			}
			std::optional<std::vector<std::string>> strings(const annotated_row& row,
															std::string_view field)
			{
				const auto* c = cell(row, field);
				if (!c)
					return {};
				const auto* bytes = std::get_if<std::vector<std::byte>>(&*c->value);
				if (!bytes)
					fail(std::string{field}, "set-bytes-required");
				tick(bytes->size());
				retain(bytes->size());
				std::vector<std::string> result;
				std::size_t at{};
				while (at < bytes->size())
				{
					tick();
					if (bytes->size() - at < 4U)
						fail(std::string{field}, "invalid-set-length");
					std::uint32_t n{};
					for (unsigned shift{}; shift < 32U; shift += 8U)
						n |= std::uint32_t(std::to_integer<unsigned char>((*bytes)[at++])) << shift;
					if (!n || n > 4096U || n > bytes->size() - at)
						fail(std::string{field}, "invalid-set-element");
					if (result.size() >= limits.maximum_members)
						fail(std::string{field}, "limit-exceeded", "sdk.preprocessor-budget");
					retain(n + sizeof(std::string));
					std::string id;
					id.reserve(n);
					for (std::uint32_t i{}; i < n; ++i)
						id.push_back(std::to_integer<char>((*bytes)[at++]));
					if (!result.empty() && result.back() >= id)
						fail(std::string{field}, "canonical-set-required");
					result.push_back(std::move(id));
				}
				return result;
			}
		};
		struct indexed
		{
			indices refs;
			std::string payload, pragma_payload;
			bool conflict{}, pragma_conflict{};
		};
		using index = std::map<key, indexed>;
		struct work
		{
			budget b;
			preprocessor_projection output;
			std::array<index, 8U> maps;
			std::map<owner_key, std::set<std::string>> observed, inventory_owners;
			const annotated_row& row(std::size_t ref) const
			{
				return output.evidence.at(ref).row;
			}
			const indexed* find(std::size_t group, const key& k)
			{
				b.tick();
				const auto found = maps[group].find(k);
				return found == maps[group].end() ? nullptr : &found->second;
			}
			void gap(preprocessor_population& p,
					 std::string code,
					 std::string detail = {},
					 bool conflict = false,
					 bool enumeration = true)
			{
				b.tick();
				b.retain(code.size() + detail.size() + p.id.size() + sizeof(query_unresolved));
				p.gaps.push_back(
					{std::move(code), p.id.empty() ? p.source_snapshot : p.id, std::move(detail)});
				if (enumeration)
					weaken(p.state, conflict);
			}
			void gap(preprocessor_event& e, std::string code, std::string detail = {})
			{
				b.tick();
				b.retain(code.size() + detail.size() + e.id.size() + sizeof(query_unresolved));
				e.gaps.push_back({std::move(code), e.id, std::move(detail)});
			}
			bool source(preprocessor_event& e, std::string id)
			{
				const auto* span = find(2U, event_world(e, id));
				if (!span || span->conflict)
					return false;
				const auto& s = row(span->refs.front());
				const auto snapshot = text(s, "snapshot"), file = text(s, "file");
				const auto* f = find(1U, event_world(e, snapshot));
				if (!f || f->conflict || text(row(f->refs.front()), "file") != file)
					return false;
				const auto begin = number(s, "begin"), end = number(s, "end"),
						   size = number(row(f->refs.front()), "size");
				if (!begin || !end || !size || *begin > *end || *end > *size)
					return false;
				b.evidence(e.evidence, span->refs);
				b.evidence(e.evidence, f->refs);
				if (id == e.source_span)
				{
					e.file = file;
					e.source_snapshot = snapshot;
					e.source_begin = begin;
					e.source_end = end;
				}
				return true;
			}
			bool token(preprocessor_event& e, const std::string& id, bool same_source = false)
			{
				const auto* found = find(6U, event_world(e, id));
				if (!found || found->conflict)
					return false;
				const auto& t = row(found->refs.front());
				if (text(t, "compile_unit") != e.compile_unit || text(t, "phase") != "raw")
					return false;
				const auto* span = find(2U, event_world(e, text(t, "source")));
				if (!span || span->conflict)
					return false;
				const auto& s = row(span->refs.front());
				if (same_source &&
					(text(s, "file") != e.file || text(s, "snapshot") != e.source_snapshot))
					return false;
				if (!source(e, text(t, "source")))
					return false;
				b.evidence(e.evidence, found->refs);
				return true;
			}
			preprocessor_event event(const key& k, const indexed& input)
			{
				b.member();
				const auto& r = row(input.refs.front());
				b.retain(input.payload.size() * 2U + sizeof(preprocessor_event));
				preprocessor_event e;
				e.id = k[0];
				e.universe = k[1];
				e.variant = k[2];
				e.interpretation = k[3];
				e.compile_unit = text(r, "compile_unit");
				e.source_span = text(r, "source");
				e.kind = text(r, "kind");
				e.phase = text(r, "phase");
				e.profile = text(r, "profile");
				e.symbol = text(r, "symbol");
				e.display_value = text(r, "value");
				e.observation_state = text(r, "state");
				e.raw_event = optional_text(r, "raw_event");
				e.parent_event = optional_text(r, "parent_event");
				e.closing_event = optional_text(r, "closing_event");
				e.branch_event = optional_text(r, "branch_event");
				e.region_begin = number(r, "region_begin");
				e.region_end = number(r, "region_end");
				e.depth = number(r, "depth");
				e.structure_state = state(text(r, "structure_state"));
				e.activity = optional_text(r, "activity");
				e.feature_symbols = b.strings(r, "feature_symbols");
				e.condition_tokens = b.strings(r, "condition_tokens");
				e.function_like = boolean(r, "function_like");
				e.variadic = boolean(r, "variadic");
				e.parameter_count = number(r, "parameter_count");
				e.replacement_count = number(r, "replacement_count");
				e.parameter_index = number(r, "parameter_index");
				e.argument_index = number(r, "argument_index");
				e.parameter_symbol = optional_text(r, "parameter_symbol");
				e.raw_token_ids = b.strings(r, "raw_token_ids");
				e.stringify_token_ids = b.strings(r, "stringify_token_ids");
				e.paste_token_ids = b.strings(r, "paste_token_ids");
				e.substitution_count = number(r, "substitution_count");
				e.evaluating_substitution_count = number(r, "evaluating_substitution_count");
				e.argument_source = optional_text(r, "argument_source");
				e.argument_empty = boolean(r, "argument_empty");
				if (const auto* c = cell(r, "spelling_bytes"))
				{
					if (const auto* bytes = std::get_if<std::vector<std::byte>>(&*c->value))
					{
						b.retain(bytes->size());
						e.spelling_bytes = *bytes;
					}
					else
						fail("spelling_bytes", "bytes-required");
				}
				e.parent_expansion = optional_text(r, "parent_expansion");
				e.expansion_depth = number(r, "expansion_depth");
				e.definition_event = optional_text(r, "definition_event");
				e.effect_expression = optional_text(r, "effect_expression");
				e.effect_function = optional_text(r, "effect_function");
				e.argument_may_have_side_effects = boolean(r, "argument_may_have_side_effects");
				e.effect_state = state(text(r, "effect_state"));
				e.macro_state = state(text(r, "macro_state"));
				e.pragma_kind = optional_text(r, "pragma_kind");
				e.pragma_profile = optional_text(r, "pragma_profile");
				e.pragma_state = state(text(r, "pragma_state"));
				if (e.pragma_kind)
					b.retain(e.pragma_kind->size());
				if (e.pragma_profile)
					b.retain(e.pragma_profile->size());
				b.evidence(e.evidence, input.refs);
				if (e.pragma_state == preprocessor_state::complete)
				{
					constexpr std::array<std::string_view, 10> kinds{"pack",
																	 "once",
																	 "push_macro",
																	 "pop_macro",
																	 "diagnostic",
																	 "visibility",
																	 "weak",
																	 "message",
																	 "omp",
																	 "other"};
					if (e.kind != "raw_pragma" || e.phase != "raw")
					{
						weaken(e.pragma_state, true);
						gap(e, "sdk.preprocessor-pragma-applicability-conflicting");
					}
					else if (!e.pragma_profile ||
							 *e.pragma_profile != "clang22-original-raw-pragma-kind/1")
					{
						e.pragma_state = preprocessor_state::unsupported;
						gap(e, "sdk.preprocessor-pragma-profile-unsupported");
					}
					else if (!e.pragma_kind ||
							 std::ranges::find(kinds, *e.pragma_kind) == kinds.end())
					{
						weaken(e.pragma_state, false);
						gap(e, "sdk.preprocessor-pragma-kind-unavailable");
					}
				}
				if (input.pragma_conflict)
				{
					weaken(e.pragma_state, true);
					gap(e, "sdk.preprocessor-pragma-conflicting");
				}
				if (input.conflict)
				{
					weaken(e.pragma_state, true);
					gap(e, "sdk.preprocessor-event-conflicting");
				}
				if (!source(e, e.source_span))
				{
					weaken(e.pragma_state, false);
					gap(e, "sdk.preprocessor-source-unbound", e.source_span);
				}
				if (e.argument_source && !source(e, *e.argument_source))
				{
					gap(e, "sdk.preprocessor-argument-source-unbound", *e.argument_source);
					weaken(e.effect_state, false);
				}
				for (const auto* ids : {&e.raw_token_ids,
										&e.condition_tokens,
										&e.stringify_token_ids,
										&e.paste_token_ids})
					if (*ids)
						for (const auto& id : **ids)
						{
							b.tick();
							if (!token(e, id, ids == &e.condition_tokens))
							{
								gap(e, "sdk.preprocessor-token-unbound", id);
								weaken(e.macro_state, false);
								weaken(e.structure_state, false);
							}
						}
				if (e.region_begin && e.region_end)
				{
					const auto* f = find(1U, event_world(e, e.source_snapshot));
					const auto size = f ? number(row(f->refs.front()), "size") : std::nullopt;
					if (!size || *e.region_begin > *e.region_end || *e.region_end > *size)
					{
						gap(e, "sdk.preprocessor-region-conflicting");
						weaken(e.structure_state, true);
					}
				}
				else if (e.region_begin || e.region_end)
				{
					gap(e, "sdk.preprocessor-region-incomplete");
					weaken(e.structure_state, false);
				}
				if (e.structure_state == preprocessor_state::complete &&
					(!e.region_begin || !e.region_end || !e.depth))
				{
					gap(e, "sdk.preprocessor-structure-facet-missing");
					weaken(e.structure_state, false);
				}
				if (e.macro_state == preprocessor_state::complete &&
					(e.kind == "raw_define" || e.kind == "macro_definition") &&
					(!e.function_like || !e.variadic || !e.parameter_count ||
					 !e.replacement_count || !e.raw_token_ids || !e.stringify_token_ids ||
					 !e.paste_token_ids))
				{
					gap(e, "sdk.preprocessor-macro-facet-missing");
					weaken(e.macro_state, false);
				}
				if (e.effect_state == preprocessor_state::complete)
				{
					bool bound = e.argument_may_have_side_effects.has_value();
					if (e.effect_expression)
					{
						const auto* node = find(7U, event_world(e, *e.effect_expression));
						bound &= node && !node->conflict;
						if (node)
						{
							const auto& n = row(node->refs.front());
							bound &= text(n, "compile_unit") == e.compile_unit &&
								optional_text(n, "function") == e.effect_function;
							b.evidence(e.evidence, node->refs);
							bound &= source(e, text(n, "source"));
						}
					}
					else
						bound &= e.argument_empty == true &&
							e.argument_may_have_side_effects == false && !e.effect_function;
					if (!bound)
					{
						gap(e, "sdk.preprocessor-effect-unbound");
						weaken(e.effect_state, false);
					}
				}
				return e;
			}
		};
		void append_gap(work& w, preprocessor_population& p, const preprocessor_event& e)
		{
			for (const auto& g : e.gaps)
			{
				w.gap(p, g.code, g.detail, false, false);
				if (g.code == "sdk.preprocessor-event-conflicting")
					weaken(p.state, true);
				if (g.code == "sdk.preprocessor-source-unbound")
					weaken(p.state, false);
			}
			constexpr std::array<std::string_view, 18U> conditional_kinds{"raw_if",
																		  "raw_ifdef",
																		  "raw_ifndef",
																		  "raw_elif",
																		  "raw_elifdef",
																		  "raw_elifndef",
																		  "raw_else",
																		  "raw_endif",
																		  "if",
																		  "ifdef",
																		  "ifndef",
																		  "elif",
																		  "elifdef",
																		  "elifndef",
																		  "else",
																		  "endif",
																		  "skipped_range",
																		  "defined_test"};
			const bool structure = e.structure_state != preprocessor_state::unknown ||
				std::ranges::find(conditional_kinds, e.kind) != conditional_kinds.end();
			const bool macro = e.macro_state != preprocessor_state::unknown ||
				e.kind == "raw_define" || e.kind == "macro_definition" ||
				e.kind == "macro_parameter" || e.kind == "macro_replacement_operator";
			const bool effect =
				e.effect_state != preprocessor_state::unknown || e.kind == "macro_argument";
			if (structure && e.structure_state != preprocessor_state::complete)
				weaken(p.structure_state, e.structure_state == preprocessor_state::conflicting);
			if (macro && e.macro_state != preprocessor_state::complete)
				weaken(p.macro_state, e.macro_state == preprocessor_state::conflicting);
			if (effect && e.effect_state != preprocessor_state::complete)
				weaken(p.argument_effect_state, e.effect_state == preprocessor_state::conflicting);
		}
	} // namespace

	result<preprocessor_projection> project_preprocessor(preprocessor_input input,
														 finite_population_limits limits,
														 std::stop_token stop)
	{
		if (auto valid = limits.validate(); !valid)
			return valid.error();
		try
		{
			work w{{limits, stop}, {}, {}, {}, {}};
			w.b.tick();
			w.output.compile_units_complete = input.compile_units_complete;
			w.output.inventory_inputs_complete = input.inventory_inputs_complete;
			w.output.event_inputs_complete = input.event_inputs_complete;
			w.output.token_inputs_complete = input.token_inputs_complete;
			w.output.syntax_inputs_complete = input.syntax_inputs_complete;
			const std::array groups{input.units,
									input.files,
									input.spans,
									input.inventories,
									input.events,
									input.token_inventories,
									input.tokens,
									input.syntax_nodes};
			constexpr std::array<std::string_view, 8U> id_fields{"compile_unit",
																 "snapshot",
																 "span",
																 "inventory",
																 "event",
																 "inventory",
																 "token",
																 "node"};
			struct entry
			{
				std::size_t group;
				const annotated_row* row;
				std::string encoded;
			};
			std::vector<entry> entries;
			std::size_t count{}, expanded{}, evidence_bytes{};
			for (std::size_t group{}; group < groups.size(); ++group)
				for (const auto& row : groups[group])
				{
					w.b.tick();
					if (count >= limits.maximum_rows)
						fail("rows", "limit-exceeded", "sdk.preprocessor-budget");
					++count;
					const std::array<const relation_descriptor*, 8U> descriptors{
						&build::relations::compile_unit::descriptor(),
						&source::relations::file::descriptor(),
						&source::relations::span::descriptor(),
						&source::relations::preprocessor_inventory::descriptor(),
						&source::relations::preprocessor_event::descriptor(),
						&source::relations::token_inventory::descriptor(),
						&source::relations::token::descriptor(),
						&cc::relations::syntax_node::descriptor()};
					if (auto valid = detail::validate_projected_relation_row(
							row,
							*descriptors[group],
							"sdk.preprocessor-input-invalid",
							[&]
							{
								w.b.tick();
							});
						!valid)
						return valid.error();
					if (!row.presence.validate() || row.interpretation.empty())
						fail("condition", "validated-world-required");
					if (expanded > limits.maximum_condition_expansions ||
						row.presence.fragments.size() >
							limits.maximum_condition_expansions - expanded)
						fail("condition-expansions", "limit-exceeded", "sdk.preprocessor-budget");
					expanded += row.presence.fragments.size();
					const auto encoded = row.canonical_form();
					w.b.tick(encoded.size());
					if (evidence_bytes > limits.maximum_evidence_bytes ||
						encoded.size() > limits.maximum_evidence_bytes - evidence_bytes)
						fail("evidence-bytes", "limit-exceeded", "sdk.preprocessor-budget");
					evidence_bytes += encoded.size();
					w.b.retain(encoded.size() * 3U + sizeof(entry) + sizeof(preprocessor_evidence));
					entries.push_back({group, &row, encoded});
				}
			std::ranges::sort(entries,
							  {},
							  [](const auto& e)
							  {
								  return std::tie(e.group, e.encoded);
							  });
			for (const auto& item : entries)
			{
				w.b.tick();
				const auto ref = w.output.evidence.size();
				w.output.evidence.push_back({std::string{relations[item.group]}, *item.row});
				std::string payload, pragma_payload;
				for (const auto& [name, c] : item.row->values)
				{
					w.b.tick();
					const auto part = name + '=' + c.canonical_form() + '\n';
					if (item.group == 4U &&
						(name == "output.pragma_kind" || name == "output.pragma_state" ||
						 name == "output.pragma_profile"))
						pragma_payload += part;
					else
						payload += part;
				}
				const auto id = text(*item.row, id_fields[item.group]);
				if (id.empty())
					fail(std::string{id_fields[item.group]}, "identity-required");
				for (const auto& variant : item.row->presence.fragments)
				{
					w.b.tick();
					const auto k = world(id, *item.row, variant);
					w.b.retain(payload.size() + pragma_payload.size() + 256U);
					for (const auto& part : k)
						w.b.retain(part.size());
					auto& indexed = w.maps[item.group][k];
					if (indexed.refs.empty())
					{
						indexed.payload = payload;
						indexed.pragma_payload = pragma_payload;
					}
					else
					{
						indexed.conflict |= indexed.payload != payload;
						indexed.pragma_conflict |= indexed.pragma_payload != pragma_payload;
					}
					w.b.evidence(indexed.refs, {ref});
				}
			}
			using phase_key = std::array<std::string, 5U>;
			std::set<phase_key> owner_unknown;
			for (const auto& [k, entry] : w.maps[4U])
			{
				w.b.tick();
				const auto& r = w.row(entry.refs.front());
				const auto unit = text(r, "compile_unit"), phase = text(r, "phase"),
						   profile = text(r, "profile");
				const auto* span = w.find(2U, world(text(r, "source"), r, k[2]));
				if (!span || span->conflict || phase.empty() || profile.empty())
				{
					const phase_key pk{unit, phase, k[1], k[2], k[3]};
					w.b.retain(512U);
					for (const auto& part : pk)
						w.b.retain(part.size());
					owner_unknown.insert(pk);
					w.b.retain(k[0].size() + 128U);
					w.output.unresolved.push_back({"sdk.preprocessor-event-owner-unbound",
												   k[0],
												   "supply-original-source-and-profile"});
					continue;
				}
				if (!w.find(0U, world(unit, r, k[2])))
				{
					w.output.compile_units_complete = false;
					w.b.retain(k[0].size() + unit.size() + 128U);
					w.output.unresolved.push_back(
						{"sdk.preprocessor-event-unit-unbound", k[0], unit});
				}
				const auto& bound = w.row(span->refs.front());
				const auto o = owner(r, k[2], text(bound, "file"), text(bound, "snapshot"));
				w.b.retain(k[0].size() + 256U);
				for (const auto& part : o)
					w.b.retain(part.size());
				w.observed[o].insert(k[0]);
			}
			for (const auto& [k, entry] : w.maps[3U])
			{
				w.b.tick();
				const auto& r = w.row(entry.refs.front());
				const auto o = owner(r, k[2], text(r, "file"), text(r, "source_snapshot"));
				w.b.retain(k[0].size() + 256U);
				for (const auto& part : o)
					w.b.retain(part.size());
				w.inventory_owners[o].insert(k[0]);
			}
			using raw_key = std::array<std::string, 5U>;
			struct raw_stream
			{
				std::uint64_t count{};
				std::set<std::string> directives;
				indices evidence;
				bool invalid{}, conflicting{};
			};
			std::map<raw_key, raw_stream> raw_streams;
			std::map<raw_key, std::vector<key>> raw_inventories;
			std::set<key> unbound_raw;
			for (const auto& [id, entry] : w.maps[6U])
			{
				w.b.tick();
				const auto& r = w.row(entry.refs.front());
				if (text(r, "phase") != "raw")
					continue;
				const auto unit = text(r, "compile_unit");
				const auto* span = w.find(2U, world(text(r, "source"), r, id[2]));
				if (!span || span->conflict)
				{
					w.b.retain(unit.size() + 128U);
					unbound_raw.insert(world(unit, r, id[2]));
					continue;
				}
				const auto& bound = w.row(span->refs.front());
				const auto snapshot = text(bound, "snapshot");
				const raw_key rk{unit, snapshot, id[1], id[2], id[3]};
				w.b.retain(unit.size() + snapshot.size() + 256U);
				for (const auto& part : rk)
					w.b.retain(part.size());
				auto& stream = raw_streams[rk];
				++stream.count;
				stream.conflicting |= entry.conflict;
				stream.invalid |= text(r, "profile") != "clang22.tokens.lexical-pp-template.v1" ||
					!boolean(r, "directive_start");
				const auto* file = w.find(1U, world(snapshot, r, id[2]));
				const auto begin = number(bound, "begin"), end = number(bound, "end"),
						   size = file ? number(w.row(file->refs.front()), "size") : std::nullopt;
				stream.invalid |= !file || !begin || !end || !size ||
					(begin && end && *begin > *end) || (end && size && *end > *size) ||
					(file && text(w.row(file->refs.front()), "file") != text(bound, "file"));
				if (boolean(r, "directive_start") == true)
				{
					w.b.retain(id[0].size() + 96U);
					stream.directives.insert(id[0]);
					w.b.evidence(stream.evidence, entry.refs);
					w.b.evidence(stream.evidence, span->refs);
				}
			}
			for (const auto& [id, entry] : w.maps[5U])
			{
				w.b.tick();
				const auto& r = w.row(entry.refs.front());
				if (text(r, "phase") != "raw")
					continue;
				const raw_key rk{
					text(r, "compile_unit"), text(r, "source_snapshot"), id[1], id[2], id[3]};
				w.b.retain(512U);
				for (const auto& part : rk)
					w.b.retain(part.size());
				for (const auto& part : id)
					w.b.retain(part.size());
				raw_inventories[rk].push_back(id);
			}
			std::set<owner_key> admitted;
			for (const auto& [k, entry] : w.maps[3U])
			{
				w.b.tick();
				if (w.output.populations.size() >= limits.maximum_populations)
					fail("populations", "limit-exceeded", "sdk.preprocessor-budget");
				const auto& r = w.row(entry.refs.front());
				w.b.retain(entry.payload.size() * 2U + sizeof(preprocessor_population));
				preprocessor_population p;
				p.id = k[0];
				p.universe = k[1];
				p.variant = k[2];
				p.interpretation = k[3];
				p.compile_unit = text(r, "compile_unit");
				p.file = text(r, "file");
				p.source_snapshot = text(r, "source_snapshot");
				p.phase = text(r, "phase");
				p.profile = text(r, "profile");
				p.declared_count = number(r, "event_count");
				p.state = state(text(r, "enumeration_state"));
				p.structure_state = state(text(r, "structure_state"));
				p.activity_state = state(text(r, "activity_state"));
				p.macro_state = state(text(r, "macro_state"));
				p.candidate_state = state(text(r, "candidate_state"));
				p.expansion_state = state(text(r, "expansion_state"));
				p.argument_effect_state = state(text(r, "argument_effect_state"));
				p.reason = text(r, "reason");
				w.b.evidence(p.evidence, entry.refs);
				const auto o = owner(r, k[2], p.file, p.source_snapshot);
				admitted.insert(o);
				if (entry.conflict || w.inventory_owners.at(o).size() != 1U)
					w.gap(p, "sdk.preprocessor-inventory-conflicting", p.id, true);
				const auto* unit = w.find(0U, world(p.compile_unit, r, k[2]));
				if (!unit)
					w.gap(p, "sdk.preprocessor-unit-missing", p.compile_unit);
				else
				{
					w.b.evidence(p.evidence, unit->refs);
					if (unit->conflict)
						w.gap(p, "sdk.preprocessor-unit-conflicting", p.compile_unit, true);
				}
				const auto* file = w.find(1U, world(p.source_snapshot, r, k[2]));
				if (!file || text(w.row(file->refs.front()), "file") != p.file)
					w.gap(p, "sdk.preprocessor-file-unbound", p.file);
				else
				{
					w.b.evidence(p.evidence, file->refs);
					p.source_size = number(w.row(file->refs.front()), "size");
					if (file->conflict)
						w.gap(p, "sdk.preprocessor-file-conflicting", p.file, true);
				}
				const std::string expected = p.phase == "raw" ? "clang22-frozen-raw-directives/1"
					: p.phase == "evaluated"				  ? "clang22-evaluated-preprocessor/1"
															  : "";
				if (expected.empty() || p.profile != expected)
					w.gap(p, "sdk.preprocessor-profile-unsupported", p.profile);
				if (!input.inventory_inputs_complete)
					w.gap(p, "sdk.preprocessor-inventory-scan-incomplete");
				if (!input.event_inputs_complete)
					w.gap(p, "sdk.preprocessor-event-scan-incomplete");
				if (owner_unknown.contains(
						{p.compile_unit, p.phase, p.universe, p.variant, p.interpretation}) ||
					owner_unknown.contains(
						{p.compile_unit, "", p.universe, p.variant, p.interpretation}))
					w.gap(p, "sdk.preprocessor-event-owner-unbound", p.compile_unit);
				const auto listed = w.b.strings(r, "event_ids");
				if (!listed || !p.declared_count || *p.declared_count != listed->size())
					w.gap(p, "sdk.preprocessor-count-conflicting", p.id, true);
				if (listed)
					p.event_ids = *listed;
				std::map<std::string, std::size_t, std::less<>> members;
				for (const auto& id : p.event_ids)
				{
					w.b.tick();
					const auto* found = w.find(4U, world(id, r, k[2]));
					if (!found)
					{
						w.gap(p, "sdk.preprocessor-event-missing", id);
						continue;
					}
					auto e = w.event(world(id, r, k[2]), *found);
					if (e.compile_unit != p.compile_unit || e.file != p.file ||
						e.source_snapshot != p.source_snapshot || e.phase != p.phase ||
						e.profile != p.profile)
						w.gap(p, "sdk.preprocessor-event-owner-conflicting", id, true);
					append_gap(w, p, e);
					w.b.retain(id.size() + 128U);
					members.emplace(id, p.events.size());
					p.events.push_back(std::move(e));
				}
				if (const auto found = w.observed.find(o); found != w.observed.end())
					for (const auto& id : found->second)
					{
						w.b.tick();
						if (!std::ranges::binary_search(p.event_ids, id))
							w.gap(p, "sdk.preprocessor-event-unlisted", id, true);
					}
				for (auto& e : p.events)
				{
					const auto link = [&](const std::optional<std::string>& id,
										  std::string_view field,
										  bool raw,
										  bool local)
					{
						w.b.tick();
						if (!id)
							return;
						const auto* found = w.find(4U, event_world(e, *id));
						bool bound = found && !found->conflict;
						if (found)
						{
							const auto& linked = w.row(found->refs.front());
							bound &= text(linked, "compile_unit") == e.compile_unit;
							if (raw)
								bound &= text(linked, "phase") == "raw";
							else
								bound &= text(linked, "phase") == e.phase;
							if (local)
								bound &= members.contains(*id);
							w.b.evidence(e.evidence, found->refs);
						}
						if (!bound)
						{
							w.gap(
								e, "sdk.preprocessor-link-unbound", std::string{field} + ":" + *id);
							if (field == "parent_expansion" || field == "definition_event")
								weaken(p.expansion_state, false);
							else
								weaken(p.structure_state, false);
						}
					};
					link(e.raw_event, "raw_event", true, false);
					link(e.parent_event, "parent_event", false, true);
					link(e.closing_event, "closing_event", false, true);
					link(e.branch_event, "branch_event", false, true);
					link(e.definition_event, "definition_event", true, false);
					link(e.parent_expansion, "parent_expansion", false, false);
					if (e.effect_state == preprocessor_state::complete &&
						!input.syntax_inputs_complete && e.effect_expression)
					{
						w.gap(e, "sdk.preprocessor-syntax-scan-incomplete");
						weaken(e.effect_state, false);
						weaken(p.argument_effect_state, false);
					}
				}
				for (const bool expansion : {false, true})
					for (auto& e : p.events)
					{
						w.b.tick();
						std::set<std::string> chain;
						const auto* current = &e;
						while (current)
						{
							w.b.tick();
							const auto& parent =
								expansion ? current->parent_expansion : current->parent_event;
							if (!parent)
								break;
							w.b.retain(parent->size() + 96U);
							if (!chain.insert(*parent).second)
							{
								w.gap(e, "sdk.preprocessor-link-cycle", *parent);
								if (expansion)
									weaken(p.expansion_state, true);
								else
									weaken(p.structure_state, true);
								break;
							}
							const auto found = members.find(*parent);
							current = found == members.end() ? nullptr : &p.events[found->second];
						}
					}
				if (p.phase == "raw")
				{
					struct macro_children
					{
						std::map<std::uint64_t, std::string> parameters;
						std::set<std::string> stringify, paste;
						bool conflicting{};
					};
					std::map<std::string, macro_children> children;
					for (const auto& e : p.events)
						if (e.kind == "macro_parameter" || e.kind == "macro_replacement_operator")
						{
							w.b.tick();
							if (!e.raw_event || e.parent_event != e.raw_event)
							{
								w.gap(
									p, "sdk.preprocessor-macro-owner-unbound", e.id, false, false);
								weaken(p.macro_state, false);
								continue;
							}
							const auto parent = members.find(*e.raw_event);
							if (parent == members.end() ||
								p.events[parent->second].kind != "raw_define")
							{
								w.gap(
									p, "sdk.preprocessor-macro-owner-unbound", e.id, false, false);
								weaken(p.macro_state, false);
								continue;
							}
							w.b.retain(e.raw_event->size() + 256U);
							auto& group = children[*e.raw_event];
							if (e.kind == "macro_parameter")
							{
								if (!e.parameter_index || !e.parameter_symbol)
								{
									w.gap(p,
										  "sdk.preprocessor-parameter-facet-missing",
										  e.id,
										  false,
										  false);
									weaken(p.macro_state, false);
									continue;
								}
								w.b.retain(e.parameter_symbol->size() + 96U);
								group.conflicting |=
									!group.parameters
										 .emplace(*e.parameter_index, *e.parameter_symbol)
										 .second;
							}
							else
							{
								if (!e.stringify_token_ids || !e.paste_token_ids)
								{
									w.gap(p,
										  "sdk.preprocessor-operator-facet-missing",
										  e.id,
										  false,
										  false);
									weaken(p.macro_state, false);
									continue;
								}
								for (const auto& id : *e.stringify_token_ids)
								{
									w.b.tick();
									w.b.retain(id.size() + 96U);
									group.conflicting |= !group.stringify.insert(id).second;
								}
								for (const auto& id : *e.paste_token_ids)
								{
									w.b.tick();
									w.b.retain(id.size() + 96U);
									group.conflicting |= !group.paste.insert(id).second;
								}
							}
						}
					for (auto& e : p.events)
						if (e.kind == "raw_define" && e.macro_state == preprocessor_state::complete)
						{
							w.b.tick();
							const auto found = children.find(e.id);
							const macro_children empty;
							const auto& group = found == children.end() ? empty : found->second;
							bool conflict = group.conflicting || !e.parameter_count ||
								group.parameters.size() != e.parameter_count ||
								(!e.function_like.value_or(false) && !group.parameters.empty());
							std::uint64_t expected_index{};
							for (const auto& [idx, symbol] : group.parameters)
							{
								w.b.tick();
								(void)symbol;
								conflict |= idx != expected_index++;
							}
							if (!e.stringify_token_ids || !e.paste_token_ids ||
								!std::ranges::equal(group.stringify, *e.stringify_token_ids) ||
								!std::ranges::equal(group.paste, *e.paste_token_ids))
								conflict = true;
							if (conflict)
							{
								w.gap(p,
									  "sdk.preprocessor-macro-population-conflicting",
									  e.id,
									  true,
									  false);
								weaken(p.macro_state, true);
								weaken(e.macro_state, true);
							}
						}
					const raw_key rk{
						p.compile_unit, p.source_snapshot, p.universe, p.variant, p.interpretation};
					const auto inventory = raw_inventories.find(rk);
					const auto stream = raw_streams.find(rk);
					const std::set<std::string> empty_directives;
					const auto& actual_directives =
						stream == raw_streams.end() ? empty_directives : stream->second.directives;
					if (stream != raw_streams.end())
					{
						w.b.evidence(p.evidence, stream->second.evidence);
						if (stream->second.invalid)
							w.gap(p, "sdk.preprocessor-directive-classification-missing");
						if (stream->second.conflicting)
							w.gap(p, "sdk.preprocessor-token-conflicting", p.source_snapshot, true);
					}
					if (inventory == raw_inventories.end() || !input.token_inputs_complete)
						w.gap(p, "sdk.preprocessor-token-scan-incomplete");
					else
					{
						if (inventory->second.size() != 1U)
							w.gap(p,
								  "sdk.preprocessor-token-inventory-conflicting",
								  p.source_snapshot,
								  true);
						for (const auto& tk : inventory->second)
						{
							w.b.tick();
							const auto& indexed = w.maps[5U].at(tk);
							const auto& t = w.row(indexed.refs.front());
							w.b.evidence(p.evidence, indexed.refs);
							if (indexed.conflict)
								w.gap(
									p, "sdk.preprocessor-token-inventory-conflicting", tk[0], true);
							if (text(t, "enumeration_state") != "complete" ||
								text(t, "file") != p.file ||
								text(t, "profile") != "clang22.tokens.lexical-pp-template.v1")
								w.gap(p, "sdk.preprocessor-token-enumeration-incomplete", tk[0]);
							if (number(t, "token_count") !=
									(stream == raw_streams.end() ? 0U : stream->second.count) ||
								number(t, "directive_count") != actual_directives.size())
								w.gap(
									p, "sdk.preprocessor-directive-count-conflicting", tk[0], true);
						}
					}
					if (unbound_raw.contains(
							{p.compile_unit, p.universe, p.variant, p.interpretation}))
						w.gap(p, "sdk.preprocessor-token-source-unbound");
					std::set<std::string> admitted_directives;
					for (auto& e : p.events)
						if (raw_primary(e.kind) && !e.raw_event)
						{
							w.b.tick();
							std::size_t starts{};
							if (e.raw_token_ids)
								for (const auto& id : *e.raw_token_ids)
								{
									w.b.tick();
									if (actual_directives.contains(id))
									{
										++starts;
										w.b.retain(id.size() + 96U);
										if (!admitted_directives.insert(id).second)
											w.gap(p,
												  "sdk.preprocessor-directive-duplicate",
												  id,
												  true);
									}
								}
							if (starts != 1U)
								w.gap(p, "sdk.preprocessor-directive-admission-missing", e.id);
						}
					if (actual_directives != admitted_directives)
						w.gap(p, "sdk.preprocessor-directive-population-conflicting", p.id, true);
				}
				for (auto& e : p.events)
				{
					canonical(e.gaps);
					w.output.unresolved.insert(
						w.output.unresolved.end(), e.gaps.begin(), e.gaps.end());
				}
				canonical(p.gaps);
				w.output.unresolved.insert(w.output.unresolved.end(), p.gaps.begin(), p.gaps.end());
				w.output.populations.push_back(std::move(p));
			}
			using unit_file_key = std::array<std::string, 6U>;
			std::map<unit_file_key, indices> unit_files;
			std::set<key> associated_units;
			const auto associate = [&](const annotated_row& r,
									   const std::string& variant,
									   const std::string& file,
									   const std::string& snapshot,
									   const indices& refs)
			{
				w.b.tick();
				const auto unit = text(r, "compile_unit");
				const auto* source = w.find(1U, world(snapshot, r, variant));
				const auto* owner = w.find(0U, world(unit, r, variant));
				if (!source || !owner || text(w.row(source->refs.front()), "file") != file)
				{
					w.b.retain(unit.size() + snapshot.size() + 128U);
					w.output.unresolved.push_back(
						{"sdk.preprocessor-unit-file-unbound", unit, snapshot});
					return;
				}
				const unit_file_key k{
					unit, file, snapshot, r.presence.universe, variant, r.interpretation};
				w.b.retain(512U);
				for (const auto& part : k)
					w.b.retain(part.size());
				auto& evidence = unit_files[k];
				w.b.evidence(evidence, refs);
				w.b.evidence(evidence, source->refs);
				w.b.evidence(evidence, owner->refs);
				w.b.retain(unit.size() + 256U);
				associated_units.insert(world(unit, r, variant));
			};
			// A source file's existence does not imply that every TU opened it. Only original
			// token/preprocessor rows establish the actual unit/file/snapshot membership.
			for (auto group : {3U, 5U})
				for (const auto& [k, entry] : w.maps[group])
				{
					w.b.tick();
					const auto& r = w.row(entry.refs.front());
					associate(r, k[2], text(r, "file"), text(r, "source_snapshot"), entry.refs);
				}
			for (auto group : {4U, 6U})
				for (const auto& [k, entry] : w.maps[group])
				{
					w.b.tick();
					const auto& r = w.row(entry.refs.front());
					if (const auto* span = w.find(2U, world(text(r, "source"), r, k[2])))
					{
						const auto& bound = w.row(span->refs.front());
						associate(
							r, k[2], text(bound, "file"), text(bound, "snapshot"), entry.refs);
					}
				}
			for (const auto& [unit, refs] : w.maps[0U])
				if (!associated_units.contains(unit))
				{
					w.b.tick();
					w.b.retain(unit[0].size() + 256U);
					w.output.unresolved.push_back(
						{"sdk.preprocessor-unit-files-unobserved",
						 unit[0],
						 "supply-original-unit-file-token-or-preprocessor-membership"});
				}
			for (const auto& [k, refs] : unit_files)
				for (const auto phase : {"raw", "evaluated"})
				{
					w.b.tick();
					const std::string profile = std::string{phase} == "raw"
						? "clang22-frozen-raw-directives/1"
						: "clang22-evaluated-preprocessor/1";
					const owner_key o{k[0], k[1], k[2], phase, profile, k[3], k[4], k[5]};
					if (admitted.contains(o))
						continue;
					if (w.output.populations.size() >= limits.maximum_populations)
						fail("populations", "limit-exceeded", "sdk.preprocessor-budget");
					w.b.retain(512U + profile.size());
					for (const auto& part : k)
						w.b.retain(part.size());
					preprocessor_population p;
					p.compile_unit = k[0];
					p.file = k[1];
					p.source_snapshot = k[2];
					p.phase = phase;
					p.profile = profile;
					p.universe = k[3];
					p.variant = k[4];
					p.interpretation = k[5];
					const auto* source = w.find(1U, key{k[2], k[3], k[4], k[5]});
					p.source_size = number(w.row(source->refs.front()), "size");
					w.b.evidence(p.evidence, refs);
					w.gap(p, "sdk.preprocessor-inventory-missing", p.source_snapshot);
					w.output.unresolved.insert(
						w.output.unresolved.end(), p.gaps.begin(), p.gaps.end());
					w.output.populations.push_back(std::move(p));
				}

			std::ranges::sort(w.output.populations,
							  {},
							  [](const auto& p)
							  {
								  return std::tie(p.universe,
												  p.variant,
												  p.interpretation,
												  p.compile_unit,
												  p.file,
												  p.source_snapshot,
												  p.phase,
												  p.profile,
												  p.id);
							  });
			for (auto& p : w.output.populations)
			{
				std::ranges::sort(p.evidence);
				p.evidence.erase(std::ranges::unique(p.evidence).begin(), p.evidence.end());
				for (auto& e : p.events)
				{
					std::ranges::sort(e.evidence);
					e.evidence.erase(std::ranges::unique(e.evidence).begin(), e.evidence.end());
				}
			}
			canonical(w.output.unresolved);
			return std::move(w.output);
		}
		catch (const failure& f)
		{
			return f.value;
		}
		catch (const std::bad_alloc&)
		{
			return error{"sdk.preprocessor-resource-exhausted", "projection", "allocation"};
		}
		catch (const std::length_error&)
		{
			return error{"sdk.preprocessor-resource-exhausted", "projection", "length"};
		}
	}

	result<preprocessor_projection> project_preprocessor(const application_query_results& input,
														 finite_population_limits limits,
														 std::stop_token stop)
	{
		if (auto valid = limits.validate(); !valid)
			return valid.error();
		try
		{
			budget b{limits, stop};
			b.tick();
			std::size_t source_plan_bytes{};
			if (auto bounded = detail::check_source_plan_limits(input,
																limits.maximum_source_queries,
																limits.maximum_source_plan_bytes,
																stop,
																"sdk.preprocessor",
																&source_plan_bytes,
																limits.cancelled);
				!bounded)
				return bounded.error();
			b.retain(source_plan_bytes);
			std::array<std::vector<annotated_row>, 8U> groups;
			std::array<bool, 8U> present{}, complete{};
			complete.fill(true);
			std::size_t count{}, bytes{};
			for (const auto& scan : input.scans)
			{
				b.tick();
				const auto found = std::ranges::find(relations, scan.relation_id);
				if (found == relations.end())
					continue;
				const auto group = static_cast<std::size_t>(found - relations.begin());
				present[group] = true;
				complete[group] &= scan.result.execution() == execution_status::complete &&
					scan.result.inputs_complete() && scan.result.conflicts().empty() &&
					scan.result.differential_disagreements().empty();
				auto cursor = scan.result.rows();
				while (true)
				{
					b.tick();
					auto next = cursor.next();
					if (!next)
						return next.error();
					if (!*next)
						break;
					if (count >= limits.maximum_rows)
						fail("scan-rows", "limit-exceeded", "sdk.preprocessor-budget");
					++count;
					auto row = (*next)->copy();
					if (!row)
						return row.error();
					const auto n = row->canonical_form().size();
					if (bytes > limits.maximum_evidence_bytes ||
						n > limits.maximum_evidence_bytes - bytes)
						fail("scan-bytes", "limit-exceeded", "sdk.preprocessor-budget");
					bytes += n;
					b.tick(n);
					b.retain(n + sizeof(annotated_row));
					groups[group].push_back(std::move(*row));
				}
			}
			if (b.retained >= limits.maximum_retained_bytes ||
				b.operations >= limits.maximum_operations)
				fail("projection", "limit-exceeded", "sdk.preprocessor-budget");
			auto remaining = limits;
			remaining.maximum_retained_bytes -= b.retained;
			remaining.maximum_operations -= b.operations;
			auto output =
				project_preprocessor({groups[0],
									  groups[1],
									  groups[2],
									  groups[3],
									  groups[4],
									  groups[5],
									  groups[6],
									  groups[7],
									  present[0] && complete[0],
									  present[3] && complete[3],
									  present[4] && complete[4],
									  present[5] && complete[5] && present[6] && complete[6],
									  present[7] && complete[7]},
									 remaining,
									 stop);
			if (!output)
				return output;
			for (std::size_t group{}; group < relations.size(); ++group)
				if (!present[group])
					output->unresolved.push_back({"sdk.preprocessor-scan-missing",
												  std::string{relations[group]},
												  "supply-independent-public-scan"});
			canonical(output->unresolved);
			output->source_queries = input;
			return output;
		}
		catch (const failure& f)
		{
			return f.value;
		}
		catch (const std::bad_alloc&)
		{
			return error{"sdk.preprocessor-resource-exhausted", "projection", "allocation"};
		}
		catch (const std::length_error&)
		{
			return error{"sdk.preprocessor-resource-exhausted", "projection", "length"};
		}
	}
} // namespace cxxlens::sdk::query
