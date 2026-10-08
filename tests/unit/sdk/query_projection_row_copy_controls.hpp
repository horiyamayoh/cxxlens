#pragma once

#include <algorithm>
#include <cstddef>

#include <cxxlens/sdk/query.hpp>

namespace query_copy_controls
{
	template <class Rows, class Names, class Input, class Project, class Check>
	void projection(Rows& rows, const Names& names, Input input, Project project, Check check)
	{
		namespace q = cxxlens::sdk::query;
		auto& entity = rows[3].front();
		auto bytes =
			cxxlens::sdk::detached_cell::bytes(std::vector<std::byte>(32768U, std::byte{0xff}));
		bytes.type = entity.values.at("output.provider_local_key").type;
		entity.values["output.provider_local_key"] = std::move(bytes);
		for (auto& group : rows)
			for (auto& row : group)
			{
				row.claim_contributors.push_back("claim:z");
				row.producer_contracts.push_back({"z.projected", "semantic:z"});
				row.provenance.push_back("zz:evidence");
				row.contributor_guarantees.push_back(
					{"exact", "zz", "zz", {"native", "schema_validated"}});
				row.contributor_edges.push_back({row.claim_contributors.back(),
												 row.producer_contracts.back(),
												 row.provenance.back(),
												 row.contributor_guarantees.back(),
												 row.presence,
												 row.interpretation});
			}
		const auto query = input();
		q::projection_resource_usage measured;
		const auto out = project(query, q::finite_population_limits{}, measured);
		check(bool(out), "typed copy enriched public projection");
		check(!out->evidence.empty(), "typed copy retained evidence");
		std::size_t evidence_bytes{};
		for (const auto& evidence : out->evidence)
		{
			const auto group = std::ranges::find(names, evidence.relation_id);
			check(group != names.end(), "typed copy retained relation identity");
			const auto index = static_cast<std::size_t>(group - names.begin());
			const auto canonical = evidence.row.canonical_form();
			check(std::ranges::any_of(rows[index],
									  [&](const auto& row)
									  {
										  return row.canonical_form() == canonical;
									  }),
				  "typed copy retained every original cell and annotation");
			evidence_bytes += canonical.size();
		}
		check(out->source_queries.has_value() &&
				  out->source_queries->snapshot_id == query.snapshot_id &&
				  out->source_queries->scans.size() == query.scans.size(),
			  "typed copy retained complete source query owner");
		for (std::size_t i{}; i < query.scans.size(); ++i)
			check(out->source_queries->scans[i].result.canonical_form() ==
					  query.scans[i].result.canonical_form(),
				  "typed copy retained query sidechannels");
		q::finite_population_limits exact;
		exact.maximum_operations = measured.operations;
		exact.maximum_retained_bytes = measured.retained_bytes_bound;
		exact.maximum_evidence_bytes = evidence_bytes;
		q::projection_resource_usage repeated;
		check(bool(project(query, exact, repeated)) && repeated.operations == measured.operations &&
				  repeated.retained_bytes_bound == measured.retained_bytes_bound,
			  "typed copy accepts exact work/storage/evidence caps");
		--exact.maximum_evidence_bytes;
		check(!project(query, exact, repeated) && !repeated.operations &&
				  !repeated.retained_bytes_bound,
			  "typed copy respects canonical evidence byte cap");
		exact.maximum_evidence_bytes = evidence_bytes;
		--exact.maximum_operations;
		check(!project(query, exact, repeated) && !repeated.operations &&
				  !repeated.retained_bytes_bound,
			  "typed copy respects actual work cap");
		exact.maximum_operations = measured.operations;
		--exact.maximum_retained_bytes;
		check(!project(query, exact, repeated) && !repeated.operations &&
				  !repeated.retained_bytes_bound,
			  "typed copy respects retained cap");
	}
} // namespace query_copy_controls
