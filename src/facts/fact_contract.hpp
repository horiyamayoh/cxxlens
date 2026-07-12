#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <cxxlens/facts.hpp>

namespace cxxlens::detail::facts
{
	/** Canonical NameIR identity; display spelling is deliberately separate. */
	struct name_identity
	{
		std::string display_qualified_name;
		std::optional<std::string> usr;
		std::optional<std::string> semantic_owner;
		std::optional<std::string> declaration_kind;
		std::optional<std::string> signature_structure;
	};

	/** Canonical TypeIR identity; pretty spelling is deliberately non-authoritative. */
	struct type_identity
	{
		std::string display_spelling;
		std::string canonical_structure;
		std::optional<symbol_id> declaration;
		std::vector<type_id> components;
		bool builtin{};
	};

	struct observation_record
	{
		std::string schema{"cxxlens.observation.v1"};
		std::string adapter_id;
		std::string adapter_version;
		std::uint16_t llvm_major{};
		compile_unit_id compile_unit;
		build_variant_id variant;
		fact_kind kind{fact_kind::custom};
		std::optional<source_span> source;
		std::uint32_t payload_version{};
		std::map<std::string, std::string> payload;
		std::vector<diagnostic> diagnostics;
		std::vector<coverage_unit> coverage_contributions;
		std::optional<name_identity> name;
		std::optional<type_identity> type;
	};

	struct detached_fact_record
	{
		std::string schema{"cxxlens.fact.v1"};
		fact_id id;
		fact_kind kind{fact_kind::custom};
		std::string stable_key;
		std::optional<source_span> source;
		provenance origin;
		std::uint32_t payload_version{};
		std::map<std::string, std::string> payload;
		std::optional<name_identity> name;
		std::optional<type_identity> type;
	};

	struct call_fact_record
	{
		fact_id id;
		call_kind kind{call_kind::unknown};
		source_span location;
		std::optional<symbol_id> caller;
		std::optional<symbol_id> direct_callee;
		std::vector<symbol_id> possible_callees;
		std::optional<type_id> receiver_static_type;
		dispatch_kind dispatch{dispatch_kind::unresolved};
		confidence certainty{confidence::possible};
		result_guarantee guarantee{result_guarantee::best_effort};
		evidence why;
	};

	[[nodiscard]] result<void> validate(const name_identity& value);
	[[nodiscard]] result<void> validate(const type_identity& value);
	[[nodiscard]] result<void> validate(const observation_record& value);
	[[nodiscard]] result<void> validate(const detached_fact_record& value);
	[[nodiscard]] result<void> validate(const call_fact_record& value);
	[[nodiscard]] std::vector<fact_kind> resolve_dependency(std::string_view dependency);
} // namespace cxxlens::detail::facts
