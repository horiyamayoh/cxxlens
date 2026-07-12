#include "workspace/provisioning.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <ranges>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::detail;
	using namespace cxxlens::detail::scheduling;

	void require(const bool condition, const std::string& message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	void write(const std::filesystem::path& file, const std::string& content)
	{
		std::ofstream output{file};
		output << content;
		require(output.good(), "provisioning fixture write failed");
	}

	[[nodiscard]] std::string read(const std::filesystem::path& file)
	{
		std::ifstream input{file};
		return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
	}

	struct fixture
	{
		std::filesystem::path root;
		workspace value;
	};

	[[nodiscard]] fixture make_fixture(const std::filesystem::path& root,
									   const bool persistent = false,
									   const bool configured = false,
									   const bool reverse_database = false)
	{
		std::filesystem::remove_all(root);
		std::filesystem::create_directories(root);
		write(root / "multi.cpp", "int multi(){return 1;}\n");
		write(root / "other.cpp", "int other(){return 2;}\n");
		const auto directory = root.generic_string();
		const auto multi = (root / "multi.cpp").generic_string();
		const auto other = (root / "other.cpp").generic_string();
		std::vector<std::string> entries{
			"{\"directory\":\"" + directory + "\",\"file\":\"" + multi +
				"\",\"arguments\":[\"clang++\",\"-DONE=1\",\"" + multi + "\"]}",
			"{\"directory\":\"" + directory + "\",\"file\":\"" + multi +
				"\",\"arguments\":[\"clang++\",\"-DONE=2\",\"" + multi + "\"]}",
			"{\"directory\":\"" + directory + "\",\"file\":\"" + other +
				"\",\"arguments\":[\"clang++\",\"-std=c++23\",\"" + other + "\"]}"};
		if (reverse_database)
			std::ranges::reverse(entries);
		write(root / "compile_commands.json",
			  "[" + entries[0] + "," + entries[1] + "," + entries[2] + "]");
		auto options = workspace_options::from_compilation_database(root / "compile_commands.json");
		options.project_root = root;
		if (configured)
		{
			write(root / ".cxxlens.yaml", "profile: one\n");
			options.configuration_file = root / ".cxxlens.yaml";
		}
		if (persistent)
		{
			std::filesystem::create_directories(root / "cache");
			options.cache_directory = root / "cache";
		}
		auto opened = workspace::open(std::move(options));
		require(opened.has_value(), "provisioning workspace did not open");
		return {root, std::move(opened.value())};
	}

	[[nodiscard]] std::string observation_key(const facts::observation_record& value)
	{
		return std::to_string(static_cast<std::uint16_t>(value.kind)) + ":" +
			value.payload.at("semantic_key");
	}

	class fake_worker final : public worker_port
	{
	  public:
		std::atomic<std::size_t> calls{};
		std::set<std::string> failures;
		bool invalid_batch{};
		std::stop_source* cancellation_to_request{};

		result<frontend::observation_batch> execute(const task_request& task,
													execution_context) override
		{
			++calls;
			if (cancellation_to_request != nullptr)
				cancellation_to_request->request_stop();
			const auto unit = std::string{task.parse.unit.id().value()};
			if (failures.contains(unit))
			{
				error failure;
				failure.code.value = "parse.frontend-failed";
				failure.message = "injected parse failure";
				failure.scope = failure_scope::compile_unit;
				return failure;
			}
			frontend::observation_batch batch;
			batch.adapter_id = invalid_batch ? "invalid.adapter" : "clang22.frontend";
			batch.adapter_version = "provisioning-test";
			batch.unit = task.parse.unit.id();
			batch.variant = task.parse.unit.variant_id();
			batch.coverage.parsed = 1U;
			constexpr std::array supported{
				fact_kind::file,
				fact_kind::symbol,
				fact_kind::type,
				fact_kind::declaration,
				fact_kind::definition,
				fact_kind::reference,
				fact_kind::call,
				fact_kind::inheritance,
				fact_kind::override_relation,
				fact_kind::include_relation,
				fact_kind::macro_definition,
				fact_kind::macro_expansion,
			};
			for (const auto kind : supported)
			{
				facts::observation_record observation;
				observation.adapter_id = batch.adapter_id;
				observation.adapter_version = batch.adapter_version;
				observation.llvm_major = 22U;
				observation.compile_unit = batch.unit;
				observation.variant = batch.variant;
				observation.kind = kind;
				observation.payload_version = 1U;
				const auto key =
					"fixture:" + std::to_string(static_cast<std::uint16_t>(kind)) + ":" + unit;
				observation.payload = {{"domain.value", unit},
									   {"extractor.id", "provisioning-fixture"},
									   {"extractor.version", "1.0.0"},
									   {"semantic_key", key}};
				observation.coverage_contributions.push_back(
					{"fixture", key, coverage_state::covered, std::nullopt});
				batch.observations.push_back(std::move(observation));
			}
			std::ranges::sort(batch.observations, {}, observation_key);
			return batch;
		}
	};

	void bind_worker(workspace& value, const std::shared_ptr<fake_worker>& worker)
	{
		workspace_provisioning_access::set_worker(value, worker);
	}

	[[nodiscard]] execution_context execution(const std::size_t jobs)
	{
		execution_context output;
		output.parallelism = jobs;
		return output;
	}

	void exact_delta_warm_and_partial(fixture& fixture)
	{
		auto worker = std::make_shared<fake_worker>();
		bind_worker(fixture.value, worker);
		auto multi_scope = analysis_scope::files({"multi.cpp"});
		auto first = fixture.value.ensure(fact_profile::minimal(), multi_scope, execution(8U));
		require(first.has_value() && worker->calls == 2U,
				"first ensure did not schedule both requested variants exactly once");
		auto first_coverage = fixture.value.facts().coverage(fact_profile::minimal(), multi_scope);
		require(first_coverage.validate() && first_coverage.complete() &&
					first_coverage.requested().size() == 4U,
				"TU/variant/fact requirement coverage was not exact");
		auto exact_facts = fixture.value.facts().find(fact_query::all());
		require(exact_facts &&
					std::ranges::all_of(exact_facts.value(),
										[](const fact& value)
										{
											return value.kind() == fact_kind::file ||
												value.kind() == fact_kind::compile_command;
										}),
				"first ensure exposed facts outside the exact requested profile");
		require(fixture.value.facts()
						.coverage(fact_profile::minimal(),
								  analysis_scope::files({fixture.root / "multi.cpp"}))
						.requested()
						.size() == 4U,
				"absolute file scope did not preserve exact fact coverage");
		const auto one_unit = fixture.value.command_for("multi.cpp").front();
		require(fixture.value.facts()
							.coverage(fact_profile::minimal(),
									  analysis_scope::compile_units({one_unit.id()}))
							.requested()
							.size() == 2U &&
					fixture.value.facts()
							.coverage(fact_profile::minimal(),
									  multi_scope.variants({one_unit.variant_id()}))
							.requested()
							.size() == 2U,
				"compile-unit or variant coverage scope broadened silently");
		const auto first_json = fixture.value.facts().to_json();
		const auto first_coverage_json = first_coverage.to_json();

		auto repeated = fixture.value.ensure(fact_profile::minimal(), multi_scope, execution(1U));
		require(repeated.has_value() && worker->calls == 2U, "warm ensure scheduled frontend work");
		require(
			fixture.value.facts().to_json() == first_json &&
				fixture.value.facts().coverage(fact_profile::minimal(), multi_scope).to_json() ==
					first_coverage_json,
			"warm ensure changed facts or coverage");
		auto warm_trace = workspace_provisioning_access::last_trace(fixture.value);
		require(warm_trace.validate() && warm_trace.scheduled == 0U && warm_trace.warm == 4U,
				"warm coverage delta trace was not explicit");

		auto expanded = fixture.value.ensure(
			fact_profile::minimal().include(fact_kind::symbol), multi_scope, execution(2U));
		require(expanded.has_value() && worker->calls == 2U,
				"retained extractor batches were not reused for a new fact kind");
		require(fixture.value.facts()
						.coverage(fact_profile::minimal().include(fact_kind::symbol), multi_scope)
						.requested()
						.size() == 6U,
				"new fact-kind requirement closure was incomplete");
		const auto precise_profile = fact_profile::minimal()
										 .include(fact_kind::symbol)
										 .precision(precision_level::local_semantic);
		require(
			fixture.value.ensure(precise_profile, multi_scope).has_value() && worker->calls == 2U &&
				fixture.value.facts().coverage(precise_profile, multi_scope).requested().size() ==
					6U,
			"precision-specific coverage was broadened or reparsed implicitly");

		write(fixture.root / "multi.cpp", "int multi(){return 7;}\n");
		const auto failed_unit = fixture.value.command_for("multi.cpp").front().id();
		worker->failures.insert(std::string{failed_unit.value()});
		auto partial = fixture.value.ensure(
			fact_profile::minimal().include(fact_kind::symbol), multi_scope, execution(8U));
		require(partial.has_value() && worker->calls == 4U,
				"source change did not invalidate only its two variant tasks");
		auto partial_coverage = fixture.value.facts().coverage(
			fact_profile::minimal().include(fact_kind::symbol), multi_scope);
		require(partial_coverage.validate() &&
					partial_coverage.count(coverage_state::failed) == 2U &&
					partial_coverage.count(coverage_state::covered) == 4U,
				"one variant failure did not preserve successful facts and explicit coverage");
		auto partial_facts = fixture.value.facts().find(fact_query::all());
		require(partial_facts &&
					std::ranges::none_of(partial_facts.value(),
										 [&](const fact& value)
										 {
											 return (value.kind() == fact_kind::file ||
													 value.kind() == fact_kind::symbol) &&
												 std::ranges::contains(value.origin().compile_units,
																	   failed_unit);
										 }),
				"failed changed variant retained stale semantic facts");
	}

	void cancellation_capability_and_invalid_batch(fixture& fixture)
	{
		auto worker = std::make_shared<fake_worker>();
		bind_worker(fixture.value, worker);
		require(fixture.value.ensure(fact_profile::minimal()).has_value(),
				"baseline ensure failed");
		const auto baseline_facts = fixture.value.facts().to_json();
		const auto baseline_coverage =
			fixture.value.facts().coverage(fact_profile::minimal()).to_json();
		std::stop_source stop;
		stop.request_stop();
		execution_context cancelled_context;
		cancelled_context.cancellation = stop.get_token();
		auto cancelled = fixture.value.ensure(
			fact_profile::semantic_search(), analysis_scope::all(), cancelled_context);
		require(!cancelled && cancelled.error().code.value == "core.cancelled" &&
					fixture.value.facts().to_json() == baseline_facts &&
					fixture.value.facts().coverage(fact_profile::minimal()).to_json() ==
						baseline_coverage,
				"cancellation corrupted the prior immutable snapshot");
		std::stop_source mid_operation_stop;
		worker->cancellation_to_request = &mid_operation_stop;
		write(fixture.root / "other.cpp", "int other(){return 7;}\n");
		execution_context mid_operation_context;
		mid_operation_context.cancellation = mid_operation_stop.get_token();
		auto interrupted = fixture.value.ensure(fact_profile::semantic_search(),
												analysis_scope::files({"other.cpp"}),
												mid_operation_context);
		worker->cancellation_to_request = nullptr;
		require(!interrupted && interrupted.error().code.value == "core.cancelled" &&
					fixture.value.facts().to_json() == baseline_facts,
				"mid-operation cancellation committed a partial snapshot");
		execution_context budget;
		budget.memory_budget_mb = 1U;
		write(fixture.root / "other.cpp", "int other(){return 8;}\n");
		auto exhausted = fixture.value.ensure(
			fact_profile::semantic_search(), analysis_scope::files({"other.cpp"}), budget);
		require(!exhausted && exhausted.error().code.value == "core.budget-exhausted" &&
					fixture.value.facts().to_json() == baseline_facts,
				"budget exhaustion corrupted the prior immutable snapshot");
		execution_context deadline;
		deadline.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds{1};
		auto timed_out = fixture.value.ensure(
			fact_profile::semantic_search(), analysis_scope::files({"other.cpp"}), deadline);
		require(!timed_out && timed_out.error().code.value == "core.deadline-exceeded" &&
					fixture.value.facts().to_json() == baseline_facts,
				"deadline exhaustion corrupted the prior immutable snapshot");

		auto unsupported =
			fixture.value.ensure(fact_profile::minimal().include(fact_kind::cfg_summary));
		require(!unsupported && unsupported.error().code.value == "core.capability-unavailable" &&
					unsupported.error().attributes.at("capability") == "extractor.flow-summary" &&
					unsupported.error().attributes.at("action") ==
						"enable-required-extractor-capability" &&
					fixture.value.facts()
							.coverage(fact_profile::minimal().include(fact_kind::cfg_summary))
							.count(coverage_state::unresolved) ==
						fixture.value.compile_units().size(),
				"unsupported capability became an empty success");

		const auto before_invalid = fixture.value.facts().to_json();
		write(fixture.root / "other.cpp", "int other(){return 9;}\n");
		worker->invalid_batch = true;
		auto invalid =
			fixture.value.ensure(fact_profile::minimal(), analysis_scope::files({"other.cpp"}));
		require(!invalid && invalid.error().code.value == "extractor.invalid-observation" &&
					fixture.value.facts().to_json() == before_invalid,
				"invalid worker/reducer input committed a snapshot");
	}

	void configuration_invalidation(const std::filesystem::path& root)
	{
		auto configured = make_fixture(root, false, true);
		auto worker = std::make_shared<fake_worker>();
		bind_worker(configured.value, worker);
		require(configured.value.ensure(fact_profile::minimal()).has_value() && worker->calls == 3U,
				"configured cold ensure did not provision every variant");
		write(root / ".cxxlens.yaml", "profile: two\n");
		require(configured.value.ensure(fact_profile::minimal()).has_value() &&
					worker->calls == 6U &&
					workspace_provisioning_access::last_trace(configured.value).scheduled == 3U,
				"configuration change did not invalidate exactly the dependent closure");
	}

	void production_pipeline(const std::filesystem::path& root)
	{
		auto production = make_fixture(root);
		const auto profile = fact_profile::semantic_search();
		auto ensured = production.value.ensure(profile, analysis_scope::files({"other.cpp"}));
		const auto coverage =
			production.value.facts().coverage(profile, analysis_scope::files({"other.cpp"}));
		if (cxxlens::capabilities().has("frontend.clang22"))
		{
			require(ensured && coverage.validate() && coverage.complete() &&
						production.value.facts().find(fact_query::all()).value().size() >= 3U,
					"real frontend/extractor/reducer/store provisioning pipeline failed");
			require(production.value.ensure(profile, analysis_scope::files({"other.cpp"})) &&
						workspace_provisioning_access::last_trace(production.value).scheduled == 0U,
					"real frontend warm ensure reparsed an equivalent requirement");
		}
		else
		{
			auto doctor = production.value.doctor();
			require(!ensured && ensured.error().code.value == "core.capability-unavailable" &&
						ensured.error().attributes.at("capability") == "frontend.clang22" &&
						coverage.validate() && coverage.count(coverage_state::unresolved) != 0U &&
						doctor && !doctor.value().healthy() &&
						std::ranges::any_of(doctor.value().diagnostics(),
											[](const diagnostic& value)
											{
												return value.id == "workspace.frontend-unavailable";
											}),
					"unavailable production frontend became empty success");
		}
	}

	void persistence_determinism_and_doctor(const std::filesystem::path& base)
	{
		auto persistent = make_fixture(base / "persistent", true);
		auto first_worker = std::make_shared<fake_worker>();
		bind_worker(persistent.value, first_worker);
		require(persistent.value
					.ensure(fact_profile::semantic_search(), analysis_scope::all(), execution(1U))
					.has_value(),
				"SQLite cold ensure failed");
		const auto expected_facts = persistent.value.facts().to_json();
		const auto expected_coverage =
			persistent.value.facts().coverage(fact_profile::semantic_search()).to_json();
		auto reopened_options =
			workspace_options::from_compilation_database(persistent.root / "compile_commands.json");
		reopened_options.project_root = persistent.root;
		reopened_options.cache_directory = persistent.root / "cache";
		auto reopened = workspace::open(std::move(reopened_options));
		require(reopened.has_value(), "SQLite workspace did not reopen");
		auto warm_worker = std::make_shared<fake_worker>();
		bind_worker(reopened.value(), warm_worker);
		auto warm_result = reopened.value().ensure(
			fact_profile::semantic_search(), analysis_scope::all(), execution(8U));
		const auto warm_facts = reopened.value().facts().to_json();
		const auto warm_coverage =
			reopened.value().facts().coverage(fact_profile::semantic_search()).to_json();
		require(warm_result && warm_worker->calls == 0U && warm_facts == expected_facts &&
					warm_coverage == expected_coverage,
				"SQLite warm reopen did not restore facts and coverage without parsing");

		const auto cache_stamp =
			std::filesystem::last_write_time(persistent.root / "cache/facts.sqlite3");
		auto doctor = reopened.value().doctor();
		auto repeated_doctor = reopened.value().doctor();
		require(doctor && repeated_doctor && doctor.value().healthy() &&
					doctor.value().diagnostics().empty() &&
					doctor.value().to_json() == repeated_doctor.value().to_json() &&
					doctor.value().to_json().find("cxxlens.workspace-doctor.v1") !=
						std::string::npos &&
					std::filesystem::last_write_time(persistent.root / "cache/facts.sqlite3") ==
						cache_stamp,
				"doctor was not stable, structured, and side-effect free");
		require(reopened.value().capabilities().has("workspace.incremental-provisioning") &&
					reopened.value().capabilities().has("facts.sqlite") &&
					reopened.value().capabilities().has("frontend.clang22") ==
						cxxlens::capabilities().has("frontend.clang22"),
				"workspace capability states were incomplete or implicit");
		auto relocated_fixture = make_fixture(base / "sqlite-relocated");
		auto relocated_options = workspace_options::from_compilation_database(
			relocated_fixture.root / "compile_commands.json");
		relocated_options.project_root = relocated_fixture.root;
		relocated_options.cache_directory = persistent.root / "cache";
		auto relocated = workspace::open(std::move(relocated_options));
		require(relocated.has_value(), "relocated SQLite workspace did not open");
		auto relocated_worker = std::make_shared<fake_worker>();
		bind_worker(relocated.value(), relocated_worker);
		require(relocated.value().ensure(fact_profile::semantic_search()).has_value() &&
					relocated_worker->calls == 0U &&
					relocated.value().facts().to_json() == expected_facts &&
					relocated.value().facts().coverage(fact_profile::semantic_search()).to_json() ==
						expected_coverage,
				"root relocation invalidated or changed a compatible SQLite snapshot");

		std::string canonical;
		for (const auto jobs : {1U, 2U, 8U})
		{
			auto run = make_fixture(
				base / ("determinism-" + std::to_string(jobs)), false, false, jobs == 2U);
			auto worker = std::make_shared<fake_worker>();
			bind_worker(run.value, worker);
			require(
				run.value
					.ensure(fact_profile::semantic_search(), analysis_scope::all(), execution(jobs))
					.has_value(),
				"determinism ensure failed");
			const auto projection = run.value.facts().to_json() + "\n" +
				run.value.facts().coverage(fact_profile::semantic_search()).to_json();
			if (canonical.empty())
				canonical = projection;
			else
				require(canonical == projection,
						"jobs 1/2/8 or relocated root changed semantic facts/coverage");
		}
		require(canonical == expected_facts + "\n" + expected_coverage,
				"memory and SQLite backends changed semantic facts/coverage");

		auto changed_database = read(persistent.root / "compile_commands.json");
		const auto command = changed_database.find("-DONE=2");
		require(command != std::string::npos, "command invalidation fixture was malformed");
		changed_database.replace(command, std::string{"-DONE=2"}.size(), "-DTWO=3");
		write(persistent.root / "compile_commands.json", changed_database);
		auto changed_options =
			workspace_options::from_compilation_database(persistent.root / "compile_commands.json");
		changed_options.project_root = persistent.root;
		changed_options.cache_directory = persistent.root / "cache";
		auto changed = workspace::open(std::move(changed_options));
		require(changed.has_value(), "command-changed workspace did not reopen");
		auto changed_worker = std::make_shared<fake_worker>();
		bind_worker(changed.value(), changed_worker);
		require(changed.value().ensure(fact_profile::semantic_search()).has_value() &&
					changed_worker->calls == 1U &&
					changed.value().facts().coverage(fact_profile::semantic_search()).complete(),
				"one command change did not invalidate exactly its variant closure");
	}
} // namespace

int main(const int argument_count, const char* const* arguments)
{
	const auto base = std::filesystem::temp_directory_path() / "cxxlens-provisioning-test";
	std::filesystem::remove_all(base);
	std::filesystem::create_directories(base);
	if (argument_count == 2 && std::string_view{arguments[1]} == "--emit")
	{
		auto emitted = make_fixture(base / "emit");
		auto worker = std::make_shared<fake_worker>();
		bind_worker(emitted.value, worker);
		require(emitted.value.ensure(fact_profile::minimal()).has_value(),
				"provisioning schema emission failed");
		std::cout << workspace_provisioning_access::last_trace(emitted.value).to_json() << '\n';
		std::cout << emitted.value.capabilities().to_json() << '\n';
		std::cout << emitted.value.doctor().value().to_json() << '\n';
		std::filesystem::remove_all(base);
		return 0;
	}
	auto delta = make_fixture(base / "delta");
	exact_delta_warm_and_partial(delta);
	auto controls = make_fixture(base / "controls");
	cancellation_capability_and_invalid_batch(controls);
	configuration_invalidation(base / "configuration");
	production_pipeline(base / "production");
	persistence_determinism_and_doctor(base);
	std::filesystem::remove_all(base);
	return 0;
}
