#include "reducer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "../core/canonical_encoding.hpp"
#include "../core/canonical_json.hpp"
#include "../runtime/hash_port.hpp"

namespace cxxlens::detail::facts
{
	namespace
	{
		[[nodiscard]] error reducer_error(std::string reason)
		{
			error failure;
			failure.code.value = "extractor.invalid-observation";
			failure.message = "Fact reduction invariant failed";
			failure.scope = failure_scope::workspace;
			failure.attributes.emplace("reason", std::move(reason));
			return failure;
		}

		[[nodiscard]] std::string kind_name(const fact_kind kind)
		{
			constexpr std::array<std::string_view, 22U> names{"file",
															  "compile_command",
															  "symbol",
															  "type",
															  "declaration",
															  "definition",
															  "reference",
															  "call",
															  "construction",
															  "conversion",
															  "inheritance",
															  "override_relation",
															  "include_relation",
															  "macro_definition",
															  "macro_expansion",
															  "cfg_summary",
															  "flow_summary",
															  "effect_summary",
															  "dynamic_observation",
															  "coverage_region",
															  "custom",
															  "invalid"};
			const auto index = static_cast<std::size_t>(kind);
			return std::string{index < names.size() - 1U ? names.at(index) : names.back()};
		}

		template <class T>
		void sort_unique(std::vector<T>& values)
		{
			std::ranges::sort(values,
							  {},
							  [](const T& value)
							  {
								  return value.value();
							  });
			const auto duplicate = std::ranges::unique(values);
			values.erase(duplicate.begin(), duplicate.end());
		}

		[[nodiscard]] std::string frame(const std::string_view value)
		{
			return std::to_string(value.size()) + ":" + std::string{value};
		}

		[[nodiscard]] std::string name_key(const std::optional<name_identity>& value)
		{
			if (!value)
				return "none";
			return frame(value->display_qualified_name) + frame(value->usr.value_or("")) +
				frame(value->semantic_owner.value_or("")) +
				frame(value->declaration_kind.value_or("")) +
				frame(value->signature_structure.value_or(""));
		}

		[[nodiscard]] std::string type_key(const std::optional<type_identity>& value)
		{
			if (!value)
				return "none";
			std::string output = frame(value->display_spelling) +
				frame(value->canonical_structure) +
				frame(value->declaration ? std::string{value->declaration->value()} : "") +
				(value->builtin ? "1" : "0");
			for (const auto& component : value->components)
				output += frame(component.value());
			return output;
		}

		[[nodiscard]] bool operational_key(const std::string_view key)
		{
			return key == "semantic_key" || key == "variant" || key.starts_with("extractor.") ||
				key.starts_with("cache.") || key.starts_with("run.") || key.starts_with("arrival.");
		}

		[[nodiscard]] bool absolute_path_value(const std::string_view value)
		{
			return value.starts_with('/') || value.starts_with('\\') ||
				(value.size() > 2U && std::isalpha(static_cast<unsigned char>(value[0])) != 0 &&
				 value[1] == ':' && (value[2] == '/' || value[2] == '\\'));
		}

		[[nodiscard]] std::string domain_payload(const observation_record& observation)
		{
			std::string output =
				"name=" + name_key(observation.name) + "|type=" + type_key(observation.type);
			if (observation.kind == fact_kind::definition)
				output += "|definition-source=" +
					frame(observation.source ? observation.source->to_canonical_json() : "");
			for (const auto& [key, value] : observation.payload)
			{
				if (operational_key(key))
					continue;
				output += "|" + frame(key) + frame(value);
			}
			return output;
		}

