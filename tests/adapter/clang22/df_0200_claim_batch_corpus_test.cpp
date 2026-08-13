#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_entity.hpp>
#include <cxxlens/sdk.hpp>

namespace
{
	constexpr std::string_view corpus_format{
		"cxxlens.df-0200.claim-batch-differential-corpus.tsv.v1"};
	constexpr std::string_view corpus_schema_path{
		"schemas/cxxlens_ng_df_0200_claim_batch_differential_corpus.schema.yaml"};
	constexpr std::string_view corpus_api{"cxxlens::sdk::claim_batch::commit"};
	constexpr std::string_view expected_artifact_sha256{
		"sha256:f05513d05b0b57788b6f94d9c1a477c88d589b64dd8232d88a5c6c6022a84836"};
	constexpr std::string_view columns{
		"id\tequivalence_group\tverdict_group\tadded_templates\texisting_templates\t"
		"input_encoding_hex\toutcome\terror_code\terror_field\terror_detail\tclaim_count\t"
		"unresolved_count\tconflict_count\tdifferential_count\toutput_encoding_hex\t"
		"expected_tuple_encoding_hex\tverdict_encoding_hex"};

	void require(const bool condition, const std::string& message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] cxxlens::sdk::detached_cell string_cell(cxxlens::sdk::value_type type,
														  std::string value)
	{
		return {std::move(type),
				cxxlens::sdk::cell_state::present,
				cxxlens::sdk::scalar_value{std::move(value)},
				std::nullopt};
	}

	[[nodiscard]] cxxlens::sdk::detached_row make_call_row()
	{
		using relation = cxxlens::cc::relations::call_site;
		cxxlens::sdk::row_builder builder{relation::descriptor()};
		require(builder
					.set(cxxlens::sdk::query::col<relation::call>(),
						 cxxlens::sdk::detached_cell::typed("cc_call_id", "call:1"))
					.has_value(),
				"call cell rejected");
		require(builder
					.set(cxxlens::sdk::query::col<relation::compile_unit>(),
						 cxxlens::sdk::detached_cell::typed("compile_unit_id", "unit:1"))
					.has_value(),
				"compile unit cell rejected");
		require(
			builder
				.set(cxxlens::sdk::query::col<relation::kind>(),
					 string_cell({cxxlens::sdk::scalar_kind::open_symbol, "cc.call-kind/1", false},
								 "function"))
				.has_value(),
			"kind cell rejected");
		require(builder
					.set(cxxlens::sdk::query::col<relation::source>(),
						 cxxlens::sdk::detached_cell::typed("source_span_id", "source:1"))
					.has_value(),
				"source cell rejected");
		require(builder
					.set(cxxlens::sdk::query::col<relation::ordinal>(),
						 cxxlens::sdk::detached_cell::unsigned_integer(0U))
					.has_value(),
				"ordinal cell rejected");
		auto row = std::move(builder).finish();
		require(row.has_value(), "call row did not finish");
		return std::move(*row);
	}

	[[nodiscard]] cxxlens::sdk::detached_row make_direct_target_row(std::string target)
	{
		using relation = cxxlens::cc::relations::call_direct_target;
		relation::builder builder;
		require(
			builder.set<relation::call>(cxxlens::sdk::detached_cell::typed("cc_call_id", "call:1"))
				.has_value(),
			"direct target call rejected");
		require(builder
					.set<relation::target>(
						cxxlens::sdk::detached_cell::typed("cc_entity_id", std::move(target)))
					.has_value(),
				"direct target entity rejected");
		require(builder
					.set<relation::resolution>(string_cell({cxxlens::sdk::scalar_kind::open_symbol,
															"cc.direct-target-resolution/1",
															false},
														   "syntactic"))
					.has_value(),
				"direct target resolution rejected");
		auto row = std::move(builder).finish();
		require(row.has_value(), "direct target row did not finish");
		return std::move(*row);
	}

