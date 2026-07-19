#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <cxxlens/sdk.hpp>

namespace cxxlens::sdk
{
	result<void> rewrite_publication_payload_for_testing(
		snapshot_store&, std::string_view, std::string_view, std::string_view, std::size_t);
	result<void> rewrite_publication_identity_field_for_testing(snapshot_store&,
																std::string_view,
																std::string_view);
	result<std::string> rewrite_snapshot_version_for_testing(
		snapshot_store&, std::string_view, std::string_view, std::uint64_t, std::uint32_t);
	result<std::string> rewrite_publication_counters_for_testing(snapshot_store&,
																 std::string_view,
																 std::uint64_t,
																 std::uint64_t);
	result<void>
	poison_rejected_generation_for_testing(snapshot_store&, std::string_view, std::uint64_t);
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

	[[nodiscard]] std::string
	expected_publication_identity(const cxxlens::sdk::publication_record& value)
	{
		using cxxlens::sdk::canonical_value;
		constexpr auto signed_max =
			static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
		const auto canonical_sequence = value.sequence <= signed_max
			? static_cast<std::int64_t>(value.sequence)
			: -1 -
				static_cast<std::int64_t>(std::numeric_limits<std::uint64_t>::max() -
										  value.sequence);
		const std::array fields{
			canonical_value::from_string(value.series_id),
			canonical_value::from_string(value.snapshot_id),
			canonical_value::from_integer(canonical_sequence),
			canonical_value::from_string(value.parent_publication.value_or("")),
		};
		auto identity = cxxlens::sdk::canonical_identity_digest("publication", fields);
		require(identity.has_value(), "publication identity test oracle rejected a valid record");
		return std::move(*identity);
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

	[[nodiscard]] cxxlens::sdk::partition_draft
	occurrence_partition(const cxxlens::sdk::relation_engine& value, const bool reverse)
	{
		auto draft = partition(value, false);
		auto base = make_claim(value, "item:occurrence", "shared");
		auto provenance = base;
		provenance.provenance_root = "evidence:alternate";
		auto producer = base;
		producer.producer.id = "company.test.alternate-provider";
		auto guarantee = base;
		guarantee.guarantee.approximation = "under_approximation";
		draft.claims = {base, provenance, producer, guarantee, base};
		if (reverse)
			std::ranges::reverse(draft.claims);
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

	[[nodiscard]] cxxlens::sdk::result<cxxlens::sdk::snapshot_handle>
	try_publish(cxxlens::sdk::snapshot_store& store,
				const cxxlens::sdk::relation_engine& value,
				std::optional<std::string> parent,
				const std::string_view channel = "stable")
	{
		auto draft = snapshot_draft(value, std::move(parent));
		draft.series.channel_id = channel;
		auto writer = store.begin(std::move(draft));
		require(writer && writer->stage(partition(value, false)) && writer->validate(),
				"counter boundary publication setup failed");
		return writer->publish();
	}

	[[nodiscard]] cxxlens::sdk::partition_draft
	derived_partition(const cxxlens::sdk::relation_engine& value,
					  std::string input_snapshot,
					  std::vector<std::string> consumed)
	{
		auto inputs = partition(value, false).claims;
		const std::array input_span{inputs.front()};
		cxxlens::sdk::observation output{
			row("item:derived", "derived"),
			{"universe-1", {"all"}},
			"company.test.canonical-1",
			{"company.test.provider", std::string{producer_digest}},
			{"sha256:9999999999999999999999999999999999999999999999999999999999999999"},
			"evidence:derived",
			{"exact", "partition", "assumptions:none", {"schema_validated"}},
		};
		auto derived = cxxlens::sdk::make_derived_claim(
			value,
			input_span,
			std::move(output),
			std::move(input_snapshot),
			std::move(consumed),
			"sha256:7777777777777777777777777777777777777777777777777777777777777777");
		require(derived.has_value(), "derived store claim rejected");
		cxxlens::sdk::partition_draft draft;
		draft.relation_descriptor_id = descriptor().id;
		draft.scope = "compile-unit-derived";
		draft.condition = derived->presence;
		draft.interpretation = derived->interpretation;
		draft.producer_semantics = derived->producer.semantic_contract;
		draft.precision_profile = "exact";
		draft.assumption_set_id = "assumptions-empty";
		draft.claims = {std::move(*derived)};
		auto basis = cxxlens::sdk::claim_input_basis_digest(draft.claims.front().input_basis);
		require(basis.has_value(), "derived partition basis rejected");
		draft.producer_input_basis_digest = std::move(*basis);
		draft.coverage = {{"compile-unit", "compile-unit-derived", "covered", ""}};
		return draft;
	}

	[[nodiscard]] cxxlens::sdk::result<void>
	validate_derived_partition(cxxlens::sdk::snapshot_store& store,
							   const cxxlens::sdk::relation_engine& value,
							   const std::string& parent_publication,
							   std::string input_snapshot,
							   std::vector<std::string> consumed)
	{
		auto writer = store.begin(snapshot_draft(value, parent_publication));
		require(writer &&
					writer->stage(
						derived_partition(value, std::move(input_snapshot), std::move(consumed))),
				"derived writer setup failed");
		return writer->validate();
	}

	[[nodiscard]] bool validates_derived_partition(cxxlens::sdk::snapshot_store& store,
												   const cxxlens::sdk::relation_engine& value,
												   const std::string& parent_publication,
												   std::string input_snapshot,
												   std::vector<std::string> consumed)
	{
		return validate_derived_partition(
				   store, value, parent_publication, std::move(input_snapshot), std::move(consumed))
			.has_value();
	}

	void check_canonical_vectors()
	{
		using cxxlens::sdk::canonical_value;
		const std::array fields{canonical_value::from_string("cxxlens"),
								canonical_value::from_integer(1),
								canonical_value::from_boolean(true),
								canonical_value::null()};
		require(*cxxlens::sdk::canonical_identity_digest("canonical-vector", fields) ==
					"canonical-vector:sha256:"
					"ee43d1e2b86e53b4b52e5452eec0fd6843e858cc0f0230b7f5bf7922b47dde90",
				"canonical tuple domain vector diverged");

		const std::array key_fields{
			canonical_value::from_string("cc.call_site.v1"),
			canonical_value::from_integer(1),
			canonical_value::from_tuple({canonical_value::from_string("call-1")})};
		const auto key = cxxlens::sdk::canonical_identity_digest("semantic-key", key_fields);
		require(key &&
					*key ==
						"semantic-key:sha256:"
						"ac3f63290b7c6ebdc6f333eea07752bd32c5b0fa6df8b2ea56f136486e42aae4",
				"semantic key vector diverged");
		const std::array assertion_fields{
			canonical_value::from_string(*key),
			canonical_value::from_string("universe-1"),
			canonical_value::from_string("condition-1"),
			canonical_value::from_string("cc.canonical-1"),
			canonical_value::from_string(std::string{producer_digest})};
		const auto assertion =
			cxxlens::sdk::canonical_identity_digest("assertion", assertion_fields);
		require(assertion &&
					*assertion ==
						"assertion:sha256:"
						"83dc78eb2a592a09badb3eb16199aa25de05b7e4283a0281d56eea3ca6f8b5dc",
				"assertion vector diverged");
		const std::array content_fields{
			canonical_value::from_string(*assertion),
			canonical_value::from_tuple({canonical_value::from_string("compile-unit-1"),
										 canonical_value::from_string("direct")})};
		require(*cxxlens::sdk::canonical_identity_digest("claim-content", content_fields) ==
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
		require(!stale_publish && stale_publish.error().code == "store.publication-conflict",
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
		auto corrupt_semantic = sqlite->open(expected_id);
		require(!corrupt_semantic && corrupt_semantic.error().code == "store.snapshot-corrupt",
				"semantic snapshot open admitted corrupt latest publication");
		auto corrupt_export = sqlite->canonical_export(expected_id);
		require(!corrupt_export && corrupt_export.error().code == "store.snapshot-corrupt",
				"canonical export admitted corrupt latest publication");
		auto corrupt_explicit = sqlite->open_publication(reopened->publication().publication_id);
		require(!corrupt_explicit && corrupt_explicit.error().code == "store.publication-corrupt",
				"explicit publication open admitted corruption");
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
		auto diagnosed_semantic = reopened_corrupt->open(expected_id);
		require(!diagnosed_semantic && diagnosed_semantic.error().code == "store.snapshot-corrupt",
				"SQLite reopen silently fell back to an older semantic publication");
		require(reopened_corrupt->open_publication(prior_publication).has_value(),
				"reopen lost an explicit intact prior publication");
		auto compact_corrupt = reopened_corrupt->compact();
		require(!compact_corrupt &&
					compact_corrupt.error().code == "store.compact-validation-failed",
				"compaction erased the corruption verdict");

		auto corruption_memory = cxxlens::sdk::make_in_memory_snapshot_store(relation_engine);
		require(corruption_memory.has_value(), "memory corruption parity store unavailable");
		auto memory_first = publish(*corruption_memory, relation_engine, false);
		auto memory_second = publish(
			*corruption_memory, relation_engine, true, memory_first.publication().publication_id);
		require(memory_first.id() == memory_second.id(),
				"memory duplicate semantic snapshot setup diverged");
		require(cxxlens::sdk::mark_publication_corrupt_for_testing(
					*corruption_memory, memory_second.publication().publication_id)
					.has_value(),
				"memory corruption injection failed");
		auto memory_current = corruption_memory->current(selector(relation_engine));
		auto memory_semantic = corruption_memory->open(memory_second.id());
		auto memory_explicit =
			corruption_memory->open_publication(memory_second.publication().publication_id);
		auto corrupt_memory_export = corruption_memory->canonical_export(memory_second.id());
		require(!memory_current && memory_current.error().code == "store.current-corrupt" &&
					!memory_semantic && memory_semantic.error().code == "store.snapshot-corrupt" &&
					!memory_explicit &&
					memory_explicit.error().code == "store.publication-corrupt" &&
					!corrupt_memory_export &&
					corrupt_memory_export.error().code == "store.snapshot-corrupt",
				"memory and SQLite corruption read paths diverged");
		require(corruption_memory->open_publication(memory_first.publication().publication_id)
					.has_value(),
				"intact prior publication became unreadable");

		auto non_latest = cxxlens::sdk::make_in_memory_snapshot_store(relation_engine);
		require(non_latest.has_value(), "non-latest corruption store unavailable");
		auto non_latest_first = publish(*non_latest, relation_engine, false);
		auto non_latest_second = publish(
			*non_latest, relation_engine, true, non_latest_first.publication().publication_id);
		require(cxxlens::sdk::mark_publication_corrupt_for_testing(
					*non_latest, non_latest_first.publication().publication_id)
					.has_value(),
				"non-latest corruption injection failed");
		require(non_latest->open(non_latest_second.id()).has_value(),
				"corrupt non-latest publication shadowed the intact latest publication");
		auto rejected_non_latest =
			non_latest->open_publication(non_latest_first.publication().publication_id);
		require(!rejected_non_latest &&
					rejected_non_latest.error().code == "store.publication-corrupt",
				"explicit corrupt non-latest publication open succeeded");
		std::filesystem::remove(path);
	}

	[[nodiscard]] std::vector<std::string>
	occurrence_evidence(const cxxlens::sdk::snapshot_handle& snapshot)
	{
		auto cursor = snapshot.open_claims(descriptor().id);
		require(cursor.has_value(), "claim occurrence cursor unavailable");
		std::vector<std::string> output;
		while (true)
		{
			auto next = cursor->next();
			require(next.has_value(), "claim occurrence cursor failed");
			if (!*next)
				break;
			auto occurrence = (*next)->copy();
			require(occurrence.has_value(), "claim occurrence view unavailable");
			output.push_back(occurrence->producer.id + '|' + occurrence->provenance_root + '|' +
							 occurrence->guarantee.approximation);
		}
		std::ranges::sort(output);
		return output;
	}

	void check_occurrence_round_trip()
	{
		const auto relation_engine = engine();
		auto manifest = cxxlens::sdk::make_partition_manifest(
			relation_engine, occurrence_partition(relation_engine, false));
		require(manifest && manifest->claim_count == 1U,
				"evidence occurrences changed the semantic claim set");
		auto memory = cxxlens::sdk::make_in_memory_snapshot_store(relation_engine);
		require(memory.has_value(), "occurrence memory store unavailable");
		const auto path = std::filesystem::temp_directory_path() /
			(std::string{"cxxlens-ng-occurrence-"} +
			 std::string{relation_engine.registry_digest().substr(7U, 12U)} + ".sqlite");
		std::filesystem::remove(path);
		auto sqlite = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
		require(sqlite.has_value(), "occurrence SQLite store unavailable");
		const auto publish_occurrences =
			[&](cxxlens::sdk::snapshot_store& store, const bool reverse)
		{
			auto writer = store.begin(snapshot_draft(relation_engine));
			require(writer && writer->stage(occurrence_partition(relation_engine, reverse)) &&
						writer->validate(),
					"occurrence publication setup failed");
			auto published = writer->publish();
			require(published.has_value(), "occurrence publication failed");
			return std::move(*published);
		};
		auto memory_snapshot = publish_occurrences(*memory, false);
		auto sqlite_snapshot = publish_occurrences(*sqlite, true);
		const std::vector<std::string> expected{
			"company.test.alternate-provider|evidence:root|exact",
			"company.test.provider|evidence:alternate|exact",
			"company.test.provider|evidence:root|exact",
			"company.test.provider|evidence:root|under_approximation"};
		require(memory_snapshot.id() == sqlite_snapshot.id() &&
					occurrence_evidence(memory_snapshot) == expected &&
					occurrence_evidence(sqlite_snapshot) == expected,
				"memory/SQLite occurrence evidence was not lossless and canonical");
		auto relation = relation_engine.require_id(descriptor().id);
		require(relation.has_value(), "occurrence row relation unavailable");
		auto rows = sqlite_snapshot.open(*relation);
		require(rows.has_value(), "occurrence row cursor unavailable");
		auto semantic_row = rows->next();
		auto semantic_end = rows->next();
		require(semantic_row && *semantic_row && semantic_end && !*semantic_end,
				"evidence occurrences duplicated the semantic snapshot row");
		auto memory_export = memory->canonical_export(memory_snapshot.id());
		auto sqlite_export = sqlite->canonical_export(sqlite_snapshot.id());
		require(memory_export && sqlite_export && *memory_export == *sqlite_export,
				"occurrence canonical export depended on backend or input order");
		const auto publication_id = std::string{sqlite_snapshot.publication().publication_id};
		sqlite = cxxlens::sdk::result<cxxlens::sdk::snapshot_store>{
			cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine).value()};
		auto reopened = sqlite->open_publication(publication_id);
		require(reopened && occurrence_evidence(*reopened) == expected,
				"SQLite reopen lost claim occurrence evidence");
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

	void check_publication_identity_binding()
	{
		const auto relation_engine = engine();
		const std::array fields{"publication_id", "series_id", "snapshot_id", "sequence", "parent"};
		for (std::size_t index = 0U; index < fields.size(); ++index)
		{
			const auto path = std::filesystem::temp_directory_path() /
				("cxxlens-ng-publication-identity-" + std::to_string(index) + ".sqlite");
			std::filesystem::remove(path);
			std::string target;
			{
				auto store =
					cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
				require(store.has_value(), "publication identity tamper store unavailable");
				auto first = publish(*store, relation_engine, false);
				target = first.publication().publication_id;
				if (std::string_view{fields[index]} == "parent")
				{
					auto second = publish(*store, relation_engine, true, target);
					target = second.publication().publication_id;
				}
				require(cxxlens::sdk::rewrite_publication_identity_field_for_testing(
							*store, target, fields[index])
							.has_value(),
						"publication identity mutation fixture failed: " +
							std::string{fields[index]});
				if (std::string_view{fields[index]} == "publication_id")
					target.back() = target.back() == '0' ? '1' : '0';
			}
			auto reopened =
				cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
			require(reopened.has_value(),
					"identity-mismatched store could not reopen for diagnosis: " +
						std::string{fields[index]});
			auto opened = reopened->open_publication(target);
			require(!opened && opened.error().code == "store.publication-corrupt",
					"publication identity mutation was accepted: " + std::string{fields[index]});
			auto compacted = reopened->compact();
			require(!compacted && compacted.error().code == "store.compact-validation-failed",
					"compaction accepted publication identity mutation: " +
						std::string{fields[index]});
			std::filesystem::remove(path);
		}

		const auto path = std::filesystem::temp_directory_path() /
			"cxxlens-ng-publication-generation-identity.sqlite";
		std::filesystem::remove(path);
		auto store = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
		require(store.has_value(), "publication generation store unavailable");
		auto published = publish(*store, relation_engine, false);
		const auto publication_id = std::string{published.publication().publication_id};
		const auto generation = published.publication().physical_generation;
		require(publication_id == expected_publication_identity(published.publication()),
				"new publication did not satisfy its identity binding");
		require(store->compact().has_value(), "valid publication compaction failed");
		auto compacted = store->open_publication(publication_id);
		require(compacted && compacted->publication().physical_generation > generation &&
					compacted->publication().publication_id ==
						expected_publication_identity(compacted->publication()),
				"physical generation changed publication identity");
		store = cxxlens::sdk::result<cxxlens::sdk::snapshot_store>{
			cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine).value()};
		auto loaded = store->open_publication(publication_id);
		require(loaded &&
					loaded->publication().publication_id ==
						expected_publication_identity(loaded->publication()),
				"loaded publication escaped identity validation");
		std::filesystem::remove(path);
	}

	void check_snapshot_version_wire_bounds()
	{
		const auto relation_engine = engine();
		const std::array components{"major", "minor", "patch"};
		constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
		for (std::size_t component_index = 0U; component_index < components.size();
			 ++component_index)
		{
			for (const bool overflow : {false, true})
			{
				const auto path = std::filesystem::temp_directory_path() /
					("cxxlens-ng-snapshot-version-" + std::to_string(component_index) + "-" +
					 (overflow ? "overflow" : "maximum") + ".sqlite");
				std::filesystem::remove(path);
				std::string publication_id;
				{
					auto store =
						cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
					require(store.has_value(), "snapshot version boundary store unavailable");
					auto snapshot = publish(*store, relation_engine, false);
					auto rewritten = cxxlens::sdk::rewrite_snapshot_version_for_testing(
						*store,
						snapshot.publication().publication_id,
						components[component_index],
						overflow ? static_cast<std::uint64_t>(maximum) + 1U : maximum,
						overflow ? 0U : maximum);
					require(rewritten.has_value(),
							"snapshot version mutation fixture failed: " +
								std::string{components[component_index]});
					publication_id = std::move(*rewritten);
				}
				auto reopened =
					cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
				require(reopened.has_value(), "snapshot version boundary store did not reopen");
				auto opened = reopened->open_publication(publication_id);
				if (overflow)
				{
					require(!opened && opened.error().code == "store.publication-corrupt",
							"oversized snapshot version component was accepted: " +
								std::string{components[component_index]});
					auto compacted = reopened->compact();
					require(!compacted &&
								compacted.error().code == "store.compact-validation-failed",
							"compaction accepted oversized snapshot version component");
				}
				else
				{
					require(opened.has_value(),
							"UINT32_MAX snapshot version component was rejected: " +
								std::string{components[component_index]});
					const auto& version = opened->manifest().snapshot_semantics_version;
					const auto actual = component_index == 0U ? version.major
						: component_index == 1U				  ? version.minor
															  : version.patch;
					require(actual == maximum && reopened->compact().has_value(),
							"canonical maximum version payload was not byte-stable");
				}
				std::filesystem::remove(path);
			}
		}
	}

	void check_publication_counter_bounds()
	{
		const auto relation_engine = engine();
		constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
		const auto poison_path =
			std::filesystem::temp_directory_path() / "cxxlens-ng-corrupt-generation-poison.sqlite";
		std::filesystem::remove(poison_path);
		{
			auto store =
				cxxlens::sdk::open_sqlite_snapshot_store(poison_path.string(), relation_engine);
			require(store.has_value(), "generation poison store unavailable");
			auto snapshot = publish(*store, relation_engine, false);
			require(cxxlens::sdk::poison_rejected_generation_for_testing(
						*store, snapshot.publication().publication_id, maximum)
						.has_value(),
					"generation poison fixture failed");
		}
		auto poisoned =
			cxxlens::sdk::open_sqlite_snapshot_store(poison_path.string(), relation_engine);
		require(poisoned.has_value(), "poisoned generation store did not reopen");
		auto recovered = try_publish(*poisoned, relation_engine, std::nullopt, "recovery");
		require(recovered && recovered->publication().physical_generation == 1U,
				"valid-checksum rejected max generation poisoned the global counter");
		std::filesystem::remove(poison_path);

		enum class boundary_case
		{
			generation_max,
			sequence_max,
			generation_almost_max,
			sequence_almost_max,
		};
		const std::array cases{boundary_case::generation_max,
							   boundary_case::sequence_max,
							   boundary_case::generation_almost_max,
							   boundary_case::sequence_almost_max};
		for (const bool sqlite : {false, true})
		{
			for (std::size_t case_index = 0U; case_index < cases.size(); ++case_index)
			{
				const auto path = std::filesystem::temp_directory_path() /
					("cxxlens-ng-counter-boundary-" + std::string{sqlite ? "sqlite" : "memory"} +
					 "-" + std::to_string(case_index) + ".sqlite");
				std::filesystem::remove(path);
				auto store = sqlite
					? cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine)
					: cxxlens::sdk::make_in_memory_snapshot_store(relation_engine);
				require(store.has_value(), "counter boundary store unavailable");
				auto first = publish(*store, relation_engine, false);
				const bool generation_axis = cases[case_index] == boundary_case::generation_max ||
					cases[case_index] == boundary_case::generation_almost_max;
				const bool at_max = cases[case_index] == boundary_case::generation_max ||
					cases[case_index] == boundary_case::sequence_max;
				const auto sequence = generation_axis ? 1U : at_max ? maximum : maximum - 1U;
				const auto generation = generation_axis ? (at_max ? maximum : maximum - 1U) : 1U;
				auto rewritten = cxxlens::sdk::rewrite_publication_counters_for_testing(
					*store, first.publication().publication_id, sequence, generation);
				require(rewritten.has_value(), "counter boundary fixture failed");
				const auto parent = std::move(*rewritten);
				if (sqlite)
					store = cxxlens::sdk::result<cxxlens::sdk::snapshot_store>{
						cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine)
							.value()};

				if (cases[case_index] == boundary_case::generation_max)
				{
					auto compacted = store->compact();
					require(!compacted && compacted.error().code == "store.counter-overflow",
							"max generation compaction did not fail closed");
				}
				auto next = try_publish(*store, relation_engine, parent);
				if (at_max)
					require(!next && next.error().code == "store.counter-overflow",
							"max publication counter wrapped");
				else
				{
					require(next.has_value(), "max-minus-one counter did not allow one increment");
					auto overflow = try_publish(
						*store, relation_engine, std::string{next->publication().publication_id});
					require(!overflow && overflow.error().code == "store.counter-overflow",
							"counter allowed an increment after reaching max");
				}
				std::filesystem::remove(path);
			}
		}
	}

	void check_sqlite_multi_instance_cas()
	{
		const auto relation_engine = engine();
		const auto path =
			std::filesystem::temp_directory_path() / "cxxlens-ng-store-multi-instance-cas.sqlite";
		std::filesystem::remove(path);
		auto seed = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
		require(seed.has_value(), "SQLite CAS seed store unavailable");
		auto base = publish(*seed, relation_engine, false);
		seed = cxxlens::sdk::result<cxxlens::sdk::snapshot_store>{
			cxxlens::sdk::unexpected(cxxlens::sdk::error{"test.release", "seed", {}})};

		auto first_store = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
		auto second_store =
			cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
		require(first_store && second_store, "independent SQLite CAS stores unavailable");
		const auto prepare_writer =
			[&](cxxlens::sdk::result<cxxlens::sdk::snapshot_writer>& writer, std::string scope)
		{
			auto staged = partition(relation_engine, false);
			staged.scope = std::move(scope);
			staged.coverage.front().key = staged.scope;
			require(writer && writer->stage(std::move(staged)) && writer->validate(),
					"independent SQLite CAS writer setup failed");
		};
		auto first_writer =
			first_store->begin(snapshot_draft(relation_engine, base.publication().publication_id));
		auto second_writer =
			second_store->begin(snapshot_draft(relation_engine, base.publication().publication_id));
		prepare_writer(first_writer, "compile-unit-cas-a");
		prepare_writer(second_writer, "compile-unit-cas-b");
		auto winner = first_writer->publish();
		auto loser = second_writer->publish();
		require(winner && !loser && loser.error().code == "store.publication-conflict",
				"two SQLite instances committed forked heads or returned an unstable conflict");

		auto reopened = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
		require(reopened.has_value(), "SQLite CAS result reopened as an ambiguous head");
		auto head = reopened->current(selector(relation_engine));
		require(head && head->publication().publication_id == winner->publication().publication_id,
				"SQLite CAS winner was not the durable series head");
		auto retry =
			reopened->begin(snapshot_draft(relation_engine, head->publication().publication_id));
		prepare_writer(retry, "compile-unit-cas-retry");
		auto retried = retry->publish();
		require(retried && retried->publication().sequence == head->publication().sequence + 1U,
				"CAS loser could not retry from the durable head");
		auto final = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
		auto final_head = final
			? final->current(selector(relation_engine))
			: cxxlens::sdk::result<cxxlens::sdk::snapshot_handle>{final.error()};
		require(final && final_head &&
					final_head->publication().publication_id ==
						retried->publication().publication_id,
				"SQLite CAS retry did not remain unambiguous after reopen");
		std::filesystem::remove(path);
	}

	void check_compaction_resolver_order()
	{
		const auto relation_engine = engine();
		for (const bool sqlite : {false, true})
		{
			const auto path = std::filesystem::temp_directory_path() /
				("cxxlens-ng-compaction-resolver-" + std::string{sqlite ? "sqlite" : "memory"} +
				 ".sqlite");
			std::filesystem::remove(path);
			auto store = sqlite
				? cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine)
				: cxxlens::sdk::make_in_memory_snapshot_store(relation_engine);
			require(store.has_value(), "compaction resolver store unavailable");
			auto stable = try_publish(*store, relation_engine, std::nullopt, "stable");
			auto alternate = try_publish(*store, relation_engine, std::nullopt, "alternate");
			require(stable && alternate && stable->id() == alternate->id() &&
						stable->publication().sequence == alternate->publication().sequence &&
						stable->publication().physical_generation <
							alternate->publication().physical_generation,
					"two-series resolver setup was not globally ordered");
			const auto stable_id = std::string{stable->publication().publication_id};
			const auto alternate_id = std::string{alternate->publication().publication_id};
			auto selected_before = store->open(stable->id());
			require(selected_before &&
						selected_before->publication().publication_id == alternate_id,
					"same-snapshot resolver did not select the prior physical order");
			const auto maximum_before = alternate->publication().physical_generation;
			require(store->compact().has_value(), "two-series compaction failed");
			auto compacted_stable = store->open_publication(stable_id);
			auto compacted_alternate = store->open_publication(alternate_id);
			auto selected_after = store->open(stable->id());
			require(compacted_stable && compacted_alternate && selected_after &&
						compacted_stable->publication().physical_generation > maximum_before &&
						compacted_stable->publication().physical_generation <
							compacted_alternate->publication().physical_generation &&
						selected_after->publication().publication_id == alternate_id,
					"compaction collapsed generations or changed resolver order");
			if (sqlite)
			{
				auto reopened =
					cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
				auto selected = reopened
					? reopened->open(stable->id())
					: cxxlens::sdk::result<cxxlens::sdk::snapshot_handle>{reopened.error()};
				require(reopened && selected &&
							selected->publication().publication_id == alternate_id,
						"SQLite reopen lost compacted resolver order");
			}
			std::filesystem::remove(path);
		}
	}

