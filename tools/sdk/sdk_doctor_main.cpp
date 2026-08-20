#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/relations/build_compile_unit.hpp>
#include <cxxlens/relations/build_project.hpp>
#include <cxxlens/relations/build_toolchain_context.hpp>
#include <cxxlens/relations/build_variant.hpp>
#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_declaration.hpp>
#include <cxxlens/relations/cc_entity.hpp>
#include <cxxlens/relations/cc_type.hpp>
#include <cxxlens/relations/cc_type_component.hpp>
#include <cxxlens/relations/company_lock_acquire.hpp>
#include <cxxlens/relations/core_claim_conflict.hpp>
#include <cxxlens/relations/core_differential_disagreement.hpp>
#include <cxxlens/relations/core_provider_execution.hpp>
#include <cxxlens/relations/core_unresolved.hpp>
#include <cxxlens/relations/source_file.hpp>
#include <cxxlens/relations/source_origin.hpp>
#include <cxxlens/relations/source_span.hpp>
#include <cxxlens/sdk.hpp>

namespace
{
	/**
	 * @brief Build a registry over every relation descriptor this SDK build ships.
	 *
	 * This is the exact set of generated relation tags under `include/cxxlens/relations`; it is
	 * the real, already-shipped relation catalog, not an invented capability list.
	 */
	[[nodiscard]] cxxlens::sdk::result<cxxlens::sdk::relation_registry> known_relation_registry()
	{
		cxxlens::sdk::relation_registry registry;
		const std::array descriptors{
			cxxlens::build::relations::compile_unit::descriptor(),
			cxxlens::build::relations::project::descriptor(),
			cxxlens::build::relations::toolchain_context::descriptor(),
			cxxlens::build::relations::variant::descriptor(),
			cxxlens::cc::relations::call_direct_target::descriptor(),
			cxxlens::cc::relations::call_site::descriptor(),
			cxxlens::cc::relations::declaration::descriptor(),
			cxxlens::cc::relations::entity::descriptor(),
			cxxlens::cc::relations::type::descriptor(),
			cxxlens::cc::relations::type_component::descriptor(),
			cxxlens::company::relations::lock_acquire::descriptor(),
			cxxlens::core::relations::claim_conflict::descriptor(),
			cxxlens::core::relations::differential_disagreement::descriptor(),
			cxxlens::core::relations::provider_execution::descriptor(),
			cxxlens::core::relations::unresolved::descriptor(),
			cxxlens::source::relations::file::descriptor(),
			cxxlens::source::relations::origin::descriptor(),
			cxxlens::source::relations::span::descriptor(),
		};
		for (const auto& descriptor : descriptors)
			if (auto added = registry.add(descriptor); !added)
				return added.error();
		return registry;
	}

	/**
	 * @brief Split a canonical `"<dotted.lowercase.name>.v<major>"` relation ID.
	 * @return The name and major version, or nothing when `id` is not that exact shape.
	 */
	[[nodiscard]] std::optional<std::pair<std::string_view, std::uint32_t>>
	split_relation_id(const std::string_view id)
	{
		const auto canonical_character = [](const char byte)
		{
			return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '.' ||
				byte == '_';
		};
		if (id.empty() || !std::ranges::all_of(id, canonical_character))
			return std::nullopt;
		const auto separator = id.rfind(".v");
		if (separator == std::string_view::npos || separator == 0U)
			return std::nullopt;
		const auto name = id.substr(0U, separator);
		const auto version_digits = id.substr(separator + 2U);
		if (name.empty() || version_digits.empty())
			return std::nullopt;
		std::uint32_t major{};
		const auto parsed = std::from_chars(
			version_digits.data(), version_digits.data() + version_digits.size(), major);
		if (parsed.ec != std::errc{} || parsed.ptr != version_digits.data() + version_digits.size())
			return std::nullopt;
		return std::pair{name, major};
	}

	/** @brief One requested relation's presence outcome against `known_relation_registry()`. */
	struct component_check
	{
		std::string_view id;
		bool present{};
		std::string reason_code;
	};

