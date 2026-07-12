#include "provisioning.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <map>
#include <mutex>
#include <ranges>
#include <set>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>

#include "../core/canonical_json.hpp"
#include "../facts/reducer.hpp"
#include "../runtime/filesystem_port.hpp"
#include "../runtime/hash_port.hpp"
#include "../runtime/time_port.hpp"
#include "../store/fact_store_access.hpp"
#include "../store/store_port.hpp"
#include "frontend_scheduler_worker.hpp"
#include "value_access.hpp"

namespace cxxlens::detail::provisioning
{
	namespace
	{
		using json::json_value;

		struct requirement
		{
			std::string logical_key;
			std::string id;
			std::string file;
			compile_unit unit;
			fact_kind kind{fact_kind::custom};
			precision_level precision{precision_level::ast_structural};
			std::string input_digest;
		};

		struct requirement_state
		{
			requirement value;
			coverage_state state{coverage_state::unresolved};
			std::optional<std::string> reason;
		};

		struct cached_batch
		{
			std::string input_digest;
			frontend::observation_batch value;
		};

		[[nodiscard]] error provisioning_error(std::string code,
											   std::string reason,
											   failure_scope scope = failure_scope::operation)
		{
			error failure;
			failure.code.value = std::move(code);
			failure.message = "Workspace fact provisioning failed";
			failure.scope = scope;
			failure.attributes.emplace("reason", std::move(reason));
			return failure;
		}

		[[nodiscard]] std::string semantic_path(const path& root, path value)
		{
			if (value.is_relative())
				value = root / value;
			value = value.lexically_normal();
			auto relative = value.lexically_relative(root.lexically_normal());
			if (!relative.empty() && !relative.generic_string().starts_with(".."))
				return relative.generic_string();
			return value.filename().generic_string();
		}

