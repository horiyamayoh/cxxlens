#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path


def replace_exact(path: str, old: str, new: str, *, count: int = 1) -> None:
    file = Path(path)
    text = file.read_text(encoding="utf-8")
    actual = text.count(old)
    if actual != count:
        raise RuntimeError(f"{path}: expected {count} matches, found {actual}")
    file.write_text(text.replace(old, new), encoding="utf-8")


def insert_before(path: str, marker: str, content: str) -> None:
    replace_exact(path, marker, content + marker)


claims_path = "src/llvm/clang22/materialization_claims.cpp"

# A canonical, fail-closed decoder for descriptor-declared container references.
insert_before(
    claims_path,
    "\n\t\t[[nodiscard]] sdk::result<std::string> base_row_digest(",
    r'''
		[[nodiscard]] sdk::result<std::vector<std::string>>
		reference_container_elements(const sdk::detached_cell& cell)
		{
			if (cell.state != sdk::cell_state::present || !cell.value)
				return sdk::unexpected(claim_error("materialization.claim-invalid",
												   "reference-container",
												   "unknown-or-missing-cell"));
			const auto* encoded = std::get_if<std::vector<std::byte>>(&*cell.value);
			if (encoded == nullptr)
				return sdk::unexpected(claim_error("materialization.claim-invalid",
												   "reference-container",
												   "type"));
			std::vector<std::string> output;
			for (std::size_t offset{}; offset < encoded->size();)
			{
				if (encoded->size() - offset < sizeof(std::uint32_t))
					return sdk::unexpected(claim_error("materialization.claim-invalid",
													   "reference-container",
													   "truncated-length"));
				std::uint32_t length{};
				for (std::size_t byte{}; byte < sizeof(length); ++byte)
					length |= std::to_integer<std::uint32_t>((*encoded)[offset + byte])
							  << (byte * 8U);
				offset += sizeof(length);
				if (length == 0U || length > encoded->size() - offset)
					return sdk::unexpected(claim_error("materialization.claim-invalid",
													   "reference-container",
													   "invalid-length"));
				output.emplace_back(reinterpret_cast<const char*>(encoded->data() + offset),
									length);
				offset += length;
			}
			return output;
		}

		void canonicalize_unresolved(std::vector<sdk::unresolved_reference>& values)
		{
			std::ranges::sort(values,
							  [](const sdk::unresolved_reference& left,
								 const sdk::unresolved_reference& right)
							  {
								  return std::tie(left.source_assertion,
												  left.source_relation,
												  left.target_relation,
												  left.source_columns,
												  left.reason) <
										 std::tie(right.source_assertion,
												  right.source_relation,
												  right.target_relation,
												  right.source_columns,
												  right.reason);
							  });
			values.erase(std::unique(values.begin(), values.end()), values.end());
		}
''',
)

# Provider unresolved evidence is a valid typed side channel, not invalid coverage.
replace_exact(
    claims_path,
    r'''			if (!result.provider_seal().unresolved().empty())
				return sdk::unexpected(claim_error("materialization.coverage-incomplete",
												   "provider.unresolved",
												   "qualified-zero"));
''',
    "",
)

# The complete legacy request path keeps soft unresolved while preserving the conflict policy.
replace_exact(
    claims_path,
    r'''		if (!committed->unresolved.empty() || !committed->conflicts.empty() ||
			!committed->differential_disagreements.empty())
		{
			return sdk::unexpected(claim_error("materialization.claim-invalid",
											   "complete-final-claim-batch",
											   "nonzero-unresolved-conflict-or-differential"));
		}
''',
    r'''		if (!committed->conflicts.empty() || !committed->differential_disagreements.empty())
		{
			return sdk::unexpected(claim_error("materialization.claim-invalid",
											   "complete-final-claim-batch",
											   "nonzero-conflict-or-differential"));
		}
''',
)

# Both legacy and bounded partition builders need an assertion-to-partition ownership index.
replace_exact(
    claims_path,
    "\t\tstd::map<partition_key, partition_accumulator> partition_groups;\n",
    "\t\tstd::map<partition_key, partition_accumulator> partition_groups;\n"
    "\t\tstd::map<std::string, std::vector<partition_accumulator*>, std::less<>>\n"
    "\t\t\tsource_partitions;\n",
    count=2,
)

replace_exact(
    claims_path,
    r'''			(*partition)->claim_contents.insert(claim_value.content);
		}

		for (std::size_t task_index{}; task_index < contexts.size(); ++task_index)
''',
    r'''			(*partition)->claim_contents.insert(claim_value.content);
			auto& owners = source_partitions[claim_value.assertion];
			if (std::ranges::find(owners, *partition) == owners.end())
				owners.push_back(*partition);
		}

		for (const auto& unresolved : committed->unresolved)
		{
			const auto owners = source_partitions.find(unresolved.source_assertion);
			if (owners == source_partitions.end() || owners->second.empty())
				return sdk::unexpected(claim_error("materialization.claim-invalid",
												   "partition.unresolved",
												   "source-claim-owner-missing"));
			for (auto* owner : owners->second)
				owner->draft.unresolved.push_back(unresolved);
		}

		for (std::size_t task_index{}; task_index < contexts.size(); ++task_index)
''',
)

replace_exact(
    claims_path,
    r'''				(*partition)->claim_contents.insert(final.content);
				return {};
''',
    r'''				(*partition)->claim_contents.insert(final.content);
				auto& owners = source_partitions[final.assertion];
				if (std::ranges::find(owners, *partition) == owners.end())
					owners.push_back(*partition);
				return {};
''',
)

