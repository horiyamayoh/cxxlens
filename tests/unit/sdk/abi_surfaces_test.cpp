#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <set>
#include <utility>

#include <cxxlens/sdk.hpp>
#include <cxxlens/sdk/abi_surfaces.hpp>

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
	return 0;
}
