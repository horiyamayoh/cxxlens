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

	constexpr std::string_view header_source = R"cpp(#pragma once
namespace left { int same(int); }
namespace right { int same(double); }
int overloaded(int);
double overloaded(double);
int direct_function(int);

struct Base {
  virtual int start(int) const;
  void fixed();
  virtual ~Base();
};
struct Derived : Base {
  int start(int) const override final;
  void own();
};
struct MoreDerived : Derived {};

struct Op {
  int operator()(int) const;
  Op operator+(const Op&) const;
};

template<class T> struct Box { T value; };
template<class T> int dependent_call(T value) { return overloaded(value); }
using function_pointer = int (*)(int);
#define START(receiver) (receiver).start(7)
)cpp";

	constexpr std::string_view main_source = R"cpp(#include "semantic.hpp"
namespace { int internal_value = 3; }
int exercise(Base& base, Derived& derived, function_pointer pointer, Op op) {
  const int qualified = internal_value;
  const int* pointer_value = &qualified;
  Box<int>* box = nullptr;
  Box<double>* other_box = nullptr;
  left::same(1);
  right::same(1.0);
  overloaded(1);
  overloaded(1.0);
  direct_function(1);
  base.start(1);
  derived.start(2);
  derived.fixed();
  pointer(3);
  Op sum = op + op;
  op(4);
  Derived object;
  object.own();
  START(base);
#if MODE == 1
  left::same(5);
#else
  right::same(5.0);
#endif
  return *pointer_value + box->value + static_cast<int>(other_box->value) + sum(1);
}
void destroy(Derived* value) { value->~Derived(); }
template int dependent_call<int>(int);
)cpp";

	constexpr std::string_view other_source = R"cpp(#include "semantic.hpp"