# Strict container decoding and vacuous resolution for an empty canonical set.
replace_exact(
    claims_path,
    r'''					if (reference.container_elements)
					{
						const auto source = value->row.cells.find(reference.source_columns.front());
						std::vector<std::string> elements;
						if (source != value->row.cells.end() && source->second.value)
						{
							if (const auto* encoded =
									std::get_if<std::vector<std::byte>>(&*source->second.value))
							{
								for (std::size_t offset{}; offset < encoded->size();)
								{
									if (encoded->size() - offset < sizeof(std::uint32_t))
										break;
									std::uint32_t length{};
									for (std::size_t byte{}; byte < sizeof(length); ++byte)
										length |= std::to_integer<std::uint32_t>(
													  (*encoded)[offset + byte])
												  << (byte * 8U);
									offset += sizeof(length);
									if (length == 0U || length > encoded->size() - offset)
										break;
									elements.emplace_back(
										reinterpret_cast<const char*>(encoded->data() + offset),
										length);
									offset += length;
								}
							}
						}
						resolved = !elements.empty() &&
							std::ranges::all_of(elements,
											  [&](const std::string& element)
											  {
												  return matches(std::string_view{element});
											  });
					}
''',
    r'''					if (reference.container_elements)
					{
						if (reference.source_columns.empty())
							return sdk::unexpected(claim_error("materialization.claim-invalid",
															   value->descriptor,
															   "reference-container-columns"));
						const auto source =
							value->row.cells.find(reference.source_columns.front());
						if (source == value->row.cells.end())
							return sdk::unexpected(claim_error("materialization.claim-invalid",
															   value->descriptor,
															   "reference-container-source"));
						auto elements = reference_container_elements(source->second);
						if (!elements)
							return sdk::unexpected(std::move(elements.error()));
						resolved = std::ranges::all_of(
							*elements,
							[&](const std::string& element)
							{
								return matches(std::string_view{element});
							});
					}
''',
)

replace_exact(
    claims_path,
    r'''					if (!resolved)
						return sdk::unexpected(
							claim_error("materialization.claim-invalid",
										value->descriptor,
										reference.strength == sdk::reference_strength::hard
											? "hard-reference-missing"
											: "soft-reference-unresolved"));
''',
    r'''					if (!resolved)
					{
						if (reference.strength == sdk::reference_strength::hard)
							return sdk::unexpected(claim_error("materialization.claim-invalid",
															   value->descriptor,
															   "hard-reference-missing"));
						const auto owners = source_partitions.find(value->assertion);
						if (owners == source_partitions.end() || owners->second.empty())
							return sdk::unexpected(claim_error("materialization.claim-invalid",
															   "partition.unresolved",
															   "source-claim-owner-missing"));
						const sdk::unresolved_reference unresolved{
							value->assertion,
							value->descriptor,
							reference.target_relation,
							reference.source_columns,
							"soft-reference-missing",
						};
						for (auto* owner : owners->second)
							owner->draft.unresolved.push_back(unresolved);
					}
''',
)

# Canonicalize unresolved records before deriving partition identity and completeness.
replace_exact(
    claims_path,
    r'''			for (const auto& [coverage_id, coverage] : accumulator.coverage)
			{
				(void)coverage_id;
				accumulator.draft.coverage.push_back(coverage);
			}
			auto manifest = sdk::make_partition_manifest(request.engine, accumulator.draft);
''',
    r'''			for (const auto& [coverage_id, coverage] : accumulator.coverage)
			{
				(void)coverage_id;
				accumulator.draft.coverage.push_back(coverage);
			}
			canonicalize_unresolved(accumulator.draft.unresolved);
			auto manifest = sdk::make_partition_manifest(request.engine, accumulator.draft);
''',
    count=2,
)

replace_exact(
    claims_path,
    "manifest->complete != !accumulator.coverage.empty()",
    "manifest->complete !=\n"
    "\t\t\t\t\t(!accumulator.coverage.empty() && accumulator.draft.unresolved.empty())",
    count=2,
)

# Add an internal exact-request constructor used by the coordinator and focused regression tests.
header_path = "src/llvm/clang22/materialization_bounded_claim_source.hpp"
replace_exact(
    header_path,
    r'''		[[nodiscard]] static sdk::result<materialization_bounded_claim_source>
		begin(const validated_materialization_request& request);

		/** Begin a bounded source using v2.1 request authority without a legacy task vector. */
''',
    r'''		[[nodiscard]] static sdk::result<materialization_bounded_claim_source>
		begin(const validated_materialization_request& request);

		/** Begin from already validated exact request identity and task census. */
		[[nodiscard]] static sdk::result<materialization_bounded_claim_source>
		begin(std::string materialization_request_id,
			  const sdk::relation_engine& engine,
			  std::uint64_t expected_task_count);

		/** Begin a bounded source using v2.1 request authority without a legacy task vector. */
''',
)

bounded_path = "src/llvm/clang22/materialization_bounded_claim_source.cpp"
replace_exact(
    bounded_path,
    r'''#include <cstdint>
#include <limits>
#include <new>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>
''',
    r'''#include <cstdint>
#include <functional>
#include <limits>
#include <new>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
''',
)

