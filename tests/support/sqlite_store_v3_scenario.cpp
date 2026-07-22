#include "sqlite_store_v3_scenario.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "sqlite_store_fixture.hpp"

namespace cxxlens::sdk
{
	result<void> rewrite_publication_payload_schema_for_testing(snapshot_store& store,
																std::string_view publication_id,
																std::uint8_t payload_version);
	result<std::string>
	insert_noncommitted_publication_for_testing(snapshot_store& store,
												std::string_view source_publication_id,
												std::uint8_t payload_version);
} // namespace cxxlens::sdk

namespace cxxlens::test::sqlite_v3_scenario
{
	namespace
	{
		constexpr std::string_view producer_digest =
			"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

		[[noreturn]] void fail(const std::string_view operation, const cxxlens::sdk::error& value)
		{
			throw std::runtime_error{std::string{operation} + ": " + value.code + '/' +
									 value.field + '/' + value.detail};
		}

		[[nodiscard]] cxxlens::sdk::relation_descriptor descriptor()
		{
			cxxlens::sdk::relation_descriptor value;
			value.id = "company.test.sqlite_v3_item.v1";
			value.name = "company.test.sqlite_v3_item";
			value.version = {1U, 0U, 0U};
			value.semantic_major = 1U;
			value.semantics = "company.test.sqlite_v3_item/1";
			value.owner_namespace = "company.test";
			value.columns = {
				{"company.test.sqlite_v3_item.v1.key",
				 "key",
				 {cxxlens::sdk::scalar_kind::typed_id, "sqlite_v3_item_id", false},
				 true,
				 cxxlens::sdk::column_role::claim_key},
				{"company.test.sqlite_v3_item.v1.value",
				 "value",
				 {cxxlens::sdk::scalar_kind::utf8_string, {}, false},
				 true,
				 cxxlens::sdk::column_role::authoritative_payload},
			};
			value.key_columns = {"company.test.sqlite_v3_item.v1.key"};
			value.merge = cxxlens::sdk::merge_mode::set;
			auto digest = cxxlens::sdk::semantic_digest("cxxlens.relation-descriptor-binding.v2",
														value.contract_digest + "\n" +
															value.canonical_form());
			if (!digest)
				fail("relation descriptor digest", digest.error());
			value.descriptor_digest = std::move(*digest);
			return value;
		}

		[[nodiscard]] cxxlens::sdk::detached_row row(std::string key, std::string payload)
		{
			auto relation = descriptor();
			cxxlens::sdk::row_builder builder{relation};
			if (auto inserted = builder.set(
					{relation.id, relation.columns[0].id, relation.columns[0].type},
					cxxlens::sdk::detached_cell::typed("sqlite_v3_item_id", std::move(key)));
				!inserted)
				fail("row key", inserted.error());
			if (auto inserted =
					builder.set({relation.id, relation.columns[1].id, relation.columns[1].type},
								cxxlens::sdk::detached_cell::utf8(std::move(payload)));
				!inserted)
				fail("row payload", inserted.error());
			auto output = std::move(builder).finish();
			if (!output)
				fail("row finish", output.error());
			return std::move(*output);
		}

		[[nodiscard]] cxxlens::sdk::claim make_claim(const cxxlens::sdk::relation_engine& engine,
													 std::string key,
													 std::string payload)
		{
			cxxlens::sdk::observation observed{
				row(std::move(key), std::move(payload)),
				{"universe-sqlite-v3", {"all"}},
				"company.test.sqlite-v3-canonical-1",
				{"company.test.sqlite-v3-provider", std::string{producer_digest}},
				{"sha256:9999999999999999999999999999999999999999999999999999999999999999"},
				"evidence:sqlite-v3",
				{"exact", "partition", "assumptions:none", {"schema_validated"}},
			};
			auto output = cxxlens::sdk::make_assertion(engine, std::move(observed));
			if (!output)
				fail("claim", output.error());
			return std::move(*output);
		}

		void finish_partition_authority(cxxlens::sdk::partition_draft& partition)
		{
			if (partition.claims.empty())
				throw std::runtime_error{"SQLite v3 scenario partition has no claims"};
			auto basis =
				cxxlens::sdk::claim_input_basis_digest(partition.claims.front().input_basis);
			if (!basis)
				fail("partition input basis", basis.error());
			partition.producer_input_basis_digest = std::move(*basis);
		}

