#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <ranges>
#include <string>
#include <vector>

#include "llvm/clang22/provider_worker.hpp"

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::detail::clang22;

	void require(const bool condition, const std::string& message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] detached_observation observation(const observation_kind kind,
												   std::string semantic_key)
	{
		detached_observation value;
		value.kind = kind;
		value.compile_unit = "cu-" + std::string(64U, 'a');
		value.semantic_key = std::move(semantic_key);
		value.source_span_id = "span-" + std::string(64U, 'd');
		return value;
	}

	[[nodiscard]] observation_batch batch()
	{
		observation_batch value;
		value.unit = "cu-" + std::string(64U, 'a');
		value.variant = "variant-" + std::string(64U, 'b');

		auto entity = observation(observation_kind::entity, "clang-usr:target");
		entity.payload.emplace("symbol.kind", "function");
		entity.payload.emplace("symbol.qualified_name", "ns::target");
		entity.payload.emplace("symbol.signature", "int ()");

		auto type = observation(observation_kind::type, "type:int");
		type.payload.emplace("type.canonical", "int");

		auto call = observation(observation_kind::call, "call:main:12");
		call.payload.emplace("call.kind", "direct_function");
		call.payload.emplace("call.direct_callee", "clang-usr:target");
		value.observations = {std::move(entity), std::move(type), std::move(call)};
		require(value.validate().has_value(), "normalizer fixture batch is invalid");
		return value;
	}

	[[nodiscard]] std::string string_cell(const sdk::detached_row& row, const std::string& column)
	{
		const auto found = row.cells.find(column);
		require(found != row.cells.end() && found->second.value.has_value(),
				"expected target identity cell");
		const auto* value = std::get_if<std::string>(&*found->second.value);
		require(value != nullptr, "expected string target identity");
		return *value;
	}
} // namespace

int main()
{
	using namespace cxxlens::detail::clang22;

	require(entity_observation_descriptor().validate().has_value(),
			"entity observation descriptor is invalid");
	require(type_observation_descriptor().validate().has_value(),
			"type observation descriptor is invalid");
	require(call_observation_descriptor().validate().has_value(),
			"call observation descriptor is invalid");

	clang22_task_input task{
		"cu-" + std::string(64U, 'a'),
		"variant-" + std::string(64U, 'b'),
		"input.cpp",
		"int target(); int main(){ return target(); }",
		{"-std=c++23"},
	};
	auto encoded = encode_task_input(task);
	require(encoded.has_value(), "task input encoding failed");
	auto decoded = decode_task_input(*encoded);
	require(decoded && decoded->source == task.source && decoded->arguments == task.arguments,
			"task input did not round trip");

	auto exact = canonicalize_provider_batch(
		batch(), "sha256:1111111111111111111111111111111111111111111111111111111111111111", true);
	require(exact && exact->exact_equivalence && exact->entity_observations.size() == 1U &&
				exact->type_observations.size() == 1U && exact->call_observations.size() == 1U &&
				exact->entities.size() == 1U && exact->call_sites.size() == 1U &&
				exact->direct_targets.size() == 1U && exact->unresolved.empty(),
			"exact Clang observation canonicalization failed");
	const auto same_tu_target =
		string_cell(exact->direct_targets.front(), "cc.call_direct_target.v1.target");

	auto cross_tu = batch();
	cross_tu.unit = "cu-" + std::string(64U, 'c');
	cross_tu.observations.erase(std::ranges::remove_if(cross_tu.observations,
													   [](const detached_observation& value)
													   {
														   return value.kind !=
															   observation_kind::call;
													   })
									.begin(),
								cross_tu.observations.end());
	for (auto& value : cross_tu.observations)
		value.compile_unit = cross_tu.unit;
	require(cross_tu.validate().has_value(), "cross-TU call-only batch is invalid");
	auto cross_tu_result = canonicalize_provider_batch(
		cross_tu, "sha256:1111111111111111111111111111111111111111111111111111111111111111", true);
	require(cross_tu_result && cross_tu_result->entities.empty() &&
				cross_tu_result->direct_targets.size() == 1U &&
				cross_tu_result->unresolved.empty() &&
				string_cell(cross_tu_result->direct_targets.front(),
							"cc.call_direct_target.v1.target") == same_tu_target,
			"header/external/other-TU direct callee depended on a local entity row");

	auto reordered = batch();
	std::ranges::reverse(reordered.observations);
	auto reordered_result = canonicalize_provider_batch(
		reordered, "sha256:1111111111111111111111111111111111111111111111111111111111111111", true);
	require(reordered_result && reordered_result->direct_targets.size() == 1U &&
				string_cell(reordered_result->direct_targets.front(),
							"cc.call_direct_target.v1.target") == same_tu_target,
			"target identity changed with observation order");

	auto indirect = cross_tu;
	indirect.observations.front().payload.erase("call.direct_callee");
	indirect.observations.front().payload.emplace("call.unresolved_reason", "no-direct-callee");
	auto indirect_result = canonicalize_provider_batch(
		indirect, "sha256:1111111111111111111111111111111111111111111111111111111111111111", true);
	require(indirect_result && indirect_result->direct_targets.empty() &&
				indirect_result->unresolved.size() == 1U,
			"indirect call received a fabricated direct target");

	const std::vector<std::string> gcc_specific{"g++", "-fabi-version=18", "-c", "input.cpp"};
	std::vector<std::string> limitations;
	require(!invocation_has_exact_equivalence(gcc_specific, limitations) &&
				limitations.front().starts_with("ignored-or-gcc-specific-option:"),
			"GCC-specific semantic option claimed exact equivalence");

	auto provider_local = canonicalize_provider_batch(
		batch(),
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		false,
		limitations);
	require(provider_local && !provider_local->exact_equivalence &&
				!provider_local->equivalence_limitations.empty() &&
				provider_local->entity_observations.front().canonical_form().contains(
					"ignored-or-gcc-specific-option"),
			"semantic loss was not preserved as provider-local evidence");
}