# Shared exact reconciliation helpers for sealed claim spools.
insert_before(
    bounded_path,
    "\n\t} // namespace\n\n\tsdk::result<materialization_bounded_claim_source>\n",
    r'''
		void canonicalize_unresolved(std::vector<sdk::unresolved_reference>& values)
		{
			std::ranges::sort(values,
							  [](const sdk::unresolved_reference& left,
								 const sdk::unresolved_reference& right)
							  {
								  return std::tie(left.source_assertion,
												  left.source_relation,
												  left.target_relation,
												  left.source_columns,
												  left.reason) <
										 std::tie(right.source_assertion,
												  right.source_relation,
												  right.target_relation,
												  right.source_columns,
												  right.reason);
							  });
			values.erase(std::unique(values.begin(), values.end()), values.end());
		}

		using spooled_claim_consumer =
			std::function<sdk::result<void>(const sdk::claim&)>;

		[[nodiscard]] sdk::result<void>
		for_each_spooled_claim(materialization_replayable_spool& spool,
							  const sdk::relation_engine& engine,
							  const spooled_claim_consumer& consumer)
		{
			std::uint64_t offset{};
			while (offset < spool.size_bytes())
			{
				if (spool.size_bytes() - offset < sizeof(std::uint64_t))
					return sdk::unexpected(source_error("claims", "truncated-length"));
				std::array<std::byte, sizeof(std::uint64_t)> length_bytes{};
				if (auto read = read_exact(spool, offset, length_bytes); !read)
					return read;
				offset += length_bytes.size();
				const auto length = decode_u64(length_bytes);
				if (length > spool.size_bytes() - offset ||
					length > std::numeric_limits<std::size_t>::max())
					return sdk::unexpected(source_error("claims", "length"));
				std::vector<std::byte> encoded(static_cast<std::size_t>(length));
				if (auto read = read_exact(spool, offset, encoded); !read)
					return read;
				offset += length;
				auto claim = sdk::detail::decode_store_claim(encoded, engine);
				if (!claim)
					return sdk::unexpected(std::move(claim.error()));
				if (auto consumed = consumer(*claim); !consumed)
					return consumed;
			}
			return {};
		}

		[[nodiscard]] bool reference_source_absent(
			const sdk::claim& source,
			const sdk::relation_reference_descriptor& reference)
		{
			return std::ranges::any_of(
				reference.source_columns,
				[&](const std::string& column)
				{
					const auto found = source.row.cells.find(column);
					return found == source.row.cells.end() ||
						found->second.state == sdk::cell_state::absent;
				});
		}

		[[nodiscard]] sdk::result<std::vector<std::string>>
		reference_container_elements(const sdk::detached_cell& cell)
		{
			if (cell.state != sdk::cell_state::present || !cell.value)
				return sdk::unexpected(source_error("reference-container",
													"unknown-or-missing-cell"));
			const auto* encoded = std::get_if<std::vector<std::byte>>(&*cell.value);
			if (encoded == nullptr)
				return sdk::unexpected(source_error("reference-container", "type"));
			std::vector<std::string> output;
			for (std::size_t offset{}; offset < encoded->size();)
			{
				if (encoded->size() - offset < sizeof(std::uint32_t))
					return sdk::unexpected(source_error("reference-container",
														"truncated-length"));
				std::uint32_t length{};
				for (std::size_t byte{}; byte < sizeof(length); ++byte)
					length |= std::to_integer<std::uint32_t>((*encoded)[offset + byte])
							  << (byte * 8U);
				offset += sizeof(length);
				if (length == 0U || length > encoded->size() - offset)
					return sdk::unexpected(source_error("reference-container",
														"invalid-length"));
				output.emplace_back(reinterpret_cast<const char*>(encoded->data() + offset),
									length);
				offset += length;
			}
			return output;
		}

		[[nodiscard]] sdk::result<bool>
		reference_matches(const sdk::relation_engine& engine,
						  const sdk::claim& source,
						  const sdk::relation_reference_descriptor& reference,
						  const sdk::claim& target,
						  const std::optional<std::string_view> element)
		{
			auto target_descriptor = engine.require_id(target.descriptor);
			if (!target_descriptor)
				return sdk::unexpected(std::move(target_descriptor.error()));
			if (target_descriptor->descriptor().name != reference.target_relation)
				return false;
			if (source.interpretation != target.interpretation ||
				source.presence.universe != target.presence.universe ||
				!std::ranges::includes(target.presence.fragments, source.presence.fragments))
				return false;
			if (reference.source_columns.size() != reference.target_columns.size() ||
				reference.source_columns.empty())
				return sdk::unexpected(source_error("reference", "column-shape"));
			for (std::size_t index{}; index < reference.source_columns.size(); ++index)
			{
				const auto left = source.row.cells.find(reference.source_columns[index]);
				const auto right = target.row.cells.find(reference.target_columns[index]);
				if (left == source.row.cells.end() || right == target.row.cells.end() ||
					left->second.state != sdk::cell_state::present ||
					right->second.state != sdk::cell_state::present ||
					!left->second.value || !right->second.value)
					return false;
				if (reference.container_elements)
				{
					if (!element)
						return false;
					const auto* target_value =
						std::get_if<std::string>(&*right->second.value);
					if (target_value == nullptr || *target_value != *element)
						return false;
				}
				else if (left->second.value != right->second.value)
					return false;
			}
			return true;
		}
''',
)

replace_exact(
    bounded_path,
    r'''		return materialization_bounded_claim_source{
			std::move(*request_id),
			request.engine,
			static_cast<std::uint64_t>(request.tasks.size())};
	}

	sdk::result<materialization_bounded_claim_source> materialization_bounded_claim_source::begin(
		const materialization_v2_1_claim_authority& authority)
''',
    r'''		return begin(std::move(*request_id),
					 request.engine,
					 static_cast<std::uint64_t>(request.tasks.size()));
	}

	sdk::result<materialization_bounded_claim_source>
	materialization_bounded_claim_source::begin(std::string materialization_request_id,
												const sdk::relation_engine& engine,
												const std::uint64_t expected_task_count)
	{
		if (materialization_request_id.empty() || expected_task_count == 0U)
			return sdk::unexpected(source_error("request", "empty-or-unbound"));
		return materialization_bounded_claim_source{
			std::move(materialization_request_id), engine, expected_task_count};
	}

	sdk::result<materialization_bounded_claim_source> materialization_bounded_claim_source::begin(
		const materialization_v2_1_claim_authority& authority)
''',
)

replace_exact(
    bounded_path,
    r'''		return materialization_bounded_claim_source{
			std::string{authority.materialization_request_id()},
			*authority.engine(),
			authority.task_count()};
''',
    r'''		return begin(std::string{authority.materialization_request_id()},
					 *authority.engine(),
					 authority.task_count());
''',
)

# Replace finalize with request-wide residual reconciliation before any Store replay.
start = '''sdk::result<materialization_bounded_claim_source>
	materialization_bounded_claim_source::finalize() &&
	{'''
