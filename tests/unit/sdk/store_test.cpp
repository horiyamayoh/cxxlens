#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <cxxlens/sdk.hpp>

namespace cxxlens::sdk
{
	result<void> rewrite_publication_payload_for_testing(
		snapshot_store&, std::string_view, std::string_view, std::string_view, std::size_t);
} // namespace cxxlens::sdk

namespace
{
	void require(const bool condition, const std::string& message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] cxxlens::sdk::relation_descriptor descriptor()
	{
		cxxlens::sdk::relation_descriptor value;
		value.id = "company.test.item.v1";
		value.name = "company.test.item";
		value.version = {1U, 0U, 0U};
		value.semantic_major = 1U;
		value.semantics = "company.test.item/1";
		value.owner_namespace = "company.test";
		value.columns = {
			{"company.test.item.v1.key",
			 "key",
			 {cxxlens::sdk::scalar_kind::typed_id, "company_item_id", false},
			 true,
			 cxxlens::sdk::column_role::claim_key},
			{"company.test.item.v1.value",
			 "value",
			 {cxxlens::sdk::scalar_kind::utf8_string, {}, false},
			 true,
			 cxxlens::sdk::column_role::authoritative_payload},
		};
		value.key_columns = {"company.test.item.v1.key"};
		value.merge = cxxlens::sdk::merge_mode::set;
		value.descriptor_digest =
			*cxxlens::sdk::semantic_digest("cxxlens.relation-descriptor-binding.v2",
										   value.contract_digest + "\n" + value.canonical_form());
		return value;
	}

	[[nodiscard]] cxxlens::sdk::relation_engine engine()
	{
		cxxlens::sdk::relation_registry registry;
		require(registry.add(descriptor()).has_value(), "store descriptor rejected");
		auto built = registry.build("engine-ng0-test");
		require(built.has_value(), "store engine rejected");
		return std::move(*built);
	}

	[[nodiscard]] cxxlens::sdk::detached_row row(std::string key, std::string payload)
	{
		auto relation = descriptor();
		cxxlens::sdk::row_builder builder{relation};
		require(builder
					.set({relation.id, relation.columns[0].id, relation.columns[0].type},
						 cxxlens::sdk::detached_cell::typed("company_item_id", std::move(key)))
					.has_value(),
				"store row key rejected");
		require(builder
					.set({relation.id, relation.columns[1].id, relation.columns[1].type},
						 cxxlens::sdk::detached_cell::utf8(std::move(payload)))
					.has_value(),
				"store row payload rejected");
		auto finished = std::move(builder).finish();
		require(finished.has_value(), "store row did not finish");
		return std::move(*finished);
	}

	constexpr std::string_view producer_digest =
		"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

	[[nodiscard]] cxxlens::sdk::claim
	make_claim(const cxxlens::sdk::relation_engine& value, std::string key, std::string payload)
	{
		cxxlens::sdk::observation observed{
			row(std::move(key), std::move(payload)),
			{"universe-1", {"all"}},
			"company.test.canonical-1",
			{"company.test.provider", std::string{producer_digest}},
			{"sha256:9999999999999999999999999999999999999999999999999999999999999999"},
			"evidence:root",
			{"exact", "partition", "assumptions:none", {"schema_validated"}},
		};
		auto claim = cxxlens::sdk::make_assertion(value, std::move(observed));
		require(claim.has_value(), "store claim rejected");
		return std::move(*claim);
	}

	[[nodiscard]] cxxlens::sdk::partition_draft partition(
		const cxxlens::sdk::relation_engine& value, const bool reverse, const bool partial = false)
	{
		cxxlens::sdk::partition_draft draft;
		draft.relation_descriptor_id = descriptor().id;
		draft.scope = "compile-unit-1";
		draft.condition = {"universe-1", {"all"}};
		draft.interpretation = "company.test.canonical-1";
		draft.producer_semantics = producer_digest;
		draft.precision_profile = "exact";
		draft.assumption_set_id = "assumptions-empty";
		draft.claims = {make_claim(value, "item:1", "one"), make_claim(value, "item:2", "two")};
		auto basis = cxxlens::sdk::claim_input_basis_digest(draft.claims.front().input_basis);
		require(basis.has_value(), "partition basis digest rejected");
		draft.producer_input_basis_digest = std::move(*basis);
		if (reverse)
			std::ranges::reverse(draft.claims);
		draft.coverage = {{"compile-unit",
						   "compile-unit-1",
						   partial ? "unknown" : "covered",
						   partial ? "provider-partial" : ""}};
		if (partial)
			draft.unresolved.push_back({draft.claims.front().assertion,
										descriptor().id,
										"company.test.target",
										{"company.test.item.v1.key"},
										"soft-reference-missing"});
		return draft;
	}

