#include "fact_contract.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ranges>
#include <string_view>

namespace cxxlens::detail::facts
{
	namespace
	{
		[[nodiscard]] result<void> invalid(std::string field, std::string reason)
		{
			error failure;
			failure.code.value = "extractor.invalid-observation";
			failure.message = "fact contract invariant failed";
			failure.attributes.emplace("field", std::move(field));
			failure.attributes.emplace("reason", std::move(reason));
			return failure;
		}

		template <class T>
		[[nodiscard]] bool sorted_unique(const std::vector<T>& values)
		{
			return std::ranges::is_sorted(values,
										  {},
										  [](const T& value)
										  {
											  return value.value();
										  }) &&
				std::adjacent_find(values.begin(), values.end()) == values.end();
		}

		[[nodiscard]] bool forbidden_payload_key(const std::string_view key)
		{
			constexpr std::array fragments{"clang",
										   "llvm",
										   "ast",
										   "pointer",
										   "address",
										   "source_manager",
										   "compiler_instance",
										   "qual_type",
										   "decl_ptr"};
			std::string lower{key};
			std::ranges::transform(lower,
								   lower.begin(),
								   [](const unsigned char value)
								   {
									   return static_cast<char>(std::tolower(value));
								   });
			return std::ranges::any_of(fragments,
									   [&](const std::string_view fragment)
									   {
										   return lower.find(fragment) != lower.npos;
									   });
		}
	} // namespace

	result<void> validate(const name_identity& value)
	{
		if (value.usr && !value.usr->empty())
			return {};
		if (!value.semantic_owner || value.semantic_owner->empty() || !value.declaration_kind ||
			value.declaration_kind->empty() || !value.signature_structure ||
			value.signature_structure->empty())
			return invalid("name_identity", "qualified-name-only");
		return {};
	}

	result<void> validate(const type_identity& value)
	{
		if (value.canonical_structure.empty())
			return invalid("type_identity", "pretty-string-only");
		if (!value.builtin && !value.declaration && value.components.empty())
			return invalid("type_identity", "missing-structural-components");
		if (!sorted_unique(value.components))
			return invalid("type_identity.components", "not-canonical-sorted-unique");
		return {};
	}

	result<void> validate(const observation_record& value)
	{
		if (value.schema != "cxxlens.observation.v1" || value.adapter_id.empty() ||
			value.adapter_version.empty() || value.llvm_major == 0U ||
			!value.compile_unit.valid() || !value.variant.valid() || value.payload_version == 0U)
			return invalid("observation", "missing-versioned-owner");
		if (value.source && value.source->validate())
			return invalid("source", "invalid-source-span");
		for (const auto& [key, unused] : value.payload)
		{
			const auto native_value = unused.find("clang::") != unused.npos ||
				unused.find("llvm::") != unused.npos ||
				(unused.find("0x") != unused.npos && unused.size() >= 8U);
			if (forbidden_payload_key(key) || native_value)
				return invalid("payload." + key, "native-lifetime-payload");
		}
		if (value.name)
		{
			auto status = validate(*value.name);
			if (!status)
				return status;
		}
		if (value.type)
		{
			auto status = validate(*value.type);
			if (!status)
				return status;
		}
		for (const auto& diagnostic : value.diagnostics)
		{
			auto status = diagnostic.validate();
			if (!status)
				return invalid("diagnostics", "invalid-diagnostic");
		}
		return {};
	}

	result<void> validate(const detached_fact_record& value)
	{
		if (value.schema != "cxxlens.fact.v1" || !value.id.valid() || value.stable_key.empty() ||
			value.payload_version == 0U || value.origin.extractor_id.empty() ||
			value.origin.extractor_version.empty())
			return invalid("fact", "missing-detached-identity");
		if (!sorted_unique(value.origin.compile_units) || !sorted_unique(value.origin.variants))
			return invalid("provenance", "not-canonical-sorted-unique");
		if (value.source && value.source->validate())
			return invalid("source", "invalid-source-span");
		for (const auto& [key, payload] : value.payload)
		{
			const auto native_value = payload.find("clang::") != payload.npos ||
				payload.find("llvm::") != payload.npos ||
				(payload.find("0x") != payload.npos && payload.size() >= 8U);
			if (forbidden_payload_key(key) || native_value)
				return invalid("payload." + key, "native-lifetime-payload");
		}
		if (value.name)
		{
			auto status = validate(*value.name);
			if (!status)
				return status;
		}
		if (value.type)
		{
			auto status = validate(*value.type);
			if (!status)
				return status;
		}
		return {};
	}

	result<void> validate(const call_fact_record& value)
	{
		if (!value.id.valid() || value.location.validate())
			return invalid("call", "missing-id-or-location");
		if (!sorted_unique(value.possible_callees))
			return invalid("possible_callees", "not-canonical-sorted-unique");
		if (value.dispatch == dispatch_kind::direct_exact && !value.direct_callee)
			return invalid("direct_callee", "required-for-direct-dispatch");
		if ((value.dispatch == dispatch_kind::virtual_candidate_set ||
			 value.dispatch == dispatch_kind::indirect_candidate_set) &&
			value.possible_callees.empty())
			return invalid("possible_callees", "required-for-candidate-dispatch");
		if ((value.kind == call_kind::member || value.kind == call_kind::virtual_member) &&
			!value.receiver_static_type)
			return invalid("receiver_static_type", "required-for-member-call");
		if (value.why.items().empty())
			return invalid("evidence", "missing-call-evidence");
		return {};
	}

	std::vector<fact_kind> resolve_dependency(const std::string_view dependency)
	{
		constexpr std::array names{"file",
								   "compile_command",
								   "symbol",
								   "type",
								   "declaration",
								   "definition",
								   "reference",
								   "call",
								   "construction",
								   "conversion",
								   "inheritance",
								   "override_relation",
								   "include_relation",
								   "macro_definition",
								   "macro_expansion",
								   "cfg_summary",
								   "flow_summary",
								   "effect_summary",
								   "dynamic_observation",
								   "coverage_region",
								   "custom"};
		const auto found = std::ranges::find(names, dependency);
		if (found != names.end())
			return {static_cast<fact_kind>(std::distance(names.begin(), found))};
		if (dependency == "coverage")
			return {fact_kind::coverage_region};
		if (dependency == "control_flow")
			return {fact_kind::cfg_summary};
		if (dependency == "finding")
			return {fact_kind::custom};
		return {};
	}
} // namespace cxxlens::detail::facts