end = '''	sdk::result<void> materialization_bounded_claim_source::replay(
'''
text = Path(bounded_path).read_text(encoding="utf-8")
start_at = text.index(start)
end_at = text.index(end, start_at)
new_finalize = r'''sdk::result<materialization_bounded_claim_source>
	materialization_bounded_claim_source::finalize() &&
	{
		if (sealed_ || failed_ || partitions_.empty())
			return sdk::unexpected(source_error("lifecycle", "empty-or-already-sealed"));
		if (consumed_task_count_ != expected_task_count_)
		{
			failed_ = true;
			return sdk::unexpected(source_error("lifecycle", "incomplete-task-set"));
		}
		bool completed = false;
		const consume_failure_guard failure_guard{failed_, completed};
		try
		{
			for (auto& [partition_id, state] : partitions_)
			{
				(void)partition_id;
				if (!state.claims)
					return sdk::unexpected(source_error("claims", "missing-spool"));
				canonicalize_unresolved(state.unresolved);
				std::vector<sdk::unresolved_reference> residual;
				for (const auto& unresolved : state.unresolved)
				{
					if (unresolved.reason != "soft-reference-missing" ||
						unresolved.source_relation != state.identity.relation_descriptor_id)
						return sdk::unexpected(source_error("partition.unresolved",
															"identity-binding"));
					auto source_descriptor = engine_->require_id(unresolved.source_relation);
					if (!source_descriptor)
						return sdk::unexpected(std::move(source_descriptor.error()));
					const sdk::relation_reference_descriptor* reference{};
					for (const auto& candidate : source_descriptor->descriptor().references)
					{
						if (candidate.target_relation != unresolved.target_relation ||
							candidate.source_columns != unresolved.source_columns)
							continue;
						if (reference != nullptr)
							return sdk::unexpected(source_error("partition.unresolved",
																"ambiguous-reference"));
						reference = &candidate;
					}
					if (reference == nullptr ||
						reference->strength != sdk::reference_strength::soft_semantic)
						return sdk::unexpected(source_error("partition.unresolved",
															"soft-reference-binding"));

					bool source_seen{};
					bool unresolved_remains{};
					auto sources = for_each_spooled_claim(
						*state.claims,
						*engine_,
						[&](const sdk::claim& source) -> sdk::result<void>
						{
							if (source.assertion != unresolved.source_assertion)
								return {};
							source_seen = true;
							if (source.descriptor != unresolved.source_relation ||
								reference_source_absent(source, *reference))
								return sdk::unexpected(source_error("partition.unresolved",
																	"source-binding"));

							const auto target_exists =
								[&](const std::optional<std::string_view> element)
								-> sdk::result<bool>
							{
								bool found{};
								for (auto& [target_partition_id, target_state] : partitions_)
								{
									(void)target_partition_id;
									if (found || !target_state.claims)
										continue;
									auto targets = for_each_spooled_claim(
										*target_state.claims,
										*engine_,
										[&](const sdk::claim& target) -> sdk::result<void>
										{
											if (found)
												return {};
											auto matches = reference_matches(
												*engine_, source, *reference, target, element);
											if (!matches)
												return sdk::unexpected(std::move(matches.error()));
											found = *matches;
											return {};
										});
									if (!targets)
										return sdk::unexpected(std::move(targets.error()));
								}
								return found;
							};

							bool resolved{};
							if (reference->container_elements)
							{
								if (reference->source_columns.empty())
									return sdk::unexpected(source_error("reference",
																		"container-columns"));
								const auto cell =
									source.row.cells.find(reference->source_columns.front());
								if (cell == source.row.cells.end())
									return sdk::unexpected(source_error("reference",
																		"container-source"));
								auto elements = reference_container_elements(cell->second);
								if (!elements)
									return sdk::unexpected(std::move(elements.error()));
								resolved = true;
								for (const auto& element : *elements)
								{
									auto matched = target_exists(std::string_view{element});
									if (!matched)
										return sdk::unexpected(std::move(matched.error()));
									if (!*matched)
										resolved = false;
								}
							}
							else
							{
								auto matched = target_exists(std::nullopt);
								if (!matched)
									return sdk::unexpected(std::move(matched.error()));
								resolved = *matched;
							}
							if (!resolved)
								unresolved_remains = true;
							return {};
						});
					if (!sources)
						return sdk::unexpected(std::move(sources.error()));
					if (!source_seen)
						return sdk::unexpected(source_error("partition.unresolved",
															"source-claim-missing"));
					if (unresolved_remains)
						residual.push_back(unresolved);
				}
				canonicalize_unresolved(residual);
				state.unresolved = std::move(residual);
			}

			for (auto& [partition_id, state] : partitions_)
			{
				(void)partition_id;
				if (auto sealed = state.claims->seal(); !sealed)
					return sdk::unexpected(source_error("claims", "spool-seal"));
			}
			if (!claim_envelopes_ || !canonicalization_edges_ || !origin_associations_)
				return sdk::unexpected(source_error("report-metadata", "missing-spool"));
			for (auto* spool :
				 {claim_envelopes_.get(), canonicalization_edges_.get(), origin_associations_.get()})
				if (auto sealed = spool->seal(); !sealed)
					return sdk::unexpected(source_error("report-metadata", "spool-seal"));
			sealed_ = true;
			completed = true;
			return std::move(*this);
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(source_error("claims", "allocation"));
		}
	}

'''
Path(bounded_path).write_text(text[:start_at] + new_finalize + text[end_at:], encoding="utf-8")