	void check_sqlite_publish_refreshes_committed_census()
	{
		const auto relation_engine = engine();
		const auto path = std::filesystem::temp_directory_path() /
			"cxxlens-ng-publish-refreshes-committed-census.sqlite";
		std::filesystem::remove(path);
		auto first = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
		auto second = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
		require(first && second, "publish census refresh stores unavailable");

		auto external = try_publish(*second, relation_engine, std::nullopt, "external");
		auto local = try_publish(*first, relation_engine, std::nullopt, "local");
		require(external && local && external->id() == local->id() &&
					external->publication().physical_generation == 1U &&
					local->publication().physical_generation == 2U,
				"publish census refresh setup failed");
		auto external_selector = selector(relation_engine);
		external_selector.channel_id = "external";
		auto visible_external = first->open_publication(external->publication().publication_id);
		auto current_external = first->current(external_selector);
		auto selected_local = first->open(local->id());
		require(visible_external && current_external && selected_local &&
					current_external->publication().publication_id ==
						external->publication().publication_id &&
					selected_local->publication().publication_id ==
						local->publication().publication_id,
				"publish discarded an external committed row from process-local read authority");

		auto latest = try_publish(*second, relation_engine, std::nullopt, "latest");
		require(latest && latest->publication().physical_generation == 3U,
				"second stale writer did not use the full database generation census");
		auto local_selector = selector(relation_engine);
		local_selector.channel_id = "local";
		auto visible_local = second->open_publication(local->publication().publication_id);
		auto current_local = second->current(local_selector);
		auto selected_latest = second->open(latest->id());
		require(visible_local && current_local && selected_latest &&
					current_local->publication().publication_id ==
						local->publication().publication_id &&
					selected_latest->publication().publication_id ==
						latest->publication().publication_id,
				"post-commit census refresh was not symmetric across long-lived stores");
		std::filesystem::remove(path);
	}

