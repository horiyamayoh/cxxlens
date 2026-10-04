#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

#include <cxxlens/sdk/source_tokens.hpp>

#include "query_projection_plan_limits_internal.hpp"

namespace cxxlens::sdk::query
{
	namespace
	{
		using world_key = std::array<std::string, 4U>;
		using stream_key = std::array<std::string, 7U>;
		using owner_key = std::array<std::string, 6U>;
		struct projection_failure
		{
			error value;
		};
		[[noreturn]] void fail(std::string code, std::string field, std::string detail)
		{
			throw projection_failure{{std::move(code), std::move(field), std::move(detail)}};
		}
		void cancelled(std::stop_token token)
		{
			if (token.stop_requested())
				fail("sdk.token-cancelled", "projection", "stop-requested");
		}
		void
		add(std::size_t& target, std::size_t amount, std::size_t maximum, std::string_view field)
		{
			if (target > maximum || amount > maximum - target)
				fail("sdk.token-budget", std::string{field}, "limit-exceeded");
			target += amount;
		}
		const detached_cell* cell(const annotated_row& row, std::string_view name)
		{
			const auto found = row.values.find("output." + std::string{name});
			return found == row.values.end() || found->second.state != cell_state::present ||
					!found->second.value
				? nullptr
				: &found->second;
		}
		std::string text(const annotated_row& row, std::string_view name)
		{
			const auto* found = cell(row, name);
			const auto* value = found ? std::get_if<std::string>(&*found->value) : nullptr;
			return value ? *value : std::string{};
		}
		std::optional<std::uint64_t> number(const annotated_row& row, std::string_view name)
		{
			const auto* found = cell(row, name);
			const auto* value = found ? std::get_if<std::uint64_t>(&*found->value) : nullptr;
			return value ? std::optional{*value} : std::nullopt;
		}
		std::optional<bool> boolean(const annotated_row& row, std::string_view name)
		{
			const auto* found = cell(row, name);
			const auto* value = found ? std::get_if<bool>(&*found->value) : nullptr;
			return value ? std::optional{*value} : std::nullopt;
		}
		std::string payload(const annotated_row& row)
		{
			std::string result;
			for (const auto& [name, value] : row.values)
				result += name + '=' + value.canonical_form() + '\n';
			return result;
		}
		world_key world(std::string id, const annotated_row& row, const std::string& variant)
		{
			return {std::move(id), row.presence.universe, variant, row.interpretation};
		}
		owner_key owner(const annotated_row& row, const std::string& variant)
		{
			return {text(row, "compile_unit"),
					text(row, "phase"),
					text(row, "profile"),
					row.presence.universe,
					variant,
					row.interpretation};
		}
		stream_key subject(const owner_key& owner, std::string snapshot)
		{
			return {
				owner[0], std::move(snapshot), owner[1], owner[2], owner[3], owner[4], owner[5]};
		}
		void gap(source_token_stream& stream,
				 std::string code,
				 std::string detail = {},
				 bool conflict = false,
				 bool enumeration = true)
		{
			stream.gaps.push_back({std::move(code), stream.id, std::move(detail)});
			if (conflict)
				stream.state = source_token_state::conflicting;
			else if (enumeration && stream.state != source_token_state::conflicting)
				stream.state = source_token_state::partial;
		}
		void canonical(std::vector<query_unresolved>& gaps)
		{
			std::ranges::sort(gaps,
							  {},
							  [](const auto& value)
							  {
								  return std::tie(value.code, value.subject, value.detail);
							  });
			gaps.erase(std::ranges::unique(gaps).begin(), gaps.end());
		}
		struct entry
		{
			unsigned kind{};
			const annotated_row* row{};
			std::string canonical;
		};
		struct fact
		{
			std::size_t first{};
			std::string payload;
			std::vector<std::size_t> evidence;
			bool conflicting{};
		};
		struct work_stream
		{
			source_token_stream value;
			std::string payload;
			std::map<std::string, std::pair<std::size_t, std::string>, std::less<>> tokens;
		};
	} // namespace