# Carry canonical unresolved records into the bounded claim-batch v2 digest.
replace_exact(
    bounded_path,
    r'''			std::uint64_t claim_count{};
			std::uint64_t unresolved_count{};

			if (auto replayed = replay(
''',
    r'''			std::uint64_t claim_count{};
			std::vector<sdk::unresolved_reference> unresolved;

			if (auto replayed = replay(
''',
)
replace_exact(
    bounded_path,
    r'''						if (draft.unresolved.size() >
							std::numeric_limits<std::uint64_t>::max() - unresolved_count)
							return sdk::unexpected(source_error("claims.unresolved", "overflow"));
						unresolved_count += static_cast<std::uint64_t>(draft.unresolved.size());
''',
    r'''						if (draft.unresolved.size() >
							std::numeric_limits<std::size_t>::max() - unresolved.size())
							return sdk::unexpected(source_error("claims.unresolved", "overflow"));
						unresolved.insert(unresolved.end(),
										  draft.unresolved.begin(),
										  draft.unresolved.end());
''',
)
replace_exact(
    bounded_path,
    r'''			if (ordered_count != claim_count)
				return sdk::unexpected(source_error("claims", "census-mismatch"));

			if (unresolved_count != 0U)
				// Task admission rejects functional conflicts and differential disagreements before
				// they become bounded partitions; zero here is the source contract, not an inferred
				// absence from the streaming census.
				return materialization_bounded_claim_batch_status{
					{}, claim_count, unresolved_count, 0U, 0U, partitions_.size()};

			if (auto sealed = records->seal(); !sealed)
''',
    r'''			if (ordered_count != claim_count)
				return sdk::unexpected(source_error("claims", "census-mismatch"));

			canonicalize_unresolved(unresolved);
			if (unresolved.size() > std::numeric_limits<std::uint64_t>::max())
				return sdk::unexpected(source_error("claims.unresolved", "overflow"));
			const auto unresolved_count = static_cast<std::uint64_t>(unresolved.size());
			std::vector<sdk::canonical_value> unresolved_records;
			unresolved_records.reserve(unresolved.size());
			for (const auto& value : unresolved)
			{
				std::vector<sdk::canonical_value> source_columns;
				source_columns.reserve(value.source_columns.size());
				for (const auto& column : value.source_columns)
					source_columns.push_back(sdk::canonical_value::from_string(column));
				unresolved_records.push_back(sdk::canonical_value::from_tuple({
					sdk::canonical_value::from_string("unresolved"),
					sdk::canonical_value::from_string(value.source_assertion),
					sdk::canonical_value::from_string(value.source_relation),
					sdk::canonical_value::from_string(value.target_relation),
					sdk::canonical_value::from_tuple(std::move(source_columns)),
					sdk::canonical_value::from_string(value.reason),
				}));
			}
			auto unresolved_binary = sdk::canonical_binary(
				sdk::canonical_value::from_tuple(std::move(unresolved_records)));
			if (!unresolved_binary ||
				unresolved_binary->size() > std::numeric_limits<std::uint64_t>::max())
				return sdk::unexpected(source_error("claims.unresolved", "encode"));

			if (auto sealed = records->seal(); !sealed)
''',
)
replace_exact(
    bounded_path,
    r'''			std::uint64_t claim_batch_marker_size{};
			if (!checked_canonical_string_size("cxxlens.claim-batch.v2", claim_batch_marker_size))
				return sdk::unexpected(source_error("claims", "size-overflow"));
			std::uint64_t child_sum{};
''',
    r'''			const auto unresolved_tuple_size =
				static_cast<std::uint64_t>(unresolved_binary->size());
			std::uint64_t claim_batch_marker_size{};
			if (!checked_canonical_string_size("cxxlens.claim-batch.v2", claim_batch_marker_size))
				return sdk::unexpected(source_error("claims", "size-overflow"));
			std::uint64_t child_sum{};
''',
)
replace_exact(
    bounded_path,
    r'''			if (!add_child(claim_batch_marker_size) || !add_child(claims_tuple_size) ||
				!add_child(9U) || !add_child(9U) || !add_child(9U))
''',
    r'''			if (!add_child(claim_batch_marker_size) || !add_child(claims_tuple_size) ||
				!add_child(unresolved_tuple_size) || !add_child(9U) || !add_child(9U))
''',
)
replace_exact(
    bounded_path,
    r'''			for (std::size_t index{}; index < 3U; ++index)
			{
				const auto empty_length = encode_u64(9U);
				if (auto updated = update_digest(*digest, empty_length, "claims.digest"); !updated)
					return sdk::unexpected(std::move(updated.error()));
				if (auto updated = update_empty_tuple(*digest, "claims.digest"); !updated)
					return sdk::unexpected(std::move(updated.error()));
			}
''',
    r'''			const auto unresolved_length = encode_u64(unresolved_tuple_size);
			if (auto updated =
					update_digest(*digest, unresolved_length, "claims.digest");
				!updated)
				return sdk::unexpected(std::move(updated.error()));
			if (auto updated =
					update_digest(*digest, *unresolved_binary, "claims.digest");
				!updated)
				return sdk::unexpected(std::move(updated.error()));
			for (std::size_t index{}; index < 2U; ++index)
			{
				const auto empty_length = encode_u64(9U);
				if (auto updated = update_digest(*digest, empty_length, "claims.digest"); !updated)
					return sdk::unexpected(std::move(updated.error()));
				if (auto updated = update_empty_tuple(*digest, "claims.digest"); !updated)
					return sdk::unexpected(std::move(updated.error()));
			}
''',
)
replace_exact(
    bounded_path,
    r'''			// The task contract above is the sole authority for these two zero-valued fields. This
			// bounded status path deliberately does not materialize or recompute their detail rows.
			return materialization_bounded_claim_batch_status{
				stream_digest, claim_count, 0U, 0U, 0U, partitions_.size()};
''',
    r'''			// Functional conflicts and differential disagreements remain fatal at task admission.
			return materialization_bounded_claim_batch_status{
				stream_digest, claim_count, unresolved_count, 0U, 0U, partitions_.size()};
''',
)

# Focused regression coverage in the existing Store test target avoids a new asset/CMake surface.
test_path = "tests/adapter/clang22/materialization_store_test.cpp"
replace_exact(
    test_path,
    '#include "llvm/clang22/materialization_store.hpp"\n',
    '#include "llvm/clang22/materialization_bounded_claim_source.hpp"\n'
    '#include "llvm/clang22/materialization_store.hpp"\n',
)
replace_exact(
    test_path,
    "#include <optional>\n",
    "#include <optional>\n#include <span>\n",
)