		[[nodiscard]] cxxlens::sdk::partition_draft partition_shell()
		{
			cxxlens::sdk::partition_draft output;
			output.relation_descriptor_id = descriptor().id;
			output.scope = "compile-unit-sqlite-v3";
			output.condition = {"universe-sqlite-v3", {"all"}};
			output.interpretation = "company.test.sqlite-v3-canonical-1";
			output.producer_semantics = producer_digest;
			output.precision_profile = "exact";
			output.assumption_set_id = "assumptions-empty";
			output.coverage = {{"compile-unit", "compile-unit-sqlite-v3", "covered", ""}};
			return output;
		}

		[[nodiscard]] std::string generated_value(const std::size_t seed)
		{
			std::string output;
			output.reserve(large_value_byte_count);
			std::array<char, generator_scratch_byte_count> scratch{};
			for (std::size_t offset{}; offset < large_value_byte_count;)
			{
				const auto count = std::min(scratch.size(), large_value_byte_count - offset);
				for (std::size_t index{}; index < count; ++index)
					scratch[index] = static_cast<char>('a' + ((offset + index + seed * 17U) % 26U));
				output.append(scratch.data(), count);
				offset += count;
			}
			return output;
		}

		[[nodiscard]] std::string digest_text(const std::string_view value)
		{
			return cxxlens::sdk::content_digest(
				std::as_bytes(std::span{value.data(), value.size()}));
		}

		[[nodiscard]] cxxlens::sdk::partition_draft
		legacy_payload_partition(const cxxlens::sdk::relation_engine& engine,
								 const std::uint8_t payload_version)
		{
			auto output = partition_shell();
			const auto suffix = ":v" + std::to_string(payload_version);
			output.claims = {
				make_claim(engine, "item:1" + suffix, "one" + suffix),
				make_claim(engine, "item:2" + suffix, "two" + suffix),
			};
			if (payload_version % 2U == 0U)
				std::ranges::reverse(output.claims);
			finish_partition_authority(output);
			return output;
		}
	} // namespace

	cxxlens::sdk::relation_engine make_engine()
	{
		cxxlens::sdk::relation_registry registry;
		if (auto added = registry.add(descriptor()); !added)
			fail("relation registry", added.error());
		auto output = registry.build("engine-ng0-sqlite-v3-scenario");
		if (!output)
			fail("relation engine", output.error());
		return std::move(*output);
	}

	cxxlens::sdk::snapshot_series_selector selector(const cxxlens::sdk::relation_engine& engine)
	{
		return {"catalog-sqlite-v3",
				"stable",
				std::string{engine.generation()},
				"universe-sqlite-v3",
				std::string{engine.registry_digest()},
				"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
				"sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"};
	}

	cxxlens::sdk::snapshot_draft snapshot_draft(const cxxlens::sdk::relation_engine& engine,
												std::optional<std::string> parent)
	{
		return {selector(engine),
				{1U, 0U, 0U},
				"sha256:6666666666666666666666666666666666666666666666666666666666666666",
				std::move(parent)};
	}

	cxxlens::sdk::partition_draft small_partition(const cxxlens::sdk::relation_engine& engine,
												  const bool reverse)
	{
		auto output = partition_shell();
		output.claims = {
			make_claim(engine, "item:1", "one"),
			make_claim(engine, "item:2", "two"),
		};
		if (reverse)
			std::ranges::reverse(output.claims);
		finish_partition_authority(output);
		return output;
	}

	cxxlens::sdk::partition_draft
	bounded_large_v5_partition(const cxxlens::sdk::relation_engine& engine)
	{
		static_assert(generator_scratch_byte_count < 8U * 1024U * 1024U);
		static_assert(large_value_byte_count < 8U * 1024U * 1024U);
		static_assert(large_value_count * large_value_byte_count > 16U * 1024U * 1024U);

		auto output = partition_shell();
		output.claims.reserve(large_value_count);
		for (std::size_t index{}; index < large_value_count; ++index)
		{
			output.claims.push_back(
				make_claim(engine, "item:large:" + std::to_string(index), generated_value(index)));
		}
		finish_partition_authority(output);
		return output;
	}

