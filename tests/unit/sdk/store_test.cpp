#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <cxxlens/sdk.hpp>

#include "../../support/sqlite_store_fixture.hpp"
#include "sdk/sqlite_store_fault_injection_internal.hpp"

static_assert(std::is_same_v<decltype(&cxxlens::sdk::snapshot_store::begin),
							 cxxlens::sdk::result<cxxlens::sdk::snapshot_writer> (
								 cxxlens::sdk::snapshot_store::*)(cxxlens::sdk::snapshot_draft)>);
static_assert(std::is_same_v<decltype(&cxxlens::sdk::snapshot_store::compact),
							 cxxlens::sdk::result<void> (cxxlens::sdk::snapshot_store::*)()>);

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
	void set_sqlite_source_shm_symbols_available_for_testing(bool) noexcept;
} // namespace cxxlens::sdk

namespace
{
	std::string test_executable_path;

	void require(const bool condition, const std::string& message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	void remove_stale_sqlite_files(const std::filesystem::path& path)
	{
		cxxlens::test::sqlite_fixture::remove_sqlite_database_files(path);
	}

#if defined(__unix__) || defined(__APPLE__)
	struct sqlite_source_file_family_state
	{
		cxxlens::test::sqlite_fixture::database_files files;
		std::map<std::string,
				 std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>,
				 std::less<>>
			identities_and_sizes;

		[[nodiscard]] bool operator==(const sqlite_source_file_family_state&) const = default;
	};

	[[nodiscard]] sqlite_source_file_family_state
	capture_sqlite_source_file_family_state(const std::filesystem::path& input)
	{
		const auto absolute = std::filesystem::absolute(input);
		sqlite_source_file_family_state output;
		output.files = cxxlens::test::sqlite_fixture::capture_files(absolute);
		for (const auto& suffix :
			 {std::string{}, std::string{"-wal"}, std::string{"-shm"}, std::string{"-journal"}})
		{
			const auto path = std::filesystem::path{absolute.string() + suffix};
			struct stat status
			{
			};
			if (::stat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode))
				continue;
			output.identities_and_sizes.emplace(
				path.filename().string(),
				std::tuple{static_cast<std::uint64_t>(status.st_dev),
						   static_cast<std::uint64_t>(status.st_ino),
						   static_cast<std::uint64_t>(status.st_size)});
		}
		return output;
	}
#endif

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

