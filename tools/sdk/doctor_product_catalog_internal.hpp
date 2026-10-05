#pragma once

#include "doctor_product_provider_authority_internal.hpp"

namespace cxxlens::sdk::doctor
{
	struct relation_check
	{
		std::string id;
		std::string state;
		std::string reason_code;
	};

	[[nodiscard]] inline result<relation_registry> known_relation_registry()
	{
		relation_registry registry;
		for (const auto& descriptor : standard_relation_descriptors())
		{
			if (auto added = registry.add(descriptor); !added)
				return added.error();
		}
		return registry;
	}

	struct parsed_relation_id
	{
		std::string_view name;
		std::optional<std::uint32_t> semantic_major;
	};

	[[nodiscard]] inline std::optional<parsed_relation_id>
	split_relation_id(const std::string_view id)
	{
		if (id.empty() || id.size() > maximum_json_string_bytes)
			return std::nullopt;
		const auto marker = id.rfind(".v");
		if (marker == std::string_view::npos || marker == 0U || marker + 2U == id.size())
			return std::nullopt;
		const auto name = id.substr(0U, marker);
		if (name.front() == '.' || name.back() == '.' ||
			name.find("..") != std::string_view::npos || name.find('.') == std::string_view::npos)
			return std::nullopt;
		for (const auto byte : name)
			if ((byte < 'a' || byte > 'z') && (byte < '0' || byte > '9') && byte != '.' &&
				byte != '_')
				return std::nullopt;
		std::uint32_t major{};
		const auto digits = id.substr(marker + 2U);
		if (digits.size() > 1U && digits.front() == '0')
			return std::nullopt;
		if (!std::ranges::all_of(digits,
								 [](const char byte)
								 {
									 return byte >= '0' && byte <= '9';
								 }))
			return std::nullopt;
		const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), major);
		if (parsed.ptr != digits.data() + digits.size())
			return std::nullopt;
		if (parsed.ec == std::errc::result_out_of_range)
			return parsed_relation_id{name, std::nullopt};
		if (parsed.ec != std::errc{})
			return std::nullopt;
		if (major == 0U)
			return std::nullopt;
		return parsed_relation_id{name, major};
	}

	[[nodiscard]] inline std::variant<std::vector<relation_check>, product_error>
	check_relations(const std::span<const std::string_view> relation_ids)
	{
		if (relation_ids.empty())
			return product_error{"doctor.relation-request-invalid", "relation", "empty"};
		if (relation_ids.size() > maximum_json_collection_count)
			return product_error{"doctor.relation-request-invalid", "relation", "count-limit"};
		if (std::ranges::any_of(relation_ids,
								[](const std::string_view id)
								{
									return id.size() > maximum_json_string_bytes;
								}))
			return product_error{"doctor.relation-request-invalid", "relation", "byte-limit"};
		std::vector<std::string_view> ordered_ids{relation_ids.begin(), relation_ids.end()};
		std::ranges::sort(ordered_ids);
		if (std::ranges::adjacent_find(ordered_ids) != ordered_ids.end())
			return product_error{"doctor.relation-request-invalid", "relation", "duplicate-id"};
		auto registry = known_relation_registry();
		if (!registry)
			return product_error{registry.error().code, "relation", registry.error().detail};
		std::vector<relation_check> output;
		output.reserve(relation_ids.size());
		for (const auto id : ordered_ids)
		{
			const auto parsed = split_relation_id(id);
			if (!parsed)
				return product_error{"doctor.relation-request-invalid", "relation", "malformed-id"};
			if (!parsed->semantic_major)
			{
				output.push_back({std::string{id}, "unknown", "sdk.relation-major-mismatch"});
				continue;
			}
			auto found = registry->require(parsed->name, *parsed->semantic_major);
			if (found)
				output.push_back({std::string{id}, "proved", "none"});
			else
				output.push_back({std::string{id}, "unknown", found.error().code});
		}
		return output;
	}

	enum class capability_kind : std::uint8_t
	{
		input,
		provider,
		relation,
		query,
		store,
		recipe,
	};

	enum class capability_probe : std::uint8_t
	{
		project_catalog,
		source_closure,
		provider_protocol,
		provider_features,
		provider_relations,
		dependency_only,
		store,
	};

	struct capability_spec
	{
		std::string id;
		capability_kind kind;
		std::vector<std::string> dependencies;
		std::vector<std::string> consumers;
		std::vector<std::string> relation_ids;
		std::string completion_action;
		[[nodiscard]] bool operator==(const capability_spec&) const = default;
	};

	// Probe behavior is a deterministic interpretation of the catalog-authenticated
	// capability kind and ID.  It is not a second, identity-external catalog field.
	[[nodiscard]] inline std::optional<capability_probe>
	derived_capability_probe(const capability_spec& capability) noexcept
	{
		switch (capability.kind)
		{
			case capability_kind::input:
				if (capability.id == "input.project-catalog.v1")
					return capability_probe::project_catalog;
				if (capability.id == "input.source-closure.v1")
					return capability_probe::source_closure;
				break;
			case capability_kind::provider:
				if (capability.id == "provider.protocol.v2")
					return capability_probe::provider_protocol;
				if (capability.id == "provider.source-closure.v1")
					return capability_probe::provider_features;
				break;
			case capability_kind::relation:
				return capability_probe::provider_relations;
			case capability_kind::query:
			case capability_kind::recipe:
				return capability_probe::dependency_only;
			case capability_kind::store:
				return capability_probe::store;
		}
		return std::nullopt;
	}

	struct use_case_spec
	{
		std::string id;
		std::string consumer;
		std::string question;
		std::vector<std::string> capability_path;
		[[nodiscard]] bool operator==(const use_case_spec&) const = default;
	};

	struct command_exit_codes
	{
		std::uint32_t proved{};
		std::uint32_t not_proved{};
		std::uint32_t invalid_request{};
		[[nodiscard]] bool operator==(const command_exit_codes&) const = default;
	};

	struct command_spec
	{
		std::string id;
		std::string consumer;
		std::string output_schema;
		std::vector<std::string> formats;
		command_exit_codes exit_codes;
		[[nodiscard]] bool operator==(const command_spec&) const = default;
	};

	struct candidate_identity_spec
	{
		std::string domain;
		std::string encoding;
		std::string producer;
		std::string input_binding;
		[[nodiscard]] bool operator==(const candidate_identity_spec&) const = default;
	};

	struct conflict_policy_spec
	{
		std::string subject;
		std::string selection;
		std::string duplicate_identity;
		std::string same_provider_version_distinct_identity;
		std::string multiple_valid_candidates;
		std::string fallback;
		[[nodiscard]] bool operator==(const conflict_policy_spec&) const = default;
	};

	struct provider_support_spec
	{
		std::uint32_t protocol_major{};
		std::uint32_t protocol_minor{};
		std::string protocol_downgrade;
		std::vector<std::string> required_features;
		std::vector<std::string> required_relations;
		std::vector<std::string> required_interpretations;
		std::string sandbox_minimum;
		candidate_identity_spec candidate_identity;
		std::vector<std::string> support_tuple_fields;
		std::vector<support_tuple> supported_tuples;
		conflict_policy_spec conflict_policy;
		[[nodiscard]] bool operator==(const provider_support_spec&) const = default;
	};

	struct store_support_spec
	{
		std::vector<std::string> backends;
		std::string format;
		[[nodiscard]] bool operator==(const store_support_spec&) const = default;
	};

	struct capability_catalog
	{
		std::string binding_id;
		std::string document_version;
		std::vector<command_spec> commands;
		std::vector<use_case_spec> use_cases;
		std::vector<capability_spec> capabilities;
		provider_support_spec provider_support;
		store_support_spec store_support;
		[[nodiscard]] bool operator==(const capability_catalog&) const = default;
	};

	[[nodiscard]] inline capability_catalog sdk_doctor_catalog_value()
	{
		return {
			"cxxlens.sdk-doctor-catalog.v1",
			"1.0.0",
			{{"relation-presence",
			  "sdk-relation-consumer",
			  "cxxlens.sdk-doctor-relation-presence.v2",
			  {"json", "markdown"},
			  {0U, 1U, 2U}},
			 {"missing",
			  "semantic-query-consumer",
			  "cxxlens.sdk-doctor-resolution.v2",
			  {"json", "markdown"},
			  {0U, 1U, 2U}}},
			{{"cxxlens.clang22.materialize-and-query.v1",
			  "semantic-query-consumer",
			  "Can this project be materialized and queried with semantic partiality preserved?",
			  {"input.project-catalog.v1",
			   "input.source-closure.v1",
			   "provider.protocol.v2",
			   "provider.source-closure.v1",
			   "relation.cc-entity.v1",
			   "relation.cc-call-site.v1",
			   "query.logical-ir.v1",
			   "store.snapshot.v3",
			   "recipe.calls-to-function.v1"}}},
			{{"input.project-catalog.v1",
			  capability_kind::input,
			  {},
			  {"provider.protocol.v2", "store.snapshot.v3"},
			  {},
			  "Supply a valid content-bound project catalog."},
			 {"input.source-closure.v1",
			  capability_kind::input,
			  {"input.project-catalog.v1"},
			  {"provider.source-closure.v1"},
			  {},
			  "Supply the source-closure snapshot and compilation database identities."},
			 {"provider.protocol.v2",
			  capability_kind::provider,
			  {"input.project-catalog.v1"},
			  {"provider.source-closure.v1", "relation.cc-entity.v1", "relation.cc-call-site.v1"},
			  {},
			  "Configure one compatible Protocol 2.0 provider."},
			 {"provider.source-closure.v1",
			  capability_kind::provider,
			  {"provider.protocol.v2", "input.source-closure.v1"},
			  {"recipe.calls-to-function.v1"},
			  {},
			  "Provide task-input-chunks-v2 and task-source-closure-v2."},
			 {"relation.cc-entity.v1",
			  capability_kind::relation,
			  {"provider.protocol.v2"},
			  {"query.logical-ir.v1"},
			  {"cc.entity.v1"},
			  "Offer cc.entity.v1 under the required interpretation."},
			 {"relation.cc-call-site.v1",
			  capability_kind::relation,
			  {"provider.protocol.v2"},
			  {"query.logical-ir.v1"},
			  {"cc.call_site.v1"},
			  "Offer cc.call_site.v1 under the required interpretation."},
			 {"query.logical-ir.v1",
			  capability_kind::query,
			  {"relation.cc-entity.v1", "relation.cc-call-site.v1"},
			  {"recipe.calls-to-function.v1"},
			  {},
			  "Provide the logical query capability for both required relations."},
			 {"store.snapshot.v3",
			  capability_kind::store,
			  {"input.project-catalog.v1"},
			  {"recipe.calls-to-function.v1"},
			  {},
			  "Select a supported memory or SQLite snapshot v3 store."},
			 {"recipe.calls-to-function.v1",
			  capability_kind::recipe,
			  {"query.logical-ir.v1", "store.snapshot.v3", "provider.source-closure.v1"},
			  {"semantic-query-consumer"},
			  {},
			  "Satisfy every prerequisite before executing the recipe."}},
			{2U,
			 0U,
			 "forbidden",
			 {"task-input-chunks-v2", "task-source-closure-v2"},
			 {"cc.call_site.v1", "cc.entity.v1"},
			 {"cc.clang22-canonical-1"},
			 "enforced",
			 {"cxxlens.provider-candidate.v1",
			  "semantic-v2-sha256",
			  "provider-discovery",
			  "unique-candidate-id-required"},
			 {"release_version",
			  "surface",
			  "os",
			  "architecture",
			  "compiler_provider_major",
			  "linkage"},
			 {{"1.0.0", "core", "linux", "x86_64", "clang22", "static"},
			  {"1.0.0", "core", "linux", "x86_64", "clang22", "shared"},
			  {"1.0.0", "provider-sdk", "linux", "x86_64", "clang22", "static"},
			  {"1.0.0", "provider-sdk", "linux", "x86_64", "clang22", "shared"}},
			 {"capability-provider-candidate-set",
			  "exact-one-valid-candidate",
			  "reject",
			  "conflicting",
			  "conflicting",
			  "forbidden"}},
			{{"memory", "sqlite"}, "cxxlens.snapshot.v3"}};
	}

	[[nodiscard]] inline std::string_view catalog_kind_token(const capability_kind kind) noexcept
	{
		switch (kind)
		{
			case capability_kind::input:
				return "input";
			case capability_kind::provider:
				return "provider";
			case capability_kind::relation:
				return "relation";
			case capability_kind::query:
				return "query";
			case capability_kind::store:
				return "store";
			case capability_kind::recipe:
				return "recipe";
		}
		return "invalid";
	}

	class installed_product_catalog_loader final
	{
	  public:
		[[nodiscard]] std::variant<capability_catalog, product_error> load() const
		{
			return sdk_doctor_catalog_value();
		}
	};
} // namespace cxxlens::sdk::doctor