		[[nodiscard]] std::string group_key(const observation_record& observation)
		{
			const auto prefix = kind_name(observation.kind) + ":";
			const auto field = [&](const std::string_view key) -> std::optional<std::string>
			{
				if (const auto found = observation.payload.find(std::string{key});
					found != observation.payload.end())
					return found->second;
				return std::nullopt;
			};
			switch (observation.kind)
			{
				case fact_kind::symbol:
					if (auto value = field("symbol.id"))
						return prefix + *value;
					break;
				case fact_kind::type:
					if (auto value = field("type.id"))
						return prefix + *value;
					break;
				case fact_kind::definition:
					if (auto value = field("symbol.id"))
						return prefix + *value;
					break;
				case fact_kind::inheritance:
					if (auto derived = field("inheritance.derived"))
						return prefix + *derived + ":" +
							field("inheritance.base")
								.value_or(field("inheritance.base_type").value_or("unresolved"));
					break;
				case fact_kind::override_relation:
					if (auto overriding = field("override.overriding"))
						return prefix + *overriding + ":" +
							field("override.overridden").value_or("unresolved");
					break;
				default:
					break;
			}
			return prefix + observation.payload.at("semantic_key");
		}

		[[nodiscard]] std::string source_key(const std::optional<source_span>& source)
		{
			return source ? source->to_canonical_json() : std::string{};
		}

		struct observation_ref
		{
			const observation_record* observation{};
			const frontend::observation_batch* batch{};
		};

		[[nodiscard]] std::string observation_unit(const observation_ref& value)
		{
			return std::string{value.observation->compile_unit.value()} + ":" +
				std::string{value.observation->variant.value()} + ":" +
				group_key(*value.observation) + ":" + frame(domain_payload(*value.observation));
		}

		[[nodiscard]] coverage_state observation_state(const observation_record& observation)
		{
			coverage_state output = coverage_state::covered;
			for (const auto& contribution : observation.coverage_contributions)
			{
				if (contribution.state == coverage_state::failed)
					return coverage_state::failed;
				if (contribution.state == coverage_state::unresolved)
					output = coverage_state::unresolved;
				else if (contribution.state == coverage_state::excluded &&
						 output == coverage_state::covered)
					output = coverage_state::excluded;
				else if (contribution.state == coverage_state::not_applicable &&
						 output == coverage_state::covered)
					output = coverage_state::not_applicable;
			}
			return output;
		}

		[[nodiscard]] result<std::string>
		make_stable_id(const std::string& prefix, // NOLINT(bugprone-easily-swappable-parameters)
					   const std::string& domain,
					   const std::vector<std::pair<std::string, std::string>>& fields,
					   identity::collision_registry& collisions,
					   const runtime::hash_port& hashes)
		{
			identity::canonical_encoder encoder{domain, {1U, 1U}};
			for (const auto& [name, value] : fields)
				encoder.string_field(name, value);
			auto payload = encoder.finish();
			if (!payload)
				return reducer_error("identity-encoding-failed");
			identity::identity_service identities{hashes};
			auto generated = identities.make_id(prefix, payload.value(), collisions);
			if (!generated)
				return reducer_error("identity-hash-or-collision-failed");
			return generated.value();
		}

		[[nodiscard]] std::vector<compile_unit_id>
		contributors(const std::vector<observation_ref>& rows)
		{
			std::vector<compile_unit_id> output;
			output.reserve(rows.size());
			for (const auto& row : rows)
				output.push_back(row.observation->compile_unit);
			sort_unique(output);
			return output;
		}

		[[nodiscard]] std::vector<build_variant_id>
		variants(const std::vector<observation_ref>& rows)
		{
			std::vector<build_variant_id> output;
			output.reserve(rows.size());
			for (const auto& row : rows)
				output.push_back(row.observation->variant);
			sort_unique(output);
			return output;
		}

		[[nodiscard]] std::string variant_signature(const std::vector<build_variant_id>& values)
		{
			std::string output;
			for (const auto& value : values)
				output += frame(value.value());
			return output;
		}

