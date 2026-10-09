#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <tuple>
#include <type_traits>

#include <cxxlens/sdk/abi_surfaces.hpp>

#include "query_projected_row_encoding_internal.hpp"
#include "query_projection_plan_limits_internal.hpp"
#include "query_result_internal.hpp"

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
		constexpr std::array<std::string_view, 5> unconsumed_bitfield_columns{
			"field_bitfield_profile",
			"field_bitfield_state",
			"field_is_bitfield",
			"field_bit_width",
			"field_bitfield_reason"};
		using binding_key = std::tuple<std::size_t, std::string, world>;
		using surface_key = std::tuple<world, std::string, std::string, std::string, std::string>;
		struct evidence_owner
		{
			query_transfer_access::evidence_owner_view original;
			query_transfer_access::row_size_view sizes;
			bool validated{};
		};
		using owner_groups = std::array<std::vector<evidence_owner>, 6>;
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
			std::size_t bytes{}, peak{}, evidence{}, references{}, operations{}, extents{};
			std::size_t estimate(const annotated_row& row,
								 abi_surface_limits limits,
								 std::stop_token stop,
								 std::size_t* deferred_payload = nullptr)
			{
				std::size_t total = 2048;
				const auto add = [&](std::size_t n, std::size_t factor = 8U)
				{
					check(stop);
					if (deferred_payload)
						charge(
							*deferred_payload, n, std::numeric_limits<std::size_t>::max(), "row");
					else
						charge(operations, n, limits.maximum_operations, "operations");
					charge(operations, 1U, limits.maximum_operations, "operations");
					if (total > limits.maximum_retained_bytes ||
						n > (limits.maximum_retained_bytes - total) / factor)
						fail("sdk.abi-budget", "row", "limit-exceeded");
					total += n * factor;
				};
				const auto fixed = [&](std::size_t n)
				{
					check(stop);
					charge(operations, 1U, limits.maximum_operations, "operations");
					if (total > limits.maximum_retained_bytes ||
						n > limits.maximum_retained_bytes - total)
						fail("sdk.abi-budget", "row", "limit-exceeded");
					total += n;
				};
				for (const auto& [name, c] : row.values)
				{
					// One owned detached cell plus map-node/framing overhead. The
					// dynamic payload allowance below still covers encoded copies.
					fixed(sizeof(decltype(row.values)::value_type) + 256U);
					add(name.size() + c.type.parameter.size());
					if (c.unknown_reason)
						add(c.unknown_reason->size(), 16U);
					if (c.value)
					{
						if (const auto* v = std::get_if<std::string>(&*c.value))
							add(v->size(), 16U);
						if (const auto* v = std::get_if<std::vector<std::byte>>(&*c.value))
							add(v->size());
					}
				}
				const auto strings = [&](const auto& values)
				{
					for (const auto& v : values)
					{
						fixed(sizeof(v) + 128U);
						add(v.size());
					}
				};
				const auto producer = [&](const auto& p)
				{
					fixed(sizeof(p) + 128U);
					add(p.id.size() + p.semantic_contract.size());
				};
				const auto guarantee = [&](const auto& g)
				{
					fixed(sizeof(g) + 128U);
					add(g.approximation.size() + g.scope.size() + g.assumptions.size());
					strings(g.verification_modalities);
				};
				strings(row.claim_contributors);
				strings(row.provenance);
				strings(row.presence.fragments);
				add(row.interpretation.size() + row.presence.universe.size());
				for (const auto& p : row.producer_contracts)
					producer(p);
				for (const auto& g : row.contributor_guarantees)
					guarantee(g);
				for (const auto& e : row.contributor_edges)
				{
					fixed(sizeof(e) + 128U);
					add(e.claim_contributor.size() + e.provenance.size() + e.interpretation.size() +
						e.condition.universe.size());
					producer(e.producer);
					guarantee(e.guarantee);
					strings(e.condition.fragments);
				}
				return total;
			}
		};
		struct entry
		{
			std::size_t group{};
			const annotated_row* row{};
			std::string canonical;
			std::string prefix;
			const evidence_owner* owner{};
			std::size_t deferred_payload{};
			bool full_encoded{};
			bool borrowed_prefix{};
		};
		std::size_t prefix_size(const entry& value)
		{
			return value.borrowed_prefix ? value.row->claim_contributors.front().size() + 4U
										 : value.prefix.size();
		}
		unsigned char prefix_byte(const entry& value, std::size_t offset)
		{
			if (!value.borrowed_prefix)
				return static_cast<unsigned char>(value.prefix[offset]);
			// The complete canonical singleton array, including both string quotes
			// and its closing bracket. Only admitted printable ASCII without escapes
			// uses this view; arbitrary strings retain the authoritative row writer.
			if (offset < 2U)
				return static_cast<unsigned char>(std::string_view{"[\""}[offset]);
			const auto& claim = value.row->claim_contributors.front();
			if (offset - 2U < claim.size())
				return static_cast<unsigned char>(claim[offset - 2U]);
			return static_cast<unsigned char>(std::string_view{"\"]"}[offset - 2U - claim.size()]);
		}
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
		result<abi_surface_projection>
		project_rows(abi_surface_input input,
					 abi_surface_limits limits,
					 std::stop_token stop,
					 budget& retained,
					 const std::array<std::vector<const annotated_row*>, 6>* borrowed = nullptr,
					 const owner_groups* owners = nullptr)
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
				const auto peak = [&](std::size_t count)
				{
					if (retained.bytes > limits.maximum_retained_bytes ||
						count > limits.maximum_retained_bytes - retained.bytes)
						fail("sdk.abi-budget", "temporary-growth", "limit-exceeded");
					retained.peak = std::max(retained.peak, retained.bytes + count);
				};
				const auto grow = [&](auto& values)
				{
					if (values.size() != values.capacity())
						return;
					using value_type =
						typename std::remove_reference_t<decltype(values)>::value_type;
					const auto capacity = values.capacity();
					if (capacity > std::numeric_limits<std::size_t>::max() / 2U)
						fail("sdk.abi-budget", "temporary-growth", "limit-exceeded");
					const auto next = capacity ? 2U * capacity : 1U;
					if (next > limits.maximum_retained_bytes / sizeof(value_type))
						fail("sdk.abi-budget", "temporary-growth", "limit-exceeded");
					constexpr auto words =
						(sizeof(value_type) + sizeof(void*) - 1U) / sizeof(void*);
					if (values.size() > (limits.maximum_operations - 1U) / (2U * words))
						fail("sdk.abi-budget", "operations", "limit-exceeded");
					work(2U * words * values.size() + 1U);
					peak(next * sizeof(value_type));
					bytes((next - capacity) * sizeof(value_type), "temporary-growth");
					values.reserve(next);
				};
				const auto canonical_less = [&](std::string_view left, std::string_view right)
				{
					work();
					const auto common = std::min(left.size(), right.size());
					for (std::size_t i{}; i < common; ++i)
					{
						work(2U);
						const auto a = static_cast<unsigned char>(left[i]);
						const auto b = static_cast<unsigned char>(right[i]);
						if (a != b)
							return a < b;
					}
					return left.size() < right.size();
				};
				const auto prefix_less = [&](const entry& left, const entry& right)
				{
					work();
					const auto left_size = prefix_size(left), right_size = prefix_size(right);
					const auto common = std::min(left_size, right_size);
					for (std::size_t i{}; i < common; ++i)
					{
						work(2U);
						const auto a = prefix_byte(left, i), b = prefix_byte(right, i);
						if (a != b)
							return a < b;
					}
					return left_size < right_size;
				};
				const auto copy_text = [&](std::string_view value) -> std::string
				{
					bytes(value.size() + sizeof(std::string), "owned-text");
					return std::string{value};
				};
				bytes(sizeof(abi_surface_projection), "projection-framing");
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
					const auto count = borrowed ? (*borrowed)[group].size() : groups[group].size();
					for (std::size_t row_index{}; row_index < count; ++row_index)
					{
						const auto& row =
							borrowed ? *(*borrowed)[group][row_index] : groups[group][row_index];
						work();
						if (entries.size() >= limits.maximum_rows)
							fail("sdk.abi-budget", "rows", "limit-exceeded");
						const evidence_owner* original_owner{};
						std::optional<std::size_t> wire_base;
						if (owners)
							for (const auto& candidate : (*owners)[group])
							{
								work();
								const auto less = std::less<const annotated_row*>{};
								std::size_t first{}, last = candidate.original.rows.size();
								while (first < last)
								{
									work();
									const auto middle = first + (last - first) / 2U;
									if (less(&candidate.original.rows[middle], &row))
										first = middle + 1U;
									else
										last = middle;
								}
								work();
								if (first == candidate.original.rows.size() ||
									&candidate.original.rows[first] != &row)
									continue;
								original_owner = &candidate;
								wire_base = candidate.sizes.find(&row,
																 [&]
																 {
																	 work();
																 });
								break;
							}
						const bool validated = original_owner && original_owner->validated;
						// Omit the old joint payload charge only when generic validation,
						// detached copying and full row encoding are all physically absent.
						// A tied prefix will pay this deferred charge before its full encoder.
						// ABI-surface rows also encode every scalar into candidate payload
						// keys below. Their existing joint byte charge remains in full.
						const bool deferred = group != 5U && validated && wire_base &&
							!row.claim_contributors.empty();
						std::size_t deferred_payload{};
						bytes(retained.estimate(
								  row, limits, stop, deferred ? &deferred_payload : nullptr),
							  "owned-row-and-temporaries");
						if (!validated)
							if (auto valid = row.validate(); !valid)
								return valid.error();
						for (const auto& column : descriptor->columns)
						{
							work();
							// These additive original facts are not consumed by this
							// projection. Preserve older rows without fabricating cells.
							if (group == 4U && column.type.optional &&
								std::ranges::find(unconsumed_bitfield_columns, column.name) !=
									unconsumed_bitfield_columns.end() &&
								!row.values.contains("output." + column.name))
								continue;
							const auto& actual = cell(row, column.name);
							if (actual.type != column.type || (!validated && !actual.validate()))
								fail("sdk.abi-input-invalid",
									 column.id,
									 "column-type-or-value-invalid");
						}
						charge(expansions,
							   row.presence.fragments.size(),
							   limits.maximum_condition_expansions,
							   "condition-expansions");
						std::string canonical;
						std::size_t canonical_size{};
						if (deferred)
							canonical_size = detail::admitted_projected_row_size_from_base(
								*wire_base,
								row.multiplicity,
								[&]
								{
									work();
								},
								[&]
								{
									fail("sdk.abi-budget", "evidence-bytes", "limit-exceeded");
								});
						else
						{
							canonical = row.canonical_form();
							canonical_size = canonical.size();
						}
						charge(retained.evidence,
							   canonical_size,
							   limits.maximum_evidence_bytes,
							   "evidence-bytes");
						std::string prefix;
						bool borrowed_prefix{};
						if (owners)
						{
							work();
							if (row.claim_contributors.size() == 1U)
							{
								borrowed_prefix = true;
								const auto& claim = row.claim_contributors.front();
								for (const auto byte : claim)
								{
									work();
									const auto value = static_cast<unsigned char>(byte);
									if (value < 0x20U || value >= 0x7FU || value == '"' ||
										value == '\\')
									{
										borrowed_prefix = false;
										break;
									}
								}
								if (claim.size() > std::numeric_limits<std::size_t>::max() - 4U)
									fail("sdk.abi-budget", "canonical-prefix", "limit-exceeded");
							}
							if (!borrowed_prefix)
							{
								std::size_t prefix_visits{};
								detail::projected_row_size_sink encoded_prefix_size{
									[&]
									{
										work();
										charge(prefix_visits,
											   1U,
											   std::numeric_limits<std::size_t>::max(),
											   "canonical-prefix");
									},
									[&]
									{
										fail(
											"sdk.abi-budget", "canonical-prefix", "limit-exceeded");
									}};
								detail::emit_row_strings(encoded_prefix_size,
														 row.claim_contributors);
								if (encoded_prefix_size.size() >
										(limits.maximum_retained_bytes - retained.bytes) / 2U ||
									encoded_prefix_size.size() >
										(limits.maximum_operations - 1U) / 3U)
									fail("sdk.abi-budget", "canonical-prefix", "limit-exceeded");
								// Invalid UTF-8 encodes as empty quotes: retain the actual
								// measured scan bound even when its wire spelling is short.
								// Then prepay escaped temporary and final writes separately.
								work(std::max(encoded_prefix_size.size(), prefix_visits));
								work(3U * encoded_prefix_size.size() + 1U);
								bytes(2U * encoded_prefix_size.size(), "canonical-prefix");
								detail::projected_row_string_sink encoded;
								encoded.value.reserve(encoded_prefix_size.size());
								detail::emit_row_strings(encoded, row.claim_contributors);
								prefix = std::move(encoded.value);
							}
							grow(entries);
							work(2U * ((sizeof(entry) + sizeof(void*) - 1U) / sizeof(void*)) + 1U);
						}
						else
							// Encoding and detached-row storage were precharged before allocation.
							bytes(sizeof(entry) + sizeof(abi_surface_evidence), "evidence-rows");
						entries.push_back({group,
										   &row,
										   std::move(canonical),
										   std::move(prefix),
										   original_owner,
										   deferred_payload,
										   !deferred,
										   borrowed_prefix});
					}
				}
				std::ranges::sort(
					entries,
					[&](const auto& a, const auto& b)
					{
						work();
						if (!owners)
							return std::tie(a.group, a.canonical) < std::tie(b.group, b.canonical);
						return a.group != b.group ? a.group < b.group : prefix_less(a, b);
					});
				if (owners)
					for (std::size_t first{}; first < entries.size();)
					{
						work();
						std::size_t last = first + 1U;
						while (last < entries.size())
						{
							work();
							if (entries[first].group != entries[last].group ||
								prefix_size(entries[first]) != prefix_size(entries[last]) ||
								prefix_less(entries[first], entries[last]))
								break;
							++last;
						}
						if (last - first > 1U)
						{
							for (std::size_t i = first; i < last; ++i)
							{
								work();
								if (entries[i].full_encoded)
									continue;
								work(entries[i].deferred_payload);
								entries[i].canonical = entries[i].row->canonical_form();
								entries[i].full_encoded = true;
							}
							std::ranges::sort(std::span{entries}.subspan(first, last - first),
											  [&](const auto& a, const auto& b)
											  {
												  return canonical_less(a.canonical, b.canonical);
											  });
						}
						first = last;
					}
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
					if (owners)
						grow(output.evidence);
					std::optional<abi_surface_evidence> shared;
					if (e.owner)
					{
						bytes(relations[e.group].size() + sizeof(std::string), "owned-text");
						shared = query_transfer_access::share_evidence_row<abi_surface_evidence>(
							e.owner->original, e.row, relations[e.group], work);
						if (!shared)
							fail("sdk.abi-input-invalid",
								 "evidence-owner",
								 "original-owner-differs");
					}
					if (shared)
					{
						work(2U *
								 ((sizeof(abi_surface_evidence) + sizeof(void*) - 1U) /
								  sizeof(void*)) +
							 1U);
						output.evidence.push_back(std::move(*shared));
					}
					else
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
					std::set<std::string> abi_payloads, layout_payloads, target_payloads,
						packing_payloads;
					for (const auto ref : candidates)
					{
						work();
						std::array<std::string, 5> keys;
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
							const bool target = name == "output.target_data_model_state" ||
								name == "output.target_data_model_profile" ||
								name == "output.long_width_bits" ||
								name == "output.pointer_width_bits" ||
								name == "output.wchar_width_bits" ||
								name == "output.plain_char_signed" || name == "output.byte_order" ||
								name == "output.abi_context";
							const bool packing = name == "output.packing_state" ||
								name == "output.packing_profile" ||
								name == "output.packed_attribute" ||
								name == "output.packing_applied" ||
								name == "output.maximum_field_alignment_bits";
							for (std::size_t key{}; key < keys.size(); ++key)
								if (!key || (key == 1U && abi) || (key == 2U && layout) ||
									(key == 3U && target) || (key == 4U && packing))
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
						target_payloads.insert(std::move(keys[3]));
						packing_payloads.insert(std::move(keys[4]));
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
						const auto portability_state = [](std::string_view name)
						{
							return name == "conflicting" ? abi_surface_state::conflicting
														 : state(name);
						};
						value.target_data_model_state =
							portability_state(text(row, "target_data_model_state"));
						value.packing_state = portability_state(text(row, "packing_state"));
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
								if (group == 0U)
									downgrade(value.target_data_model_state, false);
								downgrade(value.packing_state, false);
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
								if (group == 0U)
									downgrade(value.target_data_model_state, true);
								downgrade(value.packing_state, true);
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
						{
							downgrade(value.target_data_model_state, true);
							downgrade(value.packing_state, true);
							gap("sdk.abi-identity-conflicting", value.id, true);
						}
						for (const auto ref : candidates)
							if (text(*entries[ref].row, "kind") != value.kind ||
								text(*entries[ref].row, "surface") != value.id)
							{
								downgrade(value.target_data_model_state, true);
								downgrade(value.packing_state, true);
								gap("sdk.abi-identity-conflicting", value.id, true);
							}
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
							{
								downgrade(value.packing_state, true);
								gap("sdk.abi-declaration-conflicting", value.entity, true);
							}
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
						{
							downgrade(value.packing_state, true);
							gap("sdk.abi-declaration-conflicting", value.entity, true);
						}
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
						const auto portability_text =
							[&](std::string_view field, std::optional<std::string>& destination)
						{
							if (const auto* actual = optional<std::string>(row, field))
								destination = copy_text(*actual);
						};
						const auto portability_number =
							[&](std::string_view field, std::optional<std::uint64_t>& destination)
						{
							if (const auto* actual = optional<std::uint64_t>(row, field))
								destination = *actual;
						};
						const auto portability_boolean =
							[&](std::string_view field, std::optional<bool>& destination)
						{
							if (const auto* actual = optional<bool>(row, field))
								destination = *actual;
						};
						portability_text("target_data_model_profile",
										 value.target_data_model_profile);
						portability_text("byte_order", value.byte_order);
						portability_number("long_width_bits", value.long_width_bits);
						portability_number("pointer_width_bits", value.pointer_width_bits);
						portability_number("wchar_width_bits", value.wchar_width_bits);
						portability_boolean("plain_char_signed", value.plain_char_signed);
						portability_text("packing_profile", value.packing_profile);
						portability_boolean("packed_attribute", value.packed_attribute);
						portability_boolean("packing_applied", value.packing_applied);
						portability_number("maximum_field_alignment_bits",
										   value.maximum_field_alignment_bits);
						const auto facet_gap = [&](abi_surface_state& facet,
												   std::string code,
												   std::string detail,
												   bool conflict = false)
						{
							downgrade(facet, conflict);
							gap(std::move(code), std::move(detail), conflict, false, false);
						};
						if (target_payloads.size() > 1U)
							facet_gap(value.target_data_model_state,
									  "sdk.abi-target-data-model-conflicting",
									  "retain-every-conditioned-candidate",
									  true);
						if (packing_payloads.size() > 1U)
							facet_gap(value.packing_state,
									  "sdk.abi-packing-conflicting",
									  "retain-every-conditioned-candidate",
									  true);
						if (value.target_data_model_state == abi_surface_state::complete)
						{
							if (!value.target_data_model_profile ||
								*value.target_data_model_profile !=
									"clang22-original-target-data-model/1")
							{
								value.target_data_model_state = abi_surface_state::unknown;
								facet_gap(value.target_data_model_state,
										  "sdk.abi-target-data-model-profile-unsupported",
										  "profile");
							}
							else if (!value.abi_context || !value.long_width_bits ||
									 !value.pointer_width_bits || !value.wchar_width_bits ||
									 !value.plain_char_signed || !value.byte_order)
							{
								facet_gap(value.target_data_model_state,
										  "sdk.abi-target-data-model-missing",
										  "complete-facet-requires-original-target-payload");
							}
							else if (!*value.long_width_bits || !*value.pointer_width_bits ||
									 !*value.wchar_width_bits)
							{
								facet_gap(value.target_data_model_state,
										  "sdk.abi-target-data-model-conflicting",
										  "zero-width",
										  true);
							}
							else if (*value.byte_order != "little" && *value.byte_order != "big")
								facet_gap(value.target_data_model_state,
										  "sdk.abi-target-byte-order-unsupported",
										  *value.byte_order);
						}
						if (value.packing_state == abi_surface_state::complete)
						{
							if (!value.packing_profile ||
								*value.packing_profile !=
									"clang22-original-record-packing-attributes/1")
							{
								value.packing_state = abi_surface_state::unknown;
								facet_gap(value.packing_state,
										  "sdk.abi-packing-profile-unsupported",
										  "profile");
							}
							else if (!declaration)
								facet_gap(value.packing_state,
										  "sdk.abi-packing-declaration-unavailable",
										  value.entity);
							else if (value.kind != "record" || !definition)
								facet_gap(value.packing_state,
										  "sdk.abi-packing-applicability-conflicting",
										  "original-complete-record-definition-required",
										  true);
							else if (!value.packed_attribute || !value.packing_applied ||
									 !value.maximum_field_alignment_bits)
								facet_gap(value.packing_state,
										  "sdk.abi-packing-missing",
										  "complete-facet-requires-original-attributes");
							else if (*value.packing_applied !=
									 (*value.packed_attribute ||
									  *value.maximum_field_alignment_bits != 0U))
								facet_gap(value.packing_state,
										  "sdk.abi-packing-conflicting",
										  "attribute-membership-disagrees",
										  true);
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
				{
					work();
					bytes(sizeof(query_unresolved) + 256U, "observation-gap");
					output.unresolved.push_back({"sdk.abi-observations-partial",
												 "projection",
												 "local-interface-does-not-close-project"});
				}
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
		if (evidence_ownership != projection_evidence_ownership::detached &&
			evidence_ownership != projection_evidence_ownership::shared_immutable)
			return error{"sdk.abi-limit-invalid", "evidence_ownership", "unsupported"};
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
		projection_resource_usage usage;
		return project_abi_surfaces(input, limits, stop, usage);
	}
	result<abi_surface_projection> project_abi_surfaces(abi_surface_input input,
														abi_surface_limits limits,
														std::stop_token stop,
														projection_resource_usage& usage)
	{
		usage = {};
		budget retained;
		auto output = project_rows(input, limits, stop, retained);
		if (output)
			usage = {retained.operations, std::max(retained.bytes, retained.peak)};
		return output;
	}
	result<abi_surface_projection> project_abi_surfaces(const application_query_results& input,
														abi_surface_limits limits,
														std::stop_token stop)
	{
		projection_resource_usage usage;
		return project_abi_surfaces(input, limits, stop, usage);
	}
	result<abi_surface_projection> project_abi_surfaces(const application_query_results& input,
														abi_surface_limits limits,
														std::stop_token stop,
														projection_resource_usage& usage)
	{
		usage = {};
		if (auto valid = limits.validate(); !valid)
			return valid.error();
		try
		{
			check(stop);
			budget retained;
			std::size_t plan_bytes{};
			const std::function<bool()> cancelled = [&]
			{
				check(stop);
				charge(retained.operations, 1U, limits.maximum_operations, "operations");
				return false;
			};
			if (auto valid = detail::check_source_plan_limits(input,
															  limits.maximum_source_queries,
															  limits.maximum_source_plan_bytes,
															  stop,
															  "sdk.abi",
															  &plan_bytes,
															  cancelled);
				!valid)
				return valid.error();
			charge(retained.bytes, plan_bytes, limits.maximum_retained_bytes, "source-plans");
			std::array<std::vector<const annotated_row*>, 6> groups;
			std::optional<owner_groups> evidence_owners;
			if (limits.evidence_ownership == projection_evidence_ownership::shared_immutable)
			{
				charge(retained.bytes,
					   sizeof(owner_groups),
					   limits.maximum_retained_bytes,
					   "owner-views");
				evidence_owners.emplace();
			}
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
				if (evidence_owners)
				{
					const auto work = [&](std::size_t count = 1U)
					{
						check(stop);
						charge(retained.operations, count, limits.maximum_operations, "operations");
					};
					work();
					charge(retained.bytes,
						   query_transfer_access::row_size_metadata_bytes(
							   scan.result,
							   [&]
							   {
								   fail("sdk.abi-budget", "owner-size-facts", "limit-exceeded");
							   }),
						   limits.maximum_retained_bytes,
						   "owner-size-facts");
					auto& owners = (*evidence_owners)[group];
					if (owners.size() == owners.capacity())
					{
						const auto capacity = owners.capacity();
						if (capacity > std::numeric_limits<std::size_t>::max() / 2U)
							fail("sdk.abi-budget", "owner-views", "limit-exceeded");
						const auto next = capacity ? 2U * capacity : 1U;
						constexpr auto words =
							(sizeof(evidence_owner) + sizeof(void*) - 1U) / sizeof(void*);
						if (next > limits.maximum_retained_bytes / sizeof(evidence_owner) ||
							owners.size() > (limits.maximum_operations - 1U) / (2U * words))
							fail("sdk.abi-budget", "owner-views", "limit-exceeded");
						work(2U * words * owners.size() + 1U);
						const auto added = next * sizeof(evidence_owner);
						if (retained.bytes > limits.maximum_retained_bytes ||
							added > limits.maximum_retained_bytes - retained.bytes)
							fail("sdk.abi-budget", "owner-views", "limit-exceeded");
						retained.peak = std::max(retained.peak, retained.bytes + added);
						charge(retained.bytes,
							   (next - capacity) * sizeof(evidence_owner),
							   limits.maximum_retained_bytes,
							   "owner-views");
						owners.reserve(next);
					}
					work(2U * ((sizeof(evidence_owner) + sizeof(void*) - 1U) / sizeof(void*)) + 1U);
					owners.push_back({query_transfer_access::borrow_evidence_owner(scan.result),
									  query_transfer_access::borrow_row_sizes(scan.result),
									  query_transfer_access::rows_validated(scan.result)});
				}
				present[group] = true;
				scan_complete[group] &= scan.result.execution() == execution_status::complete &&
					scan.result.inputs_complete() && scan.result.conflicts().empty() &&
					scan.result.differential_disagreements().empty();
				const auto original_rows = query_transfer_access::borrow_rows(scan.result);
				charge(rows, original_rows.size(), limits.maximum_rows, "rows");
				if (original_rows.size() > limits.maximum_rows ||
					groups[group].size() > limits.maximum_rows - original_rows.size())
					fail("sdk.abi-budget", "rows", "limit-exceeded");
				const auto pointer_count = groups[group].size() + original_rows.size();
				if (pointer_count > limits.maximum_retained_bytes / sizeof(const annotated_row*))
					fail("sdk.abi-budget", "detached-scan-index", "limit-exceeded");
				charge(retained.bytes,
					   pointer_count * sizeof(const annotated_row*),
					   limits.maximum_retained_bytes,
					   "detached-scan-index");
				groups[group].reserve(pointer_count);
				for (const auto& row : original_rows)
				{
					check(stop);
					charge(retained.operations, 1U, limits.maximum_operations, "operations");
					groups[group].push_back(&row);
				}
			}
			bool complete = true;
			for (std::size_t group{}; group < present.size(); ++group)
				complete &= present[group] && scan_complete[group];
			auto output = project_rows({{},
										{},
										{},
										{},
										{},
										{},
										complete,
										present[0] && scan_complete[0],
										present[5] && scan_complete[5]},
									   limits,
									   stop,
									   retained,
									   &groups,
									   evidence_owners ? &*evidence_owners : nullptr);
			if (!output)
				return output.error();
			for (std::size_t group{}; group < present.size(); ++group)
			{
				check(stop);
				charge(retained.operations, 1U, limits.maximum_operations, "operations");
				if (!present[group])
				{
					charge(retained.bytes,
						   sizeof(query_unresolved) * 2U + relations[group].size() + 128U,
						   limits.maximum_retained_bytes,
						   "missing-scan-gap");
					output->unresolved.push_back({"sdk.abi-scan-missing",
												  std::string{relations[group]},
												  "independent-scan-unavailable"});
				}
			}
			normalize(output->unresolved);
			output->source_queries = input;
			usage = {retained.operations, std::max(retained.bytes, retained.peak)};
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
