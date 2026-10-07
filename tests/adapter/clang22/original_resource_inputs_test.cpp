#include "original_resource_inputs.hpp"

#include <cstdlib>
#include <iostream>

#include <clang/AST/DeclBase.h>
#include <clang/AST/Stmt.h>
#include <cxxlens/relations/cc_address_transfer.hpp>
#include <cxxlens/relations/cc_declaration_attribute.hpp>
#include <cxxlens/relations/cc_move_event.hpp>

namespace input = cxxlens::detail::clang22::resource_inputs;
namespace sdk = cxxlens::sdk;
namespace rel = cxxlens::cc::relations;
// Valid native objects, used as borrowed callback keys. No frontend is launched.
struct native_declaration final : clang::Decl
{
	native_declaration() : clang::Decl(clang::Decl::Empty, nullptr, {}) {}
	~native_declaration() override = default;
};
void require(bool value, const char* message)
{
	if (!value)
	{
		std::cerr << message << '\n';
		std::exit(1);
	}
}
const sdk::detached_cell& cell(const sdk::detached_row& row, const char* name)
{
	return row.cells.at(row.descriptor_id + "." + name);
}
std::uint64_t number(const sdk::detached_row& row, const char* name)
{
	return std::get<std::uint64_t>(*cell(row, name).value);
}
std::string string(const sdk::detached_row& row, const char* name)
{
	return std::get<std::string>(*cell(row, name).value);
}
int main()
{
	native_declaration declaration;
	clang::NullStmt syntax{clang::SourceLocation{}};
	input::bindings bindings;
	bindings.compile_unit = "original-unit";
	bindings.declaration = [&](const clang::Decl& d) -> std::string_view
	{
		return &d == &declaration ? "original-declaration" : "";
	};
	bindings.syntax = [&](const clang::Stmt& s) -> std::string_view
	{
		return &s == &syntax ? "original-syntax" : "";
	};
	bindings.descriptor = [](std::string_view name) -> const sdk::relation_descriptor*
	{
		if (name == "cc.declaration_attribute")
			return &rel::declaration_attribute::descriptor();
		if (name == "cc.address_transfer")
			return &rel::address_transfer::descriptor();
		if (name == "cc.move_event")
			return &rel::move_event::descriptor();
		return nullptr;
	};
	require(bool(rel::declaration_attribute::descriptor().validate()),
			"attribute descriptor invalid");
	require(bool(rel::address_transfer::descriptor().validate()), "address descriptor invalid");
	input::observations attributes;
	attributes.admitted_declaration = &declaration;
	attributes.attributes_admitted = true;
	input::attribute_observation carrier;
	carrier.declaration = &declaration;
	carrier.ordinal = 0;
	carrier.original_kind = 123;
	carrier.annotation = std::string_view{"opaque\0payload", 14};
	carrier.spelling = "annotate";
	carrier.argument_state = "complete";
	carrier.arguments.push_back(
		{0, "annotation_expression", "observed", {}, {}, {}, nullptr, nullptr});
	attributes.attributes.push_back(carrier);
	input::attribute_observation ownership;
	ownership.declaration = &declaration;
	ownership.ordinal = 1;
	ownership.original_kind = 456;
	ownership.ownership_kind = "takes";
	ownership.argument_state = "complete";
	ownership.module = "heap";
	ownership.arguments.push_back(
		{0, "parameter_index", "unresolved", 1U, 0U, false, nullptr, nullptr});
	ownership.arguments.push_back(
		{1, "parameter_index", "unresolved", 1U, 0U, false, nullptr, nullptr});
	attributes.attributes.push_back(ownership);
	auto no_attributes = input::observe_declaration(declaration);
	require(no_attributes && no_attributes->attributes_admitted &&
				no_attributes->attributes.empty(),
			"independent native empty attribute census unavailable");
	input::limits cancelled_observation;
	cancelled_observation.cancelled = []
	{
		return true;
	};
	auto observation_cancel = input::observe_declaration(declaration, cancelled_observation);
	require(!observation_cancel &&
				observation_cancel.error().code == "native.resource-input-cancelled",
			"empty native observation ignored cancellation");
	auto no_inputs = input::observe_statement(syntax);
	require(no_inputs && no_inputs->moves_admitted && no_inputs->addresses_admitted &&
				no_inputs->moves.empty() && no_inputs->addresses.empty(),
			"independent native empty statement census unavailable");
	auto statement_cancel = input::observe_statement(syntax, {}, cancelled_observation);
	require(!statement_cancel && statement_cancel.error().code == "native.resource-input-cancelled",
			"empty statement ignored cancellation");
	auto first = input::detach(attributes, bindings);
	if (!first)
		std::cerr << first.error().code << ':' << first.error().field << ':' << first.error().detail
				  << '\n';
	require(bool(first), "attribute detachment failed");
	require(first->rows.size() == 5 && first->summaries.size() == 1,
			"attribute occurrence cardinality lost");
	require(first->summaries[0].count == 2 && first->summaries[0].argument_count == 3,
			"attribute census lost");
	const sdk::detached_row* annotation{};
	std::vector<const sdk::detached_row*> parameter_rows;
	for (const auto& row : first->rows)
	{
		if (number(row, "argument_slot") == 0 && number(row, "attribute_ordinal") == 0)
			annotation = &row;
		if (string(row, "record_kind") == "argument" && number(row, "attribute_ordinal") == 1)
			parameter_rows.push_back(&row);
	}
	require(annotation, "annotation carrier missing");
	const auto& payload =
		std::get<std::vector<std::byte>>(*cell(*annotation, "annotation_payload").value);
	require(payload.size() == 14 && payload[6] == std::byte{}, "annotation bytes truncated");
	require(parameter_rows.size() == 2 &&
				string(*parameter_rows[0], "attribute") != string(*parameter_rows[1], "attribute"),
			"repeated formal slots coalesced");
	require(string(*parameter_rows[0], "carrier_attribute") ==
				string(*parameter_rows[1], "carrier_attribute"),
			"argument carrier reference lost");
	require(cell(*parameter_rows[0], "formal_declaration").state == sdk::cell_state::unknown,
			"unbound formal silently absent");
	auto second = input::detach(attributes, bindings);
	require(bool(second), "deterministic replay failed");
	for (std::size_t i = 0; i < first->rows.size(); ++i)
		require(first->rows[i].canonical_form() == second->rows[i].canonical_form(),
				"detachment nondeterministic");

	input::observations empty;
	empty.admitted_syntax = &syntax;
	empty.moves_admitted = empty.addresses_admitted = true;
	auto zero = input::detach(empty, bindings);
	require(zero && zero->rows.empty() && zero->summaries.size() == 2,
			"known-empty census dropped");
	require(zero->summaries[0].count == 0 && zero->summaries[0].ids.empty(),
			"empty is unavailable");
	input::address_observation endpoint;
	endpoint.carrier_syntax = &syntax;
	endpoint.kind = "assignment";
	endpoint.context = "unknown";
	endpoint.origin_state = "not_classified";
	endpoint.destination_state = "unbound";
	empty.addresses.push_back(endpoint);
	auto unbound = input::detach(empty, bindings);
	require(unbound && unbound->rows.size() == 1, "unbound factual endpoint dropped");
	require(cell(unbound->rows[0], "origin_declaration").state == sdk::cell_state::absent,
			"plain propagation acquired a referent");
	require(cell(unbound->rows[0], "destination_declaration").state == sdk::cell_state::unknown,
			"missing destination became absent");
	input::limits cancelled;
	cancelled.cancelled = []
	{
		return true;
	};
	auto cancel = input::detach(attributes, bindings, cancelled);
	require(!cancel && cancel.error().code == "native.resource-input-cancelled",
			"cancel not enforced");
	input::limits tiny;
	tiny.maximum_retained_bytes = 1;
	auto bounded = input::detach(attributes, bindings, tiny);
	require(!bounded && bounded.error().code == "native.resource-input-budget",
			"byte bound not enforced");
	tiny = {};
	tiny.maximum_fields = 1;
	auto fields = input::detach(attributes, bindings, tiny);
	require(!fields && fields.error().code == "native.resource-input-budget",
			"field bound not enforced");
	auto malformed = attributes;
	malformed.attributes[1].arguments[1].ordinal = 0;
	auto duplicate = input::detach(malformed, bindings);
	require(!duplicate && duplicate.error().code == "native.resource-input-observation",
			"duplicate argument occurrence admitted");
	auto generic = attributes;
	generic.attributes.clear();
	input::attribute_observation ordinary;
	ordinary.declaration = &declaration;
	ordinary.original_kind = 789;
	ordinary.spelling = "deprecated";
	generic.attributes.push_back(ordinary);
	auto unsupported = input::detach(generic, bindings);
	require(unsupported && unsupported->rows.size() == 1, "generic attribute excluded from census");
	require(string(unsupported->rows[0], "argument_state") == "unsupported" &&
				cell(unsupported->rows[0], "argument_count").state == sdk::cell_state::absent &&
				cell(unsupported->rows[0], "argument_ids").state == sdk::cell_state::absent &&
				!unsupported->summaries[0].argument_inventory_complete,
			"generic payload acquired closure");
	bindings.syntax = {};
	auto missing = input::detach(empty, bindings);
	require(!missing && missing.error().code == "native.resource-input-binding",
			"unknown identity admitted");
	std::cout << "original resource input detacher checks passed\n";
}