# Add soft-reference fixture helpers after the existing engine helper.
marker = r'''	[[nodiscard]] sdk::snapshot_series_selector selector(const sdk::relation_engine& value)
'''
helpers = r'''
	[[nodiscard]] sdk::relation_descriptor soft_target_descriptor()
	{
		sdk::relation_descriptor value;
		value.id = "company.test.target.v1";
		value.name = "company.test.target";
		value.version = {1U, 0U, 0U};
		value.semantic_major = 1U;
		value.semantics = "company.test.target/1";
		value.owner_namespace = "company.test";
		value.columns = {
			{"company.test.target.v1.key",
			 "key",
			 {sdk::scalar_kind::typed_id, "company_target_id", false},
			 true,
			 sdk::column_role::claim_key},
		};
		value.key_columns = {"company.test.target.v1.key"};
		value.merge = sdk::merge_mode::set;
		value.descriptor_digest =
			*sdk::semantic_digest("cxxlens.relation-descriptor-binding.v2",
								  value.contract_digest + "\n" + value.canonical_form());
		return value;
	}

	[[nodiscard]] sdk::relation_descriptor soft_source_descriptor()
	{
		const auto target = soft_target_descriptor();
		sdk::relation_descriptor value;
		value.id = "company.test.source.v1";
		value.name = "company.test.source";
		value.version = {1U, 0U, 0U};
		value.semantic_major = 1U;
		value.semantics = "company.test.source/1";
		value.owner_namespace = "company.test";
		value.columns = {
			{"company.test.source.v1.key",
			 "key",
			 {sdk::scalar_kind::typed_id, "company_source_id", false},
			 true,
			 sdk::column_role::claim_key},
			{"company.test.source.v1.target",
			 "target",
			 {sdk::scalar_kind::typed_id, "company_target_id", false},
			 true,
			 sdk::column_role::authoritative_payload},
		};
		value.key_columns = {"company.test.source.v1.key"};
		value.references = {{
			{"company.test.source.v1.target"},
			target.name,
			{"company.test.target.v1.key"},
			sdk::reference_strength::soft_semantic,
			false,
		}};
		value.merge = sdk::merge_mode::set;
		value.descriptor_digest =
			*sdk::semantic_digest("cxxlens.relation-descriptor-binding.v2",
								  value.contract_digest + "\n" + value.canonical_form());
		return value;
	}

	[[nodiscard]] sdk::relation_engine soft_reference_engine()
	{
		sdk::relation_registry registry;
		require(registry.add(soft_target_descriptor()).has_value(),
				"soft target descriptor registration failed");
		require(registry.add(soft_source_descriptor()).has_value(),
				"soft source descriptor registration failed");
		auto built = registry.build("engine-materialization-soft-unresolved-test");
		require(built.has_value(), "soft-reference relation engine build failed");
		return std::move(*built);
	}

	[[nodiscard]] sdk::detached_row soft_target_row(std::string key)
	{
		const auto relation = soft_target_descriptor();
		sdk::row_builder builder{relation};
		require(builder
					.set({relation.id, relation.columns[0U].id, relation.columns[0U].type},
						 sdk::detached_cell::typed("company_target_id", std::move(key)))
					.has_value(),
				"soft target row key rejected");
		auto finished = std::move(builder).finish();
		require(finished.has_value(), "soft target row did not finish");
		return std::move(*finished);
	}

	[[nodiscard]] sdk::detached_row
	soft_source_row(std::string key, std::string target)
	{
		const auto relation = soft_source_descriptor();
		sdk::row_builder builder{relation};
		require(builder
					.set({relation.id, relation.columns[0U].id, relation.columns[0U].type},
						 sdk::detached_cell::typed("company_source_id", std::move(key)))
					.has_value(),
				"soft source row key rejected");
		require(builder
					.set({relation.id, relation.columns[1U].id, relation.columns[1U].type},
						 sdk::detached_cell::typed("company_target_id", std::move(target)))
					.has_value(),
				"soft source row target rejected");
		auto finished = std::move(builder).finish();
		require(finished.has_value(), "soft source row did not finish");
		return std::move(*finished);
	}

	[[nodiscard]] sdk::claim soft_claim(const sdk::relation_engine& value,
										sdk::detached_row row_value,
										std::string provenance)
	{
		constexpr std::string_view producer_digest =
			"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
		sdk::observation observation{
			std::move(row_value),
			{"universe-materialization-soft-unresolved", {"all"}},
			"company.test.canonical-1",
			{"company.test.provider", std::string{producer_digest}},
			{"sha256:9999999999999999999999999999999999999999999999999999999999999999"},
			std::move(provenance),
			{"exact", "partition", "assumptions:none", {"schema_validated"}},
		};
		auto claim = sdk::make_assertion(value, std::move(observation));
		require(claim.has_value(), "soft-reference test claim rejected");
		return std::move(*claim);
	}

	[[nodiscard]] std::vector<sdk::unresolved_reference>
	soft_unresolved_for(const sdk::relation_engine& value, const sdk::claim& claim)
	{
		sdk::claim_batch batch;
		require(batch.add(claim).has_value(), "soft-reference claim batch add failed");
		auto committed = std::move(batch).commit(value);
		require(committed.has_value() && committed->claims.size() == 1U &&
					committed->unresolved.size() == 1U &&
					committed->unresolved.front().reason == "soft-reference-missing",
				"SDK reference authority did not produce one soft unresolved");
		return committed->unresolved;
	}

	[[nodiscard]] std::string stored_claim_ref(const sdk::claim& claim)
	{
		auto singleton = sdk::claim_batch_content_digest(
			std::span<const sdk::claim>{&claim, 1U}, {}, {}, {});
		require(singleton.has_value(), "soft-reference singleton digest failed");
		const std::array fields{
			sdk::canonical_value::from_string("stored_final"),
			sdk::canonical_value::from_string(*singleton),
		};
		auto reference =
			sdk::canonical_identity_digest("materialization-claim-envelope", fields);
		require(reference.has_value(), "soft-reference claim ref failed");
		return std::move(*reference);
	}

	[[nodiscard]] materialization_bounded_task_claims
	soft_bounded_task(const sdk::relation_engine& value,
					  sdk::claim claim,
					  std::vector<sdk::unresolved_reference> unresolved,
					  const std::string& suffix)
	{
		auto claim_ref = stored_claim_ref(claim);
		auto singleton = sdk::claim_batch_content_digest(
			std::span<const sdk::claim>{&claim, 1U}, {}, {}, {});
		require(singleton.has_value(), "soft-reference singleton digest failed");
		auto basis = sdk::claim_input_basis_digest(claim.input_basis);
		require(basis.has_value(), "soft-reference input basis digest failed");

		sdk::partition_draft draft;
		draft.relation_descriptor_id = claim.descriptor;
		draft.scope = claim.guarantee.scope;
		draft.condition = claim.presence;
		draft.interpretation = claim.interpretation;
		draft.producer_semantics = claim.producer.semantic_contract;
		draft.producer_input_basis_digest = std::move(*basis);
		draft.precision_profile = claim.guarantee.approximation;
		draft.assumption_set_id = claim.guarantee.assumptions;
		draft.claims = {claim};
		draft.coverage = {{"materialization.task", "task:" + suffix, "covered", ""}};
		draft.unresolved = std::move(unresolved);
		auto manifest = sdk::make_partition_manifest(value, draft);
		require(manifest.has_value(), "soft-reference partition manifest failed");
		const sdk::snapshot_partition_binding binding{
			manifest->partition_id,
			draft.relation_descriptor_id,
			draft.scope,
			draft.condition,
			draft.interpretation,
			draft.producer_semantics,
			draft.producer_input_basis_digest,
			draft.precision_profile,
			draft.assumption_set_id,
		};
		materialization_claim_envelope envelope{
			"stored_final",
			"row:" + suffix,
			claim_ref,
			std::move(*singleton),
			claim,
		};
		materialization_origin_association association{
			"association:" + suffix,
			claim_ref,
			{"provider-task:" + suffix,
			 "sha256:1111111111111111111111111111111111111111111111111111111111111111",
			 "catalog-unit:" + suffix,
			 "compile-unit:" + suffix,
			 "universe-materialization-soft-unresolved",
			 "all",
			 "company.test.canonical-1"},
			claim.content,
			std::nullopt,
		};
		materialization_claim_partition partition{
			std::move(draft),
			std::move(*manifest),
			binding,
			{claim_ref},
			{claim.content},
			1U,
			1U,
			false,
		};
		return {
			"sha256:2222222222222222222222222222222222222222222222222222222222222222",
			"sha256:3333333333333333333333333333333333333333333333333333333333333333",
			"sha256:4444444444444444444444444444444444444444444444444444444444444444",
			"sha256:5555555555555555555555555555555555555555555555555555555555555555",
			"assumptions:none",
			{std::move(envelope)},
			{},
			{std::move(association)},
			{std::move(partition)},
		};
	}

	[[nodiscard]] materialization_bounded_claim_source
	begin_soft_source(const sdk::relation_engine& value, const std::uint64_t task_count)
	{
		auto source = materialization_bounded_claim_source::begin(
			"materialization-request:soft-unresolved", value, task_count);
		require(source.has_value(), "soft-reference bounded source begin failed");
		return std::move(*source);
	}

'''
insert_before(test_path, marker, helpers)

