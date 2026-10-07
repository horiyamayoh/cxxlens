#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <set>

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Tooling/Tooling.h>
#include <cxxlens/sdk/source_features.hpp>

#include "../../../src/sdk/query_result_internal.hpp"
#include "project_language_environment.hpp"
#include "project_source_feature_rows.hpp"

namespace native = cxxlens::detail::clang22;
namespace sdk = cxxlens::sdk;
namespace query = cxxlens::sdk::query;

namespace
{
	void require(bool condition, const char* message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}
	std::string string(const sdk::detached_row& row, std::string_view name)
	{
		return std::get<std::string>(
			*row.cells.at(row.descriptor_id + "." + std::string{name}).value);
	}
	std::uint64_t number(const sdk::detached_row& row, std::string_view name)
	{
		return std::get<std::uint64_t>(
			*row.cells.at(row.descriptor_id + "." + std::string{name}).value);
	}
	query::annotated_row annotate(const sdk::detached_row& row)
	{
		query::annotated_row result;
		result.presence = {"original-source-feature-test", {"debug"}};
		result.interpretation = "clang22";
		result.claim_contributors = {"original-native-feature"};
		result.provenance = {"original-native-observation"};
		result.producer_contracts = {{"source-feature-test", "semantic:original"}};
		result.contributor_guarantees = {
			{"exact", "original-feature", "compiler", {"schema_validated"}}};
		result.contributor_edges = {{result.claim_contributors.front(),
									 result.producer_contracts.front(),
									 result.provenance.front(),
									 result.contributor_guarantees.front(),
									 result.presence,
									 result.interpretation}};
		for (const auto& [name, value] : row.cells)
			result.values.emplace("output." + name.substr(row.descriptor_id.size() + 1U), value);
		return result;
	}
	sdk::detached_row
	supporting_row(std::string_view descriptor,
				   std::initializer_list<std::pair<std::string_view, sdk::detached_cell>> fields)
	{
		sdk::detached_row row;
		row.descriptor_id = descriptor;
		const auto descriptors = sdk::standard_relation_descriptors();
		const auto d = std::ranges::find(descriptors, descriptor, &sdk::relation_descriptor::id);
		require(d != descriptors.end(), "support descriptor missing");
		for (const auto& column : d->columns)
		{
			auto value = sdk::detached_cell::utf8("original-fixture");
			if (column.type.optional)
				value = sdk::detached_cell::absent(column.type);
			else if (column.type.scalar == sdk::scalar_kind::boolean)
				value = sdk::detached_cell::boolean(false);
			else if (column.type.scalar == sdk::scalar_kind::unsigned_integer)
				value = sdk::detached_cell::unsigned_integer(0U);
			else if (column.type.scalar == sdk::scalar_kind::digest)
				value = sdk::detached_cell::utf8(sdk::content_digest({}));
			else if (column.type.scalar == sdk::scalar_kind::set ||
					 column.type.scalar == sdk::scalar_kind::bytes)
				value = sdk::detached_cell::bytes({});
			else if (column.type.scalar == sdk::scalar_kind::closed_symbol)
				value = sdk::detached_cell::utf8("canonicalized");
			value.type = column.type;
			row.cells.emplace(row.descriptor_id + "." + column.name, std::move(value));
		}
		for (const auto& [field, value] : fields)
		{
			auto original = value;
			const auto key = row.descriptor_id + "." + std::string{field};
			original.type = row.cells.at(key).type;
			row.cells.at(key) = std::move(original);
		}
		return row;
	}
	constexpr std::string_view source = R"cpp(
[[deprecated]] int facility(int value = 3);
int global_value = facility();
struct Record { int field_value = facility(); };
template<class T> int dependent(T value) { return sizeof(value); }
void written() { (void)sizeof(facility()); }
)cpp";
} // namespace

