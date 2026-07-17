#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <ranges>
#include <string>
#include <vector>

#include <cxxlens/provider/clang22.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_entity.hpp>
#include <cxxlens/relations/source_span.hpp>

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

	[[nodiscard]] sdk::detached_cell
	symbol_cell(const sdk::scalar_kind kind, std::string parameter, std::string value)
	{
		return {{kind, std::move(parameter), false},
				sdk::cell_state::present,
				sdk::scalar_value{std::move(value)},
				std::nullopt};
	}

	[[nodiscard]] sdk::detached_cell optional_typed(std::string parameter, std::string value)
	{
		return {{sdk::scalar_kind::typed_id, std::move(parameter), true},
				sdk::cell_state::present,
				sdk::scalar_value{std::move(value)},
				std::nullopt};
	}

	[[nodiscard]] sdk::detached_cell optional_bytes(std::vector<std::byte> value)
	{
		return {{sdk::scalar_kind::bytes, {}, true},
				sdk::cell_state::present,
				sdk::scalar_value{std::move(value)},
				std::nullopt};
	}

	[[nodiscard]] sdk::detached_cell optional_utf8(std::string value)
	{
		return {{sdk::scalar_kind::utf8_string, {}, true},
				sdk::cell_state::present,
				sdk::scalar_value{std::move(value)},
				std::nullopt};
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
		entity.source_span_id = *sdk::source_span_identity(
			"source-snapshot:one", "file:stable", 10U, 18U, "declaration");
		entity.payload.emplace("symbol.kind", "function");
		entity.payload.emplace("symbol.qualified_name", "ns::target");
		entity.payload.emplace("symbol.signature", "int ()");
		const auto entity_anchor = *entity.source_span_id;

		auto type = observation(observation_kind::type, "type:int");
		type.payload.emplace("type.canonical", "int");

		auto call = observation(observation_kind::call, "call:main:12");
		call.source_span_id = *sdk::source_span_identity(
			"source-snapshot:one", "file:stable", 10U, 18U, "expression");
		call.payload.emplace("call.kind", "direct_function");
		call.payload.emplace("call.direct_callee", "clang-usr:target");
		call.payload.emplace("call.direct_callee_kind", "function");
		call.payload.emplace("call.direct_callee_signature", "int ()");
		call.payload.emplace("call.direct_callee_qualified_name", "ns::target");
		call.payload.emplace("call.direct_callee_anchor", entity_anchor);
		value.observations = {std::move(entity), std::move(type), std::move(call)};
		require(value.validate().has_value(), "normalizer fixture batch is invalid");
		return value;
	}

	[[nodiscard]] detached_observation redeclaration(std::string semantic_key,
													 std::string signature,
													 const std::uint64_t begin,
													 const bool definition,
													 const bool canonical)
	{
		auto value = observation(observation_kind::entity, std::move(semantic_key));
		value.source_span_id = *sdk::source_span_identity(
			"source-snapshot:one", "file:stable", begin, begin + 8U, "declaration");
		value.payload.emplace("symbol.kind", "function");
		value.payload.emplace("symbol.qualified_name", "ns::redeclared");
		value.payload.emplace("symbol.signature", std::move(signature));
		value.payload.emplace("symbol.is_definition", definition ? "true" : "false");
		value.payload.emplace("symbol.is_canonical_declaration", canonical ? "true" : "false");
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

	[[nodiscard]] std::vector<std::byte> bytes_cell(const sdk::detached_row& row,
													const std::string& column)
	{
		const auto found = row.cells.find(column);
		require(found != row.cells.end() && found->second.value.has_value(),
				"expected bytes identity cell");
		const auto* value = std::get_if<std::vector<std::byte>>(&*found->second.value);
		require(value != nullptr, "expected bytes identity value");
		return *value;
	}

	[[nodiscard]] std::string canonical_batch(const canonicalized_provider_batch& batch)
	{
		std::string output;
		for (const auto* rows : {
				 &batch.entity_observations,
				 &batch.type_observations,
				 &batch.call_observations,
				 &batch.entities,
				 &batch.call_sites,
				 &batch.direct_targets,
			 })
			for (const auto& row : *rows)
				output += row.canonical_form() + '\n';
		return output;
	}
} // namespace

