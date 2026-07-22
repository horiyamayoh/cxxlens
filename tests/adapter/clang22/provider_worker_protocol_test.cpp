#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
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
#if defined(CXXLENS_SANITIZER_INSTRUMENTED)
	constexpr std::uint64_t provider_address_space_budget =
		std::numeric_limits<std::uint64_t>::max();
	constexpr std::uint64_t provider_subprocess_budget = 1024U;
#else
	constexpr std::uint64_t provider_address_space_budget = 512U * 1024U * 1024U;
	constexpr std::uint64_t provider_subprocess_budget = 1U;
#endif
	void require(const bool condition, const std::string& message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] sandbox_policy baseline_policy()
	{
		auto policies = builtin_sandbox_policies();
		require(policies.size() == 2U && policies.front().validate().has_value(),
				"built-in sandbox policy registry is invalid");
		return std::move(policies.front());
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
		"frontend.clang22.call_observation@2",
		"frontend.clang22.entity_observation@2",
		"frontend.clang22.type_observation@2",
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

	auto sandbox_policy = baseline_policy();
	sandbox_report discovered_sandbox{
		"linux-glibc",
		sandbox_policy.mechanisms,
		sandbox_assurance::enforced,
		sandbox_policy.policy_digest(),
		"sha256:9090909090909090909090909090909090909090909090909090909090909090",
	};
	provider_candidate candidate{
		description,
		discovery_source::explicit_path,
		{executable},
		true,
		true,
		true,
		{"canonical-semantic-qualified", "sandbox-qualified", "schema-conformant"},
		discovered_sandbox,
		{},
	};
	provider_selection_request selection_authority{
		description.provider_id,
		description.provider_version,
		description.provider_binary_digest,
		description.provider_semantic_contract_digest,
		{sandbox_assurance::enforced, sandbox_policy.policy_digest()},
		true,
		std::nullopt,
	};
	auto selection = select_provider(selection_authority, std::span{&candidate, 1U});
	require(selection.has_value(), "Clang worker provider selection failed");
	process_task_request request;
	request.selection = std::move(*selection);
	request.output_descriptors = {
		cxxlens::cc::relations::call_direct_target::descriptor(),
		cxxlens::cc::relations::call_site::descriptor(),
		cxxlens::cc::relations::entity::descriptor(),
	};
	request.task_id = "clang22-malformed-input-" + std::string(300U, 'x');
	request.payload = {std::byte{0x01}, std::byte{0x02}};
	request.task_input_digest = content_digest(request.payload);
	request.normalized_invocation_digest =
		"sha256:abababababababababababababababababababababababababababababababab";
	request.toolchain_digest =
		"sha256:cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd";
	request.environment_digest =
		"sha256:efefefefefefefefefefefefefefefefefefefefefefefefefefefefefefefef";
	request.sandbox = {sandbox_assurance::enforced, sandbox_policy.policy_digest()};
	request.budget.wall_ms = 2000U;
	request.budget.cpu_ms = 2000U;
	request.budget.address_space_bytes = provider_address_space_budget;
	request.budget.output_bytes = 8U * 1024U * 1024U;
	request.budget.open_files = 128U;
	request.budget.subprocesses = provider_subprocess_budget;

	auto processes = make_system_provider_process_port();
	require(processes != nullptr, "system process provider is unavailable");
	process_provider_runtime runtime{*processes};
	auto report = runtime.execute(request);
	auto failure = report && !report->frames.empty()
		? decode_task_failed_metadata(report->frames.back().control)
		: result<task_failed_metadata>{unexpected(error{"sdk.test-setup", "terminal", {}})};
	if (!report || report->terminal != "provider.frontend-request-invalid" ||
		report->frames.empty() || !failure)
	{
		std::cerr << "terminal=" << (report ? report->terminal : report.error().code)
				  << " frames=" << (report ? report->frames.size() : 0U);
		if (!failure)
			std::cerr << " failure=" << failure.error().code << ':' << failure.error().field << ':'
					  << failure.error().detail;
		std::cerr << '\n';
	}
	require(report && report->terminal == "provider.frontend-request-invalid" &&
				report->frames.front().type == message_type::hello &&
				report->frames.back().type == message_type::task_failed && failure &&
				failure->task_id == request.task_id && failure->error_field == "payload",
			"Clang 22 worker did not use the validated provider protocol");
}