		[[nodiscard]] std::string hex_encode(const std::string_view input)
		{
			constexpr std::array digits{
				'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
			std::string output;
			output.reserve(input.size() * 2U);
			for (const auto character : input)
			{
				const auto byte = static_cast<unsigned char>(character);
				output.push_back(digits.at(byte >> 4U));
				output.push_back(digits.at(byte & 0x0FU));
			}
			return output;
		}

		[[nodiscard]] std::optional<std::string> hex_decode(const std::string_view input)
		{
			if ((input.size() % 2U) != 0U)
				return std::nullopt;
			auto nibble = [](const char value) -> std::optional<unsigned char>
			{
				if (value >= '0' && value <= '9')
					return static_cast<unsigned char>(value - '0');
				if (value >= 'a' && value <= 'f')
					return static_cast<unsigned char>(value - 'a' + 10);
				return std::nullopt;
			};
			std::string output;
			output.reserve(input.size() / 2U);
			for (std::size_t index = 0U; index < input.size(); index += 2U)
			{
				auto high = nibble(input[index]);
				auto low = nibble(input[index + 1U]);
				if (!high || !low)
					return std::nullopt;
				output.push_back(static_cast<char>((*high << 4U) | *low));
			}
			return output;
		}

		[[nodiscard]] std::map<std::string, std::string> parse_fields(const std::string_view id)
		{
			std::map<std::string, std::string> fields;
			std::size_t begin{};
			while (begin <= id.size())
			{
				const auto end = id.find(';', begin);
				const auto field = id.substr(
					begin, end == std::string_view::npos ? id.size() - begin : end - begin);
				const auto separator = field.find('=');
				if (separator != std::string_view::npos)
					fields.emplace(std::string{field.substr(0U, separator)},
								   std::string{field.substr(separator + 1U)});
				if (end == std::string_view::npos)
					break;
				begin = end + 1U;
			}
			return fields;
		}

		[[nodiscard]] std::string
		logical_key(const compile_unit& unit, const fact_kind kind, const precision_level precision)
		{
			return "u=" + std::string{unit.id().value()} +
				";v=" + std::string{unit.variant_id().value()} +
				";k=" + std::to_string(static_cast<std::uint16_t>(kind)) +
				";p=" + std::to_string(static_cast<std::uint8_t>(precision));
		}

		[[nodiscard]] requirement make_requirement(const path& root,
												   const compile_unit& unit,
												   const fact_kind kind,
												   const precision_level precision,
												   std::string input_digest)
		{
			requirement output;
			output.file = semantic_path(root, unit.command().file);
			output.unit = unit;
			output.kind = kind;
			output.precision = precision;
			output.input_digest = std::move(input_digest);
			output.logical_key = logical_key(unit, kind, precision);
			output.id = "f=" + hex_encode(output.file) + ";" + output.logical_key +
				";i=" + output.input_digest;
			return output;
		}

		[[nodiscard]] bool parse_supported(const fact_kind kind,
										   const precision_level precision) noexcept
		{
			if (precision > precision_level::workspace_semantic)
				return false;
			switch (kind)
			{
				case fact_kind::file:
				case fact_kind::symbol:
				case fact_kind::type:
				case fact_kind::declaration:
				case fact_kind::definition:
				case fact_kind::reference:
				case fact_kind::call:
				case fact_kind::inheritance:
				case fact_kind::override_relation:
				case fact_kind::include_relation:
				case fact_kind::macro_definition:
				case fact_kind::macro_expansion:
					return true;
				case fact_kind::compile_command:
				case fact_kind::construction:
				case fact_kind::conversion:
				case fact_kind::cfg_summary:
				case fact_kind::flow_summary:
				case fact_kind::effect_summary:
				case fact_kind::dynamic_observation:
				case fact_kind::coverage_region:
				case fact_kind::custom:
					return false;
			}
			return false;
		}

		[[nodiscard]] std::string missing_capability(const fact_kind kind,
													 const precision_level precision)
		{
			if (precision > precision_level::workspace_semantic)
				return "precision." + std::to_string(static_cast<std::uint8_t>(precision));
			switch (kind)
			{
				case fact_kind::construction:
				case fact_kind::conversion:
					return "extractor.extended-expression";
				case fact_kind::cfg_summary:
				case fact_kind::flow_summary:
				case fact_kind::effect_summary:
					return "extractor.flow-summary";
				case fact_kind::dynamic_observation:
					return "extractor.dynamic-observation";
				case fact_kind::coverage_region:
					return "extractor.coverage-region";
				case fact_kind::custom:
					return "extractor.custom";
				default:
					return "frontend.clang22";
			}
		}

		[[nodiscard]] std::string observation_key(const facts::observation_record& value)
		{
			const auto semantic = value.payload.find("semantic_key");
			return std::to_string(static_cast<std::uint16_t>(value.kind)) + ":" +
				(semantic == value.payload.end() ? std::string{} : semantic->second);
		}

		[[nodiscard]] facts::observation_record
		compile_command_observation(const compile_unit& unit,
									const std::string_view adapter_version)
		{
			facts::observation_record output;
			output.adapter_id = "clang22.frontend";
			output.adapter_version = adapter_version;
			output.llvm_major = 22U;
			output.compile_unit = unit.id();
			output.variant = unit.variant_id();
			output.kind = fact_kind::compile_command;
			output.payload_version = 1U;
			output.payload = {{"command.digest", unit.command_digest()},
							  {"extractor.id", "workspace-catalog"},
							  {"extractor.version", "1.0.0"},
							  {"semantic_key", "compile-command:" + std::string{unit.id().value()}},
							  {"variant", std::string{unit.variant_id().value()}}};
			output.coverage_contributions.push_back({"compile_command",
													 output.payload.at("semantic_key"),
													 coverage_state::covered,
													 std::nullopt});
			return output;
		}

		[[nodiscard]] bool fact_less(const facts::detached_fact_record& left,
									 const facts::detached_fact_record& right)
		{
			return std::tuple{left.kind, left.stable_key, left.id.value()} <
				std::tuple{right.kind, right.stable_key, right.id.value()};
		}

		void sort_unique(std::vector<compile_unit_id>& values)
		{
			std::ranges::sort(values,
							  {},
							  [](const auto& value)
							  {
								  return value.value();
							  });
			values.erase(std::ranges::unique(values).begin(), values.end());
		}

		void sort_unique(std::vector<build_variant_id>& values)
		{
			std::ranges::sort(values,
							  {},
							  [](const auto& value)
							  {
								  return value.value();
							  });
			values.erase(std::ranges::unique(values).begin(), values.end());
		}

		[[nodiscard]] coverage_report
		make_coverage(const std::map<std::string, requirement_state>& states)
		{
			coverage_report output;
			for (const auto& [unused, value] : states)
			{
				(void)unused;
				output.request({"fact-unit", value.value.id});
				output.classify({"fact-unit", value.value.id, value.state, value.reason});
			}
			return output;
		}
	} // namespace