	void check_sqlite_transactional_generation_authority()
	{
		const auto relation_engine = engine();
		const auto path = std::filesystem::temp_directory_path() /
			"cxxlens-ng-transactional-generation-authority.sqlite";
		std::filesystem::remove(path);
		auto first = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
		auto second = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
		require(first && second, "long-lived SQLite stores unavailable");
		auto stable = try_publish(*first, relation_engine, std::nullopt, "stable");
		auto alternate = try_publish(*second, relation_engine, std::nullopt, "alternate");
		require(stable && alternate && stable->publication().physical_generation == 1U &&
					alternate->publication().physical_generation == 2U,
				"different-series writers reused a process-local generation");
		const auto stable_id = std::string{stable->publication().publication_id};
		const auto alternate_id = std::string{alternate->publication().publication_id};
		const auto snapshot_id = std::string{stable->id()};

		require(first->compact().has_value(), "first long-lived compaction failed");
		require(second->compact().has_value(), "stale compact-vs-compact census failed");
		auto compacted_stable = second->open_publication(stable_id);
		auto compacted_alternate = second->open_publication(alternate_id);
		require(compacted_stable && compacted_alternate &&
					compacted_stable->publication().physical_generation == 5U &&
					compacted_alternate->publication().physical_generation == 6U,
				"stale compaction omitted external publications or reused generations");

		auto external = try_publish(*first, relation_engine, std::nullopt, "external");
		require(external && external->publication().physical_generation == 7U,
				"publish-after-external-compaction did not use database authority");
		const auto external_id = std::string{external->publication().publication_id};
		require(second->compact().has_value(), "compact-after-external-publish failed");
		auto final_stable = second->open_publication(stable_id);
		auto final_alternate = second->open_publication(alternate_id);
		auto final_external = second->open_publication(external_id);
		auto selected = second->open(snapshot_id);
		require(final_stable && final_alternate && final_external && selected &&
					final_stable->publication().physical_generation == 8U &&
					final_alternate->publication().physical_generation == 9U &&
					final_external->publication().physical_generation == 10U &&
					selected->publication().publication_id == external_id,
				"publish/compact interleaving did not preserve the full ordered authority set");
		std::filesystem::remove(path);
	}

