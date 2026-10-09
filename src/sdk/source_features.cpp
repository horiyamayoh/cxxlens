#include <algorithm>
#include <array>
#include <bit>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <type_traits>

#include <cxxlens/sdk/source_features.hpp>

#include "query_projected_row_encoding_internal.hpp"
#include "query_projection_plan_limits_internal.hpp"
#include "query_projection_row_copy_internal.hpp"
#include "query_projection_rows_internal.hpp"
#include "query_projection_span_lookup_internal.hpp"
#include "query_result_internal.hpp"
namespace cxxlens::sdk::query
{
	namespace
	{
		using refs = std::vector<std::size_t>;
		using rows = std::vector<const annotated_row*>;
		using groups = std::array<rows, 10>;
		constexpr std::array<std::string_view, 10> relations{"build.compile_unit.v1",
															 "source.file.v1",
															 "source.span.v1",
															 "cc.entity.v1",
															 "cc.declaration.v1",
															 "cc.declaration_inventory.v1",
															 "cc.type.v1",
															 "cc.syntax_node.v1",
															 "cc.source_feature.v1",
															 "cc.source_feature_inventory.v1"};
		constexpr std::array<std::string_view, 10> identifiers{"compile_unit",
															   "snapshot",
															   "span",
															   "entity",
															   "declaration",
															   "inventory",
															   "type",
															   "node",
															   "feature",
															   "inventory"};
		constexpr std::string_view feature_profile = "clang22-original-static-source-features/1",
								   environment_profile = "clang22-original-language-environment/1";
		using state = finite_population_state;

		struct failure
		{
			error value;
		};
		[[noreturn]] void fail(std::string_view field,
							   std::string_view reason,
							   std::string_view code = "sdk.source-feature-input-invalid")
		{
			throw failure{{std::string{code}, std::string{field}, std::string{reason}}};
		}
		const detached_cell* cell(const annotated_row& row, std::string_view name)
		{
			const auto at = row.values.find("output." + std::string{name});
			return at == row.values.end() ? nullptr : &at->second;
		}
		bool present(const annotated_row& row, std::string_view name)
		{
			const auto* c = cell(row, name);
			return c && c->state == cell_state::present && c->value;
		}
		std::string_view text(const annotated_row& row, std::string_view name)
		{
			const auto* c = cell(row, name);
			const auto* v =
				c && present(row, name) ? std::get_if<std::string>(&*c->value) : nullptr;
			return v ? std::string_view{*v} : std::string_view{};
		}
		const std::vector<std::byte>* bytes(const annotated_row& row, std::string_view name)
		{
			const auto* c = cell(row, name);
			return c && present(row, name) ? std::get_if<std::vector<std::byte>>(&*c->value)
										   : nullptr;
		}
		std::optional<std::uint64_t> number(const annotated_row& row, std::string_view name)
		{
			const auto* c = cell(row, name);
			const auto* v =
				c && present(row, name) ? std::get_if<std::uint64_t>(&*c->value) : nullptr;
			return v ? std::optional{*v} : std::nullopt;
		}
		std::optional<bool> boolean(const annotated_row& row, std::string_view name)
		{
			const auto* c = cell(row, name);
			const auto* v = c && present(row, name) ? std::get_if<bool>(&*c->value) : nullptr;
			return v ? std::optional{*v} : std::nullopt;
		}
		using identity = std::array<std::string, 4>;
		using view_identity = std::array<std::string_view, 4>;
		struct budget;
		int compare_identity_text(budget& meter, std::string_view left, std::string_view right);
		struct identity_less
		{
			using is_transparent = void;
			budget* meter{};
			template <class L, class R>
			bool operator()(const L& l, const R& r) const
			{
				for (std::size_t i = 0; i < 4U; ++i)
				{
					if (meter)
					{
						const auto order = compare_identity_text(*meter, l[i], r[i]);
						if (order != 0)
							return order < 0;
						continue;
					}
					if (l[i] < r[i])
						return true;
					if (r[i] < l[i])
						return false;
				}
				return false;
			}
		};
		finite_population_state combine(finite_population_state a, finite_population_state b)
		{
			if (a == finite_population_state::conflicting ||
				b == finite_population_state::conflicting)
				return finite_population_state::conflicting;
			if (a == finite_population_state::complete && b == finite_population_state::complete)
				return finite_population_state::complete;
			if (a == finite_population_state::unknown && b == finite_population_state::unknown)
				return finite_population_state::unknown;
			return finite_population_state::partial;
		}