	struct provisioning_service::implementation
	{
		service_options options;
		store::snapshot_metadata metadata;
		std::shared_ptr<store::fact_store_port> store;
		std::shared_ptr<const store::snapshot_data> snapshot;
		std::shared_ptr<scheduling::worker_port> worker;
		bool worker_requires_clang{true};
		std::map<std::string, requirement_state> requirements;
		std::map<std::string, cached_batch> batches;
		provisioning_trace trace;
		mutable std::mutex state_mutex;
		mutable std::mutex operation_mutex;
	};

	result<void> provisioning_trace::validate() const
	{
		if (schema != "cxxlens.provisioning-trace.v1" ||
			requested != covered + failed + unresolved || warm > requested ||
			scheduled > requested || rows.size() != requested || !std::ranges::is_sorted(rows))
			return provisioning_error("core.schema-validation-failed", "provisioning-trace");
		return {};
	}

	std::string provisioning_trace::to_json() const
	{
		json_value::array values;
		for (const auto& row : rows)
			values.emplace_back(json_value::object{{"reason_code", row.reason_code},
												   {"requirement_id", row.requirement_id},
												   {"state", row.state}});
		auto encoded = json::write(json_value{json::envelope({"cxxlens.provisioning-trace.v1"},
															 {{"covered", covered},
															  {"failed", failed},
															  {"requested", requested},
															  {"rows", std::move(values)},
															  {"scheduled", scheduled},
															  {"unresolved", unresolved},
															  {"warm", warm}})});
		return encoded ? std::move(encoded.value()) : std::string{};
	}

	provisioning_service::provisioning_service(std::unique_ptr<implementation> value)
		: implementation_{std::move(value)}
	{
	}

	result<std::shared_ptr<provisioning_service>>
	provisioning_service::create(service_options options,
								 std::shared_ptr<scheduling::worker_port> worker)
	{
		if (options.root.empty() || options.workspace_key.empty())
			return provisioning_error("core.invalid-argument", "service-options");
		if (!options.files)
			options.files = std::make_shared<runtime::standard_filesystem_adapter>();
		auto value = std::make_unique<implementation>();
		value->options = std::move(options);
		value->metadata.workspace_key = value->options.workspace_key;
		value->metadata.extractor_versions = {{"call-relation", "1.0.0"},
											  {"source-preprocessor", "1.0.0"},
											  {"symbol-type", "1.0.0"},
											  {"workspace-catalog", "1.0.0"}};
		const auto sqlite_database =
			value->options.cache_directory.value_or(path{}) / "facts.sqlite3";
		if (value->options.cache_directory)
		{
			runtime::request_context context;
			context.operation = "workspace.cache-directory.create";
			auto created =
				value->options.files->create_directories(*value->options.cache_directory, context);
			if (!created || !created.value())
				return provisioning_error("facts.backend-unavailable",
										  "cache-directory-create-failed",
										  failure_scope::workspace);
		}
		result<std::shared_ptr<store::fact_store_port>> opened = value->options.cache_directory
			? store::open_sqlite_store(sqlite_database, value->metadata)
			: store::make_in_memory_store(value->metadata);
		if (!opened)
			return std::move(opened.error());
		value->store = std::move(opened.value());
		if (value->store->compatibility() == store::compatibility_state::rebuild_required)
		{
			if (auto rebuilt = value->store->rebuild(value->metadata); !rebuilt)
				return std::move(rebuilt.error());
		}
		if (value->store->compatibility() == store::compatibility_state::corrupt)
			return provisioning_error("facts.store-corrupt", "cache-integrity");
		auto snapshot = value->store->read();
		if (!snapshot)
			return std::move(snapshot.error());
		value->snapshot = std::move(snapshot.value());
		for (const auto& unit : value->snapshot->coverage.units())
		{
			if (unit.kind != "fact-unit")
				continue;
			const auto fields = parse_fields(unit.id);
			if (!fields.contains("f") || !fields.contains("u") || !fields.contains("v") ||
				!fields.contains("k") || !fields.contains("p") || !fields.contains("i"))
				continue;
			const auto found =
				std::ranges::find_if(value->options.units,
									 [&](const compile_unit& candidate)
									 {
										 return candidate.id().value() == fields.at("u");
									 });
			if (found == value->options.units.end())
				continue;
			auto file = hex_decode(fields.at("f"));
			if (!file)
				continue;
			try
			{
				const auto kind = static_cast<fact_kind>(std::stoul(fields.at("k")));
				const auto precision = static_cast<precision_level>(std::stoul(fields.at("p")));
				auto request =
					make_requirement(value->options.root, *found, kind, precision, fields.at("i"));
				request.file = *file;
				request.id = unit.id;
				const auto logical = request.logical_key;
				value->requirements[logical] = {std::move(request), unit.state, unit.reason};
			}
			catch (const std::exception&)
			{
				continue;
			}
		}
		const bool custom_worker = static_cast<bool>(worker);
		value->worker = custom_worker
			? std::move(worker)
			: std::shared_ptr<scheduling::worker_port>{
				  std::make_shared<scheduling::frontend_scheduler_worker>()};
		value->worker_requires_clang = !custom_worker;
		return std::shared_ptr<provisioning_service>{new provisioning_service{std::move(value)}};
	}