	void check_compaction_failure_isolation()
	{
		const auto relation_engine = engine();
		constexpr auto almost_maximum = std::numeric_limits<std::uint64_t>::max() - 1U;
		for (const bool sqlite : {false, true})
		{
			const auto path = std::filesystem::temp_directory_path() /
				("cxxlens-ng-compaction-rollback-" + std::string{sqlite ? "sqlite" : "memory"} +
				 ".sqlite");
			std::filesystem::remove(path);
			auto store = sqlite
				? cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine)
				: cxxlens::sdk::make_in_memory_snapshot_store(relation_engine);
			require(store.has_value(), "compaction rollback store unavailable");
			auto first = try_publish(*store, relation_engine, std::nullopt, "first");
			auto second = try_publish(*store, relation_engine, std::nullopt, "second");
			require(first && second, "compaction rollback publications failed");
			const auto first_id = std::string{first->publication().publication_id};
			auto rewritten = cxxlens::sdk::rewrite_publication_counters_for_testing(
				*store, second->publication().publication_id, 1U, almost_maximum);
			require(rewritten.has_value(), "compaction rollback boundary setup failed");
			const auto second_id = std::move(*rewritten);
			auto compacted = store->compact();
			require(!compacted && compacted.error().code == "store.counter-overflow",
					"partial compaction allocation did not fail closed");
			if (sqlite)
				store = cxxlens::sdk::result<cxxlens::sdk::snapshot_store>{
					cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine)
						.value()};
			auto preserved_first = store->open_publication(first_id);
			auto preserved_second = store->open_publication(second_id);
			require(preserved_first && preserved_second &&
						preserved_first->publication().physical_generation == 1U &&
						preserved_second->publication().physical_generation == almost_maximum,
					"failed compaction partially replaced prior generations");
			std::filesystem::remove(path);
		}
	}

	void check_sqlite_authority_corruption()
	{
		const auto relation_engine = engine();
		const std::array mutations{"missing", "publication", "sequence"};
		for (const auto* mutation : mutations)
		{
			const auto path = std::filesystem::temp_directory_path() /
				("cxxlens-ng-head-corruption-" + std::string{mutation} + ".sqlite");
			std::filesystem::remove(path);
			{
				auto store =
					cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
				require(store.has_value(), "head corruption store unavailable");
				auto published = publish(*store, relation_engine, false);
				require(cxxlens::sdk::rewrite_publication_payload_for_testing(
							*store,
							published.publication().publication_id,
							"test.series-head",
							mutation,
							0U)
							.has_value(),
						"head corruption setup failed");
			}
			auto reopened =
				cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
			require(!reopened && reopened.error().code == "store.corrupt",
					"missing, different, or sequence-drifted durable head was accepted");
			std::filesystem::remove(path);
		}

		const auto duplicate_path =
			std::filesystem::temp_directory_path() / "cxxlens-ng-duplicate-valid-sequence.sqlite";
		std::filesystem::remove(duplicate_path);
		auto store =
			cxxlens::sdk::open_sqlite_snapshot_store(duplicate_path.string(), relation_engine);
		require(store.has_value(), "duplicate sequence store unavailable");
		auto published = publish(*store, relation_engine, false);
		require(
			cxxlens::sdk::rewrite_publication_payload_for_testing(
				*store, published.publication().publication_id, "test.duplicate-sequence", "", 0U)
				.has_value(),
			"duplicate valid authority setup failed");
		auto compacted = store->compact();
		require(!compacted && compacted.error().code == "store.compact-validation-failed",
				"duplicate valid sequence was admitted to compaction authority");
		auto duplicate_reopen =
			cxxlens::sdk::open_sqlite_snapshot_store(duplicate_path.string(), relation_engine);
		require(!duplicate_reopen && duplicate_reopen.error().code == "store.current-ambiguous",
				"duplicate valid sequence was admitted by reopen head derivation");
		std::filesystem::remove(duplicate_path);

		const auto orphan_path =
			std::filesystem::temp_directory_path() / "cxxlens-ng-orphan-parent.sqlite";
		std::filesystem::remove(orphan_path);
		{
			auto orphan_store =
				cxxlens::sdk::open_sqlite_snapshot_store(orphan_path.string(), relation_engine);
			require(orphan_store.has_value(), "orphan parent store unavailable");
			auto orphan = publish(*orphan_store, relation_engine, false);
			require(cxxlens::sdk::rewrite_publication_payload_for_testing(
						*orphan_store,
						orphan.publication().publication_id,
						"test.orphan-parent",
						"",
						0U)
						.has_value(),
					"orphan parent setup failed");
		}
		auto orphan_reopen =
			cxxlens::sdk::open_sqlite_snapshot_store(orphan_path.string(), relation_engine);
		require(!orphan_reopen && orphan_reopen.error().code == "store.corrupt",
				"identity-valid orphan publication topology was admitted by reopen");
		std::filesystem::remove(orphan_path);
	}

	void check_unsigned_counter_codec_transition()
	{
		const auto relation_engine = engine();
		constexpr auto signed_maximum =
			static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
		const std::array values{
			signed_maximum, signed_maximum + 1U, std::numeric_limits<std::uint64_t>::max()};
		for (const auto value : values)
		{
			const auto path = std::filesystem::temp_directory_path() /
				("cxxlens-ng-u64-codec-" + std::to_string(value) + ".sqlite");
			std::filesystem::remove(path);
			std::string publication_id;
			{
				auto store =
					cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
				require(store.has_value(), "u64 codec store unavailable");
				auto published = publish(*store, relation_engine, false);
				auto rewritten = cxxlens::sdk::rewrite_publication_counters_for_testing(
					*store, published.publication().publication_id, value, value);
				require(rewritten.has_value(), "u64 codec rewrite failed");
				publication_id = std::move(*rewritten);
			}
			auto reopened =
				cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
			auto opened = reopened
				? reopened->open_publication(publication_id)
				: cxxlens::sdk::result<cxxlens::sdk::snapshot_handle>{reopened.error()};
			require(reopened && opened && opened->publication().sequence == value &&
						opened->publication().physical_generation == value &&
						opened->publication().publication_id ==
							expected_publication_identity(opened->publication()),
					"u64 SQLite or canonical identity codec did not round trip a sign boundary");
			std::filesystem::remove(path);
		}
	}

	void check_derived_basis_membership()
	{
		const auto relation_engine = engine();
		const auto fake =
			std::string{"partition-content:sha256:"
						"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
		{
			auto store = cxxlens::sdk::make_in_memory_snapshot_store(relation_engine);
			require(store.has_value(), "derived exact store unavailable");
			auto prior = publish(*store, relation_engine, false);
			const auto exact = prior.manifest().partitions.front().content_digest;
			require(validates_derived_partition(*store,
												relation_engine,
												prior.publication().publication_id,
												std::string{prior.id()},
												{exact}),
					"exact prior partition membership was rejected");
			require(!validates_derived_partition(*store,
												 relation_engine,
												 prior.publication().publication_id,
												 std::string{prior.id()},
												 {fake}),
					"nonexistent consumed partition was accepted");
			require(!validates_derived_partition(*store,
												 relation_engine,
												 prior.publication().publication_id,
												 std::string{prior.id()},
												 {exact, fake}),
					"partially invalid consumed partition set was accepted");
			require(!validates_derived_partition(*store,
												 relation_engine,
												 prior.publication().publication_id,
												 "snapshot:uncommitted",
												 {exact}),
					"uncommitted input snapshot was accepted");
		}
		{
			auto store = cxxlens::sdk::make_in_memory_snapshot_store(relation_engine);
			require(store.has_value(), "derived cross-snapshot store unavailable");
			auto first = publish(*store, relation_engine, false);
			auto alternate = partition(relation_engine, false);
			alternate.scope = "compile-unit-2";
			alternate.coverage.front().key = "compile-unit-2";
			auto writer =
				store->begin(snapshot_draft(relation_engine, first.publication().publication_id));
			require(writer && writer->stage(std::move(alternate)) && writer->validate(),
					"alternate snapshot setup failed");
			auto second = writer->publish();
			require(second.has_value() && second->id() != first.id(),
					"alternate snapshot identity did not change");
			const auto foreign = second->manifest().partitions.front().content_digest;
			require(!validates_derived_partition(*store,
												 relation_engine,
												 second->publication().publication_id,
												 std::string{first.id()},
												 {foreign}),
					"partition from another snapshot was accepted");
		}
		{
			auto store = cxxlens::sdk::make_in_memory_snapshot_store(relation_engine);
			require(store.has_value(), "derived duplicate publication store unavailable");
			auto first = publish(*store, relation_engine, false);
			auto second =
				publish(*store, relation_engine, true, first.publication().publication_id);
			require(first.id() == second.id(), "duplicate semantic snapshot ID changed");
			const auto exact = second.manifest().partitions.front().content_digest;
			require(validates_derived_partition(*store,
												relation_engine,
												second.publication().publication_id,
												std::string{second.id()},
												{exact}),
					"duplicate physical publication changed membership");
		}
		{
			auto store = cxxlens::sdk::make_in_memory_snapshot_store(relation_engine);
			require(store.has_value(), "derived corrupt publication store unavailable");
			auto prior = publish(*store, relation_engine, false);
			const auto exact = prior.manifest().partitions.front().content_digest;
			require(cxxlens::sdk::mark_publication_corrupt_for_testing(
						*store, prior.publication().publication_id)
						.has_value(),
					"derived corrupt prior setup failed");
			require(!validates_derived_partition(*store,
												 relation_engine,
												 prior.publication().publication_id,
												 std::string{prior.id()},
												 {exact}),
					"corrupt current input snapshot was accepted");
		}
	}

	void check_derived_basis_uses_checked_snapshot_resolver()
	{
		const auto relation_engine = engine();
		for (const bool sqlite : {false, true})
		{
			const auto path = std::filesystem::temp_directory_path() /
				("cxxlens-ng-derived-resolver-" + std::string{sqlite ? "sqlite" : "memory"} +
				 ".sqlite");
			std::filesystem::remove(path);
			auto store = sqlite
				? cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine)
				: cxxlens::sdk::make_in_memory_snapshot_store(relation_engine);
			require(store.has_value(), "derived resolver store unavailable");
			auto stable = try_publish(*store, relation_engine, std::nullopt, "stable");
			auto alternate = try_publish(*store, relation_engine, std::nullopt, "alternate");
			require(stable && alternate && stable->id() == alternate->id() &&
						stable->publication().physical_generation <
							alternate->publication().physical_generation,
					"derived resolver corrupt-latest setup failed");
			const auto partition_content = stable->manifest().partitions.front().content_digest;
			require(cxxlens::sdk::mark_publication_corrupt_for_testing(
						*store, alternate->publication().publication_id)
						.has_value(),
					"derived resolver latest-corrupt setup failed");
			auto opened = store->open(stable->id());
			auto derived = validate_derived_partition(*store,
													  relation_engine,
													  stable->publication().publication_id,
													  std::string{stable->id()},
													  {partition_content});
			require(!opened && opened.error().code == "store.snapshot-corrupt" && !derived &&
						derived.error().code == "store.snapshot-corrupt",
					"derived basis fell back behind the corrupt selected publication");
			std::filesystem::remove(path);
		}

		for (const bool sqlite : {false, true})
		{
			const auto path = std::filesystem::temp_directory_path() /
				("cxxlens-ng-derived-ambiguous-" + std::string{sqlite ? "sqlite" : "memory"} +
				 ".sqlite");
			std::filesystem::remove(path);
			auto store = sqlite
				? cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine)
				: cxxlens::sdk::make_in_memory_snapshot_store(relation_engine);
			require(store.has_value(), "derived ambiguity store unavailable");
			auto stable = try_publish(*store, relation_engine, std::nullopt, "stable");
			auto alternate = try_publish(*store, relation_engine, std::nullopt, "alternate");
			auto latest = try_publish(*store, relation_engine, std::nullopt, "latest");
			require(stable && alternate && latest && stable->id() == alternate->id() &&
						stable->id() == latest->id(),
					"derived ambiguity publications differ semantically");
			std::array publication_ids{std::string{stable->publication().publication_id},
									   std::string{alternate->publication().publication_id},
									   std::string{latest->publication().publication_id}};
			const auto high = std::ranges::min_element(publication_ids);
			for (const auto& publication_id : publication_ids)
			{
				if (publication_id == *high)
					continue;
				auto rewritten = cxxlens::sdk::rewrite_publication_counters_for_testing(
					*store, publication_id, 1U, 1U);
				require(rewritten.has_value(), "derived ambiguity low-key setup failed");
			}
			auto rewritten_high =
				cxxlens::sdk::rewrite_publication_counters_for_testing(*store, *high, 1U, 3U);
			require(rewritten_high.has_value(), "derived ambiguity high-key setup failed");
			const auto partition_content = stable->manifest().partitions.front().content_digest;
			auto opened = store->open(stable->id());
			auto derived = validate_derived_partition(*store,
													  relation_engine,
													  stable->publication().publication_id,
													  std::string{stable->id()},
													  {partition_content});
			require(!opened && opened.error().code == "store.snapshot-ambiguous" && !derived &&
						derived.error().code == "store.snapshot-ambiguous",
					"derived basis did not share the semantic snapshot ambiguity verdict");
			if (sqlite)
			{
				auto reopened =
					cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
				auto reopened_semantic = reopened
					? reopened->open(stable->id())
					: cxxlens::sdk::result<cxxlens::sdk::snapshot_handle>{reopened.error()};
				require(reopened && !reopened_semantic &&
							reopened_semantic.error().code == "store.snapshot-ambiguous",
						"SQLite reopen missed a lower-order ambiguous resolver pair");
			}
			std::filesystem::remove(path);
		}
	}

	void check_v5_manifest_order_is_canonical()
	{
		const auto relation_engine = engine();
		const auto path =
			std::filesystem::temp_directory_path() / "cxxlens-ng-manifest-order.sqlite";
		std::filesystem::remove(path);
		std::string publication_id;
		std::string snapshot_id;
		{
			auto store = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
			require(store.has_value(), "manifest order store unavailable");
			auto writer = store->begin(snapshot_draft(relation_engine));
			auto first = partition(relation_engine, false);
			auto second = partition(relation_engine, true);
			second.scope = "compile-unit-2";
			second.coverage.front().key = "compile-unit-2";
			require(writer && writer->stage(std::move(first)) && writer->stage(std::move(second)) &&
						writer->validate(),
					"two-partition manifest setup failed");
			auto published = writer->publish();
			require(published && published->manifest().partitions.size() == 2U,
					"two-partition manifest did not publish");
			publication_id = published->publication().publication_id;
			snapshot_id = published->id();
			require(cxxlens::sdk::rewrite_publication_payload_for_testing(
						*store, publication_id, "test.reverse-manifest-partitions", "", 0U)
						.has_value(),
					"manifest order tamper failed");
		}
		auto reopened = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
		auto current = reopened
			? reopened->current(selector(relation_engine))
			: cxxlens::sdk::result<cxxlens::sdk::snapshot_handle>{reopened.error()};
		auto semantic = reopened
			? reopened->open(snapshot_id)
			: cxxlens::sdk::result<cxxlens::sdk::snapshot_handle>{reopened.error()};
		require(reopened && !current && current.error().code == "store.current-corrupt" &&
					!semantic && semantic.error().code == "store.snapshot-corrupt",
				"noncanonical v5 manifest order was exposed as an intact snapshot");
		std::filesystem::remove(path);
	}
} // namespace

int main()
{
	check_canonical_vectors();
	check_backend_parity();
	check_occurrence_round_trip();
	check_sqlite_multi_instance_cas();
	check_compaction_resolver_order();
	check_sqlite_publish_refreshes_committed_census();
	check_sqlite_transactional_generation_authority();
	check_compaction_failure_isolation();
	check_sqlite_authority_corruption();
	check_unsigned_counter_codec_transition();
	check_sqlite_semantic_graph_tamper();
	check_publication_identity_binding();
	check_snapshot_version_wire_bounds();
	check_publication_counter_bounds();
	check_derived_basis_membership();
	check_derived_basis_uses_checked_snapshot_resolver();
	check_v5_manifest_order_is_canonical();
	return 0;
}