	cxxlens::sdk::snapshot_handle publish_partition(cxxlens::sdk::snapshot_store& store,
													const cxxlens::sdk::relation_engine& engine,
													cxxlens::sdk::partition_draft partition,
													std::optional<std::string> parent)
	{
		auto writer = store.begin(snapshot_draft(engine, std::move(parent)));
		if (!writer)
			fail("Store begin", writer.error());
		if (auto staged = writer->stage(std::move(partition)); !staged)
			fail("Store stage", staged.error());
		if (auto validated = writer->validate(); !validated)
			fail("Store validate", validated.error());
		auto output = writer->publish();
		if (!output)
			fail("Store publish", output.error());
		return std::move(*output);
	}

	cxxlens::sdk::snapshot_handle publish_small(cxxlens::sdk::snapshot_store& store,
												const cxxlens::sdk::relation_engine& engine,
												const bool reverse,
												std::optional<std::string> parent)
	{
		return publish_partition(
			store, engine, small_partition(engine, reverse), std::move(parent));
	}

	cxxlens::sdk::snapshot_handle
	publish_bounded_large_v5(cxxlens::sdk::snapshot_store& store,
							 const cxxlens::sdk::relation_engine& engine,
							 std::optional<std::string> parent)
	{
		return publish_partition(
			store, engine, bounded_large_v5_partition(engine), std::move(parent));
	}

	store_projection capture_projection(cxxlens::sdk::snapshot_store& store,
										const cxxlens::sdk::snapshot_handle& snapshot)
	{
		auto exported = store.canonical_export(snapshot.id());
		if (!exported)
			fail("Store canonical export", exported.error());
		return {std::string{snapshot.id()},
				std::string{snapshot.publication().publication_id},
				snapshot.publication().sequence,
				snapshot.publication().physical_generation,
				digest_text(*exported)};
	}

	store_projection create_current_v3_scenario(const std::filesystem::path& path,
												const cxxlens::sdk::relation_engine& engine)
	{
		auto store = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), engine);
		if (!store)
			fail("create current-v3 Store", store.error());
		auto snapshot = publish_small(*store, engine);
		return capture_projection(*store, snapshot);
	}

	exact_v2_scenario create_exact_v2_scenario(const std::filesystem::path& path,
											   const cxxlens::sdk::relation_engine& engine)
	{
		exact_v2_scenario output;
		std::string diagnostic_publication_id;
		{
			auto store = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), engine);
			if (!store)
				fail("create v2-source Store", store.error());
			std::optional<std::string> parent;
			std::string first_publication_id;
			for (std::uint8_t version = 1U; version <= 5U; ++version)
			{
				auto published = publish_partition(
					*store, engine, legacy_payload_partition(engine, version), parent);
				if (auto rewritten = cxxlens::sdk::rewrite_publication_payload_schema_for_testing(
						*store, published.publication().publication_id, version);
					!rewritten)
					fail("v2-source legacy payload", rewritten.error());
				if (version == 1U)
				{
					output.prior = capture_projection(*store, published);
					first_publication_id = published.publication().publication_id;
				}
				if (version == 5U)
					output.current = capture_projection(*store, published);
				parent = published.publication().publication_id;
			}
			auto diagnostic = cxxlens::sdk::insert_noncommitted_publication_for_testing(
				*store, first_publication_id, 1U);
			if (!diagnostic)
				fail("v2-source diagnostic payload", diagnostic.error());
			diagnostic_publication_id = std::move(*diagnostic);
			auto exported = store->canonical_export(output.current.snapshot_id);
			if (!exported)
				fail("v2-source canonical export", exported.error());
			output.current_canonical_export = std::move(*exported);
		}
		sqlite_fixture::downgrade_v3_to_exact_v2(path);
		sqlite_fixture::rewrite_v2_checksum(
			path,
			diagnostic_publication_id,
			"sha256:0000000000000000000000000000000000000000000000000000000000000000");
		sqlite_fixture::require_wal_header_and_quiescent_sidecars(path);
		return output;
	}
} // namespace cxxlens::test::sqlite_v3_scenario