	[[nodiscard]] cxxlens::sdk::partition_draft partition_with_payload(
		const cxxlens::sdk::relation_engine& value, std::string key, std::string payload)
	{
		auto draft = partition(value, false);
		draft.claims = {make_claim(value, std::move(key), std::move(payload))};
		auto basis = cxxlens::sdk::claim_input_basis_digest(draft.claims.front().input_basis);
		require(basis.has_value(), "payload partition basis digest rejected");
		draft.producer_input_basis_digest = std::move(*basis);
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

	[[nodiscard]] cxxlens::sdk::snapshot_handle
	publish_partition(cxxlens::sdk::snapshot_store& store,
					  const cxxlens::sdk::relation_engine& value,
					  cxxlens::sdk::partition_draft value_partition,
					  std::optional<std::string> parent = std::nullopt)
	{
		auto writer = store.begin(snapshot_draft(value, std::move(parent)));
		require(writer.has_value(), "payload Store writer did not begin");
		require(writer->stage(std::move(value_partition)).has_value(),
				"payload Store partition did not stage");
		require(writer->validate().has_value(), "payload Store candidate did not validate");
		auto published = writer->publish();
		require(published.has_value(), "payload Store candidate did not publish");
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
		remove_stale_sqlite_files(path);
		auto opened_sqlite =
			cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
		require(opened_sqlite.has_value(), "SQLite store unavailable");
		std::optional<cxxlens::sdk::snapshot_store> sqlite{std::move(*opened_sqlite)};

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
		{
			auto stale = sqlite->begin(snapshot_draft(relation_engine, prior_publication));
			require(stale && stale->stage(partition(relation_engine, false)) && stale->validate(),
					"stale candidate setup failed");
			auto stale_publish = stale->publish();
			require(!stale_publish && stale_publish.error().code == "store.publication-conflict",
					"stale parent publish was accepted");
		}
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
		sqlite.reset();
		auto reopened_store =
			cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
		require(reopened_store.has_value(), "SQLite reopen unavailable");
		sqlite.emplace(std::move(*reopened_store));
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
		sqlite.reset();
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
		remove_stale_sqlite_files(path);
		auto opened_sqlite =
			cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
		require(opened_sqlite.has_value(), "occurrence SQLite store unavailable");
		std::optional<cxxlens::sdk::snapshot_store> sqlite{
			std::move(*opened_sqlite)};
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
		sqlite.reset();
		auto reopened_sqlite =
			cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
		require(reopened_sqlite.has_value(), "occurrence SQLite store did not reopen");
		sqlite.emplace(std::move(*reopened_sqlite));
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
			remove_stale_sqlite_files(path);
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
			remove_stale_sqlite_files(path);
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
		remove_stale_sqlite_files(path);
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
				remove_stale_sqlite_files(path);
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
		remove_stale_sqlite_files(poison_path);
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
				remove_stale_sqlite_files(path);
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
		remove_stale_sqlite_files(path);
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
			remove_stale_sqlite_files(path);
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
		remove_stale_sqlite_files(path);
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
		remove_stale_sqlite_files(path);
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
			remove_stale_sqlite_files(path);
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
			remove_stale_sqlite_files(path);
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
		remove_stale_sqlite_files(duplicate_path);
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
		remove_stale_sqlite_files(orphan_path);
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
			remove_stale_sqlite_files(path);
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
			remove_stale_sqlite_files(path);
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
			remove_stale_sqlite_files(path);
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
				require(!reopened && reopened.error().code == "store.corrupt" &&
							reopened.error().field == "sqlite-chunk-authority" &&
							reopened.error().detail ==
								"global-orphan-retired-or-duplicate-committed-generation",
						"SQLite reopen did not reject globally duplicated committed generations");
			}
			std::filesystem::remove(path);
		}
	}

	void check_v5_manifest_order_is_canonical()
	{
		const auto relation_engine = engine();
		const auto path =
			std::filesystem::temp_directory_path() / "cxxlens-ng-manifest-order.sqlite";
		remove_stale_sqlite_files(path);
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

	void check_sqlite_v3_fresh_schema_and_chunk_boundary()
	{
		using namespace cxxlens::test::sqlite_fixture;
		const auto relation_engine = engine();
		temporary_directory directory{"sqlite-v3-fresh"};
		const auto empty_path = directory.path() / "empty.sqlite";
		{
			auto store =
				cxxlens::sdk::open_sqlite_snapshot_store(empty_path.string(), relation_engine);
			require(store.has_value(), "fresh SQLite v3 Store unavailable");
			const auto compatibility = store->compatibility();
			require(compatibility.backend == "sqlite" &&
						compatibility.readable_format.major == 3U &&
						compatibility.readable_format.minor == 0U &&
						compatibility.readable_format.patch == 0U && compatibility.direct_open &&
						!compatibility.migration_required,
					"fresh SQLite Store did not report exact v3 compatibility");
			require(store->retained_generation_count() == 0U,
					"fresh SQLite Store unexpectedly retained a generation");
		}

		const std::vector<std::pair<std::string, std::string>> expected_metadata{
			{"payload_chunk_maximum_bytes", "8388608"},
			{"payload_chunk_profile", "cxxlens.sqlite-payload-chunks.v1"},
			{"physical_format", "cxxlens.sqlite-semantic-store.v3"},
			{"physical_format_version", "3.0.0"},
		};
		require(read_metadata(empty_path) == expected_metadata,
				"fresh SQLite v3 metadata projection drifted");

		const std::map<std::string, std::pair<std::string, std::string>, std::less<>>
			expected_schema{
				{"cxxlens_ng_metadata",
				 {"table",
				  "CREATE TABLE cxxlens_ng_metadata(key TEXT NOT NULL PRIMARY KEY,value TEXT NOT "
				  "NULL) STRICT, WITHOUT ROWID"}},
				{"cxxlens_ng_payload_chunk",
				 {"table",
				  "CREATE TABLE cxxlens_ng_payload_chunk(publication_id TEXT NOT NULL,generation "
				  "INTEGER NOT NULL,chunk_ordinal INTEGER NOT NULL,byte_offset INTEGER NOT "
				  "NULL,byte_count INTEGER NOT NULL,checksum TEXT NOT NULL,payload BLOB NOT "
				  "NULL,PRIMARY KEY(publication_id,generation,chunk_ordinal),CHECK(chunk_ordinal "
				  "BETWEEN 0 AND 2199023255551),CHECK(byte_count BETWEEN 1 AND "
				  "8388608),CHECK(length(payload)=byte_count)) STRICT, WITHOUT ROWID"}},
				{"cxxlens_ng_payload_chunk_locator",
				 {"index",
				  "CREATE INDEX cxxlens_ng_payload_chunk_locator ON "
				  "cxxlens_ng_payload_chunk(publication_id,generation,chunk_ordinal)"}},
				{"cxxlens_ng_publication",
				 {"table",
				  "CREATE TABLE cxxlens_ng_publication(publication_id TEXT NOT NULL PRIMARY "
				  "KEY,series_id TEXT NOT NULL,snapshot_id TEXT NOT NULL,sequence INTEGER NOT "
				  "NULL,generation INTEGER NOT NULL,parent TEXT,state INTEGER NOT NULL CHECK(state "
				  "BETWEEN 0 AND 5),payload_checksum TEXT NOT NULL,payload_byte_count INTEGER NOT "
				  "NULL,payload_chunk_count INTEGER NOT NULL CHECK(payload_chunk_count BETWEEN 0 "
				  "AND 2199023255552)) STRICT, WITHOUT ROWID"}},
				{"cxxlens_ng_publication_series",
				 {"index",
				  "CREATE INDEX cxxlens_ng_publication_series ON "
				  "cxxlens_ng_publication(series_id,sequence)"}},
				{"cxxlens_ng_series_head",
				 {"table",
				  "CREATE TABLE cxxlens_ng_series_head(series_id TEXT NOT NULL PRIMARY "
				  "KEY,current_publication TEXT NOT NULL,sequence INTEGER NOT NULL) STRICT, "
				  "WITHOUT ROWID"}},
			};
		const auto schema = read_schema_objects(empty_path);
		require(schema.size() == expected_schema.size(),
				"fresh SQLite v3 user-object census drifted");
		for (const auto& object : schema)
		{
			const auto expected = expected_schema.find(object.name);
			require(expected != expected_schema.end() && object.type == expected->second.first &&
						object.sql == expected->second.second,
					"fresh SQLite v3 canonical DDL drifted: " + object.name);
		}
		{
			connection database{empty_path, true};
			const auto counts =
				database.query("SELECT (SELECT count(*) FROM cxxlens_ng_publication),"
							   "(SELECT count(*) FROM cxxlens_ng_payload_chunk),"
							   "(SELECT count(*) FROM cxxlens_ng_series_head)");
			require(counts.size() == 1U && counts.front().size() == 3U &&
						integer(counts.front()[0U]) == 0 && integer(counts.front()[1U]) == 0 &&
						integer(counts.front()[2U]) == 0,
					"fresh SQLite v3 authority census was not empty");
		}

		const auto populated_path = directory.path() / "populated.sqlite";
		const auto payload_size = static_cast<std::size_t>(chunk_maximum_bytes) + 65'537U;
		const auto large =
			partition_with_payload(relation_engine, "item:large", std::string(payload_size, 'x'));
		std::string expected_snapshot_id;
		std::string expected_publication_id;
		std::string expected_export;
		{
			auto memory = cxxlens::sdk::make_in_memory_snapshot_store(relation_engine);
			auto sqlite =
				cxxlens::sdk::open_sqlite_snapshot_store(populated_path.string(), relation_engine);
			require(memory && sqlite, "large payload parity Stores unavailable");
			auto memory_snapshot = publish_partition(*memory, relation_engine, large);
			auto sqlite_snapshot = publish_partition(*sqlite, relation_engine, large);
			require(memory_snapshot.id() == sqlite_snapshot.id(),
					"large payload memory/SQLite snapshot IDs diverged");
			auto memory_export = memory->canonical_export(memory_snapshot.id());
			auto sqlite_export = sqlite->canonical_export(sqlite_snapshot.id());
			require(memory_export && sqlite_export && *memory_export == *sqlite_export,
					"large payload memory/SQLite canonical exports diverged");
			auto dynamic = relation_engine.require("company.test.item", 1U);
			auto cursor = dynamic ? sqlite_snapshot.open(*dynamic)
								  : cxxlens::sdk::result<cxxlens::sdk::row_cursor>{dynamic.error()};
			auto first = cursor
				? cursor->next()
				: cxxlens::sdk::result<std::optional<cxxlens::sdk::row_view>>{cursor.error()};
			require(dynamic && cursor && first && first->has_value() && (*first)->copy(),
					"large SQLite payload was not query-visible");
			expected_snapshot_id = sqlite_snapshot.id();
			expected_publication_id = sqlite_snapshot.publication().publication_id;
			expected_export = *sqlite_export;
		}

		const auto publications = read_v3_publications(populated_path);
		const auto chunks = read_chunks(populated_path);
		require(publications.size() == 1U &&
					publications.front().publication_id == expected_publication_id &&
					publications.front().payload_byte_count > chunk_maximum_bytes &&
					publications.front().payload_chunk_count >= 2 &&
					static_cast<std::int64_t>(chunks.size()) ==
						publications.front().payload_chunk_count,
				"large SQLite publication did not use bounded v3 chunks");
		for (std::size_t index{}; index < chunks.size(); ++index)
		{
			const auto& value = chunks[index];
			const auto final = index + 1U == chunks.size();
			require(value.publication_id == expected_publication_id &&
						value.ordinal == static_cast<std::int64_t>(index) &&
						value.offset == value.ordinal * chunk_maximum_bytes &&
						value.byte_count == static_cast<std::int64_t>(value.payload.size()) &&
						(final ? value.byte_count > 0 && value.byte_count <= chunk_maximum_bytes
							   : value.byte_count == chunk_maximum_bytes) &&
						value.checksum == cxxlens::sdk::content_digest(value.payload),
					"large SQLite chunk projection was not canonical");
		}
		quiesce_wal_sidecars(populated_path);

		auto reopened =
			cxxlens::sdk::open_sqlite_snapshot_store(populated_path.string(), relation_engine);
		auto current = reopened
			? reopened->current(selector(relation_engine))
			: cxxlens::sdk::result<cxxlens::sdk::snapshot_handle>{reopened.error()};
		auto cold_export = reopened ? reopened->canonical_export(expected_snapshot_id)
									: cxxlens::sdk::result<std::string>{reopened.error()};
		require(reopened && current && current->id() == expected_snapshot_id && cold_export &&
					*cold_export == expected_export,
				"cold reopen changed the v3 chunked semantic projection");
	}

	struct v2_fixture_expectation
	{
		std::string current_publication_id;
		std::string prior_publication_id;
		std::string snapshot_id;
		std::string canonical_export;
		std::optional<std::string> diagnostic_publication_id;
	};

	[[nodiscard]] v2_fixture_expectation
	make_exact_v2_fixture(const std::filesystem::path& path,
						  const cxxlens::sdk::relation_engine& relation_engine,
						  const bool include_corrupt_diagnostic)
	{
		v2_fixture_expectation output;
		{
			auto store = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
			require(store.has_value(), "v2 fixture source v3 Store unavailable");
			auto first = publish(*store, relation_engine, false);
			auto second =
				publish(*store, relation_engine, true, first.publication().publication_id);
			output.prior_publication_id = first.publication().publication_id;
			output.current_publication_id = second.publication().publication_id;
			output.snapshot_id = second.id();
			auto exported = store->canonical_export(second.id());
			require(exported.has_value(), "v2 fixture source export unavailable");
			output.canonical_export = std::move(*exported);
			if (include_corrupt_diagnostic)
			{
				auto diagnostic = try_publish(*store, relation_engine, std::nullopt, "diagnostic");
				require(diagnostic.has_value(), "v2 diagnostic source publication failed");
				output.diagnostic_publication_id = diagnostic->publication().publication_id;
				require(cxxlens::sdk::poison_rejected_generation_for_testing(
							*store,
							*output.diagnostic_publication_id,
							diagnostic->publication().physical_generation)
							.has_value(),
						"v2 diagnostic source poisoning failed");
			}
		}
		cxxlens::test::sqlite_fixture::downgrade_v3_to_exact_v2(path);
		if (output.diagnostic_publication_id)
			cxxlens::test::sqlite_fixture::rewrite_v2_checksum(
				path,
				*output.diagnostic_publication_id,
				"sha256:0000000000000000000000000000000000000000000000000000000000000000");
		cxxlens::test::sqlite_fixture::require_wal_header_and_quiescent_sidecars(path);
		return output;
	}

	void check_sqlite_format_discriminator_and_index_profile_precedence()
	{
		using namespace cxxlens::test::sqlite_fixture;
		const auto relation_engine = engine();
		temporary_directory directory{"sqlite-format-discriminator"};
		std::string mutation_failures;
		const auto require_factory_rejection = [&](const std::filesystem::path& path,
												   std::string_view code,
												   std::string_view field,
												   std::string_view detail,
												   std::string_view label)
		{
			const auto before = capture_files(path);
			auto opened = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
			require(!opened && opened.error().code == code && opened.error().field == field &&
						opened.error().detail == detail,
					std::string{label} + " did not return the exact factory rejection tuple");
			const auto after = capture_files(path);
			if (after != before)
			{
				if (!mutation_failures.empty())
					mutation_failures += ", ";
				mutation_failures += label;
			}
		};

		const auto pristine_v3_path = directory.path() / "exact-empty-v3.sqlite";
		{
			auto store = cxxlens::sdk::open_sqlite_snapshot_store(pristine_v3_path.string(),
																  relation_engine);
			require(store.has_value(), "empty exact-v3 discriminator source Store unavailable");
		}
		require_wal_header_and_quiescent_sidecars(pristine_v3_path);
		const auto mixed_v3_path = directory.path() / "v3-marker-v2-layout.sqlite";
		const auto index_profile_path = directory.path() / "v3-index-profile.sqlite";
		const auto application_id_path = directory.path() / "unknown-application-id.sqlite";
		require(std::filesystem::copy_file(pristine_v3_path, mixed_v3_path) &&
					std::filesystem::copy_file(pristine_v3_path, index_profile_path) &&
					std::filesystem::copy_file(pristine_v3_path, application_id_path),
				"exact-v3 discriminator fixture copies failed");

		apply_quiescent_schema_mutation(application_id_path,
										"DROP INDEX cxxlens_ng_payload_chunk_locator;"
										"DROP INDEX cxxlens_ng_publication_series;"
										"DROP TABLE cxxlens_ng_payload_chunk;"
										"DROP TABLE cxxlens_ng_series_head;"
										"DROP TABLE cxxlens_ng_publication;"
										"DROP TABLE cxxlens_ng_metadata;"
										"PRAGMA application_id=1;");
		require_factory_rejection(application_id_path,
								  "store.format-incompatible",
								  "sqlite-physical-format",
								  "unknown-format-or-layout",
								  "empty schema plus nonzero application_id");

		add_v2_single_blob_layout_signal_to_empty_v3(mixed_v3_path);
		require_factory_rejection(mixed_v3_path,
								  "store.corrupt",
								  "sqlite-format-classification",
								  "mixed-v2-v3",
								  "exact-v3 marker plus v2 single-BLOB layout");

		make_v3_publication_series_index_descending(index_profile_path);
		require_factory_rejection(index_profile_path,
								  "store.corrupt",
								  "sqlite-current-v3",
								  "schema-or-authority-damage",
								  "current-v3 descending publication index profile");

		const auto mixed_v2_path = directory.path() / "v2-marker-v3-layout.sqlite";
		(void)make_exact_v2_fixture(mixed_v2_path, relation_engine, false);
		add_v3_chunk_layout_signal_to_v2(mixed_v2_path);
		require_factory_rejection(mixed_v2_path,
								  "store.corrupt",
								  "sqlite-format-classification",
								  "mixed-v2-v3",
								  "exact-v2 marker plus v3 chunk layout");
		require(mutation_failures.empty(),
				"format discriminator/profile rejection changed Store or sidecar authority: " +
					mutation_failures);
	}

	void check_sqlite_locator_prevalidation()
	{
		using namespace cxxlens::test::sqlite_fixture;
		const auto relation_engine = engine();
		temporary_directory directory{"sqlite-locator-prevalidation"};
		const auto observation_path = directory.path() / "observation.sqlite";
		const auto before = capture_files(observation_path);

		auto empty = cxxlens::sdk::open_sqlite_snapshot_store("", relation_engine);
		require(!empty && empty.error().code == "store.sqlite-path-empty" &&
					empty.error().field == "database_path" && empty.error().detail.empty(),
				"empty SQLite locator did not retain its distinct rejection tuple");

		std::string embedded_nul = (directory.path() / "embedded-nul.sqlite").string();
		embedded_nul.push_back('\0');
		embedded_nul.append("hidden");
		const std::array invalid{
			embedded_nul,
			std::string{"file:"} + (directory.path() / "uri.sqlite").string(),
			(directory.path() / "query.sqlite").string() + "?mode=ro",
			(directory.path() / "fragment.sqlite").string() + "#authority",
		};
		for (const auto& locator : invalid)
		{
			auto opened = cxxlens::sdk::open_sqlite_snapshot_store(locator, relation_engine);
			require(!opened && opened.error().code == "store.sqlite-failure" &&
						opened.error().field == "sqlite-locator" &&
						opened.error().detail == "invalid-filesystem-path",
					"invalid filesystem locator did not fail before SQLite runtime use");
			require(capture_files(observation_path) == before,
					"invalid filesystem locator changed the parent namespace");
		}

		{
			auto memory = cxxlens::sdk::open_sqlite_snapshot_store(":memory:", relation_engine);
			require(memory.has_value(), "exact :memory: locator failed");
			require(memory->compatibility().backend == "sqlite",
					"exact :memory: locator did not remain a distinct valid backend");
		}
		require(capture_files(observation_path) == before,
				"exact :memory: locator changed the filesystem namespace");
	}

	void check_sqlite_v2_compact_rejects_corrupt_committed_before_write()
	{
		using namespace cxxlens::test::sqlite_fixture;
		const auto relation_engine = engine();
		temporary_directory directory{"sqlite-v2-compact-corrupt"};
		const auto path = directory.path() / "legacy.sqlite";
		const auto expected = make_exact_v2_fixture(path, relation_engine, false);
		rewrite_v2_checksum(
			path,
			expected.current_publication_id,
			"sha256:0000000000000000000000000000000000000000000000000000000000000000");
		require_wal_header_and_quiescent_sidecars(path);

		auto store = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
		require(store.has_value(), "corrupt committed exact-v2 Store did not open for diagnosis");
		auto current = store->current(selector(relation_engine));
		require(!current && current.error().code == "store.current-corrupt" &&
					current.error().field == expected.current_publication_id &&
					current.error().detail.empty(),
				"corrupt committed exact-v2 head did not retain the exact current error");

		const auto before = capture_files(path);
		auto compacted = store->compact();
		require(!compacted && compacted.error().code == "store.compact-validation-failed" &&
					compacted.error().field == expected.current_publication_id &&
					compacted.error().detail.empty(),
				"exact-v2 compact did not reject the first corrupt committed publication");
		const auto after = capture_files(path);
		require(after == before,
				"corrupt exact-v2 compact crossed validation into allocation or a Store write");
	}

	void check_sqlite_v2_read_only_and_begin_precedence()
	{
		using namespace cxxlens::test::sqlite_fixture;
		const auto relation_engine = engine();
		temporary_directory directory{"sqlite-v2-read"};
		const auto path = directory.path() / "legacy.sqlite";
		const auto expected = make_exact_v2_fixture(path, relation_engine, false);
		const auto before = capture_files(path);
		{
			auto store = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
			require(store.has_value(), "exact v2 Store did not open read-only");
			const auto compatibility = store->compatibility();
			require(compatibility.backend == "sqlite" &&
						compatibility.readable_format.major == 2U &&
						compatibility.readable_format.minor == 6U &&
						compatibility.readable_format.patch == 0U && compatibility.direct_open &&
						compatibility.migration_required,
					"exact v2 Store compatibility tuple drifted");
			auto current = store->current(selector(relation_engine));
			auto semantic = store->open(expected.snapshot_id);
			auto publication = store->open_publication(expected.current_publication_id);
			auto prior = store->open_publication(expected.prior_publication_id);
			auto exported = store->canonical_export(expected.snapshot_id);
			require(current && semantic && publication && prior && exported &&
						current->publication().publication_id == expected.current_publication_id &&
						semantic->id() == expected.snapshot_id &&
						publication->id() == expected.snapshot_id &&
						prior->id() == expected.snapshot_id &&
						*exported == expected.canonical_export,
					"exact v2 read APIs lost the eager semantic projection");
			auto dynamic = relation_engine.require("company.test.item", 1U);
			auto cursor = dynamic ? current->open(*dynamic)
								  : cxxlens::sdk::result<cxxlens::sdk::row_cursor>{dynamic.error()};
			auto first = cursor
				? cursor->next()
				: cxxlens::sdk::result<std::optional<cxxlens::sdk::row_view>>{cursor.error()};
			require(dynamic && cursor && first && first->has_value() && (*first)->copy(),
					"exact v2 query projection was unavailable");

			auto invalid_selector = snapshot_draft(relation_engine);
			invalid_selector.series.channel_id.clear();
			auto invalid_selector_result = store->begin(std::move(invalid_selector));
			require(!invalid_selector_result &&
						invalid_selector_result.error().code ==
							"store.selection-authority-incomplete",
					"v2 migration result shadowed the existing selector error");
			auto invalid_authority = snapshot_draft(relation_engine);
			invalid_authority.catalog_semantic_digest = "not-a-digest";
			auto invalid_authority_result = store->begin(std::move(invalid_authority));
			require(!invalid_authority_result &&
						invalid_authority_result.error().code == "store.draft-authority-mismatch",
					"v2 migration result shadowed the existing draft authority error");
			auto valid_result = store->begin(snapshot_draft(relation_engine));
			require(!valid_result && valid_result.error().code == "store.migration-required" &&
						valid_result.error().field == "sqlite-physical-format" &&
						valid_result.error().detail == "cxxlens.sqlite-semantic-store.v2-to-v3",
					"valid v2 draft did not return the exact migration-required tuple");
		}
		const auto after = capture_files(path);
		require(after == before,
				"v2 open/read/query/begin changed main, sidecar, or directory-entry bytes");
	}

	void check_sqlite_v2_compact_migrates_and_preserves_diagnostics()
	{
		using namespace cxxlens::test::sqlite_fixture;
		const auto relation_engine = engine();
		temporary_directory directory{"sqlite-v2-migrate"};
		const auto path = directory.path() / "legacy.sqlite";
		const auto expected = make_exact_v2_fixture(path, relation_engine, true);
		const auto source_rows = read_v2_publications(path);
		const auto source_heads = read_heads(path);
		quiesce_wal_sidecars(path);
		const auto diagnostic_id = *expected.diagnostic_publication_id;
		const auto source_diagnostic =
			std::ranges::find(source_rows, diagnostic_id, &publication::publication_id);
		require(source_diagnostic != source_rows.end() && source_diagnostic->state == 4,
				"v2 corrupt noncommitted diagnostic fixture was not retained");
		std::uint64_t source_committed_maximum{};
		for (const auto& value : source_rows)
			if (value.state == 3)
				source_committed_maximum = std::max(source_committed_maximum,
													static_cast<std::uint64_t>(value.generation));

		{
			auto store = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
			require(store.has_value(), "v2 migration Store unavailable");
			auto old_handle = store->current(selector(relation_engine));
			require(old_handle.has_value(), "v2 migration pre-handle unavailable");
			auto dynamic = relation_engine.require("company.test.item", 1U);
			auto old_cursor = dynamic
				? old_handle->open(*dynamic)
				: cxxlens::sdk::result<cxxlens::sdk::row_cursor>{dynamic.error()};
			auto old_view = old_cursor
				? old_cursor->next()
				: cxxlens::sdk::result<std::optional<cxxlens::sdk::row_view>>{old_cursor.error()};
			require(dynamic && old_cursor && old_view && old_view->has_value(),
					"v2 migration pre-cursor unavailable");
			const auto pins_before = store->retained_generation_count();
			auto migrated = store->compact();
			if (!migrated)
				require(false,
						"explicit v2 compact migration failed: " + migrated.error().code + '/' +
							migrated.error().field + '/' + migrated.error().detail);
			const auto compatibility = store->compatibility();
			require(compatibility.readable_format.major == 3U &&
						compatibility.readable_format.minor == 0U &&
						compatibility.readable_format.patch == 0U && compatibility.direct_open &&
						!compatibility.migration_required,
					"successful v2 migration did not install v3 compatibility");
			auto current = store->current(selector(relation_engine));
			auto exported = store->canonical_export(expected.snapshot_id);
			require(current && exported &&
						current->publication().publication_id == expected.current_publication_id &&
						current->id() == expected.snapshot_id &&
						*exported == expected.canonical_export,
					"v2 migration changed head, IDs, or canonical export");
			require(store->retained_generation_count() > pins_before && (*old_view)->copy() &&
						old_handle->id() == expected.snapshot_id,
					"v2 migration invalidated a preexisting handle, cursor, or generation pin");
		}

		const auto target_rows = read_v3_publications(path);
		const auto target_heads = read_heads(path);
		require(target_heads == source_heads, "v2 migration changed durable series heads");
		const auto target_diagnostic =
			std::ranges::find(target_rows, diagnostic_id, &publication::publication_id);
		require(target_diagnostic != target_rows.end() &&
					target_diagnostic->series_id == source_diagnostic->series_id &&
					target_diagnostic->snapshot_id == source_diagnostic->snapshot_id &&
					target_diagnostic->sequence == source_diagnostic->sequence &&
					target_diagnostic->generation == source_diagnostic->generation &&
					target_diagnostic->parent == source_diagnostic->parent &&
					target_diagnostic->state == source_diagnostic->state &&
					target_diagnostic->checksum == source_diagnostic->checksum &&
					target_diagnostic->payload == source_diagnostic->payload,
				"v2 migration repaired, reclassified, or rewrote a noncommitted diagnostic");
		std::vector<std::uint64_t> committed_generations;
		for (const auto& value : target_rows)
			if (value.state == 3)
				committed_generations.push_back(static_cast<std::uint64_t>(value.generation));
		std::ranges::sort(committed_generations);
		require(committed_generations.size() == 2U &&
					committed_generations[0U] == source_committed_maximum + 1U &&
					committed_generations[1U] == source_committed_maximum + 2U,
				"v2 migration did not allocate the deterministic contiguous replacement range");
		quiesce_wal_sidecars(path);

		auto reopened = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
		auto current = reopened
			? reopened->current(selector(relation_engine))
			: cxxlens::sdk::result<cxxlens::sdk::snapshot_handle>{reopened.error()};
		auto exported = reopened ? reopened->canonical_export(expected.snapshot_id)
								 : cxxlens::sdk::result<std::string>{reopened.error()};
		require(reopened && current && exported &&
					current->publication().publication_id == expected.current_publication_id &&
					*exported == expected.canonical_export,
				"cold reopen lost the migrated v3 head or semantic projection");
	}

	void check_sqlite_v2_migration_terminal_faults()
	{
		using namespace cxxlens::sdk;
		using namespace cxxlens::test::sqlite_fixture;
		const auto relation_engine = engine();
		temporary_directory directory{"sqlite-v2-migration-terminal"};

		{
			const auto path = directory.path() / "precommit.sqlite";
			const auto expected = make_exact_v2_fixture(path, relation_engine, false);
			auto store = open_sqlite_snapshot_store(path.string(), relation_engine);
			require(store.has_value(), "v2 precommit terminal Store unavailable");
			const sqlite_store_fault_event event{sqlite_store_operation::migrate_predecessor,
												 sqlite_store_fault_boundary::transaction_begin,
												 sqlite_store_fault_timing::after,
												 1U,
												 1U};
			sqlite_store_fault_observation observed;
			{
				sqlite_store_fault_scope fault{{event, sqlite_store_fault_action::report_failure}};
				auto migrated = store->compact();
				require(!migrated && migrated.error().code == "store.sqlite-failure" &&
							migrated.error().field == "migration-fault" &&
							migrated.error().detail == "injected",
						"exact-v2 precommit terminal did not return the original trigger");
				observed = fault.observation();
			}
			require(observed.matching_event_count == 1U && observed.issued_directive_count == 1U,
					"v2 precommit fault did not dispatch exactly once");
			const auto compatibility = store->compatibility();
			auto current = store->current(selector(relation_engine));
			require(compatibility.migration_required && compatibility.direct_open && current &&
						current->publication().publication_id == expected.current_publication_id,
					"exact-v2 precommit recovery did not reinstall the predecessor authority");
		}

		{
			const auto path = directory.path() / "commit-unknown.sqlite";
			const auto expected = make_exact_v2_fixture(path, relation_engine, true);
			auto store = open_sqlite_snapshot_store(path.string(), relation_engine);
			require(store.has_value(), "v2 commit-unknown terminal Store unavailable");
			const sqlite_store_fault_event event{sqlite_store_operation::migrate_predecessor,
												 sqlite_store_fault_boundary::transaction_commit,
												 sqlite_store_fault_timing::after,
												 1U,
												 1U};
			sqlite_store_fault_observation observed;
			{
				sqlite_store_fault_scope fault{
					{event, sqlite_store_fault_action::report_failure_after_delegate}};
				auto migrated = store->compact();
				require(migrated.has_value(),
						"exact-v3 migration candidate was not recovered as idempotent success");
				observed = fault.observation();
			}
			require(observed.matching_event_count == 1U && observed.issued_directive_count == 1U,
					"v3 commit-unknown fault did not dispatch exactly once");
			const auto compatibility = store->compatibility();
			auto current = store->current(selector(relation_engine));
			require(!compatibility.migration_required && compatibility.direct_open && current &&
						current->publication().publication_id == expected.current_publication_id,
					"commit-unknown success did not install the exact v3 candidate");
		}
		{
			const auto path = directory.path() / "payload-chunk.sqlite";
			const auto expected = make_exact_v2_fixture(path, relation_engine, false);
			const auto rows = read_v2_publications(path);
			std::uint64_t total{};
			constexpr std::uint64_t chunk_maximum = 8U * 1024U * 1024U;
			for (const auto& row : rows)
				if (!row.payload.empty())
					total += 2U * (1U + ((row.payload.size() - 1U) / chunk_maximum));
			require(total != 0U, "v2 payload fault fixture did not contain a chunk");
			quiesce_wal_sidecars(path);
			auto store = open_sqlite_snapshot_store(path.string(), relation_engine);
			require(store.has_value(), "v2 payload terminal Store unavailable");
			const sqlite_store_fault_event event{sqlite_store_operation::migrate_predecessor,
												 sqlite_store_fault_boundary::payload_chunk,
												 sqlite_store_fault_timing::before,
												 1U,
												 total};
			{
				sqlite_store_fault_scope fault{{event, sqlite_store_fault_action::report_failure}};
				auto migrated = store->compact();
				require(!migrated && migrated.error().code == "store.sqlite-failure" &&
							migrated.error().field == "migration-fault" &&
							migrated.error().detail == "injected",
						"payload-chunk precommit failure did not return the original trigger");
				require(fault.observation().matching_event_count == 1U &&
							fault.observation().issued_directive_count == 1U,
						"payload-chunk fault did not dispatch at the exact typed boundary");
			}
			auto current = store->current(selector(relation_engine));
			require(store->compatibility().migration_required && current &&
						current->publication().publication_id == expected.current_publication_id,
					"payload-chunk rollback did not reinstall exact v2 authority");
		}
		{
			const auto path = directory.path() / "close-non-ok.sqlite";
			(void)make_exact_v2_fixture(path, relation_engine, false);
			auto store = open_sqlite_snapshot_store(path.string(), relation_engine);
			require(store.has_value(), "v2 close terminal Store unavailable");
			const sqlite_store_fault_event event{sqlite_store_operation::migrate_predecessor,
												 sqlite_store_fault_boundary::connection_close,
												 sqlite_store_fault_timing::after,
												 1U,
												 1U};
			{
				sqlite_store_fault_scope fault{
					{event, sqlite_store_fault_action::request_close_non_ok}};
				auto migrated = store->compact();
				require(!migrated && migrated.error().code == "store.sqlite-failure" &&
							migrated.error().field == "migration-recovery" &&
							migrated.error().detail == "opaque",
						"migration close uncertainty did not return the opaque terminal tuple");
				require(fault.observation().matching_event_count == 1U &&
							fault.observation().issued_directive_count == 1U,
						"migration close uncertainty did not dispatch exactly once");
			}
			auto blocked = store->current(selector(relation_engine));
			require(!blocked && blocked.error().code == "store.backend-unavailable" &&
						blocked.error().field == "sqlite-connection" &&
						blocked.error().detail == "reopen-required" &&
						!store->compatibility().direct_open,
					"migration close uncertainty did not poison result operations");
		}
		{
			const auto path = directory.path() / "observe-only.sqlite";
			(void)make_exact_v2_fixture(path, relation_engine, false);
			auto store = open_sqlite_snapshot_store(path.string(), relation_engine);
			require(store.has_value(), "v2 observe-only Store unavailable");
			const sqlite_store_fault_event event{sqlite_store_operation::migrate_predecessor,
												 sqlite_store_fault_boundary::format_marker,
												 sqlite_store_fault_timing::before,
												 1U,
												 1U};
			sqlite_store_fault_observation observed;
			{
				sqlite_store_fault_scope fault{{event, sqlite_store_fault_action::observe_only}};
				require(store->compact().has_value(),
						"observe-only migration directive changed execution");
				observed = fault.observation();
			}
			require(observed.matching_event_count == 1U && observed.issued_directive_count == 0U,
					"observe-only migration event did not remain passive");
		}
#if defined(__unix__) || defined(__APPLE__)
		{
			const auto path = directory.path() / "process crash % authority.sqlite";
			const auto expected = make_exact_v2_fixture(path, relation_engine, false);
			{
				set_sqlite_source_shm_symbols_available_for_testing(false);
				auto quiescent_without_source_shm_symbols =
					open_sqlite_snapshot_store(path.string(), relation_engine);
				set_sqlite_source_shm_symbols_available_for_testing(true);
				require(quiescent_without_source_shm_symbols &&
						quiescent_without_source_shm_symbols->compatibility().migration_required,
						"quiescent exact-v2 incorrectly required active-WAL source-SHM symbols");
			}
			active_wal_sidecar_fixture crash_source{
				path, wal_source_authority::predecessor_v2};
			const auto prepared_active_source =
				capture_sqlite_source_file_family_state(path);
			const auto prepared_wal = prepared_active_source.identities_and_sizes.find(
				path.filename().string() + "-wal");
			require(prepared_wal != prepared_active_source.identities_and_sizes.end() &&
					prepared_active_source.identities_and_sizes.contains(
						path.filename().string() + "-shm") &&
					std::get<2>(prepared_wal->second) > 32U,
					"migration crash source did not begin as an authentic active-v2 family");
			crash_source.close();
			const auto native_path = path.string();
			const auto child = ::fork();
			require(child >= 0, "migration crash subprocess fork failed");
			if (child == 0)
			{
				std::array arguments{const_cast<char*>(test_executable_path.c_str()),
									 const_cast<char*>("--sqlite-migration-crash-child"),
									 const_cast<char*>(native_path.c_str()),
									 static_cast<char*>(nullptr)};
				::execv(test_executable_path.c_str(), arguments.data());
				std::_Exit(127);
			}
			int status{};
			require(::waitpid(child, &status, 0) == child && WIFEXITED(status) &&
						WEXITSTATUS(status) == 86,
					"migration crash directive did not terminate only the subprocess");
			const auto crash_remnant = capture_sqlite_source_file_family_state(path);
			const auto crash_wal =
				crash_remnant.identities_and_sizes.find(path.filename().string() + "-wal");
			require(crash_remnant.identities_and_sizes.contains(path.filename().string()) &&
					crash_wal != crash_remnant.identities_and_sizes.end() &&
					crash_remnant.identities_and_sizes.contains(path.filename().string() + "-shm") &&
					std::get<2>(crash_wal->second) > 32U,
					"migration crash fixture did not retain an authentic raw "
					"main/WAL/SHM remnant");

			set_sqlite_source_shm_symbols_available_for_testing(false);
			auto unavailable = open_sqlite_snapshot_store(path.string(), relation_engine);
			set_sqlite_source_shm_symbols_available_for_testing(true);
			require(!unavailable && unavailable.error().code == "store.backend-unavailable" &&
					unavailable.error().field == "sqlite" &&
					unavailable.error().detail == "source-shm-readonly-qualification",
					"active WAL+SHM did not apply its branch-local required-symbol gate");
			require(capture_sqlite_source_file_family_state(path) == crash_remnant,
					"active source-SHM symbol failure opened, changed, or privately fell back from the "
					"raw post-crash active-v2 source");

			{
				auto reopened = open_sqlite_snapshot_store(path.string(), relation_engine);
				auto current = reopened ? reopened->current(selector(relation_engine))
										: result<snapshot_handle>{reopened.error()};
				auto exported = reopened ? reopened->canonical_export(expected.snapshot_id)
								 : result<std::string>{reopened.error()};
				require(reopened && reopened->compatibility().migration_required && current &&
							exported && current->id() == expected.snapshot_id &&
							current->publication().publication_id ==
								expected.current_publication_id &&
							*exported == expected.canonical_export,
						"raw post-crash active-v2 source did not preserve exact predecessor authority");
			}
			require(capture_sqlite_source_file_family_state(path) == crash_remnant,
					"qualified cold read changed raw post-crash identities, sizes, or bytes");
		}
#endif
	}

	void check_sqlite_v3_compaction_pins_and_empty()
	{
		using namespace cxxlens::test::sqlite_fixture;
		const auto relation_engine = engine();
		temporary_directory directory{"sqlite-v3-compact"};
		const auto populated_path = directory.path() / "populated.sqlite";
		std::string expected_snapshot_id;
		std::string expected_publication_id;
		std::string expected_export;
		{
			auto store =
				cxxlens::sdk::open_sqlite_snapshot_store(populated_path.string(), relation_engine);
			require(store.has_value(), "v3 compaction Store unavailable");
			auto first = publish(*store, relation_engine, false);
			auto second =
				publish(*store, relation_engine, true, first.publication().publication_id);
			expected_snapshot_id = second.id();
			expected_publication_id = second.publication().publication_id;
			auto exported = store->canonical_export(second.id());
			require(exported.has_value(), "v3 compaction source export unavailable");
			expected_export = *exported;
			auto old_handle = store->current(selector(relation_engine));
			auto dynamic = relation_engine.require("company.test.item", 1U);
			auto old_cursor = old_handle && dynamic
				? old_handle->open(*dynamic)
				: cxxlens::sdk::result<cxxlens::sdk::row_cursor>{old_handle ? dynamic.error()
																			: old_handle.error()};
			auto old_view = old_cursor
				? old_cursor->next()
				: cxxlens::sdk::result<std::optional<cxxlens::sdk::row_view>>{old_cursor.error()};
			require(old_handle && dynamic && old_cursor && old_view && old_view->has_value(),
					"v3 compaction preexisting cursor unavailable");
			const auto pins_before = store->retained_generation_count();
			require(store->compact().has_value(), "ordinary v3 compaction failed");
			auto current = store->current(selector(relation_engine));
			auto compacted_export = store->canonical_export(expected_snapshot_id);
			require(current && compacted_export &&
						current->publication().publication_id == expected_publication_id &&
						*compacted_export == expected_export && (*old_view)->copy() &&
						old_handle->id() == expected_snapshot_id &&
						store->retained_generation_count() > pins_before,
					"v3 compaction changed semantics or invalidated an old pin");
		}
		auto reopened =
			cxxlens::sdk::open_sqlite_snapshot_store(populated_path.string(), relation_engine);
		auto cold = reopened
			? reopened->current(selector(relation_engine))
			: cxxlens::sdk::result<cxxlens::sdk::snapshot_handle>{reopened.error()};
		auto cold_export = reopened ? reopened->canonical_export(expected_snapshot_id)
									: cxxlens::sdk::result<std::string>{reopened.error()};
		require(reopened && cold && cold_export &&
					cold->publication().publication_id == expected_publication_id &&
					*cold_export == expected_export,
				"cold reopen lost an ordinary v3 compaction result");

		const auto empty_path = directory.path() / "empty.sqlite";
		{
			auto empty =
				cxxlens::sdk::open_sqlite_snapshot_store(empty_path.string(), relation_engine);
			require(empty.has_value(), "zero-row v3 Store unavailable");
		}
		const auto before = capture_files(empty_path);
		{
			auto empty =
				cxxlens::sdk::open_sqlite_snapshot_store(empty_path.string(), relation_engine);
			require(empty && empty->compact() && empty->retained_generation_count() == 0U,
					"zero-row v3 compaction was not a successful no-generation operation");
		}
		const auto after = capture_files(empty_path);
		require(after == before, "zero-row v3 compaction changed durable database authority");
		auto empty_reopened =
			cxxlens::sdk::open_sqlite_snapshot_store(empty_path.string(), relation_engine);
		auto missing = empty_reopened
			? empty_reopened->current(selector(relation_engine))
			: cxxlens::sdk::result<cxxlens::sdk::snapshot_handle>{empty_reopened.error()};
		require(empty_reopened && !missing && missing.error().code == "store.current-not-found",
				"zero-row v3 compaction invented a durable head");
	}

	void check_sqlite_v3_chunk_corruption_classes()
	{
		using namespace cxxlens::test::sqlite_fixture;
		const auto relation_engine = engine();
		temporary_directory directory{"sqlite-v3-corruption"};
		const auto pristine = directory.path() / "pristine.sqlite";
		std::string current_publication;
		std::string prior_publication;
		std::string snapshot_id;
		{
			auto store =
				cxxlens::sdk::open_sqlite_snapshot_store(pristine.string(), relation_engine);
			require(store.has_value(), "chunk corruption source Store unavailable");
			auto first = publish(*store, relation_engine, false);
			auto second = publish_partition(
				*store,
				relation_engine,
				partition_with_payload(
					relation_engine,
					"item:chunk-boundary",
					std::string(static_cast<std::size_t>(chunk_maximum_bytes) + 65'537U, 'z')),
				first.publication().publication_id);
			prior_publication = first.publication().publication_id;
			current_publication = second.publication().publication_id;
			snapshot_id = second.id();
		}

		const auto require_local_corruption =
			[&](const std::filesystem::path& path, std::string_view label)
		{
			auto local = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), relation_engine);
			auto local_current = local
				? local->current(selector(relation_engine))
				: cxxlens::sdk::result<cxxlens::sdk::snapshot_handle>{local.error()};
			auto local_semantic = local
				? local->open(snapshot_id)
				: cxxlens::sdk::result<cxxlens::sdk::snapshot_handle>{local.error()};
			auto local_publication = local
				? local->open_publication(current_publication)
				: cxxlens::sdk::result<cxxlens::sdk::snapshot_handle>{local.error()};
			auto intact_prior = local
				? local->open_publication(prior_publication)
				: cxxlens::sdk::result<cxxlens::sdk::snapshot_handle>{local.error()};
			auto local_compact =
				local ? local->compact() : cxxlens::sdk::result<void>{local.error()};
			require(local && !local_current &&
						local_current.error().code == "store.current-corrupt" && !local_semantic &&
						local_semantic.error().code == "store.snapshot-corrupt" &&
						!local_publication &&
						local_publication.error().code == "store.publication-corrupt" &&
						intact_prior && !local_compact &&
						local_compact.error().code == "store.compact-validation-failed" &&
						local_compact.error().field == current_publication,
					"publication-local " + std::string{label} +
						" did not retain exact no-fallback diagnostics");
		};

		const auto missing_path = directory.path() / "missing.sqlite";
		std::filesystem::copy_file(pristine, missing_path);
		delete_chunk(missing_path, current_publication, 0);
		require_local_corruption(missing_path, "missing chunk");

		const auto extra_path = directory.path() / "extra.sqlite";
		std::filesystem::copy_file(pristine, extra_path);
		insert_extra_chunk(extra_path, current_publication);
		require_local_corruption(extra_path, "extra chunk");

		const auto offset_path = directory.path() / "offset.sqlite";
		std::filesystem::copy_file(pristine, offset_path);
		drift_chunk_offset(offset_path, current_publication, 0);
		require_local_corruption(offset_path, "malformed offset");

		const auto checksum_path = directory.path() / "checksum.sqlite";
		std::filesystem::copy_file(pristine, checksum_path);
		rewrite_chunk_checksum(checksum_path, current_publication, 0);
		require_local_corruption(checksum_path, "wrong checksum");

		const auto boundary_path = directory.path() / "boundary.sqlite";
		std::filesystem::copy_file(pristine, boundary_path);
		shift_nonfinal_chunk_boundary(boundary_path, current_publication);
		require_local_corruption(boundary_path, "shifted nonfinal boundary");

		const auto orphan_path = directory.path() / "orphan.sqlite";
		std::filesystem::copy_file(pristine, orphan_path);
		insert_orphan_chunk(orphan_path, current_publication);
		auto global =
			cxxlens::sdk::open_sqlite_snapshot_store(orphan_path.string(), relation_engine);
		require(!global && global.error().code == "store.corrupt" &&
					global.error().field == "sqlite-chunk-authority" &&
					global.error().detail ==
						"global-orphan-retired-or-duplicate-committed-generation",
				"global orphan chunk did not fail the Store factory with the exact tuple");
	}

	void check_sqlite_active_wal_and_wal_only_routes()
	{
		using namespace cxxlens::test::sqlite_fixture;
		const auto relation_engine = engine();
		temporary_directory directory{"sqlite-wal-routes"};

		const auto active_path = directory.path() / "active.sqlite";
		std::string active_publication;
		std::string active_snapshot;
		{
			auto source =
				cxxlens::sdk::open_sqlite_snapshot_store(active_path.string(), relation_engine);
			require(source.has_value(), "active-WAL source Store unavailable");
			auto published = publish(*source, relation_engine, false);
			active_publication = published.publication().publication_id;
			active_snapshot = published.id();
		}
		{
			active_wal_sidecar_fixture active{active_path};
#if defined(__linux__) && defined(F_OFD_SETLK)
			const auto active_source_before =
				capture_sqlite_source_file_family_state(active.path());
			auto opened =
				cxxlens::sdk::open_sqlite_snapshot_store(active.path().string(), relation_engine);
			require(opened.has_value(), "active WAL+SHM Store route unavailable");
			auto current = opened->current(selector(relation_engine));
			require(current && current->id() == active_snapshot &&
						current->publication().publication_id == active_publication,
					"active WAL+SHM route changed the recovered authority");
			require(capture_sqlite_source_file_family_state(active.path()) == active_source_before,
					"qualified active WAL+SHM read changed source identities, sizes, or bytes");
			auto next = publish(*opened, relation_engine, true, active_publication);
			require(next.publication().sequence == current->publication().sequence + 1U,
					"active WAL+SHM route did not hand off to the normal writer");
#else
			const auto active_source_before = capture_files(active.path());
			auto opened =
				cxxlens::sdk::open_sqlite_snapshot_store(active.path().string(), relation_engine);
			require(!opened && opened.error().code == "store.backend-unavailable" &&
						opened.error().field == "sqlite" &&
						opened.error().detail == "source-shm-readonly-qualification",
					"active WAL+SHM route did not report exact qualification unavailability");
			require(capture_files(active.path()) == active_source_before,
					"unavailable active WAL+SHM route changed source bytes");
#endif
		}

		const auto cold_source_path = directory.path() / "cold-source.sqlite";
		const auto cold_path = directory.path() / "cold-copy.sqlite";
		std::string cold_publication;
		std::string cold_snapshot;
		{
			auto source = cxxlens::sdk::open_sqlite_snapshot_store(cold_source_path.string(),
																   relation_engine);
			require(source.has_value(), "WAL-only source Store unavailable");
			auto published = publish(*source, relation_engine, false);
			cold_publication = published.publication().publication_id;
			cold_snapshot = published.id();
		}
		auto cold = make_cold_wal_only_copy(cold_source_path, cold_path);
		const auto before_read = capture_files(cold.path());
		std::string recovered_publication;
		{
			auto opened =
				cxxlens::sdk::open_sqlite_snapshot_store(cold.path().string(), relation_engine);
			require(opened.has_value(), "cold WAL-only private recovery route unavailable");
			auto current = opened->current(selector(relation_engine));
			require(current && current->id() == cold_snapshot &&
						current->publication().publication_id == cold_publication,
					"cold WAL-only route changed the recovered authority");
			require(capture_files(cold.path()) == before_read,
					"cold WAL-only eager read mutated the source file family");
			auto next = publish(*opened, relation_engine, true, cold_publication);
			recovered_publication = next.publication().publication_id;
			require(next.publication().sequence == current->publication().sequence + 1U,
					"cold WAL-only first mutation did not complete the recovery handoff");
		}
		auto reopened =
			cxxlens::sdk::open_sqlite_snapshot_store(cold.path().string(), relation_engine);
		require(reopened.has_value(), "recovered WAL-only Store did not reopen");
		auto recovered = reopened->current(selector(relation_engine));
		require(recovered && recovered->publication().publication_id == recovered_publication,
				"WAL-only recovery handoff did not durably preserve the first mutation");

		const auto drift_path = directory.path() / "cold-drift.sqlite";
		auto drift = make_cold_wal_only_copy(cold_source_path, drift_path);
		auto drifted =
			cxxlens::sdk::open_sqlite_snapshot_store(drift.path().string(), relation_engine);
		require(drifted.has_value(), "WAL-only drift source Store unavailable");
		auto writer = drifted->begin(snapshot_draft(relation_engine, cold_publication));
		require(writer && writer->stage(partition(relation_engine, false)) && writer->validate(),
				"WAL-only drift publication setup failed");
		const std::filesystem::path drift_wal{drift.path().string() + "-wal"};
		std::filesystem::resize_file(drift_wal, std::filesystem::file_size(drift_wal) + 1U);
		auto rejected = writer->publish();
		require(!rejected && rejected.error().code == "store.sqlite-failure" &&
					rejected.error().field == "sqlite-initialization-sidecar" &&
					rejected.error().detail == "concurrent-source-change",
				"WAL-only pre-effect drift did not return the exact no-write tuple");
		auto retained = drifted->current(selector(relation_engine));
		require(retained && retained->id() == cold_snapshot &&
					retained->publication().publication_id == cold_publication,
				"WAL-only pre-effect drift discarded the eager read state");

		const auto v2_source_path = directory.path() / "v2-source.sqlite";
		const auto v2_path = directory.path() / "v2-cold-copy.sqlite";
		const auto v2_expected = make_exact_v2_fixture(v2_source_path, relation_engine, false);
		auto v2_cold =
			make_cold_wal_only_copy(v2_source_path, v2_path, wal_source_authority::predecessor_v2);
		const auto v2_before_read = capture_files(v2_cold.path());
		{
			auto opened =
				cxxlens::sdk::open_sqlite_snapshot_store(v2_cold.path().string(), relation_engine);
			require(opened.has_value(), "v2 cold WAL-only private recovery route unavailable");
			const auto before_compatibility = opened->compatibility();
			auto current = opened->current(selector(relation_engine));
			auto exported = opened->canonical_export(v2_expected.snapshot_id);
			require(before_compatibility.migration_required && current && exported &&
						current->id() == v2_expected.snapshot_id &&
						current->publication().publication_id ==
							v2_expected.current_publication_id &&
						*exported == v2_expected.canonical_export,
					"v2 cold WAL-only eager read changed the predecessor authority");
			require(capture_files(v2_cold.path()) == v2_before_read,
					"v2 cold WAL-only eager read mutated the source file family");

			auto compacted = opened->compact();
			require(compacted.has_value(), "v2 cold WAL-only explicit compact failed");
			const auto after_compatibility = opened->compatibility();
			auto migrated = opened->current(selector(relation_engine));
			auto migrated_export = opened->canonical_export(v2_expected.snapshot_id);
			require(!after_compatibility.migration_required &&
						after_compatibility.readable_format.major == 3U && migrated &&
						migrated_export && migrated->id() == v2_expected.snapshot_id &&
						migrated->publication().publication_id ==
							v2_expected.current_publication_id &&
						*migrated_export == v2_expected.canonical_export,
					"v2 WAL-only compact did not preserve the migrated semantic authority");
		}
		auto v2_reopened =
			cxxlens::sdk::open_sqlite_snapshot_store(v2_cold.path().string(), relation_engine);
		auto v2_reopened_current = v2_reopened
			? v2_reopened->current(selector(relation_engine))
			: cxxlens::sdk::result<cxxlens::sdk::snapshot_handle>{v2_reopened.error()};
		require(v2_reopened && !v2_reopened->compatibility().migration_required &&
					v2_reopened_current &&
					v2_reopened_current->publication().publication_id ==
						v2_expected.current_publication_id,
				"migrated v2 WAL-only Store did not cold-reopen as current v3");
	}

	void check_sqlite_poison_guards_and_nonresult_observers()
	{
		using namespace cxxlens::sdk;
		using namespace cxxlens::test::sqlite_fixture;
		const auto relation_engine = engine();
		temporary_directory directory{"sqlite-poison-observers"};
		const auto source_path = directory.path() / "source.sqlite";
		const auto cold_path = directory.path() / "cold.sqlite";
		std::string publication_id;
		std::string snapshot_id;
		{
			auto source = open_sqlite_snapshot_store(source_path.string(), relation_engine);
			require(source.has_value(), "poison source Store unavailable");
			auto baseline = publish(*source, relation_engine, false);
			publication_id = baseline.publication().publication_id;
			snapshot_id = baseline.id();
		}

		auto cold = make_cold_wal_only_copy(source_path, cold_path);
		auto store = open_sqlite_snapshot_store(cold.path().string(), relation_engine);
		require(store.has_value(), "poison WAL-only Store unavailable");
		auto old_handle = store->current(selector(relation_engine));
		auto dynamic = relation_engine.require("company.test.item", 1U);
		require(old_handle.has_value() && dynamic.has_value(),
				"poison preexisting snapshot projection unavailable");
		auto old_cursor =
			dynamic ? old_handle->open(*dynamic) : result<row_cursor>{dynamic.error()};
		auto first_row =
			old_cursor ? old_cursor->next() : result<std::optional<row_view>>{old_cursor.error()};
		require(old_handle && dynamic && old_cursor && first_row && first_row->has_value(),
				"poison preexisting handle/cursor setup failed");
		auto first_copy_before = (*first_row)->copy();
		require(first_copy_before.has_value(), "poison preexisting row view was unavailable");

		auto triggering_writer = store->begin(snapshot_draft(relation_engine, publication_id));
		auto preexisting_writer = store->begin(snapshot_draft(relation_engine, publication_id));
		require(triggering_writer && triggering_writer->stage(partition(relation_engine, false)) &&
					triggering_writer->validate() && preexisting_writer &&
					preexisting_writer->stage(partition(relation_engine, true)) &&
					preexisting_writer->validate(),
				"poison preexisting writers were not independently validated");

		const auto compatibility_before = store->compatibility();
		const auto pins_before = store->retained_generation_count();
		sqlite_store_fault_observation fault_observation;
		{
			const sqlite_store_fault_event close_after{
				sqlite_store_operation::wal_recovery_handoff,
				sqlite_store_fault_boundary::connection_close,
				sqlite_store_fault_timing::after,
				1U,
				1U};
			sqlite_store_fault_scope fault{
				{close_after, sqlite_store_fault_action::request_close_non_ok}};
			require(fault.active(), "poison close fault scope was not active");
			auto poisoned = triggering_writer->publish();
			require(!poisoned && poisoned.error().code == "store.sqlite-failure" &&
						poisoned.error().field == "sqlite-recovery-handoff" &&
						poisoned.error().detail == "opaque",
					"typed close uncertainty did not produce the handoff terminal result");
			fault_observation = fault.observation();
			require(fault_observation.observed_event_count == 1U &&
						fault_observation.matching_event_count == 1U &&
						fault_observation.issued_directive_count == 1U &&
						fault_observation.has_last_observed_event &&
						fault_observation.has_matched_event &&
						fault_observation.last_observed_event == close_after &&
						fault_observation.matched_event == close_after,
					"typed close fault did not retain its exact dispatch observation");
		}

		const auto require_reopen = [&](const auto& value, const std::string_view label)
		{
			require(!value && value.error().code == "store.backend-unavailable" &&
						value.error().field == "sqlite-connection" &&
						value.error().detail == "reopen-required",
					std::string{label});
		};
		auto invalid_selector = selector(relation_engine);
		invalid_selector.channel_id.clear();
		require_reopen(store->current(invalid_selector),
					   "poison did not precede current selector validation");
		require_reopen(store->open("not-a-snapshot"), "poison did not guard open");
		require_reopen(store->open_publication("not-a-publication"),
					   "poison did not guard open_publication");
		auto invalid_draft = snapshot_draft(relation_engine);
		invalid_draft.series.channel_id.clear();
		require_reopen(store->begin(std::move(invalid_draft)),
					   "poison did not precede begin draft validation");
		require_reopen(store->compact(), "poison did not guard compact");
		require_reopen(store->canonical_export("not-a-snapshot"),
					   "poison did not guard canonical_export");
		require_reopen(preexisting_writer->publish(),
					   "poison did not precede a preexisting writer publish");

		const auto compatibility_after = store->compatibility();
		require(compatibility_after.backend == compatibility_before.backend &&
					compatibility_after.readable_format == compatibility_before.readable_format &&
					!compatibility_after.direct_open &&
					compatibility_after.migration_required ==
						compatibility_before.migration_required,
				"poison changed the last independently validated compatibility tuple");
		require(store->retained_generation_count() == pins_before,
				"poison changed the exact live process-generation pin count");
		auto first_copy_after = (*first_row)->copy();
		require(old_handle->id() == snapshot_id &&
					old_handle->publication().publication_id == publication_id &&
					first_copy_after &&
					first_copy_after->canonical_form() == first_copy_before->canonical_form(),
				"poison invalidated a preexisting immutable handle or row view");
		for (;;)
		{
			auto remaining = old_cursor->next();
			require(remaining.has_value(), "poison invalidated a preexisting cursor");
			if (!*remaining)
				break;
			require((*remaining)->copy().has_value(),
					"poison invalidated a remaining preexisting cursor row");
		}
	}
} // namespace

