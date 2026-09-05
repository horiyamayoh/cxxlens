#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <source_location>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
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

#include "llvm/clang23_gcc_replay/clangcl_worker_command_internal.hpp"
#include "llvm/clang23_gcc_replay/provider_worker.hpp"
#include "llvm/clang23_gcc_replay/replay_frontend_authority.hpp"
#include "msvc_worker/msvc_capture_bundle.hpp"
#include "runtime/detached_application_materialization_file_service_internal.hpp"
#include "sdk/application_analysis_internal.hpp"
#include "sdk/application_materialization_execution_internal.hpp"
#include "sdk/detached_provider_run_internal.hpp"

namespace
{
	template <class value_type>
	void require(const value_type& condition,
				 const std::source_location location = std::source_location::current())
	{
		if (!static_cast<bool>(condition))
		{
			std::cerr << location.file_name() << ':' << location.line() << ": requirement failed\n";
			std::abort();
		}
	}

	[[nodiscard]] std::string digest(const char digit)
	{
		return "sha256:" + std::string(64U, digit);
	}

	[[nodiscard]] std::string plain_digest(const std::string_view value)
	{
		return cxxlens::sdk::content_digest(std::as_bytes(std::span{value.data(), value.size()}));
	}

	[[nodiscard]] std::vector<std::byte> bytes(const std::string_view value)
	{
		const auto raw = std::as_bytes(std::span{value.data(), value.size()});
		return {raw.begin(), raw.end()};
	}

	[[nodiscard]] std::string text(const std::span<const std::byte> value)
	{
		std::string output;
		output.reserve(value.size());
		std::ranges::transform(value,
							   std::back_inserter(output),
							   [](const std::byte byte)
							   {
								   return static_cast<char>(std::to_integer<unsigned char>(byte));
							   });
		return output;
	}

	class in_process_clangcl_child final
		: public cxxlens::detail::clang23_gcc_replay::clangcl_sandbox_process_port
	{
	  public:
		[[nodiscard]] cxxlens::sdk::result<std::vector<std::byte>>
		execute(const std::span<const std::byte> host_transcript,
				const cxxlens::detail::clang23_gcc_replay::provider_worker_authority& authority,
				const cxxlens::sdk::import_limits limits) const override
		{
			std::istringstream input{text(host_transcript)};
			auto result =
				cxxlens::detail::clang23_gcc_replay::run_provider_worker(input, authority, limits);
			if (!result)
				return cxxlens::sdk::unexpected(std::move(result.error()));
			return std::move(result->protocol_transcript);
		}
	};

	[[nodiscard]] std::byte hex_nibble(const char value)
	{
		if (value >= '0' && value <= '9')
			return static_cast<std::byte>(value - '0');
		if (value >= 'a' && value <= 'f')
			return static_cast<std::byte>(value - 'a' + 10);
		require(false);
		return {};
	}

	template <std::size_t Size>
	[[nodiscard]] std::array<std::byte, Size> hex_bytes(const std::string_view value)
	{
		require(value.size() == Size * 2U);
		std::array<std::byte, Size> output{};
		for (std::size_t index{}; index < output.size(); ++index)
			output[index] =
				(hex_nibble(value[index * 2U]) << 4U) | hex_nibble(value[index * 2U + 1U]);
		return output;
	}

	class temporary_directory
	{
	  public:
		temporary_directory()
		{
			std::random_device random;
			for (std::size_t attempt{}; attempt < 32U; ++attempt)
			{
				path_ = std::filesystem::temp_directory_path() /
					("cxxlens msvc detached e2e " + std::to_string(random()) + '-' +
					 std::to_string(random()));
				std::error_code error;
				if (std::filesystem::create_directory(path_, error))
					return;
				require(!error || error == std::errc::file_exists);
			}
			require(false);
		}

		~temporary_directory()
		{
			std::error_code ignored;
			std::filesystem::remove_all(path_, ignored);
		}

		[[nodiscard]] const std::filesystem::path& path() const noexcept
		{
			return path_;
		}

	  private:
		std::filesystem::path path_;
	};