		struct budget
		{
			finite_population_limits limits;
			std::stop_token stop;
			std::size_t retained{}, temporary_peak{}, evidence{}, references{}, operations{},
				conditions{}, members{}, rows{}, populations{};
			void charge(std::size_t& used,
						std::size_t amount,
						std::size_t maximum,
						std::string_view field)
			{
				if (used > maximum || amount > maximum - used)
					fail(field, "limit-exceeded", "sdk.source-feature-budget");
				used += amount;
			}
			void work(std::size_t n = 1)
			{
				if (stop.stop_requested() || (limits.cancelled && limits.cancelled()))
					fail("projection", "stop-requested", "sdk.source-feature-cancelled");
				charge(operations, n, limits.maximum_operations, "operations");
			}
			bool canonical_less(std::string_view left, std::string_view right)
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
			}
			void retain(std::size_t n)
			{
				charge(retained, n, limits.maximum_retained_bytes, "retained-bytes");
			}
			void bind(refs& into, const refs& from)
			{
				work(from.size());
				charge(references,
					   from.size(),
					   limits.maximum_evidence_references,
					   "evidence-references");
				retain(from.size() * sizeof(std::size_t));
				into.insert(into.end(), from.begin(), from.end());
			}
			std::size_t estimate(const annotated_row& row,
								 bool encoding = true,
								 std::size_t* validation_payload = nullptr)
			{
				std::size_t total = 2048;
				const auto add = [&](std::size_t n, std::size_t factor = 8U)
				{
					if (!encoding)
						factor = 2U;
					// Geometry reads lengths; encoding and owned copies charge their bytes.
					work();
					if (total > limits.maximum_retained_bytes ||
						n > (limits.maximum_retained_bytes - total) / factor)
						fail("row", "limit-exceeded", "sdk.source-feature-budget");
					total += n * factor;
					if (validation_payload)
					{
						if (n > limits.maximum_operations - *validation_payload)
							fail("operations", "limit-exceeded", "sdk.source-feature-budget");
						*validation_payload += n;
					}
				};
				const auto fixed = [&](std::size_t n)
				{
					work();
					if (total > limits.maximum_retained_bytes ||
						n > limits.maximum_retained_bytes - total)
						fail("row", "limit-exceeded", "sdk.source-feature-budget");
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
		int compare_identity_text(budget& meter, std::string_view left, std::string_view right)
		{
			meter.work();
			const auto common = std::min(left.size(), right.size());
			for (std::size_t i{}; i < common; ++i)
			{
				meter.work(2U);
				const auto a = static_cast<unsigned char>(left[i]);
				const auto b = static_cast<unsigned char>(right[i]);
				if (a != b)
					return a < b ? -1 : 1;
			}
			return left.size() < right.size() ? -1 : (left.size() > right.size() ? 1 : 0);
		}

		struct projector
		{
			budget& b;
			groups& input;
			source_feature_projection output;
			std::array<std::map<view_identity, rows, identity_less>, 10> index;
			std::map<const annotated_row*, std::size_t> owned;
			bool row_validation_reused{};
			std::string copy(std::string_view value)
			{
				b.work(value.size() + 1U);
				b.retain(2U * value.size() + sizeof(std::string));
				return std::string{value};
			}
			void lookup_work(const view_identity& key, std::size_t size)
			{
				const auto factor = static_cast<std::size_t>(std::bit_width(size)) + 1U;
				for (auto value : key)
				{
					if (value.size() + 1U > b.limits.maximum_operations / factor)
						fail("operations", "limit-exceeded", "sdk.source-feature-budget");
					b.work((value.size() + 1U) * factor);
				}
			}
			const rows& find(std::size_t group, std::string_view id, const view_identity& world)
			{
				static const rows empty;
				if (id.empty())
					return empty;
				view_identity key{id, world[1], world[2], world[3]};
				b.work();
				if (group == 1U)
					return file_index.find(key);
				if (group == 2U)
					return span_index.find(key);
				if (group == 3U)
					return entity_index.find(key);
				if (group == 4U)
					return declaration_index.find(key);
				if (group == 6U)
					return type_index.find(key);
				if (group == 7U)
					return syntax_index.find(key);
				const auto at = index[group].find(key);
				return at == index[group].end() ? empty : at->second;
			}
			void peak(std::size_t amount)
			{
				if (amount > b.limits.maximum_retained_bytes - b.retained)
					fail("temporary", "limit-exceeded", "sdk.source-feature-budget");
				b.temporary_peak = std::max(b.temporary_peak, b.retained + amount);
			}
			std::string canonical(const annotated_row& row)
			{
				peak(2U * b.estimate(row));
				auto value = row.canonical_form();
				b.work(value.size() + 1U);
				return value;
			}
			void initialize()
			{
				for (auto& group : index)
					group = std::map<view_identity, rows, identity_less>{identity_less{&b}};
				const auto descriptors = standard_relation_descriptors();
				for (std::size_t group = 0; group < input.size(); ++group)
				{
					const auto d =
						std::ranges::find(descriptors, relations[group], &relation_descriptor::id);
					if (d == descriptors.end())
						fail(relations[group], "descriptor-missing");
					for (const auto* row : input[group])
					{
						b.work();
						b.charge(b.rows, 1U, b.limits.maximum_rows, "rows");
						b.charge(b.conditions,
								 row->presence.fragments.size(),
								 b.limits.maximum_condition_expansions,
								 "conditions");
						// Inspect every original field/container, including rows whose optional
						// facet is absent. Length inspection does not traverse payload bytes.
						std::size_t validation_payload{};
						(void)b.estimate(
							*row, false, row_validation_reused ? nullptr : &validation_payload);
						// Generic row validation traverses values and annotated text.
						b.work(validation_payload);
						if (auto valid = detail::validate_projected_relation_row(
								*row,
								*d,
								"sdk.source-feature-input-invalid",
								[&]
								{
									b.work();
								},
								row_validation_reused);
							!valid)
							throw failure{valid.error()};
						const auto id = text(*row, identifiers[group]);
						if (id.empty())
							fail(relations[group], "identity-missing");
						for (const auto& variant : row->presence.fragments)
						{
							view_identity key{
								id, row->presence.universe, variant, row->interpretation};
							b.work();
							// Conservative capacity/node allowance before both map and vector
							// growth.
							if (group == 1U)
								file_index.add(key, row);
							else if (group == 2U)
								span_index.add(key, row);
							else if (group == 3U)
								entity_index.add(key, row);
							else if (group == 4U)
								declaration_index.add(key, row);
							else if (group == 6U)
								type_index.add(key, row);
							else if (group == 7U)
								syntax_index.add(key, row);
							else
							{
								b.retain(sizeof(view_identity) + 256U +
										 2U * sizeof(const annotated_row*));
								index[group][key].push_back(row);
							}
						}
					}
				}
				std::size_t retained_keys{};
				{
					// Only populations with alternatives need ordering keys. One borrowed
					// original can belong to several worlds; encode it once across them.
					const auto pointer_less =
						[&](const annotated_row* left, const annotated_row* right)
					{
						b.work();
						return std::less<const annotated_row*>{}(left, right);
					};
					std::map<const annotated_row*, std::string, decltype(pointer_less)> keys{
						pointer_less};
					const auto order_alternatives = [&](rows& alternatives)
					{
						b.work();
						if (alternatives.size() < 2U)
							return;
						for (const auto* original : alternatives)
						{
							b.work();
							if (keys.find(original) != keys.end())
								continue;
							constexpr auto node = sizeof(decltype(keys)::value_type) + 256U;
							b.retain(node);
							auto encoded = canonical(*original);
							const auto buffer = encoded.capacity() + 1U;
							b.retain(buffer);
							retained_keys += node + buffer;
							keys.emplace(original, std::move(encoded));
						}
						std::ranges::sort(alternatives,
										  [&](const auto* left, const auto* right)
										  {
											  return b.canonical_less(keys.at(left),
																	  keys.at(right));
										  });
					};
					for (auto& group : index)
						for (auto& [key, alternatives] : group)
						{
							(void)key;
							order_alternatives(alternatives);
						}
					file_index.visit(order_alternatives);
					span_index.visit(order_alternatives);
					entity_index.visit(order_alternatives);
					declaration_index.visit(order_alternatives);
					type_index.visit(order_alternatives);
					syntax_index.visit(order_alternatives);
				}
				// The map and string buffers have expired before their reservation is
				// refunded; the observed scratch peak remains part of returned usage.
				b.retained -= retained_keys;
			}
			annotated_row clone_row(const annotated_row& row)
			{
				return detail::copy_projected_row(row,
												  [&](std::size_t amount)
												  {
													  b.work(amount);
												  });
			}
			void
			bind(refs& evidence, std::size_t group, std::span<const annotated_row* const> originals)
			{
				for (const auto* r : originals)
				{
					b.work(static_cast<std::size_t>(std::bit_width(owned.size())) + 1U);
					auto at = owned.find(r);
					if (at == owned.end())
					{
						b.retain(b.estimate(*r, false) + 2U * sizeof(finite_population_evidence) +
								 2U * relations[group].size() + 128U);
						// Only the exact wire size is needed for the evidence-byte cap.
						// Ordering alternatives above still retains complete canonical strings.
						peak(2U * b.estimate(*r));
						const auto encoded_size = detail::admitted_projected_row_size(
							*r,
							[&]
							{
								b.work();
							},
							[&]
							{
								fail("evidence-bytes",
									 "limit-exceeded",
									 "sdk.source-feature-budget");
							});
						b.charge(b.evidence,
								 encoded_size,
								 b.limits.maximum_evidence_bytes,
								 "evidence-bytes");
						const auto ref = output.evidence.size();
						b.work(relations[group].size() + 1U);
						output.evidence.push_back({std::string{relations[group]}, clone_row(*r)});
						b.retain(128U + sizeof(std::pair<const annotated_row* const, std::size_t>));
						at = owned.emplace(r, ref).first;
					}
					b.charge(b.references,
							 1U,
							 b.limits.maximum_evidence_references,
							 "evidence-references");
					b.retain(2U * sizeof(std::size_t));
					evidence.push_back(at->second);
				}
			}
			bool agree(const rows& originals, std::initializer_list<std::string_view> fields)
			{
				// Unobserved optional columns are not a contrary observation. Comparing
				// only actual present observations also keeps unrelated flags out of this
				// axis.
				for (auto name : fields)
				{
					const detached_cell* first = nullptr;
					for (const auto* r : originals)
					{
						b.work();
						const auto* value = cell(*r, name);
						if (!present(*r, name))
							continue;
						if (!first || first == value)
						{
							first = value;
							continue;
						}
						b.work(name.size() + value->type.parameter.size() + 1U);
						if (value->value)
							std::visit(
								[&](const auto& v)
								{
									if constexpr (requires { v.size(); })
										b.work(v.size());
								},
								*value->value);
						if (first->type != value->type || first->value != value->value)
							return false;
					}
				}
				return true;
			}
			const annotated_row& representative(const rows& original,
												std::initializer_list<std::string_view> fields)
			{
				const annotated_row* best = original.front();
				std::size_t most{};
				for (const auto* r : original)
				{
					std::size_t count{};
					for (auto field : fields)
					{
						b.work();
						count += present(*r, field) ? 1U : 0U;
					}
					if (count > most)
					{
						most = count;
						best = r;
					}
				}
				return *best;
			}
			template <class T>
			void gap(T& into, std::string_view subject, std::string_view reason)
			{
				b.work();
				b.retain(2U * (sizeof(query_unresolved) + subject.size() + reason.size() + 64U));
				into.gaps.push_back(
					{"sdk.source-feature-" + std::string{reason}, std::string{subject}, {}});
			}
			state symbol_state(std::string_view value)
			{
				return value == "complete"	 ? state::complete
					: value == "conflicting" ? state::conflicting
					: value == "partial"	 ? state::partial
											 : state::unknown;
			}
			bool all_agree(std::size_t group, const rows& originals)
			{
				const auto descriptors = standard_relation_descriptors();
				const auto d =
					std::ranges::find(descriptors, relations[group], &relation_descriptor::id);
				for (const auto& field : d->columns)
				{
					const detached_cell* previous = nullptr;
					for (const auto* r : originals)
					{
						b.work();
						if (!present(*r, field.name))
							continue;
						const auto* value = cell(*r, field.name);
						if (!previous || previous == value)
						{
							previous = value;
							continue;
						}
						b.work(field.name.size() + value->type.parameter.size() + 1U);
						if (value->value)
							std::visit(
								[&](const auto& v)
								{
									if constexpr (requires { v.size(); })
										b.work(v.size());
								},
								*value->value);
						if (previous->type != value->type || previous->value != value->value)
							return false;
						previous = value;
					}
				}
				return true;
			}
			state unit(std::string_view id, const view_identity& world, refs& evidence)
			{
				const auto& original = find(0U, id, world);
				if (original.empty())
					return state::unknown;
				bind(evidence, 0U, original);
				return agree(original, {"compile_unit", "main_source", "toolchain", "variant"})
					? state::complete
					: state::conflicting;
			}
			state target(std::size_t group,
						 std::string_view id,
						 const view_identity& world,
						 refs& evidence,
						 std::initializer_list<std::string_view> fields)
			{
				const auto& original = find(group, id, world);
				if (original.empty())
					return state::unknown;
				bind(evidence, group, original);
				return agree(original, fields) ? state::complete : state::conflicting;
			}
			state source(std::string_view id,
						 std::string_view file,
						 std::string_view snapshot,
						 const view_identity& world,
						 refs& evidence)
			{
				const auto& spans = find(2U, id, world);
				if (spans.empty())
					return state::unknown;
				bind(evidence, 2U, spans);
				if (!agree(spans, {"span", "snapshot", "file", "begin", "end"}))
					return state::conflicting;
				const auto& original = *spans.front();
				if (text(original, "file") != file || text(original, "snapshot") != snapshot)
					return state::conflicting;
				const auto& files = find(1U, snapshot, world);
				if (files.empty())
					return state::partial;
				bind(evidence, 1U, files);
				if (!agree(files, {"snapshot", "file", "size"}) ||
					text(*files.front(), "file") != file)
					return state::conflicting;
				const auto begin = number(original, "begin"), end = number(original, "end"),
						   size = number(*files.front(), "size");
				return begin && end && size && *begin <= *end && *end <= *size ? state::complete
																			   : state::conflicting;
			}
			state file_snapshot(std::string_view file,
								std::string_view snapshot,
								const view_identity& world,
								refs& evidence)
			{
				const auto& originals = find(1U, snapshot, world);
				if (originals.empty())
					return state::unknown;
				bind(evidence, 1U, originals);
				return agree(originals, {"file", "snapshot", "size"}) &&
						text(*originals.front(), "file") == file
					? state::complete
					: state::conflicting;
			}
			static bool supported_source_none(const observed_source_feature& value)
			{
				if (!value.file.empty() || !value.source_snapshot.empty() ||
					!value.source_span.empty())
					return false;
				if (value.source_none_reason == "clang22-invalid-range-implicit-origin/1")
					return value.origin == "implicit" && value.is_implicit == true &&
						(value.feature_class == "declaration" ||
						 value.feature_class == "attribute");
				if (value.source_none_reason == "clang22-invalid-range-implicit-type-source-info/1")
					return value.feature_class == "type_location" && value.origin == "implicit" &&
						value.is_implicit != false;
				if (value.source_none_reason ==
					"clang22-invalid-range-default-activation-wrapper/1")
					return value.feature_class == "statement" && value.origin == "activated" &&
						(value.kind == "CXXDefaultArgExpr" || value.kind == "CXXDefaultInitExpr");
				return false;
			}
			struct member_less
			{
				budget* meter;
				bool operator()(std::string_view left, std::string_view right) const
				{
					return compare_identity_text(*meter, left, right) < 0;
				}
			};
			struct member_index
			{
				budget& meter;
				std::size_t retained{};
				std::set<std::string_view, member_less> values;
				explicit member_index(budget& owner) : meter(owner), values(member_less{&owner}) {}
				~member_index()
				{
					// The ID views borrow the original immutable cell. Only set nodes are
					// owned here, and all nodes expire before their reservation is refunded.
					values.clear();
					meter.retained -= retained;
				}
				bool insert(std::string_view id)
				{
					constexpr auto node = 128U + sizeof(std::string_view);
					meter.retain(node);
					retained += node;
					meter.temporary_peak = std::max(meter.temporary_peak, meter.retained);
					return values.insert(id).second;
				}
				bool contains(std::string_view id) const
				{
					meter.work();
					return values.find(id) != values.end();
				}
			};
			struct decoded_set
			{
				bool observed{}, duplicate{};
				std::size_t count{};
			};
			struct inventory_pointer_less
			{
				budget* meter;
				bool operator()(const annotated_row* left, const annotated_row* right) const
				{
					meter->work();
					return std::less<const annotated_row*>{}(left, right);
				}
			};
			struct cached_inventory
			{
				member_index members;
				decoded_set decoded;
				explicit cached_inventory(budget& meter) : members(meter) {}
			};
			struct inventory_cache
			{
				using index =
					std::map<const annotated_row*, cached_inventory, inventory_pointer_less>;
				budget& meter;
				std::size_t retained{};
				std::array<index, 2U> values;
				explicit inventory_cache(budget& owner)
					: meter(owner), values{index{inventory_pointer_less{&owner}},
										   index{inventory_pointer_less{&owner}}}
				{
				}
				~inventory_cache()
				{
					clear();
				}
				void clear()
				{
					// Nested borrowed-ID set nodes and map nodes expire before their
					// separate reservations are refunded, including every failure exit.
					for (auto& group : values)
						group.clear();
					meter.retained -= retained;
					retained = 0U;
				}
			};
			inventory_cache declaration_members{b};
			template <class Callback>
			decoded_set members(const annotated_row& r,
								std::string_view field,
								Callback emit,
								member_index* retained_members = nullptr)
			{
				const auto* values = bytes(r, field);
				if (!values)
					return {};
				decoded_set result{true, false, 0U};
				std::size_t position{};
				member_index local_members{b};
				auto& seen = retained_members ? *retained_members : local_members;
				while (position < values->size())
				{
					b.work();
					if (values->size() - position < 4U)
						fail(field, "truncated-set");
					std::uint32_t length{};
					for (unsigned shift{}; shift < 32U; shift += 8U)
						length |= std::to_integer<std::uint32_t>((*values)[position++]) << shift;
					if (!length || length > values->size() - position)
						fail(field, "invalid-set-member");
					b.charge(b.members, 1U, b.limits.maximum_members, "set-members");
					const std::string_view id{
						reinterpret_cast<const char*>(values->data() + position), length};
					result.duplicate |= !seen.insert(id);
					emit(id);
					++result.count;
					position += length;
				}
				return result;
			}
			const cached_inventory& declared_members(const annotated_row& row, bool physical)
			{
				b.work();
				auto& cache_group = declaration_members.values[physical ? 1U : 0U];
				const auto found = cache_group.find(&row);
				if (found != cache_group.end())
					return found->second;
				constexpr auto node = sizeof(inventory_cache::index::value_type) + 256U;
				b.retain(node);
				declaration_members.retained += node;
				b.temporary_peak = std::max(b.temporary_peak, b.retained);
				auto& cached = cache_group.try_emplace(&row, b).first->second;
				cached.decoded = members(
					row,
					physical ? "physical_definition_ids" : "declarations",
					[](std::string_view)
					{
					},
					&cached.members);
				return cached;
			}
			state declaration(std::string_view id,
							  std::string_view compile_unit,
							  const view_identity& world,
							  refs& evidence)
			{
				const auto& originals = find(4U, id, world);
				if (originals.empty())
					return state::unknown;
				bind(evidence, 4U, originals);
				if (!agree(originals, {"declaration", "entity", "source", "kind", "is_implicit"}))
					return state::conflicting;
				bool admitted{}, contradiction{};
				for (const auto& [key, inventories] : index[5U])
				{
					b.work();
					if (key[1] != world[1] || key[2] != world[2] || key[3] != world[3])
						continue;
					for (const auto* inventory : inventories)
					{
						b.work();
						if (text(*inventory, "compile_unit") != compile_unit)
							continue;
						for (const bool physical : {false, true})
						{
							const auto profile = text(
								*inventory, physical ? "physical_definition_profile" : "profile");
							if (profile !=
								(physical ? "clang22-original-physical-definitions/1"
										  : "clang22-explicit-admitted-named-declarations/1"))
								continue;
							const auto& cached = declared_members(*inventory, physical);
							const auto& decoded = cached.decoded;
							if (!cached.members.contains(id))
								continue;
							bind(evidence, 5U, rows{inventory});
							const auto count = number(*inventory,
													  physical ? "physical_definition_count"
															   : "declaration_count");
							const auto native_state =
								text(*inventory,
									 physical ? "physical_definition_state" : "enumeration_state");
							if (decoded.duplicate || !count || *count < decoded.count ||
								(native_state == "complete" && *count != decoded.count) ||
								native_state == "conflicting")
								contradiction = true;
							else if (native_state == "complete" || native_state == "partial")
								admitted = true;
						}
					}
				}
				if (contradiction)
					return state::conflicting;
				return admitted ? state::complete : state::partial;
			}
			void environment(const view_identity& world, const rows& originals)
			{
				b.charge(b.populations, 1U, b.limits.maximum_populations, "observations");
				b.retain(2U * sizeof(observed_language_environment));
				const auto& r = representative(
					originals, {"freestanding", "freestanding_profile", "freestanding_state"});
				observed_language_environment value;
				value.compile_unit = copy(world[0]);
				value.universe = copy(world[1]);
				value.variant = copy(world[2]);
				value.interpretation = copy(world[3]);
				value.profile = copy(text(r, "freestanding_profile"));
				value.observation_state = copy(text(r, "freestanding_state"));
				value.freestanding = boolean(r, "freestanding");
				bind(value.evidence, 0U, originals);
				if (!agree(originals,
						   {"freestanding", "freestanding_profile", "freestanding_state"}))
					value.state = state::conflicting;
				else if (value.profile == environment_profile && value.freestanding)
					value.state = symbol_state(value.observation_state);
				if (value.state != state::complete)
					gap(value, value.compile_unit, "language-environment-unavailable");
				output.environments.push_back(std::move(value));
			}
			std::map<view_identity, std::size_t, identity_less> feature_index{identity_less{&b}};
			detail::projection_span_lookup<budget> file_index{b}, span_index{b};
			detail::projection_span_lookup<budget> entity_index{b}, declaration_index{b},
				type_index{b}, syntax_index{b};
			void feature(const view_identity& world, const rows& originals)
			{
				b.charge(b.members, 1U, b.limits.maximum_members, "features");
				b.retain(2U * sizeof(observed_source_feature) + 256U + sizeof(view_identity));
				const auto& r = *originals.front();
				observed_source_feature value;
				value.feature = copy(world[0]);
				value.compile_unit = copy(text(r, "compile_unit"));
				value.universe = copy(world[1]);
				value.variant = copy(world[2]);
				value.interpretation = copy(world[3]);
				value.profile = copy(text(r, "profile"));
				value.ordinal = *number(r, "ordinal");
				value.original_node_ordinal = *number(r, "original_node_ordinal");
				value.compiler_kind = *number(r, "compiler_kind");
				value.feature_class = copy(text(r, "feature_class"));
				value.kind = copy(text(r, "kind"));
				value.origin = copy(text(r, "origin"));
				value.evaluation = copy(text(r, "evaluation"));
				value.observation_state = copy(text(r, "state"));
				value.file = copy(text(r, "file"));
				value.source_snapshot = copy(text(r, "source_snapshot"));
				value.source_span = copy(text(r, "source"));
				value.source_none_reason = copy(text(r, "source_none_reason"));
				value.declaration = copy(text(r, "declaration"));
				value.context_declaration = copy(text(r, "context_declaration"));
				value.subject_entity = copy(text(r, "subject_entity"));
				value.call_target = copy(text(r, "call_target"));
				value.subject_type = copy(text(r, "subject_type"));
				value.syntax = copy(text(r, "syntax"));
				value.reference_kind = copy(text(r, "reference_kind"));
				value.type_role = copy(text(r, "type_role"));
				value.is_implicit = boolean(r, "is_implicit");
				value.is_system = boolean(r, "is_system");
				value.source_binding_state = copy(text(r, "source_binding_state"));
				value.declaration_binding_state = copy(text(r, "declaration_binding_state"));
				value.context_binding_state = copy(text(r, "context_binding_state"));
				value.entity_binding_state = copy(text(r, "entity_binding_state"));
				value.call_binding_state = copy(text(r, "call_binding_state"));
				value.type_binding_state = copy(text(r, "type_binding_state"));
				bind(value.evidence, 8U, originals);
				value.identity_state = unit(value.compile_unit, world, value.evidence);
				if (!agree(
						originals,
						{"feature", "compile_unit", "ordinal", "profile", "original_node_ordinal"}))
					value.identity_state = state::conflicting;
				const bool supported = value.profile == feature_profile;
				const bool known_class = value.feature_class == "declaration" ||
					value.feature_class == "statement" || value.feature_class == "attribute" ||
					value.feature_class == "type_location";
				value.observation = supported && known_class && !value.kind.empty()
					? symbol_state(value.observation_state)
					: state::unknown;
				if (!all_agree(8U, originals))
					value.observation = state::conflicting;
				if (supported)
				{
					if (value.source_binding_state == "complete")
						value.source_state = source(value.source_span,
													value.file,
													value.source_snapshot,
													world,
													value.evidence);
					else if (value.source_binding_state == "none")
						value.source_state =
							supported_source_none(value) ? state::complete : state::conflicting;
					if (value.declaration_binding_state == "complete")
						value.declaration_state = declaration(
							value.declaration, value.compile_unit, world, value.evidence);
					if (value.context_binding_state == "complete")
						value.context_state = declaration(
							value.context_declaration, value.compile_unit, world, value.evidence);
					if (value.entity_binding_state == "complete")
						value.entity_state = target(3U,
													value.subject_entity,
													world,
													value.evidence,
													{"entity", "kind", "provider_local_key"});
					if (value.call_binding_state == "complete")
					{
						value.call_state = target(3U,
												  value.call_target,
												  world,
												  value.evidence,
												  {"entity", "kind", "provider_local_key"});
						const auto& targets = find(3U, value.call_target, world);
						if (!targets.empty())
						{
							const auto kind = text(*targets.front(), "kind");
							if (kind != "function" && kind != "method" && kind != "constructor" &&
								kind != "destructor" && kind != "conversion")
								value.call_state = combine(value.call_state, state::unknown);
						}
					}
					if (value.type_binding_state == "complete")
						value.type_state = target(6U,
												  value.subject_type,
												  world,
												  value.evidence,
												  {"type",
												   "constructor",
												   "dependent",
												   "structure_profile",
												   "structure_state"});
					if (!value.syntax.empty())
					{
						const auto& syntax = find(7U, value.syntax, world);
						value.syntax_state = target(7U,
													value.syntax,
													world,
													value.evidence,
													{"node", "compile_unit", "kind", "source"});
						if (!syntax.empty() &&
							text(*syntax.front(), "compile_unit") != value.compile_unit)
							value.syntax_state = state::conflicting;
					}
				}
				if (value.observation != state::complete)
					gap(value, value.feature, "observation-unavailable");
				feature_index.emplace(world, output.features.size());
				output.features.push_back(std::move(value));
			}
			void population(const view_identity& world, const rows& originals, bool scans_complete)
			{
				b.charge(b.populations, 1U, b.limits.maximum_populations, "populations");
				b.retain(2U * sizeof(source_feature_population));
				const auto& r = *originals.front();
				source_feature_population value;
				value.inventory = copy(world[0]);
				value.compile_unit = copy(text(r, "compile_unit"));
				value.scope = copy(text(r, "scope"));
				value.file = copy(text(r, "file"));
				value.source_snapshot = copy(text(r, "source_snapshot"));
				value.profile = copy(text(r, "profile"));
				value.universe = copy(world[1]);
				value.variant = copy(world[2]);
				value.interpretation = copy(world[3]);
				value.feature_count = *number(r, "feature_count");
				value.unbound_feature_count = *number(r, "unbound_feature_count");
				value.entered_file_count = number(r, "entered_file_count");
				value.enumeration_observation = copy(text(r, "enumeration_state"));
				value.traversal_observation = copy(text(r, "traversal_state"));
				value.entry_observation = copy(text(r, "entry_state"));
				value.source_binding_observation = copy(text(r, "source_binding_state"));
				value.entered_file_observation = copy(text(r, "entered_file_state"));
				bind(value.evidence, 9U, originals);
				value.identity_state = unit(value.compile_unit, world, value.evidence);
				if (!agree(originals,
						   {"inventory",
							"compile_unit",
							"scope",
							"file",
							"source_snapshot",
							"profile"}))
					value.identity_state = state::conflicting;
				const bool tu = value.scope == "translation_unit",
						   file = value.scope == "entered_file";
				if ((tu && (!value.file.empty() || !value.source_snapshot.empty())) ||
					(file && (value.file.empty() || value.source_snapshot.empty())))
					value.identity_state = state::conflicting;
				const auto pair_state = file
					? file_snapshot(value.file, value.source_snapshot, world, value.evidence)
					: state::complete;
				value.identity_state = combine(value.identity_state, pair_state);
				state membership = state::complete, source_membership = state::complete;
				member_index feature_members{b}, unbound_members{b};
				const auto members_set = members(
					r,
					"feature_ids",
					[&](std::string_view id)
					{
						value.feature_ids.push_back(copy(id));
						const view_identity key{id, world[1], world[2], world[3]};
						const auto found = feature_index.find(key);
						if (found == feature_index.end())
						{
							membership = combine(membership, state::unknown);
							return;
						}
						const auto& feature = output.features[found->second];
						if (feature.compile_unit != value.compile_unit ||
							feature.profile != value.profile ||
							(file &&
							 (feature.file != value.file ||
							  feature.source_snapshot != value.source_snapshot ||
							  feature.source_binding_state != "complete")))
							membership = state::conflicting;
						membership = combine(membership, feature.observation);
						membership = combine(membership, feature.identity_state);
						source_membership = combine(source_membership, feature.source_state);
						b.retain(2U * sizeof(std::size_t));
						value.features.push_back(found->second);
					},
					&feature_members);
				const auto unbound_set = members(
					r,
					"unbound_feature_ids",
					[&](std::string_view id)
					{
						value.unbound_feature_ids.push_back(copy(id));
						if (!feature_members.contains(id))
							membership = state::conflicting;
						const view_identity key{id, world[1], world[2], world[3]};
						if (const auto found = feature_index.find(key);
							found != feature_index.end())
							if (output.features[found->second].source_binding_state == "none" ||
								output.features[found->second].source_binding_state == "complete")
								membership = state::conflicting;
					},
					&unbound_members);
				const auto files_set = members(r,
											   "entered_file_ids",
											   [&](std::string_view id)
											   {
												   value.entered_file_ids.push_back(copy(id));
											   });
				const auto snapshots_set =
					members(r,
							"entered_source_snapshots",
							[&](std::string_view id)
							{
								value.entered_source_snapshots.push_back(copy(id));
							});
				// A complete native inventory must account for every observed member
				// in that exact unit/world/profile, not only the IDs it happens to list.
				for (const auto& feature : output.features)
				{
					b.work();
					if (feature.compile_unit != value.compile_unit ||
						feature.universe != value.universe || feature.variant != value.variant ||
						feature.interpretation != value.interpretation ||
						feature.profile != value.profile ||
						(file &&
						 (feature.file != value.file ||
						  feature.source_snapshot != value.source_snapshot)))
						continue;
					if (!feature_members.contains(feature.feature))
						membership = state::conflicting;
					if (tu && feature.source_binding_state != "none" &&
						feature.source_binding_state != "complete")
					{
						if (!unbound_members.contains(feature.feature))
							membership = state::conflicting;
					}
				}
				if (value.profile == feature_profile && (tu || file))
				{
					value.enumeration_state = symbol_state(value.enumeration_observation);
					value.traversal_state = symbol_state(value.traversal_observation);
					value.entry_state = symbol_state(value.entry_observation);
					value.source_state = symbol_state(value.source_binding_observation);
					value.source_state = combine(value.source_state, source_membership);
					value.entry_state = combine(value.entry_state, pair_state);
					value.source_state = combine(value.source_state, pair_state);
					value.entered_file_state =
						tu ? symbol_state(value.entered_file_observation) : state::unknown;
					value.membership_state =
						members_set.observed && unbound_set.observed ? membership : state::unknown;
					const bool bad_count = members_set.duplicate || unbound_set.duplicate ||
						value.feature_count < members_set.count ||
						value.unbound_feature_count != unbound_set.count ||
						(value.enumeration_state == state::complete &&
						 value.feature_count != members_set.count);
					if (bad_count)
						value.enumeration_state = value.membership_state = state::conflicting;
					if (file && unbound_set.count)
						value.source_state = value.membership_state = state::conflicting;
					if (value.source_state == state::complete && unbound_set.count)
						value.source_state = state::conflicting;
					if (tu)
					{
						if (!files_set.observed || !snapshots_set.observed ||
							!value.entered_file_count)
							value.entered_file_state = state::unknown;
						else if (files_set.duplicate || snapshots_set.duplicate ||
								 *value.entered_file_count < files_set.count ||
								 files_set.count != snapshots_set.count ||
								 (value.entered_file_state == state::complete &&
								  *value.entered_file_count != files_set.count))
							value.entered_file_state = state::conflicting;
						for (const auto& id : value.entered_source_snapshots)
						{
							const auto& originals_files = find(1U, id, world);
							bind(value.evidence, 1U, originals_files);
							if (originals_files.empty())
								value.entered_file_state =
									combine(value.entered_file_state, state::unknown);
							else if (!agree(originals_files, {"file", "snapshot", "size"}) ||
									 std::ranges::find(value.entered_file_ids,
													   text(*originals_files.front(), "file")) ==
										 value.entered_file_ids.end())
								value.entered_file_state = state::conflicting;
						}
						for (const auto& file_id : value.entered_file_ids)
						{
							std::size_t matches{};
							for (const auto& snapshot_id : value.entered_source_snapshots)
							{
								b.work(file_id.size());
								const auto& original_files = find(1U, snapshot_id, world);
								if (!original_files.empty() &&
									text(*original_files.front(), "file") == file_id)
									++matches;
							}
							if (matches > 1U ||
								(matches != 1U && value.entered_file_state == state::complete))
								value.entered_file_state = state::conflicting;
						}
					}
					if (!scans_complete)
					{
						value.enumeration_state = combine(value.enumeration_state, state::unknown);
						value.membership_state = combine(value.membership_state, state::unknown);
					}
					if (!all_agree(9U, originals))
						value.enumeration_state = value.membership_state = state::conflicting;
				}
				if (value.enumeration_state != state::complete ||
					value.membership_state != state::complete)
					gap(value, value.inventory, "inventory-membership-unavailable");
				output.populations.push_back(std::move(value));
			}
			source_feature_projection run(bool scans_complete)
			{
				initialize();
				for (const auto& [world, originals] : index[0U])
					environment(world, originals);
				for (const auto& [world, originals] : index[8U])
					feature(world, originals);
				// Declaration/context attribution is finished. Retire its per-call
				// original-field decode cache before the later population phase.
				declaration_members.clear();
				using occurrence_key = std::tuple<std::string_view,
												  std::string_view,
												  std::string_view,
												  std::string_view,
												  std::string_view,
												  std::uint64_t>;
				// This index only detects exact occurrence duplicates; it never orders
				// observations. Ordinals distinguish most keys before visiting text.
				const auto occurrence_less =
					[&](const occurrence_key& left, const occurrence_key& right)
				{
					b.work();
					if (std::get<5U>(left) != std::get<5U>(right))
						return std::get<5U>(left) < std::get<5U>(right);
					if (const auto order =
							compare_identity_text(b, std::get<0U>(left), std::get<0U>(right)))
						return order < 0;
					if (const auto order =
							compare_identity_text(b, std::get<1U>(left), std::get<1U>(right)))
						return order < 0;
					if (const auto order =
							compare_identity_text(b, std::get<2U>(left), std::get<2U>(right)))
						return order < 0;
					if (const auto order =
							compare_identity_text(b, std::get<3U>(left), std::get<3U>(right)))
						return order < 0;
					return compare_identity_text(b, std::get<4U>(left), std::get<4U>(right)) < 0;
				};
				std::map<occurrence_key, std::size_t, decltype(occurrence_less)> occurrences{
					occurrence_less};
				for (std::size_t at{}; at < output.features.size(); ++at)
				{
					auto& value = output.features[at];
					b.work();
					const occurrence_key key{value.compile_unit,
											 value.profile,
											 value.universe,
											 value.variant,
											 value.interpretation,
											 value.ordinal};
					b.retain(sizeof(occurrence_key) + sizeof(std::size_t) + 128U);
					const auto [found, inserted] = occurrences.emplace(key, at);
					if (!inserted)
						value.identity_state = output.features[found->second].identity_state =
							state::conflicting;
				}
				for (const auto& [world, originals] : index[9U])
					population(world, originals, scans_complete);
				for (auto& population : output.populations)
				{
					if (population.scope != "entered_file" || population.profile != feature_profile)
						continue;
					bool admitted{}, found{};
					for (const auto& tu : output.populations)
					{
						b.work();
						if (tu.scope != "translation_unit" ||
							tu.compile_unit != population.compile_unit ||
							tu.universe != population.universe ||
							tu.variant != population.variant ||
							tu.interpretation != population.interpretation ||
							tu.profile != feature_profile)
							continue;
						found = true;
						b.work(tu.entered_file_ids.size() + tu.entered_source_snapshots.size());
						const bool has_file =
							std::ranges::find(tu.entered_file_ids, population.file) !=
							tu.entered_file_ids.end();
						const bool has_snapshot = std::ranges::find(tu.entered_source_snapshots,
																	population.source_snapshot) !=
							tu.entered_source_snapshots.end();
						if (has_file && has_snapshot && tu.entered_file_state == state::complete)
							admitted = true;
						else if (tu.entered_file_state == state::complete)
							population.entry_state = state::conflicting;
					}
					if (population.entry_state != state::conflicting && !admitted)
						population.entry_state = found ? state::partial : state::unknown;
				}
				return std::move(output);
			}
		};

		result<source_feature_projection> project(source_feature_input input,
												  const application_query_results* queries,
												  finite_population_limits limits,
												  std::stop_token stop,
												  projection_resource_usage* usage)
		{
			if (usage)
				*usage = {};
			if (auto valid = limits.validate(); !valid)
				return valid.error();
			try
			{
				budget b{limits, stop};
				b.work();
				groups borrowed;
				bool row_validation_reused = queries != nullptr;
				std::array<bool, 10> available{}, seen{};
				available.fill(true);
				if (queries)
				{
					std::size_t plan_bytes{};
					if (auto valid =
							detail::check_source_plan_limits(*queries,
															 limits.maximum_source_queries,
															 limits.maximum_source_plan_bytes,
															 stop,
															 "sdk.source-feature",
															 &plan_bytes,
															 [&]
															 {
																 b.work();
																 return false;
															 });
						!valid)
						return valid.error();
					b.retain(plan_bytes);
					for (const auto& scan : queries->scans)
					{
						b.work();
						const auto at = std::ranges::find(relations, scan.relation_id);
						if (at == relations.end())
							continue;
						const auto group = static_cast<std::size_t>(at - relations.begin());
						seen[group] = true;
						row_validation_reused &= query_transfer_access::rows_validated(scan.result);
						available[group] &= scan.result.execution() == execution_status::complete &&
							scan.result.conflicts().empty() &&
							scan.result.differential_disagreements().empty();
						const auto originals = query_transfer_access::borrow_rows(scan.result);
						b.retain(1024U +
								 2U * (borrowed[group].size() + originals.size()) *
									 sizeof(const annotated_row*));
						if (originals.size() > limits.maximum_rows - b.rows)
							fail("rows", "limit-exceeded", "sdk.source-feature-budget");
						b.rows += originals.size();
						borrowed[group].reserve(borrowed[group].size() + originals.size());
						for (const auto& r : originals)
						{
							b.work();
							borrowed[group].push_back(&r);
						}
					}
					b.rows = 0;
					const auto complete = [&](std::size_t group)
					{
						return seen[group] && available[group];
					};
					input.compile_units_complete = complete(0U);
					input.feature_inputs_complete = complete(8U);
					input.inventory_inputs_complete = complete(9U);
				}
				else
				{
					const std::array spans{input.units,
										   input.files,
										   input.spans,
										   input.entities,
										   input.declarations,
										   input.declaration_inventories,
										   input.types,
										   input.syntax_nodes,
										   input.features,
										   input.inventories};
					for (std::size_t group = 0; group < spans.size(); ++group)
					{
						b.retain(2U * spans[group].size() * sizeof(const annotated_row*));
						if (spans[group].size() > limits.maximum_rows - b.rows)
							fail("rows", "limit-exceeded", "sdk.source-feature-budget");
						b.rows += spans[group].size();
						borrowed[group].reserve(spans[group].size());
						for (const auto& r : spans[group])
						{
							b.work();
							borrowed[group].push_back(&r);
						}
					}
					b.rows = 0;
				}
				projector work{
					b,
					borrowed,
					{},
					{},
					{},
					row_validation_reused,
					projector::inventory_cache{b},
					std::map<view_identity, std::size_t, identity_less>{identity_less{&b}}};
				auto output =
					work.run(input.feature_inputs_complete && input.inventory_inputs_complete);
				output.compile_units_complete = input.compile_units_complete;
				output.feature_inputs_complete = input.feature_inputs_complete;
				output.inventory_inputs_complete = input.inventory_inputs_complete;
				if (queries)
				{
					for (std::size_t group = 0; group < seen.size(); ++group)
						if (!seen[group])
						{
							b.retain(2U *
									 (sizeof(query_unresolved) + relations[group].size() + 128U));
							output.unresolved.push_back({"sdk.source-feature-scan-missing",
														 std::string{relations[group]},
														 "independent-scan-unavailable"});
						}
					b.retain(sizeof(application_query_results) + 2U * queries->snapshot_id.size() +
							 2U * queries->scans.size() * sizeof(application_relation_scan));
					output.source_queries = *queries;
				}
				if (usage)
					*usage = {b.operations, std::max(b.retained, b.temporary_peak)};
				return output;
			}
			catch (const failure& e)
			{
				return e.value;
			}
			catch (const std::bad_alloc&)
			{
				return error{"sdk.source-feature-resource-exhausted", "projection", "allocation"};
			}
			catch (const std::length_error&)
			{
				return error{"sdk.source-feature-resource-exhausted", "projection", "length"};
			}
		}
	} // namespace
	result<source_feature_projection> project_source_features(source_feature_input input,
															  finite_population_limits limits,
															  std::stop_token stop)
	{
		return project(input, nullptr, limits, stop, nullptr);
	}
	result<source_feature_projection>
	project_source_features(const application_query_results& input,
							finite_population_limits limits,
							std::stop_token stop)
	{
		return project({}, &input, limits, stop, nullptr);
	}
	result<source_feature_projection> project_source_features(source_feature_input input,
															  finite_population_limits limits,
															  std::stop_token stop,
															  projection_resource_usage& usage)
	{
		return project(input, nullptr, limits, stop, &usage);
	}
	result<source_feature_projection>
	project_source_features(const application_query_results& input,
							finite_population_limits limits,
							std::stop_token stop,
							projection_resource_usage& usage)
	{
		return project({}, &input, limits, stop, &usage);
	}
} // namespace cxxlens::sdk::query