	/**
	 * @brief Report, for each requested relation ID, whether this SDK build's registry has it.
	 *
	 * `relation_ids` names the relations a consumer configuration (a provider or query author)
	 * depends on, e.g. a manifest's `offered_relations`/`required_relations` entries. Absence is
	 * derived from the real `cxxlens::sdk::relation_registry::require` outcome, reusing its exact
	 * `sdk.relation-not-found` / `sdk.relation-major-mismatch` reason codes rather than inventing
	 * new ones.
	 */
	int run_missing(const std::span<char*> relation_ids)
	{
		if (relation_ids.empty())
		{
			std::cerr << "usage: cxxlens-sdk-doctor missing <relation-id> [<relation-id> ...]\n";
			return 2;
		}
		auto registry = known_relation_registry();
		if (!registry)
		{
			std::cerr << "sdk.doctor-contract-invalid\n";
			return 1;
		}
		std::vector<std::pair<std::string_view, std::uint32_t>> parsed;
		parsed.reserve(relation_ids.size());
		for (const auto* argument : relation_ids)
		{
			const auto split = split_relation_id(std::string_view{argument});
			if (!split)
			{
				std::cerr << "sdk.relation-id-malformed: " << argument << '\n';
				return 2;
			}
			parsed.push_back(*split);
		}
		std::vector<component_check> checks;
		checks.reserve(relation_ids.size());
		std::size_t missing_count{};
		for (std::size_t index = 0U; index < relation_ids.size(); ++index)
		{
			const auto [name, major] = parsed[index];
			auto found = registry->require(name, major);
			if (found)
			{
				checks.push_back({std::string_view{relation_ids[index]}, true, {}});
				continue;
			}
			++missing_count;
			checks.push_back({std::string_view{relation_ids[index]}, false, found.error().code});
		}
		std::cout << R"({"schema":"cxxlens.sdk-doctor-missing.v1","mode":"missing","requested":)"
				  << checks.size() << R"(,"missing":)" << missing_count << R"(,"status":")"
				  << (missing_count == 0U ? "complete" : "incomplete") << R"(","components":[)";
		for (std::size_t index = 0U; index < checks.size(); ++index)
		{
			if (index != 0U)
				std::cout << ',';
			std::cout << R"({"id":")" << checks[index].id << R"(","status":")"
					  << (checks[index].present ? "present" : "missing") << '"';
			if (!checks[index].present)
				std::cout << R"(,"reason_code":")" << checks[index].reason_code << '"';
			std::cout << '}';
		}
		std::cout << "]}\n";
		return missing_count == 0U ? 0 : 1;
	}
} // namespace

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::cerr
			<< "usage: cxxlens-sdk-doctor inspect|doctor|query-ir|provider-manifest|missing\n";
		return 2;
	}
	const std::string_view mode{argv[1]};
	if (mode == "missing")
		return run_missing(std::span<char*>{argv + 2, static_cast<std::size_t>(argc - 2)});
	if (argc != 2 ||
		(mode != "inspect" && mode != "doctor" && mode != "query-ir" &&
		 mode != "provider-manifest"))
	{
		std::cerr
			<< "usage: cxxlens-sdk-doctor inspect|doctor|query-ir|provider-manifest|missing\n";
		return 2;
	}
	using relation = cxxlens::cc::relations::call_site;
	auto typed = cxxlens::sdk::query::from<relation>();
	cxxlens::sdk::relation_registry registry;
	auto added = registry.add(relation::descriptor());
	auto dynamic = registry.require("cc.call_site", 1U);
	if (!typed || !added || !dynamic)
	{
		std::cerr << "sdk.doctor-contract-invalid\n";
		return 1;
	}
	auto dynamic_query = cxxlens::sdk::query::dynamic_query::from(*dynamic);
	if (!dynamic_query || typed->ir().digest() != dynamic_query->ir().digest())
	{
		std::cerr << "sdk.static-dynamic-ir-mismatch\n";
		return 1;
	}
	if (mode == "query-ir")
	{
		auto predicate =
			cxxlens::sdk::query::equals_present(cxxlens::sdk::query::col<relation::ordinal>(),
												cxxlens::sdk::query::literal::unsigned_integer(0U));
		if (!predicate)
			return 1;
		auto filtered = std::move(*typed).where(std::move(*predicate));
		if (!filtered)
			return 1;
		const std::array keys{cxxlens::sdk::query::col<relation::call>()};
		auto ordered = std::move(*filtered).order_by(keys);
		if (!ordered)
			return 1;
		const std::array output{cxxlens::sdk::query::col<relation::call>(),
								cxxlens::sdk::query::col<relation::source>()};
		auto projected = std::move(*ordered).project(output);
		if (!projected || !projected->ir().validate())
			return 1;
		std::cout << projected->ir().canonical_form() << '\n';
		return 0;
	}
	if (mode == "provider-manifest")
	{
		const auto zero_digest = "sha256:" + std::string(64U, '0');
		cxxlens::sdk::provider::manifest manifest;
		manifest.provider_id = "company.example.doctor";
		manifest.provider_version = {1U, 0U, 0U};
		manifest.package_identity = "company.example.doctor-package";
		manifest.publisher = "company.example";
		manifest.license = "Apache-2.0";
		manifest.platform_tuples = {"linux-x86_64"};
		manifest.provider_binary_digest = zero_digest;
		manifest.provider_semantic_contract_digest = zero_digest;
		manifest.offered_relations = {"cc.call_site.v1"};
		manifest.interpretation_domains = {"cc.canonical-1"};
		manifest.invalidation_contract = zero_digest;
		manifest.determinism_contract = zero_digest;
		manifest.resource_class = "provider.standard";
		manifest.requested_qualifications = {"schema-conformant"};
		if (!manifest.validate())
			return 1;
		std::cout << manifest.canonical_json() << '\n';
		return 0;
	}
	std::cout << "{\"descriptor\":\"" << relation::descriptor().descriptor_digest
			  << "\",\"mode\":\"" << mode << "\",\"ordinary_llvm_dependency\":false,"
			  << "\"query_ir\":\"" << typed->ir().digest() << "\",\"status\":\"accepted\"}\n";
	return 0;
}