	[[nodiscard]] cxxlens::sdk::snapshot_series_selector
	selector(const cxxlens::sdk::relation_engine& value)
	{
		return {"catalog-1",
				"stable",
				std::string{value.generation()},
				"universe-1",
				std::string{value.registry_digest()},
				"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
				"sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"};
	}

	[[nodiscard]] cxxlens::sdk::snapshot_draft
	snapshot_draft(const cxxlens::sdk::relation_engine& value,
				   std::optional<std::string> parent = std::nullopt)
	{
		return {selector(value),
				{1U, 0U, 0U},
				"sha256:6666666666666666666666666666666666666666666666666666666666666666",
				std::move(parent)};
	}

	[[nodiscard]] cxxlens::sdk::partition_certificate_subject
	certificate_subject(const cxxlens::sdk::partition_manifest& manifest,
						const cxxlens::sdk::partition_draft& draft)
	{
		auto subject =
			cxxlens::sdk::make_partition_certificate_subject(manifest,
															 {manifest.partition_id,
															  draft.relation_descriptor_id,
															  draft.scope,
															  draft.condition,
															  draft.interpretation,
															  draft.producer_semantics,
															  draft.producer_input_basis_digest,
															  draft.precision_profile,
															  draft.assumption_set_id});
		require(subject.has_value(), "partition certificate subject rejected");
		return std::move(*subject);
	}

	[[nodiscard]] cxxlens::sdk::snapshot_handle
	publish(cxxlens::sdk::snapshot_store& store,
			const cxxlens::sdk::relation_engine& value,
			const bool reverse,
			std::optional<std::string> parent = std::nullopt)
	{
		auto writer = store.begin(snapshot_draft(value, std::move(parent)));
		require(writer.has_value(), "store writer did not begin");
		require(writer->stage(partition(value, reverse)).has_value(),
				"store partition did not stage");
		require(writer->validate().has_value(), "store candidate did not validate");
		auto published = writer->publish();
		require(published.has_value(), "store candidate did not publish");
		return std::move(*published);
	}

	void check_canonical_vectors()
	{
		using cxxlens::sdk::canonical_value;
		const std::array fields{canonical_value::from_string("cxxlens"),
								canonical_value::from_integer(1),
								canonical_value::from_boolean(true),
								canonical_value::null()};
		require(cxxlens::sdk::canonical_identity_digest("canonical-vector", fields) ==
					"canonical-vector:sha256:"
					"ee43d1e2b86e53b4b52e5452eec0fd6843e858cc0f0230b7f5bf7922b47dde90",
				"canonical tuple domain vector diverged");

		const std::array key_fields{
			canonical_value::from_string("cc.call_site.v1"),
			canonical_value::from_integer(1),
			canonical_value::from_tuple({canonical_value::from_string("call-1")})};
		const auto key = cxxlens::sdk::canonical_identity_digest("semantic-key", key_fields);
		require(key ==
					"semantic-key:sha256:"
					"ac3f63290b7c6ebdc6f333eea07752bd32c5b0fa6df8b2ea56f136486e42aae4",
				"semantic key vector diverged");
		const std::array assertion_fields{
			canonical_value::from_string(key),
			canonical_value::from_string("universe-1"),
			canonical_value::from_string("condition-1"),
			canonical_value::from_string("cc.canonical-1"),
			canonical_value::from_string(std::string{producer_digest})};
		const auto assertion =
			cxxlens::sdk::canonical_identity_digest("assertion", assertion_fields);
		require(
			assertion ==
				"assertion:sha256:83dc78eb2a592a09badb3eb16199aa25de05b7e4283a0281d56eea3ca6f8b5dc",
			"assertion vector diverged");
		const std::array content_fields{
			canonical_value::from_string(assertion),
			canonical_value::from_tuple({canonical_value::from_string("compile-unit-1"),
										 canonical_value::from_string("direct")})};
		require(cxxlens::sdk::canonical_identity_digest("claim-content", content_fields) ==
					"claim-content:sha256:"
					"3ea3d46232c1c1f29193563bd63e175e3bb0bf96b98f8d2f45602e46853f47d2",
				"claim content vector diverged");
	}