	[[nodiscard]] cxxlens::sdk::relation_descriptor
	make_target_descriptor(std::string name, std::string column_name, std::string type)
	{
		cxxlens::sdk::relation_descriptor descriptor;
		descriptor.id = name + ".v1";
		descriptor.name = std::move(name);
		descriptor.version = {1U, 0U, 0U};
		descriptor.semantic_major = 1U;
		descriptor.semantics = descriptor.name + "/1";
		descriptor.owner_namespace = "cxxlens.standard";
		const auto column_id = descriptor.id + '.' + column_name;
		descriptor.columns = {{column_id,
							   std::move(column_name),
							   {cxxlens::sdk::scalar_kind::typed_id, std::move(type), false},
							   true,
							   cxxlens::sdk::column_role::claim_key}};
		descriptor.key_columns = {column_id};
		descriptor.merge = cxxlens::sdk::merge_mode::set;
		descriptor.descriptor_digest = *cxxlens::sdk::semantic_digest(
			"cxxlens.relation-descriptor-binding.v2",
			descriptor.contract_digest + "\n" + descriptor.canonical_form());
		return descriptor;
	}

	[[nodiscard]] cxxlens::sdk::observation
	observe(cxxlens::sdk::detached_row row,
			std::vector<std::string> fragments = {"all"},
			std::string interpretation = "company.test.domain")
	{
		return {std::move(row),
				{"build.universe", std::move(fragments)},
				std::move(interpretation),
				{"company.test.provider", "company.test.provider-semantics.v1"},
				{"sha256:0000000000000000000000000000000000000000000000000000000000000000"},
				"evidence:root",
				{"exact", "project", "assumptions:none", {"schema_validated"}}};
	}

	struct case_definition
	{
		std::string id;
		std::string equivalence_group;
		std::string verdict_group;
		std::vector<std::string> added_templates;
		std::vector<std::string> existing_templates;
	};

	struct computed_case
	{
		case_definition definition;
		std::vector<std::byte> input_encoding;
		std::string outcome;
		cxxlens::sdk::error failure;
		std::size_t claim_count{};
		std::size_t unresolved_count{};
		std::size_t conflict_count{};
		std::size_t differential_count{};
		std::vector<std::byte> output_encoding;
		std::vector<std::byte> expected_tuple_encoding;
		std::vector<std::byte> verdict_encoding;
		std::vector<std::byte> group_encoding;
	};

	[[nodiscard]] std::vector<case_definition> definitions()
	{
		return {
			{"hard-missing", "", "hard-missing", {"direct-a"}, {}},
			{"soft-unresolved-exact-duplicate",
			 "",
			 "soft-unresolved",
			 {"direct-a", "direct-a"},
			 {"call"}},
			{"metadata-distinct-occurrences",
			 "",
			 "metadata-distinct",
			 {"direct-a", "direct-provenance", "direct-producer", "direct-guarantee", "direct-a"},
			 {"call"}},
			{"one-shot-conflict-forward",
			 "one-shot-permutation",
			 "conflict-a-b",
			 {"overlap-a", "overlap-b"},
			 {"call"}},
			{"one-shot-conflict-reverse",
			 "one-shot-permutation",
			 "conflict-a-b",
			 {"overlap-b", "overlap-a"},
			 {"call"}},
			{"split-new-existing-conflict",
			 "",
			 "conflict-a-b",
			 {"overlap-b"},
			 {"overlap-a", "call"}},
			{"existing-existing-non-reclassification",
			 "",
			 "no-new-conflict",
			 {"disjoint"},
			 {"overlap-a", "overlap-b", "call"}},
			{"new-existing-same-payload",
			 "",
			 "no-new-conflict",
			 {"same-payload"},
			 {"overlap-a", "call"}},
			{"new-existing-disjoint", "", "no-new-conflict", {"disjoint"}, {"overlap-a", "call"}},
			{"new-existing-cross-domain-differential",
			 "",
			 "cross-domain-differential",
			 {"other-domain"},
			 {"overlap-a", "call", "other-call"}},
		};
	}

	[[nodiscard]] std::vector<std::byte> encode_or_fail(const cxxlens::sdk::canonical_value& value,
														const std::string_view subject)
	{
		auto encoded = cxxlens::sdk::canonical_binary(value);
		require(encoded.has_value(), std::string{subject} + " canonical encoding failed");
		return std::move(*encoded);
	}

	[[nodiscard]] cxxlens::sdk::canonical_value strings(const std::vector<std::string>& values)
	{
		std::vector<cxxlens::sdk::canonical_value> output;
		output.reserve(values.size());
		for (const auto& value : values)
			output.push_back(cxxlens::sdk::canonical_value::from_string(value));
		return cxxlens::sdk::canonical_value::from_tuple(std::move(output));
	}

