#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "llvm/clang22/provider_worker.hpp"

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::detail;

	void require(const bool condition, const std::string& message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] source_span span()
	{
		const file_id file{"file_" + std::string(64U, 'f')};
		source_span value;
		value.primary = {
			source_point::at(file, 10U, 2U, 3U),
			source_point::at(file, 14U, 2U, 7U),
			source_range_kind::token,
		};
		value.origin = source_origin::directly_spelled;
		value.digest = {"sha256", 1U, std::string(64U, 'd')};
		return value;
	}

	[[nodiscard]] facts::observation_record observation(const frontend::observation_batch& batch,
														const fact_kind kind,
														std::string semantic_key)
	{
		facts::observation_record value;
		value.adapter_id = batch.adapter_id;
		value.adapter_version = batch.adapter_version;
		value.llvm_major = 22U;
		value.compile_unit = batch.unit;
		value.variant = batch.variant;
		value.kind = kind;
		value.source = span();
		value.payload_version = 1U;
		value.payload.emplace("semantic_key", std::move(semantic_key));
		return value;
	}

	[[nodiscard]] frontend::observation_batch batch()
	{
		frontend::observation_batch value;
		value.adapter_id = "clang22.frontend";
		value.adapter_version = "1.0.0";
		value.unit = compile_unit_id{"cu_" + std::string(64U, 'a')};
		value.variant = build_variant_id{"variant_" + std::string(64U, 'b')};
		value.coverage.parsed = 1U;

		auto entity = observation(value, fact_kind::symbol, "entity");
		entity.payload.emplace("symbol.id", "symbol_" + std::string(64U, 'c'));
		entity.payload.emplace("symbol.kind", "function");
		entity.payload.emplace("symbol.qualified_name", "ns::target");
		entity.name = facts::name_identity{
			"ns::target", "c:@N@ns@F@target#", std::nullopt, std::nullopt, std::nullopt};

		auto type = observation(value, fact_kind::type, "type");
		type.payload.emplace("type.id", "type_" + std::string(64U, 'e'));
		type.type = facts::type_identity{"int", "builtin(i32)", std::nullopt, {}, true};

		auto call = observation(value, fact_kind::call, "call");
		call.payload.emplace("call.kind", "direct_function");
		call.payload.emplace("call.direct_callee", "symbol_" + std::string(64U, 'c'));
		value.observations = {std::move(entity), std::move(type), std::move(call)};
		require(value.validate().has_value(), "normalizer fixture batch is invalid");
		return value;
	}
} // namespace

int main()
{
	using namespace cxxlens;
	using namespace cxxlens::detail::clang22;

	require(entity_observation_descriptor().validate().has_value(),
			"entity observation descriptor is invalid");
	require(type_observation_descriptor().validate().has_value(),
			"type observation descriptor is invalid");
	require(call_observation_descriptor().validate().has_value(),
			"call observation descriptor is invalid");

	auto exact = canonicalize_provider_batch(
		batch(), "sha256:1111111111111111111111111111111111111111111111111111111111111111", true);
	require(exact && exact->exact_equivalence && exact->entity_observations.size() == 1U &&
				exact->type_observations.size() == 1U && exact->call_observations.size() == 1U &&
				exact->entities.size() == 1U && exact->call_sites.size() == 1U &&
				exact->direct_targets.size() == 1U && exact->unresolved.empty(),
			"exact Clang observation canonicalization failed");

	compile_command gcc_specific;
	gcc_specific.arguments = {"g++", "-fabi-version=18", "-c", "input.cpp"};
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
