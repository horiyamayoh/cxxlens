#include "llvm/clang22/project_exceptional_exit_rows.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

#include <cxxlens/relations/cc_exceptional_exit.hpp>

namespace native = cxxlens::detail::clang22;
namespace sdk = cxxlens::sdk;
namespace
{
	void require(bool value, std::string_view message)
	{
		if (!value)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}
	native::project_exceptional_exit_observations original()
	{
		native::project_exceptional_exit_observations value;
		value.frozen = value.hooks_installed = true;
		native::exceptional_physical_scope scope;
		scope.owner_usr = "c:@F@one#";
		scope.detail = "detail:one";
		scope.function = "entity:one";
		scope.definition_source = "span:definition";
		scope.body_id = "body:one";
		scope.complete = scope.admitted = true;
		native::exceptional_lowering_variant variant;
		variant.kind = "function";
		variant.symbol = "_Z3onev";
		variant.complete = true;
		scope.variants.push_back(std::move(variant));
		value.scopes.push_back(std::move(scope));
		return value;
	}
	std::string_view text(const sdk::detached_row& row, std::string_view name)
	{
		return std::get<std::string>(
			*row.cells.at("cc.exceptional_exit.v1." + std::string{name}).value);
	}
} // namespace
int main()
{
	native::exceptional_exit_row_bindings bindings{"unit:one",
												   [](native::exceptional_source)
												   {
													   return "span:occurrence";
												   }};
	auto input = original();
	auto empty = native::detach_project_exceptional_exits(input, bindings);
	require(empty.has_value(), "original empty variant detaches");
	require(empty->rows.size() == 1U && text(empty->rows.front(), "role") == "lowering_variant",
			"known empty lowering retains its original excluded carrier");
	require(std::get<std::uint64_t>(
				*empty->scopes.front().fields.at("exceptional_exit_count").value) == 1U,
			"finite count includes original variant carriers, independently of metric eligibility");
	native::exceptional_occurrence occurrence;
	occurrence.role = "written_throw";
	occurrence.eligibility = "eligible";
	occurrence.expression = "syntax:throw";
	occurrence.source = {1U, 2U, false};
	occurrence.original_expression_ordinal = 0U;
	input.scopes.front().variants.front().occurrences.push_back(occurrence);
	occurrence.role = "escaping_call";
	occurrence.source = {};
	occurrence.expression.clear();
	occurrence.target = "entity:target";
	occurrence.target_usr = std::string{"usr\0bytes", 9U};
	occurrence.block_ordinal = 2U;
	occurrence.instruction_ordinal = 4U;
	occurrence.cleanup_declaration = "declaration:actual-local";
	occurrence.cleanup_registration_ordinal = 1U;
	occurrence.cleanup_emission_ordinal = 2U;
	occurrence.cleanup_route = "exceptional";
	occurrence.cleanup_profile = "clang22-destroy-object-cleanup-emission/1";
	occurrence.cleanup_target = "entity:cleanup-dtor";
	occurrence.cleanup_target_usr = std::string{"dtor\0\xff", 6U};
	occurrence.cleanup_target_dtor_type = 0U;
	occurrence.cleanup_target_profile = "clang22-destructor-emission-target/1";
	input.scopes.front().variants.front().occurrences.push_back(occurrence);
	auto detached = native::detach_project_exceptional_exits(input, bindings);
	require(detached && detached->rows.size() == 3U,
			"all original roles detach without source deduplication");
	const auto& descriptor = cxxlens::cc::relations::exceptional_exit::descriptor();
	for (const auto& row : detached->rows)
	{
		require(sdk::validate_row(descriptor, row).has_value(), "original row wire types validate");
		require(sdk::validate_domain_identity(descriptor, row).has_value(),
				"original row identity validates");
	}
	require(text(detached->rows[1U], "variant") == text(detached->rows.front(), "exit"),
			"occurrence references its actual variant carrier");
	require(text(detached->rows[2U], "observation_state") == "complete" &&
				detached->rows[2U].cells.at("cc.exceptional_exit.v1.expression").state ==
					sdk::cell_state::absent,
			"original IR membership stays independent from missing AST binding");
	require(std::get<std::vector<std::byte>>(
				*detached->rows[2U].cells.at("cc.exceptional_exit.v1.target_usr").value)
					.size() == 9U,
			"target USR retains exact original byte framing");
	require(text(detached->rows[2U], "cleanup_declaration") == "declaration:actual-local" &&
				text(detached->rows[2U], "cleanup_route") == "exceptional" &&
				text(detached->rows[2U], "cleanup_target") == "entity:cleanup-dtor",
			"actual cleanup origin, route, and independent emitter target detach");
	const auto& cleanup_usr = std::get<std::vector<std::byte>>(
		*detached->rows[2U].cells.at("cc.exceptional_exit.v1.cleanup_target_usr").value);
	require(cleanup_usr.size() == 6U && cleanup_usr[4U] == std::byte{0} &&
				cleanup_usr[5U] == std::byte{0xff} &&
				std::get<std::uint64_t>(
					*detached->rows[2U]
						 .cells.at("cc.exceptional_exit.v1.cleanup_target_dtor_type")
						 .value) == 0U,
			"cleanup target raw bytes and original zero enum value remain lossless");
	require(detached->rows[1U].cells.at("cc.exceptional_exit.v1.cleanup_declaration").state ==
					sdk::cell_state::absent &&
				detached->rows[1U].cells.at("cc.exceptional_exit.v1.cleanup_target").state ==
					sdk::cell_state::absent,
			"missing original cleanup fields remain absent on independent occurrences");
	input.scopes.front().detail.clear();
	auto unbound = native::detach_project_exceptional_exits(input, bindings);
	require(
		unbound && unbound->rows.empty() && unbound->unbound_scopes.size() == 1U,
		"missing physical definition binding stays a frontier without invented hard references");
	input = original();
	input.hooks_installed = false;
	input.scopes.front().complete = false;
	input.scopes.front().variants.clear();
	auto stock = native::detach_project_exceptional_exits(input, bindings);
	require(stock &&
				std::get<std::string>(
					*stock->scopes.front().fields.at("exceptional_exit_state").value) ==
					"unsupported",
			"stock compiler population is unsupported, never known empty");
	input = original();
	native::exceptional_exit_limits limits;
	limits.maximum_retained_bytes = 1U;
	require(!native::detach_project_exceptional_exits(input, bindings, limits),
			"storage guard precedes retained rows");
	limits = {};
	limits.maximum_occurrences = 0U;
	require(!native::detach_project_exceptional_exits(input, bindings, limits),
			"variant carriers count toward occurrence quota");
	limits = {};
	limits.cancelled = []
	{
		return true;
	};
	auto cancelled = native::detach_project_exceptional_exits(input, bindings, limits);
	require(!cancelled && cancelled.error().code == "native.exceptional-exit-cancelled",
			"active detachment cancellation is retained");
	std::cout << "exceptional original detachment/type/identity/variant/frontier/bounds PASS\n";
}