int other(Base& value) { return value.start(MODE); }
)cpp";

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
		std::filesystem::create_directories(file.parent_path());
		std::ofstream output{file, std::ios::binary};
		output.write(content.data(), static_cast<std::streamsize>(content.size()));
		require(output.good(), "semantic fixture write failed");
	}

	struct fixture
	{
		workspace catalog;
		std::vector<frontend::virtual_source_file> files;
	};

	[[nodiscard]] fixture make_fixture(const std::filesystem::path& root, const bool reverse_input)
	{
		std::filesystem::remove_all(root);
		std::filesystem::create_directories(root);
		const auto main = root / "main.cpp";
		const auto other = root / "other.cpp";
		const auto header = root / "semantic.hpp";
		write(main, "physical main replaced by snapshot\n");
		write(other, "physical other replaced by snapshot\n");
		write(header, "physical header replaced by snapshot\n");
		const auto entry = [&root](const std::filesystem::path& file, const int mode)
		{
			return "{\"directory\":\"" + root.generic_string() + "\",\"file\":\"" +
				file.generic_string() +
				"\",\"arguments\":[\"clang++-22\",\"-std=c++23\",\"-DMODE=" + std::to_string(mode) +
				"\",\"-I\",\"" + root.generic_string() + "\",\"" + file.generic_string() + "\"]}";
		};
		std::vector<std::string> entries{
			entry(main, 1), entry(main, 2), entry(other, 1), entry(other, 2)};
		if (reverse_input)
			std::ranges::reverse(entries);
		write(root / "compile_commands.json",
			  "[" + entries[0] + "," + entries[1] + "," + entries[2] + "," + entries[3] + "]");
		auto opened = workspace::open(
			workspace_options::from_compilation_database(root / "compile_commands.json"));
		require(opened.has_value(), "semantic fixture workspace did not open");
		std::vector<frontend::virtual_source_file> files{
			{main, std::string{main_source}},
			{other, std::string{other_source}},
			{header, std::string{header_source}},
		};
		if (reverse_input)
			std::ranges::reverse(files);
		return {std::move(opened.value()), std::move(files)};
	}

	[[nodiscard]] std::vector<frontend::observation_batch> execute_fixture(fixture& value)
	{
		std::vector<frontend::observation_batch> batches;
		for (const auto& unit : value.catalog.compile_units())
		{
			auto result = clang22::execute({unit, value.files});
			require(result.has_value(), "semantic frontend returned operation failure");
			if (result.value().coverage.parsed != 1U)
				for (const auto& diagnostic : result.value().diagnostics)
					std::cerr << diagnostic.id << ": " << diagnostic.message << '\n';
			require(result.value().coverage.parsed == 1U, "valid semantic fixture did not parse");
			require(result.value().validate().has_value(), "semantic batch failed validation");
			batches.push_back(std::move(result.value()));
		}
		std::ranges::sort(batches,
						  {},
						  [](const frontend::observation_batch& batch)
						  {
							  return std::string{batch.unit.value()} + ":" +
								  std::string{batch.variant.value()};
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

	[[nodiscard]] std::vector<const facts::observation_record*>
	find_all(const std::vector<frontend::observation_batch>& batches,
			 const fact_kind kind,
			 const std::initializer_list<std::pair<std::string_view, std::string_view>> fields)
	{
		std::vector<const facts::observation_record*> output;
		for (const auto& batch : batches)
			for (const auto& observation : batch.observations)
				if (observation.kind == kind &&
					std::ranges::all_of(fields,
										[&](const auto& field)
										{
											return payload_is(
												observation, field.first, field.second);
										}))
					output.push_back(&observation);
		return output;
	}

	void check_symbols_and_types(const std::vector<frontend::observation_batch>& batches)
	{
		const auto left_same =
			find_all(batches, fact_kind::symbol, {{"symbol.qualified_name", "left::same"}});
		const auto right_same =
			find_all(batches, fact_kind::symbol, {{"symbol.qualified_name", "right::same"}});
		require(!left_same.empty() && !right_same.empty(),
				"same-name namespace symbols were omitted");
		require(left_same.front()->payload.at("symbol.id") !=
					right_same.front()->payload.at("symbol.id"),
				"same unqualified name collapsed semantic identity");
		require(left_same.front()->name && left_same.front()->name->usr,
				"canonical USR was not retained when available");

		const auto internals =
			find_all(batches, fact_kind::symbol, {{"symbol.name", "internal_value"}});
		require(!internals.empty() && payload_is(*internals.front(), "symbol.linkage", "internal"),
				"anonymous/internal linkage was not explicit");

		const auto pointers = find_all(batches, fact_kind::type, {{"type.kind", "pointer"}});
		const auto references =
			find_all(batches, fact_kind::type, {{"type.kind", "lvalue_reference"}});
		const auto functions = find_all(batches, fact_kind::type, {{"type.kind", "function"}});
		require(!pointers.empty() && !references.empty() && !functions.empty(),
				"pointer/reference/function TypeIR classes were not separated");
		for (const auto* observation : pointers)
			require(observation->type &&
						observation->type->canonical_structure.find("pointer(") !=
							std::string::npos &&
						!observation->type->components.empty(),
					"pointer TypeIR relied on display spelling");
		const auto dependent = find_all(batches, fact_kind::type, {{"type.dependent", "true"}});
		require(!dependent.empty(), "dependent TypeIR was silently omitted");

		std::vector<std::string> box_structures;
		for (const auto* observation : pointers)
			if (observation->type &&
				observation->type->display_spelling.find("Box<") != std::string::npos)
				box_structures.push_back(observation->type->canonical_structure);
		std::ranges::sort(box_structures);
		const auto unique = std::ranges::unique(box_structures);
		box_structures.erase(unique.begin(), unique.end());
		require(box_structures.size() >= 2U,
				"template arguments did not contribute to canonical TypeIR identity");
	}

	void check_calls_and_relations(const std::vector<frontend::observation_batch>& batches)
	{
		const auto direct = find_all(batches, fact_kind::call, {{"call.kind", "direct_function"}});
		const auto members = find_all(batches, fact_kind::call, {{"call.kind", "member"}});
		const auto virtual_members =
			find_all(batches, fact_kind::call, {{"call.kind", "virtual_member"}});
		const auto constructors =
			find_all(batches, fact_kind::call, {{"call.kind", "constructor"}});
		const auto destructors = find_all(batches, fact_kind::call, {{"call.kind", "destructor"}});
		const auto overloaded_operators =
			find_all(batches, fact_kind::call, {{"call.kind", "overloaded_operator"}});
		const auto builtin_operators =
			find_all(batches, fact_kind::call, {{"call.kind", "builtin_operator"}});
		const auto indirect =
			find_all(batches, fact_kind::call, {{"call.kind", "function_pointer"}});
		require(!direct.empty() && !members.empty() && !virtual_members.empty() &&
					!constructors.empty() && !destructors.empty() &&
					!overloaded_operators.empty() && !builtin_operators.empty() &&
					!indirect.empty(),
				"required call classifications were not emitted distinctly");
		for (const auto* call : virtual_members)
			require(call->type && payload_is(*call, "call.dispatch", "static_member_target") &&
						call->payload.at("call.direct_callee") != "unresolved" &&
						call->payload.at("call.receiver_static_type") != "none",
					"virtual call lost receiver/static target separation");
		for (const auto* call : indirect)
			require(payload_is(*call, "call.dispatch", "unresolved") &&
						call->coverage_contributions.front().state == coverage_state::unresolved,
					"indirect call was fabricated as an empty exact target");

		const auto dependent_calls = find_all(
			batches, fact_kind::call, {{"call.unresolved_reason", "dependent-call-target"}});
		require(!dependent_calls.empty() &&
					!dependent_calls.front()->payload.at("call.possible_callees").empty(),
				"dependent overload set was first-selected or omitted");

		const auto macro_calls = find_all(batches, fact_kind::call, {{"call.from_macro", "true"}});
		require(!macro_calls.empty() && macro_calls.front()->source &&
					!macro_calls.front()->source->is_directly_editable(),
				"macro-origin call became directly editable");

		const auto inheritance = find_all(batches, fact_kind::inheritance, {});
		const auto overrides = find_all(batches, fact_kind::override_relation, {});
		require(!inheritance.empty() && !overrides.empty(),
				"direct inheritance/override observations were omitted");
		for (const auto* edge : inheritance)
			require(payload_is(*edge, "inheritance.direct", "true"),
					"invented transitive inheritance row was emitted");
		for (const auto* edge : overrides)
			require(payload_is(*edge, "override.direct", "true"),
					"override edge was not marked direct");
		for (const auto& batch : batches)
		{
			const auto direct_bases =
				std::ranges::count_if(batch.observations,
									  [](const auto& observation)
									  {
										  return observation.kind == fact_kind::inheritance;
									  });
			const auto direct_overrides =
				std::ranges::count_if(batch.observations,
									  [](const auto& observation)
									  {
										  return observation.kind == fact_kind::override_relation;
									  });
			require(direct_bases == 2 && direct_overrides == 1,
					"direct relation fixture gained an invented transitive edge");
		}
	}

	void check_variants(const std::vector<frontend::observation_batch>& batches)
	{
		std::vector<std::string> conditional_counts;
		for (const auto& batch : batches)
		{
			std::string left_id;
			std::string right_id;
			for (const auto& observation : batch.observations)
			{
				if (observation.kind != fact_kind::symbol)
					continue;
				if (payload_is(observation, "symbol.qualified_name", "left::same"))
					left_id = observation.payload.at("symbol.id");
				if (payload_is(observation, "symbol.qualified_name", "right::same"))
					right_id = observation.payload.at("symbol.id");
			}
			if (left_id.empty() || right_id.empty())
				continue;
			std::size_t left_calls{};
			std::size_t right_calls{};
			for (const auto& observation : batch.observations)
			{
				if (observation.kind != fact_kind::call)
					continue;
				left_calls += payload_is(observation, "call.direct_callee", left_id) ? 1U : 0U;
				right_calls += payload_is(observation, "call.direct_callee", right_id) ? 1U : 0U;
				require(observation.variant == batch.variant &&
							observation.payload.at("variant") == batch.variant.value(),
						"call escaped its compile variant provenance");
			}
			if (left_calls + right_calls >= 3U)
				conditional_counts.push_back(std::to_string(left_calls) + ":" +
											 std::to_string(right_calls));
		}
		std::ranges::sort(conditional_counts);
		const auto unique = std::ranges::unique(conditional_counts);
		conditional_counts.erase(unique.begin(), unique.end());
		require(conditional_counts == std::vector<std::string>{"1:2", "2:1"},
				"variant-dependent direct targets were first-wins collapsed");
	}

	void check_determinism(fixture& value)
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
			request.profile = fact_profile::semantic_search();
			request.snapshot_key = "semantic-fixture-v1";
			request.subscribers = {{std::string{unit.variant_id().value()}, {}}};
			base.push_back(std::move(request));
		}
		std::string canonical;
		for (const auto jobs : {1U, 2U, 8U})
		{
			auto requests = base;
			if (jobs != 1U)
				std::ranges::reverse(requests);
			scheduling::frontend_scheduler_worker worker;
			scheduling::scheduler_options options;
			options.jobs = jobs;
			options.seed = jobs * 7919U;
			auto output = scheduler.run(std::move(requests), worker, options);
			require(output.has_value(), "semantic scheduler run failed");
			if (canonical.empty())
				canonical = output.value().to_json();
			else
				require(canonical == output.value().to_json(),
						"jobs/seed/TU/variant order changed semantic observations");
		}
	}

	void check_partial_failure(fixture& value)
	{
		const auto unit = value.catalog.compile_units().front();
		auto files = value.files;
		for (auto& file : files)
			if (file.file.filename() == unit.command().file.filename())
				file.content =
					"#include \"semantic.hpp\"\nint broken( { Base value; value.start(1);\n";
		auto result = clang22::execute({unit, files});
		require(result && result.value().coverage.failed == 1U &&
					!result.value().diagnostics.empty() && !result.value().observations.empty() &&
					result.value().validate(),
				"semantic parse failure lost partial observations/diagnostics/coverage");
	}

	[[nodiscard]] std::string projection(const std::vector<frontend::observation_batch>& batches)
	{
		std::map<std::string, std::string> rows;
		for (const auto& batch : batches)
			for (const auto& observation : batch.observations)
			{
				if (!observation.payload.contains("extractor.id") ||
					observation.payload.at("extractor.id") != "clang22.semantic")
					continue;
				std::string row;
				if (observation.kind == fact_kind::symbol)
				{
					const auto name = observation.payload.at("symbol.qualified_name");
					if (name != "left::same" && name != "right::same" &&
						name.find("internal_value") == std::string::npos && name != "overloaded")
						continue;
					row = "symbol|symbol.qualified_name=" + name +
						"|symbol.kind=" + observation.payload.at("symbol.kind");
				}
				else if (observation.kind == fact_kind::type)
					row = "type|type.kind=" + observation.payload.at("type.kind") +
						"|type.dependent=" + observation.payload.at("type.dependent");
				else if (observation.kind == fact_kind::call)
				{
					row = "call|call.kind=" + observation.payload.at("call.kind") +
						"|call.dispatch=" + observation.payload.at("call.dispatch");
					if (const auto reason = observation.payload.find("call.unresolved_reason");
						reason != observation.payload.end())
						row += "|call.unresolved_reason=" + reason->second;
				}
				else if (observation.kind == fact_kind::inheritance)
					row = "inheritance|inheritance.direct=" +
						observation.payload.at("inheritance.direct");
				else if (observation.kind == fact_kind::override_relation)
					row = "override|override.direct=" + observation.payload.at("override.direct");
				else
					continue;
				rows.emplace(row, row);
			}
		std::string output;
		for (const auto& [unused, row] : rows)
		{
			(void)unused;
			output += row + '\n';
		}
		return output;
	}
} // namespace

int main(const int argument_count, const char* const* arguments)
{
	const auto capability = cxxlens::detail::clang22::capability();
	require(capability.available && capability.llvm_major == 22U,
			"semantic adapter test requires exact Clang 22");
	const auto base = std::filesystem::temp_directory_path() / "cxxlens-semantic-extractor";
	auto first_fixture = make_fixture(base / "root-a", false);
	auto first = execute_fixture(first_fixture);
	check_symbols_and_types(first);
	check_calls_and_relations(first);
	check_variants(first);
	check_determinism(first_fixture);
	check_partial_failure(first_fixture);

	auto relocated_fixture = make_fixture(base / "different-root" / "root-b", true);
	auto relocated = execute_fixture(relocated_fixture);
	require(first.size() == relocated.size(), "root/TU/variant relocation changed batch count");
	for (std::size_t index = 0U; index < first.size(); ++index)
		require(first[index].semantic_representation() ==
					relocated[index].semantic_representation(),
				"root/VFS/TU/variant ordering changed canonical semantic observations");

	if (argument_count == 2 && std::string_view{arguments[1]} == "--emit")
		std::cout << projection(first);
	std::filesystem::remove_all(base);
	return 0;
}