	void check_backend_parity()
	{
		auto relation_engine = engine();
		auto memory = cxxlens::sdk::make_in_memory_snapshot_store(relation_engine);
		require(memory.has_value(), "memory store unavailable");
		const auto path = std::filesystem::temp_directory_path() /
			(std::string{"cxxlens-ng-store-"} +
			 std::string{relation_engine.registry_digest().substr(7U, 12U)} + ".sqlite");
		std::filesystem::remove(path);
		auto sqlite = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
		require(sqlite.has_value(), "SQLite store unavailable");

		auto memory_writer = memory->begin(snapshot_draft(relation_engine));
		require(memory_writer && memory_writer->stage(partition(relation_engine, false)),
				"memory staged candidate rejected");
		auto staged_invisible = memory->current(selector(relation_engine));
		require(!staged_invisible && staged_invisible.error().code == "store.current-not-found",
				"staged claims became visible");
		memory_writer->cancel();
		require(memory_writer->state() == cxxlens::sdk::publication_state::rolled_back,
				"cancelled writer did not roll back");

		auto memory_snapshot = publish(*memory, relation_engine, false);
		auto sqlite_snapshot = publish(*sqlite, relation_engine, true);
		require(memory_snapshot.id() == sqlite_snapshot.id(),
				"memory/SQLite snapshot IDs diverged under reverse order");
		auto memory_export = memory->canonical_export(memory_snapshot.id());
		auto sqlite_export = sqlite->canonical_export(sqlite_snapshot.id());
		require(memory_export && sqlite_export && *memory_export == *sqlite_export,
				"memory/SQLite canonical exports diverged");

		auto dynamic = relation_engine.require("company.test.item", 1U);
		require(dynamic.has_value(), "store dynamic relation unavailable");
		auto cursor = sqlite_snapshot.open(*dynamic);
		require(cursor.has_value(), "SQLite cursor unavailable");
		auto first = cursor->next();
		require(first && first->has_value() && (*first)->copy(), "SQLite row view unavailable");
		require(cursor->next().has_value(), "SQLite cursor advance failed");
		auto expired = (*first)->copy();
		require(!expired && expired.error().code == "sdk.row-view-expired",
				"cursor advance did not expire its view");

		const auto prior_publication = std::string{sqlite_snapshot.publication().publication_id};
		auto second = publish(*sqlite, relation_engine, false, prior_publication);
		require(second.id() == sqlite_snapshot.id() &&
					second.publication().sequence == sqlite_snapshot.publication().sequence + 1U,
				"publication identity leaked into semantic snapshot identity");
		auto stale = sqlite->begin(snapshot_draft(relation_engine, prior_publication));
		require(stale && stale->stage(partition(relation_engine, false)) && stale->validate(),
				"stale candidate setup failed");
		auto stale_publish = stale->publish();
		require(!stale_publish && stale_publish.error().code == "store.publish-stale-parent",
				"stale parent publish was accepted");
		auto current = sqlite->current(selector(relation_engine));
		require(current &&
					current->publication().publication_id == second.publication().publication_id,
				"failed publish changed the series head");
		auto prior = sqlite->open_publication(prior_publication);
		require(prior && prior->id() == second.id(),
				"explicit prior publication became unreadable");

		auto partial = partition(relation_engine, false, true);
		auto partial_manifest = cxxlens::sdk::make_partition_manifest(relation_engine, partial);
		require(partial_manifest && !partial_manifest->complete,
				"partial partition looked complete");
		cxxlens::sdk::closure_candidate closure{
			partial.relation_descriptor_id,
			partial_manifest->partition_id,
			partial_manifest->content_digest,
			partial_manifest->coverage_digest,
			"sha256:4444444444444444444444444444444444444444444444444444444444444444",
			partial.condition,
			partial.interpretation,
			partial.assumption_set_id,
			"relation-key-enumeration",
			partial.producer_semantics,
			"sha256:5555555555555555555555555555555555555555555555555555555555555555"};
		auto partial_subject = certificate_subject(*partial_manifest, partial);
		auto rejected_closure = cxxlens::sdk::make_closure_certificate(partial_subject, closure);
		require(!rejected_closure &&
					rejected_closure.error().code == "store.partial-partition-closure",
				"partial partition received a closure certificate");

		auto complete = partition(relation_engine, false);
		auto complete_manifest = cxxlens::sdk::make_partition_manifest(relation_engine, complete);
		require(complete_manifest.has_value(), "complete closure partition rejected");
		auto complete_subject = certificate_subject(*complete_manifest, complete);
		cxxlens::sdk::closure_candidate exact{
			complete.relation_descriptor_id,
			complete_manifest->partition_id,
			complete_manifest->content_digest,
			complete_manifest->coverage_digest,
			"sha256:4444444444444444444444444444444444444444444444444444444444444444",
			complete.condition,
			complete.interpretation,
			complete.assumption_set_id,
			"relation-key-enumeration",
			complete.producer_semantics,
			"sha256:5555555555555555555555555555555555555555555555555555555555555555"};
		auto first_certificate = cxxlens::sdk::make_closure_certificate(complete_subject, exact);
		auto second_certificate = cxxlens::sdk::make_closure_certificate(complete_subject, exact);
		const auto writer_accepts = [&](cxxlens::sdk::closure_candidate candidate)
		{
			auto isolated_store = cxxlens::sdk::make_in_memory_snapshot_store(relation_engine);
			require(isolated_store.has_value(), "closure parity store unavailable");
			auto writer = isolated_store->begin(snapshot_draft(relation_engine));
			require(writer && writer->stage(complete) && writer->add_closure(std::move(candidate)),
					"closure parity writer setup failed");
			return writer->validate().has_value();
		};
		require(first_certificate && second_certificate &&
					first_certificate->id == second_certificate->id && writer_accepts(exact),
				"exact closure certificate identity was not deterministic");
		const auto reject_mutation =
			[&](cxxlens::sdk::closure_candidate forged, const std::string& label)
		{
			const auto standalone =
				cxxlens::sdk::make_closure_certificate(complete_subject, forged);
			require(!standalone && !writer_accepts(std::move(forged)),
					"closure subject mismatch was accepted: " + label);
		};
		auto forged = exact;
		forged.condition.fragments = {"release"};
		reject_mutation(std::move(forged), "condition");
		forged = exact;
		forged.interpretation = "company.test.other-domain";
		reject_mutation(std::move(forged), "interpretation");
		forged = exact;
		forged.assumption_set_id = "assumptions-other";
		reject_mutation(std::move(forged), "assumption-set");
		forged = exact;
		forged.producer_semantics =
			"sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
		reject_mutation(std::move(forged), "producer-semantics");
		forged = exact;
		forged.key_domain_digest = "not-a-digest";
		reject_mutation(std::move(forged), "key-domain");
		forged = exact;
		forged.closure_kind = "unsupported-kind";
		reject_mutation(std::move(forged), "closure-kind");
		forged = exact;
		forged.evidence_digest = "not-a-digest";
		reject_mutation(std::move(forged), "evidence");

		const auto generations_before = sqlite->retained_generation_count();
		require(sqlite->compact().has_value(), "SQLite compaction failed");
		require(sqlite->retained_generation_count() > generations_before,
				"compaction reclaimed a pinned generation");
		const auto expected_id = std::string{second.id()};
		sqlite = cxxlens::sdk::result<cxxlens::sdk::snapshot_store>{
			cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine).value()};
		auto reopened = sqlite->current(selector(relation_engine));
		require(reopened && reopened->id() == expected_id,
				"SQLite reopen changed semantic identity or lost the head");
		require(cxxlens::sdk::mark_publication_corrupt_for_testing(
					*sqlite, reopened->publication().publication_id)
					.has_value(),
				"corruption fault injection failed");
		auto corrupt_current = sqlite->current(selector(relation_engine));
		require(!corrupt_current && corrupt_current.error().code == "store.current-corrupt",
				"corrupt current silently fell back to a prior publication");
		auto intact_prior = sqlite->open_publication(prior_publication);
		require(intact_prior && intact_prior->id() == expected_id,
				"corrupt current made an explicit intact prior unreadable");
		auto reopened_corrupt =
			cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
		require(reopened_corrupt.has_value(),
				"corrupt SQLite store could not reopen for diagnosis");
		auto diagnosed = reopened_corrupt->current(selector(relation_engine));
		require(!diagnosed && diagnosed.error().code == "store.current-corrupt",
				"reopen lost corrupt-current state");
		require(reopened_corrupt->open_publication(prior_publication).has_value(),
				"reopen lost an explicit intact prior publication");
		std::filesystem::remove(path);
	}

	void check_sqlite_semantic_graph_tamper()
	{
		struct mutation
		{
			std::string before;
			std::string after;
			std::size_t occurrence;
			std::string label;
		};
		const auto relation_engine = engine();
		std::vector mutations{
			mutation{"one", "uno", 1U, "annotation-row"},
			mutation{"one", "uno", 0U, "detached-row"},
			mutation{"one", "uno", 2U, "partition-envelope-row"},
			mutation{"all", "any", 0U, "condition"},
			mutation{"company.test.canonical-1", "company.test.canonical-2", 0U, "interpretation"},
			mutation{"company.test.provider", "company.test.provides", 0U, "producer"},
			mutation{"evidence:root", "evidence:boot", 0U, "provenance"},
			mutation{"partition", "statement", 0U, "guarantee"},
			mutation{"compile-unit-1", "compile-unit-2", 0U, "coverage"},
		};
		auto source_partition = partition(relation_engine, false);
		auto source_manifest =
			cxxlens::sdk::make_partition_manifest(relation_engine, source_partition);
		require(source_manifest.has_value(), "tamper source manifest rejected");
		auto changed_claim_set = source_manifest->claim_set_digest;
		changed_claim_set.back() = changed_claim_set.back() == '0' ? '1' : '0';
		mutations.push_back(
			{source_manifest->claim_set_digest, changed_claim_set, 0U, "claim-set-digest"});
		auto changed_content = source_manifest->content_digest;
		changed_content.back() = changed_content.back() == '0' ? '1' : '0';
		mutations.push_back(
			{source_manifest->content_digest, changed_content, 0U, "partition-content-digest"});
		auto claim_count_before = source_manifest->content_digest;
		claim_count_before.append(7U, '\0');
		claim_count_before.push_back('\2');
		claim_count_before.push_back('\1');
		auto claim_count_after = source_manifest->content_digest;
		claim_count_after.append(7U, '\0');
		claim_count_after.push_back('\3');
		claim_count_after.push_back('\1');
		mutations.push_back(
			{std::move(claim_count_before), std::move(claim_count_after), 0U, "claim-count"});
		for (std::size_t index = 0U; index < mutations.size(); ++index)
		{
			const auto path = std::filesystem::temp_directory_path() /
				("cxxlens-ng-store-tamper-" + std::to_string(index) + ".sqlite");
			std::filesystem::remove(path);
			{
				auto sqlite =
					cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
				require(sqlite.has_value(), "tamper SQLite store unavailable");
				auto snapshot = publish(*sqlite, relation_engine, false);
				const auto& item = mutations[index];
				require(cxxlens::sdk::rewrite_publication_payload_for_testing(
							*sqlite,
							snapshot.publication().publication_id,
							item.before,
							item.after,
							item.occurrence)
							.has_value(),
						"semantic payload mutation fixture failed: " + std::string{item.label});
			}
			auto reopened =
				cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
			require(reopened.has_value(),
					"tampered SQLite store could not reopen for diagnosis: " +
						std::string{mutations[index].label});
			auto current = reopened->current(selector(relation_engine));
			require(!current && current.error().code == "store.current-corrupt",
					"semantic payload mutation was accepted: " +
						std::string{mutations[index].label});
			auto compacted = reopened->compact();
			require(!compacted && compacted.error().code == "store.compact-validation-failed",
					"compaction accepted a corrupt semantic payload: " +
						std::string{mutations[index].label});
			std::filesystem::remove(path);
		}
	}
} // namespace

int main()
{
	check_canonical_vectors();
	check_backend_parity();
	check_sqlite_semantic_graph_tamper();
	return 0;
}