	result<void> provisioning_service::ensure(const fact_profile& profile,
											  const analysis_scope& scope,
											  execution_context context)
	{
		if (context.cancellation.stop_requested())
			return provisioning_error("core.cancelled", "before-planning");
		runtime::real_time_adapter clock;
		if (context.deadline && clock.steady_now() >= *context.deadline)
			return provisioning_error("core.deadline-exceeded", "before-planning");
		std::unique_lock operation_lock{implementation_->operation_mutex};
		auto execution_stop = [&]() -> std::optional<error>
		{
			if (context.cancellation.stop_requested())
				return provisioning_error("core.cancelled", "operation-checkpoint");
			if (context.deadline && clock.steady_now() >= *context.deadline)
				return provisioning_error("core.deadline-exceeded", "operation-checkpoint");
			return std::nullopt;
		};
		std::map<std::string, requirement_state> states;
		std::map<std::string, cached_batch> batches;
		std::shared_ptr<const store::snapshot_data> old_snapshot;
		std::shared_ptr<scheduling::worker_port> worker;
		bool worker_requires_clang{};
		{
			const std::scoped_lock lock{implementation_->state_mutex};
			states = implementation_->requirements;
			batches = implementation_->batches;
			old_snapshot = implementation_->snapshot;
			worker = implementation_->worker;
			worker_requires_clang = implementation_->worker_requires_clang;
		}

		std::vector<compile_unit> selected;
		const auto scope_kind = workspace_value_access::kind(scope);
		const auto& scoped_variants = workspace_value_access::variants(scope);
		for (const auto& unit : implementation_->options.units)
		{
			bool include = scope_kind == workspace_value_access::scope_kind::all;
			if (scope_kind == workspace_value_access::scope_kind::compile_units)
				include = std::ranges::contains(workspace_value_access::units(scope), unit.id());
			else if (scope_kind == workspace_value_access::scope_kind::files ||
					 scope_kind == workspace_value_access::scope_kind::changed_files)
			{
				const auto unit_file =
					semantic_path(implementation_->options.root, unit.command().file);
				include = std::ranges::any_of(
					workspace_value_access::files(scope),
					[&](const path& file)
					{
						return semantic_path(implementation_->options.root, file) == unit_file;
					});
			}
			if (include && scoped_variants.has_value())
				include = std::ranges::contains(scoped_variants.value(), unit.variant_id());
			if (include)
				selected.push_back(unit);
		}
		if (selected.empty() &&
			(scope_kind == workspace_value_access::scope_kind::files ||
			 scope_kind == workspace_value_access::scope_kind::changed_files) &&
			workspace_value_access::includes_headers(scope))
		{
			for (const auto& unit : implementation_->options.units)
				if (std::ranges::any_of(workspace_value_access::files(scope),
										[&](const path& file)
										{
											return file.stem() == unit.command().file.stem();
										}))
					selected.push_back(unit);
		}
		if (selected.empty() &&
			(scope_kind != workspace_value_access::scope_kind::all ||
			 !implementation_->options.units.empty()) &&
			!(scoped_variants.has_value() && scoped_variants.value().empty()))
			return provisioning_error("workspace.compile-command-missing", "empty-scope");

		const auto& files = *implementation_->options.files;
		runtime::fnv1a_hash_adapter hashes;
		runtime::request_context request_context;
		request_context.operation = "workspace.ensure.input-digest";
		request_context.cancellation = context.cancellation;
		request_context.deadline = context.deadline;
		std::string configuration_digest{"none"};
		if (implementation_->options.configuration_file)
		{
			auto digest = runtime::digest_file(files,
											   hashes,
											   *implementation_->options.configuration_file,
											   "cxxlens.provisioning-config.v1",
											   request_context);
			if (!digest)
			{
				if (auto stopped = execution_stop())
					return std::move(*stopped);
				return provisioning_error("config.file-not-found", "configuration-digest");
			}
			configuration_digest = digest->hexadecimal;
		}

		std::map<std::string, std::string> inputs;
		std::set<std::string> changed_units;
		for (const auto& unit : selected)
		{
			auto source = files.read(unit.command().file, request_context);
			const auto content = source ? *source : std::string{"<unreadable>"};
			std::string versioned_payload = unit.command_digest();
			versioned_payload.append("|config=");
			versioned_payload.append(configuration_digest);
			versioned_payload.append("|source=");
			versioned_payload.append(std::to_string(content.size()));
			versioned_payload.push_back(':');
			versioned_payload.append(content);
			versioned_payload.append("|adapter=");
			versioned_payload.append(implementation_->metadata.adapter_id);
			versioned_payload.push_back('@');
			versioned_payload.append(implementation_->metadata.adapter_version);
			for (const auto& [extractor, version] : implementation_->metadata.extractor_versions)
			{
				versioned_payload.append("|extractor=");
				versioned_payload.append(extractor);
				versioned_payload.push_back('@');
				versioned_payload.append(version);
			}
			auto digest = hashes.calculate(
				runtime::make_hash_request("cxxlens.fact-requirement-input.v1", versioned_payload),
				request_context);
			if (!digest)
				return provisioning_error("core.cancelled", "input-digest");
			inputs.emplace(std::string{unit.id().value()}, digest->hexadecimal);
			const auto batch = batches.find(std::string{unit.id().value()});
			if (batch != batches.end() && batch->second.input_digest != digest->hexadecimal)
			{
				changed_units.insert(std::string{unit.id().value()});
				batches.erase(batch);
			}
			for (auto position = states.begin(); position != states.end();)
			{
				if (position->second.value.unit.id() == unit.id() &&
					position->second.value.input_digest != digest->hexadecimal)
				{
					changed_units.insert(std::string{unit.id().value()});
					position = states.erase(position);
				}
				else
					++position;
			}
		}

		provisioning_trace trace;
		std::vector<requirement> current_requests;
		std::map<std::string, std::vector<requirement>> parse_requests;
		bool unavailable{};
		std::string unavailable_capability;
		for (const auto& unit : selected)
			for (const auto kind : fact_profile_access::kinds(profile))
			{
				auto needed = make_requirement(implementation_->options.root,
											   unit,
											   kind,
											   fact_profile_access::precision(profile),
											   inputs.at(std::string{unit.id().value()}));
				current_requests.push_back(needed);
				++trace.requested;
				if (const auto existing = states.find(needed.logical_key);
					existing != states.end() && existing->second.value.id == needed.id)
				{
					++trace.warm;
					continue;
				}
				if (kind == fact_kind::compile_command)
				{
					states[needed.logical_key] = {needed, coverage_state::covered, std::nullopt};
					continue;
				}
				if (!parse_supported(kind, needed.precision))
				{
					states[needed.logical_key] = {
						needed, coverage_state::unresolved, "core.unsupported-capability"};
					unavailable = true;
					if (unavailable_capability.empty())
						unavailable_capability = missing_capability(kind, needed.precision);
					continue;
				}
				if (const auto batch = batches.find(std::string{unit.id().value()});
					batch != batches.end() && batch->second.input_digest == needed.input_digest)
				{
					states[needed.logical_key] = {needed, coverage_state::covered, std::nullopt};
					++trace.warm;
					continue;
				}
				parse_requests[std::string{unit.id().value()}].push_back(needed);
			}

		if (!parse_requests.empty() && worker_requires_clang &&
			!cxxlens::capabilities().has("frontend.clang22"))
		{
			for (const auto& [unit, requests] : parse_requests)
			{
				(void)unit;
				for (const auto& needed : requests)
					states[needed.logical_key] = {
						needed, coverage_state::unresolved, "core.capability-unavailable"};
			}
			parse_requests.clear();
			unavailable = true;
			unavailable_capability = "frontend.clang22";
		}

		std::set<std::pair<std::string, fact_kind>> rebuilt;
		if (!parse_requests.empty())
		{
			if (context.memory_budget_mb != 0U && context.memory_budget_mb < 256U)
				return provisioning_error("core.budget-exhausted", "frontend-memory-budget");
			runtime::real_time_adapter time;
			scheduling::scheduler scheduler{hashes, time};
			std::vector<scheduling::task_request> tasks;
			std::map<std::string, std::string> task_units;
			for (const auto& [unit_id, requests] : parse_requests)
			{
				scheduling::task_request task;
				task.profile = fact_profile{};
				task.priority = scope_kind == workspace_value_access::scope_kind::changed_files
					? scheduling::task_priority::changed
					: scheduling::task_priority::explicit_target;
				task.parse.unit = requests.front().unit;
				task.parse.files = implementation_->options.virtual_files;
				for (const auto& needed : requests)
					task.profile = task.profile.include(needed.kind);
				task.snapshot_key = requests.front().input_digest;
				task.subscribers = {{"workspace.ensure", {}}};
				auto key = scheduler.task_key(task);
				if (!key)
					return std::move(key.error());
				task_units.emplace(key.value(), unit_id);
				tasks.push_back(std::move(task));
			}
			trace.scheduled = tasks.size();
			scheduling::scheduler_options scheduler_options;
			scheduler_options.jobs = context.parallelism == 0U
				? std::max(std::size_t{1U},
						   static_cast<std::size_t>(std::thread::hardware_concurrency()))
				: context.parallelism;
			scheduler_options.execution = context;
			scheduler_options.execution.progress = {};
			auto scheduled = scheduler.run(std::move(tasks), *worker, scheduler_options);
			if (!scheduled)
				return std::move(scheduled.error());
			for (const auto& result : scheduled.value().tasks)
			{
				const auto& unit_id = task_units.at(result.task_key);
				const auto& requests = parse_requests.at(unit_id);
				if (result.state == scheduling::task_state::cancelled ||
					result.state == scheduling::task_state::deadline_exceeded ||
					result.state == scheduling::task_state::budget_exhausted ||
					result.state == scheduling::task_state::output_limited)
					return provisioning_error(result.reason_code, "scheduler-stopped");
				if (result.reason_code == "extractor.invalid-observation" ||
					result.reason_code == "core.internal-invariant-violation")
					return provisioning_error(result.reason_code, "invalid-worker-batch");
				if (result.state == scheduling::task_state::succeeded && result.observation_batch)
				{
					batches[unit_id] = {requests.front().input_digest, *result.observation_batch};
					for (const auto& needed : requests)
					{
						states[needed.logical_key] = {
							needed, coverage_state::covered, std::nullopt};
						rebuilt.emplace(unit_id, needed.kind);
					}
				}
				else
				{
					const auto capability_failure =
						result.reason_code == "core.capability-unavailable";
					for (const auto& needed : requests)
						states[needed.logical_key] = {needed,
													  capability_failure
														  ? coverage_state::unresolved
														  : coverage_state::failed,
													  result.reason_code};
					unavailable = unavailable || capability_failure;
					if (capability_failure && unavailable_capability.empty())
						unavailable_capability = "frontend.clang22";
				}
			}
		}
		if (auto stopped = execution_stop())
			return std::move(*stopped);

		std::map<std::string, std::set<fact_kind>> wanted;
		std::map<std::string, compile_unit> catalog_units;
		for (const auto& unit : implementation_->options.units)
			catalog_units.emplace(std::string{unit.id().value()}, unit);
		for (const auto& [unused, state] : states)
		{
			(void)unused;
			if (state.state == coverage_state::covered)
				wanted[std::string{state.value.unit.id().value()}].insert(state.value.kind);
		}
		std::vector<frontend::observation_batch> reduction_batches;
		for (const auto& [unit_id, cached] : batches)
		{
			if (!wanted.contains(unit_id))
				continue;
			auto batch = cached.value;
			const auto& kinds = wanted.at(unit_id);
			std::erase_if(batch.observations,
						  [&](const facts::observation_record& observation)
						  {
							  return !kinds.contains(observation.kind);
						  });
			if (kinds.contains(fact_kind::compile_command) &&
				std::ranges::none_of(batch.observations,
									 [](const auto& observation)
									 {
										 return observation.kind == fact_kind::compile_command;
									 }))
			{
				const auto unit = catalog_units.find(unit_id);
				if (unit != catalog_units.end())
					batch.observations.push_back(
						compile_command_observation(unit->second, batch.adapter_version));
			}
			std::ranges::sort(batch.observations, {}, observation_key);
			if (auto checked = batch.validate(); !checked)
				return provisioning_error("extractor.invalid-observation", "filtered-batch");
			reduction_batches.push_back(std::move(batch));
		}
		for (const auto& unit : selected)
		{
			const auto unit_id = std::string{unit.id().value()};
			if (batches.contains(unit_id) || !wanted[unit_id].contains(fact_kind::compile_command))
				continue;
			frontend::observation_batch batch;
			batch.adapter_id = "clang22.frontend";
			batch.adapter_version = "1.0.0";
			batch.unit = unit.id();
			batch.variant = unit.variant_id();
			batch.coverage.parsed = 1U;
			batch.observations.push_back(compile_command_observation(unit, batch.adapter_version));
			if (auto checked = batch.validate(); !checked)
				return provisioning_error("extractor.invalid-observation", "catalog-batch");
			reduction_batches.push_back(std::move(batch));
		}

		auto reduced = facts::reduce_observations(std::move(reduction_batches));
		if (!reduced)
			return std::move(reduced.error());
		auto final_reduction = std::move(reduced.value());
		std::map<std::string, facts::detached_fact_record> merged;
		std::map<std::string, build_variant_id> unit_variants;
		for (const auto& unit : implementation_->options.units)
			unit_variants.emplace(std::string{unit.id().value()}, unit.variant_id());
		for (auto fact : old_snapshot->facts)
		{
			std::erase_if(fact.origin.compile_units,
						  [&](const compile_unit_id& contributor)
						  {
							  const auto id = std::string{contributor.value()};
							  return !unit_variants.contains(id) || changed_units.contains(id) ||
								  rebuilt.contains({id, fact.kind});
						  });
			if (fact.origin.compile_units.empty())
				continue;
			fact.origin.variants.clear();
			for (const auto& contributor : fact.origin.compile_units)
				if (unit_variants.contains(std::string{contributor.value()}))
					fact.origin.variants.push_back(
						unit_variants.at(std::string{contributor.value()}));
			sort_unique(fact.origin.variants);
			merged.emplace(std::string{fact.id.value()}, std::move(fact));
		}
		for (auto& fact : final_reduction.facts)
		{
			const auto key = std::string{fact.id.value()};
			if (const auto existing = merged.find(key); existing != merged.end())
			{
				if (existing->second.kind != fact.kind ||
					existing->second.stable_key != fact.stable_key ||
					existing->second.payload != fact.payload)
					return provisioning_error("facts.reduction-conflict", "persistent-merge");
				fact.origin.compile_units.insert(fact.origin.compile_units.end(),
												 existing->second.origin.compile_units.begin(),
												 existing->second.origin.compile_units.end());
				fact.origin.variants.insert(fact.origin.variants.end(),
											existing->second.origin.variants.begin(),
											existing->second.origin.variants.end());
				sort_unique(fact.origin.compile_units);
				sort_unique(fact.origin.variants);
			}
			merged[key] = std::move(fact);
		}
		final_reduction.facts.clear();
		for (auto& [unused, fact] : merged)
		{
			(void)unused;
			final_reduction.facts.push_back(std::move(fact));
		}
		std::ranges::sort(final_reduction.facts, fact_less);
		final_reduction.coverage = make_coverage(states);
		if (auto checked = final_reduction.validate(); !checked)
			return provisioning_error("facts.transaction-failed", "final-reduction-invalid");
		if (auto stopped = execution_stop())
			return std::move(*stopped);

		auto transaction = implementation_->store->begin(implementation_->metadata);
		if (!transaction)
			return std::move(transaction.error());
		if (auto staged = transaction.value()->stage(final_reduction); !staged)
			return std::move(staged.error());
		if (auto validated = transaction.value()->validate(); !validated)
			return std::move(validated.error());
		if (auto stopped = execution_stop())
			return std::move(*stopped);
		if (auto committed = transaction.value()->commit(); !committed)
			return std::move(committed.error());
		auto snapshot = implementation_->store->read();
		if (!snapshot)
			return std::move(snapshot.error());

		for (const auto& needed : current_requests)
		{
			const auto& state = states.at(needed.logical_key);
			if (state.state == coverage_state::covered)
				++trace.covered;
			else if (state.state == coverage_state::failed)
				++trace.failed;
			else
				++trace.unresolved;
			trace.rows.push_back({needed.id,
								  state.state == coverage_state::covered	  ? "covered"
									  : state.state == coverage_state::failed ? "failed"
																			  : "unresolved",
								  state.reason.value_or("ok")});
		}
		std::ranges::sort(trace.rows);
		if (auto checked = trace.validate(); !checked)
			return std::move(checked.error());
		{
			const std::scoped_lock lock{implementation_->state_mutex};
			implementation_->requirements = std::move(states);
			implementation_->batches = std::move(batches);
			implementation_->snapshot = std::move(snapshot.value());
			implementation_->trace = trace;
		}
		operation_lock.unlock();
		if (context.progress)
			context.progress(1.0, "committed");
		if (unavailable)
		{
			auto failure = provisioning_error("core.capability-unavailable", "requested-facts");
			failure.attributes.emplace("capability",
									   unavailable_capability.empty() ? "frontend.clang22"
																	  : unavailable_capability);
			failure.attributes.emplace("action", "enable-required-extractor-capability");
			return failure;
		}
		return {};
	}