# Add the focused end-to-end tests before the existing SQLite reopen test.
test_marker = r'''	void sqlite_reopen_failure_retains_commit()
'''
test_code = r'''
	void bounded_soft_unresolved_is_published_losslessly()
	{
		const auto value = soft_reference_engine();
		const auto source_claim = soft_claim(
			value,
			soft_source_row("source:one", "target:missing"),
			"evidence:soft-source");
		const auto unresolved = soft_unresolved_for(value, source_claim);
		auto source = begin_soft_source(value, 1U);
		require(source
					.consume_task(soft_bounded_task(
						value, source_claim, unresolved, "source-only"))
					.has_value(),
				"soft unresolved task was rejected by bounded source");
		auto sealed = std::move(source).finalize();
		require(sealed.has_value(), "soft unresolved bounded source did not seal");
		auto replay_source = std::move(*sealed);

		auto status = replay_source.claim_batch_status();
		require(status.has_value() && !status->content_digest.empty() &&
					status->claim_count == 1U && status->unresolved_count == 1U &&
					status->conflict_count == 0U &&
					status->differential_disagreement_count == 0U,
				"soft unresolved claim-batch status was not canonical");
		auto expected_digest = sdk::claim_batch_content_digest(
			std::span<const sdk::claim>{&source_claim, 1U}, unresolved, {}, {});
		require(expected_digest.has_value() &&
					status->content_digest == *expected_digest,
				"bounded soft unresolved digest differs from SDK authority");

		std::vector<sdk::partition_draft> replayed;
		require(replay_source
					.replay([&](sdk::partition_draft draft) -> sdk::result<void>
							{
								replayed.push_back(std::move(draft));
								return {};
							})
					.has_value(),
				"soft unresolved partition replay failed");
		require(replayed.size() == 1U && replayed.front().claims.size() == 1U &&
					replayed.front().unresolved == unresolved,
				"soft unresolved partition lost source claim or typed unresolved");
		auto manifest = sdk::make_partition_manifest(value, replayed.front());
		require(manifest.has_value() && !manifest->complete,
				"soft unresolved partition was falsely marked complete");

		const auto selector_value = selector(value);
		const auto memory_request =
			publication_request(selector_value, "memory", std::nullopt);
		streaming_prepared_store_transaction memory_plan{
			{memory_request.selector,
			 {1U, 0U, 0U},
			 "sha256:6666666666666666666666666666666666666666666666666666666666666666",
			 memory_request.expected_parent_publication},
			{},
			{}};
		auto memory = execute_materialization_store_streaming(
			value, memory_request, std::move(memory_plan), replay_source);
		require(!memory.first_issue && memory.publish_returned_record &&
					memory.publish_returned_handle && memory.verification_store,
				"memory Store rejected a valid soft unresolved publication");
		require(memory.publish_returned_handle->unresolved_items() == unresolved,
				"memory Store lost the typed unresolved record");
		auto source_relation = value.require_id(soft_source_descriptor().id);
		require(source_relation.has_value(), "soft source relation lookup failed");
		auto rows = memory.publish_returned_handle->open(*source_relation);
		require(rows.has_value(), "soft source row query was unavailable");
		auto first = rows->next();
		require(first && first->has_value(),
				"soft unresolved source row was discarded");
		auto end = rows->next();
		require(end && !*end, "soft unresolved source row query was not finite");
		auto memory_export = memory.verification_store->canonical_export(
			memory.publish_returned_record->snapshot_id);
		require(memory_export.has_value(), "memory soft unresolved export failed");

		temporary_working_directory working_directory;
		const auto sqlite_request = publication_request(
			selector_value, "sqlite", std::nullopt, "soft-unresolved.sqlite");
		streaming_prepared_store_transaction sqlite_plan{
			{sqlite_request.selector,
			 {1U, 0U, 0U},
			 "sha256:6666666666666666666666666666666666666666666666666666666666666666",
			 sqlite_request.expected_parent_publication},
			{},
			{}};
		auto sqlite = execute_materialization_store_streaming(
			value, sqlite_request, std::move(sqlite_plan), replay_source);
		if (sqlite.first_issue)
		{
			const auto* unavailable =
				std::get_if<materialization_store_sdk_failure>(&*sqlite.first_issue);
			require(unavailable &&
						unavailable->operation == materialization_store_operation::store_open &&
						unavailable->error.code == "store.backend-unavailable" &&
						unavailable->error.field == "sqlite" &&
						unavailable->error.detail == "source-shm-readonly-qualification",
					"SQLite soft unresolved publication failed for an unexpected reason");
			return;
		}
		require(sqlite.publish_returned_record && sqlite.verification_store,
				"SQLite soft unresolved publication returned no committed record");
		sqlite.verification_store.reset();
		auto reopened =
			sdk::open_sqlite_snapshot_store("soft-unresolved.sqlite", value);
		require(reopened.has_value(), "SQLite soft unresolved Store did not reopen");
		auto current = reopened->current(selector_value);
		require(current.has_value() && current->unresolved_items() == unresolved,
				"reopened SQLite Store lost the typed unresolved record");
		auto sqlite_export = reopened->canonical_export(current->id());
		require(sqlite_export.has_value() && *sqlite_export == *memory_export,
				"memory and reopened SQLite soft unresolved exports diverged");
	}

	struct soft_source_summary
	{
		materialization_bounded_claim_batch_status status;
		std::vector<sdk::partition_manifest> manifests;
	};

	[[nodiscard]] soft_source_summary
	run_cross_task_soft_resolution(const sdk::relation_engine& value,
								   const sdk::claim& source_claim,
								   const sdk::claim& target_claim,
								   const bool reverse)
	{
		auto source = begin_soft_source(value, 2U);
		auto source_task = soft_bounded_task(
			value, source_claim, soft_unresolved_for(value, source_claim), "source");
		auto target_task =
			soft_bounded_task(value, target_claim, {}, "target");
		if (reverse)
		{
			require(source.consume_task(std::move(target_task)).has_value(),
					"reverse target task consumption failed");
			require(source.consume_task(std::move(source_task)).has_value(),
					"reverse source task consumption failed");
		}
		else
		{
			require(source.consume_task(std::move(source_task)).has_value(),
					"source task consumption failed");
			require(source.consume_task(std::move(target_task)).has_value(),
					"target task consumption failed");
		}
		auto sealed = std::move(source).finalize();
		require(sealed.has_value(), "cross-task soft resolution did not seal");
		auto replay_source = std::move(*sealed);
		auto status = replay_source.claim_batch_status();
		require(status.has_value(), "cross-task soft resolution status failed");
		std::vector<sdk::partition_manifest> manifests;
		require(replay_source
					.replay([&](sdk::partition_draft draft) -> sdk::result<void>
							{
								require(draft.unresolved.empty(),
										"cross-task target left a residual unresolved");
								auto manifest = sdk::make_partition_manifest(value, draft);
								require(manifest.has_value() && manifest->complete,
										"resolved cross-task partition was not complete");
								manifests.push_back(std::move(*manifest));
								return {};
							})
					.has_value(),
				"cross-task soft resolution replay failed");
		std::ranges::sort(manifests, {}, &sdk::partition_manifest::partition_id);
		return {std::move(*status), std::move(manifests)};
	}

	void bounded_cross_task_soft_resolution_is_order_independent()
	{
		const auto value = soft_reference_engine();
		const auto source_claim = soft_claim(
			value,
			soft_source_row("source:one", "target:shared"),
			"evidence:soft-source");
		const auto target_claim = soft_claim(
			value,
			soft_target_row("target:shared"),
			"evidence:soft-target");
		const auto forward =
			run_cross_task_soft_resolution(value, source_claim, target_claim, false);
		const auto reverse =
			run_cross_task_soft_resolution(value, source_claim, target_claim, true);
		require(forward.status.unresolved_count == 0U &&
					reverse.status.unresolved_count == 0U &&
					forward.status.claim_count == 2U &&
					reverse.status.claim_count == 2U &&
					forward.status.content_digest == reverse.status.content_digest &&
					forward.manifests == reverse.manifests,
				"cross-task soft resolution depends on task order");
		const std::array claims{source_claim, target_claim};
		auto expected = sdk::claim_batch_content_digest(claims, {}, {}, {});
		require(expected.has_value() &&
					forward.status.content_digest == *expected,
				"cross-task bounded digest differs from SDK authority");
	}

'''
insert_before(test_path, test_marker, test_code)

replace_exact(
    test_path,
    r'''	streaming_store_replays_and_rechecks_exact_partitions();
	sqlite_reopen_failure_retains_commit();
''',
    r'''	streaming_store_replays_and_rechecks_exact_partitions();
	bounded_soft_unresolved_is_published_losslessly();
	bounded_cross_task_soft_resolution_is_order_independent();
	sqlite_reopen_failure_retains_commit();
''',
)

print("Applied issue #272 soft unresolved materialization patch.")