	[[nodiscard]] std::vector<std::byte>
	claim_projection(const std::vector<cxxlens::sdk::claim>& values)
	{
		const std::span<const cxxlens::sdk::unresolved_reference> unresolved;
		const std::span<const cxxlens::sdk::claim_conflict> conflicts;
		const std::span<const cxxlens::sdk::differential_disagreement> differential;
		auto encoded =
			cxxlens::sdk::claim_batch_content_encoding(values, unresolved, conflicts, differential);
		require(encoded.has_value(), "claim input projection encoding failed");
		return std::move(*encoded);
	}

	[[nodiscard]] std::vector<std::byte>
	case_input_encoding(const case_definition& definition,
						const std::vector<cxxlens::sdk::claim>& added,
						const std::vector<cxxlens::sdk::claim>& existing)
	{
		return encode_or_fail(
			cxxlens::sdk::canonical_value::from_tuple({
				cxxlens::sdk::canonical_value::from_string(
					"cxxlens.df-0200.claim-batch-case-input.v1"),
				cxxlens::sdk::canonical_value::from_string(definition.id),
				cxxlens::sdk::canonical_value::from_tuple({
					cxxlens::sdk::canonical_value::from_string("added"),
					strings(definition.added_templates),
					cxxlens::sdk::canonical_value::from_bytes(claim_projection(added)),
				}),
				cxxlens::sdk::canonical_value::from_tuple({
					cxxlens::sdk::canonical_value::from_string("existing"),
					strings(definition.existing_templates),
					cxxlens::sdk::canonical_value::from_bytes(claim_projection(existing)),
				}),
			}),
			"case input");
	}

	[[nodiscard]] std::vector<cxxlens::sdk::claim>
	select(const std::map<std::string, cxxlens::sdk::claim>& templates,
		   const std::vector<std::string>& ids)
	{
		std::vector<cxxlens::sdk::claim> output;
		output.reserve(ids.size());
		for (const auto& id : ids)
		{
			const auto value = templates.find(id);
			require(value != templates.end(), "unknown claim template: " + id);
			output.push_back(value->second);
		}
		return output;
	}