	fact_store provisioning_service::facts() const
	{
		const std::scoped_lock lock{implementation_->state_mutex};
		return fact_store_access::make_store(implementation_->snapshot,
											 implementation_->options.root);
	}

	capability_set provisioning_service::capabilities() const
	{
		return cxxlens::capabilities();
	}

	result<std::vector<diagnostic>> provisioning_service::diagnose(execution_context context) const
	{
		if (context.cancellation.stop_requested())
			return provisioning_error("core.cancelled", "doctor-cancelled");
		runtime::real_time_adapter clock;
		if (context.deadline && clock.steady_now() >= *context.deadline)
			return provisioning_error("core.deadline-exceeded", "doctor-deadline");
		std::vector<diagnostic> output;
		if (implementation_->options.units.empty())
			output.push_back(diagnostic{.id = "workspace.catalog-empty",
										.message = "Compilation database contains no compile units",
										.level = severity::error,
										.primary = {},
										.related = {},
										.compiler_option = {}});
		if (implementation_->worker_requires_clang &&
			!cxxlens::capabilities().has("frontend.clang22"))
			output.push_back(
				diagnostic{.id = "workspace.frontend-unavailable",
						   .message = "LLVM/Clang 22 frontend is unavailable in this build",
						   .level = severity::error,
						   .primary = {},
						   .related = {},
						   .compiler_option = {}});
		{
			const std::scoped_lock lock{implementation_->state_mutex};
			if (implementation_->snapshot->coverage.count(coverage_state::failed) != 0U)
				output.push_back(diagnostic{.id = "workspace.fact-coverage-failed",
											.message = "One or more requested fact units failed",
											.level = severity::error,
											.primary = {},
											.related = {},
											.compiler_option = {}});
			if (implementation_->snapshot->coverage.count(coverage_state::unresolved) != 0U)
				output.push_back(
					diagnostic{.id = "workspace.fact-coverage-unresolved",
							   .message = "One or more requested fact units are unresolved",
							   .level = severity::warning,
							   .primary = {},
							   .related = {},
							   .compiler_option = {}});
		}
		std::ranges::sort(output, {}, &diagnostic::id);
		return output;
	}

	provisioning_trace provisioning_service::last_trace() const
	{
		const std::scoped_lock lock{implementation_->state_mutex};
		return implementation_->trace;
	}

	void provisioning_service::set_worker(std::shared_ptr<scheduling::worker_port> worker,
										  const bool requires_clang_capability)
	{
		const std::scoped_lock operation_lock{implementation_->operation_mutex};
		const std::scoped_lock state_lock{implementation_->state_mutex};
		if (worker)
		{
			implementation_->worker = std::move(worker);
			implementation_->worker_requires_clang = requires_clang_capability;
		}
		else
		{
			implementation_->worker = std::make_shared<scheduling::frontend_scheduler_worker>();
			implementation_->worker_requires_clang = true;
		}
	}
} // namespace cxxlens::detail::provisioning