		[[nodiscard]] result<detached_fact_record>
		make_fact(const std::string& key, // NOLINT(bugprone-easily-swappable-parameters)
				  const std::string& payload_key,
				  const std::vector<observation_ref>& rows,
				  const bool variant_split,
				  identity::collision_registry& collisions,
				  const runtime::hash_port& hashes)
		{
			const auto& representative = *rows.front().observation;
			auto units = contributors(rows);
			auto build_variants = variants(rows);
			const auto stable_key =
				key + (variant_split ? "|variant=" + variant_signature(build_variants) : "");
			auto generated = make_stable_id(
				"fact",
				"cxxlens.fact-id.v1",
				{{"kind", kind_name(representative.kind)},
				 {"owner", stable_key},
				 {"payload", payload_key},
				 {"variant", variant_split ? variant_signature(build_variants) : "shared"}},
				collisions,
				hashes);
			if (!generated)
				return std::move(generated.error());
			detached_fact_record output;
			output.id = fact_id{generated.value()};
			output.kind = representative.kind;
			output.stable_key = stable_key;
			output.origin.compile_units = std::move(units);
			output.origin.variants = std::move(build_variants);
			output.origin.extractor_id = representative.adapter_id;
			output.origin.extractor_version = representative.adapter_version;
			output.payload_version = representative.payload_version;
			for (const auto& [name, value] : representative.payload)
				if (!operational_key(name))
					output.payload.emplace(name, value);
			output.name = representative.name;
			output.type = representative.type;
			std::map<std::string, source_span> sources;
			for (const auto& row : rows)
				if (row.observation->source)
					sources.emplace(source_key(row.observation->source), *row.observation->source);
			if (!sources.empty())
				output.source = sources.begin()->second;
			if (auto checked = validate(output); !checked)
				return std::move(checked.error());
			return output;
		}

		[[nodiscard]] bool definition_source_conflict(const fact_kind kind,
													  const std::vector<observation_ref>& rows)
		{
			if (kind != fact_kind::definition)
				return false;
			std::map<std::string, std::set<std::string>> sources;
			for (const auto& row : rows)
				sources[std::string{row.observation->variant.value()}].insert(
					source_key(row.observation->source));
			return std::ranges::any_of(sources,
									   [](const auto& value)
									   {
										   return value.second.size() > 1U;
									   });
		}

		[[nodiscard]] bool
		same_variant_conflict(const std::map<std::string, std::vector<observation_ref>>& partitions)
		{
			std::map<std::string, std::size_t> owners;
			for (const auto& [unused, rows] : partitions)
			{
				(void)unused;
				std::set<std::string> local;
				for (const auto& row : rows)
					local.insert(std::string{row.observation->variant.value()});
				for (const auto& variant : local)
					if (++owners[variant] > 1U)
						return true;
			}
			return false;
		}

		[[nodiscard]] std::string decision_name(const reduction_decision decision)
		{
			switch (decision)
			{
				case reduction_decision::merged:
					return "merged";
				case reduction_decision::variant_split:
					return "variant_split";
				case reduction_decision::conflict:
					return "conflict";
			}
			return "conflict";
		}

		[[nodiscard]] bool fact_less(const detached_fact_record& left,
									 const detached_fact_record& right)
		{
			return std::tuple{left.kind, left.stable_key, left.id.value()} <
				std::tuple{right.kind, right.stable_key, right.id.value()};
		}

		[[nodiscard]] bool trace_less(const reduction_trace_row& left,
									  const reduction_trace_row& right)
		{
			return left.group_key < right.group_key;
		}

		[[nodiscard]] bool conflict_less(const reduction_conflict& left,
										 const reduction_conflict& right)
		{
			return std::tuple{left.group_key, left.code, left.id} <
				std::tuple{right.group_key, right.code, right.id};
		}
	} // namespace