	[[nodiscard]] std::vector<computed_case> compute_cases()
	{
		cxxlens::sdk::relation_registry registry;
		require(registry
					.add(make_target_descriptor(
						"build.compile_unit", "compile_unit", "compile_unit_id"))
					.has_value(),
				"compile-unit descriptor rejected");
		require(registry.add(make_target_descriptor("source.span", "span", "source_span_id"))
					.has_value(),
				"source-span descriptor rejected");
		require(registry.add(cxxlens::cc::relations::entity::descriptor()).has_value(),
				"entity descriptor rejected");
		require(registry.add(cxxlens::cc::relations::call_site::descriptor()).has_value(),
				"call-site descriptor rejected");
		require(registry.add(cxxlens::cc::relations::call_direct_target::descriptor()).has_value(),
				"direct-target descriptor rejected");
		auto engine = registry.build("df-0200-corpus-engine-generation-1");
		require(static_cast<bool>(engine), "relation engine build failed");

		std::map<std::string, cxxlens::sdk::claim> templates;
		const auto add_assertion = [&](std::string id, cxxlens::sdk::observation observation)
		{
			auto assertion = cxxlens::sdk::make_assertion(*engine, std::move(observation));
			require(assertion.has_value(), "claim template rejected: " + id);
			templates.emplace(std::move(id), std::move(*assertion));
		};
		add_assertion("call", observe(make_call_row(), {"all", "asan", "debug", "release"}));
		add_assertion(
			"other-call",
			observe(make_call_row(), {"all", "asan", "debug", "release"}, "company.other.domain"));
		add_assertion("direct-a", observe(make_direct_target_row("entity:a")));
		templates.emplace("direct-provenance", templates.at("direct-a"));
		templates.at("direct-provenance").provenance_root = "evidence:alternate";
		templates.emplace("direct-producer", templates.at("direct-a"));
		templates.at("direct-producer").producer.id = "company.test.alternate-provider";
		templates.emplace("direct-guarantee", templates.at("direct-a"));
		templates.at("direct-guarantee").guarantee.approximation = "under_approximation";
		add_assertion("overlap-a",
					  observe(make_direct_target_row("entity:a"), {"debug", "release"}));
		add_assertion("overlap-b", observe(make_direct_target_row("entity:b"), {"release"}));
		add_assertion("same-payload", observe(make_direct_target_row("entity:a"), {"release"}));
		add_assertion("disjoint", observe(make_direct_target_row("entity:c"), {"asan"}));
		add_assertion(
			"other-domain",
			observe(make_direct_target_row("entity:d"), {"release"}, "company.other.domain"));

		std::vector<computed_case> output;
		for (const auto& definition : definitions())
		{
			auto added = select(templates, definition.added_templates);
			auto existing = select(templates, definition.existing_templates);
			computed_case computed;
			computed.definition = definition;
			computed.input_encoding = case_input_encoding(definition, added, existing);
			cxxlens::sdk::claim_batch batch;
			for (const auto& value : added)
				require(batch.add(value).has_value(), "claim batch add failed: " + definition.id);
			auto result = std::move(batch).commit(*engine, existing);
			if (!result)
			{
				computed.outcome = "error";
				computed.failure = result.error();
				computed.expected_tuple_encoding = encode_or_fail(
					cxxlens::sdk::canonical_value::from_tuple({
						cxxlens::sdk::canonical_value::from_string("error"),
						cxxlens::sdk::canonical_value::from_string(computed.failure.code),
						cxxlens::sdk::canonical_value::from_string(computed.failure.field),
						cxxlens::sdk::canonical_value::from_string(computed.failure.detail),
					}),
					"error tuple");
				computed.verdict_encoding = computed.expected_tuple_encoding;
				computed.group_encoding = computed.verdict_encoding;
				output.push_back(std::move(computed));
				continue;
			}
			computed.outcome = "success";
			computed.claim_count = result->claims.size();
			computed.unresolved_count = result->unresolved.size();
			computed.conflict_count = result->conflicts.size();
			computed.differential_count = result->differential_disagreements.size();
			auto full =
				cxxlens::sdk::claim_batch_content_encoding(result->claims,
														   result->unresolved,
														   result->conflicts,
														   result->differential_disagreements);
			require(full.has_value(), "full output encoding failed: " + definition.id);
			computed.output_encoding = std::move(*full);
			computed.expected_tuple_encoding = encode_or_fail(
				cxxlens::sdk::canonical_value::from_tuple({
					cxxlens::sdk::canonical_value::from_string("success"),
					cxxlens::sdk::canonical_value::from_integer(
						static_cast<std::int64_t>(computed.claim_count)),
					cxxlens::sdk::canonical_value::from_integer(
						static_cast<std::int64_t>(computed.unresolved_count)),
					cxxlens::sdk::canonical_value::from_integer(
						static_cast<std::int64_t>(computed.conflict_count)),
					cxxlens::sdk::canonical_value::from_integer(
						static_cast<std::int64_t>(computed.differential_count)),
					cxxlens::sdk::canonical_value::from_bytes(computed.output_encoding),
				}),
				"success tuple");
			const std::span<const cxxlens::sdk::claim> no_claims;
			auto semantic_records =
				cxxlens::sdk::claim_batch_content_encoding(no_claims,
														   result->unresolved,
														   result->conflicts,
														   result->differential_disagreements);
			require(semantic_records.has_value(), "verdict encoding failed: " + definition.id);
			computed.verdict_encoding = encode_or_fail(
				cxxlens::sdk::canonical_value::from_tuple({
					cxxlens::sdk::canonical_value::from_string("success-verdict"),
					cxxlens::sdk::canonical_value::from_bytes(std::move(*semantic_records)),
				}),
				"success verdict");
			const std::span<const cxxlens::sdk::unresolved_reference> no_unresolved;
			const std::span<const cxxlens::sdk::claim_conflict> no_conflicts;
			const std::span<const cxxlens::sdk::differential_disagreement> no_differential;
			cxxlens::sdk::result<std::vector<std::byte>> group_records =
				definition.verdict_group == "conflict-a-b" ||
					definition.verdict_group == "no-new-conflict"
				? cxxlens::sdk::claim_batch_content_encoding(
					  no_claims, no_unresolved, result->conflicts, no_differential)
				: definition.verdict_group == "cross-domain-differential"
				? cxxlens::sdk::claim_batch_content_encoding(
					  no_claims, no_unresolved, no_conflicts, result->differential_disagreements)
				: cxxlens::sdk::claim_batch_content_encoding(no_claims,
															 result->unresolved,
															 result->conflicts,
															 result->differential_disagreements);
			require(group_records.has_value(), "group verdict encoding failed: " + definition.id);
			computed.group_encoding = encode_or_fail(
				cxxlens::sdk::canonical_value::from_tuple({
					cxxlens::sdk::canonical_value::from_string("success-verdict"),
					cxxlens::sdk::canonical_value::from_bytes(std::move(*group_records)),
				}),
				"group verdict");
			output.push_back(std::move(computed));
		}
		return output;
	}

