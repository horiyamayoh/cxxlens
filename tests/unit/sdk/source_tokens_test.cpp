#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <cxxlens/sdk/source_tokens.hpp>

namespace
{
	using namespace cxxlens::sdk;
	namespace q = cxxlens::sdk::query;
	void require(bool condition, std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}
	q::annotated_row row(std::initializer_list<std::pair<std::string, detached_cell>> values)
	{
		q::annotated_row result;
		result.presence = {"token:test", {"debug"}};
		result.interpretation = "clang22";
		result.provenance = {"token:witness"};
		for (const auto& [name, value] : values)
			result.values["output." + name] = value;
		return result;
	}
	struct fixture
	{
		std::vector<q::annotated_row> files, spans, inventories, tokens;
		fixture()
		{
			files.push_back(
				row({{"snapshot", detached_cell::typed("source_snapshot_id", "source:test")},
					 {"file", detached_cell::typed("file_id", "file:test")},
					 {"size", detached_cell::unsigned_integer(20U)}}));
			inventories.push_back(row(
				{{"inventory", detached_cell::typed("source_token_inventory_id", "inventory:test")},
				 {"compile_unit", detached_cell::typed("compile_unit_id", "tu:test")},
				 {"source_snapshot", detached_cell::typed("source_snapshot_id", "source:test")},
				 {"file", detached_cell::typed("file_id", "file:test")},
				 {"phase", detached_cell::utf8("raw")},
				 {"profile", detached_cell::utf8("tokens:test-v1")},
				 {"token_count", detached_cell::unsigned_integer(3U)},
				 {"directive_count", detached_cell::unsigned_integer(1U)},
				 {"enumeration_state", detached_cell::utf8("complete")},
				 {"active_state", detached_cell::utf8("complete")},
				 {"template_state", detached_cell::utf8("complete")}}));
			for (std::uint64_t index{}; index < 3U; ++index)
			{
				const auto id = "span:" + std::to_string(index);
				spans.push_back(
					row({{"span", detached_cell::typed("source_span_id", id)},
						 {"snapshot", detached_cell::typed("source_snapshot_id", "source:test")},
						 {"file", detached_cell::typed("file_id", "file:test")},
						 {"begin", detached_cell::unsigned_integer(index)},
						 {"end", detached_cell::unsigned_integer(index + 1U)},
						 {"role", detached_cell::utf8("raw_token")}}));
				tokens.push_back(row(
					{{"token",
					  detached_cell::typed("source_token_id", "token:" + std::to_string(index))},
					 {"compile_unit", detached_cell::typed("compile_unit_id", "tu:test")},
					 {"source", detached_cell::typed("source_span_id", id)},
					 {"phase", detached_cell::utf8("raw")},
					 {"profile", detached_cell::utf8("tokens:test-v1")},
					 {"kind", detached_cell::utf8(index == 0U ? "hash" : "identifier")},
					 {"spelling", detached_cell::utf8(index == 0U ? "#" : "name")},
					 {"ordinal", detached_cell::unsigned_integer(index)},
					 {"macro", detached_cell::boolean(false)},
					 {"active", detached_cell::boolean(false)},
					 {"template_context", detached_cell::boolean(false)},
					 {"preprocessor", detached_cell::boolean(true)},
					 {"directive_start", detached_cell::boolean(index == 0U)}}));
			}
		}
		q::source_token_input input() const
		{
			return {files, spans, inventories, tokens};
		}
	};
	q::source_token_projection project(const fixture& data)
	{
		auto result = q::project_source_tokens(data.input());
		require(result.has_value(), "projection failed");
		return std::move(*result);
	}
	bool complete(const fixture& data)
	{
		const auto result = project(data);
		return result.streams.size() == 1U &&
			result.streams.front().state == q::source_token_state::complete;
	}
} // namespace