	result<reduction_result> reduce_observations(std::vector<frontend::observation_batch> batches)
	{
		std::ranges::sort(batches,
						  {},
						  [](const frontend::observation_batch& batch)
						  {
							  return std::string{batch.unit.value()} + ":" +
								  std::string{batch.variant.value()};
						  });
		std::map<std::string, std::vector<observation_ref>> groups;
		reduction_result output;
		std::set<std::string> coverage_units;
		std::map<std::string, std::pair<coverage_state, std::optional<std::string>>>
			observation_coverage;
		for (const auto& batch : batches)
		{
			if (auto checked = batch.validate(); !checked)
				return reducer_error("invalid-input-batch");
			output.diagnostics.insert(
				output.diagnostics.end(), batch.diagnostics.begin(), batch.diagnostics.end());
			const auto batch_unit = "batch:" + std::string{batch.unit.value()} + ":" +
				std::string{batch.variant.value()};
			if (coverage_units.insert(batch_unit).second)
			{
				output.coverage.request({"reducer-batch", batch_unit});
				output.coverage.classify(
					{"reducer-batch",
					 batch_unit,
					 batch.coverage.failed != 0U ? coverage_state::failed : coverage_state::covered,
					 batch.coverage.failed != 0U
						 ? std::optional<std::string>{"reducer.frontend-batch-failed"}
						 : std::nullopt});
			}
			for (const auto& observation : batch.observations)
			{
				for (const auto& [key, value] : observation.payload)
					if (!operational_key(key) && absolute_path_value(value))
						return reducer_error("absolute-root-in-observation");
				observation_ref row{&observation, &batch};
				const auto unit = observation_unit(row);
				if (coverage_units.insert(unit).second)
				{
					const auto state = observation_state(observation);
					observation_coverage.emplace(
						unit,
						std::pair{state,
								  state == coverage_state::covered
									  ? std::optional<std::string>{}
									  : std::optional<std::string>{"reducer.input-not-covered"}});
				}
				groups[group_key(observation)].push_back(row);
			}
		}
		std::ranges::sort(output.diagnostics);
		const auto diagnostic_duplicate = std::ranges::unique(output.diagnostics);
		output.diagnostics.erase(diagnostic_duplicate.begin(), diagnostic_duplicate.end());

		runtime::fnv1a_hash_adapter hashes;
		identity::collision_registry collisions;
		for (auto& [key, rows] : groups)
		{
			std::ranges::sort(rows,
							  {},
							  [](const observation_ref& row)
							  {
								  return observation_unit(row) + ":" +
									  source_key(row.observation->source);
							  });
			std::map<std::string, std::vector<observation_ref>> partitions;
			for (const auto& row : rows)
				partitions[domain_payload(*row.observation)].push_back(row);
			const auto conflict = same_variant_conflict(partitions) ||
				definition_source_conflict(rows.front().observation->kind, rows);
			reduction_trace_row trace;
			trace.group_key = key;
			trace.contributors = contributors(rows);
			trace.variants = variants(rows);
			if (conflict)
			{
				trace.decision = reduction_decision::conflict;
				auto conflict_id =
					make_stable_id("conflict",
								   "cxxlens.reduction-conflict.v1",
								   {{"code", "reducer.authoritative-conflict"}, {"group", key}},
								   collisions,
								   hashes);
				if (!conflict_id)
					return std::move(conflict_id.error());
				output.conflicts.push_back({conflict_id.value(),
											"reducer.authoritative-conflict",
											key,
											trace.contributors,
											trace.variants});
				for (const auto& row : rows)
					observation_coverage[observation_unit(row)] = {
						coverage_state::unresolved, "reducer.authoritative-conflict"};
			}
			else
			{
				const auto split = partitions.size() > 1U;
				trace.decision =
					split ? reduction_decision::variant_split : reduction_decision::merged;
				for (const auto& [payload, partition] : partitions)
				{
					auto fact = make_fact(key, payload, partition, split, collisions, hashes);
					if (!fact)
						return std::move(fact.error());
					trace.facts.push_back(fact.value().id);
					output.facts.push_back(std::move(fact.value()));
				}
				sort_unique(trace.facts);
			}
			output.trace.push_back(std::move(trace));
		}
		for (const auto& [unit, classification] : observation_coverage)
		{
			output.coverage.request({"reducer-observation", unit});
			output.coverage.classify(
				{"reducer-observation", unit, classification.first, classification.second});
		}
		std::ranges::sort(output.facts, fact_less);
		std::ranges::sort(output.trace, trace_less);
		std::ranges::sort(output.conflicts, conflict_less);
		if (auto checked = output.validate(); !checked)
			return std::move(checked.error());
		return output;
	}