	[[nodiscard]] std::string hex(const std::span<const std::byte> bytes)
	{
		constexpr std::string_view digits{"0123456789abcdef"};
		std::string output;
		output.reserve(bytes.size() * 2U);
		for (const auto value : bytes)
		{
			const auto byte = std::to_integer<unsigned int>(value);
			output.push_back(digits[byte >> 4U]);
			output.push_back(digits[byte & 0x0fU]);
		}
		return output;
	}

	[[nodiscard]] std::string csv(const std::vector<std::string>& values)
	{
		if (values.empty())
			return "-";
		std::string output;
		for (const auto& value : values)
		{
			if (!output.empty())
				output.push_back(',');
			output += value;
		}
		return output;
	}

	[[nodiscard]] std::string optional(const std::string& value)
	{
		return value.empty() ? "-" : value;
	}

	[[nodiscard]] std::string census(const std::vector<computed_case>& cases)
	{
		std::size_t success_count{};
		std::size_t error_count{};
		std::size_t added_claim_count{};
		std::size_t existing_claim_count{};
		std::size_t projection_byte_count{};
		std::vector<cxxlens::sdk::canonical_value> case_projections;
		case_projections.reserve(cases.size());
		for (const auto& value : cases)
		{
			success_count += value.outcome == "success" ? 1U : 0U;
			error_count += value.outcome == "error" ? 1U : 0U;
			added_claim_count += value.definition.added_templates.size();
			existing_claim_count += value.definition.existing_templates.size();
			projection_byte_count += value.input_encoding.size();
			projection_byte_count += value.expected_tuple_encoding.size();
			projection_byte_count += value.verdict_encoding.size();
			case_projections.push_back(cxxlens::sdk::canonical_value::from_tuple({
				cxxlens::sdk::canonical_value::from_string(value.definition.id),
				cxxlens::sdk::canonical_value::from_bytes(value.input_encoding),
				cxxlens::sdk::canonical_value::from_bytes(value.expected_tuple_encoding),
				cxxlens::sdk::canonical_value::from_bytes(value.verdict_encoding),
			}));
		}
		auto projection = encode_or_fail(
			cxxlens::sdk::canonical_value::from_tuple({
				cxxlens::sdk::canonical_value::from_string(
					"cxxlens.df-0200.claim-batch-corpus-census.v1"),
				cxxlens::sdk::canonical_value::from_integer(
					static_cast<std::int64_t>(cases.size())),
				cxxlens::sdk::canonical_value::from_integer(
					static_cast<std::int64_t>(success_count)),
				cxxlens::sdk::canonical_value::from_integer(static_cast<std::int64_t>(error_count)),
				cxxlens::sdk::canonical_value::from_integer(
					static_cast<std::int64_t>(added_claim_count)),
				cxxlens::sdk::canonical_value::from_integer(
					static_cast<std::int64_t>(existing_claim_count)),
				cxxlens::sdk::canonical_value::from_integer(
					static_cast<std::int64_t>(projection_byte_count)),
				cxxlens::sdk::canonical_value::from_tuple(std::move(case_projections)),
			}),
			"corpus census");
		std::string payload;
		payload.reserve(projection.size());
		for (const auto value : projection)
			payload.push_back(static_cast<char>(std::to_integer<unsigned char>(value)));
		auto digest =
			cxxlens::sdk::semantic_digest("cxxlens.df-0200.claim-batch-corpus-census.v1", payload);
		require(digest.has_value(), "corpus census digest failed");
		return "case_count=" + std::to_string(cases.size()) +
			";success_count=" + std::to_string(success_count) +
			";error_count=" + std::to_string(error_count) +
			";added_claim_count=" + std::to_string(added_claim_count) +
			";existing_claim_count=" + std::to_string(existing_claim_count) +
			";projection_byte_count=" + std::to_string(projection_byte_count) +
			";projection_digest=" + *digest;
	}

