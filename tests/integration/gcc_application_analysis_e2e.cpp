#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
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
#include <cxxlens/relations/source_file.hpp>
#include <cxxlens/relations/source_span.hpp>
#include <cxxlens/sdk.hpp>
#include <cxxlens/sdk/application_analysis.hpp>

namespace
{
	using namespace cxxlens::sdk;

	[[nodiscard]] std::string digest(const char digit)
	{
		return "sha256:" + std::string(64U, digit);
	}

	[[nodiscard]] result<std::vector<std::byte>> read_bounded(const std::string& path)
	{
		std::ifstream input{path, std::ios::binary | std::ios::ate};
		if (!input)
			return error{"application-analysis.fixture-unreadable",
						 "bundle",
						 "capture bundle could not be opened"};
		const auto end = input.tellg();
		constexpr std::uint64_t maximum_bytes{std::uint64_t{64U} * 1024U * 1024U};
		if (end <= std::streampos{})
			return error{
				"application-analysis.fixture-size-invalid", "bundle", "capture bundle was empty"};
		const auto size = static_cast<std::uint64_t>(static_cast<std::streamoff>(end));
		if (size > maximum_bytes ||
			size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
			return error{"application-analysis.fixture-size-invalid",
						 "bundle",
						 "capture bundle exceeded the test input boundary"};
		std::vector<std::byte> bytes(static_cast<std::size_t>(size));
		input.seekg(0);
		input.read(reinterpret_cast<char*>(bytes.data()),
				   static_cast<std::streamsize>(bytes.size()));
		if (!input)
			return error{"application-analysis.fixture-read-failed",
						 "bundle",
						 "capture bundle could not be read completely"};
		return bytes;
	}

	[[nodiscard]] provider::provider_candidate
	worker_candidate(const std::string& worker,
					 const std::string& worker_digest,
					 const std::vector<std::string>& relation_ids,
					 const provider::sandbox_policy& policy)
	{
		provider::manifest manifest;
		manifest.provider_id = "cxxlens.clang23-gcc-replay";
		manifest.provider_version = {1U, 0U, 0U};
		manifest.package_identity = "cxxlens.clang23-gcc-replay.package";
		manifest.publisher = "cxxlens.project";
		manifest.license = "Apache-2.0 WITH LLVM-exception";
		manifest.protocol = {provider::protocol_v2_major,
							 provider::protocol_v2_minor,
							 provider::protocol_v2_minor,
							 {"credit-backpressure", "task-input-chunks-v2"},
							 {}};
		manifest.platform_tuples = {"linux-x86_64-clang23"};
		manifest.provider_binary_digest = worker_digest;
		manifest.provider_semantic_contract_digest = "semantic-v2:" + digest('b');
		manifest.offered_relations = relation_ids;
		manifest.interpretation_domains = {"cc.clang23-gcc-replay-1"};
		manifest.invalidation_contract = digest('c');
		manifest.determinism_contract = digest('d');
		manifest.resource_class = "provider.application-analysis";
		manifest.requested_qualifications = {"experimental"};
		return {std::move(manifest),
				provider::discovery_source::explicit_path,
				{worker},
				true,
				true,
				true,
				{"experimental"},
				{"linux-glibc",
				 policy.mechanisms,
				 provider::sandbox_assurance::enforced,
				 policy.policy_digest(),
				 digest('e')},
				{}};
	}

	[[nodiscard]] result<std::size_t> query_rows(const snapshot_handle& snapshot,
												 const relation_descriptor& descriptor,
												 const bool require_unresolved)
	{
		auto logical = query::builder::from(descriptor);
		if (!logical)
			return unexpected(std::move(logical.error()));
		auto runtime = query::reference_engine::bind(snapshot);
		if (!runtime)
			return unexpected(std::move(runtime.error()));
		auto queried = runtime->execute(std::move(*logical).finish());
		if (!queried)
			return unexpected(std::move(queried.error()));
		if (queried->execution() != query::execution_status::complete ||
			(require_unresolved && queried->unresolved_items().empty()))
			return error{"application-analysis.query-fidelity-lost",
						 "query",
						 "partial GCC replay must remain query-visible as unresolved"};
		auto rows = queried->rows();
		std::size_t count{};
		while (true)
		{
			auto next = rows.next();
			if (!next)
				return unexpected(std::move(next.error()));
			if (!*next)
				break;
			if (!(*next)->copy())
				return error{"application-analysis.query-row-invalid",
							 "query",
							 "source.file query returned an invalid row"};
			++count;
		}
		return count;
	}

	int run(const int argc, char** argv)
	{
		if (argc != 3)
			return 2;
		auto bundle_bytes = read_bounded(argv[1]);
		auto worker_bytes = read_bounded(argv[2]);
		if (!bundle_bytes || !worker_bytes)
			return 3;
		auto bundle = decode_capture_bundle(*bundle_bytes);
		if (!bundle || bundle->production_compiler() != "gcc-16.2.0" ||
			bundle->capture_adapter() != "compile-commands" || bundle->compile_unit_count() != 2U)
			return 4;
		auto project = import_capture(*bundle);
		if (!project || project->replay_plans().size() != 2U || project->unresolved().empty())
			return 5;

		const std::array descriptors{
			&cxxlens::build::relations::project::descriptor(),
			&cxxlens::build::relations::compile_unit::descriptor(),
			&cxxlens::build::relations::variant::descriptor(),
			&cxxlens::build::relations::toolchain_context::descriptor(),
			&cxxlens::source::relations::file::descriptor(),
			&cxxlens::source::relations::span::descriptor(),
			&cxxlens::cc::relations::entity::descriptor(),
			&cxxlens::cc::relations::declaration::descriptor(),
			&cxxlens::cc::relations::type::descriptor(),
			&cxxlens::cc::relations::call_site::descriptor(),
			&cxxlens::cc::relations::call_direct_target::descriptor(),
		};
		relation_registry registry;
		std::vector<std::string> relation_ids;
		for (const auto* descriptor : descriptors)
		{
			if (!registry.add(*descriptor))
				return 6;
			relation_ids.push_back(descriptor->id);
		}
		std::ranges::sort(relation_ids);
		auto engine = registry.build("gcc-application-analysis-e2e");
		const auto policies = provider::builtin_sandbox_policies();
		if (!engine || policies.empty())
			return 7;

		const auto worker_digest = content_digest(*worker_bytes);
		auto candidate = worker_candidate(argv[2], worker_digest, relation_ids, policies.front());
		provider::provider_selection_request selection{
			candidate.description.provider_id,
			candidate.description.provider_version,
			candidate.description.provider_binary_digest,
			candidate.description.provider_semantic_contract_digest,
			{provider::sandbox_assurance::enforced, policies.front().policy_digest()},
			true,
			std::nullopt};
		snapshot_draft publication{{"catalog:gcc-application-analysis-e2e",
									"experimental",
									std::string{engine->generation()},
									"condition:gcc-application-analysis-e2e",
									std::string{engine->registry_digest()},
									digest('f'),
									digest('0')},
								   {1U, 0U, 0U},
								   std::string{project->catalog_semantic_digest()},
								   std::nullopt};
		auto request = materialization_request::make(
			*engine, publication, relation_ids, "cc.clang23-gcc-replay-1", selection, {candidate});
		auto store = make_in_memory_snapshot_store(*engine);
		if (!request || !store)
			return 8;
		auto result = materialize(*store, *project, *request);
		if (!result)
		{
			std::cerr << result.error().code << ':' << result.error().field << ':'
					  << result.error().detail << '\n';
			return 9;
		}
		if (result->terminal() != materialization_terminal::published_partial ||
			!result->published_snapshot() || !result->provenance() ||
			result->unresolved().empty() || store->retained_generation_count() != 1U)
		{
			std::cerr << "unexpected materialization result: terminal="
					  << static_cast<int>(result->terminal())
					  << " snapshot=" << result->published_snapshot().has_value()
					  << " provenance=" << result->provenance().has_value()
					  << " unresolved=" << result->unresolved().size()
					  << " generations=" << store->retained_generation_count() << '\n';
			return 9;
		}
		if (result->provenance()->provider_binary_digest != worker_digest ||
			result->provenance()->provider_id != candidate.description.provider_id)
			return 10;
		auto source_files = query_rows(
			*result->published_snapshot(), cxxlens::source::relations::file::descriptor(), false);
		auto entities = query_rows(
			*result->published_snapshot(), cxxlens::cc::relations::entity::descriptor(), true);
		if (!source_files)
			std::cerr << source_files.error().code << ':' << source_files.error().field << ':'
					  << source_files.error().detail << '\n';
		if (!entities)
			std::cerr << entities.error().code << ':' << entities.error().field << ':'
					  << entities.error().detail << '\n';
		return source_files && *source_files >= 3U && entities && *entities >= 2U ? 0 : 11;
	}
} // namespace

int main(const int argc, char** argv) noexcept
{
	try
	{
		return run(argc, argv);
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 12;
	}
	catch (...)
	{
		return 13;
	}
}