	result<void> source_token_limits::validate() const
	{
		if (!maximum_rows || !maximum_condition_expansions || !maximum_evidence_bytes ||
			!maximum_expanded_row_bytes || !maximum_evidence_references || !maximum_streams ||
			!maximum_stream_tokens || !maximum_source_queries || !maximum_source_plan_bytes)
			return error{"sdk.token-limit-invalid", "limits", "positive-required"};
		return {};
	}

	result<source_token_projection> project_source_tokens(source_token_input input,
														  source_token_limits limits,
														  std::stop_token cancellation)
	{
		if (auto valid = limits.validate(); !valid)
			return valid.error();
		try
		{
			cancelled(cancellation);
			std::size_t rows{}, bytes{}, expansions{}, expanded_bytes{}, references{};
			const std::array groups{input.files, input.spans, input.inventories, input.tokens};
			std::vector<entry> entries;
			for (unsigned kind{}; kind < groups.size(); ++kind)
			{
				add(rows, groups[kind].size(), limits.maximum_rows, "rows");
				for (const auto& row : groups[kind])
				{
					cancelled(cancellation);
					if (!row.presence.validate() || row.interpretation.empty())
						fail("sdk.token-input-invalid", "condition", "validated-world-required");
					add(expansions,
						row.presence.fragments.size(),
						limits.maximum_condition_expansions,
						"condition-expansions");
					auto encoded = row.canonical_form();
					add(bytes, encoded.size(), limits.maximum_evidence_bytes, "evidence-bytes");
					if (encoded.size() > limits.maximum_expanded_row_bytes /
							std::max(std::size_t{1U}, row.presence.fragments.size()))
						fail("sdk.token-budget", "expanded-row-bytes", "limit-exceeded");
					add(expanded_bytes,
						encoded.size() * row.presence.fragments.size(),
						limits.maximum_expanded_row_bytes,
						"expanded-row-bytes");
					entries.push_back({kind, &row, std::move(encoded)});
				}
			}
			std::ranges::sort(entries,
							  {},
							  [](const auto& item)
							  {
								  return std::tie(item.kind, item.canonical);
							  });
			source_token_projection output;
			std::map<world_key, fact> files, spans;
			std::map<stream_key, work_stream> streams;
			std::map<owner_key, std::vector<stream_key>> owners;
			std::map<world_key, std::set<stream_key>> inventory_owners, token_owners;
			const auto retain = [&](auto& destination, const auto& source)
			{
				add(references,
					source.size(),
					limits.maximum_evidence_references,
					"evidence-references");
				destination.insert(destination.end(), source.begin(), source.end());
			};
			for (std::size_t ref{}; ref < entries.size(); ++ref)
			{
				cancelled(cancellation);
				const auto& item = entries[ref];
				const auto& row = *item.row;
				output.evidence_rows.push_back(row);
				if (item.kind >= 2U)
					continue;
				const auto id = text(row, item.kind == 0U ? "snapshot" : "span");
				if (id.empty())
					fail("sdk.token-input-invalid", "source", "source-identity-required");
				for (const auto& variant : row.presence.fragments)
				{
					cancelled(cancellation);
					auto& cache = item.kind == 0U ? files : spans;
					auto [where, inserted] = cache.try_emplace(world(id, row, variant));
					auto& fact = where->second;
					const auto value = payload(row);
					if (inserted)
					{
						fact.first = ref;
						fact.payload = value;
					}
					else if (fact.payload != value)
						fact.conflicting = true;
					add(references, 1U, limits.maximum_evidence_references, "evidence-references");
					fact.evidence.push_back(ref);
				}
			}
			for (std::size_t ref{}; ref < entries.size(); ++ref)
			{
				cancelled(cancellation);
				if (entries[ref].kind != 2U)
					continue;
				const auto& row = *entries[ref].row;
				const auto id = text(row, "inventory"), snapshot = text(row, "source_snapshot"),
						   file = text(row, "file");
				if (id.empty() || snapshot.empty() || file.empty() ||
					text(row, "compile_unit").empty())
					fail("sdk.token-input-invalid", "inventory", "subject-source-owner-required");
				for (const auto& variant : row.presence.fragments)
				{
					cancelled(cancellation);
					const auto own = owner(row, variant);
					const auto key = subject(own, snapshot);
					inventory_owners[world(id, row, variant)].insert(key);
					if (!streams.contains(key) && streams.size() >= limits.maximum_streams)
						fail("sdk.token-budget", "streams", "limit-exceeded");
					auto [where, inserted] = streams.try_emplace(key);
					auto& work = where->second;
					auto& stream = work.value;
					add(references, 1U, limits.maximum_evidence_references, "evidence-references");
					stream.evidence.push_back(ref);
					const auto value = payload(row);
					if (!inserted)
					{
						if (work.payload != value)
							gap(stream,
								"sdk.token-inventory-conflicting",
								"overlapping-payloads",
								true);
						continue;
					}
					work.payload = value;
					owners[own].push_back(key);
					stream.id = id;
					stream.compile_unit = own[0];
					stream.file = file;
					stream.source_snapshot = snapshot;
					stream.phase = own[1];
					stream.profile = own[2];
					stream.universe = own[3];
					stream.variant = variant;
					stream.interpretation = own[5];
					stream.declared_tokens = number(row, "token_count");
					stream.directive_count = number(row, "directive_count");
					stream.active_state = text(row, "active_state");
					stream.template_state = text(row, "template_state");
					stream.state = source_token_state::complete;
					if (text(row, "enumeration_state") != "complete")
						gap(stream, "sdk.token-enumeration-partial", text(row, "reason"));
					if (stream.phase != "raw" && stream.phase != "expanded")
						gap(stream, "sdk.token-phase-unsupported", stream.phase);
					if (stream.profile.empty())
						gap(stream, "sdk.token-profile-missing", "versioned-profile-required");
					const auto source = files.find(world(snapshot, row, variant));
					if (source == files.end())
						gap(stream, "sdk.token-source-missing", snapshot);
					else
					{
						const auto& source_row = *entries[source->second.first].row;
						retain(stream.evidence, source->second.evidence);
						stream.source_size = number(source_row, "size");
						if (source->second.conflicting)
							gap(stream, "sdk.token-source-conflicting", snapshot, true);
						if (text(source_row, "file") != file || !stream.source_size)
							gap(stream, "sdk.token-source-binding-mismatch", snapshot);
					}
				}
			}
			std::map<owner_key, std::size_t> orphan_tokens;
			std::set<owner_key> ambiguous_span_owners;
			for (std::size_t ref{}; ref < entries.size(); ++ref)
			{
				cancelled(cancellation);
				if (entries[ref].kind != 3U)
					continue;
				const auto& row = *entries[ref].row;
				const auto id = text(row, "token"), span_id = text(row, "source");
				if (id.empty())
					fail("sdk.token-input-invalid", "token", "token-identity-required");
				for (const auto& variant : row.presence.fragments)
				{
					cancelled(cancellation);
					const auto own = owner(row, variant);
					const auto span = spans.find(world(span_id, row, variant));
					if (span == spans.end())
					{
						output.unresolved.push_back({"sdk.token-span-missing", id, span_id});
						++orphan_tokens[own];
						continue;
					}
					const auto& source = *entries[span->second.first].row;
					if (span->second.conflicting)
						ambiguous_span_owners.insert(own);
					const auto stream_it = streams.find(subject(own, text(source, "snapshot")));
					if (stream_it == streams.end())
					{
						output.unresolved.push_back(
							{"sdk.token-inventory-missing", id, text(source, "snapshot")});
						continue;
					}
					auto& work = stream_it->second;
					auto& stream = work.value;
					token_owners[world(id, row, variant)].insert(stream_it->first);
					if (span->second.conflicting)
						gap(stream, "sdk.token-span-conflicting", span_id, true);
					const auto value = payload(row);
					const auto existing = work.tokens.find(id);
					if (existing != work.tokens.end())
					{
						add(references,
							1U,
							limits.maximum_evidence_references,
							"evidence-references");
						stream.tokens[existing->second.first].evidence.push_back(ref);
						if (existing->second.second != value)
							gap(stream, "sdk.token-row-conflicting", id, true);
						continue;
					}
					if (stream.tokens.size() >= limits.maximum_stream_tokens)
						fail("sdk.token-budget", "stream-tokens", stream.id);
					source_token token{id,
									   span_id,
									   text(row, "kind"),
									   text(row, "spelling"),
									   number(row, "ordinal"),
									   number(source, "begin"),
									   number(source, "end"),
									   boolean(row, "macro"),
									   boolean(row, "active"),
									   boolean(row, "template_context"),
									   boolean(row, "preprocessor"),
									   boolean(row, "directive_start"),
									   {}};
					if (text(source, "role") != stream.phase + "_token")
						gap(stream, "sdk.token-span-role-mismatch", id);
					if (text(source, "file") != stream.file || !token.begin || !token.end ||
						!stream.source_size || *token.end < *token.begin ||
						*token.end > *stream.source_size)
						gap(stream, "sdk.token-outside-source", id);
					add(references, 1U, limits.maximum_evidence_references, "evidence-references");
					token.evidence.push_back(ref);
					retain(token.evidence, span->second.evidence);
					work.tokens.emplace(id, std::pair{stream.tokens.size(), value});
					stream.tokens.push_back(std::move(token));
				}
			}
			for (const auto& ownership : {std::cref(inventory_owners), std::cref(token_owners)})
				for (const auto& [id, keys] : ownership.get())
				{
					cancelled(cancellation);
					if (keys.size() > 1U)
						for (const auto& key : keys)
							gap(streams.at(key).value, "sdk.token-owner-conflicting", id[0], true);
				}
			for (const auto& own : ambiguous_span_owners)
			{
				cancelled(cancellation);
				const auto found = owners.find(own);
				if (found != owners.end())
					for (const auto& key : found->second)
					{
						cancelled(cancellation);
						gap(streams.at(key).value,
							"sdk.token-span-owner-conflicting",
							"ambiguous-source-binding",
							true);
					}
			}
			for (const auto& [own, count] : orphan_tokens)
			{
				cancelled(cancellation);
				const auto found = owners.find(own);
				if (found != owners.end())
					for (const auto& key : found->second)
					{
						cancelled(cancellation);
						gap(streams.at(key).value,
							"sdk.token-span-missing",
							"unmapped-token-count=" + std::to_string(count));
					}
			}
			for (auto& [key, work] : streams)
			{
				(void)key;
				cancelled(cancellation);
				auto& stream = work.value;
				std::ranges::sort(stream.tokens,
								  {},
								  [](const auto& token)
								  {
									  return std::tie(token.ordinal, token.id);
								  });
				if (!stream.declared_tokens || *stream.declared_tokens != stream.tokens.size())
					gap(stream,
						"sdk.token-cardinality-mismatch",
						"observed=" + std::to_string(stream.tokens.size()));
				bool ordinal_gap{}, ordinal_conflict{}, active_gap{}, template_gap{},
					directive_gap{}, directive_invalid{};
				std::uint64_t directives{};
				for (std::size_t index{}; index < stream.tokens.size(); ++index)
				{
					cancelled(cancellation);
					const auto& token = stream.tokens[index];
					ordinal_gap |= !token.ordinal || *token.ordinal != index;
					ordinal_conflict |= index && token.ordinal &&
						token.ordinal == stream.tokens[index - 1U].ordinal;
					active_gap |= !token.active;
					template_gap |= !token.template_context;
					directive_gap |= !token.directive_start;
					if (token.directive_start && *token.directive_start)
					{
						++directives;
						directive_invalid |= stream.phase != "raw" || !token.preprocessor ||
							!*token.preprocessor || token.kind != "hash";
					}
				}
				if (ordinal_conflict)
					gap(stream,
						"sdk.token-ordinal-conflicting",
						"distinct-tokens-share-ordinal",
						true);
				if (ordinal_gap)
					gap(stream, "sdk.token-ordinal-gap", "contiguous-zero-based-sequence-required");
				if (directive_invalid)
					gap(stream,
						"sdk.token-directive-conflicting",
						"raw-preprocessor-hash-required",
						true);
				if (stream.phase == "raw" &&
					(!stream.directive_count || directive_gap ||
					 *stream.directive_count != directives))
				{
					stream.directive_count.reset();
					gap(stream,
						"sdk.token-directive-count-mismatch",
						"observed=" + std::to_string(directives),
						false,
						false);
				}
				if (stream.active_state == "complete" && active_gap)
				{
					stream.active_state = "partial";
					gap(stream, "sdk.token-active-classification-missing", {}, false, false);
				}
				if (stream.template_state == "complete" && template_gap)
				{
					stream.template_state = "partial";
					gap(stream, "sdk.token-template-classification-missing", {}, false, false);
				}
				std::ranges::sort(stream.evidence);
				stream.evidence.erase(std::ranges::unique(stream.evidence).begin(),
									  stream.evidence.end());
				canonical(stream.gaps);
				output.streams.push_back(std::move(stream));
			}
			canonical(output.unresolved);
			cancelled(cancellation);
			return output;
		}
		catch (const projection_failure& failure)
		{
			return failure.value;
		}
		catch (const std::bad_alloc&)
		{
			return error{"sdk.token-resource-exhausted", "projection", "allocation"};
		}
		catch (const std::length_error&)
		{
			return error{"sdk.token-resource-exhausted", "projection", "length"};
		}
	}