	void write(const std::filesystem::path& path, const std::span<const std::byte> value)
	{
		std::ofstream output{path, std::ios::binary | std::ios::trunc};
		output.write(reinterpret_cast<const char*>(value.data()),
					 static_cast<std::streamsize>(value.size()));
		require(static_cast<bool>(output));
	}

	[[nodiscard]] cxxlens::sdk::result<std::size_t>
	query_rows(const cxxlens::sdk::snapshot_handle& snapshot,
			   const cxxlens::sdk::relation_descriptor& descriptor,
			   const bool require_unresolved)
	{
		using namespace cxxlens::sdk;
		auto logical = query::builder::from(descriptor);
		if (!logical)
			return unexpected(std::move(logical.error()));
		auto runtime = query::reference_engine::bind(snapshot);
		if (!runtime)
			return unexpected(std::move(runtime.error()));
		auto result = runtime->execute(std::move(*logical).finish());
		if (!result)
			return unexpected(std::move(result.error()));
		if (result->execution() != query::execution_status::complete ||
			(require_unresolved && result->unresolved_items().empty()))
			return error{"application-analysis.msvc-e2e-query-invalid",
						 "query",
						 "partial replay must remain query-visible as unresolved"};
		auto rows = result->rows();
		std::size_t count{};
		while (true)
		{
			auto next = rows.next();
			if (!next)
				return unexpected(std::move(next.error()));
			if (!*next)
				return count;
			if (!(*next)->copy())
				return error{"application-analysis.msvc-e2e-query-invalid",
							 "query",
							 "materialized row is invalid"};
			++count;
		}
	}
} // namespace