int main()
{
	using namespace cxxlens::detail::clang22;
	using cxxlens::provider::clang22::detached_source_span;
	using cxxlens::provider::clang22::source_range_identity;

	const source_range_identity source_identity{"source-snapshot:one", "file:stable", "expression"};
	auto span = sdk::source_span_identity(
		source_identity.source_snapshot, source_identity.file, 10U, 18U, source_identity.role);
	auto changed_snapshot = sdk::source_span_identity(
		"source-snapshot:two", source_identity.file, 10U, 18U, source_identity.role);
	auto changed_file = sdk::source_span_identity(
		source_identity.source_snapshot, "file:other", 10U, 18U, source_identity.role);
	auto changed_role = sdk::source_span_identity(
		source_identity.source_snapshot, source_identity.file, 10U, 18U, "declaration");
	require(
		span && changed_snapshot && changed_file && changed_role && *span != *changed_snapshot &&
			*span != *changed_file && *span != *changed_role &&
			!sdk::source_span_identity({}, source_identity.file, 10U, 18U, source_identity.role),
		"source.span identity is not bound to snapshot/file/range role");
	detached_source_span original{source_identity.source_snapshot,
								  source_identity.file,
								  source_identity.role,
								  "src/a.cpp",
								  10U,
								  18U,
								  false,
								  *span,
								  {}};
	original.origin_chain = {
		{"macro-spelling", "/checkout/include/macros.hpp", 40U, 48U, true},
		{"macro-spelling", "include/wrapper.hpp", 12U, 20U, true},
	};
	auto relocated = original;
	relocated.logical_path = "relocated/src/a.cpp";
	relocated.origin_chain.front().logical_path = "/other-root/include/macros.hpp";
	require(original.validate().has_value() && relocated.validate().has_value() &&
				original.id == relocated.id,
			"logical root relocation changed source.span identity");
	auto editable_origin = original;
	editable_origin.origin_chain.front().read_only = false;
	require(!editable_origin.validate(), "macro spelling origin was not forced read-only");
	using source_span = cxxlens::source::relations::span;
	source_span::builder source_builder;
	for (auto result : {
			 source_builder.set<source_span::span_column>(
				 sdk::detached_cell::typed("source_span_id", *span)),
			 source_builder.set<source_span::snapshot>(
				 sdk::detached_cell::typed("source_snapshot_id", source_identity.source_snapshot)),
			 source_builder.set<source_span::file>(
				 sdk::detached_cell::typed("file_id", source_identity.file)),
			 source_builder.set<source_span::begin>(sdk::detached_cell::unsigned_integer(10U)),
			 source_builder.set<source_span::end>(sdk::detached_cell::unsigned_integer(18U)),
			 source_builder.set<source_span::role>(
				 {{sdk::scalar_kind::open_symbol, "source.range-role/1", false},
				  sdk::cell_state::present,
				  sdk::scalar_value{source_identity.role},
				  std::nullopt}),
			 source_builder.set<source_span::read_only>(sdk::detached_cell::boolean(false)),
		 })
		require(result.has_value(), "source.span authority row rejected shared identity");
	auto source_row = std::move(source_builder).finish();
	require(source_row.has_value(), "source.span builder rejected the shared identity row");
	auto derived_source = sdk::derive_domain_identity(source_span::descriptor(), *source_row);
	require(derived_source && *derived_source == *span &&
				sdk::validate_domain_identity(source_span::descriptor(), *source_row).has_value(),
			"source.span builder and descriptor identity diverged");
	auto invalid_source_row = *source_row;
	invalid_source_row.cells.at("source.span.v1.span") =
		sdk::detached_cell::typed("source_span_id", "source-span:wrong");
	require(!sdk::validate_domain_identity(source_span::descriptor(), invalid_source_row),
			"domain identity validator accepted a mismatched result ID");

	require(entity_observation_descriptor().validate().has_value(),
			"entity observation descriptor is invalid");
	require(type_observation_descriptor().validate().has_value(),
			"type observation descriptor is invalid");
	require(call_observation_descriptor().validate().has_value(),
			"call observation descriptor is invalid");

	clang22_task_input task{
		"cu-" + std::string(64U, 'a'),
		"variant-" + std::string(64U, 'b'),
		"source-snapshot-" + std::string(64U, 'c'),
		"file-" + std::string(64U, 'd'),
		"input.cpp",
		"int target(); int main(){ return target(); }",
		{"-std=c++23"},
	};
	auto encoded = encode_task_input(task);
	require(encoded.has_value(), "task input encoding failed");
	auto decoded = decode_task_input(*encoded);
	require(decoded && decoded->source_snapshot == task.source_snapshot &&
				decoded->file == task.file && decoded->source == task.source &&
				decoded->arguments == task.arguments,
			"task input did not round trip");
	auto missing_source_authority = task;
	missing_source_authority.source_snapshot.clear();
	require(!encode_task_input(missing_source_authority),
			"worker task fabricated a span identity without source authority");

	auto exact = canonicalize_provider_batch(
		batch(), "sha256:1111111111111111111111111111111111111111111111111111111111111111", true);
	require(exact && exact->exact_equivalence && exact->entity_observations.size() == 1U &&
				exact->type_observations.size() == 1U && exact->call_observations.size() == 1U &&
				exact->entities.size() == 1U && exact->call_sites.size() == 1U &&
				exact->direct_targets.size() == 1U && exact->unresolved.empty() &&
				string_cell(exact->call_sites.front(), "cc.call_site.v1.kind") == "direct_function",
			"exact Clang observation canonicalization failed");
	require(string_cell(exact->call_sites.front(), "cc.call_site.v1.source") == *span,
			"worker call-site hard reference differs from base source.span identity");
	const auto& entity_row = exact->entities.front();
	const auto& call_row = exact->call_sites.front();
	require(
		sdk::validate_domain_identity(cxxlens::cc::relations::entity::descriptor(), entity_row) &&
			sdk::validate_domain_identity(cxxlens::cc::relations::call_site::descriptor(),
										  call_row),
		"worker standard row result ID differs from its descriptor projection");
	const std::array entity_reference{
		sdk::canonical_value::from_string("canonicalized"),
		sdk::canonical_value::from_string("function"),
		sdk::canonical_value::null(),
		sdk::canonical_value::from_string(
			string_cell(entity_row, "cc.entity.v1.structural_signature_digest")),
		sdk::canonical_value::from_string(string_cell(entity_row, "cc.entity.v1.anchor")),
		sdk::canonical_value::from_string(string_cell(entity_row, "cc.entity.v1.toolchain")),
		sdk::canonical_value::from_bytes(bytes_cell(entity_row, "cc.entity.v1.provider_local_key")),
	};
	require(*sdk::canonical_identity_digest("cc-entity", entity_reference) ==
				string_cell(entity_row, "cc.entity.v1.entity"),
			"independent entity identity encoder differs from descriptor helper");
	const std::array call_reference{
		sdk::canonical_value::from_string(string_cell(call_row, "cc.call_site.v1.compile_unit")),
		sdk::canonical_value::from_string(string_cell(call_row, "cc.call_site.v1.source")),
		sdk::canonical_value::from_string("direct_function"),
		sdk::canonical_value::from_integer(0),
		sdk::canonical_value::null(),
	};
	require(*sdk::canonical_identity_digest("cc-call", call_reference) ==
				string_cell(call_row, "cc.call_site.v1.call"),
			"independent call identity encoder differs from descriptor helper");
	const auto entity_id = string_cell(entity_row, "cc.entity.v1.entity");
	for (auto mutation : {
			 std::pair{"cc.entity.v1.canonicalization",
					   symbol_cell(sdk::scalar_kind::closed_symbol,
								   "cc.canonicalization-state/1",
								   "provider_local")},
			 std::pair{"cc.entity.v1.kind",
					   symbol_cell(sdk::scalar_kind::open_symbol, "cc.entity-kind/1", "method")},
			 std::pair{"cc.entity.v1.semantic_owner",
					   optional_typed("cc_entity_id", "cc-entity:owner")},
			 std::pair{
				 "cc.entity.v1.structural_signature_digest",
				 symbol_cell(sdk::scalar_kind::digest, {}, "sha256:" + std::string(64U, 'f'))},
			 std::pair{"cc.entity.v1.anchor",
					   optional_typed("source_span_id", "source-span:changed")},
			 std::pair{"cc.entity.v1.toolchain",
					   optional_typed("toolchain_context_id", "toolchain-context:changed")},
			 std::pair{"cc.entity.v1.provider_local_key", optional_bytes({std::byte{0x42}})},
		 })
	{
		auto changed = entity_row;
		changed.cells.insert_or_assign(mutation.first, std::move(mutation.second));
		auto changed_id =
			sdk::derive_domain_identity(cxxlens::cc::relations::entity::descriptor(), changed);
		require(changed_id && *changed_id != entity_id,
				"entity projection mutation did not change identity: " +
					std::string{mutation.first});
	}
	const auto call_id = string_cell(call_row, "cc.call_site.v1.call");
	for (auto mutation : {
			 std::pair{"cc.call_site.v1.compile_unit",
					   sdk::detached_cell::typed("compile_unit_id", "compile-unit:changed")},
			 std::pair{"cc.call_site.v1.source",
					   sdk::detached_cell::typed("source_span_id", "source-span:changed")},
			 std::pair{"cc.call_site.v1.kind",
					   symbol_cell(sdk::scalar_kind::open_symbol, "cc.call-kind/1", "virtual")},
			 std::pair{"cc.call_site.v1.ordinal", sdk::detached_cell::unsigned_integer(1U)},
			 std::pair{"cc.call_site.v1.caller",
					   optional_typed("cc_entity_id", "cc-entity:caller")},
		 })
	{
		auto changed = call_row;
		changed.cells.insert_or_assign(mutation.first, std::move(mutation.second));
		auto changed_id =
			sdk::derive_domain_identity(cxxlens::cc::relations::call_site::descriptor(), changed);
		require(changed_id && *changed_id != call_id,
				"call projection mutation did not change identity");
	}
	auto display_only = entity_row;
	display_only.cells.at("cc.entity.v1.qualified_name") = optional_utf8("renamed::display");
	auto display_identity =
		sdk::derive_domain_identity(cxxlens::cc::relations::entity::descriptor(), display_only);
	auto unchanged_identity =
		sdk::derive_domain_identity(cxxlens::cc::relations::entity::descriptor(), entity_row);
	require(display_identity && unchanged_identity && *display_identity == *unchanged_identity,
			"non-projection display field changed entity identity");
	auto hidden_variant = batch();
	hidden_variant.variant = "variant-" + std::string(64U, 'f');
	auto hidden_result = canonicalize_provider_batch(
		hidden_variant,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(hidden_result &&
				string_cell(hidden_result->entities.front(), "cc.entity.v1.entity") == entity_id &&
				string_cell(hidden_result->call_sites.front(), "cc.call_site.v1.call") == call_id,
			"hidden batch variant changed a standard relation ID");

	observation_batch redeclaration_batch;
	redeclaration_batch.unit = "cu-" + std::string(64U, 'a');
	redeclaration_batch.variant = "variant-" + std::string(64U, 'b');
	redeclaration_batch.observations = {
		redeclaration("clang-usr:redeclared", "void ()", 10U, false, true),
		redeclaration("clang-usr:redeclared", "void ()", 40U, true, false),
	};
	auto canonical_redeclaration = canonicalize_provider_batch(
		redeclaration_batch,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	const auto definition_anchor = *redeclaration_batch.observations.back().source_span_id;
	require(canonical_redeclaration && canonical_redeclaration->exact_equivalence &&
				canonical_redeclaration->entities.size() == 1U &&
				canonical_redeclaration->entity_observations.size() == 2U &&
				canonical_redeclaration->unresolved.empty() &&
				string_cell(canonical_redeclaration->entities.front(), "cc.entity.v1.anchor") ==
					definition_anchor,
			"forward declaration and definition did not select one definition entity");
	auto reversed_redeclarations = redeclaration_batch;
	std::ranges::reverse(reversed_redeclarations.observations);
	auto reversed_canonical = canonicalize_provider_batch(
		reversed_redeclarations,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(reversed_canonical && reversed_canonical->entities.size() == 1U &&
				reversed_canonical->entities.front().canonical_form() ==
					canonical_redeclaration->entities.front().canonical_form(),
			"redeclaration arrival order changed canonical entity payload");
	auto declarations_only = redeclaration_batch;
	declarations_only.observations.back() =
		redeclaration("clang-usr:redeclared", "void ()", 40U, false, false);
	auto declaration_result = canonicalize_provider_batch(
		declarations_only,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(declaration_result && declaration_result->entities.size() == 1U &&
				string_cell(declaration_result->entities.front(), "cc.entity.v1.anchor") ==
					*declarations_only.observations.front().source_span_id,
			"declaration-only chain did not select the canonical declaration");
	auto moved_anchor = declarations_only;
	moved_anchor.observations = {
		redeclaration("clang-usr:redeclared", "void ()", 80U, false, true)};
	auto moved_result = canonicalize_provider_batch(
		moved_anchor,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(moved_result &&
				string_cell(moved_result->entities.front(),
							"cc.entity.v1.structural_signature_digest") ==
					string_cell(declaration_result->entities.front(),
								"cc.entity.v1.structural_signature_digest"),
			"source anchor movement changed structural signature identity");
	auto overloads = declarations_only;
	overloads.observations.push_back(
		redeclaration("clang-usr:redeclared-overload", "void (int)", 100U, true, true));
	auto overload_result = canonicalize_provider_batch(
		overloads, "sha256:1111111111111111111111111111111111111111111111111111111111111111", true);
	require(overload_result && overload_result->entities.size() == 2U,
			"distinct overload semantic keys were collapsed");
	auto incompatible = declarations_only;
	incompatible.observations.back() =
		redeclaration("clang-usr:redeclared", "int ()", 40U, true, false);
	auto incompatible_result = canonicalize_provider_batch(
		incompatible,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(incompatible_result && !incompatible_result->exact_equivalence &&
				incompatible_result->entities.size() == 1U &&
				incompatible_result->unresolved.size() == 1U &&
				incompatible_result->unresolved.front().code ==
					"provider.entity-redeclaration-incompatible",
			"incompatible redeclarations were not explicitly accounted");
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
	auto macro_calls = cross_tu;
	macro_calls.observations.front().source_origin_chain = {
		"macro-spelling:/checkout/include/macros.hpp:10:18",
		"macro-spelling:include/wrapper.hpp:20:28",
	};
	auto second_expansion = macro_calls.observations.front();
	second_expansion.semantic_key = "call:macro-expansion:two";
	second_expansion.source_span_id =
		*sdk::source_span_identity("source-snapshot:one", "file:stable", 30U, 42U, "expression");
	macro_calls.observations.push_back(std::move(second_expansion));
	auto macro_result = canonicalize_provider_batch(
		macro_calls,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(macro_result && macro_result->call_sites.size() == 2U &&
				macro_result->direct_targets.size() == 2U &&
				macro_result->call_observations.size() == 2U &&
				!bytes_cell(macro_result->call_observations.front(),
							"frontend.clang22.call_observation.v1.source_origin_chain")
					 .empty() &&
				string_cell(macro_result->call_sites.front(), "cc.call_site.v1.call") !=
					string_cell(macro_result->call_sites.back(), "cc.call_site.v1.call"),
			"distinct macro expansion offsets were deduplicated or lost their origin chain");
	auto relocated_macro = macro_calls;
	for (auto& call : relocated_macro.observations)
		call.source_origin_chain.front() = "macro-spelling:/relocated/include/macros.hpp:10:18";
	auto relocated_macro_result = canonicalize_provider_batch(
		relocated_macro,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(relocated_macro_result && relocated_macro_result->call_sites.size() == 2U &&
				string_cell(relocated_macro_result->call_sites.front(), "cc.call_site.v1.call") ==
					string_cell(macro_result->call_sites.front(), "cc.call_site.v1.call") &&
				string_cell(relocated_macro_result->call_sites.back(), "cc.call_site.v1.call") ==
					string_cell(macro_result->call_sites.back(), "cc.call_site.v1.call"),
			"macro origin root relocation changed semantic call identities");

	auto reordered = batch();
	std::ranges::reverse(reordered.observations);
	auto reordered_result = canonicalize_provider_batch(
		reordered, "sha256:1111111111111111111111111111111111111111111111111111111111111111", true);
	require(reordered_result && reordered_result->direct_targets.size() == 1U &&
				string_cell(reordered_result->direct_targets.front(),
							"cc.call_direct_target.v1.target") == same_tu_target,
			"target identity changed with observation order");
	require(reordered_result && canonical_batch(*reordered_result) == canonical_batch(*exact),
			"observation permutation changed canonical batch bytes");
	auto inserted_call = batch();
	auto unrelated = inserted_call.observations.back();
	unrelated.semantic_key = "call:unrelated-prefix";
	unrelated.source_span_id =
		*sdk::source_span_identity("source-snapshot:one", "file:stable", 1U, 5U, "expression");
	inserted_call.observations.insert(inserted_call.observations.begin(), std::move(unrelated));
	auto inserted_result = canonicalize_provider_batch(
		inserted_call,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(inserted_result.has_value(), "unrelated call insertion failed canonicalization");
	const auto original_source = string_cell(exact->call_sites.front(), "cc.call_site.v1.source");
	auto original_after_insertion = std::ranges::find_if(
		inserted_result->call_sites,
		[&](const sdk::detached_row& row)
		{
			return string_cell(row, "cc.call_site.v1.source") == original_source;
		});
	require(original_after_insertion != inserted_result->call_sites.end() &&
				string_cell(*original_after_insertion, "cc.call_site.v1.call") == call_id,
			"unrelated call insertion changed an existing call identity");
	auto same_span = batch();
	auto duplicate = same_span.observations.back();
	duplicate.semantic_key = "call:same-span-tie";
	same_span.observations.push_back(std::move(duplicate));
	auto same_span_result = canonicalize_provider_batch(
		same_span, "sha256:1111111111111111111111111111111111111111111111111111111111111111", true);
	std::ranges::reverse(same_span.observations);
	auto reversed_same_span = canonicalize_provider_batch(
		same_span, "sha256:1111111111111111111111111111111111111111111111111111111111111111", true);
	require(same_span_result && reversed_same_span && same_span_result->call_sites.size() == 2U &&
				canonical_batch(*same_span_result) == canonical_batch(*reversed_same_span) &&
				string_cell(same_span_result->call_sites.front(), "cc.call_site.v1.call") !=
					string_cell(same_span_result->call_sites.back(), "cc.call_site.v1.call"),
			"same-span canonical tie-break was absent or input-order dependent");

	auto indirect = cross_tu;
	indirect.observations.front().payload.erase("call.direct_callee");
	indirect.observations.front().payload.insert_or_assign("call.kind", "indirect_function");
	indirect.observations.front().payload.emplace("call.unresolved_reason",
												  "function-pointer-target-not-modeled");
	auto indirect_result = canonicalize_provider_batch(
		indirect, "sha256:1111111111111111111111111111111111111111111111111111111111111111", true);
	require(indirect_result && indirect_result->direct_targets.empty() &&
				indirect_result->unresolved.size() == 1U &&
				indirect_result->unresolved.front().code == "provider.indirect-target-unresolved" &&
				string_cell(indirect_result->call_sites.front(), "cc.call_site.v1.kind") ==
					"indirect_function",
			"indirect call received a fabricated direct target");
	auto member_pointer = indirect;
	member_pointer.observations.front().payload.insert_or_assign("call.kind",
																 "indirect_member_pointer");
	member_pointer.observations.front().payload.insert_or_assign(
		"call.unresolved_reason", "member-pointer-target-not-modeled");
	auto member_pointer_result = canonicalize_provider_batch(
		member_pointer,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(member_pointer_result && member_pointer_result->direct_targets.empty() &&
				member_pointer_result->unresolved.size() == 1U &&
				string_cell(member_pointer_result->call_sites.front(), "cc.call_site.v1.kind") ==
					"indirect_member_pointer",
			"member-pointer call was not classified as indirect");
	auto dependent = indirect;
	dependent.observations.front().payload.insert_or_assign("call.kind", "dependent");
	dependent.observations.front().payload.insert_or_assign("call.unresolved_reason",
															"dependent-callee");
	auto dependent_result = canonicalize_provider_batch(
		dependent, "sha256:1111111111111111111111111111111111111111111111111111111111111111", true);
	require(dependent_result && dependent_result->direct_targets.empty() &&
				dependent_result->unresolved.size() == 1U &&
				dependent_result->unresolved.front().code == "provider.call-target-unresolved" &&
				string_cell(dependent_result->call_sites.front(), "cc.call_site.v1.kind") ==
					"dependent",
			"dependent call was not preserved as explicit unresolved");
	auto virtual_call = cross_tu;
	virtual_call.observations.front().payload.insert_or_assign("call.kind", "virtual_member");
	auto virtual_result = canonicalize_provider_batch(
		virtual_call,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(virtual_result && virtual_result->direct_targets.size() == 1U &&
				virtual_result->unresolved.empty() &&
				string_cell(virtual_result->call_sites.front(), "cc.call_site.v1.kind") ==
					"virtual_member",
			"virtual dispatch lost its static direct target or dynamic-dispatch kind");
	auto inconsistent = indirect;
	inconsistent.observations.front().payload.insert_or_assign("call.kind", "direct_function");
	auto inconsistent_result = canonicalize_provider_batch(
		inconsistent,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(inconsistent_result && !inconsistent_result->exact_equivalence &&
				inconsistent_result->direct_targets.empty() &&
				inconsistent_result->unresolved.size() == 1U &&
				inconsistent_result->unresolved.front().code ==
					"provider.call-kind-target-inconsistent",
			"direct kind without a direct callee was not rejected by the invariant");
	auto fabricated_indirect = cross_tu;
	fabricated_indirect.observations.front().payload.insert_or_assign("call.kind",
																	  "indirect_function");
	auto fabricated_result = canonicalize_provider_batch(
		fabricated_indirect,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		true);
	require(fabricated_result && !fabricated_result->exact_equivalence &&
				fabricated_result->direct_targets.empty() &&
				fabricated_result->unresolved.size() == 1U &&
				fabricated_result->unresolved.front().code ==
					"provider.call-kind-target-inconsistent",
			"indirect kind with a direct callee fabricated a direct-target row");

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