int main()
{
	fixture data;
	auto value = project(data);
	require(complete(data) && value.streams.front().directive_count == 1U &&
				value.streams.front().tokens.size() == 3U && value.evidence_rows.size() == 8U,
			"bounded source inventory did not produce the exact stream");
	const auto original = value;
	std::ranges::reverse(data.tokens);
	std::ranges::reverse(data.spans);
	value = project(data);
	for (std::size_t index{}; index < value.evidence_rows.size(); ++index)
		require(value.evidence_rows[index].canonical_form() ==
					original.evidence_rows[index].canonical_form(),
				"input permutation changed retained evidence order");
	require(value.streams.front().tokens.front().evidence ==
				original.streams.front().tokens.front().evidence,
			"input permutation changed token evidence references");
	data.tokens.push_back(data.tokens.front());
	data.tokens.back().provenance = {"independent-witness"};
	value = project(data);
	require(complete(data) &&
				std::ranges::any_of(value.streams.front().tokens,
									[](const auto& token)
									{
										return token.evidence.size() == 3U;
									}),
			"equal observations lost supporting token or source evidence");
	data.tokens.back().values["output.spelling"] = detached_cell::utf8("different");
	require(project(data).streams.front().state == q::source_token_state::conflicting,
			"conflicting token payload was selected as exact");
	data = fixture{};
	data.inventories.push_back(data.inventories.front());
	data.inventories.back().values["output.token_count"] = detached_cell::unsigned_integer(4U);
	require(project(data).streams.front().state == q::source_token_state::conflicting,
			"competing inventories became first-wins");
	data = fixture{};
	data.tokens.pop_back();
	require(!complete(data), "missing token became a complete smaller stream");
	data = fixture{};
	data.tokens.back().values["output.ordinal"] = detached_cell::unsigned_integer(1U);
	require(project(data).streams.front().state == q::source_token_state::conflicting,
			"distinct token IDs with the same ordinal were silently merged");
	data = fixture{};
	data.tokens.back().presence.fragments = {"release"};
	require(!complete(data), "another variant repaired a missing token");
	data = fixture{};
	data.tokens.back().interpretation = "other-frontend";
	require(!complete(data), "another interpretation repaired a missing token");
	data = fixture{};
	data.spans.back().values["output.end"] = detached_cell::unsigned_integer(21U);
	require(!complete(data), "token outside the frozen source bounds was exact");
	data = fixture{};
	data.spans.back().values["output.role"] = detached_cell::utf8("expanded_token");
	require(!complete(data), "expanded source range entered a raw stream");
	data = fixture{};
	data.files.push_back(data.files.front());
	data.files.back().values["output.size"] = detached_cell::unsigned_integer(19U);
	require(project(data).streams.front().state == q::source_token_state::conflicting,
			"conflicting frozen file size became exact");
	data = fixture{};
	data.inventories.front().values["output.enumeration_state"] = detached_cell::utf8("partial");
	require(!complete(data), "partial enumeration became exact through cardinality alone");
	data = fixture{};
	data.tokens.back().values.erase("output.template_context");
	value = project(data);
	require(complete(data) && value.streams.front().template_state == "partial" &&
				!value.streams.front().tokens.back().template_context,
			"unknown template classification became false or destroyed the lexical enumeration");
	data = fixture{};
	data.inventories.front().values["output.directive_count"] = detached_cell::unsigned_integer(2U);
	value = project(data);
	require(complete(data) && !value.streams.front().directive_count &&
				!value.streams.front().gaps.empty(),
			"unverified declared directive count remained exact");
	data = fixture{};
	data.tokens.front().values["output.preprocessor"] = detached_cell::boolean(false);
	require(project(data).streams.front().state == q::source_token_state::conflicting,
			"directive start outside a preprocessing directive was exact");
	data = fixture{};
	data.tokens.clear();
	data.spans.clear();
	data.inventories.front().values["output.token_count"] = detached_cell::unsigned_integer(0U);
	data.inventories.front().values["output.directive_count"] = detached_cell::unsigned_integer(0U);
	require(complete(data), "observed empty source stream was confused with missing evidence");
	data = fixture{};
	for (auto& inventory : data.inventories)
	{
		inventory.values["output.phase"] = detached_cell::utf8("expanded");
		inventory.values.erase("output.directive_count");
	}
	for (auto& span : data.spans)
		span.values["output.role"] = detached_cell::utf8("expanded_token");
	for (auto& token : data.tokens)
	{
		token.values["output.phase"] = detached_cell::utf8("expanded");
		token.values["output.source"] = data.tokens.front().values.at("output.source");
		token.values["output.directive_start"] = detached_cell::boolean(false);
		token.values["output.preprocessor"] = detached_cell::boolean(false);
		token.values["output.macro"] = detached_cell::boolean(true);
	}
	value = project(data);
	require(complete(data) && value.streams.front().tokens.size() == 3U &&
				value.streams.front().tokens.front().begin ==
					value.streams.front().tokens.back().begin,
			"distinct expanded tokens at one macro invocation were collapsed");
	data = fixture{};
	data.spans.clear();
	value = project(data);
	require(!complete(data) && value.unresolved.size() == 3U &&
				value.streams.front().gaps.size() <= 4U,
			"unmapped tokens disappeared or inflated every stream's gap list");
	data = fixture{};
	const auto source = data.files.front(), inventory = data.inventories.front(),
			   orphan_template = data.tokens.front();
	data.files.clear();
	data.inventories.clear();
	data.spans.clear();
	data.tokens.clear();
	for (std::size_t index{}; index < 64U; ++index)
	{
		auto file = source, stream = inventory;
		const auto snapshot = "source:fanout:" + std::to_string(index);
		const auto file_id = "file:fanout:" + std::to_string(index);
		file.values["output.snapshot"] = detached_cell::typed("source_snapshot_id", snapshot);
		file.values["output.file"] = detached_cell::typed("file_id", file_id);
		stream.values["output.inventory"] = detached_cell::typed(
			"source_token_inventory_id", "inventory:fanout:" + std::to_string(index));
		stream.values["output.source_snapshot"] =
			detached_cell::typed("source_snapshot_id", snapshot);
		stream.values["output.file"] = detached_cell::typed("file_id", file_id);
		stream.values["output.token_count"] = detached_cell::unsigned_integer(0U);
		stream.values["output.directive_count"] = detached_cell::unsigned_integer(0U);
		data.files.push_back(std::move(file));
		data.inventories.push_back(std::move(stream));
	}
	for (std::size_t index{}; index < 1000U; ++index)
	{
		auto orphan = orphan_template;
		orphan.values["output.token"] =
			detached_cell::typed("source_token_id", "orphan:" + std::to_string(index));
		orphan.values["output.source"] =
			detached_cell::typed("source_span_id", "missing:" + std::to_string(index));
		data.tokens.push_back(std::move(orphan));
	}
	value = project(data);
	require(
		value.streams.size() == 64U && value.unresolved.size() == 1000U &&
			std::ranges::all_of(value.streams,
								[](const auto& stream)
								{
									return stream.state == q::source_token_state::partial &&
										stream.gaps.size() <= 2U;
								}),
		"orphan tokens multiplied per-stream gaps instead of preserving bounded owner summaries");
	data = fixture{};
	data.inventories.front().presence.fragments = {"debug", "release"};
	for (auto& file : data.files)
		file.presence.fragments = {"debug", "release"};
	for (auto& span : data.spans)
		span.presence.fragments = {"debug", "release"};
	for (auto& token : data.tokens)
		token.presence.fragments = {"debug", "release"};
	value = project(data);
	require(value.streams.size() == 2U && value.streams[0U].variant != value.streams[1U].variant &&
				value.streams[0U].state == q::source_token_state::complete &&
				value.streams[1U].state == q::source_token_state::complete,
			"condition fragments were unioned into an artificial single stream");
	for (const auto field :
		 {"rows", "bytes", "expanded", "conditions", "references", "streams", "tokens"})
	{
		q::source_token_limits limits;
		if (field == std::string_view{"rows"})
			limits.maximum_rows = 1U;
		if (field == std::string_view{"bytes"})
			limits.maximum_evidence_bytes = 1U;
		if (field == std::string_view{"expanded"})
			limits.maximum_expanded_row_bytes = 1U;
		if (field == std::string_view{"conditions"})
			limits.maximum_condition_expansions = 1U;
		if (field == std::string_view{"references"})
			limits.maximum_evidence_references = 1U;
		if (field == std::string_view{"streams"})
			limits.maximum_streams = 1U;
		if (field == std::string_view{"tokens"})
			limits.maximum_stream_tokens = 1U;
		auto bounded = q::project_source_tokens(data.input(), limits);
		require(!bounded && bounded.error().code == "sdk.token-budget",
				"configured resource bound was ignored");
	}
	q::source_token_limits invalid;
	invalid.maximum_rows = 0U;
	require(!q::project_source_tokens(data.input(), invalid), "zero projection limit was accepted");
	std::stop_source stop;
	stop.request_stop();
	auto stopped = q::project_source_tokens(data.input(), {}, stop.get_token());
	require(!stopped && stopped.error().code == "sdk.token-cancelled",
			"projection cancellation was ignored");
	auto missing = q::project_source_tokens(q::application_query_results{"snapshot:test", {}});
	require(missing && missing->source_queries && missing->unresolved.size() == 4U,
			"missing independent scans became complete empty input");
	return 0;
}
