#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <tuple>

#include <cxxlens/sdk/abi_surfaces.hpp>

#include "query_projection_plan_limits_internal.hpp"

namespace cxxlens::sdk::query
{
	namespace
	{
		constexpr std::array<std::string_view, 6> relations{"build.compile_unit.v1",
															"source.file.v1",
															"source.span.v1",
															"cc.entity.v1",
															"cc.entity_detail.v1",
															"cc.abi_surface.v1"};
		using world = std::array<std::string, 3>;
		using binding_key = std::tuple<std::size_t, std::string, world>;
		using surface_key = std::tuple<world, std::string, std::string, std::string, std::string>;
		struct failure
		{
			error value;
		};
		[[noreturn]] void fail(std::string code, std::string field, std::string detail)
		{
			throw failure{{std::move(code), std::move(field), std::move(detail)}};
		}
		void check(std::stop_token stop)
		{
			if (stop.stop_requested())
				fail("sdk.abi-cancelled", "projection", "stop-requested");
		}
		void
		charge(std::size_t& total, std::size_t amount, std::size_t limit, std::string_view field)
		{
			if (total > limit || amount > limit - total)
				fail("sdk.abi-budget", std::string{field}, "limit-exceeded");
			total += amount;
		}
		const detached_cell& cell(const annotated_row& row, std::string_view name)
		{
			const auto found = row.values.find("output." + std::string{name});
			if (found == row.values.end())
				fail("sdk.abi-input-invalid", std::string{name}, "column-missing");
			return found->second;
		}
		template <class T>
		const T* optional(const annotated_row& row, std::string_view name)
		{
			const auto& value = cell(row, name);
			if (value.state != cell_state::present || !value.value)
				return nullptr;
			const auto* result = std::get_if<T>(&*value.value);
			if (!result)
				fail("sdk.abi-input-invalid", std::string{name}, "scalar-type-invalid");
			return result;
		}
		std::string_view text(const annotated_row& row, std::string_view name)
		{
			const auto* value = optional<std::string>(row, name);
			return value ? std::string_view{*value} : std::string_view{};
		}
		template <class T>
		T scalar(const annotated_row& row, std::string_view name)
		{
			const auto* value = optional<T>(row, name);
			if (!value)
				fail("sdk.abi-input-invalid", std::string{name}, "present-scalar-required");
			return *value;
		}
		bool same_payload(const annotated_row& a, const annotated_row& b)
		{
			return std::ranges::equal(a.values,
									  b.values,
									  [](const auto& x, const auto& y)
									  {
										  return x.first == y.first &&
											  x.second.type == y.second.type &&
											  x.second.state == y.second.state &&
											  x.second.value == y.second.value &&
											  x.second.unknown_reason == y.second.unknown_reason;
									  });
		}
		void normalize(std::vector<query_unresolved>& gaps)
		{
			std::ranges::sort(gaps,
							  {},
							  [](const auto& gap)
							  {
								  return std::tie(gap.code, gap.subject, gap.detail);
							  });
			gaps.erase(std::ranges::unique(gaps).begin(), gaps.end());
		}
		struct budget
		{
			std::size_t bytes{}, evidence{}, references{}, operations{}, extents{};
		};
		struct entry
		{
			std::size_t group{};
			const annotated_row* row{};
			std::string canonical;
		};
		abi_surface_state state(std::string_view name)
		{
			return name == "complete" ? abi_surface_state::complete
				: name == "partial"	  ? abi_surface_state::partial
									  : abi_surface_state::unknown;
		}
		void downgrade(abi_surface_state& value, bool conflict)
		{
			if (conflict)
				value = abi_surface_state::conflicting;
			else if (value == abi_surface_state::complete)
				value = abi_surface_state::partial;
		}
		void length(std::string& value, std::uint64_t count)
		{
			for (unsigned shift{}; shift < 64U; shift += 8U)
				value.push_back(static_cast<char>((count >> shift) & 255U));
		}
		result<abi_surface_projection> project_rows(abi_surface_input input,
													abi_surface_limits limits,
													std::stop_token stop,
													budget& retained)
		{
			if (auto valid = limits.validate(); !valid)
				return valid.error();
			try
			{
				check(stop);
				const auto work = [&](std::size_t count = 1U)
				{
					check(stop);
					charge(retained.operations, count, limits.maximum_operations, "operations");
				};
				const auto bytes = [&](std::size_t count, std::string_view field)
				{
					charge(retained.bytes, count, limits.maximum_retained_bytes, field);
				};
				const auto copy_text = [&](std::string_view value) -> std::string
				{
					bytes(value.size() + sizeof(std::string), "owned-text");
					return std::string{value};
				};
				const std::array groups{input.units,
										input.files,
										input.spans,
										input.entities,
										input.details,
										input.surfaces};
				constexpr std::array<std::string_view, 6> identifiers{
					"compile_unit", "snapshot", "span", "entity", "entity", "surface"};
				std::vector<entry> entries;
				std::size_t expansions{};
				const auto descriptors = standard_relation_descriptors();
				for (std::size_t group{}; group < groups.size(); ++group)
				{
					const auto descriptor =
						std::ranges::find(descriptors, relations[group], &relation_descriptor::id);
					if (descriptor == descriptors.end())
						fail("sdk.abi-input-invalid",
							 std::string{relations[group]},
							 "descriptor-missing");
					for (const auto& row : groups[group])
					{
						work();
						if (entries.size() >= limits.maximum_rows)
							fail("sdk.abi-budget", "rows", "limit-exceeded");
						if (auto valid = row.validate(); !valid)
							return valid.error();
						for (const auto& column : descriptor->columns)
						{
							work();
							const auto& actual = cell(row, column.name);
							if (actual.type != column.type || !actual.validate())
								fail("sdk.abi-input-invalid",
									 column.id,
									 "column-type-or-value-invalid");
						}
						charge(expansions,
							   row.presence.fragments.size(),
							   limits.maximum_condition_expansions,
							   "condition-expansions");
						auto canonical = row.canonical_form();
						charge(retained.evidence,
							   canonical.size(),
							   limits.maximum_evidence_bytes,
							   "evidence-bytes");
						bytes(canonical.size(), "canonical-row");
						bytes(canonical.size(), "owned-evidence");
						bytes(sizeof(entry) + sizeof(abi_surface_evidence), "evidence-rows");
						entries.push_back({group, &row, std::move(canonical)});
					}
				}
				std::ranges::sort(entries,
								  [&](const auto& a, const auto& b)
								  {
									  work();
									  return std::tie(a.group, a.canonical) <
										  std::tie(b.group, b.canonical);
								  });
				abi_surface_projection output;
				output.compile_units_complete = input.compile_units_complete;
				output.abi_inputs_complete = input.abi_inputs_complete;
				std::map<binding_key, std::vector<std::size_t>> index;
				std::map<surface_key, std::vector<std::size_t>> surfaces;
				std::map<std::pair<world, std::string>, std::set<surface_key>> surface_ids;
				for (std::size_t ref{}; ref < entries.size(); ++ref)
				{
					work();
					const auto& e = entries[ref];
					output.evidence.push_back({copy_text(relations[e.group]), *e.row});
					const auto id = text(*e.row, identifiers[e.group]);
					for (const auto& fragment : e.row->presence.fragments)
					{
						work();
						world condition{copy_text(e.row->presence.universe),
										copy_text(fragment),
										copy_text(e.row->interpretation)};
						bytes(sizeof(binding_key) + 96U + sizeof(ref), "conditioned-index");
						charge(retained.references,
							   1U,
							   limits.maximum_evidence_references,
							   "index-references");
						index[{e.group, copy_text(id), condition}].push_back(ref);
						if (e.group != 5U)
							continue;
						bytes(sizeof(surface_key) * 2U + 192U + sizeof(ref), "surface-index");
						const surface_key identity{condition,
												   copy_text(text(*e.row, "compile_unit")),
												   copy_text(text(*e.row, "entity")),
												   copy_text(text(*e.row, "source")),
												   copy_text(text(*e.row, "profile"))};
						surfaces[identity].push_back(ref);
						surface_ids[{condition, copy_text(id)}].insert(identity);
					}
				}
				for (const auto& [identity, candidates] : surfaces)
				{
					work();
					const auto& condition = std::get<0>(identity);
					std::map<std::string, std::vector<std::size_t>> payloads;
					std::set<std::string> abi_payloads, layout_payloads;
					for (const auto ref : candidates)
					{
						work();
						std::array<std::string, 3> keys;
						for (const auto& [name, value] : entries[ref].row->values)
						{
							work();
							const auto canonical = value.canonical_form();
							const bool abi = name == "output.abi_state" ||
								name == "output.abi_context" || name == "output.abi_signature" ||
								name == "output.abi_fingerprint";
							const bool layout = name == "output.layout_state" ||
								name == "output.byte_size" || name == "output.byte_alignment" ||
								name == "output.occupied_ranges";
							for (std::size_t key{}; key < keys.size(); ++key)
								if (!key || (key == 1U && abi) || (key == 2U && layout))
								{
									bytes(name.size() + canonical.size() + 2U,
										  "candidate-payloads");
									keys[key] += name + ":" + canonical + "\n";
								}
						}
						bytes(288U + sizeof(ref), "candidate-payload-index");
						payloads[std::move(keys[0])].push_back(ref);
						abi_payloads.insert(std::move(keys[1]));
						layout_payloads.insert(std::move(keys[2]));
					}
					for (const auto& [payload, refs] : payloads)
					{
						work();
						if (output.surfaces.size() >= limits.maximum_surfaces)
							fail("sdk.abi-budget", "surfaces", "limit-exceeded");
						bytes(sizeof(abi_surface), "surface-output");
						const auto& row = *entries[refs.front()].row;
						abi_surface value;
						value.id = copy_text(text(row, "surface"));
						value.entity = copy_text(text(row, "entity"));
						value.compile_unit = copy_text(text(row, "compile_unit"));
						value.source_span = copy_text(text(row, "source"));
						value.kind = copy_text(text(row, "kind"));
						value.profile = copy_text(text(row, "profile"));
						value.universe = copy_text(condition[0]);
						value.variant = copy_text(condition[1]);
						value.interpretation = copy_text(condition[2]);
						const auto abi_state = text(row, "abi_state"),
								   layout_state = text(row, "layout_state");
						value.abi_state = state(abi_state);
						value.layout_state = state(layout_state);
						const auto gap = [&](std::string code,
											 std::string detail,
											 bool conflict = false,
											 bool abi = true,
											 bool layout = true)
						{
							bytes(sizeof(query_unresolved) + code.size() + value.id.size() +
									  detail.size(),
								  "frontier-bytes");
							charge(retained.references,
								   1U,
								   limits.maximum_evidence_references,
								   "frontier-references");
							value.gaps.push_back({std::move(code), value.id, std::move(detail)});
							if (abi)
								downgrade(value.abi_state, conflict);
							if (layout)
								downgrade(value.layout_state, conflict);
						};
						const auto retain = [&](std::size_t ref)
						{
							charge(retained.references,
								   1U,
								   limits.maximum_evidence_references,
								   "output-references");
							bytes(sizeof(ref), "output-references");
							value.evidence.push_back(ref);
						};
						const auto lookup =
							[&](std::size_t group,
								const std::string& id) -> const std::vector<std::size_t>&
						{
							work();
							const auto found = index.find({group, id, condition});
							static const std::vector<std::size_t> empty;
							return found == index.end() ? empty : found->second;
						};
						const auto bind = [&](std::size_t group,
											  const std::string& id,
											  std::string_view name) -> const annotated_row*
						{
							const auto& bound = lookup(group, id);
							if (bound.empty())
							{
								gap("sdk.abi-binding-missing", std::string{name} + ":" + id);
								return nullptr;
							}
							bool consistent = true;
							for (const auto ref : bound)
							{
								work();
								retain(ref);
								if (!same_payload(*entries[bound.front()].row, *entries[ref].row))
									consistent = false;
							}
							if (!consistent)
							{
								gap("sdk.abi-binding-conflicting",
									std::string{name} + ":" + id,
									true);
								return nullptr;
							}
							return entries[bound.front()].row;
						};
						for (const auto ref : refs)
							retain(ref);
						if (abi_payloads.size() > 1U)
							gap("sdk.abi-interface-candidate-conflicting",
								"retain-every-conditioned-candidate",
								true,
								true,
								false);
						if (layout_payloads.size() > 1U)
							gap("sdk.abi-layout-candidate-conflicting",
								"retain-every-conditioned-candidate",
								true,
								false,
								true);
						if (surface_ids.at({condition, value.id}).size() > 1U)
							gap("sdk.abi-identity-conflicting", value.id, true);
						for (const auto ref : candidates)
							if (text(*entries[ref].row, "kind") != value.kind ||
								text(*entries[ref].row, "surface") != value.id)
								gap("sdk.abi-identity-conflicting", value.id, true);
						(void)bind(0U, value.compile_unit, "compile-unit");
						if (const auto* entity = bind(3U, value.entity, "entity"))
						{
							const auto kind = text(*entity, "kind");
							const bool record =
								kind == "class" || kind == "struct" || kind == "union";
							const bool function = kind == "function" || kind == "method" ||
								kind == "constructor" || kind == "destructor" ||
								kind == "conversion";
							if ((value.kind == "record" && !record) ||
								(value.kind == "function" && !function))
								gap("sdk.abi-entity-kind-conflicting", value.entity, true);
						}
						if (const auto* span = bind(2U, value.source_span, "source-span"))
						{
							value.file = copy_text(text(*span, "file"));
							value.source_snapshot = copy_text(text(*span, "snapshot"));
							if (const auto* file =
									bind(1U, value.source_snapshot, "source-snapshot"))
								if (text(*file, "file") != value.file ||
									scalar<std::uint64_t>(*span, "begin") >
										scalar<std::uint64_t>(*span, "end") ||
									scalar<std::uint64_t>(*span, "end") >
										scalar<std::uint64_t>(*file, "size"))
									gap("sdk.abi-source-conflicting", value.source_span, true);
						}
						bool declaration{}, definition{}, nondefinition{};
						const annotated_row* first_detail{};
						for (const auto ref : lookup(4U, value.entity))
						{
							work();
							const auto& detail = *entries[ref].row;
							if (text(detail, "compile_unit") != value.compile_unit ||
								text(detail, "source") != value.source_span)
								continue;
							retain(ref);
							declaration = true;
							if (first_detail && !same_payload(*first_detail, detail))
								gap("sdk.abi-declaration-conflicting", value.entity, true);
							else if (!first_detail)
								first_detail = &detail;
							if (scalar<bool>(detail, "is_definition"))
								definition = true;
							else
								nondefinition = true;
						}
						if (!declaration)
							gap("sdk.abi-declaration-missing", value.entity);
						if (definition && nondefinition)
							gap("sdk.abi-declaration-conflicting", value.entity, true);
						if (value.kind == "record" &&
							(abi_state == "complete" || layout_state == "complete") &&
							declaration && !definition)
							gap("sdk.abi-definition-conflicting",
								"record-definition-required-for-complete-facet",
								true,
								abi_state == "complete",
								layout_state == "complete");
						if (const auto* size = optional<std::uint64_t>(row, "byte_size"))
							value.byte_size = *size;
						if (const auto* alignment = optional<std::uint64_t>(row, "byte_alignment"))
							value.byte_alignment = *alignment;
						if ((cell(row, "byte_size").state == cell_state::absent) !=
							(cell(row, "byte_alignment").state == cell_state::absent))
							gap("sdk.abi-layout-payload-conflicting",
								"size-and-alignment-must-be-supplied-together",
								true,
								false,
								true);
						const auto* ranges =
							optional<std::vector<std::byte>>(row, "occupied_ranges");
						bool canonical = true;
						std::uint64_t occupied{};
						if (ranges)
						{
							if (ranges->size() % 16U)
								fail("sdk.abi-input-invalid",
									 "occupied_ranges",
									 "extent-pair-truncated");
							const auto count = ranges->size() / 16U;
							charge(retained.extents, count, limits.maximum_extents, "extents");
							bytes(ranges->size(), "occupied-extents");
							value.occupied_ranges.reserve(count);
							for (std::size_t offset{}; offset < ranges->size(); offset += 16U)
							{
								work();
								abi_byte_extent extent;
								for (unsigned shift{}; shift < 64U; shift += 8U)
								{
									extent.begin |= std::to_integer<std::uint64_t>(
														(*ranges)[offset + shift / 8U])
										<< shift;
									extent.end |= std::to_integer<std::uint64_t>(
													  (*ranges)[offset + 8U + shift / 8U])
										<< shift;
								}
								if (extent.begin >= extent.end ||
									(!value.occupied_ranges.empty() &&
									 value.occupied_ranges.back().end >= extent.begin) ||
									(value.byte_size && extent.end > *value.byte_size))
									canonical = false;
								if (extent.begin < extent.end)
								{
									const auto size = extent.end - extent.begin;
									if (size > std::numeric_limits<std::uint64_t>::max() - occupied)
										canonical = false;
									else
										occupied += size;
								}
								value.occupied_ranges.push_back(extent);
							}
							if (!canonical)
								gap("sdk.abi-layout-extents-conflicting",
									"canonical-object-storage-extents-required",
									true,
									false,
									true);
						}
						if (layout_state == "complete")
						{
							if (value.kind != "record")
								gap("sdk.abi-layout-kind-conflicting",
									value.kind,
									true,
									false,
									true);
							if (!value.byte_size || !value.byte_alignment || !ranges)
								gap("sdk.abi-layout-payload-missing",
									"complete-layout-requires-size-alignment-extents",
									true,
									false,
									true);
							else if (!*value.byte_alignment || occupied > *value.byte_size ||
									 *value.byte_size % *value.byte_alignment != 0U)
								gap("sdk.abi-layout-size-conflicting",
									"positive-alignment-and-aligned-object-size-required",
									true,
									false,
									true);
						}
						if (const auto* context = optional<std::string>(row, "abi_context"))
							value.abi_context = copy_text(*context);
						if (const auto* fingerprint = optional<std::string>(row, "abi_fingerprint"))
							value.abi_fingerprint = copy_text(*fingerprint);
						if (const auto* signature =
								optional<std::vector<std::byte>>(row, "abi_signature"))
						{
							bytes(signature->size(), "abi-signature");
							value.abi_signature = *signature;
						}
						if ((cell(row, "abi_signature").state == cell_state::absent) !=
							(cell(row, "abi_fingerprint").state == cell_state::absent))
							gap("sdk.abi-interface-payload-conflicting",
								"signature-and-fingerprint-must-be-supplied-together",
								true,
								true,
								false);
						if (abi_state == "complete" &&
							(!value.abi_context || !value.abi_fingerprint || !value.abi_signature ||
							 value.abi_signature->empty()))
							gap("sdk.abi-interface-payload-missing",
								"complete-interface-requires-context-signature-fingerprint",
								true,
								true,
								false);
						const bool supported =
							value.profile == "clang22-storage-and-call-interface/1";
						if (supported && value.abi_context && value.abi_signature &&
							value.abi_fingerprint)
						{
							bytes(16U, "fingerprint-input");
							bytes(value.abi_context->size(), "fingerprint-input");
							bytes(value.abi_signature->size(), "fingerprint-input");
							std::string fingerprint_input;
							length(fingerprint_input, value.abi_context->size());
							fingerprint_input += *value.abi_context;
							length(fingerprint_input, value.abi_signature->size());
							for (const auto byte : *value.abi_signature)
							{
								work();
								fingerprint_input.push_back(std::to_integer<char>(byte));
							}
							auto fingerprint =
								semantic_digest("cc.clang22.abi-surface.v1", fingerprint_input);
							if (!fingerprint)
								return fingerprint.error();
							if (*fingerprint != *value.abi_fingerprint)
								gap("sdk.abi-fingerprint-conflicting",
									"context-signature-digest-mismatch",
									true,
									true,
									false);
						}
						if (!supported)
						{
							if (value.abi_state != abi_surface_state::conflicting)
								value.abi_state = abi_surface_state::unknown;
							if (value.layout_state != abi_surface_state::conflicting)
								value.layout_state = abi_surface_state::unknown;
							gap("sdk.abi-profile-unsupported", value.profile);
						}
						if (value.kind != "record" && value.kind != "function")
						{
							if (value.abi_state != abi_surface_state::conflicting)
								value.abi_state = abi_surface_state::unknown;
							if (value.layout_state != abi_surface_state::conflicting)
								value.layout_state = abi_surface_state::unknown;
							gap("sdk.abi-kind-unsupported", value.kind);
						}
						if (abi_state != "complete" && abi_state != "partial" &&
							abi_state != "unknown")
							gap("sdk.abi-interface-state-unsupported",
								std::string{abi_state},
								false,
								true,
								false);
						if (layout_state != "complete" && layout_state != "partial" &&
							layout_state != "unknown")
							gap("sdk.abi-layout-state-unsupported",
								std::string{layout_state},
								false,
								false,
								true);
						if (const auto reason = text(row, "reason"); !reason.empty())
							gap("sdk.abi-producer-frontier",
								std::string{reason},
								false,
								abi_state != "complete",
								layout_state != "complete");
						if (value.abi_state != abi_surface_state::complete &&
							abi_state != "complete")
							gap("sdk.abi-interface-unavailable",
								std::string{text(row, "reason")},
								false,
								true,
								false);
						if (value.layout_state != abi_surface_state::complete &&
							layout_state != "complete")
							gap("sdk.abi-layout-unavailable",
								std::string{text(row, "reason")},
								false,
								false,
								true);
						if (value.layout_state == abi_surface_state::complete && canonical &&
							ranges && value.byte_size)
						{
							value.occupied_bytes = occupied;
							value.padding_bytes = *value.byte_size - occupied;
						}
						std::ranges::sort(value.evidence);
						value.evidence.erase(std::ranges::unique(value.evidence).begin(),
											 value.evidence.end());
						normalize(value.gaps);
						output.surfaces.push_back(std::move(value));
					}
				}
				if (!input.observations_complete)
					output.unresolved.push_back({"sdk.abi-observations-partial",
												 "projection",
												 "local-interface-does-not-close-project"});
				return output;
			}
			catch (const failure& value)
			{
				return value.value;
			}
			catch (const std::bad_alloc&)
			{
				return error{"sdk.abi-resource-exhausted", "projection", "allocation"};
			}
			catch (const std::length_error&)
			{
				return error{"sdk.abi-resource-exhausted", "projection", "length"};
			}
		}
	} // namespace
	result<void> abi_surface_limits::validate() const
	{
		if (!maximum_rows || !maximum_condition_expansions || !maximum_evidence_bytes ||
			!maximum_retained_bytes || !maximum_evidence_references || !maximum_surfaces ||
			!maximum_extents || !maximum_operations || !maximum_source_queries ||
			!maximum_source_plan_bytes)
			return error{"sdk.abi-limit-invalid", "limits", "positive-required"};
		return {};
	}
	result<abi_surface_projection>
	project_abi_surfaces(abi_surface_input input, abi_surface_limits limits, std::stop_token stop)
	{
		budget retained;
		return project_rows(input, limits, stop, retained);
	}
	result<abi_surface_projection> project_abi_surfaces(const application_query_results& input,
														abi_surface_limits limits,
														std::stop_token stop)
	{
		if (auto valid = limits.validate(); !valid)
			return valid.error();
		try
		{
			check(stop);
			if (auto valid = detail::check_source_plan_limits(input,
															  limits.maximum_source_queries,
															  limits.maximum_source_plan_bytes,
															  stop,
															  "sdk.abi");
				!valid)
				return valid.error();
			budget retained;
			std::array<std::vector<annotated_row>, 6> groups;
			std::array<bool, 6> present{}, scan_complete{};
			scan_complete.fill(true);
			std::size_t rows{};
			for (const auto& scan : input.scans)
			{
				check(stop);
				const auto found = std::ranges::find(relations, scan.relation_id);
				if (found == relations.end())
					continue;
				const auto group = static_cast<std::size_t>(found - relations.begin());
				present[group] = true;
				scan_complete[group] &= scan.result.execution() == execution_status::complete &&
					scan.result.inputs_complete() && scan.result.conflicts().empty() &&
					scan.result.differential_disagreements().empty();
				auto cursor = scan.result.rows();
				while (true)
				{
					check(stop);
					auto next = cursor.next();
					if (!next)
						return next.error();
					if (!*next)
						break;
					charge(rows, 1U, limits.maximum_rows, "rows");
					auto row = (*next)->copy();
					if (!row)
						return row.error();
					const auto canonical = row->canonical_form();
					charge(retained.evidence,
						   canonical.size(),
						   limits.maximum_evidence_bytes,
						   "detached-scan-evidence");
					charge(retained.bytes,
						   canonical.size() + sizeof(annotated_row),
						   limits.maximum_retained_bytes,
						   "detached-scan-rows");
					groups[group].push_back(std::move(*row));
				}
			}
			bool complete = true;
			for (std::size_t group{}; group < present.size(); ++group)
				complete &= present[group] && scan_complete[group];
			auto output = project_rows({groups[0],
										groups[1],
										groups[2],
										groups[3],
										groups[4],
										groups[5],
										complete,
										present[0] && scan_complete[0],
										present[5] && scan_complete[5]},
									   limits,
									   stop,
									   retained);
			if (!output)
				return output.error();
			for (std::size_t group{}; group < present.size(); ++group)
				if (!present[group])
					output->unresolved.push_back({"sdk.abi-scan-missing",
												  std::string{relations[group]},
												  "independent-scan-unavailable"});
			normalize(output->unresolved);
			output->source_queries = input;
			return output;
		}
		catch (const failure& value)
		{
			return value.value;
		}
		catch (const std::bad_alloc&)
		{
			return error{"sdk.abi-resource-exhausted", "projection", "allocation"};
		}
		catch (const std::length_error&)
		{
			return error{"sdk.abi-resource-exhausted", "projection", "length"};
		}
	}
} // namespace cxxlens::sdk::query