	result<void> reduction_result::validate() const
	{
		if (schema != "cxxlens.reduction-trace.v1")
			return reducer_error("reduction-schema");
		if (auto checked = coverage.validate(); !checked)
			return reducer_error("reduction-coverage");
		if (!std::ranges::is_sorted(facts, fact_less) ||
			!std::ranges::is_sorted(trace, trace_less) ||
			!std::ranges::is_sorted(conflicts, conflict_less) ||
			!std::ranges::is_sorted(diagnostics))
			return reducer_error("reduction-order");
		std::set<std::string> ids;
		for (const auto& fact : facts)
		{
			if (auto checked = facts::validate(fact);
				!checked || !ids.insert(std::string{fact.id.value()}).second)
				return reducer_error("reduction-fact-invariant");
		}
		for (const auto& row : trace)
		{
			if (row.group_key.empty() ||
				!std::ranges::is_sorted(row.facts,
										{},
										[](const fact_id& id)
										{
											return id.value();
										}) ||
				!std::ranges::is_sorted(row.contributors,
										{},
										[](const compile_unit_id& id)
										{
											return id.value();
										}) ||
				!std::ranges::is_sorted(row.variants,
										{},
										[](const build_variant_id& id)
										{
											return id.value();
										}))
				return reducer_error("reduction-trace-invariant");
			if (row.decision == reduction_decision::conflict && !row.facts.empty())
				return reducer_error("conflict-has-arbitrary-winner");
		}
		return {};
	}

	std::string reduction_result::to_json() const
	{
		json::json_value::array fact_rows;
		for (const auto& fact : facts)
			fact_rows.emplace_back(json::json_value::object{
				{"id", std::string{fact.id.value()}},
				{"kind", kind_name(fact.kind)},
				{"stable_key", fact.stable_key},
				{"compile_units", static_cast<std::uint64_t>(fact.origin.compile_units.size())},
				{"variants", static_cast<std::uint64_t>(fact.origin.variants.size())},
			});
		json::json_value::array trace_rows;
		for (const auto& row : trace)
			trace_rows.emplace_back(json::json_value::object{
				{"group_key", row.group_key},
				{"decision", decision_name(row.decision)},
				{"facts", static_cast<std::uint64_t>(row.facts.size())},
				{"contributors", static_cast<std::uint64_t>(row.contributors.size())},
				{"variants", static_cast<std::uint64_t>(row.variants.size())},
			});
		json::json_value::array conflict_rows;
		for (const auto& conflict : conflicts)
			conflict_rows.emplace_back(json::json_value::object{
				{"id", conflict.id}, {"code", conflict.code}, {"group_key", conflict.group_key}});
		auto document = json::envelope(
			{schema, "1.0.0", "0.1.0"},
			{{"facts", std::move(fact_rows)},
			 {"trace", std::move(trace_rows)},
			 {"conflicts", std::move(conflict_rows)},
			 {"diagnostics", static_cast<std::uint64_t>(diagnostics.size())},
			 {"coverage",
			  json::json_value::object{
				  {"requested", static_cast<std::uint64_t>(coverage.requested().size())},
				  {"covered", static_cast<std::uint64_t>(coverage.count(coverage_state::covered))},
				  {"excluded",
				   static_cast<std::uint64_t>(coverage.count(coverage_state::excluded))},
				  {"failed", static_cast<std::uint64_t>(coverage.count(coverage_state::failed))},
				  {"unresolved",
				   static_cast<std::uint64_t>(coverage.count(coverage_state::unresolved))},
				  {"not_applicable",
				   static_cast<std::uint64_t>(coverage.count(coverage_state::not_applicable))},
			  }}});
		auto encoded = json::write(json::json_value{std::move(document)});
		return encoded ? std::move(encoded.value()) : std::string{};
	}
} // namespace cxxlens::detail::facts