	result<source_token_projection> project_source_tokens(const application_query_results& input,
														  source_token_limits limits,
														  std::stop_token cancellation)
	{
		if (auto valid = limits.validate(); !valid)
			return valid.error();
		try
		{
			cancelled(cancellation);
			if (auto bounded = detail::check_source_plan_limits(input,
																limits.maximum_source_queries,
																limits.maximum_source_plan_bytes,
																cancellation,
																"sdk.token");
				!bounded)
				return bounded.error();
			const std::array<std::string_view, 4U> ids{
				"source.file.v1", "source.span.v1", "source.token_inventory.v1", "source.token.v1"};
			std::array<std::vector<annotated_row>, 4U> groups;
			std::array<bool, 4U> present{};
			std::size_t rows{}, bytes{};
			for (const auto& scan : input.scans)
			{
				cancelled(cancellation);
				const auto found = std::ranges::find(ids, scan.relation_id);
				if (found == ids.end())
					continue;
				const auto index = static_cast<std::size_t>(found - ids.begin());
				present[index] = true;
				auto cursor = scan.result.rows();
				while (true)
				{
					cancelled(cancellation);
					auto next = cursor.next();
					if (!next)
						return next.error();
					if (!*next)
						break;
					add(rows, 1U, limits.maximum_rows, "rows");
					auto row = (*next)->copy();
					if (!row)
						return row.error();
					add(bytes,
						row->canonical_form().size(),
						limits.maximum_evidence_bytes,
						"evidence-bytes");
					groups[index].push_back(std::move(*row));
				}
			}
			auto output = project_source_tokens(
				source_token_input{groups[0], groups[1], groups[2], groups[3]},
				limits,
				cancellation);
			if (!output)
				return output;
			for (std::size_t index{}; index < ids.size(); ++index)
				if (!present[index])
				{
					output->unresolved.push_back({"sdk.token-scan-missing",
												  std::string{ids[index]},
												  "independent-scan-required"});
					for (auto& stream : output->streams)
						gap(stream, "sdk.token-scan-missing", std::string{ids[index]});
				}
			output->source_queries = input;
			return output;
		}
		catch (const projection_failure& failure)
		{
			return failure.value;
		}
		catch (const std::bad_alloc&)
		{
			return error{"sdk.token-resource-exhausted", "projection", "allocation"};
		}
		catch (const std::length_error&)
		{
			return error{"sdk.token-resource-exhausted", "projection", "length"};
		}
	}
} // namespace cxxlens::sdk::query
