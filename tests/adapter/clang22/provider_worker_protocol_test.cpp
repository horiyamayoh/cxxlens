#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>

#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_entity.hpp>
#include <cxxlens/sdk.hpp>

namespace
{
	using namespace cxxlens::sdk;
	using namespace cxxlens::sdk::provider;

	constexpr std::string_view semantic_contract_digest =
		"sha256:1212121212121212121212121212121212121212121212121212121212121212";
	constexpr std::string_view sandbox_policy_digest =
		"sha256:3434343434343434343434343434343434343434343434343434343434343434";

	void require(const bool condition, const std::string& message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] std::string executable_digest(const std::string& executable)
	{
		std::ifstream input{executable, std::ios::binary};
		require(input.good(), "Clang worker could not be opened");
		const std::string bytes{std::istreambuf_iterator<char>{input},
								std::istreambuf_iterator<char>{}};
		require(!input.bad(), "Clang worker could not be read");
		return content_digest(std::as_bytes(std::span{bytes}));
	}
} // namespace

int main(const int argument_count, const char* const* arguments)
{
	using namespace cxxlens::sdk;
	using namespace cxxlens::sdk::provider;
	require(argument_count == 2, "Clang worker path is missing");
	const std::string executable{arguments[1]};

	manifest description;
	description.provider_id = "cxxlens.clang22.reference";
	description.provider_version = {1U, 0U, 0U};
	description.package_identity = "cxxlens.clang22.reference.package";
	description.publisher = "cxxlens";
	description.license = "Apache-2.0";
	description.protocol = {1U, 0U, 0U, {"credit-backpressure"}, {}};
	description.platform_tuples = {"linux-glibc"};
	description.provider_binary_digest = executable_digest(executable);
	description.provider_semantic_contract_digest = semantic_contract_digest;
	description.offered_relations = {
		"cc.call_direct_target@1",
		"cc.call_site@1",
		"cc.entity@1",
		"frontend.clang22.call_observation@1",
		"frontend.clang22.entity_observation@1",
		"frontend.clang22.type_observation@1",
	};
	description.interpretation_domains = {"cc.clang22-canonical-1"};
	description.invalidation_contract =
		"sha256:5656565656565656565656565656565656565656565656565656565656565656";
	description.determinism_contract =
		"sha256:7878787878787878787878787878787878787878787878787878787878787878";
	description.resource_class = "provider.clang22";
	description.sandbox_minimum = "enforced";
	description.requested_qualifications = {
		"canonical-semantic-qualified", "sandbox-qualified", "schema-conformant"};
	require(description.validate().has_value(), "Clang worker manifest is invalid");

	sandbox_report discovered_sandbox{
		"linux-glibc",
		{"no-shell-argv-exec"},
		sandbox_assurance::enforced,
		std::string{sandbox_policy_digest},
		"sha256:9090909090909090909090909090909090909090909090909090909090909090",
	};
	provider_selection selection{
		{description,
		 discovery_source::explicit_path,
		 {executable},
		 true,
		 true,
		 true,
		 {"canonical-semantic-qualified", "sandbox-qualified", "schema-conformant"},
		 discovered_sandbox,
		 {}},
		{},
		false,
		std::nullopt,
	};
	process_task_request request;
	request.selection = std::move(selection);
	request.output_descriptors = {
		cxxlens::cc::relations::entity::descriptor(),
		cxxlens::cc::relations::call_site::descriptor(),
		cxxlens::cc::relations::call_direct_target::descriptor(),
	};
	request.task_id = "clang22-malformed-input";
	request.payload = {std::byte{0x01}, std::byte{0x02}};
	request.task_input_digest = content_digest(request.payload);
	request.normalized_invocation_digest =
		"sha256:abababababababababababababababababababababababababababababababab";
	request.toolchain_digest =
		"sha256:cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd";
	request.environment_digest =
		"sha256:efefefefefefefefefefefefefefefefefefefefefefefefefefefefefefefef";
	request.sandbox = {sandbox_assurance::enforced, std::string{sandbox_policy_digest}};
	request.budget.wall_ms = 2000U;
	request.budget.cpu_ms = 2000U;
	request.budget.rss_bytes = 512U * 1024U * 1024U;
	request.budget.output_bytes = 8U * 1024U * 1024U;
	request.budget.open_files = 128U;
	request.budget.subprocesses = 1U;

	auto processes = make_system_provider_process_port();
	require(processes != nullptr, "system process provider is unavailable");
	process_provider_runtime runtime{*processes};
	auto report = runtime.execute(request);
	require(report && report->terminal == "provider.task-input-invalid" &&
				report->frames.front().type == message_type::hello &&
				report->frames.back().type == message_type::task_failed,
			"Clang 22 worker did not use the validated provider protocol");
}
