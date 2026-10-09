#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <locale>
#include <memory>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>

#include <cxxlens/sdk.hpp>
#include <cxxlens/sdk/abi_surfaces.hpp>

#include "../../../src/sdk/query_result_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;
	namespace q = cxxlens::sdk::query;
	constexpr std::array<std::string_view, 6> relations{"build.compile_unit.v1",
														"source.file.v1",
														"source.span.v1",
														"cc.entity.v1",
														"cc.entity_detail.v1",
														"cc.abi_surface.v1"};
	void require(bool value, std::string_view message)
	{
		if (!value)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}
	template <class T>
	T take(result<T> value)
	{
		if (!value)
		{
			std::cerr << value.error().code << ':' << value.error().field << ':'
					  << value.error().detail << '\n';
			std::exit(1);
		}
		return std::move(*value);
	}
	void take(result<void> value)
	{
		if (!value)
		{
			std::cerr << value.error().code << ':' << value.error().field << ':'
					  << value.error().detail << '\n';
			std::exit(1);
		}
	}
	q::annotated_row row(std::string_view relation,
						 std::initializer_list<std::pair<std::string, detached_cell>> values)
	{
		q::annotated_row result;
		result.presence = {"abi:test", {"debug"}};
		result.interpretation = "clang22";
		result.claim_contributors = {"claim:test"};
		result.producer_contracts = {
			{"abi.test", take(semantic_digest("abi.fixture", "contract"))}};
		result.provenance = {"abi:evidence"};
		result.contributor_guarantees = {
			{"exact", "finite-local-interface", "fixture", {"schema_validated"}}};
		result.contributor_edges = {{result.claim_contributors.front(),
									 result.producer_contracts.front(),
									 result.provenance.front(),
									 result.contributor_guarantees.front(),
									 result.presence,
									 result.interpretation}};
		const auto descriptors = standard_relation_descriptors();
		const auto descriptor = std::ranges::find(descriptors, relation, &relation_descriptor::id);
		require(descriptor != descriptors.end(), "missing relation");
		for (const auto& column : descriptor->columns)
		{
			auto cell = detached_cell::utf8("fixture");
			if (column.type.optional)
				cell = detached_cell::absent(column.type);
			else if (column.type.scalar == scalar_kind::boolean)
				cell = detached_cell::boolean(false);
			else if (column.type.scalar == scalar_kind::unsigned_integer)
				cell = detached_cell::unsigned_integer(0U);
			else if (column.type.scalar == scalar_kind::digest)
				cell = detached_cell::utf8(content_digest({}));
			else if (column.type.scalar == scalar_kind::closed_symbol)
				cell = detached_cell::utf8("canonicalized");
			else if (column.type.scalar == scalar_kind::set ||
					 column.type.scalar == scalar_kind::bytes)
				cell = detached_cell::bytes({});
			cell.type = column.type;
			result.values.emplace("output." + column.name, std::move(cell));
		}
		for (const auto& [name, value] : values)
		{
			auto actual = value;
			actual.type = result.values.at("output." + name).type;
			result.values.insert_or_assign("output." + name, std::move(actual));
		}
		return result;
	}
	void replace(q::annotated_row& row, std::string name, detached_cell value)
	{
		value.type = row.values.at("output." + name).type;
		row.values["output." + name] = std::move(value);
	}
	std::vector<std::byte> extents(std::initializer_list<q::abi_byte_extent> values)
	{
		std::vector<std::byte> result;
		for (const auto& value : values)
			for (const auto endpoint : {value.begin, value.end})
				for (unsigned shift{}; shift < 64U; shift += 8U)
					result.push_back(static_cast<std::byte>((endpoint >> shift) & 255U));
		return result;
	}
	std::string fingerprint(std::string_view context, const std::vector<std::byte>& signature)
	{
		std::string payload;
		for (unsigned shift{}; shift < 64U; shift += 8U)
			payload.push_back(static_cast<char>((context.size() >> shift) & 255U));
		payload += context;
		for (unsigned shift{}; shift < 64U; shift += 8U)
			payload.push_back(static_cast<char>((signature.size() >> shift) & 255U));
		for (const auto byte : signature)
			payload.push_back(std::to_integer<char>(byte));
		return take(semantic_digest("cc.clang22.abi-surface.v1", payload));
	}
	struct fixture
	{
		std::array<std::vector<q::annotated_row>, 6> groups;
		fixture()
		{
			groups[0] = {row(relations[0], {{"compile_unit", detached_cell::utf8("tu:test")}})};
			groups[1] = {row(relations[1],
							 {{"snapshot", detached_cell::utf8("snapshot:test")},
							  {"file", detached_cell::utf8("file:test")},
							  {"size", detached_cell::unsigned_integer(100U)}})};
			groups[2] = {row(relations[2],
							 {{"span", detached_cell::utf8("span:test")},
							  {"snapshot", detached_cell::utf8("snapshot:test")},
							  {"file", detached_cell::utf8("file:test")},
							  {"end", detached_cell::unsigned_integer(100U)}})};
			groups[3] = {row(relations[3],
							 {{"entity", detached_cell::utf8("record:test")},
							  {"kind", detached_cell::utf8("struct")}})};
			groups[4] = {row(relations[4],
							 {{"entity", detached_cell::utf8("record:test")},
							  {"compile_unit", detached_cell::utf8("tu:test")},
							  {"source", detached_cell::utf8("span:test")},
							  {"is_definition", detached_cell::boolean(true)}})};
			const auto context =
				take(semantic_digest("cc.clang22.abi-context.v1", "hand-labelled-target"));
			const std::vector<std::byte> signature{std::byte{1}, std::byte{0}, std::byte{42}};
			groups[5] = {
				row(relations[5],
					{{"surface", detached_cell::utf8("abi:record")},
					 {"compile_unit", detached_cell::utf8("tu:test")},
					 {"entity", detached_cell::utf8("record:test")},
					 {"source", detached_cell::utf8("span:test")},
					 {"kind", detached_cell::utf8("record")},
					 {"profile", detached_cell::utf8("clang22-storage-and-call-interface/1")},
					 {"abi_state", detached_cell::utf8("complete")},
					 {"layout_state", detached_cell::utf8("complete")},
					 {"byte_size", detached_cell::unsigned_integer(8U)},
					 {"byte_alignment", detached_cell::unsigned_integer(4U)},
					 {"occupied_ranges", detached_cell::bytes(extents({{0U, 1U}, {4U, 8U}}))},
					 {"abi_context", detached_cell::utf8(context)},
					 {"abi_signature", detached_cell::bytes(signature)},
					 {"abi_fingerprint", detached_cell::utf8(fingerprint(context, signature))}})};
		}
		q::abi_surface_input input(bool complete = true) const
		{
			return {groups[0],
					groups[1],
					groups[2],
					groups[3],
					groups[4],
					groups[5],
					complete,
					complete,
					complete};
		}
	};
	q::abi_surface_projection project(const fixture& data)
	{
		return take(q::project_abi_surfaces(data.input()));
	}
	bool gap(const q::abi_surface& value, std::string_view name)
	{
		return std::ranges::any_of(value.gaps,
								   [&](const auto& gap)
								   {
									   return gap.code == name;
								   });
	}
	void state_is(const q::abi_surface& value,
				  q::abi_surface_state abi,
				  q::abi_surface_state layout,
				  std::string_view message)
	{
		require(value.abi_state == abi && value.layout_state == layout, message);
	}

	void portability_facet_tests()
	{
		fixture data;
		auto& original = data.groups[5].front();
		replace(original, "target_data_model_state", detached_cell::utf8("complete"));
		replace(original,
				"target_data_model_profile",
				detached_cell::utf8("clang22-original-target-data-model/1"));
		replace(original, "long_width_bits", detached_cell::unsigned_integer(64U));
		replace(original, "pointer_width_bits", detached_cell::unsigned_integer(64U));
		replace(original, "wchar_width_bits", detached_cell::unsigned_integer(32U));
		replace(original, "plain_char_signed", detached_cell::boolean(false));
		replace(original, "byte_order", detached_cell::utf8("little"));
		replace(original, "packing_state", detached_cell::utf8("complete"));
		replace(original,
				"packing_profile",
				detached_cell::utf8("clang22-original-record-packing-attributes/1"));
		replace(original, "packed_attribute", detached_cell::boolean(false));
		replace(original, "maximum_field_alignment_bits", detached_cell::unsigned_integer(8U));
		replace(original, "packing_applied", detached_cell::boolean(true));
		auto actual = project(data);
		const auto& value = actual.surfaces.front();
		require(value.target_data_model_state == q::abi_surface_state::complete &&
					value.long_width_bits == 64U && value.pointer_width_bits == 64U &&
					value.wchar_width_bits == 32U && value.plain_char_signed == false &&
					value.byte_order == "little",
				"actual target model facet not retained");
		require(value.packing_state == q::abi_surface_state::complete &&
					value.packed_attribute == false && value.maximum_field_alignment_bits == 8U &&
					value.packing_applied == true,
				"actual packing facet not retained");
		state_is(value,
				 q::abi_surface_state::complete,
				 q::abi_surface_state::complete,
				 "portability facets changed old axes");
		auto missing = data;
		replace(
			missing.groups[5][0],
			"long_width_bits",
			detached_cell::absent(missing.groups[5][0].values.at("output.long_width_bits").type));
		actual = project(missing);
		require(actual.surfaces[0].target_data_model_state == q::abi_surface_state::partial &&
					actual.surfaces[0].abi_state == q::abi_surface_state::complete &&
					actual.surfaces[0].packing_state == q::abi_surface_state::complete,
				"missing target width poisoned independent ABI/packing");
		missing = data;
		replace(missing.groups[5][0], "target_data_model_profile", detached_cell::utf8("future/1"));
		actual = project(missing);
		require(actual.surfaces[0].target_data_model_state == q::abi_surface_state::unknown &&
					actual.surfaces[0].abi_state == q::abi_surface_state::complete,
				"unsupported target profile forged complete");
		missing = data;
		replace(missing.groups[5][0], "packing_applied", detached_cell::boolean(false));
		actual = project(missing);
		require(actual.surfaces[0].packing_state == q::abi_surface_state::conflicting &&
					actual.surfaces[0].layout_state == q::abi_surface_state::complete,
				"packing contradiction lost or poisoned old layout");
		missing = data;
		auto conflicting = missing.groups[5][0];
		replace(conflicting, "long_width_bits", detached_cell::unsigned_integer(32U));
		missing.groups[5].push_back(conflicting);
		actual = project(missing);
		require(actual.surfaces.size() == 2U, "target conflict candidate lost");
		for (const auto& candidate : actual.surfaces)
			require(candidate.target_data_model_state == q::abi_surface_state::conflicting &&
						candidate.abi_state == q::abi_surface_state::complete &&
						candidate.layout_state == q::abi_surface_state::complete,
					"target conflict poisoned independent ABI/layout");
		auto absent = project(fixture{});
		require(absent.surfaces[0].target_data_model_state == q::abi_surface_state::unknown &&
					absent.surfaces[0].packing_state == q::abi_surface_state::unknown,
				"legacy facets inferred known");
	}
	void optional_bitfield_compatibility_tests()
	{
		constexpr std::array<std::string_view, 5> columns{"field_bitfield_profile",
														  "field_bitfield_state",
														  "field_is_bitfield",
														  "field_bit_width",
														  "field_bitfield_reason"};
		fixture data;
		const auto current = project(data);
		for (auto& detail : data.groups[4])
			for (const auto column : columns)
				detail.values.erase("output." + std::string{column});
		q::projection_resource_usage usage;
		const auto sparse = take(q::project_abi_surfaces(data.input(), {}, {}, usage));
		require(sparse.surfaces.size() == 1U &&
					sparse.surfaces.front().abi_state == current.surfaces.front().abi_state &&
					sparse.surfaces.front().layout_state == current.surfaces.front().layout_state &&
					sparse.surfaces.front().byte_size == current.surfaces.front().byte_size,
				"unconsumed absent bit-field facts changed ABI/layout projection");
		const auto retained =
			std::ranges::find(sparse.evidence, relations[4], &q::abi_surface_evidence::relation_id);
		require(retained != sparse.evidence.end() &&
					retained->row.canonical_form() == data.groups[4].front().canonical_form(),
				"sparse original detail acquired invented bit-field cells");
		for (const auto column : columns)
		{
			auto malformed = data;
			malformed.groups[4].front().values.emplace(
				"output." + std::string{column}, detached_cell::utf8("wrong descriptor type"));
			const auto rejected = q::project_abi_surfaces(malformed.input());
			require(!rejected && rejected.error().code == "sdk.abi-input-invalid",
					"present mistyped bit-field fact escaped ABI input validation");
		}
		for (const auto column : {"detail", "canonical_type"})
		{
			auto missing = data;
			missing.groups[4].front().values.erase("output." + std::string{column});
			const auto rejected = q::project_abi_surfaces(missing.input());
			require(!rejected && rejected.error().code == "sdk.abi-input-invalid" &&
						rejected.error().detail == "column-missing",
					"bit-field compatibility relaxed an existing column check");
		}
		auto limits = q::abi_surface_limits{};
		limits.maximum_operations = usage.operations;
		limits.maximum_retained_bytes = usage.retained_bytes_bound;
		require(q::project_abi_surfaces(data.input(), limits).has_value(),
				"sparse ABI inputs rejected their charged resource bound");
		--limits.maximum_operations;
		const auto work_failure = q::project_abi_surfaces(data.input(), limits);
		require(!work_failure && work_failure.error().code == "sdk.abi-budget",
				"sparse ABI inputs escaped the work bound");
		++limits.maximum_operations;
		--limits.maximum_retained_bytes;
		const auto byte_failure = q::project_abi_surfaces(data.input(), limits);
		require(!byte_failure && byte_failure.error().code == "sdk.abi-budget",
				"sparse ABI inputs escaped the retained storage bound");
		std::stop_source stopped;
		stopped.request_stop();
		const auto cancelled = q::project_abi_surfaces(data.input(), {}, stopped.get_token());
		require(!cancelled && cancelled.error().code == "sdk.abi-cancelled",
				"sparse ABI inputs escaped cancellation");
	}
	void positive_tests()
	{
		fixture data;
		auto actual = project(data);
		require(actual.surfaces.size() == 1U && actual.evidence.size() == 6U,
				"finite object or evidence lost");
		const auto& value = actual.surfaces.front();
		state_is(value,
				 q::abi_surface_state::complete,
				 q::abi_surface_state::complete,
				 "complete facets lost");
		require(value.byte_size == 8U && value.byte_alignment == 4U && value.occupied_bytes == 5U &&
					value.padding_bytes == 3U,
				"union occupied storage padding incorrect");
		require(value.file == "file:test" && value.source_snapshot == "snapshot:test" &&
					value.evidence.size() == 6U && value.gaps.empty(),
				"actual declaration binding or local completeness lost");
		auto raw = data.input(false);
		auto local = take(q::project_abi_surfaces(raw));
		state_is(local.surfaces.front(),
				 q::abi_surface_state::complete,
				 q::abi_surface_state::complete,
				 "global closure poisoned local interface");
		require(!local.unresolved.empty() && !local.compile_units_complete &&
					!local.abi_inputs_complete,
				"raw defaults invented closure");
		for (const auto size : {1U, 5U})
		{
			replace(data.groups[5].front(), "byte_size", detached_cell::unsigned_integer(size));
			replace(data.groups[5].front(), "byte_alignment", detached_cell::unsigned_integer(1U));
			replace(data.groups[5].front(),
					"occupied_ranges",
					detached_cell::bytes(extents({{0U, size}})));
			const auto packed = project(data);
			require(packed.surfaces.front().padding_bytes == 0U,
					"empty or packed occupied storage gained padding");
		}
		data = fixture{};
		replace(data.groups[5].front(), "byte_size", detached_cell::unsigned_integer(0U));
		replace(data.groups[5].front(), "byte_alignment", detached_cell::unsigned_integer(1U));
		replace(data.groups[5].front(), "occupied_ranges", detached_cell::bytes({}));
		const auto zero_sized = project(data);
		state_is(zero_sized.surfaces.front(),
				 q::abi_surface_state::complete,
				 q::abi_surface_state::complete,
				 "observed GNU zero-sized storage was discarded");
		require(zero_sized.surfaces.front().byte_size == 0U &&
					zero_sized.surfaces.front().occupied_bytes == 0U &&
					zero_sized.surfaces.front().padding_bytes == 0U,
				"zero-sized extension gained positive storage");
		data = fixture{};
		replace(data.groups[3].front(), "kind", detached_cell::utf8("function"));
		replace(data.groups[4].front(), "is_definition", detached_cell::boolean(false));
		replace(data.groups[5].front(), "kind", detached_cell::utf8("function"));
		replace(data.groups[5].front(), "layout_state", detached_cell::utf8("unknown"));
		for (const auto name : {"byte_size", "byte_alignment", "occupied_ranges"})
			replace(data.groups[5].front(),
					name,
					detached_cell::absent(
						data.groups[5].front().values.at("output." + std::string{name}).type));
		replace(
			data.groups[5].front(), "reason", detached_cell::utf8("function-has-no-object-layout"));
		const auto function = project(data);
		state_is(function.surfaces.front(),
				 q::abi_surface_state::complete,
				 q::abi_surface_state::unknown,
				 "function declaration ABI depended on body or object layout");
		require(!function.surfaces.front().padding_bytes &&
					function.surfaces.front().abi_signature->size() == 3U,
				"function padding invented or binary signature lost");
	}
	void negative_tests()
	{
		fixture data;
		for (const auto values : {std::initializer_list<q::abi_byte_extent>{{0U, 2U}, {1U, 3U}},
								  std::initializer_list<q::abi_byte_extent>{{0U, 1U}, {1U, 3U}},
								  std::initializer_list<q::abi_byte_extent>{{4U, 8U}, {0U, 1U}},
								  std::initializer_list<q::abi_byte_extent>{{0U, 9U}},
								  std::initializer_list<q::abi_byte_extent>{{3U, 3U}}})
		{
			data = fixture{};
			replace(
				data.groups[5].front(), "occupied_ranges", detached_cell::bytes(extents(values)));
			const auto result = project(data);
			state_is(result.surfaces.front(),
					 q::abi_surface_state::complete,
					 q::abi_surface_state::conflicting,
					 "storage contradiction poisoned ABI or escaped validation");
			require(!result.surfaces.front().padding_bytes &&
						gap(result.surfaces.front(), "sdk.abi-layout-extents-conflicting"),
					"invalid extents produced padding");
		}
		data = fixture{};
		replace(data.groups[5].front(), "occupied_ranges", detached_cell::bytes({std::byte{0}}));
		auto malformed = q::project_abi_surfaces(data.input());
		require(!malformed && malformed.error().code == "sdk.abi-input-invalid" &&
					malformed.error().field == "occupied_ranges",
				"truncated extent accepted");
		data = fixture{};
		replace(data.groups[5].front(), "abi_fingerprint", detached_cell::utf8(content_digest({})));
		state_is(project(data).surfaces.front(),
				 q::abi_surface_state::conflicting,
				 q::abi_surface_state::complete,
				 "bad ABI digest poisoned complete object storage");
		data = fixture{};
		replace(data.groups[5].front(), "profile", detached_cell::utf8("future-storage/1"));
		const auto unsupported = project(data);
		state_is(unsupported.surfaces.front(),
				 q::abi_surface_state::unknown,
				 q::abi_surface_state::unknown,
				 "unsupported profile proved storage");
		require(!unsupported.surfaces.front().padding_bytes &&
					gap(unsupported.surfaces.front(), "sdk.abi-profile-unsupported"),
				"unsupported profile lacked frontier");
		for (const auto group : {0U, 1U, 2U, 3U, 4U})
		{
			data = fixture{};
			data.groups[group].clear();
			state_is(project(data).surfaces.front(),
					 q::abi_surface_state::partial,
					 q::abi_surface_state::partial,
					 "missing declaration binding proved interface");
		}
		data = fixture{};
		replace(data.groups[3].front(), "kind", detached_cell::utf8("variable"));
		state_is(project(data).surfaces.front(),
				 q::abi_surface_state::conflicting,
				 q::abi_surface_state::conflicting,
				 "nonrecord entity gained object ABI");
		data = fixture{};
		replace(data.groups[2].front(), "end", detached_cell::unsigned_integer(101U));
		state_is(project(data).surfaces.front(),
				 q::abi_surface_state::conflicting,
				 q::abi_surface_state::conflicting,
				 "out of snapshot source bound");
		data = fixture{};
		replace(data.groups[4].front(), "is_definition", detached_cell::boolean(false));
		state_is(project(data).surfaces.front(),
				 q::abi_surface_state::conflicting,
				 q::abi_surface_state::conflicting,
				 "forward record acquired complete layout");
		replace(data.groups[5].front(), "layout_state", detached_cell::utf8("unknown"));
		const auto false_interface = project(data);
		state_is(false_interface.surfaces.front(),
				 q::abi_surface_state::conflicting,
				 q::abi_surface_state::unknown,
				 "forward record acquired complete interface through unknown layout");
		require(gap(false_interface.surfaces.front(), "sdk.abi-definition-conflicting"),
				"missing definition interface conflict lost");
		data = fixture{};
		replace(data.groups[5].front(), "byte_alignment", detached_cell::unsigned_integer(0U));
		state_is(project(data).surfaces.front(),
				 q::abi_surface_state::complete,
				 q::abi_surface_state::conflicting,
				 "zero object alignment accepted");
		data = fixture{};
		replace(data.groups[5].front(), "byte_size", detached_cell::unsigned_integer(10U));
		state_is(project(data).surfaces.front(),
				 q::abi_surface_state::complete,
				 q::abi_surface_state::conflicting,
				 "misaligned C++ object size accepted");
		data = fixture{};
		replace(data.groups[5].front(), "abi_signature", detached_cell::bytes({}));
		require(project(data).surfaces.front().abi_state == q::abi_surface_state::conflicting,
				"empty complete interface accepted");
		data = fixture{};
		replace(data.groups[5].front(), "abi_state", detached_cell::utf8("unknown"));
		replace(
			data.groups[5].front(),
			"abi_signature",
			detached_cell::absent(data.groups[5].front().values.at("output.abi_signature").type));
		state_is(project(data).surfaces.front(),
				 q::abi_surface_state::conflicting,
				 q::abi_surface_state::complete,
				 "unpaired ABI payload escaped schema invariant through unknown state");
		data = fixture{};
		replace(data.groups[5].front(), "layout_state", detached_cell::utf8("unknown"));
		replace(
			data.groups[5].front(),
			"byte_alignment",
			detached_cell::absent(data.groups[5].front().values.at("output.byte_alignment").type));
		state_is(project(data).surfaces.front(),
				 q::abi_surface_state::complete,
				 q::abi_surface_state::conflicting,
				 "unpaired storage payload escaped schema invariant through unknown state");
		data = fixture{};
		data.groups[5].front().values.erase("output.kind");
		auto missing = q::project_abi_surfaces(data.input());
		require(!missing && missing.error().code == "sdk.abi-input-invalid",
				"missing required column accepted");
	}
	void candidate_and_condition_tests()
	{
		fixture data;
		data.groups[5].push_back(data.groups[5].front());
		replace(
			data.groups[5].back(), "occupied_ranges", detached_cell::bytes(extents({{0U, 8U}})));
		const auto candidates = project(data);
		require(candidates.surfaces.size() == 2U, "candidate first wins");
		for (const auto& value : candidates.surfaces)
			state_is(value,
					 q::abi_surface_state::complete,
					 q::abi_surface_state::conflicting,
					 "layout candidates poisoned matching ABI");
		for (auto& group : data.groups)
			std::ranges::reverse(group);
		const auto reverse = project(data);
		require(reverse.evidence.size() == candidates.evidence.size() &&
					reverse.surfaces.size() == candidates.surfaces.size(),
				"candidate reorder changed population");
		for (std::size_t index{}; index < reverse.evidence.size(); ++index)
			require(reverse.evidence[index].row.canonical_form() ==
						candidates.evidence[index].row.canonical_form(),
					"canonical evidence changed on reorder");
		for (std::size_t index{}; index < reverse.surfaces.size(); ++index)
			require(reverse.surfaces[index].occupied_ranges ==
							candidates.surfaces[index].occupied_ranges &&
						reverse.surfaces[index].evidence == candidates.surfaces[index].evidence &&
						reverse.surfaces[index].gaps == candidates.surfaces[index].gaps,
					"surface order, evidence or frontier changed on reorder");
		data = fixture{};
		for (auto& group : data.groups)
		{
			auto other = group.front();
			other.presence.fragments = {"release"};
			other.contributor_edges.front().condition = other.presence;
			group.push_back(std::move(other));
		}
		replace(data.groups[5].back(), "byte_size", detached_cell::unsigned_integer(12U));
		const auto variants = project(data);
		require(variants.surfaces.size() == 2U && variants.surfaces[0].variant == "debug" &&
					variants.surfaces[1].variant == "release",
				"observed variants collapsed");
		for (const auto& value : variants.surfaces)
			state_is(value,
					 q::abi_surface_state::complete,
					 q::abi_surface_state::complete,
					 "world-specific storage disagreed globally");
		data.groups[0].pop_back();
		const auto absent_unit = project(data);
		require(absent_unit.surfaces[0].abi_state == q::abi_surface_state::complete &&
					absent_unit.surfaces[1].abi_state == q::abi_surface_state::partial,
				"debug unit joined release observation");
		data = fixture{};
		auto detail = data.groups[4].front();
		replace(detail, "compile_unit", detached_cell::utf8("tu:other"));
		data.groups[4] = {std::move(detail)};
		state_is(project(data).surfaces.front(),
				 q::abi_surface_state::partial,
				 q::abi_surface_state::partial,
				 "other TU declaration bound interface");
		data = fixture{};
		data.groups[3].push_back(data.groups[3].front());
		replace(data.groups[3].back(), "kind", detached_cell::utf8("function"));
		state_is(project(data).surfaces.front(),
				 q::abi_surface_state::conflicting,
				 q::abi_surface_state::conflicting,
				 "contradictory entity was first wins");
		data = fixture{};
		data.groups[4].push_back(data.groups[4].front());
		replace(data.groups[4].back(), "canonical_type", detached_cell::utf8("type:other"));
		state_is(project(data).surfaces.front(),
				 q::abi_surface_state::conflicting,
				 q::abi_surface_state::conflicting,
				 "contradictory declaration payload was first wins");
		data = fixture{};
		for (const auto group : {0U, 4U, 5U})
		{
			data.groups[group].push_back(data.groups[group].front());
			replace(data.groups[group].back(), "compile_unit", detached_cell::utf8("tu:other"));
		}
		const auto reused_id = project(data);
		require(reused_id.surfaces.size() == 2U, "reused ID dropped conditioned identity");
		for (const auto& value : reused_id.surfaces)
			state_is(value,
					 q::abi_surface_state::conflicting,
					 q::abi_surface_state::conflicting,
					 "surface ID reused across compile units was first wins");
	}
	q::application_query_results
	queries(const fixture& data, std::string_view omitted = {}, std::string_view uncovered = {})
	{
		relation_registry registry;
		for (const auto& descriptor : standard_relation_descriptors())
			take(registry.add(descriptor));
		auto engine = take(registry.build("abi-public-query-fixture"));
		auto store = take(make_in_memory_snapshot_store(engine));
		const auto digest = content_digest({});
		auto writer = take(store.begin({{"catalog:abi-fixture",
										 "tests",
										 std::string{engine.generation()},
										 "abi:test",
										 std::string{engine.registry_digest()},
										 digest,
										 digest},
										{1U, 0U, 0U},
										digest,
										{}}));
		std::set<std::pair<std::string, std::vector<std::string>>> conditions{
			{"clang22", {"debug"}}};
		for (const auto& group : data.groups)
			for (const auto& value : group)
				conditions.emplace(value.interpretation, value.presence.fragments);
		for (std::size_t group{}; group < relations.size(); ++group)
		{
			const auto relation = relations[group];
			if (relation == omitted)
				continue;
			std::size_t number{};
			for (const auto& [interpretation, fragments] : conditions)
			{
				partition_draft partition;
				partition.relation_descriptor_id = relation;
				partition.scope = "fixture:" + std::to_string(number++);
				partition.condition = {"abi:test", fragments};
				partition.interpretation = interpretation;
				partition.producer_semantics = digest;
				partition.producer_input_basis_digest =
					take(claim_input_basis_digest(direct_claim_basis{digest}));
				partition.precision_profile = "exact";
				partition.assumption_set_id = "assumptions:none";
				partition.coverage = {{partition.scope,
									   "fixture",
									   relation == uncovered ? "not_covered" : "covered",
									   relation == uncovered ? "fixture-domain-unavailable" : ""}};
				const auto descriptor = take(engine.require_id(relation));
				for (const auto& value : data.groups[group])
				{
					if (value.interpretation != interpretation ||
						value.presence.fragments != fragments)
						continue;
					detached_row detached;
					detached.descriptor_id = relation;
					for (const auto& column : descriptor.descriptor().columns)
						detached.cells.emplace(column.id, value.values.at("output." + column.name));
					partition.claims.push_back(take(make_assertion(engine,
																   {std::move(detached),
																	value.presence,
																	value.interpretation,
																	{"abi.public-fixture", digest},
																	{digest},
																	"fixture:hand-labelled",
																	{"exact",
																	 "finite-observed-interface",
																	 "assumptions:none",
																	 {"schema_validated"}}})));
				}
				take(writer.stage(std::move(partition)));
			}
		}
		take(writer.validate());
		auto snapshot = take(writer.publish());
		auto query_engine = take(q::reference_engine::bind(snapshot));
		q::application_query_results result;
		result.snapshot_id = snapshot.id();
		for (const auto relation : relations)
		{
			if (relation == omitted)
				continue;
			const auto descriptor = take(engine.require_id(relation));
			auto builder = take(q::builder::from(descriptor.descriptor()));
			auto ir = std::move(builder).finish();
			auto result_scan = take(query_engine.execute(ir));
			result.scans.push_back({std::string{relation}, std::move(ir), std::move(result_scan)});
		}
		return result;
	}
	void public_query_tests()
	{
		fixture data;
		const auto bundle = queries(data);
		const auto actual = take(q::project_abi_surfaces(bundle));
		require(actual.compile_units_complete && actual.abi_inputs_complete &&
					actual.source_queries && actual.source_queries->scans.size() == 6U,
				"public scans lost independent completion or source bundle");
		state_is(actual.surfaces.front(),
				 q::abi_surface_state::complete,
				 q::abi_surface_state::complete,
				 "public query failed local binding");
		require(actual.surfaces.front().padding_bytes == 3U, "public query padding differs");
		for (std::size_t index{}; index < bundle.scans.size(); ++index)
			require(actual.source_queries->scans[index].result.canonical_form() ==
						bundle.scans[index].result.canonical_form(),
					"query side channels changed");
		const auto missing = take(q::project_abi_surfaces(queries(data, relations[5])));
		require(missing.surfaces.empty() && missing.compile_units_complete &&
					!missing.abi_inputs_complete && !missing.unresolved.empty(),
				"missing ABI scan became known empty population");
		const auto unbound = take(q::project_abi_surfaces(queries(data, relations[0])));
		require(!unbound.compile_units_complete && unbound.abi_inputs_complete &&
					unbound.surfaces.front().abi_state == q::abi_surface_state::partial,
				"missing compile unit scan poisoned ABI scan completion or bound rows");
		auto partial_bundle = queries(data, {}, relations[5]);
		const auto partial = take(q::project_abi_surfaces(partial_bundle));
		require(partial.compile_units_complete && !partial.abi_inputs_complete &&
					!partial.unresolved.empty(),
				"uncovered ABI domain became complete input");
		state_is(partial.surfaces.front(),
				 q::abi_surface_state::complete,
				 q::abi_surface_state::complete,
				 "relation-wide uncovered domain poisoned retained local storage");
		partial_bundle.scans.push_back(bundle.scans.back());
		const auto mixed = take(q::project_abi_surfaces(partial_bundle));
		require(!mixed.abi_inputs_complete && mixed.source_queries->scans.size() == 7U,
				"duplicate complete scan erased independent incomplete scan");
		auto limits = q::abi_surface_limits{};
		limits.maximum_source_queries = 1U;
		auto count = q::project_abi_surfaces(bundle, limits);
		require(!count && count.error().field == "source-queries", "query count budget escaped");
		limits = {};
		limits.maximum_source_plan_bytes = 1U;
		auto plans = q::project_abi_surfaces(bundle, limits);
		require(!plans && plans.error().field == "source-plan-bytes", "source plan bytes escaped");
	}
	void fault_tests()
	{
		fixture data;
		for (const auto field : {&q::abi_surface_limits::maximum_rows,
								 &q::abi_surface_limits::maximum_condition_expansions,
								 &q::abi_surface_limits::maximum_evidence_bytes,
								 &q::abi_surface_limits::maximum_retained_bytes,
								 &q::abi_surface_limits::maximum_evidence_references,
								 &q::abi_surface_limits::maximum_extents,
								 &q::abi_surface_limits::maximum_operations})
		{
			auto limits = q::abi_surface_limits{};
			limits.*field = 1U;
			auto limited = q::project_abi_surfaces(data.input(), limits);
			require(!limited && limited.error().code == "sdk.abi-budget",
					"projection budget escaped");
		}
		auto limits = q::abi_surface_limits{};
		limits.maximum_rows = 0U;
		auto invalid = q::project_abi_surfaces(data.input(), limits);
		require(!invalid && invalid.error().code == "sdk.abi-limit-invalid", "zero limit accepted");
		std::stop_source cancelled;
		cancelled.request_stop();
		auto stopped = q::project_abi_surfaces(data.input(), {}, cancelled.get_token());
		require(!stopped && stopped.error().code == "sdk.abi-cancelled",
				"cancelled projection proceeded");
		data.groups[5].push_back(data.groups[5].front());
		replace(
			data.groups[5].back(), "occupied_ranges", detached_cell::bytes(extents({{0U, 8U}})));
		limits = {};
		limits.maximum_surfaces = 1U;
		auto surfaces = q::project_abi_surfaces(data.input(), limits);
		require(!surfaces && surfaces.error().field == "surfaces",
				"candidate population budget escaped");
	}
	void measured_usage_tests()
	{
		fixture data;
		q::projection_resource_usage usage{999U, 999U};
		const auto raw = take(q::project_abi_surfaces(data.input(), {}, {}, usage));
		const auto raw_usage = usage;
		require(usage.operations > 0U && usage.retained_bytes_bound > 0U &&
					usage.operations < q::abi_surface_limits{}.maximum_operations &&
					usage.retained_bytes_bound < q::abi_surface_limits{}.maximum_retained_bytes,
				"successful ABI usage reports charged resources");
		const auto bundle = queries(data);
		const auto owned = take(q::project_abi_surfaces(bundle, {}, {}, usage));
		require(usage.operations > raw_usage.operations &&
					usage.retained_bytes_bound > raw_usage.retained_bytes_bound &&
					owned.source_queries.has_value() &&
					owned.surfaces.front().padding_bytes == raw.surfaces.front().padding_bytes,
				"measured query includes source plans and temporary pointer indexes");
		auto limits = q::abi_surface_limits{};
		limits.maximum_operations = usage.operations;
		limits.maximum_retained_bytes = usage.retained_bytes_bound;
		require(q::project_abi_surfaces(bundle, limits, {}, usage).has_value(),
				"exact successful charge can be reused");
		--limits.maximum_retained_bytes;
		usage = {999U, 999U};
		require(!q::project_abi_surfaces(bundle, limits, {}, usage) && usage.operations == 0U &&
					usage.retained_bytes_bound == 0U,
				"retained failure leaves measured usage zero");
		std::stop_source cancelled;
		cancelled.request_stop();
		usage = {999U, 999U};
		require(!q::project_abi_surfaces(data.input(), {}, cancelled.get_token(), usage) &&
					usage.operations == 0U && usage.retained_bytes_bound == 0U,
				"cancelled raw ABI usage remains zero");
		limits = {};
		limits.maximum_source_plan_bytes = 1U;
		usage = {999U, 999U};
		require(!q::project_abi_surfaces(bundle, limits, {}, usage) && usage.operations == 0U &&
					usage.retained_bytes_bound == 0U,
				"failed source-plan bound leaves measured usage zero");
		q::application_query_results empty;
		empty.snapshot_id = "empty-original";
		const auto missing = take(q::project_abi_surfaces(empty, {}, {}, usage));
		require(usage.operations > 0U &&
					usage.retained_bytes_bound >=
						missing.unresolved.size() * sizeof(q::query_unresolved) &&
					missing.unresolved.size() == 7U && missing.source_queries.has_value(),
				"empty query charges its original handle and independent missing-scan gaps");
		limits = {};
		limits.maximum_retained_bytes = usage.retained_bytes_bound;
		require(q::project_abi_surfaces(empty, limits, {}, usage).has_value(),
				"empty query may reuse exact charged storage bound");
		std::cout << "ABI measured usage 8 focused checks PASS\n";
	}

	auto surface_fields(const q::abi_surface& value)
	{
		return std::tie(value.id,
						value.entity,
						value.compile_unit,
						value.source_span,
						value.file,
						value.source_snapshot,
						value.kind,
						value.profile,
						value.universe,
						value.variant,
						value.interpretation,
						value.abi_state,
						value.layout_state,
						value.byte_size,
						value.byte_alignment,
						value.occupied_bytes,
						value.padding_bytes,
						value.occupied_ranges,
						value.abi_context,
						value.abi_fingerprint,
						value.abi_signature,
						value.target_data_model_state,
						value.target_data_model_profile,
						value.byte_order,
						value.long_width_bits,
						value.pointer_width_bits,
						value.wchar_width_bits,
						value.plain_char_signed,
						value.packing_state,
						value.packing_profile,
						value.packed_attribute,
						value.packing_applied,
						value.maximum_field_alignment_bits,
						value.evidence,
						value.gaps);
	}
	bool same_raw_row(const q::annotated_row& a, const q::annotated_row& z)
	{
		const auto producer = [](const auto& value)
		{
			return std::tie(value.id, value.semantic_contract);
		};
		const auto guarantee = [](const auto& value)
		{
			return std::tie(
				value.approximation, value.scope, value.assumptions, value.verification_modalities);
		};
		return a.multiplicity == z.multiplicity && a.presence.universe == z.presence.universe &&
			a.presence.fragments == z.presence.fragments && a.interpretation == z.interpretation &&
			a.claim_contributors == z.claim_contributors && a.provenance == z.provenance &&
			std::ranges::equal(a.values,
							   z.values,
							   [](const auto& x, const auto& y)
							   {
								   return x.first == y.first && x.second.type == y.second.type &&
									   x.second.state == y.second.state &&
									   x.second.value == y.second.value &&
									   x.second.unknown_reason == y.second.unknown_reason;
							   }) &&
			std::ranges::equal(a.producer_contracts,
							   z.producer_contracts,
							   [&](const auto& x, const auto& y)
							   {
								   return producer(x) == producer(y);
							   }) &&
			std::ranges::equal(a.contributor_guarantees,
							   z.contributor_guarantees,
							   [&](const auto& x, const auto& y)
							   {
								   return guarantee(x) == guarantee(y);
							   }) &&
			std::ranges::equal(a.contributor_edges,
							   z.contributor_edges,
							   [&](const auto& x, const auto& y)
							   {
								   return x.claim_contributor == y.claim_contributor &&
									   producer(x.producer) == producer(y.producer) &&
									   x.provenance == y.provenance &&
									   guarantee(x.guarantee) == guarantee(y.guarantee) &&
									   x.condition.universe == y.condition.universe &&
									   x.condition.fragments == y.condition.fragments &&
									   x.interpretation == y.interpretation;
							   });
	}
	void same_abi(const q::abi_surface_projection& expected,
				  const q::abi_surface_projection& actual,
				  bool compare_queries = true)
	{
		require(expected.compile_units_complete == actual.compile_units_complete &&
					expected.abi_inputs_complete == actual.abi_inputs_complete &&
					expected.unresolved == actual.unresolved &&
					expected.surfaces.size() == actual.surfaces.size() &&
					expected.evidence.size() == actual.evidence.size(),
				"shared ABI changed populations, closure or unresolved metadata");
		for (std::size_t i{}; i < expected.surfaces.size(); ++i)
			require(surface_fields(expected.surfaces[i]) == surface_fields(actual.surfaces[i]),
					"shared ABI changed a complete typed surface or its evidence order");
		for (std::size_t i{}; i < expected.evidence.size(); ++i)
			require(expected.evidence[i].relation_id == actual.evidence[i].relation_id &&
						same_raw_row(expected.evidence[i].original_row(),
									 actual.evidence[i].original_row()) &&
						expected.evidence[i].original_row().canonical_form() ==
							actual.evidence[i].original_row().canonical_form(),
					"shared ABI changed a complete original row or annotation");
		if (compare_queries)
		{
			require(
				expected.source_queries && actual.source_queries &&
					expected.source_queries->snapshot_id == actual.source_queries->snapshot_id &&
					expected.source_queries->scans.size() == actual.source_queries->scans.size(),
				"shared ABI lost its independent source queries");
			for (std::size_t i{}; i < expected.source_queries->scans.size(); ++i)
			{
				const auto& a = expected.source_queries->scans[i];
				const auto& z = actual.source_queries->scans[i];
				require(a.relation_id == z.relation_id &&
							a.logical_ir.canonical_form() == z.logical_ir.canonical_form() &&
							a.result.canonical_form() == z.result.canonical_form(),
						"shared ABI dropped original plans or query side channels");
			}
		}
	}
	std::string bundle_wire(const q::application_query_results& input)
	{
		std::string wire =
			"{\"schema\":\"cxxlens.application-query-results.v1\",\"snapshot_id\":\"" +
			input.snapshot_id + "\",\"queries\":[";
		for (std::size_t i{}; i < input.scans.size(); ++i)
		{
			if (i)
				wire += ',';
			const auto& scan = input.scans[i];
			wire += "{\"relation_id\":\"" + scan.relation_id +
				"\",\"logical_ir\":" + scan.logical_ir.canonical_form() +
				",\"result\":" + scan.result.canonical_form() + '}';
		}
		return wire + "]}";
	}
	relation_engine abi_engine()
	{
		relation_registry registry;
		for (const auto& descriptor : standard_relation_descriptors())
			take(registry.add(descriptor));
		return take(registry.build("abi-public-query-fixture"));
	}
	void canonical_evidence_oracle(const q::application_query_results& input,
								   const q::abi_surface_projection& actual)
	{
		std::vector<std::pair<std::size_t, std::string>> complete_rows;
		for (const auto& scan : input.scans)
		{
			const auto relation = std::ranges::find(relations, scan.relation_id);
			require(relation != relations.end(), "canonical oracle requires a known scan");
			const auto originals = scan.result.readonly_rows();
			for (const auto& original : originals.rows())
				complete_rows.emplace_back(static_cast<std::size_t>(relation - relations.begin()),
										   original.canonical_form());
		}
		// The unchanged full public row writer supplies the old complete ordering key.
		std::ranges::sort(complete_rows);
		require(complete_rows.size() == actual.evidence.size(),
				"prefix sorting omitted duplicates or original evidence");
		for (std::size_t i{}; i < complete_rows.size(); ++i)
			require(actual.evidence[i].relation_id == relations[complete_rows[i].first] &&
						actual.evidence[i].original_row().canonical_form() ==
							complete_rows[i].second,
					"first-claim prefix order differs from the complete canonical row oracle");
	}
	q::application_query_results prefix_queries()
	{
		auto input = queries(fixture{});
		const std::vector<std::vector<std::string>> claims{{"claim:a"},
														   {"claim:aa"},
														   {"claim:a\""},
														   {"claim:a", "claim:z"},
														   {"claim:\\line\n"},
														   {"claim:日本😀"},
														   {"claim:tie"},
														   {"claim:tie"},
														   {"claim:tie"}};
		for (auto& scan : input.scans)
		{
			const auto old = q::query_transfer_access::borrow_evidence_owner(scan.result);
			require(!scan.result.readonly_rows().rows_validated(),
					"actual native query does not supply decoded validation or row-size facts");
			auto data = std::make_shared<q::query_result::data>(*old.owner);
			const auto original = data->row_values.front();
			data->row_values.clear();
			for (std::size_t i{}; i < claims.size(); ++i)
			{
				auto row = original;
				row.claim_contributors = claims[i];
				row.provenance = {i == 6U ? "evidence:z-late" : "evidence:a-late"};
				row.multiplicity = 1234567U;
				row.contributor_edges.clear();
				for (const auto& claim : row.claim_contributors)
					row.contributor_edges.push_back({claim,
													 row.producer_contracts.front(),
													 row.provenance.front(),
													 row.contributor_guarantees.front(),
													 row.presence,
													 row.interpretation});
				take(row.validate());
				data->row_values.push_back(std::move(row));
			}
			std::ranges::reverse(data->row_values);
			data->ordered = false;
			data->closures.push_back(
				take(semantic_digest("abi.fixture.closure", "original-source-sidechannel")));
			data->unresolved.push_back(
				{"abi-original-gap", "abi-original-subject", "abi-original-reason"});
			data->guarantee.approximation = "unknown";
			data->physical.text += "\nABI original physical explanation";
			scan.result = q::query_transfer_access::make(std::move(data));
		}
		std::ranges::sort(input.scans, {}, &q::application_relation_scan::relation_id);
		return input;
	}
	void immutable_prefix_tests()
	{
		const auto previous = std::locale();
		struct restore_locale
		{
			std::locale previous;
			~restore_locale()
			{
				std::locale::global(previous);
			}
		} restored{previous};
		std::locale::global(std::locale::classic());
		const auto engine = abi_engine();
		auto native = prefix_queries();
		auto input = take(q::decode_application_queries(engine, bundle_wire(native)));
		q::abi_surface_limits shared;
		shared.evidence_ownership = q::projection_evidence_ownership::shared_immutable;
		for (const auto& scan : input.scans)
			require(scan.result.readonly_rows().rows_validated() &&
						q::query_transfer_access::borrow_row_sizes(scan.result).rows.size() == 9U,
					"real transfer decode supplies complete immutable validation and size facts");
		q::projection_resource_usage detached_usage, shared_usage;
		auto detached = take(q::project_abi_surfaces(input, {}, {}, detached_usage));
		auto alias = take(q::project_abi_surfaces(input, shared, {}, shared_usage));
		same_abi(detached, alias);
		canonical_evidence_oracle(input, alias);
		std::cout << "ABI mixed-prefix fixture work detached=" << detached_usage.operations
				  << " shared=" << shared_usage.operations << '\n';
		std::size_t evidence_bytes{};
		std::vector<const q::annotated_row*> addresses;
		for (std::size_t i{}; i < alias.evidence.size(); ++i)
		{
			const auto& evidence = alias.evidence[i];
			bool exact{};
			for (const auto& scan : input.scans)
				if (scan.relation_id == evidence.relation_id)
				{
					const auto originals = scan.result.readonly_rows();
					for (const auto& row : originals.rows())
						exact |= &evidence.original_row() == &row;
				}
			require(exact && evidence.row.values.empty() &&
						&detached.evidence[i].original_row() == &detached.evidence[i].row &&
						!detached.evidence[i].row.values.empty(),
					"opt-in aliases exact immutable rows while default keeps public detached rows");
			addresses.push_back(&evidence.original_row());
			evidence_bytes += evidence.original_row().canonical_form().size();
		}
		for (unsigned bound{}; bound < 3U; ++bound)
			for (const bool under : {false, true})
			{
				auto cap = shared;
				const auto decrement = static_cast<std::size_t>(under);
				if (bound == 0U)
					cap.maximum_operations = shared_usage.operations - decrement;
				else if (bound == 1U)
					cap.maximum_retained_bytes = shared_usage.retained_bytes_bound - decrement;
				else
					cap.maximum_evidence_bytes = evidence_bytes - decrement;
				q::projection_resource_usage usage{999U, 999U};
				const auto bounded = q::project_abi_surfaces(input, cap, {}, usage);
				require(bool(bounded) == !under,
						"shared ABI honors exact and one-under work, peak storage and complete "
						"evidence");
				if (bounded)
					same_abi(detached, *bounded);
				else
					require(bounded.error().code == "sdk.abi-budget" && !usage.operations &&
								!usage.retained_bytes_bound,
							"failed shared ABI revokes all output ownership and successful usage");
			}
		const auto native_default = take(q::project_abi_surfaces(native));
		const auto native_shared = take(q::project_abi_surfaces(native, shared));
		same_abi(native_default, native_shared);
		same_abi(detached, native_shared);
		canonical_evidence_oracle(native, native_shared);
		auto no_sizes = input;
		for (auto& scan : no_sizes.scans)
		{
			const auto owner = q::query_transfer_access::borrow_evidence_owner(scan.result);
			auto data = std::make_shared<q::query_result::data>(*owner.owner);
			data->row_wire_base_sizes.clear();
			scan.result = q::query_transfer_access::make(std::move(data));
		}
		same_abi(detached, take(q::project_abi_surfaces(no_sizes, shared)));
		fixture raw;
		q::projection_resource_usage raw_default_usage, raw_shared_usage;
		const auto raw_default =
			take(q::project_abi_surfaces(raw.input(), {}, {}, raw_default_usage));
		const auto raw_shared =
			take(q::project_abi_surfaces(raw.input(), shared, {}, raw_shared_usage));
		same_abi(raw_default, raw_shared, false);
		require(
			raw_default_usage.operations == raw_shared_usage.operations &&
				raw_default_usage.retained_bytes_bound == raw_shared_usage.retained_bytes_bound,
			"raw spans retain the complete detached fallback and its original resource charges");
		for (const auto& evidence : raw_shared.evidence)
			require(&evidence.original_row() == &evidence.row && !evidence.row.values.empty(),
					"shared-mode request never aliases mutable raw-span inputs");
		std::vector<std::weak_ptr<const q::query_result::data>> owners;
		for (const auto& scan : input.scans)
			owners.push_back(q::query_transfer_access::borrow_evidence_owner(scan.result).owner);
		auto copied = alias;
		copied.source_queries.reset();
		alias.source_queries.reset();
		detached.source_queries.reset();
		input = {};
		auto moved = std::move(alias);
		same_abi(detached, moved, false);
		same_abi(detached, copied, false);
		for (std::size_t i{}; i < moved.evidence.size(); ++i)
			require(&moved.evidence[i].original_row() == addresses[i] &&
						&copied.evidence[i].original_row() == addresses[i],
					"copied/moved ABI evidence outlives original input and source_queries");
		for (const auto& owner : owners)
			require(!owner.expired(), "shared original remains live through evidence-only owners");
		moved = {};
		copied = {};
		for (const auto& owner : owners)
			require(owner.expired(), "last ABI alias releases its exact immutable query backing");
		std::cout << "ABI immutable evidence and canonical prefix controls PASS\n";
	}

	template <class Change>
	q::application_query_results changed_native_query(const q::application_query_results& input,
													  std::size_t group,
													  Change change)
	{
		auto output = input;
		const auto selected = std::ranges::find(
			output.scans, relations[group], &q::application_relation_scan::relation_id);
		require(selected != output.scans.end(), "native fault requires its exact relation scan");
		auto& scan = *selected;
		require(!scan.result.readonly_rows().rows_validated(),
				"mutable fault fixture must retain native false validation proof");
		const auto original = q::query_transfer_access::borrow_evidence_owner(scan.result);
		auto data = std::make_shared<q::query_result::data>(*original.owner);
		change(*data);
		scan.result = q::query_transfer_access::make(std::move(data));
		return output;
	}
	void immutable_guard_tests()
	{
		const auto engine = abi_engine();
		auto original = queries(fixture{});
		std::ranges::sort(original.scans, {}, &q::application_relation_scan::relation_id);
		q::abi_surface_limits shared;
		shared.evidence_ownership = q::projection_evidence_ownership::shared_immutable;
		for (unsigned fault{}; fault < 13U; ++fault)
		{
			const std::array<std::size_t, 13> groups{
				0U, 5U, 5U, 5U, 4U, 2U, 1U, 4U, 4U, 4U, 3U, 5U, 5U};
			const auto changed = changed_native_query(
				original,
				groups[fault],
				[&](auto& data)
				{
					auto& late = data.row_values.back();
					switch (fault)
					{
						case 0U:
							data.row_values.clear();
							break;
						case 1U:
							replace(late, "entity", detached_cell::utf8("record:foreign"));
							break;
						case 2U:
							replace(late, "source", detached_cell::utf8("span:foreign"));
							break;
						case 3U:
							replace(late, "compile_unit", detached_cell::utf8("tu:foreign"));
							break;
						case 4U:
							replace(late, "is_definition", detached_cell::boolean(false));
							break;
						case 5U:
							replace(late, "end", detached_cell::unsigned_integer(101U));
							break;
						case 6U:
							replace(late, "snapshot", detached_cell::utf8("snapshot:foreign"));
							break;
						case 7U:
							late.presence.universe = "abi:foreign";
							break;
						case 8U:
							late.presence.fragments = {"release"};
							break;
						case 9U:
							late.interpretation = "clang:foreign";
							break;
						case 10U:
							replace(late, "kind", detached_cell::utf8("variable"));
							break;
						case 11U:
							replace(late, "profile", detached_cell::utf8("future-storage/1"));
							break;
						case 12U:
							replace(
								late, "abi_fingerprint", detached_cell::utf8(content_digest({})));
							break;
					}
					if (fault >= 7U && fault <= 9U)
						for (auto& edge : late.contributor_edges)
						{
							edge.condition = late.presence;
							edge.interpretation = late.interpretation;
						}
					// Empty exact scans without original closure cannot claim an exact summary.
					if (fault == 0U)
						data.guarantee.approximation = "unknown";
				});
			const auto expected = take(q::project_abi_surfaces(changed));
			require(expected.surfaces.front().abi_state != q::abi_surface_state::complete ||
						expected.surfaces.front().layout_state != q::abi_surface_state::complete,
					"original World/FK/type/profile/witness defect must remain noncomplete");
			same_abi(expected, take(q::project_abi_surfaces(changed, shared)));
			const auto decoded = take(q::decode_application_queries(engine, bundle_wire(changed)));
			same_abi(expected, take(q::project_abi_surfaces(decoded, shared)));
		}
		for (unsigned fault{}; fault < 4U; ++fault)
		{
			const auto changed = changed_native_query(
				original,
				5U,
				[&](auto& data)
				{
					data.row_values.push_back(data.row_values.front());
					auto& late = data.row_values.back();
					replace(late, "surface", detached_cell::utf8("abi:late-foreign"));
					if (fault == 0U)
						late.values.at("output.kind") = detached_cell::boolean(true);
					else if (fault == 1U)
						late.values.at("output.kind").value = std::string{"\xed\xa0\x80", 3U};
					else if (fault == 2U)
						late.values.at("output.kind").value = std::string{"record\0late", 11U};
					else
						replace(late, "occupied_ranges", detached_cell::bytes({std::byte{0}}));
				});
			for (const auto mode : {q::projection_evidence_ownership::detached,
									q::projection_evidence_ownership::shared_immutable})
			{
				auto cap = shared;
				cap.evidence_ownership = mode;
				q::projection_resource_usage usage{999U, 999U};
				const auto invalid = q::project_abi_surfaces(changed, cap, {}, usage);
				require(
					!invalid && !usage.operations && !usage.retained_bytes_bound,
					"an early foreign surface never hides a late malformed scalar, UTF8 or extent");
			}
			if (fault < 2U)
				require(!q::decode_application_queries(engine, bundle_wire(changed)),
						"public decode cannot forge validated proof for malformed raw rows");
		}
		const auto invalid_claim =
			changed_native_query(original,
								 0U,
								 [&](auto& data)
								 {
									 auto& late = data.row_values.back();
									 late.claim_contributors = {std::string(8192U, 'x') + '\xff'};
									 late.contributor_edges.front().claim_contributor =
										 late.claim_contributors.front();
									 take(late.validate());
								 });
		const auto old_invalid_claim = take(q::project_abi_surfaces(invalid_claim));
		const auto shared_invalid_claim = take(q::project_abi_surfaces(invalid_claim, shared));
		same_abi(old_invalid_claim, shared_invalid_claim);
		canonical_evidence_oracle(invalid_claim, shared_invalid_claim);
		require(!q::decode_application_queries(engine, bundle_wire(invalid_claim)),
				"native false proof preserves old invalid-claim encoder semantics without decoder "
				"admission");
		for (const auto omitted : {relations[0], relations[5]})
		{
			const auto input = queries(fixture{}, omitted);
			same_abi(take(q::project_abi_surfaces(input)),
					 take(q::project_abi_surfaces(input, shared)));
		}
		const auto uncovered = queries(fixture{}, {}, relations[5]);
		same_abi(take(q::project_abi_surfaces(uncovered)),
				 take(q::project_abi_surfaces(uncovered, shared)));
		auto invalid_mode = shared;
		invalid_mode.evidence_ownership = static_cast<q::projection_evidence_ownership>(255U);
		q::projection_resource_usage usage{999U, 999U};
		require(!q::project_abi_surfaces(original, invalid_mode, {}, usage) && !usage.operations &&
					!usage.retained_bytes_bound,
				"unsupported ownership mode cannot publish output or successful usage");
		std::cout << "ABI immutable input guards PASS\n";
	}
	struct grouped_multiplicity final : std::numpunct<char>
	{
		char do_thousands_sep() const override
		{
			return ',';
		}
		std::string do_grouping() const override
		{
			return "\3";
		}
	};
	struct observed_multiplicity final : std::num_put<char>
	{
		using std::num_put<char>::do_put;
		std::size_t& visits;
		std::size_t stop_at;
		std::stop_source* cancellation;
		observed_multiplicity(std::size_t& observed, std::size_t at, std::stop_source* stop)
			: visits(observed), stop_at(at), cancellation(stop)
		{
		}
		void observe(std::uint64_t value) const
		{
			if (value == 1234567U && ++visits == stop_at && cancellation)
				cancellation->request_stop();
		}
		iter_type do_put(iter_type output,
						 std::ios_base& stream,
						 char_type fill,
						 unsigned long value) const override
		{
			observe(value);
			return std::num_put<char>::do_put(output, stream, fill, value);
		}
		iter_type do_put(iter_type output,
						 std::ios_base& stream,
						 char_type fill,
						 unsigned long long value) const override
		{
			observe(value);
			return std::num_put<char>::do_put(output, stream, fill, value);
		}
	};
	void immutable_locale_and_stop_tests()
	{
		const auto previous = std::locale();
		struct restore_locale
		{
			std::locale previous;
			~restore_locale()
			{
				std::locale::global(previous);
			}
		} restored{previous};
		std::locale::global(std::locale::classic());
		const auto input =
			take(q::decode_application_queries(abi_engine(), bundle_wire(prefix_queries())));
		q::abi_surface_limits shared;
		shared.evidence_ownership = q::projection_evidence_ownership::shared_immutable;
		const auto grouped = std::locale{std::locale::classic(), new grouped_multiplicity};
		std::locale::global(grouped);
		const auto expected = take(q::project_abi_surfaces(input));
		const auto actual = take(q::project_abi_surfaces(input, shared));
		same_abi(expected, actual);
		canonical_evidence_oracle(input, actual);
		std::size_t evidence_bytes{};
		for (const auto& evidence : actual.evidence)
			evidence_bytes += evidence.original_row().canonical_form().size();
		for (const bool under : {false, true})
		{
			auto cap = shared;
			cap.maximum_evidence_bytes = evidence_bytes - static_cast<std::size_t>(under);
			q::projection_resource_usage usage{999U, 999U};
			const auto bounded = q::project_abi_surfaces(input, cap, {}, usage);
			require(bool(bounded) == !under,
					"decoded ABI row-size facts retain live locale multiplicity width");
			if (bounded)
				same_abi(expected, *bounded);
			else
				require(!usage.operations && !usage.retained_bytes_bound,
						"locale evidence failure revokes output ownership and usage");
		}
		std::size_t visits{};
		std::locale::global(std::locale{grouped, new observed_multiplicity{visits, 0U, nullptr}});
		q::projection_resource_usage measured;
		const auto counted = take(q::project_abi_surfaces(input, shared, {}, measured));
		std::locale::global(grouped);
		same_abi(expected, counted);
		require(visits > 20U, "real multiplicity callbacks must execute during ABI projection");
		for (const auto stop_at : {std::size_t{1U}, visits / 2U, visits - 1U})
		{
			std::size_t visited{};
			std::stop_source stop;
			std::locale::global(
				std::locale{grouped, new observed_multiplicity{visited, stop_at, &stop}});
			q::projection_resource_usage usage{999U, 999U};
			const auto interrupted =
				q::project_abi_surfaces(input, shared, stop.get_token(), usage);
			std::locale::global(grouped);
			require(!interrupted && interrupted.error().code == "sdk.abi-cancelled" &&
						stop.stop_requested() && visited >= stop_at && !usage.operations &&
						!usage.retained_bytes_bound,
					"stop requested by a real multiplicity operation revokes pre/mid/late output");
			q::projection_resource_usage retried;
			same_abi(expected, take(q::project_abi_surfaces(input, shared, {}, retried)));
			require(retried.operations == measured.operations &&
						retried.retained_bytes_bound == measured.retained_bytes_bound,
					"fresh ABI retry does not inherit cancelled ownership or work state");
		}
		std::stop_source pre;
		pre.request_stop();
		q::projection_resource_usage usage{999U, 999U};
		require(!q::project_abi_surfaces(input, shared, pre.get_token(), usage) &&
					!usage.operations && !usage.retained_bytes_bound,
				"shared ABI honors a pre-requested stop token");
		std::cout << "ABI locale evidence and real stop/retry controls PASS\n";
	}

} // namespace

int main()
{
	portability_facet_tests();
	optional_bitfield_compatibility_tests();
	positive_tests();
	negative_tests();
	candidate_and_condition_tests();
	public_query_tests();
	fault_tests();
	measured_usage_tests();
	immutable_prefix_tests();
	immutable_guard_tests();
	immutable_locale_and_stop_tests();
	return 0;
}