int main(const int argc, char** argv)
{
#if defined(__unix__) || defined(__APPLE__)
	std::error_code executable_error;
	test_executable_path = std::filesystem::canonical(argv[0], executable_error).string();
	if (executable_error)
		test_executable_path = argv[0];
	if (argc == 3 && std::string_view{argv[1]} == "--sqlite-migration-crash-child")
	{
		const auto relation_engine = engine();
		auto store = cxxlens::sdk::open_sqlite_snapshot_store(argv[2], relation_engine);
		if (!store)
			return 2;
		const cxxlens::sdk::sqlite_store_fault_event event{
			cxxlens::sdk::sqlite_store_operation::migrate_predecessor,
			cxxlens::sdk::sqlite_store_fault_boundary::format_marker,
			cxxlens::sdk::sqlite_store_fault_timing::after,
			1U,
			1U};
		cxxlens::sdk::sqlite_store_fault_scope fault{
			{event, cxxlens::sdk::sqlite_store_fault_action::request_process_crash}};
		(void)store->compact();
		return 3;
	}
#else
	(void)argc;
	(void)argv;
#endif
	check_sqlite_locator_prevalidation();
	check_sqlite_format_discriminator_and_index_profile_precedence();
	check_sqlite_v2_compact_rejects_corrupt_committed_before_write();
	check_sqlite_v3_chunk_corruption_classes();
	check_sqlite_v3_compaction_pins_and_empty();
	check_sqlite_v2_compact_migrates_and_preserves_diagnostics();
	check_sqlite_v2_migration_terminal_faults();
	check_sqlite_v3_fresh_schema_and_chunk_boundary();
	check_sqlite_v2_read_only_and_begin_precedence();
	check_sqlite_active_wal_and_wal_only_routes();
	check_sqlite_poison_guards_and_nonresult_observers();
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