	[[nodiscard]] std::string render(const std::vector<computed_case>& cases)
	{
		std::ostringstream output;
		output << "#format\t" << corpus_format << '\n';
		output << "#artifact_version\t1.0.0\n";
		output << "#schema_path\t" << corpus_schema_path << '\n';
		output << "#source_api\t" << corpus_api << '\n';
		output << "#census\t" << census(cases) << '\n';
		output << "#columns\t" << columns << '\n';
		for (const auto& value : cases)
		{
			output << value.definition.id << '\t' << optional(value.definition.equivalence_group)
				   << '\t' << optional(value.definition.verdict_group) << '\t'
				   << csv(value.definition.added_templates) << '\t'
				   << csv(value.definition.existing_templates) << '\t' << hex(value.input_encoding)
				   << '\t' << value.outcome << '\t' << optional(value.failure.code) << '\t'
				   << optional(value.failure.field) << '\t' << optional(value.failure.detail)
				   << '\t' << value.claim_count << '\t' << value.unresolved_count << '\t'
				   << value.conflict_count << '\t' << value.differential_count << '\t'
				   << optional(hex(value.output_encoding)) << '\t'
				   << hex(value.expected_tuple_encoding) << '\t' << hex(value.verdict_encoding)
				   << '\n';
		}
		return output.str();
	}

	void verify_semantic_groups(const std::vector<computed_case>& cases)
	{
		std::map<std::string, std::vector<std::byte>> exact_groups;
		std::map<std::string, std::vector<std::byte>> verdict_groups;
		for (const auto& value : cases)
		{
			if (!value.definition.equivalence_group.empty())
			{
				auto [entry, inserted] = exact_groups.emplace(value.definition.equivalence_group,
															  value.expected_tuple_encoding);
				require(inserted || entry->second == value.expected_tuple_encoding,
						"exact equivalence group diverged: " + value.definition.equivalence_group);
			}
			if (!value.definition.verdict_group.empty())
			{
				auto [entry, inserted] =
					verdict_groups.emplace(value.definition.verdict_group, value.group_encoding);
				require(inserted || entry->second == value.group_encoding,
						"semantic verdict group diverged: " + value.definition.verdict_group);
			}
		}
		require(exact_groups.contains("one-shot-permutation"),
				"permutation equivalence group was not exercised");
		require(verdict_groups.contains("conflict-a-b"),
				"one-shot/split verdict group was not exercised");
	}

	[[nodiscard]] std::string read_file(const std::string& path)
	{
		std::ifstream input{path, std::ios::binary};
		require(input.good(), "could not open frozen corpus: " + path);
		return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
	}

	void write_file(const std::string& path, const std::string& content)
	{
		std::ofstream output{path, std::ios::binary | std::ios::trunc};
		require(output.good(), "could not create emitted corpus: " + path);
		output.write(content.data(), static_cast<std::streamsize>(content.size()));
		require(output.good(), "could not finish emitted corpus: " + path);
	}
} // namespace

int main(const int argc, char** argv)
{
	const auto cases = compute_cases();
	require(cases.size() == 10U, "frozen corpus case census changed");
	verify_semantic_groups(cases);
	const auto generated = render(cases);
	if (argc >= 2 && std::string_view{argv[1]} == "--emit")
	{
		if (argc == 2)
			std::cout << generated;
		else
		{
			require(argc == 3, "usage: --emit [artifact-path]");
			write_file(argv[2], generated);
		}
		return 0;
	}
	require(argc == 2, "usage: df_0200_claim_batch_corpus_test <artifact-path>");
	const auto artifact = read_file(argv[1]);
	require(artifact == generated,
			"frozen corpus bytes diverged from the current public claim_batch::commit result");
	const auto raw_digest =
		cxxlens::sdk::content_digest(std::as_bytes(std::span{artifact.data(), artifact.size()}));
	require(raw_digest == expected_artifact_sha256,
			"frozen corpus raw SHA-256 diverged from the source-bound digest: " + raw_digest);
	return 0;
}
