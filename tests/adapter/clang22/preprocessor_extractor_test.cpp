#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "llvm/clang22/frontend_job.hpp"
#include "runtime/hash_port.hpp"
#include "runtime/time_port.hpp"
#include "workspace/frontend_scheduler_worker.hpp"
#include "workspace/scheduler.hpp"

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::detail;

	constexpr std::string_view main_source = R"cpp(#define INNER(x) ((x) + GENERATED_VALUE)
#define OUTER(y) INNER(y)
#if FEATURE
#include "generated.hpp"
#include <system.hpp>
#else
#include "unused.hpp"
#endif
int value = OUTER(FEATURE);
int line_value = __LINE__;
)cpp";
	constexpr std::string_view generated_source = "#pragma once\n#define GENERATED_VALUE 3\n";
	constexpr std::string_view unused_source = "#pragma once\n#define UNUSED_VALUE 9\n";
	constexpr std::string_view system_source = "#pragma once\nusing system_size = unsigned long;\n";

	void require(const bool condition, const std::string& message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	void write(const std::filesystem::path& file, const std::string_view content)
	{
		std::ofstream output{file, std::ios::binary};
		output.write(content.data(), static_cast<std::streamsize>(content.size()));
		require(output.good(), "preprocessor fixture write failed");
	}

	struct fixture
	{
		workspace catalog;
		std::vector<frontend::virtual_source_file> files;
	};

	[[nodiscard]] fixture make_fixture(const std::filesystem::path& root, const bool reverse_files)
	{
		std::filesystem::remove_all(root);
		std::filesystem::create_directories(root);
		const auto main = root / "main.cpp";
		const auto generated = root / "generated.hpp";
		const auto unused = root / "unused.hpp";
		const auto system = root / "system" / "system.hpp";
		std::filesystem::create_directories(system.parent_path());
		write(main, "physical main replaced by snapshot\n");
		write(generated, "physical generated replaced by snapshot\n");
		write(unused, "physical unused replaced by snapshot\n");
		write(system, "physical system replaced by snapshot\n");
		const auto entry = [&root, &main](const int feature)
		{
			return "{\"directory\":\"" + root.generic_string() + "\",\"file\":\"" +
				main.generic_string() +
				"\",\"arguments\":[\"clang++-22\",\"-std=c++23\",\"-DFEATURE=" +
				std::to_string(feature) + "\",\"-I\",\"" + root.generic_string() +
				"\",\"-isystem\",\"" + (root / "system").generic_string() + "\",\"" +
				main.generic_string() + "\"]}";
		};
		write(root / "compile_commands.json", "[" + entry(1) + "," + entry(2) + "]");
		auto opened = workspace::open(
			workspace_options::from_compilation_database(root / "compile_commands.json"));
		require(opened.has_value(), "preprocessor fixture workspace did not open");
		std::vector<frontend::virtual_source_file> files{
			{main, std::string{main_source}},
			{generated, std::string{generated_source}},
			{unused, std::string{unused_source}},
			{system, std::string{system_source}},
		};
		if (reverse_files)
			std::ranges::reverse(files);
		return {std::move(opened.value()), std::move(files)};
	}

	[[nodiscard]] std::vector<frontend::observation_batch> execute_fixture(fixture& value)
	{
		std::vector<frontend::observation_batch> batches;
		for (const auto& unit : value.catalog.compile_units())
		{
			auto result = clang22::execute({unit, value.files});
			require(result.has_value(), "preprocessor frontend returned operation failure");
			if (result.value().coverage.parsed != 1U)
				for (const auto& diagnostic : result.value().diagnostics)
					std::cerr << diagnostic.id << ": " << diagnostic.message << '\n';
			require(result.value().coverage.parsed == 1U,
					"valid preprocessor fixture did not parse");
			require(result.value().validate().has_value(), "preprocessor batch failed validation");
			batches.push_back(std::move(result.value()));
		}
		std::ranges::sort(batches,
						  {},
						  [](const frontend::observation_batch& batch)
						  {
							  return batch.variant.value();
						  });
		return batches;
	}

	[[nodiscard]] bool payload_is(const facts::observation_record& observation,
								  const std::string_view key,
								  const std::string_view value)
	{
		const auto found = observation.payload.find(std::string{key});
		return found != observation.payload.end() && found->second == value;
	}

	[[nodiscard]] const facts::observation_record*
	find_observation(const frontend::observation_batch& batch,
					 fact_kind kind,
					 std::initializer_list<std::pair<std::string_view, std::string_view>> fields)
	{
		const auto found =
			std::ranges::find_if(batch.observations,
								 [&](const auto& observation)
								 {
									 return observation.kind == kind &&
										 std::ranges::all_of(fields,
															 [&](const auto& field)
															 {
																 return payload_is(observation,
																				   field.first,
																				   field.second);
															 });
								 });
		return found == batch.observations.end() ? nullptr : &*found;
	}

	[[nodiscard]] std::string origin_name(const source_origin origin)
	{
		switch (origin)
		{
			case source_origin::directly_spelled:
				return "direct";
			case source_origin::macro_argument:
				return "argument";
			case source_origin::macro_body:
				return "body";
			case source_origin::macro_expansion:
				return "expansion";
			case source_origin::implicit_compiler_node:
				return "implicit";
			case source_origin::generated_file:
				return "generated";
			case source_origin::system_header:
				return "system";
			case source_origin::builtin:
				return "builtin";
			case source_origin::unknown:
				return "unknown";
		}
		return "unknown";
	}

	void check_source_macro_include(const frontend::observation_batch& batch)
	{
		const auto* main_file =
			find_observation(batch, fact_kind::file, {{"file.path", "main.cpp"}});
		require(main_file != nullptr && main_file->source &&
					main_file->source->origin == source_origin::directly_spelled &&
					main_file->source->is_directly_editable(),
				"direct main source origin/editability was lost");
		const auto* generated =
			find_observation(batch, fact_kind::file, {{"file.path", "generated.hpp"}});
		require(generated != nullptr && generated->source &&
					generated->source->origin == source_origin::generated_file &&
					!generated->source->is_directly_editable(),
				"generated file was not explicit/read-only");

		const auto* quoted = find_observation(
			batch, fact_kind::include_relation, {{"include.spelling", "generated.hpp"}});
		require(quoted != nullptr && payload_is(*quoted, "include.angled", "false") &&
					payload_is(*quoted, "include.system", "false") &&
					payload_is(*quoted, "include.resolved", "generated.hpp") && quoted->source &&
					quoted->payload.at("variant") == batch.variant.value() &&
					quoted->payload.at("conditional.context").find("FEATURE") != std::string::npos,
				"quoted conditional include metadata was lost");
		const auto* angled = find_observation(
			batch, fact_kind::include_relation, {{"include.spelling", "system.hpp"}});
		require(angled != nullptr && payload_is(*angled, "include.angled", "true") &&
					payload_is(*angled, "include.system", "true") &&
					payload_is(*angled, "include.resolved", "system/system.hpp") &&
					angled->payload.at("variant") == batch.variant.value(),
				"angled system include metadata was lost");

		const auto* definition =
			find_observation(batch, fact_kind::macro_definition, {{"macro.name", "OUTER"}});
		require(definition != nullptr && definition->source &&
					definition->source->origin == source_origin::directly_spelled,
				"project macro definition was not directly mapped");
		const auto* body = find_observation(
			batch, fact_kind::macro_expansion, {{"macro.name", "OUTER"}, {"macro.role", "body"}});
		require(body != nullptr && body->source &&
					body->source->origin == source_origin::macro_body && body->source->spelling &&
					body->source->expansion && !body->source->is_directly_editable(),
				"macro body mapping/edit guard was lost");
		const auto* argument =
			find_observation(batch,
							 fact_kind::macro_expansion,
							 {{"macro.name", "OUTER"}, {"macro.role", "argument"}});
		require(argument != nullptr && argument->source &&
					argument->source->origin == source_origin::macro_argument &&
					argument->source->macro_stack.back().argument_index == 0U &&
					!argument->source->is_directly_editable(),
				"macro argument/index mapping was lost");
		const auto* nested =
			find_observation(batch,
							 fact_kind::macro_expansion,
							 {{"macro.name", "INNER"}, {"macro.role", "expansion"}});
		require(nested != nullptr && nested->source && nested->source->macro_stack.size() >= 2U,
				"nested macro stack was flattened");

		const auto* builtin =
			find_observation(batch,
							 fact_kind::macro_expansion,
							 {{"macro.name", "__LINE__"}, {"macro.role", "expansion"}});
		require(builtin != nullptr && payload_is(*builtin, "macro.builtin", "true") &&
					(payload_is(*builtin, "macro.definition_location", "invalid") ||
					 payload_is(*builtin, "macro.definition_location", "builtin")) &&
					builtin->source && !builtin->source->is_directly_editable(),
				"builtin/invalid macro origin was fabricated as direct source");
	}

	void check_variants(const std::vector<frontend::observation_batch>& batches)
	{
		require(batches.size() == 2U && batches[0].variant != batches[1].variant,
				"build variants were collapsed");
		std::vector<std::string> feature_values;
		for (const auto& batch : batches)
		{
			const auto* feature =
				find_observation(batch,
								 fact_kind::macro_expansion,
								 {{"macro.name", "FEATURE"}, {"macro.role", "expansion"}});
			if (feature == nullptr)
				for (const auto& observation : batch.observations)
					if (observation.payload.contains("macro.name"))
						std::cerr << "macro=" << observation.payload.at("macro.name") << " role="
								  << (observation.payload.contains("macro.role")
										  ? observation.payload.at("macro.role")
										  : "definition")
								  << '\n';
			require(feature != nullptr && feature->source &&
						payload_is(*feature, "macro.builtin", "true") &&
						feature->payload.at("variant") == batch.variant.value(),
					"command-line variant macro provenance was lost");
			for (const auto& observation : batch.observations)
				require(observation.variant == batch.variant,
						"observation escaped its build variant");
			const auto* body =
				find_observation(batch,
								 fact_kind::macro_expansion,
								 {{"macro.name", "FEATURE"}, {"macro.role", "body"}});
			require(body != nullptr, "variant macro body observation was omitted");
			feature_values.push_back(body->payload.at("macro.text"));
		}
		std::ranges::sort(feature_values);
		require(feature_values == std::vector<std::string>{"1", "2"},
				"variant-specific macro values were first-wins collapsed");
	}

	void check_partial_failure(fixture& value)
	{
		const auto unit = value.catalog.compile_units().front();
		auto broken_files = value.files;
		for (auto& file : broken_files)
			if (file.file.filename() == "main.cpp")
				file.content = "#define BAD(x) x\nint broken( { BAD(1)\n";
		auto broken = clang22::execute({unit, broken_files});
		require(broken && broken.value().coverage.failed == 1U &&
					!broken.value().diagnostics.empty() && !broken.value().observations.empty() &&
					broken.value().validate(),
				"parse failure did not retain diagnostics/observations/coverage");

		auto invalid_utf8 = value.files;
		for (auto& file : invalid_utf8)
			if (file.file.filename() == "main.cpp")
				file.content = std::string{"int bad_"} + static_cast<char>(0xFF) + " = 0;\n";
		auto invalid = clang22::execute({unit, invalid_utf8});
		require(invalid && invalid.value().coverage.failed == 1U &&
					!invalid.value().diagnostics.empty() && invalid.value().validate(),
				"invalid UTF-8 source was omitted or crashed extraction");
	}

	void check_jobs_determinism(fixture& value)
	{
		runtime::fnv1a_hash_adapter hashes;
		runtime::fixed_time_adapter time{std::chrono::system_clock::time_point{},
										 std::chrono::steady_clock::time_point{}};
		scheduling::scheduler scheduler{hashes, time};
		std::vector<scheduling::task_request> base;
		for (const auto& unit : value.catalog.compile_units())
		{
			scheduling::task_request request;
			request.parse = {unit, value.files};
			request.profile = fact_profile::minimal()
								  .include(fact_kind::include_relation)
								  .include(fact_kind::macro_definition)
								  .include(fact_kind::macro_expansion);
			request.snapshot_key = "preprocessor-fixture-v1";
			request.subscribers = {{std::string{unit.variant_id().value()}, {}}};
			base.push_back(std::move(request));
		}
		std::string canonical;
		for (const auto jobs : {1U, 2U, 8U})
		{
			auto requests = base;
			if (jobs == 2U)
				std::ranges::reverse(requests);
			scheduling::frontend_scheduler_worker worker;
			scheduling::scheduler_options options;
			options.jobs = jobs;
			options.seed = jobs == 8U ? 0xC771EU : jobs;
			auto output = scheduler.run(std::move(requests), worker, options);
			require(output.has_value(), "preprocessor scheduler run failed");
			if (canonical.empty())
				canonical = output.value().to_json();
			else
				require(canonical == output.value().to_json(),
						"jobs/seed/enqueue order changed preprocessor observations");
		}
	}

	[[nodiscard]] std::string projection(const frontend::observation_batch& batch)
	{
		std::string output;
		for (const auto& observation : batch.observations)
		{
			if (observation.kind != fact_kind::file &&
				observation.kind != fact_kind::include_relation &&
				observation.kind != fact_kind::macro_definition &&
				observation.kind != fact_kind::macro_expansion)
				continue;
			output += std::to_string(static_cast<std::uint16_t>(observation.kind)) + "|" +
				observation.payload.at("semantic_key") + "|";
			if (observation.source)
			{
				output += origin_name(observation.source->origin) + "|" +
					std::to_string(observation.source->primary.begin.byte_offset) + "-" +
					std::to_string(observation.source->primary.end.byte_offset) + "|stack=";
				for (const auto& frame : observation.source->macro_stack)
					output += frame.macro_name +
						(frame.argument_index ? "[" + std::to_string(*frame.argument_index) + "]"
											  : "") +
						",";
			}
			else
				output += "none|0-0|stack=";
			for (const auto key : {"file.path",
								   "include.spelling",
								   "include.angled",
								   "include.system",
								   "conditional.context",
								   "macro.name",
								   "macro.role",
								   "macro.argument_index",
								   "macro.builtin",
								   "macro.definition_location",
								   "macro.text"})
				if (const auto found = observation.payload.find(key);
					found != observation.payload.end())
					output += "|" + std::string{key} + "=" + found->second;
			output += '\n';
		}
		return output;
	}
} // namespace

