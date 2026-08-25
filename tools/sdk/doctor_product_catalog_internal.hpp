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
		const std::array descriptors{
			build::relations::compile_unit::descriptor(),
			build::relations::project::descriptor(),
			build::relations::toolchain_context::descriptor(),
			build::relations::variant::descriptor(),
			cc::relations::call_direct_target::descriptor(),
			cc::relations::call_site::descriptor(),
			cc::relations::declaration::descriptor(),
			cc::relations::entity::descriptor(),
			cc::relations::type::descriptor(),
			cc::relations::type_component::descriptor(),
			company::relations::lock_acquire::descriptor(),
			core::relations::claim_conflict::descriptor(),
			core::relations::differential_disagreement::descriptor(),
			core::relations::provider_execution::descriptor(),
			core::relations::unresolved::descriptor(),
			source::relations::file::descriptor(),
			source::relations::origin::descriptor(),
			source::relations::span::descriptor(),
		};
		for (const auto& descriptor : descriptors)
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

	struct provider_trust_spec
	{
		std::string manifest;
		std::string binary;
		std::string semantics;
		std::string signature;
		std::string certification;
		std::string revocation;
		[[nodiscard]] bool operator==(const provider_trust_spec&) const = default;
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
		provider_trust_spec trust;
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

	// Semantic identity of the shipped product catalog value.  This binds product semantics,
	// not implementation source bytes; the focused schema test derives the same value from the
	// installed YAML catalog independently.
	inline constexpr std::string_view sdk_doctor_catalog_semantic_identity{
		"semantic-v2:sha256:2b24615bb07b9a3c525a6edf4c95604d4829131b3c6ebd68bc05a4836fa33e8d"};

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
			  "Select one exact trusted Protocol 2.0 provider support tuple."},
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
			 {"exact-digest-required",
			  "exact-digest-required",
			  "exact-digest-required",
			  "verified-required",
			  "exact-registry-binding-required",
			  "not-revoked-required"},
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

	class installed_product_catalog_loader;

	class authenticated_capability_catalog final
	{
	  public:
		[[nodiscard]] const capability_catalog& catalog() const noexcept
		{
			return catalog_;
		}

		[[nodiscard]] std::string_view semantic_identity() const noexcept
		{
			return semantic_identity_;
		}

	  private:
		friend class installed_product_catalog_loader;

		authenticated_capability_catalog(capability_catalog catalog, std::string semantic_identity)
			: catalog_{std::move(catalog)}, semantic_identity_{std::move(semantic_identity)}
		{
		}

		capability_catalog catalog_;
		std::string semantic_identity_;
	};

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

	[[nodiscard]] inline json_value::array_type
	catalog_string_array(std::vector<std::string> values)
	{
		json_value::array_type output;
		output.reserve(values.size());
		for (auto& value : values)
			output.push_back(json_value::string_value(std::move(value)));
		return output;
	}

	[[nodiscard]] inline bool bounded_capability_catalog(const capability_catalog& catalog) noexcept
	{
		std::size_t bytes{};
		const auto text = [&](const std::string& value)
		{
			if (value.size() > maximum_json_string_bytes ||
				value.size() > maximum_project_bytes - bytes)
				return false;
			bytes += value.size();
			return valid_utf8(value);
		};
		const auto texts = [&](const std::vector<std::string>& values)
		{
			return values.size() <= maximum_capability_count && std::ranges::all_of(values, text);
		};
		if (catalog.commands.empty() || catalog.commands.size() > maximum_capability_count ||
			catalog.use_cases.empty() || catalog.use_cases.size() > maximum_capability_count ||
			catalog.capabilities.empty() ||
			catalog.capabilities.size() > maximum_capability_count || !text(catalog.binding_id) ||
			!text(catalog.document_version))
			return false;
		for (const auto& command : catalog.commands)
			if (!text(command.id) || !text(command.consumer) || !text(command.output_schema) ||
				!texts(command.formats))
				return false;
		for (const auto& use_case : catalog.use_cases)
			if (!text(use_case.id) || !text(use_case.consumer) || !text(use_case.question) ||
				!texts(use_case.capability_path))
				return false;
		for (const auto& capability : catalog.capabilities)
			if (!text(capability.id) || !texts(capability.dependencies) ||
				!texts(capability.consumers) || !texts(capability.relation_ids) ||
				!text(capability.completion_action))
				return false;
		const auto& provider = catalog.provider_support;
		if (!text(provider.protocol_downgrade) || !texts(provider.required_features) ||
			!texts(provider.required_relations) || !texts(provider.required_interpretations) ||
			!text(provider.sandbox_minimum) || !text(provider.trust.manifest) ||
			!text(provider.trust.binary) || !text(provider.trust.semantics) ||
			!text(provider.trust.signature) || !text(provider.trust.certification) ||
			!text(provider.trust.revocation) || !text(provider.candidate_identity.domain) ||
			!text(provider.candidate_identity.encoding) ||
			!text(provider.candidate_identity.producer) ||
			!text(provider.candidate_identity.input_binding) ||
			!texts(provider.support_tuple_fields) || !text(provider.conflict_policy.subject) ||
			!text(provider.conflict_policy.selection) ||
			!text(provider.conflict_policy.duplicate_identity) ||
			!text(provider.conflict_policy.same_provider_version_distinct_identity) ||
			!text(provider.conflict_policy.multiple_valid_candidates) ||
			!text(provider.conflict_policy.fallback) || provider.supported_tuples.empty() ||
			provider.supported_tuples.size() > maximum_capability_count ||
			!texts(catalog.store_support.backends) || !text(catalog.store_support.format))
			return false;
		for (const auto& tuple : provider.supported_tuples)
			if (!text(tuple.release_version) || !text(tuple.surface) || !text(tuple.os) ||
				!text(tuple.architecture) || !text(tuple.compiler_provider_major) ||
				!text(tuple.linkage))
				return false;
		return true;
	}

	[[nodiscard]] inline std::variant<std::string, product_error>
	catalog_semantic_projection(const capability_catalog& input)
	{
		if (!bounded_capability_catalog(input))
			return product_error{"doctor.catalog-invalid", "catalog", "bound"};
		auto catalog = input;
		json_value::array_type commands;
		for (auto& command : catalog.commands)
			commands.push_back(json_value::object_value({
				{"consumer", json_value::string_value(std::move(command.consumer))},
				{"exit_codes",
				 json_value::object_value({
					 {"invalid_request",
					  json_value::unsigned_value(command.exit_codes.invalid_request)},
					 {"not_proved", json_value::unsigned_value(command.exit_codes.not_proved)},
					 {"proved", json_value::unsigned_value(command.exit_codes.proved)},
				 })},
				{"formats",
				 json_value::array_value(catalog_string_array(std::move(command.formats)))},
				{"id", json_value::string_value(std::move(command.id))},
				{"output_schema", json_value::string_value(std::move(command.output_schema))},
			}));
		json_value::array_type use_cases;
		for (auto& use_case : catalog.use_cases)
			use_cases.push_back(json_value::object_value({
				{"capability_path",
				 json_value::array_value(
					 catalog_string_array(std::move(use_case.capability_path)))},
				{"consumer", json_value::string_value(std::move(use_case.consumer))},
				{"id", json_value::string_value(std::move(use_case.id))},
				{"question", json_value::string_value(std::move(use_case.question))},
			}));
		json_value::array_type capabilities;
		for (auto& capability : catalog.capabilities)
		{
			json_value::object_type projection{
				{"completion_action",
				 json_value::string_value(std::move(capability.completion_action))},
				{"consumers",
				 json_value::array_value(catalog_string_array(std::move(capability.consumers)))},
				{"id", json_value::string_value(std::move(capability.id))},
				{"kind",
				 json_value::string_value(std::string{catalog_kind_token(capability.kind)})},
				{"requires",
				 json_value::array_value(catalog_string_array(std::move(capability.dependencies)))},
			};
			if (!capability.relation_ids.empty())
				projection.emplace("relation_ids",
								   json_value::array_value(
									   catalog_string_array(std::move(capability.relation_ids))));
			capabilities.push_back(json_value::object_value(std::move(projection)));
		}
		json_value::array_type tuples;
		for (auto& tuple : catalog.provider_support.supported_tuples)
			tuples.push_back(json_value::object_value({
				{"architecture", json_value::string_value(std::move(tuple.architecture))},
				{"compiler_provider_major",
				 json_value::string_value(std::move(tuple.compiler_provider_major))},
				{"linkage", json_value::string_value(std::move(tuple.linkage))},
				{"os", json_value::string_value(std::move(tuple.os))},
				{"release_version", json_value::string_value(std::move(tuple.release_version))},
				{"surface", json_value::string_value(std::move(tuple.surface))},
			}));
		const auto projection = canonical_json(json_value::object_value({
			{"capabilities", json_value::array_value(std::move(capabilities))},
			{"commands", json_value::array_value(std::move(commands))},
			{"document_version", json_value::string_value(std::move(catalog.document_version))},
			{"provider_support",
			 json_value::object_value({
				 {"candidate_identity",
				  json_value::object_value({
					  {"domain",
					   json_value::string_value(
						   std::move(catalog.provider_support.candidate_identity.domain))},
					  {"encoding",
					   json_value::string_value(
						   std::move(catalog.provider_support.candidate_identity.encoding))},
					  {"input_binding",
					   json_value::string_value(
						   std::move(catalog.provider_support.candidate_identity.input_binding))},
					  {"producer",
					   json_value::string_value(
						   std::move(catalog.provider_support.candidate_identity.producer))},
				  })},
				 {"conflict_policy",
				  json_value::object_value({
					  {"duplicate_identity",
					   json_value::string_value(
						   std::move(catalog.provider_support.conflict_policy.duplicate_identity))},
					  {"fallback",
					   json_value::string_value(
						   std::move(catalog.provider_support.conflict_policy.fallback))},
					  {"multiple_valid_candidates",
					   json_value::string_value(std::move(
						   catalog.provider_support.conflict_policy.multiple_valid_candidates))},
					  {"same_provider_version_distinct_identity",
					   json_value::string_value(
						   std::move(catalog.provider_support.conflict_policy
										 .same_provider_version_distinct_identity))},
					  {"selection",
					   json_value::string_value(
						   std::move(catalog.provider_support.conflict_policy.selection))},
					  {"subject",
					   json_value::string_value(
						   std::move(catalog.provider_support.conflict_policy.subject))},
				  })},
				 {"protocol",
				  json_value::object_value({
					  {"downgrade",
					   json_value::string_value(
						   std::move(catalog.provider_support.protocol_downgrade))},
					  {"major",
					   json_value::unsigned_value(catalog.provider_support.protocol_major)},
					  {"minor",
					   json_value::unsigned_value(catalog.provider_support.protocol_minor)},
				  })},
				 {"required_features",
				  json_value::array_value(
					  catalog_string_array(std::move(catalog.provider_support.required_features)))},
				 {"required_interpretations",
				  json_value::array_value(catalog_string_array(
					  std::move(catalog.provider_support.required_interpretations)))},
				 {"required_relations",
				  json_value::array_value(catalog_string_array(
					  std::move(catalog.provider_support.required_relations)))},
				 {"sandbox_minimum",
				  json_value::string_value(std::move(catalog.provider_support.sandbox_minimum))},
				 {"support_tuple_fields",
				  json_value::array_value(catalog_string_array(
					  std::move(catalog.provider_support.support_tuple_fields)))},
				 {"supported_tuples", json_value::array_value(std::move(tuples))},
				 {"trust",
				  json_value::object_value({
					  {"binary",
					   json_value::string_value(std::move(catalog.provider_support.trust.binary))},
					  {"certification",
					   json_value::string_value(
						   std::move(catalog.provider_support.trust.certification))},
					  {"manifest",
					   json_value::string_value(
						   std::move(catalog.provider_support.trust.manifest))},
					  {"revocation",
					   json_value::string_value(
						   std::move(catalog.provider_support.trust.revocation))},
					  {"semantics",
					   json_value::string_value(
						   std::move(catalog.provider_support.trust.semantics))},
					  {"signature",
					   json_value::string_value(
						   std::move(catalog.provider_support.trust.signature))},
				  })},
			 })},
			{"schema", json_value::string_value(std::move(catalog.binding_id))},
			{"store_support",
			 json_value::object_value({
				 {"backends",
				  json_value::array_value(
					  catalog_string_array(std::move(catalog.store_support.backends)))},
				 {"format", json_value::string_value(std::move(catalog.store_support.format))},
			 })},
			{"use_cases", json_value::array_value(std::move(use_cases))},
		}));
		return projection;
	}

	[[nodiscard]] inline std::variant<std::string, product_error>
	catalog_semantic_identity(const capability_catalog& input)
	{
		auto projection = catalog_semantic_projection(input);
		if (std::holds_alternative<product_error>(projection))
			return std::get<product_error>(std::move(projection));
		auto digest = sdk::semantic_digest("cxxlens.sdk-doctor-catalog.v1",
										   std::get<std::string>(std::move(projection)));
		if (!digest)
			return product_error{digest.error().code, digest.error().field, digest.error().detail};
		return std::move(*digest);
	}

	// The shipped catalog is typed data embedded in the installed executable.  It is
	// accepted only after its complete semantic projection matches the independently
	// constructed compiled expectation; path presence and declared IDs are insufficient.
	class installed_product_catalog_loader final
	{
	  public:
		[[nodiscard]] std::variant<authenticated_capability_catalog, product_error> load() const
		{
			return load(sdk_doctor_catalog_value());
		}

		[[nodiscard]] std::variant<authenticated_capability_catalog, product_error>
		load(const capability_catalog& installed) const
		{
			const auto expected_catalog = sdk_doctor_catalog_value();
			if (installed != expected_catalog)
				return product_error{
					"doctor.catalog-invalid", "catalog_binding", "typed-value-mismatch"};
			auto observed = catalog_semantic_identity(installed);
			if (std::holds_alternative<product_error>(observed))
				return std::get<product_error>(std::move(observed));
			auto observed_identity = std::get<std::string>(std::move(observed));
			if (observed_identity != sdk_doctor_catalog_semantic_identity)
				return product_error{
					"doctor.catalog-invalid", "catalog_binding", "semantic-identity-mismatch"};
			return authenticated_capability_catalog{installed, std::move(observed_identity)};
		}
	};

} // namespace cxxlens::sdk::doctor
