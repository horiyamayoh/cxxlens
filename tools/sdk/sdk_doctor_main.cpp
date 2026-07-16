#include <array>
#include <iostream>
#include <string_view>
#include <utility>

#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/sdk.hpp>

int main(int argc, char** argv)
{
	if (argc != 2 ||
		(std::string_view{argv[1]} != "inspect" && std::string_view{argv[1]} != "doctor" &&
		 std::string_view{argv[1]} != "query-ir" &&
		 std::string_view{argv[1]} != "provider-manifest"))
	{
		std::cerr << "usage: cxxlens-sdk-doctor inspect|doctor|query-ir|provider-manifest\n";
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
	if (std::string_view{argv[1]} == "query-ir")
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
	if (std::string_view{argv[1]} == "provider-manifest")
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
			  << "\",\"mode\":\"" << argv[1] << "\",\"ordinary_llvm_dependency\":false,"
			  << "\"query_ir\":\"" << typed->ir().digest() << "\",\"status\":\"accepted\"}\n";
	return 0;
}