int main(const int argument_count, const char* const* arguments)
{
	const auto capability = cxxlens::detail::clang22::capability();
	require(capability.available && capability.llvm_major == 22U,
			"preprocessor adapter test requires exact Clang 22");
	const auto base = std::filesystem::temp_directory_path() / "cxxlens-preprocessor-extractor";
	auto first_fixture = make_fixture(base / "root-a", false);
	auto first = execute_fixture(first_fixture);
	check_variants(first);
	for (const auto& batch : first)
		check_source_macro_include(batch);

	auto relocated_fixture = make_fixture(base / "different-root" / "root-b", true);
	auto relocated = execute_fixture(relocated_fixture);
	require(first.size() == relocated.size(), "root relocation changed variant count");
	for (std::size_t index = 0U; index < first.size(); ++index)
		require(first[index].semantic_representation() ==
					relocated[index].semantic_representation(),
				"root/VFS insertion order changed canonical observations");
	check_jobs_determinism(first_fixture);
	check_partial_failure(first_fixture);

	if (argument_count == 2 && std::string_view{arguments[1]} == "--emit")
		std::cout << projection(first.front());
	if (argument_count == 2 && std::string_view{arguments[1]} == "--emit-spans")
		for (const auto& observation : first.front().observations)
			if (observation.source)
				std::cout << observation.source->to_canonical_json() << '\n';
	std::filesystem::remove_all(base);
	return 0;
}