int main()
{
	auto ast = clang::tooling::buildASTFromCodeWithArgs(
		std::string{source}, {"-std=c++23", "-Wno-deprecated-declarations"}, "source-features.cpp");
	require(bool(ast), "original AST unavailable");
	auto& context = ast->getASTContext();
	require(!native::observe_original_language_environment(context).freestanding,
			"actual hosted LangOptions changed");
	std::set<std::string, std::less<>> source_ids;
	std::map<std::string, std::pair<std::uint64_t, std::uint64_t>, std::less<>> source_geometry;
	native::source_feature_bindings bindings;
	bindings.compile_unit = "original-unit";
	bindings.source = [&](const native::original_source_feature_view& feature)
	{
		auto& manager = context.getSourceManager();
		const auto begin = manager.getExpansionLoc(feature.source.getBegin());
		const auto end = manager.getExpansionLoc(feature.source.getEnd());
		if (begin.isInvalid() || end.isInvalid() || !manager.isWrittenInSameFile(begin, end))
			return native::source_feature_source_binding{};
		const auto id = "original-span:" + std::to_string(manager.getFileOffset(begin)) + ":" +
			std::to_string(manager.getFileOffset(end));
		source_geometry.emplace(
			id, std::pair{manager.getFileOffset(begin), manager.getFileOffset(end)});
		return native::source_feature_source_binding{"original-file",
													 "original-snapshot",
													 *source_ids.insert(id).first,
													 manager.isInSystemHeader(begin)};
	};
	bindings.entity = [](const clang::NamedDecl& declaration) -> std::string_view
	{
		const auto* identifier = declaration.getIdentifier();
		return identifier && identifier->getName() == "facility" ? "original-facility" : "";
	};
	bindings.canonical_type = [](clang::QualType type) -> std::string_view
	{
		return type->isIntegerType() ? "original-integer-type" : "";
	};
	const native::source_feature_entered_file entered[]{{"original-file", "original-snapshot"}};
	const auto full = native::detach_original_source_features(context, bindings, entered, true);
	if (!full)
		std::cerr << full.error().code << ':' << full.error().field << ':' << full.error().detail
				  << '\n';
	require(bool(full), "original feature detachment failed");
	if (!full->traversal.completed || !full->source_complete)
	{
		std::cerr << "members=" << full->traversal.members << " work=" << full->operations
				  << " bytes=" << full->retained_bytes_bound
				  << " traversal=" << full->traversal.completed
				  << " frontier=" << full->traversal.frontier << '\n';
		for (const auto& row : full->rows)
			if (row.descriptor_id == "cc.source_feature.v1" &&
				string(row, "source_binding_state") == "unknown")
				std::cerr << "unbound " << string(row, "feature_class") << ':'
						  << string(row, "kind") << '\n';
	}
	require(full->traversal.completed && full->source_complete,
			"full original traversal not closed");
	std::size_t declarations{}, statements{}, attributes{}, types{}, calls{}, source_none{};
	std::map<std::uint64_t, std::size_t> repeated;
	bool global{}, field{}, parameter{}, dependent{}, unevaluated{};
	for (const auto& row : full->rows)
		if (row.descriptor_id == "cc.source_feature.v1")
		{
			const auto feature_class = string(row, "feature_class");
			declarations += feature_class == "declaration";
			statements += feature_class == "statement";
			attributes += feature_class == "attribute";
			types += feature_class == "type_location";
			calls += string(row, "call_binding_state") == "complete";
			source_none += string(row, "source_binding_state") == "none";
			++repeated[number(row, "original_node_ordinal")];
			global |= feature_class == "declaration" && string(row, "kind") == "Var";
			field |= feature_class == "declaration" && string(row, "kind") == "Field";
			parameter |= feature_class == "declaration" && string(row, "kind") == "ParmVar";
			dependent |= string(row, "kind") == "TemplateTypeParm";
			unevaluated |= string(row, "kind") == "UnaryExprOrTypeTraitExpr";
		}
	require(declarations && statements && attributes && types && calls,
			"original feature class or selected facility missing");
	require(global && field && parameter && dependent && unevaluated,
			"written global/default/field/template/unevaluated domain omitted");
	require(source_none, "implicit source-none observations omitted");
	require(std::ranges::any_of(repeated,
								[](const auto& entry)
								{
									return entry.second > 1U;
								}),
			"shared original node visits lost their independent occurrence identity");
	{
		constexpr std::array<std::string_view, 10> relations{"build.compile_unit.v1",
															 "source.file.v1",
															 "source.span.v1",
															 "cc.entity.v1",
															 "cc.declaration.v1",
															 "cc.declaration_inventory.v1",
															 "cc.type.v1",
															 "cc.syntax_node.v1",
															 "cc.source_feature.v1",
															 "cc.source_feature_inventory.v1"};
		std::array<std::vector<query::annotated_row>, 10> rows;
		const auto txt = [](std::string_view value)
		{
			return sdk::detached_cell::utf8(std::string{value});
		};
		rows[0].push_back(annotate(supporting_row(
			"build.compile_unit.v1",
			{{"compile_unit", txt("original-unit")},
			 {"freestanding",
			  sdk::detached_cell::boolean(
				  native::observe_original_language_environment(context).freestanding)},
			 {"freestanding_state", txt("complete")},
			 {"freestanding_profile", txt("clang22-original-language-environment/1")}})));
		rows[1].push_back(annotate(
			supporting_row("source.file.v1",
						   {{"file", txt("original-file")},
							{"snapshot", txt("original-snapshot")},
							{"size", sdk::detached_cell::unsigned_integer(source.size())}})));
		for (const auto& [id, geometry] : source_geometry)
			rows[2].push_back(annotate(
				supporting_row("source.span.v1",
							   {{"span", txt(id)},
								{"file", txt("original-file")},
								{"snapshot", txt("original-snapshot")},
								{"begin", sdk::detached_cell::unsigned_integer(geometry.first)},
								{"end", sdk::detached_cell::unsigned_integer(geometry.second)}})));
		for (const auto& row : full->rows)
			rows[row.descriptor_id == relations[8] ? 8U : 9U].push_back(annotate(row));
		query::application_query_results queries;
		queries.snapshot_id = "original-source-features";
		for (std::size_t index{}; index < rows.size(); ++index)
		{
			auto data = std::make_shared<query::query_result::data>();
			data->row_values = rows[index];
			data->status = query::execution_status::complete;
			data->input_complete = false;
			queries.scans.push_back({std::string{relations[index]},
									 {},
									 query::query_transfer_access::make(std::move(data))});
		}
		query::projection_resource_usage usage;
		const auto observed = query::project_source_features(queries, {}, {}, usage);
		if (!observed)
			std::cerr << observed.error().code << ':' << observed.error().field << ':'
					  << observed.error().detail << '\n';
		require(bool(observed) && observed->features.size() == full->traversal.members &&
					observed->populations.size() == 2U && usage.operations &&
					usage.retained_bytes_bound,
				"native observations failed public measured projection");
		for (const auto& population : observed->populations)
			require(population.enumeration_state == query::finite_population_state::complete &&
						population.membership_state == query::finite_population_state::complete &&
						population.source_state == query::finite_population_state::complete &&
						population.entry_state == query::finite_population_state::complete,
					"native original census or exact source bindings failed public projection");
		require(std::ranges::any_of(observed->features,
									[](const auto& value)
									{
										return value.call_binding_state == "complete" &&
											value.call_state !=
											query::finite_population_state::complete;
									}),
				"missing optional target did not retain an independent binding frontier");
	}
	const auto replay = native::detach_original_source_features(context, bindings, entered, true);
	require(bool(replay) && replay->rows.size() == full->rows.size(),
			"original deterministic replay failed");
	for (std::size_t index{}; index < full->rows.size(); ++index)
		require(full->rows[index].canonical_form() == replay->rows[index].canonical_form(),
				"original occurrence identity changed");
	native::source_feature_row_limits limited;
	limited.traversal.maximum_members = 2U;
	const auto partial =
		native::detach_original_source_features(context, bindings, entered, true, limited);
	require(bool(partial) && !partial->traversal.completed && partial->traversal.members == 2U,
			"member frontier lost partial inventory");
	limited = {};
	limited.maximum_retained_bytes = 256U * 1024U;
	const auto storage =
		native::detach_original_source_features(context, bindings, entered, true, limited);
	require(bool(storage) && !storage->traversal.completed &&
				storage->retained_bytes_bound <= limited.maximum_retained_bytes,
			"storage frontier unbounded");
	limited = {};
	limited.traversal.maximum_operations = 256U * 1024U;
	const auto work =
		native::detach_original_source_features(context, bindings, entered, true, limited);
	require(bool(work) && work->operations <= limited.traversal.maximum_operations,
			"shared traversal/row work charge exceeded cap");
	limited = {};
	limited.traversal.cancelled = [](void*) noexcept
	{
		return true;
	};
	const auto cancelled =
		native::detach_original_source_features(context, bindings, entered, true, limited);
	require(bool(cancelled) && !cancelled->traversal.completed &&
				cancelled->traversal.members == 0U,
			"cancelled traversal fabricated known-empty membership");
	auto unbound_bindings = bindings;
	unbound_bindings.source = {};
	const auto unbound =
		native::detach_original_source_features(context, unbound_bindings, entered, true);
	require(bool(unbound) && unbound->traversal.completed && !unbound->source_complete,
			"valid unbound source incorrectly closed entered-file zero");
	const auto no_entry = native::detach_original_source_features(context, bindings, {}, false);
	require(bool(no_entry) && no_entry->traversal.completed && !no_entry->source_complete,
			"missing original entry census closed file domain");
	auto freestanding = clang::tooling::buildASTFromCodeWithArgs(
		"int value;", {"-std=c++23", "-ffreestanding"}, "freestanding.cpp");
	require(bool(freestanding) &&
				native::observe_original_language_environment(freestanding->getASTContext())
					.freestanding,
			"actual freestanding LangOptions missing");
	std::cout << "original source feature controls passed: " << full->traversal.members
			  << " members\n";
}