int main()
{
	using namespace cxxlens;
	using namespace cxxlens::application_analysis_worker;
	using namespace cxxlens::detail::clang23_gcc_replay;
	using namespace cxxlens::sdk;
	using namespace cxxlens::sdk::detail;
	using namespace cxxlens::sdk::provider;

	msvc_capture_input capture;
	capture.project_id = "project:msvc-detached-e2e";
	capture.canonical_project_root = "C:\\workspace\\msvc-detached-e2e";
	capture.canonical_working_directory = "C:\\workspace\\msvc-detached-e2e\\build";
	capture.canonical_compiler_path =
		"C:\\VS\\VC\\Tools\\MSVC\\14.51.36231\\bin\\Hostx64\\x64\\cl.exe";
	capture.compiler_binary_digest = digest('1');
	capture.windows_sdk_root = "C:\\Program Files (x86)\\Windows Kits\\10";
	capture.abi_digest = digest('2');
	capture.builtin_headers_digest = digest('3');
	capture.builtin_macros_digest = digest('4');
	capture.include_search_digest = digest('5');
	capture.original_arguments = {capture.canonical_compiler_path,
								  "@C:\\workspace\\msvc-detached-e2e\\build\\options.rsp",
								  "C:\\workspace\\msvc-detached-e2e\\src\\main.cpp",
								  "/c"};
	capture.main_source = {"C:\\workspace\\msvc-detached-e2e\\src\\main.cpp",
						   bytes("#include \"model.hpp\"\nint main(){return model();}\n"),
						   "main",
						   "utf8"};
	capture.dependency_sources = {{"C:\\workspace\\msvc-detached-e2e\\include\\model.hpp",
								   bytes("inline int model(){return 23;}\n"),
								   "header",
								   "utf8"}};
	capture.response_files = {
		{"C:\\workspace\\msvc-detached-e2e\\build\\options.rsp",
		 bytes("/nologo /std:c++latest /IC:\\workspace\\msvc-detached-e2e\\include"),
		 std::nullopt}};
	auto encoded_capture = encode_msvc_capture_bundle(capture);
	require(encoded_capture);
	auto bundle = decode_capture_bundle(*encoded_capture);
	require(bundle);
	auto project = import_capture(*bundle);
	require(project && project->replay_plans().size() == 1U &&
			project->replay_plans().front().analysis_frontend() == msvc_replay_frontend_id);

	const std::array descriptors{
		&build::relations::project::descriptor(),
		&build::relations::compile_unit::descriptor(),
		&build::relations::variant::descriptor(),
		&build::relations::toolchain_context::descriptor(),
		&source::relations::file::descriptor(),
		&source::relations::span::descriptor(),
		&cc::relations::entity::descriptor(),
		&cc::relations::declaration::descriptor(),
		&cc::relations::type::descriptor(),
		&cc::relations::call_site::descriptor(),
		&cc::relations::call_direct_target::descriptor(),
	};
	relation_registry registry;
	std::vector<std::string> relation_ids;
	for (const auto* descriptor : descriptors)
	{
		require(registry.add(*descriptor));
		relation_ids.push_back(descriptor->id);
	}
	std::ranges::sort(relation_ids);
	auto engine = registry.build("application-analysis-msvc-detached-e2e");
	require(engine);
	const auto policies = builtin_sandbox_policies();
	const auto policy_iterator =
		std::ranges::find(policies,
						  std::string_view{"cxxlens.sandbox.windows-clangcl-appcontainer"},
						  &sandbox_policy::id);
	require(policy_iterator != policies.end());
	const auto& policy = *policy_iterator;

	manifest manifest_value;
	manifest_value.provider_id = std::string{msvc_provider_id};
	manifest_value.provider_version = msvc_provider_version;
	manifest_value.package_identity = "cxxlens.clangcl23-msvc-replay.package";
	manifest_value.publisher = "cxxlens.project";
	manifest_value.license = "Apache-2.0 WITH LLVM-exception";
	manifest_value.signature = digest('f');
	manifest_value.protocol = {protocol_v2_major,
							   protocol_v2_minor,
							   protocol_v2_minor,
							   {"credit-backpressure", "task-input-chunks-v2"},
							   {}};
	manifest_value.platform_tuples = {"windows-x86_64-clangcl23"};
	manifest_value.provider_binary_digest = digest('a');
	manifest_value.provider_semantic_contract_digest =
		plain_digest("cxxlens.clangcl23-msvc-replay-provider.v1\nclang-cl-23.1.0-msvc-mode");
	manifest_value.offered_relations = relation_ids;
	manifest_value.interpretation_domains = {"cc.clangcl23-msvc-replay-1"};
	manifest_value.invalidation_contract = digest('c');
	manifest_value.determinism_contract = digest('d');
	manifest_value.resource_class = "provider.application-analysis";
	manifest_value.requested_qualifications = {"experimental"};
	require(manifest_value.validate());
	provider_candidate candidate{manifest_value,
								 provider::discovery_source::explicit_path,
								 {},
								 true,
								 true,
								 true,
								 {"experimental"},
								 {"detached-functional-test",
								  policy.mechanisms,
								  sandbox_assurance::enforced,
								  policy.policy_digest(),
								  digest('e')},
								 {}};
	provider_selection_request selection_request{
		manifest_value.provider_id,
		manifest_value.provider_version,
		manifest_value.provider_binary_digest,
		manifest_value.provider_semantic_contract_digest,
		{sandbox_assurance::enforced, policy.policy_digest()},
		true,
		std::nullopt};
	auto selection = select_provider(selection_request, {&candidate, 1U});
	require(selection);
	snapshot_draft publication{{"catalog:msvc-detached-e2e",
								"experimental",
								std::string{engine->generation()},
								"condition:msvc-detached-e2e",
								std::string{engine->registry_digest()},
								digest('6'),
								digest('7')},
							   {1U, 0U, 0U},
							   std::string{project->catalog_semantic_digest()},
							   std::nullopt};
	const auto& imported = application_analysis_imported_value_internal(*project);
	auto plan = make_application_materialization_execution_plan(
		imported,
		*engine,
		publication,
		relation_ids,
		"cc.clangcl23-msvc-replay-1",
		*selection,
		{},
		{},
		{},
		application_materialization_execution_transport::detached);
	require(plan && plan->units.size() == 1U);
	const auto& unit = plan->units.front();
	host_transcript_expectation expectation{manifest_value.canonical_json(),
											{unit.process.task_id,
											 unit.process.task_input_digest,
											 unit.process.normalized_invocation_digest,
											 unit.process.toolchain_digest,
											 unit.process.environment_digest},
											unit.process.limits};
	auto host =
		encode_host_transcript({expectation, unit.process.output_credit, unit.process.payload});
	require(host);

	const temporary_directory directory;
	const auto private_key = hex_bytes<32U>("9d61b19deffd5a60ba844af492ec2cc4"
											"4449c5697b326919703bac031cae7f60");
	const auto public_key = hex_bytes<32U>("d75a980182b10ab7d54bfed3c964073a"
										   "0ee172f3daa62325af021a68f707511a");
	const auto private_path = directory.path() / "worker private key.raw";
	const auto public_path = directory.path() / "worker public key.raw";
	const auto run_path = directory.path() / "detached provider run.bin";
	write(private_path, private_key);
	write(public_path, public_key);
	clangcl_worker_launch_configuration launch{manifest_value.canonical_json(),
											   manifest_value.provider_id,
											   manifest_value.provider_binary_digest,
											   manifest_value.provider_semantic_contract_digest,
											   policy.policy_digest(),
											   unit.process.task_id,
											   unit.process.task_input_digest,
											   unit.process.normalized_invocation_digest,
											   unit.process.toolchain_digest,
											   unit.process.environment_digest,
											   "2",
											   "0",
											   *manifest_value.signature,
											   "not-revoked",
											   "worker:clangcl23-msvc-detached-e2e",
											   private_path.string(),
											   public_path.string()};
	std::istringstream worker_input{text(*host)};
	std::ostringstream worker_output;
	const in_process_clangcl_child child;
	auto executed = execute_clangcl_worker_command(worker_input, worker_output, launch, child);
	if (!executed)
		std::cerr << executed.error().code << ':' << executed.error().field << ':'
				  << executed.error().detail << '\n';
	require(executed);
	std::istringstream repeated_worker_input{text(*host)};
	std::ostringstream repeated_worker_output;
	auto repeated = execute_clangcl_worker_command(
		repeated_worker_input, repeated_worker_output, std::move(launch), child);
	require(repeated && repeated_worker_output.str() == worker_output.str());
	const auto run_bytes = bytes(worker_output.str());
	auto detached_run = decode_detached_provider_run(run_bytes);
	require(detached_run && detached_run->value().provider.provider_id == msvc_provider_id &&
			detached_run->value().task_id == unit.process.task_id);
	write(run_path, run_bytes);

	auto request = materialization_request::make(*engine,
												 publication,
												 relation_ids,
												 "cc.clangcl23-msvc-replay-1",
												 selection_request,
												 {candidate});
	require(request);
	auto store = make_in_memory_snapshot_store(*engine);
	require(store);
	runtime::detached_application_materialization_file_request files{
		{run_path.string()},
		"worker:clangcl23-msvc-detached-e2e",
		public_path.string(),
		detached_run_public_key_state::trusted,
		{}};
	auto foreign_files = files;
	foreign_files.signer_id = "worker:foreign";
	auto rejected_store = make_in_memory_snapshot_store(*engine);
	require(rejected_store);
	auto rejected = runtime::materialize_detached_application_from_files(
		*rejected_store, *project, *request, foreign_files);
	require(!rejected && !rejected_store->current(publication.series));
	auto result =
		runtime::materialize_detached_application_from_files(*store, *project, *request, files);
	if (!result)
		std::cerr << result.error().code << ':' << result.error().field << ':'
				  << result.error().detail << '\n';
	require(result &&
			result->terminal() == cxxlens::sdk::materialization_terminal::published_partial &&
			result->published_snapshot() && result->provenance());
	require(result->provenance()->provider_id == msvc_provider_id &&
			result->provenance()->provider_binary_digest == manifest_value.provider_binary_digest &&
			!result->unresolved().empty());
	auto source_rows =
		query_rows(*result->published_snapshot(), source::relations::file::descriptor(), false);
	auto declaration_rows =
		query_rows(*result->published_snapshot(), cc::relations::declaration::descriptor(), true);
	if (!source_rows)
		std::cerr << source_rows.error().code << ':' << source_rows.error().field << ':'
				  << source_rows.error().detail << '\n';
	if (!declaration_rows)
		std::cerr << declaration_rows.error().code << ':' << declaration_rows.error().field << ':'
				  << declaration_rows.error().detail << '\n';
	require(source_rows && *source_rows >= 2U && declaration_rows && *declaration_rows >= 2U);
	return EXIT_SUCCESS;
}
